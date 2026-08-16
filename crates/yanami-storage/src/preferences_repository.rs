use rusqlite::OptionalExtension;

use crate::{AppStorage, StorageError};

impl AppStorage {
    pub fn set_preference<T: serde::Serialize>(
        &self,
        key: &str,
        value: &T,
    ) -> Result<(), StorageError> {
        let json = serde_json::to_string(value)?;
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute(
                "INSERT INTO preferences(key, value_json) VALUES(?1, ?2) \
                 ON CONFLICT(key) DO UPDATE SET value_json=excluded.value_json",
                rusqlite::params![key, json],
            )?;
        Ok(())
    }

    pub fn delete_preference(&self, key: &str) -> Result<(), StorageError> {
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute("DELETE FROM preferences WHERE key=?1", [key])?;
        Ok(())
    }

    pub fn preference<T: serde::de::DeserializeOwned>(
        &self,
        key: &str,
    ) -> Result<Option<T>, StorageError> {
        let json: Option<String> = self
            .connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .query_row(
                "SELECT value_json FROM preferences WHERE key=?1",
                [key],
                |row| row.get(0),
            )
            .optional()?;
        json.map(|value| serde_json::from_str(&value).map_err(StorageError::from))
            .transpose()
    }
}
