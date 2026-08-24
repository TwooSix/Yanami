use std::{
    array,
    collections::HashMap,
    fs::{self, File},
    hash::{BuildHasherDefault, Hash, Hasher},
    io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write},
    mem::size_of,
    path::{Path, PathBuf},
    sync::Mutex,
    time::{Instant, SystemTime, UNIX_EPOCH},
};

const MAGIC: &[u8; 8] = b"YMCIDX06";
const CHECKSUM_OFFSET: usize = 44;
const HEADER_LEN: usize = CHECKSUM_OFFSET + size_of::<u64>();
const DIRECTORY_ENTRY_LEN: usize = 22;
const ID_LOOKUP_ENTRY_LEN: usize = 12;
const GROUP_COUNT: usize = 6;
const DICTIONARY_BOUNDARY_COUNT: usize = GROUP_COUNT + 1;
const DICTIONARY_HEADER_LEN: usize = DICTIONARY_BOUNDARY_COUNT * size_of::<u32>();
const DICTIONARY_BLOCK_SIZE: usize = 32;
const RANK_COUNT: usize = 13;
const SHORT_SCALAR_LIMIT: usize = 3;
const MAX_GRAMS_PER_GROUP: usize = 3;
const NO_RANK: u8 = u8::MAX;
pub(super) const CATEGORY_TITLE: u8 = 0;
pub(super) const CATEGORY_EPISODE: u8 = 1;
pub(super) const CATEGORY_OTHER: u8 = 2;
const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0100_0000_01b3;

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

#[derive(Clone)]
pub(super) struct IndexDocument {
    pub item_id: String,
    pub category: u8,
    pub sort_key: String,
    pub title: String,
    pub original_title: String,
    pub series_title: String,
    pub season_title: String,
    pub aliases: Vec<String>,
    pub pinyin_full: Vec<String>,
    pub pinyin_initials: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct IndexHit {
    pub item_id: String,
    pub rank: u8,
}

pub(super) struct IndexQueryResult {
    pub hits: Vec<IndexHit>,
    pub total_matches: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct PostingKey {
    kind: u8,
    group: u8,
    key: u64,
}

impl Hash for PostingKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let tag = (u64::from(self.kind) << 8) | u64::from(self.group);
        state.write_u64(self.key ^ tag.wrapping_mul(0x9e37_79b1_85eb_ca87));
    }
}

#[derive(Default)]
struct PostingKeyHasher(u64);

impl Hasher for PostingKeyHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        let mut value = FNV_OFFSET_BASIS;
        for byte in bytes {
            value = (value ^ u64::from(*byte)).wrapping_mul(FNV_PRIME);
        }
        self.0 = avalanche(value);
    }

    fn write_u64(&mut self, value: u64) {
        self.0 = avalanche(value);
    }
}

type PostingMap<V> = HashMap<PostingKey, V, BuildHasherDefault<PostingKeyHasher>>;

fn new_posting_map<V>() -> PostingMap<V> {
    HashMap::with_hasher(BuildHasherDefault::default())
}

fn avalanche(mut value: u64) -> u64 {
    value ^= value >> 30;
    value = value.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value ^= value >> 27;
    value = value.wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

#[derive(Clone, Default)]
struct EncodedPosting {
    bytes: Vec<u8>,
    count: u32,
    last: Option<u32>,
}

impl EncodedPosting {
    fn append(&mut self, ordinal: u32) -> io::Result<()> {
        if self.last == Some(ordinal) {
            return Ok(());
        }
        let delta = self
            .last
            .map_or(ordinal, |last| ordinal.saturating_sub(last));
        if self.last.is_some_and(|last| ordinal <= last) {
            return Err(invalid_data("posting ordinals are not strictly increasing"));
        }
        write_var_u32_vec(&mut self.bytes, delta);
        self.count = self
            .count
            .checked_add(1)
            .ok_or_else(|| invalid_data("posting count exceeds u32"))?;
        self.last = Some(ordinal);
        Ok(())
    }
}

#[derive(Clone, Copy)]
struct PostingMeta {
    data_offset: u32,
    byte_len: u32,
    count: u32,
}

struct ChecksumWriter<W> {
    inner: W,
    checksum: ChecksumState,
}

impl<W> ChecksumWriter<W> {
    fn new(inner: W, checksum: ChecksumState) -> Self {
        Self { inner, checksum }
    }

    fn checksum(&self) -> u64 {
        self.checksum.finish()
    }

    fn into_inner(self) -> W {
        self.inner
    }
}

impl<W: Write> Write for ChecksumWriter<W> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let written = self.inner.write(bytes)?;
        self.checksum.update(&bytes[..written]);
        Ok(written)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

#[derive(Clone)]
struct ChecksumState {
    state: u64,
    total_len: u64,
    tail: [u8; size_of::<u64>()],
    tail_len: usize,
}

impl ChecksumState {
    fn new() -> Self {
        Self {
            state: 0x9e37_79b1_85eb_ca87,
            total_len: 0,
            tail: [0; size_of::<u64>()],
            tail_len: 0,
        }
    }

    fn update(&mut self, mut bytes: &[u8]) {
        self.total_len = self.total_len.wrapping_add(bytes.len() as u64);
        if self.tail_len != 0 {
            let copied = (size_of::<u64>() - self.tail_len).min(bytes.len());
            self.tail[self.tail_len..self.tail_len + copied].copy_from_slice(&bytes[..copied]);
            self.tail_len += copied;
            bytes = &bytes[copied..];
            if self.tail_len < size_of::<u64>() {
                return;
            }
            self.mix(u64::from_le_bytes(self.tail));
            self.tail = [0; size_of::<u64>()];
            self.tail_len = 0;
        }
        let mut chunks = bytes.chunks_exact(size_of::<u64>());
        for chunk in &mut chunks {
            self.mix(u64::from_le_bytes(chunk.try_into().expect("u64 chunk")));
        }
        let remainder = chunks.remainder();
        self.tail[..remainder.len()].copy_from_slice(remainder);
        self.tail_len = remainder.len();
    }

    fn finish(&self) -> u64 {
        let mut finished = self.clone();
        let tail_tag = (finished.tail_len as u64) << 56;
        finished.mix(u64::from_le_bytes(finished.tail) ^ tail_tag);
        let mut value = finished.state ^ finished.total_len.wrapping_mul(0x9e37_79b1_85eb_ca87);
        value ^= value >> 33;
        value = value.wrapping_mul(0xff51_afd7_ed55_8ccd);
        value ^= value >> 33;
        value = value.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
        value ^ (value >> 33)
    }

    fn mix(&mut self, word: u64) {
        self.state ^= word.wrapping_mul(0xd6e8_feb8_6659_fd93);
        self.state = self
            .state
            .rotate_left(27)
            .wrapping_mul(0x9e37_79b1_85eb_ca87)
            .wrapping_add(0xa076_1d64_78bd_642f);
    }
}

#[derive(Default)]
struct StringInterner {
    by_value: HashMap<String, u32>,
    next_id: u32,
}

impl StringInterner {
    fn intern(&mut self, value: String) -> io::Result<u32> {
        if let Some(id) = self.by_value.get(&value) {
            return Ok(*id);
        }
        let id = self.next_id;
        self.next_id = self
            .next_id
            .checked_add(1)
            .ok_or_else(|| invalid_data("dictionary exceeds u32"))?;
        self.by_value.insert(value, id);
        Ok(id)
    }

    fn into_sorted(self) -> io::Result<SortedDictionary> {
        let mut entries = self.by_value.into_iter().collect::<Vec<_>>();
        entries.sort_unstable_by(|left, right| left.0.cmp(&right.0));
        let mut remap = vec![0_u32; entries.len()];
        let mut values = Vec::with_capacity(entries.len());
        for (new_id, (value, old_id)) in entries.into_iter().enumerate() {
            let new_id =
                u32::try_from(new_id).map_err(|_| invalid_data("dictionary exceeds u32"))?;
            *remap
                .get_mut(old_id as usize)
                .ok_or_else(|| invalid_data("dictionary id is out of bounds"))? = new_id;
            values.push(value);
        }
        Ok(SortedDictionary { values, remap })
    }
}

struct SortedDictionary {
    values: Vec<String>,
    remap: Vec<u32>,
}

pub(super) struct SidecarBuilder {
    generation: i64,
    scope_fingerprint: u64,
    final_path: PathBuf,
    sidecar_temp_path: PathBuf,
    document_temp_path: PathBuf,
    compact_document_temp_path: PathBuf,
    document_writer: Option<BufWriter<File>>,
    document_offsets: Vec<u32>,
    id_lookup: Vec<(u64, u32)>,
    postings: PostingMap<EncodedPosting>,
    interners: [StringInterner; GROUP_COUNT],
    key_scratch: Vec<PostingKey>,
    record_scratch: Vec<u8>,
}

impl SidecarBuilder {
    pub fn new(database_path: &Path, generation: i64, scope_fingerprint: u64) -> io::Result<Self> {
        if generation < 0 {
            return Err(invalid_data("sidecar generation must not be negative"));
        }
        let final_path = generation_path(database_path, generation);
        let nonce = format!("{}-{}", std::process::id(), now_nanos());
        let sidecar_temp_path = exact_suffix_path(&final_path, &format!(".tmp-{nonce}"));
        let document_temp_path = exact_suffix_path(&final_path, &format!(".refs-{nonce}"));
        let compact_document_temp_path = exact_suffix_path(&final_path, &format!(".docs-{nonce}"));
        let document_writer = BufWriter::new(File::create(&document_temp_path)?);
        Ok(Self {
            generation,
            scope_fingerprint,
            final_path,
            sidecar_temp_path,
            document_temp_path,
            compact_document_temp_path,
            document_writer: Some(document_writer),
            document_offsets: vec![0],
            id_lookup: Vec::new(),
            postings: new_posting_map(),
            interners: array::from_fn(|_| StringInterner::default()),
            key_scratch: Vec::with_capacity(512),
            record_scratch: Vec::with_capacity(64),
        })
    }

    pub fn add_document(&mut self, document: IndexDocument) -> io::Result<()> {
        let ordinal = u32::try_from(self.id_lookup.len())
            .map_err(|_| invalid_data("document count exceeds u32"))?;
        append_document_postings(
            &mut self.postings,
            &document,
            ordinal,
            &mut self.key_scratch,
        )?;

        let IndexDocument {
            item_id,
            category,
            sort_key: _,
            title,
            original_title,
            series_title,
            season_title,
            aliases,
            pinyin_full,
            pinyin_initials,
        } = document;
        if item_id.is_empty() {
            return Err(invalid_data("document id is empty"));
        }
        let title = self.interners[GROUP_TITLE as usize].intern(title)?;
        let original = self.interners[GROUP_ORIGINAL as usize].intern(original_title)?;
        let mut record = std::mem::take(&mut self.record_scratch);
        record.clear();
        write_var_u32_vec(
            &mut record,
            u32::try_from(item_id.len()).map_err(|_| invalid_data("document id exceeds u32"))?,
        );
        record.extend_from_slice(item_id.as_bytes());
        if category > CATEGORY_OTHER {
            return Err(invalid_data("document category is invalid"));
        }
        record.push(category);
        write_var_u32_vec(&mut record, title);
        write_var_u32_vec(&mut record, original);
        intern_values_into_record(
            &mut record,
            &mut self.interners[GROUP_ALIASES as usize],
            aliases.len(),
            aliases,
        )?;
        let series_season_count =
            usize::from(!series_title.is_empty()) + usize::from(!season_title.is_empty());
        intern_values_into_record(
            &mut record,
            &mut self.interners[GROUP_SERIES_SEASON as usize],
            series_season_count,
            [series_title, season_title]
                .into_iter()
                .filter(|value| !value.is_empty()),
        )?;
        intern_values_into_record(
            &mut record,
            &mut self.interners[GROUP_PINYIN_FULL as usize],
            pinyin_full.len(),
            pinyin_full,
        )?;
        intern_values_into_record(
            &mut record,
            &mut self.interners[GROUP_PINYIN_INITIALS as usize],
            pinyin_initials.len(),
            pinyin_initials,
        )?;
        let next_offset = self
            .document_offsets
            .last()
            .copied()
            .unwrap_or_default()
            .checked_add(
                u32::try_from(record.len())
                    .map_err(|_| invalid_data("document record exceeds u32"))?,
            )
            .ok_or_else(|| invalid_data("document pool exceeds u32"))?;
        self.document_writer
            .as_mut()
            .ok_or_else(|| invalid_data("sidecar builder is already finished"))?
            .write_all(&record)?;
        self.record_scratch = record;
        self.document_offsets.push(next_offset);
        self.id_lookup
            .push((stable_hash(item_id.as_bytes()), ordinal));
        Ok(())
    }

    pub fn finish(mut self) -> io::Result<PersistentIndex> {
        self.write_sidecar()?;
        PersistentIndex::open(&self.final_path, self.generation, self.scope_fingerprint)
    }

    #[allow(clippy::too_many_lines)]
    fn write_sidecar(&mut self) -> io::Result<()> {
        let diagnostics = std::env::var_os("YANAMI_SEARCH_BUILD_DIAGNOSTICS").is_some();
        let total_started = Instant::now();
        let mut document_writer = self
            .document_writer
            .take()
            .ok_or_else(|| invalid_data("sidecar builder is already finished"))?;
        document_writer.flush()?;
        drop(document_writer);

        let dictionaries_started = Instant::now();
        let [
            title,
            original,
            aliases,
            series_season,
            pinyin_full,
            pinyin_initials,
        ] = std::mem::take(&mut self.interners);
        let dictionaries = [
            title.into_sorted()?,
            original.into_sorted()?,
            aliases.into_sorted()?,
            series_season.into_sorted()?,
            pinyin_full.into_sorted()?,
            pinyin_initials.into_sorted()?,
        ];
        if diagnostics {
            eprintln!(
                "search-build-stage dictionaries_sort elapsed_ms={:.3}",
                dictionaries_started.elapsed().as_secs_f64() * 1_000.0
            );
        }
        let rewrite_started = Instant::now();
        self.document_offsets = rewrite_document_pool(
            &self.document_temp_path,
            &self.compact_document_temp_path,
            &self.document_offsets,
            &dictionaries,
        )?;
        if diagnostics {
            eprintln!(
                "search-build-stage rewrite_document_pool elapsed_ms={:.3}",
                rewrite_started.elapsed().as_secs_f64() * 1_000.0
            );
        }
        let document_count = u32::try_from(self.id_lookup.len())
            .map_err(|_| invalid_data("document count exceeds u32"))?;
        let document_pool_len = self.document_offsets.last().copied().unwrap_or_default();
        let dictionary_pool_len = dictionary_pool_encoded_len(&dictionaries)?;
        let finalize_started = Instant::now();
        self.id_lookup.sort_unstable();
        let mut postings = std::mem::take(&mut self.postings)
            .into_iter()
            .collect::<Vec<_>>();
        postings.sort_unstable_by_key(|(key, _)| *key);
        let entry_count = u32::try_from(postings.len())
            .map_err(|_| invalid_data("posting entry count exceeds u32"))?;
        let posting_blob_len = postings.iter().try_fold(0_u32, |total, (_, posting)| {
            total
                .checked_add(
                    u32::try_from(posting.bytes.len())
                        .map_err(|_| invalid_data("posting byte length exceeds u32"))?,
                )
                .ok_or_else(|| invalid_data("posting blob exceeds u32"))
        })?;
        if diagnostics {
            eprintln!(
                "search-build-stage finalize_tables entries={} posting_bytes={} elapsed_ms={:.3}",
                postings.len(),
                posting_blob_len,
                finalize_started.elapsed().as_secs_f64() * 1_000.0
            );
        }

        let mut header = Vec::with_capacity(HEADER_LEN);
        header.extend_from_slice(MAGIC);
        write_i64(&mut header, self.generation)?;
        write_u64(&mut header, self.scope_fingerprint)?;
        write_u32(&mut header, document_count)?;
        write_u32(&mut header, document_pool_len)?;
        write_u32(&mut header, dictionary_pool_len)?;
        write_u32(&mut header, entry_count)?;
        write_u32(&mut header, posting_blob_len)?;
        write_u64(&mut header, 0)?;
        if header.len() != HEADER_LEN {
            return Err(invalid_data("sidecar header length is invalid"));
        }

        let write_started = Instant::now();
        let mut buffered = BufWriter::new(File::create(&self.sidecar_temp_path)?);
        buffered.write_all(&header)?;
        let mut checksum = ChecksumState::new();
        checksum.update(&header[..CHECKSUM_OFFSET]);
        let mut writer = ChecksumWriter::new(buffered, checksum);
        for offset in &self.document_offsets {
            write_u32(&mut writer, *offset)?;
        }
        let mut document_reader = BufReader::new(File::open(&self.compact_document_temp_path)?);
        io::copy(&mut document_reader, &mut writer)?;
        for (hash, ordinal) in &self.id_lookup {
            write_u64(&mut writer, *hash)?;
            write_u32(&mut writer, *ordinal)?;
        }
        write_dictionary_pool(&mut writer, &dictionaries)?;
        let mut data_offset = 0_u32;
        for (key, posting) in &postings {
            writer.write_all(&[key.kind, key.group])?;
            write_u64(&mut writer, key.key)?;
            write_u32(&mut writer, data_offset)?;
            let byte_len = u32::try_from(posting.bytes.len())
                .map_err(|_| invalid_data("posting byte length exceeds u32"))?;
            write_u32(&mut writer, byte_len)?;
            write_u32(&mut writer, posting.count)?;
            data_offset = data_offset
                .checked_add(byte_len)
                .ok_or_else(|| invalid_data("posting data offset overflow"))?;
        }
        for (_, posting) in &postings {
            writer.write_all(&posting.bytes)?;
        }
        writer.flush()?;
        let checksum = writer.checksum();
        let mut buffered = writer.into_inner();
        buffered.flush()?;
        drop(buffered);
        let mut file = File::options().write(true).open(&self.sidecar_temp_path)?;
        file.seek(SeekFrom::Start(CHECKSUM_OFFSET as u64))?;
        file.write_all(&checksum.to_le_bytes())?;
        file.sync_all()?;
        drop(file);
        match fs::remove_file(&self.final_path) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
        fs::rename(&self.sidecar_temp_path, &self.final_path)?;
        if diagnostics {
            eprintln!(
                "search-build-stage write_fsync bytes={} elapsed_ms={:.3} write_sidecar_total_ms={:.3}",
                HEADER_LEN as u64
                    + self.document_offsets.len() as u64 * size_of::<u32>() as u64
                    + u64::from(document_pool_len)
                    + self.id_lookup.len() as u64 * ID_LOOKUP_ENTRY_LEN as u64
                    + u64::from(dictionary_pool_len)
                    + postings.len() as u64 * DIRECTORY_ENTRY_LEN as u64
                    + u64::from(posting_blob_len),
                write_started.elapsed().as_secs_f64() * 1_000.0,
                total_started.elapsed().as_secs_f64() * 1_000.0
            );
        }
        Ok(())
    }
}

impl Drop for SidecarBuilder {
    fn drop(&mut self) {
        // Close the only long-lived handle before removing exact, builder-owned
        // scratch paths. The published sidecar and SQLite facts are deliberately
        // outside this cleanup boundary.
        self.document_writer.take();
        for path in [
            &self.document_temp_path,
            &self.compact_document_temp_path,
            &self.sidecar_temp_path,
        ] {
            let _ = fs::remove_file(path);
        }
    }
}

impl IndexDocument {
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
            _ => unreachable!("known posting group"),
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
            _ => unreachable!("known posting group"),
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
            _ => unreachable!("known posting group"),
        }
    }

    fn match_group(&self, group: u8, query: &str) -> Option<u8> {
        match group {
            GROUP_TITLE => rank_scalar(&self.title, query, 0, 3, 6),
            GROUP_ORIGINAL => (!self.original_title.is_empty())
                .then(|| rank_scalar(&self.original_title, query, 1, 4, 7))?,
            GROUP_ALIASES => {
                if self.aliases.iter().any(|value| value == query) {
                    Some(2)
                } else if self.aliases.iter().any(|value| value.contains(query)) {
                    Some(7)
                } else {
                    None
                }
            }
            GROUP_SERIES_SEASON => {
                if self.series_title.starts_with(query) || self.season_title.starts_with(query) {
                    Some(5)
                } else if self.series_title.contains(query) || self.season_title.contains(query) {
                    Some(8)
                } else {
                    None
                }
            }
            GROUP_PINYIN_FULL => {
                if self
                    .pinyin_full
                    .first()
                    .is_some_and(|value| value.starts_with(query))
                {
                    Some(9)
                } else if self.pinyin_full.iter().any(|value| value.contains(query)) {
                    Some(11)
                } else {
                    None
                }
            }
            GROUP_PINYIN_INITIALS => {
                if self
                    .pinyin_initials
                    .first()
                    .is_some_and(|value| value.starts_with(query))
                {
                    Some(10)
                } else if self
                    .pinyin_initials
                    .iter()
                    .any(|value| value.contains(query))
                {
                    Some(12)
                } else {
                    None
                }
            }
            _ => None,
        }
    }
}

pub(super) struct PersistentIndex {
    blob: Vec<u8>,
    document_count: u32,
    document_offsets_at: usize,
    document_pool_at: usize,
    id_lookup_at: usize,
    dictionary_pool_at: usize,
    dictionary_pool_len: u32,
    directory_at: usize,
    entry_count: u32,
    postings_at: usize,
    scratch_pool: Mutex<Vec<SearchScratch>>,
}

impl PersistentIndex {
    #[allow(clippy::too_many_lines)]
    pub fn open(
        path: &Path,
        expected_generation: i64,
        expected_scope_fingerprint: u64,
    ) -> io::Result<Self> {
        let blob = fs::read(path)?;
        if blob.len() < HEADER_LEN || &blob[..MAGIC.len()] != MAGIC {
            return Err(invalid_data("sidecar magic/version mismatch"));
        }
        let generation = read_i64_at(&blob, 8)?;
        if generation != expected_generation {
            return Err(invalid_data("sidecar generation mismatch"));
        }
        if read_u64_at(&blob, 16)? != expected_scope_fingerprint {
            return Err(invalid_data("sidecar catalog scope mismatch"));
        }
        let expected_checksum = read_u64_at(&blob, CHECKSUM_OFFSET)?;
        let mut checksum = ChecksumState::new();
        checksum.update(&blob[..CHECKSUM_OFFSET]);
        checksum.update(&blob[HEADER_LEN..]);
        let actual_checksum = checksum.finish();
        if actual_checksum != expected_checksum {
            return Err(invalid_data("sidecar checksum mismatch"));
        }
        let document_count = read_u32_at(&blob, 24)?;
        let document_pool_len = read_u32_at(&blob, 28)?;
        let dictionary_pool_len = read_u32_at(&blob, 32)?;
        let entry_count = read_u32_at(&blob, 36)?;
        let posting_blob_len = read_u32_at(&blob, 40)?;
        let document_offsets_at = HEADER_LEN;
        let document_offsets_len = (document_count as usize + 1)
            .checked_mul(size_of::<u32>())
            .ok_or_else(|| invalid_data("document offset table overflow"))?;
        let document_pool_at = document_offsets_at
            .checked_add(document_offsets_len)
            .ok_or_else(|| invalid_data("document pool offset overflow"))?;
        let id_lookup_at = document_pool_at
            .checked_add(document_pool_len as usize)
            .ok_or_else(|| invalid_data("id lookup offset overflow"))?;
        let id_lookup_len = (document_count as usize)
            .checked_mul(ID_LOOKUP_ENTRY_LEN)
            .ok_or_else(|| invalid_data("id lookup length overflow"))?;
        let dictionary_pool_at = id_lookup_at
            .checked_add(id_lookup_len)
            .ok_or_else(|| invalid_data("dictionary pool offset overflow"))?;
        let directory_at = dictionary_pool_at
            .checked_add(dictionary_pool_len as usize)
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
            return Err(invalid_data(format!(
                "sidecar section lengths do not match file: expected {expected_len}, actual {}",
                blob.len()
            )));
        }
        let index = Self {
            blob,
            document_count,
            document_offsets_at,
            document_pool_at,
            id_lookup_at,
            dictionary_pool_at,
            dictionary_pool_len,
            directory_at,
            entry_count,
            postings_at,
            scratch_pool: Mutex::new(Vec::new()),
        };
        index.validate_dictionaries()?;
        index.validate_documents(document_pool_len)?;
        index.validate_id_lookup()?;
        index.validate_postings(posting_blob_len)?;
        Ok(index)
    }

    pub fn document_count(&self) -> usize {
        self.document_count as usize
    }

    pub fn find_ordinal(&self, item_id: &str) -> Option<u32> {
        let target = stable_hash(item_id.as_bytes());
        let mut low = 0_u32;
        let mut high = self.document_count;
        while low < high {
            let middle = low + (high - low) / 2;
            let (hash, _) = self.id_lookup_entry(middle).ok()?;
            if hash < target {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        let mut index = low;
        while index < self.document_count {
            let (hash, ordinal) = self.id_lookup_entry(index).ok()?;
            if hash != target {
                break;
            }
            if self.document_id(ordinal) == item_id {
                return Some(ordinal);
            }
            index += 1;
        }
        None
    }

    pub fn query(
        &self,
        query: &str,
        limit: usize,
        category: u8,
        excluded: &[u64],
    ) -> io::Result<IndexQueryResult> {
        let mut scratch = self
            .scratch_pool
            .lock()
            .map_err(|_| io::Error::other("search scratch pool is poisoned"))?
            .pop()
            .unwrap_or_else(|| SearchScratch::new(self.document_count as usize));
        scratch.reset();
        let result = if query.is_empty() {
            IndexQueryResult {
                hits: Vec::new(),
                total_matches: 0,
            }
        } else if query.chars().count() <= SHORT_SCALAR_LIMIT {
            self.query_short(query, limit, category, excluded, &mut scratch)?
        } else {
            self.query_long(query, limit, category, excluded, &mut scratch)?
        };
        self.scratch_pool
            .lock()
            .map_err(|_| io::Error::other("search scratch pool is poisoned"))?
            .push(scratch);
        Ok(result)
    }

    fn query_short(
        &self,
        query: &str,
        limit: usize,
        category: u8,
        excluded: &[u64],
        scratch: &mut SearchScratch,
    ) -> io::Result<IndexQueryResult> {
        let key = scalar_query_key(query)
            .ok_or_else(|| invalid_data("short scalar query key is unavailable"))?;
        for (kind, group, rank) in short_query_plans() {
            let Some(meta) = self.posting(PostingKey { kind, group, key }) else {
                continue;
            };
            let mut decoder = self.posting_decoder(meta)?;
            while let Some(ordinal) = decoder.next_ordinal()? {
                if !bit_is_set(excluded, ordinal) {
                    scratch.mark(ordinal, rank);
                }
            }
        }
        Ok(self.finish_query(scratch, limit, category))
    }

    fn query_long(
        &self,
        query: &str,
        limit: usize,
        category: u8,
        excluded: &[u64],
        scratch: &mut SearchScratch,
    ) -> io::Result<IndexQueryResult> {
        let query_keys = query_byte_gram_keys(query);
        for group in 0..GROUP_COUNT as u8 {
            self.group_candidates(group, &query_keys, scratch)?;
            for candidate_index in 0..scratch.candidates.len() {
                let ordinal = scratch.candidates[candidate_index];
                if bit_is_set(excluded, ordinal) {
                    continue;
                }
                if let Some(rank) = self.match_group(ordinal, group, query, scratch)? {
                    scratch.mark(ordinal, rank);
                }
            }
        }
        Ok(self.finish_query(scratch, limit, category))
    }

    fn finish_query(
        &self,
        scratch: &mut SearchScratch,
        limit: usize,
        category: u8,
    ) -> IndexQueryResult {
        let mut buckets: [Vec<u32>; RANK_COUNT] = array::from_fn(|_| Vec::new());
        let mut total_matches = 0_u64;
        for (word_index, word) in scratch.match_bits.iter().copied().enumerate() {
            let mut remaining = word;
            while remaining != 0 {
                let bit = remaining.trailing_zeros() as usize;
                let ordinal = word_index * u64::BITS as usize + bit;
                if ordinal < scratch.ranks.len()
                    && scratch.rank_generations[ordinal] == scratch.generation
                    && self.document_category(ordinal as u32) == category
                {
                    total_matches += 1;
                    let rank = scratch.ranks[ordinal];
                    if buckets[rank as usize].len() < limit {
                        buckets[rank as usize].push(ordinal as u32);
                    }
                }
                remaining &= remaining - 1;
            }
        }
        let mut hits = Vec::with_capacity(limit.min(total_matches as usize));
        for (rank, bucket) in buckets.iter().enumerate() {
            for ordinal in bucket {
                if hits.len() == limit {
                    break;
                }
                hits.push(IndexHit {
                    item_id: self.document_id(*ordinal).to_owned(),
                    rank: rank as u8,
                });
            }
            if hits.len() == limit {
                break;
            }
        }
        IndexQueryResult {
            hits,
            total_matches,
        }
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
            let Some(meta) = self.posting(PostingKey {
                kind: KIND_BYTE_GRAM,
                group,
                key: *key,
            }) else {
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
        while let Some(ordinal) = decoder.next_ordinal()? {
            scratch.candidates.push(ordinal);
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

    fn match_group(
        &self,
        ordinal: u32,
        group: u8,
        query: &str,
        scratch: &mut SearchScratch,
    ) -> io::Result<Option<u8>> {
        let references = self.document_group_references(ordinal, group)?;
        let result = match group {
            GROUP_TITLE | GROUP_ORIGINAL => {
                let mut references = references;
                let Some(id) = references.next_id()? else {
                    return Ok(None);
                };
                let value =
                    self.dictionary_value(group as usize, id, &mut scratch.dictionary_value)?;
                if group == GROUP_TITLE {
                    rank_scalar(value, query, 0, 3, 6)
                } else if value.is_empty() {
                    None
                } else {
                    rank_scalar(value, query, 1, 4, 7)
                }
            }
            GROUP_ALIASES => {
                if self.dictionary_references_any(
                    references.clone(),
                    group as usize,
                    &mut scratch.dictionary_value,
                    |value| value == query,
                )? {
                    Some(2)
                } else if self.dictionary_references_any(
                    references,
                    group as usize,
                    &mut scratch.dictionary_value,
                    |value| value.contains(query),
                )? {
                    Some(7)
                } else {
                    None
                }
            }
            GROUP_SERIES_SEASON => {
                if self.dictionary_references_any(
                    references.clone(),
                    group as usize,
                    &mut scratch.dictionary_value,
                    |value| value.starts_with(query),
                )? {
                    Some(5)
                } else if self.dictionary_references_any(
                    references,
                    group as usize,
                    &mut scratch.dictionary_value,
                    |value| value.contains(query),
                )? {
                    Some(8)
                } else {
                    None
                }
            }
            GROUP_PINYIN_FULL | GROUP_PINYIN_INITIALS => {
                let mut first = references.clone();
                let prefix = if let Some(id) = first.next_id()? {
                    self.dictionary_value(group as usize, id, &mut scratch.dictionary_value)?
                        .starts_with(query)
                } else {
                    false
                };
                if prefix {
                    Some(if group == GROUP_PINYIN_FULL { 9 } else { 10 })
                } else if self.dictionary_references_any(
                    references,
                    group as usize,
                    &mut scratch.dictionary_value,
                    |value| value.contains(query),
                )? {
                    Some(if group == GROUP_PINYIN_FULL { 11 } else { 12 })
                } else {
                    None
                }
            }
            _ => None,
        };
        Ok(result)
    }

    fn dictionary_references_any(
        &self,
        mut references: ReferenceList<'_>,
        dictionary: usize,
        value_buffer: &mut Vec<u8>,
        mut predicate: impl FnMut(&str) -> bool,
    ) -> io::Result<bool> {
        while let Some(id) = references.next_id()? {
            if predicate(self.dictionary_value(dictionary, id, value_buffer)?) {
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn validate_documents(&self, document_pool_len: u32) -> io::Result<()> {
        if self.document_relative_offset(0)? != 0
            || self.document_relative_offset(self.document_count)? != document_pool_len
        {
            return Err(invalid_data("document offset table endpoints are invalid"));
        }
        let mut previous = 0_u32;
        for ordinal in 0..self.document_count {
            let next = self.document_relative_offset(ordinal + 1)?;
            if next <= previous {
                return Err(invalid_data("document offsets are not strictly ordered"));
            }
            self.validate_document_record(ordinal)?;
            previous = next;
        }
        Ok(())
    }

    fn validate_document_record(&self, ordinal: u32) -> io::Result<()> {
        let record = self.document_record(ordinal)?;
        let mut cursor = 0;
        let id = read_string_slice(record, &mut cursor)
            .ok_or_else(|| invalid_data("document id is invalid"))?;
        if id.is_empty() {
            return Err(invalid_data("document id is empty"));
        }
        let category = *record
            .get(cursor)
            .ok_or_else(|| invalid_data("document category is missing"))?;
        if category > CATEGORY_OTHER {
            return Err(invalid_data("document category is invalid"));
        }
        cursor += 1;
        for dictionary in 0..GROUP_COUNT {
            let count = if dictionary < GROUP_ALIASES as usize {
                1
            } else {
                read_var_u32_slice(record, &mut cursor)
                    .ok_or_else(|| invalid_data("document reference count is invalid"))?
            };
            let dictionary_count = self.dictionary_meta(dictionary)?.count;
            for _ in 0..count {
                let reference = read_var_u32_slice(record, &mut cursor)
                    .ok_or_else(|| invalid_data("document dictionary reference is invalid"))?;
                if reference >= dictionary_count {
                    return Err(invalid_data(
                        "document dictionary reference is out of bounds",
                    ));
                }
            }
        }
        if cursor != record.len() {
            return Err(invalid_data("document record contains trailing bytes"));
        }
        Ok(())
    }

    fn validate_dictionaries(&self) -> io::Result<()> {
        if self.dictionary_relative_offset(0)? != DICTIONARY_HEADER_LEN as u32
            || self.dictionary_relative_offset(GROUP_COUNT as u32)? != self.dictionary_pool_len
        {
            return Err(invalid_data("dictionary pool boundaries are invalid"));
        }
        let mut previous_boundary = DICTIONARY_HEADER_LEN as u32;
        let mut value = Vec::new();
        for dictionary in 0..GROUP_COUNT {
            let boundary = self.dictionary_relative_offset(dictionary as u32 + 1)?;
            if boundary <= previous_boundary {
                return Err(invalid_data("dictionary sections are not strictly ordered"));
            }
            previous_boundary = boundary;
            let meta = self.dictionary_meta(dictionary)?;
            let block_count = (meta.count as usize).div_ceil(DICTIONARY_BLOCK_SIZE);
            if self.dictionary_block_offset(&meta, 0)? != 0
                || self.dictionary_block_offset(&meta, block_count as u32)? != meta.data_len
            {
                return Err(invalid_data("dictionary block boundaries are invalid"));
            }
            let mut previous_value: Option<Vec<u8>> = None;
            for block in 0..block_count {
                let start = self.dictionary_block_offset(&meta, block as u32)?;
                let end = self.dictionary_block_offset(&meta, block as u32 + 1)?;
                if end <= start {
                    return Err(invalid_data("dictionary blocks are not strictly ordered"));
                }
                let bytes = self
                    .blob
                    .get(meta.data_at + start as usize..meta.data_at + end as usize)
                    .ok_or_else(|| invalid_data("dictionary block range is invalid"))?;
                let entry_count = (meta.count as usize - block * DICTIONARY_BLOCK_SIZE)
                    .min(DICTIONARY_BLOCK_SIZE);
                let mut cursor = 0;
                value.clear();
                for entry in 0..entry_count {
                    let prefix = read_var_u32_slice(bytes, &mut cursor)
                        .ok_or_else(|| invalid_data("dictionary prefix is invalid"))?
                        as usize;
                    let suffix_len = read_var_u32_slice(bytes, &mut cursor)
                        .ok_or_else(|| invalid_data("dictionary suffix length is invalid"))?
                        as usize;
                    if (entry == 0 && prefix != 0) || prefix > value.len() {
                        return Err(invalid_data("dictionary prefix is out of bounds"));
                    }
                    let suffix_end = cursor
                        .checked_add(suffix_len)
                        .ok_or_else(|| invalid_data("dictionary suffix range overflow"))?;
                    let suffix = bytes
                        .get(cursor..suffix_end)
                        .ok_or_else(|| invalid_data("dictionary suffix is out of bounds"))?;
                    value.truncate(prefix);
                    value.extend_from_slice(suffix);
                    cursor = suffix_end;
                    std::str::from_utf8(&value)
                        .map_err(|_| invalid_data("dictionary value is not UTF-8"))?;
                    if previous_value
                        .as_ref()
                        .is_some_and(|previous| previous.as_slice() >= value.as_slice())
                    {
                        return Err(invalid_data("dictionary values are not strictly ordered"));
                    }
                    previous_value
                        .get_or_insert_with(Vec::new)
                        .clone_from(&value);
                }
                if cursor != bytes.len() {
                    return Err(invalid_data("dictionary block contains trailing bytes"));
                }
            }
        }
        Ok(())
    }

    fn validate_id_lookup(&self) -> io::Result<()> {
        let mut previous = None;
        for index in 0..self.document_count {
            let entry = self.id_lookup_entry(index)?;
            if entry.1 >= self.document_count || previous.is_some_and(|value| entry < value) {
                return Err(invalid_data(
                    "id lookup is not ordered or references an invalid id",
                ));
            }
            if stable_hash(self.document_id(entry.1).as_bytes()) != entry.0 {
                return Err(invalid_data("id lookup hash does not match document id"));
            }
            previous = Some(entry);
        }
        Ok(())
    }

    fn validate_postings(&self, posting_blob_len: u32) -> io::Result<()> {
        let mut previous_key = None;
        let mut expected_offset = 0_u32;
        for entry_index in 0..self.entry_count {
            let (key, meta) = self.directory_entry(entry_index)?;
            if key.group as usize >= GROUP_COUNT || key.kind > KIND_EXACT_SCALAR {
                return Err(invalid_data("posting key kind/group is invalid"));
            }
            if previous_key.is_some_and(|previous| key <= previous)
                || meta.data_offset != expected_offset
            {
                return Err(invalid_data("posting directory is not ordered/contiguous"));
            }
            previous_key = Some(key);
            expected_offset = expected_offset
                .checked_add(meta.byte_len)
                .ok_or_else(|| invalid_data("posting range overflow"))?;
            if meta.count == 0 || meta.byte_len == 0 {
                return Err(invalid_data("posting entry is empty"));
            }
        }
        if expected_offset != posting_blob_len {
            return Err(invalid_data("posting blob length mismatch"));
        }
        Ok(())
    }

    fn document_relative_offset(&self, ordinal: u32) -> io::Result<u32> {
        if ordinal > self.document_count {
            return Err(invalid_data("document offset ordinal is out of bounds"));
        }
        read_u32_at(
            &self.blob,
            self.document_offsets_at + ordinal as usize * size_of::<u32>(),
        )
    }

    fn document_record(&self, ordinal: u32) -> io::Result<&[u8]> {
        if ordinal >= self.document_count {
            return Err(invalid_data("document ordinal is out of bounds"));
        }
        let start = self.document_relative_offset(ordinal)? as usize;
        let end = self.document_relative_offset(ordinal + 1)? as usize;
        self.blob
            .get(self.document_pool_at + start..self.document_pool_at + end)
            .ok_or_else(|| invalid_data("document record range is invalid"))
    }

    fn document_group_references(&self, ordinal: u32, group: u8) -> io::Result<ReferenceList<'_>> {
        if group as usize >= GROUP_COUNT {
            return Err(invalid_data("document group is out of bounds"));
        }
        let record = self.document_record(ordinal)?;
        let mut cursor = 0;
        read_string_slice(record, &mut cursor)
            .ok_or_else(|| invalid_data("document id is invalid"))?;
        cursor = cursor
            .checked_add(1)
            .filter(|cursor| *cursor <= record.len())
            .ok_or_else(|| invalid_data("document category is missing"))?;
        for dictionary in 0..GROUP_COUNT as u8 {
            if dictionary < GROUP_ALIASES {
                let id = read_var_u32_slice(record, &mut cursor)
                    .ok_or_else(|| invalid_data("document scalar reference is invalid"))?;
                if dictionary == group {
                    return Ok(ReferenceList::Single(Some(id)));
                }
            } else {
                let count = read_var_u32_slice(record, &mut cursor)
                    .ok_or_else(|| invalid_data("document reference count is invalid"))?;
                if dictionary == group {
                    return Ok(ReferenceList::Encoded {
                        bytes: record,
                        position: cursor,
                        remaining: count,
                    });
                }
                for _ in 0..count {
                    read_var_u32_slice(record, &mut cursor)
                        .ok_or_else(|| invalid_data("document reference is invalid"))?;
                }
            }
        }
        Err(invalid_data("document group is unavailable"))
    }

    fn document_id(&self, ordinal: u32) -> &str {
        let record = self.document_record(ordinal).expect("validated document");
        let mut cursor = 0;
        read_string_slice(record, &mut cursor).expect("validated document id")
    }

    fn document_category(&self, ordinal: u32) -> u8 {
        let record = self.document_record(ordinal).expect("validated document");
        let mut cursor = 0;
        read_string_slice(record, &mut cursor).expect("validated document id");
        record[cursor]
    }

    fn dictionary_relative_offset(&self, boundary: u32) -> io::Result<u32> {
        if boundary as usize >= DICTIONARY_BOUNDARY_COUNT {
            return Err(invalid_data("dictionary boundary is out of bounds"));
        }
        read_u32_at(
            &self.blob,
            self.dictionary_pool_at + boundary as usize * size_of::<u32>(),
        )
    }

    fn dictionary_meta(&self, dictionary: usize) -> io::Result<DictionaryMeta> {
        if dictionary >= GROUP_COUNT {
            return Err(invalid_data("dictionary is out of bounds"));
        }
        let start = self.dictionary_relative_offset(dictionary as u32)? as usize;
        let end = self.dictionary_relative_offset(dictionary as u32 + 1)? as usize;
        let section_at = self
            .dictionary_pool_at
            .checked_add(start)
            .ok_or_else(|| invalid_data("dictionary section offset overflow"))?;
        let count = read_u32_at(&self.blob, section_at)?;
        let block_count = (count as usize).div_ceil(DICTIONARY_BLOCK_SIZE);
        let block_offsets_at = section_at + size_of::<u32>();
        let offsets_len = (block_count + 1)
            .checked_mul(size_of::<u32>())
            .ok_or_else(|| invalid_data("dictionary block table overflow"))?;
        let data_at = block_offsets_at
            .checked_add(offsets_len)
            .ok_or_else(|| invalid_data("dictionary data offset overflow"))?;
        let section_end = self
            .dictionary_pool_at
            .checked_add(end)
            .ok_or_else(|| invalid_data("dictionary section end overflow"))?;
        if data_at > section_end {
            return Err(invalid_data("dictionary block table exceeds its section"));
        }
        Ok(DictionaryMeta {
            count,
            block_offsets_at,
            data_at,
            data_len: u32::try_from(section_end - data_at)
                .map_err(|_| invalid_data("dictionary data exceeds u32"))?,
        })
    }

    fn dictionary_block_offset(&self, meta: &DictionaryMeta, block: u32) -> io::Result<u32> {
        let block_count = (meta.count as usize).div_ceil(DICTIONARY_BLOCK_SIZE);
        if block as usize > block_count {
            return Err(invalid_data("dictionary block is out of bounds"));
        }
        read_u32_at(
            &self.blob,
            meta.block_offsets_at + block as usize * size_of::<u32>(),
        )
    }

    fn dictionary_value<'a>(
        &self,
        dictionary: usize,
        id: u32,
        output: &'a mut Vec<u8>,
    ) -> io::Result<&'a str> {
        let meta = self.dictionary_meta(dictionary)?;
        self.decode_dictionary_value(&meta, id, output)?;
        std::str::from_utf8(output).map_err(|_| invalid_data("dictionary value is not UTF-8"))
    }

    fn decode_dictionary_value(
        &self,
        meta: &DictionaryMeta,
        id: u32,
        output: &mut Vec<u8>,
    ) -> io::Result<()> {
        if id >= meta.count {
            return Err(invalid_data("dictionary id is out of bounds"));
        }
        let block = id as usize / DICTIONARY_BLOCK_SIZE;
        let within_block = id as usize % DICTIONARY_BLOCK_SIZE;
        let start = self.dictionary_block_offset(meta, block as u32)? as usize;
        let end = self.dictionary_block_offset(meta, block as u32 + 1)? as usize;
        let bytes = self
            .blob
            .get(meta.data_at + start..meta.data_at + end)
            .ok_or_else(|| invalid_data("dictionary block range is invalid"))?;
        let mut cursor = 0;
        output.clear();
        for entry in 0..=within_block {
            let prefix = read_var_u32_slice(bytes, &mut cursor)
                .ok_or_else(|| invalid_data("dictionary prefix is invalid"))?
                as usize;
            let suffix_len = read_var_u32_slice(bytes, &mut cursor)
                .ok_or_else(|| invalid_data("dictionary suffix length is invalid"))?
                as usize;
            if (entry == 0 && prefix != 0) || prefix > output.len() {
                return Err(invalid_data("dictionary prefix is out of bounds"));
            }
            let suffix_end = cursor
                .checked_add(suffix_len)
                .ok_or_else(|| invalid_data("dictionary suffix range overflow"))?;
            let suffix = bytes
                .get(cursor..suffix_end)
                .ok_or_else(|| invalid_data("dictionary suffix is out of bounds"))?;
            output.truncate(prefix);
            output.extend_from_slice(suffix);
            cursor = suffix_end;
        }
        Ok(())
    }

    fn id_lookup_entry(&self, index: u32) -> io::Result<(u64, u32)> {
        if index >= self.document_count {
            return Err(invalid_data("id lookup index is out of bounds"));
        }
        let offset = self.id_lookup_at + index as usize * ID_LOOKUP_ENTRY_LEN;
        Ok((
            read_u64_at(&self.blob, offset)?,
            read_u32_at(&self.blob, offset + 8)?,
        ))
    }

    fn directory_entry(&self, index: u32) -> io::Result<(PostingKey, PostingMeta)> {
        if index >= self.entry_count {
            return Err(invalid_data("posting directory index is out of bounds"));
        }
        let offset = self.directory_at + index as usize * DIRECTORY_ENTRY_LEN;
        Ok((
            PostingKey {
                kind: *self
                    .blob
                    .get(offset)
                    .ok_or_else(|| invalid_data("posting kind is missing"))?,
                group: *self
                    .blob
                    .get(offset + 1)
                    .ok_or_else(|| invalid_data("posting group is missing"))?,
                key: read_u64_at(&self.blob, offset + 2)?,
            },
            PostingMeta {
                data_offset: read_u32_at(&self.blob, offset + 10)?,
                byte_len: read_u32_at(&self.blob, offset + 14)?,
                count: read_u32_at(&self.blob, offset + 18)?,
            },
        ))
    }

    fn posting(&self, target: PostingKey) -> Option<PostingMeta> {
        let (mut low, mut high) = (0_u32, self.entry_count);
        while low < high {
            let middle = low + (high - low) / 2;
            let (key, meta) = self.directory_entry(middle).ok()?;
            match key.cmp(&target) {
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
        Ok(PostingDecoder {
            bytes: self
                .blob
                .get(start..end)
                .ok_or_else(|| invalid_data("posting byte range is invalid"))?,
            position: 0,
            remaining: meta.count,
            previous: 0,
            first: true,
        })
    }
}

#[derive(Clone)]
pub(super) struct DeltaIndex {
    documents: Vec<IndexDocument>,
    latest: HashMap<String, u32>,
    postings: PostingMap<EncodedPosting>,
    key_scratch: Vec<PostingKey>,
}

impl DeltaIndex {
    pub fn new() -> Self {
        Self {
            documents: Vec::new(),
            latest: HashMap::new(),
            postings: new_posting_map(),
            key_scratch: Vec::with_capacity(512),
        }
    }

    pub fn upsert(&mut self, document: IndexDocument) -> io::Result<()> {
        let ordinal = u32::try_from(self.documents.len())
            .map_err(|_| invalid_data("delta document count exceeds u32"))?;
        append_document_postings(
            &mut self.postings,
            &document,
            ordinal,
            &mut self.key_scratch,
        )?;
        self.latest.insert(document.item_id.clone(), ordinal);
        self.documents.push(document);
        Ok(())
    }

    pub fn remove(&mut self, item_id: &str) {
        self.latest.remove(item_id);
    }

    pub fn query(&self, query: &str, limit: usize, category: u8) -> io::Result<IndexQueryResult> {
        if query.is_empty() {
            return Ok(IndexQueryResult {
                hits: Vec::new(),
                total_matches: 0,
            });
        }
        let mut ranks = HashMap::<u32, u8>::new();
        if query.chars().count() <= SHORT_SCALAR_LIMIT {
            let key = scalar_query_key(query)
                .ok_or_else(|| invalid_data("short scalar query key is unavailable"))?;
            for (kind, group, rank) in short_query_plans() {
                let Some(posting) = self.postings.get(&PostingKey { kind, group, key }) else {
                    continue;
                };
                let mut decoder = PostingDecoder::from_encoded(posting);
                while let Some(ordinal) = decoder.next_ordinal()? {
                    if self.is_latest(ordinal)
                        && self.documents[ordinal as usize].category == category
                    {
                        ranks
                            .entry(ordinal)
                            .and_modify(|current| *current = (*current).min(rank))
                            .or_insert(rank);
                    }
                }
            }
        } else {
            let query_keys = query_byte_gram_keys(query);
            let mut candidates = Vec::new();
            let mut intersection = Vec::new();
            for group in 0..GROUP_COUNT as u8 {
                delta_group_candidates(
                    &self.postings,
                    group,
                    &query_keys,
                    &mut candidates,
                    &mut intersection,
                )?;
                for ordinal in &candidates {
                    if self.is_latest(*ordinal)
                        && self.documents[*ordinal as usize].category == category
                    {
                        if let Some(rank) =
                            self.documents[*ordinal as usize].match_group(group, query)
                        {
                            ranks
                                .entry(*ordinal)
                                .and_modify(|current| *current = (*current).min(rank))
                                .or_insert(rank);
                        }
                    }
                }
            }
        }
        let total_matches = ranks.len() as u64;
        let mut ranked = ranks.into_iter().collect::<Vec<_>>();
        let compare = |left: &(u32, u8), right: &(u32, u8)| {
            let left_document = &self.documents[left.0 as usize];
            let right_document = &self.documents[right.0 as usize];
            (&left.1, &left_document.sort_key, &left_document.item_id).cmp(&(
                &right.1,
                &right_document.sort_key,
                &right_document.item_id,
            ))
        };
        if ranked.len() > limit {
            ranked.select_nth_unstable_by(limit, &compare);
            ranked.truncate(limit);
        }
        ranked.sort_unstable_by(compare);
        let hits = ranked
            .into_iter()
            .map(|(ordinal, rank)| IndexHit {
                item_id: self.documents[ordinal as usize].item_id.clone(),
                rank,
            })
            .collect();
        Ok(IndexQueryResult {
            hits,
            total_matches,
        })
    }

    fn is_latest(&self, ordinal: u32) -> bool {
        let document = &self.documents[ordinal as usize];
        self.latest.get(&document.item_id) == Some(&ordinal)
    }
}

fn delta_group_candidates(
    postings: &PostingMap<EncodedPosting>,
    group: u8,
    query_keys: &[u64],
    candidates: &mut Vec<u32>,
    intersection: &mut Vec<u32>,
) -> io::Result<()> {
    candidates.clear();
    let mut selected = Vec::with_capacity(query_keys.len());
    for key in query_keys {
        let Some(posting) = postings.get(&PostingKey {
            kind: KIND_BYTE_GRAM,
            group,
            key: *key,
        }) else {
            return Ok(());
        };
        selected.push((*key, posting));
    }
    selected.sort_unstable_by_key(|(key, posting)| (posting.count, *key));
    selected.truncate(MAX_GRAMS_PER_GROUP);
    let Some((_, first)) = selected.first().copied() else {
        return Ok(());
    };
    let mut decoder = PostingDecoder::from_encoded(first);
    while let Some(ordinal) = decoder.next_ordinal()? {
        candidates.push(ordinal);
    }
    for (_, posting) in selected.into_iter().skip(1) {
        intersection.clear();
        intersect_compressed(
            candidates,
            PostingDecoder::from_encoded(posting),
            intersection,
        )?;
        std::mem::swap(candidates, intersection);
        if candidates.is_empty() {
            break;
        }
    }
    Ok(())
}

struct SearchScratch {
    ranks: Vec<u8>,
    rank_generations: Vec<u32>,
    generation: u32,
    match_bits: Vec<u64>,
    touched_words: Vec<u32>,
    candidates: Vec<u32>,
    intersection: Vec<u32>,
    dictionary_value: Vec<u8>,
}

impl SearchScratch {
    fn new(document_count: usize) -> Self {
        Self {
            ranks: vec![NO_RANK; document_count],
            rank_generations: vec![0; document_count],
            generation: 0,
            match_bits: vec![0; document_count.div_ceil(u64::BITS as usize)],
            touched_words: Vec::new(),
            candidates: Vec::new(),
            intersection: Vec::new(),
            dictionary_value: Vec::new(),
        }
    }

    fn reset(&mut self) {
        for word in self.touched_words.drain(..) {
            self.match_bits[word as usize] = 0;
        }
        if self.generation == u32::MAX {
            self.rank_generations.fill(0);
            self.generation = 1;
        } else {
            self.generation += 1;
        }
        self.candidates.clear();
        self.intersection.clear();
        self.dictionary_value.clear();
    }

    fn mark(&mut self, ordinal: u32, rank: u8) {
        let index = ordinal as usize;
        if self.rank_generations[index] == self.generation {
            self.ranks[index] = self.ranks[index].min(rank);
        } else {
            self.rank_generations[index] = self.generation;
            self.ranks[index] = rank;
        }
        let word_index = index / u64::BITS as usize;
        if self.match_bits[word_index] == 0 {
            self.touched_words.push(word_index as u32);
        }
        self.match_bits[word_index] |= 1_u64 << (index % u64::BITS as usize);
    }
}

#[derive(Clone, Copy)]
struct DictionaryMeta {
    count: u32,
    block_offsets_at: usize,
    data_at: usize,
    data_len: u32,
}

#[derive(Clone)]
enum ReferenceList<'a> {
    Single(Option<u32>),
    Encoded {
        bytes: &'a [u8],
        position: usize,
        remaining: u32,
    },
}

impl ReferenceList<'_> {
    fn next_id(&mut self) -> io::Result<Option<u32>> {
        match self {
            Self::Single(value) => Ok(value.take()),
            Self::Encoded {
                bytes,
                position,
                remaining,
            } => {
                if *remaining == 0 {
                    return Ok(None);
                }
                let value = read_var_u32_slice(bytes, position)
                    .ok_or_else(|| invalid_data("document reference is invalid"))?;
                *remaining -= 1;
                Ok(Some(value))
            }
        }
    }
}

struct PostingDecoder<'a> {
    bytes: &'a [u8],
    position: usize,
    remaining: u32,
    previous: u32,
    first: bool,
}

impl<'a> PostingDecoder<'a> {
    fn from_encoded(posting: &'a EncodedPosting) -> Self {
        Self {
            bytes: &posting.bytes,
            position: 0,
            remaining: posting.count,
            previous: 0,
            first: true,
        }
    }

    fn next_ordinal(&mut self) -> io::Result<Option<u32>> {
        if self.remaining == 0 {
            return Ok(None);
        }
        let delta = read_var_u32_slice(self.bytes, &mut self.position)
            .ok_or_else(|| invalid_data("invalid posting varint"))?;
        let ordinal = if self.first {
            self.first = false;
            delta
        } else {
            self.previous
                .checked_add(delta)
                .ok_or_else(|| invalid_data("posting delta overflow"))?
        };
        self.previous = ordinal;
        self.remaining -= 1;
        Ok(Some(ordinal))
    }
}

fn intersect_compressed(
    left: &[u32],
    mut right: PostingDecoder<'_>,
    output: &mut Vec<u32>,
) -> io::Result<()> {
    output.reserve(left.len().min(right.remaining as usize));
    let mut right_value = right.next_ordinal()?;
    for left_value in left {
        while right_value.is_some_and(|value| value < *left_value) {
            right_value = right.next_ordinal()?;
        }
        if right_value == Some(*left_value) {
            output.push(*left_value);
            right_value = right.next_ordinal()?;
        }
        if right_value.is_none() {
            break;
        }
    }
    Ok(())
}

fn short_query_plans() -> [(u8, u8, u8); 14] {
    [
        (KIND_EXACT_SCALAR, GROUP_TITLE, 0),
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
    ]
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

fn append_document_postings(
    postings: &mut PostingMap<EncodedPosting>,
    document: &IndexDocument,
    ordinal: u32,
    keys: &mut Vec<PostingKey>,
) -> io::Result<()> {
    for group in 0..GROUP_COUNT as u8 {
        keys.clear();
        document.for_each_group_field(group, |field| {
            add_byte_trigram_keys(group, field.as_bytes(), keys);
            add_all_scalar_gram_keys(group, field, keys);
        });
        document.for_each_prefix_field(group, |field| {
            add_prefix_scalar_keys(group, field, keys);
        });
        document.for_each_exact_field(group, |field| {
            add_exact_scalar_key(group, field, keys);
        });
        for key in &*keys {
            postings.entry(*key).or_default().append(ordinal)?;
        }
    }
    Ok(())
}

fn add_byte_trigram_keys(group: u8, bytes: &[u8], output: &mut Vec<PostingKey>) {
    output.extend(bytes.windows(3).map(|window| PostingKey {
        kind: KIND_BYTE_GRAM,
        group,
        key: u64::from(byte_gram_key(window)),
    }));
}

fn add_all_scalar_gram_keys(group: u8, value: &str, output: &mut Vec<PostingKey>) {
    let mut previous = None;
    let mut previous_two = None;
    for current in value.chars() {
        output.push(PostingKey {
            kind: KIND_SCALAR_GRAM,
            group,
            key: scalar_key(&[current]),
        });
        if let Some(previous) = previous {
            output.push(PostingKey {
                kind: KIND_SCALAR_GRAM,
                group,
                key: scalar_key(&[previous, current]),
            });
        }
        if let Some((first, second)) = previous_two {
            output.push(PostingKey {
                kind: KIND_SCALAR_GRAM,
                group,
                key: scalar_key(&[first, second, current]),
            });
        }
        previous_two = previous.map(|previous| (previous, current));
        previous = Some(current);
    }
}

fn add_prefix_scalar_keys(group: u8, value: &str, output: &mut Vec<PostingKey>) {
    let mut scalars = ['\0'; SHORT_SCALAR_LIMIT];
    for (count, scalar) in value.chars().take(SHORT_SCALAR_LIMIT).enumerate() {
        scalars[count] = scalar;
        output.push(PostingKey {
            kind: KIND_PREFIX_SCALAR,
            group,
            key: scalar_key(&scalars[..=count]),
        });
    }
}

fn add_exact_scalar_key(group: u8, value: &str, output: &mut Vec<PostingKey>) {
    if let Some((scalars, count)) = short_scalars(value) {
        output.push(PostingKey {
            kind: KIND_EXACT_SCALAR,
            group,
            key: scalar_key(&scalars[..count]),
        });
    }
}

fn query_byte_gram_keys(query: &str) -> Vec<u64> {
    let bytes = query.as_bytes();
    let mut keys = bytes
        .windows(3)
        .map(|window| u64::from(byte_gram_key(window)))
        .collect::<Vec<_>>();
    keys.sort_unstable();
    keys.dedup();
    keys
}

fn byte_gram_key(bytes: &[u8]) -> u32 {
    let payload = bytes
        .iter()
        .fold(0_u32, |value, byte| (value << 8) | u32::from(*byte));
    ((bytes.len() as u32) << 24) | payload
}

fn scalar_query_key(value: &str) -> Option<u64> {
    short_scalars(value).map(|(scalars, count)| scalar_key(&scalars[..count]))
}

fn short_scalars(value: &str) -> Option<([char; SHORT_SCALAR_LIMIT], usize)> {
    let mut scalars = ['\0'; SHORT_SCALAR_LIMIT];
    let mut count = 0;
    for scalar in value.chars() {
        if count == SHORT_SCALAR_LIMIT {
            return None;
        }
        scalars[count] = scalar;
        count += 1;
    }
    (count > 0).then_some((scalars, count))
}

fn scalar_key(scalars: &[char]) -> u64 {
    scalars.iter().fold(0_u64, |value, character| {
        // Bias every scalar so a leading U+0000 cannot disappear from the
        // folded integer (for example, "a" must differ from "\0a").
        (value << 21) | u64::from(u32::from(*character) + 1)
    })
}

fn intern_values_into_record(
    output: &mut Vec<u8>,
    interner: &mut StringInterner,
    value_count: usize,
    values: impl IntoIterator<Item = String>,
) -> io::Result<()> {
    write_var_u32_vec(
        output,
        u32::try_from(value_count).map_err(|_| invalid_data("reference list exceeds u32"))?,
    );
    let mut written = 0_usize;
    for value in values {
        write_var_u32_vec(output, interner.intern(value)?);
        written += 1;
    }
    if written != value_count {
        return Err(invalid_data("reference list count changed during encoding"));
    }
    Ok(())
}

fn rewrite_document_pool(
    input_path: &Path,
    output_path: &Path,
    input_offsets: &[u32],
    dictionaries: &[SortedDictionary; GROUP_COUNT],
) -> io::Result<Vec<u32>> {
    if input_offsets.first().copied() != Some(0) {
        return Err(invalid_data("temporary document offsets are invalid"));
    }
    let input_len = input_offsets.last().copied().unwrap_or_default();
    if File::open(input_path)?.metadata()?.len() != u64::from(input_len) {
        return Err(invalid_data("temporary document pool length mismatch"));
    }
    let mut reader = BufReader::new(File::open(input_path)?);
    let mut writer = BufWriter::new(File::create(output_path)?);
    let mut input = Vec::new();
    let mut output = Vec::new();
    let mut output_offsets = Vec::with_capacity(input_offsets.len());
    output_offsets.push(0_u32);
    for pair in input_offsets.windows(2) {
        let record_len = pair[1]
            .checked_sub(pair[0])
            .ok_or_else(|| invalid_data("temporary document offsets are not ordered"))?;
        input.resize(record_len as usize, 0);
        reader.read_exact(&mut input)?;
        remap_document_record(&input, dictionaries, &mut output)?;
        writer.write_all(&output)?;
        let next = output_offsets
            .last()
            .copied()
            .unwrap_or_default()
            .checked_add(
                u32::try_from(output.len())
                    .map_err(|_| invalid_data("compact document record exceeds u32"))?,
            )
            .ok_or_else(|| invalid_data("compact document pool exceeds u32"))?;
        output_offsets.push(next);
    }
    writer.flush()?;
    Ok(output_offsets)
}

fn remap_document_record(
    input: &[u8],
    dictionaries: &[SortedDictionary; GROUP_COUNT],
    output: &mut Vec<u8>,
) -> io::Result<()> {
    output.clear();
    let mut cursor = 0;
    let item_id = read_string_slice(input, &mut cursor)
        .ok_or_else(|| invalid_data("temporary document id is invalid"))?;
    write_var_u32_vec(
        output,
        u32::try_from(item_id.len()).map_err(|_| invalid_data("document id exceeds u32"))?,
    );
    output.extend_from_slice(item_id.as_bytes());
    let category = *input
        .get(cursor)
        .ok_or_else(|| invalid_data("temporary document category is missing"))?;
    if category > CATEGORY_OTHER {
        return Err(invalid_data("temporary document category is invalid"));
    }
    cursor += 1;
    output.push(category);
    for (dictionary, values) in dictionaries.iter().enumerate() {
        let count = if dictionary < GROUP_ALIASES as usize {
            1
        } else {
            let count = read_var_u32_slice(input, &mut cursor)
                .ok_or_else(|| invalid_data("temporary reference count is invalid"))?;
            write_var_u32_vec(output, count);
            count
        };
        for _ in 0..count {
            let old_id = read_var_u32_slice(input, &mut cursor)
                .ok_or_else(|| invalid_data("temporary dictionary reference is invalid"))?;
            let new_id =
                values.remap.get(old_id as usize).copied().ok_or_else(|| {
                    invalid_data("temporary dictionary reference is out of bounds")
                })?;
            write_var_u32_vec(output, new_id);
        }
    }
    if cursor != input.len() {
        return Err(invalid_data("temporary document contains trailing bytes"));
    }
    Ok(())
}

fn dictionary_pool_encoded_len(dictionaries: &[SortedDictionary; GROUP_COUNT]) -> io::Result<u32> {
    let total = dictionaries
        .iter()
        .try_fold(DICTIONARY_HEADER_LEN, |total, dictionary| {
            total
                .checked_add(dictionary_encoded_len(dictionary)?)
                .ok_or_else(|| invalid_data("dictionary pool length overflow"))
        })?;
    u32::try_from(total).map_err(|_| invalid_data("dictionary pool exceeds u32"))
}

fn dictionary_encoded_len(dictionary: &SortedDictionary) -> io::Result<usize> {
    let block_count = dictionary.values.len().div_ceil(DICTIONARY_BLOCK_SIZE);
    let mut total = size_of::<u32>()
        .checked_add(
            (block_count + 1)
                .checked_mul(size_of::<u32>())
                .ok_or_else(|| invalid_data("dictionary block table length overflow"))?,
        )
        .ok_or_else(|| invalid_data("dictionary length overflow"))?;
    let mut previous = "";
    for (index, value) in dictionary.values.iter().enumerate() {
        let prefix = if index % DICTIONARY_BLOCK_SIZE == 0 {
            0
        } else {
            common_prefix_len(previous.as_bytes(), value.as_bytes())
        };
        let suffix_len = value.len() - prefix;
        total = total
            .checked_add(var_u32_len_u64(prefix as u64))
            .and_then(|value| value.checked_add(var_u32_len_u64(suffix_len as u64)))
            .and_then(|value| value.checked_add(suffix_len))
            .ok_or_else(|| invalid_data("dictionary length overflow"))?;
        previous = value;
    }
    Ok(total)
}

fn write_dictionary_pool(
    writer: &mut impl Write,
    dictionaries: &[SortedDictionary; GROUP_COUNT],
) -> io::Result<()> {
    let mut boundary = u32::try_from(DICTIONARY_HEADER_LEN)
        .map_err(|_| invalid_data("dictionary header exceeds u32"))?;
    write_u32(writer, boundary)?;
    for dictionary in dictionaries {
        boundary = boundary
            .checked_add(
                u32::try_from(dictionary_encoded_len(dictionary)?)
                    .map_err(|_| invalid_data("dictionary exceeds u32"))?,
            )
            .ok_or_else(|| invalid_data("dictionary boundary overflow"))?;
        write_u32(writer, boundary)?;
    }
    for dictionary in dictionaries {
        write_dictionary(writer, dictionary)?;
    }
    Ok(())
}

fn write_dictionary(writer: &mut impl Write, dictionary: &SortedDictionary) -> io::Result<()> {
    write_u32(
        writer,
        u32::try_from(dictionary.values.len())
            .map_err(|_| invalid_data("dictionary exceeds u32"))?,
    )?;
    let block_count = dictionary.values.len().div_ceil(DICTIONARY_BLOCK_SIZE);
    let mut offsets = Vec::with_capacity(block_count + 1);
    offsets.push(0_u32);
    let mut encoded_len = 0_u32;
    let mut previous = "";
    for (index, value) in dictionary.values.iter().enumerate() {
        if index != 0 && index % DICTIONARY_BLOCK_SIZE == 0 {
            offsets.push(encoded_len);
        }
        let prefix = if index % DICTIONARY_BLOCK_SIZE == 0 {
            0
        } else {
            common_prefix_len(previous.as_bytes(), value.as_bytes())
        };
        let suffix_len = value.len() - prefix;
        let entry_len = var_u32_len_u64(prefix as u64)
            .checked_add(var_u32_len_u64(suffix_len as u64))
            .and_then(|length| length.checked_add(suffix_len))
            .ok_or_else(|| invalid_data("dictionary entry length overflow"))?;
        encoded_len = encoded_len
            .checked_add(
                u32::try_from(entry_len)
                    .map_err(|_| invalid_data("dictionary entry exceeds u32"))?,
            )
            .ok_or_else(|| invalid_data("dictionary data exceeds u32"))?;
        previous = value;
    }
    if block_count > 0 {
        offsets.push(encoded_len);
    }
    for offset in offsets {
        write_u32(writer, offset)?;
    }
    previous = "";
    for (index, value) in dictionary.values.iter().enumerate() {
        let prefix = if index % DICTIONARY_BLOCK_SIZE == 0 {
            0
        } else {
            common_prefix_len(previous.as_bytes(), value.as_bytes())
        };
        let suffix = &value.as_bytes()[prefix..];
        write_var_u32(
            writer,
            u32::try_from(prefix).map_err(|_| invalid_data("dictionary prefix exceeds u32"))?,
        )?;
        write_var_u32(
            writer,
            u32::try_from(suffix.len())
                .map_err(|_| invalid_data("dictionary suffix exceeds u32"))?,
        )?;
        writer.write_all(suffix)?;
        previous = value;
    }
    Ok(())
}

fn common_prefix_len(left: &[u8], right: &[u8]) -> usize {
    left.iter()
        .zip(right)
        .take_while(|(left, right)| left == right)
        .count()
}

fn read_string_slice<'a>(bytes: &'a [u8], cursor: &mut usize) -> Option<&'a str> {
    let len = read_var_u32_slice(bytes, cursor)? as usize;
    let end = cursor.checked_add(len)?;
    let value = std::str::from_utf8(bytes.get(*cursor..end)?).ok()?;
    *cursor = end;
    Some(value)
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

fn write_var_u32(writer: &mut impl Write, mut value: u32) -> io::Result<()> {
    while value >= 0x80 {
        writer.write_all(&[((value as u8) & 0x7f) | 0x80])?;
        value >>= 7;
    }
    writer.write_all(&[value as u8])
}

fn write_var_u32_vec(output: &mut Vec<u8>, mut value: u32) {
    while value >= 0x80 {
        output.push(((value as u8) & 0x7f) | 0x80);
        value >>= 7;
    }
    output.push(value as u8);
}

fn var_u32_len_u64(mut value: u64) -> usize {
    let mut len = 1;
    while value >= 0x80 {
        value >>= 7;
        len += 1;
    }
    len
}

fn write_u32(writer: &mut impl Write, value: u32) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}

fn write_u64(writer: &mut impl Write, value: u64) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}

fn write_i64(writer: &mut impl Write, value: i64) -> io::Result<()> {
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

fn read_i64_at(bytes: &[u8], offset: usize) -> io::Result<i64> {
    let value = bytes
        .get(offset..offset + size_of::<i64>())
        .ok_or_else(|| invalid_data("i64 read is out of bounds"))?;
    Ok(i64::from_le_bytes(
        value
            .try_into()
            .map_err(|_| invalid_data("i64 byte width mismatch"))?,
    ))
}

fn bit_is_set(bits: &[u64], ordinal: u32) -> bool {
    let index = ordinal as usize;
    bits.get(index / u64::BITS as usize)
        .is_some_and(|word| word & (1_u64 << (index % u64::BITS as usize)) != 0)
}

fn stable_hash(bytes: &[u8]) -> u64 {
    stable_hash_update(FNV_OFFSET_BASIS, bytes)
}

fn stable_hash_update(hash: u64, bytes: &[u8]) -> u64 {
    bytes.iter().fold(hash, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(FNV_PRIME)
    })
}

pub(super) fn scope_fingerprint(parts: &[&str]) -> u64 {
    parts.iter().fold(FNV_OFFSET_BASIS, |hash, part| {
        let hash = stable_hash_update(hash, &(part.len() as u64).to_le_bytes());
        stable_hash_update(hash, part.as_bytes())
    })
}

pub(super) fn generation_path(database_path: &Path, generation: i64) -> PathBuf {
    exact_suffix_path(database_path, &format!(".search-{generation}.idx"))
}

pub(super) fn cleanup_sidecars(
    database_path: &Path,
    keep_generation: Option<i64>,
    strict: bool,
) -> io::Result<()> {
    let Some(parent) = database_path.parent() else {
        return Ok(());
    };
    let Some(database_name) = database_path.file_name().and_then(|name| name.to_str()) else {
        return Ok(());
    };
    let prefix = format!("{database_name}.search-");
    let entries = match fs::read_dir(parent) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    for entry in entries {
        let entry = entry?;
        let Some(name) = entry.file_name().to_str().map(ToOwned::to_owned) else {
            continue;
        };
        let Some(remainder) = name.strip_prefix(&prefix) else {
            continue;
        };
        let generation = remainder
            .strip_suffix(".idx")
            .and_then(|value| value.parse::<i64>().ok());
        let is_temp = remainder.contains(".idx.tmp-")
            || remainder.contains(".idx.refs-")
            || remainder.contains(".idx.docs-");
        let remove = generation.is_some_and(|value| Some(value) != keep_generation) || is_temp;
        if !remove {
            continue;
        }
        match fs::remove_file(entry.path()) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) if !strict => {
                let _ = error;
            }
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

fn exact_suffix_path(path: &Path, suffix: &str) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(suffix);
    PathBuf::from(value)
}

fn invalid_data(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.into())
}

fn now_nanos() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos()
}

#[cfg(test)]
mod tests {
    use std::fs;

    use tempfile::TempDir;

    use super::{
        CATEGORY_TITLE, EncodedPosting, IndexDocument, PostingDecoder, SidecarBuilder,
        add_byte_trigram_keys, query_byte_gram_keys, scalar_key,
    };

    #[test]
    fn scalar_keys_preserve_leading_nul_length() {
        assert_ne!(scalar_key(&['a']), scalar_key(&['\0', 'a']));
    }

    #[test]
    fn posting_append_ignores_same_document_duplicates() {
        let mut posting = EncodedPosting::default();
        posting.append(7).unwrap();
        posting.append(7).unwrap();
        posting.append(9).unwrap();
        let mut decoder = PostingDecoder::from_encoded(&posting);
        assert_eq!(decoder.next_ordinal().unwrap(), Some(7));
        assert_eq!(decoder.next_ordinal().unwrap(), Some(9));
        assert_eq!(decoder.next_ordinal().unwrap(), None);
        assert_eq!(posting.count, 2);
    }

    #[test]
    fn persistent_builder_emits_only_queryable_byte_trigrams() {
        let mut postings = Vec::new();
        add_byte_trigram_keys(0, b"abcdef", &mut postings);
        assert_eq!(postings.len(), 4);
        assert!(postings.iter().all(|posting| posting.key >> 24 == 3));
        let built = postings
            .iter()
            .map(|posting| posting.key)
            .collect::<Vec<_>>();
        assert_eq!(query_byte_gram_keys("abcdef"), built);
        assert!(query_byte_gram_keys("ab").is_empty());
    }

    #[test]
    fn builder_drop_removes_exact_scratch_after_publish_error() {
        let temporary = TempDir::new().unwrap();
        let database_path = temporary.path().join("catalog.sqlite3");
        let mut builder = SidecarBuilder::new(&database_path, 1, 0).unwrap();
        let scratch_paths = [
            builder.document_temp_path.clone(),
            builder.compact_document_temp_path.clone(),
            builder.sidecar_temp_path.clone(),
        ];
        let final_path = builder.final_path.clone();
        fs::create_dir(&final_path).unwrap();
        builder
            .add_document(IndexDocument {
                item_id: "valid-item".to_owned(),
                category: CATEGORY_TITLE,
                sort_key: "valid item".to_owned(),
                title: "valid item".to_owned(),
                original_title: String::new(),
                series_title: String::new(),
                season_title: String::new(),
                aliases: Vec::new(),
                pinyin_full: Vec::new(),
                pinyin_initials: Vec::new(),
            })
            .unwrap();

        assert!(builder.finish().is_err());
        assert!(
            final_path.is_dir(),
            "cleanup must not remove the final path"
        );
        for path in scratch_paths {
            assert!(
                !path.exists(),
                "orphaned sidecar scratch: {}",
                path.display()
            );
        }
    }
}
