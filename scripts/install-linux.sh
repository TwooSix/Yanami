#!/usr/bin/env bash

# Keep every operation inside one complete function. When this file is streamed
# to Bash, a truncated response can therefore define at most an inert/invalid
# function; installation starts only after Bash reaches the final invocation.
main() {
    set -euo pipefail
    umask 077

    readonly yanami_repository="${YANAMI_GITHUB_REPOSITORY:-TwooSix/Yanami}"
    readonly github_api_base="${YANAMI_GITHUB_API_BASE:-https://api.github.com}"
    readonly github_download_base="${YANAMI_GITHUB_DOWNLOAD_BASE:-https://github.com/${yanami_repository}/releases/download}"
    readonly desktop_file_name="io.github.TwooSix.Yanami.desktop"
    readonly icon_file_name="io.github.TwooSix.Yanami.png"
    readonly managed_marker="X-Yanami-Installer=install-linux.sh"
    readonly current_euid="$EUID"

    local requested_version=""
    local uninstall=false
    local install_desktop=true
    local tmp_dir=""
    local pending_image=""
    local pending_state=""
    local pending_launcher=""
    local pending_desktop=""
    local pending_icon=""
    local transaction_active=false
    local -a txn_targets=()
    local -a txn_stages=()
    local -a txn_backups=()
    local -a txn_backed_up=()
    local -a txn_committed=()
    local -a retained_paths=()

    usage() {
        cat <<'EOF'
Install or update the latest published Yanami Linux x86_64 AppImage for this user.

Usage:
  install-linux.sh [--version VERSION] [--no-desktop]
  install-linux.sh --uninstall

Options:
  --version VERSION  Install an exact release version (an optional leading v is accepted).
  --no-desktop       Do not create the desktop-menu entry and icon.
  --uninstall        Remove files managed by this script for the current user.
  -h, --help         Show this help.

Locations can be overridden for testing or nonstandard layouts with
YANAMI_INSTALL_DIR, YANAMI_BIN_DIR, YANAMI_APPLICATIONS_DIR, and
YANAMI_ICON_DIR. No sudo access is requested. Uninstall is available on any
Linux architecture even though the published AppImage is currently x86_64-only.
EOF
    }

    fail() {
        printf 'Yanami installer: %s\n' "$*" >&2
        exit 1
    }

    parent_path() {
        local path="${1%/}"
        if [[ "$path" == /*/* ]]; then
            REPLY="${path%/*}"
            [[ -n "$REPLY" ]] || REPLY="/"
        else
            REPLY="/"
        fi
    }

    assert_path_syntax() {
        local path="$1"
        [[ "$path" == /* && "$path" != "/" ]] \
            || fail "installation paths must be absolute non-root paths: $path"
        case "$path" in
            *$'\n'*|*$'\r'*|*$'\t'*) fail "installation paths cannot contain control whitespace" ;;
            *//*|*/./*|*/.|*/../*|*/..) fail "installation paths must not contain empty, '.' or '..' components: $path" ;;
        esac
    }

    assert_owned_not_writable() {
        local path="$1"
        local description="$2"
        local metadata owner mode mode_value
        metadata="$(stat -c '%u %a' -- "$path")" \
            || fail "could not inspect $description: $path"
        owner="${metadata%% *}"
        mode="${metadata##* }"
        [[ "$owner" == "$current_euid" ]] \
            || fail "$description is not owned by the current user: $path"
        [[ "$mode" =~ ^[0-7]{3,4}$ ]] \
            || fail "could not validate permissions for $description: $path"
        mode_value=$((8#$mode))
        (( (mode_value & 8#022) == 0 )) \
            || fail "$description is writable by group or other users: $path"
    }

    # Reject symbolic-link directory components. For a missing target, also
    # require its nearest existing parent to be owned by this user and not
    # group/world writable. Linux has no Windows reparse-point type; lstat-style
    # -L checks are the relevant path-redirection defense here.
    validate_directory_destination() {
        local path="$1"
        local current="/"
        local remainder="${path#/}"
        local component
        local nearest="/"
        local previous_nearest="/"
        local missing=false

        assert_path_syntax "$path"
        while [[ -n "$remainder" ]]; do
            component="${remainder%%/*}"
            if [[ "$remainder" == */* ]]; then
                remainder="${remainder#*/}"
            else
                remainder=""
            fi
            current="${current%/}/$component"
            if $missing; then
                continue
            fi
            [[ ! -L "$current" ]] \
                || fail "refusing a symbolic-link directory component: $current"
            if [[ -e "$current" ]]; then
                [[ -d "$current" ]] \
                    || fail "an installation directory component is not a directory: $current"
                previous_nearest="$nearest"
                nearest="$current"
            else
                missing=true
            fi
        done

        [[ "$nearest" != "/" ]] \
            || fail "the nearest existing installation parent must be owned by the current user: $path"
        assert_owned_not_writable "$nearest" "installation directory or nearest existing parent"
        if ! $missing; then
            [[ "$previous_nearest" != "/" ]] \
                || fail "the installation directory parent must be owned by the current user: $path"
            assert_owned_not_writable "$previous_nearest" "installation directory parent"
        fi
    }

    ensure_secure_directory() {
        local path="$1"
        validate_directory_destination "$path"
        if [[ ! -d "$path" ]]; then
            mkdir -p -- "$path"
        fi
        validate_directory_destination "$path"
        [[ -d "$path" && ! -L "$path" ]] \
            || fail "could not create a secure installation directory: $path"
        assert_owned_not_writable "$path" "installation directory"
    }

    assert_secure_regular_or_absent() {
        local path="$1"
        local description="$2"
        [[ ! -L "$path" ]] || fail "refusing symbolic-link $description: $path"
        if [[ -e "$path" ]]; then
            [[ -f "$path" ]] || fail "$description is not a regular file: $path"
            assert_owned_not_writable "$path" "$description"
        fi
    }

    assert_secure_launcher_or_absent() {
        local path="$1"
        if [[ -L "$path" ]]; then
            local owner
            owner="$(stat -c '%u' -- "$path")" \
                || fail "could not inspect launcher link: $path"
            [[ "$owner" == "$current_euid" ]] \
                || fail "launcher link is not owned by the current user: $path"
        elif [[ -e "$path" ]]; then
            fail "launcher path is not a symbolic link: $path"
        fi
    }

    is_managed_desktop_file() {
        [[ -f "$desktop_file" && ! -L "$desktop_file" ]] \
            && grep -Fqx "$managed_marker" "$desktop_file"
    }

    is_managed_install() {
        [[ -f "$state_file" && ! -L "$state_file" ]] \
            && grep -Fqx 'installer=install-linux.sh' "$state_file"
    }

    append_retained() {
        local candidate="$1"
        local existing
        for existing in "${retained_paths[@]:-}"; do
            [[ "$existing" != "$candidate" ]] || return 0
        done
        retained_paths+=("$candidate")
    }

    rollback_transaction() {
        local index target backup
        for ((index=${#txn_targets[@]} - 1; index >= 0; index--)); do
            target="${txn_targets[index]}"
            backup="${txn_backups[index]}"
            if [[ "${txn_committed[index]}" == "1" ]]; then
                rm -f -- "$target" 2>/dev/null || true
                txn_committed[index]=0
            fi
            if [[ "${txn_backed_up[index]}" == "1" ]]; then
                if mv -fT -- "$backup" "$target" 2>/dev/null; then
                    txn_backed_up[index]=0
                    txn_backups[index]=""
                else
                    printf 'Yanami installer: could not restore %s; the previous file is preserved for manual recovery at %s\n' \
                        "$target" "$backup" >&2
                fi
            fi
        done
        transaction_active=false
    }

    cleanup() {
        if $transaction_active; then
            rollback_transaction
        fi
        [[ -z "$pending_image" ]] || rm -f -- "$pending_image" 2>/dev/null || true
        [[ -z "$pending_state" ]] || rm -f -- "$pending_state" 2>/dev/null || true
        [[ -z "$pending_launcher" ]] || rm -f -- "$pending_launcher" 2>/dev/null || true
        [[ -z "$pending_desktop" ]] || rm -f -- "$pending_desktop" 2>/dev/null || true
        [[ -z "$pending_icon" ]] || rm -f -- "$pending_icon" 2>/dev/null || true
        local index backup
        for ((index=0; index<${#txn_backups[@]}; index++)); do
            backup="${txn_backups[index]}"
            # A failed restore must not delete the only copy of the old file.
            [[ "${txn_backed_up[index]}" != "1" ]] || continue
            [[ -z "$backup" ]] || rm -f -- "$backup" 2>/dev/null || true
        done
        if [[ -n "$tmp_dir" ]]; then
            case "$tmp_dir" in
                "${TMPDIR:-/tmp}"/yanami-install.*) rm -rf -- "$tmp_dir" ;;
                *) printf 'Yanami installer: refusing unsafe temporary cleanup: %s\n' "$tmp_dir" >&2 ;;
            esac
        fi
    }
    trap cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    while (($# > 0)); do
        case "$1" in
            --version)
                (($# >= 2)) || fail "--version requires a value"
                requested_version="$2"
                shift 2
                ;;
            --no-desktop)
                install_desktop=false
                shift
                ;;
            --uninstall)
                uninstall=true
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                fail "unknown option: $1"
                ;;
        esac
    done

    [[ "$(uname -s)" == "Linux" ]] || fail "this script only supports Linux"
    [[ -n "${HOME:-}" ]] || fail 'HOME is not set'

    local data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
    local install_dir="${YANAMI_INSTALL_DIR:-$data_home/yanami}"
    local bin_dir="${YANAMI_BIN_DIR:-$HOME/.local/bin}"
    local applications_dir="${YANAMI_APPLICATIONS_DIR:-$data_home/applications}"
    local icon_dir="${YANAMI_ICON_DIR:-$data_home/icons/hicolor/512x512/apps}"
    install_dir="${install_dir%/}"
    bin_dir="${bin_dir%/}"
    applications_dir="${applications_dir%/}"
    icon_dir="${icon_dir%/}"
    readonly data_home install_dir bin_dir applications_dir icon_dir
    readonly app_image="$install_dir/Yanami.AppImage"
    readonly state_file="$install_dir/install-state"
    readonly launcher_link="$bin_dir/yanami"
    readonly desktop_file="$applications_dir/$desktop_file_name"
    readonly icon_file="$icon_dir/$icon_file_name"

    for target_dir in "$install_dir" "$bin_dir" "$applications_dir" "$icon_dir"; do
        assert_path_syntax "$target_dir"
    done

    for command_name in grep mv readlink rm rmdir stat uname; do
        command -v "$command_name" >/dev/null 2>&1 \
            || fail "required command is missing: $command_name"
    done

    if $uninstall; then
        local managed_desktop=false
        local launcher_owner=""

        for candidate in "$launcher_link" "$desktop_file" "$app_image" "$state_file" "$icon_file"; do
            if [[ -e "$candidate" || -L "$candidate" ]]; then
                parent_path "$candidate"
                validate_directory_destination "$REPLY"
            fi
        done
        if [[ -L "$launcher_link" ]]; then
            launcher_owner="$(stat -c '%u' -- "$launcher_link")" \
                || fail "could not inspect launcher link: $launcher_link"
            [[ "$launcher_owner" == "$current_euid" ]] \
                || fail "launcher link is not owned by the current user: $launcher_link"
        elif [[ -e "$launcher_link" ]]; then
            assert_owned_not_writable "$launcher_link" "launcher path"
        fi
        assert_secure_regular_or_absent "$desktop_file" "desktop entry"
        assert_secure_regular_or_absent "$app_image" "AppImage"
        assert_secure_regular_or_absent "$state_file" "installer state"
        assert_secure_regular_or_absent "$icon_file" "desktop icon"

        if [[ -L "$launcher_link" ]]; then
            local link_target
            link_target="$(readlink "$launcher_link")"
            if [[ "$link_target" == "$app_image" ]]; then
                rm -f -- "$launcher_link"
            else
                printf 'Yanami installer: leaving unrelated symlink %s -> %s\n' \
                    "$launcher_link" "$link_target" >&2
                append_retained "$launcher_link"
            fi
        elif [[ -e "$launcher_link" ]]; then
            printf 'Yanami installer: leaving unrelated launcher %s\n' \
                "$launcher_link" >&2
            append_retained "$launcher_link"
        fi

        if is_managed_desktop_file; then
            managed_desktop=true
            rm -f -- "$desktop_file"
        elif [[ -e "$desktop_file" ]]; then
            printf 'Yanami installer: leaving unmanaged desktop entry %s\n' \
                "$desktop_file" >&2
            append_retained "$desktop_file"
        fi

        if is_managed_install; then
            rm -f -- "$app_image" "$state_file"
        else
            if [[ -e "$app_image" ]]; then
                printf 'Yanami installer: leaving AppImage without a managed state marker at %s\n' \
                    "$app_image" >&2
                append_retained "$app_image"
            fi
            if [[ -e "$state_file" ]]; then
                printf 'Yanami installer: leaving unverified installer state at %s\n' \
                    "$state_file" >&2
                append_retained "$state_file"
            fi
        fi

        if $managed_desktop; then
            rm -f -- "$icon_file"
        elif [[ -e "$icon_file" ]]; then
            printf 'Yanami installer: leaving icon without a managed desktop marker at %s\n' \
                "$icon_file" >&2
            append_retained "$icon_file"
        fi

        if ! rmdir -- "$install_dir" 2>/dev/null && [[ -d "$install_dir" ]]; then
            append_retained "$install_dir"
        fi
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
        fi
        if ((${#retained_paths[@]} > 0)); then
            printf 'Yanami uninstall is incomplete; retained unverified or unrelated paths:\n' >&2
            printf '  %s\n' "${retained_paths[@]}" >&2
            cleanup
            trap - EXIT HUP INT TERM
            return 1
        fi
        printf 'Yanami has been removed from this user account.\n'
        cleanup
        trap - EXIT HUP INT TERM
        return 0
    fi

    case "$(uname -m)" in
        x86_64|amd64) ;;
        *) fail "only the published Linux x86_64 AppImage is currently supported (uninstall remains available)" ;;
    esac

    for command_name in awk chmod curl install ln mkdir mktemp mv sha256sum tr; do
        command -v "$command_name" >/dev/null 2>&1 \
            || fail "required command is missing: $command_name"
    done
    mv --help 2>&1 | grep -q -- '--no-target-directory' \
        || fail "GNU mv with -T/--no-target-directory is required"
    ln --help 2>&1 | grep -q -- '--no-target-directory' \
        || fail "GNU ln with -T/--no-target-directory is required"

    validate_directory_destination "$install_dir"
    validate_directory_destination "$bin_dir"
    if $install_desktop; then
        validate_directory_destination "$applications_dir"
        validate_directory_destination "$icon_dir"
    fi

    assert_secure_launcher_or_absent "$launcher_link"
    assert_secure_regular_or_absent "$app_image" "AppImage"
    assert_secure_regular_or_absent "$state_file" "installer state"
    if [[ -e "$launcher_link" || -L "$launcher_link" ]]; then
        [[ -L "$launcher_link" && "$(readlink "$launcher_link")" == "$app_image" ]] \
            || fail "refusing to replace unrelated launcher: $launcher_link"
    fi
    if $install_desktop; then
        assert_secure_regular_or_absent "$desktop_file" "desktop entry"
        assert_secure_regular_or_absent "$icon_file" "desktop icon"
        if [[ -e "$desktop_file" ]] && ! is_managed_desktop_file; then
            fail "refusing to replace unmanaged desktop entry: $desktop_file"
        fi
        if [[ -e "$icon_file" ]] && ! is_managed_desktop_file; then
            fail "refusing to replace an icon without a managed desktop entry: $icon_file"
        fi
    fi
    if ! is_managed_install; then
        [[ ! -e "$state_file" ]] \
            || fail "refusing to replace an unmanaged state file: $state_file"
        [[ ! -e "$app_image" ]] \
            || fail "refusing to replace an unmanaged AppImage: $app_image"
    fi

    local semver_pattern='^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$'
    local version release_json release_tag
    if [[ -n "$requested_version" ]]; then
        version="${requested_version#v}"
    else
        release_json="$(curl \
            --fail --silent --show-error --location --retry 3 \
            --proto '=https' --tlsv1.2 \
            --header 'Accept: application/vnd.github+json' \
            --header 'X-GitHub-Api-Version: 2022-11-28' \
            --user-agent 'Yanami Linux installer' \
            "$github_api_base/repos/$yanami_repository/releases?per_page=1")" \
            || fail "could not resolve the latest published release"
        release_tag=""
        while IFS= read -r json_line; do
            if [[ "$json_line" =~ \"tag_name\"[[:space:]]*:[[:space:]]*\"([^\"]+)\" ]]; then
                release_tag="${BASH_REMATCH[1]}"
                break
            fi
        done <<< "$release_json"
        [[ "$release_tag" == v* ]] \
            || fail "GitHub did not return a published Yanami release tag"
        version="${release_tag#v}"
    fi
    [[ "$version" =~ $semver_pattern ]] || fail "invalid release version: $version"

    local asset_name="Yanami-${version}-Linux-x86_64.AppImage"
    local asset_url="$github_download_base/v${version}/${asset_name}"
    local checksum_url="${asset_url}.sha256"
    readonly asset_name asset_url checksum_url

    local tmp_base="${TMPDIR:-/tmp}"
    [[ -d "$tmp_base" && ! -L "$tmp_base" ]] \
        || fail "temporary directory must be an existing non-symlink directory: $tmp_base"
    tmp_dir="$(mktemp -d "$tmp_base/yanami-install.XXXXXX")"
    [[ -d "$tmp_dir" && ! -L "$tmp_dir" ]] \
        || fail "could not create a secure temporary directory"
    assert_owned_not_writable "$tmp_dir" "temporary directory"

    local downloaded_asset="$tmp_dir/$asset_name"
    local downloaded_checksum="$downloaded_asset.sha256"
    curl --fail --silent --show-error --location --retry 3 \
        --proto '=https' --tlsv1.2 --user-agent 'Yanami Linux installer' \
        --output "$downloaded_asset" "$asset_url"
    curl --fail --silent --show-error --location --retry 3 \
        --proto '=https' --tlsv1.2 --user-agent 'Yanami Linux installer' \
        --output "$downloaded_checksum" "$checksum_url"
    assert_secure_regular_or_absent "$downloaded_asset" "downloaded AppImage"
    assert_secure_regular_or_absent "$downloaded_checksum" "downloaded checksum"

    local checksum_text expected_hash checksum_name actual_hash
    checksum_text="$(tr -d '\r' < "$downloaded_checksum")"
    if [[ ! "$checksum_text" =~ ^([0-9a-fA-F]{64})[[:space:]]+\*?([^[:space:]]+)[[:space:]]*$ ]]; then
        fail "the published checksum file is malformed"
    fi
    expected_hash="${BASH_REMATCH[1],,}"
    checksum_name="${BASH_REMATCH[2]}"
    [[ "$checksum_name" == "$asset_name" ]] \
        || fail "the checksum names a different asset: $checksum_name"
    actual_hash="$(sha256sum "$downloaded_asset")"
    actual_hash="${actual_hash%% *}"
    actual_hash="${actual_hash,,}"
    [[ "$actual_hash" == "$expected_hash" ]] \
        || fail "the AppImage SHA-256 does not match its published checksum"
    chmod 0755 "$downloaded_asset"

    local extracted_icon=""
    if $install_desktop; then
        (
            cd "$tmp_dir"
            "$downloaded_asset" --appimage-extract \
                "usr/share/icons/hicolor/512x512/apps/$icon_file_name" >/dev/null
        ) || fail "the verified AppImage does not contain its desktop icon"
        extracted_icon="$tmp_dir/squashfs-root/usr/share/icons/hicolor/512x512/apps/$icon_file_name"
        [[ -f "$extracted_icon" && ! -L "$extracted_icon" ]] \
            || fail "the verified AppImage is missing a regular desktop icon"
        assert_owned_not_writable "$extracted_icon" "extracted desktop icon"
    fi

    # Downloads may take time. Revalidate every destination immediately before
    # creating directories or staging files so a path swap cannot redirect the
    # commit.
    validate_directory_destination "$install_dir"
    validate_directory_destination "$bin_dir"
    if $install_desktop; then
        validate_directory_destination "$applications_dir"
        validate_directory_destination "$icon_dir"
    fi
    assert_secure_launcher_or_absent "$launcher_link"
    assert_secure_regular_or_absent "$app_image" "AppImage"
    assert_secure_regular_or_absent "$state_file" "installer state"
    if [[ -e "$launcher_link" || -L "$launcher_link" ]]; then
        [[ -L "$launcher_link" && "$(readlink "$launcher_link")" == "$app_image" ]] \
            || fail "launcher changed while the release was downloading: $launcher_link"
    fi
    if $install_desktop; then
        assert_secure_regular_or_absent "$desktop_file" "desktop entry"
        assert_secure_regular_or_absent "$icon_file" "desktop icon"
        if [[ -e "$desktop_file" ]] && ! is_managed_desktop_file; then
            fail "desktop entry changed while the release was downloading: $desktop_file"
        fi
        if [[ -e "$icon_file" ]] && ! is_managed_desktop_file; then
            fail "desktop icon changed while the release was downloading: $icon_file"
        fi
    fi
    if is_managed_install; then
        :
    else
        [[ ! -e "$app_image" && ! -e "$state_file" ]] \
            || fail "installation ownership changed while the release was downloading"
    fi

    ensure_secure_directory "$install_dir"
    ensure_secure_directory "$bin_dir"
    if $install_desktop; then
        ensure_secure_directory "$applications_dir"
        ensure_secure_directory "$icon_dir"
    fi

    local install_action="installed"
    if [[ -f "$app_image" && ! -L "$app_image" && -x "$app_image" ]] \
        && [[ "$(sha256sum "$app_image" | awk '{print tolower($1)}')" == "$expected_hash" ]]; then
        install_action="already current"
    fi

    pending_image="$(mktemp "$install_dir/.Yanami.AppImage.XXXXXX.new")"
    install -m 0755 -- "$downloaded_asset" "$pending_image"
    assert_secure_regular_or_absent "$pending_image" "staged AppImage"

    pending_state="$(mktemp "$install_dir/.install-state.XXXXXX.new")"
    printf 'installer=install-linux.sh\nversion=%s\nsha256=%s\nasset=%s\n' \
        "$version" "$expected_hash" "$asset_name" > "$pending_state"
    chmod 0644 "$pending_state"
    assert_secure_regular_or_absent "$pending_state" "staged installer state"

    pending_launcher="$(mktemp "$bin_dir/.yanami.XXXXXX.new")"
    rm -f -- "$pending_launcher"
    ln -sT -- "$app_image" "$pending_launcher"
    [[ -L "$pending_launcher" && "$(readlink "$pending_launcher")" == "$app_image" ]] \
        || fail "could not stage the launcher link"

    if $install_desktop; then
        pending_icon="$(mktemp "$icon_dir/.${icon_file_name}.XXXXXX.new")"
        install -m 0644 -- "$extracted_icon" "$pending_icon"
        assert_secure_regular_or_absent "$pending_icon" "staged desktop icon"

        local escaped_exec="$app_image"
        escaped_exec="${escaped_exec//\\/\\\\}"
        escaped_exec="${escaped_exec//\"/\\\"}"
        escaped_exec="${escaped_exec//\`/\\\`}"
        escaped_exec="${escaped_exec//\$/\\\$}"
        escaped_exec="${escaped_exec//\%/%%}"
        pending_desktop="$(mktemp "$applications_dir/.${desktop_file_name}.XXXXXX.new")"
        {
            printf '[Desktop Entry]\n'
            printf 'Type=Application\n'
            printf 'Name=Yanami\n'
            printf 'Comment=A modern Emby desktop client with danmaku support\n'
            printf 'Exec="%s"\n' "$escaped_exec"
            printf 'Icon=io.github.TwooSix.Yanami\n'
            printf 'Terminal=false\n'
            printf 'Categories=AudioVideo;Video;Player;\n'
            printf 'StartupWMClass=Yanami\n'
            printf 'X-AppImage-Version=%s\n' "$version"
            printf '%s\n' "$managed_marker"
        } > "$pending_desktop"
        chmod 0644 "$pending_desktop"
        assert_secure_regular_or_absent "$pending_desktop" "staged desktop entry"
    fi

    txn_targets=("$app_image" "$state_file" "$launcher_link")
    txn_stages=("$pending_image" "$pending_state" "$pending_launcher")
    if $install_desktop; then
        txn_targets+=("$icon_file" "$desktop_file")
        txn_stages+=("$pending_icon" "$pending_desktop")
    fi
    local item_count="${#txn_targets[@]}"
    local index target stage backup_template backup
    for ((index=0; index<item_count; index++)); do
        txn_backups+=("")
        txn_backed_up+=(0)
        txn_committed+=(0)
    done
    transaction_active=true

    # Stage every derived artifact first, then swap the set transactionally.
    # Any later failure (or EXIT/signal) removes committed replacements and
    # restores all prior files from same-directory random backups.
    for ((index=0; index<item_count; index++)); do
        target="${txn_targets[index]}"
        stage="${txn_stages[index]}"
        if [[ -e "$target" || -L "$target" ]]; then
            parent_path "$target"
            backup_template="$REPLY/.yanami-backup.XXXXXX"
            backup="$(mktemp "$backup_template")"
            txn_backups[index]="$backup"
            if ! mv -fT -- "$target" "$backup"; then
                fail "could not preserve the existing installation artifact: $target"
            fi
            txn_backed_up[index]=1
        fi
        if ! mv -nT -- "$stage" "$target"; then
            fail "could not commit the staged installation artifact: $target"
        fi
        if [[ -e "$stage" || -L "$stage" ]]; then
            fail "the installation destination changed during commit: $target"
        fi
        if [[ "$target" == "$launcher_link" ]]; then
            [[ -L "$target" && "$(readlink "$target")" == "$app_image" ]] \
                || fail "the committed launcher is invalid: $target"
        else
            assert_secure_regular_or_absent "$target" "committed installation artifact"
            [[ -f "$target" ]] \
                || fail "the committed installation artifact is missing: $target"
        fi
        txn_committed[index]=1
        txn_stages[index]=""
        case "$stage" in
            "$pending_image") pending_image="" ;;
            "$pending_state") pending_state="" ;;
            "$pending_launcher") pending_launcher="" ;;
            "$pending_icon") pending_icon="" ;;
            "$pending_desktop") pending_desktop="" ;;
        esac
    done

    transaction_active=false
    for ((index=0; index<item_count; index++)); do
        backup="${txn_backups[index]}"
        [[ -z "$backup" ]] || rm -f -- "$backup"
        txn_backups[index]=""
    done

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
    fi

    printf 'Yanami %s is %s at %s\n' "$version" "$install_action" "$app_image"
    case ":${PATH:-}:" in
        *":$bin_dir:"*) ;;
        *) printf 'Add %s to PATH to launch Yanami with: yanami\n' "$bin_dir" ;;
    esac
    cleanup
    trap - EXIT HUP INT TERM
}

main "$@"
