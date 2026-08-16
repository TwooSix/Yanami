use futures_util::StreamExt;

use crate::client::EmbyError;

const MAX_JSON_RESPONSE_BYTES: usize = 32 * 1024 * 1024;
pub(crate) const MAX_IMAGE_RESPONSE_BYTES: usize = 25 * 1024 * 1024;
const MAX_ERROR_RESPONSE_BYTES: usize = 64 * 1024;

pub(crate) async fn decode<T: serde::de::DeserializeOwned>(
    response: reqwest::Response,
) -> Result<T, EmbyError> {
    let response = ensure_http_success(response).await?;
    let body = bounded_body(response, MAX_JSON_RESPONSE_BYTES).await?;
    Ok(serde_json::from_slice(&body)?)
}

pub(crate) async fn ensure_success(response: reqwest::Response) -> Result<(), EmbyError> {
    ensure_http_success(response).await.map(|_| ())
}

pub(crate) async fn response_bytes(response: reqwest::Response) -> Result<Vec<u8>, EmbyError> {
    let response = ensure_http_success(response).await?;
    bounded_body(response, MAX_IMAGE_RESPONSE_BYTES).await
}

async fn ensure_http_success(response: reqwest::Response) -> Result<reqwest::Response, EmbyError> {
    let status = response.status();
    if status.is_success() {
        Ok(response)
    } else {
        Err(EmbyError::Api {
            status: status.as_u16(),
            message: bounded_error_message(response).await,
        })
    }
}

async fn bounded_body(
    response: reqwest::Response,
    limit_bytes: usize,
) -> Result<Vec<u8>, EmbyError> {
    if response
        .content_length()
        .is_some_and(|length| length > limit_bytes as u64)
    {
        return Err(EmbyError::ResponseTooLarge { limit_bytes });
    }
    let initial_capacity = response
        .content_length()
        .and_then(|length| usize::try_from(length).ok())
        .unwrap_or_default()
        .min(limit_bytes);
    let mut body = Vec::with_capacity(initial_capacity);
    let mut stream = response.bytes_stream();
    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        if body.len().saturating_add(chunk.len()) > limit_bytes {
            return Err(EmbyError::ResponseTooLarge { limit_bytes });
        }
        body.extend_from_slice(&chunk);
    }
    Ok(body)
}

async fn bounded_error_message(response: reqwest::Response) -> String {
    let mut message = Vec::new();
    let mut stream = response.bytes_stream();
    while message.len() < MAX_ERROR_RESPONSE_BYTES {
        let Some(chunk) = stream.next().await else {
            break;
        };
        let Ok(chunk) = chunk else {
            break;
        };
        let remaining = MAX_ERROR_RESPONSE_BYTES - message.len();
        message.extend_from_slice(&chunk[..chunk.len().min(remaining)]);
        if chunk.len() > remaining {
            break;
        }
    }
    String::from_utf8_lossy(&message).into_owned()
}
