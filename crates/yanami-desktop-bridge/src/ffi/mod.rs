#![deny(unsafe_op_in_unsafe_fn)]
// Every exported function delegates pointer validation and panic containment
// to the shared runners below. Export-specific safety contracts are documented
// here and the lifecycle/ABI boundary is described in docs/ARCHITECTURE.md.
#![allow(clippy::missing_safety_doc)]

mod catalog;
mod danmaku;
mod media;
mod playback;
mod session;

use std::{
    ffi::{CStr, CString, c_char},
    panic::{AssertUnwindSafe, catch_unwind},
    path::Path,
    ptr,
};

use serde::{Serialize, de::DeserializeOwned};
use yanami_application::{ApplicationError, ApplicationErrorCode, ApplicationOpenOptions};

use crate::{
    backend::YanamiBackend,
    codec::{BACKEND_ABI_VERSION, encode_error, encode_response, panic_error},
};

pub use catalog::*;
pub use danmaku::*;
pub use media::*;
pub use playback::*;
pub use session::*;

#[unsafe(no_mangle)]
pub extern "C" fn yanami_backend_abi_version() -> u32 {
    BACKEND_ABI_VERSION
}

/// Additive options for `yanami_backend_new_with_options`.
///
/// Callers must initialize `struct_size` to `sizeof(YanamiBackendOptions)`
/// and use either 0 or 1 for `isolated_credentials`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct YanamiBackendOptions {
    pub struct_size: u32,
    pub isolated_credentials: u32,
}

impl Default for YanamiBackendOptions {
    fn default() -> Self {
        Self {
            struct_size: std::mem::size_of::<Self>() as u32,
            isolated_credentials: 0,
        }
    }
}

#[unsafe(no_mangle)]
/// Cancels in-flight work so the Qt shell can join native workers promptly.
///
/// # Safety
/// `backend` must be null or a live pointer returned by either backend creation
/// function.
pub unsafe extern "C" fn yanami_backend_cancel_all(backend: *mut YanamiBackend) {
    if let Some(backend) = unsafe { backend.as_ref() } {
        backend.cancel_all();
    }
}

#[unsafe(no_mangle)]
/// Creates the application backend.
///
/// # Safety
/// `data_dir` must be valid NUL-terminated UTF-8. `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_new(
    data_dir: *const c_char,
    error: *mut *mut c_char,
) -> *mut YanamiBackend {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let data_dir = unsafe { required_string(data_dir, "data directory") }?;
        YanamiBackend::open(Path::new(&data_dir))
    }));
    match result {
        Ok(Ok(backend)) => Box::into_raw(Box::new(backend)),
        Ok(Err(application_error)) => {
            unsafe { set_output(error, &encode_error(&application_error)) };
            ptr::null_mut()
        }
        Err(_) => {
            unsafe { set_output(error, &panic_error()) };
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// Creates the application backend with explicit storage options.
///
/// # Safety
/// `data_dir` must be valid NUL-terminated UTF-8. `options` must point to a
/// readable `YanamiBackendOptions`. `error` must be null or writable.
pub unsafe extern "C" fn yanami_backend_new_with_options(
    data_dir: *const c_char,
    options: *const YanamiBackendOptions,
    error: *mut *mut c_char,
) -> *mut YanamiBackend {
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let options = unsafe { required_backend_options(options) }?;
        let data_dir = unsafe { required_string(data_dir, "data directory") }?;
        YanamiBackend::open_with_options(Path::new(&data_dir), options)
    }));
    match result {
        Ok(Ok(backend)) => Box::into_raw(Box::new(backend)),
        Ok(Err(application_error)) => {
            unsafe { set_output(error, &encode_error(&application_error)) };
            ptr::null_mut()
        }
        Err(_) => {
            unsafe { set_output(error, &panic_error()) };
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// Frees a backend after all callers have stopped using it.
///
/// # Safety
/// `backend` must be null or an unfreed pointer returned by either backend
/// creation function.
pub unsafe extern "C" fn yanami_backend_free(backend: *mut YanamiBackend) {
    if !backend.is_null() {
        drop(unsafe { Box::from_raw(backend) });
    }
}

#[unsafe(no_mangle)]
/// Frees a string returned by this library.
///
/// # Safety
/// `value` must be null or an unfreed pointer returned through an output parameter.
pub unsafe extern "C" fn yanami_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(unsafe { CString::from_raw(value) });
    }
}

pub(crate) unsafe fn run_json<T, F>(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
    operation: F,
) -> i32
where
    T: Serialize,
    F: FnOnce(&YanamiBackend) -> Result<T, ApplicationError>,
{
    unsafe {
        clear_output(output);
        clear_output(error);
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        operation(backend).and_then(|value| encode_response(&value))
    }));
    finish_string_result(result, output, error)
}

pub(crate) unsafe fn run_status<F>(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
    operation: F,
) -> i32
where
    F: FnOnce(&YanamiBackend) -> Result<(), ApplicationError>,
{
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        operation(backend)
    }));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(application_error)) => {
            unsafe { set_output(error, &encode_error(&application_error)) };
            1
        }
        Err(_) => {
            unsafe { set_output(error, &panic_error()) };
            2
        }
    }
}

pub(crate) unsafe fn run_bool<F>(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
    operation: F,
) -> i32
where
    F: FnOnce(&YanamiBackend) -> Result<bool, ApplicationError>,
{
    unsafe { clear_output(error) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let backend = unsafe { backend_ref(backend) }?;
        operation(backend)
    }));
    match result {
        Ok(Ok(true)) => 1,
        Ok(Ok(false)) => 0,
        Ok(Err(application_error)) => {
            unsafe { set_output(error, &encode_error(&application_error)) };
            -1
        }
        Err(_) => {
            unsafe { set_output(error, &panic_error()) };
            -2
        }
    }
}

pub(crate) unsafe fn required_string(
    value: *const c_char,
    label: &str,
) -> Result<String, ApplicationError> {
    if value.is_null() {
        return Err(ApplicationError::invalid(format!("{label} is required")));
    }
    unsafe { CStr::from_ptr(value) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| ApplicationError::invalid(format!("{label} must be valid UTF-8")))
}

unsafe fn required_backend_options(
    options: *const YanamiBackendOptions,
) -> Result<ApplicationOpenOptions, ApplicationError> {
    let options = unsafe { options.as_ref() }
        .ok_or_else(|| ApplicationError::invalid("backend options are required"))?;
    let expected_size = std::mem::size_of::<YanamiBackendOptions>() as u32;
    if options.struct_size != expected_size {
        return Err(ApplicationError::invalid(format!(
            "backend options size must be {expected_size} bytes"
        )));
    }
    let isolated_credentials = match options.isolated_credentials {
        0 => false,
        1 => true,
        _ => {
            return Err(ApplicationError::invalid(
                "isolated_credentials must be 0 or 1",
            ));
        }
    };
    Ok(ApplicationOpenOptions {
        isolated_credentials,
    })
}

pub(crate) unsafe fn parse_json<T: DeserializeOwned>(
    value: *const c_char,
    label: &str,
) -> Result<T, ApplicationError> {
    let value = unsafe { required_string(value, label) }?;
    serde_json::from_str(&value)
        .map_err(|error| ApplicationError::invalid(format!("invalid {label}: {error}")))
}

unsafe fn backend_ref<'a>(
    backend: *mut YanamiBackend,
) -> Result<&'a YanamiBackend, ApplicationError> {
    unsafe { backend.as_ref() }.ok_or_else(|| {
        ApplicationError::new(ApplicationErrorCode::Internal, "backend pointer is null")
    })
}

unsafe fn clear_output(output: *mut *mut c_char) {
    if let Some(output) = unsafe { output.as_mut() } {
        *output = ptr::null_mut();
    }
}

unsafe fn set_output(output: *mut *mut c_char, value: &str) {
    let Some(output) = (unsafe { output.as_mut() }) else {
        return;
    };
    let sanitized = value.replace('\0', "�");
    if let Ok(value) = CString::new(sanitized) {
        *output = value.into_raw();
    }
}

fn finish_string_result(
    result: Result<Result<String, ApplicationError>, Box<dyn std::any::Any + Send>>,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    match result {
        Ok(Ok(value)) => {
            unsafe { set_output(output, &value) };
            0
        }
        Ok(Err(application_error)) => {
            unsafe { set_output(error, &encode_error(&application_error)) };
            1
        }
        Err(_) => {
            unsafe { set_output(error, &panic_error()) };
            2
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reports_abi_v3() {
        assert_eq!(yanami_backend_abi_version(), 3);
    }

    #[test]
    fn backend_options_have_a_stable_c_layout() {
        assert_eq!(std::mem::size_of::<YanamiBackendOptions>(), 8);
        assert_eq!(std::mem::align_of::<YanamiBackendOptions>(), 4);
        let options = YanamiBackendOptions::default();
        assert_eq!(options.struct_size, 8);
        assert_eq!(options.isolated_credentials, 0);
    }

    #[test]
    fn backend_accepts_a_utf8_non_ascii_data_directory() {
        let temporary = tempfile::tempdir().unwrap();
        let data_dir = temporary.path().join("数据");
        let data_dir = CString::new(data_dir.to_string_lossy().as_bytes()).unwrap();
        let mut error = ptr::null_mut();

        let backend =
            unsafe { yanami_backend_new(data_dir.as_ptr(), std::ptr::addr_of_mut!(error)) };

        if backend.is_null() && !error.is_null() {
            let message = unsafe { CStr::from_ptr(error) }
                .to_string_lossy()
                .into_owned();
            unsafe { yanami_string_free(error) };
            panic!("backend creation failed: {message}");
        }
        assert!(!backend.is_null());
        assert!(error.is_null());
        unsafe { yanami_backend_free(backend) };
    }

    #[test]
    fn backend_options_create_an_isolated_backend_without_changing_the_legacy_entrypoint() {
        let temporary = tempfile::tempdir().unwrap();
        let data_dir = CString::new(temporary.path().to_string_lossy().as_bytes()).unwrap();
        let options = YanamiBackendOptions {
            isolated_credentials: 1,
            ..YanamiBackendOptions::default()
        };
        let mut error = ptr::null_mut();

        let backend = unsafe {
            yanami_backend_new_with_options(
                data_dir.as_ptr(),
                std::ptr::addr_of!(options),
                std::ptr::addr_of_mut!(error),
            )
        };

        assert!(!backend.is_null());
        assert!(error.is_null());
        unsafe { yanami_backend_free(backend) };
    }

    #[test]
    fn backend_options_reject_missing_or_invalid_values() {
        let temporary = tempfile::tempdir().unwrap();
        let data_dir = CString::new(temporary.path().to_string_lossy().as_bytes()).unwrap();
        let missing: *const YanamiBackendOptions = ptr::null();
        let wrong_size = YanamiBackendOptions {
            struct_size: 0,
            isolated_credentials: 0,
        };
        let invalid_flag = YanamiBackendOptions {
            isolated_credentials: 2,
            ..YanamiBackendOptions::default()
        };

        for options in [
            missing,
            std::ptr::addr_of!(wrong_size),
            std::ptr::addr_of!(invalid_flag),
        ] {
            let mut error = ptr::null_mut();
            let backend = unsafe {
                yanami_backend_new_with_options(
                    data_dir.as_ptr(),
                    options,
                    std::ptr::addr_of_mut!(error),
                )
            };
            assert!(backend.is_null());
            assert!(!error.is_null());
            let message = unsafe { CStr::from_ptr(error) }.to_str().unwrap();
            let envelope: serde_json::Value = serde_json::from_str(message).unwrap();
            assert_eq!(envelope["code"], "invalid_input");
            unsafe { yanami_string_free(error) };
        }
    }

    #[test]
    fn null_backend_is_a_structured_error() {
        let mut error = ptr::null_mut();
        let status =
            unsafe { yanami_backend_logout_emby(ptr::null_mut(), std::ptr::addr_of_mut!(error)) };
        assert_eq!(status, 1);
        assert!(!error.is_null());
        let message = unsafe { CStr::from_ptr(error) }.to_str().unwrap();
        let envelope: serde_json::Value = serde_json::from_str(message).unwrap();
        assert_eq!(envelope["code"], "internal");
        unsafe { yanami_string_free(error) };
    }

    #[test]
    fn malformed_json_is_reported_as_invalid_input() {
        let malformed = CString::new("{not-json}").unwrap();
        let error =
            unsafe { parse_json::<serde_json::Value>(malformed.as_ptr(), "request") }.unwrap_err();
        assert_eq!(error.code(), ApplicationErrorCode::InvalidInput);
        assert!(error.message().starts_with("invalid request:"));
    }

    #[test]
    fn empty_catalog_search_query_is_valid_utf8_input() {
        let empty = CString::new("").unwrap();
        let query = unsafe { required_string(empty.as_ptr(), "catalog search query") }.unwrap();
        assert!(query.is_empty());
    }
}
