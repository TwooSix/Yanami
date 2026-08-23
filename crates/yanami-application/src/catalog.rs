use std::collections::BTreeMap;

use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::{Map, Value, json};
use yanami_emby::{BaseItem, ItemQuery};

use crate::{
    Application, ApplicationError, presentation,
    presentation::{
        ImagePurpose, card_subtitle, is_playable_item, is_supported_view, library_view_json,
        media_card_json, select_episode,
    },
};

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LibraryOutcome {
    entities: BTreeMap<String, CatalogEntity>,
    queries: CatalogQueries,
    user_capabilities: UserCapabilities,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FavoritesOutcome {
    entities: BTreeMap<String, CatalogEntity>,
    queries: CatalogQueries,
    total_record_count: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ActivityOutcome {
    entities: BTreeMap<String, CatalogEntity>,
    queries: CatalogQueries,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CollectionOutcome {
    entities: BTreeMap<String, CatalogEntity>,
    queries: CatalogQueries,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(untagged)]
pub enum CatalogEntity {
    Media(CatalogMediaEntity),
    View(CatalogViewEntity),
    Parent(CatalogParentEntity),
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CatalogMediaEntity {
    id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    title: Option<String>,
    item_type: Option<String>,
    series_id: Option<String>,
    series_title: Option<String>,
    season_id: Option<String>,
    image_url: Option<String>,
    resume_ticks: u64,
    played: bool,
    favorite: bool,
    progress: Option<f64>,
    unplayed_count: Option<u32>,
    child_count: Option<u32>,
    latest_episode_subtitle: Option<String>,
    overview: Option<String>,
    date_created: Option<String>,
    release_date: Option<String>,
    production_year: Option<i32>,
    updated_at: Option<String>,
    source_updated_at: Option<String>,
    source_version: String,
    provider_ids: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CatalogViewEntity {
    id: String,
    title: String,
    item_type: Option<String>,
    collection_type: Option<String>,
    image_url: Option<String>,
    source_updated_at: Option<String>,
    source_version: String,
    provider_ids: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CatalogParentEntity {
    id: String,
    title: String,
    item_type: Option<String>,
    collection_type: Option<String>,
    series_id: Option<String>,
    child_count: Option<u32>,
    unplayed_count: Option<u32>,
    production_year: Option<i32>,
    overview: Option<String>,
    image_url: Option<String>,
    backdrop_url: Option<String>,
    can_edit_items: Option<bool>,
    can_delete: Option<bool>,
    source_updated_at: Option<String>,
    source_version: String,
    provider_ids: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct CatalogQueries {
    #[serde(skip_serializing_if = "Option::is_none")]
    library: Option<CatalogQuery>,
    #[serde(skip_serializing_if = "Option::is_none")]
    views: Option<CatalogQuery>,
    #[serde(skip_serializing_if = "Option::is_none")]
    resume: Option<CatalogQuery>,
    #[serde(skip_serializing_if = "Option::is_none")]
    recent: Option<CatalogQuery>,
    #[serde(skip_serializing_if = "Option::is_none")]
    favorites: Option<CatalogQuery>,
    #[serde(skip_serializing_if = "Option::is_none")]
    collection: Option<CatalogQuery>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CatalogQuery {
    scope_id: String,
    parent_id: String,
    parent_decoration: CatalogDecoration,
    rows: Vec<CatalogRow>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CatalogDecoration {
    #[serde(skip_serializing_if = "Option::is_none")]
    subtitle: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    has_latest_episode: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    continue_label: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    title: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    playlist_entry_id: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CatalogRow {
    row_key: String,
    entity_id: String,
    decoration: CatalogDecoration,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UserCapabilities {
    user_name: String,
    is_administrator: bool,
    can_download: bool,
    can_delete: bool,
}

fn decode_catalog_outcome<T: DeserializeOwned>(value: Value) -> Result<T, ApplicationError> {
    serde_json::from_value(value).map_err(|error| ApplicationError::internal(error.to_string()))
}

const PLAYLISTS_VIEW_ID: &str = "__yanami_playlists__";
const MAX_FAVORITES_ITEMS: usize = 20_000;
const MAX_FAVORITES_PAGES: usize = 40;

fn user_container_view(id: &str, name: &str, collection_type: &str, count: usize) -> BaseItem {
    BaseItem {
        id: id.to_owned(),
        name: name.to_owned(),
        item_type: Some("UserView".to_owned()),
        collection_type: Some(collection_type.to_owned()),
        child_count: u32::try_from(count).ok(),
        ..BaseItem::default()
    }
}

impl Application {
    #[allow(clippy::too_many_lines)]
    pub fn library(&self) -> Result<LibraryOutcome, ApplicationError> {
        let client = self.active_client()?;
        let library_request = self.block_on(async {
            let library_query = ItemQuery {
                include_item_types: vec!["Movie".to_owned(), "Series".to_owned()],
                recursive: true,
                limit: 400,
                sort_by: vec!["SortName".to_owned()],
                sort_order: Some("Ascending".to_owned()),
                ..ItemQuery::default()
            };
            let library_request = client.items(&library_query);
            let views_request = client.user_views();
            let recent_request = client.latest_items(&["Episode"], 20, false);
            let resume_request = client.continue_watching(16);
            // Emby returns parent Series DTOs on some server versions when
            // GroupItems=true. Fetch a broad, newest-first episode window
            // instead so every card can show an actual SxxExx + episode name.
            let latest_request = client.latest_items(&["Episode"], 5000, false);
            let user_request = client.current_user();
            let playlists_request = client.playlists();
            Ok::<_, ApplicationError>(tokio::join!(
                library_request,
                views_request,
                recent_request,
                resume_request,
                latest_request,
                user_request,
                playlists_request,
            ))
        })?;
        let (
            library_result,
            views_result,
            recent_items,
            resume_result,
            latest_per_series,
            user,
            playlists_result,
        ) = library_request;
        let library_result = library_result.map_err(ApplicationError::from)?;
        let views_result = views_result.map_err(ApplicationError::from)?;
        let recent_items = recent_items.map_err(ApplicationError::from)?;
        let resume_result = resume_result.map_err(ApplicationError::from)?;
        let latest_per_series = latest_per_series.map_err(ApplicationError::from)?;
        let user = user.map_err(ApplicationError::from)?;
        let playlists = playlists_result.map_or_else(
            |error| {
                tracing::warn!(error = %error, "user playlist view probe failed");
                Vec::new()
            },
            |result| result.items,
        );

        let mut latest_by_series = BTreeMap::new();
        for item in latest_per_series {
            if let Some(series_id) = item.series_id.clone() {
                latest_by_series.entry(series_id).or_insert(item);
            }
        }
        // Collections (BoxSets) are server-global administrator objects in
        // Emby, so Yanami deliberately does not expose their server view.
        // Playlists remain user-scoped and are queried for the active user.
        let mut views: Vec<_> = views_result
            .items
            .into_iter()
            .filter(is_supported_view)
            .filter(|item| {
                !matches!(
                    item.collection_type.as_deref(),
                    Some("playlists" | "boxsets")
                )
            })
            .collect();
        if !playlists.is_empty() {
            views.push(user_container_view(
                PLAYLISTS_VIEW_ID,
                "Playlists",
                "playlists",
                playlists.len(),
            ));
        }
        // Membership, ordering and de-duplication are server-owned. The Emby
        // adapter selects the version-compatible official home endpoint.
        let resume_items = resume_result.items;
        let recent_images =
            self.cache_images(&client, &recent_items, ImagePurpose::EpisodeStill)?;
        let resume_images =
            self.cache_images(&client, &resume_items, ImagePurpose::EpisodeStill)?;
        let view_images = self.cache_images(&client, &views, ImagePurpose::Backdrop)?;
        let library_images =
            self.cache_images(&client, &library_result.items, ImagePurpose::Poster)?;
        let library: Vec<_> = library_result
            .items
            .iter()
            .zip(library_images)
            .map(|(item, image_url)| {
                media_card_json(
                    item,
                    image_url.as_deref(),
                    false,
                    latest_by_series.get(&item.id),
                )
            })
            .collect();
        let library_views: Vec<_> = views
            .iter()
            .zip(view_images)
            .map(|(item, image_url)| library_view_json(item, image_url.as_deref()))
            .collect();
        let recent: Vec<_> = recent_items
            .iter()
            .zip(recent_images)
            .map(|(item, image_url)| media_card_json(item, image_url.as_deref(), true, None))
            .collect();
        let resume: Vec<_> = resume_items
            .iter()
            .zip(resume_images)
            .map(|(item, image_url)| media_card_json(item, image_url.as_deref(), true, None))
            .collect();
        decode_catalog_outcome(normalized_query_payload(
            vec![
                ("library", String::new(), library, None),
                ("views", String::new(), library_views, None),
                ("resume", String::new(), resume, None),
                ("recent", String::new(), recent, None),
            ],
            json!({
                "userCapabilities": {
                    "userName": user.name,
                    "isAdministrator": user.policy.is_administrator,
                    "canDownload": user.policy.enable_content_downloading,
                    "canDelete": user.policy.is_administrator
                        || user.policy.enable_content_deletion,
                },
            }),
        )?)
    }

    pub fn favorites(&self) -> Result<FavoritesOutcome, ApplicationError> {
        let client = self.active_client()?;
        let (items, total_record_count) = self.block_on(async {
            let mut items = Vec::new();
            let mut pages = 0_usize;
            let total_record_count = loop {
                if pages >= MAX_FAVORITES_PAGES {
                    return Err(ApplicationError::unsupported(format!(
                        "Emby favorites exceeded the {MAX_FAVORITES_PAGES}-page safety limit"
                    )));
                }
                pages += 1;
                let result = client
                    .items(&ItemQuery {
                        include_item_types: vec![
                            "Movie".to_owned(),
                            "Series".to_owned(),
                            "Season".to_owned(),
                            "Episode".to_owned(),
                            "Video".to_owned(),
                            "MusicVideo".to_owned(),
                        ],
                        recursive: true,
                        start_index: items.len() as u32,
                        limit: 500,
                        sort_by: vec!["SortName".to_owned()],
                        sort_order: Some("Ascending".to_owned()),
                        filters: vec!["IsFavorite".to_owned()],
                        ..ItemQuery::default()
                    })
                    .await
                    .map_err(ApplicationError::from)?;
                let total_record_count = result.total_record_count;
                if total_record_count > MAX_FAVORITES_ITEMS as u64
                    || items.len().saturating_add(result.items.len()) > MAX_FAVORITES_ITEMS
                {
                    return Err(ApplicationError::unsupported(format!(
                        "Emby favorites exceeded the {MAX_FAVORITES_ITEMS}-item safety limit"
                    )));
                }
                let page_size = result.items.len();
                items.extend(result.items);
                if page_size == 0 || items.len() as u64 >= total_record_count {
                    break total_record_count;
                }
            };
            Ok::<_, ApplicationError>((items, total_record_count))
        })?;
        let image_urls = self.cache_images(&client, &items, ImagePurpose::Poster)?;
        let favorites: Vec<_> = items
            .iter()
            .zip(image_urls)
            .map(|(item, image_url)| {
                media_card_json(
                    item,
                    image_url.as_deref(),
                    item.item_type.as_deref() == Some("Episode"),
                    None,
                )
            })
            .collect();
        decode_catalog_outcome(normalized_query_payload(
            vec![("favorites", String::new(), favorites, None)],
            json!({
                "totalRecordCount": total_record_count,
            }),
        )?)
    }

    #[allow(clippy::too_many_lines)]
    pub fn activity(&self) -> Result<ActivityOutcome, ApplicationError> {
        let client = self.active_client()?;
        let (recent_items, resume_result) = self.block_on_emby(async {
            let recent_request = client.latest_items(&["Episode"], 20, false);
            let resume_request = client.continue_watching(16);
            tokio::try_join!(recent_request, resume_request)
        })?;
        let resume_items = resume_result.items;
        let recent_images =
            self.cache_images(&client, &recent_items, ImagePurpose::EpisodeStill)?;
        let resume_images =
            self.cache_images(&client, &resume_items, ImagePurpose::EpisodeStill)?;
        let recent: Vec<_> = recent_items
            .iter()
            .zip(recent_images)
            .map(|(item, image_url)| media_card_json(item, image_url.as_deref(), true, None))
            .collect();
        let resume: Vec<_> = resume_items
            .iter()
            .zip(resume_images)
            .map(|(item, image_url)| media_card_json(item, image_url.as_deref(), true, None))
            .collect();
        decode_catalog_outcome(normalized_query_payload(
            vec![
                ("resume", String::new(), resume, None),
                ("recent", String::new(), recent, None),
            ],
            json!({}),
        )?)
    }

    #[allow(clippy::too_many_lines)]
    pub fn collection(&self, parent_id: &str) -> Result<CollectionOutcome, ApplicationError> {
        let client = self.active_client()?;
        let parent = match parent_id {
            PLAYLISTS_VIEW_ID => {
                user_container_view(PLAYLISTS_VIEW_ID, "Playlists", "playlists", 0)
            }
            _ => self.block_on_emby(client.item(parent_id))?,
        };
        let (items, purpose, continue_item) = match parent.item_type.as_deref() {
            Some("Series") => {
                let (seasons, next_up) = self.block_on_emby(async {
                    tokio::try_join!(
                        client.seasons(parent_id),
                        client.next_up(Some(parent_id), 1)
                    )
                })?;
                (
                    seasons.items,
                    ImagePurpose::Poster,
                    next_up.items.into_iter().find(is_playable_item),
                )
            }
            Some("Season") => {
                let series_id = parent.series_id.as_deref().ok_or_else(|| {
                    ApplicationError::unsupported("Emby did not provide the season's series id")
                })?;
                let episodes: Vec<_> = self
                    .block_on_emby(client.episodes(series_id, Some(parent_id)))?
                    .items
                    .into_iter()
                    .filter(is_playable_item)
                    .collect();
                let continue_item = select_episode(episodes.clone());
                (episodes, ImagePurpose::EpisodeStill, continue_item)
            }
            Some("Playlist") => {
                let entries = self.block_on_emby(client.playlist_items(parent_id))?.items;
                (entries, ImagePurpose::EpisodeStill, None)
            }
            _ if is_supported_view(&parent) => {
                let items = match parent.collection_type.as_deref() {
                    Some("playlists") => self.block_on_emby(client.playlists())?.items,
                    _ => {
                        self.block_on_emby(client.items(&ItemQuery {
                            parent_id: Some(parent_id.to_owned()),
                            include_item_types: vec!["Movie".to_owned(), "Series".to_owned()],
                            recursive: true,
                            limit: 2000,
                            sort_by: vec!["SortName".to_owned()],
                            sort_order: Some("Ascending".to_owned()),
                            ..ItemQuery::default()
                        }))?
                        .items
                    }
                };
                (items, ImagePurpose::Poster, None)
            }
            _ => {
                return Err(ApplicationError::unsupported(
                    "this item cannot be opened as a collection",
                ));
            }
        };
        // Queue the visible hero images before a potentially large collection.
        let parent_poster = self
            .cache_images(&client, std::slice::from_ref(&parent), ImagePurpose::Poster)?
            .into_iter()
            .next()
            .flatten();
        let parent_backdrop = self
            .cache_images(
                &client,
                std::slice::from_ref(&parent),
                ImagePurpose::Backdrop,
            )?
            .into_iter()
            .next()
            .flatten();
        let image_urls = self.cache_images(&client, &items, purpose)?;
        let children: Vec<_> = items
            .iter()
            .zip(image_urls)
            .map(|(item, image_url)| media_card_json(item, image_url.as_deref(), false, None))
            .collect();
        let continue_label = continue_item
            .as_ref()
            .map(|item| card_subtitle(item, true))
            .unwrap_or_default();
        let parent_card = collection_parent_json(
            &parent,
            parent_poster.as_deref(),
            parent_backdrop.as_deref(),
            &continue_label,
        );
        decode_catalog_outcome(normalized_query_payload(
            vec![(
                "collection",
                parent_id.to_owned(),
                children,
                Some(parent_card),
            )],
            json!({}),
        )?)
    }
}

fn collection_parent_json(
    parent: &BaseItem,
    parent_poster: Option<&str>,
    parent_backdrop: Option<&str>,
    continue_label: &str,
) -> Value {
    json!({
        "id": parent.id,
        "title": parent.name,
        "itemType": parent.item_type,
        "collectionType": parent.collection_type,
        "seriesId": parent.series_id,
        "subtitle": parent.collection_type.as_ref().map_or_else(
            || card_subtitle(parent, false),
            |_| String::new(),
        ),
        "childCount": parent.recursive_item_count.or(parent.child_count),
        "unplayedCount": parent
            .user_data
            .as_ref()
            .and_then(|data| data.unplayed_item_count),
        "productionYear": parent.production_year,
        "hasLatestEpisode": false,
        "overview": parent.overview,
        "imageUrl": parent_poster,
        "backdropUrl": parent_backdrop,
        "continueLabel": continue_label,
        "canEditItems": parent.can_edit_items,
        "canDelete": parent.can_delete,
        "sourceUpdatedAt": parent.date_last_saved,
        "sourceVersion": presentation::source_version(parent),
        "providerIds": parent.provider_ids,
    })
}

fn normalized_query_payload(
    queries: Vec<(&str, String, Vec<Value>, Option<Value>)>,
    extra: Value,
) -> Result<Value, String> {
    let Value::Object(mut payload) = extra else {
        return Err("normalized desktop response extras must be an object".to_owned());
    };
    let mut entities = Map::new();
    let mut query_objects = Map::new();
    for (kind, scope_id, cards, parent) in queries {
        let mut occurrences = BTreeMap::<String, usize>::new();
        let mut rows = Vec::with_capacity(cards.len());
        for card in cards {
            let mut entity = card
                .as_object()
                .cloned()
                .ok_or_else(|| format!("{kind} query row must be an object"))?;
            let entity_id = entity
                .get("id")
                .and_then(Value::as_str)
                .filter(|id| !id.is_empty())
                .ok_or_else(|| format!("{kind} query row requires an id"))?
                .to_owned();
            let playlist_entry_id = entity
                .remove("playlistEntryId")
                .and_then(|value| value.as_str().map(str::to_owned))
                .unwrap_or_default();
            let contextual_title = entity
                .remove("titleIsContextual")
                .and_then(|value| value.as_bool())
                .unwrap_or(false);
            let mut decoration = Map::new();
            for key in ["subtitle", "hasLatestEpisode", "continueLabel"] {
                if let Some(value) = entity.remove(key) {
                    decoration.insert(key.to_owned(), value);
                }
            }
            if contextual_title {
                if let Some(value) = entity.remove("title") {
                    decoration.insert("title".to_owned(), value);
                }
            }
            if !playlist_entry_id.is_empty() {
                decoration.insert("playlistEntryId".to_owned(), json!(playlist_entry_id));
            }
            let existing = entities
                .entry(entity_id.clone())
                .or_insert_with(|| Value::Object(Map::new()));
            let existing = existing
                .as_object_mut()
                .ok_or_else(|| "normalized entity table is invalid".to_owned())?;
            for (key, value) in entity {
                existing.insert(key, value);
            }
            let occurrence = occurrences.entry(entity_id.clone()).or_default();
            let row_key = if !playlist_entry_id.is_empty() {
                format!("playlist:{playlist_entry_id}")
            } else if *occurrence == 0 {
                entity_id.clone()
            } else {
                format!("{entity_id}#{occurrence}")
            };
            *occurrence += 1;
            rows.push(json!({
                "rowKey": row_key,
                "entityId": entity_id,
                "decoration": decoration,
            }));
        }

        let mut parent_id = String::new();
        let mut parent_decoration = Map::new();
        if let Some(parent) = parent {
            let mut parent_entity = parent
                .as_object()
                .cloned()
                .ok_or_else(|| format!("{kind} query parent must be an object"))?;
            parent_entity
                .get("id")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .clone_into(&mut parent_id);
            for key in ["subtitle", "hasLatestEpisode", "continueLabel"] {
                if let Some(value) = parent_entity.remove(key) {
                    parent_decoration.insert(key.to_owned(), value);
                }
            }
            if !parent_id.is_empty() {
                entities.insert(parent_id.clone(), Value::Object(parent_entity));
            }
        }
        query_objects.insert(
            kind.to_owned(),
            json!({
                "scopeId": scope_id,
                "parentId": parent_id,
                "parentDecoration": parent_decoration,
                "rows": rows,
            }),
        );
    }
    payload.insert("entities".to_owned(), Value::Object(entities));
    payload.insert("queries".to_owned(), Value::Object(query_objects));
    Ok(Value::Object(payload))
}

#[cfg(test)]
mod tests {
    use serde::{Serialize, de::DeserializeOwned};
    use serde_json::{Value, json};
    use yanami_emby::{BaseItem, UserItemData};

    use super::{
        ActivityOutcome, CatalogEntity, CollectionOutcome, FavoritesOutcome, LibraryOutcome,
        collection_parent_json, decode_catalog_outcome, normalized_query_payload,
    };
    use crate::presentation::{library_view_json, media_card_json};

    fn media_item() -> BaseItem {
        BaseItem {
            id: "episode-1".to_owned(),
            name: "Episode 1".to_owned(),
            item_type: Some("Episode".to_owned()),
            series_id: Some("series-1".to_owned()),
            series_name: Some("Series".to_owned()),
            playlist_item_id: Some("entry-1".to_owned()),
            date_created: Some("2026-08-16T00:00:00Z".to_owned()),
            user_data: Some(UserItemData {
                playback_position_ticks: 42,
                unplayed_item_count: Some(1),
                ..UserItemData::default()
            }),
            ..BaseItem::default()
        }
    }

    fn view_item() -> BaseItem {
        BaseItem {
            id: "view-1".to_owned(),
            name: "TV".to_owned(),
            item_type: Some("CollectionFolder".to_owned()),
            collection_type: Some("tvshows".to_owned()),
            ..BaseItem::default()
        }
    }

    fn parent_item() -> BaseItem {
        BaseItem {
            id: "series-1".to_owned(),
            name: "Series".to_owned(),
            item_type: Some("Series".to_owned()),
            child_count: Some(12),
            can_edit_items: Some(true),
            can_delete: Some(false),
            user_data: Some(UserItemData {
                unplayed_item_count: Some(2),
                ..UserItemData::default()
            }),
            ..BaseItem::default()
        }
    }

    fn round_trip<T>(raw: &Value) -> T
    where
        T: DeserializeOwned + Serialize,
    {
        let typed: T = decode_catalog_outcome(raw.clone()).unwrap();
        assert_eq!(serde_json::to_value(&typed).unwrap(), *raw);
        typed
    }

    #[test]
    fn library_contract_covers_media_and_view_entities() {
        let media = media_item();
        let view = view_item();
        let raw = normalized_query_payload(
            vec![
                (
                    "library",
                    String::new(),
                    vec![media_card_json(&media, None, false, None)],
                    None,
                ),
                (
                    "views",
                    String::new(),
                    vec![library_view_json(&view, Some("file:///view.jpg"))],
                    None,
                ),
                (
                    "resume",
                    String::new(),
                    vec![media_card_json(&media, None, true, None)],
                    None,
                ),
                (
                    "recent",
                    String::new(),
                    vec![media_card_json(&media, None, true, None)],
                    None,
                ),
            ],
            json!({
                "userCapabilities": {
                    "userName": "Tester",
                    "isAdministrator": true,
                    "canDownload": true,
                    "canDelete": false
                }
            }),
        )
        .unwrap();
        let typed: LibraryOutcome = round_trip(&raw);
        assert!(matches!(
            typed.entities.get("episode-1"),
            Some(CatalogEntity::Media(_))
        ));
        assert!(matches!(
            typed.entities.get("view-1"),
            Some(CatalogEntity::View(_))
        ));
    }

    #[test]
    fn favorites_contract_round_trips_contextual_media_decoration() {
        let media = media_item();
        let raw = normalized_query_payload(
            vec![(
                "favorites",
                String::new(),
                vec![media_card_json(&media, None, true, None)],
                None,
            )],
            json!({"totalRecordCount": 1}),
        )
        .unwrap();
        let typed: FavoritesOutcome = round_trip(&raw);
        assert!(matches!(
            typed.entities.get("episode-1"),
            Some(CatalogEntity::Media(_))
        ));
    }

    #[test]
    fn activity_contract_round_trips_resume_and_recent_queries() {
        let media = media_item();
        let raw = normalized_query_payload(
            vec![
                (
                    "resume",
                    String::new(),
                    vec![media_card_json(&media, None, true, None)],
                    None,
                ),
                (
                    "recent",
                    String::new(),
                    vec![media_card_json(&media, None, true, None)],
                    None,
                ),
            ],
            json!({}),
        )
        .unwrap();
        let _: ActivityOutcome = round_trip(&raw);
    }

    #[test]
    fn activity_contract_preserves_server_continue_watching_order() {
        let mut movie = media_item();
        movie.id = "movie-b".to_owned();
        movie.name = "Movie B".to_owned();
        movie.item_type = Some("Movie".to_owned());
        movie.series_id = None;

        let mut episode_a2 = media_item();
        episode_a2.id = "episode-a2".to_owned();
        episode_a2.name = "A2".to_owned();
        episode_a2.series_id = Some("series-a".to_owned());

        let mut episode_a3 = episode_a2.clone();
        episode_a3.id = "episode-a3".to_owned();
        episode_a3.name = "A3".to_owned();

        let cards = [&movie, &episode_a2, &episode_a3]
            .into_iter()
            .map(|item| media_card_json(item, None, true, None))
            .collect();
        let raw = normalized_query_payload(vec![("resume", String::new(), cards, None)], json!({}))
            .unwrap();
        let typed: ActivityOutcome = round_trip(&raw);
        assert_eq!(
            typed
                .queries
                .resume
                .unwrap()
                .rows
                .iter()
                .map(|row| row.entity_id.as_str())
                .collect::<Vec<_>>(),
            ["movie-b", "episode-a2", "episode-a3"]
        );
    }

    #[test]
    fn collection_contract_covers_parent_and_child_entities() {
        let media = media_item();
        let parent = parent_item();
        let parent_card = collection_parent_json(
            &parent,
            Some("file:///series-poster.jpg"),
            Some("file:///series-backdrop.jpg"),
            "Continue S01E02",
        );
        let raw = normalized_query_payload(
            vec![(
                "collection",
                parent.id.clone(),
                vec![media_card_json(&media, None, false, None)],
                Some(parent_card),
            )],
            json!({}),
        )
        .unwrap();
        let typed: CollectionOutcome = round_trip(&raw);
        assert!(matches!(
            typed.entities.get("episode-1"),
            Some(CatalogEntity::Media(_))
        ));
        assert!(matches!(
            typed.entities.get("series-1"),
            Some(CatalogEntity::Parent(_))
        ));
    }
}
