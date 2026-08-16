//! Typed `DanDanPlay` API integration.

mod bundled;
mod client;
mod credentials;
mod error;
mod media_hash;
mod model;
mod transport;

pub use bundled::{bundled_credentials, bundled_credentials_available};
pub use client::DandanClient;
pub use credentials::DandanCredentials;
pub use error::DandanError;
pub use media_hash::hash_remote_prefix;
pub use model::{
    AnimeEpisodeSearchResult, AnimeSearchResult, EpisodeMatch, EpisodeSearchResult, MatchInput,
    SearchEpisodesInput,
};
