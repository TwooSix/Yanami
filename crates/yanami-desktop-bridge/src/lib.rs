//! Narrow C ABI used by the Qt shell. All returned strings are owned by Rust
//! and must be released with `yanami_string_free`.

use std::{
    collections::BTreeMap,
    ffi::{CStr, CString, c_char},
    fs,
    panic::{AssertUnwindSafe, catch_unwind},
    path::{Path, PathBuf},
    ptr,
    sync::{Arc, Mutex},
    time::Duration,
};

use chrono::Utc;
use secrecy::SecretString;
use serde_json::json;
use tokio::runtime::{Builder, Runtime};
use url::Url;
use uuid::Uuid;
use zeroize::Zeroize;

use yanami_application::{ApplicationServices, DandanCredentialSource};
use yanami_core::{PlaybackMethod, ServerProfile, TrackKind, UserSession};
use yanami_danmaku::{AssConfig, AssGenerator, MatchInput, hash_remote_prefix};
use yanami_emby::{
    BaseItem, ChapterInfo, ClientIdentity, EmbyClient, ItemQuery, PlaybackPlanner,
    PlaybackPreference, PlaybackProgress,
};
use yanami_storage::{AppStorage, DanmakuMatchRecord, SystemCredentialVault};

struct ActiveSession {
    profile: ServerProfile,
    session: UserSession,
}

#[derive(Clone)]
struct ActivePlayback {
    item_id: String,
    media_source_id: String,
    play_session_id: String,
    play_method: &'static str,
    audio_stream_index: Option<i32>,
    subtitle_stream_index: Option<i32>,
}

#[derive(Clone, Copy)]
enum PlaybackReportKind {
    Started,
    Progress,
    Stopped,
}

#[derive(Clone, Copy)]
enum ImagePurpose {
    Poster,
    EpisodeStill,
    Backdrop,
}

pub struct YanamiBackend {
    runtime: Runtime,
    services: ApplicationServices,
    data_dir: PathBuf,
    active_session: Mutex<Option<ActiveSession>>,
    active_playback: Mutex<Option<ActivePlayback>>,
    latest_episodes: Mutex<BTreeMap<String, BaseItem>>,
}

impl YanamiBackend {
    fn open(data_dir: &Path) -> Result<Self, String> {
        fs::create_dir_all(data_dir).map_err(display_error)?;
        let storage =
            Arc::new(AppStorage::open(data_dir.join("yanami.sqlite3")).map_err(display_error)?);
        let active_session = storage
            .latest_user_session()
            .map_err(display_error)?
            .and_then(|session| {
                storage
                    .list_servers()
                    .ok()?
                    .into_iter()
                    .find(|profile| profile.local_id == session.server_local_id)
                    .map(|profile| ActiveSession { profile, session })
            });
        let vault = Arc::new(SystemCredentialVault::new("Yanami"));
        let runtime = Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(display_error)?;
        Ok(Self {
            runtime,
            services: ApplicationServices::new(storage, vault),
            data_dir: data_dir.to_owned(),
            active_session: Mutex::new(active_session),
            active_playback: Mutex::new(None),
            latest_episodes: Mutex::new(BTreeMap::new()),
        })
    }

    fn login_emby(
        &self,
        server_name: &str,
        server_url: &str,
        username: &str,
        password: &str,
    ) -> Result<(), String> {
        let mut profile = ServerProfile::new(
            server_name.trim(),
            Url::parse(server_url.trim()).map_err(display_error)?,
        )
        .map_err(display_error)?;
        let device_id = Uuid::new_v4();
        let mut client = EmbyClient::new(
            profile.clone(),
            ClientIdentity::yanami(device_id.to_string()),
        )
        .map_err(display_error)?;
        let authentication = self
            .runtime
            .block_on(client.authenticate(username.trim(), password))
            .map_err(display_error)?;

        profile.server_id = Some(authentication.server_id.clone());
        let session = UserSession {
            server_local_id: profile.local_id,
            server_id: authentication.server_id,
            user_id: authentication.user.id,
            user_name: authentication.user.name,
            device_id,
            credential_key: format!("emby.{}.token", profile.local_id),
        };
        let now = Utc::now().timestamp();
        self.services
            .storage()
            .upsert_server(&profile, now)
            .map_err(display_error)?;
        self.services
            .save_emby_session(
                &session,
                &SecretString::from(authentication.access_token),
                now,
            )
            .map_err(display_error)?;
        *self
            .active_session
            .lock()
            .map_err(|_| "session lock is poisoned")? = Some(ActiveSession { profile, session });
        Ok(())
    }

    fn active_client(&self) -> Result<EmbyClient, String> {
        let active = self
            .active_session
            .lock()
            .map_err(|_| "session lock is poisoned")?;
        let active = active.as_ref().ok_or("no Emby server is connected")?;
        let token = self
            .services
            .emby_token(&active.session)
            .map_err(display_error)?
            .ok_or("the Emby login token is unavailable; please sign in again")?;
        EmbyClient::with_session(
            active.profile.clone(),
            ClientIdentity::yanami(active.session.device_id.to_string()),
            active.session.user_id.clone(),
            token,
        )
        .map_err(display_error)
    }

    fn logout_emby(&self) -> Result<(), String> {
        let mut active = self
            .active_session
            .lock()
            .map_err(|_| "session lock is poisoned")?;
        if let Some(session) = active.as_ref() {
            self.services
                .logout(&session.session)
                .map_err(display_error)?;
        }
        *active = None;
        *self
            .active_playback
            .lock()
            .map_err(|_| "playback lock is poisoned")? = None;
        self.latest_episodes
            .lock()
            .map_err(|_| "latest episode cache is poisoned")?
            .clear();
        Ok(())
    }

    fn library_json(&self) -> Result<String, String> {
        let client = self.active_client()?;
        let (library_result, views_result, recent_items, resume_result, latest_per_series) = self
            .runtime
            .block_on(async {
                let library_query = ItemQuery {
                    include_item_types: vec!["Movie".to_owned(), "Series".to_owned()],
                    recursive: true,
                    limit: 400,
                    sort_by: vec!["SortName".to_owned()],
                    sort_order: Some("Ascending".to_owned()),
                    ..ItemQuery::default()
                };
                let library_request = client.items(&library_query);
                let views_request = client.user_views();
                let recent_request = client.latest_items(&["Episode"], 20, false);
                let resume_query = ItemQuery {
                    include_item_types: vec!["Episode".to_owned()],
                    recursive: true,
                    limit: 20,
                    sort_by: vec!["DatePlayed".to_owned()],
                    sort_order: Some("Descending".to_owned()),
                    filters: vec!["IsResumable".to_owned()],
                    ..ItemQuery::default()
                };
                let resume_request = client.items(&resume_query);
                // Emby returns parent Series DTOs on some server versions when
                // GroupItems=true. Fetch a broad, newest-first episode window
                // instead so every card can show an actual SxxExx + episode name.
                let latest_request = client.latest_items(&["Episode"], 5000, false);
                tokio::try_join!(
                    library_request,
                    views_request,
                    recent_request,
                    resume_request,
                    latest_request
                )
            })
            .map_err(display_error)?;

        let mut latest_by_series = BTreeMap::new();
        for item in latest_per_series {
            if let Some(series_id) = item.series_id.clone() {
                latest_by_series.entry(series_id).or_insert(item);
            }
        }
        *self
            .latest_episodes
            .lock()
            .map_err(|_| "latest episode cache is poisoned")? = latest_by_series.clone();
        let views: Vec<_> = views_result
            .items
            .into_iter()
            .filter(is_supported_view)
            .collect();
        let resume_items: Vec<_> = resume_result
            .items
            .into_iter()
            .filter(is_playable_item)
            .collect();
        let library_images =
            self.cache_images(&client, &library_result.items, ImagePurpose::Poster)?;
        let view_images = self.cache_images(&client, &views, ImagePurpose::Backdrop)?;
        let recent_images =
            self.cache_images(&client, &recent_items, ImagePurpose::EpisodeStill)?;
        let resume_images =
            self.cache_images(&client, &resume_items, ImagePurpose::EpisodeStill)?;
        let library: Vec<_> = library_result
            .items
            .iter()
            .zip(library_images)
            .map(|(item, image_url)| {
                media_card_json(item, image_url, false, latest_by_series.get(&item.id))
            })
            .collect();
        let library_views: Vec<_> = views
            .iter()
            .zip(view_images)
            .map(|(item, image_url)| library_view_json(item, image_url))
            .collect();
        let recent: Vec<_> = recent_items
            .iter()
            .zip(recent_images)
            .map(|(item, image_url)| media_card_json(item, image_url, true, None))
            .collect();
        let resume: Vec<_> = resume_items
            .iter()
            .zip(resume_images)
            .map(|(item, image_url)| media_card_json(item, image_url, true, None))
            .collect();
        serde_json::to_string(&json!({
            "library": library,
            "views": library_views,
            "resume": resume,
            "recent": recent,
        }))
        .map_err(display_error)
    }

    fn activity_json(&self) -> Result<String, String> {
        let client = self.active_client()?;
        let (recent_items, resume_result) = self
            .runtime
            .block_on(async {
                let recent_request = client.latest_items(&["Episode"], 20, false);
                let resume_query = ItemQuery {
                    include_item_types: vec!["Episode".to_owned()],
                    recursive: true,
                    limit: 20,
                    sort_by: vec!["DatePlayed".to_owned()],
                    sort_order: Some("Descending".to_owned()),
                    filters: vec!["IsResumable".to_owned()],
                    ..ItemQuery::default()
                };
                let resume_request = client.items(&resume_query);
                tokio::try_join!(recent_request, resume_request)
            })
            .map_err(display_error)?;
        let resume_items: Vec<_> = resume_result
            .items
            .into_iter()
            .filter(is_playable_item)
            .collect();
        let recent_images =
            self.cache_images(&client, &recent_items, ImagePurpose::EpisodeStill)?;
        let resume_images =
            self.cache_images(&client, &resume_items, ImagePurpose::EpisodeStill)?;
        let recent: Vec<_> = recent_items
            .iter()
            .zip(recent_images)
            .map(|(item, image_url)| media_card_json(item, image_url, true, None))
            .collect();
        let resume: Vec<_> = resume_items
            .iter()
            .zip(resume_images)
            .map(|(item, image_url)| media_card_json(item, image_url, true, None))
            .collect();
        serde_json::to_string(&json!({
            "resume": resume,
            "recent": recent,
        }))
        .map_err(display_error)
    }

    fn collection_json(&self, parent_id: &str) -> Result<String, String> {
        let client = self.active_client()?;
        let parent = self
            .runtime
            .block_on(client.item(parent_id))
            .map_err(display_error)?;
        let (items, purpose, continue_item) = match parent.item_type.as_deref() {
            Some("Series") => {
                let (seasons, next_up) = self
                    .runtime
                    .block_on(async {
                        tokio::try_join!(client.seasons(parent_id), client.next_up(parent_id))
                    })
                    .map_err(display_error)?;
                (
                    seasons.items,
                    ImagePurpose::Poster,
                    next_up.items.into_iter().find(is_playable_item),
                )
            }
            Some("Season") => {
                let series_id = parent
                    .series_id
                    .as_deref()
                    .ok_or("Emby did not provide the season's series id")?;
                let episodes: Vec<_> = self
                    .runtime
                    .block_on(client.episodes(series_id, Some(parent_id)))
                    .map_err(display_error)?
                    .items
                    .into_iter()
                    .filter(is_playable_item)
                    .collect();
                let continue_item = select_episode(episodes.clone());
                (episodes, ImagePurpose::EpisodeStill, continue_item)
            }
            _ if is_supported_view(&parent) => {
                let items = self
                    .runtime
                    .block_on(client.items(&ItemQuery {
                        parent_id: Some(parent_id.to_owned()),
                        include_item_types: vec!["Movie".to_owned(), "Series".to_owned()],
                        recursive: true,
                        limit: 2000,
                        sort_by: vec!["SortName".to_owned()],
                        sort_order: Some("Ascending".to_owned()),
                        ..ItemQuery::default()
                    }))
                    .map_err(display_error)?
                    .items;
                (items, ImagePurpose::Poster, None)
            }
            _ => return Err("this item cannot be opened as a collection".to_owned()),
        };
        let image_urls = self.cache_images(&client, &items, purpose)?;
        let latest_episodes = self
            .latest_episodes
            .lock()
            .map_err(|_| "latest episode cache is poisoned")?;
        let children: Vec<_> = items
            .iter()
            .zip(image_urls)
            .map(|(item, image_url)| {
                media_card_json(item, image_url, false, latest_episodes.get(&item.id))
            })
            .collect();
        drop(latest_episodes);
        let parent_poster = self
            .cache_images(&client, std::slice::from_ref(&parent), ImagePurpose::Poster)?
            .into_iter()
            .next()
            .flatten();
        let parent_backdrop = self
            .cache_images(
                &client,
                std::slice::from_ref(&parent),
                ImagePurpose::Backdrop,
            )?
            .into_iter()
            .next()
            .flatten();
        let continue_label = continue_item
            .as_ref()
            .map(|item| card_subtitle(item, true))
            .unwrap_or_default();
        serde_json::to_string(&json!({
            "parent": {
                "id": parent.id,
                "title": parent.name,
                "itemType": parent.item_type,
                "collectionType": parent.collection_type,
                "seriesId": parent.series_id,
                "subtitle": parent.collection_type.as_ref().map_or_else(
                    || card_subtitle(&parent, false),
                    |_| String::new(),
                ),
                "childCount": parent.recursive_item_count.or(parent.child_count),
                "unplayedCount": parent
                    .user_data
                    .as_ref()
                    .and_then(|data| data.unplayed_item_count),
                "productionYear": parent.production_year,
                "hasLatestEpisode": false,
                "overview": parent.overview,
                "imageUrl": parent_poster,
                "backdropUrl": parent_backdrop,
                "continueLabel": continue_label,
            },
            "items": children,
        }))
        .map_err(display_error)
    }

    fn cache_images(
        &self,
        client: &EmbyClient,
        items: &[BaseItem],
        purpose: ImagePurpose,
    ) -> Result<Vec<Option<String>>, String> {
        let cache_dir = self.data_dir.join("cache").join("images");
        fs::create_dir_all(&cache_dir).map_err(display_error)?;

        let mut cache_paths = Vec::with_capacity(items.len());
        let mut downloads = Vec::new();
        for item in items {
            let Some((image_item_id, image_type, image_tag)) = image_reference(item, purpose)
            else {
                cache_paths.push(None);
                continue;
            };
            let cache_name = format!(
                "{}-{}-{}.jpg",
                safe_cache_component(image_item_id),
                image_type.to_ascii_lowercase(),
                safe_cache_component(image_tag)
            );
            let cache_path = cache_dir.join(cache_name);
            if !cache_path.is_file() {
                downloads.push((
                    image_item_id.to_owned(),
                    image_type.to_owned(),
                    image_tag.to_owned(),
                    cache_path.clone(),
                ));
            }
            cache_paths.push(Some(cache_path));
        }

        self.runtime.block_on(async {
            let semaphore = Arc::new(tokio::sync::Semaphore::new(12));
            let mut tasks = tokio::task::JoinSet::new();
            for (image_item_id, image_type, image_tag, cache_path) in downloads {
                let client = client.clone();
                let semaphore = Arc::clone(&semaphore);
                tasks.spawn(async move {
                    let Ok(_permit) = semaphore.acquire_owned().await else {
                        return;
                    };
                    let Ok(bytes) = client
                        .image(&image_item_id, &image_type, &image_tag, 720)
                        .await
                    else {
                        return;
                    };
                    let _ = fs::write(cache_path, bytes);
                });
            }
            while tasks.join_next().await.is_some() {}
        });

        Ok(cache_paths
            .into_iter()
            .map(|path| {
                path.filter(|value| value.is_file())
                    .and_then(|value| Url::from_file_path(value).ok())
                    .map(String::from)
            })
            .collect())
    }

    fn playback_json(&self, item_id: &str) -> Result<String, String> {
        let client = self.active_client()?;
        let requested_item = self
            .runtime
            .block_on(client.item(item_id))
            .map_err(display_error)?;
        let item = self.resolve_playable_item(&client, requested_item)?;
        let playable_id = item.id.clone();
        let (previous_item, next_item) = if item.item_type.as_deref() == Some("Episode") {
            item.series_id
                .as_deref()
                .and_then(|series_id| {
                    self.runtime
                        .block_on(client.episodes(series_id, None))
                        .ok()
                        .map(|result| neighboring_episodes(result.items, &playable_id))
                })
                .unwrap_or_default()
        } else {
            (None, None)
        };
        let playback = self
            .runtime
            .block_on(client.playback_info(&playable_id, None))
            .map_err(display_error)?;
        let resume_ticks = item
            .user_data
            .as_ref()
            .map_or(0, |data| data.playback_position_ticks);
        let plan = PlaybackPlanner::plan(
            &client.profile().base_url,
            &playable_id,
            resume_ticks,
            &playback,
            client.access_token(),
            PlaybackPreference::default(),
        )
        .map_err(display_error)?;

        let mut danmaku_file = None;
        let mut danmaku_warning = None;
        match self.services.dandanplay_configured() {
            Ok(true) => match self.prepare_danmaku(&client, &item.name, &playback, &plan) {
                Ok(path) => danmaku_file = path,
                Err(error) => danmaku_warning = Some(error),
            },
            Ok(false) => {}
            Err(error) => {
                danmaku_warning = Some(format!("credential status unavailable: {error}"));
            }
        }

        let external_subtitles: Vec<_> = plan
            .tracks
            .iter()
            .filter(|track| track.kind == TrackKind::Subtitle && track.external)
            .filter_map(|track| {
                track.delivery_url.as_ref().map(|url| {
                    json!({
                        "url": url,
                        "title": track.title.as_deref().unwrap_or("External subtitle"),
                        "selected": plan.subtitle_stream_index == Some(track.index),
                    })
                })
            })
            .collect();
        let intro_range = playback
            .media_sources
            .iter()
            .find(|source| source.id == plan.media_source_id)
            .and_then(|source| intro_range_from_chapters(&source.chapters))
            .or_else(|| intro_range_from_chapters(&item.chapters));
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
            audio_stream_index: plan.audio_stream_index,
            subtitle_stream_index: plan.subtitle_stream_index,
        });
        serde_json::to_string(&json!({
            "url": plan.url,
            "headers": &*plan.request_headers,
            "resumeTicks": plan.resume_position_ticks,
            "title": playback_title(&item),
            "previousItem": previous_item.as_ref().map(playback_navigation_item),
            "nextItem": next_item.as_ref().map(playback_navigation_item),
            "externalSubtitles": external_subtitles,
            "introStartTicks": intro_range.map(|range| range.0),
            "introEndTicks": intro_range.map(|range| range.1),
            "danmakuFile": danmaku_file,
            "danmakuWarning": danmaku_warning,
        }))
        .map_err(display_error)
    }

    fn report_playback(
        &self,
        kind: PlaybackReportKind,
        position_ticks: u64,
        is_paused: bool,
    ) -> Result<(), String> {
        let playback = {
            let active = self
                .active_playback
                .lock()
                .map_err(|_| "playback lock is poisoned")?;
            active.clone()
        }
        .ok_or("no playback session is active")?;
        let client = self.active_client()?;
        self.runtime
            .block_on(async {
                let progress = PlaybackProgress {
                    item_id: &playback.item_id,
                    media_source_id: &playback.media_source_id,
                    play_session_id: &playback.play_session_id,
                    position_ticks,
                    is_paused,
                    is_muted: false,
                    volume_level: 100,
                    playback_rate: 1.0,
                    play_method: playback.play_method,
                    audio_stream_index: playback.audio_stream_index,
                    subtitle_stream_index: playback.subtitle_stream_index,
                    event_name: match kind {
                        PlaybackReportKind::Progress if is_paused => Some("Pause"),
                        PlaybackReportKind::Progress => Some("TimeUpdate"),
                        PlaybackReportKind::Started | PlaybackReportKind::Stopped => None,
                    },
                    can_seek: true,
                };
                match kind {
                    PlaybackReportKind::Started => client.report_started(&progress).await,
                    PlaybackReportKind::Progress => client.report_progress(&progress).await,
                    PlaybackReportKind::Stopped => client.report_stopped(&progress).await,
                }
            })
            .map_err(display_error)?;
        if matches!(kind, PlaybackReportKind::Stopped) {
            let mut active = self
                .active_playback
                .lock()
                .map_err(|_| "playback lock is poisoned")?;
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
    ) -> Result<BaseItem, String> {
        match item.item_type.as_deref() {
            Some("Series") => {
                let next_up = self
                    .runtime
                    .block_on(client.next_up(&item.id))
                    .map_err(display_error)?;
                if let Some(next) = next_up.items.into_iter().find(is_playable_item) {
                    return Ok(next);
                }
                let episodes = self
                    .runtime
                    .block_on(client.episodes(&item.id, None))
                    .map_err(display_error)?;
                select_episode(episodes.items)
                    .ok_or_else(|| "this series has no playable episodes".to_owned())
            }
            Some("Season") => {
                let series_id = item
                    .series_id
                    .as_deref()
                    .ok_or("Emby did not provide the season's series id")?;
                let episodes = self
                    .runtime
                    .block_on(client.episodes(series_id, Some(&item.id)))
                    .map_err(display_error)?;
                select_episode(episodes.items)
                    .ok_or_else(|| "this season has no playable episodes".to_owned())
            }
            Some("Episode" | "Movie" | "Video") => Ok(item),
            _ => Err("this library item cannot be played".to_owned()),
        }
    }

    fn prepare_danmaku(
        &self,
        client: &EmbyClient,
        item_name: &str,
        playback: &yanami_emby::PlaybackInfo,
        plan: &yanami_core::PlaybackPlan,
    ) -> Result<Option<String>, String> {
        let source = playback
            .media_sources
            .iter()
            .find(|source| source.id == plan.media_source_id)
            .ok_or("the selected Emby media source disappeared")?;
        let server_id = client.profile().server_id.as_deref().unwrap_or_default();
        let now = Utc::now().timestamp();
        let matched = self
            .services
            .storage()
            .find_match(server_id, &plan.item_id, &plan.media_source_id)
            .map_err(display_error)?;
        let matched = if let Some(matched) = matched {
            matched
        } else {
            if plan.method != PlaybackMethod::DirectStream {
                return Ok(None);
            }
            let file_size = source.size.ok_or("Emby did not provide the media size")?;
            let duration = source
                .run_time_ticks
                .map(|ticks| ticks / 10_000_000)
                .ok_or("Emby did not provide the media duration")?;
            let http = reqwest::Client::builder()
                .timeout(Duration::from_secs(45))
                .build()
                .map_err(display_error)?;
            let file_hash = self
                .runtime
                .block_on(hash_remote_prefix(
                    &http,
                    plan.url.clone(),
                    &plan.request_headers,
                ))
                .map_err(display_error)?;
            let file_name = source
                .path
                .as_deref()
                .map(|path| media_file_stem(path, item_name))
                .unwrap_or_else(|| item_name.to_owned());
            let candidates = self
                .runtime
                .block_on(self.services.match_dandanplay(&MatchInput {
                    file_name,
                    file_hash,
                    file_size,
                    video_duration_seconds: duration,
                }))
                .map_err(display_error)?;
            if candidates.len() > 1 {
                return Err("multiple danmaku matches require a manual choice".to_owned());
            }
            let Some(candidate) = candidates.into_iter().next() else {
                return Ok(None);
            };
            let display_title = if candidate.episode_title.is_empty() {
                candidate.anime_title
            } else {
                format!("{} · {}", candidate.anime_title, candidate.episode_title)
            };
            let record = DanmakuMatchRecord {
                server_id: server_id.to_owned(),
                item_id: plan.item_id.clone(),
                media_source_id: plan.media_source_id.clone(),
                episode_id: candidate.episode_id,
                display_title,
                time_offset_seconds: candidate.shift,
                updated_at: now,
            };
            self.services
                .storage()
                .put_match(&record)
                .map_err(display_error)?;
            record
        };

        let comments = self
            .runtime
            .block_on(self.services.comments(matched.episode_id, now, false))
            .map_err(display_error)?;
        let directory = self.data_dir.join("cache").join("danmaku");
        fs::create_dir_all(&directory).map_err(display_error)?;
        let final_path = directory.join(format!("{}.ass", matched.episode_id));
        let temporary_path = directory.join(format!("{}.ass.tmp", matched.episode_id));
        let ass = AssGenerator::generate(
            &comments.comments,
            &AssConfig {
                time_offset_seconds: matched.time_offset_seconds,
                ..AssConfig::default()
            },
        );
        fs::write(&temporary_path, ass).map_err(display_error)?;
        if final_path.exists() {
            fs::remove_file(&final_path).map_err(display_error)?;
        }
        fs::rename(&temporary_path, &final_path).map_err(display_error)?;
        Ok(Some(final_path.to_string_lossy().into_owned()))
    }
}

fn display_error(error: impl std::fmt::Display) -> String {
    error.to_string()
}

fn card_title(item: &BaseItem, prefer_series_title: bool) -> String {
    if prefer_series_title && item.item_type.as_deref() == Some("Episode") {
        item.series_name
            .as_deref()
            .filter(|value| !value.trim().is_empty())
            .unwrap_or(&item.name)
            .to_owned()
    } else {
        item.name.clone()
    }
}

fn episode_code(item: &BaseItem) -> String {
    match (item.parent_index_number, item.index_number) {
        (Some(season), Some(episode)) => format!("S{season:02}E{episode:02}"),
        (None, Some(episode)) => format!("E{episode:02}"),
        _ => String::new(),
    }
}

fn card_subtitle(item: &BaseItem, include_episode_name: bool) -> String {
    if item.item_type.as_deref() == Some("Episode") {
        let episode_code = episode_code(item);
        if episode_code.is_empty() {
            return item.name.clone();
        }
        if !include_episode_name || item.name.trim().is_empty() {
            return episode_code;
        }
        return format!("{episode_code} · {}", item.name);
    }
    if matches!(item.item_type.as_deref(), Some("Series" | "Season")) {
        let total = item.recursive_item_count.or(item.child_count);
        let unplayed = item
            .user_data
            .as_ref()
            .and_then(|data| data.unplayed_item_count);
        return match (total, unplayed) {
            (Some(total), Some(unplayed)) if unplayed > 0 => {
                format!("{total} episodes · {unplayed} unplayed")
            }
            (Some(total), _) => format!("{total} episodes"),
            _ => item
                .production_year
                .map_or_else(|| "Series".to_owned(), |year| year.to_string()),
        };
    }
    item.production_year.map_or_else(
        || item.item_type.as_deref().unwrap_or("Video").to_owned(),
        |year| year.to_string(),
    )
}

fn media_card_json(
    item: &BaseItem,
    image_url: Option<String>,
    recent_episode: bool,
    subtitle_source: Option<&BaseItem>,
) -> serde_json::Value {
    let user_data = item.user_data.as_ref();
    let child_count = item.recursive_item_count.or(item.child_count);
    json!({
        "id": item.id,
        "title": card_title(item, recent_episode),
        "subtitle": subtitle_source
            .map(|episode| card_subtitle(episode, true))
            .unwrap_or_else(|| card_subtitle(item, recent_episode)),
        "itemType": item.item_type,
        "seriesId": item.series_id,
        "seasonId": item.season_id,
        "imageUrl": image_url,
        "resumeTicks": user_data.map_or(0, |data| data.playback_position_ticks),
        "played": user_data.is_some_and(|data| data.played),
        "progress": user_data.and_then(|data| data.played_percentage),
        "unplayedCount": user_data.and_then(|data| data.unplayed_item_count),
        "childCount": child_count,
        "hasLatestEpisode": subtitle_source.is_some(),
        "overview": item.overview,
        "dateCreated": item.date_created,
        "releaseDate": item.premiere_date,
        "productionYear": item.production_year,
        "updatedAt": subtitle_source
            .and_then(|episode| episode.date_created.as_deref())
            .or(item.date_created.as_deref()),
    })
}

fn library_view_json(item: &BaseItem, image_url: Option<String>) -> serde_json::Value {
    json!({
        "id": item.id,
        "title": item.name,
        "subtitle": "",
        "collectionType": item.collection_type,
        "imageUrl": image_url,
    })
}

fn is_supported_view(item: &BaseItem) -> bool {
    matches!(
        item.collection_type.as_deref(),
        None | Some("tvshows" | "movies" | "homevideos" | "musicvideos")
    ) && matches!(
        item.item_type.as_deref(),
        Some("CollectionFolder" | "UserView" | "Folder" | "AggregateFolder")
    )
}

fn image_reference(item: &BaseItem, purpose: ImagePurpose) -> Option<(&str, &str, &str)> {
    if matches!(purpose, ImagePurpose::Backdrop) {
        if let Some(tag) = item.backdrop_image_tags.first() {
            return Some((item.id.as_str(), "Backdrop", tag));
        }
        if let (Some(parent_id), Some(parent_tag)) = (
            item.parent_backdrop_item_id.as_deref(),
            item.parent_backdrop_image_tags.first(),
        ) {
            return Some((parent_id, "Backdrop", parent_tag));
        }
    }
    if matches!(purpose, ImagePurpose::EpisodeStill) {
        if let Some(tag) = item.image_tags.get("Primary") {
            return Some((item.id.as_str(), "Primary", tag));
        }
        if let Some(tag) = item.image_tags.get("Thumb") {
            return Some((item.id.as_str(), "Thumb", tag));
        }
        if let (Some(parent_id), Some(parent_tag)) = (
            item.parent_thumb_item_id.as_deref(),
            item.parent_thumb_image_tag.as_deref(),
        ) {
            return Some((parent_id, "Thumb", parent_tag));
        }
    }
    if matches!(item.item_type.as_deref(), Some("Season" | "Episode")) {
        if let Some(tag) = item.image_tags.get("Primary") {
            return Some((item.id.as_str(), "Primary", tag));
        }
        if let (Some(series_id), Some(series_tag)) = (
            item.series_id.as_deref(),
            item.series_primary_image_tag.as_deref(),
        ) {
            return Some((series_id, "Primary", series_tag));
        }
        if let (Some(parent_id), Some(parent_tag)) = (
            item.parent_thumb_item_id.as_deref(),
            item.parent_thumb_image_tag.as_deref(),
        ) {
            return Some((parent_id, "Thumb", parent_tag));
        }
    }
    item.image_tags
        .get("Primary")
        .or(item.primary_image_tag.as_ref())
        .map(|tag| (item.id.as_str(), "Primary", tag.as_str()))
}

fn is_playable_item(item: &BaseItem) -> bool {
    item.location_type.as_deref() != Some("Virtual")
}

fn select_episode(items: Vec<BaseItem>) -> Option<BaseItem> {
    let mut playable: Vec<_> = items.into_iter().filter(is_playable_item).collect();
    let preferred = playable.iter().position(|item| {
        item.user_data
            .as_ref()
            .is_some_and(|data| !data.played && data.playback_position_ticks > 0)
    });
    if let Some(index) = preferred {
        return Some(playable.remove(index));
    }
    let unplayed = playable
        .iter()
        .position(|item| item.user_data.as_ref().is_none_or(|data| !data.played));
    if let Some(index) = unplayed {
        return Some(playable.remove(index));
    }
    playable.into_iter().next()
}

fn neighboring_episodes(
    items: Vec<BaseItem>,
    current_id: &str,
) -> (Option<BaseItem>, Option<BaseItem>) {
    let playable: Vec<_> = items.into_iter().filter(is_playable_item).collect();
    let Some(index) = playable.iter().position(|item| item.id == current_id) else {
        return (None, None);
    };
    (
        index
            .checked_sub(1)
            .and_then(|value| playable.get(value).cloned()),
        playable.get(index + 1).cloned(),
    )
}

fn playback_navigation_item(item: &BaseItem) -> serde_json::Value {
    json!({
        "id": item.id,
        "title": playback_title(item),
    })
}

fn playback_title(item: &BaseItem) -> String {
    if item.item_type.as_deref() != Some("Episode") {
        return item.name.clone();
    }
    let mut parts = Vec::new();
    if let Some(series_name) = item.series_name.as_deref() {
        parts.push(series_name.to_owned());
    }
    let code = episode_code(item);
    if !code.is_empty() {
        parts.push(code);
    }
    if !item.name.trim().is_empty() {
        parts.push(item.name.clone());
    }
    parts.join(" · ")
}

fn intro_range_from_chapters(chapters: &[ChapterInfo]) -> Option<(u64, u64)> {
    let intro_start = chapters
        .iter()
        .find(|chapter| {
            chapter
                .marker_type
                .as_deref()
                .is_some_and(|value| value.eq_ignore_ascii_case("IntroStart"))
        })
        .map(|chapter| chapter.start_position_ticks)?;
    let intro_end = chapters
        .iter()
        .find(|chapter| {
            chapter
                .marker_type
                .as_deref()
                .is_some_and(|value| value.eq_ignore_ascii_case("IntroEnd"))
        })
        .map(|chapter| chapter.start_position_ticks)?;
    (intro_end > intro_start).then_some((intro_start, intro_end))
}

fn safe_cache_component(value: &str) -> String {
    value
        .chars()
        .filter(|character| character.is_ascii_alphanumeric() || matches!(character, '-' | '_'))
        .collect()
}

fn media_file_stem(path: &str, fallback: &str) -> String {
    let leaf = path
        .rsplit(['/', '\\'])
        .find(|segment| !segment.is_empty())
        .unwrap_or(fallback);
    leaf.rsplit_once('.')
        .map_or(leaf, |(stem, _)| stem)
        .to_owned()
}

unsafe fn required_string(pointer: *const c_char, name: &str) -> Result<String, String> {
    if pointer.is_null() {
        return Err(format!("{name} is required"));
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| format!("{name} must be UTF-8"))
}

unsafe fn set_output(output: *mut *mut c_char, value: String) {
    if output.is_null() {
        return;
    }
    let sanitized = value.replace('\0', " ");
    let value = CString::new(sanitized).expect("NUL bytes were removed");
    unsafe { *output = value.into_raw() };
}

unsafe fn clear_output(output: *mut *mut c_char) {
    if !output.is_null() {
        unsafe { *output = ptr::null_mut() };
    }
}

unsafe fn backend_ref<'a>(backend: *mut YanamiBackend) -> Result<&'a YanamiBackend, String> {
    unsafe { backend.as_ref() }.ok_or_else(|| "backend is not initialized".to_owned())
}

#[unsafe(no_mangle)]
/// Creates a backend instance.
///
/// # Safety
/// `data_dir` must point to a valid NUL-terminated UTF-8 string. When non-null,
/// `error` must point to writable storage for one pointer.
pub unsafe extern "C" fn yanami_backend_new(
    data_dir: *const c_char,
    error: *mut *mut c_char,
) -> *mut YanamiBackend {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let data_dir = unsafe { required_string(data_dir, "data directory") }?;
        YanamiBackend::open(Path::new(&data_dir))
    }));
    match result {
        Ok(Ok(backend)) => Box::into_raw(Box::new(backend)),
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            ptr::null_mut()
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// Releases a backend instance.
///
/// # Safety
/// `backend` must be null or a pointer returned by `yanami_backend_new` that has
/// not already been freed and is not being used by another thread.
pub unsafe extern "C" fn yanami_backend_free(backend: *mut YanamiBackend) {
    if !backend.is_null() {
        drop(unsafe { Box::from_raw(backend) });
    }
}

#[unsafe(no_mangle)]
/// Releases a string returned by this library.
///
/// # Safety
/// `value` must be null or an unfreed pointer returned through an output
/// parameter by this library.
pub unsafe extern "C" fn yanami_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(unsafe { CString::from_raw(value) });
    }
}

#[unsafe(no_mangle)]
/// Returns whether `DanDanPlay` credentials are configured.
///
/// # Safety
/// `backend` must be a live backend pointer. When non-null, `error` must point
/// to writable storage for one pointer.
pub unsafe extern "C" fn yanami_backend_dandanplay_configured(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    match catch_unwind(AssertUnwindSafe(|| {
        unsafe { backend_ref(backend) }?
            .services
            .dandanplay_configured()
            .map_err(display_error)
    })) {
        Ok(Ok(true)) => 1,
        Ok(Ok(false)) => 0,
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            -1
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            -1
        }
    }
}

#[unsafe(no_mangle)]
/// Returns the active Dandanplay credential source: 0 none, 1 bundled, 2 BYOK.
///
/// # Safety
/// `backend` must be a live backend pointer. When non-null, `error` must point
/// to writable storage for one pointer.
pub unsafe extern "C" fn yanami_backend_dandanplay_credential_source(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    match catch_unwind(AssertUnwindSafe(|| {
        unsafe { backend_ref(backend) }?
            .services
            .dandanplay_credential_source()
            .map_err(display_error)
    })) {
        Ok(Ok(DandanCredentialSource::None)) => 0,
        Ok(Ok(DandanCredentialSource::Bundled)) => 1,
        Ok(Ok(DandanCredentialSource::UserProvided)) => 2,
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            -1
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            -1
        }
    }
}

#[unsafe(no_mangle)]
/// Returns whether a persisted Emby session is available.
///
/// # Safety
/// `backend` must be a live backend pointer. When non-null, `error` must point
/// to writable storage for one pointer.
pub unsafe extern "C" fn yanami_backend_emby_connected(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    match catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        backend
            .active_session
            .lock()
            .map(|session| session.is_some())
            .map_err(|_| "session lock is poisoned".to_owned())
    })) {
        Ok(Ok(true)) => 1,
        Ok(Ok(false)) => 0,
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            -1
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            -1
        }
    }
}

#[unsafe(no_mangle)]
/// Validates and stores user-provided `DanDanPlay` credentials.
///
/// # Safety
/// `backend` must be live, both string pointers must be valid NUL-terminated
/// UTF-8 strings, and `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_configure_dandanplay(
    backend: *mut YanamiBackend,
    app_id: *const c_char,
    app_secret: *const c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        let app_id = unsafe { required_string(app_id, "AppId") }?;
        let mut app_secret = unsafe { required_string(app_secret, "AppSecret") }?;
        let result = backend.runtime.block_on(
            backend
                .services
                .configure_dandanplay(&app_id, SecretString::from(app_secret.clone())),
        );
        app_secret.zeroize();
        result.map_err(display_error)
    }));
    finish_status(result, error)
}

#[unsafe(no_mangle)]
/// Removes the stored `DanDanPlay` credentials.
///
/// # Safety
/// `backend` must be live and `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_clear_dandanplay(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        unsafe { backend_ref(backend) }?
            .services
            .clear_dandanplay()
            .map_err(display_error)
    }));
    finish_status(result, error)
}

#[unsafe(no_mangle)]
/// Authenticates against an Emby server and stores the returned token.
///
/// # Safety
/// `backend` must be live, input pointers must be valid NUL-terminated UTF-8
/// strings, and `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_login_emby(
    backend: *mut YanamiBackend,
    server_name: *const c_char,
    server_url: *const c_char,
    username: *const c_char,
    password: *const c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        let server_name = unsafe { required_string(server_name, "server name") }?;
        let server_url = unsafe { required_string(server_url, "server URL") }?;
        let username = unsafe { required_string(username, "username") }?;
        let mut password = unsafe { required_string(password, "password") }?;
        let result = backend.login_emby(&server_name, &server_url, &username, &password);
        password.zeroize();
        result
    }));
    finish_status(result, error)
}

#[unsafe(no_mangle)]
/// Deletes the persisted Emby token and clears the active session.
///
/// # Safety
/// `backend` must be live and `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_logout_emby(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        unsafe { backend_ref(backend) }?.logout_emby()
    }));
    finish_status(result, error)
}

#[unsafe(no_mangle)]
/// Loads the active Emby user's library as JSON.
///
/// # Safety
/// `backend` must be live. Output pointers must be null or point to writable
/// storage and returned strings must be freed with `yanami_string_free`.
pub unsafe extern "C" fn yanami_backend_library_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { string_operation(backend, output, error, YanamiBackend::library_json) }
}

#[unsafe(no_mangle)]
/// Refreshes the lightweight home activity feeds as JSON.
///
/// # Safety
/// `backend` must be live. Output pointers must be null or point to writable
/// storage and returned strings must be freed with `yanami_string_free`.
pub unsafe extern "C" fn yanami_backend_activity_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { string_operation(backend, output, error, YanamiBackend::activity_json) }
}

#[unsafe(no_mangle)]
/// Loads the seasons of a series or the episodes of a season as JSON.
///
/// # Safety
/// `backend` must be live, `parent_id` must be a valid NUL-terminated UTF-8
/// string, and output pointers must be null or writable.
pub unsafe extern "C" fn yanami_backend_collection_json(
    backend: *mut YanamiBackend,
    parent_id: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        clear_output(output);
        clear_output(error);
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        let parent_id = unsafe { required_string(parent_id, "parent item ID") }?;
        backend.collection_json(&parent_id)
    }));
    finish_string_result(result, output, error)
}

#[unsafe(no_mangle)]
/// Creates a playback plan and optional local danmaku track as JSON.
///
/// # Safety
/// `backend` must be live, `item_id` must be a valid NUL-terminated UTF-8
/// string, and output pointers must be null or writable.
pub unsafe extern "C" fn yanami_backend_playback_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        clear_output(output);
        clear_output(error);
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        let item_id = unsafe { required_string(item_id, "item ID") }?;
        backend.playback_json(&item_id)
    }));
    finish_string_result(result, output, error)
}

#[unsafe(no_mangle)]
/// Queues an Emby playback lifecycle report without blocking the Qt UI thread.
///
/// # Safety
/// `backend` must be live, `event` must be a valid NUL-terminated UTF-8 string,
/// and `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_report_playback(
    backend: *mut YanamiBackend,
    event: *const c_char,
    position_ticks: u64,
    is_paused: i32,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        let kind = match unsafe { required_string(event, "playback event") }?.as_str() {
            "started" => PlaybackReportKind::Started,
            "progress" => PlaybackReportKind::Progress,
            "stopped" => PlaybackReportKind::Stopped,
            _ => return Err("unsupported playback event".to_owned()),
        };
        backend.report_playback(kind, position_ticks, is_paused != 0)
    }));
    finish_status(result, error)
}

fn finish_string_result(
    result: Result<Result<String, String>, Box<dyn std::any::Any + Send>>,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    match result {
        Ok(Ok(value)) => {
            unsafe { set_output(output, value) };
            0
        }
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            1
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            2
        }
    }
}

unsafe fn string_operation(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
    operation: fn(&YanamiBackend) -> Result<String, String>,
) -> i32 {
    unsafe {
        clear_output(output);
        clear_output(error);
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        operation(unsafe { backend_ref(backend) }?)
    }));
    match result {
        Ok(Ok(value)) => {
            unsafe { set_output(output, value) };
            0
        }
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            1
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            2
        }
    }
}

fn finish_status(
    result: Result<Result<(), String>, Box<dyn std::any::Any + Send>>,
    error: *mut *mut c_char,
) -> i32 {
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            unsafe { set_output(error, message) };
            1
        }
        Err(_) => {
            unsafe { set_output(error, "Rust backend panicked".to_owned()) };
            2
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        card_subtitle, card_title, intro_range_from_chapters, media_file_stem,
        neighboring_episodes, safe_cache_component, select_episode,
    };
    use yanami_emby::{BaseItem, ChapterInfo};

    #[test]
    fn reads_intro_range_from_emby_chapter_markers() {
        let chapters = vec![
            ChapterInfo {
                start_position_ticks: 120_000_000,
                marker_type: Some("IntroStart".to_owned()),
                ..ChapterInfo::default()
            },
            ChapterInfo {
                start_position_ticks: 960_000_000,
                marker_type: Some("IntroEnd".to_owned()),
                ..ChapterInfo::default()
            },
        ];

        assert_eq!(
            intro_range_from_chapters(&chapters),
            Some((120_000_000, 960_000_000))
        );
    }

    #[test]
    fn ignores_incomplete_or_reversed_intro_markers() {
        let reversed = vec![
            ChapterInfo {
                start_position_ticks: 960_000_000,
                marker_type: Some("IntroStart".to_owned()),
                ..ChapterInfo::default()
            },
            ChapterInfo {
                start_position_ticks: 120_000_000,
                marker_type: Some("IntroEnd".to_owned()),
                ..ChapterInfo::default()
            },
        ];

        assert_eq!(intro_range_from_chapters(&reversed), None);
        assert_eq!(intro_range_from_chapters(&reversed[..1]), None);
    }

    #[test]
    fn extracts_cross_platform_media_stems() {
        assert_eq!(
            media_file_stem(r"D:\Anime\Episode 01.mkv", "fallback"),
            "Episode 01"
        );
        assert_eq!(
            media_file_stem("/media/Anime/Episode 02.mp4", "fallback"),
            "Episode 02"
        );
        assert_eq!(media_file_stem("", "fallback"), "fallback");
    }

    #[test]
    fn episode_cards_put_the_series_name_first() {
        let item = BaseItem {
            id: "episode-id".to_owned(),
            name: "第 1 集".to_owned(),
            item_type: Some("Episode".to_owned()),
            series_name: Some("地狱乐".to_owned()),
            parent_index_number: Some(1),
            index_number: Some(1),
            ..BaseItem::default()
        };

        assert_eq!(card_title(&item, true), "地狱乐");
        assert_eq!(card_subtitle(&item, true), "S01E01 · 第 1 集");
    }

    #[test]
    fn continue_selection_prefers_an_episode_in_progress() {
        let completed = BaseItem {
            id: "done".to_owned(),
            name: "1".to_owned(),
            user_data: Some(yanami_emby::UserItemData {
                played: true,
                ..yanami_emby::UserItemData::default()
            }),
            ..BaseItem::default()
        };
        let in_progress = BaseItem {
            id: "current".to_owned(),
            name: "2".to_owned(),
            user_data: Some(yanami_emby::UserItemData {
                playback_position_ticks: 10,
                ..yanami_emby::UserItemData::default()
            }),
            ..BaseItem::default()
        };
        let next = BaseItem {
            id: "next".to_owned(),
            name: "3".to_owned(),
            ..BaseItem::default()
        };

        assert_eq!(
            select_episode(vec![completed, in_progress, next])
                .expect("an episode should be selected")
                .id,
            "current"
        );
    }

    #[test]
    fn finds_episode_neighbors_in_emby_order() {
        let episodes = ["one", "two", "three"]
            .into_iter()
            .map(|id| BaseItem {
                id: id.to_owned(),
                item_type: Some("Episode".to_owned()),
                ..BaseItem::default()
            })
            .collect();

        let (previous, next) = neighboring_episodes(episodes, "two");
        assert_eq!(previous.map(|item| item.id), Some("one".to_owned()));
        assert_eq!(next.map(|item| item.id), Some("three".to_owned()));
    }

    #[test]
    fn poster_cache_names_drop_path_characters() {
        assert_eq!(safe_cache_component("abc/../tag:42"), "abctag42");
    }
}
