use thiserror::Error;

#[derive(Debug, Error)]
pub enum DandanError {
    #[error("DanDanPlay request failed: {0}")]
    Transport(#[from] reqwest::Error),
    #[error("DanDanPlay returned HTTP {status}: {message}")]
    Http { status: u16, message: String },
    #[error("DanDanPlay error {code:?}: {message}")]
    Api { code: Option<i32>, message: String },
    #[error("media server did not return any bytes for hashing")]
    EmptyMedia,
    #[error("system clock is earlier than the Unix epoch")]
    InvalidClock,
    #[error("invalid DanDanPlay API URL")]
    InvalidApiUrl,
    #[error("DanDanPlay returned malformed JSON: {0}")]
    InvalidJson(#[from] serde_json::Error),
    #[error("DanDanPlay response exceeded the {limit_bytes}-byte safety limit")]
    ResponseTooLarge { limit_bytes: usize },
    #[error("DanDanPlay returned an invalid comment download redirect")]
    InvalidCommentRedirect,
    #[error("DanDanPlay comment download exceeded the redirect limit")]
    TooManyCommentRedirects,
}
