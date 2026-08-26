use std::{
    collections::{BTreeMap, HashMap, HashSet},
    error::Error,
    fs,
    io::{BufRead, BufReader},
    path::Path,
    sync::{
        Arc, Barrier,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

#[cfg(any(target_os = "windows", target_os = "linux"))]
use std::process::Command;

use pinyin::ToPinyin;
use serde::Deserialize;
use serde_json::{Value, json};
use unicode_casefold::UnicodeCaseFold;
use unicode_normalization::UnicodeNormalization;
use yanami_storage::{CatalogItem, CatalogScope, MediaCatalog, MediaCatalogError};

use crate::Measurement;

const SOURCE_ITEM_COUNT: usize = 110_000;
const DERIVED_SEASON_COUNT: usize = 30_000;
const SEARCHABLE_ENTITY_COUNT: usize = SOURCE_ITEM_COUNT + DERIVED_SEASON_COUNT;
const QUERY_COUNT: usize = 20_000;
const CATALOG_PAGE_SIZE: usize = 500;
const SEARCH_GROUP_LIMIT: usize = 50;
const SEARCH_RESULT_LIMIT: usize = SEARCH_GROUP_LIMIT * 2;
const INCREMENTAL_ITEM_COUNT: usize = 1_000;
const STRICT_OPERATION_SAMPLES: usize = 10;
const REBUILD_OPERATION_SAMPLES: usize = 10;
const CONCURRENT_REBUILD_SAMPLES: usize = 3;
const CONCURRENT_QUERY_WORKLOAD_SIZE: usize = 17;

type ProbeResult<T> = Result<T, Box<dyn Error>>;

pub(crate) struct SearchProbeResult {
    pub(crate) measurements: Vec<Measurement>,
    pub(crate) invariants: Vec<Value>,
    pub(crate) fixture_details: Value,
}

#[derive(Debug, Deserialize)]
struct RawItem {
    id: String,
    kind: String,
    title: String,
    #[serde(default)]
    aliases: Vec<String>,
    #[serde(rename = "seriesId")]
    series_id: Option<String>,
    season: Option<i32>,
    episode: Option<i32>,
}

#[derive(Debug, Deserialize)]
struct RawQuery {
    id: String,
    category: String,
    query: String,
    #[serde(rename = "imeCommitted")]
    ime_committed: bool,
    expectation: QueryExpectation,
}

#[derive(Debug, Default, Deserialize)]
struct QueryExpectation {
    rank1: Option<String>,
    #[serde(rename = "matchCount")]
    match_count: Option<usize>,
    oracle: Option<String>,
    kind: Option<String>,
    #[serde(rename = "seriesId")]
    series_id: Option<String>,
    season: Option<i32>,
    episode: Option<i32>,
}

struct SearchFixture {
    items: Vec<CatalogItem>,
    queries: Vec<RawQuery>,
    id_to_item: HashMap<String, usize>,
    children_by_series: HashMap<String, Vec<usize>>,
    title_items: Vec<usize>,
    derived_season_count: usize,
}

#[derive(Default)]
struct CorrectnessSummary {
    exact_count: usize,
    exact_passed: usize,
    recall_sum: f64,
    recall_count: usize,
    reciprocal_rank_sum: f64,
    reciprocal_rank_count: usize,
    ndcg_sum: f64,
    ndcg_count: usize,
    duplicate_free: bool,
    categories: BTreeMap<String, CategoryCorrectness>,
}

#[derive(Default)]
struct CategoryCorrectness {
    queries: usize,
    passed: usize,
}

#[derive(Default)]
struct RssObservation {
    peak_mib: Option<f64>,
    sample_count: usize,
    duration_ms: f64,
}

struct SidecarRebuildObservation {
    elapsed_ms: f64,
    peak_rss: RssObservation,
    residual_rss_mib: Option<f64>,
    removed_sidecar_name: String,
    removed_sidecar_bytes: u64,
}

struct ConcurrentRebuildObservation {
    elapsed_ms: f64,
    rebuild_start_observed: bool,
    query_succeeded: bool,
    query_count: usize,
    peak_rss: RssObservation,
    residual_rss_mib: Option<f64>,
    removed_sidecar_name: String,
    removed_sidecar_bytes: u64,
}

struct SearchPageExpectation {
    category: String,
    query: String,
    ids: Vec<String>,
    total_matches: u64,
    has_more: bool,
}

#[allow(clippy::too_many_lines)]
pub(crate) fn collect(fixture_directory: &Path, profile: &str) -> ProbeResult<SearchProbeResult> {
    let fixture = load_fixture(fixture_directory)?;
    let strict_profile = !matches!(profile, "pr" | "pullrequest");
    let resource_profile = matches!(profile, "nightly" | "weekly" | "release");
    let rss_baseline_mib = current_rss_mib();
    let process_peak_rss_baseline_mib = process_peak_rss_mib();
    let temporary = tempfile::Builder::new()
        .prefix("yanami-production-search-probe-")
        .tempdir()?;
    let catalog_path = temporary.path().join("catalog.sqlite3");
    let scope = CatalogScope::new("perf-local", "perf-server", "perf-user");

    let open_started = Instant::now();
    let catalog = Arc::new(MediaCatalog::open(&catalog_path, scope.clone())?);
    let initial_open_ms = elapsed_ms(open_started);
    let (initial_rebuild_ms, initial_rebuild_rss) =
        timed_full_sync(&catalog, &fixture.items, resource_profile)?;

    let mut measurements = Vec::new();
    let mut invariants = vec![
        json!({
            "id": "search.no_hot_path_network",
            "passed": true,
            "details": {
                "evidence": "production-media-catalog",
                "observedPath": "yanami_storage::MediaCatalog::search",
                "networkClientConstructed": false,
                "fixtureSource": "validated local F110K files"
            }
        }),
        json!({
            "id": "search.oracle_is_independent",
            "passed": true,
            "details": {
                "evidence": "independent-fixture-oracle",
                "implementation": "probe-owned NFKC/casefold/transliteration matcher and fixture relationship oracle",
                "productionRankingCodeCalledByOracle": false
            }
        }),
    ];

    if resource_profile {
        warm_all_categories(&catalog, &fixture.queries)?;
        if let Some(baseline_mib) = rss_baseline_mib {
            let mut samples = Vec::with_capacity(3);
            for _ in 0..3 {
                let current =
                    current_rss_mib().ok_or("RSS became unavailable during search probe")?;
                samples.push((current - baseline_mib).max(0.0));
                thread::sleep(Duration::from_millis(20));
            }
            measurements.push(measurement(
                "search.steady_rss_increment_mib",
                "MiB",
                samples,
                production_attributes(json!({
                    "baselineMiB": baseline_mib,
                    "definition": "current process working set after one completed initial index build and representative hot-query warmup, before repeated open or sidecar-recovery stress, minus the post-fixture pre-catalog baseline",
                    "collector": rss_collector_name()
                })),
            ));
        }
    }

    let mut rebuild_samples = Vec::new();
    let mut rebuild_queries_succeeded = true;
    let mut rebuild_query_count = 0_usize;
    let mut rebuild_peak_rss_increments = Vec::new();
    let mut rebuild_peak_rss_phases = Vec::new();
    let mut rebuild_residual_rss_increments = Vec::new();
    let mut rebuild_residual_rss_phases = Vec::new();
    let mut rebuild_rss_sample_counts = Vec::new();
    let mut rebuild_rss_sample_durations_ms = Vec::new();
    let mut rebuild_sidecars = Vec::new();
    if resource_profile {
        let rebuild_expectations = representative_search_expectations(
            &catalog,
            &fixture.queries,
            CONCURRENT_QUERY_WORKLOAD_SIZE,
        )?;
        for offset in 0..REBUILD_OPERATION_SAMPLES {
            let observation =
                timed_isolated_sidecar_rebuild(&catalog_path, &scope, &rebuild_expectations)?;
            rebuild_samples.push(observation.elapsed_ms);
            if let (Some(baseline_mib), Some(peak_mib)) =
                (rss_baseline_mib, observation.peak_rss.peak_mib)
            {
                rebuild_peak_rss_increments.push((peak_mib - baseline_mib).max(0.0));
                rebuild_peak_rss_phases
                    .push(format!("isolated derived sidecar rebuild {}", offset + 1));
                rebuild_rss_sample_counts.push(observation.peak_rss.sample_count);
                rebuild_rss_sample_durations_ms.push(observation.peak_rss.duration_ms);
            }
            if let (Some(baseline_mib), Some(residual_mib)) =
                (rss_baseline_mib, observation.residual_rss_mib)
            {
                rebuild_residual_rss_increments.push((residual_mib - baseline_mib).max(0.0));
                rebuild_residual_rss_phases
                    .push(format!("isolated derived sidecar rebuild {}", offset + 1));
            }
            rebuild_sidecars.push(json!({
                "name": observation.removed_sidecar_name,
                "bytes": observation.removed_sidecar_bytes
            }));
        }

        let old_generation_expectation = rebuild_expectations
            .iter()
            .find(|expectation| !expectation.ids.is_empty())
            .ok_or("the representative rebuild workload contains no non-empty search")?;
        let mut concurrent_rebuild_samples = Vec::with_capacity(CONCURRENT_REBUILD_SAMPLES);
        let mut concurrent_rounds = Vec::with_capacity(CONCURRENT_REBUILD_SAMPLES);
        for offset in 0..CONCURRENT_REBUILD_SAMPLES {
            let observation = timed_sidecar_rebuild_while_queryable(
                &catalog,
                &catalog_path,
                &scope,
                old_generation_expectation,
                &rebuild_expectations,
            )?;
            concurrent_rebuild_samples.push(observation.elapsed_ms);
            let round_passed = observation.rebuild_start_observed
                && observation.query_succeeded
                && observation.query_count > 0;
            rebuild_queries_succeeded &= round_passed;
            rebuild_query_count = rebuild_query_count.saturating_add(observation.query_count);
            concurrent_rounds.push(json!({
                "round": offset + 1,
                "rebuildStartObserved": observation.rebuild_start_observed,
                "queriesCompletedAfterStartBeforePublish": observation.query_count,
                "allQueriesMatchedOldGeneration": observation.query_succeeded,
                "removedSidecar": {
                    "name": observation.removed_sidecar_name,
                    "bytes": observation.removed_sidecar_bytes
                },
                "passed": round_passed
            }));
            if let (Some(baseline_mib), Some(peak_mib)) =
                (rss_baseline_mib, observation.peak_rss.peak_mib)
            {
                rebuild_peak_rss_increments.push((peak_mib - baseline_mib).max(0.0));
                rebuild_peak_rss_phases
                    .push(format!("concurrent derived sidecar rebuild {}", offset + 1));
                rebuild_rss_sample_counts.push(observation.peak_rss.sample_count);
                rebuild_rss_sample_durations_ms.push(observation.peak_rss.duration_ms);
            }
            if let (Some(baseline_mib), Some(residual_mib)) =
                (rss_baseline_mib, observation.residual_rss_mib)
            {
                rebuild_residual_rss_increments.push((residual_mib - baseline_mib).max(0.0));
                rebuild_residual_rss_phases
                    .push(format!("concurrent derived sidecar rebuild {}", offset + 1));
            }
        }
        invariants.push(json!({
            "id": "search.rebuild_keeps_old_index_queryable",
            "passed": rebuild_queries_succeeded,
            "details": {
                "evidence": "production-media-catalog",
                "concurrentDerivedSidecarRebuilds": CONCURRENT_REBUILD_SAMPLES,
                "queriesCompletedWhileWriterWasActive": rebuild_query_count,
                "rounds": concurrent_rounds,
                "sameCatalogPublicApi": true,
                "writerStartFence": "the probe first observes a posting-builder temporary file after the worker crosses its public MediaCatalog::open start barrier; a query counts only when it completes while the removed final sidecar path is still absent and the open worker has not completed",
                "oldGenerationSource": "the original MediaCatalog retains the published immutable in-memory generation while a second public MediaCatalog::open rebuilds the removed disposable sidecar from SQLite"
            }
        }));
        measurements.push(measurement(
            "search.index_rebuild_110k_ms",
            "ms",
            rebuild_samples,
            production_attributes(json!({
                "sourceItems": SOURCE_ITEM_COUNT,
                "indexedEntities": fixture.items.len(),
                "resultEligibleEntities": SOURCE_ITEM_COUNT,
                "derivedSeasonEntities": fixture.derived_season_count,
                "initialBuildDiagnosticMs": initial_rebuild_ms,
                "removedSidecars": rebuild_sidecars,
                "validationOutsideTimedSection": {
                    "cachedCount": SEARCHABLE_ENTITY_COUNT,
                    "representativeQueries": rebuild_expectations.len(),
                    "queryCategories": rebuild_expectations.iter().map(|expectation| expectation.category.as_str()).collect::<Vec<_>>(),
                    "comparison": "exact IDs, total_matches, and has_more against the original published generation after every rebuild"
                },
                "definition": "ten isolated homogeneous clean derived-index rebuild rounds: remove the current disposable posting sidecar, then time only public MediaCatalog::open rebuilding the complete 140k-entity generation from retained raw SQLite facts; no reader competes with the canonical timed rounds"
            })),
        ));
        measurements.push(measurement(
            "search.production_catalog.concurrent_rebuild_ms",
            "ms",
            concurrent_rebuild_samples,
            production_attributes(json!({
                "enforcement": "observation",
                "rounds": CONCURRENT_REBUILD_SAMPLES,
                "definition": "public MediaCatalog::open duration in the separate old-generation-queryability scenario; excluded from canonical rebuild CV because reader scheduling is intentionally concurrent"
            })),
        ));

        if let (Some(baseline_mib), Some(initial_peak_mib)) =
            (rss_baseline_mib, initial_rebuild_rss.peak_mib)
        {
            rebuild_peak_rss_increments.insert(0, (initial_peak_mib - baseline_mib).max(0.0));
            rebuild_peak_rss_phases.insert(0, "initial full synchronization".to_owned());
            rebuild_rss_sample_counts.insert(0, initial_rebuild_rss.sample_count);
            rebuild_rss_sample_durations_ms.insert(0, initial_rebuild_rss.duration_ms);
        }
        if !rebuild_peak_rss_increments.is_empty() {
            measurements.push(measurement(
                "search.production_catalog.peak_rss_increment_mib",
                "MiB",
                rebuild_peak_rss_increments,
                production_attributes(json!({
                    "enforcement": "observation",
                    "baselineMiB": rss_baseline_mib,
                    "phases": rebuild_peak_rss_phases,
                    "workingSetSamplesPerPhase": rebuild_rss_sample_counts,
                    "sampleDurationsMsPerPhase": rebuild_rss_sample_durations_ms,
                    "samplingCaveat": "phase samples are diagnostic snapshots; the OS-maintained process high-water metric below is the authoritative no-missed-peak evidence",
                    "definition": "maximum sampled process working set during each complete 140k-entity posting generation build minus the post-fixture, pre-catalog baseline",
                    "collector": rss_collector_name()
                })),
            ));
        }
        if !rebuild_residual_rss_increments.is_empty() {
            measurements.push(measurement(
                "search.production_catalog.rebuild_residual_rss_increment_mib",
                "MiB",
                rebuild_residual_rss_increments,
                production_attributes(json!({
                    "enforcement": "observation",
                    "baselineMiB": rss_baseline_mib,
                    "phases": rebuild_residual_rss_phases,
                    "definition": "process working set after the rebuilt verification catalog is dropped, minus the post-fixture, pre-catalog baseline",
                    "collector": rss_collector_name()
                })),
            ));
        }
        if let (Some(baseline_current_mib), Some(final_peak_mib)) =
            (rss_baseline_mib, process_peak_rss_mib())
        {
            measurements.push(measurement(
                "search.production_catalog.process_peak_rss_increment_mib",
                "MiB",
                vec![(final_peak_mib - baseline_current_mib).max(0.0)],
                production_attributes(json!({
                    "enforcement": "observation",
                    "baselineCurrentWorkingSetMiB": baseline_current_mib,
                    "baselineLifetimeProcessPeakMiBDiagnostic": process_peak_rss_baseline_mib,
                    "finalProcessPeakMiB": final_peak_mib,
                    "expectedMaximumIncrementMiB": 256,
                    "phasesCovered": "initial full build, ten isolated full recovery rebuilds, and three concurrent recovery rebuilds",
                    "definition": "final operating-system-maintained lifetime working-set high-water minus the post-fixture pre-catalog current working set; unlike polling or subtracting an earlier lifetime high-water, this cannot miss or mask a short-lived build peak",
                    "collector": process_peak_rss_collector_name()
                })),
            ));
        }
    }

    if strict_profile {
        let first_reopen_started = Instant::now();
        drop(MediaCatalog::open(&catalog_path, scope.clone())?);
        let first_reopen_ms = elapsed_ms(first_reopen_started);
        measurements.push(measurement(
            "search.index_first_open_110k_ms",
            "ms",
            vec![first_reopen_ms],
            production_attributes(json!({
                "enforcement": "observation",
                "definition": "first unpreheated MediaCatalog::open on the already-persisted 140k-entity sidecar within this process; OS cache is not forcibly cleared"
            })),
        ));
        let mut open_samples = Vec::with_capacity(STRICT_OPERATION_SAMPLES);
        // The first populated reopen is retained above as a diagnostic. The
        // canonical samples all follow that single deterministic warmup and
        // therefore describe one steady reopen cache state.
        for _ in 0..STRICT_OPERATION_SAMPLES {
            let started = Instant::now();
            drop(MediaCatalog::open(&catalog_path, scope.clone())?);
            open_samples.push(elapsed_ms(started));
        }
        measurements.push(measurement(
            "search.index_open_ms",
            "ms",
            open_samples,
            production_attributes(json!({
                "initialEmptyCatalogOpenMs": initial_open_ms,
                "firstPopulatedReopenDiagnosticMs": first_reopen_ms,
                "warmupReopensExcluded": 1,
                "operatingSystemPageCache": "not forcibly cleared",
                "definition": "ten steady MediaCatalog::open rounds on the same persisted 140k-entity sidecar after one unmeasured populated-index warmup reopen"
            })),
        ));

        let mut cold_samples = Vec::with_capacity(17);
        for query in fixture.queries.iter().take(17) {
            let reopened = MediaCatalog::open(&catalog_path, scope.clone())?;
            let started = Instant::now();
            let _ = reopened.search(&query.query)?;
            cold_samples.push(elapsed_ms(started));
        }
        measurements.push(measurement(
            "search.query.cold_ms",
            "ms",
            cold_samples,
            production_attributes(json!({
                "connectionState": "new read-only SQLite connection per sample",
                "operatingSystemPageCache": "unmodified",
                "categories": fixture.queries.iter().take(17).map(|query| query.category.as_str()).collect::<Vec<_>>()
            })),
        ));
    }

    warm_all_categories(&catalog, &fixture.queries)?;
    let hot_sample_count = hot_sample_count(profile);
    let mut hot_samples = Vec::with_capacity(hot_sample_count);
    let mut correctness = CorrectnessSummary {
        duplicate_free: true,
        ..CorrectnessSummary::default()
    };
    for query in fixture.queries.iter().take(hot_sample_count) {
        let started = Instant::now();
        let page = catalog.search(&query.query)?;
        hot_samples.push(elapsed_ms(started));
        evaluate_query(
            &fixture,
            query,
            &page
                .items
                .iter()
                .map(|hit| hit.item.id.as_str())
                .collect::<Vec<_>>(),
            page.total_matches,
            page.has_more,
            &mut correctness,
        )?;
    }

    let category_counts = correctness
        .categories
        .iter()
        .map(|(category, result)| {
            (
                category.clone(),
                json!({
                    "queries": result.queries,
                    "passed": result.passed,
                    "ratio": ratio(result.passed, result.queries)
                }),
            )
        })
        .collect::<serde_json::Map<_, _>>();
    measurements.push(measurement(
        "search.query.hot_ms",
        "ms",
        hot_samples,
        production_attributes(json!({
            "queryCount": hot_sample_count,
            "querySelection": "ordered prefix of the validated 20k F110K corpus",
            "resultLimit": SEARCH_RESULT_LIMIT,
            "resultLimitPerGroup": SEARCH_GROUP_LIMIT,
            "resultGroups": ["Movie/Series", "Episode"],
            "categoryCorrectness": category_counts
        })),
    ));
    measurements.extend(correctness_measurements(&correctness));

    for (category, result) in &correctness.categories {
        measurements.push(measurement(
            format!(
                "search.production_catalog.correctness.{}_ratio",
                metric_slug(category)
            ),
            "ratio",
            vec![ratio(result.passed, result.queries)],
            production_attributes(json!({
                "category": category,
                "queries": result.queries,
                "passed": result.passed,
                "enforcement": "observation"
            })),
        ));
    }

    invariants.push(json!({
        "id": "search.production_catalog.no_duplicate_rows",
        "passed": correctness.duplicate_free,
        "details": {
            "evidence": "production-media-catalog",
            "enforcement": "observation",
            "note": "Storage result uniqueness only; cross-session presentation fencing is measured by the desktop production-path harness."
        }
    }));
    let scope_mismatch_rejected = matches!(
        MediaCatalog::open(
            &catalog_path,
            CatalogScope::new("other-local", "other-server", "other-user")
        ),
        Err(MediaCatalogError::ScopeMismatch)
    );
    invariants.push(json!({
        "id": "search.production_catalog.scope_mismatch_rejected",
        "passed": scope_mismatch_rejected,
        "details": {
            "evidence": "production-media-catalog",
            "enforcement": "observation",
            "note": "Database scope validation only; this does not certify SearchCoordinator session-generation fencing."
        }
    }));

    if resource_profile {
        let files = catalog_files(temporary.path())?;
        let database_file_name = catalog_path
            .file_name()
            .and_then(|value| value.to_str())
            .ok_or("catalog database path has no UTF-8 file name")?;
        let mut sqlite_bytes = 0_u64;
        let mut sidecar_bytes = 0_u64;
        for file in &files {
            let name = file["name"].as_str().unwrap_or_default();
            let bytes = file["bytes"].as_u64().unwrap_or_default();
            if name == database_file_name
                || name == format!("{database_file_name}-wal")
                || name == format!("{database_file_name}-shm")
            {
                sqlite_bytes = sqlite_bytes.saturating_add(bytes);
            } else {
                sidecar_bytes = sidecar_bytes.saturating_add(bytes);
            }
        }
        let disk_mib = (sqlite_bytes.saturating_add(sidecar_bytes)) as f64 / (1024.0 * 1024.0);
        measurements.push(measurement(
            "search.index_disk_mib",
            "MiB",
            vec![disk_mib],
            production_attributes(json!({
                "sqliteBytes": sqlite_bytes,
                "sidecarBytes": sidecar_bytes,
                "files": files,
                "definition": "sum of all files retained in the isolated production catalog directory"
            })),
        ));
        measurements.extend([
            measurement(
                "search.production_catalog.sqlite_disk_mib",
                "MiB",
                vec![sqlite_bytes as f64 / (1024.0 * 1024.0)],
                production_attributes(json!({
                    "enforcement": "observation",
                    "definition": "SQLite database plus exact -wal and -shm companions"
                })),
            ),
            measurement(
                "search.production_catalog.sidecar_disk_mib",
                "MiB",
                vec![sidecar_bytes as f64 / (1024.0 * 1024.0)],
                production_attributes(json!({
                    "enforcement": "observation",
                    "definition": "persistent posting-index generation sidecars"
                })),
            ),
        ]);
    }

    if strict_profile {
        let (incremental_samples, isolated_alias_versions_valid) =
            measure_isolated_incremental_updates(
                &catalog_path,
                &scope,
                temporary.path(),
                &fixture.items,
            )?;
        let (
            concurrent_query_samples,
            writer_under_reader_samples,
            queries_succeeded,
            concurrent_alias_versions_valid,
        ) = measure_queries_during_incremental(&catalog, &fixture.items, &fixture.queries)?;
        let alias_versions_valid = isolated_alias_versions_valid && concurrent_alias_versions_valid;
        measurements.push(measurement(
            "search.incremental_1000_ms",
            "ms",
            incremental_samples,
            production_attributes(json!({
                "itemsPerTransaction": INCREMENTAL_ITEM_COUNT,
                "iterations": STRICT_OPERATION_SAMPLES,
                "warmupTransactionsExcluded": 1,
                "concurrentReader": false,
                "definition": "ten isolated public upsert_page transactions for 1000 existing catalog facts",
                "reproducibilityWorkload": "each round restores and opens the same completed 140k-catalog snapshot with the same 1000-row baseline marker, then replaces it with one same-shape controlled marker without a concurrent reader"
            })),
        ));
        measurements.push(measurement(
            "search.query_during_incremental_ms",
            "ms",
            concurrent_query_samples,
            production_attributes(json!({
                "writer": "MediaCatalog::upsert_page",
                "reader": "MediaCatalog::search",
                "writerRounds": STRICT_OPERATION_SAMPLES,
                "queriesSucceeded": queries_succeeded,
                "readerWriterConnections": "independent public MediaCatalog connections",
                "backgroundWorkload": "the same ordered 17-category query cycle restarts at query 0 for every incremental round"
            })),
        ));
        measurements.push(measurement(
            "search.production_catalog.incremental_1000_under_reader_ms",
            "ms",
            writer_under_reader_samples,
            production_attributes(json!({
                "enforcement": "observation",
                "itemsPerTransaction": INCREMENTAL_ITEM_COUNT,
                "iterations": STRICT_OPERATION_SAMPLES,
                "definition": "writer duration during the query-during-incremental scenario; diagnostic only because OS scheduling and reader lock phase are not canonical incremental latency samples"
            })),
        ));
        invariants.push(json!({
            "id": "search.production_catalog.query_during_incremental_succeeded",
            "passed": queries_succeeded > 0,
            "details": {
                "evidence": "production-media-catalog",
                "enforcement": "observation",
                "queriesSucceeded": queries_succeeded
            }
        }));
        invariants.push(json!({
            "id": "search.production_catalog.incremental_alias_version_visible",
            "passed": alias_versions_valid,
            "details": {
                "evidence": "production-media-catalog",
                "enforcement": "observation",
                "isolatedIterations": STRICT_OPERATION_SAMPLES,
                "concurrentIterations": STRICT_OPERATION_SAMPLES,
                "semantics": "Every isolated round restores the same warmup marker and replaces it on 1000 rows; concurrent rounds replace the prior round after the first. Each new marker must resolve only updated IDs and every applicable prior marker must return zero rows."
            }
        }));
    }

    let final_status = catalog.sync_status()?;
    let fixture_details = json!({
        "sourceItemCount": SOURCE_ITEM_COUNT,
        "derivedSeasonCount": fixture.derived_season_count,
        "indexedEntityCount": fixture.items.len(),
        "resultEligibleEntityCount": SOURCE_ITEM_COUNT,
        "queryCount": fixture.queries.len(),
        "productionApi": "yanami_storage::MediaCatalog",
        "catalogStatusAfterProbe": format!("{:?}", final_status.state),
        "canonicalSchedulerEvidenceProvider": {
            "probe": "desktop production SearchCoordinator delayed-fake harness",
            "evidence": "production-search-coordinator-delayed-fake",
            "idsNotEmittedByStorageProbe": [
                "search.no_stale_commit",
                "search.final_query_complete",
                "search.queue_bounded",
                "search.no_duplicates_or_cross_session_results"
            ]
        }
    });

    Ok(SearchProbeResult {
        measurements,
        invariants,
        fixture_details,
    })
}

#[allow(clippy::too_many_lines)]
fn load_fixture(directory: &Path) -> ProbeResult<SearchFixture> {
    let items_path = directory.join("f110k-items.jsonl");
    let queries_path = directory.join("search-queries-v1.jsonl");
    let mut items = Vec::with_capacity(SEARCHABLE_ENTITY_COUNT);
    let mut series_titles = HashMap::with_capacity(7_500);
    let mut derived_seasons = BTreeMap::<(String, i32), String>::new();

    let reader = BufReader::new(fs::File::open(items_path)?);
    for (line_number, line) in reader.lines().enumerate() {
        let raw: RawItem = serde_json::from_str(&line?)
            .map_err(|error| format!("invalid F110K item on line {}: {error}", line_number + 1))?;
        let item = match raw.kind.as_str() {
            "Movie" | "Series" => {
                if raw.kind == "Series" {
                    series_titles.insert(raw.id.clone(), raw.title.clone());
                }
                CatalogItem {
                    id: raw.id.clone(),
                    item_type: raw.kind.clone(),
                    title: raw.title.clone(),
                    sort_title: raw.title,
                    series_id: (raw.kind == "Series").then_some(raw.id),
                    aliases: raw.aliases,
                    ..CatalogItem::default()
                }
            }
            "Episode" => {
                let series_id = raw
                    .series_id
                    .as_deref()
                    .ok_or_else(|| format!("episode {} is missing seriesId", raw.id))?;
                let series_title = series_titles
                    .get(series_id)
                    .ok_or_else(|| {
                        format!("episode {} references unknown series {series_id}", raw.id)
                    })?
                    .clone();
                let season = raw
                    .season
                    .ok_or_else(|| format!("episode {} is missing season", raw.id))?;
                let episode = raw
                    .episode
                    .ok_or_else(|| format!("episode {} is missing episode number", raw.id))?;
                let season_id = format!("season:{series_id}:{season}");
                let mut aliases = raw.aliases;
                let padded_code = format!("S{season:02}E{episode:02}");
                aliases.extend([
                    format!("S{season:02}"),
                    format!("S{season}"),
                    format!("Season {season}"),
                    format!("第{season}季"),
                    padded_code.clone(),
                    format!("S{season}E{episode}"),
                    format!("{series_title} S{season:02}"),
                    format!("{series_title} Season {season}"),
                    format!("{series_title} 第{season}季"),
                    format!("{series_title} {padded_code}"),
                ]);
                derived_seasons
                    .entry((series_id.to_owned(), season))
                    .or_insert_with(|| series_title.clone());
                CatalogItem {
                    id: raw.id,
                    item_type: raw.kind,
                    title: raw.title.clone(),
                    sort_title: raw.title,
                    parent_id: Some(season_id.clone()),
                    series_id: Some(series_id.to_owned()),
                    series_title: Some(series_title),
                    season_id: Some(season_id),
                    season_title: Some(format!("第{season}季")),
                    season_number: Some(season),
                    episode_number: Some(episode),
                    aliases,
                    ..CatalogItem::default()
                }
            }
            kind => return Err(format!("unsupported F110K kind '{kind}'").into()),
        };
        items.push(item);
    }
    if items.len() != SOURCE_ITEM_COUNT {
        return Err(format!(
            "F110K source item count mismatch: expected {SOURCE_ITEM_COUNT}, got {}",
            items.len()
        )
        .into());
    }

    let derived_season_count = derived_seasons.len();
    if derived_season_count != DERIVED_SEASON_COUNT {
        return Err(format!(
            "F110K derived Season count mismatch: expected {DERIVED_SEASON_COUNT}, got {derived_season_count}"
        )
        .into());
    }
    for ((series_id, season), series_title) in derived_seasons {
        let season_id = format!("season:{series_id}:{season}");
        let padded_code = format!("S{season:02}");
        items.push(CatalogItem {
            id: season_id.clone(),
            item_type: "Season".to_owned(),
            title: format!("第{season}季"),
            sort_title: format!("{series_title} {season:04}"),
            parent_id: Some(series_id.clone()),
            series_id: Some(series_id),
            series_title: Some(series_title.clone()),
            season_id: Some(season_id),
            season_title: Some(format!("第{season}季")),
            season_number: Some(season),
            aliases: vec![
                padded_code.clone(),
                format!("S{season}"),
                format!("Season {season}"),
                format!("第{season}季"),
                format!("{series_title} {padded_code}"),
                format!("{series_title} Season {season}"),
                format!("{series_title} 第{season}季"),
            ],
            ..CatalogItem::default()
        });
    }
    if items.len() != SEARCHABLE_ENTITY_COUNT {
        return Err(format!(
            "F110K searchable entity count mismatch: expected {SEARCHABLE_ENTITY_COUNT}, got {}",
            items.len()
        )
        .into());
    }

    let mut queries = Vec::with_capacity(QUERY_COUNT);
    let reader = BufReader::new(fs::File::open(queries_path)?);
    for (line_number, line) in reader.lines().enumerate() {
        let query: RawQuery = serde_json::from_str(&line?)
            .map_err(|error| format!("invalid F110K query on line {}: {error}", line_number + 1))?;
        validate_query_contract(&query)?;
        queries.push(query);
    }
    if queries.len() != QUERY_COUNT {
        return Err(format!(
            "F110K query count mismatch: expected {QUERY_COUNT}, got {}",
            queries.len()
        )
        .into());
    }

    let mut id_to_item = HashMap::with_capacity(items.len());
    let mut children_by_series = HashMap::<String, Vec<usize>>::new();
    let mut title_items = Vec::with_capacity(10_000);
    for (index, item) in items.iter().enumerate() {
        if id_to_item.insert(item.id.clone(), index).is_some() {
            return Err(format!("duplicate searchable entity id {}", item.id).into());
        }
        if matches!(item.item_type.as_str(), "Movie" | "Series") {
            title_items.push(index);
        }
        if matches!(item.item_type.as_str(), "Season" | "Episode") {
            if let Some(series_id) = &item.series_id {
                children_by_series
                    .entry(series_id.clone())
                    .or_default()
                    .push(index);
            }
        }
    }

    Ok(SearchFixture {
        items,
        queries,
        id_to_item,
        children_by_series,
        title_items,
        derived_season_count,
    })
}

fn validate_query_contract(query: &RawQuery) -> ProbeResult<()> {
    if query.id.trim().is_empty()
        || query.category.trim().is_empty()
        || query.query.trim().is_empty()
        || !query.ime_committed
    {
        return Err(format!("query {} violates the committed-query contract", query.id).into());
    }
    let expectation_count = usize::from(query.expectation.rank1.is_some())
        + usize::from(query.expectation.match_count.is_some())
        + usize::from(query.expectation.oracle.is_some());
    if expectation_count != 1 {
        return Err(format!(
            "query {} must carry exactly one independent expectation",
            query.id
        )
        .into());
    }
    if query.category == "season"
        && (query.expectation.kind.as_deref() != Some("Season")
            || query.expectation.series_id.is_none()
            || query.expectation.season.is_none())
    {
        return Err(format!("season query {} has incomplete oracle metadata", query.id).into());
    }
    if query.category == "episode-number"
        && (query.expectation.kind.as_deref() != Some("Episode")
            || query.expectation.series_id.is_none()
            || query.expectation.season.is_none()
            || query.expectation.episode.is_none())
    {
        return Err(format!("episode query {} has incomplete oracle metadata", query.id).into());
    }
    Ok(())
}

fn timed_full_sync(
    catalog: &MediaCatalog,
    items: &[CatalogItem],
    observe_rss: bool,
) -> ProbeResult<(f64, RssObservation)> {
    let done = AtomicBool::new(false);
    thread::scope(|scope| {
        let monitor = observe_rss.then(|| scope.spawn(|| monitor_rss_until(&done)));
        let started = Instant::now();
        let result = full_sync(catalog, items);
        let elapsed = elapsed_ms(started);
        done.store(true, Ordering::Release);
        let rss = monitor
            .map(|handle| handle.join().unwrap_or_default())
            .unwrap_or_default();
        result?;
        Ok((elapsed, rss))
    })
}

fn full_sync(catalog: &MediaCatalog, items: &[CatalogItem]) -> ProbeResult<()> {
    let now = unix_time_ms()?;
    let item_count = u64::try_from(items.len())?;
    let run = catalog.begin_sync(Some(item_count), now)?;
    for (page_index, page) in items.chunks(CATALOG_PAGE_SIZE).enumerate() {
        let next = ((page_index + 1) * CATALOG_PAGE_SIZE).min(items.len());
        catalog.upsert_page(
            run,
            page,
            &[],
            u64::try_from(next)?,
            Some(item_count),
            unix_time_ms()?,
        )?;
    }
    // Production synchronization records the final cross-stage count even
    // when the last remote stage/page is empty. Exercise that reconciliation
    // contract explicitly before publishing the immutable search generation.
    catalog.record_sync_progress(run, item_count, item_count, unix_time_ms()?)?;
    let membership = items
        .iter()
        .map(|item| item.id.clone())
        .collect::<HashSet<_>>();
    catalog.verify_sync_membership(run, &membership, item_count, unix_time_ms()?)?;
    catalog.complete_verified_sync(run, unix_time_ms()?)?;
    Ok(())
}

fn representative_search_expectations(
    catalog: &MediaCatalog,
    queries: &[RawQuery],
    category_count: usize,
) -> ProbeResult<Vec<SearchPageExpectation>> {
    let mut categories = HashSet::new();
    let mut expectations = Vec::with_capacity(category_count);
    for query in queries {
        if !categories.insert(query.category.as_str()) {
            continue;
        }
        let page = catalog.search(&query.query)?;
        expectations.push(SearchPageExpectation {
            category: query.category.clone(),
            query: query.query.clone(),
            ids: page.items.iter().map(|hit| hit.item.id.clone()).collect(),
            total_matches: page.total_matches,
            has_more: page.has_more,
        });
        if expectations.len() == category_count {
            break;
        }
    }
    if expectations.len() != category_count {
        return Err(format!(
            "expected {category_count} representative rebuild query categories, found {}",
            expectations.len()
        )
        .into());
    }
    Ok(expectations)
}

fn validate_rebuilt_catalog(
    rebuilt: &MediaCatalog,
    expectations: &[SearchPageExpectation],
) -> ProbeResult<()> {
    let status = rebuilt.sync_status()?;
    if status.cached_count != u64::try_from(SEARCHABLE_ENTITY_COUNT)? {
        return Err(format!(
            "rebuilt catalog cached_count mismatch: expected {SEARCHABLE_ENTITY_COUNT}, got {}",
            status.cached_count
        )
        .into());
    }
    for expectation in expectations {
        let page = rebuilt.search(&expectation.query)?;
        let ids_match = page.items.len() == expectation.ids.len()
            && page
                .items
                .iter()
                .zip(&expectation.ids)
                .all(|(hit, expected_id)| hit.item.id == *expected_id);
        if !ids_match
            || page.total_matches != expectation.total_matches
            || page.has_more != expectation.has_more
        {
            return Err(format!(
                "rebuilt sidecar did not reproduce category '{}' query '{}'",
                expectation.category, expectation.query
            )
            .into());
        }
    }
    Ok(())
}

fn remove_current_search_sidecar(
    catalog_path: &Path,
) -> ProbeResult<(std::path::PathBuf, String, u64)> {
    let sidecar_path = current_search_sidecar(catalog_path)?;
    let removed_sidecar_name = sidecar_path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or("search sidecar path has no UTF-8 file name")?
        .to_owned();
    let removed_sidecar_bytes = fs::metadata(&sidecar_path)?.len();
    fs::remove_file(&sidecar_path)?;
    Ok((sidecar_path, removed_sidecar_name, removed_sidecar_bytes))
}

fn timed_isolated_sidecar_rebuild(
    catalog_path: &Path,
    scope: &CatalogScope,
    expectations: &[SearchPageExpectation],
) -> ProbeResult<SidecarRebuildObservation> {
    let (sidecar_path, removed_sidecar_name, removed_sidecar_bytes) =
        remove_current_search_sidecar(catalog_path)?;
    let done = AtomicBool::new(false);
    let (rebuilt, elapsed, peak_rss) = thread::scope(|thread_scope| {
        let monitor = thread_scope.spawn(|| monitor_rss_until(&done));
        let started = Instant::now();
        let rebuilt = MediaCatalog::open(catalog_path, scope.clone());
        let elapsed = elapsed_ms(started);
        done.store(true, Ordering::Release);
        let peak_rss = monitor.join().unwrap_or_default();
        (rebuilt, elapsed, peak_rss)
    });
    let rebuilt = rebuilt?;
    validate_rebuilt_catalog(&rebuilt, expectations)?;
    drop(rebuilt);
    let residual_rss_mib = current_rss_mib();
    if !sidecar_path.is_file() {
        return Err(format!(
            "MediaCatalog::open did not restore removed search sidecar {}",
            sidecar_path.display()
        )
        .into());
    }
    Ok(SidecarRebuildObservation {
        elapsed_ms: elapsed,
        peak_rss,
        residual_rss_mib,
        removed_sidecar_name,
        removed_sidecar_bytes,
    })
}

fn timed_sidecar_rebuild_while_queryable(
    catalog: &MediaCatalog,
    catalog_path: &Path,
    scope: &CatalogScope,
    old_generation_expectation: &SearchPageExpectation,
    rebuilt_expectations: &[SearchPageExpectation],
) -> ProbeResult<ConcurrentRebuildObservation> {
    let (sidecar_path, removed_sidecar_name, removed_sidecar_bytes) =
        remove_current_search_sidecar(catalog_path)?;
    let mut query_succeeded = true;
    let mut query_count = 0_usize;
    let done = AtomicBool::new(false);
    let start_barrier = Barrier::new(2);
    let (result, rebuild_start_observed, peak_rss) = thread::scope(|thread_scope| {
        let monitor = thread_scope.spawn(|| monitor_rss_until(&done));
        let writer = thread_scope.spawn(|| {
            start_barrier.wait();
            let started = Instant::now();
            let rebuilt = MediaCatalog::open(catalog_path, scope.clone());
            let elapsed = elapsed_ms(started);
            done.store(true, Ordering::Release);
            let rebuilt = rebuilt.map_err(|error| error.to_string())?;
            validate_rebuilt_catalog(&rebuilt, rebuilt_expectations)
                .map_err(|error| error.to_string())?;
            Ok::<_, String>(elapsed)
        });
        start_barrier.wait();
        let rebuild_start_observed = wait_for_search_rebuild_artifact(catalog_path, &done);
        if rebuild_start_observed {
            while !done.load(Ordering::Acquire) {
                let query_result = catalog.search(&old_generation_expectation.query);
                let completed_before_publish =
                    !done.load(Ordering::Acquire) && !sidecar_path.is_file();
                if !completed_before_publish {
                    break;
                }
                query_count = query_count.saturating_add(1);
                match query_result {
                    Ok(page) => {
                        let ids_match = page.items.len() == old_generation_expectation.ids.len()
                            && page
                                .items
                                .iter()
                                .zip(&old_generation_expectation.ids)
                                .all(|(hit, expected_id)| hit.item.id == *expected_id);
                        if !ids_match
                            || page.total_matches != old_generation_expectation.total_matches
                            || page.has_more != old_generation_expectation.has_more
                        {
                            query_succeeded = false;
                        }
                    }
                    Err(_) => query_succeeded = false,
                }
                thread::yield_now();
            }
        }
        let result = match writer.join() {
            Ok(result) => result,
            Err(_) => Err("catalog sidecar rebuild worker panicked".to_owned()),
        };
        done.store(true, Ordering::Release);
        let peak_rss = monitor.join().unwrap_or_default();
        (result, rebuild_start_observed, peak_rss)
    });
    let elapsed = result.map_err(|error| -> Box<dyn Error> { error.into() })?;
    let residual_rss_mib = current_rss_mib();
    if !sidecar_path.is_file() {
        return Err(format!(
            "MediaCatalog::open did not restore removed search sidecar {}",
            sidecar_path.display()
        )
        .into());
    }
    Ok(ConcurrentRebuildObservation {
        elapsed_ms: elapsed,
        rebuild_start_observed,
        query_succeeded,
        query_count,
        peak_rss,
        residual_rss_mib,
        removed_sidecar_name,
        removed_sidecar_bytes,
    })
}

fn wait_for_search_rebuild_artifact(catalog_path: &Path, done: &AtomicBool) -> bool {
    while !done.load(Ordering::Acquire) {
        if search_rebuild_artifact_exists(catalog_path) {
            return true;
        }
        thread::yield_now();
    }
    false
}

fn search_rebuild_artifact_exists(catalog_path: &Path) -> bool {
    let Some(parent) = catalog_path.parent() else {
        return false;
    };
    let Some(database_name) = catalog_path.file_name().and_then(|value| value.to_str()) else {
        return false;
    };
    let prefix = format!("{database_name}.search-");
    fs::read_dir(parent).is_ok_and(|entries| {
        entries.filter_map(Result::ok).any(|entry| {
            entry.file_name().to_str().is_some_and(|name| {
                name.starts_with(&prefix)
                    && (name.contains(".idx.refs-")
                        || name.contains(".idx.docs-")
                        || name.contains(".idx.tmp-"))
            })
        })
    })
}

fn monitor_rss_until(done: &AtomicBool) -> RssObservation {
    let mut observation = RssObservation::default();
    let started = Instant::now();
    loop {
        if let Some(current_mib) = current_rss_mib() {
            observation.peak_mib = Some(
                observation
                    .peak_mib
                    .map_or(current_mib, |peak| peak.max(current_mib)),
            );
            observation.sample_count = observation.sample_count.saturating_add(1);
        }
        if done.load(Ordering::Acquire) {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    observation.duration_ms = elapsed_ms(started);
    observation
}

fn current_search_sidecar(catalog_path: &Path) -> ProbeResult<std::path::PathBuf> {
    let parent = catalog_path
        .parent()
        .ok_or("catalog path has no parent directory")?;
    let database_name = catalog_path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or("catalog database path has no UTF-8 file name")?;
    let prefix = format!("{database_name}.search-");
    let mut candidates = Vec::new();
    for entry in fs::read_dir(parent)? {
        let entry = entry?;
        let Some(name) = entry.file_name().to_str().map(ToOwned::to_owned) else {
            continue;
        };
        let Some(generation) = name
            .strip_prefix(&prefix)
            .and_then(|value| value.strip_suffix(".idx"))
            .and_then(|value| value.parse::<i64>().ok())
        else {
            continue;
        };
        candidates.push((generation, entry.path()));
    }
    if candidates.len() != 1 {
        return Err(format!(
            "expected exactly one published search sidecar for {}, found {}",
            catalog_path.display(),
            candidates.len()
        )
        .into());
    }
    Ok(candidates.remove(0).1)
}

fn warm_all_categories(catalog: &MediaCatalog, queries: &[RawQuery]) -> ProbeResult<()> {
    let mut warmed = HashSet::new();
    for query in queries {
        if warmed.insert(query.category.as_str()) {
            let _ = catalog.search(&query.query)?;
        }
        if warmed.len() == 17 {
            break;
        }
    }
    if warmed.len() != 17 {
        return Err(format!("expected 17 F110K query categories, found {}", warmed.len()).into());
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SearchResultGroup {
    Title,
    Episode,
}

fn search_result_group(item: &CatalogItem) -> Option<SearchResultGroup> {
    match item.item_type.as_str() {
        "Movie" | "Series" => Some(SearchResultGroup::Title),
        "Episode" => Some(SearchResultGroup::Episode),
        _ => None,
    }
}

fn grouped_result_shape(fixture: &SearchFixture, ids: &[&str]) -> Option<(usize, usize)> {
    let mut title_count = 0_usize;
    let mut episode_count = 0_usize;
    let mut episode_section_started = false;
    for id in ids {
        let item = fixture
            .id_to_item
            .get(*id)
            .and_then(|index| fixture.items.get(*index))?;
        match search_result_group(item)? {
            SearchResultGroup::Title if !episode_section_started => {
                title_count = title_count.saturating_add(1);
            }
            SearchResultGroup::Title => return None,
            SearchResultGroup::Episode => {
                episode_section_started = true;
                episode_count = episode_count.saturating_add(1);
            }
        }
    }
    (title_count <= SEARCH_GROUP_LIMIT
        && episode_count <= SEARCH_GROUP_LIMIT
        && ids.len() <= SEARCH_RESULT_LIMIT)
        .then_some((title_count, episode_count))
}

fn expected_group_counts(
    fixture: &SearchFixture,
    expected_ids: &HashSet<String>,
) -> ProbeResult<(usize, usize)> {
    let mut title_count = 0_usize;
    let mut episode_count = 0_usize;
    for id in expected_ids {
        let index = fixture
            .id_to_item
            .get(id)
            .ok_or_else(|| format!("independent oracle references missing item {id}"))?;
        match search_result_group(&fixture.items[*index]) {
            Some(SearchResultGroup::Title) => title_count = title_count.saturating_add(1),
            Some(SearchResultGroup::Episode) => {
                episode_count = episode_count.saturating_add(1);
            }
            None => {
                return Err(format!(
                    "independent oracle exposed non-result item {} ({})",
                    id, fixture.items[*index].item_type
                )
                .into());
            }
        }
    }
    Ok((title_count, episode_count))
}

fn expected_page_matches(
    fixture: &SearchFixture,
    expected_ids: &HashSet<String>,
    actual_ids: &[&str],
    total_matches: u64,
    has_more: bool,
) -> ProbeResult<bool> {
    let Some((actual_titles, actual_episodes)) = grouped_result_shape(fixture, actual_ids) else {
        return Ok(false);
    };
    let (expected_titles, expected_episodes) = expected_group_counts(fixture, expected_ids)?;
    let visible_titles = expected_titles.min(SEARCH_GROUP_LIMIT);
    let visible_episodes = expected_episodes.min(SEARCH_GROUP_LIMIT);
    let visible_count = visible_titles.saturating_add(visible_episodes);
    let expected_total = u64::try_from(expected_ids.len())?;
    Ok(total_matches == expected_total
        && actual_titles == visible_titles
        && actual_episodes == visible_episodes
        && actual_ids.len() == visible_count
        && has_more == (expected_ids.len() > visible_count)
        && actual_ids.iter().all(|id| expected_ids.contains(*id)))
}

fn query_ndcg(
    fixture: &SearchFixture,
    actual_ids: &[&str],
    normalized_query: &str,
    expected: Option<&HashSet<String>>,
    explicit_target: Option<&str>,
) -> f64 {
    if let Some(expected_id) = explicit_target {
        return actual_ids
            .iter()
            .take(10)
            .position(|id| *id == expected_id)
            .map_or(0.0, |position| 1.0 / (position as f64 + 2.0).log2());
    }
    let relevant = actual_ids
        .iter()
        .take(10)
        .filter(|id| {
            expected.map_or_else(
                || {
                    fixture.id_to_item.get(**id).is_some_and(|index| {
                        oracle_matches(&fixture.items[*index], normalized_query)
                    })
                },
                |expected_ids| expected_ids.contains(**id),
            )
        })
        .count();
    let ideal = expected.map_or(10, |ids| ids.len().min(10));
    if ideal == 0 {
        1.0
    } else {
        binary_dcg(relevant) / binary_dcg(ideal)
    }
}

fn evaluate_query(
    fixture: &SearchFixture,
    query: &RawQuery,
    actual_ids: &[&str],
    total_matches: u64,
    has_more: bool,
    summary: &mut CorrectnessSummary,
) -> ProbeResult<()> {
    let category = summary
        .categories
        .entry(query.category.clone())
        .or_default();
    category.queries = category.queries.saturating_add(1);

    let unique = actual_ids.iter().copied().collect::<HashSet<_>>();
    if unique.len() != actual_ids.len() {
        summary.duplicate_free = false;
    }
    let normalized_query = normalize_search_text(&query.query);
    let actual_are_relevant = actual_ids.iter().all(|id| {
        fixture.id_to_item.get(*id).is_some_and(|index| {
            search_result_group(&fixture.items[*index]).is_some()
                && oracle_matches(&fixture.items[*index], &normalized_query)
        })
    });
    let expected = oracle_expected_ids(fixture, query, &normalized_query)?;

    let explicit_target = query.expectation.rank1.as_deref().filter(|target_id| {
        fixture
            .id_to_item
            .get(*target_id)
            .is_some_and(|index| search_result_group(&fixture.items[*index]).is_some())
    });
    let explicit_passed =
        explicit_target.is_none_or(|expected_id| actual_ids.first().copied() == Some(expected_id));
    if let Some(expected_id) = explicit_target {
        if query.category == "exact" {
            summary.exact_count = summary.exact_count.saturating_add(1);
            summary.exact_passed = summary
                .exact_passed
                .saturating_add(usize::from(explicit_passed));
        }
        let reciprocal_rank = actual_ids
            .iter()
            .take(10)
            .position(|id| *id == expected_id)
            .map_or(0.0, |position| 1.0 / (position as f64 + 1.0));
        summary.reciprocal_rank_sum += reciprocal_rank;
        summary.reciprocal_rank_count = summary.reciprocal_rank_count.saturating_add(1);
    }

    if let Some(expected_ids) = &expected {
        let (expected_titles, expected_episodes) = expected_group_counts(fixture, expected_ids)?;
        if !expected_ids.is_empty()
            && expected_titles <= SEARCH_GROUP_LIMIT
            && expected_episodes <= SEARCH_GROUP_LIMIT
        {
            let intersection = actual_ids
                .iter()
                .filter(|id| expected_ids.contains(**id))
                .count();
            let recall = intersection as f64 / expected_ids.len() as f64;
            summary.recall_sum += recall;
            summary.recall_count = summary.recall_count.saturating_add(1);
        }
    }

    if query.expectation.match_count != Some(0) {
        let ndcg = query_ndcg(
            fixture,
            actual_ids,
            &normalized_query,
            expected.as_ref(),
            explicit_target,
        );
        summary.ndcg_sum += ndcg;
        summary.ndcg_count = summary.ndcg_count.saturating_add(1);
    }

    let grouped_shape = grouped_result_shape(fixture, actual_ids);
    let page_contract_valid = grouped_shape.is_some()
        && usize::try_from(total_matches).is_ok_and(|total| total >= actual_ids.len())
        && has_more == (total_matches > u64::try_from(actual_ids.len())?);
    let category_passed = if explicit_target.is_some() {
        explicit_passed && actual_are_relevant && page_contract_valid
    } else if let Some(expected_ids) = expected {
        expected_page_matches(fixture, &expected_ids, actual_ids, total_matches, has_more)?
    } else if matches!(
        query.category.as_str(),
        "prefix" | "substring" | "one-han" | "two-han"
    ) {
        grouped_shape == Some((SEARCH_GROUP_LIMIT, SEARCH_GROUP_LIMIT))
            && total_matches > u64::try_from(SEARCH_RESULT_LIMIT)?
            && has_more
            && actual_are_relevant
            && page_contract_valid
    } else {
        !actual_ids.is_empty() && actual_are_relevant && page_contract_valid
    };
    category.passed = category.passed.saturating_add(usize::from(category_passed));
    Ok(())
}

fn oracle_expected_ids(
    fixture: &SearchFixture,
    query: &RawQuery,
    normalized_query: &str,
) -> ProbeResult<Option<HashSet<String>>> {
    if query.expectation.match_count == Some(0) {
        return Ok(Some(HashSet::new()));
    }
    if let Some(target_id) = query.expectation.rank1.as_deref() {
        let target = *fixture
            .id_to_item
            .get(target_id)
            .ok_or_else(|| format!("query {} references missing target {target_id}", query.id))?;
        if fixture.items[target].item_type == "Season" {
            let series_id = fixture.items[target]
                .series_id
                .as_deref()
                .ok_or_else(|| format!("season target {target_id} is missing seriesId"))?;
            let season_number = fixture.items[target]
                .season_number
                .ok_or_else(|| format!("season target {target_id} is missing season number"))?;
            return Ok(Some(
                fixture
                    .children_by_series
                    .get(series_id)
                    .into_iter()
                    .flatten()
                    .copied()
                    .filter(|index| {
                        fixture.items[*index].item_type == "Episode"
                            && fixture.items[*index].season_number == Some(season_number)
                            && oracle_matches(&fixture.items[*index], normalized_query)
                    })
                    .map(|index| fixture.items[index].id.clone())
                    .collect(),
            ));
        }
        let mut candidates = vec![target];
        if fixture.items[target].item_type == "Series" {
            if let Some(children) = fixture.children_by_series.get(target_id) {
                candidates.extend(children.iter().copied());
            }
        }
        return Ok(Some(
            candidates
                .into_iter()
                .filter(|index| {
                    search_result_group(&fixture.items[*index]).is_some()
                        && oracle_matches(&fixture.items[*index], normalized_query)
                })
                .map(|index| fixture.items[index].id.clone())
                .collect(),
        ));
    }
    if query.category == "backspace" {
        let mut candidates = HashSet::new();
        for index in &fixture.title_items {
            let item = &fixture.items[*index];
            if !oracle_matches(item, normalized_query) {
                continue;
            }
            candidates.insert(item.id.clone());
            if item.item_type == "Series" {
                if let Some(children) = fixture.children_by_series.get(&item.id) {
                    for child in children {
                        if search_result_group(&fixture.items[*child]).is_some()
                            && oracle_matches(&fixture.items[*child], normalized_query)
                        {
                            candidates.insert(fixture.items[*child].id.clone());
                        }
                    }
                }
            }
        }
        return Ok(Some(candidates));
    }
    Ok(None)
}

fn oracle_matches(item: &CatalogItem, normalized_query: &str) -> bool {
    let mut sources = vec![normalize_search_text(&item.title)];
    if let Some(value) = &item.original_title {
        sources.push(normalize_search_text(value));
    }
    if let Some(value) = &item.series_title {
        sources.push(normalize_search_text(value));
    }
    if let Some(value) = &item.season_title {
        sources.push(normalize_search_text(value));
    }
    sources.extend(
        item.aliases
            .iter()
            .map(|value| normalize_search_text(value)),
    );
    sources.iter().any(|value| value.contains(normalized_query))
        || sources.iter().any(|value| {
            let (full, initials) = oracle_pinyin_forms(value);
            full.iter().any(|form| form.contains(normalized_query))
                || initials.is_some_and(|form| form.contains(normalized_query))
        })
}

fn oracle_pinyin_forms(value: &str) -> (Vec<String>, Option<String>) {
    let mut compact = String::new();
    let mut spaced = Vec::new();
    let mut literal = String::new();
    let mut initials = String::new();
    let mut has_pinyin = false;
    for character in value.chars() {
        if let Some(pinyin) = character.to_pinyin() {
            if !literal.is_empty() {
                spaced.push(std::mem::take(&mut literal));
            }
            has_pinyin = true;
            compact.push_str(pinyin.plain());
            spaced.push(pinyin.plain().to_owned());
            initials.push_str(pinyin.first_letter());
        } else if character.is_alphanumeric() {
            compact.push(character);
            literal.push(character);
            initials.push(character);
        } else if !literal.is_empty() {
            spaced.push(std::mem::take(&mut literal));
        }
    }
    if !literal.is_empty() {
        spaced.push(literal);
    }
    if has_pinyin {
        (vec![compact, spaced.join(" ")], Some(initials))
    } else {
        (Vec::new(), None)
    }
}

fn normalize_search_text(value: &str) -> String {
    let compatibility = value.nfkc().collect::<String>();
    let folded = compatibility.as_str().case_fold().collect::<String>();
    let normalized = folded.as_str().nfkc().collect::<String>();
    let mut result = String::with_capacity(normalized.len());
    let mut pending_space = false;
    for character in normalized.chars() {
        if character.is_whitespace() {
            pending_space = !result.is_empty();
        } else if !character.is_control() {
            if pending_space {
                result.push(' ');
                pending_space = false;
            }
            result.push(character);
        }
    }
    result
}

fn measure_isolated_incremental_updates(
    catalog_path: &Path,
    scope: &CatalogScope,
    workspace: &Path,
    items: &[CatalogItem],
) -> ProbeResult<(Vec<f64>, bool)> {
    let mut updates = items
        .iter()
        .take(INCREMENTAL_ITEM_COUNT)
        .cloned()
        .collect::<Vec<_>>();
    let mut update_samples = Vec::with_capacity(STRICT_OPERATION_SAMPLES);
    let mut alias_versions_valid = true;
    let update_ids = updates
        .iter()
        .map(|item| item.id.clone())
        .collect::<HashSet<_>>();
    let benchmark_path = workspace.join("incremental-benchmark.sqlite3");
    assert_database_checkpointed(catalog_path)?;
    fs::copy(catalog_path, &benchmark_path)?;
    let baseline_catalog = MediaCatalog::open(&benchmark_path, scope.clone())?;
    if baseline_catalog.sync_status()?.cached_count != u64::try_from(SEARCHABLE_ENTITY_COUNT)? {
        return Err(
            "incremental baseline copy does not contain all 140k searchable entities".into(),
        );
    }
    let warmup_marker = "incremental-probe-marker-warmup".to_owned();
    set_incremental_marker(&mut updates, &warmup_marker, 0, 0);
    let warmup_run =
        baseline_catalog.begin_sync(Some(u64::try_from(items.len())?), unix_time_ms()?)?;
    baseline_catalog.upsert_page(
        warmup_run,
        &updates,
        &[],
        u64::try_from(INCREMENTAL_ITEM_COUNT)?,
        Some(u64::try_from(items.len())?),
        unix_time_ms()?,
    )?;
    alias_versions_valid &=
        marker_version_visible(&baseline_catalog, &warmup_marker, None, &update_ids)?;
    baseline_catalog.fail_sync(
        warmup_run,
        "performance probe isolated incremental warmup complete",
        unix_time_ms()?,
    )?;
    drop(baseline_catalog);
    assert_database_checkpointed(&benchmark_path)?;
    let snapshot_directory = workspace.join("incremental-baseline-snapshot");
    fs::create_dir(&snapshot_directory)?;
    copy_catalog_snapshot(&benchmark_path, &snapshot_directory)?;
    for iteration in 0..STRICT_OPERATION_SAMPLES {
        restore_catalog_snapshot(&benchmark_path, &snapshot_directory)?;
        let round_catalog = MediaCatalog::open(&benchmark_path, scope.clone())?;
        if round_catalog.sync_status()?.cached_count != u64::try_from(SEARCHABLE_ENTITY_COUNT)? {
            return Err(format!(
                "incremental round {} did not restore the 140k-entity baseline",
                iteration + 1
            )
            .into());
        }
        let marker = format!("incremental-probe-marker-{iteration:02}");
        set_incremental_marker(&mut updates, &marker, iteration, 0);
        let run = round_catalog.begin_sync(Some(u64::try_from(items.len())?), unix_time_ms()?)?;
        let started = Instant::now();
        round_catalog.upsert_page(
            run,
            &updates,
            &[],
            u64::try_from(INCREMENTAL_ITEM_COUNT)?,
            Some(u64::try_from(items.len())?),
            unix_time_ms()?,
        )?;
        update_samples.push(elapsed_ms(started));
        alias_versions_valid &=
            marker_version_visible(&round_catalog, &marker, Some(&warmup_marker), &update_ids)?;
        round_catalog.fail_sync(
            run,
            "performance probe isolated incremental transaction complete",
            unix_time_ms()?,
        )?;
        drop(round_catalog);
    }
    Ok((update_samples, alias_versions_valid))
}

fn assert_database_checkpointed(catalog_path: &Path) -> ProbeResult<()> {
    let database_name = catalog_path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or("catalog database path has no UTF-8 file name")?;
    let wal_path = catalog_path.with_file_name(format!("{database_name}-wal"));
    if wal_path.is_file() && fs::metadata(&wal_path)?.len() != 0 {
        return Err(format!(
            "cannot snapshot catalog with a non-empty WAL: {}",
            wal_path.display()
        )
        .into());
    }
    Ok(())
}

fn copy_catalog_snapshot(catalog_path: &Path, snapshot_directory: &Path) -> ProbeResult<()> {
    let sidecar_path = current_search_sidecar(catalog_path)?;
    for source in [catalog_path.to_owned(), sidecar_path] {
        let file_name = source
            .file_name()
            .ok_or("catalog snapshot source has no file name")?;
        fs::copy(&source, snapshot_directory.join(file_name))?;
    }
    Ok(())
}

fn restore_catalog_snapshot(catalog_path: &Path, snapshot_directory: &Path) -> ProbeResult<()> {
    MediaCatalog::remove_disposable_files(catalog_path)?;
    for entry in fs::read_dir(snapshot_directory)? {
        let entry = entry?;
        if entry.file_type()?.is_file() {
            fs::copy(entry.path(), catalog_path.with_file_name(entry.file_name()))?;
        }
    }
    Ok(())
}

#[allow(clippy::too_many_lines)]
fn measure_queries_during_incremental(
    catalog: &MediaCatalog,
    items: &[CatalogItem],
    queries: &[RawQuery],
) -> ProbeResult<(Vec<f64>, Vec<f64>, usize, bool)> {
    let mut updates = items
        .iter()
        .take(INCREMENTAL_ITEM_COUNT)
        .cloned()
        .collect::<Vec<_>>();
    let update_ids = updates
        .iter()
        .map(|item| item.id.clone())
        .collect::<HashSet<_>>();
    let mut query_samples = Vec::new();
    let mut writer_samples = Vec::with_capacity(STRICT_OPERATION_SAMPLES);
    let mut queries_succeeded = 0_usize;
    let mut query_failures = 0_usize;
    let mut alias_versions_valid = true;
    let mut previous_marker = None;
    for iteration in 0..STRICT_OPERATION_SAMPLES {
        let marker = format!("query-during-incremental-marker-{iteration:02}");
        set_incremental_marker(&mut updates, &marker, iteration, 1);
        let run = catalog.begin_sync(Some(u64::try_from(items.len())?), unix_time_ms()?)?;
        let updated_at_ms = unix_time_ms()?;
        let incremental_count = u64::try_from(INCREMENTAL_ITEM_COUNT)?;
        let total_count = u64::try_from(items.len())?;
        let barrier = Barrier::new(2);
        let done = AtomicBool::new(false);
        let mut query_index = 0_usize;
        let writer_result = thread::scope(|scope| {
            let writer = scope.spawn(|| {
                barrier.wait();
                let started = Instant::now();
                let result = catalog
                    .upsert_page(
                        run,
                        &updates,
                        &[],
                        incremental_count,
                        Some(total_count),
                        updated_at_ms,
                    )
                    .map_err(|error| error.to_string());
                let elapsed = elapsed_ms(started);
                done.store(true, Ordering::Release);
                result.map(|()| elapsed)
            });
            barrier.wait();
            loop {
                let query = &queries[query_index % CONCURRENT_QUERY_WORKLOAD_SIZE].query;
                query_index = query_index.saturating_add(1);
                let started = Instant::now();
                if catalog.search(query).is_ok() {
                    query_samples.push(elapsed_ms(started));
                    queries_succeeded = queries_succeeded.saturating_add(1);
                } else {
                    query_failures = query_failures.saturating_add(1);
                }
                if done.load(Ordering::Acquire) {
                    break;
                }
            }
            writer
                .join()
                .map_err(|_| "concurrent incremental writer panicked".to_owned())?
        });
        let elapsed = writer_result.map_err(|error| -> Box<dyn Error> { error.into() })?;
        writer_samples.push(elapsed);
        alias_versions_valid &=
            marker_version_visible(catalog, &marker, previous_marker.as_deref(), &update_ids)?;
        catalog.fail_sync(
            run,
            "performance probe concurrent incremental transaction complete",
            unix_time_ms()?,
        )?;
        previous_marker = Some(marker);
    }
    if query_samples.len() < STRICT_OPERATION_SAMPLES {
        return Err(format!(
            "only {} queries overlapped incremental writes; expected at least {STRICT_OPERATION_SAMPLES}",
            query_samples.len()
        )
        .into());
    }
    if query_failures != 0 {
        return Err(format!(
            "{query_failures} production searches failed while an incremental transaction was active"
        )
        .into());
    }
    Ok((
        query_samples,
        writer_samples,
        queries_succeeded,
        alias_versions_valid,
    ))
}

fn set_incremental_marker(
    updates: &mut [CatalogItem],
    marker: &str,
    iteration: usize,
    minute: usize,
) {
    for item in updates {
        item.source_updated_at = Some(format!("2026-08-24T00:{minute:02}:{iteration:02}Z"));
        item.aliases.retain(|alias| {
            !alias.starts_with("incremental-probe-marker-")
                && !alias.starts_with("query-during-incremental-marker-")
        });
        item.aliases.push(marker.to_owned());
    }
}

fn marker_version_visible(
    catalog: &MediaCatalog,
    marker: &str,
    previous_marker: Option<&str>,
    update_ids: &HashSet<String>,
) -> ProbeResult<bool> {
    let current_page = catalog.search(marker)?;
    let mut valid = current_page.items.len() == SEARCH_GROUP_LIMIT
        && current_page.has_more
        && current_page.items.iter().all(|hit| {
            update_ids.contains(hit.item.id.as_str())
                && hit.item.aliases.iter().any(|alias| alias == marker)
        });
    if let Some(previous_marker) = previous_marker {
        let stale_page = catalog.search(previous_marker)?;
        valid &= stale_page.items.is_empty() && !stale_page.has_more;
    }
    Ok(valid)
}

fn correctness_measurements(summary: &CorrectnessSummary) -> Vec<Measurement> {
    vec![
        measurement(
            "search.exact_title_rank1_ratio",
            "ratio",
            vec![ratio(summary.exact_passed, summary.exact_count)],
            production_attributes(json!({
                "judgments": summary.exact_count,
                "passed": summary.exact_passed,
                "definition": "fixture category=exact queries whose independent target must rank first"
            })),
        ),
        measurement(
            "search.recall_at_50",
            "ratio",
            vec![mean(summary.recall_sum, summary.recall_count)],
            production_attributes(json!({
                "judgments": summary.recall_count,
                "eligibility": "independent oracle match count at most 50 within each product result group",
                "groupLimit": SEARCH_GROUP_LIMIT,
                "groups": ["Movie/Series", "Episode"]
            })),
        ),
        measurement(
            "search.mrr_at_10",
            "ratio",
            vec![mean(
                summary.reciprocal_rank_sum,
                summary.reciprocal_rank_count,
            )],
            production_attributes(json!({
                "judgments": summary.reciprocal_rank_count,
                "definition": "reciprocal rank of explicit fixture target within production Top10"
            })),
        ),
        measurement(
            "search.ndcg_at_10",
            "ratio",
            vec![mean(summary.ndcg_sum, summary.ndcg_count)],
            production_attributes(json!({
                "judgments": summary.ndcg_count,
                "relevance": "independent binary fixture oracle"
            })),
        ),
    ]
}

fn measurement(
    id: impl Into<String>,
    unit: impl Into<String>,
    samples: Vec<f64>,
    attributes: Value,
) -> Measurement {
    Measurement {
        id: id.into(),
        unit: unit.into(),
        samples,
        attributes,
    }
}

fn production_attributes(extra: Value) -> Value {
    let mut attributes = json!({
        "evidence": "production-media-catalog",
        "productionApi": "yanami_storage::MediaCatalog",
        "fixture": "F110K-v1",
        "sourceItems": SOURCE_ITEM_COUNT,
        "indexedEntities": SEARCHABLE_ENTITY_COUNT,
        "resultEligibleEntities": SOURCE_ITEM_COUNT,
        "resultGroups": ["Movie/Series", "Episode"],
        "resultLimitPerGroup": SEARCH_GROUP_LIMIT
    });
    if let (Some(attributes), Value::Object(extra)) = (attributes.as_object_mut(), extra) {
        attributes.extend(extra);
    }
    attributes
}

fn hot_sample_count(profile: &str) -> usize {
    match profile {
        "pr" | "pullrequest" => 100,
        "lab" => 500,
        _ => QUERY_COUNT,
    }
}

fn binary_dcg(relevant_count: usize) -> f64 {
    (0..relevant_count.min(10))
        .map(|position| 1.0 / (position as f64 + 2.0).log2())
        .sum()
}

fn mean(sum: f64, count: usize) -> f64 {
    if count == 0 { 0.0 } else { sum / count as f64 }
}

fn ratio(passed: usize, total: usize) -> f64 {
    if total == 0 {
        0.0
    } else {
        passed as f64 / total as f64
    }
}

fn metric_slug(category: &str) -> String {
    category
        .chars()
        .map(|character| {
            if character.is_ascii_alphanumeric() {
                character.to_ascii_lowercase()
            } else {
                '_'
            }
        })
        .collect()
}

fn unix_time_ms() -> ProbeResult<i64> {
    Ok(i64::try_from(
        SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis(),
    )?)
}

fn elapsed_ms(started: Instant) -> f64 {
    started.elapsed().as_secs_f64() * 1_000.0
}

fn catalog_files(path: &Path) -> ProbeResult<Vec<Value>> {
    let mut files = Vec::new();
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let metadata = entry.metadata()?;
        if metadata.is_file() {
            files.push(json!({
                "name": entry.file_name().to_string_lossy(),
                "bytes": metadata.len()
            }));
        }
    }
    files.sort_by(|left, right| left["name"].as_str().cmp(&right["name"].as_str()));
    Ok(files)
}

#[cfg(target_os = "windows")]
fn current_rss_mib() -> Option<f64> {
    let command = format!("(Get-Process -Id {}).WorkingSet64", std::process::id());
    let output = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", &command])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let bytes = String::from_utf8(output.stdout)
        .ok()?
        .trim()
        .parse::<u64>()
        .ok()?;
    Some(bytes as f64 / (1024.0 * 1024.0))
}

#[cfg(target_os = "windows")]
fn process_peak_rss_mib() -> Option<f64> {
    let command = format!("(Get-Process -Id {}).PeakWorkingSet64", std::process::id());
    let output = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", &command])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let bytes = String::from_utf8(output.stdout)
        .ok()?
        .trim()
        .parse::<u64>()
        .ok()?;
    Some(bytes as f64 / (1024.0 * 1024.0))
}

#[cfg(target_os = "linux")]
fn current_rss_mib() -> Option<f64> {
    let statm = fs::read_to_string("/proc/self/statm").ok()?;
    let resident_pages = statm.split_whitespace().nth(1)?.parse::<u64>().ok()?;
    let page_size = Command::new("getconf")
        .arg("PAGESIZE")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .and_then(|value| value.trim().parse::<u64>().ok())?;
    Some(resident_pages.saturating_mul(page_size) as f64 / (1024.0 * 1024.0))
}

#[cfg(target_os = "linux")]
fn process_peak_rss_mib() -> Option<f64> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    let kib = status.lines().find_map(|line| {
        line.strip_prefix("VmHWM:")?
            .split_whitespace()
            .next()?
            .parse::<u64>()
            .ok()
    })?;
    Some(kib as f64 / 1024.0)
}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
fn current_rss_mib() -> Option<f64> {
    None
}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
fn process_peak_rss_mib() -> Option<f64> {
    None
}

#[cfg(target_os = "windows")]
fn rss_collector_name() -> &'static str {
    "Get-Process.WorkingSet64"
}

#[cfg(target_os = "windows")]
fn process_peak_rss_collector_name() -> &'static str {
    "Get-Process.PeakWorkingSet64"
}

#[cfg(target_os = "linux")]
fn process_peak_rss_collector_name() -> &'static str {
    "/proc/self/status VmHWM"
}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
fn process_peak_rss_collector_name() -> &'static str {
    "unavailable"
}

#[cfg(target_os = "linux")]
fn rss_collector_name() -> &'static str {
    "/proc/self/statm"
}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
fn rss_collector_name() -> &'static str {
    "unavailable"
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn independent_normalizer_handles_width_case_and_composed_unicode() {
        assert_eq!(normalize_search_text("  ＣＡＦＥ\u{301}  "), "café");
    }

    #[test]
    fn independent_oracle_derives_compact_and_initial_pinyin() {
        let (full, initials) = oracle_pinyin_forms("星河档案 000123");
        assert!(full.iter().any(|value| value == "xinghedangan000123"));
        assert_eq!(initials.as_deref(), Some("xhda000123"));
    }

    #[test]
    fn binary_dcg_is_normalized_by_the_same_ideal_depth() {
        assert!((binary_dcg(10) / binary_dcg(10) - 1.0).abs() < f64::EPSILON);
        assert!(binary_dcg(5) < binary_dcg(10));
    }
}
