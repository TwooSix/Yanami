use base64::{Engine as _, engine::general_purpose::STANDARD};
use secrecy::{ExposeSecret, SecretString};
use sha2::{Digest, Sha256};

pub struct DandanCredentials {
    app_id: String,
    app_secret: SecretString,
}

impl DandanCredentials {
    pub fn new(app_id: impl Into<String>, app_secret: SecretString) -> Self {
        Self {
            app_id: app_id.into(),
            app_secret,
        }
    }

    pub fn app_id(&self) -> &str {
        &self.app_id
    }

    pub fn signature(&self, timestamp: i64, path: &str) -> String {
        let normalized_path = path.to_ascii_lowercase();
        let payload = format!(
            "{}{}{}{}",
            self.app_id,
            timestamp,
            normalized_path,
            self.app_secret.expose_secret()
        );
        STANDARD.encode(Sha256::digest(payload.as_bytes()))
    }
}

#[cfg(test)]
mod tests {
    use secrecy::SecretString;

    use super::DandanCredentials;

    #[test]
    fn signature_uses_lowercase_path_and_hides_secret() {
        let credentials = DandanCredentials::new("app", SecretString::from("secret"));
        assert_eq!(
            credentials.signature(1_735_660_800, "/API/V2/Comment/123"),
            credentials.signature(1_735_660_800, "/api/v2/comment/123")
        );
        assert!(
            !credentials
                .signature(1_735_660_800, "/api/v2/comment/123")
                .contains("secret")
        );
    }
}
