use std::ffi::c_char;

use yanami_application::{ActivityOutcome, CollectionOutcome, FavoritesOutcome, LibraryOutcome};

use crate::backend::YanamiBackend;

use super::{required_string, run_json};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_library_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<LibraryOutcome, _>(backend, output, error, |backend| {
            backend.application.library()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_activity_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<ActivityOutcome, _>(backend, output, error, |backend| {
            backend.application.activity()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_favorites_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<FavoritesOutcome, _>(backend, output, error, |backend| {
            backend.application.favorites()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_collection_json(
    backend: *mut YanamiBackend,
    parent_id: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<CollectionOutcome, _>(backend, output, error, |backend| {
            let parent_id = required_string(parent_id, "parent item ID")?;
            backend.application.collection(&parent_id)
        })
    }
}
