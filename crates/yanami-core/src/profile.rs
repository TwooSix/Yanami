use serde::{Deserialize, Serialize};
use thiserror::Error;
use url::Url;
use uuid::Uuid;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum TlsPolicy {
    Strict,
    TrustCertificateSha256(String),
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServerProfile {
    pub local_id: Uuid,
    pub server_id: Option<String>,
    pub display_name: String,
    pub base_url: Url,
    pub tls_policy: TlsPolicy,
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum ProfileError {
    #[error("server URL must use http or https")]
    UnsupportedScheme,
    #[error("server URL must include a host")]
    MissingHost,
    #[error("server URL cannot contain query parameters or a fragment")]
    UnexpectedUrlParts,
    #[error("server URL cannot contain embedded credentials")]
    EmbeddedCredentials,
}

impl ServerProfile {
    /// Normalizes an Emby base URL while preserving reverse-proxy path prefixes.
    pub fn new(display_name: impl Into<String>, mut base_url: Url) -> Result<Self, ProfileError> {
        if !matches!(base_url.scheme(), "http" | "https") {
            return Err(ProfileError::UnsupportedScheme);
        }
        if base_url.host_str().is_none() {
            return Err(ProfileError::MissingHost);
        }
        if base_url.query().is_some() || base_url.fragment().is_some() {
            return Err(ProfileError::UnexpectedUrlParts);
        }
        if !base_url.username().is_empty() || base_url.password().is_some() {
            return Err(ProfileError::EmbeddedCredentials);
        }

        let normalized = base_url.path().trim_end_matches('/').to_owned();
        base_url.set_path(if normalized.is_empty() {
            "/"
        } else {
            &normalized
        });

        Ok(Self {
            local_id: Uuid::new_v4(),
            server_id: None,
            display_name: display_name.into(),
            base_url,
            tls_policy: TlsPolicy::Strict,
        })
    }

    /// Builds an API URL without discarding a reverse-proxy prefix.
    pub fn api_url(&self, api_path: &str) -> Url {
        let prefix = self.base_url.path().trim_end_matches('/');
        let suffix = api_path.trim_start_matches('/');
        let mut url = self.base_url.clone();
        url.set_path(&format!("{prefix}/{suffix}"));
        url
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UserSession {
    pub server_local_id: Uuid,
    pub server_id: String,
    pub user_id: String,
    pub user_name: String,
    pub device_id: Uuid,
    /// Lookup key for a token stored in the operating-system credential vault.
    pub credential_key: String,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keeps_reverse_proxy_prefix() {
        let profile = ServerProfile::new(
            "Home",
            Url::parse("https://media.example.test/tv/emby/").unwrap(),
        )
        .unwrap();

        assert_eq!(
            profile.api_url("Users/Public").as_str(),
            "https://media.example.test/tv/emby/Users/Public"
        );
    }

    #[test]
    fn rejects_credentials_in_query_shaped_urls() {
        let error = ServerProfile::new(
            "Bad",
            Url::parse("https://example.test/emby?api_key=secret").unwrap(),
        )
        .unwrap_err();

        assert_eq!(error, ProfileError::UnexpectedUrlParts);
    }

    #[test]
    fn rejects_embedded_basic_auth_credentials() {
        let error = ServerProfile::new(
            "Bad",
            Url::parse("https://viewer:secret@example.test/emby").unwrap(),
        )
        .unwrap_err();

        assert_eq!(error, ProfileError::EmbeddedCredentials);
    }
}
