use std::{fmt, ops::Deref};

use serde::Serialize;
use thiserror::Error;
use url::Url;

#[derive(Debug, Error, Clone, Copy, PartialEq, Eq)]
pub enum SameOriginUrlError {
    #[error("authorized URLs must use http or https")]
    UnsupportedScheme,
    #[error("authorized URLs must include a host")]
    MissingHost,
    #[error("authorized URLs cannot contain embedded credentials")]
    EmbeddedCredentials,
    #[error("authorized URL is not on the trusted server origin")]
    CrossOrigin,
}

/// An HTTP URL proven to share the trusted server's scheme, host and effective port.
///
/// The inner URL is immutable outside this module so later code cannot accidentally
/// replace its authority after the origin check.
#[derive(Clone, PartialEq, Eq, Serialize)]
#[serde(transparent)]
pub struct SameOriginUrl(Url);

impl SameOriginUrl {
    pub fn new(trusted_origin: &Url, candidate: Url) -> Result<Self, SameOriginUrlError> {
        if !matches!(candidate.scheme(), "http" | "https") {
            return Err(SameOriginUrlError::UnsupportedScheme);
        }
        if candidate.host().is_none() {
            return Err(SameOriginUrlError::MissingHost);
        }
        if !candidate.username().is_empty() || candidate.password().is_some() {
            return Err(SameOriginUrlError::EmbeddedCredentials);
        }
        if !same_origin(trusted_origin, &candidate) {
            return Err(SameOriginUrlError::CrossOrigin);
        }
        Ok(Self(candidate))
    }

    pub fn as_url(&self) -> &Url {
        &self.0
    }

    pub fn into_url(self) -> Url {
        self.0
    }

    /// Replaces one query parameter without changing the checked origin.
    pub fn set_query_parameter(&mut self, name: &str, value: &str) {
        let retained: Vec<_> = self
            .0
            .query_pairs()
            .filter(|(key, _)| !key.eq_ignore_ascii_case(name))
            .map(|(key, value)| (key.into_owned(), value.into_owned()))
            .collect();
        self.0.set_query(None);
        self.0
            .query_pairs_mut()
            .extend_pairs(retained)
            .append_pair(name, value);
    }
}

impl AsRef<Url> for SameOriginUrl {
    fn as_ref(&self) -> &Url {
        self.as_url()
    }
}

impl Deref for SameOriginUrl {
    type Target = Url;

    fn deref(&self) -> &Self::Target {
        self.as_url()
    }
}

impl fmt::Debug for SameOriginUrl {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("SameOriginUrl([REDACTED])")
    }
}

pub fn same_origin(left: &Url, right: &Url) -> bool {
    left.scheme() == right.scheme()
        && left.host() == right.host()
        && left.port_or_known_default() == right.port_or_known_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_default_port_equivalence_and_replaces_secret_query() {
        let base = Url::parse("https://media.example.test/emby").unwrap();
        let mut endpoint = SameOriginUrl::new(
            &base,
            Url::parse("https://media.example.test:443/video?api_key=old&x=1").unwrap(),
        )
        .unwrap();

        endpoint.set_query_parameter("api_key", "new");
        let query: Vec<_> = endpoint.query_pairs().collect();
        assert_eq!(
            query,
            [("x".into(), "1".into()), ("api_key".into(), "new".into())]
        );
    }

    #[test]
    fn rejects_cross_origin_downgrade_and_credentials() {
        let base = Url::parse("https://media.example.test/emby").unwrap();
        for candidate in [
            "http://media.example.test/video",
            "https://other.example.test/video",
            "https://media.example.test:444/video",
        ] {
            assert_eq!(
                SameOriginUrl::new(&base, Url::parse(candidate).unwrap()).unwrap_err(),
                SameOriginUrlError::CrossOrigin
            );
        }
        assert_eq!(
            SameOriginUrl::new(
                &base,
                Url::parse("https://user:secret@media.example.test/video").unwrap()
            )
            .unwrap_err(),
            SameOriginUrlError::EmbeddedCredentials
        );
    }
}
