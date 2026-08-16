use thiserror::Error;
use url::Url;

use yanami_core::{
    MediaTrack, PlaybackHeaders, PlaybackMethod, PlaybackPlan, PlaybackWarning, SameOriginUrl,
    SameOriginUrlError, TrackKind,
};

use crate::{MediaSource, PlaybackInfo};

#[derive(Debug, Error, PartialEq, Eq)]
pub enum PlanningError {
    #[error("Emby did not return a playable media source")]
    NoPlayableSource,
    #[error("secure server transcoding is unavailable in this release")]
    SecureTranscodingUnavailable,
    #[error("Emby returned a malformed playback URL")]
    InvalidPlaybackUrl,
    #[error("Emby returned a playback URL outside the configured server origin")]
    CrossOriginPlaybackUrl,
    #[error("Emby requested playback headers that cannot be scoped safely by libmpv")]
    UnsafePlaybackHeaders,
}

pub struct PlaybackPlanner;

impl PlaybackPlanner {
    pub fn plan(
        base_url: &Url,
        item_id: &str,
        resume_position_ticks: u64,
        info: &PlaybackInfo,
        token: Option<&str>,
    ) -> Result<PlaybackPlan, PlanningError> {
        let source = select_source(&info.media_sources).ok_or({
            if info.media_sources.is_empty() {
                PlanningError::NoPlayableSource
            } else {
                PlanningError::SecureTranscodingUnavailable
            }
        })?;
        let method = PlaybackMethod::DirectStream;
        let mut url = direct_url(base_url, item_id, &info.play_session_id, source)?;

        let request_headers = PlaybackHeaders::try_from_map(&source.required_http_headers)
            .map_err(|_| PlanningError::UnsafePlaybackHeaders)?;
        if let Some(token) = token.filter(|token| !token.is_empty()) {
            // libmpv applies http-header-fields globally to redirects, HLS child
            // requests and external subtitles. Keep the account token scoped to
            // each already checked server URL instead.
            url.set_query_parameter("api_key", token);
        }
        let (tracks, warnings) =
            tracks(base_url, item_id, &source.id, &source.media_streams, token);
        let subtitle_stream_index = source
            .default_subtitle_stream_index
            .filter(|selected| {
                tracks.iter().any(|track| {
                    track.index == *selected
                        && track.kind == TrackKind::Subtitle
                        && (!track.external || track.delivery_url.is_some())
                })
            })
            // Emby commonly omits DefaultSubtitleStreamIndex for sidecar
            // subtitles. Once a sidecar has a verified delivery URL, select
            // the first one so playback does not silently start with it off.
            .or_else(|| {
                tracks
                    .iter()
                    .find(|track| {
                        track.kind == TrackKind::Subtitle
                            && track.external
                            && track.delivery_url.is_some()
                    })
                    .map(|track| track.index)
            });

        Ok(PlaybackPlan {
            item_id: item_id.to_owned(),
            media_source_id: source.id.clone(),
            play_session_id: info.play_session_id.clone(),
            method,
            url,
            request_headers,
            resume_position_ticks,
            audio_stream_index: source.default_audio_stream_index,
            subtitle_stream_index,
            tracks,
            warnings,
        })
    }
}

fn select_source(sources: &[MediaSource]) -> Option<&MediaSource> {
    sources
        .iter()
        .filter(|source| source.supports_direct_stream && is_progressive_source(source))
        .max_by_key(|source| source.bitrate.unwrap_or_default())
}

fn is_progressive_source(source: &MediaSource) -> bool {
    if source
        .container
        .as_deref()
        .is_some_and(is_adaptive_extension)
    {
        return false;
    }
    source
        .direct_stream_url
        .as_deref()
        .is_none_or(|raw| !is_adaptive_url(raw))
}

fn is_adaptive_extension(extension: &str) -> bool {
    matches!(
        extension
            .trim()
            .trim_start_matches('.')
            .to_ascii_lowercase()
            .as_str(),
        "m3u8" | "m3u" | "mpd"
    )
}

fn is_adaptive_url(raw: &str) -> bool {
    Url::parse(raw).ok().map_or_else(
        || raw.split(['?', '#']).next().is_some_and(is_adaptive_path),
        |url| is_adaptive_path(url.path()),
    )
}

fn is_adaptive_path(path: &str) -> bool {
    path.rsplit_once('.')
        .is_some_and(|(_, extension)| is_adaptive_extension(extension))
}

fn direct_url(
    base_url: &Url,
    item_id: &str,
    play_session_id: &str,
    source: &MediaSource,
) -> Result<SameOriginUrl, PlanningError> {
    if let Some(raw) = &source.direct_stream_url {
        if is_adaptive_url(raw) {
            return Err(PlanningError::SecureTranscodingUnavailable);
        }
        return resolve_url(base_url, raw);
    }

    let mut url = base_url.clone();
    let prefix = base_url.path().trim_end_matches('/');
    let extension = source.container.as_deref().unwrap_or("mkv");
    if is_adaptive_extension(extension) {
        return Err(PlanningError::SecureTranscodingUnavailable);
    }
    url.set_path(&format!("{prefix}/Videos/{item_id}/stream.{extension}"));
    url.query_pairs_mut()
        .append_pair("static", "true")
        .append_pair("MediaSourceId", &source.id)
        .append_pair("PlaySessionId", play_session_id);
    checked_url(base_url, url)
}

fn resolve_url(base_url: &Url, raw: &str) -> Result<SameOriginUrl, PlanningError> {
    let candidate = Url::parse(raw).ok().or_else(|| {
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
    });
    checked_url(
        base_url,
        candidate.ok_or(PlanningError::InvalidPlaybackUrl)?,
    )
}

fn checked_url(base_url: &Url, candidate: Url) -> Result<SameOriginUrl, PlanningError> {
    SameOriginUrl::new(base_url, candidate).map_err(|error| match error {
        SameOriginUrlError::CrossOrigin => PlanningError::CrossOriginPlaybackUrl,
        SameOriginUrlError::UnsupportedScheme
        | SameOriginUrlError::MissingHost
        | SameOriginUrlError::EmbeddedCredentials => PlanningError::InvalidPlaybackUrl,
    })
}

fn tracks(
    base_url: &Url,
    item_id: &str,
    media_source_id: &str,
    streams: &[crate::MediaStream],
    token: Option<&str>,
) -> (Vec<MediaTrack>, Vec<PlaybackWarning>) {
    let mut tracks = Vec::new();
    let mut warnings = Vec::new();
    for stream in streams {
        let kind = match stream.stream_type.as_str() {
            "Video" => TrackKind::Video,
            "Audio" => TrackKind::Audio,
            "Subtitle" => TrackKind::Subtitle,
            _ => continue,
        };
        // Delivery URLs are used only to load external subtitles. Parsing
        // arbitrary URLs attached to unused video/audio/internal streams
        // would let an irrelevant DTO field break otherwise safe playback.
        let mut delivery_url = None;
        if kind == TrackKind::Subtitle && stream.is_external {
            delivery_url = external_subtitle_url(base_url, item_id, media_source_id, stream);
            if delivery_url.is_none() {
                warnings.push(PlaybackWarning::ExternalSubtitleUnavailable {
                    index: stream.index,
                });
            }
        }
        if let (Some(url), Some(token)) = (
            delivery_url.as_mut(),
            token.filter(|token| !token.is_empty()),
        ) {
            url.set_query_parameter("api_key", token);
        }
        tracks.push(MediaTrack {
            index: stream.index,
            kind,
            codec: stream.codec.clone(),
            language: stream.language.clone(),
            title: stream
                .display_title
                .clone()
                .or_else(|| stream.title.clone()),
            external: stream.is_external,
            delivery_url,
        });
    }
    (tracks, warnings)
}

fn external_subtitle_url(
    base_url: &Url,
    item_id: &str,
    media_source_id: &str,
    stream: &crate::MediaStream,
) -> Option<SameOriginUrl> {
    if let Some(url) = stream
        .delivery_url
        .as_deref()
        .and_then(|raw| resolve_url(base_url, raw).ok())
    {
        return Some(url);
    }
    if !stream.supports_external_stream {
        return None;
    }

    let url = standard_external_subtitle_url(base_url, item_id, media_source_id, stream)?;
    tracing::debug!(
        subtitle_index = stream.index,
        "using the standard same-origin Emby route for an external subtitle"
    );
    Some(url)
}

fn standard_external_subtitle_url(
    base_url: &Url,
    item_id: &str,
    media_source_id: &str,
    stream: &crate::MediaStream,
) -> Option<SameOriginUrl> {
    let item_id = item_id.trim();
    let media_source_id = media_source_id.trim();
    let subtitle_format = stream.codec.as_deref()?.trim();
    if item_id.is_empty()
        || media_source_id.is_empty()
        || subtitle_format.is_empty()
        || stream.index < 0
    {
        return None;
    }

    let mut candidate = base_url.clone();
    candidate.set_query(None);
    candidate.set_fragment(None);
    let index = stream.index.to_string();
    let file_name = format!("Stream.{subtitle_format}");
    {
        let mut segments = candidate.path_segments_mut().ok()?;
        segments
            .pop_if_empty()
            .push("Videos")
            .push(item_id)
            .push(media_source_id)
            .push("Subtitles")
            .push(&index)
            .push(&file_name);
    }
    checked_url(base_url, candidate).ok()
}

#[cfg(test)]
#[path = "planner_tests.rs"]
mod tests;
