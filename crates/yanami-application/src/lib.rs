//! Application use cases for the Yanami desktop client.
//!
//! This crate owns session state, Emby orchestration, presentation DTOs,
//! image caching, playback preparation and danmaku policy. Transport adapters
//! must depend on this crate instead of reaching into protocol or storage
//! crates directly.

mod catalog;
mod danmaku;
mod error;
mod images;
mod media;
mod playback;
mod presentation;
mod session;

use std::{
    collections::{BTreeMap, HashSet},
    future::Future,
    path::PathBuf,
    sync::{Arc, Mutex},
};

use tokio::{runtime::Handle, sync::watch, task::JoinHandle};
use yanami_emby::{EmbyClient, RefreshProgress};
use yanami_storage::{AppStorage, CredentialVault};

pub use catalog::{ActivityOutcome, CollectionOutcome, FavoritesOutcome, LibraryOutcome};
pub use danmaku::{
    DanmakuApplyOutcome, DanmakuApplyRequest, DanmakuAutoOutcome, DanmakuSearchOutcome,
    DanmakuSearchRequest,
};
pub use error::{ApplicationError, ApplicationErrorCode, ErrorEnvelope};
pub use media::{
    AddToPlaylistOutcome, AddToPlaylistRequest, DeleteItemOutcome, ImageApplyOutcome,
    ImageApplyRequest, ImageDeleteOutcome, ImageDeleteRequest, ImageEditorOutcome,
    ImageProvidersOutcome, ImageSearchOutcome, ImageSearchRequest, ImageUploadOutcome,
    ImageUploadRequest, MetadataOutcome, MetadataPatch, MetadataRefreshMode,
    PlaylistTargetsOutcome, RefreshMetadataOutcome, RefreshMetadataRequest,
    RemoveFromPlaylistOutcome, RemoveFromPlaylistRequest, ScanLibraryFilesOutcome,
    SetFavoriteOutcome, SetPlayedOutcome, UpdateMetadataOutcome,
};
pub use playback::{
    PlaybackContext, PlaybackContextKind, PlaybackPreparedOutcome, PlaybackReportKind,
    PlaybackTelemetry,
};
pub use session::{DandanCredentialSource, EmbySettingsOutcome, RefreshProgressOutcome};

use playback::ActivePlayback;
use session::ActiveSession;

pub(crate) enum BackgroundTaskScope {
    Global,
    Image { item_id: String, image_type: String },
}

pub(crate) struct RegisteredTask {
    scope: BackgroundTaskScope,
    handle: JoinHandle<()>,
}

impl RegisteredTask {
    pub(crate) fn global(handle: JoinHandle<()>) -> Self {
        Self {
            scope: BackgroundTaskScope::Global,
            handle,
        }
    }
}

/// Thread-safe application facade shared by the synchronous desktop ABI.
pub struct Application {
    pub(crate) background: Handle,
    pub(crate) cancellation: watch::Sender<bool>,
    pub(crate) data_dir: PathBuf,
    pub(crate) storage: Arc<AppStorage>,
    pub(crate) vault: Arc<dyn CredentialVault>,
    pub(crate) active_session: Mutex<Option<ActiveSession>>,
    pub(crate) active_client_cache: Mutex<Option<(uuid::Uuid, EmbyClient)>>,
    pub(crate) active_playback: Mutex<Option<ActivePlayback>>,
    pub(crate) refresh_progress: Arc<Mutex<BTreeMap<String, RefreshProgress>>>,
    pub(crate) refresh_monitor: Mutex<Option<JoinHandle<()>>>,
    pub(crate) background_tasks: Mutex<Vec<RegisteredTask>>,
    pub(crate) image_downloads: Arc<Mutex<HashSet<PathBuf>>>,
    pub(crate) image_download_slots: Arc<tokio::sync::Semaphore>,
    pub(crate) image_mutation_generations: Mutex<BTreeMap<(String, String), u64>>,
}

impl Drop for Application {
    fn drop(&mut self) {
        self.shutdown();
    }
}

impl Application {
    /// Stops application-owned background work. The ABI calls this before
    /// joining native workers and dropping the Tokio runtime.
    pub fn shutdown(&self) {
        self.cancellation.send_replace(true);
        self.stop_refresh_monitor();
        self.cancel_background_tasks();
    }

    pub(crate) fn spawn_background_task<F>(
        &self,
        scope: BackgroundTaskScope,
        future: F,
    ) -> Result<(), String>
    where
        F: Future<Output = ()> + Send + 'static,
    {
        if *self.cancellation.borrow() {
            return Err("operation cancelled during shutdown".to_owned());
        }
        let mut tasks = self
            .background_tasks
            .lock()
            .map_err(|_| "background task registry is poisoned")?;
        tasks.retain(|task| !task.handle.is_finished());
        tasks.push(RegisteredTask {
            scope,
            handle: self.background.spawn(future),
        });
        Ok(())
    }

    pub(crate) fn abort_and_drain_tasks(&self, tasks: Vec<JoinHandle<()>>) {
        for task in &tasks {
            task.abort();
        }
        self.background.block_on(async move {
            for task in tasks {
                let _ = task.await;
            }
        });
    }

    pub(crate) fn cancel_background_tasks(&self) {
        let tasks = self
            .background_tasks
            .lock()
            .map(|mut tasks| std::mem::take(&mut *tasks))
            .unwrap_or_default();
        self.abort_and_drain_tasks(tasks.into_iter().map(|task| task.handle).collect());
        if let Ok(mut downloads) = self.image_downloads.lock() {
            downloads.clear();
        }
    }

    pub(crate) fn cancel_image_tasks(&self, item_id: &str, image_type: &str) {
        let normalized_type = image_type.to_ascii_lowercase();
        let cancelled = self
            .background_tasks
            .lock()
            .map(|mut tasks| {
                let mut cancelled = Vec::new();
                let mut retained = Vec::with_capacity(tasks.len());
                for task in tasks.drain(..) {
                    if matches!(
                        &task.scope,
                        BackgroundTaskScope::Image {
                            item_id: task_item_id,
                            image_type: task_image_type,
                        } if task_item_id == item_id && task_image_type == &normalized_type
                    ) {
                        cancelled.push(task.handle);
                    } else {
                        retained.push(task);
                    }
                }
                *tasks = retained;
                cancelled
            })
            .unwrap_or_default();
        self.abort_and_drain_tasks(cancelled);
    }

    #[cfg(test)]
    pub(crate) fn background_task_count(&self) -> usize {
        self.background_tasks.lock().map_or(0, |tasks| tasks.len())
    }
}

pub(crate) fn display_error(error: impl std::fmt::Display) -> String {
    error.to_string()
}
