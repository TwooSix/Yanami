use serde::Deserialize;
use yanami_core::{DanmakuComment, DanmakuMode};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MatchInput {
    pub file_name: String,
    pub file_hash: String,
    pub file_size: u64,
    pub video_duration_seconds: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchEpisodesInput {
    pub anime: String,
    pub episode: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EpisodeSearchResult {
    pub anime_id: i64,
    pub anime_title: String,
    pub type_description: Option<String>,
    pub episode_id: i64,
    pub episode_title: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AnimeSearchResult {
    pub anime_id: i64,
    pub anime_title: String,
    pub type_description: Option<String>,
    pub episodes: Vec<AnimeEpisodeSearchResult>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AnimeEpisodeSearchResult {
    pub episode_id: i64,
    pub episode_title: String,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EpisodeMatch {
    pub episode_id: i64,
    #[serde(default)]
    pub anime_id: Option<i64>,
    #[serde(default, deserialize_with = "null_default")]
    pub anime_title: String,
    #[serde(default, deserialize_with = "null_default")]
    pub episode_title: String,
    #[serde(default)]
    pub type_description: Option<String>,
    #[serde(default)]
    pub shift: f64,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct MatchResponse {
    pub success: bool,
    #[serde(default)]
    pub error_code: Option<i32>,
    #[serde(default)]
    pub error_message: Option<String>,
    #[serde(default)]
    pub is_matched: bool,
    #[serde(default, deserialize_with = "null_default")]
    pub matches: Vec<EpisodeMatch>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct CommentResponse {
    #[serde(default = "default_true")]
    pub success: bool,
    #[serde(default)]
    pub error_code: Option<i32>,
    #[serde(default)]
    pub error_message: Option<String>,
    #[serde(default, deserialize_with = "null_default")]
    pub comments: Vec<RawComment>,
}

#[derive(Debug, Deserialize)]
pub(crate) struct RawComment {
    #[serde(default)]
    pub cid: Option<i64>,
    #[serde(default)]
    pub p: Option<String>,
    #[serde(rename = "m")]
    #[serde(default)]
    pub text: Option<String>,
}

impl RawComment {
    pub fn parse(self) -> Option<DanmakuComment> {
        let parameters = self.p?;
        let mut parts = parameters.splitn(4, ',');
        let time_seconds = parts.next()?.parse().ok()?;
        let mode = match parts.next()?.parse::<u8>().ok()? {
            4 => DanmakuMode::Bottom,
            5 => DanmakuMode::Top,
            _ => DanmakuMode::Scroll,
        };
        let color_rgb = parts.next()?.parse().ok()?;
        let sender = parts
            .next()
            .filter(|value| !value.is_empty())
            .map(str::to_owned);
        Some(DanmakuComment {
            time_seconds,
            mode,
            color_rgb,
            text: self.text?,
            source_id: self.cid,
            sender,
        })
    }
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SearchEpisodesResponse {
    pub success: bool,
    #[serde(default)]
    pub error_code: Option<i32>,
    #[serde(default)]
    pub error_message: Option<String>,
    #[serde(default, deserialize_with = "null_default")]
    pub animes: Vec<SearchEpisodesAnime>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SearchEpisodesAnime {
    pub anime_id: i64,
    #[serde(default, deserialize_with = "null_default")]
    pub anime_title: String,
    #[serde(default)]
    pub type_description: Option<String>,
    #[serde(default, deserialize_with = "null_default")]
    pub episodes: Vec<SearchEpisodeDetails>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SearchEpisodeDetails {
    pub episode_id: i64,
    #[serde(default, deserialize_with = "null_default")]
    pub episode_title: String,
}

fn default_true() -> bool {
    true
}

fn null_default<'de, D, T>(deserializer: D) -> Result<T, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Deserialize<'de> + Default,
{
    Option::<T>::deserialize(deserializer).map(Option::unwrap_or_default)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_nullable_titles_and_fractional_shift() {
        let response: MatchResponse = serde_json::from_str(
            r#"{"success":true,"isMatched":true,"matches":[{"episodeId":7,"animeTitle":null,"episodeTitle":null,"shift":-1.25}]}"#,
        )
        .unwrap();

        assert_eq!(response.matches[0].anime_title, "");
        assert!((response.matches[0].shift - -1.25).abs() < f64::EPSILON);
    }

    #[test]
    fn accepts_current_comment_response_without_response_base() {
        let response: CommentResponse = serde_json::from_str(
            r#"{"count":1,"comments":[{"cid":3,"p":"1.5,1,16777215,user","m":"hello"}]}"#,
        )
        .unwrap();

        assert!(response.success);
        assert_eq!(response.comments.len(), 1);
        assert_eq!(
            response
                .comments
                .into_iter()
                .next()
                .unwrap()
                .parse()
                .unwrap()
                .text,
            "hello"
        );
    }
}
