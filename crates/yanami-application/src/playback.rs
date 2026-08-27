use std::{collections::BTreeMap, time::Instant};

use serde::{Deserialize, Serialize};
use yanami_core::{PlaybackMethod, PlaybackWarning, SameOriginUrl, TrackKind};
use yanami_emby::{BaseItem, EmbyClient, PlaybackPlanner, PlaybackProgress};

use crate::{
    Application, ApplicationError,
    images::IMAGE_DOWNLOAD_CONCURRENCY,
    presentation::{
        ImagePurpose, card_subtitle, intro_range_from_chapters, is_playable_item, media_file_stem,
        playback_title, select_episode,
    },
};

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum PlaybackContextKind {
    #[default]
    Auto,
    Series,
    Playlist,
    Single,
}

impl PlaybackContextKind {
    pub(crate) const fn as_str(self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Series => "series",
            Self::Playlist => "playlist",
            Self::Single => "single",
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackContext {
    #[serde(default)]
    pub kind: PlaybackContextKind,
    #[serde(default)]
    pub source_id: Option<String>,
    #[serde(default)]
    pub source_title: Option<String>,
    #[serde(default)]
    pub playlist_entry_id: Option<String>,
    #[serde(default)]
    pub queue_index: Option<usize>,
}

impl PlaybackContext {
    pub(crate) fn normalized(&self) -> Self {
        let mut context = self.clone();
        context.source_id = context
            .source_id
            .take()
            .map(|value| value.trim().to_owned())
            .filter(|value| !value.is_empty());
        context.source_title = context
            .source_title
            .take()
            .map(|value| value.trim().to_owned())
            .filter(|value| !value.is_empty());
        context.playlist_entry_id = context
            .playlist_entry_id
            .take()
            .map(|value| value.trim().to_owned())
            .filter(|value| !value.is_empty());
        if matches!(
            context.kind,
            PlaybackContextKind::Series | PlaybackContextKind::Playlist
        ) && context.source_id.is_none()
        {
            Self::default()
        } else {
            context
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum PlaybackReportKind {
    Started,
    Progress,
    Stopped,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackTelemetry {
    pub report_session_id: String,
    pub event: PlaybackReportKind,
    pub position_ticks: u64,
    #[serde(default)]
    pub paused: bool,
    #[serde(default)]
    pub muted: bool,
    #[serde(default = "default_volume")]
    pub volume: u8,
    #[serde(default = "default_rate")]
    pub rate: f64,
    #[serde(default)]
    pub audio_stream_index: Option<i32>,
    #[serde(default)]
    pub subtitle_stream_index: Option<i32>,
    #[serde(default = "default_seekable")]
    pub seekable: bool,
}

const fn default_volume() -> u8 {
    100
}

const fn default_rate() -> f64 {
    1.0
}

const fn default_seekable() -> bool {
    true
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum PreparedTrackKind {
    Audio,
    Subtitle,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackTrackDescriptor {
    kind: PreparedTrackKind,
    stream_index: i32,
    external: bool,
    delivery_url: Option<SameOriginUrl>,
    language: Option<String>,
    codec: Option<String>,
    title: Option<String>,
    selected: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackQueueContext {
    kind: PlaybackContextKind,
    source_id: Option<String>,
    source_title: Option<String>,
    playlist_entry_id: Option<String>,
    queue_index: usize,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackQueueItem {
    id: String,
    title: String,
    subtitle: String,
    item_type: Option<String>,
    image_url: Option<String>,
    playlist_entry_id: Option<String>,
    queue_entry_id: String,
    queue_index: usize,
    playback_context: PlaybackQueueContext,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ExternalSubtitle {
    url: SameOriginUrl,
    title: String,
    selected: bool,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PlaybackPreparationWarningCode {
    ExternalSubtitleUnavailable,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackPreparationWarning {
    code: PlaybackPreparationWarningCode,
    stream_index: i32,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackPreparationDiagnostics {
    total_ms: u64,
    item_ms: u64,
    resolve_ms: u64,
    neighbors_ms: u64,
    neighbors_succeeded: bool,
    playback_info_ms: u64,
    planning_ms: u64,
    resolved_item_changed: bool,
    background_image_downloads: usize,
    active_image_downloads: usize,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlaybackPreparedOutcome {
    item_id: String,
    report_session_id: String,
    audio_tracks: Vec<PlaybackTrackDescriptor>,
    subtitle_tracks: Vec<PlaybackTrackDescriptor>,
    url: SameOriginUrl,
    headers: BTreeMap<String, String>,
    resume_ticks: u64,
    title: String,
    previous_item: Option<PlaybackQueueItem>,
    next_item: Option<PlaybackQueueItem>,
    playback_context: PlaybackQueueContext,
    playback_queue: Vec<PlaybackQueueItem>,
    current_queue_index: usize,
    external_subtitles: Vec<ExternalSubtitle>,
    playback_warnings: Vec<PlaybackPreparationWarning>,
    intro_start_ticks: Option<u64>,
    intro_end_ticks: Option<u64>,
    danmaku_warning: Option<String>,
    danmaku_search_anime: String,
    danmaku_search_episode: Option<String>,
    preparation_diagnostics: PlaybackPreparationDiagnostics,
}

#[derive(Clone)]
pub(crate) struct ActivePlayback {
    pub(crate) item_id: String,
    pub(crate) media_source_id: String,
    pub(crate) play_session_id: String,
    pub(crate) play_method: &'static str,
    pub(crate) media_url: SameOriginUrl,
    pub(crate) file_name: String,
    pub(crate) file_size: Option<u64>,
    pub(crate) duration_seconds: Option<u64>,
    pub(crate) series_name: String,
    pub(crate) episode_number: Option<i32>,
    pub(crate) season_number: Option<i32>,
    pub(crate) playlist_item_id: Option<String>,
    pub(crate) playlist_index: i32,
    pub(crate) playlist_length: i32,
}

#[derive(Clone, Debug)]
struct PlaybackQueueEntry {
    item: BaseItem,
    playlist_entry_id: Option<String>,
    image_url: Option<String>,
}

#[derive(Clone, Debug)]
struct PlaybackQueue {
    kind: PlaybackContextKind,
    source_id: Option<String>,
    source_title: Option<String>,
    entries: Vec<PlaybackQueueEntry>,
    current_index: usize,
}

fn playback_queue_candidate(item: &BaseItem) -> bool {
    item.location_type.as_deref() != Some("Virtual")
        && matches!(
            item.item_type.as_deref(),
            Some("Episode" | "Movie" | "Video" | "MusicVideo" | "Series" | "Season")
        )
}

fn single_playback_queue(item: BaseItem) -> PlaybackQueue {
    PlaybackQueue {
        kind: PlaybackContextKind::Single,
        source_id: None,
        source_title: None,
        entries: vec![PlaybackQueueEntry {
            item,
            playlist_entry_id: None,
            image_url: None,
        }],
        current_index: 0,
    }
}

fn series_playback_queue(
    items: Vec<BaseItem>,
    current_item_id: &str,
    source_id: &str,
    source_title: Option<String>,
) -> Option<PlaybackQueue> {
    let entries: Vec<_> = items
        .into_iter()
        .filter(playback_queue_candidate)
        .map(|item| PlaybackQueueEntry {
            item,
            playlist_entry_id: None,
            image_url: None,
        })
        .collect();
    let current_index = entries
        .iter()
        .position(|entry| entry.item.id == current_item_id)?;
    Some(PlaybackQueue {
        kind: PlaybackContextKind::Series,
        source_id: Some(source_id.to_owned()),
        source_title,
        entries,
        current_index,
    })
}

fn playlist_playback_queue(
    items: Vec<BaseItem>,
    requested_item_id: &str,
    context: &PlaybackContext,
) -> Option<PlaybackQueue> {
    let entries: Vec<_> = items
        .into_iter()
        .filter(playback_queue_candidate)
        .map(|item| PlaybackQueueEntry {
            playlist_entry_id: item.playlist_item_id.clone(),
            item,
            image_url: None,
        })
        .collect();
    let current_index = context
        .playlist_entry_id
        .as_deref()
        .and_then(|entry_id| {
            entries.iter().position(|entry| {
                entry.playlist_entry_id.as_deref() == Some(entry_id)
                    && entry.item.id == requested_item_id
            })
        })
        .or_else(|| {
            context.queue_index.and_then(|index| {
                entries
                    .get(index)
                    .is_some_and(|entry| entry.item.id == requested_item_id)
                    .then_some(index)
            })
        })
        .or_else(|| {
            entries
                .iter()
                .position(|entry| entry.item.id == requested_item_id)
        })?;
    Some(PlaybackQueue {
        kind: PlaybackContextKind::Playlist,
        source_id: context.source_id.clone(),
        source_title: context.source_title.clone(),
        entries,
        current_index,
    })
}

fn playback_queue_context(queue: &PlaybackQueue, index: usize) -> PlaybackQueueContext {
    let entry = &queue.entries[index];
    PlaybackQueueContext {
        kind: queue.kind,
        source_id: queue.source_id.clone(),
        source_title: queue.source_title.clone(),
        playlist_entry_id: entry.playlist_entry_id.clone(),
        queue_index: index,
    }
}

fn playback_queue_item(queue: &PlaybackQueue, index: usize) -> PlaybackQueueItem {
    let entry = &queue.entries[index];
    let queue_entry_id = entry.playlist_entry_id.clone().unwrap_or_else(|| {
        format!(
            "{}:{index}:{}",
            queue.source_id.as_deref().unwrap_or(queue.kind.as_str()),
            entry.item.id
        )
    });
    PlaybackQueueItem {
        id: entry.item.id.clone(),
        title: playback_title(&entry.item),
        subtitle: card_subtitle(&entry.item, true),
        item_type: entry.item.item_type.clone(),
        image_url: entry.image_url.clone(),
        playlist_entry_id: entry.playlist_entry_id.clone(),
        queue_entry_id,
        queue_index: index,
        playback_context: playback_queue_context(queue, index),
    }
}

impl Application {
    fn build_playback_queue(
        &self,
        client: &EmbyClient,
        requested_item: &BaseItem,
        playable_item: &BaseItem,
        context: &PlaybackContext,
    ) -> (PlaybackQueue, bool) {
        if context.kind == PlaybackContextKind::Single {
            return (single_playback_queue(playable_item.clone()), true);
        }

        if let Some(source_id) = context
            .source_id
            .as_deref()
            .filter(|_| context.kind == PlaybackContextKind::Playlist)
        {
            match self.block_on_emby(client.playlist_items(source_id)) {
                Ok(result) => {
                    if let Some(queue) =
                        playlist_playback_queue(result.items, &requested_item.id, context)
                    {
                        return (queue, true);
                    }
                }
                Err(error) => {
                    tracing::warn!(
                        playlist_id = source_id,
                        error = %error,
                        "playlist playback context could not be resolved; falling back"
                    );
                }
            }
        }

        let inferred_series_id = playable_item.series_id.as_deref();
        let requested_series_id = (context.kind == PlaybackContextKind::Series)
            .then_some(context.source_id.as_deref())
            .flatten();
        let mut series_ids = Vec::with_capacity(2);
        if let Some(series_id) = requested_series_id {
            series_ids.push(series_id);
        }
        if let Some(series_id) =
            inferred_series_id.filter(|series_id| !series_ids.contains(series_id))
        {
            series_ids.push(series_id);
        }
        let mut queue_request_succeeded = context.kind != PlaybackContextKind::Playlist;
        for series_id in series_ids {
            match self.block_on_emby(client.episodes(series_id, None)) {
                Ok(result) => {
                    queue_request_succeeded = true;
                    let source_title = context
                        .source_title
                        .clone()
                        .filter(|_| requested_series_id == Some(series_id))
                        .or_else(|| playable_item.series_name.clone());
                    if let Some(queue) = series_playback_queue(
                        result.items,
                        &playable_item.id,
                        series_id,
                        source_title,
                    ) {
                        return (queue, true);
                    }
                }
                Err(error) => {
                    queue_request_succeeded = false;
                    tracing::warn!(
                        series_id,
                        error = %error,
                        "series playback context could not be resolved; falling back"
                    );
                }
            }
        }
        (
            single_playback_queue(playable_item.clone()),
            queue_request_succeeded,
        )
    }

    #[allow(clippy::too_many_lines)]
    pub fn prepare_playback(
        &self,
        item_id: &str,
        context: &PlaybackContext,
    ) -> Result<PlaybackPreparedOutcome, ApplicationError> {
        let total_started = Instant::now();
        let background_image_downloads = self
            .image_downloads
            .lock()
            .map(|downloads| downloads.len())
            .unwrap_or_default();
        let active_image_downloads = IMAGE_DOWNLOAD_CONCURRENCY
            .saturating_sub(self.image_download_slots.available_permits());
        let client = self.active_client()?;
        let item_started = Instant::now();
        let requested_item = self
            .block_on_emby(client.item(item_id))
            .map_err(|error| playback_stage_error("item", item_started, &error))?;
        let item_ms = elapsed_millis(item_started);
        let resolve_started = Instant::now();
        let item = self
            .resolve_playable_item(&client, requested_item.clone())
            .map_err(|error| playback_stage_error("resolve-playable", resolve_started, &error))?;
        let resolve_ms = elapsed_millis(resolve_started);
        let playable_id = item.id.clone();
        let neighbors_started = Instant::now();
        let requested_context = context.normalized();
        let (mut playback_queue, neighbors_succeeded) =
            self.build_playback_queue(&client, &requested_item, &item, &requested_context);
        // cache_images returns stable local URLs immediately and performs the
        // downloads behind a bounded semaphore. Prime every row so scrolling
        // never exposes permanent placeholders, without blocking playback.
        let image_items: Vec<_> = playback_queue
            .entries
            .iter()
            .map(|entry| entry.item.clone())
            .collect();
        match self.cache_images(&client, &image_items, ImagePurpose::EpisodeStill) {
            Ok(images) => {
                for (entry, image_url) in playback_queue.entries.iter_mut().zip(images) {
                    entry.image_url = image_url;
                }
            }
            Err(error) => tracing::warn!(
                error = %error,
                "playback queue artwork could not be prepared; using placeholders"
            ),
        }
        let previous_item = playback_queue
            .current_index
            .checked_sub(1)
            .map(|index| playback_queue_item(&playback_queue, index));
        let next_item = (playback_queue.current_index + 1 < playback_queue.entries.len())
            .then(|| playback_queue_item(&playback_queue, playback_queue.current_index + 1));
        let neighbors_ms = elapsed_millis(neighbors_started);
        let playback_info_started = Instant::now();
        let playback = self
            .block_on_emby(client.playback_info(&playable_id))
            .map_err(|error| {
                playback_stage_error("playback-info", playback_info_started, &error)
            })?;
        let playback_info_ms = elapsed_millis(playback_info_started);
        let resume_ticks = item
            .user_data
            .as_ref()
            .map_or(0, |data| data.playback_position_ticks);
        let planning_started = Instant::now();
        let plan = PlaybackPlanner::plan(
            &client.profile().base_url,
            &playable_id,
            resume_ticks,
            &playback,
            client.access_token(),
        )
        .map_err(|error| {
            playback_stage_error(
                "plan",
                planning_started,
                &ApplicationError::unsupported(error.to_string()),
            )
        })?;
        let planning_ms = elapsed_millis(planning_started);
        if !plan.warnings.is_empty() {
            tracing::warn!(
                ignored_external_subtitles = plan.warnings.len(),
                "unsafe or unavailable external subtitles were omitted from playback"
            );
        }
        let playback_warnings: Vec<_> = plan
            .warnings
            .iter()
            .map(|warning| match warning {
                PlaybackWarning::ExternalSubtitleUnavailable { index } => {
                    PlaybackPreparationWarning {
                        code: PlaybackPreparationWarningCode::ExternalSubtitleUnavailable,
                        stream_index: *index,
                    }
                }
            })
            .collect();

        let audio_tracks: Vec<_> = plan
            .tracks
            .iter()
            .filter(|track| track.kind == TrackKind::Audio)
            .map(|track| PlaybackTrackDescriptor {
                kind: PreparedTrackKind::Audio,
                stream_index: track.index,
                external: track.external,
                delivery_url: track.delivery_url.clone(),
                language: track.language.clone(),
                codec: track.codec.clone(),
                title: track.title.clone(),
                selected: plan.audio_stream_index == Some(track.index),
            })
            .collect();
        let subtitle_tracks: Vec<_> = plan
            .tracks
            .iter()
            .filter(|track| track.kind == TrackKind::Subtitle)
            .map(|track| PlaybackTrackDescriptor {
                kind: PreparedTrackKind::Subtitle,
                stream_index: track.index,
                external: track.external,
                delivery_url: track.delivery_url.clone(),
                language: track.language.clone(),
                codec: track.codec.clone(),
                title: track.title.clone(),
                selected: plan.subtitle_stream_index == Some(track.index),
            })
            .collect();

        let external_subtitles: Vec<_> = plan
            .tracks
            .iter()
            .filter(|track| track.kind == TrackKind::Subtitle && track.external)
            .filter_map(|track| {
                track.delivery_url.clone().map(|url| ExternalSubtitle {
                    url,
                    title: track
                        .title
                        .clone()
                        .unwrap_or_else(|| "External subtitle".to_owned()),
                    selected: plan.subtitle_stream_index == Some(track.index),
                })
            })
            .collect();
        let intro_range = playback
            .media_sources
            .iter()
            .find(|source| source.id == plan.media_source_id)
            .and_then(|source| intro_range_from_chapters(&source.chapters))
            .or_else(|| intro_range_from_chapters(&item.chapters));
        let source = playback
            .media_sources
            .iter()
            .find(|source| source.id == plan.media_source_id)
            .ok_or("the selected Emby media source disappeared")?;
        let file_name = source.path.as_deref().map_or_else(
            || item.name.clone(),
            |path| media_file_stem(path, &item.name),
        );
        *self
            .active_playback
            .lock()
            .map_err(|_| "playback lock is poisoned")? = Some(ActivePlayback {
            item_id: plan.item_id.clone(),
            media_source_id: plan.media_source_id.clone(),
            play_session_id: plan.play_session_id.clone(),
            play_method: match plan.method {
                PlaybackMethod::DirectStream => "DirectStream",
                PlaybackMethod::Transcode => "Transcode",
            },
            media_url: plan.url.clone(),
            file_name,
            file_size: source.size,
            duration_seconds: source.run_time_ticks.map(|ticks| ticks / 10_000_000),
            series_name: item
                .series_name
                .clone()
                .unwrap_or_else(|| item.name.clone()),
            episode_number: item.index_number,
            season_number: item.parent_index_number,
            playlist_item_id: playback_queue.entries[playback_queue.current_index]
                .playlist_entry_id
                .clone(),
            playlist_index: i32::try_from(playback_queue.current_index).unwrap_or(i32::MAX),
            playlist_length: i32::try_from(playback_queue.entries.len()).unwrap_or(i32::MAX),
        });
        let current_queue_index = playback_queue.current_index;
        let playback_context = playback_queue_context(&playback_queue, current_queue_index);
        let playback_queue_items: Vec<_> = (0..playback_queue.entries.len())
            .map(|index| playback_queue_item(&playback_queue, index))
            .collect();
        Ok(PlaybackPreparedOutcome {
            item_id: plan.item_id,
            report_session_id: plan.play_session_id,
            audio_tracks,
            subtitle_tracks,
            url: plan.url,
            headers: (*plan.request_headers).clone(),
            resume_ticks: plan.resume_position_ticks,
            title: playback_title(&item),
            previous_item,
            next_item,
            playback_context,
            playback_queue: playback_queue_items,
            current_queue_index,
            external_subtitles,
            playback_warnings,
            intro_start_ticks: intro_range.map(|range| range.0),
            intro_end_ticks: intro_range.map(|range| range.1),
            danmaku_warning: None,
            danmaku_search_anime: item
                .series_name
                .clone()
                .unwrap_or_else(|| item.name.clone()),
            danmaku_search_episode: item.index_number.map(|value| value.to_string()),
            preparation_diagnostics: PlaybackPreparationDiagnostics {
                total_ms: elapsed_millis(total_started),
                item_ms,
                resolve_ms,
                neighbors_ms,
                neighbors_succeeded,
                playback_info_ms,
                planning_ms,
                resolved_item_changed: playable_id != item_id,
                background_image_downloads,
                active_image_downloads,
            },
        })
    }

    pub fn report_playback(&self, telemetry: &PlaybackTelemetry) -> Result<(), ApplicationError> {
        let playback = {
            let active = self
                .active_playback
                .lock()
                .map_err(|_| ApplicationError::internal("playback lock is poisoned"))?;
            active.clone()
        }
        .ok_or_else(|| {
            ApplicationError::new(
                crate::ApplicationErrorCode::NotConnected,
                "no playback session is active",
            )
        })?;
        ensure_current_report_session(&playback.play_session_id, &telemetry.report_session_id)?;
        let client = self.active_client()?;
        self.block_on_emby(async {
            let progress = PlaybackProgress {
                item_id: &playback.item_id,
                media_source_id: &playback.media_source_id,
                play_session_id: &playback.play_session_id,
                position_ticks: telemetry.position_ticks,
                is_paused: telemetry.paused,
                is_muted: telemetry.muted,
                volume_level: telemetry.volume.min(100),
                playback_rate: if telemetry.rate.is_finite() && telemetry.rate > 0.0 {
                    telemetry.rate
                } else {
                    1.0
                },
                play_method: playback.play_method,
                audio_stream_index: telemetry.audio_stream_index,
                subtitle_stream_index: telemetry.subtitle_stream_index,
                event_name: match telemetry.event {
                    PlaybackReportKind::Progress if telemetry.paused => Some("Pause"),
                    PlaybackReportKind::Progress => Some("TimeUpdate"),
                    PlaybackReportKind::Started | PlaybackReportKind::Stopped => None,
                },
                playlist_item_id: playback.playlist_item_id.as_deref(),
                playlist_index: Some(playback.playlist_index),
                playlist_length: Some(playback.playlist_length),
                can_seek: telemetry.seekable,
            };
            match telemetry.event {
                PlaybackReportKind::Started => client.report_started(&progress).await,
                PlaybackReportKind::Progress => client.report_progress(&progress).await,
                PlaybackReportKind::Stopped => client.report_stopped(&progress).await,
            }
        })?;
        if matches!(telemetry.event, PlaybackReportKind::Stopped) {
            let mut active = self
                .active_playback
                .lock()
                .map_err(|_| ApplicationError::internal("playback lock is poisoned"))?;
            if active
                .as_ref()
                .is_some_and(|current| current.play_session_id == playback.play_session_id)
            {
                *active = None;
            }
        }
        Ok(())
    }

    fn resolve_playable_item(
        &self,
        client: &EmbyClient,
        item: BaseItem,
    ) -> Result<BaseItem, ApplicationError> {
        match item.item_type.as_deref() {
            Some("Series") => {
                let next_up = self.block_on_emby(client.next_up(Some(&item.id), 1))?;
                if let Some(next) = next_up.items.into_iter().find(is_playable_item) {
                    return Ok(next);
                }
                let episodes = self.block_on_emby(client.episodes(&item.id, None))?;
                select_episode(episodes.items).ok_or_else(|| {
                    ApplicationError::not_found("this series has no playable episodes")
                })
            }
            Some("Season") => {
                let series_id = item.series_id.as_deref().ok_or_else(|| {
                    ApplicationError::unsupported("Emby did not provide the season's series id")
                })?;
                let episodes = self.block_on_emby(client.episodes(series_id, Some(&item.id)))?;
                select_episode(episodes.items).ok_or_else(|| {
                    ApplicationError::not_found("this season has no playable episodes")
                })
            }
            Some("Episode" | "Movie" | "Video" | "MusicVideo") => Ok(item),
            _ => Err(ApplicationError::unsupported(
                "this library item cannot be played",
            )),
        }
    }
}

fn elapsed_millis(started: Instant) -> u64 {
    u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX)
}

fn playback_stage_error(
    stage: &str,
    started: Instant,
    error: &ApplicationError,
) -> ApplicationError {
    ApplicationError::new(
        error.code(),
        format!(
            "playback preparation stage {stage} failed after {} ms: {error}",
            elapsed_millis(started)
        ),
    )
}

fn ensure_current_report_session(
    active_session_id: &str,
    report_session_id: &str,
) -> Result<(), ApplicationError> {
    if active_session_id == report_session_id {
        Ok(())
    } else {
        Err(ApplicationError::new(
            crate::ApplicationErrorCode::Cancelled,
            "the playback report belongs to a stale session",
        ))
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use serde_json::json;
    use url::Url;
    use yanami_core::SameOriginUrl;
    use yanami_emby::BaseItem;

    use super::{
        ExternalSubtitle, PlaybackPreparationDiagnostics, PlaybackPreparationWarning,
        PlaybackPreparationWarningCode, PlaybackPreparedOutcome, PlaybackQueueContext,
        PlaybackQueueItem, PlaybackTrackDescriptor, PreparedTrackKind,
        ensure_current_report_session, playback_queue_context, playback_queue_item,
        playlist_playback_queue, series_playback_queue,
    };
    use crate::{ApplicationErrorCode, PlaybackContext, PlaybackContextKind};

    #[test]
    fn stale_report_session_is_rejected_before_server_io() {
        assert!(ensure_current_report_session("active", "active").is_ok());
        let error = ensure_current_report_session("active", "stale").unwrap_err();
        assert_eq!(error.code(), ApplicationErrorCode::Cancelled);
        assert!(error.message().contains("stale session"));
    }

    #[test]
    fn playlist_queue_preserves_order_and_disambiguates_duplicate_items() {
        let item = |id: &str, entry_id: &str| BaseItem {
            id: id.to_owned(),
            name: format!("Item {entry_id}"),
            item_type: Some("Episode".to_owned()),
            playlist_item_id: Some(entry_id.to_owned()),
            ..BaseItem::default()
        };
        let context = PlaybackContext {
            kind: PlaybackContextKind::Playlist,
            source_id: Some("playlist-1".to_owned()),
            source_title: Some("My list".to_owned()),
            playlist_entry_id: Some("entry-a-2".to_owned()),
            queue_index: Some(0),
        };
        let queue = playlist_playback_queue(
            vec![
                item("episode-a", "entry-a-1"),
                item("episode-b", "entry-b"),
                item("episode-a", "entry-a-2"),
            ],
            "episode-a",
            &context,
        )
        .unwrap();

        assert_eq!(queue.current_index, 2);
        assert_eq!(queue.entries[0].item.id, "episode-a");
        assert_eq!(queue.entries[1].item.id, "episode-b");
        assert_eq!(queue.entries[2].item.id, "episode-a");
        assert_eq!(
            playback_queue_context(&queue, queue.current_index)
                .playlist_entry_id
                .as_deref(),
            Some("entry-a-2")
        );
        assert_eq!(
            playback_queue_item(&queue, 1).playback_context.queue_index,
            1
        );
    }

    #[test]
    fn series_queue_crosses_season_boundary_in_server_order() {
        let episode = |id: &str, season: i32, index: i32| BaseItem {
            id: id.to_owned(),
            name: id.to_owned(),
            item_type: Some("Episode".to_owned()),
            series_id: Some("series-1".to_owned()),
            series_name: Some("Series".to_owned()),
            parent_index_number: Some(season),
            index_number: Some(index),
            ..BaseItem::default()
        };
        let queue = series_playback_queue(
            vec![
                episode("s1e1", 1, 1),
                episode("s1e2", 1, 2),
                episode("s2e1", 2, 1),
            ],
            "s1e2",
            "series-1",
            Some("Series".to_owned()),
        )
        .unwrap();

        assert_eq!(queue.current_index, 1);
        assert_eq!(queue.entries[queue.current_index - 1].item.id, "s1e1");
        assert_eq!(queue.entries[queue.current_index + 1].item.id, "s2e1");
    }

    #[test]
    #[allow(clippy::too_many_lines)] // The full object guards the flat schema 8 ABI contract.
    fn prepared_outcome_preserves_schema_eight_wire_shape() {
        let trusted = Url::parse("https://media.example.test/emby").unwrap();
        let stream_url = SameOriginUrl::new(
            &trusted,
            Url::parse("https://media.example.test/Videos/item/stream").unwrap(),
        )
        .unwrap();
        let context = PlaybackQueueContext {
            kind: PlaybackContextKind::Series,
            source_id: Some("series-1".to_owned()),
            source_title: Some("Series".to_owned()),
            playlist_entry_id: None,
            queue_index: 0,
        };
        let queue_item = PlaybackQueueItem {
            id: "episode-1".to_owned(),
            title: "Series - S01E01".to_owned(),
            subtitle: "Pilot".to_owned(),
            item_type: Some("Episode".to_owned()),
            image_url: Some("file:///cache/episode-1.jpg".to_owned()),
            playlist_entry_id: None,
            queue_entry_id: "series-1:0:episode-1".to_owned(),
            queue_index: 0,
            playback_context: context.clone(),
        };
        let outcome = PlaybackPreparedOutcome {
            item_id: "episode-1".to_owned(),
            report_session_id: "report-1".to_owned(),
            audio_tracks: vec![PlaybackTrackDescriptor {
                kind: PreparedTrackKind::Audio,
                stream_index: 1,
                external: false,
                delivery_url: None,
                language: Some("eng".to_owned()),
                codec: Some("aac".to_owned()),
                title: Some("English".to_owned()),
                selected: true,
            }],
            subtitle_tracks: Vec::new(),
            url: stream_url.clone(),
            headers: BTreeMap::from([("user-agent".to_owned(), "Yanami".to_owned())]),
            resume_ticks: 42,
            title: "Series - S01E01".to_owned(),
            previous_item: None,
            next_item: None,
            playback_context: context,
            playback_queue: vec![queue_item],
            current_queue_index: 0,
            external_subtitles: Vec::<ExternalSubtitle>::new(),
            playback_warnings: vec![PlaybackPreparationWarning {
                code: PlaybackPreparationWarningCode::ExternalSubtitleUnavailable,
                stream_index: 3,
            }],
            intro_start_ticks: Some(10),
            intro_end_ticks: Some(20),
            danmaku_warning: None,
            danmaku_search_anime: "Series".to_owned(),
            danmaku_search_episode: Some("1".to_owned()),
            preparation_diagnostics: PlaybackPreparationDiagnostics {
                total_ms: 9,
                item_ms: 1,
                resolve_ms: 1,
                neighbors_ms: 2,
                neighbors_succeeded: true,
                playback_info_ms: 3,
                planning_ms: 2,
                resolved_item_changed: false,
                background_image_downloads: 4,
                active_image_downloads: 1,
            },
        };

        assert_eq!(
            serde_json::to_value(outcome).unwrap(),
            json!({
                "itemId": "episode-1",
                "reportSessionId": "report-1",
                "audioTracks": [{
                    "kind": "audio",
                    "streamIndex": 1,
                    "external": false,
                    "deliveryUrl": null,
                    "language": "eng",
                    "codec": "aac",
                    "title": "English",
                    "selected": true
                }],
                "subtitleTracks": [],
                "url": stream_url,
                "headers": {"user-agent": "Yanami"},
                "resumeTicks": 42,
                "title": "Series - S01E01",
                "previousItem": null,
                "nextItem": null,
                "playbackContext": {
                    "kind": "series",
                    "sourceId": "series-1",
                    "sourceTitle": "Series",
                    "playlistEntryId": null,
                    "queueIndex": 0
                },
                "playbackQueue": [{
                    "id": "episode-1",
                    "title": "Series - S01E01",
                    "subtitle": "Pilot",
                    "itemType": "Episode",
                    "imageUrl": "file:///cache/episode-1.jpg",
                    "playlistEntryId": null,
                    "queueEntryId": "series-1:0:episode-1",
                    "queueIndex": 0,
                    "playbackContext": {
                        "kind": "series",
                        "sourceId": "series-1",
                        "sourceTitle": "Series",
                        "playlistEntryId": null,
                        "queueIndex": 0
                    }
                }],
                "currentQueueIndex": 0,
                "externalSubtitles": [],
                "playbackWarnings": [{
                    "code": "external_subtitle_unavailable",
                    "streamIndex": 3
                }],
                "introStartTicks": 10,
                "introEndTicks": 20,
                "danmakuWarning": null,
                "danmakuSearchAnime": "Series",
                "danmakuSearchEpisode": "1",
                "preparationDiagnostics": {
                    "totalMs": 9,
                    "itemMs": 1,
                    "resolveMs": 1,
                    "neighborsMs": 2,
                    "neighborsSucceeded": true,
                    "playbackInfoMs": 3,
                    "planningMs": 2,
                    "resolvedItemChanged": false,
                    "backgroundImageDownloads": 4,
                    "activeImageDownloads": 1
                }
            })
        );
    }
}
