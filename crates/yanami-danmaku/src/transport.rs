use url::Url;
use yanami_core::same_origin;

use crate::DandanError;

pub(crate) const MAX_JSON_RESPONSE_BYTES: usize = 32 * 1024 * 1024;
const MAX_ERROR_RESPONSE_BYTES: usize = 64 * 1024;

pub(crate) fn same_origin_redirect_policy(base_url: &Url) -> reqwest::redirect::Policy {
    let trusted_origin = base_url.clone();
    reqwest::redirect::Policy::custom(move |attempt| {
        if attempt.previous().len() > 5 {
            attempt.error("too many same-origin redirects")
        } else if same_origin(&trusted_origin, attempt.url()) {
            attempt.follow()
        } else {
            attempt.error("cross-origin redirect rejected")
        }
    })
}

pub(crate) async fn decode<T: serde::de::DeserializeOwned>(
    response: reqwest::Response,
) -> Result<T, DandanError> {
    let response = ensure_http(response).await?;
    let body = bounded_body(response, MAX_JSON_RESPONSE_BYTES).await?;
    Ok(serde_json::from_slice(&body)?)
}

pub(crate) async fn ensure_http(
    response: reqwest::Response,
) -> Result<reqwest::Response, DandanError> {
    let status = response.status();
    if status.is_success() {
        Ok(response)
    } else {
        let header_message = response
            .headers()
            .get("X-Error-Message")
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned);
        let body = bounded_error_message(response).await;
        Err(DandanError::Http {
            status: status.as_u16(),
            message: header_message
                .filter(|message| !message.is_empty())
                .unwrap_or(body),
        })
    }
}

async fn bounded_body(
    mut response: reqwest::Response,
    limit_bytes: usize,
) -> Result<Vec<u8>, DandanError> {
    if response
        .content_length()
        .is_some_and(|length| length > limit_bytes as u64)
    {
        return Err(DandanError::ResponseTooLarge { limit_bytes });
    }
    let initial_capacity = response
        .content_length()
        .and_then(|length| usize::try_from(length).ok())
        .unwrap_or_default()
        .min(limit_bytes);
    let mut body = Vec::with_capacity(initial_capacity);
    while let Some(chunk) = response.chunk().await? {
        if body.len().saturating_add(chunk.len()) > limit_bytes {
            return Err(DandanError::ResponseTooLarge { limit_bytes });
        }
        body.extend_from_slice(&chunk);
    }
    Ok(body)
}

async fn bounded_error_message(mut response: reqwest::Response) -> String {
    let mut message = Vec::new();
    while message.len() < MAX_ERROR_RESPONSE_BYTES {
        let Ok(Some(chunk)) = response.chunk().await else {
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

#[cfg(test)]
mod tests {
    use tokio::{
        io::{AsyncReadExt, AsyncWriteExt},
        net::{TcpListener, TcpStream},
    };

    use super::{MAX_JSON_RESPONSE_BYTES, decode};
    use crate::DandanError;

    async fn read_http_headers(stream: &mut TcpStream) {
        let mut request = Vec::new();
        let mut chunk = [0_u8; 2048];
        loop {
            let count = stream.read(&mut chunk).await.unwrap();
            assert!(count > 0, "HTTP request ended before its headers");
            request.extend_from_slice(&chunk[..count]);
            if request.windows(4).any(|bytes| bytes == b"\r\n\r\n") {
                return;
            }
        }
    }

    #[tokio::test]
    async fn rejects_declared_oversized_json_without_buffering_the_body() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            read_http_headers(&mut stream).await;
            stream
                .write_all(
                    b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 33554433\r\nConnection: close\r\n\r\n",
                )
                .await
                .unwrap();
        });

        let response = reqwest::Client::new()
            .get(format!("http://{address}/oversized"))
            .send()
            .await
            .unwrap();
        assert!(matches!(
            decode::<serde_json::Value>(response).await,
            Err(DandanError::ResponseTooLarge {
                limit_bytes: MAX_JSON_RESPONSE_BYTES
            })
        ));
        server.await.unwrap();
    }
}
