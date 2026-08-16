use base64::{Engine as _, engine::general_purpose::STANDARD};

use crate::{
    client::{EmbyClient, EmbyError, RemoteImageQuery},
    models::{ImageInfo, ImageProviderInfo, RemoteImageResult},
    transport::{decode, ensure_success, response_bytes},
};

impl EmbyClient {
    pub async fn item_images(&self, item_id: &str) -> Result<Vec<ImageInfo>, EmbyError> {
        decode(
            self.request(reqwest::Method::GET, &format!("Items/{item_id}/Images"))
                .send()
                .await?,
        )
        .await
    }

    pub async fn remote_image_providers(
        &self,
        item_id: &str,
    ) -> Result<Vec<ImageProviderInfo>, EmbyError> {
        decode(
            self.request(
                reqwest::Method::GET,
                &format!("Items/{item_id}/RemoteImages/Providers"),
            )
            .send()
            .await?,
        )
        .await
    }

    pub async fn remote_images(
        &self,
        item_id: &str,
        query: RemoteImageQuery<'_>,
    ) -> Result<RemoteImageResult, EmbyError> {
        let mut request = self
            .request(
                reqwest::Method::GET,
                &format!("Items/{item_id}/RemoteImages"),
            )
            .query(&[
                ("Type", query.image_type.to_owned()),
                ("StartIndex", query.start_index.to_string()),
                ("Limit", query.limit.clamp(1, 60).to_string()),
                (
                    "IncludeAllLanguages",
                    query.include_all_languages.to_string(),
                ),
                ("EnableSeriesImages", query.enable_series_images.to_string()),
            ]);
        if let Some(provider_name) = query.provider_name.filter(|value| !value.trim().is_empty()) {
            request = request.query(&[("ProviderName", provider_name)]);
        }
        decode(request.send().await?).await
    }

    pub async fn download_remote_image(
        &self,
        item_id: &str,
        image_type: &str,
        provider_name: Option<&str>,
        image_url: &str,
        image_index: Option<u32>,
    ) -> Result<(), EmbyError> {
        let mut request = self
            .request(
                reqwest::Method::POST,
                &format!("Items/{item_id}/RemoteImages/Download"),
            )
            .query(&[
                ("Type", image_type.to_owned()),
                ("ImageUrl", image_url.to_owned()),
            ]);
        if let Some(provider_name) = provider_name.filter(|value| !value.trim().is_empty()) {
            request = request.query(&[("ProviderName", provider_name)]);
        }
        if let Some(image_index) = image_index {
            request = request.query(&[("ImageIndex", image_index.to_string())]);
        }
        ensure_success(request.send().await?).await
    }

    pub async fn upload_item_image(
        &self,
        item_id: &str,
        image_type: &str,
        image_index: Option<u32>,
        content_type: &str,
        bytes: &[u8],
    ) -> Result<(), EmbyError> {
        let path = image_index.map_or_else(
            || format!("Items/{item_id}/Images/{image_type}"),
            |index| format!("Items/{item_id}/Images/{image_type}/{index}"),
        );
        let request = self
            .request(reqwest::Method::POST, &path)
            .header(reqwest::header::CONTENT_TYPE, content_type)
            .body(STANDARD.encode(bytes));
        ensure_success(request.send().await?).await
    }

    pub async fn delete_item_image(
        &self,
        item_id: &str,
        image_type: &str,
        image_index: Option<u32>,
    ) -> Result<(), EmbyError> {
        let path = image_index.map_or_else(
            || format!("Items/{item_id}/Images/{image_type}"),
            |index| format!("Items/{item_id}/Images/{image_type}/{index}"),
        );
        ensure_success(self.request(reqwest::Method::DELETE, &path).send().await?).await
    }

    pub async fn item_image_preview(
        &self,
        item_id: &str,
        image_type: &str,
        image_index: u32,
        image_tag: Option<&str>,
        max_height: u32,
    ) -> Result<Vec<u8>, EmbyError> {
        let path = if image_index == 0 && image_type != "Backdrop" {
            format!("Items/{item_id}/Images/{image_type}")
        } else {
            format!("Items/{item_id}/Images/{image_type}/{image_index}")
        };
        let mut request = self.request(reqwest::Method::GET, &path).query(&[
            ("MaxHeight", max_height.to_string()),
            ("Quality", "88".to_owned()),
            ("Format", "jpg".to_owned()),
        ]);
        if let Some(image_tag) = image_tag.filter(|value| !value.trim().is_empty()) {
            request = request.query(&[("Tag", image_tag)]);
        }
        response_bytes(request.send().await?).await
    }

    pub async fn remote_image_preview(&self, image_url: &str) -> Result<Vec<u8>, EmbyError> {
        response_bytes(
            self.request(reqwest::Method::GET, "Images/Remote")
                .query(&[("ImageUrl", image_url)])
                .send()
                .await?,
        )
        .await
    }

    pub async fn image(
        &self,
        item_id: &str,
        image_type: &str,
        tag: &str,
        max_height: u32,
    ) -> Result<Vec<u8>, EmbyError> {
        let image_path = if image_type == "Backdrop" {
            format!("Items/{item_id}/Images/Backdrop/0")
        } else {
            format!("Items/{item_id}/Images/{image_type}")
        };
        let response = self
            .request(reqwest::Method::GET, &image_path)
            .query(&[
                ("Tag", tag.to_owned()),
                ("MaxHeight", max_height.to_string()),
                ("Quality", "88".to_owned()),
                ("Format", "jpg".to_owned()),
            ])
            .send()
            .await?;
        response_bytes(response).await
    }
}
