use crate::client::{
    BROWSE_FIELDS, ClientIdentity, EmbyClient, ItemQuery, parse_notification_message,
    parse_refresh_progress_message,
};
use futures_util::{SinkExt, StreamExt};
use secrecy::SecretString;
use serde_json::json;
use std::{collections::BTreeMap, time::Duration};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream},
};
use tokio_tungstenite::{accept_async, tungstenite::Message};
use url::Url;
use yanami_core::{ServerProfile, TransportSecurity};

use crate::EmbyNotification;

fn local_http_profile(name: &str, url: Url) -> ServerProfile {
    ServerProfile::with_transport_security(name, url, TransportSecurity::AllowInsecureHttp).unwrap()
}

#[test]
fn parses_item_refresh_progress_and_completion() {
    let progress = parse_refresh_progress_message(&json!({
        "MessageType": "RefreshProgress",
        "Data": { "ItemId": "library-42", "Progress": 58.25 }
    }))
    .unwrap();
    assert_eq!(progress.item_id, "library-42");
    assert!((progress.progress - 58.25).abs() < f64::EPSILON);
    assert!(!progress.complete);

    // Emby Server 4.9 serializes this field as a JSON string on the
    // refresh notification channel. Emby Web likewise uses parseFloat.
    let string_progress = parse_refresh_progress_message(&json!({
        "MessageType": "RefreshProgress",
        "Data": { "ItemId": "library-42", "Progress": "63.75" }
    }))
    .unwrap();
    assert!((string_progress.progress - 63.75).abs() < f64::EPSILON);

    let complete = parse_refresh_progress_message(&json!({
        "MessageType": "RefreshCompleted",
        "Data": { "ItemId": "library-42", "Progress": 99.0 }
    }))
    .unwrap();
    assert!((complete.progress - 100.0).abs() < f64::EPSILON);
    assert!(complete.complete);
}

#[test]
fn ignores_unrelated_or_unattributed_messages() {
    assert!(
        parse_refresh_progress_message(&json!({
            "MessageType": "ScheduledTasksInfo",
            "Data": []
        }))
        .is_none()
    );
    assert!(
        parse_refresh_progress_message(&json!({
            "MessageType": "RefreshProgress",
            "Data": { "Progress": 20.0 }
        }))
        .is_none()
    );
    for invalid_progress in [
        json!(null),
        json!(""),
        json!("unknown"),
        json!("NaN"),
        json!("inf"),
        json!({}),
    ] {
        assert!(
            parse_refresh_progress_message(&json!({
                "MessageType": "RefreshProgress",
                "Data": { "ItemId": "library-42", "Progress": invalid_progress }
            }))
            .is_none()
        );
    }
}

#[test]
fn parses_library_and_user_data_notifications() {
    let notification = parse_notification_message(&json!({
        "MessageType": "LibraryChanged",
        "Data": {
            "ItemsAdded": ["new", ""],
            "ItemsUpdated": ["updated"],
            "ItemsRemoved": ["removed"],
            "FoldersAddedTo": ["folder"],
            "FoldersRemovedFrom": [],
            "CollectionFolders": [],
            "IsEmpty": false
        }
    }))
    .unwrap();
    let EmbyNotification::LibraryChanged(change) = notification else {
        panic!("expected a library change");
    };
    assert_eq!(change.items_added, ["new"]);
    assert_eq!(change.items_updated, ["updated"]);
    assert_eq!(change.items_removed, ["removed"]);
    assert!(change.requires_membership);

    let notification = parse_notification_message(&json!({
        "MessageType": "UserDataChanged",
        "Data": {
            "UserId": "active-user",
            "UserDataList": [{"ItemId": "episode-1"}]
        }
    }))
    .unwrap();
    let EmbyNotification::UserDataChanged(change) = notification else {
        panic!("expected a user data change");
    };
    assert_eq!(change.user_id, "active-user");
    assert_eq!(change.item_ids, ["episode-1"]);
    assert!(!change.requires_catchup);
}

#[test]
fn malformed_user_data_list_requires_catchup_and_preserves_valid_ids() {
    for (data, expected_ids) in [
        (json!({ "UserId": "active-user" }), Vec::<String>::new()),
        (
            json!({ "UserId": "active-user", "UserDataList": "not-an-array" }),
            Vec::new(),
        ),
        (
            json!({
                "UserId": "active-user",
                "UserDataList": [
                    {"ItemId": "episode-1"},
                    {"ItemId": ""},
                    {}
                ]
            }),
            vec!["episode-1".to_owned()],
        ),
    ] {
        let notification = parse_notification_message(&json!({
            "MessageType": "UserDataChanged",
            "Data": data
        }))
        .unwrap();
        let EmbyNotification::UserDataChanged(change) = notification else {
            panic!("expected a user data change");
        };
        assert_eq!(change.user_id, "active-user");
        assert_eq!(change.item_ids, expected_ids);
        assert!(change.requires_catchup);
    }
}

#[test]
fn malformed_library_change_fails_closed_to_membership_reconciliation() {
    for message in [
        json!({ "MessageType": "LibraryChanged" }),
        json!({ "MessageType": "LibraryChanged", "Data": null }),
        json!({ "MessageType": "LibraryChanged", "Data": {} }),
        json!({
            "MessageType": "LibraryChanged",
            "Data": {
                "ItemsAdded": "not-an-array",
                "ItemsUpdated": [],
                "ItemsRemoved": []
            }
        }),
        json!({
            "MessageType": "LibraryChanged",
            "Data": {
                "ItemsAdded": ["valid", 42],
                "ItemsUpdated": [],
                "ItemsRemoved": []
            }
        }),
    ] {
        let Some(EmbyNotification::LibraryChanged(change)) = parse_notification_message(&message)
        else {
            panic!("malformed LibraryChanged must remain observable");
        };
        assert!(change.requires_membership);
    }

    for user_id in [json!(null), json!(""), json!("   ")] {
        assert!(
            parse_notification_message(&json!({
                "MessageType": "UserDataChanged",
                "Data": { "UserId": user_id, "UserDataList": [] }
            }))
            .is_none()
        );
    }
}

#[tokio::test]
async fn local_websocket_exchanges_refresh_subscription_and_messages() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut socket = accept_async(stream).await.unwrap();
        let subscription = socket.next().await.unwrap().unwrap();
        let subscription: serde_json::Value =
            serde_json::from_str(subscription.to_text().unwrap()).unwrap();
        assert_eq!(subscription["MessageType"], "RefreshProgressStart");
        assert_eq!(subscription["Data"], "0,250");

        socket
            .send(Message::Ping(vec![1, 2, 3].into()))
            .await
            .unwrap();
        assert!(matches!(
            socket.next().await.unwrap().unwrap(),
            Message::Pong(payload) if payload.as_ref() == [1, 2, 3]
        ));
        socket
            .send(Message::Text(
                json!({ "MessageType": "ForceKeepAlive" })
                    .to_string()
                    .into(),
            ))
            .await
            .unwrap();
        let keep_alive = socket.next().await.unwrap().unwrap();
        let keep_alive: serde_json::Value =
            serde_json::from_str(keep_alive.to_text().unwrap()).unwrap();
        assert_eq!(keep_alive["MessageType"], "KeepAlive");

        for message in [
            json!({ "MessageType": "ScheduledTasksInfo", "Data": [] }),
            json!({
                "MessageType": "RefreshProgress",
                "Data": { "ItemId": "library-a", "Progress": 58.0 }
            }),
            json!({
                "MessageType": "RefreshProgress",
                "Data": { "ItemId": "library-b", "Progress": 100.0 }
            }),
            json!({
                "MessageType": "LibraryChanged",
                "Data": {
                    "ItemsAdded": ["episode-a"],
                    "ItemsUpdated": [],
                    "ItemsRemoved": [],
                    "FoldersAddedTo": [],
                    "FoldersRemovedFrom": [],
                    "CollectionFolders": [],
                    "IsEmpty": false
                }
            }),
        ] {
            socket
                .send(Message::Text(message.to_string().into()))
                .await
                .unwrap();
        }
    });

    let profile = local_http_profile(
        "Local test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let socket = client.notification_stream().await.unwrap();
    futures_util::pin_mut!(socket);
    let mut parsed = Vec::new();
    while parsed.len() < 3 {
        let message = socket.next().await.unwrap();
        parsed.push(message);
    }
    let EmbyNotification::RefreshProgress(first) = &parsed[0] else {
        panic!("expected refresh progress");
    };
    assert_eq!(first.item_id, "library-a");
    assert!((first.progress - 58.0).abs() < f64::EPSILON);
    assert!(!first.complete);
    let EmbyNotification::RefreshProgress(second) = &parsed[1] else {
        panic!("expected refresh progress");
    };
    assert_eq!(second.item_id, "library-b");
    assert!(second.complete, "Progress >= 100 must complete the scan");
    let EmbyNotification::LibraryChanged(change) = &parsed[2] else {
        panic!("expected library change");
    };
    assert_eq!(change.items_added, ["episode-a"]);
    server.await.unwrap();
}

async fn read_http_request_line(stream: &mut TcpStream) -> String {
    let mut request = Vec::new();
    let mut chunk = [0_u8; 2048];
    loop {
        let count = stream.read(&mut chunk).await.unwrap();
        assert!(count > 0, "HTTP request ended before its headers");
        request.extend_from_slice(&chunk[..count]);
        if request.windows(4).any(|bytes| bytes == b"\r\n\r\n") {
            break;
        }
    }
    String::from_utf8_lossy(&request)
        .lines()
        .next()
        .unwrap()
        .to_owned()
}

async fn read_http_headers(stream: &mut TcpStream) -> String {
    let mut request = Vec::new();
    let mut chunk = [0_u8; 2048];
    loop {
        let count = stream.read(&mut chunk).await.unwrap();
        assert!(count > 0, "HTTP request ended before its headers");
        request.extend_from_slice(&chunk[..count]);
        if request.windows(4).any(|bytes| bytes == b"\r\n\r\n") {
            break;
        }
    }
    String::from_utf8_lossy(&request).into_owned()
}

async fn read_http_request(stream: &mut TcpStream) -> Vec<u8> {
    let mut request = Vec::new();
    let mut chunk = [0_u8; 4096];
    let (header_end, content_length) = loop {
        let count = stream.read(&mut chunk).await.unwrap();
        assert!(count > 0, "HTTP request ended before its headers");
        request.extend_from_slice(&chunk[..count]);
        let Some(header_end) = request.windows(4).position(|bytes| bytes == b"\r\n\r\n") else {
            continue;
        };
        let headers = String::from_utf8_lossy(&request[..header_end]);
        let content_length = headers
            .lines()
            .find_map(|line| {
                line.split_once(':').and_then(|(name, value)| {
                    name.eq_ignore_ascii_case("content-length")
                        .then(|| value.trim().parse::<usize>().unwrap())
                })
            })
            .unwrap_or(0);
        break (header_end + 4, content_length);
    };
    while request.len() < header_end + content_length {
        let count = stream.read(&mut chunk).await.unwrap();
        assert!(count > 0, "HTTP request ended before its body");
        request.extend_from_slice(&chunk[..count]);
    }
    request
}

async fn write_json_response(stream: &mut TcpStream, status: &str, body: &str) {
    stream
        .write_all(
            format!(
                "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            )
            .as_bytes(),
        )
        .await
        .unwrap();
    stream.write_all(body.as_bytes()).await.unwrap();
}

fn request_url(request_line: &str) -> Url {
    let target = request_line.split_whitespace().nth(1).unwrap();
    Url::parse(&format!("http://localhost{target}")).unwrap()
}

fn empty_items_query() -> super::ItemQuery {
    super::ItemQuery {
        limit: 1,
        ..super::ItemQuery::default()
    }
}

#[tokio::test]
async fn scoped_latest_media_uses_parent_and_server_grouping_without_type_filter() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        for expected_is_played in [Some("false"), None] {
            let (mut stream, _) = listener.accept().await.unwrap();
            let request = read_http_request_line(&mut stream).await;
            let url = request_url(&request);
            assert_eq!(url.path(), "/Users/fake-user/Items/Latest");
            let parameters: BTreeMap<_, _> = url.query_pairs().into_owned().collect();
            assert_eq!(
                parameters.get("ParentId").map(String::as_str),
                Some("tv-view")
            );
            assert_eq!(parameters.get("Limit").map(String::as_str), Some("16"));
            assert_eq!(
                parameters.get("GroupItems").map(String::as_str),
                Some("true")
            );
            assert_eq!(
                parameters.get("IsPlayed").map(String::as_str),
                expected_is_played
            );
            assert!(!parameters.contains_key("IncludeItemTypes"));
            write_json_response(
                &mut stream,
                "200 OK",
                r#"[
                    {"Id":"series-b","Name":"Series B","Type":"Series","ChildCount":2},
                    {"Id":"series-a","Name":"Series A","Type":"Series","ChildCount":1}
                ]"#,
            )
            .await;
        }
    });

    let profile = local_http_profile(
        "Latest Media test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let items = client
        .latest_items_for_parent("tv-view", 16, true)
        .await
        .unwrap();
    assert_eq!(
        items
            .iter()
            .map(|item| item.id.as_str())
            .collect::<Vec<_>>(),
        ["series-b", "series-a"]
    );
    assert_eq!(items[0].child_count, Some(2));
    let visible_items = client
        .latest_items_for_parent("tv-view", 16, false)
        .await
        .unwrap();
    assert_eq!(visible_items.len(), 2);
    server.await.unwrap();
}

#[test]
fn full_catalog_page_is_lightweight_and_server_ordered() {
    let query = ItemQuery::full_catalog_page(1_000, 500);

    assert_eq!(
        query.include_item_types,
        ["Movie", "Series", "Season", "Episode"]
    );
    assert!(query.recursive);
    assert_eq!(query.start_index, 1_000);
    assert_eq!(query.limit, 500);
    assert_eq!(query.sort_by, ["SortName"]);
    assert_eq!(query.sort_order.as_deref(), Some("Ascending"));
    assert_eq!(query.enable_images, Some(true));
    assert_eq!(query.enable_user_data, Some(false));
    let fields = query.fields.unwrap();
    for required in [
        "Aliases",
        "OriginalTitle",
        "SortName",
        "ParentId",
        "DateLastSaved",
        "DateModified",
        "Etag",
        "PrimaryImageAspectRatio",
    ] {
        assert!(fields.iter().any(|field| field == required));
    }
}

#[tokio::test]
async fn default_items_query_preserves_legacy_wire_contract() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let url = request_url(&read_http_request_line(&mut stream).await);
        assert_eq!(url.path(), "/Users/fake-user/Items");
        let query: BTreeMap<_, _> = url.query_pairs().into_owned().collect();
        assert_eq!(query.get("Recursive").map(String::as_str), Some("false"));
        assert_eq!(query.get("StartIndex").map(String::as_str), Some("0"));
        assert_eq!(query.get("Limit").map(String::as_str), Some("1"));
        assert_eq!(query.get("Fields").map(String::as_str), Some(BROWSE_FIELDS));
        assert_eq!(query.get("EnableImages").map(String::as_str), Some("true"));
        assert_eq!(query.get("ImageTypeLimit").map(String::as_str), Some("1"));
        assert_eq!(
            query.get("EnableImageTypes").map(String::as_str),
            Some("Primary,Thumb,Backdrop")
        );
        assert_eq!(
            query.get("EnableUserData").map(String::as_str),
            Some("true")
        );
        for absent in ["MinDateLastSaved", "MinDateLastSavedForUser", "Ids"] {
            assert!(!query.contains_key(absent));
        }
        write_json_response(
            &mut stream,
            "200 OK",
            r#"{"Items":[],"TotalRecordCount":0}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Items defaults test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    client.items(&empty_items_query()).await.unwrap();
    server.await.unwrap();
}

#[tokio::test]
async fn items_query_supports_catalog_projection_and_incremental_filters() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let url = request_url(&read_http_request_line(&mut stream).await);
        assert_eq!(url.path(), "/Users/fake-user/Items");
        let query: BTreeMap<_, _> = url.query_pairs().into_owned().collect();
        assert_eq!(
            query.get("Fields").map(String::as_str),
            Some("Aliases,OriginalTitle,SortName,ParentId,Etag,DateModified")
        );
        assert_eq!(query.get("EnableImages").map(String::as_str), Some("false"));
        assert_eq!(
            query.get("EnableUserData").map(String::as_str),
            Some("false")
        );
        assert_eq!(
            query.get("MinDateLastSaved").map(String::as_str),
            Some("2026-08-20T10:11:12Z")
        );
        assert_eq!(
            query.get("MinDateLastSavedForUser").map(String::as_str),
            Some("2026-08-21T10:11:12Z")
        );
        assert_eq!(
            query.get("Ids").map(String::as_str),
            Some("movie-1,episode-2")
        );
        write_json_response(
            &mut stream,
            "200 OK",
            r#"{"Items":[{"Id":"episode-2","Name":"Episode Two","Aliases":["Second Episode","E02"],"OriginalTitle":"Episode II","SortName":"Episode 0002","ParentId":"season-1","Etag":"etag-2","DateModified":"2026-08-22T10:11:12Z","Type":"Episode","SeriesId":"series-1","SeriesName":"Series One","SeasonId":"season-1","SeasonName":"Season One","IndexNumber":2,"ParentIndexNumber":1,"ProductionYear":2026,"ImageTags":{"Primary":"primary-tag"},"PrimaryImageAspectRatio":0.6666666667}],"TotalRecordCount":1,"StartIndex":500}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Catalog query test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let result = client
        .items(&ItemQuery {
            start_index: 500,
            limit: 250,
            fields: Some(
                [
                    "Aliases",
                    "OriginalTitle",
                    "SortName",
                    "ParentId",
                    "Etag",
                    "DateModified",
                ]
                .into_iter()
                .map(str::to_owned)
                .collect(),
            ),
            enable_images: Some(false),
            enable_user_data: Some(false),
            min_date_last_saved: Some("2026-08-20T10:11:12Z".to_owned()),
            min_date_last_saved_for_user: Some("2026-08-21T10:11:12Z".to_owned()),
            ids: vec!["movie-1".to_owned(), "episode-2".to_owned()],
            ..ItemQuery::default()
        })
        .await
        .unwrap();
    assert_eq!(result.start_index, 500);
    let item = &result.items[0];
    assert_eq!(item.aliases, ["Second Episode", "E02"]);
    assert_eq!(item.original_title.as_deref(), Some("Episode II"));
    assert_eq!(item.sort_name.as_deref(), Some("Episode 0002"));
    assert_eq!(item.parent_id.as_deref(), Some("season-1"));
    assert_eq!(item.etag.as_deref(), Some("etag-2"));
    assert_eq!(item.date_modified.as_deref(), Some("2026-08-22T10:11:12Z"));
    assert_eq!(item.series_id.as_deref(), Some("series-1"));
    assert_eq!(item.series_name.as_deref(), Some("Series One"));
    assert_eq!(item.season_id.as_deref(), Some("season-1"));
    assert_eq!(item.season_name.as_deref(), Some("Season One"));
    assert_eq!(item.index_number, Some(2));
    assert_eq!(item.parent_index_number, Some(1));
    assert_eq!(item.production_year, Some(2026));
    assert_eq!(
        item.image_tags.get("Primary").map(String::as_str),
        Some("primary-tag")
    );
    assert_eq!(item.primary_image_aspect_ratio, Some(0.666_666_666_7));
    server.await.unwrap();
}

#[tokio::test]
async fn series_next_up_scopes_request_and_preserves_server_result() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        for body in [
            r#"{"Items":[{"Id":"episode-9","Name":"Nine","Type":"Episode","SeriesId":"series-a"},{"Id":"episode-2","Name":"Two","Type":"Episode","SeriesId":"series-a"}],"TotalRecordCount":2}"#,
            r#"{"Items":[],"TotalRecordCount":0}"#,
        ] {
            let (mut stream, _) = listener.accept().await.unwrap();
            let url = request_url(&read_http_request_line(&mut stream).await);
            assert_eq!(url.path(), "/Shows/NextUp");
            let query: BTreeMap<_, _> = url.query_pairs().into_owned().collect();
            assert_eq!(query.get("UserId").map(String::as_str), Some("fake-user"));
            assert_eq!(query.get("SeriesId").map(String::as_str), Some("series-a"));
            assert_eq!(query.get("Limit").map(String::as_str), Some("1"));
            assert_eq!(
                query.get("EnableUserData").map(String::as_str),
                Some("true")
            );
            assert_eq!(
                query.get("EnableImageTypes").map(String::as_str),
                Some("Primary,Thumb,Backdrop")
            );
            for forbidden in [
                "Filters",
                "SortBy",
                "SortOrder",
                "IncludeItemTypes",
                "Recursive",
                "IsPlayed",
            ] {
                assert!(
                    !query.contains_key(forbidden),
                    "unexpected {forbidden} query"
                );
            }
            write_json_response(&mut stream, "200 OK", body).await;
        }
    });

    let profile = local_http_profile(
        "Series Next Up test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let result = client.next_up(Some("series-a"), 1).await.unwrap();
    assert_eq!(
        result
            .items
            .iter()
            .map(|item| item.id.as_str())
            .collect::<Vec<_>>(),
        ["episode-9", "episode-2"]
    );
    assert!(
        client
            .next_up(Some("series-a"), 1)
            .await
            .unwrap()
            .items
            .is_empty()
    );
    server.await.unwrap();
}

#[tokio::test]
async fn series_episodes_request_preserves_cross_season_order_and_user_data() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let url = request_url(&read_http_request_line(&mut stream).await);
        assert_eq!(url.path(), "/Shows/series-a/Episodes");
        let query: BTreeMap<_, _> = url.query_pairs().into_owned().collect();
        assert_eq!(query.get("UserId").map(String::as_str), Some("fake-user"));
        assert_eq!(
            query.get("EnableUserData").map(String::as_str),
            Some("true")
        );
        assert!(!query.contains_key("SeasonId"));
        for forbidden in ["Limit", "SortBy", "SortOrder", "IsPlayed"] {
            assert!(
                !query.contains_key(forbidden),
                "unexpected {forbidden} query"
            );
        }
        write_json_response(
            &mut stream,
            "200 OK",
            r#"{"Items":[{"Id":"s2e1","Name":"Season two","Type":"Episode","SeriesId":"series-a","ParentIndexNumber":2,"IndexNumber":1,"UserData":{"Played":false}},{"Id":"s1e2","Name":"Played hole","Type":"Episode","SeriesId":"series-a","ParentIndexNumber":1,"IndexNumber":2,"UserData":{"Played":true}}],"TotalRecordCount":2}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Series Episodes test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let result = client.episodes("series-a", None).await.unwrap();
    assert_eq!(
        result
            .items
            .iter()
            .map(|item| item.id.as_str())
            .collect::<Vec<_>>(),
        ["s2e1", "s1e2"]
    );
    assert_eq!(result.items[0].parent_index_number, Some(2));
    assert_eq!(result.items[0].index_number, Some(1));
    assert!(result.items[1].user_data.as_ref().unwrap().played);
    server.await.unwrap();
}

#[tokio::test]
async fn stable_continue_watching_uses_resume_and_preserves_server_order() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let info_url = request_url(&read_http_request_line(&mut info).await);
        assert_eq!(info_url.path(), "/System/Info/Public");
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.9.5.0"}"#).await;

        let (mut settings, _) = listener.accept().await.unwrap();
        let settings_url = request_url(&read_http_request_line(&mut settings).await);
        assert_eq!(settings_url.path(), "/usersettings/fake-user");
        write_json_response(&mut settings, "200 OK", r#"{"homesection4":"nextup"}"#).await;

        let (mut resume, _) = listener.accept().await.unwrap();
        let resume_url = request_url(&read_http_request_line(&mut resume).await);
        assert_eq!(resume_url.path(), "/Users/fake-user/Items/Resume");
        let query: BTreeMap<_, _> = resume_url.query_pairs().into_owned().collect();
        assert_eq!(query.get("Recursive").map(String::as_str), Some("true"));
        assert_eq!(query.get("MediaTypes").map(String::as_str), Some("Video"));
        assert_eq!(
            query.get("IncludeNextUp").map(String::as_str),
            Some("false")
        );
        assert_eq!(query.get("Limit").map(String::as_str), Some("16"));
        for forbidden in ["Filters", "SortBy", "SortOrder", "IncludeItemTypes"] {
            assert!(
                !query.contains_key(forbidden),
                "unexpected {forbidden} query"
            );
        }
        write_json_response(
            &mut resume,
            "200 OK",
            r#"{"Items":[{"Id":"movie-b","Name":"Movie B","Type":"Movie"},{"Id":"episode-a2","Name":"A2","Type":"Episode","SeriesId":"series-a"},{"Id":"episode-a3","Name":"A3","Type":"Episode","SeriesId":"series-a"}],"TotalRecordCount":3}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching stable test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let result = client.continue_watching(16).await.unwrap();
    assert_eq!(
        result
            .items
            .iter()
            .map(|item| item.id.as_str())
            .collect::<Vec<_>>(),
        ["movie-b", "episode-a2", "episode-a3"]
    );
    server.await.unwrap();
}

#[tokio::test]
async fn stable_continue_watching_omits_include_next_up_for_default_web_layout() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.9.5.0"}"#).await;

        let (mut settings, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut settings).await;
        write_json_response(&mut settings, "200 OK", "{}").await;

        let (mut resume, _) = listener.accept().await.unwrap();
        let resume_url = request_url(&read_http_request_line(&mut resume).await);
        let query: BTreeMap<_, _> = resume_url.query_pairs().into_owned().collect();
        assert!(!query.contains_key("IncludeNextUp"));
        write_json_response(
            &mut resume,
            "200 OK",
            r#"{"Items":[],"TotalRecordCount":0}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching defaults test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn stable_web_layout_without_resume_is_authoritatively_empty() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.9.5.0"}"#).await;

        let (mut settings, _) = listener.accept().await.unwrap();
        let settings_url = request_url(&read_http_request_line(&mut settings).await);
        assert_eq!(settings_url.path(), "/usersettings/fake-user");
        write_json_response(&mut settings, "200 OK", r#"{"homesection1":"none"}"#).await;
    });

    let profile = local_http_profile(
        "Continue Watching hidden stable section test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn server_home_sections_drive_continue_watching_on_emby_4_10() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.10.0.4"}"#).await;

        let (mut sections, _) = listener.accept().await.unwrap();
        let sections_url = request_url(&read_http_request_line(&mut sections).await);
        assert_eq!(sections_url.path(), "/Users/fake-user/HomeSections");
        assert_eq!(
            sections_url
                .query_pairs()
                .find(|(key, _)| key == "displayMode")
                .map(|(_, value)| value.into_owned()),
            Some("mobile,desktop".to_owned())
        );
        write_json_response(
            &mut sections,
            "200 OK",
            r#"[{"Id":"resume-custom","SectionType":"resume","FutureField":true}]"#,
        )
        .await;

        let (mut items, _) = listener.accept().await.unwrap();
        let items_url = request_url(&read_http_request_line(&mut items).await);
        assert_eq!(
            items_url.path(),
            "/Users/fake-user/Sections/resume-custom/Items"
        );
        assert_eq!(
            items_url
                .query_pairs()
                .find(|(key, _)| key == "Limit")
                .map(|(_, value)| value.into_owned()),
            Some("16".to_owned())
        );
        write_json_response(
            &mut items,
            "200 OK",
            r#"{"Items":[{"Id":"server-first","Name":"First"},{"Id":"server-second","Name":"Second"}],"TotalRecordCount":2}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching 4.10 test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let result = client.continue_watching(16).await.unwrap();
    assert_eq!(
        result
            .items
            .iter()
            .map(|item| item.id.as_str())
            .collect::<Vec<_>>(),
        ["server-first", "server-second"]
    );
    server.await.unwrap();
}

#[tokio::test]
async fn server_home_layout_without_resume_is_authoritatively_empty() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.10.0.27"}"#).await;

        let (mut sections, _) = listener.accept().await.unwrap();
        let sections_url = request_url(&read_http_request_line(&mut sections).await);
        assert_eq!(sections_url.path(), "/Users/fake-user/HomeSections");
        write_json_response(
            &mut sections,
            "200 OK",
            r#"[{"Id":"latest","SectionType":"latestmedia"}]"#,
        )
        .await;

        let (mut changed_sections, _) = listener.accept().await.unwrap();
        let changed_url = request_url(&read_http_request_line(&mut changed_sections).await);
        assert_eq!(changed_url.path(), "/Users/fake-user/HomeSections");
        write_json_response(
            &mut changed_sections,
            "200 OK",
            r#"[{"Id":"resume-now-visible","SectionType":"resume"}]"#,
        )
        .await;

        let (mut items, _) = listener.accept().await.unwrap();
        let items_url = request_url(&read_http_request_line(&mut items).await);
        assert_eq!(
            items_url.path(),
            "/Users/fake-user/Sections/resume-now-visible/Items"
        );
        write_json_response(
            &mut items,
            "200 OK",
            r#"{"Items":[{"Id":"visible","Name":"Visible"}],"TotalRecordCount":1}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching hidden 4.10 section test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    assert_eq!(
        client.continue_watching(16).await.unwrap().items[0].id,
        "visible"
    );
    server.await.unwrap();
}

#[tokio::test]
async fn missing_server_home_endpoint_downgrades_once_per_session() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.10.0.4"}"#).await;

        let (mut sections, _) = listener.accept().await.unwrap();
        let sections_url = request_url(&read_http_request_line(&mut sections).await);
        assert_eq!(sections_url.path(), "/Users/fake-user/HomeSections");
        write_json_response(&mut sections, "404 Not Found", "missing").await;

        for _ in 0..2 {
            let (mut settings, _) = listener.accept().await.unwrap();
            let settings_url = request_url(&read_http_request_line(&mut settings).await);
            assert_eq!(settings_url.path(), "/usersettings/fake-user");
            write_json_response(&mut settings, "200 OK", "{}").await;

            let (mut resume, _) = listener.accept().await.unwrap();
            let resume_url = request_url(&read_http_request_line(&mut resume).await);
            assert_eq!(resume_url.path(), "/Users/fake-user/Items/Resume");
            write_json_response(
                &mut resume,
                "200 OK",
                r#"{"Items":[],"TotalRecordCount":0}"#,
            )
            .await;
        }
    });

    let profile = local_http_profile(
        "Continue Watching downgrade test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    assert!(
        client
            .clone()
            .continue_watching(16)
            .await
            .unwrap()
            .items
            .is_empty()
    );
    server.await.unwrap();
}

#[tokio::test]
async fn server_home_failures_other_than_unavailable_are_not_hidden() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.10.0.4"}"#).await;

        let (mut sections, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut sections).await;
        write_json_response(&mut sections, "500 Internal Server Error", "server failed").await;

        let (mut retry_sections, _) = listener.accept().await.unwrap();
        let retry_url = request_url(&read_http_request_line(&mut retry_sections).await);
        assert_eq!(retry_url.path(), "/Users/fake-user/HomeSections");
        write_json_response(
            &mut retry_sections,
            "200 OK",
            r#"[{"Id":"resume-retry","SectionType":"resume"}]"#,
        )
        .await;

        let (mut retry_items, _) = listener.accept().await.unwrap();
        let retry_items_url = request_url(&read_http_request_line(&mut retry_items).await);
        assert_eq!(
            retry_items_url.path(),
            "/Users/fake-user/Sections/resume-retry/Items"
        );
        write_json_response(
            &mut retry_items,
            "200 OK",
            r#"{"Items":[],"TotalRecordCount":0}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching error boundary test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(matches!(
        client.continue_watching(16).await,
        Err(super::EmbyError::Api { status: 500, .. })
    ));
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn section_items_not_found_does_not_disable_server_home_sections() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.10.0.4"}"#).await;

        let (mut sections, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut sections).await;
        write_json_response(
            &mut sections,
            "200 OK",
            r#"[{"Id":"stale-resume","SectionType":"resume"}]"#,
        )
        .await;

        let (mut items, _) = listener.accept().await.unwrap();
        let items_url = request_url(&read_http_request_line(&mut items).await);
        assert_eq!(
            items_url.path(),
            "/Users/fake-user/Sections/stale-resume/Items"
        );
        write_json_response(&mut items, "404 Not Found", "section changed").await;

        let (mut retry_sections, _) = listener.accept().await.unwrap();
        let retry_url = request_url(&read_http_request_line(&mut retry_sections).await);
        assert_eq!(retry_url.path(), "/Users/fake-user/HomeSections");
        write_json_response(
            &mut retry_sections,
            "200 OK",
            r#"[{"Id":"current-resume","SectionType":"resume"}]"#,
        )
        .await;

        let (mut retry_items, _) = listener.accept().await.unwrap();
        let retry_items_url = request_url(&read_http_request_line(&mut retry_items).await);
        assert_eq!(
            retry_items_url.path(),
            "/Users/fake-user/Sections/current-resume/Items"
        );
        write_json_response(
            &mut retry_items,
            "200 OK",
            r#"{"Items":[{"Id":"recovered","Name":"Recovered"}],"TotalRecordCount":1}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching section error boundary test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(matches!(
        client.continue_watching(16).await,
        Err(super::EmbyError::Api { status: 404, .. })
    ));
    assert_eq!(
        client.continue_watching(16).await.unwrap().items[0].id,
        "recovered"
    );
    server.await.unwrap();
}

#[tokio::test]
async fn legacy_display_preferences_control_continue_watching_visibility() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.8.11.0"}"#).await;

        let (mut settings, _) = listener.accept().await.unwrap();
        let settings_url = request_url(&read_http_request_line(&mut settings).await);
        assert_eq!(settings_url.path(), "/DisplayPreferences/usersettings");
        let query: BTreeMap<_, _> = settings_url.query_pairs().into_owned().collect();
        assert_eq!(query.get("userId").map(String::as_str), Some("fake-user"));
        assert_eq!(query.get("client").map(String::as_str), Some("emby"));
        write_json_response(
            &mut settings,
            "200 OK",
            r#"{"CustomPrefs":{"homesection1":"none"}}"#,
        )
        .await;
    });

    let profile = local_http_profile(
        "Continue Watching legacy preferences test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn settings_server_failure_is_not_replaced_with_default_layout() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let _request = read_http_request_line(&mut info).await;
        write_json_response(&mut info, "200 OK", r#"{"Version":"4.9.5.0"}"#).await;

        let (mut settings, _) = listener.accept().await.unwrap();
        let settings_url = request_url(&read_http_request_line(&mut settings).await);
        assert_eq!(settings_url.path(), "/usersettings/fake-user");
        write_json_response(&mut settings, "500 Internal Server Error", "failed").await;
    });

    let profile = local_http_profile(
        "Continue Watching settings error test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(matches!(
        client.continue_watching(16).await,
        Err(super::EmbyError::Api { status: 500, .. })
    ));
    server.await.unwrap();
}

#[tokio::test]
async fn unknown_capabilities_and_settings_dialect_are_cached_for_the_session() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut info, _) = listener.accept().await.unwrap();
        let info_url = request_url(&read_http_request_line(&mut info).await);
        assert_eq!(info_url.path(), "/System/Info/Public");
        write_json_response(&mut info, "500 Internal Server Error", "failed").await;

        let (mut sections, _) = listener.accept().await.unwrap();
        let sections_url = request_url(&read_http_request_line(&mut sections).await);
        assert_eq!(sections_url.path(), "/Users/fake-user/HomeSections");
        write_json_response(&mut sections, "404 Not Found", "missing").await;

        let (mut flat_settings, _) = listener.accept().await.unwrap();
        let flat_url = request_url(&read_http_request_line(&mut flat_settings).await);
        assert_eq!(flat_url.path(), "/usersettings/fake-user");
        write_json_response(&mut flat_settings, "404 Not Found", "missing").await;

        for _ in 0..2 {
            let (mut legacy_settings, _) = listener.accept().await.unwrap();
            let legacy_url = request_url(&read_http_request_line(&mut legacy_settings).await);
            assert_eq!(legacy_url.path(), "/DisplayPreferences/usersettings");
            write_json_response(&mut legacy_settings, "200 OK", r#"{"CustomPrefs":{}}"#).await;

            let (mut resume, _) = listener.accept().await.unwrap();
            let resume_url = request_url(&read_http_request_line(&mut resume).await);
            assert_eq!(resume_url.path(), "/Users/fake-user/Items/Resume");
            write_json_response(
                &mut resume,
                "200 OK",
                r#"{"Items":[],"TotalRecordCount":0}"#,
            )
            .await;
        }
    });

    let profile = local_http_profile(
        "Continue Watching unknown capabilities test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    assert!(client.continue_watching(16).await.unwrap().items.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn follows_same_origin_redirect_without_dropping_emby_credentials() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut first, _) = listener.accept().await.unwrap();
        let first_request = read_http_headers(&mut first).await.to_ascii_lowercase();
        assert!(first_request.contains("x-emby-token: fake-token"));
        first
            .write_all(
                b"HTTP/1.1 302 Found\r\nLocation: /redirected-items\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            )
            .await
            .unwrap();

        let (mut second, _) = listener.accept().await.unwrap();
        let second_request = read_http_headers(&mut second).await.to_ascii_lowercase();
        assert!(second_request.starts_with("get /redirected-items "));
        assert!(second_request.contains("x-emby-token: fake-token"));
        let body = br#"{"Items":[],"TotalRecordCount":0}"#;
        second
            .write_all(
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                )
                .as_bytes(),
            )
            .await
            .unwrap();
        second.write_all(body).await.unwrap();
    });

    let profile = local_http_profile(
        "Redirect test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let items = client.items(&empty_items_query()).await.unwrap();
    assert!(items.items.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn rejects_cross_origin_redirect_before_credentials_reach_the_sink() {
    let source = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let source_address = source.local_addr().unwrap();
    let sink = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let sink_address = sink.local_addr().unwrap();

    let source_server = tokio::spawn(async move {
        let (mut stream, _) = source.accept().await.unwrap();
        let request = read_http_headers(&mut stream).await.to_ascii_lowercase();
        assert!(request.contains("x-emby-token: fake-token"));
        stream
            .write_all(
                format!(
                    "HTTP/1.1 302 Found\r\nLocation: http://{sink_address}/steal\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                )
                .as_bytes(),
            )
            .await
            .unwrap();
    });
    let sink_server = tokio::spawn(async move {
        assert!(
            tokio::time::timeout(Duration::from_millis(400), sink.accept())
                .await
                .is_err(),
            "cross-origin redirect sink received an authenticated request"
        );
    });

    let profile = local_http_profile(
        "Redirect test",
        Url::parse(&format!("http://{source_address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(client.items(&empty_items_query()).await.is_err());
    source_server.await.unwrap();
    sink_server.await.unwrap();
}

#[tokio::test]
async fn rejects_declared_oversized_image_without_buffering_the_body() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let _request = read_http_headers(&mut stream).await;
        stream
            .write_all(
                b"HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: 26214401\r\nConnection: close\r\n\r\n",
            )
            .await
            .unwrap();
    });

    let profile = local_http_profile(
        "Response limit test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    assert!(matches!(
        client.image("item", "Primary", "tag", 100).await,
        Err(super::EmbyError::ResponseTooLarge {
            limit_bytes: crate::transport::MAX_IMAGE_RESPONSE_BYTES
        })
    ));
    server.await.unwrap();
}

#[tokio::test]
async fn playback_info_requests_only_progressive_direct_streaming() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let request = read_http_request(&mut stream).await;
        let header_end = request
            .windows(4)
            .position(|bytes| bytes == b"\r\n\r\n")
            .unwrap()
            + 4;
        let headers = String::from_utf8_lossy(&request[..header_end]);
        assert!(headers.starts_with("POST /Items/item-42/PlaybackInfo "));
        let body: serde_json::Value = serde_json::from_slice(&request[header_end..]).unwrap();
        assert_eq!(body["IsPlayback"], true);
        assert_eq!(body["AutoOpenLiveStream"], false);
        assert_eq!(body["EnableDirectPlay"], false);
        assert_eq!(body["EnableDirectStream"], true);
        assert_eq!(body["EnableTranscoding"], false);
        assert!(body.get("MaxStreamingBitrate").is_none());

        let response = br#"{"MediaSources":[],"PlaySessionId":"session"}"#;
        stream
            .write_all(
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    response.len()
                )
                .as_bytes(),
            )
            .await
            .unwrap();
        stream.write_all(response).await.unwrap();
    });

    let profile = local_http_profile(
        "Direct playback test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    let playback = client.playback_info("item-42").await.unwrap();
    assert!(playback.media_sources.is_empty());
    server.await.unwrap();
}

#[tokio::test]
async fn metadata_refresh_always_uses_full_provider_modes_while_scan_stays_default() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let expected = [
            ("FullRefresh", "FullRefresh", "false", "false"),
            ("FullRefresh", "FullRefresh", "true", "true"),
            ("Default", "Default", "false", "false"),
        ];
        for (metadata_mode, image_mode, replace_metadata, replace_images) in expected {
            let (mut stream, _) = listener.accept().await.unwrap();
            let request_line = read_http_request_line(&mut stream).await;
            let target = request_line.split_whitespace().nth(1).unwrap();
            let url = Url::parse(&format!("http://localhost{target}")).unwrap();
            assert_eq!(url.path(), "/Items/item-42/Refresh");
            let query: BTreeMap<_, _> = url.query_pairs().into_owned().collect();
            assert_eq!(query.get("Recursive").map(String::as_str), Some("true"));
            assert_eq!(
                query.get("MetadataRefreshMode").map(String::as_str),
                Some(metadata_mode)
            );
            assert_eq!(
                query.get("ImageRefreshMode").map(String::as_str),
                Some(image_mode)
            );
            assert_eq!(
                query.get("ReplaceAllMetadata").map(String::as_str),
                Some(replace_metadata)
            );
            assert_eq!(
                query.get("ReplaceAllImages").map(String::as_str),
                Some(replace_images)
            );
            stream
                .write_all(
                    b"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                )
                .await
                .unwrap();
        }
    });

    let profile = local_http_profile(
        "Local refresh test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    client
        .refresh_metadata("item-42", false, false)
        .await
        .unwrap();
    client
        .refresh_metadata("item-42", true, true)
        .await
        .unwrap();
    client.scan_library_files("item-42").await.unwrap();
    server.await.unwrap();
}

#[tokio::test]
async fn indexed_image_upload_uses_the_emby_path_and_base64_body() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let mut request = Vec::new();
        let mut chunk = [0_u8; 4096];
        let (header_end, content_length) = loop {
            let count = stream.read(&mut chunk).await.unwrap();
            assert!(count > 0, "HTTP request ended before its headers");
            request.extend_from_slice(&chunk[..count]);
            let Some(header_end) = request.windows(4).position(|bytes| bytes == b"\r\n\r\n") else {
                continue;
            };
            let headers = String::from_utf8_lossy(&request[..header_end]);
            let content_length = headers
                .lines()
                .find_map(|line| {
                    line.split_once(':').and_then(|(name, value)| {
                        name.eq_ignore_ascii_case("content-length")
                            .then(|| value.trim().parse::<usize>().unwrap())
                    })
                })
                .unwrap();
            break (header_end + 4, content_length);
        };
        while request.len() < header_end + content_length {
            let count = stream.read(&mut chunk).await.unwrap();
            assert!(count > 0, "HTTP request ended before its body");
            request.extend_from_slice(&chunk[..count]);
        }
        let headers = String::from_utf8_lossy(&request[..header_end]);
        assert!(
            headers
                .lines()
                .next()
                .is_some_and(|line| line.contains("POST /Items/item-7/Images/Backdrop/2 ")),
            "unexpected request line: {headers}"
        );
        assert!(
            headers
                .lines()
                .any(|line| line.eq_ignore_ascii_case("content-type: image/png"))
        );
        assert_eq!(&request[header_end..header_end + content_length], b"AQID");
        stream
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            .await
            .unwrap();
    });

    let profile = local_http_profile(
        "Local image test",
        Url::parse(&format!("http://{address}")).unwrap(),
    );
    let client = EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("fake-device"),
        "fake-user",
        SecretString::from("fake-token"),
    )
    .unwrap();
    client
        .upload_item_image("item-7", "Backdrop", Some(2), "image/png", &[1, 2, 3])
        .await
        .unwrap();
    server.await.unwrap();
}
