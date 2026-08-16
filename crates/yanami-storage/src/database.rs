use std::{path::Path, sync::Mutex, time::Duration};

use rusqlite::Connection;
use thiserror::Error;

use yanami_core::ProfileError;

use crate::schema::{DATABASE_SCHEMA_VERSION, initialize_current_schema};

#[derive(Debug, Error)]
pub enum StorageError {
    #[error("database failure: {0}")]
    Database(#[from] rusqlite::Error),
    #[error("stored JSON is invalid: {0}")]
    Json(#[from] serde_json::Error),
    #[error("database lock is poisoned")]
    Poisoned,
    #[error("database schema version {found} is newer than supported version {supported}")]
    FutureSchema { found: i64, supported: i64 },
    #[error(
        "database schema version {found} is obsolete; only a new database or schema {supported} is supported"
    )]
    ObsoleteSchema { found: i64, supported: i64 },
    #[error("stored record schema version {found} is newer than supported version {supported}")]
    FutureRecord { found: u32, supported: u32 },
    #[error(
        "stored record schema version {found} is obsolete; only schema {supported} is supported"
    )]
    ObsoleteRecord { found: u32, supported: u32 },
    #[error("stored server profile is invalid: {0}")]
    InvalidProfile(#[from] ProfileError),
}

pub struct AppStorage {
    pub(crate) connection: Mutex<Connection>,
}

impl AppStorage {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, StorageError> {
        let path = path.as_ref();
        let existing_nonempty = path.metadata().is_ok_and(|metadata| metadata.len() > 0);
        Self::from_connection(Connection::open(path)?, existing_nonempty)
    }

    pub fn in_memory() -> Result<Self, StorageError> {
        Self::from_connection(Connection::open_in_memory()?, false)
    }

    fn from_connection(
        mut connection: Connection,
        existing_nonempty: bool,
    ) -> Result<Self, StorageError> {
        connection.busy_timeout(Duration::from_secs(5))?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        let schema_version: i64 =
            connection.pragma_query_value(None, "user_version", |row| row.get(0))?;
        if schema_version > DATABASE_SCHEMA_VERSION {
            return Err(StorageError::FutureSchema {
                found: schema_version,
                supported: DATABASE_SCHEMA_VERSION,
            });
        }
        if schema_version < DATABASE_SCHEMA_VERSION {
            if existing_nonempty {
                return Err(StorageError::ObsoleteSchema {
                    found: schema_version,
                    supported: DATABASE_SCHEMA_VERSION,
                });
            }
            initialize_current_schema(&mut connection)?;
        }
        connection.pragma_update(None, "journal_mode", "WAL")?;
        Ok(Self {
            connection: Mutex::new(connection),
        })
    }

    pub fn schema_version(&self) -> Result<i64, StorageError> {
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .map_err(StorageError::from)
    }
}
