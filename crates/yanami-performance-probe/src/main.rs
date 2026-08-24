use std::{
    collections::HashSet,
    env, fs, io,
    io::{BufRead, BufReader, Read},
    path::{Path, PathBuf},
    str,
    time::{Duration, Instant},
};

use chrono::Utc;
use secrecy::SecretString;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::TcpListener,
    task::JoinSet,
    time::sleep,
};
use url::Url;
use uuid::Uuid;
use yanami_core::{ServerProfile, TransportSecurity};
use yanami_emby::{BaseItem, ClientIdentity, EmbyClient, ItemQuery, ItemsResult};

mod search_probe;

const FULL_LIBRARY_ITEMS: usize = 110_000;
const FULL_LIBRARY_PAGE_SIZE: usize = 500;

#[derive(Debug)]
struct Arguments {
    profile: String,
    mode: String,
    output: PathBuf,
}

#[derive(Debug)]
struct FixtureEvidence {
    id: &'static str,
    sha256: Option<String>,
    validated: bool,
    item_count: usize,
    query_count: usize,
}

fn contract_profile(profile: &str) -> &'static str {
    match profile {
        "pr" | "pullrequest" => "PullRequest",
        "lab" => "Lab",
        "nightly" => "Nightly",
        "weekly" => "Weekly",
        "release" => "Release",
        _ => unreachable!("profile validation occurs during argument parsing"),
    }
}

#[derive(Debug)]
pub(crate) struct Measurement {
    pub(crate) id: String,
    pub(crate) unit: String,
    pub(crate) samples: Vec<f64>,
    pub(crate) attributes: Value,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct PageRequest {
    start_index: u32,
    limit: u32,
}

#[derive(Debug)]
struct SequentialRun {
    samples: Vec<f64>,
    requested_pages: Vec<PageRequest>,
    unique_item_count: usize,
}

#[derive(Debug)]
struct QueueServerEvidence {
    accepted_ms: Vec<f64>,
    requested_pages: Vec<PageRequest>,
}

fn parse_arguments() -> Result<Arguments, String> {
    let mut profile = String::from("pr");
    let mut mode = env::var("YANAMI_PERF_GATE_MODE").unwrap_or_else(|_| String::from("collect"));
    let mut output = PathBuf::from("backend-performance.json");
    let mut arguments = env::args().skip(1);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--profile" => {
                profile = arguments
                    .next()
                    .ok_or_else(|| String::from("--profile requires a value"))?;
            }
            "--output" => {
                output = PathBuf::from(
                    arguments
                        .next()
                        .ok_or_else(|| String::from("--output requires a value"))?,
                );
            }
            "--mode" => {
                mode = arguments
                    .next()
                    .ok_or_else(|| String::from("--mode requires a value"))?;
            }
            "--help" | "-h" => {
                println!(
                    "yanami-performance-probe --profile pr|lab|nightly|weekly|release --mode collect|debt|enforce --output PATH"
                );
                std::process::exit(0);
            }
            _ => return Err(format!("unknown argument: {argument}")),
        }
    }
    let normalized = profile.to_ascii_lowercase();
    if !matches!(
        normalized.as_str(),
        "pr" | "pullrequest" | "lab" | "nightly" | "weekly" | "release"
    ) {
        return Err(format!("unsupported profile: {profile}"));
    }
    let mode = mode.to_ascii_lowercase();
    if !matches!(mode.as_str(), "collect" | "debt" | "enforce") {
        return Err(format!("unsupported mode: {mode}"));
    }
    Ok(Arguments {
        profile: normalized,
        mode,
        output,
    })
}

fn sha256_file(path: &Path) -> Result<String, Box<dyn std::error::Error>> {
    let mut reader = BufReader::new(fs::File::open(path)?);
    let mut digest = Sha256::new();
    let mut buffer = vec![0_u8; 64 * 1024];
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        digest.update(&buffer[..read]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn count_lines(path: &Path) -> Result<usize, Box<dyn std::error::Error>> {
    let mut reader = BufReader::new(fs::File::open(path)?);
    let mut buffer = Vec::with_capacity(4096);
    let mut count = 0_usize;
    while reader.read_until(b'\n', &mut buffer)? != 0 {
        count += 1;
        buffer.clear();
    }
    Ok(count)
}

fn validate_f110k_fixture() -> Result<FixtureEvidence, Box<dyn std::error::Error>> {
    let Ok(directory) = env::var("YANAMI_PERF_FIXTURE_DIR") else {
        return Ok(FixtureEvidence {
            id: "BackendSynthetic110K-v1",
            sha256: None,
            validated: true,
            item_count: FULL_LIBRARY_ITEMS,
            query_count: 0,
        });
    };
    let expected_combined = env::var("YANAMI_PERF_F110K_SHA256")
        .map_err(|_| "YANAMI_PERF_FIXTURE_DIR requires YANAMI_PERF_F110K_SHA256")?;
    let root = PathBuf::from(directory);
    let items_path = root.join("f110k-items.jsonl");
    let queries_path = root.join("search-queries-v1.jsonl");
    let item_count = count_lines(&items_path)?;
    let query_count = count_lines(&queries_path)?;
    if item_count != FULL_LIBRARY_ITEMS || query_count != 20_000 {
        return Err(format!(
            "F110K fixture count mismatch: items={item_count}, queries={query_count}"
        )
        .into());
    }
    let item_hash = sha256_file(&items_path)?;
    let query_hash = sha256_file(&queries_path)?;
    let combined = format!(
        "{:x}",
        Sha256::digest(format!("{item_hash}:{query_hash}").as_bytes())
    );
    if combined != expected_combined.to_ascii_lowercase() {
        return Err(format!(
            "F110K combined hash mismatch: expected {expected_combined}, got {combined}"
        )
        .into());
    }
    Ok(FixtureEvidence {
        id: "F110K-v1",
        sha256: Some(combined),
        validated: true,
        item_count,
        query_count,
    })
}

fn fixture_item_id(index: usize) -> String {
    format!("fixture-{index:06}")
}

fn fixture_items(start_index: usize, count: usize, overview_bytes: usize) -> Vec<BaseItem> {
    let overview = (overview_bytes > 0).then(|| "x".repeat(overview_bytes));
    (0..count)
        .map(|relative_index| {
            let index = start_index + relative_index;
            BaseItem {
                id: fixture_item_id(index),
                name: format!("性能媒体 Performance Item {index:06}"),
                item_type: Some(if index % 11 == 0 { "Movie" } else { "Episode" }.to_owned()),
                series_name: Some(format!("系列 Series {:04}", index / 10)),
                season_name: Some(format!("Season {}", (index / 10) % 12 + 1)),
                parent_index_number: Some(((index / 10) % 12 + 1) as i32),
                index_number: Some((index % 10 + 1) as i32),
                overview: overview.clone(),
                ..BaseItem::default()
            }
        })
        .collect()
}

fn items_response(
    request: PageRequest,
    overview_bytes: usize,
) -> Result<Vec<u8>, serde_json::Error> {
    let start_index = request.start_index as usize;
    let item_count = (request.limit as usize).min(FULL_LIBRARY_ITEMS.saturating_sub(start_index));
    serde_json::to_vec(&ItemsResult {
        items: fixture_items(start_index, item_count, overview_bytes),
        total_record_count: FULL_LIBRARY_ITEMS as u64,
        start_index: u64::from(request.start_index),
    })
}

async fn read_request_headers(stream: &mut tokio::net::TcpStream) -> io::Result<Vec<u8>> {
    let mut request = Vec::with_capacity(2048);
    let mut buffer = [0_u8; 1024];
    loop {
        let read = stream.read(&mut buffer).await?;
        if read == 0 {
            break;
        }
        request.extend_from_slice(&buffer[..read]);
        if request.windows(4).any(|window| window == b"\r\n\r\n") {
            break;
        }
        if request.len() > 64 * 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "request headers exceeded 64 KiB",
            ));
        }
    }
    Ok(request)
}

fn parse_page_request(request: &[u8]) -> io::Result<PageRequest> {
    let request = str::from_utf8(request)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    let request_line = request
        .split("\r\n")
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing request line"))?;
    let mut fields = request_line.split_ascii_whitespace();
    let method = fields
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing request method"))?;
    if method != "GET" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("unsupported request method: {method}"),
        ));
    }
    let target = fields
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing request target"))?;
    let request_url = Url::parse(target)
        .or_else(|_| Url::parse(&format!("http://fixture{target}")))
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;

    let mut start_index = None;
    let mut limit = None;
    for (name, value) in request_url.query_pairs() {
        let destination = if name.eq_ignore_ascii_case("StartIndex") {
            &mut start_index
        } else if name.eq_ignore_ascii_case("Limit") {
            &mut limit
        } else {
            continue;
        };
        if destination.is_some() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("duplicate {name} query parameter"),
            ));
        }
        *destination = Some(value.parse::<u32>().map_err(|error| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("invalid {name} query parameter: {error}"),
            )
        })?);
    }

    let start_index = start_index.ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidData, "request is missing StartIndex")
    })?;
    let limit = limit
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "request is missing Limit"))?;
    if limit == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "request Limit must be positive",
        ));
    }
    Ok(PageRequest { start_index, limit })
}

async fn write_page_response(
    stream: &mut tokio::net::TcpStream,
    delay: Duration,
    overview_bytes: usize,
) -> io::Result<PageRequest> {
    let request_headers = read_request_headers(stream).await?;
    let request = parse_page_request(&request_headers)?;
    let response_body = items_response(request, overview_bytes)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    if !delay.is_zero() {
        sleep(delay).await;
    }
    let header = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        response_body.len()
    );
    stream.write_all(header.as_bytes()).await?;
    stream.write_all(&response_body).await?;
    stream.shutdown().await?;
    Ok(request)
}

async fn serve_pages(
    listener: TcpListener,
    responses: usize,
    delay: Duration,
    overview_bytes: usize,
) -> io::Result<Vec<PageRequest>> {
    let mut connections = JoinSet::new();
    for _ in 0..responses {
        let (mut stream, _) = listener.accept().await?;
        connections
            .spawn(async move { write_page_response(&mut stream, delay, overview_bytes).await });
    }
    let mut requested_pages = Vec::with_capacity(responses);
    while let Some(result) = connections.join_next().await {
        requested_pages.push(result??);
    }
    Ok(requested_pages)
}

fn client_for(address: std::net::SocketAddr) -> Result<EmbyClient, Box<dyn std::error::Error>> {
    let profile = ServerProfile::with_transport_security(
        "Yanami performance fixture",
        Url::parse(&format!("http://{address}"))?,
        TransportSecurity::AllowInsecureHttp,
    )?;
    Ok(EmbyClient::with_session(
        profile,
        ClientIdentity::yanami("performance-probe"),
        "fixture-user",
        SecretString::from("fixture-token"),
    )?)
}

async fn sequential_requests(
    request_count: usize,
    delay: Duration,
    page_size: u32,
    overview_bytes: usize,
) -> Result<SequentialRun, Box<dyn std::error::Error>> {
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    let address = listener.local_addr()?;
    let server = tokio::spawn(serve_pages(listener, request_count, delay, overview_bytes));
    let client = client_for(address)?;
    let mut samples = Vec::with_capacity(request_count);
    let mut expected_pages = Vec::with_capacity(request_count);
    let mut seen_ids = HashSet::new();
    for index in 0..request_count {
        let request = PageRequest {
            start_index: u32::try_from(index.saturating_mul(page_size as usize))
                .unwrap_or(u32::MAX),
            limit: page_size,
        };
        expected_pages.push(request);
        let started = Instant::now();
        let result = client
            .items(&ItemQuery {
                start_index: request.start_index,
                limit: request.limit,
                recursive: true,
                ..ItemQuery::default()
            })
            .await?;
        validate_page_response(request, &result, &mut seen_ids)?;
        samples.push(started.elapsed().as_secs_f64() * 1000.0);
    }
    let observed_pages = server.await??;
    validate_requested_pages(&expected_pages, &observed_pages)?;
    Ok(SequentialRun {
        samples,
        requested_pages: observed_pages,
        unique_item_count: seen_ids.len(),
    })
}

fn validate_page_response(
    request: PageRequest,
    response: &ItemsResult,
    seen_ids: &mut HashSet<String>,
) -> Result<(), String> {
    if response.total_record_count != FULL_LIBRARY_ITEMS as u64 {
        return Err(format!(
            "fixture returned TotalRecordCount={}, expected {FULL_LIBRARY_ITEMS}",
            response.total_record_count
        ));
    }
    if response.start_index != u64::from(request.start_index) {
        return Err(format!(
            "fixture returned StartIndex={}, expected {}",
            response.start_index, request.start_index
        ));
    }
    let absolute_start = request.start_index as usize;
    let expected_count =
        (request.limit as usize).min(FULL_LIBRARY_ITEMS.saturating_sub(absolute_start));
    if response.items.len() != expected_count {
        return Err(format!(
            "fixture returned {} items for offset {}, expected {expected_count}",
            response.items.len(),
            request.start_index
        ));
    }
    for (relative_index, item) in response.items.iter().enumerate() {
        let expected_id = fixture_item_id(absolute_start + relative_index);
        if item.id != expected_id {
            return Err(format!(
                "fixture returned item ID {} at offset {}, expected {expected_id}",
                item.id,
                absolute_start + relative_index
            ));
        }
        if !seen_ids.insert(item.id.clone()) {
            return Err(format!("fixture returned duplicate item ID {}", item.id));
        }
    }
    Ok(())
}

fn validate_requested_pages(
    expected_pages: &[PageRequest],
    observed_pages: &[PageRequest],
) -> Result<(), String> {
    let mut expected_pages = expected_pages.to_vec();
    expected_pages.sort_unstable();
    let mut observed_pages = observed_pages.to_vec();
    observed_pages.sort_unstable();
    if observed_pages != expected_pages {
        return Err(format!(
            "fixture observed page requests {observed_pages:?}, expected {expected_pages:?}"
        ));
    }
    Ok(())
}

async fn full_sync(delay: Duration) -> Result<f64, Box<dyn std::error::Error>> {
    let pages = FULL_LIBRARY_ITEMS.div_ceil(FULL_LIBRARY_PAGE_SIZE);
    let started = Instant::now();
    let run = sequential_requests(pages, delay, FULL_LIBRARY_PAGE_SIZE as u32, 0).await?;
    if run.requested_pages.len() != pages || run.unique_item_count != FULL_LIBRARY_ITEMS {
        return Err(format!(
            "full-sync fixture coverage mismatch: pages={}, unique_items={}",
            run.requested_pages.len(),
            run.unique_item_count
        )
        .into());
    }
    Ok(started.elapsed().as_secs_f64() * 1000.0)
}

async fn repeated_full_sync(
    delay: Duration,
    count: usize,
) -> Result<Vec<f64>, Box<dyn std::error::Error>> {
    let mut samples = Vec::with_capacity(count);
    for _ in 0..count {
        samples.push(full_sync(delay).await?);
    }
    Ok(samples)
}

async fn concurrent_requests(
    concurrency: usize,
    delay: Duration,
    page_size: u32,
    overview_bytes: usize,
) -> Result<Vec<f64>, Box<dyn std::error::Error>> {
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    let address = listener.local_addr()?;
    let server = tokio::spawn(serve_pages(listener, concurrency, delay, overview_bytes));
    let client = client_for(address)?;
    let mut tasks = JoinSet::new();
    let mut expected_pages = Vec::with_capacity(concurrency);
    for index in 0..concurrency {
        let request = PageRequest {
            start_index: u32::try_from(index.saturating_mul(page_size as usize))
                .unwrap_or(u32::MAX),
            limit: page_size,
        };
        expected_pages.push(request);
        let request_client = client.clone();
        tasks.spawn(async move {
            let started = Instant::now();
            let result = request_client
                .items(&ItemQuery {
                    start_index: request.start_index,
                    limit: request.limit,
                    recursive: true,
                    ..ItemQuery::default()
                })
                .await;
            (request, started.elapsed().as_secs_f64() * 1000.0, result)
        });
    }
    let mut samples = Vec::with_capacity(concurrency);
    let mut seen_ids = HashSet::new();
    while let Some(result) = tasks.join_next().await {
        let (request, elapsed, response) = result?;
        let response = response?;
        validate_page_response(request, &response, &mut seen_ids)?;
        samples.push(elapsed);
    }
    let observed_pages = server.await??;
    validate_requested_pages(&expected_pages, &observed_pages)?;
    let expected_item_count = concurrency.saturating_mul(page_size as usize);
    if seen_ids.len() != expected_item_count {
        return Err(format!(
            "concurrent fixture returned {} unique IDs, expected {expected_item_count}",
            seen_ids.len()
        )
        .into());
    }
    Ok(samples)
}

async fn concurrent_queue_wait(
    concurrency: usize,
    delay: Duration,
    page_size: u32,
    overview_bytes: usize,
) -> Result<Vec<f64>, Box<dyn std::error::Error>> {
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    let address = listener.local_addr()?;
    let batch_started = Instant::now();
    let server = tokio::spawn(async move {
        let mut handlers = JoinSet::new();
        let mut accepted_ms = Vec::with_capacity(concurrency);
        for _ in 0..concurrency {
            let (mut stream, _) = listener.accept().await?;
            accepted_ms.push(batch_started.elapsed().as_secs_f64() * 1000.0);
            handlers.spawn(
                async move { write_page_response(&mut stream, delay, overview_bytes).await },
            );
        }
        let mut requested_pages = Vec::with_capacity(concurrency);
        while let Some(result) = handlers.join_next().await {
            requested_pages.push(result??);
        }
        Ok::<_, io::Error>(QueueServerEvidence {
            accepted_ms,
            requested_pages,
        })
    });

    let client = client_for(address)?;
    let mut requests = JoinSet::new();
    let mut expected_pages = Vec::with_capacity(concurrency);
    for index in 0..concurrency {
        let request = PageRequest {
            start_index: u32::try_from(index.saturating_mul(page_size as usize))
                .unwrap_or(u32::MAX),
            limit: page_size,
        };
        expected_pages.push(request);
        let request_client = client.clone();
        requests.spawn(async move {
            let response = request_client
                .items(&ItemQuery {
                    start_index: request.start_index,
                    limit: request.limit,
                    recursive: true,
                    ..ItemQuery::default()
                })
                .await;
            (request, response)
        });
    }
    let mut seen_ids = HashSet::new();
    while let Some(result) = requests.join_next().await {
        let (request, response) = result?;
        validate_page_response(request, &response?, &mut seen_ids)?;
    }
    let server_evidence = server.await??;
    validate_requested_pages(&expected_pages, &server_evidence.requested_pages)?;
    let expected_item_count = concurrency.saturating_mul(page_size as usize);
    if seen_ids.len() != expected_item_count {
        return Err(format!(
            "queue fixture returned {} unique IDs, expected {expected_item_count}",
            seen_ids.len()
        )
        .into());
    }
    Ok(server_evidence.accepted_ms)
}

fn metric(id: &'static str, samples: Vec<f64>, attributes: Value) -> Measurement {
    Measurement {
        id: id.to_owned(),
        unit: "ms".to_owned(),
        samples,
        attributes,
    }
}

async fn collect_backend(profile: &str) -> Result<Vec<Measurement>, Box<dyn std::error::Error>> {
    let page_runs = if matches!(profile, "pr" | "pullrequest") {
        8
    } else {
        30
    };
    let mut measurements = vec![
        metric(
            "backend.component.fixture.page200.synthetic_5ms_ms",
            sequential_requests(page_runs, Duration::from_millis(5), 200, 0)
                .await?
                .samples,
            json!({"syntheticServerDelayMs":5,"items":200,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ),
        metric(
            "backend.component.fixture.page200.synthetic_50ms_ms",
            sequential_requests(page_runs, Duration::from_millis(50), 200, 0)
                .await?
                .samples,
            json!({"syntheticServerDelayMs":50,"items":200,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ),
        metric(
            "backend.component.fixture.concurrent32.request_ms",
            concurrent_requests(32, Duration::from_millis(5), 200, 0).await?,
            json!({"concurrency":32,"itemsPerRequest":200,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ),
        metric(
            "backend.component.fixture.concurrent32.accept_delay_ms",
            concurrent_queue_wait(32, Duration::from_millis(5), 200, 0).await?,
            json!({"concurrency":32,"definition":"batch dispatch to fixture server accept","evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ),
        metric(
            "backend.component.fixture.payload_64k.synthetic_50ms_ms",
            sequential_requests(page_runs, Duration::from_millis(50), 20, 3 * 1024)
                .await?
                .samples,
            json!({"syntheticServerDelayMs":50,"payloadClass":"approximately-64KiB","evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ),
        metric(
            "backend.component.fixture.payload_1mib.synthetic_50ms_ms",
            sequential_requests(page_runs, Duration::from_millis(50), 50, 20 * 1024)
                .await?
                .samples,
            json!({"syntheticServerDelayMs":50,"payloadClass":"approximately-1MiB","evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ),
    ];

    if !matches!(profile, "pr" | "pullrequest") {
        measurements.push(metric(
            "backend.component.fixture.full_sync_110k.loopback_ms",
            repeated_full_sync(Duration::ZERO, 3).await?,
            json!({"items":FULL_LIBRARY_ITEMS,"pageSize":FULL_LIBRARY_PAGE_SIZE,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ));
        measurements.push(metric(
            "backend.component.fixture.page1000.synthetic_5ms_ms",
            sequential_requests(page_runs, Duration::from_millis(5), 1_000, 0)
                .await?
                .samples,
            json!({"items":1000,"syntheticServerDelayMs":5,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ));
    }
    if matches!(profile, "nightly" | "weekly" | "release") {
        measurements.push(metric(
            "backend.component.fixture.full_sync_110k.synthetic_5ms_ms",
            repeated_full_sync(Duration::from_millis(5), 3).await?,
            json!({"items":FULL_LIBRARY_ITEMS,"pageSize":FULL_LIBRARY_PAGE_SIZE,"syntheticServerDelayMs":5,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ));
        measurements.push(metric(
            "backend.component.fixture.full_sync_110k.synthetic_50ms_ms",
            repeated_full_sync(Duration::from_millis(50), 3).await?,
            json!({"items":FULL_LIBRARY_ITEMS,"pageSize":FULL_LIBRARY_PAGE_SIZE,"syntheticServerDelayMs":50,"evidence":"fixture-component-observation","server":"offset-aware-in-process-loopback"}),
        ));
    }
    Ok(measurements)
}

#[allow(clippy::too_many_arguments)]
fn write_manifest(
    path: &Path,
    profile: &str,
    mode: &str,
    fixture: &FixtureEvidence,
    measurements: Vec<Measurement>,
    suites: &[String],
    invariants: &[Value],
    search_fixture_details: Option<Value>,
) -> Result<(), Box<dyn std::error::Error>> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        fs::create_dir_all(parent)?;
    }
    let metrics: Vec<_> = measurements
        .into_iter()
        .map(|measurement| {
            json!({
                "id": measurement.id,
                "unit": measurement.unit,
                "samples": measurement.samples,
                "attributes": measurement.attributes,
            })
        })
        .collect();
    let reference_match = env::var("YANAMI_PERF_REFERENCE_MATCH")
        .is_ok_and(|value| value.eq_ignore_ascii_case("true") || value == "1");
    let mut fixture_details = json!({
        "documentCount": fixture.item_count,
        "queryCount": fixture.query_count,
        "usage": "scale and pagination integrity evidence for fixture/component transport observations only",
        "paginationModel": "response StartIndex and item IDs are derived from each observed request offset",
        "responseContent": "synthetic BaseItem records; pinned F110K files provide scale and hash evidence but are not served as production catalog data"
    });
    if let Some(search_details) = search_fixture_details {
        fixture_details = search_details;
    }
    let mut fixture_json = json!({
        "id": fixture.id,
        "version": "1",
        "validated": fixture.validated,
        "details": fixture_details
    });
    if let Some(sha256) = &fixture.sha256 {
        fixture_json["sha256"] = Value::String(sha256.clone());
    }
    let manifest = json!({
        "schemaVersion": "1.0",
        "runId": Uuid::new_v4().to_string(),
        "profile": contract_profile(profile),
        "startedAtUtc": Utc::now().to_rfc3339(),
        "mode": mode,
        "suites": suites,
        "environment": {
            "fingerprint": env::var("YANAMI_PERF_MACHINE_FINGERPRINT")
                .unwrap_or_else(|_| String::from("unclassified")),
            "referenceMatch": reference_match,
            "details": {
                "os": env::consts::OS,
                "architecture": env::consts::ARCH,
            }
        },
        "fixtures": [fixture_json],
        "metrics": metrics,
        "invariants": invariants
    });
    fs::write(path, serde_json::to_vec_pretty(&manifest)?)?;
    Ok(())
}

#[tokio::main(flavor = "multi_thread", worker_threads = 4)]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arguments = parse_arguments().map_err(io::Error::other)?;
    let fixture = validate_f110k_fixture()?;
    let suites = selected_probe_suites();
    if suites.is_empty() {
        return Err(
            "YANAMI_PERF_SUITES did not select Search or Backend for the Rust probe".into(),
        );
    }
    let mut measurements = Vec::new();
    let mut invariants = Vec::new();
    let mut search_fixture_details = None;
    if suites.iter().any(|suite| suite == "search") {
        if fixture.id != "F110K-v1" || !fixture.validated {
            return Err("the production Search probe requires a validated F110K-v1 fixture".into());
        }
        let fixture_directory = env::var("YANAMI_PERF_FIXTURE_DIR")
            .map(PathBuf::from)
            .map_err(|_| "the production Search probe requires YANAMI_PERF_FIXTURE_DIR")?;
        let search = search_probe::collect(&fixture_directory, &arguments.profile)?;
        measurements.extend(search.measurements);
        invariants.extend(search.invariants);
        search_fixture_details = Some(search.fixture_details);
    }
    if suites.iter().any(|suite| suite == "backend") {
        measurements.extend(collect_backend(&arguments.profile).await?);
        invariants.extend([
            json!({"id":"backend.fixture.pagination_offsets_observed","passed":true,"scope":"fixture-component-observation"}),
            json!({"id":"backend.fixture.cross_page_ids_unique","passed":true,"scope":"fixture-component-observation"}),
            json!({"id":"backend.component.fixture_requests_succeeded","passed":true,"scope":"fixture-component-observation"}),
        ]);
    }
    write_manifest(
        &arguments.output,
        &arguments.profile,
        &arguments.mode,
        &fixture,
        measurements,
        &suites,
        &invariants,
        search_fixture_details,
    )?;
    println!("Rust performance manifest: {}", arguments.output.display());
    Ok(())
}

fn selected_probe_suites() -> Vec<String> {
    let requested = env::var("YANAMI_PERF_SUITES").ok();
    probe_suites(
        requested.as_deref(),
        env::var_os("YANAMI_PERF_FIXTURE_DIR").is_some(),
    )
}

fn probe_suites(requested: Option<&str>, fixture_available: bool) -> Vec<String> {
    let mut suites = Vec::new();
    for suite in ["search", "backend"] {
        let selected = requested.map_or_else(
            || suite == "backend" || fixture_available,
            |value| {
                value
                    .split(',')
                    .map(str::trim)
                    .any(|candidate| candidate.eq_ignore_ascii_case(suite))
            },
        );
        if selected {
            suites.push(suite.to_owned());
        }
    }
    suites
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_requested_offset_and_limit() {
        let request = b"GET /Users/fixture-user/Items?Recursive=true&StartIndex=1200&Limit=200 HTTP/1.1\r\nHost: fixture\r\n\r\n";

        assert_eq!(
            parse_page_request(request).expect("request should parse"),
            PageRequest {
                start_index: 1_200,
                limit: 200,
            }
        );
    }

    #[test]
    fn response_uses_requested_offset_for_start_index_and_ids() {
        let request = PageRequest {
            start_index: 1_200,
            limit: 3,
        };

        let response: ItemsResult = serde_json::from_slice(
            &items_response(request, 0).expect("fixture response should serialize"),
        )
        .expect("fixture response should deserialize");

        assert_eq!(response.start_index, 1_200);
        assert_eq!(response.total_record_count, FULL_LIBRARY_ITEMS as u64);
        assert_eq!(
            response
                .items
                .iter()
                .map(|item| item.id.as_str())
                .collect::<Vec<_>>(),
            ["fixture-001200", "fixture-001201", "fixture-001202"]
        );
    }

    #[test]
    fn search_only_suite_selects_the_rust_production_probe_without_backend_work() {
        assert_eq!(probe_suites(Some("Search"), true), ["search"]);
        assert_eq!(probe_suites(Some("Interaction,Search"), true), ["search"]);
    }

    #[tokio::test]
    async fn sequential_probe_observes_offsets_and_unique_cross_page_ids() {
        let run = sequential_requests(3, Duration::ZERO, 5, 0)
            .await
            .expect("fixture run should succeed");

        assert_eq!(run.samples.len(), 3);
        assert_eq!(run.unique_item_count, 15);
        let mut requested_pages = run.requested_pages;
        requested_pages.sort_unstable();
        assert_eq!(
            requested_pages,
            [
                PageRequest {
                    start_index: 0,
                    limit: 5,
                },
                PageRequest {
                    start_index: 5,
                    limit: 5,
                },
                PageRequest {
                    start_index: 10,
                    limit: 5,
                },
            ]
        );
    }

    #[test]
    fn manifest_labels_loopback_results_as_non_canonical_observations() {
        let temporary_directory = tempfile::tempdir().expect("temporary directory should exist");
        let manifest_path = temporary_directory.path().join("backend.json");
        let fixture = FixtureEvidence {
            id: "BackendSynthetic110K-v1",
            sha256: None,
            validated: true,
            item_count: FULL_LIBRARY_ITEMS,
            query_count: 0,
        };

        write_manifest(
            &manifest_path,
            "pr",
            "collect",
            &fixture,
            Vec::new(),
            &["backend".to_owned()],
            &[
                json!({"id":"backend.fixture.pagination_offsets_observed","passed":true,"scope":"fixture-component-observation"}),
                json!({"id":"backend.fixture.cross_page_ids_unique","passed":true,"scope":"fixture-component-observation"}),
            ],
            None,
        )
        .expect("manifest should be written");
        let manifest: Value =
            serde_json::from_slice(&fs::read(manifest_path).expect("manifest should be readable"))
                .expect("manifest should contain JSON");
        let invariants = manifest["invariants"]
            .as_array()
            .expect("manifest should contain invariants");
        let invariant_ids = invariants
            .iter()
            .filter_map(|invariant| invariant["id"].as_str())
            .collect::<Vec<_>>();

        assert!(!invariant_ids.contains(&"backend.no_request_errors"));
        assert!(!invariant_ids.contains(&"backend.no_duplicate_pages"));
        assert!(invariants.iter().all(|invariant| {
            invariant["scope"].as_str() == Some("fixture-component-observation")
        }));
    }
}
