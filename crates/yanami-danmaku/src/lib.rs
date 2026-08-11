//! `DanDanPlay` API integration and deterministic ASS generation.

mod ass;
mod bundled;
mod client;
mod model;

pub use ass::{AssConfig, AssGenerator};
pub use bundled::{bundled_credentials, bundled_credentials_available};
pub use client::{DandanClient, DandanCredentials, DandanError, hash_remote_prefix};
pub use model::{
    DanmakuComment, DanmakuMode, EpisodeMatch, EpisodeSearchResult, MatchInput, SearchEpisodesInput,
};
