//! Pure mapping and selection helpers for the desktop transport representation.

use serde_json::{Value, json};
use yanami_emby::{BaseItem, ChapterInfo};

#[cfg(test)]
use yanami_emby::UserItemData;

pub(super) fn source_version(item: &BaseItem) -> String {
    // Stable FNV-1a over server-owned metadata. Unlike a process-randomized
    // hasher this value remains comparable across persisted desktop sessions.
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;
    let mut append = |value: &str| {
        for byte in value
            .as_bytes()
            .iter()
            .copied()
            .chain(std::iter::once(0xff))
        {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(0x0100_0000_01b3);
        }
    };
    append(item.date_last_saved.as_deref().unwrap_or_default());
    append(&item.name);
    append(item.overview.as_deref().unwrap_or_default());
    for (key, value) in &item.provider_ids {
        append(key);
        append(value);
    }
    for (key, value) in &item.image_tags {
        append(key);
        append(value);
    }
    for value in &item.backdrop_image_tags {
        append(value);
    }
    format!("{hash:016x}")
}

#[derive(Clone, Copy)]
pub(super) enum ImagePurpose {
    Poster,
    EpisodeStill,
    Backdrop,
}

fn card_title(item: &BaseItem, prefer_series_title: bool) -> String {
    if prefer_series_title && item.item_type.as_deref() == Some("Episode") {
        item.series_name
            .as_deref()
            .filter(|value| !value.trim().is_empty())
            .unwrap_or(&item.name)
            .to_owned()
    } else {
        item.name.clone()
    }
}

fn episode_code(item: &BaseItem) -> String {
    match (item.parent_index_number, item.index_number) {
        (Some(season), Some(episode)) => format!("S{season:02}E{episode:02}"),
        (None, Some(episode)) => format!("E{episode:02}"),
        _ => String::new(),
    }
}

pub(super) fn card_subtitle(item: &BaseItem, include_episode_name: bool) -> String {
    if item.item_type.as_deref() == Some("Episode") {
        let episode_code = episode_code(item);
        if episode_code.is_empty() {
            return item.name.clone();
        }
        if !include_episode_name || item.name.trim().is_empty() {
            return episode_code;
        }
        return format!("{episode_code} · {}", item.name);
    }
    if matches!(item.item_type.as_deref(), Some("Series" | "Season")) {
        let total = item.recursive_item_count.or(item.child_count);
        let unplayed = item
            .user_data
            .as_ref()
            .and_then(|data| data.unplayed_item_count);
        return match (total, unplayed) {
            (Some(total), Some(unplayed)) if unplayed > 0 => {
                format!("{total} episodes · {unplayed} unplayed")
            }
            (Some(total), _) => format!("{total} episodes"),
            _ => item
                .production_year
                .map_or_else(|| "Series".to_owned(), |year| year.to_string()),
        };
    }
    item.production_year.map_or_else(
        || item.item_type.as_deref().unwrap_or("Video").to_owned(),
        |year| year.to_string(),
    )
}

pub(super) fn media_card_json(
    item: &BaseItem,
    image_url: Option<&str>,
    recent_episode: bool,
    subtitle_source: Option<&BaseItem>,
) -> Value {
    let user_data = item.user_data.as_ref();
    let child_count = item.recursive_item_count.or(item.child_count);
    json!({
        "id": item.id,
        "playlistEntryId": item.playlist_item_id,
        "title": card_title(item, recent_episode),
        "titleIsContextual": recent_episode && item.item_type.as_deref() == Some("Episode"),
        "subtitle": subtitle_source.map_or_else(
            || card_subtitle(item, recent_episode),
            |episode| card_subtitle(episode, true),
        ),
        "itemType": item.item_type,
        "seriesId": item.series_id,
        "seriesTitle": item.series_name,
        "seasonId": item.season_id,
        "imageUrl": image_url,
        "resumeTicks": user_data.map_or(0, |data| data.playback_position_ticks),
        "played": user_data.is_some_and(|data| data.played),
        "favorite": user_data.is_some_and(|data| data.is_favorite),
        "progress": user_data.and_then(|data| data.played_percentage),
        "unplayedCount": user_data.and_then(|data| data.unplayed_item_count),
        "childCount": child_count,
        "hasLatestEpisode": subtitle_source.is_some(),
        "latestEpisodeSubtitle": subtitle_source.map(|episode| card_subtitle(episode, true)),
        "overview": item.overview,
        "dateCreated": item.date_created,
        "releaseDate": item.premiere_date,
        "productionYear": item.production_year,
        "updatedAt": subtitle_source
            .and_then(|episode| episode.date_created.as_deref())
            .or(item.date_created.as_deref()),
        "sourceUpdatedAt": item.date_last_saved,
        "sourceVersion": source_version(item),
        "providerIds": item.provider_ids,
    })
}

#[cfg(test)]
pub(super) fn media_state_json(item: &BaseItem) -> Value {
    let mut state = serde_json::Map::from_iter([
        ("id".to_owned(), json!(item.id)),
        ("seriesId".to_owned(), json!(item.series_id)),
        ("seasonId".to_owned(), json!(item.season_id)),
        ("sourceUpdatedAt".to_owned(), json!(item.date_last_saved)),
        ("sourceVersion".to_owned(), json!(source_version(item))),
    ]);
    if let Some(user_data) = item.user_data.as_ref() {
        state.insert("played".to_owned(), json!(user_data.played));
        state.insert("favorite".to_owned(), json!(user_data.is_favorite));
        state.insert(
            "resumeTicks".to_owned(),
            json!(user_data.playback_position_ticks),
        );
        if let Some(progress) = user_data.played_percentage {
            state.insert("progress".to_owned(), json!(progress));
        }
        if let Some(count) = user_data.unplayed_item_count {
            state.insert("unplayedCount".to_owned(), json!(count));
        }
    }
    Value::Object(state)
}

#[cfg(test)]
pub(super) fn user_state_json(item_id: &str, user_data: &UserItemData) -> Value {
    let mut state = serde_json::Map::from_iter([
        ("id".to_owned(), json!(item_id)),
        ("played".to_owned(), json!(user_data.played)),
        ("favorite".to_owned(), json!(user_data.is_favorite)),
        (
            "resumeTicks".to_owned(),
            json!(user_data.playback_position_ticks),
        ),
    ]);
    if let Some(progress) = user_data.played_percentage {
        state.insert("progress".to_owned(), json!(progress));
    }
    if let Some(count) = user_data.unplayed_item_count {
        state.insert("unplayedCount".to_owned(), json!(count));
    }
    Value::Object(state)
}

pub(super) fn library_view_json(item: &BaseItem, image_url: Option<&str>) -> Value {
    json!({
        "id": item.id,
        "title": item.name,
        "subtitle": "",
        "itemType": item.item_type,
        "collectionType": item.collection_type,
        "imageUrl": image_url,
        "sourceUpdatedAt": item.date_last_saved,
        "sourceVersion": source_version(item),
        "providerIds": item.provider_ids,
    })
}

pub(super) fn is_supported_view(item: &BaseItem) -> bool {
    matches!(
        item.collection_type.as_deref(),
        None | Some("tvshows" | "movies" | "homevideos" | "musicvideos" | "playlists")
    ) && matches!(
        item.item_type.as_deref(),
        Some("CollectionFolder" | "UserView" | "Folder" | "AggregateFolder" | "VirtualView")
    )
}

pub(super) fn image_reference(
    item: &BaseItem,
    purpose: ImagePurpose,
) -> Option<(&str, &str, &str)> {
    if matches!(purpose, ImagePurpose::Backdrop) {
        if let Some(tag) = item.backdrop_image_tags.first() {
            return Some((item.id.as_str(), "Backdrop", tag));
        }
        if let (Some(parent_id), Some(parent_tag)) = (
            item.parent_backdrop_item_id.as_deref(),
            item.parent_backdrop_image_tags.first(),
        ) {
            return Some((parent_id, "Backdrop", parent_tag));
        }
    }
    if matches!(purpose, ImagePurpose::EpisodeStill) {
        if let Some(tag) = item.image_tags.get("Primary") {
            return Some((item.id.as_str(), "Primary", tag));
        }
        if let Some(tag) = item.image_tags.get("Thumb") {
            return Some((item.id.as_str(), "Thumb", tag));
        }
        if let (Some(parent_id), Some(parent_tag)) = (
            item.parent_thumb_item_id.as_deref(),
            item.parent_thumb_image_tag.as_deref(),
        ) {
            return Some((parent_id, "Thumb", parent_tag));
        }
    }
    if matches!(item.item_type.as_deref(), Some("Season" | "Episode")) {
        if let Some(tag) = item.image_tags.get("Primary") {
            return Some((item.id.as_str(), "Primary", tag));
        }
        if let (Some(series_id), Some(series_tag)) = (
            item.series_id.as_deref(),
            item.series_primary_image_tag.as_deref(),
        ) {
            return Some((series_id, "Primary", series_tag));
        }
        if let (Some(parent_id), Some(parent_tag)) = (
            item.parent_thumb_item_id.as_deref(),
            item.parent_thumb_image_tag.as_deref(),
        ) {
            return Some((parent_id, "Thumb", parent_tag));
        }
    }
    item.image_tags
        .get("Primary")
        .or(item.primary_image_tag.as_ref())
        .map(|tag| (item.id.as_str(), "Primary", tag.as_str()))
}

pub(super) fn is_playable_item(item: &BaseItem) -> bool {
    item.location_type.as_deref() != Some("Virtual")
}

pub(super) fn select_episode(items: Vec<BaseItem>) -> Option<BaseItem> {
    let mut playable: Vec<_> = items.into_iter().filter(is_playable_item).collect();
    let preferred = playable.iter().position(|item| {
        item.user_data
            .as_ref()
            .is_some_and(|data| !data.played && data.playback_position_ticks > 0)
    });
    if let Some(index) = preferred {
        return Some(playable.remove(index));
    }
    let unplayed = playable
        .iter()
        .position(|item| item.user_data.as_ref().is_none_or(|data| !data.played));
    if let Some(index) = unplayed {
        return Some(playable.remove(index));
    }
    playable.into_iter().next()
}

pub(super) fn playback_title(item: &BaseItem) -> String {
    if item.item_type.as_deref() != Some("Episode") {
        return item.name.clone();
    }
    let mut parts = Vec::new();
    if let Some(series_name) = item.series_name.as_deref() {
        parts.push(series_name.to_owned());
    }
    let code = episode_code(item);
    if !code.is_empty() {
        parts.push(code);
    }
    if !item.name.trim().is_empty() {
        parts.push(item.name.clone());
    }
    parts.join(" · ")
}

pub(super) fn intro_range_from_chapters(chapters: &[ChapterInfo]) -> Option<(u64, u64)> {
    let intro_start = chapters
        .iter()
        .find(|chapter| {
            chapter
                .marker_type
                .as_deref()
                .is_some_and(|value| value.eq_ignore_ascii_case("IntroStart"))
        })
        .map(|chapter| chapter.start_position_ticks)?;
    let intro_end = chapters
        .iter()
        .find(|chapter| {
            chapter
                .marker_type
                .as_deref()
                .is_some_and(|value| value.eq_ignore_ascii_case("IntroEnd"))
        })
        .map(|chapter| chapter.start_position_ticks)?;
    (intro_end > intro_start).then_some((intro_start, intro_end))
}

pub(super) fn safe_cache_component(value: &str) -> String {
    value
        .chars()
        .filter(|character| character.is_ascii_alphanumeric() || matches!(character, '-' | '_'))
        .collect()
}

pub(super) fn media_file_stem(path: &str, fallback: &str) -> String {
    let leaf = path
        .rsplit(['/', '\\'])
        .find(|segment| !segment.is_empty())
        .unwrap_or(fallback);
    leaf.rsplit_once('.')
        .map_or(leaf, |(stem, _)| stem)
        .to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_intro_range_from_emby_chapter_markers() {
        let chapters = vec![
            ChapterInfo {
                start_position_ticks: 120_000_000,
                marker_type: Some("IntroStart".to_owned()),
                ..ChapterInfo::default()
            },
            ChapterInfo {
                start_position_ticks: 960_000_000,
                marker_type: Some("IntroEnd".to_owned()),
                ..ChapterInfo::default()
            },
        ];

        assert_eq!(
            intro_range_from_chapters(&chapters),
            Some((120_000_000, 960_000_000))
        );
    }

    #[test]
    fn ignores_incomplete_or_reversed_intro_markers() {
        let reversed = vec![
            ChapterInfo {
                start_position_ticks: 960_000_000,
                marker_type: Some("IntroStart".to_owned()),
                ..ChapterInfo::default()
            },
            ChapterInfo {
                start_position_ticks: 120_000_000,
                marker_type: Some("IntroEnd".to_owned()),
                ..ChapterInfo::default()
            },
        ];

        assert_eq!(intro_range_from_chapters(&reversed), None);
        assert_eq!(intro_range_from_chapters(&reversed[..1]), None);
    }

    #[test]
    fn extracts_cross_platform_media_stems() {
        assert_eq!(
            media_file_stem(r"D:\Anime\Episode 01.mkv", "fallback"),
            "Episode 01"
        );
        assert_eq!(
            media_file_stem("/media/Anime/Episode 02.mp4", "fallback"),
            "Episode 02"
        );
        assert_eq!(media_file_stem("", "fallback"), "fallback");
    }

    #[test]
    fn episode_cards_put_the_series_name_first() {
        let item = BaseItem {
            id: "episode-id".to_owned(),
            name: "第 1 集".to_owned(),
            item_type: Some("Episode".to_owned()),
            series_name: Some("地狱乐".to_owned()),
            parent_index_number: Some(1),
            index_number: Some(1),
            ..BaseItem::default()
        };

        assert_eq!(card_title(&item, true), "地狱乐");
        assert_eq!(card_subtitle(&item, true), "S01E01 · 第 1 集");
    }

    #[test]
    fn media_card_json_preserves_playback_state() {
        let item = BaseItem {
            id: "movie-id".to_owned(),
            name: "Movie".to_owned(),
            item_type: Some("Movie".to_owned()),
            user_data: Some(yanami_emby::UserItemData {
                playback_position_ticks: 420,
                played_percentage: Some(12.5),
                ..yanami_emby::UserItemData::default()
            }),
            ..BaseItem::default()
        };

        let card = media_card_json(&item, Some("file:///poster.jpg"), false, None);

        assert_eq!(card["id"], "movie-id");
        assert_eq!(card["title"], "Movie");
        assert_eq!(card["imageUrl"], "file:///poster.jpg");
        assert_eq!(card["resumeTicks"], 420);
        assert_eq!(card["progress"], 12.5);
        assert_eq!(card["played"], false);
    }

    #[test]
    fn playlist_cards_preserve_the_server_entry_identifier() {
        let item = BaseItem {
            id: "episode-id".to_owned(),
            name: "Episode".to_owned(),
            item_type: Some("Episode".to_owned()),
            playlist_item_id: Some("playlist-entry-id".to_owned()),
            ..BaseItem::default()
        };

        let card = media_card_json(&item, None, false, None);
        assert_eq!(card["id"], "episode-id");
        assert_eq!(card["playlistEntryId"], "playlist-entry-id");
    }

    #[test]
    fn playlist_views_are_browsable() {
        assert!(is_supported_view(&BaseItem {
            id: "playlists-view".to_owned(),
            name: "Playlists".to_owned(),
            item_type: Some("UserView".to_owned()),
            collection_type: Some("playlists".to_owned()),
            ..BaseItem::default()
        }));
    }

    #[test]
    fn continue_selection_prefers_an_episode_in_progress() {
        let completed = BaseItem {
            id: "done".to_owned(),
            name: "1".to_owned(),
            user_data: Some(yanami_emby::UserItemData {
                played: true,
                ..yanami_emby::UserItemData::default()
            }),
            ..BaseItem::default()
        };
        let in_progress = BaseItem {
            id: "current".to_owned(),
            name: "2".to_owned(),
            user_data: Some(yanami_emby::UserItemData {
                playback_position_ticks: 10,
                ..yanami_emby::UserItemData::default()
            }),
            ..BaseItem::default()
        };
        let next = BaseItem {
            id: "next".to_owned(),
            name: "3".to_owned(),
            ..BaseItem::default()
        };

        assert_eq!(
            select_episode(vec![completed, in_progress, next])
                .expect("an episode should be selected")
                .id,
            "current"
        );
    }

    #[test]
    fn state_patch_omits_unknown_optional_values() {
        let item = BaseItem {
            id: "series-1".to_owned(),
            name: "Series".to_owned(),
            item_type: Some("Series".to_owned()),
            user_data: Some(yanami_emby::UserItemData {
                played: false,
                unplayed_item_count: Some(30),
                ..yanami_emby::UserItemData::default()
            }),
            ..BaseItem::default()
        };

        let patch = media_state_json(&item);
        assert_eq!(patch["unplayedCount"], 30);
        assert!(patch.get("progress").is_none());
    }

    #[test]
    fn user_state_patch_preserves_authoritative_favorite_response() {
        let state = user_state_json(
            "episode-7",
            &yanami_emby::UserItemData {
                playback_position_ticks: 420,
                played: true,
                is_favorite: true,
                played_percentage: Some(84.5),
                unplayed_item_count: Some(2),
                ..yanami_emby::UserItemData::default()
            },
        );

        assert_eq!(state["id"], "episode-7");
        assert_eq!(state["favorite"], true);
        assert_eq!(state["played"], true);
        assert_eq!(state["resumeTicks"], 420);
        assert_eq!(state["progress"], 84.5);
        assert_eq!(state["unplayedCount"], 2);
    }

    #[test]
    fn poster_cache_names_drop_path_characters() {
        assert_eq!(safe_cache_component("abc/../tag:42"), "abctag42");
    }
}
