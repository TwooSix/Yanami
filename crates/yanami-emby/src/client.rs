use secrecy::{ExposeSecret, SecretString};
use serde::Serialize;
use std::time::Duration;
use thiserror::Error;
use yanami_core::{ServerProfile, TlsPolicy};

use crate::models::{AuthenticationResult, BaseItem, ItemsResult, PlaybackInfo, PlaybackProgress};

const BROWSE_FIELDS: &str =
    "Overview,PrimaryImageAspectRatio,UserData,DateCreated,PremiereDate,ParentId,SortName";
const ITEM_FIELDS: &str =
    "Overview,PrimaryImageAspectRatio,UserData,DateCreated,PremiereDate,ParentId,SortName,Chapters";

#[derive(Debug, Clone)]
pub struct ClientIdentity {
    pub client: String,
    pub device: String,
    pub device_id: String,
    pub version: String,
}

impl ClientIdentity {
    pub fn yanami(device_id: impl Into<String>) -> Self {
        Self {
            client: "Yanami".to_owned(),
            device: "Desktop".to_owned(),
            device_id: device_id.into(),
            version: env!("CARGO_PKG_VERSION").to_owned(),
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct ItemQuery {
    pub parent_id: Option<String>,
    pub search_term: Option<String>,
    pub include_item_types: Vec<String>,
    pub recursive: bool,
    pub start_index: u32,
    pub limit: u32,
    pub sort_by: Vec<String>,
    pub sort_order: Option<String>,
    pub filters: Vec<String>,
}

#[derive(Debug, Error)]
pub enum EmbyError {
    #[error("Emby request failed: {0}")]
    Transport(#[from] reqwest::Error),
    #[error("Emby returned HTTP {status}: {message}")]
    Api { status: u16, message: String },
    #[error("invalid URL returned by Emby: {0}")]
    InvalidUrl(#[from] url::ParseError),
    #[error("certificate pinning is not available in this build")]
    CertificatePinningUnavailable,
}

/// HTTP client scoped to exactly one Emby server and, after login, one user.
#[derive(Clone)]
pub struct EmbyClient {
    http: reqwest::Client,
    profile: ServerProfile,
    identity: ClientIdentity,
    user_id: Option<String>,
    token: Option<SecretString>,
}

impl EmbyClient {
    pub fn new(profile: ServerProfile, identity: ClientIdentity) -> Result<Self, EmbyError> {
        if !matches!(profile.tls_policy, TlsPolicy::Strict) {
            return Err(EmbyError::CertificatePinningUnavailable);
        }
        let http = reqwest::Client::builder()
            .timeout(Duration::from_secs(30))
            .build()?;
        Ok(Self {
            http,
            profile,
            identity,
            user_id: None,
            token: None,
        })
    }

    pub fn with_session(
        profile: ServerProfile,
        identity: ClientIdentity,
        user_id: impl Into<String>,
        token: SecretString,
    ) -> Result<Self, EmbyError> {
        let mut client = Self::new(profile, identity)?;
        client.user_id = Some(user_id.into());
        client.token = Some(token);
        Ok(client)
    }

    pub fn profile(&self) -> &ServerProfile {
        &self.profile
    }

    pub fn access_token(&self) -> Option<&str> {
        self.token.as_ref().map(ExposeSecret::expose_secret)
    }

    pub async fn authenticate(
        &mut self,
        username: &str,
        password: &str,
    ) -> Result<AuthenticationResult, EmbyError> {
        #[derive(Serialize)]
        #[serde(rename_all = "PascalCase")]
        struct Login<'a> {
            username: &'a str,
            pw: &'a str,
        }

        let response = self
            .request(reqwest::Method::POST, "Users/AuthenticateByName")
            .json(&Login {
                username,
                pw: password,
            })
            .send()
            .await?;
        let result: AuthenticationResult = decode(response).await?;
        self.user_id = Some(result.user.id.clone());
        self.token = Some(SecretString::from(result.access_token.clone()));
        Ok(result)
    }

    pub async fn items(&self, query: &ItemQuery) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let path = format!("Users/{user_id}/Items");
        let mut request = self.request(reqwest::Method::GET, &path);
        let include_types = query.include_item_types.join(",");
        let mut params = vec![
            ("Recursive", query.recursive.to_string()),
            ("StartIndex", query.start_index.to_string()),
            ("Limit", query.limit.max(1).to_string()),
            ("Fields", BROWSE_FIELDS.to_owned()),
            ("EnableImages", "true".to_owned()),
            ("ImageTypeLimit", "1".to_owned()),
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
            ("EnableUserData", "true".to_owned()),
        ];
        if let Some(parent_id) = &query.parent_id {
            params.push(("ParentId", parent_id.clone()));
        }
        if let Some(search_term) = &query.search_term {
            params.push(("SearchTerm", search_term.clone()));
        }
        if !include_types.is_empty() {
            params.push(("IncludeItemTypes", include_types));
        }
        if !query.sort_by.is_empty() {
            params.push(("SortBy", query.sort_by.join(",")));
        }
        if let Some(sort_order) = &query.sort_order {
            params.push(("SortOrder", sort_order.clone()));
        }
        if !query.filters.is_empty() {
            params.push(("Filters", query.filters.join(",")));
        }
        request = request.query(&params);
        decode(request.send().await?).await
    }

    pub async fn user_views(&self) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(reqwest::Method::GET, &format!("Users/{user_id}/Views"))
                .query(&[("IncludeExternalContent", "false")])
                .send()
                .await?,
        )
        .await
    }

    pub async fn latest_items(
        &self,
        include_item_types: &[&str],
        limit: u32,
        group_items: bool,
    ) -> Result<Vec<BaseItem>, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let response = self
            .request(
                reqwest::Method::GET,
                &format!("Users/{user_id}/Items/Latest"),
            )
            .query(&[
                ("Limit", limit.max(1).to_string()),
                ("Fields", BROWSE_FIELDS.to_owned()),
                ("IncludeItemTypes", include_item_types.join(",")),
                ("GroupItems", group_items.to_string()),
                ("EnableImages", "true".to_owned()),
                ("ImageTypeLimit", "1".to_owned()),
                ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
                ("EnableUserData", "true".to_owned()),
            ])
            .send()
            .await?;
        decode(response).await
    }

    pub async fn seasons(&self, series_id: &str) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let response = self
            .request(reqwest::Method::GET, &format!("Shows/{series_id}/Seasons"))
            .query(&[
                ("UserId", user_id.to_owned()),
                ("Fields", BROWSE_FIELDS.to_owned()),
                ("EnableImages", "true".to_owned()),
                ("ImageTypeLimit", "1".to_owned()),
                ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
                ("EnableUserData", "true".to_owned()),
            ])
            .send()
            .await?;
        decode(response).await
    }

    pub async fn episodes(
        &self,
        series_id: &str,
        season_id: Option<&str>,
    ) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let mut parameters = vec![
            ("UserId", user_id.to_owned()),
            ("Fields", BROWSE_FIELDS.to_owned()),
            ("EnableImages", "true".to_owned()),
            ("ImageTypeLimit", "1".to_owned()),
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
            ("EnableUserData", "true".to_owned()),
        ];
        if let Some(season_id) = season_id {
            parameters.push(("SeasonId", season_id.to_owned()));
        }
        let response = self
            .request(reqwest::Method::GET, &format!("Shows/{series_id}/Episodes"))
            .query(&parameters)
            .send()
            .await?;
        decode(response).await
    }

    pub async fn next_up(&self, series_id: &str) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let response = self
            .request(reqwest::Method::GET, "Shows/NextUp")
            .query(&[
                ("UserId", user_id.to_owned()),
                ("SeriesId", series_id.to_owned()),
                ("Limit", "1".to_owned()),
                ("Fields", BROWSE_FIELDS.to_owned()),
                ("EnableImages", "true".to_owned()),
                ("ImageTypeLimit", "1".to_owned()),
                ("EnableUserData", "true".to_owned()),
            ])
            .send()
            .await?;
        decode(response).await
    }

    pub async fn image(
        &self,
        item_id: &str,
        image_type: &str,
        tag: &str,
        max_height: u32,
    ) -> Result<Vec<u8>, EmbyError> {
        let image_path = if image_type == "Backdrop" {
            format!("Items/{item_id}/Images/Backdrop/0")
        } else {
            format!("Items/{item_id}/Images/{image_type}")
        };
        let response = self
            .request(reqwest::Method::GET, &image_path)
            .query(&[
                ("Tag", tag.to_owned()),
                ("MaxHeight", max_height.to_string()),
                ("Quality", "88".to_owned()),
                ("Format", "jpg".to_owned()),
            ])
            .send()
            .await?;
        let status = response.status();
        if !status.is_success() {
            return Err(EmbyError::Api {
                status: status.as_u16(),
                message: response.text().await.unwrap_or_default(),
            });
        }
        Ok(response.bytes().await?.to_vec())
    }

    pub async fn item(&self, item_id: &str) -> Result<BaseItem, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(
                reqwest::Method::GET,
                &format!("Users/{user_id}/Items/{item_id}"),
            )
            .query(&[
                ("Fields", ITEM_FIELDS.to_owned()),
                ("EnableImages", "true".to_owned()),
                ("ImageTypeLimit", "1".to_owned()),
                ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
                ("EnableUserData", "true".to_owned()),
            ])
            .send()
            .await?,
        )
        .await
    }

    pub async fn playback_info(
        &self,
        item_id: &str,
        max_streaming_bitrate: Option<u64>,
    ) -> Result<PlaybackInfo, EmbyError> {
        #[derive(Serialize)]
        #[serde(rename_all = "PascalCase")]
        #[allow(clippy::struct_excessive_bools)]
        struct Request<'a> {
            user_id: &'a str,
            is_playback: bool,
            auto_open_live_stream: bool,
            enable_direct_play: bool,
            enable_direct_stream: bool,
            enable_transcoding: bool,
            #[serde(skip_serializing_if = "Option::is_none")]
            max_streaming_bitrate: Option<u64>,
        }

        decode(
            self.request(
                reqwest::Method::POST,
                &format!("Items/{item_id}/PlaybackInfo"),
            )
            .json(&Request {
                user_id: self.user_id.as_deref().unwrap_or_default(),
                is_playback: true,
                auto_open_live_stream: true,
                enable_direct_play: false,
                enable_direct_stream: true,
                enable_transcoding: true,
                max_streaming_bitrate,
            })
            .send()
            .await?,
        )
        .await
    }

    pub async fn report_started(&self, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        self.report("Sessions/Playing", progress).await
    }

    pub async fn report_progress(&self, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        self.report("Sessions/Playing/Progress", progress).await
    }

    pub async fn report_stopped(&self, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        self.report("Sessions/Playing/Stopped", progress).await
    }

    async fn report(&self, path: &str, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        let response = self
            .request(reqwest::Method::POST, path)
            .json(progress)
            .send()
            .await?;
        ensure_success(response).await
    }

    fn request(&self, method: reqwest::Method, path: &str) -> reqwest::RequestBuilder {
        let mut request = self
            .http
            .request(method, self.profile.api_url(path))
            .header("Accept", "application/json")
            .header("X-Emby-Authorization", self.authorization_header());
        if let Some(token) = &self.token {
            request = request.header("X-Emby-Token", token.expose_secret());
        }
        request
    }

    fn authorization_header(&self) -> String {
        format!(
            "Emby UserId=\"{}\", Client=\"{}\", Device=\"{}\", DeviceId=\"{}\", Version=\"{}\"",
            self.user_id.as_deref().unwrap_or_default(),
            self.identity.client,
            self.identity.device,
            self.identity.device_id,
            self.identity.version
        )
    }
}

async fn decode<T: serde::de::DeserializeOwned>(
    response: reqwest::Response,
) -> Result<T, EmbyError> {
    let status = response.status();
    if !status.is_success() {
        return Err(EmbyError::Api {
            status: status.as_u16(),
            message: response.text().await.unwrap_or_default(),
        });
    }
    Ok(response.json().await?)
}

async fn ensure_success(response: reqwest::Response) -> Result<(), EmbyError> {
    let status = response.status();
    if status.is_success() {
        Ok(())
    } else {
        Err(EmbyError::Api {
            status: status.as_u16(),
            message: response.text().await.unwrap_or_default(),
        })
    }
}
