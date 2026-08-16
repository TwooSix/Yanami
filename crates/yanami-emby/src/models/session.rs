use std::time::Instant;

use serde::Deserialize;

#[derive(Debug, Clone, PartialEq)]
pub struct RefreshProgress {
    pub item_id: String,
    pub progress: f64,
    pub complete: bool,
    pub received_at: Instant,
}

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
    #[serde(default)]
    pub policy: UserPolicy,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct UserPolicy {
    #[serde(default)]
    pub is_administrator: bool,
    #[serde(default)]
    pub enable_content_downloading: bool,
    #[serde(default)]
    pub enable_content_deletion: bool,
}
