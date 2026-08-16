use std::time::Duration;

use serde::Serialize;

use crate::{
    client::{EmbyClient, EmbyError},
    models::{PlaybackInfo, PlaybackProgress},
    transport::{decode, ensure_success},
};

impl EmbyClient {
    pub async fn playback_info(&self, item_id: &str) -> Result<PlaybackInfo, EmbyError> {
        #[derive(Serialize)]
        #[serde(rename_all = "PascalCase")]
        #[allow(clippy::struct_excessive_bools)]
        struct Request<'a> {
            user_id: &'a str,
            is_playback: bool,
            auto_open_live_stream: bool,
            enable_direct_play: bool,
            enable_direct_stream: bool,
            enable_transcoding: bool,
        }

        decode(
            self.request(
                reqwest::Method::POST,
                &format!("Items/{item_id}/PlaybackInfo"),
            )
            .json(&Request {
                user_id: self.user_id.as_deref().unwrap_or_default(),
                is_playback: true,
                auto_open_live_stream: false,
                enable_direct_play: false,
                enable_direct_stream: true,
                enable_transcoding: false,
            })
            .send()
            .await?,
        )
        .await
    }

    pub async fn report_started(&self, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        self.report("Sessions/Playing", progress).await
    }

    pub async fn report_progress(&self, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        self.report("Sessions/Playing/Progress", progress).await
    }

    pub async fn report_stopped(&self, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        self.report("Sessions/Playing/Stopped", progress).await
    }

    async fn report(&self, path: &str, progress: &PlaybackProgress<'_>) -> Result<(), EmbyError> {
        let response = self
            .request(reqwest::Method::POST, path)
            // Playback check-ins are best-effort telemetry. They must not hold
            // navigation, app shutdown, or the next episode behind the normal
            // 30-second browse timeout when the server goes offline.
            .timeout(Duration::from_secs(5))
            .json(progress)
            .send()
            .await?;
        ensure_success(response).await
    }
}
