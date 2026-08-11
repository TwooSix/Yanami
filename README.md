# Yanami

> **Work in progress:** Yanami is under active development. There is no stable
> release yet, and features may change as the project evolves.

Yanami is a cross-platform desktop client for Emby. It combines a modern Qt
Quick interface with libmpv playback and a Rust application core.

## Highlights

- Connect to Emby servers over HTTP or HTTPS.
- Browse and search your media library.
- Prefer original-quality playback with automatic transcoding fallback.
- Use hardware-accelerated playback when available.
- Display external subtitles and danmaku during playback.
- Resume playback from the saved position.
- Switch between English and Simplified Chinese.

## Project status

The core browsing and playback experience is functional, but Yanami is still
an early prototype. The following areas are being improved:

- Manual selection when multiple danmaku matches are available.
- Audio, subtitle, and playback-quality controls.
- Media-library organization and navigation.
- Signed installers and platform packaging.
- Playback compatibility across more devices and codecs.

Prebuilt downloads are not available yet. For now, Yanami must be built from
source.

## Build from source

### Windows

Open PowerShell in the repository and run:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\run-windows.ps1
```

The first command installs the required build tools. The second command builds
and starts Yanami.

To build without launching the application:

```powershell
.\scripts\run-windows.ps1 -BuildOnly
```

### Other platforms

Building manually requires:

- Rust 1.85 or newer
- CMake 3.24 or newer
- Ninja
- Qt 6.8 with Qt Quick and OpenGL support
- libmpv development files

```bash
cargo test --workspace --all-targets

cmake -S apps/desktop -B build/desktop -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.x/<platform>
cmake --build build/desktop --config Release
```

## Technology

| Area | Technology |
| --- | --- |
| Application core | Rust |
| Desktop interface | Qt Quick / QML |
| Video playback | libmpv |
| Local data | SQLite |

## License

Yanami is licensed under GPL-3.0-or-later.
