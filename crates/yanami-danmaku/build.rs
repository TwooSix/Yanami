use std::{
    env, fs,
    path::PathBuf,
    process,
    time::{SystemTime, UNIX_EPOCH},
};

const APP_ID_ENV: &str = "YANAMI_DANDANPLAY_APP_ID";
const APP_SECRET_ENV: &str = "YANAMI_DANDANPLAY_APP_SECRET";

fn main() {
    println!("cargo:rerun-if-env-changed={APP_ID_ENV}");
    println!("cargo:rerun-if-env-changed={APP_SECRET_ENV}");

    let app_id = non_empty_env(APP_ID_ENV);
    let app_secret = non_empty_env(APP_SECRET_ENV);
    let generated = match (app_id, app_secret) {
        (None, None) => no_credentials_source(),
        (Some(app_id), Some(app_secret)) => credentials_source(&app_id, &app_secret),
        _ => panic!("{APP_ID_ENV} and {APP_SECRET_ENV} must either both be set or both be absent"),
    };

    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo must provide OUT_DIR"))
        .join("bundled_credentials.rs");
    fs::write(output, generated).expect("failed to write bundled credential source");
}

fn non_empty_env(name: &str) -> Option<String> {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
}

fn no_credentials_source() -> String {
    "const BUNDLED_APP_ID: Option<ObfuscatedValue> = None;\n\
     const BUNDLED_APP_SECRET: Option<ObfuscatedValue> = None;\n"
        .to_owned()
}

fn credentials_source(app_id: &str, app_secret: &str) -> String {
    let seed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("build clock must be after the Unix epoch")
        .as_nanos() as u64
        ^ u64::from(process::id())
        ^ 0xa076_1d64_78bd_642f;
    let (app_id_ciphertext, app_id_mask, seed) = obfuscate(app_id.as_bytes(), seed);
    let (secret_ciphertext, secret_mask, _) = obfuscate(app_secret.as_bytes(), seed);

    format!(
        "const BUNDLED_APP_ID: Option<ObfuscatedValue> = \
         Some(ObfuscatedValue {{ ciphertext: &{app_id_ciphertext:?}, mask: &{app_id_mask:?} }});\n\
         const BUNDLED_APP_SECRET: Option<ObfuscatedValue> = \
         Some(ObfuscatedValue {{ ciphertext: &{secret_ciphertext:?}, mask: &{secret_mask:?} }});\n"
    )
}

fn obfuscate(input: &[u8], mut state: u64) -> (Vec<u8>, Vec<u8>, u64) {
    let mut ciphertext = Vec::with_capacity(input.len());
    let mut mask = Vec::with_capacity(input.len());
    for byte in input {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        let mask_byte = state.to_le_bytes()[0];
        mask.push(mask_byte);
        ciphertext.push(byte ^ mask_byte);
    }
    (ciphertext, mask, state)
}
