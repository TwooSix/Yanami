[CmdletBinding()]
param(
    [string]$BaseSha,
    [string]$HeadSha,
    [switch]$UseMergeBase,
    [string[]]$ChangedPath,
    [string]$OutputPath = $env:GITHUB_OUTPUT,
    [string]$SummaryPath = $env:GITHUB_STEP_SUMMARY,
    [string]$ResultPath
)

$ErrorActionPreference = "Stop"

if ($PSBoundParameters.ContainsKey('ChangedPath')) {
    $changed = @($ChangedPath)
} else {
    if ([string]::IsNullOrWhiteSpace($HeadSha) -or
        $HeadSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "-HeadSha must be a full 40-character Git commit SHA."
    }
    if (-not [string]::IsNullOrWhiteSpace($BaseSha) -and
        $BaseSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "-BaseSha must be a full 40-character Git commit SHA."
    }
    $zeroSha = "0000000000000000000000000000000000000000"
    $baseExists = $BaseSha -and $BaseSha -ne $zeroSha
    if ($baseExists) {
        git cat-file -e "$BaseSha^{commit}" 2>$null
        $baseExists = $LASTEXITCODE -eq 0
    }
    if ($baseExists) {
        $diffBase = $BaseSha
        if ($UseMergeBase) {
            $diffBase = (git merge-base $BaseSha $HeadSha).Trim()
            if ($LASTEXITCODE -ne 0 -or $diffBase -notmatch '^[0-9a-fA-F]{40}$') {
                throw "Unable to resolve the pull request merge base."
            }
        }
        # Disable rename folding so both the removed and added paths are
        # classified. A product file moved under docs must still run its old
        # path's quality gates.
        $changed = @(git diff --no-renames --name-only `
            --diff-filter=ACDMRTUXB $diffBase $HeadSha)
        if ($LASTEXITCODE -ne 0) { throw "Unable to diff the CI classification range." }
    } else {
        # New branches have no usable before SHA. Inspect every tracked file
        # instead of risking a false skip.
        $changed = @(git ls-files)
        if ($LASTEXITCODE -ne 0) { throw "Unable to enumerate tracked files for CI classification." }
    }
}

$changed = @($changed | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
} | ForEach-Object { $_.Replace('\', '/') } | Select-Object -Unique)

$workflowFiles = @($changed | Where-Object { $_ -match '^\.github/' })
$rustFiles = @($changed | Where-Object {
    $_ -match '^(crates/|Cargo\.(toml|lock)$|rust-toolchain(?:\.toml)?$|VERSION$|about\.toml$|licenses/rust/|scripts/generate-rust-license-inventory\.sh$)'
})
$desktopFiles = @($changed | Where-Object {
    $_ -match '^(apps/desktop/|crates/(?!yanami-performance-probe/)|Cargo\.(toml|lock)$|rust-toolchain(?:\.toml)?$|VERSION$)'
})

# Paths explicitly owned by documentation, repository metadata, release-only
# packaging, or the performance gate are not unknown Core product paths.
$knownNonCorePattern = '^(docs/|README(?:\.|$)|LICENSE(?:$|\.)|THIRD_PARTY_NOTICES\.md$|.*\.md$|\.git(?:ignore|attributes)$|\.github/|perf/|scripts/performance/|licenses/)'
$knownCorePattern = '^(apps/desktop/|crates/|Cargo\.(toml|lock)$|rust-toolchain(?:\.toml)?$|VERSION$|about\.toml$|scripts/)'
$unknownProductFiles = @($changed | Where-Object {
    $_ -notmatch $knownNonCorePattern -and $_ -notmatch $knownCorePattern
})

$runRust = $rustFiles.Count -gt 0 -or $unknownProductFiles.Count -gt 0
$runDesktop = $desktopFiles.Count -gt 0 -or $unknownProductFiles.Count -gt 0
$toolingFiles = @($changed | Where-Object {
    $_ -match '^scripts/' -or $_ -eq '.gitattributes'
})
$runWorkflows = $workflowFiles.Count -gt 0 -or $toolingFiles.Count -gt 0
$nativeFiles = @($changed | Where-Object {
    $_ -match '(^|/)CMakeLists\.txt$' -or
    $_ -match '^apps/desktop/cmake/' -or
    $_ -match '^apps/desktop/(native|tests)/.*\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx)$'
})
$runNativeAnalysis = $nativeFiles.Count -gt 0 -or $unknownProductFiles.Count -gt 0
$packageInputPattern = '^(apps/desktop/|crates/(?!yanami-performance-probe/)|Cargo\.(toml|lock)$|rust-toolchain(?:\.toml)?$|VERSION$|about\.toml$|licenses/|README\.md$|README\.zh-CN\.md$|LICENSE$|THIRD_PARTY_NOTICES\.md$|\.gitattributes$|\.github/workflows/release\.yml$|scripts/performance/(?:PerfGate\.psm1|run-gate\.ps1|Test-BootstrapPackage\.ps1)$|scripts/(?:collect-linux-runtime-licenses|generate-rust-license-inventory|package-linux|package-macos|verify-macos-bundle)\.sh$)'
$packageFiles = @($changed | Where-Object { $_ -match $packageInputPattern })
$runPackage = $packageFiles.Count -gt 0 -or $unknownProductFiles.Count -gt 0

# Exercise every Core suite and the complete package wiring when its router or
# classifier changes. Otherwise those changes could pass without generating the
# license artifact or running the install-tree smoke test they control.
if ($changed -contains '.github/workflows/core.yml' -or
    $changed -contains '.github/scripts/Classify-CI.ps1') {
    $runRust = $true
    $runDesktop = $true
    $runNativeAnalysis = $true
    $runPackage = $true
}

$performanceClassifier = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot `
    "..\..\scripts\performance\Select-Suites.ps1"))
if (-not (Test-Path -LiteralPath $performanceClassifier -PathType Leaf)) {
    throw "Performance suite classifier is missing: $performanceClassifier"
}
$performanceResult = & $performanceClassifier `
    -ChangedPath $changed `
    -GitHubOutputPath "" `
    -StepSummaryPath ""

$result = [pscustomobject][ordered]@{
    rust = $runRust
    desktop = $runDesktop
    workflows = $runWorkflows
    nativeAnalysis = $runNativeAnalysis
    performance = [bool]$performanceResult.relevant
    performanceSuites = [string]$performanceResult.suites
    package = $runPackage
    changedCount = $changed.Count
    unknownProductCount = $unknownProductFiles.Count
}

if ($OutputPath) {
    @(
        "rust=$($result.rust.ToString().ToLowerInvariant())"
        "desktop=$($result.desktop.ToString().ToLowerInvariant())"
        "workflows=$($result.workflows.ToString().ToLowerInvariant())"
        "native_analysis=$($result.nativeAnalysis.ToString().ToLowerInvariant())"
        "performance=$($result.performance.ToString().ToLowerInvariant())"
        "performance_suites=$($result.performanceSuites)"
        "package=$($result.package.ToString().ToLowerInvariant())"
    ) | Add-Content -LiteralPath $OutputPath -Encoding utf8
}

if ($SummaryPath) {
    @(
        "## CI path classification"
        ""
        "- Changed files: $($result.changedCount)"
        "- Rust quality: $($result.rust)"
        "- Desktop matrix: $($result.desktop)"
        "- CI/tooling static analysis: $($result.workflows)"
        "- Native CodeQL analysis: $($result.nativeAnalysis)"
        "- Hosted performance suites: $(if ($result.performanceSuites) { $result.performanceSuites.Replace(',', ', ') } else { 'none' })"
        "- Desktop packaging relevant: $($result.package)"
        "- Unknown product files: $($result.unknownProductCount)"
    ) | Add-Content -LiteralPath $SummaryPath -Encoding utf8
}

if ($ResultPath) {
    $resolvedResultPath = [IO.Path]::GetFullPath($ResultPath)
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $resolvedResultPath))
    [pscustomobject][ordered]@{
        schemaVersion = 1
        packageRelevant = $result.package
        changedCount = $result.changedCount
        headSha = $HeadSha
    } | ConvertTo-Json | Set-Content -LiteralPath $resolvedResultPath -Encoding utf8
}

$result
