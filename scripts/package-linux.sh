#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"
source_version="$(tr -d '[:space:]' < "$workspace/VERSION")"
build_version="${YANAMI_BUILD_VERSION:-$source_version}"
build_commit="${YANAMI_BUILD_COMMIT:-$(git -C "$workspace" rev-parse HEAD)}"
build_run_id="${YANAMI_BUILD_RUN_ID:-local}"
build_dir="${YANAMI_BUILD_DIR:-$workspace/build/package-linux}"
output_dir="${YANAMI_OUTPUT_DIR:-$workspace/build/release-assets}"
qt_root="${QT_ROOT_DIR:-}"

if [[ -z "$qt_root" || ! -d "$qt_root" ]]; then
    echo "QT_ROOT_DIR must point to a Qt 6.8 or newer desktop installation" >&2
    exit 1
fi
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
    echo "The checked-in Linux packaging toolchain currently targets Linux x86_64" >&2
    exit 1
fi
if [[ ! "$build_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9A-Za-z.-]+$ ]]; then
    echo "YANAMI_BUILD_VERSION is not a development SemVer: $build_version" >&2
    exit 1
fi

for command_name in cmake ctest curl sha256sum; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "required command is missing: $command_name" >&2
        exit 1
    }
done

cmake -S "$workspace/apps/desktop" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$qt_root" \
    -DBUILD_TESTING=ON \
    -DYANAMI_ENABLE_DEV_HOOKS=OFF \
    -DYANAMI_BUILD_VERSION="$build_version" \
    -DYANAMI_BUILD_COMMIT="$build_commit" \
    -DYANAMI_BUILD_RUN_ID="$build_run_id" \
    -DYANAMI_PACKAGE_ARCHITECTURE=x86_64
cmake --build "$build_dir" --parallel
unset YANAMI_DANDANPLAY_APP_ID YANAMI_DANDANPLAY_APP_SECRET || true
ctest --test-dir "$build_dir" --output-on-failure

app_dir="$(mktemp -d "$build_dir/AppDir.XXXXXX")"
cmake --install "$build_dir" --prefix "$app_dir/usr"

tools_dir="$build_dir/package-tools"
mkdir -p "$tools_dir" "$output_dir"
linuxdeploy="$tools_dir/linuxdeploy-x86_64.AppImage"
qt_plugin="$tools_dir/linuxdeploy-plugin-qt-x86_64.AppImage"
linuxdeploy_url="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
qt_plugin_url="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"

curl -LfsS "$linuxdeploy_url" -o "$linuxdeploy"
curl -LfsS "$qt_plugin_url" -o "$qt_plugin"
printf '%s  %s\n' \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d \
    "$linuxdeploy" | sha256sum -c -
printf '%s  %s\n' \
    15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724 \
    "$qt_plugin" | sha256sum -c -
chmod +x "$linuxdeploy" "$qt_plugin"

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$qt_root/bin/qmake"
export QML_SOURCES_PATHS="$workspace/apps/desktop/qml"
export EXTRA_PLATFORM_PLUGINS="libqminimal.so;libqoffscreen.so"
export LD_LIBRARY_PATH="$qt_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$linuxdeploy" \
    --appdir "$app_dir" \
    --executable "$app_dir/usr/bin/yanami-desktop" \
    --library "$app_dir/usr/bin/libyanami_desktop_bridge.so" \
    --desktop-file "$app_dir/usr/share/applications/io.github.TwooSix.Yanami.desktop" \
    --icon-file "$app_dir/usr/share/icons/hicolor/1024x1024/apps/io.github.TwooSix.Yanami.png" \
    --plugin qt

bash "$script_dir/collect-linux-runtime-licenses.sh" "$app_dir" "$qt_root"
cmake -DYANAMI_PACKAGE_ROOT="$app_dir/usr" \
    -P "$workspace/apps/desktop/cmake/WritePackageManifest.cmake"

asset_name="Yanami-${build_version}-Linux-x86_64.AppImage"
if [[ -e "$output_dir/$asset_name" || -e "$output_dir/$asset_name.sha256" ]]; then
    echo "refusing to overwrite an existing release candidate: $asset_name" >&2
    exit 1
fi
export OUTPUT="$output_dir/$asset_name"
export VERSION="$build_version"
"$linuxdeploy" --appdir "$app_dir" --output appimage

if [[ ! -f "$output_dir/$asset_name" ]]; then
    generated_image="$(find "$workspace" "$build_dir" -maxdepth 2 \
        -type f -name '*.AppImage' ! -path "$tools_dir/*" \
        -printf '%T@ %p\n' | sort -nr | head -n 1 | cut -d' ' -f2-)"
    if [[ -z "$generated_image" ]]; then
        echo "linuxdeploy did not create an AppImage" >&2
        exit 1
    fi
    mv "$generated_image" "$output_dir/$asset_name"
fi

(
    cd "$output_dir"
    sha256sum "$asset_name" > "$asset_name.sha256"
)
printf '%s\n' "$output_dir/$asset_name"
