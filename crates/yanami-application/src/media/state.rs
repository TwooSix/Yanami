use serde::Serialize;
use yanami_emby::{BaseItem, EmbyClient, UserItemData};

use crate::{Application, ApplicationError, presentation::source_version};

use super::{MediaOutcome, network_error};

pub type SetPlayedOutcome = MediaOutcome<SetPlayedResult>;
pub type SetFavoriteOutcome = MediaOutcome<SetFavoriteResult>;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetPlayedResult {
    affected_items: Vec<AffectedItemState>,
    reconcile_complete: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetFavoriteResult {
    affected_items: Vec<AffectedItemState>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
#[allow(clippy::option_option)] // Omitted, explicit null, and concrete values are distinct patches.
pub struct AffectedItemState {
    id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    series_id: Option<Option<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    season_id: Option<Option<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    source_updated_at: Option<Option<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    source_version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    played: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    favorite: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    resume_ticks: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    progress: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    unplayed_count: Option<u32>,
}

impl AffectedItemState {
    fn from_user_data(item_id: &str, user_data: &UserItemData) -> Self {
        Self {
            id: item_id.to_owned(),
            series_id: None,
            season_id: None,
            source_updated_at: None,
            source_version: None,
            played: Some(user_data.played),
            favorite: Some(user_data.is_favorite),
            resume_ticks: Some(user_data.playback_position_ticks),
            progress: user_data.played_percentage,
            unplayed_count: user_data.unplayed_item_count,
        }
    }

    fn with_parent_ids(mut self, item: &BaseItem) -> Self {
        self.series_id = Some(item.series_id.clone());
        self.season_id = Some(item.season_id.clone());
        self
    }

    fn from_item(item: &BaseItem) -> Self {
        let user_data = item.user_data.as_ref();
        Self {
            id: item.id.clone(),
            series_id: Some(item.series_id.clone()),
            season_id: Some(item.season_id.clone()),
            source_updated_at: Some(item.date_last_saved.clone()),
            source_version: Some(source_version(item)),
            played: user_data.map(|data| data.played),
            favorite: user_data.map(|data| data.is_favorite),
            resume_ticks: user_data.map(|data| data.playback_position_ticks),
            progress: user_data.and_then(|data| data.played_percentage),
            unplayed_count: user_data.and_then(|data| data.unplayed_item_count),
        }
    }
}

impl Application {
    pub fn set_played(
        &self,
        item_id: &str,
        played: bool,
    ) -> Result<SetPlayedOutcome, ApplicationError> {
        let client = self.active_client()?;
        let result = self.block_on(async {
            let user_data = client
                .set_played(item_id, played)
                .await
                .map_err(network_error)?;
            let (affected_items, reconcile_complete) =
                authoritative_item_states(&client, item_id, user_data).await;
            Ok::<_, ApplicationError>(SetPlayedResult {
                affected_items,
                reconcile_complete,
            })
        })?;
        Ok(MediaOutcome::invalidated(
            item_id,
            result,
            &["activity", "collection"],
        ))
    }

    pub fn set_favorite(
        &self,
        item_id: &str,
        favorite: bool,
    ) -> Result<SetFavoriteOutcome, ApplicationError> {
        let client = self.active_client()?;
        let user_data = self.block_on_emby(client.set_favorite(item_id, favorite))?;
        let result = SetFavoriteResult {
            affected_items: vec![AffectedItemState::from_user_data(item_id, &user_data)],
        };
        Ok(MediaOutcome::invalidated(item_id, result, &["favorites"]))
    }
}

async fn authoritative_item_states(
    client: &EmbyClient,
    item_id: &str,
    mutation_state: UserItemData,
) -> (Vec<AffectedItemState>, bool) {
    let mut states = vec![AffectedItemState::from_user_data(item_id, &mutation_state)];
    let item = match client.item(item_id).await {
        Ok(item) => item,
        Err(error) => {
            tracing::warn!(item_id, error = %error, "play-state reconciliation failed");
            return (states, false);
        }
    };
    let target = states
        .pop()
        .expect("target state is initialized")
        .with_parent_ids(&item);
    states.push(target);
    let mut parent_ids = Vec::new();
    if let Some(season_id) = item
        .season_id
        .as_deref()
        .filter(|season_id| *season_id != item_id)
    {
        parent_ids.push(season_id.to_owned());
    }
    if let Some(series_id) = item
        .series_id
        .as_deref()
        .filter(|series_id| *series_id != item_id && !parent_ids.iter().any(|id| id == *series_id))
    {
        parent_ids.push(series_id.to_owned());
    }
    let mut complete = true;
    for parent_id in parent_ids {
        match client.item(&parent_id).await {
            Ok(parent) => {
                if parent
                    .user_data
                    .as_ref()
                    .and_then(|data| data.unplayed_item_count)
                    .is_none()
                {
                    complete = false;
                    tracing::warn!(item_id, parent_id, "parent play-state count missing");
                }
                states.push(AffectedItemState::from_item(&parent));
            }
            Err(error) => {
                complete = false;
                tracing::warn!(item_id, parent_id, error = %error, "parent play-state reconciliation failed");
            }
        }
    }
    (states, complete)
}

#[cfg(test)]
mod tests {
    use serde_json::json;
    use yanami_emby::UserItemData;

    use super::{AffectedItemState, SetFavoriteResult};

    #[test]
    fn favorite_outcome_contract_keeps_result_typed_and_wire_compatible() {
        let favorite = SetFavoriteResult {
            affected_items: vec![AffectedItemState::from_user_data(
                "episode-1",
                &UserItemData {
                    is_favorite: true,
                    ..UserItemData::default()
                },
            )],
        };
        assert_eq!(
            serde_json::to_value(favorite).unwrap(),
            json!({"affectedItems": [{
                "id": "episode-1", "played": false, "favorite": true, "resumeTicks": 0
            }]})
        );
    }
}
