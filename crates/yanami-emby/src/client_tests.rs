use crate::client::{ClientIdentity, EmbyClient, parse_refresh_progress_message};
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
    let socket = client.refresh_progress_stream().await.unwrap();
    futures_util::pin_mut!(socket);
    let mut parsed = Vec::new();
    while parsed.len() < 2 {
        let message = socket.next().await.unwrap();
        parsed.push(message);
    }
    assert_eq!(parsed[0].item_id, "library-a");
    assert!((parsed[0].progress - 58.0).abs() < f64::EPSILON);
    assert!(!parsed[0].complete);
    assert_eq!(parsed[1].item_id, "library-b");
    assert!(parsed[1].complete, "Progress >= 100 must complete the scan");
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

fn empty_items_query() -> super::ItemQuery {
    super::ItemQuery {
        limit: 1,
        ..super::ItemQuery::default()
    }
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
