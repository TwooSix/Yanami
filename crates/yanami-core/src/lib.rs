//! Shared domain types for the `Yanami` desktop client.

mod playback;
mod profile;

pub use playback::{
    MediaTrack, PlaybackMethod, PlaybackPlan, PlayerCommand, PlayerEvent, PlayerSnapshot,
    PlayerState, SensitiveHeaders, TrackKind,
};
pub use profile::{ServerProfile, TlsPolicy, UserSession};
