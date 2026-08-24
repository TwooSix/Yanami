use std::{
    collections::{BTreeMap, HashMap, HashSet, VecDeque},
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Condvar, Mutex, RwLock, mpsc},
    thread,
    time::{Duration, Instant},
};

use pinyin::ToPinyin;
use rusqlite::{
    Connection, OpenFlags, OptionalExtension, Row, Transaction, params, params_from_iter,
};
use thiserror::Error;
use unicode_casefold::UnicodeCaseFold;
use unicode_normalization::UnicodeNormalization;

#[path = "posting_index.rs"]
mod posting_index;

use posting_index::{
    CATEGORY_EPISODE, CATEGORY_OTHER, CATEGORY_TITLE, DeltaIndex, IndexDocument, IndexHit,
    PersistentIndex, SidecarBuilder, cleanup_sidecars, generation_path,
    scope_fingerprint as index_scope_fingerprint,
};

const MEDIA_CATALOG_SCHEMA_VERSION: i64 = 5;
const MEDIA_CATALOG_SEARCH_VERSION: i64 = 4;
const DEFAULT_SEARCH_LIMIT: usize = 50;
const MAX_SEARCH_LIMIT: usize = 500;

#[derive(Debug, Error)]
pub enum MediaCatalogError {
    #[error("media catalog database failure: {0}")]
    Database(#[from] rusqlite::Error),
    #[error("media catalog JSON failure: {0}")]
    Json(#[from] serde_json::Error),
    #[error("media catalog filesystem failure: {0}")]
    Io(#[from] std::io::Error),
    #[error("media catalog lock is poisoned")]
    Poisoned,
    #[error("media catalog scope fields must not be empty")]
    InvalidScope,
    #[error("media catalog belongs to a different server or user")]
    ScopeMismatch,
    #[error(
        "media catalog schema version {found} is incompatible with supported version {supported}"
    )]
    SchemaVersion { found: i64, supported: i64 },
    #[error(
        "media catalog search version {found} is incompatible with supported version {supported}"
    )]
    SearchVersion { found: i64, supported: i64 },
    #[error("media catalog contains an unrecognized schema")]
    UnrecognizedSchema,
    #[error("catalog item id, type, and title must not be empty")]
    InvalidItem,
    #[error("catalog numeric value is outside SQLite's supported range")]
    NumericRange,
    #[error("catalog sync run {0} is already active")]
    SyncAlreadyRunning(i64),
    #[error("catalog sync run {actual} is stale; active run is {active:?}")]
    StaleSyncRun { active: Option<i64>, actual: i64 },
    #[error(
        "catalog reconciliation failed: expected {expected:?} distinct items but observed {seen}"
    )]
    ReconciliationMismatch { expected: Option<u64>, seen: u64 },
    #[error(
        "catalog membership verification failed: {remote_only} remote-only and {local_only} local-only items"
    )]
    MembershipMismatch {
        remote_only: usize,
        local_only: usize,
    },
    #[error("catalog membership verification is missing or stale for sync run {0}")]
    MembershipNotVerified(i64),
    #[error("catalog membership probe expected {expected} distinct items but observed {observed}")]
    MembershipCountMismatch { expected: u64, observed: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CatalogScope {
    pub server_local_id: String,
    pub server_id: String,
    pub user_id: String,
}

impl CatalogScope {
    pub fn new(
        server_local_id: impl Into<String>,
        server_id: impl Into<String>,
        user_id: impl Into<String>,
    ) -> Self {
        Self {
            server_local_id: server_local_id.into(),
            server_id: server_id.into(),
            user_id: user_id.into(),
        }
    }

    fn validate(&self) -> Result<(), MediaCatalogError> {
        if self.server_local_id.trim().is_empty()
            || self.server_id.trim().is_empty()
            || self.user_id.trim().is_empty()
        {
            return Err(MediaCatalogError::InvalidScope);
        }
        Ok(())
    }
}

fn catalog_scope_fingerprint(scope: &CatalogScope, database_path: &Path) -> u64 {
    let canonical_path = fs::canonicalize(database_path).unwrap_or_else(|_| database_path.into());
    let path = canonical_path.to_string_lossy();
    index_scope_fingerprint(&[
        &scope.server_local_id,
        &scope.server_id,
        &scope.user_id,
        &path,
    ])
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct CatalogItem {
    pub id: String,
    pub item_type: String,
    pub title: String,
    pub sort_title: String,
    pub original_title: Option<String>,
    pub parent_id: Option<String>,
    pub series_id: Option<String>,
    pub series_title: Option<String>,
    pub season_id: Option<String>,
    pub season_title: Option<String>,
    pub season_number: Option<i32>,
    pub episode_number: Option<i32>,
    pub production_year: Option<i32>,
    pub source_updated_at: Option<String>,
    pub image_tag: Option<String>,
    pub primary_image_aspect_ratio: Option<f64>,
    /// Search-only enrichment loaded from the cached parent Series row.
    pub series_image_tag: Option<String>,
    /// Search-only enrichment loaded from the cached parent Series row.
    pub series_primary_image_aspect_ratio: Option<f64>,
    pub aliases: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct CatalogUserState {
    pub item_id: String,
    pub favorite: bool,
    pub played: bool,
    pub resume_ticks: u64,
    pub progress: Option<f64>,
    pub unplayed_count: Option<u32>,
    pub last_played_at: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SyncState {
    Idle,
    Running,
    Complete,
    Failed,
}

impl SyncState {
    fn from_database(value: &str) -> Result<Self, rusqlite::Error> {
        match value {
            "idle" => Ok(Self::Idle),
            "running" => Ok(Self::Running),
            "complete" => Ok(Self::Complete),
            "failed" => Ok(Self::Failed),
            _ => Err(rusqlite::Error::InvalidQuery),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SyncStatus {
    pub state: SyncState,
    pub active_run_id: Option<i64>,
    pub last_completed_run_id: Option<i64>,
    pub next_start_index: u64,
    pub cached_count: u64,
    pub total_expected: Option<u64>,
    pub started_at_ms: Option<i64>,
    pub updated_at_ms: Option<i64>,
    pub last_completed_at_ms: Option<i64>,
    pub last_error: Option<String>,
    pub dirty: bool,
    /// Opaque monotonic token for any search-visible catalog mutation.
    pub content_revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogPendingChanges {
    pub upsert_ids: Vec<String>,
    pub removed_ids: Vec<String>,
    pub catchup_required: bool,
    pub membership_required: bool,
    pub watermark_ms: Option<i64>,
    pub last_membership_check_ms: Option<i64>,
    pub failure_count: u32,
    pub last_failure_at_ms: Option<i64>,
    pub catchup_revision: u64,
    pub membership_revision: u64,
    /// Revision fence for acknowledging a batch without deleting a newer
    /// notification for the same ID.
    pub pending_revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogMembershipDiff {
    pub remote_only: Vec<String>,
    pub local_only: Vec<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CatalogSyncRun {
    id: i64,
}

impl CatalogSyncRun {
    pub fn id(self) -> i64 {
        self.id
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct CatalogSearchHit {
    pub item: CatalogItem,
    pub user_state: Option<CatalogUserState>,
    pub match_rank: u32,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CatalogSearchPage {
    pub items: Vec<CatalogSearchHit>,
    /// Exact number of matches across the persistent base and live delta.
    pub total_matches: u64,
    pub has_more: bool,
    pub status: SyncStatus,
}

/// A disposable, session-scoped media projection backed by its own `SQLite` file.
///
/// The writer and reader are serialized independently, so WAL searches never
/// queue behind a background page transaction. Keeping the read-only connection
/// open also preserves `SQLite`'s page and prepared-statement caches across
/// keystrokes.
pub struct MediaCatalog {
    path: PathBuf,
    scope: CatalogScope,
    reader: Mutex<Connection>,
    search_index: RwLock<Arc<SearchSnapshot>>,
    // Declared after `reader` so Rust drops the reader first and lets the last
    // writer close checkpoint/remove an otherwise idle WAL sidecar.
    writer: Mutex<Connection>,
}

#[derive(Clone)]
struct SearchSnapshot {
    base: Option<Arc<PersistentIndex>>,
    delta: DeltaIndex,
    overridden_base: Vec<u64>,
}

impl SearchSnapshot {
    fn new(base: Option<Arc<PersistentIndex>>) -> Self {
        let word_count = base.as_ref().map_or(0, |index| {
            index.document_count().div_ceil(u64::BITS as usize)
        });
        Self {
            base,
            delta: DeltaIndex::new(),
            overridden_base: vec![0; word_count],
        }
    }

    fn apply_delta(&mut self, document: IndexDocument) -> Result<(), MediaCatalogError> {
        if let Some(ordinal) = self
            .base
            .as_ref()
            .and_then(|index| index.find_ordinal(&document.item_id))
        {
            let word = ordinal as usize / u64::BITS as usize;
            let bit = ordinal % u64::BITS;
            self.overridden_base[word] |= 1_u64 << bit;
        }
        self.delta.upsert(document)?;
        Ok(())
    }

    fn remove_item(&mut self, item_id: &str) {
        if let Some(ordinal) = self
            .base
            .as_ref()
            .and_then(|index| index.find_ordinal(item_id))
        {
            let word = ordinal as usize / u64::BITS as usize;
            let bit = ordinal % u64::BITS;
            self.overridden_base[word] |= 1_u64 << bit;
        }
        self.delta.remove(item_id);
    }

    fn query_category(
        &self,
        query: &str,
        limit: usize,
        category: u8,
    ) -> Result<(Vec<IndexHit>, u64), MediaCatalogError> {
        let mut hits = Vec::with_capacity(limit.saturating_mul(2));
        let mut total_matches = 0_u64;
        if let Some(base) = &self.base {
            let result = base.query(query, limit, category, &self.overridden_base)?;
            total_matches = total_matches
                .checked_add(result.total_matches)
                .ok_or(MediaCatalogError::NumericRange)?;
            hits.extend(result.hits);
        }
        let delta = self.delta.query(query, limit, category)?;
        total_matches = total_matches
            .checked_add(delta.total_matches)
            .ok_or(MediaCatalogError::NumericRange)?;
        hits.extend(delta.hits);
        Ok((hits, total_matches))
    }

    fn query(&self, query: &str, limit: usize) -> Result<(Vec<IndexHit>, u64), MediaCatalogError> {
        let (mut titles, title_matches) = self.query_category(query, limit, CATEGORY_TITLE)?;
        let (episodes, episode_matches) = self.query_category(query, limit, CATEGORY_EPISODE)?;
        titles.extend(episodes);
        let total_matches = title_matches
            .checked_add(episode_matches)
            .ok_or(MediaCatalogError::NumericRange)?;
        Ok((titles, total_matches))
    }

    fn publish(&mut self, base: Arc<PersistentIndex>) {
        *self = Self::new(Some(base));
    }
}

impl MediaCatalog {
    pub fn open(path: impl AsRef<Path>, scope: CatalogScope) -> Result<Self, MediaCatalogError> {
        scope.validate()?;
        let path = path.as_ref().to_owned();
        if let Some(parent) = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
        {
            fs::create_dir_all(parent)?;
        }
        let mut writer = Connection::open(&path)?;
        configure_writer(&writer)?;
        initialize_or_validate_schema(&mut writer, &scope)?;
        let scope_fingerprint = catalog_scope_fingerprint(&scope, &path);
        let search_generation = writer.query_row(
            "SELECT search_generation FROM catalog_scope WHERE singleton=1",
            [],
            |row| row.get::<_, Option<i64>>(0),
        )?;
        let base = search_generation
            .map(|generation| open_or_rebuild_index(&path, generation, scope_fingerprint))
            .transpose()?
            .map(Arc::new);
        let mut search_snapshot = SearchSnapshot::new(base);
        for document in load_delta_documents(&path, search_generation)? {
            search_snapshot.apply_delta(document)?;
        }
        for item_id in load_search_tombstones(&writer)? {
            search_snapshot.remove_item(&item_id);
        }
        cleanup_sidecars(&path, search_generation, false)?;
        let reader = open_reader(&path)?;
        Ok(Self {
            path,
            scope,
            reader: Mutex::new(reader),
            search_index: RwLock::new(Arc::new(search_snapshot)),
            writer: Mutex::new(writer),
        })
    }

    /// Removes only the disposable database and search sidecars belonging to
    /// this exact catalog path. Other catalog scopes in the directory are not
    /// inspected or removed.
    pub fn remove_disposable_files(path: impl AsRef<Path>) -> Result<(), std::io::Error> {
        let path = path.as_ref();
        cleanup_sidecars(path, None, true)?;
        for target in [
            path.to_owned(),
            exact_database_suffix(path, "-wal"),
            exact_database_suffix(path, "-shm"),
        ] {
            match fs::remove_file(target) {
                Ok(()) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error),
            }
        }
        Ok(())
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn scope(&self) -> &CatalogScope {
        &self.scope
    }

    pub fn begin_sync(
        &self,
        total_expected: Option<u64>,
        started_at_ms: i64,
    ) -> Result<CatalogSyncRun, MediaCatalogError> {
        let total_expected = optional_u64_to_i64(total_expected)?;
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        let active = transaction.query_row(
            "SELECT active_run_id FROM catalog_scope WHERE singleton=1",
            [],
            |row| row.get::<_, Option<i64>>(0),
        )?;
        if let Some(active) = active {
            return Err(MediaCatalogError::SyncAlreadyRunning(active));
        }
        let run_id = transaction.query_row(
            "UPDATE catalog_scope SET next_run_id=next_run_id+1 WHERE singleton=1 RETURNING next_run_id",
            [],
            |row| row.get::<_, i64>(0),
        )?;
        transaction.execute(
            "UPDATE catalog_scope SET active_run_id=?1, sync_state='running', next_start_index=0, \
             total_expected=?2, started_at_ms=?3, updated_at_ms=?3, last_error=NULL,\
             sync_revision=0,sync_dirty=0,membership_verified_revision=NULL WHERE singleton=1",
            params![run_id, total_expected, started_at_ms],
        )?;
        transaction.commit()?;
        Ok(CatalogSyncRun { id: run_id })
    }

    /// Marks a persisted or aborted in-process run as failed without removing
    /// any pages it already committed. Call this after opening a catalog and
    /// before starting a replacement task, or when a task is cancelled before
    /// it can call [`Self::fail_sync`].
    pub fn recover_interrupted_sync(
        &self,
        error: &str,
        recovered_at_ms: i64,
    ) -> Result<Option<CatalogSyncRun>, MediaCatalogError> {
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        let active = transaction
            .query_row(
                "SELECT active_run_id FROM catalog_scope WHERE singleton=1 \
                 AND sync_state='running' AND active_run_id IS NOT NULL",
                [],
                |row| row.get::<_, i64>(0),
            )
            .optional()?;
        let Some(run_id) = active else {
            transaction.commit()?;
            return Ok(None);
        };
        transaction.execute(
            "UPDATE catalog_scope SET active_run_id=NULL,sync_state='failed',updated_at_ms=?1,\
             last_error=?2,sync_dirty=1,membership_verified_revision=NULL \
             WHERE singleton=1 AND active_run_id=?3",
            params![recovered_at_ms, error.trim(), run_id],
        )?;
        transaction.commit()?;
        Ok(Some(CatalogSyncRun { id: run_id }))
    }

    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn upsert_page(
        &self,
        run: CatalogSyncRun,
        items: &[CatalogItem],
        user_states: &[CatalogUserState],
        next_start_index: u64,
        total_expected: Option<u64>,
        updated_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let next_start_index = u64_to_i64(next_start_index)?;
        let total_expected = optional_u64_to_i64(total_expected)?;
        let prepared_items = items
            .iter()
            .map(|item| PreparedCatalogItem::new(item).map(|prepared| (item, prepared)))
            .collect::<Result<Vec<_>, _>>()?;
        let state_item_ids = user_states
            .iter()
            .map(|state| {
                validate_user_state(state)?;
                Ok(state.item_id.as_str())
            })
            .collect::<Result<HashSet<_>, MediaCatalogError>>()?;
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        ensure_active_run(&transaction, run)?;

        let mut changed_documents = Vec::new();
        let existing_fingerprints = load_page_fingerprints(&transaction, items)?;
        let mut unchanged_item_ids = Vec::new();
        {
            let mut statement = transaction.prepare_cached(
                "INSERT INTO catalog_items(\
                    item_id,item_type,title,sort_title,original_title,parent_id,series_id,series_title,\
                    season_id,season_title,season_number,episode_number,production_year,source_updated_at,\
                    image_tag,primary_image_aspect_ratio,aliases_json,search_fingerprint,\
                    fact_fingerprint,search_changed_run_id,seen_run_id,cached_at_ms\
                 ) VALUES(\
                    ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22\
                 ) ON CONFLICT(item_id) DO UPDATE SET \
                    item_type=excluded.item_type,title=excluded.title,sort_title=excluded.sort_title,\
                    original_title=excluded.original_title,parent_id=excluded.parent_id,series_id=excluded.series_id,\
                    series_title=excluded.series_title,season_id=excluded.season_id,\
                    season_title=excluded.season_title,season_number=excluded.season_number,\
                    episode_number=excluded.episode_number,production_year=excluded.production_year,\
                    source_updated_at=excluded.source_updated_at,image_tag=excluded.image_tag,\
                    primary_image_aspect_ratio=excluded.primary_image_aspect_ratio,\
                    aliases_json=excluded.aliases_json,\
                    search_changed_run_id=CASE WHEN \
                        catalog_items.search_fingerprint<>excluded.search_fingerprint THEN \
                        excluded.search_changed_run_id ELSE catalog_items.search_changed_run_id END,\
                    search_fingerprint=excluded.search_fingerprint,\
                    fact_fingerprint=excluded.fact_fingerprint,\
                    seen_run_id=excluded.seen_run_id,cached_at_ms=excluded.cached_at_ms",
            )?;
            for (item, prepared) in &prepared_items {
                let existing = existing_fingerprints.get(&item.id);
                if existing.map(|value| value.0.as_slice())
                    != Some(prepared.search_fingerprint.as_slice())
                {
                    changed_documents.push(prepared.index_document(item));
                }
                if existing.map(|value| value.1.as_slice())
                    == Some(prepared.fact_fingerprint.as_slice())
                {
                    unchanged_item_ids.push(item.id.as_str());
                    continue;
                }
                statement.execute(params![
                    item.id,
                    item.item_type,
                    item.title,
                    prepared.sort_title,
                    item.original_title,
                    item.parent_id,
                    item.series_id,
                    item.series_title,
                    item.season_id,
                    item.season_title,
                    item.season_number,
                    item.episode_number,
                    item.production_year,
                    item.source_updated_at,
                    item.image_tag,
                    item.primary_image_aspect_ratio,
                    prepared.aliases_json,
                    prepared.search_fingerprint,
                    prepared.fact_fingerprint,
                    run.id,
                    run.id,
                    updated_at_ms,
                ])?;
            }
        }
        mark_items_seen(&transaction, &unchanged_item_ids, run.id, updated_at_ms)?;
        delete_search_tombstones(&transaction, items.iter().map(|item| item.id.as_str()))?;

        let missing_user_states = items
            .iter()
            .map(|item| item.id.as_str())
            .filter(|item_id| !state_item_ids.contains(item_id))
            .collect::<Vec<_>>();
        delete_user_states(&transaction, &missing_user_states)?;

        {
            let mut statement = transaction.prepare_cached(
                "INSERT INTO catalog_user_state(\
                    item_id,favorite,played,resume_ticks,progress,unplayed_count,last_played_at,updated_at_ms\
                 ) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)\
                 ON CONFLICT(item_id) DO UPDATE SET favorite=excluded.favorite,played=excluded.played,\
                    resume_ticks=excluded.resume_ticks,progress=excluded.progress,\
                    unplayed_count=excluded.unplayed_count,last_played_at=excluded.last_played_at,\
                    updated_at_ms=excluded.updated_at_ms",
            )?;
            for state in user_states {
                statement.execute(params![
                    state.item_id,
                    state.favorite,
                    state.played,
                    u64_to_i64(state.resume_ticks)?,
                    state.progress,
                    state.unplayed_count.map(i64::from),
                    state.last_played_at,
                    updated_at_ms,
                ])?;
            }
        }

        transaction.execute(
            "UPDATE catalog_scope SET next_start_index=?1, total_expected=COALESCE(?2,total_expected), \
             updated_at_ms=?3,sync_revision=sync_revision+1,content_revision=content_revision+1 \
             WHERE singleton=1",
            params![next_start_index, total_expected, updated_at_ms],
        )?;
        let mut search_index = self
            .search_index
            .write()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        transaction.commit()?;
        let snapshot = Arc::make_mut(&mut search_index);
        for document in changed_documents {
            snapshot.apply_delta(document)?;
        }
        Ok(())
    }

    /// Persists the explicit final count after every catalog stage has
    /// completed, including a final stage which returned zero rows.
    pub fn record_sync_progress(
        &self,
        run: CatalogSyncRun,
        processed: u64,
        final_total_expected: u64,
        updated_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let processed = u64_to_i64(processed)?;
        let final_total_expected = u64_to_i64(final_total_expected)?;
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        ensure_active_run(&transaction, run)?;
        transaction.execute(
            "UPDATE catalog_scope SET next_start_index=?1,total_expected=?2,updated_at_ms=?3,\
             sync_revision=sync_revision+1 WHERE singleton=1",
            params![processed, final_total_expected, updated_at_ms],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Marks the projection stale. If a traversal is active, its membership
    /// verification is invalidated so it cannot reconcile deletions using a
    /// snapshot taken before this change.
    pub fn mark_dirty(&self, updated_at_ms: i64) -> Result<(), MediaCatalogError> {
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        transaction.execute(
            "UPDATE catalog_scope SET sync_dirty=1,updated_at_ms=?1,\
             sync_revision=sync_revision+CASE WHEN active_run_id IS NULL THEN 0 ELSE 1 END,\
             membership_verified_revision=NULL WHERE singleton=1",
            [updated_at_ms],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Immediately removes a locally deleted item from both persisted facts
    /// and the live search snapshot, then schedules a full membership repair.
    pub fn remove_item_and_mark_dirty(
        &self,
        item_id: &str,
        updated_at_ms: i64,
    ) -> Result<bool, MediaCatalogError> {
        self.remove_item(item_id, updated_at_ms, true)
    }

    /// Removes an item (and any catalog descendants) without invalidating the
    /// committed full-sync generation. Persistent tombstones continue to mask
    /// matching ordinals in the mmap base after a restart; a later full publish
    /// folds the deletion into a new base and clears them.
    pub fn remove_item_incrementally(
        &self,
        item_id: &str,
        updated_at_ms: i64,
    ) -> Result<bool, MediaCatalogError> {
        self.remove_item(item_id, updated_at_ms, false)
    }

    fn remove_item(
        &self,
        item_id: &str,
        updated_at_ms: i64,
        mark_full_repair: bool,
    ) -> Result<bool, MediaCatalogError> {
        if item_id.trim().is_empty() {
            return Err(MediaCatalogError::InvalidItem);
        }
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        if !mark_full_repair {
            let active_run_id = transaction.query_row(
                "SELECT active_run_id FROM catalog_scope WHERE singleton=1",
                [],
                |row| row.get::<_, Option<i64>>(0),
            )?;
            if let Some(active_run_id) = active_run_id {
                return Err(MediaCatalogError::SyncAlreadyRunning(active_run_id));
            }
        }
        let removed_item_ids = expand_hierarchical_removals(&transaction, &[item_id.to_owned()])?;
        delete_catalog_items(&transaction, &removed_item_ids)?;
        transaction.execute(
            "UPDATE catalog_scope SET sync_dirty=CASE WHEN ?2 THEN 1 ELSE sync_dirty END,\
             updated_at_ms=?1,\
             sync_revision=sync_revision+CASE WHEN ?2 AND active_run_id IS NOT NULL THEN 1 ELSE 0 END,\
             membership_verified_revision=CASE WHEN ?2 THEN NULL ELSE membership_verified_revision END,\
             content_revision=content_revision+CASE WHEN ?3 THEN 1 ELSE 0 END \
             WHERE singleton=1",
            params![updated_at_ms, mark_full_repair, !removed_item_ids.is_empty()],
        )?;
        insert_search_tombstones(&transaction, &removed_item_ids, updated_at_ms)?;
        let mut search_index = self
            .search_index
            .write()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        transaction.commit()?;
        let snapshot = Arc::make_mut(&mut search_index);
        for removed_item_id in &removed_item_ids {
            snapshot.remove_item(removed_item_id);
        }
        Ok(!removed_item_ids.is_empty())
    }

    pub fn enqueue_library_changes(
        &self,
        upsert_ids: &[String],
        removed_ids: &[String],
        requires_membership: bool,
        enqueued_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        let mut enqueued_any = false;
        {
            let mut statement = transaction.prepare_cached(
                "INSERT INTO catalog_pending_changes(item_id,change_kind,enqueued_at_ms) \
                 VALUES(?1,?2,?3) ON CONFLICT(item_id) DO UPDATE SET \
                 change_kind=excluded.change_kind,enqueued_at_ms=excluded.enqueued_at_ms",
            )?;
            for item_id in upsert_ids {
                if !item_id.trim().is_empty() {
                    statement.execute(params![item_id.trim(), "upsert", enqueued_at_ms])?;
                    enqueued_any = true;
                }
            }
            // A removal wins if a malformed/bursty message lists the same ID
            // in more than one collection.
            for item_id in removed_ids {
                if !item_id.trim().is_empty() {
                    statement.execute(params![item_id.trim(), "remove", enqueued_at_ms])?;
                    enqueued_any = true;
                }
            }
        }
        if enqueued_any {
            transaction.execute(
                "UPDATE catalog_scope SET pending_revision=pending_revision+1,updated_at_ms=?1 \
                 WHERE singleton=1",
                [enqueued_at_ms],
            )?;
        }
        if requires_membership {
            transaction.execute(
                "UPDATE catalog_scope SET membership_required=1,\
                 membership_revision=membership_revision+1,updated_at_ms=?1 WHERE singleton=1",
                [enqueued_at_ms],
            )?;
        }
        transaction.commit()?;
        Ok(())
    }

    pub fn mark_notification_gap(&self, at_ms: i64) -> Result<(), MediaCatalogError> {
        let writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        writer.execute(
            "UPDATE catalog_scope SET incremental_catchup_required=1,\
             catchup_revision=catchup_revision+1,membership_required=1,\
             membership_revision=membership_revision+1,updated_at_ms=?1 WHERE singleton=1",
            [at_ms],
        )?;
        Ok(())
    }

    pub fn request_incremental_catchup(&self, at_ms: i64) -> Result<(), MediaCatalogError> {
        let writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        writer.execute(
            "UPDATE catalog_scope SET incremental_catchup_required=1,\
             catchup_revision=catchup_revision+1,updated_at_ms=?1 WHERE singleton=1",
            [at_ms],
        )?;
        Ok(())
    }

    pub fn pending_changes(
        &self,
        limit: usize,
    ) -> Result<CatalogPendingChanges, MediaCatalogError> {
        let limit = i64::try_from(limit.max(1)).map_err(|_| MediaCatalogError::NumericRange)?;
        let reader = self
            .reader
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let (
            catchup_required,
            membership_required,
            watermark_ms,
            last_membership_check_ms,
            failure_count,
            last_failure_at_ms,
            catchup_revision,
            membership_revision,
            pending_revision,
        ) = reader.query_row(
            "SELECT incremental_catchup_required,membership_required,incremental_watermark_ms,\
             last_membership_check_ms,incremental_failure_count,last_incremental_failure_at_ms,\
             catchup_revision,membership_revision,pending_revision \
             FROM catalog_scope WHERE singleton=1",
            [],
            |row| {
                Ok((
                    row.get::<_, bool>(0)?,
                    row.get::<_, bool>(1)?,
                    row.get(2)?,
                    row.get(3)?,
                    row.get::<_, i64>(4)?,
                    row.get(5)?,
                    row.get::<_, i64>(6)?,
                    row.get::<_, i64>(7)?,
                    row.get::<_, i64>(8)?,
                ))
            },
        )?;
        let mut statement = reader.prepare_cached(
            "SELECT item_id,change_kind FROM catalog_pending_changes \
             ORDER BY enqueued_at_ms,item_id LIMIT ?1",
        )?;
        let mut rows = statement.query([limit])?;
        let mut upsert_ids = Vec::new();
        let mut removed_ids = Vec::new();
        while let Some(row) = rows.next()? {
            let item_id = row.get::<_, String>(0)?;
            match row.get::<_, String>(1)?.as_str() {
                "upsert" => upsert_ids.push(item_id),
                "remove" => removed_ids.push(item_id),
                _ => return Err(MediaCatalogError::UnrecognizedSchema),
            }
        }
        Ok(CatalogPendingChanges {
            upsert_ids,
            removed_ids,
            catchup_required,
            membership_required,
            watermark_ms,
            last_membership_check_ms,
            failure_count: u32::try_from(failure_count)
                .map_err(|_| MediaCatalogError::NumericRange)?,
            last_failure_at_ms,
            catchup_revision: i64_to_u64(catchup_revision)?,
            membership_revision: i64_to_u64(membership_revision)?,
            pending_revision: i64_to_u64(pending_revision)?,
        })
    }

    pub fn membership_difference(
        &self,
        remote_ids: &HashSet<String>,
    ) -> Result<CatalogMembershipDiff, MediaCatalogError> {
        let reader = self
            .reader
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let local_ids = load_membership(&reader, None)?;
        let mut remote_only = remote_ids
            .difference(&local_ids)
            .cloned()
            .collect::<Vec<_>>();
        let mut local_only = local_ids
            .difference(remote_ids)
            .cloned()
            .collect::<Vec<_>>();
        remote_only.sort_unstable();
        local_only.sort_unstable();
        Ok(CatalogMembershipDiff {
            remote_only,
            local_only,
        })
    }

    pub fn record_incremental_checkpoint(
        &self,
        catchup_revision: u64,
        watermark_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let catchup_revision = u64_to_i64(catchup_revision)?;
        let writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        writer.execute(
            "UPDATE catalog_scope SET incremental_watermark_ms=MAX(\
                 COALESCE(incremental_watermark_ms,?1),?1),\
             incremental_catchup_required=CASE WHEN catchup_revision=?2 THEN 0 ELSE 1 END,\
             updated_at_ms=?1 WHERE singleton=1",
            params![watermark_ms, catchup_revision],
        )?;
        Ok(())
    }

    pub fn record_membership_check(
        &self,
        membership_revision: u64,
        checked_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let membership_revision = u64_to_i64(membership_revision)?;
        let writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        writer.execute(
            "UPDATE catalog_scope SET last_membership_check_ms=?1,\
             membership_required=CASE WHEN membership_revision=?2 THEN 0 ELSE 1 END,\
             incremental_failure_count=0,last_incremental_failure_at_ms=NULL,updated_at_ms=?1 \
             WHERE singleton=1",
            params![checked_at_ms, membership_revision],
        )?;
        Ok(())
    }

    pub fn record_incremental_failure(&self, failed_at_ms: i64) -> Result<u32, MediaCatalogError> {
        let writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let failures = writer.query_row(
            "UPDATE catalog_scope SET incremental_failure_count=incremental_failure_count+1,\
             last_incremental_failure_at_ms=?1,updated_at_ms=?1 WHERE singleton=1 \
             RETURNING incremental_failure_count",
            [failed_at_ms],
            |row| row.get::<_, i64>(0),
        )?;
        u32::try_from(failures).map_err(|_| MediaCatalogError::NumericRange)
    }

    pub fn record_incremental_success(
        &self,
        completed_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        writer.execute(
            "UPDATE catalog_scope SET incremental_failure_count=0,\
             last_incremental_failure_at_ms=NULL,updated_at_ms=?1 WHERE singleton=1",
            [completed_at_ms],
        )?;
        Ok(())
    }

    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn apply_incremental_changes(
        &self,
        items: &[CatalogItem],
        user_states: &[CatalogUserState],
        acknowledged_upsert_ids: &[String],
        removed_ids: &[String],
        pending_revision: u64,
        updated_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let prepared_items = items
            .iter()
            .map(|item| PreparedCatalogItem::new(item).map(|prepared| (item, prepared)))
            .collect::<Result<Vec<_>, _>>()?;
        let state_item_ids = user_states
            .iter()
            .map(|state| {
                validate_user_state(state)?;
                Ok(state.item_id.as_str())
            })
            .collect::<Result<HashSet<_>, MediaCatalogError>>()?;
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        let active_run_id = transaction.query_row(
            "SELECT active_run_id FROM catalog_scope WHERE singleton=1",
            [],
            |row| row.get::<_, Option<i64>>(0),
        )?;
        if let Some(active_run_id) = active_run_id {
            return Err(MediaCatalogError::SyncAlreadyRunning(active_run_id));
        }
        let mutation_id = transaction.query_row(
            "UPDATE catalog_scope SET next_run_id=next_run_id+1 WHERE singleton=1 \
             RETURNING next_run_id",
            [],
            |row| row.get::<_, i64>(0),
        )?;

        let existing_fingerprints = load_page_fingerprints(&transaction, items)?;
        let mut changed_documents = Vec::new();
        {
            let mut statement = transaction.prepare_cached(
                "INSERT INTO catalog_items(\
                    item_id,item_type,title,sort_title,original_title,parent_id,series_id,series_title,\
                    season_id,season_title,season_number,episode_number,production_year,source_updated_at,\
                    image_tag,primary_image_aspect_ratio,aliases_json,search_fingerprint,\
                    fact_fingerprint,search_changed_run_id,seen_run_id,cached_at_ms\
                 ) VALUES(\
                    ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22\
                 ) ON CONFLICT(item_id) DO UPDATE SET \
                    item_type=excluded.item_type,title=excluded.title,sort_title=excluded.sort_title,\
                    original_title=excluded.original_title,parent_id=excluded.parent_id,series_id=excluded.series_id,\
                    series_title=excluded.series_title,season_id=excluded.season_id,\
                    season_title=excluded.season_title,season_number=excluded.season_number,\
                    episode_number=excluded.episode_number,production_year=excluded.production_year,\
                    source_updated_at=excluded.source_updated_at,image_tag=excluded.image_tag,\
                    primary_image_aspect_ratio=excluded.primary_image_aspect_ratio,\
                    aliases_json=excluded.aliases_json,\
                    search_changed_run_id=CASE WHEN \
                        catalog_items.search_fingerprint<>excluded.search_fingerprint THEN \
                        excluded.search_changed_run_id ELSE catalog_items.search_changed_run_id END,\
                    search_fingerprint=excluded.search_fingerprint,\
                    fact_fingerprint=excluded.fact_fingerprint,cached_at_ms=excluded.cached_at_ms",
            )?;
            for (item, prepared) in &prepared_items {
                let existing = existing_fingerprints.get(&item.id);
                if existing.map(|value| value.0.as_slice())
                    != Some(prepared.search_fingerprint.as_slice())
                {
                    changed_documents.push(prepared.index_document(item));
                }
                if existing.map(|value| value.1.as_slice())
                    == Some(prepared.fact_fingerprint.as_slice())
                {
                    continue;
                }
                statement.execute(params![
                    item.id,
                    item.item_type,
                    item.title,
                    prepared.sort_title,
                    item.original_title,
                    item.parent_id,
                    item.series_id,
                    item.series_title,
                    item.season_id,
                    item.season_title,
                    item.season_number,
                    item.episode_number,
                    item.production_year,
                    item.source_updated_at,
                    item.image_tag,
                    item.primary_image_aspect_ratio,
                    prepared.aliases_json,
                    prepared.search_fingerprint,
                    prepared.fact_fingerprint,
                    mutation_id,
                    mutation_id,
                    updated_at_ms,
                ])?;
            }
        }
        delete_search_tombstones(&transaction, items.iter().map(|item| item.id.as_str()))?;
        let missing_user_states = items
            .iter()
            .map(|item| item.id.as_str())
            .filter(|item_id| !state_item_ids.contains(item_id))
            .collect::<Vec<_>>();
        delete_user_states(&transaction, &missing_user_states)?;
        {
            let mut statement = transaction.prepare_cached(
                "INSERT INTO catalog_user_state(\
                    item_id,favorite,played,resume_ticks,progress,unplayed_count,last_played_at,updated_at_ms\
                 ) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)\
                 ON CONFLICT(item_id) DO UPDATE SET favorite=excluded.favorite,played=excluded.played,\
                    resume_ticks=excluded.resume_ticks,progress=excluded.progress,\
                    unplayed_count=excluded.unplayed_count,last_played_at=excluded.last_played_at,\
                    updated_at_ms=excluded.updated_at_ms",
            )?;
            for state in user_states {
                statement.execute(params![
                    state.item_id,
                    state.favorite,
                    state.played,
                    u64_to_i64(state.resume_ticks)?,
                    state.progress,
                    state.unplayed_count.map(i64::from),
                    state.last_played_at,
                    updated_at_ms,
                ])?;
            }
        }

        let removed_item_ids = expand_hierarchical_removals(&transaction, removed_ids)?;
        delete_catalog_items(&transaction, &removed_item_ids)?;
        insert_search_tombstones(&transaction, &removed_item_ids, updated_at_ms)?;
        let pending_revision = u64_to_i64(pending_revision)?;
        let current_pending_revision = transaction.query_row(
            "SELECT pending_revision FROM catalog_scope WHERE singleton=1",
            [],
            |row| row.get::<_, i64>(0),
        )?;
        if current_pending_revision == pending_revision {
            let mut statement = transaction
                .prepare_cached("DELETE FROM catalog_pending_changes WHERE item_id=?1")?;
            for item_id in acknowledged_upsert_ids.iter().chain(removed_ids) {
                statement.execute([item_id])?;
            }
        }
        let content_changed = !items.is_empty() || !removed_item_ids.is_empty();
        transaction.execute(
            "UPDATE catalog_scope SET updated_at_ms=?1,\
             content_revision=content_revision+CASE WHEN ?2 THEN 1 ELSE 0 END WHERE singleton=1",
            params![updated_at_ms, content_changed],
        )?;
        let mut search_index = self
            .search_index
            .write()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        transaction.commit()?;
        let snapshot = Arc::make_mut(&mut search_index);
        for document in changed_documents {
            snapshot.apply_delta(document)?;
        }
        for item_id in &removed_item_ids {
            snapshot.remove_item(item_id);
        }
        Ok(())
    }

    /// Compares a remote ID-only probe with the current committed projection.
    pub fn membership_matches(
        &self,
        remote_ids: &HashSet<String>,
    ) -> Result<bool, MediaCatalogError> {
        let reader = self
            .reader
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let local_ids = load_membership(&reader, None)?;
        Ok(local_ids == *remote_ids)
    }

    /// Records an exact second-pass membership verification for this run. The
    /// revision fence makes the result single-use if any later mutation marks
    /// the projection dirty.
    pub fn verify_sync_membership(
        &self,
        run: CatalogSyncRun,
        remote_ids: &HashSet<String>,
        final_total: u64,
        verified_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        ensure_active_run(&transaction, run)?;
        let observed =
            u64::try_from(remote_ids.len()).map_err(|_| MediaCatalogError::NumericRange)?;
        if observed != final_total {
            transaction.execute(
                "UPDATE catalog_scope SET sync_dirty=1,updated_at_ms=?1,\
                 membership_verified_revision=NULL WHERE singleton=1",
                [verified_at_ms],
            )?;
            transaction.commit()?;
            return Err(MediaCatalogError::MembershipCountMismatch {
                expected: final_total,
                observed,
            });
        }
        let local_ids = load_membership(&transaction, Some(run))?;
        let remote_only = remote_ids.difference(&local_ids).count();
        let local_only = local_ids.difference(remote_ids).count();
        if remote_only != 0 || local_only != 0 {
            transaction.execute(
                "UPDATE catalog_scope SET sync_dirty=1,updated_at_ms=?1,\
                 membership_verified_revision=NULL WHERE singleton=1",
                [verified_at_ms],
            )?;
            transaction.commit()?;
            return Err(MediaCatalogError::MembershipMismatch {
                remote_only,
                local_only,
            });
        }
        transaction.execute(
            "UPDATE catalog_scope SET membership_verified_revision=sync_revision,updated_at_ms=?1 \
             WHERE singleton=1",
            [verified_at_ms],
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Completes a traversal only after a current exact membership pass.
    pub fn complete_verified_sync(
        &self,
        run: CatalogSyncRun,
        completed_at_ms: i64,
    ) -> Result<u64, MediaCatalogError> {
        self.complete_sync_inner(run, completed_at_ms, true)
    }

    /// Completes a full traversal and removes items not seen by this run.
    /// Returns the number of reconciled deletions.
    #[cfg(test)]
    pub(crate) fn complete_sync(
        &self,
        run: CatalogSyncRun,
        completed_at_ms: i64,
    ) -> Result<u64, MediaCatalogError> {
        self.complete_sync_inner(run, completed_at_ms, false)
    }

    fn complete_sync_inner(
        &self,
        run: CatalogSyncRun,
        completed_at_ms: i64,
        require_verified_membership: bool,
    ) -> Result<u64, MediaCatalogError> {
        loop {
            let (revision, build_decision) = {
                let mut writer = self
                    .writer
                    .lock()
                    .map_err(|_| MediaCatalogError::Poisoned)?;
                let transaction = writer.transaction()?;
                ensure_active_run(&transaction, run)?;
                let reconciliation = read_reconciliation(&transaction, run)?;
                if !reconciliation.matches() {
                    mark_reconciliation_failed(&transaction, completed_at_ms)?;
                    transaction.commit()?;
                    return Err(reconciliation.into_error());
                }
                if require_verified_membership && !reconciliation.membership_verified() {
                    return Err(MediaCatalogError::MembershipNotVerified(run.id));
                }
                let build_decision = index_build_decision(&transaction, run)?;
                transaction.commit()?;
                (reconciliation.revision, build_decision)
            };

            // This is intentionally outside both the writer and index locks.
            // Queries continue using the old immutable snapshot during the
            // multi-second sidecar build.
            let candidate = build_decision
                .required
                .then(|| {
                    build_persistent_index(
                        &self.path,
                        run.id,
                        Some(run.id),
                        catalog_scope_fingerprint(&self.scope, &self.path),
                    )
                })
                .transpose()?
                .map(Arc::new);

            let mut writer = self
                .writer
                .lock()
                .map_err(|_| MediaCatalogError::Poisoned)?;
            let transaction = writer.transaction()?;
            ensure_active_run(&transaction, run)?;
            let reconciliation = read_reconciliation(&transaction, run)?;
            if !reconciliation.matches() {
                mark_reconciliation_failed(&transaction, completed_at_ms)?;
                transaction.commit()?;
                return Err(reconciliation.into_error());
            }
            if require_verified_membership && !reconciliation.membership_verified() {
                return Err(MediaCatalogError::MembershipNotVerified(run.id));
            }
            let final_build_decision = index_build_decision(&transaction, run)?;
            if reconciliation.revision != revision || final_build_decision != build_decision {
                transaction.commit()?;
                continue;
            }

            let deleted =
                transaction.execute("DELETE FROM catalog_items WHERE seen_run_id<>?1", [run.id])?;
            transaction.execute("DELETE FROM catalog_search_tombstones", [])?;
            let cached_count = reconciliation.seen;
            let published_generation = candidate.as_ref().map(|_| run.id);
            transaction.execute(
                "UPDATE catalog_scope SET active_run_id=NULL,last_completed_run_id=?1,\
                 search_generation=COALESCE(?4,search_generation),sync_state='complete',\
                 cached_count=?2,next_start_index=?2,updated_at_ms=?3,\
                 last_completed_at_ms=?3,last_error=NULL,membership_verified_revision=NULL,\
                 incremental_watermark_ms=MAX(COALESCE(incremental_watermark_ms,?3),?3),\
                 last_membership_check_ms=?3,\
                 incremental_catchup_required=0,membership_required=0,\
                 incremental_failure_count=0,last_incremental_failure_at_ms=NULL,\
                 content_revision=content_revision+1 \
                 WHERE singleton=1",
                params![run.id, cached_count, completed_at_ms, published_generation],
            )?;
            let mut search_index = self
                .search_index
                .write()
                .map_err(|_| MediaCatalogError::Poisoned)?;
            transaction.commit()?;
            if let Some(candidate) = candidate {
                Arc::make_mut(&mut search_index).publish(candidate);
            }
            // Prevent new read snapshots while the completed run's WAL is
            // checkpointed. Existing searches do not retain this lock and can
            // finish normally; TRUNCATE then leaves no duplicate fact pages on
            // disk after a full publish.
            let _ = writer.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);");
            drop(search_index);
            cleanup_sidecars(
                &self.path,
                published_generation.or(final_build_decision.generation),
                false,
            )?;
            return u64::try_from(deleted).map_err(|_| MediaCatalogError::NumericRange);
        }
    }

    pub fn fail_sync(
        &self,
        run: CatalogSyncRun,
        error: &str,
        failed_at_ms: i64,
    ) -> Result<(), MediaCatalogError> {
        let mut writer = self
            .writer
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let transaction = writer.transaction()?;
        ensure_active_run(&transaction, run)?;
        transaction.execute(
            "UPDATE catalog_scope SET active_run_id=NULL,sync_state='failed',updated_at_ms=?1,\
             last_error=?2,sync_dirty=1,membership_verified_revision=NULL WHERE singleton=1",
            params![failed_at_ms, error.trim()],
        )?;
        transaction.commit()?;
        Ok(())
    }

    pub fn sync_status(&self) -> Result<SyncStatus, MediaCatalogError> {
        let reader = self
            .reader
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        read_sync_status(&reader)
    }

    pub fn search(&self, query: &str) -> Result<CatalogSearchPage, MediaCatalogError> {
        self.search_with_limit(query, DEFAULT_SEARCH_LIMIT)
    }

    pub fn search_with_limit(
        &self,
        query: &str,
        limit: usize,
    ) -> Result<CatalogSearchPage, MediaCatalogError> {
        let normalized_query = normalize_search_text(query);
        let limit = limit.clamp(1, MAX_SEARCH_LIMIT);
        let mut reader = self
            .reader
            .lock()
            .map_err(|_| MediaCatalogError::Poisoned)?;
        let (transaction, status, candidates, total_matches) = {
            // Acquire the fence before opening the SQLite snapshot. A
            // completing writer can therefore checkpoint under its write lock
            // without a new reader opening a transaction and then waiting on
            // this same fence. Run the bounded posting query under the read
            // guard as well: keeping the snapshot Arc unique avoids copying an
            // accumulated delta when a concurrent page is published.
            let search_index = self
                .search_index
                .read()
                .map_err(|_| MediaCatalogError::Poisoned)?;
            let transaction = reader.transaction()?;
            let status = read_sync_status(&transaction)?;
            let (candidates, total_matches) = if normalized_query.is_empty() {
                (Vec::new(), 0)
            } else {
                search_index.query(&normalized_query, limit)?
            };
            (transaction, status, candidates, total_matches)
        };
        let mut items = load_search_hits(&transaction, &candidates)?;
        items.sort_by_cached_key(|hit| {
            (
                search_category(&hit.item.item_type),
                hit.match_rank,
                normalize_search_text(&hit.item.sort_title),
                hit.item.id.clone(),
            )
        });
        let mut title_count = 0_usize;
        let mut episode_count = 0_usize;
        items.retain(|hit| match search_category(&hit.item.item_type) {
            CATEGORY_TITLE if title_count < limit => {
                title_count += 1;
                true
            }
            CATEGORY_EPISODE if episode_count < limit => {
                episode_count += 1;
                true
            }
            _ => false,
        });
        let has_more = total_matches > items.len() as u64;
        transaction.commit()?;
        Ok(CatalogSearchPage {
            items,
            total_matches,
            has_more,
            status,
        })
    }
}

struct PreparedCatalogItem {
    sort_title: String,
    aliases: Vec<String>,
    aliases_json: String,
    search_fingerprint: Vec<u8>,
    fact_fingerprint: Vec<u8>,
}

type StoredFingerprints = (Vec<u8>, Vec<u8>);

impl PreparedCatalogItem {
    fn new(item: &CatalogItem) -> Result<Self, MediaCatalogError> {
        if item.id.trim().is_empty()
            || item.item_type.trim().is_empty()
            || item.title.trim().is_empty()
            || item
                .primary_image_aspect_ratio
                .is_some_and(|value| !value.is_finite() || value <= 0.0)
        {
            return Err(MediaCatalogError::InvalidItem);
        }
        let sort_title = if item.sort_title.trim().is_empty() {
            item.title.clone()
        } else {
            item.sort_title.clone()
        };
        let mut aliases = Vec::new();
        let mut seen_aliases = HashSet::new();
        for alias in &item.aliases {
            let alias = alias.trim();
            if alias.is_empty() {
                continue;
            }
            let normalized = normalize_search_text(alias);
            if !normalized.is_empty() && seen_aliases.insert(normalized) {
                aliases.push(alias.to_owned());
            }
        }
        let aliases_json = serde_json::to_string(&aliases)?;
        let search_fingerprint = raw_search_fingerprint(item, &sort_title, &aliases);
        let fact_fingerprint = raw_fact_fingerprint(item, &search_fingerprint);
        Ok(Self {
            sort_title,
            aliases,
            aliases_json,
            search_fingerprint,
            fact_fingerprint,
        })
    }

    fn index_document(&self, item: &CatalogItem) -> IndexDocument {
        prepare_index_document(
            &item.id,
            &item.item_type,
            &item.title,
            &self.sort_title,
            item.original_title.as_deref(),
            item.series_title.as_deref(),
            item.season_title.as_deref(),
            &self.aliases,
        )
    }
}

#[allow(clippy::too_many_arguments)]
fn prepare_index_document(
    item_id: &str,
    item_type: &str,
    title: &str,
    sort_title: &str,
    original_title: Option<&str>,
    series_title: Option<&str>,
    season_title: Option<&str>,
    aliases: &[String],
) -> IndexDocument {
    prepare_index_document_with_sort_key(
        item_id,
        item_type,
        title,
        normalize_search_text(sort_title),
        original_title,
        series_title,
        season_title,
        aliases,
    )
}

#[allow(clippy::too_many_arguments)]
fn prepare_index_document_with_sort_key(
    item_id: &str,
    item_type: &str,
    title: &str,
    sort_key: String,
    original_title: Option<&str>,
    series_title: Option<&str>,
    season_title: Option<&str>,
    aliases: &[String],
) -> IndexDocument {
    let title = normalize_search_text(title);
    let original_title = normalize_optional(original_title);
    let series_title = normalize_optional(series_title);
    let season_title = normalize_optional(season_title);
    let aliases = aliases
        .iter()
        .map(|alias| normalize_search_text(alias))
        .filter(|alias| !alias.is_empty())
        .collect::<Vec<_>>();
    let normalized_sources = std::iter::once(title.as_str())
        .chain((!original_title.is_empty()).then_some(original_title.as_str()))
        .chain((!series_title.is_empty()).then_some(series_title.as_str()))
        .chain((!season_title.is_empty()).then_some(season_title.as_str()))
        .chain(aliases.iter().map(String::as_str))
        .collect::<Vec<_>>();
    let (pinyin_full, pinyin_initials) = pinyin_forms(&normalized_sources);
    IndexDocument {
        item_id: item_id.to_owned(),
        category: search_category(item_type),
        sort_key,
        title,
        original_title,
        series_title,
        season_title,
        aliases,
        pinyin_full,
        pinyin_initials,
    }
}

struct FingerprintBuilder {
    first: u64,
    second: u64,
}

impl FingerprintBuilder {
    fn new() -> Self {
        Self {
            first: 0xcbf2_9ce4_8422_2325_u64,
            second: 0x8422_2325_cbf2_9ce4_u64,
        }
    }

    fn bytes(&mut self, value: &[u8]) {
        let length = u64::try_from(value.len()).unwrap_or(u64::MAX);
        for byte in length
            .to_le_bytes()
            .into_iter()
            .chain(value.iter().copied())
        {
            self.first = (self.first ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3);
            self.second = (self.second ^ u64::from(byte)).wrapping_mul(0x9e37_79b1_85eb_ca87);
        }
    }

    fn string(&mut self, value: &str) {
        self.bytes(value.as_bytes());
    }

    fn optional_string(&mut self, value: Option<&str>) {
        self.bytes(&[u8::from(value.is_some())]);
        if let Some(value) = value {
            self.string(value);
        }
    }

    fn optional_i32(&mut self, value: Option<i32>) {
        self.bytes(&[u8::from(value.is_some())]);
        if let Some(value) = value {
            self.bytes(&value.to_le_bytes());
        }
    }

    fn finish(self) -> Vec<u8> {
        let mut fingerprint = Vec::with_capacity(16);
        fingerprint.extend_from_slice(&self.first.to_le_bytes());
        fingerprint.extend_from_slice(&self.second.to_le_bytes());
        fingerprint
    }
}

fn raw_search_fingerprint(item: &CatalogItem, sort_title: &str, aliases: &[String]) -> Vec<u8> {
    let mut fingerprint = FingerprintBuilder::new();
    fingerprint.string(&item.item_type);
    fingerprint.string(&item.title);
    fingerprint.string(sort_title);
    fingerprint.optional_string(item.original_title.as_deref());
    fingerprint.optional_string(item.series_title.as_deref());
    fingerprint.optional_string(item.season_title.as_deref());
    fingerprint.bytes(
        &u64::try_from(aliases.len())
            .unwrap_or(u64::MAX)
            .to_le_bytes(),
    );
    for alias in aliases {
        fingerprint.string(alias);
    }
    fingerprint.finish()
}

fn raw_fact_fingerprint(item: &CatalogItem, search_fingerprint: &[u8]) -> Vec<u8> {
    let mut fingerprint = FingerprintBuilder::new();
    fingerprint.bytes(search_fingerprint);
    fingerprint.string(&item.id);
    fingerprint.string(&item.item_type);
    fingerprint.optional_string(item.parent_id.as_deref());
    fingerprint.optional_string(item.series_id.as_deref());
    fingerprint.optional_string(item.season_id.as_deref());
    fingerprint.optional_i32(item.season_number);
    fingerprint.optional_i32(item.episode_number);
    fingerprint.optional_i32(item.production_year);
    fingerprint.optional_string(item.source_updated_at.as_deref());
    fingerprint.optional_string(item.image_tag.as_deref());
    fingerprint.bytes(&[u8::from(item.primary_image_aspect_ratio.is_some())]);
    if let Some(value) = item.primary_image_aspect_ratio {
        fingerprint.bytes(&value.to_bits().to_le_bytes());
    }
    fingerprint.finish()
}

fn load_page_fingerprints(
    transaction: &Transaction<'_>,
    items: &[CatalogItem],
) -> Result<HashMap<String, StoredFingerprints>, MediaCatalogError> {
    if items.is_empty() {
        return Ok(HashMap::new());
    }
    let sql = format!(
        "SELECT item_id,search_fingerprint,fact_fingerprint FROM catalog_items WHERE item_id IN ({})",
        sql_placeholders(items.len())
    );
    let mut statement = transaction.prepare(&sql)?;
    let mut rows = statement.query(params_from_iter(items.iter().map(|item| item.id.as_str())))?;
    let mut fingerprints = HashMap::with_capacity(items.len());
    while let Some(row) = rows.next()? {
        fingerprints.insert(
            row.get::<_, String>(0)?,
            (row.get::<_, Vec<u8>>(1)?, row.get::<_, Vec<u8>>(2)?),
        );
    }
    Ok(fingerprints)
}

fn mark_items_seen(
    transaction: &Transaction<'_>,
    item_ids: &[&str],
    run_id: i64,
    updated_at_ms: i64,
) -> Result<(), MediaCatalogError> {
    if item_ids.is_empty() {
        return Ok(());
    }
    let sql = format!(
        "UPDATE catalog_items SET seen_run_id=?,cached_at_ms=? WHERE item_id IN ({})",
        sql_placeholders(item_ids.len())
    );
    let mut values = Vec::with_capacity(item_ids.len() + 2);
    values.push(rusqlite::types::Value::Integer(run_id));
    values.push(rusqlite::types::Value::Integer(updated_at_ms));
    values.extend(
        item_ids
            .iter()
            .map(|item_id| rusqlite::types::Value::Text((*item_id).to_owned())),
    );
    transaction.execute(&sql, params_from_iter(values))?;
    Ok(())
}

fn delete_user_states(
    transaction: &Transaction<'_>,
    item_ids: &[&str],
) -> Result<(), MediaCatalogError> {
    if item_ids.is_empty() {
        return Ok(());
    }
    let sql = format!(
        "DELETE FROM catalog_user_state WHERE item_id IN ({})",
        sql_placeholders(item_ids.len())
    );
    transaction.execute(&sql, params_from_iter(item_ids.iter().copied()))?;
    Ok(())
}

fn expand_hierarchical_removals(
    transaction: &Transaction<'_>,
    root_ids: &[String],
) -> Result<Vec<String>, MediaCatalogError> {
    let mut removed = HashSet::new();
    for root_id in root_ids
        .iter()
        .map(|value| value.trim())
        .filter(|value| !value.is_empty())
    {
        removed.insert(root_id.to_owned());
        let item_type = transaction
            .query_row(
                "SELECT item_type FROM catalog_items WHERE item_id=?1",
                [root_id],
                |row| row.get::<_, String>(0),
            )
            .optional()?;
        let predicate = match item_type.as_deref() {
            Some("Series") => Some("series_id=?1"),
            Some("Season") => Some("season_id=?1"),
            _ => None,
        };
        if let Some(predicate) = predicate {
            let mut statement = transaction.prepare(&format!(
                "SELECT item_id FROM catalog_items WHERE {predicate}"
            ))?;
            let rows = statement.query_map([root_id], |row| row.get::<_, String>(0))?;
            removed.extend(rows.collect::<Result<Vec<_>, _>>()?);
        }
    }
    let mut removed = removed.into_iter().collect::<Vec<_>>();
    removed.sort_unstable();
    Ok(removed)
}

fn delete_catalog_items(
    transaction: &Transaction<'_>,
    item_ids: &[String],
) -> Result<(), MediaCatalogError> {
    for item_ids in item_ids.chunks(500) {
        if item_ids.is_empty() {
            continue;
        }
        let sql = format!(
            "DELETE FROM catalog_items WHERE item_id IN ({})",
            sql_placeholders(item_ids.len())
        );
        transaction.execute(&sql, params_from_iter(item_ids.iter()))?;
    }
    Ok(())
}

fn load_search_tombstones(connection: &Connection) -> Result<Vec<String>, MediaCatalogError> {
    let mut statement =
        connection.prepare("SELECT item_id FROM catalog_search_tombstones ORDER BY item_id")?;
    let rows = statement.query_map([], |row| row.get::<_, String>(0))?;
    rows.collect::<Result<Vec<_>, _>>()
        .map_err(MediaCatalogError::from)
}

fn insert_search_tombstones(
    transaction: &Transaction<'_>,
    item_ids: &[String],
    removed_at_ms: i64,
) -> Result<(), MediaCatalogError> {
    let mut statement = transaction.prepare_cached(
        "INSERT INTO catalog_search_tombstones(item_id,removed_at_ms) VALUES(?1,?2) \
         ON CONFLICT(item_id) DO UPDATE SET removed_at_ms=excluded.removed_at_ms",
    )?;
    for item_id in item_ids {
        statement.execute(params![item_id, removed_at_ms])?;
    }
    Ok(())
}

fn delete_search_tombstones<'a>(
    transaction: &Transaction<'_>,
    item_ids: impl Iterator<Item = &'a str>,
) -> Result<(), MediaCatalogError> {
    let mut statement =
        transaction.prepare_cached("DELETE FROM catalog_search_tombstones WHERE item_id=?1")?;
    for item_id in item_ids {
        statement.execute([item_id])?;
    }
    Ok(())
}

fn sql_placeholders(count: usize) -> String {
    vec!["?"; count].join(",")
}

fn configure_writer(connection: &Connection) -> Result<(), MediaCatalogError> {
    connection.busy_timeout(Duration::from_secs(5))?;
    connection.pragma_update(None, "foreign_keys", "ON")?;
    connection.pragma_update(None, "synchronous", "NORMAL")?;
    connection.pragma_update(None, "temp_store", "MEMORY")?;
    connection.pragma_update(None, "cache_size", -4_096_i64)?;
    connection.pragma_update(None, "mmap_size", 0_i64)?;
    let journal_mode: String =
        connection.pragma_query_value(None, "journal_mode", |row| row.get(0))?;
    if !journal_mode.eq_ignore_ascii_case("wal") {
        connection.pragma_update(None, "journal_mode", "WAL")?;
    }
    Ok(())
}

fn open_reader(path: &Path) -> Result<Connection, MediaCatalogError> {
    let connection = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    connection.busy_timeout(Duration::from_secs(1))?;
    connection.pragma_update(None, "query_only", "ON")?;
    connection.pragma_update(None, "temp_store", "MEMORY")?;
    connection.pragma_update(None, "cache_size", -2_048_i64)?;
    connection.pragma_update(None, "mmap_size", 0_i64)?;
    Ok(connection)
}

#[allow(clippy::too_many_lines)]
fn initialize_or_validate_schema(
    connection: &mut Connection,
    scope: &CatalogScope,
) -> Result<(), MediaCatalogError> {
    let has_scope_table = connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='catalog_scope')",
        [],
        |row| row.get::<_, bool>(0),
    )?;
    let schema_version =
        connection.pragma_query_value(None, "user_version", |row| row.get::<_, i64>(0))?;
    if has_scope_table {
        if schema_version != MEDIA_CATALOG_SCHEMA_VERSION {
            return Err(MediaCatalogError::SchemaVersion {
                found: schema_version,
                supported: MEDIA_CATALOG_SCHEMA_VERSION,
            });
        }
        let stored_scope = connection
            .query_row(
                "SELECT server_local_id,server_id,user_id,schema_version,search_index_version \
                 FROM catalog_scope WHERE singleton=1",
                [],
                |row| {
                    Ok((
                        CatalogScope::new(
                            row.get::<_, String>(0)?,
                            row.get::<_, String>(1)?,
                            row.get::<_, String>(2)?,
                        ),
                        row.get::<_, i64>(3)?,
                        row.get::<_, i64>(4)?,
                    ))
                },
            )
            .optional()?
            .ok_or(MediaCatalogError::UnrecognizedSchema)?;
        if stored_scope.0 != *scope {
            return Err(MediaCatalogError::ScopeMismatch);
        }
        if stored_scope.1 != MEDIA_CATALOG_SCHEMA_VERSION {
            return Err(MediaCatalogError::SchemaVersion {
                found: stored_scope.1,
                supported: MEDIA_CATALOG_SCHEMA_VERSION,
            });
        }
        if stored_scope.2 != MEDIA_CATALOG_SEARCH_VERSION {
            return Err(MediaCatalogError::SearchVersion {
                found: stored_scope.2,
                supported: MEDIA_CATALOG_SEARCH_VERSION,
            });
        }
        return Ok(());
    }

    let existing_objects = connection.query_row(
        "SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'",
        [],
        |row| row.get::<_, i64>(0),
    )?;
    if existing_objects != 0 || schema_version != 0 {
        return Err(MediaCatalogError::UnrecognizedSchema);
    }

    let transaction = connection.transaction()?;
    transaction.execute_batch(
        r"
        CREATE TABLE catalog_scope (
            singleton INTEGER PRIMARY KEY CHECK(singleton=1),
            schema_version INTEGER NOT NULL,
            search_index_version INTEGER NOT NULL,
            server_local_id TEXT NOT NULL,
            server_id TEXT NOT NULL,
            user_id TEXT NOT NULL,
            next_run_id INTEGER NOT NULL DEFAULT 0,
            active_run_id INTEGER,
            last_completed_run_id INTEGER,
            search_generation INTEGER,
            sync_state TEXT NOT NULL DEFAULT 'idle'
                CHECK(sync_state IN ('idle','running','complete','failed')),
            sync_revision INTEGER NOT NULL DEFAULT 0,
            sync_dirty INTEGER NOT NULL DEFAULT 1,
            membership_verified_revision INTEGER,
            content_revision INTEGER NOT NULL DEFAULT 0,
            incremental_watermark_ms INTEGER,
            incremental_catchup_required INTEGER NOT NULL DEFAULT 1,
            catchup_revision INTEGER NOT NULL DEFAULT 0,
            membership_required INTEGER NOT NULL DEFAULT 1,
            membership_revision INTEGER NOT NULL DEFAULT 0,
            pending_revision INTEGER NOT NULL DEFAULT 0,
            last_membership_check_ms INTEGER,
            incremental_failure_count INTEGER NOT NULL DEFAULT 0,
            last_incremental_failure_at_ms INTEGER,
            next_start_index INTEGER NOT NULL DEFAULT 0,
            cached_count INTEGER NOT NULL DEFAULT 0,
            total_expected INTEGER,
            started_at_ms INTEGER,
            updated_at_ms INTEGER,
            last_completed_at_ms INTEGER,
            last_error TEXT
        );

        CREATE TABLE catalog_items (
            row_id INTEGER PRIMARY KEY,
            item_id TEXT NOT NULL UNIQUE,
            item_type TEXT NOT NULL,
            title TEXT NOT NULL,
            sort_title TEXT NOT NULL,
            original_title TEXT,
            parent_id TEXT,
            series_id TEXT,
            series_title TEXT,
            season_id TEXT,
            season_title TEXT,
            season_number INTEGER,
            episode_number INTEGER,
            production_year INTEGER,
            source_updated_at TEXT,
            image_tag TEXT,
            primary_image_aspect_ratio REAL,
            aliases_json TEXT NOT NULL,
            search_fingerprint BLOB NOT NULL,
            fact_fingerprint BLOB NOT NULL,
            search_changed_run_id INTEGER NOT NULL,
            seen_run_id INTEGER NOT NULL,
            cached_at_ms INTEGER NOT NULL
        );
        CREATE INDEX catalog_items_type_sort
            ON catalog_items(item_type,sort_title,item_id);
        CREATE INDEX catalog_items_series
            ON catalog_items(series_id,season_number,episode_number,item_id);
        CREATE INDEX catalog_items_season
            ON catalog_items(season_id,episode_number,item_id);
        CREATE INDEX catalog_items_seen_run ON catalog_items(seen_run_id);
        CREATE INDEX catalog_items_search_changed_run
            ON catalog_items(search_changed_run_id);

        CREATE TABLE catalog_user_state (
            item_id TEXT PRIMARY KEY,
            favorite INTEGER NOT NULL,
            played INTEGER NOT NULL,
            resume_ticks INTEGER NOT NULL,
            progress REAL,
            unplayed_count INTEGER,
            last_played_at TEXT,
            updated_at_ms INTEGER NOT NULL,
            FOREIGN KEY(item_id) REFERENCES catalog_items(item_id) ON DELETE CASCADE
        );

        CREATE TABLE catalog_pending_changes (
            item_id TEXT PRIMARY KEY,
            change_kind TEXT NOT NULL CHECK(change_kind IN ('upsert','remove')),
            enqueued_at_ms INTEGER NOT NULL
        );

        CREATE TABLE catalog_search_tombstones (
            item_id TEXT PRIMARY KEY,
            removed_at_ms INTEGER NOT NULL
        );

        CREATE TRIGGER catalog_items_ai AFTER INSERT ON catalog_items BEGIN
            UPDATE catalog_scope SET cached_count=cached_count+1 WHERE singleton=1;
        END;
        CREATE TRIGGER catalog_items_ad AFTER DELETE ON catalog_items BEGIN
            UPDATE catalog_scope SET cached_count=cached_count-1 WHERE singleton=1;
        END;
        ",
    )?;
    transaction.execute(
        "INSERT INTO catalog_scope(\
            singleton,schema_version,search_index_version,server_local_id,server_id,user_id\
         ) VALUES(1,?1,?2,?3,?4,?5)",
        params![
            MEDIA_CATALOG_SCHEMA_VERSION,
            MEDIA_CATALOG_SEARCH_VERSION,
            scope.server_local_id,
            scope.server_id,
            scope.user_id,
        ],
    )?;
    transaction.pragma_update(None, "user_version", MEDIA_CATALOG_SCHEMA_VERSION)?;
    transaction.commit()?;
    Ok(())
}

struct ReconciliationSnapshot {
    expected: Option<u64>,
    seen: u64,
    revision: i64,
    membership_verified_revision: Option<i64>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct IndexBuildDecision {
    required: bool,
    generation: Option<i64>,
}

fn index_build_decision(
    transaction: &Transaction<'_>,
    run: CatalogSyncRun,
) -> Result<IndexBuildDecision, MediaCatalogError> {
    let generation = transaction.query_row(
        "SELECT search_generation FROM catalog_scope WHERE singleton=1",
        [],
        |row| row.get::<_, Option<i64>>(0),
    )?;
    let has_unseen = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM catalog_items WHERE seen_run_id<>?1 LIMIT 1)",
        [run.id],
        |row| row.get::<_, bool>(0),
    )?;
    let has_changed_search = match generation {
        Some(generation) => transaction.query_row(
            "SELECT EXISTS(SELECT 1 FROM catalog_items WHERE search_changed_run_id>?1 LIMIT 1)",
            [generation],
            |row| row.get::<_, bool>(0),
        )?,
        None => true,
    };
    Ok(IndexBuildDecision {
        required: has_unseen || has_changed_search,
        generation,
    })
}

impl ReconciliationSnapshot {
    fn matches(&self) -> bool {
        self.expected == Some(self.seen)
    }

    fn into_error(self) -> MediaCatalogError {
        MediaCatalogError::ReconciliationMismatch {
            expected: self.expected,
            seen: self.seen,
        }
    }

    fn membership_verified(&self) -> bool {
        self.membership_verified_revision == Some(self.revision)
    }
}

fn read_reconciliation(
    transaction: &Transaction<'_>,
    run: CatalogSyncRun,
) -> Result<ReconciliationSnapshot, MediaCatalogError> {
    let (expected, revision, membership_verified_revision) = transaction.query_row(
        "SELECT total_expected,sync_revision,membership_verified_revision \
         FROM catalog_scope WHERE singleton=1",
        [],
        |row| {
            Ok((
                row.get::<_, Option<i64>>(0)?,
                row.get::<_, i64>(1)?,
                row.get::<_, Option<i64>>(2)?,
            ))
        },
    )?;
    let seen = transaction.query_row(
        "SELECT COUNT(DISTINCT item_id) FROM catalog_items WHERE seen_run_id=?1",
        [run.id],
        |row| row.get::<_, i64>(0),
    )?;
    Ok(ReconciliationSnapshot {
        expected: expected.map(i64_to_u64).transpose()?,
        seen: i64_to_u64(seen)?,
        revision,
        membership_verified_revision,
    })
}

fn mark_reconciliation_failed(
    transaction: &Transaction<'_>,
    failed_at_ms: i64,
) -> Result<(), MediaCatalogError> {
    const ERROR: &str = "catalog reconciliation failed: expected and observed item counts differ";
    transaction.execute(
        "UPDATE catalog_scope SET active_run_id=NULL,sync_state='failed',updated_at_ms=?1,\
         last_error=?2,sync_dirty=1,membership_verified_revision=NULL WHERE singleton=1",
        params![failed_at_ms, ERROR],
    )?;
    Ok(())
}

struct RawIndexFields {
    item_id: String,
    item_type: String,
    title: String,
    sort_title: String,
    original_title: Option<String>,
    series_title: Option<String>,
    season_title: Option<String>,
    aliases_json: String,
}

impl RawIndexFields {
    fn decode(row: &Row<'_>) -> rusqlite::Result<Self> {
        Ok(Self {
            item_id: row.get(0)?,
            item_type: row.get(1)?,
            title: row.get(2)?,
            sort_title: row.get(3)?,
            original_title: row.get(4)?,
            series_title: row.get(5)?,
            season_title: row.get(6)?,
            aliases_json: row.get(7)?,
        })
    }

    fn into_document(self) -> Result<IndexDocument, MediaCatalogError> {
        let sort_key = normalize_search_text(&self.sort_title);
        self.into_document_with_sort_key(sort_key)
    }

    fn into_document_with_sort_key(
        self,
        sort_key: String,
    ) -> Result<IndexDocument, MediaCatalogError> {
        let aliases = serde_json::from_str::<Vec<String>>(&self.aliases_json)?;
        Ok(prepare_index_document_with_sort_key(
            &self.item_id,
            &self.item_type,
            &self.title,
            sort_key,
            self.original_title.as_deref(),
            self.series_title.as_deref(),
            self.season_title.as_deref(),
            &aliases,
        ))
    }
}

struct PendingIndexFields {
    sort_key: String,
    fields: RawIndexFields,
}

const INDEX_PREPARE_BATCH_SIZE: usize = 256;
const MAX_INDEX_PREPARE_WORKERS: usize = 4;
const INDEX_PREPARE_WINDOW_PER_WORKER: usize = 2;

struct IndexPrepareTasks {
    pending: VecDeque<(usize, Vec<PendingIndexFields>)>,
    dispatch_limit: usize,
}

fn release_index_prepare_tasks(
    tasks: &(Mutex<IndexPrepareTasks>, Condvar),
    count: usize,
    batch_count: usize,
) {
    let mut state = tasks.0.lock().expect("index preparation queue");
    state.dispatch_limit = state.dispatch_limit.saturating_add(count).min(batch_count);
    tasks.1.notify_all();
}

#[allow(clippy::too_many_lines)]
fn populate_sidecar_builder(
    builder: &mut SidecarBuilder,
    pending: Vec<PendingIndexFields>,
    diagnostics: bool,
) -> Result<(), MediaCatalogError> {
    let batch_count = pending.len().div_ceil(INDEX_PREPARE_BATCH_SIZE);
    let worker_count = thread::available_parallelism()
        .map_or(1, usize::from)
        .min(MAX_INDEX_PREPARE_WORKERS)
        .min(batch_count.max(1));
    if diagnostics {
        eprintln!(
            "search-build-stage prepare_pipeline workers={worker_count} batches={batch_count} batch_size={INDEX_PREPARE_BATCH_SIZE}"
        );
    }
    if worker_count == 1 {
        for pending_document in pending {
            builder.add_document(
                pending_document
                    .fields
                    .into_document_with_sort_key(pending_document.sort_key)?,
            )?;
        }
        return Ok(());
    }

    let mut pending = pending.into_iter();
    let mut tasks = VecDeque::with_capacity(batch_count);
    for batch_index in 0..batch_count {
        let batch = pending
            .by_ref()
            .take(INDEX_PREPARE_BATCH_SIZE)
            .collect::<Vec<_>>();
        tasks.push_back((batch_index, batch));
    }
    let initial_dispatch = (worker_count * INDEX_PREPARE_WINDOW_PER_WORKER).min(batch_count);
    let tasks = (
        Mutex::new(IndexPrepareTasks {
            pending: tasks,
            dispatch_limit: initial_dispatch,
        }),
        Condvar::new(),
    );
    let (result_sender, result_receiver) = mpsc::sync_channel(worker_count);
    thread::scope(|scope| -> Result<(), MediaCatalogError> {
        for _ in 0..worker_count {
            let result_sender = result_sender.clone();
            let tasks = &tasks;
            scope.spawn(move || {
                loop {
                    let task = {
                        let mut state = tasks.0.lock().expect("index preparation queue");
                        loop {
                            let dispatchable =
                                state.pending.front().is_some_and(|(batch_index, _)| {
                                    *batch_index < state.dispatch_limit
                                });
                            if dispatchable || state.pending.is_empty() {
                                break state.pending.pop_front();
                            }
                            state = tasks.1.wait(state).expect("index preparation queue wait");
                        }
                    };
                    let Some((batch_index, batch)) = task else {
                        break;
                    };
                    let prepare_started = diagnostics.then(Instant::now);
                    let documents = batch
                        .into_iter()
                        .map(|pending_document| {
                            pending_document
                                .fields
                                .into_document_with_sort_key(pending_document.sort_key)
                        })
                        .collect::<Result<Vec<_>, _>>();
                    let prepare_elapsed =
                        prepare_started.map_or(Duration::ZERO, |started| started.elapsed());
                    if result_sender
                        .send((batch_index, documents, prepare_elapsed))
                        .is_err()
                    {
                        break;
                    }
                }
            });
        }
        drop(result_sender);

        let mut next_batch = 0_usize;
        let mut ready = BTreeMap::<usize, Vec<IndexDocument>>::new();
        let mut prepare_cpu = Duration::ZERO;
        let mut builder_cpu = Duration::ZERO;
        let mut ready_high_water = 0_usize;
        let mut received_batches = 0_usize;
        let mut first_error = None;
        while received_batches < batch_count {
            let Ok((batch_index, documents, prepare_elapsed)) = result_receiver.recv() else {
                first_error.get_or_insert_with(|| {
                    std::io::Error::other("index preparation worker stopped before completion")
                        .into()
                });
                release_index_prepare_tasks(&tasks, batch_count, batch_count);
                break;
            };
            received_batches += 1;
            prepare_cpu += prepare_elapsed;
            if first_error.is_some() {
                continue;
            }
            let documents = match documents {
                Ok(documents) => documents,
                Err(error) => {
                    first_error = Some(error);
                    ready.clear();
                    release_index_prepare_tasks(&tasks, batch_count, batch_count);
                    continue;
                }
            };
            ready.insert(batch_index, documents);
            ready_high_water = ready_high_water.max(ready.len());
            while let Some(documents) = ready.remove(&next_batch) {
                let builder_started = diagnostics.then(Instant::now);
                for document in documents {
                    if let Err(error) = builder.add_document(document) {
                        first_error = Some(error.into());
                        release_index_prepare_tasks(&tasks, batch_count, batch_count);
                        break;
                    }
                }
                if let Some(builder_started) = builder_started {
                    builder_cpu += builder_started.elapsed();
                }
                if first_error.is_some() {
                    ready.clear();
                    break;
                }
                next_batch += 1;
                release_index_prepare_tasks(&tasks, 1, batch_count);
            }
        }
        if diagnostics {
            eprintln!(
                "search-build-stage prepare_pipeline_detail prepare_cpu_ms={:.3} builder_cpu_ms={:.3} ready_batch_high_water={ready_high_water}",
                prepare_cpu.as_secs_f64() * 1_000.0,
                builder_cpu.as_secs_f64() * 1_000.0
            );
        }
        first_error.map_or(Ok(()), Err)
    })
}

const RAW_INDEX_COLUMNS: &str = r"
    item_id,item_type,title,sort_title,original_title,series_title,season_title,aliases_json
";

fn open_or_rebuild_index(
    database_path: &Path,
    generation: i64,
    scope_fingerprint: u64,
) -> Result<PersistentIndex, MediaCatalogError> {
    let path = generation_path(database_path, generation);
    match PersistentIndex::open(&path, generation, scope_fingerprint) {
        Ok(index) => Ok(index),
        Err(_) => build_persistent_index(database_path, generation, None, scope_fingerprint),
    }
}

fn build_persistent_index(
    database_path: &Path,
    generation: i64,
    seen_run_id: Option<i64>,
    scope_fingerprint: u64,
) -> Result<PersistentIndex, MediaCatalogError> {
    let total_started = Instant::now();
    let diagnostics = std::env::var_os("YANAMI_SEARCH_BUILD_DIAGNOSTICS").is_some();
    let mut connection = open_reader(database_path)?;
    let transaction = connection.transaction()?;
    let read_started = Instant::now();
    let mut pending = Vec::new();
    {
        let (sql, parameter) = if let Some(run_id) = seen_run_id {
            (
                format!("SELECT {RAW_INDEX_COLUMNS} FROM catalog_items WHERE seen_run_id=?1"),
                Some(run_id),
            )
        } else {
            (
                format!("SELECT {RAW_INDEX_COLUMNS} FROM catalog_items"),
                None,
            )
        };
        let mut statement = transaction.prepare(&sql)?;
        let mut rows = match parameter {
            Some(run_id) => statement.query([run_id])?,
            None => statement.query([])?,
        };
        while let Some(row) = rows.next()? {
            let fields = RawIndexFields::decode(row)?;
            pending.push(PendingIndexFields {
                sort_key: normalize_search_text(&fields.sort_title),
                fields,
            });
        }
    }
    if diagnostics {
        eprintln!(
            "search-build-stage read_sort_keys documents={} elapsed_ms={:.3}",
            pending.len(),
            read_started.elapsed().as_secs_f64() * 1_000.0
        );
    }
    let sort_started = Instant::now();
    pending.sort_unstable_by(|left, right| {
        (&left.sort_key, &left.fields.item_id).cmp(&(&right.sort_key, &right.fields.item_id))
    });
    if diagnostics {
        eprintln!(
            "search-build-stage sort_documents elapsed_ms={:.3}",
            sort_started.elapsed().as_secs_f64() * 1_000.0
        );
    }

    let mut builder = SidecarBuilder::new(database_path, generation, scope_fingerprint)?;
    let add_started = Instant::now();
    populate_sidecar_builder(&mut builder, pending, diagnostics)?;
    if diagnostics {
        eprintln!(
            "search-build-stage add_documents elapsed_ms={:.3}",
            add_started.elapsed().as_secs_f64() * 1_000.0
        );
    }
    transaction.commit()?;
    let finish_started = Instant::now();
    let index = builder.finish()?;
    if diagnostics {
        eprintln!(
            "search-build-stage finish_sidecar elapsed_ms={:.3} total_ms={:.3}",
            finish_started.elapsed().as_secs_f64() * 1_000.0,
            total_started.elapsed().as_secs_f64() * 1_000.0
        );
    }
    Ok(index)
}

fn load_delta_documents(
    database_path: &Path,
    base_generation: Option<i64>,
) -> Result<Vec<IndexDocument>, MediaCatalogError> {
    let connection = open_reader(database_path)?;
    let (sql, generation) = if let Some(generation) = base_generation {
        (
            format!(
                "SELECT {RAW_INDEX_COLUMNS} FROM catalog_items \
                 WHERE search_changed_run_id>?1 ORDER BY row_id"
            ),
            Some(generation),
        )
    } else {
        (
            format!("SELECT {RAW_INDEX_COLUMNS} FROM catalog_items ORDER BY row_id"),
            None,
        )
    };
    let mut statement = connection.prepare(&sql)?;
    let mut rows = match generation {
        Some(generation) => statement.query([generation])?,
        None => statement.query([])?,
    };
    let mut documents = Vec::new();
    while let Some(row) = rows.next()? {
        documents.push(RawIndexFields::decode(row)?.into_document()?);
    }
    Ok(documents)
}

fn exact_database_suffix(path: &Path, suffix: &str) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(suffix);
    PathBuf::from(value)
}

fn ensure_active_run(
    transaction: &Transaction<'_>,
    run: CatalogSyncRun,
) -> Result<(), MediaCatalogError> {
    let (active, state) = transaction.query_row(
        "SELECT active_run_id,sync_state FROM catalog_scope WHERE singleton=1",
        [],
        |row| Ok((row.get::<_, Option<i64>>(0)?, row.get::<_, String>(1)?)),
    )?;
    if active != Some(run.id) || state != "running" {
        return Err(MediaCatalogError::StaleSyncRun {
            active,
            actual: run.id,
        });
    }
    Ok(())
}

fn load_membership(
    connection: &Connection,
    run: Option<CatalogSyncRun>,
) -> Result<HashSet<String>, MediaCatalogError> {
    let mut statement = match run {
        Some(_) => connection.prepare("SELECT item_id FROM catalog_items WHERE seen_run_id=?1")?,
        None => connection.prepare("SELECT item_id FROM catalog_items")?,
    };
    let mut rows = match run {
        Some(run) => statement.query([run.id])?,
        None => statement.query([])?,
    };
    let mut item_ids = HashSet::new();
    while let Some(row) = rows.next()? {
        item_ids.insert(row.get::<_, String>(0)?);
    }
    Ok(item_ids)
}

fn read_sync_status(connection: &Connection) -> Result<SyncStatus, MediaCatalogError> {
    connection
        .query_row(
            "SELECT sync_state,active_run_id,last_completed_run_id,next_start_index,cached_count,\
             total_expected,started_at_ms,updated_at_ms,last_completed_at_ms,last_error,sync_dirty,\
             content_revision \
             FROM catalog_scope WHERE singleton=1",
            [],
            |row| {
                Ok(SyncStatus {
                    state: SyncState::from_database(&row.get::<_, String>(0)?)?,
                    active_run_id: row.get(1)?,
                    last_completed_run_id: row.get(2)?,
                    next_start_index: i64_to_u64(row.get(3)?)?,
                    cached_count: i64_to_u64(row.get(4)?)?,
                    total_expected: row.get::<_, Option<i64>>(5)?.map(i64_to_u64).transpose()?,
                    started_at_ms: row.get(6)?,
                    updated_at_ms: row.get(7)?,
                    last_completed_at_ms: row.get(8)?,
                    last_error: row.get(9)?,
                    dirty: row.get(10)?,
                    content_revision: i64_to_u64(row.get(11)?)?,
                })
            },
        )
        .map_err(MediaCatalogError::from)
}

fn load_search_hits(
    connection: &Connection,
    hits: &[IndexHit],
) -> Result<Vec<CatalogSearchHit>, MediaCatalogError> {
    let sql = format!(
        "SELECT {SEARCH_COLUMNS},?2 AS match_rank \
         FROM catalog_items i \
         LEFT JOIN catalog_items series \
           ON series.item_id=i.series_id AND series.item_type='Series' \
         LEFT JOIN catalog_user_state us ON us.item_id=i.item_id \
         WHERE i.item_id=?1"
    );
    let mut statement = connection.prepare_cached(&sql)?;
    let mut loaded = Vec::with_capacity(hits.len());
    for hit in hits {
        let mut rows = statement.query(params![hit.item_id, i64::from(hit.rank)])?;
        let row = rows.next()?.ok_or(rusqlite::Error::QueryReturnedNoRows)?;
        loaded.push(decode_search_hit(row)?);
    }
    Ok(loaded)
}

fn decode_search_hit(row: &Row<'_>) -> Result<CatalogSearchHit, MediaCatalogError> {
    let aliases_json: String = row.get(18)?;
    let item_id: String = row.get(0)?;
    let has_user_state: bool = row.get::<_, Option<i64>>(19)?.is_some();
    let user_state = if has_user_state {
        Some(CatalogUserState {
            item_id: item_id.clone(),
            favorite: row.get(19)?,
            played: row.get(20)?,
            resume_ticks: i64_to_u64(row.get(21)?)?,
            progress: row.get(22)?,
            unplayed_count: row.get::<_, Option<i64>>(23)?.map(i64_to_u32).transpose()?,
            last_played_at: row.get(24)?,
        })
    } else {
        None
    };
    Ok(CatalogSearchHit {
        item: CatalogItem {
            id: item_id,
            item_type: row.get(1)?,
            title: row.get(2)?,
            sort_title: row.get(3)?,
            original_title: row.get(4)?,
            parent_id: row.get(5)?,
            series_id: row.get(6)?,
            series_title: row.get(7)?,
            season_id: row.get(8)?,
            season_title: row.get(9)?,
            season_number: row.get(10)?,
            episode_number: row.get(11)?,
            production_year: row.get(12)?,
            source_updated_at: row.get(13)?,
            image_tag: row.get(14)?,
            primary_image_aspect_ratio: row.get(15)?,
            series_image_tag: row.get(16)?,
            series_primary_image_aspect_ratio: row.get(17)?,
            aliases: serde_json::from_str(&aliases_json)?,
        },
        user_state,
        match_rank: u32::try_from(row.get::<_, i64>(25)?)
            .map_err(|_| MediaCatalogError::NumericRange)?,
    })
}

const SEARCH_COLUMNS: &str = r"
    i.item_id,i.item_type,i.title,i.sort_title,i.original_title,i.parent_id,
    i.series_id,COALESCE(series.title,i.series_title),i.season_id,i.season_title,i.season_number,
    i.episode_number,i.production_year,i.source_updated_at,i.image_tag,
    i.primary_image_aspect_ratio,series.image_tag,series.primary_image_aspect_ratio,i.aliases_json,
    us.favorite,us.played,us.resume_ticks,us.progress,us.unplayed_count,us.last_played_at
";

fn search_category(item_type: &str) -> u8 {
    match item_type {
        "Movie" | "Series" => CATEGORY_TITLE,
        "Episode" => CATEGORY_EPISODE,
        _ => CATEGORY_OTHER,
    }
}

fn normalize_optional(value: Option<&str>) -> String {
    value.map_or_else(String::new, normalize_search_text)
}

fn normalize_search_text(value: &str) -> String {
    let mut result = String::with_capacity(value.len());
    let mut pending_space = false;
    for character in value.nfkc().case_fold().nfkc() {
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

fn pinyin_forms(values: &[&str]) -> (Vec<String>, Vec<String>) {
    let mut full_documents = Vec::new();
    let mut initial_documents = Vec::new();
    for value in values {
        let mut compact = String::new();
        let mut spaced = String::new();
        let mut initials = String::new();
        let mut literal_run = String::new();
        let mut has_pinyin = false;
        for character in value.chars() {
            if let Some(pinyin) = character.to_pinyin() {
                if !literal_run.is_empty() {
                    append_pinyin_token(&mut spaced, &literal_run);
                    literal_run.clear();
                }
                has_pinyin = true;
                compact.push_str(pinyin.plain());
                append_pinyin_token(&mut spaced, pinyin.plain());
                initials.push_str(pinyin.first_letter());
            } else if character.is_alphanumeric() {
                compact.push(character);
                literal_run.push(character);
                initials.push(character);
            } else if !literal_run.is_empty() {
                append_pinyin_token(&mut spaced, &literal_run);
                literal_run.clear();
            }
        }
        if !literal_run.is_empty() {
            append_pinyin_token(&mut spaced, &literal_run);
        }
        // ASCII-only sources are already present in the normalized document.
        // Transliteration fields only need entries that actually transliterate
        // at least one Han codepoint.
        if has_pinyin && !compact.is_empty() {
            full_documents.push(compact);
            full_documents.push(spaced);
            initial_documents.push(initials);
        }
    }
    (full_documents, initial_documents)
}

fn append_pinyin_token(output: &mut String, token: &str) {
    if !output.is_empty() {
        output.push(' ');
    }
    output.push_str(token);
}

fn validate_user_state(state: &CatalogUserState) -> Result<(), MediaCatalogError> {
    if state.item_id.trim().is_empty() || state.progress.is_some_and(|value| !value.is_finite()) {
        return Err(MediaCatalogError::InvalidItem);
    }
    Ok(())
}

fn u64_to_i64(value: u64) -> Result<i64, MediaCatalogError> {
    i64::try_from(value).map_err(|_| MediaCatalogError::NumericRange)
}

fn optional_u64_to_i64(value: Option<u64>) -> Result<Option<i64>, MediaCatalogError> {
    value.map(u64_to_i64).transpose()
}

fn i64_to_u64(value: i64) -> Result<u64, rusqlite::Error> {
    u64::try_from(value).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(
            0,
            rusqlite::types::Type::Integer,
            Box::new(error),
        )
    })
}

fn i64_to_u32(value: i64) -> Result<u32, rusqlite::Error> {
    u32::try_from(value).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(
            0,
            rusqlite::types::Type::Integer,
            Box::new(error),
        )
    })
}

#[cfg(test)]
mod pinyin_form_tests {
    use super::pinyin_forms;

    #[test]
    fn mixed_han_literal_runs_and_sources_preserve_golden_order() {
        let (full, initials) = pinyin_forms(&["星ab-河12", "路 x"]);
        assert_eq!(full, ["xingabhe12", "xing ab he 12", "lux", "lu x"]);
        assert_eq!(initials, ["xabh12", "lx"]);
    }
}
