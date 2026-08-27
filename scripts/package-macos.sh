#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"
source_version="$(tr -d '[:space:]' < "$workspace/VERSION")"
build_version="${YANAMI_BUILD_VERSION:-$source_version}"
build_commit="${YANAMI_BUILD_COMMIT:-$(git -C "$workspace" rev-parse HEAD)}"
build_run_id="${YANAMI_BUILD_RUN_ID:-local}"
qt_root="${QT_ROOT_DIR:-}"

case "$(uname -m)" in
    arm64) package_architecture=arm64 ;;
    x86_64) package_architecture=x86_64 ;;
    *)
        echo "unsupported macOS package architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

build_dir="${YANAMI_BUILD_DIR:-$workspace/build/package-macos-$package_architecture}"
output_dir="${YANAMI_OUTPUT_DIR:-$workspace/build/release-assets}"
if [[ "$(uname -s)" != Darwin ]]; then
    echo "This packaging script must run on macOS" >&2
    exit 1
fi
if [[ -z "$qt_root" || ! -d "$qt_root" ]]; then
    echo "QT_ROOT_DIR must point to a Qt 6.8 or newer desktop installation" >&2
    exit 1
fi
if [[ ! "$build_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9A-Za-z.-]+$ ]]; then
    echo "YANAMI_BUILD_VERSION is not a development SemVer: $build_version" >&2
    exit 1
fi

mkdir -p "$build_dir"
rust_license_inventory="${YANAMI_RUST_LICENSE_INVENTORY:-}"
if [[ -n "$rust_license_inventory" ]]; then
    if [[ ! -f "$rust_license_inventory" ]]; then
        echo "YANAMI_RUST_LICENSE_INVENTORY does not exist: $rust_license_inventory" >&2
        exit 1
    fi
else
    rust_license_inventory="$build_dir/generated-licenses/THIRD_PARTY_LICENSES.html"
    if [[ "$(cargo about --version 2>/dev/null || true)" != "cargo-about 0.9.1" ]]; then
        cargo install cargo-about --version 0.9.1 --locked --features cli --force
    fi
    bash "$script_dir/generate-rust-license-inventory.sh" "$rust_license_inventory"
fi

runtime_license_dir="$(mktemp -d "$build_dir/runtime-licenses.XXXXXX")"
formula_list="$runtime_license_dir/formulae.txt"
printf '%s\n' mpv > "$formula_list"
while true; do
    previous_count="$(wc -l < "$formula_list" | tr -d '[:space:]')"
    dependency_list="$runtime_license_dir/formulae.next"
    cp "$formula_list" "$dependency_list"
    while IFS= read -r formula_name; do
        brew deps "$formula_name" >> "$dependency_list"
    done < "$formula_list"
    sort -u "$dependency_list" > "$formula_list.sorted"
    mv "$formula_list.sorted" "$formula_list"
    current_count="$(wc -l < "$formula_list" | tr -d '[:space:]')"
    [[ "$current_count" == "$previous_count" ]] && break
done
rm -f "$runtime_license_dir/formulae.next"
runtime_formulae=()
while IFS= read -r formula_name; do
    [[ -n "$formula_name" ]] && runtime_formulae+=("$formula_name")
done < "$formula_list"
brew info --json=v2 "${runtime_formulae[@]}" \
    > "$runtime_license_dir/HOMEBREW_PACKAGE_METADATA.json"
for formula_name in "${runtime_formulae[@]}"; do
    formula_prefix="$(brew --prefix "$formula_name")"
    formula_license_dir="$runtime_license_dir/homebrew/$formula_name"
    while IFS= read -r license_file; do
        mkdir -p "$formula_license_dir"
        relative_name="${license_file#"$formula_prefix"/}"
        cp "$license_file" "$formula_license_dir/${relative_name//\//__}"
    done < <(find "$formula_prefix" -maxdepth 5 -type f \
        \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o \
           -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'NOTICE*' \) \
        | sort)
done

qt_license_directory=""
for candidate in \
    "$qt_root/LICENSES" \
    "$qt_root/../LICENSES" \
    "$qt_root/../../LICENSES" \
    "$qt_root/../../Licenses"; do
    if [[ -d "$candidate" ]]; then
        qt_license_directory="$candidate"
        break
    fi
done
mkdir -p "$runtime_license_dir/qt"
if [[ -n "$qt_license_directory" ]]; then
    cp -R "$qt_license_directory/." "$runtime_license_dir/qt/"
else
    cat > "$runtime_license_dir/qt/PACKAGE_METADATA.txt" <<EOF
Qt runtime root: $qt_root
Yanami distributes Qt under its GPL alternative; the package root contains
the complete GPL-3.0-or-later text. Qt notices and source are available at:
https://www.qt.io/licensing/open-source-lgpl-obligations
https://code.qt.io/cgit/qt/
EOF
fi

cmake -S "$workspace/apps/desktop" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$qt_root" \
    -DCMAKE_OSX_ARCHITECTURES="$package_architecture" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DBUILD_TESTING=ON \
    -DYANAMI_ENABLE_DEV_HOOKS=OFF \
    -DYANAMI_BUILD_VERSION="$build_version" \
    -DYANAMI_BUILD_COMMIT="$build_commit" \
    -DYANAMI_BUILD_RUN_ID="$build_run_id" \
    -DYANAMI_RUST_LICENSE_INVENTORY="$rust_license_inventory" \
    -DYANAMI_RUNTIME_LICENSE_DIR="$runtime_license_dir" \
    -DYANAMI_PACKAGE_ARCHITECTURE="$package_architecture"
cmake --build "$build_dir" --parallel
unset YANAMI_DANDANPLAY_APP_ID YANAMI_DANDANPLAY_APP_SECRET || true
ctest --test-dir "$build_dir" --output-on-failure
if ! cmake --build "$build_dir" --target package; then
    echo "DMG creation failed once; retrying without rebuilding the application" >&2
    cmake --build "$build_dir" --target package
fi

asset_name="Yanami-${build_version}-macOS-${package_architecture}.dmg"
archive="$build_dir/$asset_name"
checksum="$archive.sha256"
if [[ ! -f "$archive" || ! -f "$checksum" ]]; then
    echo "CPack did not create $asset_name and its checksum" >&2
    exit 1
fi

mkdir -p "$output_dir"
if [[ -e "$output_dir/$asset_name" || -e "$output_dir/$asset_name.sha256" ]]; then
    echo "refusing to overwrite an existing release candidate: $asset_name" >&2
    exit 1
fi
cp "$archive" "$checksum" "$output_dir/"
printf '%s\n' "$output_dir/$asset_name"
