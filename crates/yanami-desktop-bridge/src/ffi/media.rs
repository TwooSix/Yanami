use std::ffi::c_char;

use serde::{Serialize, de::DeserializeOwned};
use yanami_application::{
    AddToPlaylistOutcome, AddToPlaylistRequest, Application, ApplicationError, DeleteItemOutcome,
    ImageApplyOutcome, ImageApplyRequest, ImageDeleteOutcome, ImageDeleteRequest,
    ImageEditorOutcome, ImageProvidersOutcome, ImageSearchOutcome, ImageSearchRequest,
    ImageUploadOutcome, ImageUploadRequest, MetadataOutcome, MetadataPatch, PlaylistTargetsOutcome,
    RefreshMetadataOutcome, RefreshMetadataRequest, RemoveFromPlaylistOutcome,
    RemoveFromPlaylistRequest, ScanLibraryFilesOutcome, SetFavoriteOutcome, SetPlayedOutcome,
    UpdateMetadataOutcome,
};

use crate::backend::YanamiBackend;

use super::{parse_json, required_string, run_json};

unsafe fn media_without_payload<O: Serialize>(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
    operation: fn(&Application, &str) -> Result<O, ApplicationError>,
) -> i32 {
    unsafe {
        run_json::<O, _>(backend, output, error, move |backend| {
            let item_id = required_string(item_id, "item ID")?;
            operation(&backend.application, &item_id)
        })
    }
}

unsafe fn media_with_payload<T: DeserializeOwned, O: Serialize>(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    payload_json: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
    label: &'static str,
    operation: fn(&Application, &str, &T) -> Result<O, ApplicationError>,
) -> i32 {
    unsafe {
        run_json::<O, _>(backend, output, error, move |backend| {
            let item_id = required_string(item_id, "item ID")?;
            let payload = parse_json(payload_json, label)?;
            operation(&backend.application, &item_id, &payload)
        })
    }
}

macro_rules! item_export {
    ($name:ident, $outcome:ty, $operation:path) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(
            backend: *mut YanamiBackend,
            item_id: *const c_char,
            output: *mut *mut c_char,
            error: *mut *mut c_char,
        ) -> i32 {
            unsafe {
                media_without_payload::<$outcome>(backend, item_id, output, error, $operation)
            }
        }
    };
}

macro_rules! payload_export {
    ($name:ident, $payload:ty, $outcome:ty, $label:literal, $operation:path) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(
            backend: *mut YanamiBackend,
            item_id: *const c_char,
            payload_json: *const c_char,
            output: *mut *mut c_char,
            error: *mut *mut c_char,
        ) -> i32 {
            unsafe {
                media_with_payload::<$payload, $outcome>(
                    backend,
                    item_id,
                    payload_json,
                    output,
                    error,
                    $label,
                    $operation,
                )
            }
        }
    };
}

item_export!(
    yanami_backend_metadata_json,
    MetadataOutcome,
    Application::metadata
);
item_export!(
    yanami_backend_playlist_targets_json,
    PlaylistTargetsOutcome,
    Application::playlist_targets
);
item_export!(
    yanami_backend_image_editor_json,
    ImageEditorOutcome,
    Application::image_editor
);
item_export!(
    yanami_backend_image_providers_json,
    ImageProvidersOutcome,
    Application::image_providers
);
item_export!(
    yanami_backend_scan_library_files_json,
    ScanLibraryFilesOutcome,
    Application::scan_library_files
);
item_export!(
    yanami_backend_delete_item_json,
    DeleteItemOutcome,
    Application::delete_item
);

payload_export!(
    yanami_backend_update_metadata_json,
    MetadataPatch,
    UpdateMetadataOutcome,
    "metadata patch",
    Application::update_metadata
);
payload_export!(
    yanami_backend_add_to_playlist_json,
    AddToPlaylistRequest,
    AddToPlaylistOutcome,
    "playlist destination",
    Application::add_to_playlist
);
payload_export!(
    yanami_backend_remove_from_playlist_json,
    RemoveFromPlaylistRequest,
    RemoveFromPlaylistOutcome,
    "playlist removal",
    Application::remove_from_playlist
);
payload_export!(
    yanami_backend_image_search_json,
    ImageSearchRequest,
    ImageSearchOutcome,
    "image search",
    Application::image_search
);
payload_export!(
    yanami_backend_image_apply_json,
    ImageApplyRequest,
    ImageApplyOutcome,
    "image selection",
    Application::image_apply
);
payload_export!(
    yanami_backend_image_upload_json,
    ImageUploadRequest,
    ImageUploadOutcome,
    "image upload",
    Application::image_upload
);
payload_export!(
    yanami_backend_image_delete_json,
    ImageDeleteRequest,
    ImageDeleteOutcome,
    "image deletion",
    Application::image_delete
);
payload_export!(
    yanami_backend_refresh_metadata_json,
    RefreshMetadataRequest,
    RefreshMetadataOutcome,
    "metadata refresh",
    Application::refresh_metadata
);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_set_played_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    played: i32,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<SetPlayedOutcome, _>(backend, output, error, |backend| {
            let item_id = required_string(item_id, "item ID")?;
            backend.application.set_played(&item_id, played != 0)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_set_favorite_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    favorite: i32,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<SetFavoriteOutcome, _>(backend, output, error, |backend| {
            let item_id = required_string(item_id, "item ID")?;
            backend.application.set_favorite(&item_id, favorite != 0)
        })
    }
}
