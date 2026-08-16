#!/usr/bin/env bash
set -euo pipefail

app="${1:-}"
if [[ -z "$app" || ! -d "$app/Contents" ]]; then
    echo "usage: verify-macos-bundle.sh /path/to/Yanami.app" >&2
    exit 2
fi

executable="$app/Contents/MacOS/Yanami"
executable_directory="$(dirname "$executable")"
frameworks_directory="$app/Contents/Frameworks"
if [[ ! -x "$executable" ]]; then
    echo "macOS bundle executable is missing: $executable" >&2
    exit 1
fi

extract_rpaths() {
    local binary="$1"
    otool -l "$binary" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { waiting_for_path = 1; next }
        waiting_for_path && $1 == "path" { print $2; waiting_for_path = 0 }
    '
}

extract_dependencies() {
    local binary="$1"
    # `otool -L` also prints LC_ID_DYLIB. Read the actual load commands so a
    # library's own install name can never be mistaken for a dependency.
    otool -l "$binary" | awk '
        $1 == "cmd" && $2 ~ /^(LC_LOAD_DYLIB|LC_LOAD_WEAK_DYLIB|LC_REEXPORT_DYLIB|LC_LOAD_UPWARD_DYLIB|LC_LAZY_LOAD_DYLIB)$/ {
            waiting_for_name = 1
            next
        }
        waiting_for_name && $1 == "name" {
            dependency = $0
            sub(/^[[:space:]]*name[[:space:]]+/, "", dependency)
            sub(/[[:space:]]+\(offset[[:space:]][0-9]+\)$/, "", dependency)
            print dependency
            waiting_for_name = 0
        }
    '
}

expand_runtime_path() {
    local value="$1"
    local binary="$2"
    case "$value" in
        @loader_path)
            printf '%s\n' "$(dirname "$binary")"
            ;;
        @loader_path/*)
            printf '%s/%s\n' "$(dirname "$binary")" "${value#@loader_path/}"
            ;;
        @executable_path)
            printf '%s\n' "$executable_directory"
            ;;
        @executable_path/*)
            printf '%s/%s\n' "$executable_directory" "${value#@executable_path/}"
            ;;
        /*)
            printf '%s\n' "$value"
            ;;
        *)
            return 1
            ;;
    esac
}

resolve_rpath_dependency() {
    local dependency="$1"
    local binary="$2"
    local suffix="${dependency#@rpath/}"
    local candidate
    local runtime_path
    local expanded_path

    for candidate in \
        "$frameworks_directory/$suffix" \
        "$executable_directory/$suffix" \
        "$(dirname "$binary")/$suffix"; do
        [[ -e "$candidate" ]] && return 0
    done

    while IFS= read -r runtime_path; do
        [[ -z "$runtime_path" ]] && continue
        expanded_path="$(expand_runtime_path "$runtime_path" "$binary" || true)"
        [[ -z "$expanded_path" ]] && continue
        [[ -e "$expanded_path/$suffix" ]] && return 0
    done < <(
        {
            extract_rpaths "$binary"
            [[ "$binary" == "$executable" ]] || extract_rpaths "$executable"
        } | awk '!seen[$0]++'
    )
    return 1
}

verify_dependency() {
    local dependency="$1"
    local binary="$2"
    local resolved

    case "$dependency" in
        /usr/lib/*|/System/Library/*)
            return 0
            ;;
        @loader_path|@loader_path/*|@executable_path|@executable_path/*)
            resolved="$(expand_runtime_path "$dependency" "$binary" || true)"
            if [[ -n "$resolved" && -e "$resolved" ]]; then
                return 0
            fi
            ;;
        @rpath/*)
            if resolve_rpath_dependency "$dependency" "$binary"; then
                return 0
            fi
            ;;
    esac

    echo "Unresolved or non-portable dylib reference in $binary: $dependency" >&2
    return 1
}

mach_o_count=0
while IFS= read -r binary; do
    file "$binary" | grep -q 'Mach-O' || continue
    mach_o_count=$((mach_o_count + 1))
    while IFS= read -r dependency; do
        [[ -z "$dependency" ]] && continue
        verify_dependency "$dependency" "$binary"
    done < <(extract_dependencies "$binary")
done < <(find "$app/Contents" -type f | sort)

if [[ "$mach_o_count" -eq 0 ]]; then
    echo "macOS bundle contains no Mach-O files: $app" >&2
    exit 1
fi
printf 'Verified %s Mach-O files in %s\n' "$mach_o_count" "$app"
