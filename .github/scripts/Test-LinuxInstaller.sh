#!/usr/bin/env bash

set -euo pipefail

# Git for Windows otherwise implements `ln -s` as a plain file copy. Re-enter
# Bash with MSYS symlink emulation enabled so repeated install/update/uninstall
# exercises the launcher's link ownership contract without requiring NTFS
# symlink privileges.
windows_posix=false
case "${OSTYPE:-}" in
    msys*|cygwin*) windows_posix=true ;;
esac
if $windows_posix && [[ "${MSYS:-}" != *winsymlinks:* ]]; then
    export MSYS="${MSYS:+$MSYS }winsymlinks:sys"
    exec bash "$0" "$@"
fi

workspace="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
installer="$workspace/scripts/install-linux.sh"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/yanami-linux-installer-test.XXXXXX")"
cleanup() {
    case "$test_root" in
        "${TMPDIR:-/tmp}"/yanami-linux-installer-test.*) rm -rf -- "$test_root" ;;
        *) printf 'Refusing unsafe test cleanup: %s\n' "$test_root" >&2 ;;
    esac
}
trap cleanup EXIT

fixture_dir="$test_root/fixtures"
fake_bin="$test_root/bin"
test_home="$test_root/home with spaces % profile"
mkdir -p "$fixture_dir" "$fake_bin" "$test_home"
real_mv="$(command -v mv)"
real_stat="$(command -v stat)"

make_fixture() {
    local version="$1"
    local asset="$fixture_dir/Yanami-${version}-Linux-x86_64.AppImage"
    cat > "$asset" <<'APPIMAGE'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--appimage-extract" ]]; then
    icon_path="${2:?missing extraction path}"
    mkdir -p "squashfs-root/$(dirname "$icon_path")"
    printf 'fixture icon\n' > "squashfs-root/$icon_path"
    exit 0
fi
printf 'fixture AppImage\n'
APPIMAGE
    printf '# fixture version %s\n' "$version" >> "$asset"
    chmod 0755 "$asset"
    (
        cd "$fixture_dir"
        sha256sum "$(basename "$asset")" > "$(basename "$asset").sha256"
    )
}

make_fixture "0.2.0-dev.41"
make_fixture "0.2.0-dev.42"
make_fixture "0.2.0-dev.43"

cat > "$fake_bin/curl" <<'FAKE_CURL'
#!/usr/bin/env bash
set -euo pipefail
output=""
url=""
while (($# > 0)); do
    case "$1" in
        --output)
            output="${2:?missing output path}"
            shift 2
            ;;
        --header|--user-agent|--proto)
            shift 2
            ;;
        --retry)
            shift 2
            ;;
        --fail|--silent|--show-error|--location|--tlsv1.2)
            shift
            ;;
        *)
            url="$1"
            shift
            ;;
    esac
done
if [[ "$url" == */releases\?per_page=1 ]]; then
    printf '[\n  {\n    "tag_name": "v0.2.0-dev.41",\n    "draft": false\n  }\n]\n'
    exit 0
fi
[[ -n "$output" ]] || exit 2
cp "$YANAMI_TEST_FIXTURE_DIR/$(basename "$url")" "$output"
if [[ "$url" == *.sha256 && -n "${YANAMI_TEST_CURL_HOOK:-}" ]]; then
    "$YANAMI_TEST_CURL_HOOK"
fi
FAKE_CURL
chmod 0755 "$fake_bin/curl"
cat > "$fake_bin/uname" <<'FAKE_UNAME'
#!/usr/bin/env bash
case "${1:-}" in
    -s) printf 'Linux\n' ;;
    -m) printf '%s\n' "${YANAMI_TEST_MACHINE:-x86_64}" ;;
    *) printf 'Linux\n' ;;
esac
FAKE_UNAME
chmod 0755 "$fake_bin/uname"

export HOME="$test_home"
export XDG_DATA_HOME="$test_home/data"
export YANAMI_GITHUB_API_BASE="https://example.invalid/api"
export YANAMI_GITHUB_DOWNLOAD_BASE="https://example.invalid/download"
export YANAMI_TEST_FIXTURE_DIR="$fixture_dir"
export PATH="$fake_bin:$PATH"

main_call_line="$(grep -n '^main "\$@"$' "$installer" | cut -d: -f1)"
script_line_count="$(wc -l < "$installer")"
test "$main_call_line" = "$script_line_count"
# Keep quick-start commands safe and usable without coupling the tests to
# optional explanatory prose in the simplified bilingual READMEs.
readme_installer_prefix="bash -c 'set -o pipefail; curl --proto \"=https\" --tlsv1.2 -fsSL https://raw.githubusercontent.com/TwooSix/Yanami/main/scripts/install-linux.sh | bash"
for readme in "$workspace/README.md" "$workspace/README.zh-CN.md"; do
    for installer_args in "" " -s -- --version 0.2.0-dev.42" " -s -- --uninstall"; do
        if ! grep -Fxq "$readme_installer_prefix$installer_args'" "$readme"; then
            printf 'A safe install/version/uninstall command is missing from %s.\n' \
                "$readme" >&2
            exit 1
        fi
    done
    grep -Fq '${XDG_DATA_HOME:-~/.local/share}/yanami/Yanami.AppImage' "$readme"
done

for cut_line in 5 "$((main_call_line / 2))" "$((main_call_line - 1))"; do
    truncated_home="$test_root/truncated-$cut_line"
    mkdir -p "$truncated_home"
    if HOME="$truncated_home" \
        XDG_DATA_HOME="$truncated_home/data" \
        YANAMI_INSTALL_DIR="$truncated_home/data/yanami" \
        bash -c 'set -o pipefail; { head -n "$1" "$2"; exit 23; } | bash' \
            _ "$cut_line" "$installer" >/dev/null 2>&1; then
        printf 'A failed, truncated installer pipeline unexpectedly succeeded at line %s.\n' \
            "$cut_line" >&2
        exit 1
    fi
    test ! -e "$truncated_home/data/yanami"
    test ! -e "$truncated_home/.local/bin/yanami"
done

world_parent="$test_root/world-writable-parent"
mkdir -p "$world_parent"
chmod 0777 "$world_parent"
world_stat_bin="$test_root/world-stat-bin"
mkdir -p "$world_stat_bin"
cat > "$world_stat_bin/stat" <<'WORLD_STAT'
#!/usr/bin/env bash
set -euo pipefail
inspected_path="${!#}"
if [[ "$inspected_path" == "$YANAMI_TEST_WORLD_WRITABLE_PATH" ]] \
    && [[ "$*" == *"%u %a"* ]]; then
    printf '%s 777\n' "$EUID"
    exit 0
fi
exec "$YANAMI_TEST_REAL_STAT" "$@"
WORLD_STAT
chmod 0755 "$world_stat_bin/stat"
if PATH="$world_stat_bin:$PATH" \
    YANAMI_TEST_REAL_STAT="$real_stat" \
    YANAMI_TEST_WORLD_WRITABLE_PATH="$world_parent" \
    YANAMI_INSTALL_DIR="$world_parent/yanami" \
    bash "$installer" --version 0.2.0-dev.42 >/dev/null 2>&1; then
    printf 'A world-writable installation parent was unexpectedly accepted.\n' >&2
    exit 1
fi
test ! -e "$world_parent/yanami"
chmod 0700 "$world_parent"

symlink_parent_target="$test_root/symlink-parent-target"
symlink_parent="$test_root/symlink-parent"
mkdir -p "$symlink_parent_target"
ln -s "$symlink_parent_target" "$symlink_parent"
if YANAMI_INSTALL_DIR="$symlink_parent/yanami" \
    bash "$installer" --version 0.2.0-dev.42 >/dev/null 2>&1; then
    printf 'A symbolic-link installation parent was unexpectedly accepted.\n' >&2
    exit 1
fi
test ! -e "$symlink_parent_target/yanami"

if YANAMI_INSTALL_DIR='relative/install' \
    bash "$installer" --version 0.2.0-dev.42 >/dev/null 2>&1; then
    printf 'A relative installation directory was unexpectedly accepted.\n' >&2
    exit 1
fi

(umask 000; bash "$installer")

app_image="$XDG_DATA_HOME/yanami/Yanami.AppImage"
state_file="$XDG_DATA_HOME/yanami/install-state"
desktop_file="$XDG_DATA_HOME/applications/io.github.TwooSix.Yanami.desktop"
icon_file="$XDG_DATA_HOME/icons/hicolor/512x512/apps/io.github.TwooSix.Yanami.png"
launcher="$HOME/.local/bin/yanami"

test -x "$app_image"
test -f "$state_file"
grep -Fxq 'version=0.2.0-dev.41' "$state_file"
grep -Fxq 'installer=install-linux.sh' "$state_file"
if ! $windows_posix; then
    # A real Linux install must expose the launcher as a symbolic link.
    test -L "$launcher"
fi
test "$(readlink "$launcher")" = "$app_image"
test "$("$launcher")" = 'fixture AppImage'
grep -Fxq 'X-Yanami-Installer=install-linux.sh' "$desktop_file"
desktop_exec_path="${app_image//\%/%%}"
grep -Fxq "Exec=\"$desktop_exec_path\"" "$desktop_file"
test -f "$icon_file"

for secure_dir in "$XDG_DATA_HOME/yanami" "$HOME/.local/bin" \
    "$XDG_DATA_HOME/applications" \
    "$XDG_DATA_HOME/icons/hicolor/512x512/apps"; do
    secure_mode="$(stat -c '%a' "$secure_dir")"
    (( (8#$secure_mode & 8#022) == 0 ))
done

canonical_desktop="$workspace/apps/desktop/resources/linux/io.github.TwooSix.Yanami.desktop"
for desktop_key in Type Name Comment Icon Terminal Categories StartupWMClass; do
    canonical_value="$(grep -m1 "^${desktop_key}=" "$canonical_desktop")"
    grep -Fxq "$canonical_value" "$desktop_file"
done
grep -Fxq 'Categories=AudioVideo;Video;Player;' "$desktop_file"
grep -Fxq 'Comment=A modern Emby desktop client with danmaku support' "$desktop_file"
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$desktop_file"
fi

first_hash="$(sha256sum "$app_image" | awk '{print $1}')"
pending_victim="$test_root/pending-victim"
preplaced_pending="$XDG_DATA_HOME/yanami/.Yanami.AppImage.12345.new"
printf 'must not be overwritten\n' > "$pending_victim"
ln -s "$pending_victim" "$preplaced_pending"
bash "$installer"
test "$(sha256sum "$app_image" | awk '{print $1}')" = "$first_hash"
test -L "$preplaced_pending"
grep -Fxq 'must not be overwritten' "$pending_victim"
rm -f -- "$preplaced_pending"

chmod 0644 "$app_image"
if ! $windows_posix; then
    nonexec_mode="$(stat -c '%a' "$app_image")"
    (( (8#$nonexec_mode & 8#111) == 0 ))
fi
bash "$installer"
if ! $windows_posix; then
    restored_mode="$(stat -c '%a' "$app_image")"
    (( (8#$restored_mode & 8#111) == 8#111 ))
else
    # Git for Windows synthesizes executable access and reports 0644 even
    # after chmod 0755; the real permission-bit assertion runs on Linux CI.
    test -x "$app_image"
fi
test "$(sha256sum "$app_image" | awk '{print $1}')" = "$first_hash"

bash "$installer" --version v0.2.0-dev.42
grep -Fxq 'version=0.2.0-dev.42' "$state_file"
test "$(sha256sum "$app_image" | awk '{print $1}')" \
    = "$(sha256sum "$fixture_dir/Yanami-0.2.0-dev.42-Linux-x86_64.AppImage" | awk '{print $1}')"

swap_saved="$test_root/swap-saved-install"
swap_target="$test_root/swap-redirection-target"
swap_marker="$test_root/swap-hook-ran"
swap_hook="$test_root/swap-install-target.sh"
mkdir -p "$swap_target"
cat > "$swap_hook" <<'SWAP_HOOK'
#!/usr/bin/env bash
set -euo pipefail
if [[ ! -e "$YANAMI_TEST_SWAP_MARKER" ]]; then
    mv "$YANAMI_TEST_SWAP_INSTALL_DIR" "$YANAMI_TEST_SWAP_SAVED"
    ln -s "$YANAMI_TEST_SWAP_TARGET" "$YANAMI_TEST_SWAP_INSTALL_DIR"
    : > "$YANAMI_TEST_SWAP_MARKER"
fi
SWAP_HOOK
chmod 0755 "$swap_hook"
before_swap_hash="$(sha256sum "$app_image" | awk '{print $1}')"
if YANAMI_TEST_CURL_HOOK="$swap_hook" \
    YANAMI_TEST_SWAP_MARKER="$swap_marker" \
    YANAMI_TEST_SWAP_INSTALL_DIR="$XDG_DATA_HOME/yanami" \
    YANAMI_TEST_SWAP_SAVED="$swap_saved" \
    YANAMI_TEST_SWAP_TARGET="$swap_target" \
    bash "$installer" --version 0.2.0-dev.41 >/dev/null 2>&1; then
    printf 'A download-time installation-directory swap unexpectedly installed.\n' >&2
    exit 1
fi
test -L "$XDG_DATA_HOME/yanami"
test ! -e "$swap_target/Yanami.AppImage"
rm -f -- "$XDG_DATA_HOME/yanami"
mv "$swap_saved" "$XDG_DATA_HOME/yanami"
test "$(sha256sum "$app_image" | awk '{print $1}')" = "$before_swap_hash"
grep -Fxq 'version=0.2.0-dev.42' "$state_file"

fault_bin="$test_root/fault-bin"
fault_marker="$test_root/fault-mv-ran"
mkdir -p "$fault_bin"
cat > "$fault_bin/mv" <<'FAULT_MV'
#!/usr/bin/env bash
set -euo pipefail
destination="${!#}"
source_path="${@: -2:1}"
is_staged_commit=false
for argument in "$@"; do
    [[ "$argument" != "-nT" ]] || is_staged_commit=true
done
if $is_staged_commit \
    && [[ "$destination" == "${YANAMI_TEST_FAIL_MV_DEST:-}" ]] \
    && [[ ! -e "${YANAMI_TEST_FAIL_MV_MARKER:-}" ]]; then
    : > "$YANAMI_TEST_FAIL_MV_MARKER"
    exit 75
fi
if [[ "$destination" == "${YANAMI_TEST_FAIL_MV_RESTORE_DEST:-}" ]] \
    && [[ "${source_path##*/}" == .yanami-backup.* ]]; then
    exit 74
fi
exec "$YANAMI_TEST_REAL_MV" "$@"
FAULT_MV
chmod 0755 "$fault_bin/mv"
before_transaction_app="$(sha256sum "$app_image" | awk '{print $1}')"
before_transaction_state="$(sha256sum "$state_file" | awk '{print $1}')"
before_transaction_desktop="$(sha256sum "$desktop_file" | awk '{print $1}')"
before_transaction_icon="$(sha256sum "$icon_file" | awk '{print $1}')"
before_transaction_launcher="$(readlink "$launcher")"
if PATH="$fault_bin:$PATH" \
    YANAMI_TEST_REAL_MV="$real_mv" \
    YANAMI_TEST_FAIL_MV_DEST="$desktop_file" \
    YANAMI_TEST_FAIL_MV_MARKER="$fault_marker" \
    bash "$installer" --version 0.2.0-dev.43 >/dev/null 2>&1; then
    printf 'A deliberately failed transactional update unexpectedly succeeded.\n' >&2
    exit 1
fi
test -e "$fault_marker"
test "$(sha256sum "$app_image" | awk '{print $1}')" = "$before_transaction_app"
test "$(sha256sum "$state_file" | awk '{print $1}')" = "$before_transaction_state"
test "$(sha256sum "$desktop_file" | awk '{print $1}')" = "$before_transaction_desktop"
test "$(sha256sum "$icon_file" | awk '{print $1}')" = "$before_transaction_icon"
test "$(readlink "$launcher")" = "$before_transaction_launcher"
grep -Fxq 'version=0.2.0-dev.42' "$state_file"
for artifact_dir in "$XDG_DATA_HOME/yanami" "$HOME/.local/bin" \
    "$XDG_DATA_HOME/applications" \
    "$XDG_DATA_HOME/icons/hicolor/512x512/apps"; do
    if compgen -G "$artifact_dir/.yanami-backup.*" >/dev/null \
        || compgen -G "$artifact_dir/.*.new" >/dev/null; then
        printf 'A failed transaction left staging files in %s.\n' "$artifact_dir" >&2
        exit 1
    fi
done

restore_fault_marker="$test_root/fault-mv-restore-ran"
set +e
restore_fault_output="$(PATH="$fault_bin:$PATH" \
    YANAMI_TEST_REAL_MV="$real_mv" \
    YANAMI_TEST_FAIL_MV_DEST="$desktop_file" \
    YANAMI_TEST_FAIL_MV_MARKER="$restore_fault_marker" \
    YANAMI_TEST_FAIL_MV_RESTORE_DEST="$app_image" \
    bash "$installer" --version 0.2.0-dev.43 2>&1)"
restore_fault_status=$?
set -e
test "$restore_fault_status" -ne 0
test -e "$restore_fault_marker"
test ! -e "$app_image"
mapfile -t preserved_backups < <(compgen -G "$XDG_DATA_HOME/yanami/.yanami-backup.*")
test "${#preserved_backups[@]}" -eq 1
preserved_backup="${preserved_backups[0]}"
test "$(sha256sum "$preserved_backup" | awk '{print $1}')" = "$before_transaction_app"
grep -Fq 'preserved for manual recovery' <<< "$restore_fault_output"
grep -Fq "$app_image" <<< "$restore_fault_output"
grep -Fq "$preserved_backup" <<< "$restore_fault_output"
test "$(sha256sum "$state_file" | awk '{print $1}')" = "$before_transaction_state"
test "$(sha256sum "$desktop_file" | awk '{print $1}')" = "$before_transaction_desktop"
test "$(sha256sum "$icon_file" | awk '{print $1}')" = "$before_transaction_icon"
test "$(readlink "$launcher")" = "$before_transaction_launcher"
"$real_mv" -fT -- "$preserved_backup" "$app_image"
test "$(sha256sum "$app_image" | awk '{print $1}')" = "$before_transaction_app"

printf '%064d  %s\n' 0 'Yanami-0.2.0-dev.41-Linux-x86_64.AppImage' \
    > "$fixture_dir/Yanami-0.2.0-dev.41-Linux-x86_64.AppImage.sha256"
before_failed_update="$(sha256sum "$app_image" | awk '{print $1}')"
if bash "$installer" --version 0.2.0-dev.41 >/dev/null 2>&1; then
    printf 'A mismatched release checksum unexpectedly installed.\n' >&2
    exit 1
fi
test "$(sha256sum "$app_image" | awk '{print $1}')" = "$before_failed_update"
grep -Fxq 'version=0.2.0-dev.42' "$state_file"

YANAMI_TEST_MACHINE=aarch64 bash "$installer" --uninstall
test ! -e "$app_image"
test ! -e "$state_file"
test ! -e "$desktop_file"
test ! -e "$icon_file"
test ! -e "$launcher"

bash "$installer" --version 0.2.0-dev.42
printf 'installer marker was tampered\n' > "$state_file"
set +e
partial_output="$(YANAMI_TEST_MACHINE=aarch64 \
    bash "$installer" --uninstall 2>&1)"
partial_status=$?
set -e
test "$partial_status" -ne 0
grep -Fq 'uninstall is incomplete' <<< "$partial_output"
grep -Fq "$app_image" <<< "$partial_output"
grep -Fq "$state_file" <<< "$partial_output"
grep -Fq "$XDG_DATA_HOME/yanami" <<< "$partial_output"
if grep -Fq 'has been removed' <<< "$partial_output"; then
    printf 'A partial uninstall falsely reported complete removal.\n' >&2
    exit 1
fi
test -e "$app_image"
test -e "$state_file"
test ! -e "$desktop_file"
test ! -e "$icon_file"
test ! -e "$launcher"
printf 'installer=install-linux.sh\n' > "$state_file"
YANAMI_TEST_MACHINE=aarch64 bash "$installer" --uninstall >/dev/null
test ! -e "$app_image"
test ! -e "$state_file"

mkdir -p "$(dirname "$icon_file")"
printf 'unmanaged no-desktop icon\n' > "$icon_file"
bash "$installer" --version 0.2.0-dev.42 --no-desktop
test -f "$icon_file"
test ! -e "$desktop_file"
set +e
no_desktop_output="$(bash "$installer" --uninstall 2>&1)"
no_desktop_status=$?
set -e
test "$no_desktop_status" -ne 0
grep -Fq 'uninstall is incomplete' <<< "$no_desktop_output"
grep -Fq "$icon_file" <<< "$no_desktop_output"
grep -Fxq 'unmanaged no-desktop icon' "$icon_file"
test ! -e "$app_image"
test ! -e "$state_file"
test ! -e "$launcher"
rm -f -- "$icon_file"

mkdir -p "$(dirname "$app_image")" "$(dirname "$desktop_file")" \
    "$(dirname "$icon_file")" "$(dirname "$launcher")"
printf 'unmanaged AppImage\n' > "$app_image"
printf 'unmanaged state\n' > "$state_file"
printf 'unmanaged desktop\n' > "$desktop_file"
printf 'unmanaged icon\n' > "$icon_file"
printf 'unmanaged launcher\n' > "$launcher"
set +e
unmanaged_output="$(bash "$installer" --uninstall 2>&1)"
unmanaged_status=$?
set -e
test "$unmanaged_status" -ne 0
grep -Fq 'uninstall is incomplete' <<< "$unmanaged_output"
for retained_path in "$app_image" "$state_file" "$desktop_file" \
    "$icon_file" "$launcher" "$XDG_DATA_HOME/yanami"; do
    grep -Fq "$retained_path" <<< "$unmanaged_output"
done
if grep -Fq 'has been removed' <<< "$unmanaged_output"; then
    printf 'An unmanaged installation falsely reported complete removal.\n' >&2
    exit 1
fi
grep -Fxq 'unmanaged AppImage' "$app_image"
grep -Fxq 'unmanaged state' "$state_file"
grep -Fxq 'unmanaged desktop' "$desktop_file"
grep -Fxq 'unmanaged icon' "$icon_file"
grep -Fxq 'unmanaged launcher' "$launcher"

printf 'Linux installer tests passed.\n'
