#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <AppDir> <Qt root>" >&2
    exit 2
fi

app_dir="$(realpath "$1")"
qt_root="$(realpath "$2")"
license_root="$app_dir/usr/licenses"
linux_license_root="$license_root/linux"
qt_license_root="$license_root/qt"
mkdir -p "$linux_license_root" "$qt_license_root"

declare -A runtime_packages=()
while IFS= read -r bundled_file; do
    base_name="$(basename "$bundled_file")"
    case "$base_name" in
        yanami-desktop|libyanami_desktop_bridge.so|libQt6*)
            continue
            ;;
    esac

    system_path="$(ldconfig -p 2>/dev/null \
        | awk -v library="$base_name" '$1 == library { print $NF; exit }')"
    if [[ -z "$system_path" ]]; then
        continue
    fi
    package_name="$(dpkg-query -S "$(realpath "$system_path")" 2>/dev/null \
        | head -n 1 | cut -d: -f1 || true)"
    if [[ -z "$package_name" ]]; then
        package_name="$(dpkg-query -S "$system_path" 2>/dev/null \
            | head -n 1 | cut -d: -f1 || true)"
    fi
    if [[ -n "$package_name" ]]; then
        runtime_packages["$package_name"]=1
    fi
done < <(find "$app_dir/usr" -type f -o -type l | sort)

# libmpv is the non-Qt runtime boundary Yanami explicitly promises to bundle.
# Include its package even if linuxdeploy chose a versioned alias that could not
# be mapped back through ldconfig.
if dpkg-query -W libmpv2 >/dev/null 2>&1; then
    runtime_packages[libmpv2]=1
fi

metadata_file="$linux_license_root/PACKAGE_METADATA.txt"
: > "$metadata_file"
while IFS= read -r package_name; do
    doc_package_name="${package_name%%:*}"
    package_dir="$linux_license_root/$doc_package_name"
    copyright_file="/usr/share/doc/$doc_package_name/copyright"
    if [[ ! -f "$copyright_file" ]]; then
        echo "missing Debian copyright metadata for $package_name" >&2
        exit 1
    fi
    mkdir -p "$package_dir"
    cp "$copyright_file" "$package_dir/copyright"
    dpkg-query -W -f='Package: ${binary:Package}\nVersion: ${Version}\nArchitecture: ${Architecture}\n\n' \
        "$package_name" >> "$metadata_file"
done < <(printf '%s\n' "${!runtime_packages[@]}" | sort)

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

if [[ -n "$qt_license_directory" ]]; then
    cp -R "$qt_license_directory/." "$qt_license_root/"
else
    cat > "$qt_license_root/PACKAGE_METADATA.txt" <<EOF
Qt runtime root: $qt_root
The Qt binary distribution did not contain a separate LICENSES directory.
Yanami distributes Qt under its GPL alternative; the package root contains
the complete GPL-3.0-or-later text. Qt notices and source are available at:
https://www.qt.io/licensing/open-source-lgpl-obligations
https://code.qt.io/cgit/qt/
EOF
fi
