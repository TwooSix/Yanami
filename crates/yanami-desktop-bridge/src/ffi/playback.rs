use std::ffi::c_char;

use yanami_application::{PlaybackContext, PlaybackPreparedOutcome, PlaybackTelemetry};

use crate::backend::YanamiBackend;

use super::{parse_json, required_string, run_json, run_status};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_playback_request_json(
    backend: *mut YanamiBackend,
    item_id: *const c_char,
    context_json: *const c_char,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<PlaybackPreparedOutcome, _>(backend, output, error, |backend| {
            let item_id = required_string(item_id, "item ID")?;
            let context: PlaybackContext = parse_json(context_json, "playback context")?;
            backend.application.prepare_playback(&item_id, &context)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_report_playback_json(
    backend: *mut YanamiBackend,
    telemetry_json: *const c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_status(backend, error, |backend| {
            let telemetry: PlaybackTelemetry = parse_json(telemetry_json, "playback telemetry")?;
            backend.application.report_playback(&telemetry)
        })
    }
}
