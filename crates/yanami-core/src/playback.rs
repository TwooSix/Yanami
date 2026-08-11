use std::{
    collections::BTreeMap,
    fmt,
    ops::{Deref, DerefMut},
};

use serde::{Deserialize, Serialize};
use url::Url;

#[derive(Clone, PartialEq, Eq, Default)]
pub struct SensitiveHeaders(BTreeMap<String, String>);

impl SensitiveHeaders {
    pub fn insert(&mut self, name: String, value: String) -> Option<String> {
        self.0.insert(name, value)
    }
}

impl From<BTreeMap<String, String>> for SensitiveHeaders {
    fn from(value: BTreeMap<String, String>) -> Self {
        Self(value)
    }
}

impl Deref for SensitiveHeaders {
    type Target = BTreeMap<String, String>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl DerefMut for SensitiveHeaders {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl fmt::Debug for SensitiveHeaders {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("SensitiveHeaders")
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

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MediaTrack {
    pub index: i32,
    pub kind: TrackKind,
    pub codec: Option<String>,
    pub language: Option<String>,
    pub title: Option<String>,
    pub external: bool,
    pub delivery_url: Option<Url>,
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

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlaybackPlan {
    pub item_id: String,
    pub media_source_id: String,
    pub play_session_id: String,
    pub method: PlaybackMethod,
    pub url: Url,
    /// Headers passed to libmpv. Values are never persisted or logged.
    #[serde(skip)]
    pub request_headers: SensitiveHeaders,
    pub resume_position_ticks: u64,
    pub audio_stream_index: Option<i32>,
    pub subtitle_stream_index: Option<i32>,
    pub tracks: Vec<MediaTrack>,
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
            .finish()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum PlayerCommand {
    Load(PlaybackPlan),
    Play,
    Pause,
    Seek(f64),
    Stop,
    SetVolume(f64),
    SetRate(f64),
    SelectAudio(Option<i32>),
    SelectSubtitle(Option<i32>),
    AddSubtitle { url: Url, title: String },
    SetDanmakuTrack(Option<Url>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PlayerState {
    #[default]
    Idle,
    Loading,
    Playing,
    Paused,
    Buffering,
    Ended,
    Failed,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct PlayerSnapshot {
    pub state: PlayerState,
    pub position_seconds: f64,
    pub duration_seconds: f64,
    pub volume: f64,
    pub rate: f64,
    pub audio_stream_index: Option<i32>,
    pub subtitle_stream_index: Option<i32>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum PlayerEvent {
    Snapshot(PlayerSnapshot),
    TracksChanged(Vec<MediaTrack>),
    EndFile,
    Error(String),
}
