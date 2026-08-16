use serde::{Deserialize, Serialize, de::DeserializeOwned};

use crate::StorageError;

pub(crate) const PERSISTED_RECORD_VERSION: u32 = 1;

#[derive(Serialize)]
struct PersistedRecord<T> {
    schema_version: u32,
    value: T,
}

#[derive(Deserialize)]
struct PersistedEnvelope {
    schema_version: u32,
    value: serde_json::Value,
}

pub(crate) fn serialize_record<T: Serialize>(value: &T) -> Result<String, StorageError> {
    Ok(serde_json::to_string(&PersistedRecord {
        schema_version: PERSISTED_RECORD_VERSION,
        value,
    })?)
}

pub(crate) fn deserialize_record<T: DeserializeOwned>(json: &str) -> Result<T, StorageError> {
    let envelope = serde_json::from_str::<PersistedEnvelope>(json)?;
    if envelope.schema_version > PERSISTED_RECORD_VERSION {
        return Err(StorageError::FutureRecord {
            found: envelope.schema_version,
            supported: PERSISTED_RECORD_VERSION,
        });
    }
    if envelope.schema_version < PERSISTED_RECORD_VERSION {
        return Err(StorageError::ObsoleteRecord {
            found: envelope.schema_version,
            supported: PERSISTED_RECORD_VERSION,
        });
    }
    Ok(serde_json::from_value(envelope.value)?)
}
