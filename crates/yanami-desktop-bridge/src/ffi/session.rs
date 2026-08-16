use std::ffi::c_char;

use secrecy::SecretString;
use yanami_application::{DandanCredentialSource, EmbySettingsOutcome, RefreshProgressOutcome};
use zeroize::Zeroize;

use crate::backend::YanamiBackend;

use super::{required_string, run_bool, run_json, run_status};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_dandanplay_credential_source(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        super::clear_output(error);
    }
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let backend = unsafe { super::backend_ref(backend) }?;
        backend.application.dandanplay_credential_source()
    }));
    match result {
        Ok(Ok(DandanCredentialSource::None)) => 0,
        Ok(Ok(DandanCredentialSource::Bundled)) => 1,
        Ok(Ok(DandanCredentialSource::UserProvided)) => 2,
        Ok(Err(application_error)) => {
            unsafe { super::set_output(error, &crate::codec::encode_error(&application_error)) };
            -1
        }
        Err(_) => {
            unsafe { super::set_output(error, &crate::codec::panic_error()) };
            -2
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_configure_dandanplay(
    backend: *mut YanamiBackend,
    app_id: *const c_char,
    app_secret: *const c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_status(backend, error, |backend| {
            let app_id = required_string(app_id, "AppId")?;
            let mut secret = required_string(app_secret, "AppSecret")?;
            let result = backend
                .application
                .configure_dandanplay(&app_id, &SecretString::from(secret.clone()));
            secret.zeroize();
            result
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_clear_dandanplay(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_status(backend, error, |backend| {
            backend.application.clear_dandanplay()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_emby_connected(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_bool(backend, error, |backend| {
            backend.application.emby_connected()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_emby_settings_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<EmbySettingsOutcome, _>(backend, output, error, |backend| {
            backend.application.emby_settings()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_refresh_progress_json(
    backend: *mut YanamiBackend,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<RefreshProgressOutcome, _>(backend, output, error, |backend| {
            backend.application.refresh_progress()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_login_emby(
    backend: *mut YanamiBackend,
    server_name: *const c_char,
    server_url: *const c_char,
    username: *const c_char,
    password: *const c_char,
    allow_insecure_http: i32,
    output: *mut *mut c_char,
    error: *mut *mut c_char,
) -> i32 {
    unsafe {
        run_json::<EmbySettingsOutcome, _>(backend, output, error, |backend| {
            let server_name = required_string(server_name, "server name")?;
            let server_url = required_string(server_url, "server URL")?;
            let username = required_string(username, "username")?;
            let mut password = required_string(password, "password")?;
            let secret = SecretString::from(password.clone());
            password.zeroize();
            backend.application.login_emby(
                &server_name,
                &server_url,
                &username,
                &secret,
                allow_insecure_http != 0,
            )
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn yanami_backend_logout_emby(
    backend: *mut YanamiBackend,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { run_status(backend, error, |backend| backend.application.logout_emby()) }
}
