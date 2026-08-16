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
        required_http_headers: BTreeMap::from([("User-Agent".into(), "Yanami test".into())]),
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
            supports_external_stream: false,
            delivery_url: Some("/Videos/item/Subtitles/2/Stream.ass".into()),
        }],
    }
}

#[test]
fn prefers_original_stream_and_scopes_token_to_checked_urls() {
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
    )
    .unwrap();

    assert_eq!(plan.method, PlaybackMethod::DirectStream);
    assert_eq!(plan.subtitle_stream_index, Some(2));
    assert!(plan.url.as_str().contains("static=true"));
    assert_eq!(plan.request_headers["user-agent"], "Yanami test");
    assert!(!plan.request_headers.contains_key("x-emby-token"));
    assert_eq!(
        plan.url
            .query_pairs()
            .find(|(name, _)| name == "api_key")
            .map(|(_, value)| value.into_owned())
            .as_deref(),
        Some("token")
    );
    let debug = format!("{plan:?}");
    assert!(!debug.contains("token"));
    assert!(!debug.contains("example.test"));
    assert_eq!(
        plan.tracks[0].delivery_url.as_ref().unwrap().host_str(),
        Some("example.test")
    );
    assert_eq!(
        plan.tracks[0]
            .delivery_url
            .as_ref()
            .unwrap()
            .query_pairs()
            .filter(|(name, _)| name == "api_key")
            .count(),
        1
    );
}

#[test]
fn rejects_adaptive_or_transcode_only_sources() {
    let info = PlaybackInfo {
        media_sources: vec![MediaSource {
            supports_direct_stream: false,
            ..source()
        }],
        play_session_id: "session".into(),
        error_code: None,
    };
    assert_eq!(
        PlaybackPlanner::plan(
            &Url::parse("https://example.test/emby").unwrap(),
            "item",
            0,
            &info,
            None,
        )
        .unwrap_err(),
        PlanningError::SecureTranscodingUnavailable
    );

    let mut adaptive = source();
    adaptive.direct_stream_url = Some("/Videos/item/master.m3u8".into());
    let info = PlaybackInfo {
        media_sources: vec![adaptive],
        play_session_id: "session".into(),
        error_code: None,
    };
    assert_eq!(
        PlaybackPlanner::plan(
            &Url::parse("https://example.test/emby").unwrap(),
            "item",
            0,
            &info,
            None,
        )
        .unwrap_err(),
        PlanningError::SecureTranscodingUnavailable
    );
}

#[test]
fn skips_higher_bitrate_transcode_source_for_progressive_direct_source() {
    let mut transcode_only = source();
    transcode_only.id = "transcode-only".into();
    transcode_only.bitrate = Some(40_000_000);
    transcode_only.supports_direct_stream = false;
    let mut progressive = source();
    progressive.id = "progressive".into();
    progressive.bitrate = Some(10_000_000);
    let info = PlaybackInfo {
        media_sources: vec![transcode_only, progressive],
        play_session_id: "session".into(),
        error_code: None,
    };

    let plan = PlaybackPlanner::plan(
        &Url::parse("https://example.test/emby").unwrap(),
        "item",
        0,
        &info,
        None,
    )
    .unwrap();

    assert_eq!(plan.method, PlaybackMethod::DirectStream);
    assert_eq!(plan.media_source_id, "progressive");
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

#[test]
fn constructs_and_selects_standard_external_subtitle_when_server_default_is_omitted() {
    let info: PlaybackInfo = serde_json::from_value(serde_json::json!({
        "MediaSources": [{
            "Id": "mediasource_212829",
            "Container": "mkv",
            "SupportsDirectStream": true,
            "MediaStreams": [{
                "Index": 2,
                "Type": "Subtitle",
                "Codec": "ass",
                "IsExternal": true,
                "SupportsExternalStream": true
            }]
        }],
        "PlaySessionId": "session"
    }))
    .unwrap();

    let plan = PlaybackPlanner::plan(
        &Url::parse("https://example.test/tv/emby").unwrap(),
        "212829",
        0,
        &info,
        Some("token"),
    )
    .unwrap();

    assert!(plan.warnings.is_empty());
    assert_eq!(plan.subtitle_stream_index, Some(2));
    let url = plan.tracks[0].delivery_url.as_ref().unwrap();
    assert_eq!(
        url.path(),
        "/tv/emby/Videos/212829/mediasource_212829/Subtitles/2/Stream.ass"
    );
    assert_eq!(url.host_str(), Some("example.test"));
    assert_eq!(
        url.query_pairs()
            .find(|(name, _)| name == "api_key")
            .map(|(_, value)| value.into_owned())
            .as_deref(),
        Some("token")
    );
}

#[test]
fn standard_external_subtitle_fallback_never_uses_a_cross_origin_delivery_url() {
    let mut media = source();
    media.media_streams[0].delivery_url = Some("https://attacker.test/subtitle.ass".into());
    media.media_streams[0].supports_external_stream = true;
    let info = PlaybackInfo {
        media_sources: vec![media],
        play_session_id: "session".into(),
        error_code: None,
    };

    let plan = PlaybackPlanner::plan(
        &Url::parse("https://example.test/emby").unwrap(),
        "episode/1",
        0,
        &info,
        Some("token"),
    )
    .unwrap();
    let url = plan.tracks[0].delivery_url.as_ref().unwrap();

    assert_eq!(url.host_str(), Some("example.test"));
    assert_eq!(
        url.path(),
        "/emby/Videos/episode%2F1/source-1/Subtitles/2/Stream.ass"
    );
    assert!(plan.warnings.is_empty());
}

#[test]
fn rejects_cross_origin_media_and_omits_unsafe_optional_subtitles() {
    let base = Url::parse("https://example.test/emby").unwrap();
    let mut media = source();
    media.direct_stream_url = Some("https://attacker.test/video".into());
    let info = PlaybackInfo {
        media_sources: vec![media],
        play_session_id: "session".into(),
        error_code: None,
    };
    assert_eq!(
        PlaybackPlanner::plan(&base, "item", 0, &info, Some("token")).unwrap_err(),
        PlanningError::CrossOriginPlaybackUrl
    );

    let mut media = source();
    media.media_streams[0].delivery_url = Some("https://attacker.test/subtitle.ass".into());
    let info = PlaybackInfo {
        media_sources: vec![media],
        play_session_id: "session".into(),
        error_code: None,
    };
    let plan = PlaybackPlanner::plan(&base, "item", 0, &info, Some("token")).unwrap();
    assert_eq!(plan.subtitle_stream_index, None);
    assert_eq!(plan.tracks[0].delivery_url, None);
    assert_eq!(
        plan.warnings,
        vec![PlaybackWarning::ExternalSubtitleUnavailable { index: 2 }]
    );
}

#[test]
fn ignores_delivery_urls_on_streams_that_are_never_loaded_separately() {
    let mut media = source();
    media.media_streams.push(MediaStream {
        index: 0,
        stream_type: "Video".into(),
        codec: Some("h264".into()),
        language: None,
        display_title: None,
        title: None,
        is_external: false,
        supports_external_stream: false,
        delivery_url: Some("https://attacker.test/unused-video".into()),
    });
    let info = PlaybackInfo {
        media_sources: vec![media],
        play_session_id: "session".into(),
        error_code: None,
    };
    let plan = PlaybackPlanner::plan(
        &Url::parse("https://example.test/emby").unwrap(),
        "item",
        0,
        &info,
        Some("token"),
    )
    .unwrap();
    let video = plan
        .tracks
        .iter()
        .find(|track| track.kind == TrackKind::Video)
        .unwrap();
    assert_eq!(video.delivery_url, None);
    assert!(plan.warnings.is_empty());
}

#[test]
fn rejects_headers_that_libmpv_could_forward_as_credentials() {
    let mut media = source();
    media.required_http_headers =
        BTreeMap::from([("Authorization".into(), "Bearer secret".into())]);
    let info = PlaybackInfo {
        media_sources: vec![media],
        play_session_id: "session".into(),
        error_code: None,
    };
    assert_eq!(
        PlaybackPlanner::plan(
            &Url::parse("https://example.test/emby").unwrap(),
            "item",
            0,
            &info,
            Some("token"),
        )
        .unwrap_err(),
        PlanningError::UnsafePlaybackHeaders
    );
}
