use serde::{Deserialize, Serialize};

use crate::{Application, ApplicationError};

use super::{MediaOutcome, network_error, require_administrator};

pub type RefreshMetadataOutcome = MediaOutcome<RefreshMetadataResult>;
pub type ScanLibraryFilesOutcome = MediaOutcome<ScanLibraryFilesResult>;
pub type DeleteItemOutcome = MediaOutcome<DeleteItemResult>;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RefreshMetadataResult {
    refresh_started: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    source: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanLibraryFilesResult {
    refresh_started: bool,
}

#[derive(Clone, Debug, Serialize)]
pub struct DeleteItemResult {}

const LIBRARY_QUERY_KINDS: &[&str] = &["library", "activity", "favorites", "collection"];

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum MetadataRefreshMode {
    #[default]
    Missing,
    All,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RefreshMetadataRequest {
    #[serde(default)]
    pub mode: MetadataRefreshMode,
    #[serde(default)]
    pub replace_images: bool,
    #[serde(default)]
    pub source: Option<String>,
}

impl Application {
    pub fn refresh_metadata(
        &self,
        item_id: &str,
        request: &RefreshMetadataRequest,
    ) -> Result<RefreshMetadataOutcome, ApplicationError> {
        let source = validate_refresh_source(request.source.as_deref())?;
        let replace_all = matches!(request.mode, MetadataRefreshMode::All);
        let replace_images = request.replace_images;
        let client = self.active_client()?;
        let result = self.block_on(async {
            require_administrator(&client).await?;
            client
                .refresh_metadata(item_id, replace_all, replace_images)
                .await
                .map_err(network_error)?;
            Ok::<_, ApplicationError>(RefreshMetadataResult {
                refresh_started: true,
                source,
            })
        })?;
        Ok(MediaOutcome::invalidated(
            item_id,
            result,
            LIBRARY_QUERY_KINDS,
        ))
    }

    pub fn scan_library_files(
        &self,
        item_id: &str,
    ) -> Result<ScanLibraryFilesOutcome, ApplicationError> {
        let client = self.active_client()?;
        self.block_on(async {
            require_administrator(&client).await?;
            client
                .scan_library_files(item_id)
                .await
                .map_err(network_error)
        })?;
        // Prime the UI immediately while Emby's notification channel catches
        // up with the queued refresh.
        Ok(MediaOutcome::invalidated(
            item_id,
            ScanLibraryFilesResult {
                refresh_started: true,
            },
            LIBRARY_QUERY_KINDS,
        ))
    }

    pub fn delete_item(&self, item_id: &str) -> Result<DeleteItemOutcome, ApplicationError> {
        let client = self.active_client()?;
        self.block_on(async {
            let user = client.current_user().await.map_err(network_error)?;
            if !user.policy.is_administrator && !user.policy.enable_content_deletion {
                return Err(ApplicationError::permission_denied(
                    "the active Emby user cannot delete media",
                ));
            }
            client.delete_item(item_id).await.map_err(network_error)
        })?;
        if let Err(error) = self.evict_media_catalog_item(item_id) {
            tracing::warn!(item_id, error = %error, "deleted media could not be evicted from the local catalog");
        }
        Ok(MediaOutcome::invalidated(
            item_id,
            DeleteItemResult {},
            LIBRARY_QUERY_KINDS,
        ))
    }
}

fn validate_refresh_source(source: Option<&str>) -> Result<Option<String>, ApplicationError> {
    let Some(source) = source else {
        return Ok(None);
    };
    if source.trim().is_empty() {
        return Ok(None);
    }
    if source.chars().count() > 128 {
        return Err(ApplicationError::invalid(
            "metadata refresh source is too long",
        ));
    }
    // This is a caller-owned correlation label. Preserve it byte-for-byte after
    // validation rather than silently mapping it to a fixed enum.
    Ok(Some(source.to_owned()))
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::{RefreshMetadataResult, validate_refresh_source};
    use crate::{ApplicationErrorCode, media::MediaOutcome};

    #[test]
    fn refresh_source_is_bounded_and_preserves_caller_label() {
        assert_eq!(
            validate_refresh_source(Some(" dialog-1 ")).unwrap(),
            Some(" dialog-1 ".to_owned())
        );
        assert_eq!(validate_refresh_source(Some("  ")).unwrap(), None);
        assert_eq!(
            validate_refresh_source(Some(&"x".repeat(129)))
                .unwrap_err()
                .code(),
            ApplicationErrorCode::InvalidInput
        );
    }

    #[test]
    fn refresh_outcome_contract_omits_absent_source() {
        let outcome = MediaOutcome::invalidated(
            "episode-1",
            RefreshMetadataResult {
                refresh_started: true,
                source: None,
            },
            &["library"],
        );
        let encoded = serde_json::to_value(outcome).unwrap();
        assert_eq!(encoded["result"], json!({"refreshStarted": true}));
        assert!(encoded["result"].get("source").is_none());
    }
}
