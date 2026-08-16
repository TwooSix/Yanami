//! Shared domain types for the `Yanami` desktop client.

mod danmaku;
mod endpoint;
mod playback;
mod profile;

pub use danmaku::{DanmakuComment, DanmakuMode};
pub use endpoint::{SameOriginUrl, SameOriginUrlError, same_origin};
pub use playback::{
    MediaTrack, PlaybackHeaderError, PlaybackHeaders, PlaybackMethod, PlaybackPlan,
    PlaybackWarning, TrackKind,
};
pub use profile::{ProfileError, ServerProfile, TlsPolicy, TransportSecurity, UserSession};
