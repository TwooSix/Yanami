use rusqlite::{OptionalExtension, params};

use yanami_core::DanmakuComment;

use crate::{AppStorage, StorageError};

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

impl AppStorage {
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
}
