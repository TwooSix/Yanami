//! Ad-hoc F110K audit for the production `MediaCatalog` implementation.
//!
//! Run from the workspace root with:
//! `cargo run -p yanami-storage --example catalog_bench --release -- build/perf-fixture-v13`

use std::{
    collections::{BTreeMap, HashMap, HashSet},
    env,
    fs::{self, File},
    io::{BufRead, BufReader},
    path::{Path, PathBuf},
    sync::Arc,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use serde_json::Value;
use unicode_casefold::UnicodeCaseFold;
use unicode_normalization::UnicodeNormalization;
use yanami_storage::{CatalogItem, CatalogScope, MediaCatalog};

const PAGE_SIZE: usize = 500;
const QUERY_LIMIT: usize = 50;

#[derive(Clone)]
struct FixtureQuery {
    category: String,
    text: String,
    rank1: Option<String>,
    match_count: Option<u64>,
    scan_oracle: bool,
}

struct Fixture {
    source_items: Vec<CatalogItem>,
    derived_seasons: Vec<CatalogItem>,
    queries: Vec<FixtureQuery>,
}

#[allow(clippy::too_many_lines)]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let fixture_dir = env::args_os()
        .nth(1)
        .map_or_else(|| PathBuf::from("build/perf-fixture-v13"), PathBuf::from);
    let keep_path = env::args_os().nth(2).map(PathBuf::from);
    let fixture = load_fixture(&fixture_dir)?;
    let query_sample_count = env_usize("YANAMI_CATALOG_BENCH_QUERY_COUNT", 2_000);
    let category_sample_count = env_usize("YANAMI_CATALOG_BENCH_CATEGORY_COUNT", 200);
    println!(
        "fixture source={} derived_seasons={} queries={}",
        fixture.source_items.len(),
        fixture.derived_seasons.len(),
        fixture.queries.len()
    );
    let resource_pause_ms = env_usize("YANAMI_CATALOG_BENCH_RESOURCE_PAUSE_MS", 0);
    if resource_pause_ms > 0 {
        println!("resource_baseline_pause_ms={resource_pause_ms}");
        thread::sleep(Duration::from_millis(resource_pause_ms as u64));
    }

    let root = keep_path.unwrap_or_else(|| {
        env::temp_dir().join(format!(
            "yanami-catalog-bench-{}-{}",
            std::process::id(),
            now_ms()
        ))
    });
    fs::create_dir_all(&root)?;
    let db_path = root.join("catalog.sqlite3");
    let catalog_open_started = Instant::now();
    let catalog = Arc::new(MediaCatalog::open(
        &db_path,
        CatalogScope::new("bench-profile", "bench-server", "bench-user"),
    )?);
    let catalog_open_ms = catalog_open_started.elapsed().as_secs_f64() * 1_000.0;
    if env::var_os("YANAMI_CATALOG_BENCH_OPEN_ONLY").is_some() {
        println!(
            "catalog_open_only elapsed_ms={catalog_open_ms:.3} cached_count={} disk_bytes={} path={}",
            catalog.sync_status()?.cached_count,
            database_bytes(&db_path),
            db_path.display()
        );
        return Ok(());
    }

    let rebuild_started = Instant::now();
    let run = catalog.begin_sync(
        Some((fixture.source_items.len() + fixture.derived_seasons.len()) as u64),
        now_ms(),
    )?;
    for (page_number, page) in fixture.source_items.chunks(PAGE_SIZE).enumerate() {
        let next = (page_number * PAGE_SIZE + page.len()) as u64;
        catalog.upsert_page(run, page, &[], next, None, now_ms())?;
    }
    let source_rebuild_ms = rebuild_started.elapsed().as_secs_f64() * 1_000.0;
    let seasons_started = Instant::now();
    for (page_number, page) in fixture.derived_seasons.chunks(PAGE_SIZE).enumerate() {
        let next = fixture.source_items.len() + page_number * PAGE_SIZE + page.len();
        catalog.upsert_page(run, page, &[], next as u64, None, now_ms())?;
    }
    let seasons_ms = seasons_started.elapsed().as_secs_f64() * 1_000.0;
    let membership = fixture
        .source_items
        .iter()
        .chain(&fixture.derived_seasons)
        .map(|item| item.id.clone())
        .collect::<HashSet<_>>();
    let final_total = u64::try_from(membership.len())?;
    catalog.verify_sync_membership(run, &membership, final_total, now_ms())?;
    catalog.complete_verified_sync(run, now_ms())?;
    let ready_ms = rebuild_started.elapsed().as_secs_f64() * 1_000.0;
    println!(
        "rebuild source_110k_ms={source_rebuild_ms:.3} derived_30k_ms={seasons_ms:.3} ready_140k_ms={ready_ms:.3}"
    );

    let disk_bytes = database_bytes(&db_path);
    println!(
        "disk bytes={disk_bytes} mib={:.3} path={}",
        disk_bytes as f64 / 1_048_576.0,
        db_path.display()
    );

    audit_correctness(&catalog, &fixture)?;

    for category in ["one-han", "two-han", "prefix", "substring"] {
        let Some(query) = fixture
            .queries
            .iter()
            .find(|query| query.category == category)
        else {
            continue;
        };
        let started = Instant::now();
        catalog.search_with_limit(&query.text, QUERY_LIMIT)?;
        println!(
            "first_query category={category} text={:?} elapsed_ms={:.3}",
            query.text,
            started.elapsed().as_secs_f64() * 1_000.0
        );
    }

    let hot_queries = fixture
        .queries
        .iter()
        .filter(|query| query.category != "one-han")
        .take(query_sample_count)
        .collect::<Vec<_>>();
    for query in hot_queries.iter().take(32) {
        catalog.search_with_limit(&query.text, QUERY_LIMIT)?;
    }
    let hot = measure_queries(&catalog, &hot_queries)?;
    print_samples("query_hot_without_one_han", &hot);

    let all_queries = fixture
        .queries
        .iter()
        .take(query_sample_count)
        .collect::<Vec<_>>();
    let all = measure_queries(&catalog, &all_queries)?;
    print_samples("query_hot_all_categories", &all);
    for category in [
        "exact",
        "prefix",
        "substring",
        "full-width",
        "pinyin-full",
        "pinyin-initials",
        "one-han",
        "two-han",
        "episode-number",
    ] {
        let selected = fixture
            .queries
            .iter()
            .filter(|query| query.category == category)
            .take(category_sample_count)
            .collect::<Vec<_>>();
        print_samples(category, &measure_queries(&catalog, &selected)?);
    }

    drop(catalog);
    let cold_catalog = Arc::new(MediaCatalog::open(
        &db_path,
        CatalogScope::new("bench-profile", "bench-server", "bench-user"),
    )?);
    let cold_queries = fixture.queries.iter().take(500).collect::<Vec<_>>();
    print_samples(
        "query_cold_connection_os_cache_warm",
        &measure_queries(&cold_catalog, &cold_queries)?,
    );

    let mut update_items = fixture.source_items[..1_000].to_vec();
    for (index, item) in update_items.iter_mut().enumerate() {
        item.aliases.push(format!("incremental-alias-{index:04}"));
    }
    let mut incremental_samples = Vec::new();
    for iteration in 0..5 {
        let run = cold_catalog.begin_sync(None, now_ms())?;
        let started = Instant::now();
        cold_catalog.upsert_page(
            run,
            &update_items,
            &[],
            update_items.len() as u64,
            None,
            now_ms(),
        )?;
        incremental_samples.push(started.elapsed().as_secs_f64() * 1_000.0);
        cold_catalog.fail_sync(run, &format!("bench iteration {iteration}"), now_ms())?;
    }
    print_samples("incremental_1000", &incremental_samples);

    let writer_catalog = Arc::clone(&cold_catalog);
    let writer_items = update_items;
    let writer = thread::spawn(move || -> Result<(), String> {
        let run = writer_catalog
            .begin_sync(None, now_ms())
            .map_err(|error| error.to_string())?;
        for (page_number, page) in writer_items.chunks(100).enumerate() {
            writer_catalog
                .upsert_page(
                    run,
                    page,
                    &[],
                    ((page_number + 1) * 100) as u64,
                    None,
                    now_ms(),
                )
                .map_err(|error| error.to_string())?;
            thread::sleep(Duration::from_millis(1));
        }
        writer_catalog
            .fail_sync(run, "bench complete", now_ms())
            .map_err(|error| error.to_string())
    });
    let mut during_write = Vec::new();
    for query in fixture.queries.iter().cycle().take(500) {
        let started = Instant::now();
        cold_catalog.search_with_limit(&query.text, QUERY_LIMIT)?;
        during_write.push(started.elapsed().as_secs_f64() * 1_000.0);
        if writer.is_finished() {
            break;
        }
    }
    writer.join().map_err(|_| "writer thread panicked")??;
    print_samples("query_during_incremental", &during_write);

    println!("benchmark_database={}", root.display());
    Ok(())
}

#[allow(clippy::too_many_lines)]
fn load_fixture(root: &Path) -> Result<Fixture, Box<dyn std::error::Error>> {
    let items_path = root.join("f110k-items.jsonl");
    let mut raw_items = Vec::with_capacity(110_000);
    let mut titles = HashMap::with_capacity(10_000);
    for line in BufReader::new(File::open(items_path)?).lines() {
        let value: Value = serde_json::from_str(&line?)?;
        let id = required_string(&value, "id")?.to_owned();
        let kind = required_string(&value, "kind")?.to_owned();
        let title = required_string(&value, "title")?.to_owned();
        let aliases = value["aliases"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|alias| alias.as_str().map(ToOwned::to_owned))
            .collect::<Vec<_>>();
        if kind != "Episode" {
            titles.insert(id.clone(), title.clone());
        }
        raw_items.push((value, id, kind, title, aliases));
    }

    let mut source_items = Vec::with_capacity(raw_items.len());
    let mut seasons = BTreeMap::<String, CatalogItem>::new();
    for (value, id, kind, title, mut aliases) in raw_items {
        let series_id = value["seriesId"].as_str().map(ToOwned::to_owned);
        let series_title = series_id.as_ref().and_then(|id| titles.get(id)).cloned();
        let season_number = value["season"].as_i64().map(|number| number as i32);
        let episode_number = value["episode"].as_i64().map(|number| number as i32);
        let season_id = series_id
            .as_ref()
            .zip(season_number)
            .map(|(series_id, season)| format!("season:{series_id}:{season}"));
        if let (Some(series_title), Some(season), Some(episode)) =
            (&series_title, season_number, episode_number)
        {
            let padded_code = format!("S{season:02}E{episode:02}");
            aliases.extend([
                padded_code.clone(),
                format!("S{season}E{episode}"),
                format!("{series_title} {padded_code}"),
            ]);
        }
        source_items.push(CatalogItem {
            id: id.clone(),
            item_type: kind.clone(),
            sort_title: title.clone(),
            title,
            parent_id: season_id.clone(),
            series_id: if kind == "Series" {
                Some(id)
            } else {
                series_id.clone()
            },
            series_title: series_title.clone(),
            season_id,
            season_title: season_number.map(|season| format!("第{season}季")),
            season_number,
            episode_number,
            aliases,
            ..CatalogItem::default()
        });

        if let (Some(series_id), Some(series_title), Some(season)) =
            (series_id, series_title, season_number)
        {
            let id = format!("season:{series_id}:{season}");
            seasons.entry(id.clone()).or_insert_with(|| {
                let title = format!("第{season}季");
                let padded_code = format!("S{season:02}");
                CatalogItem {
                    id: id.clone(),
                    item_type: "Season".to_owned(),
                    title: title.clone(),
                    sort_title: format!("{series_title} {season:04}"),
                    parent_id: Some(series_id.clone()),
                    series_id: Some(series_id),
                    series_title: Some(series_title.clone()),
                    season_id: Some(id),
                    season_title: Some(title),
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
                }
            });
        }
    }

    let mut queries = Vec::with_capacity(20_000);
    for line in BufReader::new(File::open(root.join("search-queries-v1.jsonl"))?).lines() {
        let value: Value = serde_json::from_str(&line?)?;
        let expectation = &value["expectation"];
        queries.push(FixtureQuery {
            category: required_string(&value, "category")?.to_owned(),
            text: required_string(&value, "query")?.to_owned(),
            rank1: expectation["rank1"].as_str().map(ToOwned::to_owned),
            match_count: expectation["matchCount"].as_u64(),
            scan_oracle: expectation["oracle"].as_str() == Some("scan-normalized-fixture"),
        });
    }
    Ok(Fixture {
        source_items,
        derived_seasons: seasons.into_values().collect(),
        queries,
    })
}

fn required_string<'a>(value: &'a Value, key: &str) -> Result<&'a str, String> {
    value[key]
        .as_str()
        .ok_or_else(|| format!("fixture field {key} is missing"))
}

fn audit_correctness(
    catalog: &MediaCatalog,
    fixture: &Fixture,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut by_id =
        HashMap::with_capacity(fixture.source_items.len() + fixture.derived_seasons.len());
    for item in fixture
        .source_items
        .iter()
        .chain(fixture.derived_seasons.iter())
    {
        by_id.insert(item.id.as_str(), item);
    }
    let mut checked = BTreeMap::<String, usize>::new();
    let mut failures = Vec::new();
    for query in &fixture.queries {
        let count = checked.entry(query.category.clone()).or_default();
        if *count >= 20 {
            continue;
        }
        *count += 1;
        let page = catalog.search_with_limit(&query.text, QUERY_LIMIT)?;
        let ids = page
            .items
            .iter()
            .map(|hit| hit.item.id.as_str())
            .collect::<Vec<_>>();
        if ids.iter().copied().collect::<HashSet<_>>().len() != ids.len() {
            failures.push(format!("{} duplicate Top50 rows", query.category));
        }
        if let Some(expected) = &query.rank1 {
            if ids.first().copied() != Some(expected.as_str()) {
                failures.push(format!(
                    "{} {:?}: rank1 expected={} actual={:?}",
                    query.category,
                    query.text,
                    expected,
                    ids.first()
                ));
            }
        } else if let Some(expected) = query.match_count {
            if page.total_matches != expected {
                failures.push(format!(
                    "{} {:?}: matches expected={} actual={}",
                    query.category, query.text, expected, page.total_matches
                ));
            }
        } else if query.scan_oracle {
            let normalized = normalize(&query.text);
            for id in ids {
                let Some(item) = by_id.get(id) else {
                    failures.push(format!("{} unknown id={id}", query.category));
                    continue;
                };
                let fields = std::iter::once(item.title.as_str())
                    .chain(item.original_title.as_deref())
                    .chain(item.series_title.as_deref())
                    .chain(item.season_title.as_deref())
                    .chain(item.aliases.iter().map(String::as_str));
                if !fields
                    .map(normalize)
                    .any(|field| field.contains(&normalized))
                {
                    failures.push(format!(
                        "{} {:?}: non-matching Top50 id={id}",
                        query.category, query.text
                    ));
                }
            }
        }
    }
    if !failures.is_empty() {
        for failure in failures.iter().take(30) {
            eprintln!("correctness_failure: {failure}");
        }
        return Err(format!("{} correctness checks failed", failures.len()).into());
    }
    println!("correctness checked={checked:?} failures=0");
    Ok(())
}

fn measure_queries(
    catalog: &MediaCatalog,
    queries: &[&FixtureQuery],
) -> Result<Vec<f64>, Box<dyn std::error::Error>> {
    let mut samples = Vec::with_capacity(queries.len());
    for query in queries {
        let started = Instant::now();
        catalog.search_with_limit(&query.text, QUERY_LIMIT)?;
        samples.push(started.elapsed().as_secs_f64() * 1_000.0);
    }
    Ok(samples)
}

fn print_samples(label: &str, samples: &[f64]) {
    if samples.is_empty() {
        println!("{label} n=0");
        return;
    }
    let mut sorted = samples.to_vec();
    sorted.sort_by(f64::total_cmp);
    println!(
        "{label} n={} p50_ms={:.3} p95_ms={:.3} p99_ms={:.3} max_ms={:.3}",
        sorted.len(),
        percentile(&sorted, 0.50),
        percentile(&sorted, 0.95),
        percentile(&sorted, 0.99),
        sorted[sorted.len() - 1]
    );
}

fn percentile(sorted: &[f64], percentile: f64) -> f64 {
    let index = ((sorted.len() - 1) as f64 * percentile).ceil() as usize;
    sorted[index]
}

fn database_bytes(path: &Path) -> u64 {
    let mut total = 0;
    for suffix in ["", "-wal", "-shm"] {
        let candidate = PathBuf::from(format!("{}{suffix}", path.display()));
        if let Ok(metadata) = fs::metadata(candidate) {
            total += metadata.len();
        }
    }
    total
}

fn normalize(value: &str) -> String {
    let compatibility = value.nfkc().collect::<String>();
    let folded = compatibility.as_str().case_fold().collect::<String>();
    folded.as_str().nfkc().collect::<String>()
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock must be after Unix epoch")
        .as_millis() as i64
}

fn env_usize(name: &str, default: usize) -> usize {
    env::var(name)
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(default)
}
