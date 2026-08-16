use rusqlite::params;
use uuid::Uuid;

use yanami_core::{ServerProfile, UserSession};

use crate::{
    AppStorage, StorageError,
    record_codec::{deserialize_record, serialize_record},
};

impl AppStorage {
    pub fn upsert_server(&self, profile: &ServerProfile, now: i64) -> Result<(), StorageError> {
        let json = serialize_record(profile)?;
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
        let stored = statement
            .query_map([], |row| row.get::<_, String>(0))?
            .collect::<Result<Vec<_>, _>>()?;
        let mut profiles = Vec::with_capacity(stored.len());
        for json in &stored {
            match deserialize_record::<ServerProfile>(json) {
                Ok(profile) => {
                    profile.validate()?;
                    profiles.push(profile);
                }
                // A single truncated row does not hide otherwise usable profiles.
                Err(StorageError::Json(_)) => {}
                Err(error) => return Err(error),
            }
        }
        Ok(profiles)
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
        let json = serialize_record(session)?;
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
        let connection = self.connection.lock().map_err(|_| StorageError::Poisoned)?;
        let mut statement = connection.prepare(
            "SELECT session_json FROM user_profiles ORDER BY updated_at DESC, server_local_id, user_id",
        )?;
        let rows = statement.query_map([], |row| row.get::<_, String>(0))?;
        for row in rows {
            match deserialize_record::<UserSession>(&row?) {
                Ok(session) => return Ok(Some(session)),
                Err(StorageError::Json(_)) => {}
                Err(error) => return Err(error),
            }
        }
        Ok(None)
    }
}
