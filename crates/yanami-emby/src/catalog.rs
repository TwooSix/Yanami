use serde_json::Value;

use crate::{
    client::{BROWSE_FIELDS, EmbyClient, EmbyError, ITEM_FIELDS, ItemQuery},
    models::{BaseItem, ItemsResult, UserDto},
    transport::decode,
};

impl EmbyClient {
    pub async fn items(&self, query: &ItemQuery) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let path = format!("Users/{user_id}/Items");
        let mut request = self.request(reqwest::Method::GET, &path);
        let include_types = query.include_item_types.join(",");
        let fields = query
            .fields
            .as_ref()
            .map_or_else(|| BROWSE_FIELDS.to_owned(), |fields| fields.join(","));
        let mut params = vec![
            ("Recursive", query.recursive.to_string()),
            ("StartIndex", query.start_index.to_string()),
            ("Limit", query.limit.max(1).to_string()),
            ("Fields", fields),
            (
                "EnableImages",
                query.enable_images.unwrap_or(true).to_string(),
            ),
            ("ImageTypeLimit", "1".to_owned()),
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
            (
                "EnableUserData",
                query.enable_user_data.unwrap_or(true).to_string(),
            ),
        ];
        if let Some(parent_id) = &query.parent_id {
            params.push(("ParentId", parent_id.clone()));
        }
        if let Some(search_term) = &query.search_term {
            params.push(("SearchTerm", search_term.clone()));
        }
        if !include_types.is_empty() {
            params.push(("IncludeItemTypes", include_types));
        }
        if !query.sort_by.is_empty() {
            params.push(("SortBy", query.sort_by.join(",")));
        }
        if let Some(sort_order) = &query.sort_order {
            params.push(("SortOrder", sort_order.clone()));
        }
        if !query.filters.is_empty() {
            params.push(("Filters", query.filters.join(",")));
        }
        if let Some(can_edit_items) = query.can_edit_items {
            params.push(("CanEditItems", can_edit_items.to_string()));
        }
        if let Some(min_date_last_saved) = &query.min_date_last_saved {
            params.push(("MinDateLastSaved", min_date_last_saved.clone()));
        }
        if let Some(min_date_last_saved_for_user) = &query.min_date_last_saved_for_user {
            params.push((
                "MinDateLastSavedForUser",
                min_date_last_saved_for_user.clone(),
            ));
        }
        if !query.ids.is_empty() {
            params.push(("Ids", query.ids.join(",")));
        }
        request = request.query(&params);
        decode(request.send().await?).await
    }

    pub async fn user_views(&self) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(reqwest::Method::GET, &format!("Users/{user_id}/Views"))
                .query(&[("IncludeExternalContent", "false")])
                .send()
                .await?,
        )
        .await
    }

    pub async fn current_user(&self) -> Result<UserDto, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(reqwest::Method::GET, &format!("Users/{user_id}"))
                .send()
                .await?,
        )
        .await
    }

    pub async fn raw_item(&self, item_id: &str) -> Result<Value, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(
                reqwest::Method::GET,
                &format!("Users/{user_id}/Items/{item_id}"),
            )
            .send()
            .await?,
        )
        .await
    }

    /// Returns the item-specific editor configuration exposed by Emby.
    ///
    /// In particular, `ExternalIdInfos` is assembled by the server from the
    /// metadata providers installed for this item type. Keeping this dynamic
    /// avoids assuming that every server has exactly `IMDb`, `TMDb` and `TVDb`.
    pub async fn metadata_editor_info(&self, item_id: &str) -> Result<Value, EmbyError> {
        decode(
            self.request(
                reqwest::Method::GET,
                &format!("Items/{item_id}/MetadataEditor"),
            )
            .send()
            .await?,
        )
        .await
    }

    pub async fn latest_items(
        &self,
        include_item_types: &[&str],
        limit: u32,
        group_items: bool,
    ) -> Result<Vec<BaseItem>, EmbyError> {
        self.latest_items_query(None, include_item_types, limit, group_items, None)
            .await
    }

    /// Returns the server-owned Latest Media row for one user library.
    ///
    /// Keeping `IncludeItemTypes` unset is intentional: the library view owns
    /// its media types, while `GroupItems=true` lets Emby return a Series
    /// container for newly-added episodes instead of individual Episode DTOs.
    pub async fn latest_items_for_parent(
        &self,
        parent_id: &str,
        limit: u32,
        hide_played: bool,
    ) -> Result<Vec<BaseItem>, EmbyError> {
        self.latest_items_query(
            Some(parent_id),
            &[],
            limit,
            true,
            hide_played.then_some(false),
        )
        .await
    }

    async fn latest_items_query(
        &self,
        parent_id: Option<&str>,
        include_item_types: &[&str],
        limit: u32,
        group_items: bool,
        is_played: Option<bool>,
    ) -> Result<Vec<BaseItem>, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let mut parameters = vec![
            ("Limit", limit.max(1).to_string()),
            ("Fields", BROWSE_FIELDS.to_owned()),
            ("GroupItems", group_items.to_string()),
            ("EnableImages", "true".to_owned()),
            ("ImageTypeLimit", "1".to_owned()),
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
            ("EnableUserData", "true".to_owned()),
        ];
        if let Some(parent_id) = parent_id {
            parameters.push(("ParentId", parent_id.to_owned()));
        }
        if !include_item_types.is_empty() {
            parameters.push(("IncludeItemTypes", include_item_types.join(",")));
        }
        if let Some(is_played) = is_played {
            parameters.push(("IsPlayed", is_played.to_string()));
        }
        let response = self
            .request(
                reqwest::Method::GET,
                &format!("Users/{user_id}/Items/Latest"),
            )
            .query(&parameters)
            .send()
            .await?;
        decode(response).await
    }

    pub async fn seasons(&self, series_id: &str) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let response = self
            .request(reqwest::Method::GET, &format!("Shows/{series_id}/Seasons"))
            .query(&[
                ("UserId", user_id.to_owned()),
                ("Fields", BROWSE_FIELDS.to_owned()),
                ("EnableImages", "true".to_owned()),
                ("ImageTypeLimit", "1".to_owned()),
                ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
                ("EnableUserData", "true".to_owned()),
            ])
            .send()
            .await?;
        decode(response).await
    }

    pub async fn episodes(
        &self,
        series_id: &str,
        season_id: Option<&str>,
    ) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let mut parameters = vec![
            ("UserId", user_id.to_owned()),
            ("Fields", BROWSE_FIELDS.to_owned()),
            ("EnableImages", "true".to_owned()),
            ("ImageTypeLimit", "1".to_owned()),
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
            ("EnableUserData", "true".to_owned()),
        ];
        if let Some(season_id) = season_id {
            parameters.push(("SeasonId", season_id.to_owned()));
        }
        let response = self
            .request(reqwest::Method::GET, &format!("Shows/{series_id}/Episodes"))
            .query(&parameters)
            .send()
            .await?;
        decode(response).await
    }

    pub async fn next_up(
        &self,
        series_id: Option<&str>,
        limit: u32,
    ) -> Result<ItemsResult, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        let mut request = self.request(reqwest::Method::GET, "Shows/NextUp").query(&[
            ("UserId", user_id.to_owned()),
            ("Limit", limit.max(1).to_string()),
            ("Fields", BROWSE_FIELDS.to_owned()),
            ("EnableImages", "true".to_owned()),
            ("ImageTypeLimit", "1".to_owned()),
            ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
            ("EnableUserData", "true".to_owned()),
        ]);
        if let Some(series_id) = series_id {
            request = request.query(&[("SeriesId", series_id)]);
        }
        let response = request.send().await?;
        decode(response).await
    }

    pub async fn item(&self, item_id: &str) -> Result<BaseItem, EmbyError> {
        let user_id = self.user_id.as_deref().unwrap_or_default();
        decode(
            self.request(
                reqwest::Method::GET,
                &format!("Users/{user_id}/Items/{item_id}"),
            )
            .query(&[
                ("Fields", ITEM_FIELDS.to_owned()),
                ("EnableImages", "true".to_owned()),
                ("ImageTypeLimit", "1".to_owned()),
                ("EnableImageTypes", "Primary,Thumb,Backdrop".to_owned()),
                ("EnableUserData", "true".to_owned()),
            ])
            .send()
            .await?,
        )
        .await
    }
}
