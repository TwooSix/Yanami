use std::{collections::BTreeMap, fmt, ops::Deref};

use http::{HeaderName, HeaderValue};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::SameOriginUrl;

#[derive(Clone, PartialEq, Eq, Default)]
pub struct PlaybackHeaders(BTreeMap<String, String>);

#[derive(Debug, Clone, Copy, Error, PartialEq, Eq)]
#[error("playback headers contain an invalid or disallowed value")]
pub struct PlaybackHeaderError;

impl PlaybackHeaders {
    /// Builds the read-only header set accepted by the native playback adapter.
    ///
    /// Account credentials and arbitrary server-provided headers are rejected
    /// because libmpv applies its header list to redirects and child requests.
    pub fn try_from_map(headers: &BTreeMap<String, String>) -> Result<Self, PlaybackHeaderError> {
        let mut safe = BTreeMap::new();
        for (name, value) in headers {
            let parsed_name =
                HeaderName::from_bytes(name.as_bytes()).map_err(|_| PlaybackHeaderError)?;
            HeaderValue::from_str(value).map_err(|_| PlaybackHeaderError)?;
            if !matches!(
                parsed_name.as_str(),
                "accept"
                    | "accept-encoding"
                    | "accept-language"
                    | "cache-control"
                    | "pragma"
                    | "user-agent"
            ) {
                return Err(PlaybackHeaderError);
            }
            safe.insert(parsed_name.as_str().to_owned(), value.clone());
        }
        Ok(Self(safe))
    }
}

impl Deref for PlaybackHeaders {
    type Target = BTreeMap<String, String>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl fmt::Debug for PlaybackHeaders {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PlaybackHeaders")
            .field("count", &self.0.len())
            .field("values", &"[REDACTED]")
            .finish()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum PlaybackMethod {
    DirectStream,
    Transcode,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum TrackKind {
    Video,
    Audio,
    Subtitle,
}

#[derive(Clone, PartialEq, Eq, Serialize)]
pub struct MediaTrack {
    pub index: i32,
    pub kind: TrackKind,
    pub codec: Option<String>,
    pub language: Option<String>,
    pub title: Option<String>,
    pub external: bool,
    pub delivery_url: Option<SameOriginUrl>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub enum PlaybackWarning {
    ExternalSubtitleUnavailable { index: i32 },
}

impl fmt::Debug for MediaTrack {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("MediaTrack")
            .field("index", &self.index)
            .field("kind", &self.kind)
            .field("codec", &self.codec)
            .field("language", &self.language)
            .field("title", &self.title)
            .field("external", &self.external)
            .field(
                "delivery_url",
                &self.delivery_url.as_ref().map(|_| "[REDACTED]"),
            )
            .finish()
    }
}

#[derive(Clone, PartialEq, Eq, Serialize)]
pub struct PlaybackPlan {
    pub item_id: String,
    pub media_source_id: String,
    pub play_session_id: String,
    pub method: PlaybackMethod,
    pub url: SameOriginUrl,
    /// Headers passed to libmpv. Values are never persisted or logged.
    #[serde(skip)]
    pub request_headers: PlaybackHeaders,
    pub resume_position_ticks: u64,
    pub audio_stream_index: Option<i32>,
    pub subtitle_stream_index: Option<i32>,
    pub tracks: Vec<MediaTrack>,
    pub warnings: Vec<PlaybackWarning>,
}

impl fmt::Debug for PlaybackPlan {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PlaybackPlan")
            .field("item_id", &self.item_id)
            .field("media_source_id", &self.media_source_id)
            .field("play_session_id", &self.play_session_id)
            .field("method", &self.method)
            .field("url", &"[REDACTED]")
            .field("request_headers", &self.request_headers)
            .field("resume_position_ticks", &self.resume_position_ticks)
            .field("audio_stream_index", &self.audio_stream_index)
            .field("subtitle_stream_index", &self.subtitle_stream_index)
            .field("tracks", &self.tracks)
            .field("warnings", &self.warnings)
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn playback_headers_enforce_the_native_allowlist() {
        let safe = BTreeMap::from([("User-Agent".to_owned(), "Yanami test".to_owned())]);
        let headers = PlaybackHeaders::try_from_map(&safe).unwrap();
        assert_eq!(headers["user-agent"], "Yanami test");

        for unsafe_headers in [
            BTreeMap::from([("X-Emby-Token".to_owned(), "secret".to_owned())]),
            BTreeMap::from([("User-Agent\r\nInjected".to_owned(), "value".to_owned())]),
            BTreeMap::from([("User-Agent".to_owned(), "value\r\nInjected".to_owned())]),
        ] {
            assert_eq!(
                PlaybackHeaders::try_from_map(&unsafe_headers),
                Err(PlaybackHeaderError)
            );
        }
    }

    #[test]
    fn playback_header_debug_output_is_redacted() {
        let headers = PlaybackHeaders::try_from_map(&BTreeMap::from([(
            "User-Agent".to_owned(),
            "secret marker".to_owned(),
        )]))
        .unwrap();
        let debug = format!("{headers:?}");
        assert!(debug.contains("REDACTED"));
        assert!(!debug.contains("secret marker"));
    }
}
