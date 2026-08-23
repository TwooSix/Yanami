use serde::Serialize;
use thiserror::Error;

/// Stable error categories exposed across the desktop ABI.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ApplicationErrorCode {
    Cancelled,
    InvalidInput,
    NotConnected,
    NotFound,
    PermissionDenied,
    Unsupported,
    Storage,
    Credentials,
    Network,
    Internal,
}

#[derive(Debug, Error)]
#[error("{message}")]
pub struct ApplicationError {
    code: ApplicationErrorCode,
    message: String,
}

impl ApplicationError {
    pub fn new(code: ApplicationErrorCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    pub fn cancelled() -> Self {
        Self::new(
            ApplicationErrorCode::Cancelled,
            "operation cancelled during shutdown",
        )
    }

    pub fn invalid(message: impl Into<String>) -> Self {
        Self::new(ApplicationErrorCode::InvalidInput, message)
    }

    pub fn not_connected() -> Self {
        Self::new(
            ApplicationErrorCode::NotConnected,
            "no Emby server is connected",
        )
    }

    pub fn permission_denied(message: impl Into<String>) -> Self {
        Self::new(ApplicationErrorCode::PermissionDenied, message)
    }

    pub fn not_found(message: impl Into<String>) -> Self {
        Self::new(ApplicationErrorCode::NotFound, message)
    }

    pub fn unsupported(message: impl Into<String>) -> Self {
        Self::new(ApplicationErrorCode::Unsupported, message)
    }

    pub fn internal(message: impl Into<String>) -> Self {
        Self::new(ApplicationErrorCode::Internal, message)
    }

    pub fn code(&self) -> ApplicationErrorCode {
        self.code
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl From<String> for ApplicationError {
    fn from(message: String) -> Self {
        Self::internal(message)
    }
}

#[cfg(test)]
mod tests {
    use super::{ApplicationError, ApplicationErrorCode};

    #[test]
    fn stable_code_is_selected_by_constructor_not_message_text() {
        assert_eq!(
            ApplicationError::invalid("network administrator missing").code(),
            ApplicationErrorCode::InvalidInput
        );
        assert_eq!(
            ApplicationError::permission_denied("valid request").code(),
            ApplicationErrorCode::PermissionDenied
        );
        assert_eq!(
            ApplicationError::not_found("administrator").code(),
            ApplicationErrorCode::NotFound
        );
        assert_eq!(
            ApplicationError::from("invalid administrator not found".to_owned()).code(),
            ApplicationErrorCode::Internal
        );
    }

    #[test]
    fn emby_status_is_mapped_without_discarding_structured_error_data() {
        for (status, expected) in [
            (401, ApplicationErrorCode::Credentials),
            (403, ApplicationErrorCode::PermissionDenied),
            (404, ApplicationErrorCode::NotFound),
            (422, ApplicationErrorCode::InvalidInput),
            (503, ApplicationErrorCode::Network),
        ] {
            let error = yanami_emby::EmbyError::Api {
                status,
                message: "same message".to_owned(),
            };
            assert_eq!(ApplicationError::from(error).code(), expected);
        }
    }

    #[test]
    fn dandan_status_is_mapped_without_message_heuristics() {
        for (status, expected) in [
            (401, ApplicationErrorCode::Credentials),
            (403, ApplicationErrorCode::PermissionDenied),
            (404, ApplicationErrorCode::NotFound),
            (422, ApplicationErrorCode::InvalidInput),
            (503, ApplicationErrorCode::Network),
        ] {
            let error = yanami_danmaku::DandanError::Http {
                status,
                message: "same message".to_owned(),
            };
            assert_eq!(ApplicationError::from(error).code(), expected);
        }
        assert_eq!(
            ApplicationError::from(yanami_danmaku::DandanError::EmptyMedia).code(),
            ApplicationErrorCode::InvalidInput
        );
        assert_eq!(
            ApplicationError::from(yanami_danmaku::DandanError::InvalidJson(
                serde_json::from_str::<serde_json::Value>("{").unwrap_err(),
            ))
            .code(),
            ApplicationErrorCode::Unsupported
        );
        assert_eq!(
            ApplicationError::from(yanami_danmaku::DandanError::InvalidCommentRedirect).code(),
            ApplicationErrorCode::Unsupported
        );
    }
}

impl From<&str> for ApplicationError {
    fn from(message: &str) -> Self {
        message.to_owned().into()
    }
}

impl From<yanami_emby::EmbyError> for ApplicationError {
    fn from(error: yanami_emby::EmbyError) -> Self {
        use yanami_emby::EmbyError;

        let code = match &error {
            EmbyError::Api { status: 401, .. } => ApplicationErrorCode::Credentials,
            EmbyError::Api { status: 403, .. } => ApplicationErrorCode::PermissionDenied,
            EmbyError::Api { status: 404, .. } => ApplicationErrorCode::NotFound,
            EmbyError::Api {
                status: 400 | 409 | 422,
                ..
            } => ApplicationErrorCode::InvalidInput,
            EmbyError::Api { status, .. } if *status >= 500 => ApplicationErrorCode::Network,
            EmbyError::Api { .. }
            | EmbyError::InvalidUrl(_)
            | EmbyError::InvalidJson(_)
            | EmbyError::InvalidResponse(_)
            | EmbyError::ResponseTooLarge { .. }
            | EmbyError::CertificatePinningUnavailable
            | EmbyError::InvalidProfile(_) => ApplicationErrorCode::Unsupported,
            EmbyError::Transport(_) => ApplicationErrorCode::Network,
        };
        Self::new(code, error.to_string())
    }
}

impl From<yanami_danmaku::DandanError> for ApplicationError {
    fn from(error: yanami_danmaku::DandanError) -> Self {
        use yanami_danmaku::DandanError;

        let code = match &error {
            DandanError::Transport(error) => match error.status().map(|status| status.as_u16()) {
                Some(401) => ApplicationErrorCode::Credentials,
                Some(403) => ApplicationErrorCode::PermissionDenied,
                Some(404) => ApplicationErrorCode::NotFound,
                Some(400 | 409 | 422) => ApplicationErrorCode::InvalidInput,
                _ => ApplicationErrorCode::Network,
            },
            DandanError::Http { status: 401, .. } => ApplicationErrorCode::Credentials,
            DandanError::Http { status: 403, .. } => ApplicationErrorCode::PermissionDenied,
            DandanError::Http { status: 404, .. } => ApplicationErrorCode::NotFound,
            DandanError::Http {
                status: 400 | 409 | 422,
                ..
            }
            | DandanError::EmptyMedia => ApplicationErrorCode::InvalidInput,
            DandanError::Http { status, .. } if *status >= 500 => ApplicationErrorCode::Network,
            DandanError::Http { .. }
            | DandanError::Api { .. }
            | DandanError::InvalidClock
            | DandanError::InvalidApiUrl
            | DandanError::InvalidJson(_)
            | DandanError::ResponseTooLarge { .. }
            | DandanError::InvalidCommentRedirect
            | DandanError::TooManyCommentRedirects => ApplicationErrorCode::Unsupported,
        };
        Self::new(code, error.to_string())
    }
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ErrorEnvelope<'a> {
    pub code: ApplicationErrorCode,
    pub message: &'a str,
}

impl<'a> From<&'a ApplicationError> for ErrorEnvelope<'a> {
    fn from(error: &'a ApplicationError) -> Self {
        Self {
            code: error.code(),
            message: error.message(),
        }
    }
}
