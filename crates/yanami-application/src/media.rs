pub(crate) mod image_actions;
mod library;
mod metadata;
mod playlist;
mod state;

pub use image_actions::{
    ImageApplyOutcome, ImageApplyRequest, ImageDeleteOutcome, ImageDeleteRequest,
    ImageEditorOutcome, ImageProvidersOutcome, ImageSearchOutcome, ImageSearchRequest,
    ImageUploadOutcome, ImageUploadRequest,
};
pub use library::{
    DeleteItemOutcome, MetadataRefreshMode, RefreshMetadataOutcome, RefreshMetadataRequest,
    ScanLibraryFilesOutcome,
};
pub use metadata::{MetadataOutcome, MetadataPatch, UpdateMetadataOutcome};
pub use playlist::{
    AddToPlaylistOutcome, AddToPlaylistRequest, PlaylistTargetsOutcome, RemoveFromPlaylistOutcome,
    RemoveFromPlaylistRequest,
};
pub use state::{SetFavoriteOutcome, SetPlayedOutcome};

use serde::Serialize;
use uuid::Uuid;
use yanami_emby::EmbyClient;

use crate::ApplicationError;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InvalidationOutcome {
    operation_id: String,
    entity_ids: Vec<String>,
    query_kinds: Vec<&'static str>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaOutcome<T> {
    item_id: String,
    result: T,
    #[serde(skip_serializing_if = "Option::is_none")]
    invalidation: Option<InvalidationOutcome>,
}

impl<T> MediaOutcome<T> {
    fn read(item_id: &str, result: T) -> Self {
        Self {
            item_id: item_id.to_owned(),
            result,
            invalidation: None,
        }
    }

    fn invalidated(item_id: &str, result: T, query_kinds: &[&'static str]) -> Self {
        Self {
            item_id: item_id.to_owned(),
            result,
            invalidation: Some(InvalidationOutcome {
                operation_id: Uuid::new_v4().to_string(),
                entity_ids: vec![item_id.to_owned()],
                query_kinds: query_kinds.to_vec(),
            }),
        }
    }
}

pub(crate) async fn require_administrator(client: &EmbyClient) -> Result<(), ApplicationError> {
    let user = client.current_user().await.map_err(network_error)?;
    if user.policy.is_administrator {
        Ok(())
    } else {
        Err(ApplicationError::permission_denied(
            "this operation requires an Emby administrator",
        ))
    }
}

pub(crate) fn network_error(error: yanami_emby::EmbyError) -> ApplicationError {
    error.into()
}
