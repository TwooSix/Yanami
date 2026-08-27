$ErrorActionPreference = "Stop"
$configPath = Join-Path $PSScriptRoot "..\dependabot.yml"
$config = Get-Content -LiteralPath $configPath -Raw
$assertions = 0

function Assert-Count {
    param([string]$Label, [string]$Pattern, [int]$Expected)
    $script:assertions++
    $actual = [regex]::Matches($config, $Pattern).Count
    if ($actual -ne $Expected) {
        throw "$Label expected $Expected matches, got $actual."
    }
}

function Assert-Absent {
    param([string]$Label, [string]$Pattern)
    $script:assertions++
    if ($config -match $Pattern) {
        throw "$Label should be absent from the Dependabot policy."
    }
}

Assert-Count "configured ecosystems" '(?m)^\s*- package-ecosystem:' 2
Assert-Count "monthly schedules" '(?m)^\s*interval:\s*monthly\s*$' 2
Assert-Count "single-open-PR limits" '(?m)^\s*open-pull-requests-limit:\s*1\s*$' 2
Assert-Count "minor grouping" '(?m)^\s*- minor\s*$' 2
Assert-Count "patch grouping" '(?m)^\s*- patch\s*$' 2
Assert-Count "semver minor allow rules" 'version-update:semver-minor' 2
Assert-Count "semver patch allow rules" 'version-update:semver-patch' 2
Assert-Count "routine update groups" 'applies-to:\s*version-updates' 2
Assert-Count "security update groups" 'applies-to:\s*security-updates' 2
Assert-Count "Cargo ecosystem" 'package-ecosystem:\s*cargo' 1
Assert-Count "Actions ecosystem" 'package-ecosystem:\s*github-actions' 1
Assert-Absent "daily or weekly schedule" '(?m)^\s*interval:\s*(daily|weekly)\s*$'
Assert-Absent "routine major updates" 'semver-major|(?m)^\s*- major\s*$'

Write-Host "Dependabot maintenance policy tests passed ($assertions assertions)."
