# Yanami

Yanami is a personal project built through Vibe Coding. My goal is to create a
cross-platform Emby desktop client with a polished interface and a fast,
responsive experience.

The project is still at a very early stage of development. There is no stable
release yet, and everything may change as development continues.

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
