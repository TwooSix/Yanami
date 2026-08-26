use std::{
    collections::BTreeMap,
    sync::atomic::{AtomicBool, AtomicU8, Ordering},
};

use serde::Deserialize;
use serde_json::Value;
use tokio::sync::OnceCell;

use crate::{
    client::{BROWSE_FIELDS, EmbyClient, EmbyError},
    models::ItemsResult,
    transport::decode,
};

const SERVER_HOME_SECTIONS_VERSION: ServerVersion = ServerVersion([4, 10, 0, 4]);
const FLAT_USER_SETTINGS_VERSION: ServerVersion = ServerVersion([4, 9, 0, 23]);
const DESKTOP_HOME_DISPLAY_MODE: &str = "mobile,desktop";
const SETTINGS_API_UNKNOWN: u8 = 0;
const SETTINGS_API_DISPLAY_PREFERENCES: u8 = 1;
const SETTINGS_API_FLAT: u8 = 2;

#[derive(Default)]
pub(crate) struct EmbyCompatibilityState {
    capabilities: OnceCell<ServerCapabilities>,
}

struct ServerCapabilities {
    version: Option<String>,
    may_support_server_home_sections: bool,
    server_home_sections_disabled: AtomicBool,
    settings_api: AtomicU8,
}

impl ServerCapabilities {
    fn from_version(version: Option<String>) -> Self {
        let parsed = version.as_deref().and_then(ServerVersion::parse);
        Self {
            version,
            // Unknown versions are probed. This keeps forks and future version
            // formats working without turning the version string into truth.
            may_support_server_home_sections: parsed
                .is_none_or(|version| version >= SERVER_HOME_SECTIONS_VERSION),
            server_home_sections_disabled: AtomicBool::new(false),
            settings_api: AtomicU8::new(parsed.map_or(SETTINGS_API_UNKNOWN, |version| {
                if version >= FLAT_USER_SETTINGS_VERSION {
                    SETTINGS_API_FLAT
                } else {
                    SETTINGS_API_DISPLAY_PREFERENCES
                }
            })),
        }
    }

    fn server_home_sections_enabled(&self) -> bool {
        self.may_support_server_home_sections
            && !self.server_home_sections_disabled.load(Ordering::Acquire)
    }

    fn disable_server_home_sections(&self) {
        self.server_home_sections_disabled
            .store(true, Ordering::Release);
    }

    fn settings_api(&self) -> u8 {
        self.settings_api.load(Ordering::Acquire)
    }

    fn remember_settings_api(&self, value: u8) {
        self.settings_api.store(value, Ordering::Release);
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct ServerVersion([u32; 4]);

impl ServerVersion {
    fn parse(value: &str) -> Option<Self> {
        let value = value.trim();
        if value.is_empty() {
            return None;
        }
        let mut parts = [0_u32; 4];
        let mut count = 0_usize;
        for (index, component) in value.split('.').take(4).enumerate() {
            let digits: String = component.chars().take_while(char::is_ascii_digit).collect();
            if digits.is_empty() {
                return None;
            }
            parts[index] = digits.parse().ok()?;
            count += 1;
        }
        (count >= 2).then_some(Self(parts))
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "PascalCase")]
struct PublicSystemInfo {
    #[serde(default)]
    version: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "PascalCase")]
struct HomeSection {
    #[serde(default)]
    id: Option<String>,
    #[serde(default)]
    section_type: Option<String>,
}

#[derive(Default, Deserialize)]
#[serde(rename_all = "PascalCase")]
struct LegacyDisplayPreferences {
    #[serde(default)]
    custom_prefs: BTreeMap<String, Value>,
}

#[derive(Debug)]
struct LegacyHomeLayout {
    resume_visible: bool,
    separate_next_up: bool,
}

impl Default for LegacyHomeLayout {
    fn default() -> Self {
        Self {
            resume_visible: true,
            separate_next_up: false,
        }
    }
}

impl LegacyHomeLayout {
    fn from_preferences(preferences: &BTreeMap<String, Value>) -> Self {
        let sections: Vec<&str> = (0..7)
            .filter_map(|index| {
                let key = format!("homesection{index}");
                let configured = preferences.get(&key).and_then(Value::as_str);
                let section = configured.unwrap_or_else(|| default_home_section(index));
                (section != "none").then_some(section)
            })
            .collect();
        Self {
            resume_visible: sections.contains(&"resume"),
            separate_next_up: sections.contains(&"nextup"),
        }
    }
}

fn default_home_section(index: usize) -> &'static str {
    match index {
        0 => "smalllibrarytiles",
        1 => "resume",
        2 => "resumeaudio",
        3 => "livetv",
        5 => "latestmedia",
        _ => "none",
    }
}

fn endpoint_is_unavailable(error: &EmbyError) -> bool {
    matches!(
        error,
        EmbyError::Api {
            status: 404 | 405 | 501,
            ..
        }
    )
}

enum ServerHomeError {
    Discovery(EmbyError),
    SectionItems(EmbyError),
}

impl ServerHomeError {
    fn into_inner(self) -> EmbyError {
        match self {
            Self::Discovery(error) | Self::SectionItems(error) => error,
        }
    }
}

impl EmbyClient {
    /// Returns the same semantic Continue Watching row selected by Emby Web.
    ///
    /// Endpoint/version details stay inside this adapter so callers never
    /// reconstruct server policy by merging generic item queries locally.
    pub async fn continue_watching(&self, limit: u32) -> Result<ItemsResult, EmbyError> {
        let limit = limit.max(1);
        let capabilities = self.server_capabilities().await;

        if capabilities.server_home_sections_enabled() {
            match self.server_home_continue_watching(limit).await {
                Ok(items) => {
                    tracing::debug!(
                        server_version = capabilities.version.as_deref().unwrap_or("unknown"),
                        strategy = "server_home_sections",
                        item_count = items.items.len(),
                        "resolved Emby Continue Watching"
                    );
                    return Ok(items);
                }
                Err(ServerHomeError::Discovery(error)) if endpoint_is_unavailable(&error) => {
                    capabilities.disable_server_home_sections();
                    tracing::warn!(
                        error = %error,
                        server_version = capabilities.version.as_deref().unwrap_or("unknown"),
                        fallback = "legacy_resume",
                        "Emby server HomeSections is unavailable"
                    );
                }
                Err(error) => return Err(error.into_inner()),
            }
        }

        let items = self.legacy_continue_watching(limit, capabilities).await?;
        tracing::debug!(
            server_version = capabilities.version.as_deref().unwrap_or("unknown"),
            strategy = "legacy_resume",
            item_count = items.items.len(),
            "resolved Emby Continue Watching"
        );
        Ok(items)
    }

    async fn server_capabilities(&self) -> &ServerCapabilities {
        self.compatibility
            .capabilities
            .get_or_init(|| async {
                let result: Result<PublicSystemInfo, EmbyError> = async {
                    decode(
                        self.request(reqwest::Method::GET, "System/Info/Public")
                            .send()
                            .await?,
                    )
                    .await
                }
                .await;
                let info = match result {
                    Ok(info) => info,
                    Err(error) => {
                        // Discovery is advisory. Cache an unknown capability
                        // set and probe feature boundaries once per session.
                        tracing::warn!(
                            error = %error,
                            strategy = "endpoint_probe",
                            "Emby version discovery failed; probing Continue Watching capabilities"
                        );
                        return ServerCapabilities::from_version(None);
                    }
                };
                if info
                    .version
                    .as_deref()
                    .and_then(ServerVersion::parse)
                    .is_none()
                {
                    tracing::warn!(
                        server_version = info.version.as_deref().unwrap_or("missing"),
                        "Emby returned an unrecognized server version; probing API capabilities"
                    );
                }
                ServerCapabilities::from_version(info.version)
            })
            .await
    }

    async fn server_home_continue_watching(
        &self,
        limit: u32,
    ) -> Result<ItemsResult, ServerHomeError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let sections: Vec<HomeSection> = decode(
            self.request(
                reqwest::Method::GET,
                &format!("Users/{user_id}/HomeSections"),
            )
            .query(&[("displayMode", DESKTOP_HOME_DISPLAY_MODE)])
            .send()
            .await
            .map_err(EmbyError::from)
            .map_err(ServerHomeError::Discovery)?,
        )
        .await
        .map_err(ServerHomeError::Discovery)?;
        let Some(section) = sections.iter().find(|section| {
            section
                .section_type
                .as_deref()
                .is_some_and(|value| value.eq_ignore_ascii_case("resume"))
        }) else {
            return Ok(ItemsResult::default());
        };
        let section_id = section
            .id
            .as_deref()
            .map(str::trim)
            .filter(|value| !value.is_empty())
            .ok_or_else(|| {
                ServerHomeError::Discovery(EmbyError::InvalidResponse(
                    "Continue Watching HomeSection did not contain an Id".to_owned(),
                ))
            })?;
        let path = format!("Users/{user_id}/Sections/{section_id}/Items");
        decode(
            self.request(reqwest::Method::GET, &path)
                .query(&home_item_parameters(limit))
                .send()
                .await
                .map_err(EmbyError::from)
                .map_err(ServerHomeError::SectionItems)?,
        )
        .await
        .map_err(ServerHomeError::SectionItems)
    }

    async fn legacy_continue_watching(
        &self,
        limit: u32,
        capabilities: &ServerCapabilities,
    ) -> Result<ItemsResult, EmbyError> {
        let layout = self.legacy_home_layout(capabilities).await?;
        if !layout.resume_visible {
            return Ok(ItemsResult::default());
        }

        let user_id = self.user_id.as_deref().unwrap_or_default();
        let path = format!("Users/{user_id}/Items/Resume");
        let mut parameters = home_item_parameters(limit);
        parameters.push(("Recursive", "true".to_owned()));
        parameters.push(("MediaTypes", "Video".to_owned()));
        if layout.separate_next_up {
            parameters.push(("IncludeNextUp", "false".to_owned()));
        }
        decode(
            self.request(reqwest::Method::GET, &path)
                .query(&parameters)
                .send()
                .await?,
        )
        .await
    }

    async fn legacy_home_layout(
        &self,
        capabilities: &ServerCapabilities,
    ) -> Result<LegacyHomeLayout, EmbyError> {
        let preferences = match capabilities.settings_api() {
            SETTINGS_API_FLAT => match self.flat_user_settings().await {
                Ok(preferences) => preferences,
                Err(error) if endpoint_is_unavailable(&error) => {
                    let preferences = self.display_preferences().await?;
                    capabilities.remember_settings_api(SETTINGS_API_DISPLAY_PREFERENCES);
                    preferences
                }
                Err(error) => return Err(error),
            },
            SETTINGS_API_DISPLAY_PREFERENCES => match self.display_preferences().await {
                Ok(preferences) => preferences,
                Err(error) if endpoint_is_unavailable(&error) => {
                    let preferences = self.flat_user_settings().await?;
                    capabilities.remember_settings_api(SETTINGS_API_FLAT);
                    preferences
                }
                Err(error) => return Err(error),
            },
            _ => match self.flat_user_settings().await {
                Ok(preferences) => {
                    capabilities.remember_settings_api(SETTINGS_API_FLAT);
                    preferences
                }
                Err(error) if endpoint_is_unavailable(&error) => {
                    let preferences = self.display_preferences().await?;
                    capabilities.remember_settings_api(SETTINGS_API_DISPLAY_PREFERENCES);
                    preferences
                }
                Err(error) => return Err(error),
            },
        };
        Ok(LegacyHomeLayout::from_preferences(&preferences))
    }

    async fn flat_user_settings(&self) -> Result<BTreeMap<String, Value>, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(reqwest::Method::GET, &format!("usersettings/{user_id}"))
                .send()
                .await?,
        )
        .await
    }

    async fn display_preferences(&self) -> Result<BTreeMap<String, Value>, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode::<LegacyDisplayPreferences>(
            self.request(reqwest::Method::GET, "DisplayPreferences/usersettings")
                .query(&[("userId", user_id), ("client", "emby")])
                .send()
                .await?,
        )
        .await
        .map(|preferences| preferences.custom_prefs)
    }
}

fn home_item_parameters(limit: u32) -> Vec<(&'static str, String)> {
    vec![
        ("Fields", BROWSE_FIELDS.to_owned()),
        ("ImageTypeLimit", "1".to_owned()),
        ("EnableImageTypes", "Primary,Thumb,Backdrop,Logo".to_owned()),
        ("EnableImages", "true".to_owned()),
        ("EnableUserData", "true".to_owned()),
        ("Limit", limit.to_string()),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_emby_four_part_versions_and_suffixes() {
        assert_eq!(
            ServerVersion::parse("4.10.0.4"),
            Some(SERVER_HOME_SECTIONS_VERSION)
        );
        assert_eq!(
            ServerVersion::parse("4.10.0.27-beta"),
            Some(ServerVersion([4, 10, 0, 27]))
        );
        assert_eq!(
            ServerVersion::parse("4.9"),
            Some(ServerVersion([4, 9, 0, 0]))
        );
        assert_eq!(ServerVersion::parse("unknown"), None);
    }

    #[test]
    fn capability_thresholds_are_centralized() {
        let legacy_preferences = ServerCapabilities::from_version(Some("4.9.0.22".to_owned()));
        assert_eq!(
            legacy_preferences.settings_api(),
            SETTINGS_API_DISPLAY_PREFERENCES
        );
        let flat_preferences = ServerCapabilities::from_version(Some("4.9.0.23".to_owned()));
        assert_eq!(flat_preferences.settings_api(), SETTINGS_API_FLAT);

        let previous = ServerCapabilities::from_version(Some("4.10.0.3".to_owned()));
        assert!(!previous.server_home_sections_enabled());
        assert_eq!(previous.settings_api(), SETTINGS_API_FLAT);

        let current = ServerCapabilities::from_version(Some("4.10.0.4".to_owned()));
        assert!(current.server_home_sections_enabled());
        current.disable_server_home_sections();
        assert!(!current.server_home_sections_enabled());

        let unknown = ServerCapabilities::from_version(None);
        assert!(unknown.server_home_sections_enabled());
        assert_eq!(unknown.settings_api(), SETTINGS_API_UNKNOWN);
    }

    #[test]
    fn legacy_layout_matches_emby_web_defaults_and_next_up_override() {
        let defaults = LegacyHomeLayout::from_preferences(&BTreeMap::new());
        assert!(defaults.resume_visible);
        assert!(!defaults.separate_next_up);

        let preferences = BTreeMap::from([
            ("homesection1".to_owned(), Value::String("none".to_owned())),
            (
                "homesection4".to_owned(),
                Value::String("nextup".to_owned()),
            ),
        ]);
        let customized = LegacyHomeLayout::from_preferences(&preferences);
        assert!(!customized.resume_visible);
        assert!(customized.separate_next_up);
    }
}
