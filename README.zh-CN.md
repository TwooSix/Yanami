<div align="center">
  <img src="apps/desktop/qml/assets/yanami-logo.png" width="168" alt="Yanami Logo">

  <h1>Yanami</h1>

  <p><strong>一款通过 Vibe Coding 构建，精致、流畅的跨平台 Emby 桌面客户端。</strong></p>

  <p>
    <a href="README.md">English</a> ·
    <a href="README.zh-CN.md">简体中文</a>
  </p>

  <p>
    <a href="https://github.com/TwooSix/Yanami/releases"><img src="https://img.shields.io/badge/downloads-pre--release-8b5cf6?style=flat-square" alt="下载预发布版本"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-2563eb?style=flat-square" alt="GPL-3.0-or-later 许可"></a>
    <img src="https://img.shields.io/badge/Rust-1.85%2B-d97706?style=flat-square&amp;logo=rust&amp;logoColor=white" alt="Rust 1.85 或更高版本">
    <img src="https://img.shields.io/badge/Qt-6.8%2B-22c55e?style=flat-square&amp;logo=qt&amp;logoColor=white" alt="Qt 6.8 或更高版本">
  </p>
</div>

> *Yanami 仍处于早期开发阶段，正在快速迭代中。不同预发布版本之间的功能、
> 界面和兼容性都可能发生变化。*

## 亮点

- **现代化体验** —— 提供现代化、流畅、跨平台的应用使用体验。
- **弹幕功能** —— 基于 [DanDanPlay（弹弹play）](https://www.dandanplay.com/)
  提供弹幕匹配与播放功能。
- **动画超分** —— 基于 [Anime4K](https://github.com/bloc97/Anime4K)
  提供实时的动画超分能力。
- **支持手柄控制** —— 躺着也能拥有不亚于鼠标控制的应用体验。

## 快速开始

预发布二进制文件将发布在官方
[GitHub Releases](https://github.com/TwooSix/Yanami/releases) 页面，目前尚无
稳定版发布通道。

> **平台状态：** Windows 是当前主要开发平台。Linux 和 macOS 桌面版本
> **尚未经过真机测试**，相关说明和后续二进制产物均应视为实验性支持。

开发候选版本使用类似 `0.2.0-dev.42` 的版本号。请根据操作系统和 CPU 架构选择
对应资源。

### Windows 10/11

1. 从 [Releases](https://github.com/TwooSix/Yanami/releases) 下载 Windows ZIP。

2. 解压 ZIP，打开解压后的目录并运行 `bin\Yanami.exe`。

### Linux（实验性支持，尚未实测）

1. 从 [Releases](https://github.com/TwooSix/Yanami/releases) 下载
   `Yanami-<版本号>-Linux-x86_64.AppImage`。
2. 在下载目录中打开终端并运行：

   ```bash
   chmod +x ./Yanami-*-Linux-x86_64.AppImage
   ./Yanami-*-Linux-x86_64.AppImage
   ```

AppImage 已随包携带 Qt 和 libmpv 运行库，正常情况下无需额外安装。如果系统没有
FUSE，可使用 `APPIMAGE_EXTRACT_AND_RUN=1` 运行。Linux 打包会经过 CI 检查，但尚未
在真实硬件上测试；如果仍提示缺少动态库，请将其作为打包问题反馈。

### macOS（实验性支持，尚未实测）

1. Apple Silicon Mac 下载 `Yanami-<版本号>-macOS-arm64.dmg`，Intel Mac 下载
   `Yanami-<版本号>-macOS-x86_64.dmg`。
2. 打开 DMG，将 **Yanami** 拖入**应用程序**目录，然后启动。

DMG 已随包携带 Qt 和 libmpv，运行时无需安装 Homebrew。早期预览版本仅带有 ad-hoc
完整性签名，尚未使用 Developer ID 签名或进行公证。如果 macOS 拦截程序，请先
确认它来自本仓库，再前往**系统设置 → 隐私与安全性 → 仍要打开**。

### 首次启动

1. 打开**设置 → Emby 服务器**。
2. 输入服务器地址、用户名和密码。在不受信任的局域网以外，强烈建议使用 HTTPS。
3. 连接成功后返回首页，从媒体库中选择内容即可开始使用。

## 从源码构建并运行

### 前置依赖

| 依赖 | 最低版本 / 说明 |
| --- | --- |
| Rust | 1.85 或更高版本，使用 `rustup` 安装 |
| CMake | 3.24 或更高版本 |
| Ninja | 当前稳定版本 |
| C++ 编译器 | 支持 C++20 |
| Qt | 6.8 或更高版本，包含 Qt Quick、Quick Controls、OpenGL 和 Linguist Tools |
| libmpv | 开发头文件和库；Linux/macOS 通过 `pkg-config` 查找 |
| SDL3 | Windows 的主要控制器后端；Linux/macOS 的实验性控制器支持可选使用 |

首先克隆仓库：

```bash
git clone https://github.com/TwooSix/Yanami.git
cd Yanami
```

### Windows

在仓库根目录打开 PowerShell：

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\run-windows.ps1
```

引导脚本会安装或配置 MSYS2 UCRT64、Qt、SDL3、libmpv、CMake、Ninja 和匹配的 Rust
GNU 工具链。脚本会优先使用 Scoop，否则回退到 `winget`。

其他常用方式：

```powershell
# 使用全新的 CMake 缓存重新配置。
.\scripts\run-windows.ps1 -Fresh

# 只构建，不启动 Yanami。
.\scripts\run-windows.ps1 -BuildOnly
```

### Linux（实验性支持，尚未实测）

请安装支持 C++20 的编译器、CMake、Ninja、Qt 6.8 开发包、libmpv 开发文件、
`pkg-config`，以及系统凭据库需要的 D-Bus 开发头文件。在 Debian/Ubuntu 中，
Qt 以外的常见包名包括 `build-essential`、`cmake`、`ninja-build`、
`pkg-config`、`libmpv-dev` 和 `libdbus-1-dev`。如果发行版自带的 Qt 低于
6.8，请另行安装 Qt 6.8。

```bash
export YANAMI_QT_ROOT=/path/to/Qt/6.8.x/gcc_64

cmake -S apps/desktop -B build/desktop-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$YANAMI_QT_ROOT" \
  -DBUILD_TESTING=OFF
cmake --build build/desktop-linux --parallel
./build/desktop-linux/yanami
```

### macOS（实验性支持，尚未实测）

请先使用 `rustup` 安装 Rust，其余构建依赖可由 Homebrew 提供：

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

### 可选检查

可在任意受 Rust 支持的系统上运行以下质量检查：

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
```

在 Windows 上，请将这些命令中的 `cargo` 替换为
`cargo +stable-x86_64-pc-windows-gnu`，并确保 MSYS2 UCRT64 的 `bin` 目录位于
`PATH` 中，以匹配 Windows 脚本配置的桌面构建环境。

如需包含 C++/QML 测试，请在配置时使用 `-DBUILD_TESTING=ON`，完成构建后运行：

```bash
ctest --test-dir build/<桌面构建目录> --output-on-failure
```

维护者可参阅 [docs/RELEASING.md](docs/RELEASING.md)，了解 CI、打包、版本号和
手动发布预览版的完整流程。

## 技术栈

| 范围 | 技术 |
| --- | --- |
| 应用核心与服务 | Rust |
| 桌面界面 | Qt Quick / QML 和 C++ |
| 视频播放 | libmpv |
| 本地数据 | SQLite 与操作系统凭据库 |

## 开源许可

Yanami 是采用 [GPL-3.0-or-later](LICENSE) 许可的自由软件。第三方组件仍遵循
各自的许可条款，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
