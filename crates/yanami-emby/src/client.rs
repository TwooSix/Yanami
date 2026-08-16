use std::time::Duration;

use secrecy::{ExposeSecret, SecretString};
use serde::Serialize;
use serde_json::Value;
use thiserror::Error;
use yanami_core::{ProfileError, ServerProfile, TlsPolicy, same_origin};

use crate::{
    models::{AuthenticationResult, RefreshProgress},
    transport::decode,
};

const BUILD_VERSION: &str = match option_env!("YANAMI_BUILD_VERSION") {
    Some(version) => version,
    None => env!("CARGO_PKG_VERSION"),
};

pub(crate) const BROWSE_FIELDS: &str = "Overview,PrimaryImageAspectRatio,UserData,DateCreated,PremiereDate,DateLastSaved,ProviderIds,ParentId,SortName,CanEditItems,CanDelete";
pub(crate) const ITEM_FIELDS: &str = "Overview,PrimaryImageAspectRatio,UserData,DateCreated,PremiereDate,DateLastSaved,ProviderIds,ParentId,SortName,Chapters,CanEditItems,CanDelete";

pub(crate) fn parse_refresh_progress_message(value: &Value) -> Option<RefreshProgress> {
    let message_type = value.get("MessageType")?.as_str()?;
    if !matches!(message_type, "RefreshProgress" | "RefreshCompleted") {
        return None;
    }
    let data = value.get("Data")?;
    let item_id = data.get("ItemId")?.as_str()?.trim();
    if item_id.is_empty() {
        return None;
    }
    let completion_message = message_type == "RefreshCompleted";
    let progress = if completion_message {
        100.0
    } else {
        let progress = data.get("Progress")?;
        let parsed = progress
            .as_f64()
            .or_else(|| progress.as_str()?.trim().parse::<f64>().ok())?;
        if !parsed.is_finite() {
            return None;
        }
        parsed.clamp(0.0, 100.0)
    };
    Some(RefreshProgress {
        item_id: item_id.to_owned(),
        progress,
        complete: completion_message || progress >= 100.0,
        received_at: std::time::Instant::now(),
    })
}

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
            version: BUILD_VERSION.to_owned(),
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
    pub can_edit_items: Option<bool>,
}

#[derive(Debug, Clone, Copy)]
pub struct RemoteImageQuery<'a> {
    pub image_type: &'a str,
    pub provider_name: Option<&'a str>,
    pub include_all_languages: bool,
    pub enable_series_images: bool,
    pub start_index: u32,
    pub limit: u32,
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
    #[error("invalid Emby server profile: {0}")]
    InvalidProfile(#[from] ProfileError),
    #[error("Emby returned malformed JSON: {0}")]
    InvalidJson(#[from] serde_json::Error),
    #[error("Emby response exceeded the {limit_bytes}-byte safety limit")]
    ResponseTooLarge { limit_bytes: usize },
}

/// HTTP client scoped to exactly one Emby server and, after login, one user.
#[derive(Clone)]
pub struct EmbyClient {
    pub(crate) http: reqwest::Client,
    pub(crate) profile: ServerProfile,
    pub(crate) identity: ClientIdentity,
    pub(crate) user_id: Option<String>,
    pub(crate) token: Option<SecretString>,
}

impl EmbyClient {
    pub fn new(profile: ServerProfile, identity: ClientIdentity) -> Result<Self, EmbyError> {
        profile.validate()?;
        if !matches!(profile.tls_policy, TlsPolicy::Strict) {
            return Err(EmbyError::CertificatePinningUnavailable);
        }
        let trusted_origin = profile.base_url.clone();
        let http = reqwest::Client::builder()
            .timeout(Duration::from_secs(30))
            .redirect(reqwest::redirect::Policy::custom(move |attempt| {
                if attempt.previous().len() > 5 {
                    attempt.error("too many same-origin redirects")
                } else if same_origin(&trusted_origin, attempt.url()) {
                    attempt.follow()
                } else {
                    attempt.error("cross-origin redirect rejected")
                }
            }))
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

    pub(crate) fn request(&self, method: reqwest::Method, path: &str) -> reqwest::RequestBuilder {
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
