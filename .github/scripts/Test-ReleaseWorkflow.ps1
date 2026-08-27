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

Write-Host "Release workflow contract tests passed ($assertions assertions)."
