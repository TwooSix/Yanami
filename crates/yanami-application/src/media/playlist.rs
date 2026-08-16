use serde::{Deserialize, Serialize};
use yanami_emby::BaseItem;

use crate::{Application, ApplicationError};

use super::{MediaOutcome, network_error};

pub type PlaylistTargetsOutcome = MediaOutcome<PlaylistTargetsResult>;
pub type AddToPlaylistOutcome = MediaOutcome<AddToPlaylistResult>;
pub type RemoveFromPlaylistOutcome = MediaOutcome<RemoveFromPlaylistResult>;

#[derive(Clone, Debug, Serialize)]
pub struct PlaylistTargetsResult {
    options: Vec<PlaylistTarget>,
}

#[derive(Clone, Debug, Serialize)]
pub struct PlaylistTarget {
    id: String,
    title: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AddToPlaylistResult {
    target_id: String,
    created: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
#[allow(clippy::struct_field_names)] // These names are the stable schema 8 response fields.
pub struct RemoveFromPlaylistResult {
    playlist_id: String,
    removed_playlist_entry_id: String,
    removed_item_id: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AddToPlaylistRequest {
    #[serde(default)]
    pub target_id: Option<String>,
    #[serde(default)]
    pub new_name: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoveFromPlaylistRequest {
    pub playlist_id: String,
    pub entry_id: String,
}

impl Application {
    pub fn playlist_targets(
        &self,
        item_id: &str,
    ) -> Result<PlaylistTargetsOutcome, ApplicationError> {
        let client = self.active_client()?;
        let targets = self.block_on_emby(client.editable_playlists())?;
        Ok(MediaOutcome::read(
            item_id,
            PlaylistTargetsResult {
                options: media_target_options(targets.items),
            },
        ))
    }

    pub fn add_to_playlist(
        &self,
        item_id: &str,
        request: &AddToPlaylistRequest,
    ) -> Result<AddToPlaylistOutcome, ApplicationError> {
        let (target_id, new_name) = media_target_selection(request)?;
        let client = self.active_client()?;
        let result = self.block_on(async {
            if let Some(target_id) = target_id {
                client
                    .add_to_playlist(&target_id, item_id)
                    .await
                    .map_err(network_error)?;
                Ok::<_, ApplicationError>(AddToPlaylistResult {
                    target_id,
                    created: false,
                })
            } else if let Some(new_name) = new_name {
                let target_id = client
                    .create_playlist(&new_name, item_id)
                    .await
                    .map_err(network_error)?;
                Ok(AddToPlaylistResult {
                    target_id,
                    created: true,
                })
            } else {
                unreachable!("media_target_selection requires one destination")
            }
        })?;
        Ok(MediaOutcome::invalidated(item_id, result, &["collection"]))
    }

    pub fn remove_from_playlist(
        &self,
        item_id: &str,
        request: &RemoveFromPlaylistRequest,
    ) -> Result<RemoveFromPlaylistOutcome, ApplicationError> {
        let playlist_id = required_identifier(&request.playlist_id, "playlistId")?;
        let entry_id = required_identifier(&request.entry_id, "entryId")?;
        let client = self.active_client()?;
        let result = self.block_on(async {
            let playlist = client.item(&playlist_id).await.map_err(network_error)?;
            if playlist.item_type.as_deref() != Some("Playlist")
                || playlist.can_edit_items != Some(true)
            {
                return Err(ApplicationError::permission_denied(
                    "the active Emby user cannot edit this playlist",
                ));
            }
            client
                .remove_playlist_entry(&playlist_id, &entry_id)
                .await
                .map_err(network_error)?;
            Ok::<_, ApplicationError>(RemoveFromPlaylistResult {
                playlist_id,
                removed_playlist_entry_id: entry_id,
                removed_item_id: item_id.to_owned(),
            })
        })?;
        Ok(MediaOutcome::invalidated(item_id, result, &["collection"]))
    }
}

fn media_target_options(items: Vec<BaseItem>) -> Vec<PlaylistTarget> {
    items
        .into_iter()
        .map(|item| PlaylistTarget {
            id: item.id,
            title: item.name,
        })
        .collect()
}

fn media_target_selection(
    request: &AddToPlaylistRequest,
) -> Result<(Option<String>, Option<String>), ApplicationError> {
    let target_id = request
        .target_id
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned);
    let new_name = request
        .new_name
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned);
    if target_id.is_some() == new_name.is_some() {
        return Err(ApplicationError::invalid(
            "choose one existing destination or enter one new name",
        ));
    }
    if target_id
        .as_ref()
        .is_some_and(|identifier| identifier.chars().count() > 256)
    {
        return Err(ApplicationError::invalid("targetId is too long"));
    }
    if new_name
        .as_ref()
        .is_some_and(|name| name.chars().count() > 200)
    {
        return Err(ApplicationError::invalid(
            "the destination name cannot exceed 200 characters",
        ));
    }
    Ok((target_id, new_name))
}

fn required_identifier(value: &str, key: &str) -> Result<String, ApplicationError> {
    let value = value.trim();
    if value.is_empty() {
        return Err(ApplicationError::invalid(format!("{key} is required")));
    }
    if value.chars().count() > 256 {
        return Err(ApplicationError::invalid(format!("{key} is too long")));
    }
    Ok(value.to_owned())
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::{
        AddToPlaylistRequest, PlaylistTarget, PlaylistTargetsResult, media_target_selection,
    };
    use crate::{ApplicationErrorCode, media::MediaOutcome};

    #[test]
    fn rejects_ambiguous_playlist_targets_with_typed_error() {
        assert_eq!(
            media_target_selection(&AddToPlaylistRequest {
                target_id: Some(" playlist-1 ".to_owned()),
                new_name: None,
            })
            .unwrap(),
            (Some("playlist-1".to_owned()), None)
        );
        let error = media_target_selection(&AddToPlaylistRequest {
            target_id: Some("playlist-1".to_owned()),
            new_name: Some("Duplicate choice".to_owned()),
        })
        .unwrap_err();
        assert_eq!(error.code(), ApplicationErrorCode::InvalidInput);
    }

    #[test]
    fn playlist_outcome_contract_keeps_named_options() {
        let outcome = MediaOutcome::read(
            "episode-1",
            PlaylistTargetsResult {
                options: vec![PlaylistTarget {
                    id: "playlist-1".to_owned(),
                    title: "Queue".to_owned(),
                }],
            },
        );
        assert_eq!(
            serde_json::to_value(outcome).unwrap(),
            json!({
                "itemId": "episode-1",
                "result": {"options": [{"id": "playlist-1", "title": "Queue"}]}
            })
        );
    }
}
