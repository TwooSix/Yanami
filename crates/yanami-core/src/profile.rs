use serde::{Deserialize, Serialize};
use thiserror::Error;
use url::Url;
use uuid::Uuid;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum TlsPolicy {
    Strict,
    TrustCertificateSha256(String),
}

/// Records the user's explicit transport-security decision with the profile.
///
/// Hostname heuristics cannot reliably distinguish a private split-DNS server
/// from a public one, so accepting clear-text HTTP must be an explicit choice
/// made by the caller after presenting the risk to the user.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum TransportSecurity {
    #[default]
    RequireHttps,
    AllowInsecureHttp,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServerProfile {
    pub local_id: Uuid,
    pub server_id: Option<String>,
    pub display_name: String,
    pub base_url: Url,
    pub tls_policy: TlsPolicy,
    #[serde(default)]
    pub transport_security: TransportSecurity,
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
    #[error("unencrypted http requires explicit user approval")]
    InsecureHttpNotAllowed,
}

impl ServerProfile {
    /// Normalizes an Emby base URL while preserving reverse-proxy path prefixes.
    pub fn new(display_name: impl Into<String>, base_url: Url) -> Result<Self, ProfileError> {
        Self::with_transport_security(display_name, base_url, TransportSecurity::RequireHttps)
    }

    /// Creates a profile with an explicit transport-security decision.
    pub fn with_transport_security(
        display_name: impl Into<String>,
        mut base_url: Url,
        transport_security: TransportSecurity,
    ) -> Result<Self, ProfileError> {
        validate_base_url(&base_url, transport_security)?;

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
            transport_security,
        })
    }

    /// Revalidates profiles loaded from persistence before any network use.
    pub fn validate(&self) -> Result<(), ProfileError> {
        validate_base_url(&self.base_url, self.transport_security)
    }

    pub fn uses_insecure_http(&self) -> bool {
        self.base_url.scheme() == "http"
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

fn validate_base_url(
    base_url: &Url,
    transport_security: TransportSecurity,
) -> Result<(), ProfileError> {
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
    if base_url.scheme() == "http" && transport_security != TransportSecurity::AllowInsecureHttp {
        return Err(ProfileError::InsecureHttpNotAllowed);
    }
    Ok(())
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

    #[test]
    fn insecure_http_requires_an_explicit_persisted_decision() {
        assert_eq!(
            ServerProfile::new(
                "Unapproved",
                Url::parse("http://nas.home.arpa:8096/emby").unwrap()
            )
            .unwrap_err(),
            ProfileError::InsecureHttpNotAllowed
        );
        let approved = ServerProfile::with_transport_security(
            "Approved",
            Url::parse("http://nas.home.arpa:8096/emby").unwrap(),
            TransportSecurity::AllowInsecureHttp,
        )
        .unwrap();
        assert!(approved.uses_insecure_http());
        assert_eq!(
            approved.transport_security,
            TransportSecurity::AllowInsecureHttp
        );
    }
}
