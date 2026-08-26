# Search poster hydration report

Date: 2026-08-24

## Outcome

The local full-library search now uses a two-phase poster pipeline:

1. The bounded grouped search response (up to 50 title hits and 50 episode hits) derives stable `image://yanami` URLs from the existing cache-key policy. This phase does not create directories, inspect image files, reserve download-dedupe entries, or spawn image tasks. Episode descriptors explicitly use the parent Series image owner and tag when available.
2. After the published query remains unchanged for 150 ms, `SearchCoordinator` sends the exact published image descriptors to a new ABI v3 hydration export on a separate one-thread worker pool. Rust validates the bounded descriptor set, rechecks the active session, and schedules cache misses through the existing dedupe set and 12-permit image semaphore.

The hydration call does not query the media catalog or perform synchronous Emby requests, so SQLite search capacity and the C++ search worker remain reserved for the search hot lane.

## Cancellation and stale-result policy

- A new query, session fence, session commit, or shutdown stops the pending delay and drops the latest queued hydration request.
- At most one hydration bridge call is active and one stable replacement is retained. Repeated keystrokes cannot build an unbounded native queue.
- Query generation, session generation, and `publishedQuery` must all still match before a hydration completion is accepted for diagnostics.
- Hydration never publishes, patches, or clears `MediaStore`; stale completions therefore cannot commit UI state.
- Downloads that have already been admitted remain governed by the existing cache-path dedupe and semaphore policy. Session replacement and backend shutdown retain the existing application-wide task cancellation behavior.

## Contract changes

- ABI remains version 3 on both Rust and C++ sides.
- Added required export: `yanami_backend_catalog_search_hydrate_images`.
- The request is a JSON object containing the stable query and at most 100 `{ itemId, itemType, imageTag }` descriptors. These IDs and types describe the selected image owner rather than necessarily the playable result entity.
- The production artifact symbol policy requires the new export.

## Validation

The following checks passed with the Windows GNU/UCRT toolchain:

- `cargo fmt --all -- --check`
- `cargo test -p yanami-application -p yanami-desktop-bridge` — 54 application tests and 7 bridge tests passed.
- `cargo clippy -p yanami-application -p yanami-desktop-bridge --all-targets -- -D warnings`
- Release build targets `yanami-desktop` and `yanami-backend-architecture-tests`.
- CTest `yanami-production-artifact-policy` and `yanami-backend-architecture-tests` — 2/2 passed.
- CTest `yanami-desktop-build-runtime-smoke` passed against the direct build tree.
- PE export inspection confirmed ABI version, search, and search-hydration symbols in the built bridge DLL.
- `git diff --check`

The Rust tests cover stable URL planning without filesystem or download admission, parent-Series image ownership with Episode fallback, propagation into card presentation, typed hydration JSON, descriptor validation, and the bounded two-section result contract. The C++ architecture test covers the 150 ms single-shot delay, generation/published-query fence, dedicated hydration pool, and bridge call boundary.

## Remaining live check

A signed-in real-server smoke test should confirm that an uncached search card resolves through `AsyncImageProvider` after the delayed hydration call. This report does not claim that external-server check was performed.
