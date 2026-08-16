use std::collections::BTreeMap;

use serde::{Deserialize, Deserializer, Serialize};
use serde_json::{Map, Value, json};

use crate::{Application, ApplicationError};

use super::{MediaOutcome, network_error, require_administrator};

pub type MetadataOutcome = MediaOutcome<MetadataResult>;
pub type UpdateMetadataOutcome = MediaOutcome<UpdateMetadataResult>;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetadataResult {
    id: String,
    item_type: String,
    title: String,
    original_title: String,
    sort_name: String,
    overview: String,
    production_year: Option<i64>,
    premiere_date: String,
    end_date: String,
    official_rating: String,
    community_rating: Option<f64>,
    critic_rating: Option<f64>,
    status: String,
    air_time: String,
    air_days: Vec<String>,
    index_number: Option<i64>,
    parent_index_number: Option<i64>,
    genres: Vec<String>,
    tags: Vec<String>,
    editable_fields: Vec<&'static str>,
    external_ids: Vec<ExternalIdentifier>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ExternalIdentifier {
    key: String,
    name: String,
    value: String,
    website: String,
    url_format_string: String,
    supported_as_identifier: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateMetadataResult {
    provider_ids_changed: bool,
}

const METADATA_FIELD_MAP: &[(&str, &str)] = &[
    ("title", "Name"),
    ("originalTitle", "OriginalTitle"),
    ("sortName", "ForcedSortName"),
    ("overview", "Overview"),
    ("productionYear", "ProductionYear"),
    ("premiereDate", "PremiereDate"),
    ("endDate", "EndDate"),
    ("officialRating", "OfficialRating"),
    ("communityRating", "CommunityRating"),
    ("criticRating", "CriticRating"),
    ("status", "Status"),
    ("airTime", "AirTime"),
    ("airDays", "AirDays"),
    ("indexNumber", "IndexNumber"),
    ("parentIndexNumber", "ParentIndexNumber"),
    ("genres", "Genres"),
    ("tags", "Tags"),
];

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetadataPatch {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub title: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub original_title: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sort_name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub overview: Option<String>,
    #[serde(
        default,
        deserialize_with = "deserialize_nullable_patch",
        skip_serializing_if = "Option::is_none"
    )]
    pub production_year: Option<Option<i64>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub premiere_date: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub end_date: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub official_rating: Option<String>,
    #[serde(
        default,
        deserialize_with = "deserialize_nullable_patch",
        skip_serializing_if = "Option::is_none"
    )]
    pub community_rating: Option<Option<f64>>,
    #[serde(
        default,
        deserialize_with = "deserialize_nullable_patch",
        skip_serializing_if = "Option::is_none"
    )]
    pub critic_rating: Option<Option<f64>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub status: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub air_time: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub air_days: Option<Vec<String>>,
    #[serde(
        default,
        deserialize_with = "deserialize_nullable_patch",
        skip_serializing_if = "Option::is_none"
    )]
    pub index_number: Option<Option<i64>>,
    #[serde(
        default,
        deserialize_with = "deserialize_nullable_patch",
        skip_serializing_if = "Option::is_none"
    )]
    pub parent_index_number: Option<Option<i64>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub genres: Option<Vec<String>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tags: Option<Vec<String>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub provider_ids: Option<BTreeMap<String, String>>,
}

#[allow(clippy::option_option)] // Three states: omitted, explicit null, and concrete value.
fn deserialize_nullable_patch<'de, D, T>(deserializer: D) -> Result<Option<Option<T>>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    Option::<T>::deserialize(deserializer).map(Some)
}

impl Application {
    pub fn metadata(&self, item_id: &str) -> Result<MetadataOutcome, ApplicationError> {
        let client = self.active_client()?;
        let result = self.block_on(async {
            // These reads are independent. Keeping them concurrent avoids an
            // extra administrator round-trip before the editor can paint.
            let (administrator, item, editor_info) = tokio::join!(
                require_administrator(&client),
                client.raw_item(item_id),
                client.metadata_editor_info(item_id),
            );
            administrator?;
            let item = item.map_err(network_error)?;
            let editor_info = editor_info.unwrap_or_else(|error| {
                // Older/customized Emby servers may omit this endpoint. Basic
                // editing remains usable with a conservative ID fallback.
                tracing::warn!(item_id, error = %error, "Emby metadata editor info unavailable");
                Value::Null
            });
            Ok::<_, ApplicationError>(metadata_editor_dto(&item, &editor_info))
        })?;
        Ok(MediaOutcome::read(item_id, result))
    }

    pub fn update_metadata(
        &self,
        item_id: &str,
        request: &MetadataPatch,
    ) -> Result<UpdateMetadataOutcome, ApplicationError> {
        let client = self.active_client()?;
        let patch = serde_json::to_value(request)
            .map_err(|error| ApplicationError::internal(error.to_string()))?;
        let result = self.block_on(async {
            require_administrator(&client).await?;
            let mut item = client.raw_item(item_id).await.map_err(network_error)?;
            let provider_ids_before = normalized_provider_ids(&item);
            apply_metadata_patch(&mut item, &patch)?;
            let provider_ids_changed = provider_ids_before != normalized_provider_ids(&item);
            client
                .update_item(item_id, &item)
                .await
                .map_err(network_error)?;
            Ok::<_, ApplicationError>(UpdateMetadataResult {
                provider_ids_changed,
            })
        })?;
        Ok(MediaOutcome::invalidated(
            item_id,
            result,
            &["library", "activity", "favorites", "collection"],
        ))
    }
}

fn metadata_editor_dto(item: &Value, editor_info: &Value) -> MetadataResult {
    let item_type = item.get("Type").and_then(Value::as_str).unwrap_or_default();
    MetadataResult {
        id: string_field(item, "Id"),
        item_type: item_type.to_owned(),
        title: string_field(item, "Name"),
        original_title: string_field(item, "OriginalTitle"),
        sort_name: item
            .get("ForcedSortName")
            .or_else(|| item.get("SortName"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned(),
        overview: string_field(item, "Overview"),
        production_year: item.get("ProductionYear").and_then(Value::as_i64),
        premiere_date: string_field(item, "PremiereDate"),
        end_date: string_field(item, "EndDate"),
        official_rating: string_field(item, "OfficialRating"),
        community_rating: item.get("CommunityRating").and_then(Value::as_f64),
        critic_rating: item.get("CriticRating").and_then(Value::as_f64),
        status: string_field(item, "Status"),
        air_time: string_field(item, "AirTime"),
        air_days: string_array_field(item, "AirDays"),
        index_number: item.get("IndexNumber").and_then(Value::as_i64),
        parent_index_number: item.get("ParentIndexNumber").and_then(Value::as_i64),
        genres: string_array_field(item, "Genres"),
        tags: string_array_field(item, "Tags"),
        editable_fields: editable_metadata_fields(item_type),
        external_ids: external_identifier_fields(item, editor_info),
    }
}

fn string_field(item: &Value, key: &str) -> String {
    item.get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned()
}

fn string_array_field(item: &Value, key: &str) -> Vec<String> {
    item.get(key)
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(str::to_owned)
        .collect()
}

/// Mirrors the item-type visibility rules of Emby's metadata editor for the
/// subset of fields currently supported by the desktop client.
fn editable_metadata_fields(item_type: &str) -> Vec<&'static str> {
    let mut fields = vec!["title", "sortName", "tags"];

    if matches!(
        item_type,
        "Series" | "Season" | "Episode" | "Movie" | "Trailer" | "Person"
    ) {
        fields.push("originalTitle");
    }
    if item_type != "TvChannel" {
        fields.extend(["overview", "productionYear", "premiereDate"]);
    }
    if item_type == "TvChannel" {
        fields.push("officialRating");
    } else if !matches!(item_type, "Person" | "Genre" | "Studio" | "MusicGenre") {
        fields.extend(["officialRating", "communityRating", "genres"]);
    }
    if matches!(item_type, "Movie" | "Trailer") {
        fields.push("criticRating");
    }
    if matches!(item_type, "Series" | "Person") {
        fields.push("endDate");
    }
    if item_type == "Series" {
        fields.extend(["status", "airTime", "airDays"]);
    }
    if matches!(item_type, "Audio" | "Episode" | "Season") {
        fields.push("indexNumber");
    }
    if matches!(item_type, "Audio" | "Episode") {
        fields.push("parentIndexNumber");
    }
    fields
}

fn external_identifier_fields(item: &Value, editor_info: &Value) -> Vec<ExternalIdentifier> {
    let provider_ids = item.get("ProviderIds").and_then(Value::as_object);
    let Some(infos) = editor_info.get("ExternalIdInfos").and_then(Value::as_array) else {
        return fallback_external_identifier_fields(item);
    };

    let mut seen = std::collections::HashSet::new();
    infos
        .iter()
        .filter_map(|info| {
            let key = info.get("Key").and_then(Value::as_str)?.trim();
            if key.is_empty() || !seen.insert(key.to_ascii_lowercase()) {
                return None;
            }
            let name = info
                .get("Name")
                .and_then(Value::as_str)
                .filter(|name| !name.trim().is_empty())
                .unwrap_or(key);
            Some(ExternalIdentifier {
                key: key.to_owned(),
                name: name.to_owned(),
                value: provider_identifier_value(provider_ids, key).to_owned(),
                website: info
                    .get("Website")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_owned(),
                url_format_string: info
                    .get("UrlFormatString")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_owned(),
                supported_as_identifier: info
                    .get("IsSupportedAsIdentifier")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
            })
        })
        .collect()
}

fn fallback_external_identifier_fields(item: &Value) -> Vec<ExternalIdentifier> {
    let item_type = item.get("Type").and_then(Value::as_str).unwrap_or_default();
    let provider_ids = item.get("ProviderIds").and_then(Value::as_object);
    let mut keys: Vec<String> = match item_type {
        "Series" | "Season" | "Episode" => vec!["Tmdb", "Tvdb", "Imdb"],
        "Movie" | "Trailer" => vec!["Tmdb", "Imdb"],
        "Person" => vec!["Tmdb"],
        _ => Vec::new(),
    }
    .into_iter()
    .map(str::to_owned)
    .collect();
    if let Some(provider_ids) = provider_ids {
        for key in provider_ids.keys() {
            if let Some(index) = keys
                .iter()
                .position(|existing| existing.eq_ignore_ascii_case(key))
            {
                key.clone_into(&mut keys[index]);
            } else {
                keys.push(key.to_owned());
            }
        }
    }
    keys.into_iter()
        .map(|key| {
            let name = match key.to_ascii_lowercase().as_str() {
                "tmdb" => "TheMovieDb",
                "tvdb" => "TheTVDB",
                "imdb" => "IMDb",
                _ => key.as_str(),
            }
            .to_owned();
            let value = provider_ids
                .and_then(|ids| ids.get(&key))
                .and_then(Value::as_str)
                .unwrap_or_default();
            ExternalIdentifier {
                key,
                name,
                value: value.to_owned(),
                website: String::new(),
                url_format_string: String::new(),
                supported_as_identifier: true,
            }
        })
        .collect()
}

fn provider_identifier_value<'a>(
    provider_ids: Option<&'a Map<String, Value>>,
    key: &str,
) -> &'a str {
    provider_ids
        .and_then(|ids| {
            ids.get(key).or_else(|| {
                ids.iter()
                    .find(|(candidate, _)| candidate.eq_ignore_ascii_case(key))
                    .map(|(_, value)| value)
            })
        })
        .and_then(Value::as_str)
        .unwrap_or_default()
}

fn apply_metadata_patch(item: &mut Value, patch: &Value) -> Result<(), ApplicationError> {
    // Stage every change so a late validation error cannot leave the raw Emby
    // item partly modified.
    let mut updated_item = item.clone();
    apply_metadata_patch_in_place(&mut updated_item, patch)?;
    *item = updated_item;
    Ok(())
}

fn normalized_provider_ids(item: &Value) -> BTreeMap<String, String> {
    item.get("ProviderIds")
        .and_then(Value::as_object)
        .into_iter()
        .flatten()
        .filter_map(|(key, value)| {
            let key = key.trim();
            let value = value.as_str()?.trim();
            (!key.is_empty() && !value.is_empty())
                .then(|| (key.to_ascii_lowercase(), value.to_owned()))
        })
        .collect()
}

fn apply_metadata_patch_in_place(item: &mut Value, patch: &Value) -> Result<(), ApplicationError> {
    let item_object = item
        .as_object_mut()
        .ok_or_else(|| ApplicationError::unsupported("Emby returned invalid item metadata"))?;
    let patch_object = patch
        .as_object()
        .ok_or_else(|| ApplicationError::invalid("metadata changes must be a JSON object"))?;
    let item_type = item_object
        .get("Type")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let editable_fields = editable_metadata_fields(item_type);
    validate_metadata_patch(patch_object, &editable_fields)?;
    for (source, destination) in METADATA_FIELD_MAP {
        if editable_fields.contains(source)
            && let Some(value) = patch_object.get(*source)
        {
            item_object.insert((*destination).to_owned(), value.clone());
        }
    }

    if let Some(provider_ids) = patch_object.get("providerIds") {
        let provider_ids = provider_ids.as_object().ok_or_else(|| {
            ApplicationError::invalid("external identifiers must be a JSON object")
        })?;
        if provider_ids.len() > 64 {
            return Err(ApplicationError::invalid("too many external identifiers"));
        }
        let item_provider_ids = item_object
            .entry("ProviderIds")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .ok_or_else(|| {
                ApplicationError::unsupported("Emby returned invalid provider identifiers")
            })?;
        for (key, value) in provider_ids {
            if key.is_empty() || key.chars().count() > 128 || key.chars().any(char::is_control) {
                return Err(ApplicationError::invalid("invalid external identifier key"));
            }
            let value = value
                .as_str()
                .ok_or_else(|| {
                    ApplicationError::invalid("external identifier values must be strings")
                })?
                .trim();
            if value.chars().count() > 512 {
                return Err(ApplicationError::invalid(
                    "external identifier values cannot exceed 512 characters",
                ));
            }
            let destination_key = item_provider_ids
                .keys()
                .find(|existing| existing.eq_ignore_ascii_case(key))
                .cloned()
                .unwrap_or_else(|| key.to_owned());
            if value.is_empty() {
                item_provider_ids.remove(&destination_key);
            } else {
                item_provider_ids.insert(destination_key, json!(value));
            }
        }
    }
    Ok(())
}

fn validate_metadata_patch(
    patch: &Map<String, Value>,
    editable_fields: &[&str],
) -> Result<(), ApplicationError> {
    validate_string_fields(patch, editable_fields)?;
    validate_integer_fields(patch, editable_fields)?;
    validate_rating_fields(patch, editable_fields)?;
    validate_list_fields(patch, editable_fields)
}

fn validate_string_fields(
    patch: &Map<String, Value>,
    editable_fields: &[&str],
) -> Result<(), ApplicationError> {
    const STRING_FIELDS: &[(&str, usize)] = &[
        ("title", 1_024),
        ("originalTitle", 1_024),
        ("sortName", 1_024),
        ("overview", 200_000),
        ("premiereDate", 64),
        ("endDate", 64),
        ("officialRating", 128),
        ("status", 128),
        ("airTime", 64),
    ];
    for (field, maximum_length) in STRING_FIELDS {
        if !editable_fields.contains(field) {
            continue;
        }
        let Some(value) = patch.get(*field) else {
            continue;
        };
        let value = value
            .as_str()
            .ok_or_else(|| ApplicationError::invalid(format!("{field} must be a string")))?;
        if *field == "title" && value.trim().is_empty() {
            return Err(ApplicationError::invalid("title cannot be empty"));
        }
        if value.chars().count() > *maximum_length {
            return Err(ApplicationError::invalid(format!(
                "{field} exceeds its maximum length"
            )));
        }
    }
    Ok(())
}

fn validate_integer_fields(
    patch: &Map<String, Value>,
    editable_fields: &[&str],
) -> Result<(), ApplicationError> {
    for (field, maximum) in [
        ("productionYear", 9_999_i64),
        ("indexNumber", i64::from(i32::MAX)),
        ("parentIndexNumber", i64::from(i32::MAX)),
    ] {
        if !editable_fields.contains(&field) {
            continue;
        }
        let Some(value) = patch.get(field) else {
            continue;
        };
        if value.is_null() {
            continue;
        }
        let value = value.as_i64().ok_or_else(|| {
            ApplicationError::invalid(format!("{field} must be a whole number or null"))
        })?;
        if !(0..=maximum).contains(&value) {
            return Err(ApplicationError::invalid(format!(
                "{field} is outside the supported range"
            )));
        }
    }
    Ok(())
}

fn validate_rating_fields(
    patch: &Map<String, Value>,
    editable_fields: &[&str],
) -> Result<(), ApplicationError> {
    for (field, maximum) in [("communityRating", 10.0_f64), ("criticRating", 100.0_f64)] {
        if !editable_fields.contains(&field) {
            continue;
        }
        let Some(value) = patch.get(field) else {
            continue;
        };
        if value.is_null() {
            continue;
        }
        let value = value
            .as_f64()
            .filter(|value| value.is_finite())
            .ok_or_else(|| {
                ApplicationError::invalid(format!("{field} must be a finite number or null"))
            })?;
        if !(0.0..=maximum).contains(&value) {
            return Err(ApplicationError::invalid(format!(
                "{field} is outside the supported range"
            )));
        }
    }
    Ok(())
}

fn validate_list_fields(
    patch: &Map<String, Value>,
    editable_fields: &[&str],
) -> Result<(), ApplicationError> {
    for (field, maximum_items, maximum_entry_length) in [
        ("genres", 256_usize, 256_usize),
        ("tags", 512_usize, 256_usize),
        ("airDays", 7_usize, 32_usize),
    ] {
        if !editable_fields.contains(&field) {
            continue;
        }
        let Some(value) = patch.get(field) else {
            continue;
        };
        let values = value.as_array().ok_or_else(|| {
            ApplicationError::invalid(format!("{field} must be an array of strings"))
        })?;
        if values.len() > maximum_items {
            return Err(ApplicationError::invalid(format!(
                "{field} contains too many values"
            )));
        }
        for value in values {
            let value = value.as_str().ok_or_else(|| {
                ApplicationError::invalid(format!("{field} must contain only strings"))
            })?;
            if value.chars().count() > maximum_entry_length {
                return Err(ApplicationError::invalid(format!(
                    "{field} contains a value that is too long"
                )));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::{MetadataPatch, apply_metadata_patch, metadata_editor_dto};
    use crate::{ApplicationErrorCode, media::MediaOutcome};

    #[test]
    fn metadata_outcome_contract_has_explicit_editor_fields() {
        let result = metadata_editor_dto(
            &json!({
                "Id": "episode-1", "Type": "Episode", "Name": "Episode 1",
                "Genres": ["Animation"], "AirDays": ["Monday"]
            }),
            &json!({}),
        );
        let encoded = serde_json::to_value(MediaOutcome::read("episode-1", result)).unwrap();
        assert_eq!(encoded["itemId"], "episode-1");
        assert_eq!(encoded["result"]["id"], "episode-1");
        assert_eq!(encoded["result"]["itemType"], "Episode");
        assert_eq!(encoded["result"]["genres"], json!(["Animation"]));
        assert_eq!(encoded["result"]["airDays"], json!(["Monday"]));
        assert!(encoded["result"]["editableFields"].is_array());
        assert!(encoded["result"]["externalIds"].is_array());
        assert!(encoded.get("invalidation").is_none());
    }

    #[test]
    fn metadata_patch_is_whitelisted_atomic_and_supports_numeric_clear() {
        let original = json!({
            "Id": "episode-1",
            "Type": "Episode",
            "Name": "Before",
            "ProductionYear": 2026,
            "IndexNumber": 3,
            "ProviderIds": { "Plugin.Provider": "keep-me" },
            "Path": "/private/media.mkv"
        });
        let mut item = original.clone();
        apply_metadata_patch(
            &mut item,
            &json!({
                "title": "After",
                "productionYear": null,
                "indexNumber": null,
                "providerIds": { "Tmdb": " 12345 " },
                "Path": "/changed.mkv"
            }),
        )
        .unwrap();
        assert_eq!(item["Name"], "After");
        assert!(item["ProductionYear"].is_null());
        assert!(item["IndexNumber"].is_null());
        assert_eq!(item["ProviderIds"]["Tmdb"], "12345");
        assert_eq!(item["ProviderIds"]["Plugin.Provider"], "keep-me");
        assert_eq!(item["Path"], "/private/media.mkv");

        let before_invalid = item.clone();
        let error = apply_metadata_patch(&mut item, &json!({"title": "Valid", "indexNumber": -1}))
            .unwrap_err();
        assert_eq!(error.code(), ApplicationErrorCode::InvalidInput);
        assert_eq!(item, before_invalid, "a rejected patch must be atomic");
    }

    #[test]
    fn malformed_emby_metadata_is_an_unsupported_adapter_response() {
        let error = apply_metadata_patch(&mut json!([]), &json!({})).unwrap_err();
        assert_eq!(error.code(), ApplicationErrorCode::Unsupported);
    }

    #[test]
    fn nullable_metadata_patch_preserves_absent_clear_and_value() {
        let absent: MetadataPatch = serde_json::from_value(json!({})).unwrap();
        let clear: MetadataPatch = serde_json::from_value(json!({
            "productionYear": null,
            "communityRating": null,
        }))
        .unwrap();
        let value: MetadataPatch = serde_json::from_value(json!({
            "productionYear": 2026,
            "communityRating": 8.5,
        }))
        .unwrap();

        assert_eq!(absent.production_year, None);
        assert_eq!(absent.community_rating, None);
        assert_eq!(clear.production_year, Some(None));
        assert_eq!(clear.community_rating, Some(None));
        assert_eq!(value.production_year, Some(Some(2026)));
        assert_eq!(value.community_rating, Some(Some(8.5)));
        assert_eq!(
            serde_json::to_value(clear).unwrap(),
            json!({"productionYear": null, "communityRating": null})
        );
    }
}
