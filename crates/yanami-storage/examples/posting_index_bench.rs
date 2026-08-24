//! Compact field-grouped byte-gram postings prototype for the F110K fixture.
//!
//! The build process writes a self-contained delta-varint sidecar. A fresh
//! child process then reopens it as one byte blob: document offsets, string
//! sections, posting directory entries, and compressed posting lists are read
//! in place without recreating `String`s or a resident `Vec<u32>` per list.
//!
//! Run from the workspace root:
//! `cargo run -p yanami-storage --example posting_index_bench --release -- build/perf-fixture-v13`

use std::{
    array,
    collections::{BTreeMap, HashMap, HashSet},
    env,
    ffi::OsString,
    fs::{self, File},
    hint::black_box,
    io::{self, BufRead, BufReader, BufWriter, Write},
    mem::{size_of, size_of_val},
    path::{Path, PathBuf},
    process::Command,
    time::{Instant, SystemTime, UNIX_EPOCH},
};

use pinyin::ToPinyin;
use serde_json::Value;
use unicode_casefold::UnicodeCaseFold;
use unicode_normalization::UnicodeNormalization;

const MAGIC: &[u8; 8] = b"YPGIDX03";
const HEADER_LEN: usize = 24;
const DIRECTORY_ENTRY_LEN: usize = 22;
const DOC_SECTION_COUNT: usize = 7;
const DOC_BOUNDARY_COUNT: usize = DOC_SECTION_COUNT + 1;
const DOC_HEADER_LEN: usize = DOC_BOUNDARY_COUNT * size_of::<u32>();
const SOURCE_ITEMS: usize = 110_000;
const DERIVED_SEASONS: usize = 30_000;
const QUERY_LIMIT: usize = 50;
const CORRECTNESS_PER_CATEGORY: usize = 20;
const MAX_GRAMS_PER_GROUP: usize = 3;
const GROUP_COUNT: usize = 6;
const RANK_COUNT: usize = 13;
const NO_RANK: u8 = u8::MAX;

const GROUP_TITLE: u8 = 0;
const GROUP_ORIGINAL: u8 = 1;
const GROUP_ALIASES: u8 = 2;
const GROUP_SERIES_SEASON: u8 = 3;
const GROUP_PINYIN_FULL: u8 = 4;
const GROUP_PINYIN_INITIALS: u8 = 5;

const KIND_BYTE_GRAM: u8 = 0;
const KIND_SCALAR_GRAM: u8 = 1;
const KIND_PREFIX_SCALAR: u8 = 2;
const KIND_EXACT_SCALAR: u8 = 3;

const SECTION_ID: usize = 0;
const SECTION_TITLE: usize = 1;
const SECTION_ORIGINAL: usize = 2;
const SECTION_ALIASES: usize = 3;
const SECTION_SERIES_SEASON: usize = 4;
const SECTION_PINYIN_FULL: usize = 5;
const SECTION_PINYIN_INITIALS: usize = 6;

const GROUP_NAMES: [&str; GROUP_COUNT] = [
    "title[0,3,6]",
    "original[1,4,7]",
    "aliases[2,7]",
    "series-season[5,8]",
    "pinyin-full[9,11]",
    "pinyin-initials[10,12]",
];

#[derive(Clone)]
struct FixtureQuery {
    category: String,
    text: String,
    rank1: Option<String>,
    match_count: Option<u64>,
}

struct RawItem {
    id: String,
    kind: String,
    title: String,
    aliases: Vec<String>,
    series_id: Option<String>,
    season: Option<i32>,
    episode: Option<i32>,
}

struct BuildDocument {
    sort_key: String,
    document: Document,
}

#[derive(Default)]
struct Document {
    id: String,
    title: String,
    original_title: String,
    series_title: String,
    season_title: String,
    aliases: Vec<String>,
    pinyin_full: Vec<String>,
    pinyin_initials: Vec<String>,
}

#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
struct PostingKey {
    kind: u8,
    group: u8,
    key: u64,
}

struct BuildPostingEntry {
    posting_key: PostingKey,
    documents: Vec<u32>,
}

struct BuildIndex {
    documents: Vec<Document>,
    entries: Vec<BuildPostingEntry>,
    posting_count: usize,
}

#[derive(Clone, Copy)]
struct PostingMeta {
    data_offset: u32,
    byte_len: u32,
    count: u32,
}

struct RuntimeIndex {
    blob: Vec<u8>,
    document_count: u32,
    document_offsets_at: usize,
    document_pool_at: usize,
    directory_at: usize,
    entry_count: u32,
    postings_at: usize,
}

#[derive(Debug)]
struct SearchResult {
    top: Vec<(u32, u8)>,
    total_matches: u64,
    has_more: bool,
    candidate_unique: u64,
    candidate_checks: u64,
    group_candidates: [u32; GROUP_COUNT],
}

struct SearchScratch {
    ranks: Vec<u8>,
    rank_generations: Vec<u32>,
    generation: u32,
    match_bits: Vec<u64>,
    candidate_bits: Vec<u64>,
    touched_match_words: Vec<u32>,
    touched_candidate_words: Vec<u32>,
    candidates: Vec<u32>,
    intersection: Vec<u32>,
    buckets: [Vec<u32>; RANK_COUNT],
    group_candidates: [u32; GROUP_COUNT],
}

struct SidecarStats {
    document_section: u64,
    posting_section: u64,
    posting_blob: u64,
    file_size: u64,
}

#[derive(Clone)]
struct QuerySample {
    elapsed_ms: f64,
    candidate_unique: u64,
    candidate_checks: u64,
    total_matches: u64,
    group_candidates: [u32; GROUP_COUNT],
}

struct Measurements {
    categories: BTreeMap<String, Vec<QuerySample>>,
    substrings: BTreeMap<String, Vec<QuerySample>>,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arguments = env::args_os().collect::<Vec<_>>();
    if arguments
        .get(1)
        .is_some_and(|argument| argument == "--reopen-bench")
    {
        let sidecar = required_path_argument(&arguments, 2, "sidecar path")?;
        let fixture = required_path_argument(&arguments, 3, "fixture path")?;
        return run_reopen_benchmark(&sidecar, &fixture);
    }
    run_build_benchmark(&arguments)
}

fn run_build_benchmark(arguments: &[OsString]) -> Result<(), Box<dyn std::error::Error>> {
    let fixture_dir = arguments
        .get(1)
        .map_or_else(|| PathBuf::from("build/perf-fixture-v13"), PathBuf::from);
    let sidecar_path = arguments.get(2).map_or_else(
        || {
            env::temp_dir().join(format!(
                "yanami-posting-index-{}-{}.ypg",
                std::process::id(),
                now_ms()
            ))
        },
        PathBuf::from,
    );

    let rss_baseline = resident_bytes();
    let load_started = Instant::now();
    let documents = load_fixture_documents(&fixture_dir)?;
    let fixture_ms = elapsed_ms(load_started);
    let rss_documents = resident_bytes();
    println!(
        "fixture source={SOURCE_ITEMS} derived_seasons={DERIVED_SEASONS} total_documents={} load_prepare_sort_ms={fixture_ms:.3}",
        documents.len()
    );

    let build_started = Instant::now();
    let index = BuildIndex::build(documents)?;
    let build_ms = elapsed_ms(build_started);
    let rss_built = resident_bytes();
    let heap_bytes = index.estimated_heap_bytes();
    println!(
        "build byte_grams=1,2,3 field_rank_groups={GROUP_COUNT} entries={} posting_doc_ids={} build_ms={build_ms:.3} heap_estimate_bytes={heap_bytes} heap_estimate_mib={:.3}",
        index.entries.len(),
        index.posting_count,
        mib(heap_bytes as u64)
    );
    print_rss("build_rss_baseline", rss_baseline);
    print_rss_delta("build_rss_after_documents", rss_documents, rss_baseline);
    print_rss_delta("build_rss_after_postings", rss_built, rss_baseline);

    if let Some(parent) = sidecar_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let write_started = Instant::now();
    let sidecar_stats = index.write_sidecar(&sidecar_path)?;
    let write_ms = elapsed_ms(write_started);
    let raw_posting_bytes = index.posting_count as u64 * size_of::<u32>() as u64;
    println!(
        "sidecar write_ms={write_ms:.3} total_bytes={} total_mib={:.3} document_section_bytes={} posting_section_bytes={} posting_blob_bytes={} raw_posting_bytes={raw_posting_bytes} posting_blob_ratio={:.4} path={}",
        sidecar_stats.file_size,
        mib(sidecar_stats.file_size),
        sidecar_stats.document_section,
        sidecar_stats.posting_section,
        sidecar_stats.posting_blob,
        sidecar_stats.posting_blob as f64 / raw_posting_bytes.max(1) as f64,
        sidecar_path.display()
    );
    drop(index);

    println!("launching_fresh_reopen_process=true");
    let status = Command::new(env::current_exe()?)
        .arg("--reopen-bench")
        .arg(&sidecar_path)
        .arg(&fixture_dir)
        .status()?;
    if !status.success() {
        return Err(format!("fresh reopen benchmark failed with {status}").into());
    }
    println!("benchmark_sidecar={}", sidecar_path.display());
    Ok(())
}

fn run_reopen_benchmark(
    sidecar_path: &Path,
    fixture_dir: &Path,
) -> Result<(), Box<dyn std::error::Error>> {
    let rss_baseline = resident_bytes();
    let reopen_started = Instant::now();
    let index = RuntimeIndex::read_sidecar(sidecar_path)?;
    let reopen_ms = elapsed_ms(reopen_started);
    let rss_reopened = resident_bytes();
    println!(
        "reopen representation=single-sidecar-byte-blob copied_strings=0 decoded_resident_posting_u32=0 documents={} entries={} blob_bytes={} reopen_validate_ms={reopen_ms:.3} query_result_cache=false grams_intersected_per_group=1..={MAX_GRAMS_PER_GROUP}",
        index.document_count,
        index.entry_count,
        index.blob.capacity()
    );
    print_rss("runtime_rss_baseline", rss_baseline);
    print_rss_delta("runtime_rss_after_reopen", rss_reopened, rss_baseline);
    println!(
        "runtime_heap_index_bytes={} runtime_heap_index_mib={:.3}",
        index.estimated_heap_bytes(),
        mib(index.estimated_heap_bytes() as u64)
    );

    let queries = load_fixture_queries(fixture_dir)?;
    let mut scratch = SearchScratch::new(index.document_count as usize);
    let rss_ready = resident_bytes();
    println!(
        "queries={} scratch_bytes={}",
        queries.len(),
        scratch.estimated_heap_bytes()
    );
    print_rss_delta("runtime_rss_with_queries_scratch", rss_ready, rss_baseline);

    let first_pass = measure_queries(&index, &queries, &mut scratch);
    print_category_samples(
        "cold_reopen_first_pass_os_cache_warm",
        &first_pass.categories,
    );
    print_substring_samples(
        "cold_reopen_first_pass_os_cache_warm",
        &first_pass.substrings,
    );
    let hot_pass = measure_queries(&index, &queries, &mut scratch);
    print_category_samples("hot_second_pass", &hot_pass.categories);
    print_substring_samples("hot_second_pass", &hot_pass.substrings);
    let rss_steady = resident_bytes();
    print_rss_delta("runtime_rss_steady_after_hot", rss_steady, rss_baseline);

    audit_correctness(&index, &queries, &mut scratch)?;
    println!(
        "result_contract top_k={QUERY_LIMIT} has_more_exact=true total_matches_exact=true stable_order=normalized_sort_title_then_item_id dedup=rank-byte-array-plus-bitset count=popcount"
    );
    Ok(())
}

fn required_path_argument(
    arguments: &[OsString],
    index: usize,
    label: &str,
) -> Result<PathBuf, Box<dyn std::error::Error>> {
    arguments
        .get(index)
        .map(PathBuf::from)
        .ok_or_else(|| format!("missing {label}").into())
}

#[allow(clippy::too_many_lines)]
fn load_fixture_documents(root: &Path) -> Result<Vec<Document>, Box<dyn std::error::Error>> {
    let mut raw_items = Vec::with_capacity(SOURCE_ITEMS);
    let mut titles = HashMap::with_capacity(10_000);
    for line in BufReader::new(File::open(root.join("f110k-items.jsonl"))?).lines() {
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
        raw_items.push(RawItem {
            id,
            kind,
            title,
            aliases,
            series_id: value["seriesId"].as_str().map(ToOwned::to_owned),
            season: value["season"].as_i64().map(|number| number as i32),
            episode: value["episode"].as_i64().map(|number| number as i32),
        });
    }
    if raw_items.len() != SOURCE_ITEMS {
        return Err(format!(
            "fixture source item count mismatch: expected={SOURCE_ITEMS} actual={}",
            raw_items.len()
        )
        .into());
    }

    let mut build_documents = Vec::with_capacity(SOURCE_ITEMS + DERIVED_SEASONS);
    let mut seasons = BTreeMap::<String, BuildDocument>::new();
    for raw in raw_items {
        let series_title = raw
            .series_id
            .as_ref()
            .and_then(|series_id| titles.get(series_id))
            .cloned();
        let mut aliases = raw.aliases;
        if let (Some(series_title), Some(season), Some(episode)) =
            (&series_title, raw.season, raw.episode)
        {
            aliases.push(format!("{series_title} S{season:02}E{episode:02}"));
        }
        build_documents.push(prepare_document(
            raw.id,
            &raw.title,
            &raw.title,
            "",
            series_title.as_deref().unwrap_or_default(),
            "",
            aliases,
        ));

        if raw.kind == "Episode" {
            if let (Some(series_id), Some(series_title), Some(season)) =
                (raw.series_id, series_title, raw.season)
            {
                let id = format!("season:{series_id}:{season}");
                seasons.entry(id.clone()).or_insert_with(|| {
                    let title = format!("{series_title} 第{season}季");
                    prepare_document(id, &title, &title, "", &series_title, "", Vec::new())
                });
            }
        }
    }
    if seasons.len() != DERIVED_SEASONS {
        return Err(format!(
            "derived season count mismatch: expected={DERIVED_SEASONS} actual={}",
            seasons.len()
        )
        .into());
    }
    build_documents.extend(seasons.into_values());
    build_documents.sort_unstable_by(|left, right| {
        (&left.sort_key, &left.document.id).cmp(&(&right.sort_key, &right.document.id))
    });
    Ok(build_documents
        .into_iter()
        .map(|build| build.document)
        .collect())
}

fn load_fixture_queries(root: &Path) -> Result<Vec<FixtureQuery>, Box<dyn std::error::Error>> {
    let mut queries = Vec::with_capacity(20_000);
    for line in BufReader::new(File::open(root.join("search-queries-v1.jsonl"))?).lines() {
        let value: Value = serde_json::from_str(&line?)?;
        let expectation = &value["expectation"];
        queries.push(FixtureQuery {
            category: required_string(&value, "category")?.to_owned(),
            text: required_string(&value, "query")?.to_owned(),
            rank1: expectation["rank1"].as_str().map(ToOwned::to_owned),
            match_count: expectation["matchCount"].as_u64(),
        });
    }
    Ok(queries)
}

#[allow(clippy::too_many_arguments)]
fn prepare_document(
    id: String,
    sort_title: &str,
    title: &str,
    original_title: &str,
    series_title: &str,
    season_title: &str,
    aliases: Vec<String>,
) -> BuildDocument {
    let mut seen_aliases = HashSet::new();
    let aliases = aliases
        .into_iter()
        .filter_map(|alias| {
            let alias = normalize_search_text(alias.trim());
            (!alias.is_empty() && seen_aliases.insert(alias.clone())).then_some(alias)
        })
        .collect::<Vec<_>>();
    let title = normalize_search_text(title);
    let original_title = normalize_search_text(original_title);
    let series_title = normalize_search_text(series_title);
    let season_title = normalize_search_text(season_title);
    let normalized_sources = std::iter::once(title.as_str())
        .chain((!original_title.is_empty()).then_some(original_title.as_str()))
        .chain((!series_title.is_empty()).then_some(series_title.as_str()))
        .chain((!season_title.is_empty()).then_some(season_title.as_str()))
        .chain(aliases.iter().map(String::as_str))
        .collect::<Vec<_>>();
    let (pinyin_full, pinyin_initials) = pinyin_forms(&normalized_sources);
    BuildDocument {
        sort_key: normalize_search_text(sort_title),
        document: Document {
            id,
            title,
            original_title,
            series_title,
            season_title,
            aliases,
            pinyin_full,
            pinyin_initials,
        },
    }
}

fn pinyin_forms(values: &[&str]) -> (Vec<String>, Vec<String>) {
    let mut full_documents = Vec::new();
    let mut initial_documents = Vec::new();
    for value in values {
        let mut compact = String::new();
        let mut syllables = Vec::new();
        let mut initials = String::new();
        let mut literal_run = String::new();
        let mut has_pinyin = false;
        for character in value.chars() {
            if let Some(pinyin) = character.to_pinyin() {
                if !literal_run.is_empty() {
                    syllables.push(std::mem::take(&mut literal_run));
                }
                has_pinyin = true;
                compact.push_str(pinyin.plain());
                syllables.push(pinyin.plain().to_owned());
                initials.push_str(pinyin.first_letter());
            } else if character.is_alphanumeric() {
                compact.push(character);
                literal_run.push(character);
                initials.push(character);
            } else if !literal_run.is_empty() {
                syllables.push(std::mem::take(&mut literal_run));
            }
        }
        if !literal_run.is_empty() {
            syllables.push(literal_run);
        }
        if has_pinyin && !compact.is_empty() {
            full_documents.push(compact);
            full_documents.push(syllables.join(" "));
            initial_documents.push(initials);
        }
    }
    (full_documents, initial_documents)
}

impl Document {
    fn for_each_group_field(&self, group: u8, mut visitor: impl FnMut(&str)) {
        match group {
            GROUP_TITLE => visitor(&self.title),
            GROUP_ORIGINAL => {
                if !self.original_title.is_empty() {
                    visitor(&self.original_title);
                }
            }
            GROUP_ALIASES => self.aliases.iter().for_each(|value| visitor(value)),
            GROUP_SERIES_SEASON => {
                if !self.series_title.is_empty() {
                    visitor(&self.series_title);
                }
                if !self.season_title.is_empty() {
                    visitor(&self.season_title);
                }
            }
            GROUP_PINYIN_FULL => self.pinyin_full.iter().for_each(|value| visitor(value)),
            GROUP_PINYIN_INITIALS => self.pinyin_initials.iter().for_each(|value| visitor(value)),
            _ => unreachable!("known field/rank group"),
        }
    }

    fn for_each_prefix_field(&self, group: u8, mut visitor: impl FnMut(&str)) {
        match group {
            GROUP_TITLE => visitor(&self.title),
            GROUP_ORIGINAL => {
                if !self.original_title.is_empty() {
                    visitor(&self.original_title);
                }
            }
            GROUP_SERIES_SEASON => {
                if !self.series_title.is_empty() {
                    visitor(&self.series_title);
                }
                if !self.season_title.is_empty() {
                    visitor(&self.season_title);
                }
            }
            GROUP_PINYIN_FULL => {
                if let Some(value) = self.pinyin_full.first() {
                    visitor(value);
                }
            }
            GROUP_PINYIN_INITIALS => {
                if let Some(value) = self.pinyin_initials.first() {
                    visitor(value);
                }
            }
            GROUP_ALIASES => {}
            _ => unreachable!("known field/rank group"),
        }
    }

    fn for_each_exact_field(&self, group: u8, mut visitor: impl FnMut(&str)) {
        match group {
            GROUP_TITLE => visitor(&self.title),
            GROUP_ORIGINAL => {
                if !self.original_title.is_empty() {
                    visitor(&self.original_title);
                }
            }
            GROUP_ALIASES => self.aliases.iter().for_each(|value| visitor(value)),
            GROUP_SERIES_SEASON | GROUP_PINYIN_FULL | GROUP_PINYIN_INITIALS => {}
            _ => unreachable!("known field/rank group"),
        }
    }
}

impl BuildIndex {
    fn build(documents: Vec<Document>) -> Result<Self, Box<dyn std::error::Error>> {
        if documents.len() > u32::MAX as usize {
            return Err("prototype supports at most u32::MAX documents".into());
        }
        let mut map = HashMap::<PostingKey, Vec<u32>>::new();
        let mut keys = Vec::with_capacity(512);
        for (doc_id, document) in documents.iter().enumerate() {
            for group in 0..GROUP_COUNT as u8 {
                keys.clear();
                document.for_each_group_field(group, |field| {
                    add_all_byte_gram_keys(group, field.as_bytes(), &mut keys);
                    add_all_scalar_gram_keys(group, field, &mut keys);
                });
                document.for_each_prefix_field(group, |field| {
                    add_prefix_scalar_keys(group, field, &mut keys);
                });
                document.for_each_exact_field(group, |field| {
                    add_exact_scalar_key(group, field, &mut keys);
                });
                keys.sort_unstable();
                keys.dedup();
                for key in &keys {
                    map.entry(*key).or_default().push(doc_id as u32);
                }
            }
        }
        let mut lists = map.into_iter().collect::<Vec<_>>();
        lists.sort_unstable_by_key(|(key, _)| *key);
        let posting_count = lists.iter().map(|(_, docs)| docs.len()).sum::<usize>();
        let entries = lists
            .into_iter()
            .map(|(posting_key, documents)| BuildPostingEntry {
                posting_key,
                documents,
            })
            .collect();
        Ok(Self {
            documents,
            entries,
            posting_count,
        })
    }

    fn estimated_heap_bytes(&self) -> usize {
        let mut bytes = self.documents.capacity() * size_of::<Document>()
            + self.entries.capacity() * size_of::<BuildPostingEntry>();
        for document in &self.documents {
            bytes += document.id.capacity()
                + document.title.capacity()
                + document.original_title.capacity()
                + document.series_title.capacity()
                + document.season_title.capacity();
            bytes += vector_string_heap(&document.aliases);
            bytes += vector_string_heap(&document.pinyin_full);
            bytes += vector_string_heap(&document.pinyin_initials);
        }
        bytes
            + self
                .entries
                .iter()
                .map(|entry| entry.documents.capacity() * size_of::<u32>())
                .sum::<usize>()
    }

    fn write_sidecar(&self, path: &Path) -> io::Result<SidecarStats> {
        let document_offsets = build_document_offsets(&self.documents)?;
        let document_pool_len = *document_offsets
            .last()
            .ok_or_else(|| invalid_data("missing final document offset"))?;
        let mut posting_offsets = Vec::with_capacity(self.entries.len());
        let mut posting_blob_len = 0_u32;
        for entry in &self.entries {
            let byte_len = posting_encoded_len(&entry.documents)?;
            posting_offsets.push((posting_blob_len, byte_len));
            posting_blob_len = posting_blob_len
                .checked_add(byte_len)
                .ok_or_else(|| invalid_data("compressed postings exceed u32 sidecar limit"))?;
        }
        let document_count = u32::try_from(self.documents.len())
            .map_err(|_| invalid_data("document count exceeds u32"))?;
        let entry_count = u32::try_from(self.entries.len())
            .map_err(|_| invalid_data("posting entry count exceeds u32"))?;

        let mut writer = CountingWriter::new(BufWriter::new(File::create(path)?));
        writer.write_all(MAGIC)?;
        write_u32(&mut writer, document_count)?;
        write_u32(&mut writer, document_pool_len)?;
        write_u32(&mut writer, entry_count)?;
        write_u32(&mut writer, posting_blob_len)?;
        for offset in &document_offsets {
            write_u32(&mut writer, *offset)?;
        }
        for document in &self.documents {
            write_document(&mut writer, document)?;
        }
        let document_section = writer.bytes;
        for (entry, (data_offset, byte_len)) in self.entries.iter().zip(&posting_offsets) {
            writer.write_all(&[entry.posting_key.kind, entry.posting_key.group])?;
            write_u64(&mut writer, entry.posting_key.key)?;
            write_u32(&mut writer, *data_offset)?;
            write_u32(&mut writer, *byte_len)?;
            write_u32(
                &mut writer,
                u32::try_from(entry.documents.len())
                    .map_err(|_| invalid_data("posting list count exceeds u32"))?,
            )?;
        }
        for entry in &self.entries {
            write_posting_list(&mut writer, &entry.documents)?;
        }
        writer.flush()?;
        let file_size = writer.bytes;
        Ok(SidecarStats {
            document_section,
            posting_section: file_size - document_section,
            posting_blob: u64::from(posting_blob_len),
            file_size,
        })
    }
}

fn build_document_offsets(documents: &[Document]) -> io::Result<Vec<u32>> {
    let mut offsets = Vec::with_capacity(documents.len() + 1);
    let mut current = 0_u32;
    offsets.push(current);
    for document in documents {
        let len = document_encoded_len(document)?;
        current = current
            .checked_add(len)
            .ok_or_else(|| invalid_data("document string pool exceeds u32 sidecar limit"))?;
        offsets.push(current);
    }
    Ok(offsets)
}

fn document_encoded_len(document: &Document) -> io::Result<u32> {
    let sections = document_section_lengths(document)?;
    let total = sections.iter().try_fold(DOC_HEADER_LEN, |total, section| {
        total
            .checked_add(*section)
            .ok_or_else(|| invalid_data("document record length overflow"))
    })?;
    u32::try_from(total).map_err(|_| invalid_data("document record exceeds u32"))
}

fn document_section_lengths(document: &Document) -> io::Result<[usize; DOC_SECTION_COUNT]> {
    Ok([
        document.id.len(),
        document.title.len(),
        document.original_title.len(),
        encoded_string_list_len(&document.aliases)?,
        encoded_string_refs_len(
            [&document.series_title, &document.season_title]
                .into_iter()
                .map(String::as_str)
                .filter(|value| !value.is_empty()),
        )?,
        encoded_string_list_len(&document.pinyin_full)?,
        encoded_string_list_len(&document.pinyin_initials)?,
    ])
}

fn write_document(writer: &mut impl Write, document: &Document) -> io::Result<()> {
    let lengths = document_section_lengths(document)?;
    let mut boundary =
        u32::try_from(DOC_HEADER_LEN).map_err(|_| invalid_data("document header exceeds u32"))?;
    write_u32(writer, boundary)?;
    for length in lengths {
        boundary = boundary
            .checked_add(
                u32::try_from(length).map_err(|_| invalid_data("document section exceeds u32"))?,
            )
            .ok_or_else(|| invalid_data("document boundary overflow"))?;
        write_u32(writer, boundary)?;
    }
    writer.write_all(document.id.as_bytes())?;
    writer.write_all(document.title.as_bytes())?;
    writer.write_all(document.original_title.as_bytes())?;
    write_string_list(writer, &document.aliases)?;
    write_string_refs(
        writer,
        [&document.series_title, &document.season_title]
            .into_iter()
            .map(String::as_str)
            .filter(|value| !value.is_empty()),
    )?;
    write_string_list(writer, &document.pinyin_full)?;
    write_string_list(writer, &document.pinyin_initials)
}

impl RuntimeIndex {
    #[allow(clippy::too_many_lines)]
    fn read_sidecar(path: &Path) -> io::Result<Self> {
        let blob = fs::read(path)?;
        if blob.len() < HEADER_LEN || &blob[..MAGIC.len()] != MAGIC {
            return Err(invalid_data("sidecar magic/version mismatch"));
        }
        let document_count = read_u32_at(&blob, 8)?;
        let document_pool_len = read_u32_at(&blob, 12)?;
        let entry_count = read_u32_at(&blob, 16)?;
        let posting_blob_len = read_u32_at(&blob, 20)?;
        let document_offsets_at = HEADER_LEN;
        let document_offsets_len = (document_count as usize + 1)
            .checked_mul(size_of::<u32>())
            .ok_or_else(|| invalid_data("document offset table length overflow"))?;
        let document_pool_at = document_offsets_at
            .checked_add(document_offsets_len)
            .ok_or_else(|| invalid_data("document pool offset overflow"))?;
        let directory_at = document_pool_at
            .checked_add(document_pool_len as usize)
            .ok_or_else(|| invalid_data("posting directory offset overflow"))?;
        let directory_len = (entry_count as usize)
            .checked_mul(DIRECTORY_ENTRY_LEN)
            .ok_or_else(|| invalid_data("posting directory length overflow"))?;
        let postings_at = directory_at
            .checked_add(directory_len)
            .ok_or_else(|| invalid_data("posting data offset overflow"))?;
        let expected_len = postings_at
            .checked_add(posting_blob_len as usize)
            .ok_or_else(|| invalid_data("sidecar length overflow"))?;
        if expected_len != blob.len() {
            return Err(invalid_data("sidecar section lengths do not match file"));
        }
        let index = Self {
            blob,
            document_count,
            document_offsets_at,
            document_pool_at,
            directory_at,
            entry_count,
            postings_at,
        };
        index.validate_documents(document_pool_len)?;
        index.validate_postings(posting_blob_len)?;
        Ok(index)
    }

    fn validate_documents(&self, document_pool_len: u32) -> io::Result<()> {
        let first = self.document_relative_offset(0)?;
        let last = self.document_relative_offset(self.document_count)?;
        if first != 0 || last != document_pool_len {
            return Err(invalid_data("document offset table endpoints are invalid"));
        }
        let mut previous = first;
        for doc_id in 0..self.document_count {
            let next = self.document_relative_offset(doc_id + 1)?;
            if next <= previous {
                return Err(invalid_data("document offsets are not strictly ordered"));
            }
            self.validate_document_record(doc_id)?;
            previous = next;
        }
        Ok(())
    }

    fn validate_document_record(&self, doc_id: u32) -> io::Result<()> {
        let record = self.document_record(doc_id)?;
        if record.len() < DOC_HEADER_LEN {
            return Err(invalid_data("document record is shorter than fixed header"));
        }
        let mut previous = u32::try_from(DOC_HEADER_LEN)
            .map_err(|_| invalid_data("document header exceeds u32"))?;
        for boundary_index in 0..DOC_BOUNDARY_COUNT {
            let boundary = read_u32_at(record, boundary_index * size_of::<u32>())?;
            if boundary_index == 0 && boundary != previous {
                return Err(invalid_data("document first boundary is invalid"));
            }
            if boundary < previous || boundary as usize > record.len() {
                return Err(invalid_data("document section boundaries are invalid"));
            }
            previous = boundary;
        }
        if previous as usize != record.len() {
            return Err(invalid_data("document final boundary is invalid"));
        }
        for section in [SECTION_ID, SECTION_TITLE, SECTION_ORIGINAL] {
            std::str::from_utf8(self.document_section(doc_id, section)?)
                .map_err(|_| invalid_data("document scalar section is not UTF-8"))?;
        }
        if self.document_section(doc_id, SECTION_ID)?.is_empty() {
            return Err(invalid_data("document id is empty"));
        }
        for section in [
            SECTION_ALIASES,
            SECTION_SERIES_SEASON,
            SECTION_PINYIN_FULL,
            SECTION_PINYIN_INITIALS,
        ] {
            validate_string_list(self.document_section(doc_id, section)?)?;
        }
        Ok(())
    }

    fn validate_postings(&self, posting_blob_len: u32) -> io::Result<()> {
        let mut previous_key = None;
        let mut expected_offset = 0_u32;
        for entry_index in 0..self.entry_count {
            let (posting_key, meta) = self.directory_entry(entry_index)?;
            if posting_key.group as usize >= GROUP_COUNT || posting_key.kind > KIND_EXACT_SCALAR {
                return Err(invalid_data("posting field/rank group is invalid"));
            }
            if previous_key.is_some_and(|previous| posting_key <= previous) {
                return Err(invalid_data("posting directory is not strictly ordered"));
            }
            previous_key = Some(posting_key);
            if meta.data_offset != expected_offset {
                return Err(invalid_data("posting data ranges are not contiguous"));
            }
            expected_offset = expected_offset
                .checked_add(meta.byte_len)
                .ok_or_else(|| invalid_data("posting range overflow"))?;
            let mut decoder = self.posting_decoder(meta)?;
            let mut previous_doc = None;
            while let Some(doc_id) = decoder.next_doc()? {
                if doc_id >= self.document_count
                    || previous_doc.is_some_and(|previous| doc_id <= previous)
                {
                    return Err(invalid_data("posting document ids are invalid"));
                }
                previous_doc = Some(doc_id);
            }
            if decoder.remaining != 0 || decoder.position != decoder.bytes.len() {
                return Err(invalid_data("posting count/byte length mismatch"));
            }
        }
        if expected_offset != posting_blob_len {
            return Err(invalid_data("posting blob length mismatch"));
        }
        Ok(())
    }

    fn document_relative_offset(&self, doc_id: u32) -> io::Result<u32> {
        if doc_id > self.document_count {
            return Err(invalid_data("document offset id is out of bounds"));
        }
        read_u32_at(
            &self.blob,
            self.document_offsets_at + doc_id as usize * size_of::<u32>(),
        )
    }

    fn document_record(&self, doc_id: u32) -> io::Result<&[u8]> {
        if doc_id >= self.document_count {
            return Err(invalid_data("document id is out of bounds"));
        }
        let start = self.document_relative_offset(doc_id)? as usize;
        let end = self.document_relative_offset(doc_id + 1)? as usize;
        self.blob
            .get(self.document_pool_at + start..self.document_pool_at + end)
            .ok_or_else(|| invalid_data("document record range is invalid"))
    }

    fn document_section(&self, doc_id: u32, section: usize) -> io::Result<&[u8]> {
        if section >= DOC_SECTION_COUNT {
            return Err(invalid_data("document section is out of bounds"));
        }
        let record = self.document_record(doc_id)?;
        let start = read_u32_at(record, section * size_of::<u32>())? as usize;
        let end = read_u32_at(record, (section + 1) * size_of::<u32>())? as usize;
        record
            .get(start..end)
            .ok_or_else(|| invalid_data("document section range is invalid"))
    }

    fn document_id(&self, doc_id: u32) -> &str {
        std::str::from_utf8(
            self.document_section(doc_id, SECTION_ID)
                .expect("validated document id section"),
        )
        .expect("validated document id UTF-8")
    }

    fn directory_entry(&self, index: u32) -> io::Result<(PostingKey, PostingMeta)> {
        if index >= self.entry_count {
            return Err(invalid_data("posting directory index is out of bounds"));
        }
        let offset = self.directory_at + index as usize * DIRECTORY_ENTRY_LEN;
        let kind = *self
            .blob
            .get(offset)
            .ok_or_else(|| invalid_data("posting directory kind is missing"))?;
        let group = *self
            .blob
            .get(offset + 1)
            .ok_or_else(|| invalid_data("posting directory group is missing"))?;
        Ok((
            PostingKey {
                kind,
                group,
                key: read_u64_at(&self.blob, offset + 2)?,
            },
            PostingMeta {
                data_offset: read_u32_at(&self.blob, offset + 10)?,
                byte_len: read_u32_at(&self.blob, offset + 14)?,
                count: read_u32_at(&self.blob, offset + 18)?,
            },
        ))
    }

    fn posting(&self, posting_key: PostingKey) -> Option<PostingMeta> {
        let (mut low, mut high) = (0_u32, self.entry_count);
        while low < high {
            let middle = low + (high - low) / 2;
            let (middle_key, meta) = self
                .directory_entry(middle)
                .expect("validated posting directory");
            match middle_key.cmp(&posting_key) {
                std::cmp::Ordering::Less => low = middle + 1,
                std::cmp::Ordering::Greater => high = middle,
                std::cmp::Ordering::Equal => return Some(meta),
            }
        }
        None
    }

    fn posting_decoder(&self, meta: PostingMeta) -> io::Result<PostingDecoder<'_>> {
        let start = self
            .postings_at
            .checked_add(meta.data_offset as usize)
            .ok_or_else(|| invalid_data("posting start overflow"))?;
        let end = start
            .checked_add(meta.byte_len as usize)
            .ok_or_else(|| invalid_data("posting end overflow"))?;
        let bytes = self
            .blob
            .get(start..end)
            .ok_or_else(|| invalid_data("posting byte range is invalid"))?;
        Ok(PostingDecoder {
            bytes,
            position: 0,
            remaining: meta.count,
            previous: 0,
            first: true,
        })
    }

    fn group_candidates(
        &self,
        group: u8,
        query_keys: &[u64],
        scratch: &mut SearchScratch,
    ) -> io::Result<()> {
        scratch.candidates.clear();
        let mut postings = Vec::with_capacity(query_keys.len());
        for key in query_keys {
            let posting_key = PostingKey {
                kind: KIND_BYTE_GRAM,
                group,
                key: *key,
            };
            let Some(meta) = self.posting(posting_key) else {
                return Ok(());
            };
            postings.push((*key, meta));
        }
        postings.sort_unstable_by_key(|(key, meta)| (meta.count, *key));
        postings.truncate(MAX_GRAMS_PER_GROUP);
        let Some((_, first)) = postings.first().copied() else {
            return Ok(());
        };
        let mut decoder = self.posting_decoder(first)?;
        scratch.candidates.reserve(first.count as usize);
        while let Some(doc_id) = decoder.next_doc()? {
            scratch.candidates.push(doc_id);
        }
        for (_, meta) in postings.into_iter().skip(1) {
            scratch.intersection.clear();
            intersect_compressed(
                &scratch.candidates,
                self.posting_decoder(meta)?,
                &mut scratch.intersection,
            )?;
            std::mem::swap(&mut scratch.candidates, &mut scratch.intersection);
            if scratch.candidates.is_empty() {
                break;
            }
        }
        Ok(())
    }

    fn match_group(&self, doc_id: u32, group: u8, query: &str) -> Option<u8> {
        match group {
            GROUP_TITLE => {
                let value = self.scalar_section(doc_id, SECTION_TITLE);
                rank_scalar(value, query, 0, 3, 6)
            }
            GROUP_ORIGINAL => {
                let value = self.scalar_section(doc_id, SECTION_ORIGINAL);
                (!value.is_empty()).then(|| rank_scalar(value, query, 1, 4, 7))?
            }
            GROUP_ALIASES => {
                let values = self.string_list(doc_id, SECTION_ALIASES);
                if values.any(|value| value == query) {
                    Some(2)
                } else if values.any(|value| value.contains(query)) {
                    Some(7)
                } else {
                    None
                }
            }
            GROUP_SERIES_SEASON => {
                let values = self.string_list(doc_id, SECTION_SERIES_SEASON);
                if values.any(|value| value.starts_with(query)) {
                    Some(5)
                } else if values.any(|value| value.contains(query)) {
                    Some(8)
                } else {
                    None
                }
            }
            GROUP_PINYIN_FULL => {
                let values = self.string_list(doc_id, SECTION_PINYIN_FULL);
                if values.first().is_some_and(|value| value.starts_with(query)) {
                    Some(9)
                } else if values.any(|value| value.contains(query)) {
                    Some(11)
                } else {
                    None
                }
            }
            GROUP_PINYIN_INITIALS => {
                let values = self.string_list(doc_id, SECTION_PINYIN_INITIALS);
                if values.first().is_some_and(|value| value.starts_with(query)) {
                    Some(10)
                } else if values.any(|value| value.contains(query)) {
                    Some(12)
                } else {
                    None
                }
            }
            _ => None,
        }
    }

    fn apply_ranked_posting(
        &self,
        posting_key: PostingKey,
        rank: u8,
        scratch: &mut SearchScratch,
    ) -> io::Result<u32> {
        let Some(meta) = self.posting(posting_key) else {
            return Ok(0);
        };
        let mut decoder = self.posting_decoder(meta)?;
        while let Some(doc_id) = decoder.next_doc()? {
            scratch.mark_candidate(doc_id);
            scratch.mark_match(doc_id, rank);
        }
        Ok(meta.count)
    }

    fn search_short_scalar(
        &self,
        query: &str,
        scratch: &mut SearchScratch,
    ) -> io::Result<SearchResult> {
        let key = scalar_query_key(query)
            .ok_or_else(|| invalid_data("short scalar query key is unavailable"))?;
        let plans = [
            (KIND_EXACT_SCALAR, GROUP_TITLE, 0_u8),
            (KIND_EXACT_SCALAR, GROUP_ORIGINAL, 1),
            (KIND_EXACT_SCALAR, GROUP_ALIASES, 2),
            (KIND_PREFIX_SCALAR, GROUP_TITLE, 3),
            (KIND_PREFIX_SCALAR, GROUP_ORIGINAL, 4),
            (KIND_PREFIX_SCALAR, GROUP_SERIES_SEASON, 5),
            (KIND_SCALAR_GRAM, GROUP_TITLE, 6),
            (KIND_SCALAR_GRAM, GROUP_ORIGINAL, 7),
            (KIND_SCALAR_GRAM, GROUP_ALIASES, 7),
            (KIND_SCALAR_GRAM, GROUP_SERIES_SEASON, 8),
            (KIND_PREFIX_SCALAR, GROUP_PINYIN_FULL, 9),
            (KIND_PREFIX_SCALAR, GROUP_PINYIN_INITIALS, 10),
            (KIND_SCALAR_GRAM, GROUP_PINYIN_FULL, 11),
            (KIND_SCALAR_GRAM, GROUP_PINYIN_INITIALS, 12),
        ];
        let mut candidate_checks = 0_u64;
        for (kind, group, rank) in plans {
            let count =
                self.apply_ranked_posting(PostingKey { kind, group, key }, rank, scratch)?;
            scratch.group_candidates[group as usize] = scratch.group_candidates[group as usize]
                .checked_add(count)
                .ok_or_else(|| invalid_data("group candidate counter overflow"))?;
            candidate_checks += u64::from(count);
        }
        let candidate_unique = popcount(&scratch.candidate_bits);
        Ok(scratch.finish(candidate_unique, candidate_checks))
    }

    fn scalar_section(&self, doc_id: u32, section: usize) -> &str {
        std::str::from_utf8(
            self.document_section(doc_id, section)
                .expect("validated scalar section"),
        )
        .expect("validated scalar section UTF-8")
    }

    fn string_list(&self, doc_id: u32, section: usize) -> StringList<'_> {
        StringList {
            bytes: self
                .document_section(doc_id, section)
                .expect("validated string-list section"),
        }
    }

    fn search(&self, raw_query: &str, scratch: &mut SearchScratch) -> io::Result<SearchResult> {
        let query = normalize_search_text(raw_query);
        scratch.reset();
        if query.is_empty() {
            return Ok(scratch.finish(0, 0));
        }
        if query.chars().count() <= 3 {
            return self.search_short_scalar(&query, scratch);
        }
        let query_keys = query_gram_keys(&query);
        let mut candidate_checks = 0_u64;
        for group in 0..GROUP_COUNT as u8 {
            self.group_candidates(group, &query_keys, scratch)?;
            let group_count = u32::try_from(scratch.candidates.len())
                .map_err(|_| invalid_data("group candidate count exceeds u32"))?;
            scratch.group_candidates_mut()[group as usize] = group_count;
            candidate_checks += u64::from(group_count);
            for candidate_index in 0..scratch.candidates.len() {
                let doc_id = scratch.candidates[candidate_index];
                scratch.mark_candidate(doc_id);
                if let Some(rank) = self.match_group(doc_id, group, &query) {
                    scratch.mark_match(doc_id, rank);
                }
            }
        }
        let candidate_unique = popcount(&scratch.candidate_bits);
        Ok(scratch.finish(candidate_unique, candidate_checks))
    }

    fn scan(&self, raw_query: &str) -> SearchResult {
        let query = normalize_search_text(raw_query);
        if query.is_empty() {
            return SearchResult {
                top: Vec::new(),
                total_matches: 0,
                has_more: false,
                candidate_unique: 0,
                candidate_checks: 0,
                group_candidates: [0; GROUP_COUNT],
            };
        }
        let mut buckets: [Vec<u32>; RANK_COUNT] = array::from_fn(|_| Vec::new());
        let mut total_matches = 0_u64;
        for doc_id in 0..self.document_count {
            let rank = (0..GROUP_COUNT as u8)
                .filter_map(|group| self.match_group(doc_id, group, &query))
                .min();
            if let Some(rank) = rank {
                total_matches += 1;
                if buckets[rank as usize].len() < QUERY_LIMIT {
                    buckets[rank as usize].push(doc_id);
                }
            }
        }
        SearchResult {
            top: flatten_buckets(&buckets),
            total_matches,
            has_more: total_matches > QUERY_LIMIT as u64,
            candidate_unique: 0,
            candidate_checks: 0,
            group_candidates: [0; GROUP_COUNT],
        }
    }

    fn estimated_heap_bytes(&self) -> usize {
        self.blob.capacity() + size_of::<Self>()
    }
}

fn rank_scalar(value: &str, query: &str, exact: u8, prefix: u8, contains: u8) -> Option<u8> {
    if value == query {
        Some(exact)
    } else if value.starts_with(query) {
        Some(prefix)
    } else if value.contains(query) {
        Some(contains)
    } else {
        None
    }
}

impl SearchScratch {
    fn new(document_count: usize) -> Self {
        let words = document_count.div_ceil(u64::BITS as usize);
        Self {
            ranks: vec![NO_RANK; document_count],
            rank_generations: vec![0; document_count],
            generation: 0,
            match_bits: vec![0; words],
            candidate_bits: vec![0; words],
            touched_match_words: Vec::new(),
            touched_candidate_words: Vec::new(),
            candidates: Vec::new(),
            intersection: Vec::new(),
            buckets: array::from_fn(|_| Vec::new()),
            group_candidates: [0; GROUP_COUNT],
        }
    }

    fn reset(&mut self) {
        for word in self.touched_match_words.drain(..) {
            self.match_bits[word as usize] = 0;
        }
        for word in self.touched_candidate_words.drain(..) {
            self.candidate_bits[word as usize] = 0;
        }
        if self.generation == u32::MAX {
            self.rank_generations.fill(0);
            self.generation = 1;
        } else {
            self.generation += 1;
        }
        self.candidates.clear();
        self.intersection.clear();
        self.buckets.iter_mut().for_each(Vec::clear);
        self.group_candidates.fill(0);
    }

    fn mark_candidate(&mut self, doc_id: u32) {
        set_bit_tracking(
            &mut self.candidate_bits,
            &mut self.touched_candidate_words,
            doc_id,
        );
    }

    fn mark_match(&mut self, doc_id: u32, rank: u8) {
        let index = doc_id as usize;
        if self.rank_generations[index] == self.generation {
            self.ranks[index] = self.ranks[index].min(rank);
        } else {
            self.rank_generations[index] = self.generation;
            self.ranks[index] = rank;
        }
        set_bit_tracking(&mut self.match_bits, &mut self.touched_match_words, doc_id);
    }

    fn group_candidates_mut(&mut self) -> &mut [u32; GROUP_COUNT] {
        &mut self.group_candidates
    }

    fn finish(&mut self, candidate_unique: u64, candidate_checks: u64) -> SearchResult {
        for (word_index, word) in self.match_bits.iter().copied().enumerate() {
            let mut remaining = word;
            while remaining != 0 {
                let bit = remaining.trailing_zeros() as usize;
                let doc_index = word_index * u64::BITS as usize + bit;
                if doc_index < self.ranks.len() {
                    let rank = self.ranks[doc_index];
                    if self.rank_generations[doc_index] == self.generation
                        && rank != NO_RANK
                        && self.buckets[rank as usize].len() < QUERY_LIMIT
                    {
                        self.buckets[rank as usize].push(doc_index as u32);
                    }
                }
                remaining &= remaining - 1;
            }
        }
        let total_matches = popcount(&self.match_bits);
        SearchResult {
            top: flatten_buckets(&self.buckets),
            total_matches,
            has_more: total_matches > QUERY_LIMIT as u64,
            candidate_unique,
            candidate_checks,
            group_candidates: self.group_candidates,
        }
    }

    fn estimated_heap_bytes(&self) -> usize {
        self.ranks.capacity()
            + self.rank_generations.capacity() * size_of::<u32>()
            + (self.match_bits.capacity() + self.candidate_bits.capacity()) * size_of::<u64>()
            + (self.touched_match_words.capacity() + self.touched_candidate_words.capacity())
                * size_of::<u32>()
            + (self.candidates.capacity() + self.intersection.capacity()) * size_of::<u32>()
            + self
                .buckets
                .iter()
                .map(|bucket| bucket.capacity() * size_of::<u32>())
                .sum::<usize>()
    }
}

fn flatten_buckets(buckets: &[Vec<u32>; RANK_COUNT]) -> Vec<(u32, u8)> {
    let mut top = Vec::with_capacity(QUERY_LIMIT);
    for (rank, bucket) in buckets.iter().enumerate() {
        for doc_id in bucket {
            if top.len() == QUERY_LIMIT {
                return top;
            }
            top.push((*doc_id, rank as u8));
        }
    }
    top
}

struct StringList<'a> {
    bytes: &'a [u8],
}

impl StringList<'_> {
    fn first(&self) -> Option<&str> {
        let mut cursor = 0;
        let count = read_var_u32_slice(self.bytes, &mut cursor)?;
        (count > 0)
            .then(|| read_string_slice(self.bytes, &mut cursor))
            .flatten()
    }

    fn any(&self, mut predicate: impl FnMut(&str) -> bool) -> bool {
        let mut cursor = 0;
        let Some(count) = read_var_u32_slice(self.bytes, &mut cursor) else {
            return false;
        };
        (0..count).any(|_| read_string_slice(self.bytes, &mut cursor).is_some_and(&mut predicate))
    }
}

struct PostingDecoder<'a> {
    bytes: &'a [u8],
    position: usize,
    remaining: u32,
    previous: u32,
    first: bool,
}

impl PostingDecoder<'_> {
    fn next_doc(&mut self) -> io::Result<Option<u32>> {
        if self.remaining == 0 {
            return Ok(None);
        }
        let delta = read_var_u32_slice(self.bytes, &mut self.position)
            .ok_or_else(|| invalid_data("invalid posting varint"))?;
        let value = if self.first {
            self.first = false;
            delta
        } else {
            self.previous
                .checked_add(delta)
                .ok_or_else(|| invalid_data("posting delta overflow"))?
        };
        self.previous = value;
        self.remaining -= 1;
        Ok(Some(value))
    }
}

fn intersect_compressed(
    left: &[u32],
    mut right: PostingDecoder<'_>,
    output: &mut Vec<u32>,
) -> io::Result<()> {
    output.reserve(left.len().min(right.remaining as usize));
    let mut right_value = right.next_doc()?;
    for left_value in left {
        while right_value.is_some_and(|value| value < *left_value) {
            right_value = right.next_doc()?;
        }
        if right_value == Some(*left_value) {
            output.push(*left_value);
            right_value = right.next_doc()?;
        }
        if right_value.is_none() {
            break;
        }
    }
    Ok(())
}

fn measure_queries(
    index: &RuntimeIndex,
    queries: &[FixtureQuery],
    scratch: &mut SearchScratch,
) -> Measurements {
    let mut measurements = Measurements {
        categories: BTreeMap::new(),
        substrings: BTreeMap::new(),
    };
    for query in queries {
        let started = Instant::now();
        let result = index
            .search(&query.text, scratch)
            .expect("validated sidecar query must succeed");
        let sample = QuerySample {
            elapsed_ms: elapsed_ms(started),
            candidate_unique: result.candidate_unique,
            candidate_checks: result.candidate_checks,
            total_matches: result.total_matches,
            group_candidates: result.group_candidates,
        };
        black_box((result.total_matches, result.has_more, result.top.len()));
        measurements
            .categories
            .entry(query.category.clone())
            .or_default()
            .push(sample.clone());
        if query.category == "substring" {
            measurements
                .substrings
                .entry(query.text.clone())
                .or_default()
                .push(sample);
        }
    }
    measurements
}

fn print_category_samples(label: &str, samples: &BTreeMap<String, Vec<QuerySample>>) {
    for (category, values) in samples {
        let mut elapsed = values
            .iter()
            .map(|sample| sample.elapsed_ms)
            .collect::<Vec<_>>();
        elapsed.sort_by(f64::total_cmp);
        println!(
            "query phase={label} category={category} n={} p50_ms={:.3} p95_ms={:.3} p99_ms={:.3} max_ms={:.3}",
            elapsed.len(),
            percentile(&elapsed, 0.50),
            percentile(&elapsed, 0.95),
            percentile(&elapsed, 0.99),
            elapsed.last().copied().unwrap_or_default()
        );
    }
}

fn print_substring_samples(label: &str, samples: &BTreeMap<String, Vec<QuerySample>>) {
    for (query, values) in samples {
        let mut elapsed = values
            .iter()
            .map(|sample| sample.elapsed_ms)
            .collect::<Vec<_>>();
        elapsed.sort_by(f64::total_cmp);
        let representative = &values[0];
        let groups = GROUP_NAMES
            .iter()
            .zip(representative.group_candidates)
            .map(|(name, count)| format!("{name}:{count}"))
            .collect::<Vec<_>>()
            .join(",");
        println!(
            "substring phase={label} text={query:?} n={} candidate_unique={} candidate_checks={} exact_matches={} p50_ms={:.3} p95_ms={:.3} p99_ms={:.3} groups={groups}",
            elapsed.len(),
            representative.candidate_unique,
            representative.candidate_checks,
            representative.total_matches,
            percentile(&elapsed, 0.50),
            percentile(&elapsed, 0.95),
            percentile(&elapsed, 0.99)
        );
    }
}

fn audit_correctness(
    index: &RuntimeIndex,
    queries: &[FixtureQuery],
    scratch: &mut SearchScratch,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut checked = BTreeMap::<String, usize>::new();
    let mut failures = Vec::new();
    for query in queries {
        let count = checked.entry(query.category.clone()).or_default();
        if *count >= CORRECTNESS_PER_CATEGORY {
            continue;
        }
        *count += 1;
        let result = index.search(&query.text, scratch)?;
        let oracle = index.scan(&query.text);
        if !same_contract(&result, &oracle) {
            failures.push(format!(
                "{} {:?}: postings/oracle mismatch postings_count={} oracle_count={} postings_top={:?} oracle_top={:?}",
                query.category,
                query.text,
                result.total_matches,
                oracle.total_matches,
                top_ids(index, &result),
                top_ids(index, &oracle)
            ));
            continue;
        }
        if result.has_more != (result.total_matches > QUERY_LIMIT as u64) {
            failures.push(format!(
                "{} {:?}: has_more inconsistent with exact count",
                query.category, query.text
            ));
        }
        let mut unique = HashSet::new();
        if !result.top.iter().all(|(doc_id, _)| unique.insert(*doc_id)) {
            failures.push(format!(
                "{} {:?}: duplicate Top50 rows",
                query.category, query.text
            ));
        }
        if let Some(expected) = &query.rank1 {
            let actual = result
                .top
                .first()
                .map(|(doc_id, _)| index.document_id(*doc_id));
            if actual != Some(expected.as_str()) {
                failures.push(format!(
                    "{} {:?}: rank1 expected={} actual={actual:?}",
                    query.category, query.text, expected
                ));
            }
        }
        if let Some(expected) = query.match_count {
            if result.total_matches != expected {
                failures.push(format!(
                    "{} {:?}: matchCount expected={} actual={}",
                    query.category, query.text, expected, result.total_matches
                ));
            }
        }
    }
    if checked.len() != 17 {
        failures.push(format!(
            "expected 17 correctness categories, observed {}",
            checked.len()
        ));
    }
    for (category, count) in &checked {
        println!("correctness category={category} checked={count}");
    }
    if !failures.is_empty() {
        for failure in failures.iter().take(30) {
            eprintln!("correctness_failure: {failure}");
        }
        return Err(format!("{} correctness checks failed", failures.len()).into());
    }
    println!(
        "correctness categories={} checked={} failures=0 oracle=full_scan exact_count=true top50_and_rank=true",
        checked.len(),
        checked.values().sum::<usize>()
    );
    Ok(())
}

fn same_contract(left: &SearchResult, right: &SearchResult) -> bool {
    left.top == right.top
        && left.total_matches == right.total_matches
        && left.has_more == right.has_more
}

fn top_ids<'a>(index: &'a RuntimeIndex, result: &SearchResult) -> Vec<(&'a str, u8)> {
    result
        .top
        .iter()
        .take(5)
        .map(|(doc_id, rank)| (index.document_id(*doc_id), *rank))
        .collect()
}

fn set_bit_tracking(bits: &mut [u64], touched_words: &mut Vec<u32>, doc_id: u32) {
    let index = doc_id as usize;
    let word_index = index / u64::BITS as usize;
    if bits[word_index] == 0 {
        touched_words.push(word_index as u32);
    }
    bits[word_index] |= 1_u64 << (index % u64::BITS as usize);
}

fn popcount(bits: &[u64]) -> u64 {
    bits.iter().map(|word| u64::from(word.count_ones())).sum()
}

fn query_gram_keys(query: &str) -> Vec<u64> {
    let bytes = query.as_bytes();
    if bytes.is_empty() {
        return Vec::new();
    }
    let width = bytes.len().min(3);
    let mut keys = bytes
        .windows(width)
        .map(|window| u64::from(byte_gram_key(window)))
        .collect::<Vec<_>>();
    keys.sort_unstable();
    keys.dedup();
    keys
}

fn add_all_byte_gram_keys(group: u8, bytes: &[u8], output: &mut Vec<PostingKey>) {
    for width in 1..=bytes.len().min(3) {
        output.extend(bytes.windows(width).map(|window| PostingKey {
            kind: KIND_BYTE_GRAM,
            group,
            key: u64::from(byte_gram_key(window)),
        }));
    }
}

fn byte_gram_key(bytes: &[u8]) -> u32 {
    debug_assert!((1..=3).contains(&bytes.len()));
    let payload = bytes
        .iter()
        .fold(0_u32, |value, byte| (value << 8) | u32::from(*byte));
    ((bytes.len() as u32) << 24) | payload
}

fn add_all_scalar_gram_keys(group: u8, value: &str, output: &mut Vec<PostingKey>) {
    let scalars = value.chars().collect::<Vec<_>>();
    for width in 1..=scalars.len().min(3) {
        output.extend(scalars.windows(width).map(|window| PostingKey {
            kind: KIND_SCALAR_GRAM,
            group,
            key: scalar_key(window),
        }));
    }
}

fn add_prefix_scalar_keys(group: u8, value: &str, output: &mut Vec<PostingKey>) {
    let scalars = value.chars().take(3).collect::<Vec<_>>();
    for width in 1..=scalars.len() {
        output.push(PostingKey {
            kind: KIND_PREFIX_SCALAR,
            group,
            key: scalar_key(&scalars[..width]),
        });
    }
}

fn add_exact_scalar_key(group: u8, value: &str, output: &mut Vec<PostingKey>) {
    let scalars = value.chars().collect::<Vec<_>>();
    if (1..=3).contains(&scalars.len()) {
        output.push(PostingKey {
            kind: KIND_EXACT_SCALAR,
            group,
            key: scalar_key(&scalars),
        });
    }
}

fn scalar_query_key(value: &str) -> Option<u64> {
    let scalars = value.chars().collect::<Vec<_>>();
    (1..=3)
        .contains(&scalars.len())
        .then(|| scalar_key(&scalars))
}

fn scalar_key(scalars: &[char]) -> u64 {
    debug_assert!((1..=3).contains(&scalars.len()));
    debug_assert!(scalars.iter().all(|character| *character != '\0'));
    scalars.iter().fold(0_u64, |value, character| {
        (value << 21) | u64::from(u32::from(*character))
    })
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
            continue;
        }
        if character.is_control() {
            continue;
        }
        if pending_space {
            result.push(' ');
            pending_space = false;
        }
        result.push(character);
    }
    result
}

fn encoded_string_list_len(values: &[String]) -> io::Result<usize> {
    encoded_string_refs_len(values.iter().map(String::as_str))
}

fn encoded_string_refs_len<'a>(values: impl Iterator<Item = &'a str>) -> io::Result<usize> {
    let values = values.collect::<Vec<_>>();
    let mut total = var_u32_len(
        u32::try_from(values.len()).map_err(|_| invalid_data("string list count exceeds u32"))?,
    );
    for value in values {
        let len =
            u32::try_from(value.len()).map_err(|_| invalid_data("string length exceeds u32"))?;
        total = total
            .checked_add(var_u32_len(len) + value.len())
            .ok_or_else(|| invalid_data("encoded string list length overflow"))?;
    }
    Ok(total)
}

fn write_string_list(writer: &mut impl Write, values: &[String]) -> io::Result<()> {
    write_string_refs(writer, values.iter().map(String::as_str))
}

fn write_string_refs<'a>(
    writer: &mut impl Write,
    values: impl Iterator<Item = &'a str>,
) -> io::Result<()> {
    let values = values.collect::<Vec<_>>();
    write_var_u32(
        writer,
        u32::try_from(values.len()).map_err(|_| invalid_data("string list count exceeds u32"))?,
    )?;
    for value in values {
        write_var_u32(
            writer,
            u32::try_from(value.len()).map_err(|_| invalid_data("string length exceeds u32"))?,
        )?;
        writer.write_all(value.as_bytes())?;
    }
    Ok(())
}

fn validate_string_list(bytes: &[u8]) -> io::Result<()> {
    let mut cursor = 0;
    let count = read_var_u32_slice(bytes, &mut cursor)
        .ok_or_else(|| invalid_data("string list count is invalid"))?;
    for _ in 0..count {
        read_string_slice(bytes, &mut cursor)
            .ok_or_else(|| invalid_data("string list entry is invalid"))?;
    }
    if cursor != bytes.len() {
        return Err(invalid_data("string list contains trailing bytes"));
    }
    Ok(())
}

fn read_string_slice<'a>(bytes: &'a [u8], cursor: &mut usize) -> Option<&'a str> {
    let len = read_var_u32_slice(bytes, cursor)? as usize;
    let end = cursor.checked_add(len)?;
    let value = std::str::from_utf8(bytes.get(*cursor..end)?).ok()?;
    *cursor = end;
    Some(value)
}

fn posting_encoded_len(documents: &[u32]) -> io::Result<u32> {
    let mut total = 0_usize;
    let mut previous = 0_u32;
    for (index, doc_id) in documents.iter().copied().enumerate() {
        let delta = if index == 0 {
            doc_id
        } else {
            doc_id
                .checked_sub(previous)
                .ok_or_else(|| invalid_data("posting list is not sorted"))?
        };
        total = total
            .checked_add(var_u32_len(delta))
            .ok_or_else(|| invalid_data("posting encoded length overflow"))?;
        previous = doc_id;
    }
    u32::try_from(total).map_err(|_| invalid_data("posting byte length exceeds u32"))
}

fn write_posting_list(writer: &mut impl Write, documents: &[u32]) -> io::Result<()> {
    let mut previous = 0_u32;
    for (index, doc_id) in documents.iter().copied().enumerate() {
        let delta = if index == 0 {
            doc_id
        } else {
            doc_id
                .checked_sub(previous)
                .ok_or_else(|| invalid_data("posting list is not sorted"))?
        };
        write_var_u32(writer, delta)?;
        previous = doc_id;
    }
    Ok(())
}

fn var_u32_len(mut value: u32) -> usize {
    let mut len = 1;
    while value >= 0x80 {
        value >>= 7;
        len += 1;
    }
    len
}

fn write_var_u32(writer: &mut impl Write, mut value: u32) -> io::Result<()> {
    while value >= 0x80 {
        writer.write_all(&[((value as u8) & 0x7f) | 0x80])?;
        value >>= 7;
    }
    writer.write_all(&[value as u8])
}

fn read_var_u32_slice(bytes: &[u8], cursor: &mut usize) -> Option<u32> {
    let mut result = 0_u32;
    for shift in (0..35).step_by(7) {
        let byte = *bytes.get(*cursor)?;
        *cursor += 1;
        let payload = u32::from(byte & 0x7f);
        if shift == 28 && payload > 0x0f {
            return None;
        }
        result |= payload << shift;
        if byte & 0x80 == 0 {
            return Some(result);
        }
    }
    None
}

fn write_u32(writer: &mut impl Write, value: u32) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}

fn write_u64(writer: &mut impl Write, value: u64) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}

fn read_u32_at(bytes: &[u8], offset: usize) -> io::Result<u32> {
    let value = bytes
        .get(offset..offset + size_of::<u32>())
        .ok_or_else(|| invalid_data("u32 read is out of bounds"))?;
    Ok(u32::from_le_bytes(
        value
            .try_into()
            .map_err(|_| invalid_data("u32 byte width mismatch"))?,
    ))
}

fn read_u64_at(bytes: &[u8], offset: usize) -> io::Result<u64> {
    let value = bytes
        .get(offset..offset + size_of::<u64>())
        .ok_or_else(|| invalid_data("u64 read is out of bounds"))?;
    Ok(u64::from_le_bytes(
        value
            .try_into()
            .map_err(|_| invalid_data("u64 byte width mismatch"))?,
    ))
}

struct CountingWriter<W> {
    inner: W,
    bytes: u64,
}

impl<W> CountingWriter<W> {
    fn new(inner: W) -> Self {
        Self { inner, bytes: 0 }
    }
}

impl<W: Write> Write for CountingWriter<W> {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        let written = self.inner.write(buffer)?;
        self.bytes += written as u64;
        Ok(written)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

fn vector_string_heap(values: &[String]) -> usize {
    size_of_val(values) + values.iter().map(String::capacity).sum::<usize>()
}

fn percentile(sorted: &[f64], percentile: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let index = ((sorted.len() - 1) as f64 * percentile).ceil() as usize;
    sorted[index]
}

fn required_string<'a>(value: &'a Value, key: &str) -> Result<&'a str, String> {
    value[key]
        .as_str()
        .ok_or_else(|| format!("fixture field {key} is missing"))
}

fn invalid_data(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn elapsed_ms(started: Instant) -> f64 {
    started.elapsed().as_secs_f64() * 1_000.0
}

fn mib(bytes: u64) -> f64 {
    bytes as f64 / 1_048_576.0
}

fn print_rss(label: &str, value: Option<u64>) {
    match value {
        Some(bytes) => println!("{label}_bytes={bytes} {label}_mib={:.3}", mib(bytes)),
        None => println!("{label}=unavailable"),
    }
}

fn print_rss_delta(label: &str, value: Option<u64>, baseline: Option<u64>) {
    match (value, baseline) {
        (Some(bytes), Some(baseline)) => {
            let delta_mib = if bytes >= baseline {
                mib(bytes - baseline)
            } else {
                -mib(baseline - bytes)
            };
            println!(
                "{label}_bytes={bytes} {label}_mib={:.3} delta_from_baseline_mib={delta_mib:.3}",
                mib(bytes)
            );
        }
        (Some(bytes), None) => print_rss(label, Some(bytes)),
        _ => println!("{label}=unavailable"),
    }
}

#[cfg(windows)]
fn resident_bytes() -> Option<u64> {
    let command = format!(
        "[System.Diagnostics.Process]::GetProcessById({}).WorkingSet64",
        std::process::id()
    );
    let output = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", &command])
        .output()
        .ok()?;
    output
        .status
        .success()
        .then_some(())
        .and_then(|()| String::from_utf8(output.stdout).ok())?
        .trim()
        .parse()
        .ok()
}

#[cfg(target_os = "linux")]
fn resident_bytes() -> Option<u64> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    let line = status.lines().find(|line| line.starts_with("VmRSS:"))?;
    let kib = line.split_whitespace().nth(1)?.parse::<u64>().ok()?;
    Some(kib * 1024)
}

#[cfg(not(any(windows, target_os = "linux")))]
fn resident_bytes() -> Option<u64> {
    None
}

fn now_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock must be after Unix epoch")
        .as_millis()
}
