use std::time::Instant;

use serde::Deserialize;

#[derive(Debug, Clone, PartialEq)]
pub struct RefreshProgress {
    pub item_id: String,
    pub progress: f64,
    pub complete: bool,
    pub received_at: Instant,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LibraryChange {
    pub items_added: Vec<String>,
    pub items_updated: Vec<String>,
    pub items_removed: Vec<String>,
    /// Folder-level changes can represent an unbounded subtree and therefore
    /// require an ID-only membership reconciliation in addition to item IDs.
    pub requires_membership: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct UserDataChange {
    pub user_id: String,
    pub item_ids: Vec<String>,
    /// A malformed or incomplete active-user payload cannot be acknowledged
    /// by ID and must be repaired with the timestamp delta endpoint.
    pub requires_catchup: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub enum EmbyNotification {
    RefreshProgress(RefreshProgress),
    LibraryChanged(LibraryChange),
    UserDataChanged(UserDataChange),
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
    pub configuration: UserConfiguration,
    #[serde(default)]
    pub policy: UserPolicy,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct UserConfiguration {
    /// Library views the user has hidden from server-backed Latest Media rows.
    #[serde(default)]
    pub latest_items_excludes: Vec<String>,
    /// Mirrors the Emby Web preference for omitting played Latest Media.
    #[serde(default)]
    pub hide_played_in_latest: bool,
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
