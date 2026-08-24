use std::{collections::HashSet, fs, sync::Arc};

use rusqlite::Connection;
use tempfile::TempDir;

use crate::{
    CatalogItem, CatalogScope, CatalogUserState, MediaCatalog, MediaCatalogError, SyncState,
};

fn scope() -> CatalogScope {
    CatalogScope::new("profile-1", "server-1", "user-1")
}

fn item(id: &str, title: &str) -> CatalogItem {
    CatalogItem {
        id: id.to_owned(),
        item_type: "Movie".to_owned(),
        title: title.to_owned(),
        sort_title: title.to_owned(),
        ..CatalogItem::default()
    }
}

#[test]
fn grouped_search_keeps_titles_ahead_of_a_full_episode_section_and_joins_series_artwork() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let mut series = item("series", "Shared Needle");
    series.item_type = "Series".to_owned();
    series.image_tag = Some("series-poster".to_owned());
    series.primary_image_aspect_ratio = Some(0.667);
    let mut records = vec![series];
    for index in 0..60 {
        let mut episode = item(
            &format!("episode-{index:02}"),
            &format!("Shared Needle Episode {index:02}"),
        );
        episode.item_type = "Episode".to_owned();
        episode.series_id = Some("series".to_owned());
        episode.series_title = Some("Shared Needle".to_owned());
        episode.season_number = Some(1);
        episode.episode_number = Some(index + 1);
        episode.image_tag = Some(format!("episode-still-{index:02}"));
        records.push(episode);
    }
    let run = catalog.begin_sync(Some(61), 10).unwrap();
    catalog
        .upsert_page(run, &records, &[], 61, Some(61), 20)
        .unwrap();
    catalog.complete_sync(run, 30).unwrap();

    let page = catalog.search_with_limit("shared needle", 50).unwrap();

    assert_eq!(page.total_matches, 61);
    assert!(page.has_more);
    assert_eq!(page.items.len(), 51);
    assert_eq!(page.items[0].item.item_type, "Series");
    assert_eq!(page.items[0].item.id, "series");
    assert!(
        page.items[1..]
            .iter()
            .all(|hit| hit.item.item_type == "Episode")
    );
    assert_eq!(
        page.items[1].item.series_image_tag.as_deref(),
        Some("series-poster")
    );
    assert_eq!(
        page.items[1].item.series_primary_image_aspect_ratio,
        Some(0.667)
    );
}

fn typed_item(id: &str, item_type: &str, title: &str) -> CatalogItem {
    CatalogItem {
        id: id.to_owned(),
        item_type: item_type.to_owned(),
        title: title.to_owned(),
        sort_title: title.to_owned(),
        ..CatalogItem::default()
    }
}

fn item_ids(ids: &[&str]) -> HashSet<String> {
    ids.iter().map(|id| (*id).to_owned()).collect()
}

fn open_catalog(temp: &TempDir) -> MediaCatalog {
    MediaCatalog::open(temp.path().join("catalog.sqlite3"), scope()).unwrap()
}

#[test]
fn partial_page_is_immediately_searchable_and_failure_preserves_it() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let run = catalog.begin_sync(Some(2), 10).unwrap();
    catalog
        .upsert_page(run, &[item("movie-1", "First Light")], &[], 1, Some(2), 20)
        .unwrap();

    let page = catalog.search("first light").unwrap();
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].item.id, "movie-1");
    assert_eq!(page.total_matches, 1);
    assert!(!page.has_more);
    assert_eq!(page.status.state, SyncState::Running);
    assert_eq!(page.status.cached_count, 1);
    assert_eq!(page.status.total_expected, Some(2));

    catalog.fail_sync(run, "offline", 30).unwrap();
    let page = catalog.search("first light").unwrap();
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.status.state, SyncState::Failed);
    assert_eq!(page.status.last_error.as_deref(), Some("offline"));
}

#[test]
fn only_successful_full_sync_deletes_unseen_items() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);

    let first = catalog.begin_sync(Some(2), 10).unwrap();
    catalog
        .upsert_page(
            first,
            &[item("a", "Alpha"), item("b", "Beta")],
            &[],
            2,
            Some(2),
            20,
        )
        .unwrap();
    assert_eq!(catalog.complete_sync(first, 30).unwrap(), 0);

    let failed = catalog.begin_sync(Some(1), 40).unwrap();
    catalog
        .upsert_page(failed, &[item("a", "Alpha")], &[], 1, Some(1), 50)
        .unwrap();
    assert_eq!(catalog.search("beta").unwrap().total_matches, 1);
    catalog.fail_sync(failed, "temporary", 60).unwrap();
    assert_eq!(catalog.search("beta").unwrap().total_matches, 1);

    let complete = catalog.begin_sync(Some(1), 70).unwrap();
    catalog
        .upsert_page(complete, &[item("a", "Alpha")], &[], 1, Some(1), 80)
        .unwrap();
    assert_eq!(catalog.search("beta").unwrap().total_matches, 1);
    assert_eq!(catalog.complete_sync(complete, 90).unwrap(), 1);
    assert_eq!(catalog.search("beta").unwrap().total_matches, 0);
    assert_eq!(catalog.sync_status().unwrap().cached_count, 1);
}

#[test]
fn search_normalizes_full_width_full_case_fold_alias_and_pinyin() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let run = catalog.begin_sync(Some(3), 10).unwrap();
    let mut chinese = item("cn", "流浪地球");
    chinese.aliases = vec!["The Wandering Earth".to_owned()];
    let records = [
        item("sharp-s", "Straße"),
        item("wide", "ＭＯＶＩＥ"),
        chinese,
    ];
    catalog
        .upsert_page(run, &records, &[], 3, Some(3), 20)
        .unwrap();
    catalog.complete_sync(run, 30).unwrap();

    assert_eq!(
        catalog.search("STRASSE").unwrap().items[0].item.id,
        "sharp-s"
    );
    assert_eq!(catalog.search("movie").unwrap().items[0].item.id, "wide");
    assert_eq!(
        catalog.search("the wandering earth").unwrap().items[0]
            .item
            .id,
        "cn"
    );
    assert_eq!(
        catalog.search("liulangdiqiu").unwrap().items[0].item.id,
        "cn"
    );
    assert_eq!(catalog.search("lldq").unwrap().items[0].item.id, "cn");
}

#[test]
fn top_k_ranking_is_exact_then_prefix_with_stable_sort_order() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let run = catalog.begin_sync(Some(3), 10).unwrap();
    let mut exact = item("z-exact", "Alpha");
    exact.sort_title = "Z".to_owned();
    let mut able = item("a-prefix", "Alpha Able");
    able.sort_title = "A".to_owned();
    let mut beta = item("b-prefix", "Alpha Beta");
    beta.sort_title = "B".to_owned();
    catalog
        .upsert_page(run, &[beta, exact, able], &[], 3, Some(3), 20)
        .unwrap();
    catalog.complete_sync(run, 30).unwrap();

    let page = catalog.search_with_limit("alpha", 2).unwrap();
    assert_eq!(page.total_matches, 3);
    assert!(page.has_more);
    assert_eq!(page.items.len(), 2);
    assert_eq!(page.items[0].item.id, "z-exact");
    assert_eq!(page.items[0].match_rank, 0);
    assert_eq!(page.items[1].item.id, "a-prefix");
}

#[test]
fn user_state_and_search_survive_reopen() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    {
        let catalog = MediaCatalog::open(&path, scope()).unwrap();
        let run = catalog.begin_sync(Some(1), 10).unwrap();
        let state = CatalogUserState {
            item_id: "episode-1".to_owned(),
            favorite: true,
            played: false,
            resume_ticks: 42,
            progress: Some(12.5),
            unplayed_count: Some(1),
            last_played_at: Some("2026-08-24T00:00:00Z".to_owned()),
        };
        let mut episode = item("episode-1", "Persistent Episode");
        episode.item_type = "Episode".to_owned();
        catalog
            .upsert_page(run, &[episode], &[state], 1, Some(1), 20)
            .unwrap();
        catalog.complete_sync(run, 30).unwrap();
    }

    let reopened = MediaCatalog::open(&path, scope()).unwrap();
    let page = reopened.search("persistent episode").unwrap();
    assert_eq!(page.status.state, SyncState::Complete);
    assert_eq!(page.status.last_completed_run_id, Some(1));
    let state = page.items[0].user_state.as_ref().unwrap();
    assert!(state.favorite);
    assert_eq!(state.resume_ticks, 42);
    assert_eq!(state.progress, Some(12.5));
}

#[test]
fn interrupted_run_can_be_recovered_after_reopen() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let interrupted_id;
    {
        let catalog = MediaCatalog::open(&path, scope()).unwrap();
        let run = catalog.begin_sync(Some(1), 10).unwrap();
        interrupted_id = run.id();
        catalog
            .upsert_page(run, &[item("partial", "Partial")], &[], 1, Some(1), 20)
            .unwrap();
    }

    let reopened = MediaCatalog::open(&path, scope()).unwrap();
    assert_eq!(reopened.sync_status().unwrap().state, SyncState::Running);
    let recovered = reopened
        .recover_interrupted_sync("process interrupted", 30)
        .unwrap()
        .unwrap();
    assert_eq!(recovered.id(), interrupted_id);
    assert_eq!(reopened.sync_status().unwrap().state, SyncState::Failed);
    assert_eq!(reopened.search("partial").unwrap().total_matches, 1);
    let replacement = reopened.begin_sync(Some(1), 40).unwrap();
    assert!(replacement.id() > interrupted_id);
}

#[test]
fn scope_is_validated_inside_database() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    drop(MediaCatalog::open(&path, scope()).unwrap());

    let Err(error) = MediaCatalog::open(
        &path,
        CatalogScope::new("profile-1", "server-1", "another-user"),
    ) else {
        panic!("scope mismatch must fail");
    };
    assert!(matches!(error, MediaCatalogError::ScopeMismatch));
}

#[test]
fn replacing_an_item_overrides_base_without_leaving_old_terms() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let first = catalog.begin_sync(Some(1), 10).unwrap();
    catalog
        .upsert_page(
            first,
            &[item("same", "Old Search Title")],
            &[],
            1,
            Some(1),
            20,
        )
        .unwrap();
    catalog.complete_sync(first, 30).unwrap();

    let second = catalog.begin_sync(Some(1), 40).unwrap();
    catalog
        .upsert_page(
            second,
            &[item("same", "New Search Title")],
            &[],
            1,
            Some(1),
            50,
        )
        .unwrap();
    assert_eq!(catalog.search("old search").unwrap().total_matches, 0);
    assert_eq!(catalog.search("new search").unwrap().total_matches, 1);
    assert_eq!(catalog.search("ol").unwrap().total_matches, 0);
    assert_eq!(catalog.search("ne").unwrap().items[0].item.id, "same");
    catalog.complete_sync(second, 60).unwrap();
}

#[test]
fn one_and_two_scalar_substrings_use_the_short_index() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let run = catalog.begin_sync(Some(2), 10).unwrap();
    catalog
        .upsert_page(
            run,
            &[item("latin", "Blueprint"), item("han", "星河档案")],
            &[],
            2,
            Some(2),
            20,
        )
        .unwrap();
    catalog.complete_sync(run, 30).unwrap();

    assert_eq!(catalog.search("u").unwrap().items[0].item.id, "latin");
    assert_eq!(catalog.search("ep").unwrap().items[0].item.id, "latin");
    assert_eq!(catalog.search("河").unwrap().items[0].item.id, "han");
    assert_eq!(catalog.search("河档").unwrap().items[0].item.id, "han");
}

#[test]
fn media_catalog_is_shareable_and_search_readers_are_independent() {
    let temp = TempDir::new().unwrap();
    let catalog = Arc::new(open_catalog(&temp));
    let run = catalog.begin_sync(Some(1), 10).unwrap();
    catalog
        .upsert_page(
            run,
            &[item("shared", "Shared Catalog")],
            &[],
            1,
            Some(1),
            20,
        )
        .unwrap();

    let reader = Arc::clone(&catalog);
    let page = std::thread::spawn(move || reader.search("shared catalog").unwrap())
        .join()
        .unwrap();
    assert_eq!(page.items[0].item.id, "shared");
    catalog.complete_sync(run, 30).unwrap();
}

#[test]
fn duplicate_or_short_traversal_cannot_delete_unseen_items() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let first = catalog.begin_sync(Some(2), 10).unwrap();
    catalog
        .upsert_page(
            first,
            &[item("a", "Alpha"), item("b", "Beta")],
            &[],
            2,
            Some(2),
            20,
        )
        .unwrap();
    catalog.complete_sync(first, 30).unwrap();

    let duplicate = catalog.begin_sync(None, 40).unwrap();
    catalog
        .upsert_page(
            duplicate,
            &[item("a", "Alpha"), item("a", "Alpha")],
            &[],
            2,
            None,
            50,
        )
        .unwrap();
    catalog.record_sync_progress(duplicate, 2, 2, 60).unwrap();
    let error = catalog.complete_sync(duplicate, 70).unwrap_err();

    assert!(matches!(
        error,
        MediaCatalogError::ReconciliationMismatch {
            expected: Some(2),
            seen: 1
        }
    ));
    assert_eq!(catalog.search("beta").unwrap().total_matches, 1);
    let status = catalog.sync_status().unwrap();
    assert_eq!(status.state, SyncState::Failed);
    assert_eq!(status.active_run_id, None);
    assert!(
        status
            .last_error
            .as_deref()
            .is_some_and(|value| value.len() < 128 && value.contains("reconciliation failed"))
    );
}

#[test]
fn verified_completion_rejects_an_equal_count_wrong_membership() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let baseline = catalog.begin_sync(Some(3), 10).unwrap();
    catalog
        .upsert_page(
            baseline,
            &[item("a", "Alpha"), item("b", "Beta"), item("c", "Gamma")],
            &[],
            3,
            Some(3),
            20,
        )
        .unwrap();
    catalog.complete_sync(baseline, 30).unwrap();

    let run = catalog.begin_sync(Some(2), 40).unwrap();
    catalog
        .upsert_page(
            run,
            &[item("a", "Alpha"), item("b", "Beta")],
            &[],
            2,
            Some(2),
            50,
        )
        .unwrap();
    let error = catalog
        .verify_sync_membership(run, &item_ids(&["a", "c"]), 2, 60)
        .unwrap_err();
    assert!(matches!(
        error,
        MediaCatalogError::MembershipMismatch {
            remote_only: 1,
            local_only: 1
        }
    ));
    catalog.fail_sync(run, "membership changed", 70).unwrap();

    assert_eq!(catalog.search("gamma").unwrap().total_matches, 1);
    assert!(catalog.sync_status().unwrap().dirty);
}

#[test]
fn verified_completion_rejects_incomplete_probe_and_stale_revision() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let incomplete = catalog.begin_sync(Some(1), 10).unwrap();
    catalog
        .upsert_page(incomplete, &[item("a", "Alpha")], &[], 1, Some(1), 20)
        .unwrap();
    let error = catalog
        .verify_sync_membership(incomplete, &item_ids(&["a"]), 2, 30)
        .unwrap_err();
    assert!(matches!(
        error,
        MediaCatalogError::MembershipCountMismatch {
            expected: 2,
            observed: 1
        }
    ));
    catalog
        .fail_sync(incomplete, "probe incomplete", 40)
        .unwrap();

    let stale = catalog.begin_sync(Some(1), 50).unwrap();
    catalog
        .upsert_page(stale, &[item("a", "Alpha")], &[], 1, Some(1), 60)
        .unwrap();
    catalog
        .verify_sync_membership(stale, &item_ids(&["a"]), 1, 70)
        .unwrap();
    catalog.mark_dirty(80).unwrap();
    assert!(matches!(
        catalog.complete_verified_sync(stale, 90).unwrap_err(),
        MediaCatalogError::MembershipNotVerified(_)
    ));
    catalog.fail_sync(stale, "dirty during sync", 100).unwrap();
    assert!(catalog.sync_status().unwrap().dirty);
}

#[test]
fn hierarchical_local_deletion_immediately_suppresses_descendants() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let mut season_one = typed_item("season-1", "Season", "First Season");
    season_one.series_id = Some("series-1".to_owned());
    season_one.season_id = Some("season-1".to_owned());
    let mut episode_one = typed_item("episode-1", "Episode", "First Episode");
    episode_one.series_id = Some("series-1".to_owned());
    episode_one.season_id = Some("season-1".to_owned());
    let mut season_two = typed_item("season-2", "Season", "Second Season");
    season_two.series_id = Some("series-2".to_owned());
    season_two.season_id = Some("season-2".to_owned());
    let mut episode_two = typed_item("episode-2", "Episode", "Second Episode");
    episode_two.series_id = Some("series-2".to_owned());
    episode_two.season_id = Some("season-2".to_owned());
    let records = [
        typed_item("series-1", "Series", "First Series"),
        season_one,
        episode_one,
        typed_item("series-2", "Series", "Second Series"),
        season_two,
        episode_two,
        item("movie-1", "Standalone Movie"),
    ];
    let run = catalog.begin_sync(Some(records.len() as u64), 10).unwrap();
    catalog
        .upsert_page(
            run,
            &records,
            &[],
            records.len() as u64,
            Some(records.len() as u64),
            20,
        )
        .unwrap();
    catalog.complete_sync(run, 30).unwrap();

    assert!(catalog.remove_item_and_mark_dirty("season-2", 40).unwrap());
    assert_eq!(catalog.search("second series").unwrap().total_matches, 1);
    assert_eq!(catalog.search("second season").unwrap().total_matches, 0);
    assert_eq!(catalog.search("second episode").unwrap().total_matches, 0);
    assert!(catalog.remove_item_and_mark_dirty("series-1", 50).unwrap());
    assert_eq!(catalog.search("first series").unwrap().total_matches, 0);
    assert_eq!(catalog.search("first season").unwrap().total_matches, 0);
    assert_eq!(catalog.search("first episode").unwrap().total_matches, 0);
    assert_eq!(catalog.search("standalone movie").unwrap().total_matches, 1);
    let status = catalog.sync_status().unwrap();
    assert_eq!(status.cached_count, 2);
    assert!(status.dirty);
    drop(catalog);

    let reopened = open_catalog(&temp);
    assert_eq!(reopened.search("first episode").unwrap().total_matches, 0);
    assert_eq!(reopened.search("second episode").unwrap().total_matches, 0);
    assert_eq!(
        reopened.search("standalone movie").unwrap().total_matches,
        1
    );
}

#[test]
fn incremental_hierarchical_delete_survives_reopen_without_discarding_the_base() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let mut season = typed_item("season-1", "Season", "Restart Season");
    season.series_id = Some("series-1".to_owned());
    season.season_id = Some("season-1".to_owned());
    let mut episode = typed_item("episode-1", "Episode", "Restart Episode");
    episode.series_id = Some("series-1".to_owned());
    episode.season_id = Some("season-1".to_owned());
    let records = [
        typed_item("series-1", "Series", "Restart Series"),
        season,
        episode,
        item("movie-1", "Surviving Movie"),
    ];
    let catalog = MediaCatalog::open(&path, scope()).unwrap();
    let run = catalog.begin_sync(Some(records.len() as u64), 10).unwrap();
    catalog
        .upsert_page(
            run,
            &records,
            &[],
            records.len() as u64,
            Some(records.len() as u64),
            20,
        )
        .unwrap();
    catalog.complete_sync(run, 30).unwrap();
    let generation = Connection::open(&path)
        .unwrap()
        .query_row(
            "SELECT search_generation FROM catalog_scope WHERE singleton=1",
            [],
            |row| row.get::<_, i64>(0),
        )
        .unwrap();
    let sidecar = temp
        .path()
        .join(format!("catalog.sqlite3.search-{generation}.idx"));
    let baseline_revision = catalog.sync_status().unwrap().content_revision;

    assert!(catalog.remove_item_incrementally("series-1", 40).unwrap());
    let status = catalog.sync_status().unwrap();
    assert!(!status.dirty);
    assert!(status.content_revision > baseline_revision);
    assert_eq!(catalog.search("restart series").unwrap().total_matches, 0);
    assert_eq!(catalog.search("restart season").unwrap().total_matches, 0);
    assert_eq!(catalog.search("restart episode").unwrap().total_matches, 0);
    assert_eq!(catalog.search("surviving movie").unwrap().total_matches, 1);
    let deleted_revision = status.content_revision;
    drop(catalog);

    assert!(
        sidecar.exists(),
        "incremental deletion must retain the mmap base"
    );
    let reopened = MediaCatalog::open(&path, scope()).unwrap();
    assert_eq!(
        reopened.sync_status().unwrap().content_revision,
        deleted_revision
    );
    assert_eq!(reopened.search("restart series").unwrap().total_matches, 0);
    assert_eq!(reopened.search("restart season").unwrap().total_matches, 0);
    assert_eq!(reopened.search("restart episode").unwrap().total_matches, 0);
    assert_eq!(reopened.search("surviving movie").unwrap().total_matches, 1);
    let reopened_generation = Connection::open(&path)
        .unwrap()
        .query_row(
            "SELECT search_generation FROM catalog_scope WHERE singleton=1",
            [],
            |row| row.get::<_, i64>(0),
        )
        .unwrap();
    assert_eq!(reopened_generation, generation);
}

#[test]
fn pending_ack_is_fenced_from_a_newer_same_id_notification() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    catalog
        .enqueue_library_changes(&["same".to_owned()], &[], false, 10)
        .unwrap();
    let stale = catalog.pending_changes(10).unwrap();
    assert_eq!(stale.upsert_ids, ["same"]);

    catalog
        .enqueue_library_changes(&[], &["same".to_owned()], false, 20)
        .unwrap();
    catalog
        .apply_incremental_changes(&[], &[], &stale.upsert_ids, &[], stale.pending_revision, 30)
        .unwrap();
    let current = catalog.pending_changes(10).unwrap();
    assert_eq!(current.removed_ids, ["same"]);
    assert!(current.pending_revision > stale.pending_revision);

    catalog
        .apply_incremental_changes(
            &[],
            &[],
            &[],
            &current.removed_ids,
            current.pending_revision,
            40,
        )
        .unwrap();
    let drained = catalog.pending_changes(10).unwrap();
    assert!(drained.upsert_ids.is_empty());
    assert!(drained.removed_ids.is_empty());
}

#[test]
fn checkpoint_revision_fences_preserve_newer_gap_work() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let initial = catalog.pending_changes(10).unwrap();
    catalog
        .record_incremental_checkpoint(initial.catchup_revision, 10)
        .unwrap();
    catalog
        .record_membership_check(initial.membership_revision, 10)
        .unwrap();

    catalog.mark_notification_gap(20).unwrap();
    let stale = catalog.pending_changes(10).unwrap();
    catalog.mark_notification_gap(30).unwrap();
    catalog
        .record_incremental_checkpoint(stale.catchup_revision, 40)
        .unwrap();
    catalog
        .record_membership_check(stale.membership_revision, 40)
        .unwrap();
    let current = catalog.pending_changes(10).unwrap();
    assert!(current.catchup_required);
    assert!(current.membership_required);

    catalog
        .record_incremental_checkpoint(current.catchup_revision, 50)
        .unwrap();
    catalog
        .record_membership_check(current.membership_revision, 50)
        .unwrap();
    let drained = catalog.pending_changes(10).unwrap();
    assert!(!drained.catchup_required);
    assert!(!drained.membership_required);
}

#[test]
fn incremental_failure_count_is_only_cleared_by_completed_incremental_work() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);

    assert_eq!(catalog.record_incremental_failure(10).unwrap(), 1);
    assert_eq!(catalog.record_incremental_failure(20).unwrap(), 2);
    let pending = catalog.pending_changes(10).unwrap();

    catalog
        .record_incremental_checkpoint(pending.catchup_revision, 30)
        .unwrap();
    assert_eq!(catalog.pending_changes(10).unwrap().failure_count, 2);

    catalog
        .apply_incremental_changes(&[], &[], &[], &[], pending.pending_revision, 40)
        .unwrap();
    assert_eq!(catalog.pending_changes(10).unwrap().failure_count, 2);

    catalog.record_incremental_success(50).unwrap();
    assert_eq!(catalog.pending_changes(10).unwrap().failure_count, 0);

    assert_eq!(catalog.record_incremental_failure(60).unwrap(), 1);
    assert_eq!(catalog.record_incremental_failure(70).unwrap(), 2);
    let pending = catalog.pending_changes(10).unwrap();
    catalog
        .record_membership_check(pending.membership_revision, 80)
        .unwrap();
    assert_eq!(catalog.pending_changes(10).unwrap().failure_count, 0);
}

#[test]
fn explicit_final_total_completes_after_an_empty_final_stage() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let run = catalog.begin_sync(None, 10).unwrap();
    catalog
        .upsert_page(run, &[item("only", "Only Item")], &[], 1, None, 20)
        .unwrap();

    // The final remote stage may contain zero rows, so no final upsert_page is
    // available to persist its total. The explicit progress record closes that
    // semantic gap.
    catalog.record_sync_progress(run, 1, 1, 30).unwrap();
    catalog.complete_sync(run, 40).unwrap();

    let status = catalog.sync_status().unwrap();
    assert_eq!(status.total_expected, Some(1));
    assert_eq!(status.cached_count, 1);
    assert_eq!(catalog.search("only item").unwrap().total_matches, 1);
}

#[test]
fn missing_user_state_on_a_page_clears_the_stale_value() {
    let temp = TempDir::new().unwrap();
    let catalog = open_catalog(&temp);
    let first = catalog.begin_sync(Some(1), 10).unwrap();
    let state = CatalogUserState {
        item_id: "same".to_owned(),
        favorite: true,
        played: false,
        resume_ticks: 42,
        progress: Some(12.5),
        unplayed_count: Some(1),
        last_played_at: None,
    };
    catalog
        .upsert_page(first, &[item("same", "Stateful")], &[state], 1, Some(1), 20)
        .unwrap();
    catalog.complete_sync(first, 30).unwrap();
    assert!(
        catalog.search("stateful").unwrap().items[0]
            .user_state
            .is_some()
    );

    let second = catalog.begin_sync(Some(1), 40).unwrap();
    catalog
        .upsert_page(second, &[item("same", "Stateful")], &[], 1, Some(1), 50)
        .unwrap();
    assert!(
        catalog.search("stateful").unwrap().items[0]
            .user_state
            .is_none()
    );
    catalog.complete_sync(second, 60).unwrap();
}

#[test]
fn corrupt_search_sidecar_is_rebuilt_without_discarding_facts() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let sidecar = temp.path().join("catalog.sqlite3.search-1.idx");
    {
        let catalog = MediaCatalog::open(&path, scope()).unwrap();
        let run = catalog.begin_sync(Some(2), 10).unwrap();
        catalog
            .upsert_page(
                run,
                &[item("a", "Alpha Fact"), item("b", "Beta Fact")],
                &[],
                2,
                Some(2),
                20,
            )
            .unwrap();
        catalog.complete_sync(run, 30).unwrap();
    }
    assert!(sidecar.exists());
    fs::write(&sidecar, b"corrupt derived index").unwrap();

    let reopened = MediaCatalog::open(&path, scope()).unwrap();
    assert_eq!(reopened.sync_status().unwrap().cached_count, 2);
    assert_eq!(reopened.search("alpha fact").unwrap().items[0].item.id, "a");
    assert_eq!(reopened.search("beta fact").unwrap().items[0].item.id, "b");
    assert!(fs::metadata(sidecar).unwrap().len() > 21);
}

#[test]
fn older_search_sidecar_is_rebuilt_without_discarding_raw_facts() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let sidecar = temp.path().join("catalog.sqlite3.search-1.idx");
    {
        let catalog = MediaCatalog::open(&path, scope()).unwrap();
        let run = catalog.begin_sync(Some(2), 10).unwrap();
        catalog
            .upsert_page(
                run,
                &[
                    item("old-a", "Alpha Retained"),
                    item("old-b", "Beta Retained"),
                ],
                &[],
                2,
                Some(2),
                20,
            )
            .unwrap();
        catalog.complete_sync(run, 30).unwrap();
    }
    let mut old_sidecar = fs::read(&sidecar).unwrap();
    assert_eq!(&old_sidecar[..8], b"YMCIDX06");
    old_sidecar[..8].copy_from_slice(b"YMCIDX05");
    fs::write(&sidecar, &old_sidecar).unwrap();

    let reopened = MediaCatalog::open(&path, scope()).unwrap();
    assert_eq!(reopened.sync_status().unwrap().cached_count, 2);
    assert_eq!(
        reopened.search("alpha retained").unwrap().items[0].item.id,
        "old-a"
    );
    assert_eq!(
        reopened.search("beta retained").unwrap().items[0].item.id,
        "old-b"
    );
    assert_eq!(&fs::read(sidecar).unwrap()[..8], b"YMCIDX06");
}

#[test]
fn parallel_rebuild_drains_workers_after_invalid_raw_alias_json() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let sidecar = temp.path().join("catalog.sqlite3.search-1.idx");
    {
        let catalog = MediaCatalog::open(&path, scope()).unwrap();
        let items = (0..2_048)
            .map(|index| item(&format!("item-{index:04}"), &format!("Title {index:04}")))
            .collect::<Vec<_>>();
        let run = catalog.begin_sync(Some(items.len() as u64), 10).unwrap();
        catalog
            .upsert_page(
                run,
                &items,
                &[],
                items.len() as u64,
                Some(items.len() as u64),
                20,
            )
            .unwrap();
        catalog.complete_sync(run, 30).unwrap();
    }
    let connection = Connection::open(&path).unwrap();
    connection
        .execute(
            "UPDATE catalog_items SET aliases_json='not-json' WHERE item_id='item-0000'",
            [],
        )
        .unwrap();
    drop(connection);
    fs::remove_file(sidecar).unwrap();

    assert!(matches!(
        MediaCatalog::open(&path, scope()),
        Err(MediaCatalogError::Json(_))
    ));
    let orphaned = fs::read_dir(temp.path())
        .unwrap()
        .filter_map(Result::ok)
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|name| {
            name.contains(".idx.refs-") || name.contains(".idx.docs-") || name.contains(".idx.tmp-")
        })
        .collect::<Vec<_>>();
    assert!(
        orphaned.is_empty(),
        "orphaned sidecar scratch: {orphaned:?}"
    );
}

#[test]
fn checksum_detects_a_structurally_valid_string_flip() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let sidecar = temp.path().join("catalog.sqlite3.search-1.idx");
    {
        let catalog = MediaCatalog::open(&path, scope()).unwrap();
        let run = catalog.begin_sync(Some(1), 10).unwrap();
        catalog
            .upsert_page(run, &[item("a", "Alpha Checksum")], &[], 1, Some(1), 20)
            .unwrap();
        catalog.complete_sync(run, 30).unwrap();
    }

    let mut bytes = fs::read(&sidecar).unwrap();
    let offset = bytes
        .windows(b"alpha".len())
        .position(|window| window == b"alpha")
        .expect("front-coded dictionary retains the first value verbatim");
    bytes[offset] = b'B';
    fs::write(&sidecar, &bytes).unwrap();

    let reopened = MediaCatalog::open(&path, scope()).unwrap();
    assert_eq!(reopened.sync_status().unwrap().cached_count, 1);
    assert_eq!(reopened.search("alpha checksum").unwrap().total_matches, 1);
    assert_eq!(reopened.search("blpha checksum").unwrap().total_matches, 0);
    assert_ne!(fs::read(sidecar).unwrap(), bytes);
}

#[test]
fn a_sidecar_copied_from_another_database_is_rebuilt_from_local_facts() {
    let temp = TempDir::new().unwrap();
    let first_path = temp.path().join("first.sqlite3");
    let second_path = temp.path().join("second.sqlite3");
    let first_sidecar = temp.path().join("first.sqlite3.search-1.idx");
    let second_sidecar = temp.path().join("second.sqlite3.search-1.idx");
    {
        let first = MediaCatalog::open(&first_path, scope()).unwrap();
        let run = first.begin_sync(Some(1), 10).unwrap();
        first
            .upsert_page(run, &[item("same", "First Database")], &[], 1, Some(1), 20)
            .unwrap();
        first.complete_sync(run, 30).unwrap();
    }
    {
        let second = MediaCatalog::open(&second_path, scope()).unwrap();
        let run = second.begin_sync(Some(1), 10).unwrap();
        second
            .upsert_page(run, &[item("same", "Second Database")], &[], 1, Some(1), 20)
            .unwrap();
        second.complete_sync(run, 30).unwrap();
    }
    fs::copy(first_sidecar, &second_sidecar).unwrap();

    let reopened = MediaCatalog::open(&second_path, scope()).unwrap();
    assert_eq!(reopened.sync_status().unwrap().cached_count, 1);
    assert_eq!(reopened.search("first database").unwrap().total_matches, 0);
    assert_eq!(reopened.search("second database").unwrap().total_matches, 1);
}

#[test]
fn disposable_cleanup_is_exactly_scoped_to_one_catalog_path() {
    let temp = TempDir::new().unwrap();
    let first = temp.path().join("catalog-first.sqlite3");
    let second = temp.path().join("catalog-second.sqlite3");
    let first_sidecar = temp.path().join("catalog-first.sqlite3.search-7.idx");
    let first_temp = temp
        .path()
        .join("catalog-first.sqlite3.search-8.idx.tmp-dead");
    let first_refs_temp = temp
        .path()
        .join("catalog-first.sqlite3.search-8.idx.refs-dead");
    let second_sidecar = temp.path().join("catalog-second.sqlite3.search-7.idx");
    let similarly_named = temp.path().join("catalog-first.sqlite3.search-keep.txt");
    for path in [
        &first,
        &first_sidecar,
        &first_temp,
        &first_refs_temp,
        &second,
        &second_sidecar,
        &similarly_named,
    ] {
        fs::write(path, b"test").unwrap();
    }

    MediaCatalog::remove_disposable_files(&first).unwrap();

    assert!(!first.exists());
    assert!(!first_sidecar.exists());
    assert!(!first_temp.exists());
    assert!(!first_refs_temp.exists());
    assert!(second.exists());
    assert!(second_sidecar.exists());
    assert!(similarly_named.exists());
}

#[test]
fn fact_only_full_sweep_reuses_base_but_search_change_publishes_a_generation() {
    let temp = TempDir::new().unwrap();
    let path = temp.path().join("catalog.sqlite3");
    let generation_one = temp.path().join("catalog.sqlite3.search-1.idx");
    let generation_two = temp.path().join("catalog.sqlite3.search-2.idx");
    let generation_three = temp.path().join("catalog.sqlite3.search-3.idx");
    let catalog = MediaCatalog::open(&path, scope()).unwrap();

    let first = catalog.begin_sync(Some(1), 10).unwrap();
    catalog
        .upsert_page(first, &[item("same", "Stable Search")], &[], 1, Some(1), 20)
        .unwrap();
    catalog.complete_sync(first, 30).unwrap();
    assert!(generation_one.exists());

    let fact_only = catalog.begin_sync(Some(1), 40).unwrap();
    let mut fact_only_item = item("same", "Stable Search");
    fact_only_item.image_tag = Some("updated-image".to_owned());
    catalog
        .upsert_page(fact_only, &[fact_only_item], &[], 1, Some(1), 50)
        .unwrap();
    catalog.complete_sync(fact_only, 60).unwrap();
    assert!(generation_one.exists());
    assert!(!generation_two.exists());
    assert_eq!(
        catalog.search("stable search").unwrap().items[0]
            .item
            .image_tag
            .as_deref(),
        Some("updated-image")
    );

    let changed = catalog.begin_sync(Some(1), 70).unwrap();
    catalog
        .upsert_page(
            changed,
            &[item("same", "Changed Search")],
            &[],
            1,
            Some(1),
            80,
        )
        .unwrap();
    catalog.complete_sync(changed, 90).unwrap();
    assert!(!generation_one.exists());
    assert!(generation_three.exists());
    assert_eq!(catalog.search("stable search").unwrap().total_matches, 0);
    assert_eq!(catalog.search("changed search").unwrap().total_matches, 1);
}
