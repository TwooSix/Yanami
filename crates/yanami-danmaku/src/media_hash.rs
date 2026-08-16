use std::time::Duration;

use md5::{Digest, Md5};
use yanami_core::SameOriginUrl;

use crate::{
    DandanError,
    transport::{ensure_http, same_origin_redirect_policy},
};

const HASH_BYTES: usize = 16 * 1024 * 1024;

/// Downloads at most the first 16 MiB of an authenticated media stream and returns its MD5.
pub async fn hash_remote_prefix(url: &SameOriginUrl) -> Result<String, DandanError> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(45))
        .redirect(same_origin_redirect_policy(url.as_url()))
        .build()?;
    let response = client
        .get(url.as_url().clone())
        .header("Range", format!("bytes=0-{}", HASH_BYTES - 1))
        .send()
        .await?;
    let mut response = ensure_http(response).await?;
    let mut hasher = Md5::new();
    let mut received = 0_usize;
    while received < HASH_BYTES {
        let Some(chunk) = response.chunk().await? else {
            break;
        };
        let take = chunk.len().min(HASH_BYTES - received);
        hasher.update(&chunk[..take]);
        received += take;
    }
    if received == 0 {
        return Err(DandanError::EmptyMedia);
    }
    let digest = hasher.finalize();
    Ok(format!("{digest:x}"))
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use tokio::{
        io::{AsyncReadExt, AsyncWriteExt},
        net::{TcpListener, TcpStream},
    };
    use url::Url;
    use yanami_core::SameOriginUrl;

    use super::hash_remote_prefix;

    async fn read_http_headers(stream: &mut TcpStream) -> String {
        let mut request = Vec::new();
        let mut chunk = [0_u8; 2048];
        loop {
            let count = stream.read(&mut chunk).await.unwrap();
            assert!(count > 0, "HTTP request ended before its headers");
            request.extend_from_slice(&chunk[..count]);
            if request.windows(4).any(|bytes| bytes == b"\r\n\r\n") {
                return String::from_utf8_lossy(&request).into_owned();
            }
        }
    }

    #[tokio::test]
    async fn hashes_a_checked_same_origin_media_url() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let request = read_http_headers(&mut stream).await.to_ascii_lowercase();
            assert!(request.contains("range: bytes=0-16777215"));
            stream
                .write_all(
                    b"HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\nConnection: close\r\n\r\ntest",
                )
                .await
                .unwrap();
        });

        let base = Url::parse(&format!("http://{address}")).unwrap();
        let url = SameOriginUrl::new(&base, base.join("media").unwrap()).unwrap();
        let digest = hash_remote_prefix(&url).await.unwrap();
        assert_eq!(digest, "098f6bcd4621d373cade4e832627b4f6");
        server.await.unwrap();
    }

    #[tokio::test]
    async fn rejects_cross_origin_redirect_before_query_token_reaches_sink() {
        let source = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let source_address = source.local_addr().unwrap();
        let sink = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let sink_address = sink.local_addr().unwrap();
        let source_server = tokio::spawn(async move {
            let (mut stream, _) = source.accept().await.unwrap();
            let request = read_http_headers(&mut stream).await;
            assert!(request.starts_with("GET /media?api_key=secret "));
            stream
                .write_all(
                    format!(
                        "HTTP/1.1 302 Found\r\nLocation: http://{sink_address}/steal\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                    )
                    .as_bytes(),
                )
                .await
                .unwrap();
        });
        let sink_server = tokio::spawn(async move {
            assert!(
                tokio::time::timeout(Duration::from_millis(400), sink.accept())
                    .await
                    .is_err(),
                "cross-origin redirect sink received the media token"
            );
        });

        let base = Url::parse(&format!("http://{source_address}")).unwrap();
        let mut url = SameOriginUrl::new(&base, base.join("media").unwrap()).unwrap();
        url.set_query_parameter("api_key", "secret");
        assert!(hash_remote_prefix(&url).await.is_err());
        source_server.await.unwrap();
        sink_server.await.unwrap();
    }
}
