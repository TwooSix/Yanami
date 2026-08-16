//! `SQLite` metadata storage and operating-system credential vault integration.

mod danmaku_repository;
mod database;
mod preferences_repository;
mod record_codec;
mod schema;
mod session_repository;
mod vault;

#[cfg(test)]
mod tests;

pub use danmaku_repository::{CachedComments, DanmakuMatchRecord};
pub use database::{AppStorage, StorageError};
pub use vault::{CredentialError, CredentialVault, MemoryCredentialVault, SystemCredentialVault};
