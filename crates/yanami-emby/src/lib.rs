//! Typed access to the Emby REST API and playback-plan selection.

mod catalog;
mod client;
#[cfg(test)]
mod client_tests;
mod continue_watching;
mod images;
mod media;
mod models;
mod planner;
mod playback_api;
mod transport;

pub use client::{ClientIdentity, EmbyClient, EmbyError, ItemQuery, RemoteImageQuery};
pub use models::{
    AuthenticationResult, BaseItem, ChapterInfo, EmbyNotification, ImageInfo, ImageProviderInfo,
    ItemsResult, LibraryChange, MediaSource, MediaStream, PlaybackInfo, PlaybackProgress,
    RefreshProgress, RemoteImageInfo, RemoteImageResult, UserConfiguration, UserDataChange,
    UserDto, UserItemData,
};
pub use planner::PlaybackPlanner;
