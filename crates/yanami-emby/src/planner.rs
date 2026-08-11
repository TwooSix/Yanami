use thiserror::Error;
use url::Url;

use yanami_core::{MediaTrack, PlaybackMethod, PlaybackPlan, SensitiveHeaders, TrackKind};

use crate::{MediaSource, PlaybackInfo};

#[derive(Debug, Clone, Copy, Default)]
pub struct PlaybackPreference {
    /// When set, sources above this bitrate should use the server transcode URL.
    pub max_bitrate: Option<u64>,
    pub force_transcode: bool,
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum PlanningError {
    #[error("Emby did not return a playable media source")]
    NoPlayableSource,
    #[error("Emby returned a malformed playback URL")]
    InvalidPlaybackUrl,
}

pub struct PlaybackPlanner;

impl PlaybackPlanner {
    pub fn plan(
        base_url: &Url,
        item_id: &str,
        resume_position_ticks: u64,
        info: &PlaybackInfo,
        token: Option<&str>,
        preference: PlaybackPreference,
    ) -> Result<PlaybackPlan, PlanningError> {
        let source = select_source(&info.media_sources, preference)
            .ok_or(PlanningError::NoPlayableSource)?;
        let should_transcode = preference.force_transcode
            || preference
                .max_bitrate
                .zip(source.bitrate)
                .is_some_and(|(max, actual)| actual > max);

        let (method, url) = if !should_transcode && source.supports_direct_stream {
            (
                PlaybackMethod::DirectStream,
                direct_url(base_url, item_id, &info.play_session_id, source)?,
            )
        } else if source.supports_transcoding {
            let raw = source
                .transcoding_url
                .as_deref()
                .ok_or(PlanningError::InvalidPlaybackUrl)?;
            (
                PlaybackMethod::Transcode,
                resolve_url(base_url, raw).ok_or(PlanningError::InvalidPlaybackUrl)?,
            )
        } else if source.supports_direct_stream {
            (
                PlaybackMethod::DirectStream,
                direct_url(base_url, item_id, &info.play_session_id, source)?,
            )
        } else {
            return Err(PlanningError::NoPlayableSource);
        };

        let mut request_headers: SensitiveHeaders = source.required_http_headers.clone().into();
        if let Some(token) = token {
            request_headers.insert("X-Emby-Token".to_owned(), token.to_owned());
        }

        Ok(PlaybackPlan {
            item_id: item_id.to_owned(),
            media_source_id: source.id.clone(),
            play_session_id: info.play_session_id.clone(),
            method,
            url,
            request_headers,
            resume_position_ticks,
            audio_stream_index: source.default_audio_stream_index,
            subtitle_stream_index: source.default_subtitle_stream_index,
            tracks: tracks(base_url, &source.media_streams),
        })
    }
}

fn select_source(sources: &[MediaSource], preference: PlaybackPreference) -> Option<&MediaSource> {
    sources.iter().max_by_key(|source| {
        let original = u8::from(source.supports_direct_stream && !preference.force_transcode);
        (original, source.bitrate.unwrap_or_default())
    })
}

fn direct_url(
    base_url: &Url,
    item_id: &str,
    play_session_id: &str,
    source: &MediaSource,
) -> Result<Url, PlanningError> {
    if let Some(raw) = &source.direct_stream_url {
        return resolve_url(base_url, raw).ok_or(PlanningError::InvalidPlaybackUrl);
    }

    let mut url = base_url.clone();
    let prefix = base_url.path().trim_end_matches('/');
    let extension = source.container.as_deref().unwrap_or("mkv");
    url.set_path(&format!("{prefix}/Videos/{item_id}/stream.{extension}"));
    url.query_pairs_mut()
        .append_pair("static", "true")
        .append_pair("MediaSourceId", &source.id)
        .append_pair("PlaySessionId", play_session_id);
    Ok(url)
}

fn resolve_url(base_url: &Url, raw: &str) -> Option<Url> {
    Url::parse(raw).ok().or_else(|| {
        let mut base = base_url.clone();
        base.set_query(None);
        base.set_fragment(None);
        if raw.starts_with('/') {
            base.set_path("/");
            base.join(raw.trim_start_matches('/')).ok()
        } else {
            let path = format!("{}/", base.path().trim_end_matches('/'));
            base.set_path(&path);
            base.join(raw).ok()
        }
    })
}

fn tracks(base_url: &Url, streams: &[crate::MediaStream]) -> Vec<MediaTrack> {
    streams
        .iter()
        .filter_map(|stream| {
            let kind = match stream.stream_type.as_str() {
                "Video" => TrackKind::Video,
                "Audio" => TrackKind::Audio,
                "Subtitle" => TrackKind::Subtitle,
                _ => return None,
            };
            Some(MediaTrack {
                index: stream.index,
                kind,
                codec: stream.codec.clone(),
                language: stream.language.clone(),
                title: stream
                    .display_title
                    .clone()
                    .or_else(|| stream.title.clone()),
                external: stream.is_external,
                delivery_url: stream
                    .delivery_url
                    .as_deref()
                    .and_then(|raw| resolve_url(base_url, raw)),
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::*;
    use crate::MediaStream;

    fn source() -> MediaSource {
        MediaSource {
            id: "source-1".into(),
            name: None,
            path: Some("show.mkv".into()),
            container: Some("mkv".into()),
            size: Some(42),
            run_time_ticks: Some(100),
            bitrate: Some(20_000_000),
            supports_direct_play: true,
            supports_direct_stream: true,
            supports_transcoding: true,
            direct_stream_url: None,
            transcoding_url: Some("/Videos/item/master.m3u8".into()),
            required_http_headers: BTreeMap::from([("Referer".into(), "home".into())]),
            default_audio_stream_index: Some(1),
            default_subtitle_stream_index: Some(2),
            chapters: Vec::new(),
            media_streams: vec![MediaStream {
                index: 2,
                stream_type: "Subtitle".into(),
                codec: Some("ass".into()),
                language: Some("chi".into()),
                display_title: Some("Chinese ASS".into()),
                title: None,
                is_external: true,
                delivery_url: Some("/Videos/item/Subtitles/2/Stream.ass".into()),
            }],
        }
    }

    #[test]
    fn prefers_original_stream_and_preserves_headers() {
        let info = PlaybackInfo {
            media_sources: vec![source()],
            play_session_id: "session".into(),
            error_code: None,
        };
        let plan = PlaybackPlanner::plan(
            &Url::parse("https://example.test/emby").unwrap(),
            "item",
            25,
            &info,
            Some("token"),
            PlaybackPreference::default(),
        )
        .unwrap();

        assert_eq!(plan.method, PlaybackMethod::DirectStream);
        assert!(plan.url.as_str().contains("static=true"));
        assert_eq!(plan.request_headers["X-Emby-Token"], "token");
        let debug = format!("{plan:?}");
        assert!(!debug.contains("token"));
        assert!(!debug.contains("example.test"));
        assert_eq!(
            plan.tracks[0].delivery_url.as_ref().unwrap().host_str(),
            Some("example.test")
        );
    }

    #[test]
    fn uses_transcode_above_bandwidth_limit() {
        let info = PlaybackInfo {
            media_sources: vec![source()],
            play_session_id: "session".into(),
            error_code: None,
        };
        let plan = PlaybackPlanner::plan(
            &Url::parse("https://example.test/emby").unwrap(),
            "item",
            0,
            &info,
            None,
            PlaybackPreference {
                max_bitrate: Some(5_000_000),
                force_transcode: false,
            },
        )
        .unwrap();

        assert_eq!(plan.method, PlaybackMethod::Transcode);
        assert_eq!(plan.url.path(), "/Videos/item/master.m3u8");
    }

    #[test]
    fn relative_delivery_urls_keep_reverse_proxy_prefix() {
        let url = resolve_url(
            &Url::parse("https://example.test/tv/emby").unwrap(),
            "Videos/item/Subtitles/2/Stream.ass",
        )
        .unwrap();

        assert_eq!(url.path(), "/tv/emby/Videos/item/Subtitles/2/Stream.ass");
    }
}
