[CmdletBinding()]
param(
    [string]$BaseSha,
    [string]$HeadSha,
    [string[]]$ChangedPath,
    [string]$GitHubOutputPath = $env:GITHUB_OUTPUT,
    [string]$StepSummaryPath = $env:GITHUB_STEP_SUMMARY
)

$ErrorActionPreference = "Stop"
$changedPathSupplied = $PSBoundParameters.ContainsKey("ChangedPath")

function Get-ChangedPath {
    if ($changedPathSupplied) {
        return @($ChangedPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    }

    if ([string]::IsNullOrWhiteSpace($HeadSha)) {
        throw "-HeadSha is required when -ChangedPath is not supplied."
    }
    if ($HeadSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "-HeadSha must be a full 40-character Git commit SHA."
    }
    if (-not [string]::IsNullOrWhiteSpace($BaseSha) -and
        $BaseSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "-BaseSha must be a full 40-character Git commit SHA."
    }

    $zeroSha = "0000000000000000000000000000000000000000"
    $baseExists = -not [string]::IsNullOrWhiteSpace($BaseSha) -and $BaseSha -ne $zeroSha
    if ($baseExists) {
        & git cat-file -e "$BaseSha`^{commit}" 2>$null
        $baseExists = $LASTEXITCODE -eq 0
    }
    if ($baseExists) {
        $paths = @(& git diff --name-only --diff-filter=ACDMRTUXB $BaseSha $HeadSha)
        if ($LASTEXITCODE -ne 0) { throw "Unable to diff performance classification range." }
        return @($paths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    }

    # A new branch has no usable before SHA. Treat every tracked file as
    # changed rather than silently skipping an unfamiliar product change.
    $paths = @(& git ls-files)
    if ($LASTEXITCODE -ne 0) { throw "Unable to enumerate tracked files for performance classification." }
    return @($paths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

$changed = @(Get-ChangedPath)
$nonProductPattern = '^(docs/|licenses/|LICENSE(?:$|\.)|README(?:\.|$)|THIRD_PARTY_NOTICES\.md$|\.git(?:ignore|attributes)$|\.github/dependabot\.yml$|\.github/workflows/release\.yml$|.*\.md$)'
$relevantFiles = @($changed | Where-Object { $_.Replace('\', '/') -notmatch $nonProductPattern })
$suiteOrder = @("Search", "Backend", "Interaction", "Playback", "Danmaku", "Upscaling", "Startup")
$selected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

foreach ($path in $relevantFiles) {
    $normalized = $path.Replace('\', '/')

    if ($normalized -match '^(perf/|scripts/performance/|\.github/workflows/)' -or
        $normalized -match '^(Cargo\.toml|Cargo\.lock|VERSION)$' -or
        $normalized -match '(^|/)CMakeLists\.txt$' -or
        $normalized -match '^crates/(yanami-core|yanami-application|yanami-desktop-bridge)/' -or
        $normalized -match '^apps/desktop/native/(ApplicationViewModel|DesktopBackendServices|RustBridgeRuntime)') {
        foreach ($suite in $suiteOrder) { [void]$selected.Add($suite) }
        continue
    }

    if ($normalized -match '(?i)(search|mediaquery|mediastore|catalog|storage|index)') {
        [void]$selected.Add("Search")
    }
    if ($normalized -match '^crates/' -or
        $normalized -match '^apps/desktop/native/.*(Backend|Bridge|Coordinator|Port|Worker)') {
        [void]$selected.Add("Backend")
    }
    if ($normalized -match '^apps/desktop/(qml|native)/') {
        [void]$selected.Add("Interaction")
    }
    if ($normalized -match '(?i)(playback|player|mpv|video|subtitle)') {
        [void]$selected.Add("Playback")
    }
    if ($normalized -match '(?i)(upscal|super.?resolution|anime4k|model.?pack|SettingsPage\.qml)') {
        [void]$selected.Add("Upscaling")
        [void]$selected.Add("Playback")
    }
    if ($normalized -match '(?i)(danmaku|PlayerPage\.qml|MpvVideoItem|scene.?graph|renderer)') {
        [void]$selected.Add("Danmaku")
        [void]$selected.Add("Playback")
        [void]$selected.Add("Upscaling")
    }
    if ($normalized -match '(?i)(^apps/desktop/native/(main|WindowController)\.cpp$|^apps/desktop/qml/(Main\.qml|pages/(HomePage|LoginPage)\.qml|components/)|startup|bootstrap|session|preference|cache|storage)') {
        [void]$selected.Add("Startup")
    }
}

if ($relevantFiles.Count -gt 0 -and $selected.Count -eq 0) {
    # Unknown product paths default to broad coverage. The classifier must not
    # turn a new runtime path into a false skip.
    foreach ($suite in $suiteOrder) { [void]$selected.Add($suite) }
}

$orderedSuites = @($suiteOrder | Where-Object { $selected.Contains($_) })
$isRelevant = $relevantFiles.Count -gt 0
$result = [pscustomobject][ordered]@{
    relevant = $isRelevant
    suites = $orderedSuites -join ','
    changedCount = $changed.Count
    relevantCount = $relevantFiles.Count
}

if (-not [string]::IsNullOrWhiteSpace($GitHubOutputPath)) {
    @(
        "relevant=$($isRelevant.ToString().ToLowerInvariant())"
        "suites=$($result.suites)"
    ) | Add-Content -LiteralPath $GitHubOutputPath -Encoding UTF8
}
if (-not [string]::IsNullOrWhiteSpace($StepSummaryPath)) {
    @(
        "## Performance path classification"
        ""
        "- Changed files: $($changed.Count)"
        "- Product-relevant files: $($relevantFiles.Count)"
        "- Suites: $(if ($orderedSuites.Count) { $orderedSuites -join ', ' } else { 'none (docs/licenses only)' })"
    ) | Add-Content -LiteralPath $StepSummaryPath -Encoding UTF8
}

Write-Output $result
