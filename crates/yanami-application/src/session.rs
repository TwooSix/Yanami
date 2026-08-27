use std::{
    fs,
    future::Future,
    path::Path,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use chrono::Utc;
use futures_util::StreamExt;
use secrecy::SecretString;
use serde::Serialize;
use tokio::{runtime::Handle, sync::watch};
use url::Url;
use uuid::Uuid;
use yanami_core::{ServerProfile, TransportSecurity, UserSession};
use yanami_danmaku::{
    DandanClient, DandanCredentials, DandanError, bundled_credentials,
    bundled_credentials_available,
};
use yanami_emby::{ClientIdentity, EmbyClient, EmbyNotification};
use yanami_storage::{AppStorage, CredentialVault, MemoryCredentialVault, SystemCredentialVault};

use crate::{
    Application, ApplicationError, ApplicationErrorCode, ApplicationOpenOptions,
    images::{
        IMAGE_CACHE_MAX_AGE, IMAGE_CACHE_MAX_BYTES, IMAGE_EDITOR_CACHE_MAX_AGE,
        IMAGE_EDITOR_CACHE_MAX_BYTES, prune_cache_tree,
    },
};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RefreshProgressOutcome {
    items: Vec<RefreshProgressItem>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct RefreshProgressItem {
    item_id: String,
    progress: f64,
    complete: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbySettingsOutcome {
    display_name: String,
    server_url: String,
    user_name: String,
    server_domain: String,
}

const DANDAN_APP_ID_KEY: &str = "danmaku.dandanplay.app_id";
const DANDAN_SECRET_KEY: &str = "danmaku.dandanplay.app_secret";
const REFRESH_PROGRESS_STALE_AFTER: Duration = Duration::from_secs(2 * 60);
const REFRESH_CONNECTION_STABLE_AFTER: Duration = Duration::from_secs(30);
// Cache retention is maintenance work. Give the window, controller, and first
// content requests a quiet startup interval before walking thousands of files.
const CACHE_PRUNE_STARTUP_DELAY: Duration = Duration::from_secs(10);

fn dandan_credential_error(error: DandanError) -> ApplicationError {
    match error {
        DandanError::Api { .. } => {
            ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
        }
        other => other.into(),
    }
}

#[derive(Clone)]
pub(crate) struct ActiveSession {
    pub(crate) profile: ServerProfile,
    pub(crate) session: UserSession,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DandanCredentialSource {
    None,
    Bundled,
    UserProvided,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CredentialVaultMode {
    System,
    IsolatedMemory,
}

fn credential_vault_mode(options: ApplicationOpenOptions) -> CredentialVaultMode {
    if options.isolated_credentials {
        CredentialVaultMode::IsolatedMemory
    } else {
        CredentialVaultMode::System
    }
}

fn credential_vault(options: ApplicationOpenOptions) -> Arc<dyn CredentialVault> {
    match credential_vault_mode(options) {
        CredentialVaultMode::System => Arc::new(SystemCredentialVault::new("Yanami")),
        CredentialVaultMode::IsolatedMemory => Arc::new(MemoryCredentialVault::default()),
    }
}

impl Application {
    pub fn open(
        data_dir: &Path,
        background: Handle,
        cancellation: watch::Sender<bool>,
    ) -> Result<Self, ApplicationError> {
        Self::open_with_options(
            data_dir,
            background,
            cancellation,
            ApplicationOpenOptions::default(),
        )
    }

    pub fn open_with_options(
        data_dir: &Path,
        background: Handle,
        cancellation: watch::Sender<bool>,
        options: ApplicationOpenOptions,
    ) -> Result<Self, ApplicationError> {
        fs::create_dir_all(data_dir).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
        })?;
        let storage = Arc::new(AppStorage::open(data_dir.join("yanami.sqlite3")).map_err(
            |error| ApplicationError::new(ApplicationErrorCode::Storage, error.to_string()),
        )?);
        let active_session = match storage.latest_user_session().map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
        })? {
            Some(session) => storage
                .list_servers()
                .map_err(|error| {
                    ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
                })?
                .into_iter()
                .find(|profile| profile.local_id == session.server_local_id)
                .map(|profile| ActiveSession { profile, session }),
            None => None,
        };
        let vault = credential_vault(options);
        let cache_root = data_dir.join("cache");
        let mut cache_prune_cancellation = cancellation.subscribe();
        let cache_prune_task = background.spawn(async move {
            tokio::select! {
                () = tokio::time::sleep(CACHE_PRUNE_STARTUP_DELAY) => {}
                changed = cache_prune_cancellation.changed() => {
                    if changed.is_err() || *cache_prune_cancellation.borrow() {
                        return;
                    }
                }
            }

            let prune_result = tokio::task::spawn_blocking(move || {
                if let Err(error) = prune_cache_tree(
                    &cache_root.join("images"),
                    IMAGE_CACHE_MAX_BYTES,
                    IMAGE_CACHE_MAX_AGE,
                ) {
                    tracing::warn!(error = %error, "image cache pruning failed");
                }
                if let Err(error) = prune_cache_tree(
                    &cache_root.join("image-editor"),
                    IMAGE_EDITOR_CACHE_MAX_BYTES,
                    IMAGE_EDITOR_CACHE_MAX_AGE,
                ) {
                    tracing::warn!(error = %error, "image editor cache pruning failed");
                }
            })
            .await;
            if let Err(error) = prune_result {
                tracing::warn!(error = %error, "cache pruning worker failed");
            }
        });

        let application = Self {
            background,
            cancellation,
            data_dir: data_dir.to_owned(),
            storage,
            vault,
            active_session: Mutex::new(active_session),
            active_client_cache: Mutex::new(None),
            active_playback: Mutex::new(None),
            refresh_progress: Arc::new(Mutex::new(std::collections::BTreeMap::new())),
            refresh_monitor: Mutex::new(None),
            background_tasks: Mutex::new(vec![crate::RegisteredTask::global(cache_prune_task)]),
            media_catalog: Arc::new(Mutex::new(None)),
            catalog_notifications: Arc::new(
                Mutex::new(crate::CatalogNotificationBuffer::default()),
            ),
            catalog_wake: Arc::new(tokio::sync::Notify::new()),
            image_downloads: Arc::new(Mutex::new(std::collections::HashSet::new())),
            image_download_slots: Arc::new(tokio::sync::Semaphore::new(
                crate::images::IMAGE_DOWNLOAD_CONCURRENCY,
            )),
            image_mutation_generations: Mutex::new(std::collections::BTreeMap::new()),
        };
        if application
            .active_session
            .lock()
            .is_ok_and(|active| active.is_some())
        {
            if let Err(error) = application.start_refresh_monitor() {
                tracing::warn!(error = %error, "Emby notification monitor will retry on demand");
            }
            if let Err(error) = application.ensure_media_catalog_supervisor_background() {
                tracing::warn!(error = %error, "media catalog supervisor will retry on demand");
            }
        }
        Ok(application)
    }

    pub(crate) fn block_on<F, T, E>(&self, future: F) -> Result<T, ApplicationError>
    where
        F: Future<Output = Result<T, E>>,
        E: Into<ApplicationError>,
    {
        let mut cancellation = self.cancellation.subscribe();
        self.background.block_on(async move {
            if *cancellation.borrow() {
                return Err(ApplicationError::cancelled());
            }
            tokio::select! {
                result = future => result.map_err(Into::into),
                changed = cancellation.changed() => {
                    let _ = changed;
                    Err(ApplicationError::cancelled())
                }
            }
        })
    }

    pub(crate) fn block_on_emby<F, T>(&self, future: F) -> Result<T, ApplicationError>
    where
        F: Future<Output = Result<T, yanami_emby::EmbyError>>,
    {
        self.block_on(future)
    }

    pub(crate) fn block_on_dandan<F, T>(&self, future: F) -> Result<T, ApplicationError>
    where
        F: Future<Output = Result<T, DandanError>>,
    {
        self.block_on(future)
    }

    pub fn login_emby(
        &self,
        server_name: &str,
        server_url: &str,
        username: &str,
        password: &SecretString,
        allow_insecure_http: bool,
    ) -> Result<EmbySettingsOutcome, ApplicationError> {
        let transport_security = if allow_insecure_http {
            TransportSecurity::AllowInsecureHttp
        } else {
            TransportSecurity::RequireHttps
        };
        let mut profile = ServerProfile::with_transport_security(
            server_name.trim(),
            Url::parse(server_url.trim())
                .map_err(|error| ApplicationError::invalid(error.to_string()))?,
            transport_security,
        )
        .map_err(|error| ApplicationError::invalid(error.to_string()))?;
        let device_id = Uuid::new_v4();
        let mut client = EmbyClient::new(
            profile.clone(),
            ClientIdentity::yanami(device_id.to_string()),
        )
        .map_err(|error| ApplicationError::new(ApplicationErrorCode::Network, error.to_string()))?;
        let authentication =
            self.block_on_emby(client.authenticate(username.trim(), password.expose_secret()))?;

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
        self.storage.upsert_server(&profile, now).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
        })?;
        self.save_emby_session(
            &session,
            &SecretString::from(authentication.access_token),
            now,
        )?;
        let outcome = EmbySettingsOutcome {
            display_name: profile.display_name.clone(),
            server_url: profile.base_url.to_string(),
            user_name: session.user_name.clone(),
            server_domain: profile.base_url.host_str().unwrap_or_default().to_owned(),
        };
        self.stop_refresh_monitor();
        self.cancel_background_tasks();
        if let Ok(mut catalog) = self.media_catalog.lock() {
            *catalog = None;
        }
        *self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))? =
            Some(ActiveSession { profile, session });
        *self
            .active_client_cache
            .lock()
            .map_err(|_| ApplicationError::internal("client cache lock is poisoned"))? =
            Some((device_id, client));
        // Authentication and persistence above are the session commit point.
        // Notification setup is self-healing and must not turn an already
        // committed login into an apparent failure for the desktop lifecycle.
        if let Err(error) = self.start_refresh_monitor() {
            tracing::warn!(error = %error, "Emby refresh monitor will retry on demand");
        }
        if let Err(error) = self.ensure_media_catalog_supervisor_background() {
            tracing::warn!(error = %error, "media catalog supervisor will retry on demand");
        }
        Ok(outcome)
    }

    pub(crate) fn active_client(&self) -> Result<EmbyClient, ApplicationError> {
        self.active_client_snapshot().map(|(client, _)| client)
    }

    pub(crate) fn active_client_snapshot(&self) -> Result<(EmbyClient, Uuid), ApplicationError> {
        let (profile, session) = {
            let active = self
                .active_session
                .lock()
                .map_err(|_| ApplicationError::internal("session lock is poisoned"))?;
            let active = active
                .as_ref()
                .ok_or_else(ApplicationError::not_connected)?;
            (active.profile.clone(), active.session.clone())
        };
        if let Some((device_id, client)) = self
            .active_client_cache
            .lock()
            .map_err(|_| ApplicationError::internal("client cache lock is poisoned"))?
            .as_ref()
            .filter(|(device_id, _)| *device_id == session.device_id)
        {
            return Ok((client.clone(), *device_id));
        }
        let token = self.emby_token(&session)?.ok_or_else(|| {
            ApplicationError::new(
                ApplicationErrorCode::Credentials,
                "the Emby login token is unavailable; please sign in again",
            )
        })?;
        let client = EmbyClient::with_session(
            profile,
            ClientIdentity::yanami(session.device_id.to_string()),
            session.user_id,
            token,
        )
        .map_err(|error| ApplicationError::new(ApplicationErrorCode::Network, error.to_string()))?;
        let session_is_current = self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))?
            .as_ref()
            .is_some_and(|active| active.session.device_id == session.device_id);
        if !session_is_current {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the Emby session changed while creating a client",
            ));
        }
        *self
            .active_client_cache
            .lock()
            .map_err(|_| ApplicationError::internal("client cache lock is poisoned"))? =
            Some((session.device_id, client.clone()));
        Ok((client, session.device_id))
    }

    pub(crate) fn start_refresh_monitor(&self) -> Result<(), ApplicationError> {
        let (client, session_device_id) = self.active_client_snapshot()?;
        let (catalog_session_key, active_user_id) = self.active_catalog_notification_context()?;
        let mut monitor = self
            .refresh_monitor
            .lock()
            .map_err(|_| ApplicationError::internal("refresh monitor lock is poisoned"))?;
        let session_is_current = self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))?
            .as_ref()
            .is_some_and(|active| active.session.device_id == session_device_id);
        if !session_is_current {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the Emby session changed while opening notifications",
            ));
        }
        if monitor.as_ref().is_some_and(|task| !task.is_finished()) {
            return Ok(());
        }
        if let Some(previous) = monitor.take() {
            previous.abort();
        }
        let progress = Arc::clone(&self.refresh_progress);
        let catalog_notifications = Arc::clone(&self.catalog_notifications);
        let catalog_wake = Arc::clone(&self.catalog_wake);
        *monitor = Some(self.background.spawn(async move {
            let mut retry_delay = Duration::from_secs(1);
            if let Ok(mut pending) = catalog_notifications.lock() {
                // The application may have been offline before its first
                // socket attempt. Persist a reconnect-style repair even when
                // opening the socket itself fails.
                pending.mark_gap(&catalog_session_key);
            } else {
                return;
            }
            catalog_wake.notify_one();
            loop {
                let mut stable_connection = false;
                match client.notification_stream().await {
                    Ok(stream) => {
                        if let Ok(mut pending) = catalog_notifications.lock() {
                            pending.mark_gap(&catalog_session_key);
                        } else {
                            return;
                        }
                        catalog_wake.notify_one();
                        let connected_at = Instant::now();
                        let mut received_notification = false;
                        futures_util::pin_mut!(stream);
                        while let Some(notification) = stream.next().await {
                            received_notification = true;
                            match route_notification(
                                notification,
                                &active_user_id,
                                &catalog_session_key,
                                &progress,
                                &catalog_notifications,
                            ) {
                                Ok(true) => catalog_wake.notify_one(),
                                Ok(false) => {}
                                Err(()) => return,
                            }
                        }
                        if let Ok(mut pending) = catalog_notifications.lock() {
                            pending.mark_gap(&catalog_session_key);
                        } else {
                            return;
                        }
                        catalog_wake.notify_one();
                        stable_connection = received_notification
                            || connected_at.elapsed() >= REFRESH_CONNECTION_STABLE_AFTER;
                    }
                    Err(_) => {
                        tracing::warn!("Emby refresh notification channel unavailable; retrying");
                    }
                }
                let (wait, next_delay) = refresh_retry_schedule(retry_delay, stable_connection);
                tokio::time::sleep(wait).await;
                retry_delay = next_delay;
            }
        }));
        Ok(())
    }

    pub(crate) fn stop_refresh_monitor(&self) {
        let task = self
            .refresh_monitor
            .lock()
            .ok()
            .and_then(|mut monitor| monitor.take());
        self.abort_and_drain_tasks(task.into_iter().collect());
        if let Ok(mut progress) = self.refresh_progress.lock() {
            progress.clear();
        }
        if let Ok(mut pending) = self.catalog_notifications.lock() {
            pending.clear();
        }
    }

    pub fn refresh_progress(&self) -> Result<RefreshProgressOutcome, ApplicationError> {
        self.start_refresh_monitor()?;
        let items = take_refresh_progress_snapshot(&self.refresh_progress)?;
        Ok(RefreshProgressOutcome { items })
    }

    pub fn emby_connected(&self) -> Result<bool, ApplicationError> {
        let active = self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))?;
        let Some(active) = active.as_ref() else {
            return Ok(false);
        };
        Ok(self.emby_token(&active.session)?.is_some())
    }

    pub fn emby_settings(&self) -> Result<EmbySettingsOutcome, ApplicationError> {
        let active = self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))?;
        let active_profile = active.as_ref().map(|value| value.profile.clone());
        let active_user_name = active
            .as_ref()
            .map(|value| value.session.user_name.clone())
            .unwrap_or_default();
        drop(active);
        let profile = match active_profile {
            Some(profile) => Some(profile),
            None => self
                .storage
                .list_servers()
                .map_err(|error| {
                    ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
                })?
                .into_iter()
                .next(),
        };
        Ok(EmbySettingsOutcome {
            display_name: profile
                .as_ref()
                .map_or_else(String::new, |value| value.display_name.clone()),
            server_url: profile
                .as_ref()
                .map_or_else(String::new, |value| value.base_url.to_string()),
            user_name: active_user_name,
            server_domain: profile
                .as_ref()
                .and_then(|value| value.base_url.host_str())
                .unwrap_or_default()
                .to_owned(),
        })
    }

    pub fn logout_emby(&self) -> Result<(), ApplicationError> {
        self.cancel_background_tasks();
        if let Ok(mut catalog) = self.media_catalog.lock() {
            *catalog = None;
        }
        let mut active = self
            .active_session
            .lock()
            .map_err(|_| ApplicationError::internal("session lock is poisoned"))?;
        if let Some(session) = active.as_ref() {
            self.vault
                .delete(&session.session.credential_key)
                .map_err(|error| {
                    ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
                })?;
        }
        *active = None;
        drop(active);
        *self
            .active_client_cache
            .lock()
            .map_err(|_| ApplicationError::internal("client cache lock is poisoned"))? = None;
        self.stop_refresh_monitor();
        *self
            .active_playback
            .lock()
            .map_err(|_| ApplicationError::internal("playback lock is poisoned"))? = None;
        Ok(())
    }

    pub fn configure_dandanplay(
        &self,
        app_id: &str,
        app_secret: &SecretString,
    ) -> Result<(), ApplicationError> {
        let app_id = app_id.trim();
        let client = DandanClient::new(DandanCredentials::new(app_id, app_secret.clone()))
            .map_err(ApplicationError::from)?;
        self.block_on(async {
            client
                .validate_credentials()
                .await
                .map_err(dandan_credential_error)
        })?;
        self.vault
            .set(DANDAN_SECRET_KEY, app_secret)
            .map_err(|error| {
                ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
            })?;
        if let Err(error) = self.storage.set_preference(DANDAN_APP_ID_KEY, &app_id) {
            let _ = self.vault.delete(DANDAN_SECRET_KEY);
            return Err(ApplicationError::new(
                ApplicationErrorCode::Storage,
                error.to_string(),
            ));
        }
        Ok(())
    }

    pub fn dandanplay_credential_source(&self) -> Result<DandanCredentialSource, ApplicationError> {
        let app_id: Option<String> =
            self.storage
                .preference(DANDAN_APP_ID_KEY)
                .map_err(|error| {
                    ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
                })?;
        let secret = self.vault.get(DANDAN_SECRET_KEY).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
        })?;
        if app_id
            .as_ref()
            .is_some_and(|value| !value.trim().is_empty())
            && secret.is_some()
        {
            Ok(DandanCredentialSource::UserProvided)
        } else if bundled_credentials_available() {
            Ok(DandanCredentialSource::Bundled)
        } else {
            Ok(DandanCredentialSource::None)
        }
    }

    pub fn clear_dandanplay(&self) -> Result<(), ApplicationError> {
        self.vault.delete(DANDAN_SECRET_KEY).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
        })?;
        self.storage
            .delete_preference(DANDAN_APP_ID_KEY)
            .map_err(|error| {
                ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
            })?;
        Ok(())
    }

    pub(crate) fn dandanplay_client(&self) -> Result<DandanClient, ApplicationError> {
        let app_id: Option<String> =
            self.storage
                .preference(DANDAN_APP_ID_KEY)
                .map_err(|error| {
                    ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
                })?;
        let secret = self.vault.get(DANDAN_SECRET_KEY).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
        })?;
        match (app_id.filter(|value| !value.trim().is_empty()), secret) {
            (Some(app_id), Some(secret)) => {
                DandanClient::new(DandanCredentials::new(app_id, secret))
                    .map_err(|error| ApplicationError::invalid(error.to_string()))
            }
            _ => bundled_credentials()
                .map(DandanClient::new)
                .transpose()
                .map_err(|error| ApplicationError::invalid(error.to_string()))?
                .ok_or_else(|| {
                    ApplicationError::new(
                        ApplicationErrorCode::Credentials,
                        "DanDanPlay credentials have not been configured",
                    )
                }),
        }
    }

    fn save_emby_session(
        &self,
        session: &UserSession,
        token: &SecretString,
        now: i64,
    ) -> Result<(), ApplicationError> {
        self.vault
            .set(&session.credential_key, token)
            .map_err(|error| {
                ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
            })?;
        if let Err(error) = self.storage.upsert_user(session, now) {
            let _ = self.vault.delete(&session.credential_key);
            return Err(ApplicationError::new(
                ApplicationErrorCode::Storage,
                error.to_string(),
            ));
        }
        Ok(())
    }

    fn emby_token(&self, session: &UserSession) -> Result<Option<SecretString>, ApplicationError> {
        self.vault.get(&session.credential_key).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Credentials, error.to_string())
        })
    }
}

fn route_notification(
    notification: EmbyNotification,
    active_user_id: &str,
    catalog_session_key: &str,
    progress: &Mutex<std::collections::BTreeMap<String, yanami_emby::RefreshProgress>>,
    catalog_notifications: &Mutex<crate::CatalogNotificationBuffer>,
) -> Result<bool, ()> {
    match notification {
        EmbyNotification::RefreshProgress(update) => {
            let mut state = progress.lock().map_err(|_| ())?;
            state.insert(update.item_id.clone(), update);
            Ok(false)
        }
        EmbyNotification::LibraryChanged(change) => {
            let upsert_ids = change.items_added.into_iter().chain(change.items_updated);
            catalog_notifications.lock().map_err(|_| ())?.enqueue(
                catalog_session_key,
                upsert_ids,
                change.items_removed,
                change.requires_membership,
            );
            Ok(true)
        }
        EmbyNotification::UserDataChanged(change) if change.user_id == active_user_id => {
            let mut pending = catalog_notifications.lock().map_err(|_| ())?;
            pending.enqueue(
                catalog_session_key,
                change.item_ids,
                std::iter::empty(),
                false,
            );
            if change.requires_catchup {
                pending.mark_catchup(catalog_session_key);
            }
            Ok(true)
        }
        EmbyNotification::UserDataChanged(_) => Ok(false),
    }
}

fn refresh_retry_schedule(current: Duration, stable_connection: bool) -> (Duration, Duration) {
    let wait = if stable_connection {
        Duration::from_secs(1)
    } else {
        current
    };
    (wait, (wait * 2).min(Duration::from_secs(30)))
}

fn take_refresh_progress_snapshot(
    progress: &Mutex<std::collections::BTreeMap<String, yanami_emby::RefreshProgress>>,
) -> Result<Vec<RefreshProgressItem>, ApplicationError> {
    let mut progress = progress
        .lock()
        .map_err(|_| ApplicationError::internal("refresh progress lock is poisoned"))?;
    let items = progress
        .values()
        .filter(|item| item.complete || item.received_at.elapsed() < REFRESH_PROGRESS_STALE_AFTER)
        .map(|item| RefreshProgressItem {
            item_id: item.item_id.clone(),
            progress: item.progress,
            complete: item.complete,
        })
        .collect();
    progress.retain(|_, item| {
        !item.complete && item.received_at.elapsed() < REFRESH_PROGRESS_STALE_AFTER
    });
    Ok(items)
}

trait ExposeSecret {
    fn expose_secret(&self) -> &str;
}

impl ExposeSecret for SecretString {
    fn expose_secret(&self) -> &str {
        secrecy::ExposeSecret::expose_secret(self)
    }
}

#[cfg(test)]
mod tests {
    use std::{
        collections::{BTreeMap, HashSet},
        future::pending,
        path::PathBuf,
        sync::{
            Arc, Mutex,
            atomic::{AtomicBool, Ordering},
            mpsc,
        },
        time::{Duration, Instant},
    };

    use secrecy::SecretString;
    use tokio::{
        runtime::{Builder, Runtime},
        sync::watch,
    };
    use url::Url;
    use uuid::Uuid;
    use yanami_core::{ServerProfile, UserSession};
    use yanami_danmaku::DandanError;
    use yanami_emby::{EmbyNotification, LibraryChange, RefreshProgress, UserDataChange};
    use yanami_storage::{AppStorage, CredentialVault, MemoryCredentialVault};

    use super::{
        ActiveSession, CredentialVaultMode, credential_vault_mode, dandan_credential_error,
        refresh_retry_schedule, route_notification, take_refresh_progress_snapshot,
    };
    use crate::{Application, ApplicationErrorCode, ApplicationOpenOptions};

    #[test]
    fn application_open_options_keep_system_credentials_by_default() {
        assert_eq!(
            credential_vault_mode(ApplicationOpenOptions::default()),
            CredentialVaultMode::System
        );
    }

    #[test]
    fn isolated_application_open_options_select_an_in_memory_vault() {
        assert_eq!(
            credential_vault_mode(ApplicationOpenOptions {
                isolated_credentials: true,
            }),
            CredentialVaultMode::IsolatedMemory
        );
    }

    fn test_application() -> (Runtime, Application, UserSession) {
        let runtime = Builder::new_multi_thread()
            .worker_threads(1)
            .enable_all()
            .build()
            .unwrap();
        let storage = Arc::new(AppStorage::in_memory().unwrap());
        let profile =
            ServerProfile::new("Home", Url::parse("https://home.test/emby").unwrap()).unwrap();
        storage.upsert_server(&profile, 1).unwrap();
        let session = UserSession {
            server_local_id: profile.local_id,
            server_id: "server".to_owned(),
            user_id: "user".to_owned(),
            user_name: "Viewer".to_owned(),
            device_id: Uuid::new_v4(),
            credential_key: "emby.test.token".to_owned(),
        };
        let vault: Arc<dyn CredentialVault> = Arc::new(MemoryCredentialVault::default());
        let (cancellation, _) = watch::channel(false);
        let application = Application {
            background: runtime.handle().clone(),
            cancellation,
            data_dir: PathBuf::new(),
            storage,
            vault,
            active_session: Mutex::new(Some(ActiveSession {
                profile,
                session: session.clone(),
            })),
            active_client_cache: Mutex::new(None),
            active_playback: Mutex::new(None),
            refresh_progress: Arc::new(Mutex::new(BTreeMap::new())),
            refresh_monitor: Mutex::new(None),
            background_tasks: Mutex::new(Vec::new()),
            media_catalog: Arc::new(Mutex::new(None)),
            catalog_notifications: Arc::new(
                Mutex::new(crate::CatalogNotificationBuffer::default()),
            ),
            catalog_wake: Arc::new(tokio::sync::Notify::new()),
            image_downloads: Arc::new(Mutex::new(HashSet::new())),
            image_download_slots: Arc::new(tokio::sync::Semaphore::new(
                crate::images::IMAGE_DOWNLOAD_CONCURRENCY,
            )),
            image_mutation_generations: Mutex::new(BTreeMap::new()),
        };
        (runtime, application, session)
    }

    #[test]
    fn persisted_session_requires_a_token_and_logout_clears_connection_state() {
        let (runtime, application, session) = test_application();

        assert!(!application.emby_connected().unwrap());
        application
            .save_emby_session(&session, &SecretString::from("token"), 2)
            .unwrap();
        assert!(application.emby_connected().unwrap());
        application.logout_emby().unwrap();
        assert!(!application.emby_connected().unwrap());
        let settings = serde_json::to_value(application.emby_settings().unwrap()).unwrap();
        assert_eq!(settings["displayName"], "Home");
        assert_eq!(settings["serverUrl"], "https://home.test/emby");
        assert_eq!(settings["userName"], "");
        drop(application);
        drop(runtime);
    }

    #[test]
    fn credential_validation_api_failure_is_not_reported_as_network() {
        let error = dandan_credential_error(DandanError::Api {
            code: Some(40101),
            message: "same server wording".to_owned(),
        });
        assert_eq!(error.code(), ApplicationErrorCode::Credentials);

        let error = dandan_credential_error(DandanError::Http {
            status: 503,
            message: "same server wording".to_owned(),
        });
        assert_eq!(error.code(), ApplicationErrorCode::Network);
    }

    #[test]
    fn notification_router_fences_user_and_session_generation() {
        let progress = Mutex::new(BTreeMap::new());
        let pending = Mutex::new(crate::CatalogNotificationBuffer::default());
        assert!(
            !route_notification(
                EmbyNotification::UserDataChanged(UserDataChange {
                    user_id: "other-user".to_owned(),
                    item_ids: vec!["foreign".to_owned()],
                    requires_catchup: false,
                }),
                "active-user",
                "session-a",
                &progress,
                &pending,
            )
            .unwrap()
        );
        assert!(
            route_notification(
                EmbyNotification::UserDataChanged(UserDataChange {
                    user_id: "active-user".to_owned(),
                    item_ids: vec!["same".to_owned()],
                    requires_catchup: false,
                }),
                "active-user",
                "session-a",
                &progress,
                &pending,
            )
            .unwrap()
        );
        route_notification(
            EmbyNotification::LibraryChanged(LibraryChange {
                items_removed: vec!["same".to_owned()],
                ..LibraryChange::default()
            }),
            "active-user",
            "session-a",
            &progress,
            &pending,
        )
        .unwrap();
        let first = pending.lock().unwrap().drain("session-a");
        assert!(first.upsert_ids.is_empty());
        assert_eq!(first.removed_ids, ["same"]);

        route_notification(
            EmbyNotification::UserDataChanged(UserDataChange {
                user_id: "active-user".to_owned(),
                item_ids: vec!["stale-session-item".to_owned()],
                requires_catchup: false,
            }),
            "active-user",
            "session-a",
            &progress,
            &pending,
        )
        .unwrap();
        route_notification(
            EmbyNotification::LibraryChanged(LibraryChange {
                items_added: vec!["new-session-item".to_owned()],
                ..LibraryChange::default()
            }),
            "active-user",
            "session-b",
            &progress,
            &pending,
        )
        .unwrap();
        let second = pending.lock().unwrap().drain("session-b");
        assert_eq!(second.upsert_ids, ["new-session-item"]);
        assert!(
            !second
                .upsert_ids
                .iter()
                .any(|id| id == "stale-session-item")
        );
    }

    #[test]
    fn active_malformed_user_data_marks_catchup_without_membership_reconciliation() {
        let progress = Mutex::new(BTreeMap::new());
        let pending = Mutex::new(crate::CatalogNotificationBuffer::default());

        assert!(
            route_notification(
                EmbyNotification::UserDataChanged(UserDataChange {
                    user_id: "active-user".to_owned(),
                    item_ids: vec!["valid-item".to_owned()],
                    requires_catchup: true,
                }),
                "active-user",
                "session-a",
                &progress,
                &pending,
            )
            .unwrap()
        );

        let batch = pending.lock().unwrap().drain("session-a");
        assert_eq!(batch.upsert_ids, ["valid-item"]);
        assert!(batch.catchup_required);
        assert!(!batch.membership_required);
    }

    #[test]
    fn notification_buffer_merge_preserves_newer_remove_over_older_upsert() {
        let mut pending = crate::CatalogNotificationBuffer::default();
        pending.enqueue("session-a", ["same".to_owned()], [], false);
        let older = pending.drain("session-a");

        pending.enqueue("session-a", [], ["same".to_owned()], false);
        pending.merge("session-a", older);

        let merged = pending.drain("session-a");
        assert!(merged.upsert_ids.is_empty());
        assert_eq!(merged.removed_ids, ["same"]);
    }

    #[test]
    fn notification_buffer_merge_preserves_newer_upsert_over_older_remove() {
        let mut pending = crate::CatalogNotificationBuffer::default();
        pending.enqueue("session-a", [], ["same".to_owned()], false);
        let older = pending.drain("session-a");

        pending.enqueue("session-a", ["same".to_owned()], [], false);
        pending.merge("session-a", older);

        let merged = pending.drain("session-a");
        assert_eq!(merged.upsert_ids, ["same"]);
        assert!(merged.removed_ids.is_empty());
    }

    #[test]
    fn shutdown_aborts_and_drains_registered_background_tasks() {
        struct DropSignal(Arc<AtomicBool>);

        impl Drop for DropSignal {
            fn drop(&mut self) {
                self.0.store(true, Ordering::SeqCst);
            }
        }

        let (runtime, application, _) = test_application();
        let dropped = Arc::new(AtomicBool::new(false));
        let (started_tx, started_rx) = mpsc::channel();
        let task_dropped = Arc::clone(&dropped);
        application
            .spawn_background_task(crate::BackgroundTaskScope::Global, async move {
                let _signal = DropSignal(task_dropped);
                started_tx.send(()).unwrap();
                pending::<()>().await;
            })
            .unwrap();
        runtime.block_on(async { tokio::task::yield_now().await });
        started_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(application.background_task_count(), 1);

        application.shutdown();

        assert!(dropped.load(Ordering::SeqCst));
        assert_eq!(application.background_task_count(), 0);
        drop(application);
        drop(runtime);
    }

    #[test]
    fn image_mutation_cancels_only_tasks_for_the_matching_image_key() {
        struct DropSignal(Arc<AtomicBool>);

        impl Drop for DropSignal {
            fn drop(&mut self) {
                self.0.store(true, Ordering::SeqCst);
            }
        }

        let (runtime, application, _) = test_application();
        let primary_dropped = Arc::new(AtomicBool::new(false));
        let backdrop_dropped = Arc::new(AtomicBool::new(false));
        let (started_tx, started_rx) = mpsc::channel();
        for (image_type, dropped) in [
            ("primary", Arc::clone(&primary_dropped)),
            ("backdrop", Arc::clone(&backdrop_dropped)),
        ] {
            let started_tx = started_tx.clone();
            application
                .spawn_background_task(
                    crate::BackgroundTaskScope::Image {
                        item_id: "episode-1".to_owned(),
                        image_type: image_type.to_owned(),
                    },
                    async move {
                        let _signal = DropSignal(dropped);
                        started_tx.send(()).unwrap();
                        pending::<()>().await;
                    },
                )
                .unwrap();
        }
        runtime.block_on(async { tokio::task::yield_now().await });
        started_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        started_rx.recv_timeout(Duration::from_secs(1)).unwrap();

        application.cancel_image_tasks("episode-1", "Primary");

        assert!(primary_dropped.load(Ordering::SeqCst));
        assert!(!backdrop_dropped.load(Ordering::SeqCst));
        assert_eq!(application.background_task_count(), 1);
        application.shutdown();
        assert!(backdrop_dropped.load(Ordering::SeqCst));
        drop(application);
        drop(runtime);
    }

    #[test]
    fn refresh_retry_backoff_resets_only_after_a_stable_connection() {
        assert_eq!(
            refresh_retry_schedule(Duration::from_secs(1), false),
            (Duration::from_secs(1), Duration::from_secs(2))
        );
        assert_eq!(
            refresh_retry_schedule(Duration::from_secs(16), true),
            (Duration::from_secs(1), Duration::from_secs(2))
        );
    }

    #[test]
    fn completed_refresh_progress_is_consumed_once() {
        let progress = Mutex::new(BTreeMap::from([
            (
                "active".to_owned(),
                RefreshProgress {
                    item_id: "active".to_owned(),
                    progress: 58.0,
                    complete: false,
                    received_at: Instant::now(),
                },
            ),
            (
                "complete".to_owned(),
                RefreshProgress {
                    item_id: "complete".to_owned(),
                    progress: 100.0,
                    complete: true,
                    received_at: Instant::now(),
                },
            ),
        ]));

        let first = take_refresh_progress_snapshot(&progress).unwrap();
        assert_eq!(first.len(), 2);
        assert!(first.iter().any(|item| item.complete));
        let second = take_refresh_progress_snapshot(&progress).unwrap();
        assert_eq!(second.len(), 1);
        assert_eq!(second[0].item_id, "active");
    }
}
