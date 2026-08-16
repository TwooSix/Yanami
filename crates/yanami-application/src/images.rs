use std::{
    collections::hash_map::DefaultHasher,
    fs,
    hash::{Hash, Hasher},
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, SystemTime},
};

use url::Url;
use uuid::Uuid;
use yanami_emby::{BaseItem, EmbyClient, RemoteImageQuery};

use crate::{
    Application, ApplicationError, BackgroundTaskScope, display_error,
    media::{
        image_actions::{
            EditableImage, ImageEditorResult, ImageProvider, ImageProvidersResult,
            ImageSearchResult, RemoteImage,
        },
        require_administrator,
    },
    presentation::{ImagePurpose, image_reference, safe_cache_component},
};

pub(crate) const IMAGE_DOWNLOAD_CONCURRENCY: usize = 12;
pub(crate) const IMAGE_CACHE_MAX_BYTES: u64 = 512 * 1024 * 1024;
pub(crate) const IMAGE_EDITOR_CACHE_MAX_BYTES: u64 = 128 * 1024 * 1024;
pub(crate) const IMAGE_CACHE_MAX_AGE: Duration = Duration::from_secs(30 * 24 * 60 * 60);
pub(crate) const IMAGE_EDITOR_CACHE_MAX_AGE: Duration = Duration::from_secs(7 * 24 * 60 * 60);
const MAX_LOCAL_IMAGE_BYTES: u64 = 25 * 1024 * 1024;
const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";

#[derive(Clone, Copy)]
pub(crate) struct RemoteImageSearchQuery<'a> {
    pub(crate) image_type: &'a str,
    pub(crate) provider_name: Option<&'a str>,
    pub(crate) include_all_languages: bool,
    pub(crate) enable_series_images: bool,
    pub(crate) start_index: u32,
    pub(crate) limit: u32,
}

struct ActiveDownloadGuard {
    downloads: Arc<std::sync::Mutex<std::collections::HashSet<PathBuf>>>,
    path: PathBuf,
}

impl ActiveDownloadGuard {
    fn new(
        downloads: Arc<std::sync::Mutex<std::collections::HashSet<PathBuf>>>,
        path: PathBuf,
    ) -> Self {
        Self { downloads, path }
    }
}

impl Drop for ActiveDownloadGuard {
    fn drop(&mut self) {
        if let Ok(mut downloads) = self.downloads.lock() {
            downloads.remove(&self.path);
        }
    }
}

impl Application {
    pub(crate) fn image_editor_dto(
        &self,
        client: &EmbyClient,
        item_id: &str,
    ) -> Result<ImageEditorResult, ApplicationError> {
        let (item, images) = self.block_on(async {
            let (administrator, item, images) = tokio::join!(
                // Authorization and current-image reads are independent
                // requests. Remote provider discovery deliberately runs as a
                // separate application request so a slow or failing plug-in cannot
                // delay the editor's first useful paint.
                require_administrator(client),
                // The unrestricted item DTO carries the complete ImageTags
                // dictionary.  The regular playback-oriented `item` request
                // deliberately limits image types and is therefore not a
                // reliable cache version source for the image editor.
                client.raw_item(item_id),
                client.item_images(item_id),
            );
            administrator?;
            Ok::<_, ApplicationError>((
                item.map_err(ApplicationError::from)?,
                images.map_err(ApplicationError::from)?,
            ))
        })?;
        let item: BaseItem = serde_json::from_value(item)
            .map_err(|error| ApplicationError::unsupported(error.to_string()))?;
        let preview_urls = self
            .cache_item_image_previews(client, item_id, &item, &images)
            .map_err(ApplicationError::internal)?;
        let images = images
            .iter()
            .zip(preview_urls)
            .map(|(image, preview_url)| {
                let image_tag = current_item_image_tag(&item, image);
                EditableImage {
                    image_type: image.image_type.clone(),
                    image_index: image.image_index,
                    image_tag,
                    filename: image.filename.clone(),
                    width: image.width,
                    height: image.height,
                    size: image.size,
                    preview_url,
                }
            })
            .collect();
        Ok(ImageEditorResult {
            id: item.id,
            title: item.name,
            item_type: item.item_type,
            images,
            image_types: vec![
                "Primary", "Backdrop", "Thumb", "Banner", "Logo", "Art", "Disc",
            ],
        })
    }

    pub(crate) fn image_providers_dto(
        &self,
        client: &EmbyClient,
        item_id: &str,
    ) -> Result<ImageProvidersResult, ApplicationError> {
        let providers = self.block_on(async {
            let (administrator, providers) = tokio::join!(
                require_administrator(client),
                client.remote_image_providers(item_id),
            );
            administrator?;
            providers.map_err(ApplicationError::from)
        })?;
        let providers = providers
            .into_iter()
            .filter(|provider| !provider.name.trim().is_empty())
            .map(|provider| ImageProvider {
                name: provider.name,
                supported_images: provider.supported_images,
            })
            .collect();
        Ok(ImageProvidersResult {
            id: item_id.to_owned(),
            providers,
        })
    }

    pub(crate) fn bump_image_mutation_generation(
        &self,
        item_id: &str,
        image_type: &str,
    ) -> Result<(), ApplicationError> {
        self.cancel_image_tasks(item_id, image_type);
        let key = (item_id.to_owned(), image_type.to_ascii_lowercase());
        let mut generations = self.image_mutation_generations.lock().map_err(|_| {
            ApplicationError::internal("image mutation generation lock is poisoned")
        })?;
        let generation = generations.entry(key).or_default();
        *generation = generation.saturating_add(1);
        Ok(())
    }

    pub(crate) fn remote_image_search_dto(
        &self,
        client: &EmbyClient,
        item_id: &str,
        query: RemoteImageSearchQuery<'_>,
    ) -> Result<ImageSearchResult, ApplicationError> {
        let result = self.block_on_emby(client.remote_images(
            item_id,
            RemoteImageQuery {
                image_type: query.image_type,
                provider_name: query.provider_name,
                include_all_languages: query.include_all_languages,
                enable_series_images: query.enable_series_images,
                start_index: query.start_index,
                limit: query.limit,
            },
        ))?;
        let preview_urls = self
            .cache_remote_image_previews(client, item_id, query.image_type, &result.images)
            .map_err(ApplicationError::internal)?;
        let images = result
            .images
            .iter()
            .zip(preview_urls)
            .map(|(image, preview_url)| RemoteImage {
                provider_name: image.provider_name.clone(),
                image_url: image.url.clone(),
                preview_url,
                width: image.width,
                height: image.height,
                community_rating: image.community_rating,
                vote_count: image.vote_count,
                language: image.language.clone(),
                display_language: image.display_language.clone(),
                image_type: image.image_type.clone(),
            })
            .collect();
        Ok(ImageSearchResult {
            images,
            providers: result.providers,
            total_record_count: result.total_record_count,
            start_index: query.start_index,
            image_type: query.image_type.to_owned(),
        })
    }

    fn cache_item_image_previews(
        &self,
        client: &EmbyClient,
        item_id: &str,
        item: &BaseItem,
        images: &[yanami_emby::ImageInfo],
    ) -> Result<Vec<Option<String>>, String> {
        let cache_root = self.data_dir.join("cache");
        let cache_dir = self.image_editor_cache_dir(client)?;
        let generations = self
            .image_mutation_generations
            .lock()
            .map_err(|_| "image mutation generation lock is poisoned")?;
        let mut urls = Vec::with_capacity(images.len());
        for image in images {
            let image_tag = current_item_image_tag(item, image);
            let generation = generations
                .get(&(item_id.to_owned(), image.image_type.to_ascii_lowercase()))
                .copied()
                .unwrap_or_default();
            let cache_key =
                item_image_preview_cache_key(item_id, image, image_tag.as_deref(), generation);
            let cache_path = cache_dir.join(format!("current-{cache_key:016x}.jpg"));
            self.schedule_item_image_preview(
                client.clone(),
                item_id.to_owned(),
                image.image_type.clone(),
                image.image_index,
                image_tag,
                cache_path.clone(),
            )?;
            urls.push(Some(async_image_url(&cache_root, &cache_path)?));
        }
        Ok(urls)
    }

    fn cache_remote_image_previews(
        &self,
        client: &EmbyClient,
        item_id: &str,
        image_type: &str,
        images: &[yanami_emby::RemoteImageInfo],
    ) -> Result<Vec<Option<String>>, String> {
        let cache_root = self.data_dir.join("cache");
        let cache_dir = self.image_editor_cache_dir(client)?;
        let mut urls = Vec::with_capacity(images.len());
        for image in images {
            let source_url = image.thumbnail_url.as_deref().unwrap_or(&image.url);
            let mut hasher = DefaultHasher::new();
            source_url.hash(&mut hasher);
            let cache_path = cache_dir.join(format!("remote-{:016x}.jpg", hasher.finish()));
            self.schedule_remote_image_preview(
                client.clone(),
                item_id.to_owned(),
                image_type,
                source_url.to_owned(),
                cache_path.clone(),
            )?;
            urls.push(Some(async_image_url(&cache_root, &cache_path)?));
        }
        Ok(urls)
    }

    fn image_editor_cache_dir(&self, client: &EmbyClient) -> Result<PathBuf, String> {
        let cache_dir = self
            .data_dir
            .join("cache")
            .join("image-editor")
            .join(client.profile().local_id.to_string());
        fs::create_dir_all(&cache_dir).map_err(display_error)?;
        Ok(cache_dir)
    }

    fn schedule_item_image_preview(
        &self,
        client: EmbyClient,
        item_id: String,
        image_type: String,
        image_index: u32,
        image_tag: Option<String>,
        cache_path: PathBuf,
    ) -> Result<(), String> {
        if cache_path.is_file()
            || !self
                .image_downloads
                .lock()
                .map_err(|_| "image download cache is poisoned")?
                .insert(cache_path.clone())
        {
            return Ok(());
        }
        let active_downloads = Arc::clone(&self.image_downloads);
        let slots = Arc::clone(&self.image_download_slots);
        let scope = BackgroundTaskScope::Image {
            item_id: item_id.clone(),
            image_type: image_type.to_ascii_lowercase(),
        };
        let download_guard = ActiveDownloadGuard::new(active_downloads, cache_path.clone());
        self.spawn_background_task(scope, async move {
            let _download_guard = download_guard;
            let bytes = if let Ok(_permit) = slots.acquire_owned().await {
                client
                    .item_image_preview(
                        &item_id,
                        &image_type,
                        image_index,
                        image_tag.as_deref(),
                        720,
                    )
                    .await
                    .ok()
            } else {
                None
            };
            write_cached_image(&cache_path, bytes.as_deref());
        })?;
        Ok(())
    }

    fn schedule_remote_image_preview(
        &self,
        client: EmbyClient,
        item_id: String,
        image_type: &str,
        source_url: String,
        cache_path: PathBuf,
    ) -> Result<(), String> {
        if cache_path.is_file()
            || !self
                .image_downloads
                .lock()
                .map_err(|_| "image download cache is poisoned")?
                .insert(cache_path.clone())
        {
            return Ok(());
        }
        let active_downloads = Arc::clone(&self.image_downloads);
        let slots = Arc::clone(&self.image_download_slots);
        let scope = BackgroundTaskScope::Image {
            item_id,
            image_type: image_type.to_ascii_lowercase(),
        };
        let download_guard = ActiveDownloadGuard::new(active_downloads, cache_path.clone());
        self.spawn_background_task(scope, async move {
            let _download_guard = download_guard;
            let bytes = if let Ok(_permit) = slots.acquire_owned().await {
                client.remote_image_preview(&source_url).await.ok()
            } else {
                None
            };
            write_cached_image(&cache_path, bytes.as_deref());
        })?;
        Ok(())
    }

    #[allow(clippy::too_many_lines)]
    pub(crate) fn cache_images(
        &self,
        client: &EmbyClient,
        items: &[BaseItem],
        purpose: ImagePurpose,
    ) -> Result<Vec<Option<String>>, String> {
        let cache_root = self.data_dir.join("cache");
        let cache_dir = cache_root
            .join("images")
            .join(client.profile().local_id.to_string());
        fs::create_dir_all(&cache_dir).map_err(display_error)?;

        let generations = self
            .image_mutation_generations
            .lock()
            .map_err(|_| "image mutation generation lock is poisoned")?;
        let mut cache_paths = Vec::with_capacity(items.len());
        let mut downloads = Vec::new();
        for item in items {
            let Some((image_item_id, image_type, image_tag)) = image_reference(item, purpose)
            else {
                cache_paths.push(None);
                continue;
            };
            let normalized_image_type = image_type.to_ascii_lowercase();
            let generation = generations
                .get(&(image_item_id.to_owned(), normalized_image_type.clone()))
                .copied()
                .unwrap_or_default();
            let cache_name = if generation == 0 {
                format!(
                    "{}-{}-{}.jpg",
                    safe_cache_component(image_item_id),
                    normalized_image_type,
                    safe_cache_component(image_tag)
                )
            } else {
                format!(
                    "{}-{}-{}-m{generation}.jpg",
                    safe_cache_component(image_item_id),
                    normalized_image_type,
                    safe_cache_component(image_tag)
                )
            };
            let cache_path = cache_dir.join(cache_name);
            if !cache_path.is_file()
                && self
                    .image_downloads
                    .lock()
                    .map_err(|_| "image download cache is poisoned")?
                    .insert(cache_path.clone())
            {
                downloads.push((
                    image_item_id.to_owned(),
                    image_type.to_owned(),
                    image_tag.to_owned(),
                    cache_path.clone(),
                ));
            }
            cache_paths.push(Some(cache_path));
        }

        for (image_item_id, image_type, image_tag, cache_path) in downloads {
            let client = client.clone();
            let active_downloads = Arc::clone(&self.image_downloads);
            let slots = Arc::clone(&self.image_download_slots);
            let scope = BackgroundTaskScope::Image {
                item_id: image_item_id.clone(),
                image_type: image_type.to_ascii_lowercase(),
            };
            let download_guard = ActiveDownloadGuard::new(active_downloads, cache_path.clone());
            self.spawn_background_task(scope, async move {
                let _download_guard = download_guard;
                if let Ok(_permit) = slots.acquire_owned().await {
                    let mut downloaded = None;
                    for attempt in 0..2 {
                        match client
                            .image(&image_item_id, &image_type, &image_tag, 720)
                            .await
                        {
                            Ok(bytes) => {
                                downloaded = Some(bytes);
                                break;
                            }
                            Err(_) if attempt == 0 => {
                                tokio::time::sleep(Duration::from_millis(250)).await;
                            }
                            Err(_) => break,
                        }
                    }
                    if !cache_path.is_file()
                        && let Some(bytes) = downloaded
                    {
                        let extension = cache_path
                            .extension()
                            .and_then(|value| value.to_str())
                            .unwrap_or("image");
                        let temporary_path = cache_path
                            .with_extension(format!("{extension}.{}.part", Uuid::new_v4()));
                        if fs::write(&temporary_path, bytes).is_ok() {
                            if cache_path.is_file()
                                || fs::rename(&temporary_path, &cache_path).is_err()
                            {
                                let _ = fs::remove_file(&temporary_path);
                            }
                        } else {
                            let _ = fs::remove_file(&temporary_path);
                        }
                    }
                }
            })?;
        }

        cache_paths
            .into_iter()
            .map(|path| {
                path.map(|value| async_image_url(&cache_root, &value))
                    .transpose()
            })
            .collect()
    }
}

pub(crate) fn read_local_image(
    file_url: &str,
) -> Result<(Vec<u8>, &'static str), ApplicationError> {
    let url = Url::parse(file_url)
        .map_err(|_| ApplicationError::invalid("the selected image path is invalid"))?;
    if url.scheme() != "file" {
        return Err(ApplicationError::invalid(
            "only local image files can be uploaded",
        ));
    }
    let path = url
        .to_file_path()
        .map_err(|()| ApplicationError::invalid("the selected image path is invalid"))?;
    let metadata = fs::metadata(&path).map_err(|error| {
        ApplicationError::new(crate::ApplicationErrorCode::Storage, error.to_string())
    })?;
    if metadata.len() == 0 {
        return Err(ApplicationError::invalid("the selected image is empty"));
    }
    if metadata.len() > MAX_LOCAL_IMAGE_BYTES {
        return Err(ApplicationError::invalid(
            "the selected image exceeds the 25 MB limit",
        ));
    }
    let bytes = fs::read(&path).map_err(|error| {
        ApplicationError::new(crate::ApplicationErrorCode::Storage, error.to_string())
    })?;
    let content_type = if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        "image/jpeg"
    } else if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        "image/png"
    } else if bytes.len() >= 12 && bytes.starts_with(b"RIFF") && &bytes[8..12] == b"WEBP" {
        "image/webp"
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        "image/gif"
    } else {
        return Err(ApplicationError::invalid(
            "only JPEG, PNG, WebP, and GIF images are supported",
        ));
    };
    Ok((bytes, content_type))
}

pub(crate) fn current_item_image_tag(
    item: &BaseItem,
    image: &yanami_emby::ImageInfo,
) -> Option<String> {
    image
        .image_tag
        .as_deref()
        .map(str::trim)
        .filter(|tag| !tag.is_empty())
        .map(str::to_owned)
        .or_else(|| {
            image
                .image_type
                .eq_ignore_ascii_case("Backdrop")
                .then(|| {
                    item.backdrop_image_tags
                        .get(image.image_index as usize)
                        .map(String::as_str)
                })
                .flatten()
                .map(str::trim)
                .filter(|tag| !tag.is_empty())
                .map(str::to_owned)
        })
        .or_else(|| {
            item.image_tags
                .iter()
                .find(|(image_type, _)| image_type.eq_ignore_ascii_case(&image.image_type))
                .map(|(_, tag)| tag.trim())
                .filter(|tag| !tag.is_empty())
                .map(str::to_owned)
        })
        .or_else(|| {
            image
                .image_type
                .eq_ignore_ascii_case("Primary")
                .then_some(item.primary_image_tag.as_deref())
                .flatten()
                .map(str::trim)
                .filter(|tag| !tag.is_empty())
                .map(str::to_owned)
        })
}

pub(crate) fn item_image_preview_cache_key(
    item_id: &str,
    image: &yanami_emby::ImageInfo,
    image_tag: Option<&str>,
    mutation_generation: u64,
) -> u64 {
    let mut hasher = DefaultHasher::new();
    item_id.hash(&mut hasher);
    image.image_type.hash(&mut hasher);
    image.image_index.hash(&mut hasher);
    image_tag.hash(&mut hasher);
    mutation_generation.hash(&mut hasher);
    // Retain the legacy identity fields as a fallback for servers that do not
    // expose an image tag for a particular type.
    image.filename.hash(&mut hasher);
    image.size.hash(&mut hasher);
    hasher.finish()
}

pub(crate) fn prune_cache_tree(
    root: &Path,
    maximum_bytes: u64,
    maximum_age: Duration,
) -> std::io::Result<()> {
    if !root.is_dir() {
        return Ok(());
    }
    let mut pending = vec![root.to_owned()];
    let mut files = Vec::new();
    while let Some(directory) = pending.pop() {
        for entry in fs::read_dir(directory)? {
            let entry = entry?;
            let file_type = entry.file_type()?;
            if file_type.is_dir() {
                pending.push(entry.path());
            } else if file_type.is_file() {
                let metadata = entry.metadata()?;
                files.push((
                    entry.path(),
                    metadata.len(),
                    metadata.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                ));
            }
        }
    }

    let now = SystemTime::now();
    let mut retained = Vec::with_capacity(files.len());
    let mut retained_bytes = 0_u64;
    for (path, bytes, modified) in files {
        if now.duration_since(modified).unwrap_or_default() > maximum_age {
            let _ = fs::remove_file(path);
        } else {
            retained_bytes = retained_bytes.saturating_add(bytes);
            retained.push((path, bytes, modified));
        }
    }
    retained.sort_by_key(|(_, _, modified)| *modified);
    for (path, bytes, _) in retained {
        if retained_bytes <= maximum_bytes {
            break;
        }
        if fs::remove_file(path).is_ok() {
            retained_bytes = retained_bytes.saturating_sub(bytes);
        }
    }
    Ok(())
}

fn write_cached_image(cache_path: &Path, bytes: Option<&[u8]>) {
    let Some(bytes) = bytes else {
        return;
    };
    if cache_path.is_file() {
        return;
    }
    let extension = cache_path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("image");
    let temporary_path = cache_path.with_extension(format!("{extension}.{}.part", Uuid::new_v4()));
    if fs::write(&temporary_path, bytes).is_ok() {
        if cache_path.is_file() || fs::rename(&temporary_path, cache_path).is_err() {
            let _ = fs::remove_file(&temporary_path);
        }
    } else {
        let _ = fs::remove_file(&temporary_path);
    }
}

/// Encodes a cache-relative path as an opaque image-provider identifier. The
/// key deliberately excludes the absolute application-data path (which may
/// contain a user's account name), and the Qt provider independently rejects
/// traversal before reading the cache entry.
pub(crate) fn async_image_url(cache_root: &Path, cache_path: &Path) -> Result<String, String> {
    let relative = cache_path
        .strip_prefix(cache_root)
        .map_err(|_| "image cache path escaped the cache root")?;
    if relative
        .components()
        .any(|component| !matches!(component, std::path::Component::Normal(_)))
    {
        return Err("image cache path contains traversal".to_owned());
    }
    let relative = relative
        .to_str()
        .ok_or("image cache path is not valid UTF-8")?
        .replace('\\', "/");
    if relative.is_empty() {
        return Err("image cache path does not identify a file".to_owned());
    }

    let mut encoded = String::with_capacity(relative.len() * 2);
    for byte in relative.bytes() {
        encoded.push(HEX_DIGITS[usize::from(byte >> 4)] as char);
        encoded.push(HEX_DIGITS[usize::from(byte & 0x0f)] as char);
    }
    Ok(format!("image://yanami/v1-{encoded}"))
}

#[cfg(test)]
mod tests {
    use std::{collections::BTreeMap, fs, time::Duration};

    use url::Url;
    use yanami_emby::{BaseItem, ImageInfo};

    use super::{
        async_image_url, current_item_image_tag, item_image_preview_cache_key, prune_cache_tree,
        read_local_image,
    };

    #[test]
    fn image_preview_key_changes_with_server_tag_and_local_generation() {
        let image = ImageInfo {
            image_type: "Primary".to_owned(),
            image_index: 0,
            filename: Some("poster.jpg".to_owned()),
            ..ImageInfo::default()
        };
        let mut item = BaseItem {
            id: "series-1".to_owned(),
            name: "Series".to_owned(),
            image_tags: BTreeMap::from([("Primary".to_owned(), "tag-v1".to_owned())]),
            ..BaseItem::default()
        };

        let first_tag = current_item_image_tag(&item, &image);
        let first = item_image_preview_cache_key("series-1", &image, first_tag.as_deref(), 0);
        item.image_tags
            .insert("Primary".to_owned(), "tag-v2".to_owned());
        let second_tag = current_item_image_tag(&item, &image);
        let second = item_image_preview_cache_key("series-1", &image, second_tag.as_deref(), 0);
        let third = item_image_preview_cache_key("series-1", &image, second_tag.as_deref(), 1);

        assert_ne!(first, second);
        assert_ne!(second, third);
    }

    #[test]
    fn image_cache_and_upload_boundaries_are_enforced() {
        let temp = tempfile::tempdir().unwrap();
        for index in 0..3 {
            fs::write(temp.path().join(format!("{index}.jpg")), [index as u8; 8]).unwrap();
        }
        prune_cache_tree(temp.path(), 16, Duration::from_secs(60)).unwrap();
        let retained_bytes: u64 = fs::read_dir(temp.path())
            .unwrap()
            .map(|entry| entry.unwrap().metadata().unwrap().len())
            .sum();
        assert!(retained_bytes <= 16);

        let image_path = temp.path().join("cover.png");
        fs::write(&image_path, b"\x89PNG\r\n\x1a\nsmall-test-payload").unwrap();
        let image_url = Url::from_file_path(&image_path).unwrap();
        let (bytes, content_type) = read_local_image(image_url.as_str()).unwrap();
        assert_eq!(content_type, "image/png");
        assert_eq!(bytes, b"\x89PNG\r\n\x1a\nsmall-test-payload");
        assert!(read_local_image("https://images.example.test/poster.jpg").is_err());

        let nested = temp.path().join("scope").join("preview.jpg");
        assert!(
            async_image_url(temp.path(), &nested)
                .unwrap()
                .starts_with("image://yanami/v1-")
        );
        assert!(async_image_url(&nested, temp.path()).is_err());
    }
}
