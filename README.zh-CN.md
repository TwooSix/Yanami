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

1. 从 [Releases](https://github.com/TwooSix/Yanami/releases) 下载
   `Yanami-<版本号>-Windows-x86_64-Setup.exe`。

2. 运行安装程序。Yanami 安装向导会先展示欢迎页和安装选项，只有点击
   **安装 Yanami** 后才会写入系统。可以选择当前用户可写的安装目录，并分别选择
   是否添加到开始菜单的
   “所有应用”和是否创建桌面快捷方式。默认目录是
   `%LOCALAPPDATA%\Programs\Yanami`（通常为
   `C:\Users\<用户名>\AppData\Local\Programs\Yanami`）；默认创建开始菜单入口，
   不创建桌面快捷方式。安装范围仅限当前用户，正常情况下不请求管理员权限。

3. 安装完成页会显示实际安装目录，并可选择打开目录、稍后关闭向导或立即启动
   Yanami。卸载时进入**设置 → 应用 → 已安装的应用 → Yanami → 卸载**；也可以从
   Windows 搜索“添加或删除程序”后找到 Yanami。

普通 Windows 桌面安装程序不能在未经系统确认的情况下自动固定到开始菜单的
“已固定”区域。向导创建的是可被搜索、也可由用户右键固定的“所有应用”入口，不会
伪装成已经固定。

安装版可在**关于 → 检查更新**中更新。Yanami 会先校验下载内容；发布源存在兼容的
差分包时优先使用体积更小的差分包，否则安全回退到完整包。Windows ZIP 仍作为便携版
备用，但不会注册为已安装应用，也不参与安装版的托管更新。
预览版 Windows 产物尚未进行 Authenticode 签名，Windows 可能显示信誉警告；继续前请
先使用相邻的 `.sha256` 文件校验下载内容。

### Linux x86_64（实验性支持，尚未实测）

无需 `sudo`，使用下面的命令为当前用户安装最新发布的预览版：

```bash
bash -c 'set -o pipefail; curl --proto "=https" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash'
```

脚本会使用发布的 SHA-256 校验 AppImage，在 `~/.local/bin` 中创建 `yanami` 命令，
并添加桌面菜单入口。程序本体默认位于
`${XDG_DATA_HOME:-~/.local/share}/yanami/Yanami.AppImage`。再次执行同一条命令
即可更新；脚本依赖 Bash、curl 与 GNU coreutils。目前只提供 x86_64 安装包，
但 `--uninstall` 在其他 Linux 架构上也可使用。也可指定版本或卸载：

```bash
bash -c 'set -o pipefail; curl --proto "=https" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash -s -- --version 0.2.0-dev.42'
bash -c 'set -o pipefail; curl --proto "=https" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash -s -- --uninstall'
```

上述一行命令获取的是会变化的 `main` 分支。相邻的 SHA-256 文件可以发现 AppImage
损坏或下载不完整，但它与安装包来自同一发布源，不能作为独立的发布者身份认证。
如需更便于审查的流程，请先下载并检查脚本，再运行它（也可把 `main` 换成已审查的
标签或提交）：

```bash
curl --proto '=https' --tlsv1.2 -fSLo yanami-install-linux.sh \
  https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh && \
less yanami-install-linux.sh
bash ./yanami-install-linux.sh
```

如果不希望进行用户级安装，仍可从 Releases 直接下载并运行 AppImage。

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
