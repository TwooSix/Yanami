use std::{path::Path, sync::Mutex};

use rusqlite::{Connection, OptionalExtension, params};
use thiserror::Error;
use uuid::Uuid;

use yanami_core::{ServerProfile, UserSession};
use yanami_danmaku::DanmakuComment;

#[derive(Debug, Error)]
pub enum StorageError {
    #[error("database failure: {0}")]
    Database(#[from] rusqlite::Error),
    #[error("stored JSON is invalid: {0}")]
    Json(#[from] serde_json::Error),
    #[error("database lock is poisoned")]
    Poisoned,
}

#[derive(Debug, Clone, PartialEq)]
pub struct DanmakuMatchRecord {
    pub server_id: String,
    pub item_id: String,
    pub media_source_id: String,
    pub episode_id: i64,
    pub display_title: String,
    pub time_offset_seconds: f64,
    pub updated_at: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CachedComments {
    pub episode_id: i64,
    pub comments: Vec<DanmakuComment>,
    pub fetched_at: i64,
    pub expires_at: i64,
}

impl CachedComments {
    pub fn is_fresh_at(&self, timestamp: i64) -> bool {
        timestamp <= self.expires_at
    }

    pub fn is_usable_stale_at(&self, timestamp: i64) -> bool {
        timestamp <= self.fetched_at + 7 * 24 * 60 * 60
    }
}

pub struct AppStorage {
    connection: Mutex<Connection>,
}

impl AppStorage {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, StorageError> {
        Self::from_connection(Connection::open(path)?)
    }

    pub fn in_memory() -> Result<Self, StorageError> {
        Self::from_connection(Connection::open_in_memory()?)
    }

    fn from_connection(connection: Connection) -> Result<Self, StorageError> {
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.execute_batch(
            r"
            CREATE TABLE IF NOT EXISTS server_profiles (
                local_id TEXT PRIMARY KEY,
                profile_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS user_profiles (
                server_local_id TEXT NOT NULL,
                user_id TEXT NOT NULL,
                session_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (server_local_id, user_id),
                FOREIGN KEY (server_local_id) REFERENCES server_profiles(local_id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS danmaku_matches (
                server_id TEXT NOT NULL,
                item_id TEXT NOT NULL,
                media_source_id TEXT NOT NULL,
                episode_id INTEGER NOT NULL,
                display_title TEXT NOT NULL,
                time_offset_seconds REAL NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (server_id, item_id, media_source_id)
            );
            CREATE TABLE IF NOT EXISTS danmaku_cache (
                episode_id INTEGER PRIMARY KEY,
                comments_json TEXT NOT NULL,
                fetched_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS preferences (
                key TEXT PRIMARY KEY,
                value_json TEXT NOT NULL
            );
            ",
        )?;
        // Databases created by the earliest prototype predate the offset column.
        let has_time_offset = {
            let mut statement = connection.prepare("PRAGMA table_info(danmaku_matches)")?;
            let columns = statement.query_map([], |row| row.get::<_, String>(1))?;
            columns
                .collect::<Result<Vec<_>, _>>()?
                .iter()
                .any(|column| column == "time_offset_seconds")
        };
        if !has_time_offset {
            connection.execute(
                "ALTER TABLE danmaku_matches ADD COLUMN time_offset_seconds REAL NOT NULL DEFAULT 0",
                [],
            )?;
        }
        Ok(Self {
            connection: Mutex::new(connection),
        })
    }

    pub fn upsert_server(&self, profile: &ServerProfile, now: i64) -> Result<(), StorageError> {
        let json = serde_json::to_string(profile)?;
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute(
                "INSERT INTO server_profiles(local_id, profile_json, updated_at) VALUES(?1, ?2, ?3) \
                 ON CONFLICT(local_id) DO UPDATE SET profile_json=excluded.profile_json, updated_at=excluded.updated_at",
                params![profile.local_id.to_string(), json, now],
            )?;
        Ok(())
    }

    pub fn list_servers(&self) -> Result<Vec<ServerProfile>, StorageError> {
        let connection = self.connection.lock().map_err(|_| StorageError::Poisoned)?;
        let mut statement = connection.prepare(
            "SELECT profile_json FROM server_profiles ORDER BY updated_at DESC, local_id",
        )?;
        let rows = statement.query_map([], |row| row.get::<_, String>(0))?;
        rows.map(|row| Ok(serde_json::from_str(&row?)?)).collect()
    }

    pub fn delete_server(&self, local_id: Uuid) -> Result<(), StorageError> {
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute(
                "DELETE FROM server_profiles WHERE local_id=?1",
                [local_id.to_string()],
            )?;
        Ok(())
    }

    pub fn upsert_user(&self, session: &UserSession, now: i64) -> Result<(), StorageError> {
        let json = serde_json::to_string(session)?;
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute(
                "INSERT INTO user_profiles(server_local_id, user_id, session_json, updated_at) VALUES(?1, ?2, ?3, ?4) \
                 ON CONFLICT(server_local_id, user_id) DO UPDATE SET session_json=excluded.session_json, updated_at=excluded.updated_at",
                params![session.server_local_id.to_string(), session.user_id, json, now],
            )?;
        Ok(())
    }

    pub fn latest_user_session(&self) -> Result<Option<UserSession>, StorageError> {
        let json: Option<String> = self
            .connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .query_row(
                "SELECT session_json FROM user_profiles ORDER BY updated_at DESC LIMIT 1",
                [],
                |row| row.get(0),
            )
            .optional()?;
        json.map(|value| serde_json::from_str(&value).map_err(StorageError::from))
            .transpose()
    }

    pub fn put_match(&self, record: &DanmakuMatchRecord) -> Result<(), StorageError> {
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute(
                "INSERT INTO danmaku_matches(server_id, item_id, media_source_id, episode_id, display_title, time_offset_seconds, updated_at) \
                 VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) ON CONFLICT(server_id, item_id, media_source_id) DO UPDATE SET \
                 episode_id=excluded.episode_id, display_title=excluded.display_title, \
                 time_offset_seconds=excluded.time_offset_seconds, updated_at=excluded.updated_at",
                params![record.server_id, record.item_id, record.media_source_id, record.episode_id, record.display_title, record.time_offset_seconds, record.updated_at],
            )?;
        Ok(())
    }

    pub fn find_match(
        &self,
        server_id: &str,
        item_id: &str,
        media_source_id: &str,
    ) -> Result<Option<DanmakuMatchRecord>, StorageError> {
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .query_row(
                "SELECT episode_id, display_title, time_offset_seconds, updated_at FROM danmaku_matches \
                 WHERE server_id=?1 AND item_id=?2 AND media_source_id=?3",
                params![server_id, item_id, media_source_id],
                |row| {
                    Ok(DanmakuMatchRecord {
                        server_id: server_id.to_owned(),
                        item_id: item_id.to_owned(),
                        media_source_id: media_source_id.to_owned(),
                        episode_id: row.get(0)?,
                        display_title: row.get(1)?,
                        time_offset_seconds: row.get(2)?,
                        updated_at: row.get(3)?,
                    })
                },
            )
            .optional()
            .map_err(StorageError::from)
    }

    pub fn put_comments(&self, cache: &CachedComments) -> Result<(), StorageError> {
        let json = serde_json::to_string(&cache.comments)?;
        self.connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .execute(
                "INSERT INTO danmaku_cache(episode_id, comments_json, fetched_at, expires_at) VALUES(?1, ?2, ?3, ?4) \
                 ON CONFLICT(episode_id) DO UPDATE SET comments_json=excluded.comments_json, \
                 fetched_at=excluded.fetched_at, expires_at=excluded.expires_at",
                params![cache.episode_id, json, cache.fetched_at, cache.expires_at],
            )?;
        Ok(())
    }

    pub fn comments(&self, episode_id: i64) -> Result<Option<CachedComments>, StorageError> {
        let row: Option<(String, i64, i64)> = self
            .connection
            .lock()
            .map_err(|_| StorageError::Poisoned)?
            .query_row(
                "SELECT comments_json, fetched_at, expires_at FROM danmaku_cache WHERE episode_id=?1",
                [episode_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()?;
        row.map(|(json, fetched_at, expires_at)| {
            Ok(CachedComments {
                episode_id,
                comments: serde_json::from_str(&json)?,
                fetched_at,
                expires_at,
            })
        })
        .transpose()
    }

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
                params![key, json],
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

#[cfg(test)]
mod tests {
    use super::*;
    use url::Url;
    use yanami_danmaku::DanmakuMode;

    #[test]
    fn persists_profiles_matches_and_comments_without_credentials() {
        let storage = AppStorage::in_memory().unwrap();
        let profile =
            ServerProfile::new("Home", Url::parse("https://home.test/emby").unwrap()).unwrap();
        storage.upsert_server(&profile, 10).unwrap();
        assert_eq!(storage.list_servers().unwrap(), vec![profile.clone()]);

        let session = UserSession {
            server_local_id: profile.local_id,
            server_id: "server".into(),
            user_id: "user".into(),
            user_name: "Viewer".into(),
            device_id: Uuid::new_v4(),
            credential_key: "emby.token".into(),
        };
        storage.upsert_user(&session, 11).unwrap();
        assert_eq!(storage.latest_user_session().unwrap(), Some(session));

        let matched = DanmakuMatchRecord {
            server_id: "server".into(),
            item_id: "item".into(),
            media_source_id: "source".into(),
            episode_id: 123,
            display_title: "Episode 1".into(),
            time_offset_seconds: 0.0,
            updated_at: 10,
        };
        storage.put_match(&matched).unwrap();
        assert_eq!(
            storage.find_match("server", "item", "source").unwrap(),
            Some(matched)
        );

        let cache = CachedComments {
            episode_id: 123,
            comments: vec![DanmakuComment {
                time_seconds: 1.0,
                mode: DanmakuMode::Scroll,
                color_rgb: 0x00ff_ffff,
                text: "hello".into(),
                source_id: None,
                sender: None,
            }],
            fetched_at: 10,
            expires_at: 20,
        };
        storage.put_comments(&cache).unwrap();
        assert_eq!(storage.comments(123).unwrap(), Some(cache));
    }
}
