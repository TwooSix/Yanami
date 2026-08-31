$ErrorActionPreference = "Stop"
$workflowPath = Join-Path $PSScriptRoot "..\workflows\release.yml"
$workflow = Get-Content -LiteralPath $workflowPath -Raw
$assertions = 0

function Assert-Match {
    param([string]$Label, [string]$Pattern)
    $script:assertions++
    if ($workflow -notmatch $Pattern) {
        throw "$Label is missing from the release workflow."
    }
}

function Assert-Count {
    param([string]$Label, [string]$Pattern, [int]$Expected)
    $script:assertions++
    $actual = [regex]::Matches($workflow, $Pattern).Count
    if ($actual -ne $Expected) {
        throw "$Label expected $Expected matches, got $actual."
    }
}

function Assert-Absent {
    param([string]$Label, [string]$Pattern)
    $script:assertions++
    if ($workflow -match $Pattern) {
        throw "$Label must not appear in the release workflow."
    }
}

Assert-Match "license inventory job" `
    '(?ms)^  rust-licenses:\r?\n    name:.*?\r?\n    needs: metadata\r?\n.*?retention-days: 30\s*$'
Assert-Count "platform jobs consume the license job" `
    '(?m)^    needs: \[metadata, rust-licenses(?:, linux-package)?\]\s*$' 3
Assert-Count "platform jobs gate on license generation" `
    "needs\.rust-licenses\.result == 'success'" 3
Assert-Count "one upload and three downloads share the run-scoped artifact" `
    'name: rust-license-\$\{\{ github\.run_id \}\}' 4
Assert-Count "all platform builds receive the generated inventory" `
    'YANAMI_RUST_LICENSE_INVENTORY' 3
Assert-Count "all platform audits require the packaged HTML report" `
    'licenses[/\\]rust[/\\]THIRD_PARTY_LICENSES\.html' 3
Assert-Absent "removed tracked dependency snapshot" `
    'licenses[/\\]RUST_DEPENDENCIES\.md'
Assert-Absent "stale checked-in inventory comparison" `
    'checked-in Rust license report is stale|Get-FileHash licenses[/\\]rust[/\\]THIRD_PARTY_LICENSES\.html'
Assert-Count "Linux loader smokes scope LD_DEBUG to the tested entrypoint" `
    '(?m)^\s*LD_DEBUG=libs timeout 12s "\$entrypoint"' 2
Assert-Absent "Linux loader debugging is not exported to unrelated commands" `
    '(?m)^\s*export LD_DEBUG='
Assert-Match "Linux first-screen auto-exit has an explicit trace" `
    '(?ms)LD_DEBUG=libs timeout 12s "\$entrypoint" \\\r?\n\s*--performance-trace "\$first_screen_trace" \\\r?\n\s*--performance-runtime-auto-exit \\$'
Assert-Match "Linux first-screen smoke verifies the settled milestone" `
    'grep -q ''"milestone":"startup_settled"'' "\$first_screen_trace"'
Assert-Match "macOS resolves PowerShell before isolating PATH" `
    '(?m)^\s*pwsh_path="\$\(command -v pwsh\)"\r?\n\s*test -x "\$pwsh_path"$'
Assert-Match "macOS bootstrap smoke uses the resolved PowerShell path" `
    '(?ms)PATH=/usr/bin:/bin:/usr/sbin:/sbin \\\r?\n\s*QT_QUICK_BACKEND=software \\\r?\n\s*"\$pwsh_path" -NoLogo -NoProfile -File \\'
Assert-Absent "macOS isolated bootstrap smoke does not search for bare PowerShell" `
    '(?ms)PATH=/usr/bin:/bin:/usr/sbin:/sbin \\\r?\n\s*QT_QUICK_BACKEND=software \\\r?\n\s*pwsh -NoLogo -NoProfile -File \\'

Assert-Match "Velopack libc archive is version and hash pinned" `
    '(?s)velopack_libc_1\.2\.0\.zip.*?547262ed7a1ab1ff62f580aa53851ede2f1a451ac61b8974eb7bc01117488835'
Assert-Match "Velopack libc DLL is independently hash pinned" `
    'c36d8b984639a8af9d3397088d3ffb8213fe1bd0917f555cf0c6e33f014403ec'
Assert-Match "release CMake receives the verified Velopack runtime" `
    '-DYANAMI_VELOPACK_RUNTIME=\$env:YANAMI_VELOPACK_RUNTIME'
Assert-Match "Windows package invokes the pinned Velopack packager" `
    'scripts/package-windows-velopack\.ps1'
Assert-Match "Windows packager receives the branded installer shell" `
    'InstallerStubPath\s*=\s*\$installerStubPath'
Assert-Match "published Setup verifies its embedded backend" `
    'ArgumentList "--verify-payload"'
Assert-Match "published Setup asset has the stable versioned name" `
    'Yanami-\$\{\{ needs\.metadata\.outputs\.version \}\}-Windows-x86_64-Setup\.exe'
Assert-Match "preview full package has an exact application id name" `
    'io\.github\.TwooSix\.Yanami-\$YANAMI_RELEASE_VERSION-preview-full\.nupkg'
Assert-Match "preview feed is a public release asset" `
    'build/release-windows/releases\.preview\.json'
Assert-Match "published full baseline is verified before delta generation" `
    'Downloaded Velopack baseline failed its published size/SHA-256 check'
Assert-Match "Setup smoke checks the per-user uninstall key" `
    'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\io\.github\.TwooSix\.Yanami'
Assert-Match "Setup smoke uses the public wizard automation contract" `
    '"--install-dir", \$installRoot,\s*\r?\n\s*"--start-menu", "yes", "--desktop", "no", "--no-launch"'
Assert-Match "Setup smoke verifies an unselected desktop shortcut stays absent" `
    'Setup created a desktop shortcut even though it was disabled'
Assert-Count "selected shortcut smokes verify their exact installed targets" `
    'does not target the installed candidate' 2
Assert-Match "Windows packaging runs the Unicode shortcut reader regression" `
    '(?ms)^  windows-package:.*?run: \.\\\.github\\scripts\\Test-WindowsShortcut\.ps1'
Assert-Count "both shortcut smokes load the Unicode-safe reader" `
    '(?m)^\s*\. \./\.github/scripts/WindowsShortcut\.ps1$' 2
Assert-Count "both shortcut smokes read through the Unicode-safe helper" `
    'Get-WindowsShortcut -LiteralPath \$(?:shortcutPath|desktopShortcutPath)' 2
Assert-Count "both shortcut targets retain exact case-insensitive comparisons" `
    '\$actualTarget -ine \$expectedTarget' 2
Assert-Count "both shortcut working directories retain exact case-insensitive comparisons" `
    '\$actualWorkingDirectory -ine \$expectedWorkingDirectory' 2
Assert-Count "shortcut mismatch diagnostics include expected and actual targets" `
    'Expected TargetPath=''\$expectedTarget''; actual TargetPath=''\$actualTarget''' 2
Assert-Count "shortcut mismatch diagnostics include expected and actual working directories" `
    'Expected WorkingDirectory=''\$expectedWorkingDirectory''; actual WorkingDirectory=''\$actualWorkingDirectory''' 2
Assert-Absent "shortcut smokes do not use the ANSI WScript shortcut reader" `
    'WScript\.Shell|\.CreateShortcut\('
Assert-Match "Setup smoke exercises the alternate shortcut selection" `
    '"--start-menu", "no", "--desktop", "yes", "--no-launch"'
Assert-Match "alternate shortcut smoke retains its Unicode installation directory" `
    '"Yanami 安装 desktop smoke \$env:GITHUB_RUN_ID-\$env:GITHUB_RUN_ATTEMPT"'
Assert-Match "alternate shortcut uninstall checks for orphaned desktop links" `
    'Alternate shortcut uninstall left state behind'
Assert-Match "aggregate manifest names the optional delta exactly" `
    'delta_file="io\.github\.TwooSix\.Yanami-\$YANAMI_RELEASE_VERSION-preview-delta\.nupkg"'
Assert-Absent "aggregate does not classify arbitrary zip files as portable packages" `
    "-name '\*\.zip'"

Write-Host "Release workflow contract tests passed ($assertions assertions)."
