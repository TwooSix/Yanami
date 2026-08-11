//! Application-level services that coordinate network adapters and durable state.

use std::sync::Arc;

use secrecy::SecretString;
use thiserror::Error;
use yanami_core::UserSession;
use yanami_danmaku::{
    DandanClient, DandanCredentials, DandanError, DanmakuComment, EpisodeMatch,
    EpisodeSearchResult, MatchInput, SearchEpisodesInput, bundled_credentials,
    bundled_credentials_available,
};
use yanami_storage::{AppStorage, CachedComments, CredentialError, CredentialVault, StorageError};

const DANDAN_APP_ID_KEY: &str = "danmaku.dandanplay.app_id";
const DANDAN_SECRET_KEY: &str = "danmaku.dandanplay.app_secret";

#[derive(Debug, Error)]
pub enum ApplicationError {
    #[error(transparent)]
    Storage(#[from] StorageError),
    #[error(transparent)]
    Credential(#[from] CredentialError),
    #[error(transparent)]
    DanDanPlay(#[from] DandanError),
    #[error("DanDanPlay credentials have not been configured")]
    DanDanPlayNotConfigured,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CommentLoadResult {
    pub comments: Vec<DanmakuComment>,
    pub stale: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DandanCredentialSource {
    None,
    Bundled,
    UserProvided,
}

pub struct ApplicationServices {
    storage: Arc<AppStorage>,
    vault: Arc<dyn CredentialVault>,
}

impl ApplicationServices {
    pub fn new(storage: Arc<AppStorage>, vault: Arc<dyn CredentialVault>) -> Self {
        Self { storage, vault }
    }

    pub fn storage(&self) -> &AppStorage {
        &self.storage
    }

    /// Validates user-owned credentials before placing the secret in the OS vault.
    pub async fn configure_dandanplay(
        &self,
        app_id: &str,
        app_secret: SecretString,
    ) -> Result<(), ApplicationError> {
        let app_id = app_id.trim();
        let client = DandanClient::new(DandanCredentials::new(app_id, app_secret.clone()))?;
        client.validate_credentials().await?;

        self.vault.set(DANDAN_SECRET_KEY, &app_secret)?;
        if let Err(error) = self.storage.set_preference(DANDAN_APP_ID_KEY, &app_id) {
            let _ = self.vault.delete(DANDAN_SECRET_KEY);
            return Err(error.into());
        }
        Ok(())
    }

    pub fn dandanplay_configured(&self) -> Result<bool, ApplicationError> {
        Ok(self.dandanplay_credential_source()? != DandanCredentialSource::None)
    }

    pub fn dandanplay_credential_source(&self) -> Result<DandanCredentialSource, ApplicationError> {
        let app_id: Option<String> = self.storage.preference(DANDAN_APP_ID_KEY)?;
        let secret = self.vault.get(DANDAN_SECRET_KEY)?;
        if app_id.flatten_non_empty().is_some() && secret.is_some() {
            Ok(DandanCredentialSource::UserProvided)
        } else if bundled_credentials_available() {
            Ok(DandanCredentialSource::Bundled)
        } else {
            Ok(DandanCredentialSource::None)
        }
    }

    pub fn clear_dandanplay(&self) -> Result<(), ApplicationError> {
        self.vault.delete(DANDAN_SECRET_KEY)?;
        self.storage.delete_preference(DANDAN_APP_ID_KEY)?;
        Ok(())
    }

    pub async fn comments(
        &self,
        episode_id: i64,
        now: i64,
        force_refresh: bool,
    ) -> Result<CommentLoadResult, ApplicationError> {
        let cached = self.storage.comments(episode_id)?;
        if !force_refresh {
            if let Some(cache) = &cached {
                if cache.is_fresh_at(now) {
                    return Ok(CommentLoadResult {
                        comments: cache.comments.clone(),
                        stale: false,
                    });
                }
            }
        }

        let client = self.dandanplay_client()?;
        match client.fetch_comments(episode_id).await {
            Ok(comments) => {
                self.storage.put_comments(&CachedComments {
                    episode_id,
                    comments: comments.clone(),
                    fetched_at: now,
                    expires_at: now + 6 * 60 * 60,
                })?;
                Ok(CommentLoadResult {
                    comments,
                    stale: false,
                })
            }
            Err(error) => {
                if let Some(cache) = cached.filter(|cache| cache.is_usable_stale_at(now)) {
                    Ok(CommentLoadResult {
                        comments: cache.comments,
                        stale: true,
                    })
                } else {
                    Err(error.into())
                }
            }
        }
    }

    /// Matches media through the configured `DanDanPlay` application credentials.
    /// Callers own the policy for choosing between multiple candidate episodes.
    pub async fn match_dandanplay(
        &self,
        input: &MatchInput,
    ) -> Result<Vec<EpisodeMatch>, ApplicationError> {
        Ok(self.dandanplay_client()?.match_media(input).await?)
    }

    /// Searches for a manual episode match after an explicit user action.
    pub async fn search_dandanplay(
        &self,
        input: &SearchEpisodesInput,
    ) -> Result<Vec<EpisodeSearchResult>, ApplicationError> {
        Ok(self.dandanplay_client()?.search_episodes(input).await?)
    }

    /// Persists only the session metadata in `SQLite`; the token goes to the vault.
    pub fn save_emby_session(
        &self,
        session: &UserSession,
        token: &SecretString,
        now: i64,
    ) -> Result<(), ApplicationError> {
        self.vault.set(&session.credential_key, token)?;
        if let Err(error) = self.storage.upsert_user(session, now) {
            let _ = self.vault.delete(&session.credential_key);
            return Err(error.into());
        }
        Ok(())
    }

    pub fn emby_token(
        &self,
        session: &UserSession,
    ) -> Result<Option<SecretString>, ApplicationError> {
        self.vault.get(&session.credential_key).map_err(Into::into)
    }

    pub fn logout(&self, session: &UserSession) -> Result<(), ApplicationError> {
        self.vault
            .delete(&session.credential_key)
            .map_err(Into::into)
    }

    fn dandanplay_client(&self) -> Result<DandanClient, ApplicationError> {
        let app_id: Option<String> = self.storage.preference(DANDAN_APP_ID_KEY)?;
        let secret = self.vault.get(DANDAN_SECRET_KEY)?;
        match (app_id.flatten_non_empty(), secret) {
            (Some(app_id), Some(secret)) => {
                Ok(DandanClient::new(DandanCredentials::new(app_id, secret))?)
            }
            _ => bundled_credentials()
                .map(DandanClient::new)
                .transpose()?
                .ok_or(ApplicationError::DanDanPlayNotConfigured),
        }
    }
}

trait NonEmptyOption {
    fn flatten_non_empty(self) -> Option<String>;
}

impl NonEmptyOption for Option<String> {
    fn flatten_non_empty(self) -> Option<String> {
        self.filter(|value| !value.trim().is_empty())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use yanami_storage::MemoryCredentialVault;

    #[test]
    fn reports_unconfigured_dandanplay() {
        let services = ApplicationServices::new(
            Arc::new(AppStorage::in_memory().unwrap()),
            Arc::new(MemoryCredentialVault::default()),
        );
        assert_eq!(
            services.dandanplay_configured().unwrap(),
            bundled_credentials_available()
        );
        assert_eq!(
            services.dandanplay_credential_source().unwrap(),
            if bundled_credentials_available() {
                DandanCredentialSource::Bundled
            } else {
                DandanCredentialSource::None
            }
        );
    }

    #[test]
    fn user_credentials_take_precedence_over_a_bundled_release() {
        let storage = Arc::new(AppStorage::in_memory().unwrap());
        let vault = Arc::new(MemoryCredentialVault::default());
        storage
            .set_preference(DANDAN_APP_ID_KEY, &"user-app")
            .unwrap();
        vault
            .set(DANDAN_SECRET_KEY, &SecretString::from("user-secret"))
            .unwrap();
        let services = ApplicationServices::new(storage, vault);

        assert_eq!(
            services.dandanplay_credential_source().unwrap(),
            DandanCredentialSource::UserProvided
        );
        assert!(services.dandanplay_client().is_ok());
    }
}
