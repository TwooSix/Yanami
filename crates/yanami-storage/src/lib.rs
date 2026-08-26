//! `SQLite` metadata storage and operating-system credential vault integration.

mod danmaku_repository;
mod database;
mod media_catalog;
mod preferences_repository;
mod record_codec;
mod schema;
mod session_repository;
mod vault;

#[cfg(test)]
mod media_catalog_tests;
#[cfg(test)]
mod tests;

pub use danmaku_repository::{CachedComments, DanmakuMatchRecord};
pub use database::{AppStorage, StorageError};
pub use media_catalog::{
    CatalogItem, CatalogMembershipDiff, CatalogPendingChanges, CatalogScope, CatalogSearchHit,
    CatalogSearchPage, CatalogSyncRun, CatalogUserState, MediaCatalog, MediaCatalogError,
    SyncState, SyncStatus,
};
pub use vault::{CredentialError, CredentialVault, MemoryCredentialVault, SystemCredentialVault};
