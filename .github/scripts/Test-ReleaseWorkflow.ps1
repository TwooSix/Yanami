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

Write-Host "Release workflow contract tests passed ($assertions assertions)."
