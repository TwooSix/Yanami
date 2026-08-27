# Yanami Performance Gate v1

This directory defines performance evaluation policy. It does not prescribe or
implement search indexes, startup shortcuts, rendering changes, or playback
optimizations. Any implementation that satisfies the same observable contract
is acceptable.

## Automation boundary

The Windows job in Core CI runs deterministic correctness and catastrophe smoke
checks after its existing desktop build. There is no separate performance
workflow and no self-hosted, scheduled, or workflow-dispatch path: a
hosted/offscreen renderer cannot certify a physical GPU, native Presents,
decoded playback, or Anime4K output, and a repository without a maintained lab
runner must not advertise permanently skipped jobs as release gates.

The strict `Lab`, `Nightly`, `Weekly`, and `Release` profiles remain local
evidence evaluators for an operator-controlled machine. Their contracts are
useful for release acceptance, but GitHub Actions never substitutes hosted
measurements for the required raw playback, pixel, Present, and GPU evidence.

## What is versioned

- `slo/slo-v1.json` is the non-rolling absolute contract for search, backend,
  interaction, playback, danmaku, and startup.
- `contracts/` contains the JSON contracts for raw probe events, raw run
  manifests, and the evaluated result.
- `fixtures/` pins deterministic workload semantics and hashes. F110K contains
  10,000 Movie/Series records, 100,000 Episode records, and 20,000 queries. Its
  probe-only oracle derives searchable Season entities from episode metadata;
  that derivation is not a claim about the production catalog projection.
  `DanmakuDensity-v1` contains a deterministic 100,000-comment timeline with a
  fixed 20,000-comment hosted slice, sustained and burst density windows, mixed
  text/mode/color cases, and an independent eligible-comment oracle.
- `network/network-profiles-v1.json` defines the controlled loopback, LAN, and
  WAN conditions. Real-internet observations cannot satisfy a hard gate.
- `policy/calibration-v1.json` controls `collect`, `debt`, and `enforce` modes.
- `baselines/b0.json` is intentionally `not-captured` until the two-week lab
  calibration is complete. `bmain.json` is the 20-green-main trend window and
  is never an acceptance threshold.

The current calibration policy starts in `collect` for PR, Lab, Nightly, and
Weekly runs. A missed continuous SLO is therefore reported as performance
debt, while a measured correctness invariant still fails immediately. Release
is explicitly overridden to `enforce`, so absent P0 evidence or performance
debt cannot produce a releasable green result. Invalid hashes, malformed
contracts, or invalid strict-lab evidence are infrastructure failures, not
product passes or failures.
Strict percentile decisions also enforce profile-specific sample counts.
Search metrics opt into the 5% CV rule only when their raw samples are repeated
homogeneous operation rounds (`index_open`, full derived-index rebuild, and
isolated incremental-1000). `index_open` retains the first populated reopen as
a non-gating diagnostic, then applies CV to ten steady reopens after that one
fixed warmup without forcibly clearing the operating-system page cache.
Incremental writer latency is measured in ten isolated rounds. Each round
restores the same completed 140,000-entity snapshot containing one fixed
1000-row baseline marker, then replaces that marker through the public
`upsert_page` API. The separate query-during-incremental scenario runs ten
concurrent rounds and retains writer-under-reader duration only as a diagnostic.
Mixed query-category or mixed writer-phase request samples
such as hot, cold, and query-during-incremental latency remain subject to their
absolute percentile SLO but are not meaningful raw-sample CV evidence. An
opted-in metric with fewer than the reproducibility rule's required rounds is
reported as a metric-level deferred decision, never a silent pass; Release P0
turns that shortfall into a blocking failure. Warm-start latency
requires CV at most 5%, while accumulated cold starts allow at most 10%.
Cold-start p95 becomes eligible only after 20
same-fingerprint samples have been merged; p99 remains explicitly deferred
until 50. The runner accepts multiple raw manifests through repeated
`-InputPath` values and concatenates samples only when their profile, fixture,
and environment evidence remain compatible. The fixed-lab orchestrator owns
retaining those real-reboot manifests between runs.

The Rust loopback fake server derives every response `StartIndex` and item ID
from the observed request offset, then checks the requested offsets, total
coverage, and cross-page ID uniqueness. It reports only
`backend.component.fixture.*` metrics with
`evidence: fixture-component-observation`; these exercise transport and
deserialization code but cannot satisfy canonical backend correctness or
LAN/WAN evidence. A strict network result must use the versioned profile
outside the measured process and report `evidence: controlled-network`.

Likewise, the desktop component probe keeps proxy updates, generic request
tokens, isolated worker-lane starts, and zero-delay event-loop callbacks under
`desktop.*` or `search.component.*` observation names. Its F110K path directly
injects fixture rows, derived entities, and aliases into an in-memory model; it
does not traverse the production catalog query, presentation mapping, search
scheduler, or UI. Consequently `search.component.fixture.*` metrics and
invariants carry `evidence: fixture-component-observation` and remain
diagnostic only.

The Rust probe is the canonical production MediaCatalog component probe. It
loads the validated 110,000 source rows, independently derives the 30,000
Season entities required by the cache and season-intent oracle, writes all
140,000 catalog/index entities through public `MediaCatalog::begin_sync`, `upsert_page`, and
`complete_sync` calls, and queries only through public `MediaCatalog::search`.
Only Movie/Series and Episode rows are result-eligible: the production contract
publishes the title group first and then the episode group, with up to 50 rows
per group. A season-intent query is judged against matching Episode rows rather
than exposing a Season row as a third product result type. F110K-v1 retains its
hashed `rank1` Season metadata only as the independent series/season intent
descriptor; it is not interpreted as a published row ID.
It supplies the canonical hot/cold query, index-open, 110K-source rebuild,
incremental-1000, query-during-incremental, disk, RSS, exact-rank, recall, MRR,
and NDCG IDs. Its 17 per-category correctness observations remain in the raw
manifest so an aggregate ratio cannot hide one broken query class. The hot
latency sample count follows the profile contract: 100 hosted, 500 Lab, and the
complete 20,000-query corpus for Nightly, Weekly, and Release.
Nightly rebuild evidence deletes only the disposable published posting sidecar,
then times ten isolated homogeneous public `MediaCatalog::open` rounds rebuilding
the complete 140,000-entity derived index from retained raw SQLite facts. After
each timed open, the probe verifies `cached_count=140000` and exact result IDs,
`total_matches`, and `has_more` for one query from each of the 17 categories.
A separate three-round concurrent scenario observes a builder temporary-file
start fence and requires at least one exact old-generation query after that
fence and before publication in every round; its writer durations are
diagnostic and never enter rebuild CV. Canonical steady RSS is captured after
one initial build and representative warmup, before repeated-open/rebuild
stress. Sampled phase peaks and stress residuals remain diagnostic, while the
OS-maintained `PeakWorkingSet64`/`VmHWM` high-water increment covers short-lived
build peaks that polling could miss. That authoritative increment subtracts
the pre-catalog current working set from the final lifetime high-water; an
earlier lifetime high-water is retained only as a diagnostic and cannot mask a
later build peak.

Every canonical Search metric and invariant also declares an exact evidence
label and expected producer kind. The evaluator accepts those IDs only when the
current run-gate process passes a trusted in-memory attestation for a matching
locally executed probe artifact, its SHA-256, run ID, and machine fingerprint.
Manifest provenance is matched against that ledger but never establishes trust
by itself. A copied canonical ID, a forged provenance object, an evidence label
by itself, or fixture-component output cannot satisfy the production contract.

The MediaCatalog probe deliberately does not certify desktop scheduling or
presentation. The desktop component probe now drives the production
`SearchCoordinator` through a delayed fake bridge and emits, for one
run/process/session identity, `query_submitted`, `worker_started`,
`worker_finished`, `publish_committed`, and `request_discarded` events carrying
the session generation, request generation, normalized query, and queue depth.
The resulting canonical invariant details carry
`evidence: production-search-coordinator-delayed-fake` and prove that no
superseded or prior-session generation commits, the last submitted query
publishes its complete bounded grouped snapshot exactly once, at most one request is active and
one is queued, and published IDs contain neither duplicates nor another
session's rows. That desktop evidence satisfies
`search.no_stale_commit`, `search.final_query_complete`,
`search.queue_bounded`, and
`search.no_duplicates_or_cross_session_results`. The strict external-present
harness separately owns input-to-model, input-to-present, and frame-ratio IDs.
The Rust probe never duplicates or fabricates the four scheduler/session
invariants. An enforced strict Search gate still requires the separate
presentation artifacts.

The hosted Danmaku probe is deliberately a catastrophe-only smoke. It loads the
fixed 20,000-comment slice into the production `DanmakuOverlay` under Qt's
1920x1080 offscreen software renderer and checks timeline preparation, dense
seek/update completion, bounded frame candidates, fixture/oracle integrity,
latest-generation semantics, and bounded queue/delegate state. Its metrics are
named `danmaku.hosted_smoke.*` and carry hosted-only evidence; they can block a
gross freeze, crash, malformed timeline, stale result, or unbounded-growth
regression, but cannot certify native GPU rendering, a real displayed pixel, or
any strict Danmaku SLO.

The independently routable `upscaling` suite follows the same fail-closed
boundary. Pull requests build and run the native
`yanami-upscaling-perf-probe` over `UpscalingCapability-v1`. The probe links the
production capability oracle, catalog resolver, and performance-protection
policy instead of maintaining a script-side copy. It exercises
the Anime4K renderer-compatibility policy, the performance/balanced/quality
plans, pinned artifact metadata and shader order, and bounded one-way
performance fallback.
Its two latency metrics use
`fixture-component-observation`, and every observation explicitly records
`gpuCertified=false` and `presentCertified=false`. This hosted result is useful
for freezes and policy regressions only. The fixture exposes exactly one
production provider, `anime4k`, which is eligible on a hardware OpenGL 4.3+
renderer with a 4096+ texture limit. Direct3D, Vulkan, Metal, software
rendering, and insufficient OpenGL remain unsupported. This contract does not
inspect GPU model identities or recommend a preset; preset selection belongs
entirely to the user. The probe rejects any capability result that exposes the
removed adapter-classification or recommendation fields.

Strict upscaling requires `PlaybackMedia-v1` and `UpscalingModelPack-v1` to be
provisioned by a dedicated performance-policy change. The current model-pack
contract and approved measurement normalizer are intentionally
`not-provisioned`, so manual strict certification is blocked rather than
silently downgraded. Once provisioned, the local runner invokes an
absolute-path normalizer whose executable SHA-256 is in the model-pack
allow-list. It discards imported metric/invariant values and accepts only
freshly normalized samples bound to the candidate, fixture set, environment
fingerprint, provider/preset/runtime/model identity, playback fixture, process
ID, QPC window, and hashes of every raw evidence file.

Performance, balanced, and quality are measured as three distinct scenarios;
each scenario is evaluated against the SLO independently before the runner
generates `upscaling.strict_matrix_complete`. The timed window must retain the
requested provider, tier, and shader hashes without fallback or downgrade.
Strict upscaling is separately pinned to the production Qt OpenGL + libmpv
OpenGL path and can certify only Anime4K; it cannot reuse an ordinary
D3D11/offscreen playback certificate. Offscreen
Qt frames, internal `frameSwapped`, a self-described evidence label, or a
hosted resolver timing can never satisfy these metrics.

## Local commands

Run the evaluator and fixture self-tests:

```powershell
pwsh -NoProfile -File scripts/performance/Test-PerfGate.ps1
```

Run the path-independent PR smoke:

```powershell
pwsh -NoProfile -File scripts/performance/run-gate.ps1 `
  -Profile PullRequest `
  -OutputDirectory build/performance/pr `
  -Suites Search,Backend,Interaction,Danmaku,Upscaling
```

When `Startup` or `Interaction` is requested and no raw input manifest is
supplied, the Windows runner also builds `yanami-desktop` from the same CMake
build tree as the discovered component probe and launches it with the opt-in
runtime trace. This prevents a stale desktop binary from satisfying a current
probe run. Startup evidence requires one process/run identity, the
`desktop.runtime` scenario, exactly one copy of every required milestone, and
the declared partial order. The runner-only probe path exits through the normal
Qt event-loop shutdown after its terminal milestone, and the gate requires exit
code 0; a process that crashes after flushing a completion event is rejected.

The PR interaction runtime smoke posts one registered user event to a private
`QObject`, requests one scene-graph update, and verifies that the opt-in
application trace filter pairs it with exactly one later `frameSwapped`
generation without drops or pending work. The event is not a keyboard,
pointer, wheel, touch, or IME event; the evidence also requires Yanami's input
modality and its change-signal count to remain unchanged. Its top-level Qt
dispatch count is a bounded offscreen/software-rendered hook scenario; it can
satisfy the PR long-task smoke only. It is not a substitute for the strict
lab's real interaction workload or external Present evidence. Strict profiles
retain the native window/rendering environment supplied by the lab
orchestrator.

### Two-stage startup lifecycle

Packaged startup uses a native branded launcher and a ready-file handshake.
When the launcher receives `--performance-trace <path>`, the desktop owns and
truncates `<path>` while the launcher writes `<path>.bootstrap.jsonl`. The gate
binds the launcher PID in that sidecar to the desktop PID recorded by both
`desktop_ready` and `handoff_complete`, then validates the desktop trace against
that child PID. The required launcher milestones are:

- `bootstrap_first_visible`: branded launcher visibility candidate,
  `readiness=false`, and `progressSemantic=indeterminate`;
- `desktop_ready`: the desktop's content-ready handshake, not the launcher's
  first frame;
- `handoff_complete`: the launcher's cross-fade/hide boundary, kept separate
  from content readiness.

Each milestone occurs exactly once and in that order. Percentage-like progress
fields are rejected because the launcher has no finite work denominator. The
short handoff animation is reported as its own duration; its end must never be
reported as desktop TTFP. `frameSwapped`, ready-file commit, and launcher
visibility are process-internal candidates. They validate lifecycle ordering,
but do not prove that DWM, WindowServer, or a Linux compositor presented a
frame.

Release package smoke invokes `scripts/performance/Test-BootstrapPackage.ps1`.
It audits that the launcher does not statically import Qt, mpv, SDL, the Rust
bridge, or media libraries; validates the three-milestone sidecar and desktop
trace; and stages a non-responsive fake desktop to require bounded timeout exit
code 70. Windows and macOS exercise their native launcher window paths. Linux
runs the X11 launcher under Xvfb while the Qt child uses the packaged offscreen
backend. These are handshake and dependency-contract checks, not strict visual
performance evidence.

### First-download cold-start A/B sampling

Use a fixed Windows lab and collect complete A-B-A-B rounds from separately
extracted, immutable base and candidate packages. Keep signing status, package
origin, Mark-of-the-Web state, network state, power plan, display, and account
fixture identical. Give every run a fresh experiment-owned APPDATA/LOCALAPPDATA
root. Reset the OS page cache only through the lab's declared reboot or cache
reset policy; deleting application state alone is not a cold binary-page-cache
reset. Nightly/Weekly cold scenarios require at least 20 samples per variant;
Release requires 50 so p99 is meaningful.

For every launch retain both JSONL traces, package and artifact hashes, the
candidate/fixture/machine binding, and an external QPC plus ETW/PresentMon
capture correlated to launcher PID, child PID, and HWND. Report these boundaries
separately:

1. process entry to external launcher first Present;
2. launcher first Present to external desktop content Present;
3. internal `desktop_ready` to `handoff_complete` animation duration;
4. download/open request to process entry, when reputation testing is in scope.

Only boundary 2 can contribute to strict desktop TTFP, and only from external
Present evidence. Local builds, CI extraction, Qt `frameSwapped`, and the ready
file cannot measure Windows SmartScreen reputation delay. SmartScreen must be a
separate signed-artifact experiment using the real download channel and
preserved Mark-of-the-Web; macOS Gatekeeper/quarantine has the same separation.
Do not combine those OS reputation/download delays with in-process startup
latency.

Danmaku strict profiles use the fixed Windows machine at 3840x2160 and 60Hz.
They exercise cached 10,000-, 50,000-, and 100,000-comment loads plus the
versioned steady/burst, seek, pause/resume, controlled-buffering, playback-rate,
toggle, style/filter, resize/fullscreen, and episode-switch scenarios. Each
overlay-enabled resource and playback-impact sample is paired with the same
video, machine, renderer, settings, and time window with the overlay disabled.
Only this paired overlay-off baseline may be used to report incremental CPU,
GPU, RSS, or video dropped-frame cost; an unrelated historical run is not a
valid control.

Strict Danmaku timing is based on actual pixels and native Presents. The lab
correlates QPC scenario milestones, ETW/PresentMon Presents, pixel captures,
mpv playback telemetry, and ETW process counters under one run/process/window
identity. First-visible, disable, style, seek, schedule, pause, resume, and rate
metrics require the external pixel oracle, while frame cadence requires the
native Present stream. A `frameSwapped` signal, an offscreen frame candidate,
or a probe-reported model update cannot be relabeled as either evidence class.
The imported manifest must include the corresponding non-empty raw artifact
beside it, bind each artifact and capture index to the candidate, fixture, and
machine fingerprint, and identify a versioned QPC collector. The runner
recomputes every SHA-256, marks evidence as validated only after that check,
and retains the bytes in the output bundle; an `evidence` string alone is
always insufficient.

These gates constrain observable results only. They do not require a particular
timeline container, sorting strategy, lane allocator, batching policy, cache,
glyph atlas, scene-graph node layout, shader, or thread model. A change is free
to use any implementation as long as it satisfies the same hashed fixture,
pixel oracle, paired-baseline, correctness, latency, frame, and resource
contracts.

Every invocation writes these files, including on failure:

- `performance-result.json`
- `perf-results.xml`
- `perf-summary.md`
- raw probe manifests and traces when a probe ran

The exit codes are `0` for `pass` or `debt`, `1` for a product `fail`, and `2`
for `infra-invalid`.

## Fixed Windows lab contract

The lab administrator must pin and inventory the fields in
`environments/windows-reference-v1.json`. Set
`YANAMI_PERF_REFERENCE_MATCH=1` only after the current CPU, GPU and driver,
storage, Windows build, power plan, Qt, Rust, memory, and display configuration
match that inventory. A lab run without this attestation is `infra-invalid`.

F110K and `DanmakuDensity-v1` are generated and hash-checked automatically.
The Danmaku generator is `scripts/performance/New-DanmakuFixture.ps1`; its
versioned seed, UTF-8/LF byte contract, comment distribution, file SHA-256, and
aggregate fixture SHA-256 are pinned by
`fixtures/danmaku-density-v1.manifest.json`. The runner regenerates only its own
managed fixture directory and rejects mismatched or self-certified imported
hashes. Strict Danmaku runs additionally require the fixed playback fixture.

Playback assets are not generated or transcoded during a run: the lab must
provision the files in `playback-media-v1.manifest.json`, validate their hashes,
and provide the raw playback manifest with `-InputPath`. That policy currently
has state `not-provisioned`; a probe that claims validated playback evidence
before a dedicated policy change pins every required hash is rejected as
`infra-invalid`, rather than being allowed to create a false pass.

Qt `frameSwapped` and mpv restart events are internal correlation candidates.
They can validate hook ordering, but they cannot satisfy an SLO marked
`evidence: external-present`. Strict startup, interaction, first-video-frame,
and Danmaku frame results must come from the lab's QPC plus ETW/PresentMon
collector and declare that evidence in the raw metric attributes. Danmaku pixel
semantics additionally require the correlated external pixel oracle. Missing
strict evidence is never converted into a pass.

There are no fixed-machine jobs in GitHub Actions. Run strict profiles directly
on the isolated reference machine and retain their complete output directories
with the candidate package. A hosted workflow result is never release evidence
for a requirement marked `external-present`, `external-pixel-present`,
`controlled-network`, or GPU-certified.

## Base/head and reruns

The runner never checks out another revision in the user's working tree. A lab
orchestrator collects four complete `PerfResult` artifacts on the same machine
in merge-base/candidate/merge-base/candidate order, then supplies all four in
that exact order:

```powershell
pwsh -NoProfile -File scripts/performance/run-gate.ps1 `
  -Profile Lab `
  -OutputDirectory build/performance/head `
  -Suites Search,Backend,Danmaku,Startup `
  -BaseSha <merge-base> `
  -CandidateSha <candidate> `
  -ComparisonResultPath @(
    '<A1-performance-result.json>',
    '<B1-performance-result.json>',
    '<A2-performance-result.json>',
    '<B2-performance-result.json>'
  )
```

The gate validates four distinct run IDs, strictly increasing completion times,
the `A-B-A-B` commit SHA pattern, matching profile and SLO contract, one machine
fingerprint, identical suite coverage, and identical fixture hashes. It then
rebuilds aggregate A and B manifests from their raw samples and requires at
least one actual comparable metric. A fixed runner can provide the four paths
as a semicolon-separated `YANAMI_PERF_ABAB_RESULT_PATHS` value.

For release acceptance, each source result must itself use the `Release`
profile and contain all strict suites and raw evidence. The final evaluator is:

```powershell
pwsh -NoProfile -File scripts/performance/run-gate.ps1 `
  -Profile Release `
  -Mode enforce `
  -OutputDirectory build/performance/release-acceptance `
  -Suites Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup `
  -BaseSha <merge-base> `
  -CandidateSha <candidate> `
  -ComparisonResultPath @(
    '<A1-release-performance-result.json>',
    '<B1-release-performance-result.json>',
    '<A2-release-performance-result.json>',
    '<B2-release-performance-result.json>'
  )
```

Only `status=pass` together with `relativeComparison.state=evaluated` is a
release acceptance result. A direct Release-profile source run is collection
material, not publication approval by itself.

An ordinary `-BaseResultPath` may be schema-validated for diagnostics, but can
never activate a relative hard gate. If a base SHA is named without four valid
source results, the result explicitly says that relative comparison was not
evaluated. Continuous failures are rerun once;
pre-collected second-attempt manifests can be supplied with `-RetryInputPath`.
Correctness and infrastructure failures are not statistically retried.

Changing SLOs, fixture hashes, reference-machine policy, or locked metrics
requires a dedicated performance-policy change with raw evidence. A failing
product change must not raise its own threshold.
