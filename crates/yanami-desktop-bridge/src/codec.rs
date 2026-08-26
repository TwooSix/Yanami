use serde::Serialize;
use serde_json::{Value, json};
use yanami_application::{ApplicationError, ErrorEnvelope};

pub(crate) const DESKTOP_SCHEMA_VERSION: u64 = 8;
pub(crate) const BACKEND_ABI_VERSION: u32 = 3;

pub(crate) fn encode_response(value: &impl Serialize) -> Result<String, ApplicationError> {
    let mut value = serde_json::to_value(value)
        .map_err(|error| ApplicationError::internal(error.to_string()))?;
    let object = value
        .as_object_mut()
        .ok_or_else(|| ApplicationError::internal("desktop response payload must be an object"))?;
    object.insert("schemaVersion".to_owned(), json!(DESKTOP_SCHEMA_VERSION));
    serde_json::to_string(&value).map_err(|error| ApplicationError::internal(error.to_string()))
}

pub(crate) fn encode_error(error: &ApplicationError) -> String {
    serde_json::to_string(&ErrorEnvelope::from(error)).unwrap_or_else(|_| {
        r#"{"code":"internal","message":"failed to serialize backend error"}"#.to_owned()
    })
}

pub(crate) fn panic_error() -> String {
    serde_json::to_string(&Value::Object(serde_json::Map::from_iter([
        ("code".to_owned(), json!("internal")),
        ("message".to_owned(), json!("Rust backend panicked")),
    ])))
    .expect("static backend panic envelope must serialize")
}

#[cfg(test)]
mod tests {
    use serde_json::{Value, json};
    use yanami_application::{ApplicationError, ApplicationErrorCode};

    use super::{DESKTOP_SCHEMA_VERSION, encode_error, encode_response};

    #[test]
    fn response_schema_is_v8_and_rejects_non_object_payloads() {
        let encoded = encode_response(&json!({"result": true})).unwrap();
        let decoded: Value = serde_json::from_str(&encoded).unwrap();
        assert_eq!(decoded["schemaVersion"], DESKTOP_SCHEMA_VERSION);
        assert_eq!(decoded["result"], true);
        assert!(encode_response(&json!([])).is_err());
    }

    #[test]
    fn error_envelope_preserves_stable_code_and_message() {
        let error = ApplicationError::new(ApplicationErrorCode::InvalidInput, "bad payload");
        let envelope: Value = serde_json::from_str(&encode_error(&error)).unwrap();
        assert_eq!(
            envelope,
            json!({"code": "invalid_input", "message": "bad payload"})
        );
    }
}
