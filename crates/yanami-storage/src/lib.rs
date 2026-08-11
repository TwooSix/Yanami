//! `SQLite` metadata storage and operating-system credential vault integration.

mod database;
mod vault;

pub use database::{AppStorage, CachedComments, DanmakuMatchRecord, StorageError};
pub use vault::{CredentialError, CredentialVault, MemoryCredentialVault, SystemCredentialVault};
