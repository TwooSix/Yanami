use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct PlaybackInfo {
    #[serde(default)]
    pub media_sources: Vec<MediaSource>,
    pub play_session_id: String,
    #[serde(default)]
    pub error_code: Option<String>,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ChapterInfo {
    #[serde(default)]
    pub start_position_ticks: u64,
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default)]
    pub marker_type: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct MediaSource {
    pub id: String,
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default)]
    pub path: Option<String>,
    #[serde(default)]
    pub container: Option<String>,
    #[serde(default)]
    pub size: Option<u64>,
    #[serde(default)]
    pub run_time_ticks: Option<u64>,
    #[serde(default)]
    pub bitrate: Option<u64>,
    #[serde(default)]
    pub supports_direct_play: bool,
    #[serde(default)]
    pub supports_direct_stream: bool,
    #[serde(default)]
    pub supports_transcoding: bool,
    #[serde(default)]
    pub direct_stream_url: Option<String>,
    #[serde(default)]
    pub transcoding_url: Option<String>,
    #[serde(default)]
    pub required_http_headers: BTreeMap<String, String>,
    #[serde(default)]
    pub default_audio_stream_index: Option<i32>,
    #[serde(default)]
    pub default_subtitle_stream_index: Option<i32>,
    #[serde(default)]
    pub media_streams: Vec<MediaStream>,
    #[serde(default)]
    pub chapters: Vec<ChapterInfo>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct MediaStream {
    #[serde(default)]
    pub index: i32,
    #[serde(default, rename = "Type")]
    pub stream_type: String,
    #[serde(default)]
    pub codec: Option<String>,
    #[serde(default)]
    pub language: Option<String>,
    #[serde(default)]
    pub display_title: Option<String>,
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub is_external: bool,
    #[serde(default)]
    pub supports_external_stream: bool,
    #[serde(default)]
    pub delivery_url: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct PlaybackProgress<'a> {
    pub item_id: &'a str,
    pub media_source_id: &'a str,
    pub play_session_id: &'a str,
    pub position_ticks: u64,
    pub is_paused: bool,
    pub is_muted: bool,
    pub volume_level: u8,
    pub playback_rate: f64,
    pub play_method: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub audio_stream_index: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub subtitle_stream_index: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub event_name: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub playlist_item_id: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub playlist_index: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub playlist_length: Option<i32>,
    pub can_seek: bool,
}
