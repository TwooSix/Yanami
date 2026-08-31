<div align="center">
  <img src="apps/desktop/qml/assets/yanami-logo.png" width="168" alt="Yanami logo">

  <h1>Yanami</h1>

  <p><strong>A polished, responsive cross-platform Emby desktop client built through Vibe Coding.</strong></p>

  <p>
    <a href="README.md">English</a> ·
    <a href="README.zh-CN.md">简体中文</a>
  </p>

  <p>
    <a href="https://github.com/TwooSix/Yanami/releases"><img src="https://img.shields.io/badge/downloads-pre--release-8b5cf6?style=flat-square" alt="Pre-release downloads"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-2563eb?style=flat-square" alt="GPL-3.0-or-later license"></a>
    <img src="https://img.shields.io/badge/Rust-1.85%2B-d97706?style=flat-square&amp;logo=rust&amp;logoColor=white" alt="Rust 1.85 or newer">
    <img src="https://img.shields.io/badge/Qt-6.8%2B-22c55e?style=flat-square&amp;logo=qt&amp;logoColor=white" alt="Qt 6.8 or newer">
  </p>
</div>

> *Yanami is still in early development and evolving quickly. Features, the
> interface, and compatibility may change between pre-release builds.*

## Highlights

- **Modern experience** — a modern, fluid, cross-platform application experience.
- **Danmaku support** — danmaku matching and playback powered by
  [DanDanPlay](https://www.dandanplay.com/).
- **Anime upscaling** — real-time anime upscaling powered by
  [Anime4K](https://github.com/bloc97/Anime4K).
- **Controller support** — a couch-friendly app experience every bit as capable
  as mouse control.

## Quick start

Pre-release binaries will be published on the official
[GitHub Releases](https://github.com/TwooSix/Yanami/releases) page. There is no
stable release channel yet.

> **Platform status:** Windows is the primary development platform. Linux and
> macOS desktop builds have **not** been tested on real machines yet; their
> instructions and future binaries should be treated as experimental.

Development candidates use versions such as `0.2.0-dev.42`. Choose the asset
whose platform and CPU architecture match your computer.

### Windows 10/11

1. Download `Yanami-<version>-Windows-x86_64-Setup.exe` from
   [Releases](https://github.com/TwooSix/Yanami/releases).
2. Run the installer. The Yanami setup wizard first shows a welcome page and
   then the installation options; it does not change the system until you click
   **Install Yanami**. You can select a current-user-writable install directory,
   a Start menu **All apps** shortcut, and a desktop shortcut. The default
   directory is
   `%LOCALAPPDATA%\Programs\Yanami` (normally
   `C:\Users\<name>\AppData\Local\Programs\Yanami`). The Start menu shortcut
   is on by default; the desktop shortcut is off by default. Installation is
   scoped to the current user and normally requires no administrator access.
3. The completion page shows the exact installed location and lets you open
   that folder, close the wizard for now, or launch Yanami. To uninstall, use
   **Settings → Apps → Installed apps → Yanami → Uninstall** (or search Windows
   for “Add or remove programs”).

An ordinary Windows desktop installer cannot silently add itself to the Start
menu's **Pinned** area. The wizard creates an **All apps** entry that Windows
Search can find and that the user can pin manually; it does not claim that the
app was already pinned.

Installed builds can use **About → Check for updates**. Yanami verifies the
download before applying it and uses a smaller delta package when the release
feed provides a compatible one; otherwise it safely falls back to the full
package. The Windows ZIP remains available as a portable fallback, but it is
not registered as an installed app and does not participate in managed updates.
Preview Windows artifacts are not yet Authenticode-signed, so Windows may show
a reputation warning; verify the adjacent `.sha256` file before continuing.

### Linux x86_64 (experimental and untested)

Install the latest published preview for the current user without `sudo`:

```bash
bash -c 'set -o pipefail; curl --proto "=https" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash'
```

The script verifies the AppImage against its published SHA-256 checksum, adds a
`yanami` command under `~/.local/bin`, and creates a desktop-menu entry. The
payload itself is stored at
`${XDG_DATA_HOME:-~/.local/share}/yanami/Yanami.AppImage`. Run the same command
again to update. Bash, curl, and GNU coreutils are required. Installation is
currently x86_64-only, but `--uninstall` also works on other Linux
architectures. To select a release or uninstall:

```bash
bash -c 'set -o pipefail; curl --proto "=https" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash -s -- --version 0.2.0-dev.42'
bash -c 'set -o pipefail; curl --proto "=https" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash -s -- --uninstall'
```

These one-liners fetch the mutable `main` branch. The adjacent SHA-256 file
detects a corrupted or incomplete AppImage, but it is not independent publisher
authentication because it is obtained from the same release origin. For a more
reviewable flow, download the script first, inspect it, and only then run it (or
replace `main` with a reviewed tag or commit):

```bash
curl --proto '=https' --tlsv1.2 -fSLo yanami-install-linux.sh \
  https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh && \
less yanami-install-linux.sh
bash ./yanami-install-linux.sh
```

The AppImage can still be downloaded and run directly when a user-local install
is not wanted.

The AppImage includes the Qt and libmpv runtimes, so no separate installation
should normally be required. If FUSE is unavailable, run it once with
`APPIMAGE_EXTRACT_AND_RUN=1`. Linux packaging is checked in CI but has not yet
been tested on real hardware; report any missing library as a packaging issue.

### macOS (experimental and untested)

1. Download `Yanami-<version>-macOS-arm64.dmg` for Apple Silicon or
   `Yanami-<version>-macOS-x86_64.dmg` for an Intel Mac.
2. Open the DMG, drag **Yanami** into **Applications**, and launch it.

The DMG includes Qt and libmpv; running it does not require Homebrew. Early
preview builds carry only an ad-hoc integrity signature and are not Developer
ID signed or notarized. If macOS blocks a build, use **System Settings → Privacy
& Security → Open Anyway** only after verifying that it came from this
repository.

### First launch

1. Open **Settings → Emby server**.
2. Enter the server address, username, and password. HTTPS is strongly
   recommended outside a trusted local network.
3. Connect, return to Home, and choose something from your library.

## Build and run from source

### Prerequisites

| Dependency | Minimum / notes |
| --- | --- |
| Rust | 1.85 or newer, installed with `rustup` |
| CMake | 3.24 or newer |
| Ninja | Current stable release |
| C++ compiler | C++20 capable |
| Qt | 6.8 or newer with Qt Quick, Quick Controls, OpenGL, and Linguist Tools |
| libmpv | Development headers and library; `pkg-config` is used on Linux/macOS |
| SDL3 | Primary controller backend on Windows; optional for experimental Linux/macOS controller support |

Clone the repository first:

```bash
git clone https://github.com/TwooSix/Yanami.git
cd Yanami
```

### Windows

Open PowerShell in the repository root:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\run-windows.ps1
```

The bootstrap script installs or configures MSYS2 UCRT64, Qt, SDL3, libmpv, CMake,
Ninja, and the matching Rust GNU toolchain. It uses Scoop when available and
otherwise falls back to `winget`.

Useful variants:

```powershell
# Reconfigure from a fresh CMake cache.
.\scripts\run-windows.ps1 -Fresh

# Build without launching Yanami.
.\scripts\run-windows.ps1 -BuildOnly
```

### Linux (experimental and untested)

Install a C++20 compiler, CMake, Ninja, Qt 6.8 development packages, libmpv
development files, `pkg-config`, and the D-Bus development headers used by the
system credential vault. On Debian/Ubuntu, the non-Qt package names commonly
include `build-essential`, `cmake`, `ninja-build`, `pkg-config`, `libmpv-dev`,
and `libdbus-1-dev`. Obtain Qt 6.8 separately if the distribution ships an
older version.

```bash
export YANAMI_QT_ROOT=/path/to/Qt/6.8.x/gcc_64

cmake -S apps/desktop -B build/desktop-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$YANAMI_QT_ROOT" \
  -DBUILD_TESTING=OFF
cmake --build build/desktop-linux --parallel
./build/desktop-linux/yanami
```

### macOS (experimental and untested)

Install Rust with `rustup`. Homebrew can provide the remaining build
dependencies:

```bash
brew install cmake ninja pkg-config qt mpv

export YANAMI_QT_ROOT="$(brew --prefix qt)"

cmake -S apps/desktop -B build/desktop-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$YANAMI_QT_ROOT" \
  -DBUILD_TESTING=OFF
cmake --build build/desktop-macos --parallel
open ./build/desktop-macos/Yanami.app
```

### Optional checks

Run the Rust quality gates on any supported Rust host:

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
```

On Windows, use `cargo +stable-x86_64-pc-windows-gnu` for these commands and
ensure the MSYS2 UCRT64 `bin` directory is on `PATH`, matching the desktop build
environment configured by the Windows scripts.

To include the C++/QML test suite, configure with `-DBUILD_TESTING=ON`, build,
then run:

```bash
ctest --test-dir build/<desktop-build-directory> --output-on-failure
```

Maintainers can find the CI, packaging, versioning, and manual prerelease
procedure in [docs/RELEASING.md](docs/RELEASING.md).

## Technology

| Area | Technology |
| --- | --- |
| Application core and services | Rust |
| Desktop interface | Qt Quick / QML and C++ |
| Video playback | libmpv |
| Local data | SQLite and the OS credential vault |

## License

Yanami is free software licensed under
[GPL-3.0-or-later](LICENSE). Third-party components retain their own licenses;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
