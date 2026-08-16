use std::time::{Duration, SystemTime, UNIX_EPOCH};

use reqwest::{StatusCode, header::LOCATION};
use serde::Serialize;
use url::Url;
use yanami_core::DanmakuComment;

use crate::{
    credentials::DandanCredentials,
    error::DandanError,
    model::{
        AnimeEpisodeSearchResult, AnimeSearchResult, CommentResponse, EpisodeMatch,
        EpisodeSearchResult, MatchInput, MatchResponse, SearchEpisodesInput,
        SearchEpisodesResponse,
    },
    transport::{decode, same_origin_redirect_policy},
};

const API_BASE: &str = "https://api.dandanplay.net/api/v2/";
const BUILD_VERSION: &str = match option_env!("YANAMI_BUILD_VERSION") {
    Some(version) => version,
    None => env!("CARGO_PKG_VERSION"),
};
const MAX_COMMENT_REDIRECTS: usize = 3;

pub struct DandanClient {
    http: reqwest::Client,
    comment_http: reqwest::Client,
    base_url: Url,
    credentials: DandanCredentials,
}

impl DandanClient {
    pub fn new(credentials: DandanCredentials) -> Result<Self, DandanError> {
        let base_url = Url::parse(API_BASE).map_err(|_| DandanError::InvalidApiUrl)?;
        Ok(Self {
            http: reqwest::Client::builder()
                .timeout(Duration::from_secs(20))
                .user_agent(format!("Yanami/{BUILD_VERSION}"))
                .redirect(same_origin_redirect_policy(&base_url))
                .build()?,
            comment_http: comment_http_client()?,
            base_url,
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
        let redirect_policy = same_origin_redirect_policy(&base_url);
        Ok(Self {
            http: reqwest::Client::builder()
                .timeout(Duration::from_secs(20))
                .user_agent(format!("Yanami/{BUILD_VERSION}"))
                .redirect(redirect_policy)
                .build()?,
            comment_http: comment_http_client()?,
            base_url,
            credentials,
        })
    }

    /// Performs a harmless search request to verify user-provided credentials.
    pub async fn validate_credentials(&self) -> Result<(), DandanError> {
        let path = "/api/v2/search/episodes";
        let response = self
            .send_with_retry(|| {
                Ok(self
                    .signed_request(reqwest::Method::GET, path)?
                    .query(&[("anime", "__yanami_credential_check__")]))
            })
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
            .send_with_retry(|| {
                Ok(self
                    .signed_request(reqwest::Method::POST, "/api/v2/match")?
                    .json(&Body {
                        file_name: &input.file_name,
                        file_hash: &input.file_hash,
                        file_size: input.file_size,
                        video_duration: input.video_duration_seconds,
                        match_mode: "hashAndFileName",
                    }))
            })
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
            .send_with_retry(|| {
                Ok(self
                    .signed_request_with(&self.comment_http, reqwest::Method::GET, &path)?
                    .query(&[("withRelated", "true")]))
            })
            .await?;
        let response = self.follow_comment_redirects(response, episode_id).await?;
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
        let episode = input
            .episode
            .as_deref()
            .map(str::trim)
            .filter(|episode| !episode.is_empty());
        let response = self
            .send_with_retry(|| {
                let mut request = self
                    .signed_request(reqwest::Method::GET, path)?
                    .query(&[("anime", input.anime.trim()), ("v2", "true")]);
                if let Some(episode) = episode {
                    request = request.query(&[("episode", episode)]);
                }
                Ok(request)
            })
            .await?;
        let result: SearchEpisodesResponse = decode(response).await?;
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

    /// Searches anime titles and preserves the API's title -> episode hierarchy.
    /// This is used by the manual picker so seasons are never flattened together.
    pub async fn search_animes(
        &self,
        input: &SearchEpisodesInput,
    ) -> Result<Vec<AnimeSearchResult>, DandanError> {
        let path = "/api/v2/search/episodes";
        let response = self
            .send_with_retry(|| {
                Ok(self
                    .signed_request(reqwest::Method::GET, path)?
                    .query(&[("anime", input.anime.trim()), ("v2", "true")]))
            })
            .await?;
        let result: SearchEpisodesResponse = decode(response).await?;
        if !result.success {
            return Err(DandanError::Api {
                code: result.error_code,
                message: result
                    .error_message
                    .unwrap_or_else(|| "anime search failed".to_owned()),
            });
        }
        Ok(result
            .animes
            .into_iter()
            .map(|anime| AnimeSearchResult {
                anime_id: anime.anime_id,
                anime_title: anime.anime_title,
                type_description: anime.type_description,
                episodes: anime
                    .episodes
                    .into_iter()
                    .map(|episode| AnimeEpisodeSearchResult {
                        episode_id: episode.episode_id,
                        episode_title: episode.episode_title,
                    })
                    .collect(),
            })
            .collect())
    }

    fn signed_request(
        &self,
        method: reqwest::Method,
        path: &str,
    ) -> Result<reqwest::RequestBuilder, DandanError> {
        self.signed_request_with(&self.http, method, path)
    }

    fn signed_request_with(
        &self,
        http: &reqwest::Client,
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
        Ok(http
            .request(method, url)
            .header("Accept", "application/json")
            .header("X-AppId", self.credentials.app_id())
            .header("X-Timestamp", timestamp.to_string())
            .header("X-Signature", signature))
    }

    async fn follow_comment_redirects(
        &self,
        mut response: reqwest::Response,
        episode_id: i64,
    ) -> Result<reqwest::Response, DandanError> {
        for redirect_index in 0..MAX_COMMENT_REDIRECTS {
            if !is_followable_redirect(response.status()) {
                return Ok(response);
            }
            let target = comment_redirect_target(&self.base_url, &response)?;
            tracing::debug!(
                episode_id,
                redirect_index,
                status = response.status().as_u16(),
                host = target.host_str().unwrap_or_default(),
                "following DanDanPlay comment download redirect"
            );
            response = self
                .send_with_retry(|| {
                    Ok(self
                        .comment_http
                        .get(target.clone())
                        .header("Accept", "application/json"))
                })
                .await?;
        }
        if is_followable_redirect(response.status()) {
            Err(DandanError::TooManyCommentRedirects)
        } else {
            Ok(response)
        }
    }

    async fn send_with_retry<F>(&self, mut build: F) -> Result<reqwest::Response, DandanError>
    where
        F: FnMut() -> Result<reqwest::RequestBuilder, DandanError>,
    {
        for attempt in 0..2 {
            match build()?.send().await {
                Ok(response) => return Ok(response),
                Err(error) if attempt == 0 && (error.is_connect() || error.is_timeout()) => {
                    tokio::time::sleep(Duration::from_millis(220)).await;
                }
                Err(error) => return Err(error.into()),
            }
        }
        unreachable!("the retry loop always returns")
    }
}

fn comment_http_client() -> Result<reqwest::Client, DandanError> {
    Ok(reqwest::Client::builder()
        .timeout(Duration::from_secs(20))
        .user_agent(format!("Yanami/{BUILD_VERSION}"))
        .redirect(reqwest::redirect::Policy::none())
        .build()?)
}

fn is_followable_redirect(status: StatusCode) -> bool {
    matches!(
        status,
        StatusCode::MOVED_PERMANENTLY
            | StatusCode::FOUND
            | StatusCode::SEE_OTHER
            | StatusCode::TEMPORARY_REDIRECT
            | StatusCode::PERMANENT_REDIRECT
    )
}

fn comment_redirect_target(
    api_base_url: &Url,
    response: &reqwest::Response,
) -> Result<Url, DandanError> {
    let location = response
        .headers()
        .get(LOCATION)
        .and_then(|value| value.to_str().ok())
        .ok_or(DandanError::InvalidCommentRedirect)?;
    resolve_comment_redirect(api_base_url, response.url(), location)
}

fn resolve_comment_redirect(
    api_base_url: &Url,
    response_url: &Url,
    location: &str,
) -> Result<Url, DandanError> {
    let mut target = response_url
        .join(location)
        .map_err(|_| DandanError::InvalidCommentRedirect)?;
    let valid_scheme = match api_base_url.scheme() {
        "https" => target.scheme() == "https",
        "http" => matches!(target.scheme(), "http" | "https"),
        _ => false,
    };
    if !valid_scheme
        || target.host_str().is_none()
        || !target.username().is_empty()
        || target.password().is_some()
    {
        return Err(DandanError::InvalidCommentRedirect);
    }
    target.set_fragment(None);
    Ok(target)
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use secrecy::SecretString;
    use tokio::{
        io::{AsyncReadExt, AsyncWriteExt},
        net::{TcpListener, TcpStream},
    };

    use super::{DandanClient, DandanCredentials, DandanError, resolve_comment_redirect};

    async fn read_http_headers(stream: &mut TcpStream) -> String {
        let mut request = Vec::new();
        let mut chunk = [0_u8; 2048];
        loop {
            let count = stream.read(&mut chunk).await.unwrap();
            assert!(count > 0, "HTTP request ended before its headers");
            request.extend_from_slice(&chunk[..count]);
            if request.windows(4).any(|bytes| bytes == b"\r\n\r\n") {
                return String::from_utf8_lossy(&request).into_owned();
            }
        }
    }

    #[tokio::test]
    async fn follows_comment_redirect_without_forwarding_credentials() {
        let download_listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let download_address = download_listener.local_addr().unwrap();
        let download_server = tokio::spawn(async move {
            let (mut stream, _) = download_listener.accept().await.unwrap();
            let request = read_http_headers(&mut stream).await.to_ascii_lowercase();
            assert!(request.starts_with("get /api/comment/123?access=temporary http/1.1"));
            assert!(!request.contains("\r\nx-appid:"));
            assert!(!request.contains("\r\nx-timestamp:"));
            assert!(!request.contains("\r\nx-signature:"));

            let body =
                r#"{"count":1,"comments":[{"cid":3,"p":"1.5,1,16777215,user","m":"hello"}]}"#;
            stream
                .write_all(
                    format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                        body.len()
                    )
                    .as_bytes(),
                )
                .await
                .unwrap();
        });

        let api_listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let api_address = api_listener.local_addr().unwrap();
        let api_server = tokio::spawn(async move {
            let (mut stream, _) = api_listener.accept().await.unwrap();
            let request = read_http_headers(&mut stream).await.to_ascii_lowercase();
            assert!(request.starts_with("get /api/v2/comment/123?withrelated=true http/1.1"));
            assert!(request.contains("\r\nx-appid: app\r\n"));
            assert!(request.contains("\r\nx-timestamp:"));
            assert!(request.contains("\r\nx-signature:"));
            assert!(!request.contains("secret"));

            stream
                .write_all(
                    format!(
                        "HTTP/1.1 302 Found\r\nLocation: http://{download_address}/api/comment/123?access=temporary#ignored\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                    )
                    .as_bytes(),
                )
                .await
                .unwrap();
        });

        let client = DandanClient::with_base_url(
            DandanCredentials::new("app", SecretString::from("secret")),
            url::Url::parse(&format!("http://{api_address}/api/v2/")).unwrap(),
        )
        .unwrap();
        let comments = tokio::time::timeout(Duration::from_secs(3), client.fetch_comments(123))
            .await
            .unwrap()
            .unwrap();

        assert_eq!(comments.len(), 1);
        assert_eq!(comments[0].text, "hello");
        api_server.await.unwrap();
        download_server.await.unwrap();
    }

    #[test]
    fn rejects_https_comment_redirect_downgrade() {
        let api_base = url::Url::parse("https://api.dandanplay.net/api/v2/").unwrap();
        let response_url =
            url::Url::parse("https://api.dandanplay.net/api/v2/comment/123").unwrap();

        assert!(matches!(
            resolve_comment_redirect(
                &api_base,
                &response_url,
                "http://cas2.dandanplay.net/api/comment/123"
            ),
            Err(DandanError::InvalidCommentRedirect)
        ));
    }
}
