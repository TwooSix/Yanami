use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use serde_json::Value;

use crate::{
    client::{BROWSE_FIELDS, EmbyClient, EmbyError, ItemQuery, parse_notification_message},
    models::{EmbyNotification, ItemCreationResult, ItemsResult, UserItemData},
    transport::{decode, ensure_success},
};

impl EmbyClient {
    pub async fn update_item(&self, item_id: &str, item: &Value) -> Result<(), EmbyError> {
        ensure_success(
            self.request(reqwest::Method::POST, &format!("Items/{item_id}"))
                .json(item)
                .send()
                .await?,
        )
        .await
    }

    pub async fn set_played(&self, item_id: &str, played: bool) -> Result<UserItemData, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let method = if played {
            reqwest::Method::POST
        } else {
            reqwest::Method::DELETE
        };
        decode(
            self.request(method, &format!("Users/{user_id}/PlayedItems/{item_id}"))
                .send()
                .await?,
        )
        .await
    }

    pub async fn set_favorite(
        &self,
        item_id: &str,
        favorite: bool,
    ) -> Result<UserItemData, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let method = if favorite {
            reqwest::Method::POST
        } else {
            reqwest::Method::DELETE
        };
        decode(
            self.request(method, &format!("Users/{user_id}/FavoriteItems/{item_id}"))
                .send()
                .await?,
        )
        .await
    }

    pub async fn playlists(&self) -> Result<ItemsResult, EmbyError> {
        self.playlist_items_query(None).await
    }

    pub async fn editable_playlists(&self) -> Result<ItemsResult, EmbyError> {
        self.playlist_items_query(Some(true)).await
    }

    async fn playlist_items_query(
        &self,
        can_edit_items: Option<bool>,
    ) -> Result<ItemsResult, EmbyError> {
        self.items(&ItemQuery {
            include_item_types: vec!["Playlist".to_owned()],
            recursive: true,
            limit: 200,
            sort_by: vec!["SortName".to_owned()],
            sort_order: Some("Ascending".to_owned()),
            can_edit_items,
            ..ItemQuery::default()
        })
        .await
    }

    pub async fn add_to_playlist(&self, playlist_id: &str, item_id: &str) -> Result<(), EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        ensure_success(
            self.request(
                reqwest::Method::POST,
                &format!("Playlists/{playlist_id}/Items"),
            )
            .query(&[("UserId", user_id), ("Ids", item_id)])
            .send()
            .await?,
        )
        .await
    }

    pub async fn playlist_items(&self, playlist_id: &str) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(
                reqwest::Method::GET,
                &format!("Playlists/{playlist_id}/Items"),
            )
            .query(&[
                ("UserId", user_id),
                ("Limit", "10000"),
                ("Fields", BROWSE_FIELDS),
                ("EnableImages", "true"),
                ("ImageTypeLimit", "1"),
                ("EnableImageTypes", "Primary,Thumb,Backdrop"),
                ("EnableUserData", "true"),
            ])
            .send()
            .await?,
        )
        .await
    }

    pub async fn remove_playlist_entry(
        &self,
        playlist_id: &str,
        entry_id: &str,
    ) -> Result<(), EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        ensure_success(
            self.request(
                reqwest::Method::DELETE,
                &format!("Playlists/{playlist_id}/Items"),
            )
            .query(&[("EntryIds", entry_id), ("UserId", user_id)])
            .send()
            .await?,
        )
        .await
    }

    pub async fn create_playlist(&self, name: &str, item_id: &str) -> Result<String, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let created: ItemCreationResult = decode(
            self.request(reqwest::Method::POST, "Playlists")
                .query(&[
                    ("UserId", user_id),
                    ("Name", name),
                    ("Ids", item_id),
                    ("MediaType", "Video"),
                    ("IsPublic", "false"),
                ])
                .send()
                .await?,
        )
        .await?;
        Ok(created.id)
    }

    /// Requests the same recursive folder refresh as Emby Web's item menu.
    pub async fn scan_library_files(&self, item_id: &str) -> Result<(), EmbyError> {
        ensure_success(
            self.request(reqwest::Method::POST, &format!("Items/{item_id}/Refresh"))
                .query(&[
                    ("Recursive", "true"),
                    ("MetadataRefreshMode", "Default"),
                    ("ImageRefreshMode", "Default"),
                    ("ReplaceAllMetadata", "false"),
                    ("ReplaceAllImages", "false"),
                ])
                .send()
                .await?,
        )
        .await
    }

    /// Opens Emby's single notification channel and streams the typed events
    /// consumed by both refresh progress and catalog synchronization.
    ///
    /// A library refresh started through `Items/{Id}/Refresh` is not a
    /// scheduled task, so polling `/ScheduledTasks` cannot reliably attribute
    /// progress to one library. Emby's own web client listens for these two
    /// messages on this channel instead.
    pub async fn notification_stream(
        &self,
    ) -> Result<impl futures_util::Stream<Item = EmbyNotification> + use<>, EmbyError> {
        let token = self.access_token().ok_or_else(|| EmbyError::Api {
            status: 401,
            message: "no Emby access token is available".to_owned(),
        })?;
        // Match Emby's own ApiClient endpoint selection. Its public server
        // address resolves the notification endpoint as `embywebsocket`, not
        // as a REST-style `socket` resource.
        let mut url = self.profile.api_url("embywebsocket");
        url.set_scheme(match url.scheme() {
            "https" => "wss",
            _ => "ws",
        })
        .map_err(|()| EmbyError::InvalidUrl(url::ParseError::RelativeUrlWithoutBase))?;
        url.set_query(Some(&format!(
            "api_key={}&deviceId={}",
            url::form_urlencoded::byte_serialize(token.as_bytes()).collect::<String>(),
            url::form_urlencoded::byte_serialize(self.identity.device_id.as_bytes())
                .collect::<String>()
        )));

        let (socket, _) = tokio::time::timeout(
            Duration::from_secs(10),
            tokio_tungstenite::connect_async(url.as_str()),
        )
        .await
        .map_err(|_| EmbyError::Api {
            status: 0,
            message: "Emby notification connection timed out".to_owned(),
        })?
        .map_err(|_| EmbyError::Api {
            status: 0,
            // The connection URL carries the access token in its query. Keep
            // transport diagnostics deliberately generic so no downstream
            // logger can accidentally persist it.
            message: "Emby notification connection failed".to_owned(),
        })?;
        let mut socket = socket;
        socket
            .send(tokio_tungstenite::tungstenite::Message::Text(
                r#"{"MessageType":"RefreshProgressStart","Data":"0,250"}"#.into(),
            ))
            .await
            .map_err(|_| EmbyError::Api {
                status: 0,
                message: "unable to subscribe to Emby refresh progress".to_owned(),
            })?;

        Ok(futures_util::stream::unfold(
            socket,
            |mut socket| async move {
                use tokio_tungstenite::tungstenite::Message;

                loop {
                    let Some(Ok(message)) = socket.next().await else {
                        return None;
                    };
                    match message {
                        Message::Ping(payload) => {
                            if socket.send(Message::Pong(payload)).await.is_err() {
                                return None;
                            }
                        }
                        Message::Text(text) => {
                            let Ok(value) = serde_json::from_str::<Value>(text.as_ref()) else {
                                continue;
                            };
                            if value.get("MessageType").and_then(Value::as_str)
                                == Some("ForceKeepAlive")
                            {
                                if socket
                                    .send(Message::Text(r#"{"MessageType":"KeepAlive"}"#.into()))
                                    .await
                                    .is_err()
                                {
                                    return None;
                                }
                                continue;
                            }
                            if let Some(notification) = parse_notification_message(&value) {
                                return Some((notification, socket));
                            }
                        }
                        Message::Close(_) => return None,
                        Message::Binary(_) | Message::Pong(_) | Message::Frame(_) => {}
                    }
                }
            },
        ))
    }

    pub async fn refresh_metadata(
        &self,
        item_id: &str,
        replace_all_metadata: bool,
        replace_all_images: bool,
    ) -> Result<(), EmbyError> {
        ensure_success(
            self.request(reqwest::Method::POST, &format!("Items/{item_id}/Refresh"))
                .query(&[
                    ("Recursive", "true"),
                    // Match Emby Web's refresh dialog.  FullRefresh controls
                    // whether providers are consulted; the ReplaceAll flags
                    // independently decide whether existing values survive.
                    ("MetadataRefreshMode", "FullRefresh"),
                    ("ImageRefreshMode", "FullRefresh"),
                    (
                        "ReplaceAllMetadata",
                        if replace_all_metadata {
                            "true"
                        } else {
                            "false"
                        },
                    ),
                    (
                        "ReplaceAllImages",
                        if replace_all_images { "true" } else { "false" },
                    ),
                ])
                .send()
                .await?,
        )
        .await
    }

    pub async fn delete_item(&self, item_id: &str) -> Result<(), EmbyError> {
        ensure_success(
            self.request(reqwest::Method::DELETE, &format!("Items/{item_id}"))
                .send()
                .await?,
        )
        .await
    }
}
