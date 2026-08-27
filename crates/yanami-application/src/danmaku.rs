use chrono::Utc;
use serde::{Deserialize, Serialize};
use yanami_core::{DanmakuComment, DanmakuMode};
use yanami_danmaku::{
    AnimeSearchResult, EpisodeMatch, MatchInput, SearchEpisodesInput, hash_remote_prefix,
};
use yanami_storage::{CachedComments, DanmakuMatchRecord};

use crate::{Application, ApplicationError, ApplicationErrorCode, playback::ActivePlayback};

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DanmakuSearchRequest {
    pub anime: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DanmakuApplyRequest {
    pub episode_id: i64,
    #[serde(default)]
    pub anime_title: String,
    #[serde(default)]
    pub episode_title: String,
    #[serde(default)]
    pub time_offset: f64,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "kebab-case")]
enum DanmakuSearchStatus {
    SearchResults,
}

#[derive(Clone, Debug, Serialize)]
pub struct DanmakuSearchOutcome {
    status: DanmakuSearchStatus,
    animes: Vec<DanmakuSearchAnime>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DanmakuSearchAnime {
    anime_id: i64,
    anime_title: String,
    type_description: Option<String>,
    episodes: Vec<DanmakuSearchEpisode>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DanmakuSearchEpisode {
    episode_id: i64,
    episode_title: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(transparent)]
pub struct DanmakuAutoOutcome(DanmakuResolution);

#[derive(Clone, Debug, Serialize)]
#[serde(transparent)]
pub struct DanmakuApplyOutcome(DanmakuResolution);

#[derive(Clone, Debug, Serialize)]
#[serde(tag = "status")]
pub enum DanmakuResolution {
    #[serde(rename = "loaded")]
    Loaded {
        comments: Vec<DanmakuTimelineComment>,
        title: String,
        #[serde(rename = "commentCount")]
        comment_count: usize,
        stale: bool,
        #[serde(rename = "matchedTimeOffset")]
        matched_time_offset: f64,
    },
    #[serde(rename = "no-match")]
    NoMatch {
        #[serde(rename = "animeSuggestion")]
        anime_suggestion: String,
        #[serde(rename = "episodeSuggestion")]
        episode_suggestion: Option<String>,
        matches: Vec<DanmakuMatchOption>,
    },
    #[serde(rename = "choice-required")]
    ChoiceRequired {
        #[serde(rename = "animeSuggestion")]
        anime_suggestion: String,
        #[serde(rename = "episodeSuggestion")]
        episode_suggestion: Option<String>,
        matches: Vec<DanmakuMatchOption>,
    },
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum DanmakuTimelineMode {
    Scroll,
    Top,
    Bottom,
}

impl From<DanmakuMode> for DanmakuTimelineMode {
    fn from(mode: DanmakuMode) -> Self {
        match mode {
            DanmakuMode::Scroll => Self::Scroll,
            DanmakuMode::Top => Self::Top,
            DanmakuMode::Bottom => Self::Bottom,
        }
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct DanmakuTimelineComment {
    id: String,
    time: f64,
    mode: DanmakuTimelineMode,
    color: u32,
    text: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DanmakuMatchOption {
    episode_id: i64,
    anime_title: String,
    episode_title: String,
    type_description: Option<String>,
}

impl From<&EpisodeMatch> for DanmakuMatchOption {
    fn from(candidate: &EpisodeMatch) -> Self {
        Self {
            episode_id: candidate.episode_id,
            anime_title: candidate.anime_title.clone(),
            episode_title: candidate.episode_title.clone(),
            type_description: candidate.type_description.clone(),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
struct CommentLoadResult {
    comments: Vec<DanmakuComment>,
    stale: bool,
}

impl Application {
    pub fn search_danmaku(
        &self,
        _item_id: &str,
        request: &DanmakuSearchRequest,
    ) -> Result<DanmakuSearchOutcome, ApplicationError> {
        let anime = request.anime.trim();
        if anime.is_empty() {
            return Err(ApplicationError::invalid("enter an anime title to search"));
        }
        let animes = self.search_dandanplay_animes(&SearchEpisodesInput {
            anime: anime.to_owned(),
            episode: None,
        })?;
        Ok(DanmakuSearchOutcome {
            status: DanmakuSearchStatus::SearchResults,
            animes: animes
                .into_iter()
                .take(40)
                .map(danmaku_search_anime)
                .collect(),
        })
    }

    pub fn auto_danmaku(&self, item_id: &str) -> Result<DanmakuAutoOutcome, ApplicationError> {
        let (active, server_id, now) = self.active_danmaku_context(item_id)?;
        if let Some(matched) = self.stored_danmaku_match(&server_id, &active)? {
            return self
                .loaded_danmaku_resolution(&matched, now)
                .map(DanmakuAutoOutcome);
        }

        let file_size = active
            .file_size
            .ok_or_else(|| ApplicationError::unsupported("Emby did not provide the media size"))?;
        let duration = active.duration_seconds.ok_or_else(|| {
            ApplicationError::unsupported("Emby did not provide the media duration")
        })?;
        let file_hash = self.block_on_dandan(hash_remote_prefix(&active.media_url))?;
        let candidates = self.match_dandanplay(&MatchInput {
            file_name: active.file_name.clone(),
            file_hash,
            file_size,
            video_duration_seconds: duration,
        })?;
        let Some(candidate) = select_confident_match(
            &candidates,
            &active.series_name,
            active.season_number,
            active.episode_number,
        ) else {
            let matches = candidates.iter().map(Into::into).collect();
            let resolution = if candidates.is_empty() {
                DanmakuResolution::NoMatch {
                    anime_suggestion: active.series_name,
                    episode_suggestion: active.episode_number.map(|value| value.to_string()),
                    matches,
                }
            } else {
                DanmakuResolution::ChoiceRequired {
                    anime_suggestion: active.series_name,
                    episode_suggestion: active.episode_number.map(|value| value.to_string()),
                    matches,
                }
            };
            return Ok(DanmakuAutoOutcome(resolution));
        };

        let record = DanmakuMatchRecord {
            server_id,
            item_id: active.item_id,
            media_source_id: active.media_source_id,
            episode_id: candidate.episode_id,
            display_title: format_episode_title(&candidate.anime_title, &candidate.episode_title),
            time_offset_seconds: candidate.shift,
            updated_at: now,
        };
        self.store_danmaku_match(&record)?;
        self.loaded_danmaku_resolution(&record, now)
            .map(DanmakuAutoOutcome)
    }

    pub fn apply_danmaku(
        &self,
        item_id: &str,
        request: &DanmakuApplyRequest,
    ) -> Result<DanmakuApplyOutcome, ApplicationError> {
        let (active, server_id, now) = self.active_danmaku_context(item_id)?;
        let episode_id = (request.episode_id > 0)
            .then_some(request.episode_id)
            .ok_or_else(|| ApplicationError::invalid("choose a valid danmaku episode"))?;
        let record = DanmakuMatchRecord {
            server_id,
            item_id: active.item_id,
            media_source_id: active.media_source_id,
            episode_id,
            display_title: format_episode_title(&request.anime_title, &request.episode_title),
            time_offset_seconds: request.time_offset.clamp(-600.0, 600.0),
            updated_at: now,
        };
        self.store_danmaku_match(&record)?;
        self.loaded_danmaku_resolution(&record, now)
            .map(DanmakuApplyOutcome)
    }

    fn active_danmaku_context(
        &self,
        item_id: &str,
    ) -> Result<(ActivePlayback, String, i64), ApplicationError> {
        let active = self
            .active_playback
            .lock()
            .map_err(|_| ApplicationError::internal("playback lock is poisoned"))?
            .clone()
            .ok_or_else(|| {
                ApplicationError::new(
                    ApplicationErrorCode::NotConnected,
                    "no playback session is active",
                )
            })?;
        if active.item_id != item_id {
            return Err(ApplicationError::new(
                ApplicationErrorCode::Cancelled,
                "the danmaku request no longer belongs to the active episode",
            ));
        }
        let client = self.active_client()?;
        let server_id = client.profile().server_id.clone().unwrap_or_default();
        Ok((active, server_id, Utc::now().timestamp()))
    }

    fn stored_danmaku_match(
        &self,
        server_id: &str,
        active: &ActivePlayback,
    ) -> Result<Option<DanmakuMatchRecord>, ApplicationError> {
        self.storage
            .find_match(server_id, &active.item_id, &active.media_source_id)
            .map_err(|error| {
                ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
            })
    }

    fn store_danmaku_match(&self, record: &DanmakuMatchRecord) -> Result<(), ApplicationError> {
        self.storage.put_match(record).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
        })
    }

    fn loaded_danmaku_resolution(
        &self,
        matched: &DanmakuMatchRecord,
        now: i64,
    ) -> Result<DanmakuResolution, ApplicationError> {
        let comments = self.comments(matched.episode_id, now, false)?;
        let timeline = comments
            .comments
            .iter()
            .enumerate()
            .map(|(index, comment)| DanmakuTimelineComment {
                id: comment
                    .source_id
                    .map_or_else(|| index.to_string(), |value| value.to_string()),
                time: comment.time_seconds,
                mode: comment.mode.into(),
                color: comment.color_rgb,
                text: comment.text.clone(),
            })
            .collect();
        Ok(DanmakuResolution::Loaded {
            comments: timeline,
            title: matched.display_title.clone(),
            comment_count: comments.comments.len(),
            stale: comments.stale,
            matched_time_offset: matched.time_offset_seconds,
        })
    }

    fn comments(
        &self,
        episode_id: i64,
        now: i64,
        force_refresh: bool,
    ) -> Result<CommentLoadResult, ApplicationError> {
        let cached = self.storage.comments(episode_id).map_err(|error| {
            ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
        })?;
        if let Some(cache) = cached
            .as_ref()
            .filter(|cache| !force_refresh && cache.is_fresh_at(now))
        {
            return Ok(CommentLoadResult {
                comments: cache.comments.clone(),
                stale: false,
            });
        }
        let client = self.dandanplay_client()?;
        match self.block_on_dandan(client.fetch_comments(episode_id)) {
            Ok(comments) => {
                self.storage
                    .put_comments(&CachedComments {
                        episode_id,
                        comments: comments.clone(),
                        fetched_at: now,
                        expires_at: now + 6 * 60 * 60,
                    })
                    .map_err(|error| {
                        ApplicationError::new(ApplicationErrorCode::Storage, error.to_string())
                    })?;
                Ok(CommentLoadResult {
                    comments,
                    stale: false,
                })
            }
            Err(error) => {
                if let Some(cache) = cached.filter(|cache| cache.is_usable_stale_at(now)) {
                    Ok(CommentLoadResult {
                        comments: cache.comments,
                        stale: true,
                    })
                } else {
                    Err(error)
                }
            }
        }
    }

    fn match_dandanplay(&self, input: &MatchInput) -> Result<Vec<EpisodeMatch>, ApplicationError> {
        let client = self.dandanplay_client()?;
        self.block_on_dandan(client.match_media(input))
    }

    fn search_dandanplay_animes(
        &self,
        input: &SearchEpisodesInput,
    ) -> Result<Vec<AnimeSearchResult>, ApplicationError> {
        let client = self.dandanplay_client()?;
        self.block_on_dandan(client.search_animes(input))
    }
}

fn danmaku_search_anime(anime: AnimeSearchResult) -> DanmakuSearchAnime {
    DanmakuSearchAnime {
        anime_id: anime.anime_id,
        anime_title: anime.anime_title,
        type_description: anime.type_description,
        episodes: anime
            .episodes
            .into_iter()
            .take(200)
            .map(|episode| DanmakuSearchEpisode {
                episode_id: episode.episode_id,
                episode_title: episode.episode_title,
            })
            .collect(),
    }
}

fn format_episode_title(anime_title: &str, episode_title: &str) -> String {
    match (anime_title.trim(), episode_title.trim()) {
        ("", episode) => episode.to_owned(),
        (anime, "") => anime.to_owned(),
        (anime, episode) => format!("{anime} · {episode}"),
    }
}

fn select_confident_match<'a>(
    candidates: &'a [EpisodeMatch],
    series_name: &str,
    season_number: Option<i32>,
    episode_number: Option<i32>,
) -> Option<&'a EpisodeMatch> {
    if candidates.len() == 1 {
        return candidates.first();
    }
    let mut scored: Vec<_> = candidates
        .iter()
        .map(|candidate| {
            let mut score = title_similarity(series_name, &candidate.anime_title) * 100.0;
            if episode_number
                .is_some_and(|expected| title_numbers(&candidate.episode_title).contains(&expected))
            {
                score += 45.0;
            }
            if season_number.is_some_and(|season| {
                season > 1 && title_numbers(&candidate.anime_title).contains(&season)
            }) {
                score += 12.0;
            }
            (score, candidate)
        })
        .collect();
    scored.sort_by(|left, right| right.0.total_cmp(&left.0));
    let (best_score, best) = scored.first().copied()?;
    let runner_up = scored.get(1).map_or(f64::NEG_INFINITY, |value| value.0);
    (best_score >= 78.0 && best_score - runner_up >= 18.0).then_some(best)
}

fn normalized_title(value: &str) -> Vec<char> {
    value
        .to_lowercase()
        .chars()
        .filter(|character| {
            character.is_alphanumeric() || ('\u{4e00}'..='\u{9fff}').contains(character)
        })
        .collect()
}

fn title_similarity(left: &str, right: &str) -> f64 {
    let left = normalized_title(left);
    let right = normalized_title(right);
    if left.is_empty() || right.is_empty() {
        return 0.0;
    }
    if left == right
        || left
            .windows(right.len())
            .any(|window| window == right.as_slice())
        || right
            .windows(left.len())
            .any(|window| window == left.as_slice())
    {
        return 1.0;
    }
    let common = left
        .iter()
        .filter(|character| right.contains(character))
        .count();
    (2 * common) as f64 / (left.len() + right.len()) as f64
}

fn title_numbers(value: &str) -> Vec<i32> {
    value
        .split(|character: char| !character.is_ascii_digit())
        .filter_map(|part| (!part.is_empty()).then(|| part.parse().ok()).flatten())
        .collect()
}

#[cfg(test)]
mod tests {
    use serde_json::json;
    use yanami_danmaku::EpisodeMatch;

    use super::{
        DanmakuAutoOutcome, DanmakuMatchOption, DanmakuResolution, select_confident_match,
    };

    #[test]
    fn matcher_accepts_clear_episode_and_rejects_ambiguous_titles() {
        let candidates = vec![
            EpisodeMatch {
                episode_id: 1,
                anime_id: Some(1),
                anime_title: "尖帽子的魔法工坊".to_owned(),
                episode_title: "第 6 话".to_owned(),
                type_description: None,
                shift: 0.0,
            },
            EpisodeMatch {
                episode_id: 2,
                anime_id: Some(2),
                anime_title: "完全不同的作品".to_owned(),
                episode_title: "第 1 话".to_owned(),
                type_description: None,
                shift: 0.0,
            },
        ];
        assert_eq!(
            select_confident_match(&candidates, "尖帽子的魔法工坊", Some(1), Some(6))
                .map(|candidate| candidate.episode_id),
            Some(1)
        );

        let ambiguous = vec![
            candidates[0].clone(),
            EpisodeMatch {
                episode_id: 3,
                anime_id: Some(3),
                anime_title: "尖帽子的魔法工坊".to_owned(),
                episode_title: "第 6 话".to_owned(),
                type_description: None,
                shift: 0.0,
            },
        ];
        assert!(select_confident_match(&ambiguous, "尖帽子的魔法工坊", Some(1), Some(6)).is_none());
    }

    #[test]
    fn resolution_contract_uses_typed_tagged_variants() {
        let outcome = DanmakuAutoOutcome(DanmakuResolution::ChoiceRequired {
            anime_suggestion: "Series".to_owned(),
            episode_suggestion: Some("6".to_owned()),
            matches: vec![DanmakuMatchOption {
                episode_id: 7,
                anime_title: "Series".to_owned(),
                episode_title: "Episode 6".to_owned(),
                type_description: None,
            }],
        });
        assert_eq!(
            serde_json::to_value(outcome).unwrap(),
            json!({
                "status": "choice-required",
                "animeSuggestion": "Series",
                "episodeSuggestion": "6",
                "matches": [{
                    "episodeId": 7,
                    "animeTitle": "Series",
                    "episodeTitle": "Episode 6",
                    "typeDescription": null
                }]
            })
        );
    }
}
