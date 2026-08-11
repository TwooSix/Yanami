use std::{
    collections::BTreeMap,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use base64::{Engine as _, engine::general_purpose::STANDARD};
use md5::{Digest as _, Md5};
use secrecy::{ExposeSecret, SecretString};
use serde::Serialize;
use sha2::Sha256;
use thiserror::Error;
use url::Url;

use crate::model::{
    CommentResponse, DanmakuComment, EpisodeMatch, EpisodeSearchResult, MatchInput, MatchResponse,
    SearchEpisodesInput, SearchEpisodesResponse,
};

const API_BASE: &str = "https://api.dandanplay.net/api/v2/";
const HASH_BYTES: usize = 16 * 1024 * 1024;
const USER_AGENT: &str = concat!("Yanami/", env!("CARGO_PKG_VERSION"));

pub struct DandanCredentials {
    app_id: String,
    app_secret: SecretString,
}

impl DandanCredentials {
    pub fn new(app_id: impl Into<String>, app_secret: SecretString) -> Self {
        Self {
            app_id: app_id.into(),
            app_secret,
        }
    }

    pub fn app_id(&self) -> &str {
        &self.app_id
    }

    pub fn signature(&self, timestamp: i64, path: &str) -> String {
        use sha2::Digest;

        let normalized_path = path.to_ascii_lowercase();
        let payload = format!(
            "{}{}{}{}",
            self.app_id,
            timestamp,
            normalized_path,
            self.app_secret.expose_secret()
        );
        STANDARD.encode(Sha256::digest(payload.as_bytes()))
    }
}

#[derive(Debug, Error)]
pub enum DandanError {
    #[error("DanDanPlay request failed: {0}")]
    Transport(#[from] reqwest::Error),
    #[error("DanDanPlay returned HTTP {status}: {message}")]
    Http { status: u16, message: String },
    #[error("DanDanPlay error {code:?}: {message}")]
    Api { code: Option<i32>, message: String },
    #[error("media server did not return any bytes for hashing")]
    EmptyMedia,
    #[error("system clock is earlier than the Unix epoch")]
    InvalidClock,
    #[error("invalid DanDanPlay API URL")]
    InvalidApiUrl,
}

pub struct DandanClient {
    http: reqwest::Client,
    base_url: Url,
    credentials: DandanCredentials,
}

impl DandanClient {
    pub fn new(credentials: DandanCredentials) -> Result<Self, DandanError> {
        Ok(Self {
            http: reqwest::Client::builder()
                .timeout(Duration::from_secs(20))
                .user_agent(USER_AGENT)
                .build()?,
            base_url: Url::parse(API_BASE).map_err(|_| DandanError::InvalidApiUrl)?,
            credentials,
        })
    }

    pub fn with_base_url(
        credentials: DandanCredentials,
        mut base_url: Url,
    ) -> Result<Self, DandanError> {
        if !base_url.path().ends_with('/') {
            let path = format!("{}/", base_url.path());
            base_url.set_path(&path);
        }
        Ok(Self {
            http: reqwest::Client::builder()
                .timeout(Duration::from_secs(20))
                .user_agent(USER_AGENT)
                .build()?,
            base_url,
            credentials,
        })
    }

    /// Performs a harmless search request to verify user-provided credentials.
    pub async fn validate_credentials(&self) -> Result<(), DandanError> {
        let path = "/api/v2/search/episodes";
        let response = self
            .signed_request(reqwest::Method::GET, path)?
            .query(&[("anime", "__yanami_credential_check__")])
            .send()
            .await?;
        let response: serde_json::Value = decode(response).await?;
        if response.get("success").and_then(serde_json::Value::as_bool) == Some(false) {
            return Err(DandanError::Api {
                code: response
                    .get("errorCode")
                    .and_then(serde_json::Value::as_i64)
                    .and_then(|code| i32::try_from(code).ok()),
                message: response
                    .get("errorMessage")
                    .and_then(serde_json::Value::as_str)
                    .unwrap_or("credential validation failed")
                    .to_owned(),
            });
        }
        Ok(())
    }

    pub async fn match_media(&self, input: &MatchInput) -> Result<Vec<EpisodeMatch>, DandanError> {
        #[derive(Serialize)]
        #[serde(rename_all = "camelCase")]
        struct Body<'a> {
            file_name: &'a str,
            file_hash: &'a str,
            file_size: u64,
            video_duration: u64,
            match_mode: &'static str,
        }

        let response = self
            .signed_request(reqwest::Method::POST, "/api/v2/match")?
            .json(&Body {
                file_name: &input.file_name,
                file_hash: &input.file_hash,
                file_size: input.file_size,
                video_duration: input.video_duration_seconds,
                match_mode: "hashAndFileName",
            })
            .send()
            .await?;
        let result: MatchResponse = decode(response).await?;
        if !result.success {
            return Err(DandanError::Api {
                code: result.error_code,
                message: result
                    .error_message
                    .unwrap_or_else(|| "match failed".to_owned()),
            });
        }
        if result.is_matched || !result.matches.is_empty() {
            Ok(result.matches)
        } else {
            Ok(Vec::new())
        }
    }

    pub async fn fetch_comments(
        &self,
        episode_id: i64,
    ) -> Result<Vec<DanmakuComment>, DandanError> {
        let path = format!("/api/v2/comment/{episode_id}");
        let response = self
            .signed_request(reqwest::Method::GET, &path)?
            .query(&[("withRelated", "true")])
            .send()
            .await?;
        let result: CommentResponse = decode(response).await?;
        if !result.success {
            return Err(DandanError::Api {
                code: result.error_code,
                message: result
                    .error_message
                    .unwrap_or_else(|| "comment request failed".to_owned()),
            });
        }
        Ok(result
            .comments
            .into_iter()
            .filter_map(super::model::RawComment::parse)
            .collect())
    }

    /// Searches episodes for an explicit, user-initiated manual match.
    ///
    /// Callers should not use this method for background crawling or bulk
    /// discovery. The official API asks clients to call search on demand.
    pub async fn search_episodes(
        &self,
        input: &SearchEpisodesInput,
    ) -> Result<Vec<EpisodeSearchResult>, DandanError> {
        let path = "/api/v2/search/episodes";
        let mut request = self
            .signed_request(reqwest::Method::GET, path)?
            .query(&[("anime", input.anime.trim()), ("v2", "true")]);
        if let Some(episode) = input
            .episode
            .as_deref()
            .map(str::trim)
            .filter(|episode| !episode.is_empty())
        {
            request = request.query(&[("episode", episode)]);
        }
        let result: SearchEpisodesResponse = decode(request.send().await?).await?;
        if !result.success {
            return Err(DandanError::Api {
                code: result.error_code,
                message: result
                    .error_message
                    .unwrap_or_else(|| "episode search failed".to_owned()),
            });
        }

        Ok(result
            .animes
            .into_iter()
            .flat_map(|anime| {
                anime
                    .episodes
                    .into_iter()
                    .map(move |episode| EpisodeSearchResult {
                        anime_id: anime.anime_id,
                        anime_title: anime.anime_title.clone(),
                        type_description: anime.type_description.clone(),
                        episode_id: episode.episode_id,
                        episode_title: episode.episode_title,
                    })
            })
            .collect())
    }

    fn signed_request(
        &self,
        method: reqwest::Method,
        path: &str,
    ) -> Result<reqwest::RequestBuilder, DandanError> {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| DandanError::InvalidClock)?
            .as_secs()
            .try_into()
            .map_err(|_| DandanError::InvalidClock)?;
        let signature = self.credentials.signature(timestamp, path);
        let url = self
            .base_url
            .join(path.trim_start_matches("/api/v2/"))
            .map_err(|_| DandanError::InvalidApiUrl)?;
        Ok(self
            .http
            .request(method, url)
            .header("Accept", "application/json")
            .header("X-AppId", self.credentials.app_id())
            .header("X-Timestamp", timestamp.to_string())
            .header("X-Signature", signature))
    }
}

/// Downloads at most the first 16 MiB of an authenticated media stream and returns its MD5.
pub async fn hash_remote_prefix(
    client: &reqwest::Client,
    url: Url,
    headers: &BTreeMap<String, String>,
) -> Result<String, DandanError> {
    let mut request = client
        .get(url)
        .header("Range", format!("bytes=0-{}", HASH_BYTES - 1));
    for (name, value) in headers {
        request = request.header(name, value);
    }
    let response = request.send().await?;
    let mut response = ensure_http(response).await?;
    let mut hasher = Md5::new();
    let mut received = 0_usize;
    while received < HASH_BYTES {
        let Some(chunk) = response.chunk().await? else {
            break;
        };
        let take = chunk.len().min(HASH_BYTES - received);
        hasher.update(&chunk[..take]);
        received += take;
    }
    if received == 0 {
        return Err(DandanError::EmptyMedia);
    }
    let digest = hasher.finalize();
    Ok(format!("{digest:x}"))
}

async fn decode<T: serde::de::DeserializeOwned>(
    response: reqwest::Response,
) -> Result<T, DandanError> {
    Ok(ensure_http(response).await?.json().await?)
}

async fn ensure_http(response: reqwest::Response) -> Result<reqwest::Response, DandanError> {
    let status = response.status();
    if status.is_success() {
        Ok(response)
    } else {
        let header_message = response
            .headers()
            .get("X-Error-Message")
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned);
        let body = response.text().await.unwrap_or_default();
        Err(DandanError::Http {
            status: status.as_u16(),
            message: header_message
                .filter(|message| !message.is_empty())
                .unwrap_or(body),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signature_uses_lowercase_path_and_hides_secret() {
        let credentials = DandanCredentials::new("app", SecretString::from("secret"));
        assert_eq!(
            credentials.signature(1_735_660_800, "/API/V2/Comment/123"),
            credentials.signature(1_735_660_800, "/api/v2/comment/123")
        );
        assert!(
            !credentials
                .signature(1_735_660_800, "/api/v2/comment/123")
                .contains("secret")
        );
    }
}
