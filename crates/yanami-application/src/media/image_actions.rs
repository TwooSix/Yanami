use serde::{Deserialize, Serialize};
use url::Url;

use crate::{
    Application, ApplicationError,
    images::{RemoteImageSearchQuery, read_local_image},
};

use super::{MediaOutcome, network_error, require_administrator};

pub type ImageEditorOutcome = MediaOutcome<ImageEditorResult>;
pub type ImageProvidersOutcome = MediaOutcome<ImageProvidersResult>;
pub type ImageSearchOutcome = MediaOutcome<ImageSearchResult>;
pub type ImageApplyOutcome = MediaOutcome<ImageMutationResult>;
pub type ImageUploadOutcome = MediaOutcome<ImageMutationResult>;
pub type ImageDeleteOutcome = MediaOutcome<ImageMutationResult>;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageEditorResult {
    pub(crate) id: String,
    pub(crate) title: String,
    pub(crate) item_type: Option<String>,
    pub(crate) images: Vec<EditableImage>,
    pub(crate) image_types: Vec<&'static str>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EditableImage {
    pub(crate) image_type: String,
    pub(crate) image_index: u32,
    pub(crate) image_tag: Option<String>,
    pub(crate) filename: Option<String>,
    pub(crate) width: Option<u32>,
    pub(crate) height: Option<u32>,
    pub(crate) size: Option<u64>,
    pub(crate) preview_url: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
pub struct ImageProvidersResult {
    pub(crate) id: String,
    pub(crate) providers: Vec<ImageProvider>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageProvider {
    pub(crate) name: String,
    pub(crate) supported_images: Vec<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageSearchResult {
    pub(crate) images: Vec<RemoteImage>,
    pub(crate) providers: Vec<String>,
    pub(crate) total_record_count: u64,
    pub(crate) start_index: u32,
    pub(crate) image_type: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoteImage {
    pub(crate) provider_name: String,
    pub(crate) image_url: String,
    pub(crate) preview_url: Option<String>,
    pub(crate) width: Option<u32>,
    pub(crate) height: Option<u32>,
    pub(crate) community_rating: Option<f64>,
    pub(crate) vote_count: Option<u64>,
    pub(crate) language: Option<String>,
    pub(crate) display_language: Option<String>,
    pub(crate) image_type: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageMutationResult {
    changed: bool,
    image_tag_settled: bool,
    image_type: String,
    image_index: Option<u32>,
    reconcile_required: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageSearchRequest {
    pub image_type: String,
    #[serde(default)]
    pub provider_name: Option<String>,
    #[serde(default)]
    pub include_all_languages: bool,
    #[serde(default)]
    pub enable_series_images: bool,
    #[serde(default)]
    pub start_index: u32,
    #[serde(default = "default_image_search_limit")]
    pub limit: u32,
}

const fn default_image_search_limit() -> u32 {
    30
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageApplyRequest {
    pub image_type: String,
    pub image_url: String,
    #[serde(default)]
    pub provider_name: Option<String>,
    #[serde(default)]
    pub image_index: Option<u32>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageUploadRequest {
    pub image_type: String,
    pub file_url: String,
    #[serde(default)]
    pub image_index: Option<u32>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImageDeleteRequest {
    pub image_type: String,
    #[serde(default)]
    pub image_index: Option<u32>,
}

impl Application {
    pub fn image_editor(&self, item_id: &str) -> Result<ImageEditorOutcome, ApplicationError> {
        let client = self.active_client()?;
        let result = self.image_editor_dto(&client, item_id)?;
        Ok(MediaOutcome::read(item_id, result))
    }

    pub fn image_providers(
        &self,
        item_id: &str,
    ) -> Result<ImageProvidersOutcome, ApplicationError> {
        let client = self.active_client()?;
        let result = self.image_providers_dto(&client, item_id)?;
        Ok(MediaOutcome::read(item_id, result))
    }

    pub fn image_search(
        &self,
        item_id: &str,
        request: &ImageSearchRequest,
    ) -> Result<ImageSearchOutcome, ApplicationError> {
        let image_type = validate_image_type(&request.image_type)?;
        let provider_name =
            optional_bounded_string(request.provider_name.as_deref(), "providerName", 128)?;
        let client = self.active_client()?;
        self.block_on(require_administrator(&client))?;
        let result = self.remote_image_search_dto(
            &client,
            item_id,
            RemoteImageSearchQuery {
                image_type,
                provider_name: provider_name.as_deref(),
                include_all_languages: request.include_all_languages,
                enable_series_images: request.enable_series_images,
                start_index: request.start_index.min(10_000),
                limit: request.limit.clamp(1, 60),
            },
        )?;
        Ok(MediaOutcome::read(item_id, result))
    }

    pub fn image_apply(
        &self,
        item_id: &str,
        request: &ImageApplyRequest,
    ) -> Result<ImageApplyOutcome, ApplicationError> {
        let image_type = validate_image_type(&request.image_type)?;
        let image_url = validate_remote_image_url(&request.image_url)?;
        let provider_name =
            optional_bounded_string(request.provider_name.as_deref(), "providerName", 128)?;
        let image_index = request.image_index;
        let client = self.active_client()?;
        let result = self.block_on(async {
            require_administrator(&client).await?;
            client
                .download_remote_image(
                    item_id,
                    image_type,
                    provider_name.as_deref(),
                    &image_url,
                    image_index,
                )
                .await
                .map_err(network_error)?;
            Ok::<_, ApplicationError>(image_mutation_result(image_type, image_index))
        })?;
        self.finish_image_mutation(item_id, image_type, result)
    }

    pub fn image_upload(
        &self,
        item_id: &str,
        request: &ImageUploadRequest,
    ) -> Result<ImageUploadOutcome, ApplicationError> {
        let image_type = validate_image_type(&request.image_type)?;
        let file_url = required_bounded_string(&request.file_url, "fileUrl", 8_192)?;
        let image_index = request.image_index;
        // Read and validate the local file before starting any server mutation.
        let (bytes, content_type) = read_local_image(&file_url)?;
        let client = self.active_client()?;
        let result = self.block_on(async {
            require_administrator(&client).await?;
            client
                .upload_item_image(item_id, image_type, image_index, content_type, &bytes)
                .await
                .map_err(network_error)?;
            Ok::<_, ApplicationError>(image_mutation_result(image_type, image_index))
        })?;
        self.finish_image_mutation(item_id, image_type, result)
    }

    pub fn image_delete(
        &self,
        item_id: &str,
        request: &ImageDeleteRequest,
    ) -> Result<ImageDeleteOutcome, ApplicationError> {
        let image_type = validate_image_type(&request.image_type)?;
        let image_index = request.image_index;
        let client = self.active_client()?;
        let result = self.block_on(async {
            require_administrator(&client).await?;
            client
                .delete_item_image(item_id, image_type, image_index)
                .await
                .map_err(network_error)?;
            Ok::<_, ApplicationError>(image_mutation_result(image_type, image_index))
        })?;
        self.finish_image_mutation(item_id, image_type, result)
    }

    fn finish_image_mutation(
        &self,
        item_id: &str,
        image_type: &str,
        result: ImageMutationResult,
    ) -> Result<MediaOutcome<ImageMutationResult>, ApplicationError> {
        self.bump_image_mutation_generation(item_id, image_type)?;
        Ok(MediaOutcome::invalidated(
            item_id,
            result,
            &["library", "activity", "favorites", "collection"],
        ))
    }
}

fn image_mutation_result(image_type: &str, image_index: Option<u32>) -> ImageMutationResult {
    ImageMutationResult {
        changed: true,
        image_tag_settled: false,
        image_type: image_type.to_owned(),
        image_index,
        // Reconciliation runs on the caller's interactive query lane and may
        // be discarded if the view closes.
        reconcile_required: true,
    }
}

pub(crate) fn validate_image_type(image_type: &str) -> Result<&'static str, ApplicationError> {
    match image_type.trim() {
        "Primary" => Ok("Primary"),
        "Backdrop" => Ok("Backdrop"),
        "Thumb" => Ok("Thumb"),
        "Banner" => Ok("Banner"),
        "Logo" => Ok("Logo"),
        "Art" => Ok("Art"),
        "Disc" => Ok("Disc"),
        _ => Err(ApplicationError::invalid("unsupported image type")),
    }
}

fn required_bounded_string(
    value: &str,
    key: &str,
    max_chars: usize,
) -> Result<String, ApplicationError> {
    let value = value.trim();
    if value.is_empty() {
        return Err(ApplicationError::invalid(format!("{key} is required")));
    }
    if value.chars().count() > max_chars {
        return Err(ApplicationError::invalid(format!("{key} is too long")));
    }
    Ok(value.to_owned())
}

fn optional_bounded_string(
    value: Option<&str>,
    key: &str,
    max_chars: usize,
) -> Result<Option<String>, ApplicationError> {
    let Some(value) = value.map(str::trim).filter(|value| !value.is_empty()) else {
        return Ok(None);
    };
    if value.chars().count() > max_chars {
        return Err(ApplicationError::invalid(format!("{key} is too long")));
    }
    Ok(Some(value.to_owned()))
}

fn validate_remote_image_url(value: &str) -> Result<String, ApplicationError> {
    let value = required_bounded_string(value, "imageUrl", 8_192)?;
    let parsed = Url::parse(&value)
        .map_err(|_| ApplicationError::invalid("imageUrl must be a valid URL"))?;
    if !matches!(parsed.scheme(), "http" | "https") {
        return Err(ApplicationError::invalid("imageUrl must use http or https"));
    }
    Ok(value)
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::{image_mutation_result, validate_image_type, validate_remote_image_url};
    use crate::{ApplicationErrorCode, media::MediaOutcome};

    #[test]
    fn rejects_unsafe_image_sources_with_typed_error() {
        assert_eq!(
            validate_remote_image_url("https://images.example.test/poster.jpg").unwrap(),
            "https://images.example.test/poster.jpg"
        );
        for image_url in [
            "file:///C:/private/poster.jpg",
            "javascript:alert(1)",
            "not a url",
        ] {
            let error = validate_remote_image_url(image_url).unwrap_err();
            assert_eq!(error.code(), ApplicationErrorCode::InvalidInput);
        }
        assert_eq!(validate_image_type("Primary").unwrap(), "Primary");
        assert_eq!(
            validate_image_type("Avatar").unwrap_err().code(),
            ApplicationErrorCode::InvalidInput
        );
    }

    #[test]
    fn image_mutation_outcome_contract_is_fully_typed() {
        let outcome = MediaOutcome::invalidated(
            "episode-1",
            image_mutation_result("Primary", None),
            &["library"],
        );
        let encoded = serde_json::to_value(outcome).unwrap();
        assert_eq!(
            encoded["result"],
            json!({
                "changed": true,
                "imageTagSettled": false,
                "imageType": "Primary",
                "imageIndex": null,
                "reconcileRequired": true
            })
        );
        assert_eq!(encoded["invalidation"]["queryKinds"], json!(["library"]));
    }
}
