//! Playback state machine and the command contract implemented by the Qt/libmpv adapter.

use std::{
    collections::BTreeMap,
    sync::{Arc, Mutex},
};

use async_trait::async_trait;
use thiserror::Error;
use tokio::sync::broadcast;
use yanami_core::{PlaybackPlan, PlayerCommand, PlayerEvent, PlayerSnapshot, PlayerState};

#[derive(Debug, Error)]
pub enum PlayerError {
    #[error("player backend failed: {0}")]
    Backend(String),
    #[error("player state lock is poisoned")]
    Poisoned,
}

#[async_trait]
pub trait PlayerBackend: Send + Sync {
    async fn send(&self, command: PlayerCommand) -> Result<(), PlayerError>;
    fn subscribe(&self) -> broadcast::Receiver<PlayerEvent>;
}

pub struct PlaybackCoordinator<B> {
    backend: Arc<B>,
    current_plan: Mutex<Option<PlaybackPlan>>,
    snapshot: Mutex<PlayerSnapshot>,
}

impl<B: PlayerBackend> PlaybackCoordinator<B> {
    pub fn new(backend: Arc<B>) -> Self {
        Self {
            backend,
            current_plan: Mutex::new(None),
            snapshot: Mutex::new(PlayerSnapshot {
                volume: 100.0,
                rate: 1.0,
                ..PlayerSnapshot::default()
            }),
        }
    }

    pub async fn load(&self, plan: PlaybackPlan) -> Result<(), PlayerError> {
        self.backend.send(PlayerCommand::Load(plan.clone())).await?;
        *self
            .current_plan
            .lock()
            .map_err(|_| PlayerError::Poisoned)? = Some(plan);
        self.snapshot
            .lock()
            .map_err(|_| PlayerError::Poisoned)?
            .state = PlayerState::Loading;
        Ok(())
    }

    pub async fn send(&self, command: PlayerCommand) -> Result<(), PlayerError> {
        self.backend.send(command).await
    }

    pub fn apply_event(&self, event: &PlayerEvent) -> Result<(), PlayerError> {
        match event {
            PlayerEvent::Snapshot(snapshot) => {
                *self.snapshot.lock().map_err(|_| PlayerError::Poisoned)? = snapshot.clone();
            }
            PlayerEvent::EndFile => {
                self.snapshot
                    .lock()
                    .map_err(|_| PlayerError::Poisoned)?
                    .state = PlayerState::Ended;
            }
            PlayerEvent::Error(_) => {
                self.snapshot
                    .lock()
                    .map_err(|_| PlayerError::Poisoned)?
                    .state = PlayerState::Failed;
            }
            PlayerEvent::TracksChanged(_) => {}
        }
        Ok(())
    }

    pub fn snapshot(&self) -> Result<PlayerSnapshot, PlayerError> {
        Ok(self
            .snapshot
            .lock()
            .map_err(|_| PlayerError::Poisoned)?
            .clone())
    }

    pub fn current_plan(&self) -> Result<Option<PlaybackPlan>, PlayerError> {
        Ok(self
            .current_plan
            .lock()
            .map_err(|_| PlayerError::Poisoned)?
            .clone())
    }
}

/// Stable mpv defaults shared by the Rust coordinator and native Qt adapter.
pub fn recommended_mpv_options() -> BTreeMap<&'static str, &'static str> {
    BTreeMap::from([
        ("vo", "libmpv"),
        ("hwdec", "auto"),
        ("terminal", "no"),
        ("keep-open", "yes"),
        ("sub-auto", "no"),
        ("sub-ass-override", "scale"),
        ("secondary-sub-ass-override", "no"),
        ("audio-client-name", "Yanami"),
    ])
}

/// Test backend and headless fallback. It records commands but performs no I/O.
pub struct RecordingBackend {
    commands: Mutex<Vec<PlayerCommand>>,
    events: broadcast::Sender<PlayerEvent>,
}

impl Default for RecordingBackend {
    fn default() -> Self {
        let (events, _) = broadcast::channel(64);
        Self {
            commands: Mutex::new(Vec::new()),
            events,
        }
    }
}

impl RecordingBackend {
    pub fn commands(&self) -> Result<Vec<PlayerCommand>, PlayerError> {
        Ok(self
            .commands
            .lock()
            .map_err(|_| PlayerError::Poisoned)?
            .clone())
    }

    pub fn emit(&self, event: PlayerEvent) {
        let _ = self.events.send(event);
    }
}

#[async_trait]
impl PlayerBackend for RecordingBackend {
    async fn send(&self, command: PlayerCommand) -> Result<(), PlayerError> {
        self.commands
            .lock()
            .map_err(|_| PlayerError::Poisoned)?
            .push(command);
        Ok(())
    }

    fn subscribe(&self) -> broadcast::Receiver<PlayerEvent> {
        self.events.subscribe()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;
    use url::Url;
    use yanami_core::PlaybackMethod;

    fn plan() -> PlaybackPlan {
        PlaybackPlan {
            item_id: "item".into(),
            media_source_id: "source".into(),
            play_session_id: "session".into(),
            method: PlaybackMethod::DirectStream,
            url: Url::parse("https://example.test/video.mkv").unwrap(),
            request_headers: BTreeMap::new().into(),
            resume_position_ticks: 0,
            audio_stream_index: None,
            subtitle_stream_index: None,
            tracks: Vec::new(),
        }
    }

    #[tokio::test]
    async fn records_load_and_tracks_state() {
        let backend = Arc::new(RecordingBackend::default());
        let coordinator = PlaybackCoordinator::new(backend.clone());
        coordinator.load(plan()).await.unwrap();

        assert!(matches!(
            backend.commands().unwrap()[0],
            PlayerCommand::Load(_)
        ));
        assert_eq!(coordinator.snapshot().unwrap().state, PlayerState::Loading);
        coordinator.apply_event(&PlayerEvent::EndFile).unwrap();
        assert_eq!(coordinator.snapshot().unwrap().state, PlayerState::Ended);
    }
}
