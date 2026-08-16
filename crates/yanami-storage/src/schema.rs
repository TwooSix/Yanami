use rusqlite::Connection;

use crate::StorageError;

pub(crate) const DATABASE_SCHEMA_VERSION: i64 = 1;

pub(crate) fn initialize_current_schema(connection: &mut Connection) -> Result<(), StorageError> {
    let transaction = connection.transaction()?;
    transaction.execute_batch(
        r"
            CREATE TABLE server_profiles (
                local_id TEXT PRIMARY KEY,
                profile_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE user_profiles (
                server_local_id TEXT NOT NULL,
                user_id TEXT NOT NULL,
                session_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (server_local_id, user_id),
                FOREIGN KEY (server_local_id) REFERENCES server_profiles(local_id) ON DELETE CASCADE
            );
            CREATE TABLE danmaku_matches (
                server_id TEXT NOT NULL,
                item_id TEXT NOT NULL,
                media_source_id TEXT NOT NULL,
                episode_id INTEGER NOT NULL,
                display_title TEXT NOT NULL,
                time_offset_seconds REAL NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (server_id, item_id, media_source_id)
            );
            CREATE TABLE danmaku_cache (
                episode_id INTEGER PRIMARY KEY,
                comments_json TEXT NOT NULL,
                fetched_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL
            );
            CREATE TABLE preferences (
                key TEXT PRIMARY KEY,
                value_json TEXT NOT NULL
            );
        ",
    )?;
    transaction.pragma_update(None, "user_version", DATABASE_SCHEMA_VERSION)?;
    transaction.commit()?;
    Ok(())
}
