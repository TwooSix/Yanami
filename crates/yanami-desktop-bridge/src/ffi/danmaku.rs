use std::ffi::c_char;

use yanami_application::{
    DanmakuApplyOutcome, DanmakuApplyRequest, DanmakuAutoOutcome, DanmakuSearchOutcome,
    DanmakuSearchRequest,
};

use crate::backend::YanamiBackend;

use super::{parse_json, required_string, run_json};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_danmaku_search_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    payload_json: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<DanmakuSearchOutcome, _>(backend, output, error, |backend| {
            let item_id = required_string(item_id, "item ID")?;
            let request: DanmakuSearchRequest = parse_json(payload_json, "danmaku search")?;
            backend.application.search_danmaku(&item_id, &request)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_danmaku_auto_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<DanmakuAutoOutcome, _>(backend, output, error, |backend| {
            let item_id = required_string(item_id, "item ID")?;
            backend.application.auto_danmaku(&item_id)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_danmaku_apply_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    payload_json: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<DanmakuApplyOutcome, _>(backend, output, error, |backend| {
            let item_id = required_string(item_id, "item ID")?;
            let request: DanmakuApplyRequest = parse_json(payload_json, "danmaku selection")?;
            backend.application.apply_danmaku(&item_id, &request)
        })
    }
}
