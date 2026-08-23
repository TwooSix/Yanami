use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use super::playback::ChapterInfo;

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ItemsResult {
    #[serde(default)]
    pub items: Vec<BaseItem>,
    #[serde(default)]
    pub total_record_count: u64,
    #[serde(default)]
    pub start_index: u64,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ItemCreationResult {
    pub id: String,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct BaseItem {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub playlist_item_id: Option<String>,
    #[serde(default)]
    pub can_edit_items: Option<bool>,
    #[serde(default)]
    pub can_delete: Option<bool>,
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
    pub date_last_saved: Option<String>,
    #[serde(default)]
    pub provider_ids: BTreeMap<String, String>,
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
