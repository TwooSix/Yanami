#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
    echo "usage: $0 OUTPUT_PATH" >&2
    exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"
output_path="$1"
output_dir="$(dirname "$output_path")"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
output_path="$output_dir/$(basename "$output_path")"
temporary_path="$(mktemp "$output_path.tmp.XXXXXX")"
marker_path="$(mktemp "$output_path.markers.XXXXXX")"
stamped_path="$(mktemp "$output_path.stamped.XXXXXX")"
trap 'rm -f "$temporary_path" "$marker_path" "$stamped_path"' EXIT

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print tolower($1) }'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{ print tolower($1) }'
    else
        echo "sha256sum or shasum is required" >&2
        return 1
    fi
}

about_version="$(cargo about --version)"
if [[ "$about_version" != "cargo-about 0.9.1" ]]; then
    echo "cargo-about 0.9.1 is required, found: $about_version" >&2
    exit 1
fi

(
    cd "$workspace"
    cargo about generate --workspace --locked --fail \
        licenses/rust/about.hbs \
        -o "$temporary_path"
)

cargo_manifests=()
while IFS= read -r manifest; do
    cargo_manifests+=("$manifest")
done < <(
    cd "$workspace"
    {
        printf '%s\n' Cargo.toml
        find crates -type f -name Cargo.toml -print
    } | LC_ALL=C sort -u
)
license_inputs=(
    Cargo.lock
    about.toml
    licenses/rust/about.hbs
    scripts/generate-rust-license-inventory.sh
    "${cargo_manifests[@]}"
)
printf '%s\n' \
    '  <meta name="yanami-license-generator" content="cargo-about 0.9.1">' \
    > "$marker_path"
for relative_path in "${license_inputs[@]}"; do
    input_path="$workspace/$relative_path"
    if [[ ! -f "$input_path" ]]; then
        echo "license inventory input is missing: $relative_path" >&2
        exit 1
    fi
    input_hash="$(hash_file "$input_path")"
    printf '  <meta name="yanami-license-input-sha256" data-path="%s" content="%s">\n' \
        "$relative_path" "$input_hash" >> "$marker_path"
done

awk '
    FNR == NR { markers = markers $0 ORS; next }
    /<\/head>/ { printf "%s", markers }
    { print }
' "$marker_path" "$temporary_path" > "$stamped_path"
mv -f "$stamped_path" "$temporary_path"

size_bytes="$(wc -c < "$temporary_path" | tr -d '[:space:]')"
if (( size_bytes < 32768 || size_bytes > 2097152 )); then
    echo "generated Rust license inventory has an unexpected size: $size_bytes bytes" >&2
    exit 1
fi

grep -Fq '<!DOCTYPE html>' "$temporary_path"
grep -Fq '<title>Yanami Rust third-party licenses</title>' "$temporary_path"
grep -Fq '<meta name="yanami-license-generator" content="cargo-about 0.9.1">' "$temporary_path"
grep -Fq 'Generated from the locked Cargo dependency graph for all packaged platforms.' "$temporary_path"
grep -Fq '<h2>License overview</h2>' "$temporary_path"
grep -Fq '</html>' "$temporary_path"
while IFS= read -r marker; do
    grep -Fqx "$marker" "$temporary_path"
done < "$marker_path"
if grep -Eiq '<[[:space:]]*(script|iframe|object|embed)([[:space:]>])' "$temporary_path"; then
    echo "generated Rust license inventory unexpectedly contains active HTML" >&2
    exit 1
fi

mv -f "$temporary_path" "$output_path"
rm -f "$marker_path" "$stamped_path"
trap - EXIT
printf 'Generated %s (%s bytes)\n' "$output_path" "$size_bytes"
