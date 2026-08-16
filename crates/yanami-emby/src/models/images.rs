use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ImageInfo {
    #[serde(default)]
    pub image_type: String,
    #[serde(default)]
    pub image_index: u32,
    /// Present on some Emby-compatible servers even though older Emby
    /// `ImageInfo` schemas omit it.  The item DTO remains the authoritative
    /// fallback for cache invalidation.
    #[serde(default)]
    pub image_tag: Option<String>,
    #[serde(default)]
    pub path: Option<String>,
    #[serde(default)]
    pub filename: Option<String>,
    #[serde(default)]
    pub height: Option<u32>,
    #[serde(default)]
    pub width: Option<u32>,
    #[serde(default)]
    pub size: Option<u64>,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ImageProviderInfo {
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub supported_images: Vec<String>,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct RemoteImageInfo {
    #[serde(default)]
    pub provider_name: String,
    #[serde(default)]
    pub url: String,
    #[serde(default)]
    pub thumbnail_url: Option<String>,
    #[serde(default)]
    pub height: Option<u32>,
    #[serde(default)]
    pub width: Option<u32>,
    #[serde(default)]
    pub community_rating: Option<f64>,
    #[serde(default)]
    pub vote_count: Option<u64>,
    #[serde(default)]
    pub language: Option<String>,
    #[serde(default)]
    pub display_language: Option<String>,
    #[serde(default)]
    pub image_type: String,
}

#[derive(Debug, Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct RemoteImageResult {
    #[serde(default)]
    pub images: Vec<RemoteImageInfo>,
    #[serde(default)]
    pub total_record_count: u64,
    #[serde(default)]
    pub providers: Vec<String>,
}
