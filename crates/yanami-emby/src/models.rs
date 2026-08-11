use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

// Deliberately no `Debug`: the response owns the Emby access token.
#[derive(Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct AuthenticationResult {
    pub access_token: String,
    pub server_id: String,
    pub user: UserDto,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct UserDto {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub server_id: Option<String>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ItemsResult {
    #[serde(default)]
    pub items: Vec<BaseItem>,
    #[serde(default)]
    pub total_record_count: u64,
    #[serde(default)]
    pub start_index: u64,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct BaseItem {
    pub id: String,
    pub name: String,
    #[serde(default, rename = "Type")]
    pub item_type: Option<String>,
    #[serde(default)]
    pub collection_type: Option<String>,
    #[serde(default)]
    pub overview: Option<String>,
    #[serde(default)]
    pub production_year: Option<i32>,
    #[serde(default)]
    pub date_created: Option<String>,
    #[serde(default)]
    pub premiere_date: Option<String>,
    #[serde(default)]
    pub run_time_ticks: Option<u64>,
    #[serde(default)]
    pub series_name: Option<String>,
    #[serde(default)]
    pub index_number: Option<i32>,
    #[serde(default)]
    pub parent_index_number: Option<i32>,
    #[serde(default)]
    pub image_tags: BTreeMap<String, String>,
    #[serde(default)]
    pub primary_image_tag: Option<String>,
    #[serde(default)]
    pub series_id: Option<String>,
    #[serde(default)]
    pub season_id: Option<String>,
    #[serde(default)]
    pub season_name: Option<String>,
    #[serde(default)]
    pub series_primary_image_tag: Option<String>,
    #[serde(default)]
    pub parent_thumb_item_id: Option<String>,
    #[serde(default)]
    pub parent_thumb_image_tag: Option<String>,
    #[serde(default)]
    pub backdrop_image_tags: Vec<String>,
    #[serde(default)]
    pub parent_backdrop_item_id: Option<String>,
    #[serde(default)]
    pub parent_backdrop_image_tags: Vec<String>,
    #[serde(default)]
    pub child_count: Option<u32>,
    #[serde(default)]
    pub recursive_item_count: Option<u32>,
    #[serde(default)]
    pub location_type: Option<String>,
    #[serde(default)]
    pub primary_image_aspect_ratio: Option<f64>,
    #[serde(default)]
    pub chapters: Vec<ChapterInfo>,
    #[serde(default)]
    pub user_data: Option<UserItemData>,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct UserItemData {
    #[serde(default)]
    pub playback_position_ticks: u64,
    #[serde(default)]
    pub last_played_date: Option<String>,
    #[serde(default)]
    pub played: bool,
    #[serde(default)]
    pub is_favorite: bool,
    #[serde(default)]
    pub played_percentage: Option<f64>,
    #[serde(default)]
    pub unplayed_item_count: Option<u32>,
}

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
    pub can_seek: bool,
}
