use std::{
    collections::{BTreeMap, HashSet},
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};

#[cfg(test)]
use std::fs;

use chrono::{DateTime, SecondsFormat, Utc};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use uuid::Uuid;
use yanami_emby::{BaseItem, EmbyClient, ItemQuery, UserItemData};
use yanami_storage::{
    CatalogItem, CatalogPendingChanges, CatalogScope, CatalogSearchHit, CatalogSearchPage,
    CatalogSyncRun, CatalogUserState, MediaCatalog, MediaCatalogError, SyncState, SyncStatus,
};

use crate::{
    Application, ApplicationError, ApplicationErrorCode, BackgroundTaskScope, PlaybackContext,
    PlaybackContextKind, RegisteredTask,
    catalog::{CatalogEntity, CatalogQueries, decode_catalog_outcome, normalized_query_payload},
    presentation::{ImagePurpose, media_card_json},
};

const SEARCH_GROUP_LIMIT: usize = 50;
const SEARCH_RESULT_LIMIT: usize = SEARCH_GROUP_LIMIT * 2;
const CATALOG_PAGE_SIZE: u32 = 500;
const CATALOG_MEMBERSHIP_PAGE_SIZE: u32 = 2_000;
const CATALOG_INCREMENTAL_ID_BATCH_SIZE: usize = 200;
const CATALOG_PENDING_BATCH_SIZE: usize = 500;
const CATALOG_PAGE_YIELD: Duration = Duration::from_millis(35);
const CATALOG_REFRESH_AFTER_MS: i64 = 7 * 24 * 60 * 60 * 1_000;
const CATALOG_EMPTY_RETRY_AFTER_MS: i64 = 5 * 1_000;
const CATALOG_EMPTY_SECOND_RETRY_AFTER_MS: i64 = 15 * 1_000;
const CATALOG_RETRY_AFTER_MS: i64 = 60 * 1_000;
const CATALOG_DELTA_INTERVAL_MS: i64 = 15 * 60 * 1_000;
const CATALOG_MEMBERSHIP_INTERVAL_MS: i64 = 6 * 60 * 60 * 1_000;
const CATALOG_DELTA_OVERLAP_MS: i64 = 5 * 60 * 1_000;
const CATALOG_EVENT_DEBOUNCE: Duration = Duration::from_millis(250);
const CATALOG_INCREMENTAL_FAILURES_BEFORE_FULL: u32 = 3;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CatalogSearchOutcome {
    entities: BTreeMap<String, CatalogEntity>,
    queries: CatalogQueries,
    search_status: CatalogSearchStatus,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
#[allow(clippy::struct_excessive_bools)]
pub struct CatalogSearchStatus {
    cached_count: u64,
    total_count: Option<u64>,
    total_matches: u64,
    has_more: bool,
    complete: bool,
    syncing: bool,
    index_error: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    index_error_detail: Option<String>,
    /// Opaque token: consumers compare it for equality and never interpret it.
    catalog_revision: String,
    query: String,
    limit: usize,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CatalogSearchImageHydrationRequest {
    query: String,
    items: Vec<CatalogSearchImageHydrationItem>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct CatalogSearchImageHydrationItem {
    item_id: String,
    item_type: String,
    image_tag: String,
}

#[derive(Clone)]
struct CatalogSession {
    scope: CatalogScope,
    persistent_key: String,
    task_key: String,
    device_id: Uuid,
}

impl Application {
    /// Searches only the locally committed catalog projection. An empty query
    /// deliberately returns no rows while preserving sync status for polling.
    pub fn catalog_search(&self, query: &str) -> Result<CatalogSearchOutcome, ApplicationError> {
        let query = query.trim();
        if query.chars().count() > 512 {
            return Err(ApplicationError::invalid(
                "catalog search query must not exceed 512 characters",
            ));
        }

        let session = self.active_catalog_session()?;
        let catalog = self.media_catalog_for(&session)?;
        let start_error = self
            .ensure_media_catalog_sync_for(&session, &catalog)
            .err()
            .map(|error| {
                tracing::warn!(error = %error, "media catalog synchronization could not start");
                bounded_error(&error.to_string())
            });
        let page = catalog
            .search_with_limit(query, SEARCH_GROUP_LIMIT)
            .map_err(|error| catalog_error(&error))?;
        let items = page
            .items
            .iter()
            .map(search_image_base_item)
            .collect::<Vec<_>>();
        let image_urls = self
            .planned_image_urls(&session.scope.server_local_id, &items, ImagePurpose::Poster)
            .map_err(ApplicationError::internal)?;
        self.verify_catalog_session(&session)?;
        catalog_search_outcome(&page, query, start_error.as_deref(), &image_urls)
    }

    /// Schedules cache misses for the current local Top-K result. This second
    /// phase is intentionally separate from `catalog_search`: the desktop
    /// invokes it only after a published query has remained stable, on a
    /// dedicated bounded lane.
    pub fn hydrate_catalog_search_images(
        &self,
        request: &CatalogSearchImageHydrationRequest,
    ) -> Result<(), ApplicationError> {
        let query = request.query.trim();
        if query.chars().count() > 512 {
            return Err(ApplicationError::invalid(
                "catalog search query must not exceed 512 characters",
            ));
        }
        if query.is_empty() {
            return Ok(());
        }

        let items = search_hydration_items(request)?;
        let session = self.active_catalog_session()?;
        let (client, client_device_id) = self.active_client_snapshot()?;
        if client_device_id != session.device_id {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the Emby session changed before search image hydration",
            ));
        }
        self.verify_catalog_session(&session)?;
        self.cache_images(&client, &items, ImagePurpose::Poster)
            .map_err(ApplicationError::internal)?;
        self.verify_catalog_session(&session)
    }

    /// Best-effort entry point used by the ordinary library load. The library
    /// response remains server-owned and is never blocked on catalog indexing.
    pub(crate) fn ensure_media_catalog_sync(&self) -> Result<(), ApplicationError> {
        let session = self.active_catalog_session()?;
        let catalog = self.media_catalog_for(&session)?;
        self.ensure_media_catalog_sync_for(&session, &catalog)
    }

    /// Best-effort cache repair after a server-side media deletion succeeds.
    /// Hierarchical descendants are suppressed before the next remote sweep.
    pub(crate) fn evict_media_catalog_item(&self, item_id: &str) -> Result<(), ApplicationError> {
        let session = self.active_catalog_session()?;
        let catalog = self.media_catalog_for(&session)?;
        match catalog.remove_item_incrementally(item_id, now_ms()) {
            Ok(_) | Err(MediaCatalogError::SyncAlreadyRunning(_)) => {}
            Err(error) => return Err(catalog_error(&error)),
        }
        // Keep a durable remove disposition even after the immediate
        // tombstone. An older in-flight fetch may otherwise reinsert this ID;
        // pending_revision ensures the newer removal is applied afterwards.
        catalog
            .enqueue_library_changes(&[], &[item_id.to_owned()], false, now_ms())
            .map_err(|error| catalog_error(&error))?;
        self.catalog_wake.notify_one();
        self.ensure_media_catalog_sync_for(&session, &catalog)
    }

    fn active_catalog_session(&self) -> Result<CatalogSession, ApplicationError> {
        let active = self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))?;
        let active = active
            .as_ref()
            .ok_or_else(ApplicationError::not_connected)?;
        let scope = CatalogScope::new(
            active.session.server_local_id.to_string(),
            active.session.server_id.clone(),
            active.session.user_id.clone(),
        );
        let persistent_key = catalog_scope_fingerprint(&scope);
        Ok(CatalogSession {
            task_key: format!("{persistent_key}:{}", active.session.device_id),
            persistent_key,
            scope,
            device_id: active.session.device_id,
        })
    }

    pub(crate) fn active_catalog_notification_context(
        &self,
    ) -> Result<(String, String), ApplicationError> {
        let session = self.active_catalog_session()?;
        Ok((session.task_key, session.scope.user_id))
    }

    fn verify_catalog_session(&self, expected: &CatalogSession) -> Result<(), ApplicationError> {
        let current = self.active_catalog_session()?;
        if current.device_id == expected.device_id && current.scope == expected.scope {
            Ok(())
        } else {
            Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the Emby session changed during catalog search",
            ))
        }
    }

    fn media_catalog_for(
        &self,
        session: &CatalogSession,
    ) -> Result<Arc<MediaCatalog>, ApplicationError> {
        let mut cached = self
            .media_catalog
            .lock()
            .map_err(|_| ApplicationError::internal("media catalog lock is poisoned"))?;
        if let Some((scope, catalog)) = cached.as_ref() {
            if scope == &session.scope {
                return Ok(Arc::clone(catalog));
            }
        }
        let path = media_catalog_path(&self.data_dir, &session.persistent_key);
        let catalog = Arc::new(open_media_catalog(&path, &session.scope)?);
        *cached = Some((session.scope.clone(), Arc::clone(&catalog)));
        Ok(catalog)
    }

    fn ensure_media_catalog_sync_for(
        &self,
        session: &CatalogSession,
        catalog: &Arc<MediaCatalog>,
    ) -> Result<(), ApplicationError> {
        let (client, client_device_id) = self.active_client_snapshot()?;
        if client_device_id != session.device_id {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the Emby session changed before catalog synchronization",
            ));
        }
        self.verify_catalog_session(session)?;

        let mut tasks = self
            .background_tasks
            .lock()
            .map_err(|_| ApplicationError::internal("background task registry is poisoned"))?;
        tasks.retain(|task| !task.handle.is_finished());
        if tasks.iter().any(|task| {
            matches!(
                &task.scope,
                BackgroundTaskScope::Catalog { session_key }
                    if session_key == &session.task_key
            )
        }) {
            return Ok(());
        }
        if tasks
            .iter()
            .any(|task| matches!(task.scope, BackgroundTaskScope::Catalog { .. }))
        {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "a catalog supervisor for another session is still draining",
            ));
        }

        if *self.cancellation.borrow() {
            return Err(ApplicationError::cancelled());
        }
        let task_catalog = Arc::clone(catalog);
        let task_session_key = session.task_key.clone();
        let catalog_notifications = Arc::clone(&self.catalog_notifications);
        let catalog_wake = Arc::clone(&self.catalog_wake);
        let mut cancellation = self.cancellation.subscribe();
        let handle = self.background.spawn(async move {
            catalog_sync_supervisor(
                &client,
                task_catalog,
                &task_session_key,
                catalog_notifications,
                catalog_wake,
                &mut cancellation,
            )
            .await;
        });
        tasks.push(RegisteredTask {
            scope: BackgroundTaskScope::Catalog {
                session_key: session.task_key.clone(),
            },
            handle,
        });
        Ok(())
    }

    /// Starts the long-lived catalog owner without opening or rebuilding the
    /// catalog on the login commit path. The first potentially expensive open
    /// runs inside the registered background task.
    pub(crate) fn ensure_media_catalog_supervisor_background(
        &self,
    ) -> Result<(), ApplicationError> {
        let session = self.active_catalog_session()?;
        let (client, client_device_id) = self.active_client_snapshot()?;
        if client_device_id != session.device_id {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the Emby session changed before catalog synchronization",
            ));
        }
        let mut tasks = self
            .background_tasks
            .lock()
            .map_err(|_| ApplicationError::internal("background task registry is poisoned"))?;
        tasks.retain(|task| !task.handle.is_finished());
        if tasks.iter().any(|task| {
            matches!(
                &task.scope,
                BackgroundTaskScope::Catalog { session_key }
                    if session_key == &session.task_key
            )
        }) {
            return Ok(());
        }
        if *self.cancellation.borrow() {
            return Err(ApplicationError::cancelled());
        }

        let catalog_cache = Arc::clone(&self.media_catalog);
        let catalog_notifications = Arc::clone(&self.catalog_notifications);
        let catalog_wake = Arc::clone(&self.catalog_wake);
        let data_dir = self.data_dir.clone();
        let task_session = session.clone();
        let task_session_key = session.task_key.clone();
        let mut cancellation = self.cancellation.subscribe();
        let handle = self.background.spawn(async move {
            let catalog = {
                let Ok(mut cached) = catalog_cache.lock() else {
                    tracing::warn!("media catalog cache lock is poisoned");
                    return;
                };
                if let Some((scope, catalog)) = cached.as_ref() {
                    if scope == &task_session.scope {
                        Arc::clone(catalog)
                    } else {
                        tracing::warn!("a media catalog for another session is still cached");
                        return;
                    }
                } else {
                    let path = media_catalog_path(&data_dir, &task_session.persistent_key);
                    match open_media_catalog(&path, &task_session.scope) {
                        Ok(catalog) => {
                            let catalog = Arc::new(catalog);
                            *cached = Some((task_session.scope.clone(), Arc::clone(&catalog)));
                            catalog
                        }
                        Err(error) => {
                            tracing::warn!(error = %error, "media catalog background open failed");
                            return;
                        }
                    }
                }
            };
            catalog_sync_supervisor(
                &client,
                catalog,
                &task_session_key,
                catalog_notifications,
                catalog_wake,
                &mut cancellation,
            )
            .await;
        });
        tasks.push(RegisteredTask {
            scope: BackgroundTaskScope::Catalog {
                session_key: session.task_key,
            },
            handle,
        });
        Ok(())
    }
}

fn search_hydration_items(
    request: &CatalogSearchImageHydrationRequest,
) -> Result<Vec<BaseItem>, ApplicationError> {
    if request.items.len() > SEARCH_RESULT_LIMIT {
        return Err(ApplicationError::invalid(
            "catalog search image hydration exceeds the bounded result limit",
        ));
    }
    request
        .items
        .iter()
        .map(|item| {
            let item_id = item.item_id.trim();
            let item_type = item.item_type.trim();
            let image_tag = item.image_tag.trim();
            if item_id.is_empty() || item_id.chars().count() > 512 {
                return Err(ApplicationError::invalid(
                    "catalog search hydration item ID is invalid",
                ));
            }
            if !matches!(item_type, "Movie" | "Series" | "Season" | "Episode") {
                return Err(ApplicationError::invalid(
                    "catalog search hydration item type is invalid",
                ));
            }
            if image_tag.is_empty() || image_tag.chars().count() > 1024 {
                return Err(ApplicationError::invalid(
                    "catalog search hydration image tag is invalid",
                ));
            }
            Ok(BaseItem {
                id: item_id.to_owned(),
                item_type: Some(item_type.to_owned()),
                image_tags: BTreeMap::from([("Primary".to_owned(), image_tag.to_owned())]),
                primary_image_tag: Some(image_tag.to_owned()),
                ..BaseItem::default()
            })
        })
        .collect()
}

fn catalog_retry_after_ms(status: &SyncStatus) -> i64 {
    if status.last_completed_run_id.is_none() && status.cached_count == 0 {
        CATALOG_EMPTY_RETRY_AFTER_MS
    } else {
        CATALOG_RETRY_AFTER_MS
    }
}

fn catalog_sync_due(status: &SyncStatus, now: i64) -> bool {
    match status.state {
        SyncState::Idle => true,
        SyncState::Running => false,
        SyncState::Complete => {
            status.dirty
                || status.last_completed_at_ms.is_none_or(|completed| {
                    now.saturating_sub(completed) >= CATALOG_REFRESH_AFTER_MS
                })
        }
        SyncState::Failed => status
            .updated_at_ms
            .is_none_or(|updated| now.saturating_sub(updated) >= catalog_retry_after_ms(status)),
    }
}

fn catalog_sync_wait(status: &SyncStatus, now: i64) -> Duration {
    let remaining_ms = match status.state {
        SyncState::Idle | SyncState::Running => 0,
        SyncState::Complete if status.dirty => 0,
        SyncState::Complete => status.last_completed_at_ms.map_or(0, |completed| {
            CATALOG_REFRESH_AFTER_MS.saturating_sub(now.saturating_sub(completed))
        }),
        SyncState::Failed => status.updated_at_ms.map_or(0, |updated| {
            catalog_retry_after_ms(status).saturating_sub(now.saturating_sub(updated))
        }),
    };
    Duration::from_millis(u64::try_from(remaining_ms).unwrap_or_default())
}

#[allow(clippy::too_many_lines)]
async fn catalog_sync_supervisor(
    client: &EmbyClient,
    catalog: Arc<MediaCatalog>,
    session_key: &str,
    catalog_notifications: Arc<std::sync::Mutex<crate::CatalogNotificationBuffer>>,
    catalog_wake: Arc<tokio::sync::Notify>,
    cancellation: &mut tokio::sync::watch::Receiver<bool>,
) {
    let mut empty_catalog_failure_count = 0_u32;
    loop {
        if *cancellation.borrow() {
            return;
        }
        if let Err(error) =
            persist_catalog_notifications(&catalog, session_key, &catalog_notifications)
        {
            tracing::warn!(error = %error, "media catalog notifications could not be persisted");
            if wait_for_catalog_work(
                cancellation,
                &catalog_wake,
                Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
            )
            .await
            {
                return;
            }
            continue;
        }
        let mut status = match catalog.sync_status() {
            Ok(status) => status,
            Err(error) => {
                tracing::warn!(error = %error, "media catalog supervisor could not read status");
                if wait_for_catalog_work(
                    cancellation,
                    &catalog_wake,
                    Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
                )
                .await
                {
                    return;
                }
                continue;
            }
        };
        if status.state == SyncState::Running {
            match catalog.recover_interrupted_sync(
                "catalog synchronization was interrupted before completion",
                now_ms(),
            ) {
                Ok(_) => {}
                Err(error) => {
                    tracing::warn!(error = %error, "media catalog interrupted run could not be recovered");
                    if wait_for_catalog_work(
                        cancellation,
                        &catalog_wake,
                        Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
                    )
                    .await
                    {
                        return;
                    }
                    continue;
                }
            }
            status = match catalog.sync_status() {
                Ok(status) => status,
                Err(error) => {
                    tracing::warn!(error = %error, "media catalog recovered status could not be read");
                    continue;
                }
            };
        }

        let now = now_ms();
        if catalog_sync_due(&status, now) {
            let empty_bootstrap =
                status.last_completed_run_id.is_none() && status.cached_count == 0;
            match run_catalog_sync(client, &catalog, &status).await {
                Ok(()) => empty_catalog_failure_count = 0,
                Err(error) => {
                    tracing::warn!(error = %error, "media catalog synchronization failed");
                    if empty_bootstrap {
                        empty_catalog_failure_count = empty_catalog_failure_count.saturating_add(1);
                        if wait_for_catalog_retry(
                            cancellation,
                            empty_catalog_retry_delay(empty_catalog_failure_count),
                        )
                        .await
                        {
                            return;
                        }
                    } else {
                        empty_catalog_failure_count = 0;
                    }
                }
            }
            continue;
        }

        if status.state != SyncState::Complete {
            if wait_for_catalog_work(cancellation, &catalog_wake, catalog_sync_wait(&status, now))
                .await
            {
                return;
            }
            continue;
        }

        let pending = match catalog.pending_changes(CATALOG_PENDING_BATCH_SIZE) {
            Ok(pending) => pending,
            Err(error) => {
                tracing::warn!(error = %error, "media catalog pending changes could not be read");
                if wait_for_catalog_work(
                    cancellation,
                    &catalog_wake,
                    Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
                )
                .await
                {
                    return;
                }
                continue;
            }
        };

        if pending.failure_count > 0
            && pending
                .last_failure_at_ms
                .is_some_and(|failed_at| now.saturating_sub(failed_at) < CATALOG_RETRY_AFTER_MS)
        {
            let delay = pending
                .last_failure_at_ms
                .map_or(Duration::ZERO, |failed_at| {
                    Duration::from_millis(
                        u64::try_from(
                            CATALOG_RETRY_AFTER_MS.saturating_sub(now.saturating_sub(failed_at)),
                        )
                        .unwrap_or_default(),
                    )
                });
            if wait_for_catalog_work(cancellation, &catalog_wake, delay).await {
                return;
            }
            continue;
        }

        if !pending.upsert_ids.is_empty() || !pending.removed_ids.is_empty() {
            if wait_for_catalog_work(cancellation, &catalog_wake, CATALOG_EVENT_DEBOUNCE).await {
                return;
            }
            if let Err(error) =
                persist_catalog_notifications(&catalog, session_key, &catalog_notifications)
            {
                tracing::warn!(error = %error, "media catalog burst tail could not be persisted");
                continue;
            }
            let result = process_catalog_pending(client, &catalog)
                .await
                .and_then(|()| {
                    catalog
                        .record_incremental_success(now_ms())
                        .map_err(|error| error.to_string())
                });
            if let Err(error) = result {
                if record_incremental_failure(&catalog, &error) {
                    continue;
                }
                if wait_for_catalog_work(
                    cancellation,
                    &catalog_wake,
                    Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
                )
                .await
                {
                    return;
                }
            }
            continue;
        }

        let membership_due = pending.membership_required
            || pending.last_membership_check_ms.is_none_or(|checked_at| {
                now.saturating_sub(checked_at) >= CATALOG_MEMBERSHIP_INTERVAL_MS
            });
        if membership_due {
            let result = match run_catalog_delta(client, &catalog, &pending).await {
                Ok(()) => run_catalog_membership_reconciliation(client, &catalog).await,
                Err(error) => Err(error),
            };
            if let Err(error) = result {
                if record_incremental_failure(&catalog, &error) {
                    continue;
                }
                if wait_for_catalog_work(
                    cancellation,
                    &catalog_wake,
                    Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
                )
                .await
                {
                    return;
                }
            }
            continue;
        }

        let delta_due = pending.catchup_required
            || pending
                .watermark_ms
                .is_none_or(|watermark| now.saturating_sub(watermark) >= CATALOG_DELTA_INTERVAL_MS);
        if delta_due {
            let result = run_catalog_delta(client, &catalog, &pending)
                .await
                .and_then(|()| {
                    catalog
                        .record_incremental_success(now_ms())
                        .map_err(|error| error.to_string())
                });
            if let Err(error) = result {
                if record_incremental_failure(&catalog, &error) {
                    continue;
                }
                if wait_for_catalog_work(
                    cancellation,
                    &catalog_wake,
                    Duration::from_millis(CATALOG_RETRY_AFTER_MS as u64),
                )
                .await
                {
                    return;
                }
            }
            continue;
        }

        let full_sync_wait = catalog_sync_wait(&status, now);
        let delta_wait = pending.watermark_ms.map_or(Duration::ZERO, |watermark| {
            Duration::from_millis(
                u64::try_from(
                    CATALOG_DELTA_INTERVAL_MS.saturating_sub(now.saturating_sub(watermark)),
                )
                .unwrap_or_default(),
            )
        });
        let membership_wait =
            pending
                .last_membership_check_ms
                .map_or(Duration::ZERO, |checked_at| {
                    Duration::from_millis(
                        u64::try_from(
                            CATALOG_MEMBERSHIP_INTERVAL_MS
                                .saturating_sub(now.saturating_sub(checked_at)),
                        )
                        .unwrap_or_default(),
                    )
                });
        if wait_for_catalog_work(
            cancellation,
            &catalog_wake,
            full_sync_wait.min(delta_wait).min(membership_wait),
        )
        .await
        {
            return;
        }
    }
}

fn empty_catalog_retry_delay(failure_count: u32) -> Duration {
    let delay_ms = match failure_count {
        0 | 1 => CATALOG_EMPTY_RETRY_AFTER_MS,
        2 => CATALOG_EMPTY_SECOND_RETRY_AFTER_MS,
        _ => CATALOG_RETRY_AFTER_MS,
    };
    Duration::from_millis(u64::try_from(delay_ms).unwrap_or_default())
}

async fn wait_for_catalog_retry(
    cancellation: &mut tokio::sync::watch::Receiver<bool>,
    delay: Duration,
) -> bool {
    if *cancellation.borrow() {
        return true;
    }
    tokio::select! {
        () = tokio::time::sleep(delay) => false,
        changed = cancellation.changed() => changed.is_err() || *cancellation.borrow(),
    }
}

async fn wait_for_catalog_work(
    cancellation: &mut tokio::sync::watch::Receiver<bool>,
    wake: &tokio::sync::Notify,
    delay: Duration,
) -> bool {
    if *cancellation.borrow() {
        return true;
    }
    tokio::select! {
        () = tokio::time::sleep(delay) => false,
        () = wake.notified() => false,
        changed = cancellation.changed() => changed.is_err() || *cancellation.borrow(),
    }
}

fn persist_catalog_notifications(
    catalog: &MediaCatalog,
    session_key: &str,
    buffer: &std::sync::Mutex<crate::CatalogNotificationBuffer>,
) -> Result<(), String> {
    let batch = buffer
        .lock()
        .map_err(|_| "catalog notification buffer is poisoned".to_owned())?
        .drain(session_key);
    if batch.upsert_ids.is_empty()
        && batch.removed_ids.is_empty()
        && !batch.catchup_required
        && !batch.membership_required
    {
        return Ok(());
    }
    let result = catalog
        .enqueue_library_changes(
            &batch.upsert_ids,
            &batch.removed_ids,
            batch.membership_required,
            now_ms(),
        )
        .and_then(|()| {
            if batch.catchup_required {
                catalog.request_incremental_catchup(now_ms())
            } else {
                Ok(())
            }
        });
    if let Err(error) = result {
        if let Ok(mut pending) = buffer.lock() {
            pending.merge(session_key, batch);
        }
        return Err(error.to_string());
    }
    Ok(())
}

fn record_incremental_failure(catalog: &MediaCatalog, error: &str) -> bool {
    tracing::warn!(error, "media catalog incremental synchronization failed");
    match catalog.record_incremental_failure(now_ms()) {
        Ok(failures) if failures >= CATALOG_INCREMENTAL_FAILURES_BEFORE_FULL => {
            if let Err(mark_error) = catalog.mark_dirty(now_ms()) {
                tracing::warn!(error = %mark_error, "media catalog full-repair fallback could not be marked dirty");
                false
            } else {
                true
            }
        }
        Ok(_) => false,
        Err(record_error) => {
            tracing::warn!(error = %record_error, "media catalog incremental failure could not be recorded");
            false
        }
    }
}

async fn process_catalog_pending(
    client: &EmbyClient,
    catalog: &MediaCatalog,
) -> Result<(), String> {
    loop {
        let pending = catalog
            .pending_changes(CATALOG_PENDING_BATCH_SIZE)
            .map_err(|error| error.to_string())?;
        if pending.upsert_ids.is_empty() && pending.removed_ids.is_empty() {
            return Ok(());
        }
        let (items, user_states, missing_ids) =
            fetch_catalog_items_by_id(client, &pending.upsert_ids).await?;
        let mut removed_ids = pending.removed_ids.iter().cloned().collect::<HashSet<_>>();
        removed_ids.extend(missing_ids);
        let mut removed_ids = removed_ids.into_iter().collect::<Vec<_>>();
        removed_ids.sort_unstable();
        catalog
            .apply_incremental_changes(
                &items,
                &user_states,
                &pending.upsert_ids,
                &removed_ids,
                pending.pending_revision,
                now_ms(),
            )
            .map_err(|error| error.to_string())?;
        tokio::task::yield_now().await;
    }
}

async fn fetch_catalog_items_by_id(
    client: &EmbyClient,
    item_ids: &[String],
) -> Result<(Vec<CatalogItem>, Vec<CatalogUserState>, Vec<String>), String> {
    let requested = item_ids.iter().cloned().collect::<HashSet<_>>();
    let mut returned = HashSet::new();
    let mut items = Vec::with_capacity(item_ids.len());
    let mut user_states = Vec::with_capacity(item_ids.len());
    for item_ids in item_ids.chunks(CATALOG_INCREMENTAL_ID_BATCH_SIZE) {
        let limit = u32::try_from(item_ids.len())
            .map_err(|_| "catalog incremental ID batch exceeds the supported range".to_owned())?;
        let mut query = ItemQuery::full_catalog_page(0, limit);
        query.ids = item_ids.to_vec();
        query.enable_user_data = Some(true);
        query.enable_images = Some(true);
        let page = client
            .items(&query)
            .await
            .map_err(|error| error.to_string())?;
        for item in &page.items {
            if !requested.contains(&item.id) {
                return Err(format!(
                    "Emby returned unexpected catalog item {} for an ID-scoped query",
                    item.id
                ));
            }
            if !returned.insert(item.id.clone()) {
                return Err(format!(
                    "Emby returned duplicate catalog item {} for an ID-scoped query",
                    item.id
                ));
            }
            let Some((projected, state)) = catalog_projection(item) else {
                return Err(format!(
                    "Emby returned malformed catalog item {} for an incremental query",
                    item.id
                ));
            };
            items.push(projected);
            if let Some(state) = state {
                user_states.push(state);
            }
        }
    }
    let mut missing_ids = requested.difference(&returned).cloned().collect::<Vec<_>>();
    missing_ids.sort_unstable();
    Ok((items, user_states, missing_ids))
}

async fn run_catalog_delta(
    client: &EmbyClient,
    catalog: &MediaCatalog,
    pending: &CatalogPendingChanges,
) -> Result<(), String> {
    let checkpoint_ms = now_ms();
    let since_ms = pending
        .watermark_ms
        .unwrap_or(checkpoint_ms)
        .saturating_sub(CATALOG_DELTA_OVERLAP_MS);
    let since = DateTime::<Utc>::from_timestamp_millis(since_ms)
        .ok_or_else(|| "catalog incremental watermark is outside the supported range".to_owned())?
        .to_rfc3339_opts(SecondsFormat::Millis, true);
    let changed = collect_catalog_delta(client, &since).await?;
    let mut items = Vec::with_capacity(changed.len());
    let mut user_states = Vec::with_capacity(changed.len());
    for item in changed.values() {
        let Some((projected, state)) = catalog_projection(item) else {
            return Err(format!(
                "Emby returned malformed catalog item {} for a date delta",
                item.id
            ));
        };
        items.push(projected);
        if let Some(state) = state {
            user_states.push(state);
        }
    }
    if !items.is_empty() {
        catalog
            .apply_incremental_changes(
                &items,
                &user_states,
                &[],
                &[],
                pending.pending_revision,
                checkpoint_ms,
            )
            .map_err(|error| error.to_string())?;
    }
    catalog
        .record_incremental_checkpoint(pending.catchup_revision, checkpoint_ms)
        .map_err(|error| error.to_string())?;
    Ok(())
}

async fn collect_catalog_delta(
    client: &EmbyClient,
    since: &str,
) -> Result<BTreeMap<String, BaseItem>, String> {
    let stages: [&[&str]; 3] = [&["Movie", "Series"], &["Season"], &["Episode"]];
    let mut changed = BTreeMap::new();
    for user_delta in [false, true] {
        for item_types in stages {
            collect_catalog_delta_stage(client, item_types, since, user_delta, &mut changed)
                .await?;
        }
    }
    Ok(changed)
}

async fn collect_catalog_delta_stage(
    client: &EmbyClient,
    item_types: &[&str],
    since: &str,
    user_delta: bool,
    changed: &mut BTreeMap<String, BaseItem>,
) -> Result<(), String> {
    let mut start_index = 0_u64;
    let mut stage_total = None;
    let mut page_ids = HashSet::new();
    let mut previous_signature: Option<(String, String, usize)> = None;
    loop {
        let request_start = u32::try_from(start_index)
            .map_err(|_| "Emby catalog delta exceeds the supported paging range".to_owned())?;
        let mut query = ItemQuery::full_catalog_page(request_start, CATALOG_PAGE_SIZE);
        query.include_item_types = item_types.iter().map(|value| (*value).to_owned()).collect();
        query.enable_user_data = Some(true);
        query.enable_images = Some(true);
        if user_delta {
            query.min_date_last_saved_for_user = Some(since.to_owned());
        } else {
            query.min_date_last_saved = Some(since.to_owned());
        }
        let page = client
            .items(&query)
            .await
            .map_err(|error| error.to_string())?;
        if stage_total.is_some_and(|total| total != page.total_record_count) {
            return Err("Emby catalog delta total changed during paging".to_owned());
        }
        stage_total = Some(page.total_record_count);
        if page.items.is_empty() {
            if start_index < page.total_record_count {
                return Err(format!(
                    "Emby returned an empty catalog delta page at {start_index} of {}",
                    page.total_record_count
                ));
            }
            break;
        }
        let page_len = page.items.len();
        let signature = (
            page.items
                .first()
                .map_or_else(String::new, |item| item.id.clone()),
            page.items
                .last()
                .map_or_else(String::new, |item| item.id.clone()),
            page_len,
        );
        if previous_signature.as_ref() == Some(&signature) {
            return Err(format!(
                "Emby repeated the same catalog delta page after start index {start_index}"
            ));
        }
        previous_signature = Some(signature);
        for item in page.items {
            if item.id.trim().is_empty() || !page_ids.insert(item.id.clone()) {
                return Err("Emby catalog delta contained an empty or repeated item ID".to_owned());
            }
            changed.insert(item.id.clone(), item);
        }
        let remote_count = u64::try_from(page_len)
            .map_err(|_| "catalog delta page size is outside the supported range".to_owned())?;
        start_index = start_index
            .checked_add(remote_count)
            .ok_or_else(|| "catalog delta page index overflowed".to_owned())?;
        if start_index >= page.total_record_count {
            break;
        }
        tokio::task::yield_now().await;
        tokio::time::sleep(CATALOG_PAGE_YIELD).await;
    }
    let stage_total = stage_total.unwrap_or_default();
    if start_index != stage_total {
        return Err(format!(
            "catalog delta stage expected {stage_total} items but observed {start_index}"
        ));
    }
    Ok(())
}

async fn run_catalog_membership_reconciliation(
    client: &EmbyClient,
    catalog: &MediaCatalog,
) -> Result<(), String> {
    let pending = catalog
        .pending_changes(CATALOG_PENDING_BATCH_SIZE)
        .map_err(|error| error.to_string())?;
    let membership = collect_catalog_membership(client).await?;
    let difference = catalog
        .membership_difference(&membership.item_ids)
        .map_err(|error| error.to_string())?;
    catalog
        .enqueue_library_changes(
            &difference.remote_only,
            &difference.local_only,
            false,
            now_ms(),
        )
        .map_err(|error| error.to_string())?;
    process_catalog_pending(client, catalog).await?;
    catalog
        .record_membership_check(pending.membership_revision, now_ms())
        .map_err(|error| error.to_string())?;
    Ok(())
}

async fn run_catalog_sync(
    client: &EmbyClient,
    catalog: &Arc<MediaCatalog>,
    status: &SyncStatus,
) -> Result<(), String> {
    let run = catalog
        .begin_sync(status.total_expected, now_ms())
        .map_err(|error| error.to_string())?;
    let guard = CatalogRunGuard::new(Arc::clone(catalog), run);
    match synchronize_catalog(client, guard.catalog(), run).await {
        Ok(membership) => guard
            .complete(&membership)
            .map_err(|error| error.to_string()),
        Err(error) => {
            guard.fail(&error);
            Err(error)
        }
    }
}

struct CatalogRunGuard {
    catalog: Arc<MediaCatalog>,
    run: Option<CatalogSyncRun>,
}

impl CatalogRunGuard {
    fn new(catalog: Arc<MediaCatalog>, run: CatalogSyncRun) -> Self {
        Self {
            catalog,
            run: Some(run),
        }
    }

    fn catalog(&self) -> &MediaCatalog {
        &self.catalog
    }

    fn complete(mut self, membership: &CatalogMembership) -> Result<(), MediaCatalogError> {
        let Some(run) = self.run else {
            return Ok(());
        };
        if let Err(error) = self.catalog.verify_sync_membership(
            run,
            &membership.item_ids,
            membership.final_total,
            now_ms(),
        ) {
            self.fail_inner(&error.to_string());
            return Err(error);
        }
        match self.catalog.complete_verified_sync(run, now_ms()) {
            Ok(_) => {
                self.run = None;
                Ok(())
            }
            Err(error @ MediaCatalogError::ReconciliationMismatch { .. }) => {
                self.run = None;
                Err(error)
            }
            Err(error) => {
                self.fail_inner(&error.to_string());
                Err(error)
            }
        }
    }

    fn fail(mut self, error: &str) {
        self.fail_inner(error);
    }

    fn fail_inner(&mut self, error: &str) {
        let Some(run) = self.run else {
            return;
        };
        let error = bounded_error(error);
        match self.catalog.fail_sync(run, &error, now_ms()) {
            Ok(()) => self.run = None,
            Err(failure) => {
                tracing::warn!(error = %failure, "media catalog failure state could not be saved");
            }
        }
    }
}

impl Drop for CatalogRunGuard {
    fn drop(&mut self) {
        self.fail_inner("catalog synchronization was cancelled or interrupted");
    }
}

async fn synchronize_catalog(
    client: &EmbyClient,
    catalog: &MediaCatalog,
    run: CatalogSyncRun,
) -> Result<CatalogMembership, String> {
    let stages: [&[&str]; 3] = [&["Movie", "Series"], &["Season"], &["Episode"]];
    let mut stage_totals = [None; 3];
    let mut processed = 0_u64;

    for (stage_index, item_types) in stages.into_iter().enumerate() {
        let mut start_index = 0_u64;
        let mut previous_signature: Option<(String, String, usize)> = None;
        loop {
            let request_start = u32::try_from(start_index)
                .map_err(|_| "Emby catalog exceeds the supported paging range".to_owned())?;
            let mut query = ItemQuery::full_catalog_page(request_start, CATALOG_PAGE_SIZE);
            query.include_item_types = item_types.iter().map(|value| (*value).to_owned()).collect();
            // UserData and image metadata are lightweight and make the generic
            // catalog useful for future stale-first cards. No image bytes or
            // playback URLs are fetched or persisted by this query.
            query.enable_user_data = Some(true);
            query.enable_images = Some(true);
            let page = client
                .items(&query)
                .await
                .map_err(|error| error.to_string())?;
            stage_totals[stage_index] = Some(page.total_record_count);
            if page.items.is_empty() {
                if start_index < page.total_record_count {
                    return Err(format!(
                        "Emby returned an empty catalog page at {start_index} of {}",
                        page.total_record_count
                    ));
                }
                break;
            }

            let signature = (
                page.items
                    .first()
                    .map_or_else(String::new, |item| item.id.clone()),
                page.items
                    .last()
                    .map_or_else(String::new, |item| item.id.clone()),
                page.items.len(),
            );
            if previous_signature.as_ref() == Some(&signature) {
                return Err(format!(
                    "Emby repeated the same catalog page after start index {start_index}"
                ));
            }
            previous_signature = Some(signature);

            let remote_count = u64::try_from(page.items.len())
                .map_err(|_| "catalog page size is outside the supported range".to_owned())?;
            let mut items = Vec::with_capacity(page.items.len());
            let mut user_states = Vec::with_capacity(page.items.len());
            for item in &page.items {
                if let Some((item, state)) = catalog_projection(item) {
                    items.push(item);
                    if let Some(state) = state {
                        user_states.push(state);
                    }
                }
            }
            processed = processed
                .checked_add(remote_count)
                .ok_or_else(|| "catalog progress counter overflowed".to_owned())?;
            let total_expected = stage_totals
                .iter()
                .copied()
                .collect::<Option<Vec<_>>>()
                .and_then(|totals| totals.into_iter().try_fold(0_u64, u64::checked_add));
            catalog
                .upsert_page(
                    run,
                    &items,
                    &user_states,
                    processed,
                    total_expected,
                    now_ms(),
                )
                .map_err(|error| error.to_string())?;

            start_index = start_index
                .checked_add(remote_count)
                .ok_or_else(|| "catalog page index overflowed".to_owned())?;
            if start_index >= page.total_record_count {
                break;
            }
            tokio::task::yield_now().await;
            tokio::time::sleep(CATALOG_PAGE_YIELD).await;
        }
        tokio::task::yield_now().await;
        tokio::time::sleep(CATALOG_PAGE_YIELD).await;
    }
    let final_total = stage_totals
        .into_iter()
        .collect::<Option<Vec<_>>>()
        .ok_or_else(|| "catalog synchronization did not observe every stage total".to_owned())?
        .into_iter()
        .try_fold(0_u64, u64::checked_add)
        .ok_or_else(|| "catalog total counter overflowed".to_owned())?;
    catalog
        .record_sync_progress(run, processed, final_total, now_ms())
        .map_err(|error| error.to_string())?;
    collect_catalog_membership(client).await
}

struct CatalogMembership {
    item_ids: HashSet<String>,
    final_total: u64,
}

async fn collect_catalog_membership(client: &EmbyClient) -> Result<CatalogMembership, String> {
    let stages: [&[&str]; 3] = [&["Movie", "Series"], &["Season"], &["Episode"]];
    let mut item_ids = HashSet::new();
    let mut final_total = 0_u64;

    for item_types in stages {
        let mut start_index = 0_u64;
        let mut stage_total = None;
        let mut previous_signature: Option<(String, String, usize)> = None;
        loop {
            let request_start = u32::try_from(start_index).map_err(|_| {
                "Emby catalog membership exceeds the supported paging range".to_owned()
            })?;
            let mut query =
                ItemQuery::full_catalog_page(request_start, CATALOG_MEMBERSHIP_PAGE_SIZE);
            query.include_item_types = item_types.iter().map(|value| (*value).to_owned()).collect();
            query.fields = Some(Vec::new());
            query.enable_user_data = Some(false);
            query.enable_images = Some(false);
            let page = client
                .items(&query)
                .await
                .map_err(|error| error.to_string())?;
            if stage_total.is_some_and(|total| total != page.total_record_count) {
                return Err("Emby catalog membership total changed during paging".to_owned());
            }
            stage_total = Some(page.total_record_count);
            if page.items.is_empty() {
                if start_index < page.total_record_count {
                    return Err(format!(
                        "Emby returned an empty membership page at {start_index} of {}",
                        page.total_record_count
                    ));
                }
                break;
            }
            let signature = (
                page.items
                    .first()
                    .map_or_else(String::new, |item| item.id.clone()),
                page.items
                    .last()
                    .map_or_else(String::new, |item| item.id.clone()),
                page.items.len(),
            );
            if previous_signature.as_ref() == Some(&signature) {
                return Err(format!(
                    "Emby repeated the same membership page after start index {start_index}"
                ));
            }
            previous_signature = Some(signature);
            for item in &page.items {
                if item.id.trim().is_empty() {
                    return Err("Emby catalog membership contained an empty item ID".to_owned());
                }
                if !item_ids.insert(item.id.clone()) {
                    return Err(format!(
                        "Emby catalog membership repeated item ID {}",
                        item.id
                    ));
                }
            }
            let remote_count = u64::try_from(page.items.len()).map_err(|_| {
                "catalog membership page size is outside the supported range".to_owned()
            })?;
            start_index = start_index
                .checked_add(remote_count)
                .ok_or_else(|| "catalog membership page index overflowed".to_owned())?;
            if start_index >= page.total_record_count {
                break;
            }
            tokio::task::yield_now().await;
            tokio::time::sleep(CATALOG_PAGE_YIELD).await;
        }
        let stage_total = stage_total
            .ok_or_else(|| "catalog membership did not observe a stage total".to_owned())?;
        if start_index != stage_total {
            return Err(format!(
                "catalog membership stage expected {stage_total} items but observed {start_index}"
            ));
        }
        final_total = final_total
            .checked_add(stage_total)
            .ok_or_else(|| "catalog membership total counter overflowed".to_owned())?;
        tokio::task::yield_now().await;
        tokio::time::sleep(CATALOG_PAGE_YIELD).await;
    }

    let observed = u64::try_from(item_ids.len())
        .map_err(|_| "catalog membership count is outside the supported range".to_owned())?;
    if observed != final_total {
        return Err(format!(
            "catalog membership expected {final_total} distinct IDs but observed {observed}"
        ));
    }
    Ok(CatalogMembership {
        item_ids,
        final_total,
    })
}

fn extend_episode_search_aliases(
    aliases: &mut Vec<String>,
    season: i32,
    episode: i32,
    series_title: Option<&str>,
) {
    let padded_season_code = format!("S{season:02}");
    let season_label = format!("Season {season}");
    let chinese_season_label = format!("第{season}季");
    let episode_code = format!("S{season:02}E{episode:02}");
    aliases.extend([
        padded_season_code.clone(),
        format!("S{season}"),
        season_label.clone(),
        chinese_season_label.clone(),
        episode_code.clone(),
        format!("S{season}E{episode}"),
    ]);
    if let Some(series_title) = series_title {
        aliases.extend([
            format!("{series_title} {padded_season_code}"),
            format!("{series_title} {season_label}"),
            format!("{series_title} {chinese_season_label}"),
            format!("{series_title} {episode_code}"),
        ]);
    }
}

fn catalog_projection(item: &BaseItem) -> Option<(CatalogItem, Option<CatalogUserState>)> {
    let item_type = item.item_type.as_deref()?;
    if !matches!(item_type, "Movie" | "Series" | "Season" | "Episode")
        || item.id.trim().is_empty()
        || item.name.trim().is_empty()
    {
        return None;
    }
    let series_id = if item_type == "Series" {
        Some(item.id.clone())
    } else {
        item.series_id.clone()
    };
    let season_id = if item_type == "Season" {
        Some(item.id.clone())
    } else {
        item.season_id.clone()
    };
    let season_number = if item_type == "Season" {
        item.index_number
    } else {
        item.parent_index_number
    };
    let episode_number = (item_type == "Episode")
        .then_some(item.index_number)
        .flatten();
    let mut aliases = item.aliases.clone();
    if item_type == "Season" {
        if let Some(season) = season_number {
            let padded_code = format!("S{season:02}");
            let season_label = format!("Season {season}");
            let chinese_label = format!("第{season}季");
            aliases.extend([
                padded_code.clone(),
                format!("S{season}"),
                season_label.clone(),
                chinese_label.clone(),
            ]);
            if let Some(series_title) = item.series_name.as_deref() {
                aliases.extend([
                    format!("{series_title} {padded_code}"),
                    format!("{series_title} {season_label}"),
                    format!("{series_title} {chinese_label}"),
                ]);
            }
        }
    }
    if let (Some(season), Some(episode)) = (season_number, episode_number) {
        extend_episode_search_aliases(&mut aliases, season, episode, item.series_name.as_deref());
    }
    let image_tag = item
        .image_tags
        .get("Primary")
        .cloned()
        .or_else(|| item.primary_image_tag.clone());
    let projected = CatalogItem {
        id: item.id.clone(),
        item_type: item_type.to_owned(),
        title: item.name.clone(),
        sort_title: item.sort_name.clone().unwrap_or_else(|| item.name.clone()),
        original_title: item.original_title.clone(),
        parent_id: item.parent_id.clone(),
        series_id,
        series_title: item.series_name.clone(),
        season_id,
        season_title: item.season_name.clone(),
        season_number,
        episode_number,
        production_year: item.production_year,
        source_updated_at: item
            .date_modified
            .clone()
            .or_else(|| item.date_last_saved.clone()),
        image_tag,
        primary_image_aspect_ratio: item.primary_image_aspect_ratio,
        series_image_tag: None,
        series_primary_image_aspect_ratio: None,
        aliases,
    };
    let user_state = item.user_data.as_ref().map(|state| CatalogUserState {
        item_id: item.id.clone(),
        favorite: state.is_favorite,
        played: state.played,
        resume_ticks: state.playback_position_ticks,
        progress: state.played_percentage,
        unplayed_count: state.unplayed_item_count,
        last_played_at: state.last_played_date.clone(),
    });
    Some((projected, user_state))
}

fn catalog_search_outcome(
    page: &CatalogSearchPage,
    query: &str,
    start_error: Option<&str>,
    image_urls: &[Option<String>],
) -> Result<CatalogSearchOutcome, ApplicationError> {
    let cards = page
        .items
        .iter()
        .enumerate()
        .map(|(index, hit)| {
            search_card_json(
                hit,
                image_urls
                    .get(index)
                    .and_then(|image_url| image_url.as_deref()),
            )
        })
        .collect();
    let index_error_detail = start_error
        .map(bounded_error)
        .or_else(|| page.status.last_error.as_deref().map(bounded_error));
    let status = CatalogSearchStatus {
        cached_count: page.status.cached_count,
        total_count: page.status.total_expected,
        total_matches: page.total_matches,
        has_more: page.has_more,
        complete: page.status.last_completed_run_id.is_some(),
        syncing: page.status.state == SyncState::Running,
        index_error: index_error_detail.is_some(),
        index_error_detail,
        catalog_revision: page.status.content_revision.to_string(),
        query: query.to_owned(),
        limit: SEARCH_RESULT_LIMIT,
    };
    decode_catalog_outcome(normalized_query_payload(
        vec![("search", query.to_owned(), cards, None)],
        json!({ "searchStatus": status }),
    )?)
}

fn search_card_json(hit: &CatalogSearchHit, image_url: Option<&str>) -> Value {
    let item = base_item_from_search_hit(hit);
    let contextual_episode = hit.item.item_type == "Episode";
    let mut card = media_card_json(&item, image_url, contextual_episode, None);
    let Some(object) = card.as_object_mut() else {
        return card;
    };
    if let Some(image) = search_image_reference(hit) {
        object.insert("imageItemId".to_owned(), json!(image.item_id));
        object.insert("imageItemType".to_owned(), json!(image.item_type));
        object.insert("imageTag".to_owned(), json!(image.image_tag));
        object.insert(
            "primaryImageAspectRatio".to_owned(),
            json!(image.primary_image_aspect_ratio),
        );
    }
    if matches!(hit.item.item_type.as_str(), "Episode" | "Season") {
        let context = if let Some(series_id) = hit.item.series_id.clone() {
            PlaybackContext {
                kind: PlaybackContextKind::Series,
                source_id: Some(series_id),
                source_title: hit.item.series_title.clone(),
                playlist_entry_id: None,
                queue_index: None,
            }
        } else {
            PlaybackContext {
                kind: PlaybackContextKind::Single,
                ..PlaybackContext::default()
            }
        };
        object.insert("playbackContext".to_owned(), json!(context));
    }
    card
}

fn base_item_from_search_hit(hit: &CatalogSearchHit) -> BaseItem {
    let item = &hit.item;
    let mut image_tags = BTreeMap::new();
    let image = search_image_reference(hit);
    if let Some(image) = image {
        image_tags.insert("Primary".to_owned(), image.image_tag.to_owned());
    }
    BaseItem {
        id: item.id.clone(),
        name: item.title.clone(),
        aliases: item.aliases.clone(),
        original_title: item.original_title.clone(),
        sort_name: Some(item.sort_title.clone()),
        parent_id: item.parent_id.clone(),
        item_type: Some(item.item_type.clone()),
        production_year: item.production_year,
        date_last_saved: item.source_updated_at.clone(),
        date_modified: item.source_updated_at.clone(),
        series_name: item.series_title.clone(),
        index_number: item.episode_number.or_else(|| {
            (item.item_type == "Season")
                .then_some(item.season_number)
                .flatten()
        }),
        parent_index_number: (item.item_type == "Episode")
            .then_some(item.season_number)
            .flatten(),
        image_tags,
        primary_image_tag: image.map(|image| image.image_tag.to_owned()),
        series_id: item.series_id.clone(),
        season_id: item.season_id.clone(),
        season_name: item.season_title.clone(),
        primary_image_aspect_ratio: image.and_then(|image| image.primary_image_aspect_ratio),
        user_data: hit.user_state.as_ref().map(|state| UserItemData {
            playback_position_ticks: state.resume_ticks,
            last_played_date: state.last_played_at.clone(),
            played: state.played,
            is_favorite: state.favorite,
            played_percentage: state.progress,
            unplayed_item_count: state.unplayed_count,
        }),
        ..BaseItem::default()
    }
}

#[derive(Clone, Copy)]
struct SearchImageReference<'a> {
    item_id: &'a str,
    item_type: &'a str,
    image_tag: &'a str,
    primary_image_aspect_ratio: Option<f64>,
}

fn search_image_reference(hit: &CatalogSearchHit) -> Option<SearchImageReference<'_>> {
    let item = &hit.item;
    if matches!(item.item_type.as_str(), "Episode" | "Season") {
        if let (Some(series_id), Some(image_tag)) =
            (item.series_id.as_deref(), item.series_image_tag.as_deref())
        {
            return Some(SearchImageReference {
                item_id: series_id,
                item_type: "Series",
                image_tag,
                primary_image_aspect_ratio: item.series_primary_image_aspect_ratio,
            });
        }
    }
    item.image_tag
        .as_deref()
        .map(|image_tag| SearchImageReference {
            item_id: &item.id,
            item_type: &item.item_type,
            image_tag,
            primary_image_aspect_ratio: item.primary_image_aspect_ratio,
        })
}

fn search_image_base_item(hit: &CatalogSearchHit) -> BaseItem {
    let Some(image) = search_image_reference(hit) else {
        return BaseItem::default();
    };
    BaseItem {
        id: image.item_id.to_owned(),
        item_type: Some(image.item_type.to_owned()),
        image_tags: BTreeMap::from([("Primary".to_owned(), image.image_tag.to_owned())]),
        primary_image_tag: Some(image.image_tag.to_owned()),
        primary_image_aspect_ratio: image.primary_image_aspect_ratio,
        ..BaseItem::default()
    }
}

fn catalog_scope_fingerprint(scope: &CatalogScope) -> String {
    let mut first = 0xcbf2_9ce4_8422_2325_u64;
    let mut second = 0x8422_2325_cbf2_9ce4_u64;
    for byte in scope
        .server_local_id
        .as_bytes()
        .iter()
        .copied()
        .chain([0xff])
        .chain(scope.server_id.as_bytes().iter().copied())
        .chain([0xfe])
        .chain(scope.user_id.as_bytes().iter().copied())
    {
        first ^= u64::from(byte);
        first = first.wrapping_mul(0x0100_0000_01b3);
        second ^= u64::from(byte.rotate_left(1));
        second = second.wrapping_mul(0x0100_0000_01b3);
    }
    format!("{first:016x}{second:016x}")
}

fn media_catalog_path(data_dir: &std::path::Path, persistent_key: &str) -> PathBuf {
    data_dir
        .join("cache")
        .join("catalog")
        .join(format!("catalog-v1-{persistent_key}.sqlite3"))
}

fn open_media_catalog(path: &Path, scope: &CatalogScope) -> Result<MediaCatalog, ApplicationError> {
    match MediaCatalog::open(path, scope.clone()) {
        Ok(catalog) => Ok(catalog),
        Err(error) if rebuildable_catalog_error(&error) => {
            tracing::warn!(
                path = %path.display(),
                error = %error,
                "discarding an incompatible or corrupt media catalog cache"
            );
            remove_catalog_database_files(path).map_err(|remove_error| {
                ApplicationError::new(
                    ApplicationErrorCode::Storage,
                    format!(
                        "failed to replace media catalog cache {}: {remove_error}",
                        path.display()
                    ),
                )
            })?;
            MediaCatalog::open(path, scope.clone()).map_err(|error| catalog_error(&error))
        }
        Err(error) => Err(catalog_error(&error)),
    }
}

fn rebuildable_catalog_error(error: &MediaCatalogError) -> bool {
    match error {
        MediaCatalogError::SchemaVersion { .. }
        | MediaCatalogError::SearchVersion { .. }
        | MediaCatalogError::UnrecognizedSchema => true,
        MediaCatalogError::Database(error) => error.sqlite_error().is_some_and(|failure| {
            // SQLite primary result codes: SQLITE_CORRUPT=11 and
            // SQLITE_NOTADB=26. Busy, permission and IO errors are deliberately
            // not treated as evidence that a disposable cache is corrupt.
            matches!(failure.extended_code & 0xff, 11 | 26)
        }),
        MediaCatalogError::Io(_)
        | MediaCatalogError::Poisoned
        | MediaCatalogError::InvalidScope
        | MediaCatalogError::ScopeMismatch
        | MediaCatalogError::InvalidItem
        | MediaCatalogError::NumericRange
        | MediaCatalogError::SyncAlreadyRunning(_)
        | MediaCatalogError::StaleSyncRun { .. }
        | MediaCatalogError::ReconciliationMismatch { .. }
        | MediaCatalogError::MembershipMismatch { .. }
        | MediaCatalogError::MembershipNotVerified(_)
        | MediaCatalogError::MembershipCountMismatch { .. }
        | MediaCatalogError::Json(_) => false,
    }
}

fn remove_catalog_database_files(path: &Path) -> Result<(), std::io::Error> {
    MediaCatalog::remove_disposable_files(path)
}

#[cfg(test)]
fn catalog_sidecar_path(path: &Path, suffix: &str) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(suffix);
    PathBuf::from(value)
}

fn catalog_error(error: &MediaCatalogError) -> ApplicationError {
    ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
}

fn bounded_error(error: &str) -> String {
    error.chars().take(1_024).collect()
}

fn now_ms() -> i64 {
    Utc::now().timestamp_millis()
}

#[cfg(test)]
mod tests {
    use yanami_storage::{CatalogSearchPage, SyncStatus};

    use super::*;

    fn sync_status(state: SyncState) -> SyncStatus {
        SyncStatus {
            state,
            active_run_id: None,
            last_completed_run_id: None,
            next_start_index: 0,
            cached_count: 7,
            total_expected: Some(10),
            started_at_ms: None,
            updated_at_ms: None,
            last_completed_at_ms: None,
            last_error: None,
            dirty: false,
            content_revision: 42,
        }
    }

    #[test]
    fn episode_projection_preserves_real_navigation_and_search_aliases() {
        let item = BaseItem {
            id: "episode-3".to_owned(),
            name: "The Answer".to_owned(),
            item_type: Some("Episode".to_owned()),
            series_id: Some("series-1".to_owned()),
            series_name: Some("Example Show".to_owned()),
            season_id: Some("season-2".to_owned()),
            season_name: Some("Season 2".to_owned()),
            parent_index_number: Some(2),
            index_number: Some(3),
            ..BaseItem::default()
        };

        let (projected, _) = catalog_projection(&item).unwrap();

        assert_eq!(projected.series_id.as_deref(), Some("series-1"));
        assert_eq!(projected.season_id.as_deref(), Some("season-2"));
        assert!(projected.aliases.iter().any(|alias| alias == "S02E03"));
        assert!(
            projected
                .aliases
                .iter()
                .any(|alias| alias == "Example Show S02E03")
        );
        for alias in [
            "S02",
            "Season 2",
            "第2季",
            "Example Show S02",
            "Example Show Season 2",
            "Example Show 第2季",
        ] {
            assert!(
                projected.aliases.iter().any(|value| value == alias),
                "missing episode search alias {alias}"
            );
        }
    }

    #[test]
    fn season_projection_adds_stable_multilingual_aliases() {
        let item = BaseItem {
            id: "season-4".to_owned(),
            name: "第四期".to_owned(),
            item_type: Some("Season".to_owned()),
            series_id: Some("series-1".to_owned()),
            series_name: Some("Example Show".to_owned()),
            index_number: Some(4),
            ..BaseItem::default()
        };

        let (projected, _) = catalog_projection(&item).unwrap();

        for alias in [
            "S04",
            "Season 4",
            "第4季",
            "Example Show S04",
            "Example Show Season 4",
            "Example Show 第4季",
        ] {
            assert!(
                projected.aliases.iter().any(|value| value == alias),
                "missing alias {alias}"
            );
        }
    }

    #[test]
    fn episode_search_card_uses_series_playback_context() {
        let hit = CatalogSearchHit {
            item: CatalogItem {
                id: "episode".to_owned(),
                item_type: "Episode".to_owned(),
                title: "Pilot".to_owned(),
                sort_title: "Pilot".to_owned(),
                series_id: Some("series".to_owned()),
                series_title: Some("Show".to_owned()),
                season_number: Some(1),
                episode_number: Some(1),
                image_tag: Some("episode-poster".to_owned()),
                series_image_tag: Some("series-poster".to_owned()),
                series_primary_image_aspect_ratio: Some(0.667),
                ..CatalogItem::default()
            },
            user_state: None,
            match_rank: 0,
        };

        let card = search_card_json(&hit, Some("image://yanami/v1-stable"));

        assert_eq!(card["playbackContext"]["kind"], "series");
        assert_eq!(card["playbackContext"]["sourceId"], "series");
        assert_eq!(card["playbackContext"]["sourceTitle"], "Show");
        assert_eq!(card["imageUrl"], "image://yanami/v1-stable");
        assert_eq!(card["id"], "episode");
        assert_eq!(card["title"], "Show");
        assert_eq!(card["subtitle"], "S01E01 · Pilot");
        assert_eq!(card["imageItemId"], "series");
        assert_eq!(card["imageItemType"], "Series");
        assert_eq!(card["imageTag"], "series-poster");
        assert_eq!(card["primaryImageAspectRatio"], 0.667);

        let planned = search_image_base_item(&hit);
        assert_eq!(planned.id, "series");
        assert_eq!(planned.item_type.as_deref(), Some("Series"));
        assert_eq!(planned.image_tags["Primary"], "series-poster");
    }

    #[test]
    fn episode_search_image_falls_back_to_the_episode_when_the_series_has_no_poster() {
        let hit = CatalogSearchHit {
            item: CatalogItem {
                id: "episode".to_owned(),
                item_type: "Episode".to_owned(),
                title: "Pilot".to_owned(),
                sort_title: "Pilot".to_owned(),
                series_id: Some("series".to_owned()),
                image_tag: Some("episode-poster".to_owned()),
                ..CatalogItem::default()
            },
            user_state: None,
            match_rank: 0,
        };

        let image = search_image_reference(&hit).unwrap();
        assert_eq!(image.item_id, "episode");
        assert_eq!(image.item_type, "Episode");
        assert_eq!(image.image_tag, "episode-poster");
    }

    #[test]
    fn season_search_card_uses_series_playback_context() {
        let hit = CatalogSearchHit {
            item: CatalogItem {
                id: "season".to_owned(),
                item_type: "Season".to_owned(),
                title: "Season 2".to_owned(),
                sort_title: "Season 2".to_owned(),
                series_id: Some("series".to_owned()),
                series_title: Some("Show".to_owned()),
                season_number: Some(2),
                ..CatalogItem::default()
            },
            user_state: None,
            match_rank: 0,
        };

        let card = search_card_json(&hit, None);

        assert_eq!(card["playbackContext"]["kind"], "series");
        assert_eq!(card["playbackContext"]["sourceId"], "series");
        assert_eq!(card["playbackContext"]["sourceTitle"], "Show");
    }

    #[test]
    fn empty_search_is_a_successful_status_snapshot() {
        let page = CatalogSearchPage {
            items: Vec::new(),
            total_matches: 0,
            has_more: false,
            status: sync_status(SyncState::Running),
        };

        let outcome = catalog_search_outcome(&page, "", None, &[]).unwrap();
        let value = serde_json::to_value(outcome).unwrap();

        assert_eq!(value["queries"]["search"]["rows"], json!([]));
        assert_eq!(value["searchStatus"]["cachedCount"], 7);
        assert_eq!(value["searchStatus"]["totalCount"], 10);
        assert_eq!(value["searchStatus"]["totalMatches"], 0);
        assert_eq!(value["searchStatus"]["hasMore"], false);
        assert_eq!(value["searchStatus"]["complete"], false);
        assert_eq!(value["searchStatus"]["syncing"], true);
        assert_eq!(value["searchStatus"]["indexError"], false);
        assert!(value["searchStatus"].get("indexErrorDetail").is_none());
        assert_eq!(value["searchStatus"]["catalogRevision"], "42");
        assert_eq!(value["searchStatus"]["query"], "");
        assert_eq!(value["searchStatus"]["limit"], SEARCH_RESULT_LIMIT);
    }

    #[test]
    fn failed_empty_catalog_retries_quickly_but_existing_catalog_backs_off() {
        let mut empty = sync_status(SyncState::Failed);
        empty.cached_count = 0;
        empty.total_expected = None;
        empty.updated_at_ms = Some(10_000);

        assert!(!catalog_sync_due(&empty, 14_999));
        assert!(catalog_sync_due(&empty, 15_000));
        assert_eq!(catalog_sync_wait(&empty, 10_000), Duration::from_secs(5));

        let mut existing = empty.clone();
        existing.cached_count = 7;
        assert!(!catalog_sync_due(&existing, 69_999));
        assert!(catalog_sync_due(&existing, 70_000));
        assert_eq!(
            catalog_sync_wait(&existing, 10_000),
            Duration::from_secs(60)
        );

        assert_eq!(empty_catalog_retry_delay(1), Duration::from_secs(5));
        assert_eq!(empty_catalog_retry_delay(2), Duration::from_secs(15));
        assert_eq!(empty_catalog_retry_delay(3), Duration::from_secs(60));
    }

    #[test]
    fn failed_catalog_status_exposes_only_a_bounded_diagnostic_detail() {
        let mut status = sync_status(SyncState::Failed);
        status.last_error = Some("x".repeat(2_048));
        let page = CatalogSearchPage {
            items: Vec::new(),
            total_matches: 0,
            has_more: false,
            status,
        };

        let outcome = catalog_search_outcome(&page, "show", None, &[]).unwrap();
        let value = serde_json::to_value(outcome).unwrap();
        let detail = value["searchStatus"]["indexErrorDetail"].as_str().unwrap();

        assert_eq!(value["searchStatus"]["indexError"], true);
        assert_eq!(detail.chars().count(), 1_024);
    }

    #[test]
    fn membership_verification_failure_preserves_its_real_reason() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("catalog.sqlite3");
        let scope = CatalogScope::new("local", "server", "user");
        let catalog = Arc::new(MediaCatalog::open(&path, scope).unwrap());
        let run = catalog.begin_sync(Some(1), 10).unwrap();
        catalog
            .upsert_page(
                run,
                &[CatalogItem {
                    id: "local-only".to_owned(),
                    item_type: "Movie".to_owned(),
                    title: "Local".to_owned(),
                    sort_title: "Local".to_owned(),
                    ..CatalogItem::default()
                }],
                &[],
                1,
                Some(1),
                20,
            )
            .unwrap();
        let guard = CatalogRunGuard::new(Arc::clone(&catalog), run);
        let error = guard
            .complete(&CatalogMembership {
                item_ids: HashSet::from(["remote-only".to_owned()]),
                final_total: 1,
            })
            .unwrap_err();

        assert!(matches!(
            error,
            MediaCatalogError::MembershipMismatch { .. }
        ));
        let status = catalog.sync_status().unwrap();
        assert_eq!(status.state, SyncState::Failed);
        assert!(status.last_error.as_deref().is_some_and(|value| {
            value.contains("remote-only") && value.contains("local-only")
        }));
    }

    #[test]
    fn third_persisted_incremental_failure_escalates_to_full_repair() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("catalog.sqlite3");
        let scope = CatalogScope::new("local", "server", "user");
        let catalog = MediaCatalog::open(&path, scope.clone()).unwrap();
        let run = catalog.begin_sync(Some(1), 10).unwrap();
        let record = CatalogItem {
            id: "movie".to_owned(),
            item_type: "Movie".to_owned(),
            title: "Movie".to_owned(),
            sort_title: "Movie".to_owned(),
            ..CatalogItem::default()
        };
        catalog
            .upsert_page(run, &[record], &[], 1, Some(1), 20)
            .unwrap();
        catalog
            .verify_sync_membership(run, &HashSet::from(["movie".to_owned()]), 1, 30)
            .unwrap();
        catalog.complete_verified_sync(run, 30).unwrap();

        assert!(!record_incremental_failure(&catalog, "first"));
        assert!(!catalog.sync_status().unwrap().dirty);
        drop(catalog);

        let reopened = MediaCatalog::open(&path, scope).unwrap();
        assert_eq!(reopened.pending_changes(1).unwrap().failure_count, 1);
        assert!(!record_incremental_failure(&reopened, "second"));
        assert!(!reopened.sync_status().unwrap().dirty);
        assert!(record_incremental_failure(&reopened, "third"));
        assert!(reopened.sync_status().unwrap().dirty);
    }

    #[test]
    fn image_hydration_request_is_wire_typed_and_result_bounded() {
        let request: CatalogSearchImageHydrationRequest = serde_json::from_value(json!({
            "query": "show",
            "items": [{
                "itemId": "series-1",
                "itemType": "Series",
                "imageTag": "poster-v1"
            }]
        }))
        .unwrap();
        let items = search_hydration_items(&request).unwrap();
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].id, "series-1");
        assert_eq!(items[0].image_tags["Primary"], "poster-v1");

        let oversized = CatalogSearchImageHydrationRequest {
            query: "show".to_owned(),
            items: vec![request.items[0].clone(); SEARCH_RESULT_LIMIT + 1],
        };
        let error = search_hydration_items(&oversized).unwrap_err();
        assert_eq!(error.code(), ApplicationErrorCode::InvalidInput);
        assert!(error.message().contains("bounded result limit"));
    }

    #[test]
    fn scoped_paths_are_stable_distinct_and_do_not_expose_server_ids() {
        let first = CatalogScope::new("local", "private-server", "private-user");
        let second = CatalogScope::new("local", "private-server", "other-user");
        let first_key = catalog_scope_fingerprint(&first);
        let first_again = catalog_scope_fingerprint(&first);
        let second_key = catalog_scope_fingerprint(&second);
        let path = media_catalog_path(std::path::Path::new("data"), &first_key);

        assert_eq!(first_key, first_again);
        assert_ne!(first_key, second_key);
        let path = path.to_string_lossy();
        assert!(!path.contains("private-server"));
        assert!(!path.contains("private-user"));
    }

    #[test]
    fn corrupt_disposable_catalog_is_rebuilt_with_only_its_exact_sidecars() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("catalog.sqlite3");
        let wal = catalog_sidecar_path(&path, "-wal");
        let shm = catalog_sidecar_path(&path, "-shm");
        let unrelated = temporary.path().join("catalog.sqlite3.keep");
        fs::write(&path, b"not a SQLite database").unwrap();
        fs::write(&wal, b"stale wal").unwrap();
        fs::write(&shm, b"stale shm").unwrap();
        fs::write(&unrelated, b"keep").unwrap();
        let scope = CatalogScope::new("local", "server", "user");

        let catalog = open_media_catalog(&path, &scope).unwrap();

        assert_eq!(catalog.sync_status().unwrap().state, SyncState::Idle);
        assert!(path.exists());
        assert_ne!(fs::read(&wal).unwrap_or_default(), b"stale wal");
        assert_ne!(fs::read(&shm).unwrap_or_default(), b"stale shm");
        drop(catalog);
        assert!(!wal.exists());
        assert!(!shm.exists());
        assert!(unrelated.exists());
    }

    #[test]
    fn scope_mismatch_is_not_discarded_as_corruption() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("catalog.sqlite3");
        let first_scope = CatalogScope::new("local", "server", "first-user");
        drop(MediaCatalog::open(&path, first_scope.clone()).unwrap());

        let error = open_media_catalog(&path, &CatalogScope::new("local", "server", "second-user"))
            .err()
            .expect("scope mismatch must fail");

        assert_eq!(error.code(), ApplicationErrorCode::Storage);
        assert!(error.to_string().contains("different server or user"));
        assert!(MediaCatalog::open(path, first_scope).is_ok());
    }
}
