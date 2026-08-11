//! Typed access to the Emby REST API and playback-plan selection.

mod client;
mod models;
mod planner;

pub use client::{ClientIdentity, EmbyClient, EmbyError, ItemQuery};
pub use models::{
    AuthenticationResult, BaseItem, ChapterInfo, ItemsResult, MediaSource, MediaStream,
    PlaybackInfo, PlaybackProgress, UserDto, UserItemData,
};
pub use planner::{PlaybackPlanner, PlaybackPreference};
