use rusqlite::Connection;
use url::Url;
use uuid::Uuid;

use yanami_core::{DanmakuComment, DanmakuMode, ServerProfile, UserSession};

use crate::{
    AppStorage, CachedComments, DanmakuMatchRecord, StorageError,
    record_codec::PERSISTED_RECORD_VERSION, schema::DATABASE_SCHEMA_VERSION,
};

#[test]
fn persists_profiles_matches_comments_and_preferences() {
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
        time_offset_seconds: -1.25,
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

    storage.set_preference("example", &vec![1, 2]).unwrap();
    assert_eq!(
        storage.preference::<Vec<i32>>("example").unwrap(),
        Some(vec![1, 2])
    );
    storage.delete_preference("example").unwrap();
    assert_eq!(storage.preference::<Vec<i32>>("example").unwrap(), None);
}

#[test]
fn empty_database_is_created_at_the_current_schema() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("empty.sqlite3");
    std::fs::File::create(&path).unwrap();

    let storage = AppStorage::open(&path).unwrap();
    assert_eq!(storage.schema_version().unwrap(), DATABASE_SCHEMA_VERSION);
}

#[test]
fn rejects_nonempty_version_zero_database_without_migrating_it() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("prototype.sqlite3");
    let connection = Connection::open(&path).unwrap();
    connection
        .execute("CREATE TABLE prototype_only(value TEXT)", [])
        .unwrap();
    drop(connection);

    assert!(matches!(
        AppStorage::open(&path),
        Err(StorageError::ObsoleteSchema {
            found: 0,
            supported: DATABASE_SCHEMA_VERSION
        })
    ));
    assert!(!std::path::PathBuf::from(format!("{}.pre-v0.bak", path.display())).exists());
}

#[test]
fn rejects_future_database_versions() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("future.sqlite3");
    let connection = Connection::open(&path).unwrap();
    connection.pragma_update(None, "user_version", 2).unwrap();
    drop(connection);

    assert!(matches!(
        AppStorage::open(path),
        Err(StorageError::FutureSchema {
            found: 2,
            supported: DATABASE_SCHEMA_VERSION
        })
    ));
}

#[test]
fn rejects_future_record_versions_instead_of_silently_hiding_them() {
    let storage = AppStorage::in_memory().unwrap();
    let profile =
        ServerProfile::new("Home", Url::parse("https://home.test/emby").unwrap()).unwrap();
    storage.upsert_server(&profile, 1).unwrap();
    let future_profile = serde_json::json!({
        "schema_version": PERSISTED_RECORD_VERSION + 1,
        "value": { "shape": "only understood by a future Yanami" },
    })
    .to_string();
    storage
        .connection
        .lock()
        .unwrap()
        .execute(
            "UPDATE server_profiles SET profile_json=?1",
            [future_profile],
        )
        .unwrap();
    assert!(matches!(
        storage.list_servers(),
        Err(StorageError::FutureRecord {
            found: 2,
            supported: PERSISTED_RECORD_VERSION
        })
    ));
}

#[test]
fn rejects_obsolete_record_versions_instead_of_guessing_their_shape() {
    let storage = AppStorage::in_memory().unwrap();
    let profile =
        ServerProfile::new("Home", Url::parse("https://home.test/emby").unwrap()).unwrap();
    storage.upsert_server(&profile, 1).unwrap();
    let obsolete_profile = serde_json::json!({
        "schema_version": PERSISTED_RECORD_VERSION - 1,
        "value": profile,
    })
    .to_string();
    storage
        .connection
        .lock()
        .unwrap()
        .execute(
            "UPDATE server_profiles SET profile_json=?1",
            [obsolete_profile],
        )
        .unwrap();
    assert!(matches!(
        storage.list_servers(),
        Err(StorageError::ObsoleteRecord {
            found: 0,
            supported: PERSISTED_RECORD_VERSION
        })
    ));
}

#[test]
fn skips_corrupt_profile_and_session_rows_without_blocking_startup() {
    let storage = AppStorage::in_memory().unwrap();
    let profile =
        ServerProfile::new("Home", Url::parse("https://home.test/emby").unwrap()).unwrap();
    storage.upsert_server(&profile, 1).unwrap();
    let session = UserSession {
        server_local_id: profile.local_id,
        server_id: "server".into(),
        user_id: "valid-user".into(),
        user_name: "Viewer".into(),
        device_id: Uuid::new_v4(),
        credential_key: "emby.token".into(),
    };
    storage.upsert_user(&session, 1).unwrap();
    {
        let connection = storage.connection.lock().unwrap();
        connection
            .execute("INSERT INTO server_profiles VALUES('corrupt', '{', 2)", [])
            .unwrap();
        connection
            .execute(
                "INSERT INTO user_profiles VALUES(?1, 'corrupt-user', '{', 2)",
                [profile.local_id.to_string()],
            )
            .unwrap();
    }

    assert_eq!(storage.list_servers().unwrap(), vec![profile]);
    assert_eq!(storage.latest_user_session().unwrap(), Some(session));
}
