# Yanami

> **Development status:** Yanami is an early, actively developed prototype. It
> is not a finished product, no stable release is available, and APIs and data
> formats may change without notice.

Yanami is a cross-platform Emby desktop player with a Rust application core, a
Qt Quick/QML interface, and embedded libmpv playback. Emby integration,
danmaku matching, caching, credential handling, and application orchestration
are implemented in Rust.

## Current capabilities

- Connect to arbitrary HTTP or HTTPS Emby servers while preserving reverse
  proxy path prefixes.
- Sign in with a username and password, load the media library, prefer direct
  play, and fall back to Emby transcoding.
- Prefer libmpv hardware decoding with software-decoding fallback and keep
  external subtitles on the primary subtitle track.
- Support developer-provided (BYOK) 弹弹play `AppId`/`AppSecret` credentials.
  Credentials are validated online before the secret is stored.
- Support trusted release builds that inject project credentials from CI
  secrets. BYOK credentials always take precedence when configured.
- Hash the first 16 MiB of directly playable media for danmaku matching while
  reusing the authenticated Emby request headers.
- Match episodes, cache comments for six hours, retain a seven-day offline
  fallback, and render danmaku on an independent ASS secondary-subtitle track.
- Store Emby tokens and user-provided `AppSecret` values only in the operating
  system credential vault; SQLite never stores secrets.

This is an iterative MVP rather than a published product. It currently accepts
the only automatic danmaku match and asks for future UI work when multiple
candidates exist. Manual candidate selection, media-library grouping, audio and
subtitle track panels, and signed installers remain on the roadmap.

## 弹弹play Open Danmaku Network

Yanami integrates the
[弹弹play Open Danmaku Network](https://www.dandanplay.com/) as optional,
non-commercial infrastructure for matching local media and loading danmaku.
The integration is designed around the official
[API documentation](https://doc.dandanplay.com/open/):

- `POST /api/v2/match` is called only while preparing playback and only when no
  persisted media-to-episode association exists.
- `GET /api/v2/search/episodes` is implemented for a future explicit manual
  matching action; it is not used for background discovery or crawling.
- `GET /api/v2/comment/{episodeId}?withRelated=true` is called only for the
  selected episode when the local cache has expired or the user refreshes it.
- The first 16 MiB of the video is hashed with MD5, as required by the matching
  API. Video contents are not uploaded.
- Comments are cached for six hours, and a previously cached response can be
  used offline for up to seven days.
- Yanami does not batch-download danmaku, mirror the 弹弹play database, send
  application danmaku, or access user-restricted APIs.
- The danmaku feature is free and is not used as a paid feature or commercial
  selling point.

See [docs/DANDANPLAY.md](docs/DANDANPLAY.md) for the complete access and
compliance statement.

### Credential modes

Developer builds contain no project credentials by default. A developer can
enter an approved `AppId` and `AppSecret` in Settings; the secret is stored in
Windows Credential Manager, macOS Keychain, or Linux Secret Service.

Trusted release builds may define both of these environment variables:

```text
YANAMI_DANDANPLAY_APP_ID
YANAMI_DANDANPLAY_APP_SECRET
```

The release workflow reads them from GitHub Actions Secrets with the same
names. The build script emits only masked byte arrays, keeping plaintext out of
the repository and basic binary string scans. This is client-side obfuscation,
not a security boundary: anyone who controls the executable can eventually
recover a bundled credential. Releases must therefore use the quota assigned
to this project, monitor usage, and rotate credentials if misuse is detected.

## Technology

| Area | Choice |
| --- | --- |
| Core and networking | Rust 2024, Tokio, Reqwest, Serde |
| Metadata | SQLite through Rusqlite, with bundled SQLite |
| Secrets | Windows Credential Manager, macOS Keychain, Linux Secret Service |
| Desktop UI | Qt 6.8, Qt Quick, QML |
| Playback | libmpv render API and the Qt OpenGL scene graph |
| Native bridge | A narrow, stable C ABI exposed by a Rust `cdylib` |

For boundaries and data flow, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
For credential and network constraints, see [docs/SECURITY.md](docs/SECURITY.md).

## Development environment

Requirements:

- Rust 1.85 or newer; the repository includes `rust-toolchain.toml`.
- CMake 3.24 or newer and Ninja.
- Qt 6.8 with Gui, Quick, QuickControls2, OpenGL, Concurrent, and LinguistTools.
- A libmpv development package with a discoverable `mpv.pc` on Unix-like
  systems.
- Windows uses the supplied MSYS2/UCRT64 environment; Linux needs Secret
  Service, and macOS needs Xcode Command Line Tools.

### Windows bootstrap

Run in PowerShell:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\run-windows.ps1
```

The first command installs or completes the Rust GNU toolchain, Qt 6, CMake,
Ninja, GCC, and libmpv from one MSYS2/UCRT64 environment. The second configures,
builds, and starts Yanami. Visual Studio and a separate `CMAKE_PREFIX_PATH` are
not required.

To build without launching the application:

```powershell
.\scripts\run-windows.ps1 -BuildOnly
```

Validate the Rust workspace first:

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace --all-targets
```

Build the desktop shell manually:

```bash
cmake -S apps/desktop -B build/desktop -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.x/<platform>
cmake --build build/desktop --config Release
```

CMake builds the Rust dynamic library and copies it beside the desktop
executable. If `pkg-config` cannot locate libmpv, set `PKG_CONFIG_PATH` to the
directory containing `mpv.pc`.

## Repository layout

```text
apps/desktop/                 Qt Quick UI, Qt/libmpv adapter, Rust library loader
crates/yanami-core/           Stable domain models for servers, sessions, playback
crates/yanami-emby/           Emby API and playback-source selection
crates/yanami-danmaku/        弹弹play API, remote hashing, and ASS generation
crates/yanami-storage/        SQLite metadata and operating-system credential vault
crates/yanami-application/    Cross-module use-case orchestration and cache policy
crates/yanami-player/         Playback state machine and backend contracts
crates/yanami-desktop-bridge/ Narrow C ABI used by Qt
```

## Roadmap

- Add UI selection for multiple danmaku candidates and persist the decision.
- Add audio, subtitle, and quality-selection models.
- Finish certificate-fingerprint confirmation without weakening strict TLS.
- Build signed platform packages and document the GPU/codec compatibility
  matrix.

## License

GPL-3.0-or-later. Distribution must also account for the licenses of the chosen
libmpv build and its codec dependencies.
