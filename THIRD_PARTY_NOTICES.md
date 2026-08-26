# Third-party notices

Yanami is distributed under GPL-3.0-or-later. The exact application version,
source development line, commit, platform, and architecture of a binary package
are recorded in its `BUILD_INFO.json`. Packages dynamically link open-source
Qt and libmpv runtimes and embed a locked Rust dependency graph. The primary
Windows package also links SDL3 for controller input; experimental non-Windows
builds use SDL3 when it is available at configure time.

The package contains the following principal runtime components:

| Component | Examined Windows package version | License used by the package |
| --- | --- | --- |
| Qt Base / Qt Declarative | 6.11.1-1 | LGPL-3.0-only WITH Qt-GPL-exception-1.0, with GPL alternatives and bundled third-party terms |
| SDL3 | 3.4.14-1 | Zlib |
| mpv / libmpv | 0.41.0-5 | GPL-2.0-or-later |
| FFmpeg | 9.0-2 | GPL-3.0-or-later |
| libass | 0.17.5-1 | ISC |
| SQLite | bundled by rusqlite | Public domain |

Every package includes:

- `LICENSE`, the complete GPL version 3 text for Yanami and GPLv3 components;
- `licenses/RUST_DEPENDENCIES.md`, the complete locked Rust package, version,
  SPDX-expression and source inventory;
- `licenses/rust/THIRD_PARTY_LICENSES.html`, the complete license and copyright
  text generated from that locked Rust graph with cargo-about;
- `BUILD_INFO.json`, the immutable source and workflow identity;
- `SHA256SUMS.txt`, a hash and size manifest of every staged file.

The Windows ZIP additionally includes:

- `licenses/qt`, the license texts shipped with the exact Qt packages;
- `licenses/mpv/GPL-2.0-or-later.txt`;
- `licenses/ffmpeg/GPL-3.0-or-later.txt`;
- `licenses/libass/ISC.txt`;
- `licenses/msys2/PACKAGE_METADATA.txt`, generated while staging and containing
  the package metadata for every MSYS2-owned runtime file that could be mapped;
- `licenses/msys2/<package>`, license and notice texts for every runtime owner.
  When MSYS2 does not install them, Yanami uses a version-locked fallback whose
  upstream archive and every copied file are SHA-256 verified during packaging.

The Linux AppImage additionally includes `licenses/linux`, generated Debian
package metadata and copyright files for mapped bundled libraries, plus
`licenses/qt`. The macOS DMG includes `licenses/runtime`, generated Homebrew
formula metadata and available license or notice files for the libmpv runtime
closure, plus the Qt license inventory available to the packaging toolchain.

Before distribution, corresponding Yanami source must be published in the
project's public source repository at the exact release tag. Unmodified MSYS2
component build recipes and source links are available from
<https://packages.msys2.org/>. Homebrew formulae and source links are available
from <https://formulae.brew.sh/>. Qt's source-code offer and license obligations
are described at
<https://www.qt.io/development/open-source-lgpl-obligations>.

The automatically generated inventories are authoritative for the exact
release archive. If an inventory and this summary differ, do not distribute the
archive until the discrepancy has been reviewed.
