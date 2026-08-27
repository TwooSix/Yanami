[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("PullRequest", "Lab", "Nightly", "Weekly", "Release")]
    [string]$Profile,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$Suites,
    [string]$BaseSha,
    [string]$CandidateSha,
    [string[]]$InputPath,
    [string]$BaseResultPath,
    [string[]]$ComparisonResultPath,
    [string[]]$RetryInputPath,
    [string]$ProbePath,
    [string]$DanmakuProbePath,
    [string]$FixtureDirectory,
    [string]$DanmakuFixtureDirectory,
    [string]$DesktopExecutable,
    [string[]]$DesktopArguments = @(),
    [ValidateRange(1, 120)][int]$StartupTimeoutSeconds = 20,
    [ValidateSet("Auto", "collect", "debt", "enforce")][string]$Mode = "Auto",
    [string]$SloPath,
    [string]$PolicyPath,
    [switch]$ValidateOnly,
    [switch]$SkipProbeDiscovery,
    [switch]$RequireBootstrapSidecar,

    [switch]$UseNativeWindowForRuntimeTrace,
    [switch]$NoExit
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$scriptRoot = $PSScriptRoot
$workspace = Split-Path -Parent (Split-Path -Parent $scriptRoot)
if (-not $SloPath) { $SloPath = Join-Path $workspace "perf\slo\slo-v1.json" }
if (-not $PolicyPath) { $PolicyPath = Join-Path $workspace "perf\policy\calibration-v1.json" }
$modulePath = Join-Path $scriptRoot "PerfGate.psm1"
$runManifestSchemaPath = Join-Path $workspace "perf\contracts\run-manifest.schema.json"
$perfEventSchemaPath = Join-Path $workspace "perf\contracts\perf-event.schema.json"
$perfResultSchemaPath = Join-Path $workspace "perf\contracts\perf-result.schema.json"
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $resolvedOutput)) { [void](New-Item -ItemType Directory -Path $resolvedOutput) }
$jsonOutput = Join-Path $resolvedOutput "performance-result.json"
$junitOutput = Join-Path $resolvedOutput "perf-results.xml"
$summaryOutput = Join-Path $resolvedOutput "perf-summary.md"
$result = $null
$policy = $null
$exitCode = 2
if (-not $BaseSha -and $env:YANAMI_PERF_BASE_SHA) { $BaseSha = $env:YANAMI_PERF_BASE_SHA }
if (-not $CandidateSha -and $env:YANAMI_PERF_HEAD_SHA) { $CandidateSha = $env:YANAMI_PERF_HEAD_SHA }
if (-not $BaseResultPath -and $env:YANAMI_PERF_BASE_RESULT_PATH) { $BaseResultPath = $env:YANAMI_PERF_BASE_RESULT_PATH }
if (@($ComparisonResultPath).Count -eq 0 -and $env:YANAMI_PERF_ABAB_RESULT_PATHS) {
    $ComparisonResultPath = @($env:YANAMI_PERF_ABAB_RESULT_PATHS.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { $_.Trim() })
}

function Get-RequestedSuites {
    param([string]$Csv)
    if ([string]::IsNullOrWhiteSpace($Csv)) { return @() }
    return @($Csv.Split(",") | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ } | Select-Object -Unique)
}

function Assert-JsonSchemaText {
    param(
        [Parameter(Mandatory = $true)][string]$Json,
        [Parameter(Mandatory = $true)][string]$SchemaPath,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $testJson = Get-Command Test-Json -ErrorAction SilentlyContinue
    if (-not $testJson -or -not $testJson.Parameters.ContainsKey("SchemaFile")) {
        throw "PowerShell Test-Json -SchemaFile is required for hard contract validation ($Label)."
    }
    try { $valid = $Json | Test-Json -SchemaFile $SchemaPath -ErrorAction Stop }
    catch { throw "$Label failed JSON Schema validation: $($_.Exception.Message)" }
    if (-not $valid) { throw "$Label failed JSON Schema validation." }
}

function Assert-JsonFileSchema {
    param([string]$Path, [string]$SchemaPath, [string]$Label)
    Assert-JsonSchemaText -Json (Get-Content -LiteralPath $Path -Raw -Encoding UTF8) -SchemaPath $SchemaPath -Label $Label
}

function Assert-JsonLinesSchema {
    param([string]$Path, [string]$SchemaPath, [string]$Label)
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        Assert-JsonSchemaText -Json $line -SchemaPath $SchemaPath -Label "$Label line $lineNumber"
    }
}

function Get-ProbeProfileName {
    param([string]$GateProfile)
    switch ($GateProfile) {
        "PullRequest" { return "pr" }
        "Lab" { return "lab" }
        "Nightly" { return "nightly" }
        "Weekly" { return "weekly" }
        "Release" { return "release" }
    }
}

function Find-DesktopProbe {
    param([string]$ExplicitPath)
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "Explicit desktop performance probe does not exist: $ExplicitPath"
        }
        return [pscustomobject][ordered]@{
            path = [System.IO.Path]::GetFullPath($ExplicitPath)
            source = "explicit-parameter"
            explicit = $true
        }
    }

    $candidates = New-Object System.Collections.Generic.List[object]
    if ($env:YANAMI_PERF_PROBE) {
        $candidates.Add([pscustomobject]@{ path = $env:YANAMI_PERF_PROBE; source = "environment" })
    }
    foreach ($candidate in @(
        (Join-Path $workspace "build\desktop-windows\yanami-desktop-perf-probe.exe"),
        (Join-Path $workspace "build\ci-windows\yanami-desktop-perf-probe.exe"),
        (Join-Path $workspace "build\performance-windows\yanami-desktop-perf-probe.exe"),
        (Join-Path $workspace "build\desktop-windows\apps\desktop\yanami-desktop-perf-probe.exe")
    )) { $candidates.Add([pscustomobject]@{ path = $candidate; source = "known-build-tree" }) }
    $command = Get-Command "yanami-desktop-perf-probe" -ErrorAction SilentlyContinue
    if ($command) {
        $candidates.Add([pscustomobject]@{ path = $command.Source; source = "path-command" })
    }
    foreach ($candidate in $candidates) {
        if ($candidate.path -and (Test-Path -LiteralPath $candidate.path -PathType Leaf)) {
            return [pscustomobject][ordered]@{
                path = [System.IO.Path]::GetFullPath([string]$candidate.path)
                source = [string]$candidate.source
                explicit = $false
            }
        }
    }
    return $null
}

function Find-DanmakuProbe {
    param([string]$ExplicitPath)
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "Explicit danmaku performance probe does not exist: $ExplicitPath"
        }
        return [pscustomobject][ordered]@{
            path = [System.IO.Path]::GetFullPath($ExplicitPath)
            source = "explicit-parameter"
            explicit = $true
        }
    }

    foreach ($candidate in @(
        (Join-Path $workspace "build\desktop-windows\yanami-danmaku-perf-probe.exe"),
        (Join-Path $workspace "build\ci-windows\yanami-danmaku-perf-probe.exe"),
        (Join-Path $workspace "build\performance-windows\yanami-danmaku-perf-probe.exe"),
        (Join-Path $workspace "build\desktop-windows\apps\desktop\yanami-danmaku-perf-probe.exe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [pscustomobject][ordered]@{
                path = [System.IO.Path]::GetFullPath($candidate)
                source = "known-build-tree"
                explicit = $false
            }
        }
    }
    return $null
}

function Find-CMakeBuildDirectoryForArtifact {
    param([string]$ArtifactPath)
    if (-not $ArtifactPath) { return $null }
    $directory = Split-Path -Parent ([System.IO.Path]::GetFullPath($ArtifactPath))
    for ($depth = 0; $depth -lt 5 -and $directory; $depth++) {
        if (Test-Path -LiteralPath (Join-Path $directory "CMakeCache.txt") -PathType Leaf) {
            return $directory
        }
        $parent = Split-Path -Parent $directory
        if (-not $parent -or $parent -eq $directory) { break }
        $directory = $parent
    }
    return $null
}

function Find-DesktopExecutableInBuildDirectory {
    param([string]$BuildDirectory)
    if (-not $BuildDirectory) { return $null }
    foreach ($candidate in @(
        (Join-Path $BuildDirectory "yanami-bootstrap.exe"),
        (Join-Path $BuildDirectory "Yanami.exe"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-bootstrap.exe"),
        (Join-Path $BuildDirectory "apps\desktop\Yanami.exe"),
        (Join-Path $BuildDirectory "yanami-bootstrap"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-bootstrap"),
        (Join-Path $BuildDirectory "yanami-desktop.exe"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-desktop.exe"),
        (Join-Path $BuildDirectory "yanami-desktop"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-desktop")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Find-DesktopProbeInBuildDirectory {
    param([string]$BuildDirectory)
    if (-not $BuildDirectory) { return $null }
    foreach ($candidate in @(
        (Join-Path $BuildDirectory "yanami-desktop-perf-probe.exe"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-desktop-perf-probe.exe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Find-DanmakuProbeInBuildDirectory {
    param([string]$BuildDirectory)
    if (-not $BuildDirectory) { return $null }
    foreach ($candidate in @(
        (Join-Path $BuildDirectory "yanami-danmaku-perf-probe.exe"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-danmaku-perf-probe.exe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Find-UpscalingProbeInBuildDirectory {
    param([string]$BuildDirectory)
    if (-not $BuildDirectory) { return $null }
    foreach ($candidate in @(
        (Join-Path $BuildDirectory "yanami-upscaling-perf-probe.exe"),
        (Join-Path $BuildDirectory "apps\desktop\yanami-upscaling-perf-probe.exe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Build-DesktopProbeBesideArtifact {
    param([string]$DesktopProbePath)
    $buildDirectory = Find-CMakeBuildDirectoryForArtifact -ArtifactPath $DesktopProbePath
    if (-not $buildDirectory) { return $null }
    $cachePath = Join-Path $buildDirectory "CMakeCache.txt"
    $cmakeEntry = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
    if (-not $cmakeEntry) { return $null }
    $cmake = $cmakeEntry.Matches[0].Groups[1].Value.Trim()
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) { return $null }

    $oldPath = $env:PATH
    $oldToolchain = $env:RUSTUP_TOOLCHAIN
    try {
        $toolBin = Split-Path -Parent $cmake
        $env:PATH = "$toolBin;$env:USERPROFILE\.cargo\bin;$env:PATH"
        $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
        & $cmake --build $buildDirectory --target yanami-desktop-perf-probe --parallel 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Building yanami-desktop-perf-probe for the current run failed with code $LASTEXITCODE."
        }
    }
    finally {
        $env:PATH = $oldPath
        $env:RUSTUP_TOOLCHAIN = $oldToolchain
    }
    return Find-DesktopProbeInBuildDirectory -BuildDirectory $buildDirectory
}

function Build-DanmakuProbeBesideArtifact {
    param([string]$ArtifactPath)
    if (-not $ArtifactPath) { return $null }
    $buildDirectory = Find-CMakeBuildDirectoryForArtifact -ArtifactPath $ArtifactPath
    if (-not $buildDirectory) { return $null }
    $cachePath = Join-Path $buildDirectory "CMakeCache.txt"
    $cmakeEntry = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
    if (-not $cmakeEntry) { return $null }
    $cmake = $cmakeEntry.Matches[0].Groups[1].Value.Trim()
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) { return $null }

    $oldPath = $env:PATH
    $oldToolchain = $env:RUSTUP_TOOLCHAIN
    try {
        $toolBin = Split-Path -Parent $cmake
        $env:PATH = "$toolBin;$env:USERPROFILE\.cargo\bin;$env:PATH"
        $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
        & $cmake --build $buildDirectory --target yanami-danmaku-perf-probe --parallel 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Building yanami-danmaku-perf-probe for the current run failed with code $LASTEXITCODE."
        }
    }
    finally {
        $env:PATH = $oldPath
        $env:RUSTUP_TOOLCHAIN = $oldToolchain
    }
    return Find-DanmakuProbeInBuildDirectory -BuildDirectory $buildDirectory
}

function Build-UpscalingProbeBesideArtifact {
    param([string]$ArtifactPath)
    if (-not $ArtifactPath) { return $null }
    $buildDirectory = Find-CMakeBuildDirectoryForArtifact -ArtifactPath $ArtifactPath
    if (-not $buildDirectory) { return $null }
    $cachePath = Join-Path $buildDirectory "CMakeCache.txt"
    $cmakeEntry = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
    if (-not $cmakeEntry) { return $null }
    $cmake = $cmakeEntry.Matches[0].Groups[1].Value.Trim()
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) { return $null }

    $oldPath = $env:PATH
    $oldToolchain = $env:RUSTUP_TOOLCHAIN
    try {
        $toolBin = Split-Path -Parent $cmake
        $env:PATH = "$toolBin;$env:USERPROFILE\.cargo\bin;$env:PATH"
        $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
        & $cmake --build $buildDirectory --target yanami-upscaling-perf-probe --parallel 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Building yanami-upscaling-perf-probe for the current run failed with code $LASTEXITCODE."
        }
    }
    finally {
        $env:PATH = $oldPath
        $env:RUSTUP_TOOLCHAIN = $oldToolchain
    }
    return Find-UpscalingProbeInBuildDirectory -BuildDirectory $buildDirectory
}

function Build-DesktopExecutableBesideProbe {
    param([string]$DesktopProbePath)
    if (-not $DesktopProbePath) { return $null }
    $buildDirectory = Find-CMakeBuildDirectoryForArtifact -ArtifactPath $DesktopProbePath
    if (-not $buildDirectory) { return $null }
    $cachePath = Join-Path $buildDirectory "CMakeCache.txt"
    $cmakeEntry = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
    if (-not $cmakeEntry) { return $null }
    $cmake = $cmakeEntry.Matches[0].Groups[1].Value.Trim()
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) { return $null }

    $oldPath = $env:PATH
    $oldToolchain = $env:RUSTUP_TOOLCHAIN
    try {
        $toolBin = Split-Path -Parent $cmake
        $env:PATH = "$toolBin;$env:USERPROFILE\.cargo\bin;$env:PATH"
        $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
        & $cmake --build $buildDirectory --target yanami-desktop yanami-bootstrap --parallel 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Building yanami-desktop and yanami-bootstrap for startup tracing failed with code $LASTEXITCODE."
        }
    }
    finally {
        $env:PATH = $oldPath
        $env:RUSTUP_TOOLCHAIN = $oldToolchain
    }
    return Find-DesktopExecutableInBuildDirectory -BuildDirectory $buildDirectory
}

function Build-DesktopProbeIfAvailable {
    $isWindowsHost = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)
    if (-not $isWindowsHost) { return $null }
    $msysCandidates = New-Object System.Collections.Generic.List[string]
    if ($env:MSYS2_ROOT) { $msysCandidates.Add($env:MSYS2_ROOT) }
    $msysCandidates.Add("C:\msys64")
    if ($env:USERPROFILE) { $msysCandidates.Add((Join-Path $env:USERPROFILE "scoop\apps\msys2\current")) }
    if ($env:LOCALAPPDATA) { $msysCandidates.Add((Join-Path $env:LOCALAPPDATA "Programs\MSYS2")) }
    $msysRoot = @($msysCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ "ucrt64\bin\qmake6.exe") } | Select-Object -First 1)
    if ($msysRoot.Count -eq 0) { return $null }
    $ucrtBin = Join-Path $msysRoot[0] "ucrt64\bin"
    $cmake = Join-Path $ucrtBin "cmake.exe"
    $compiler = Join-Path $ucrtBin "g++.exe"
    if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath $compiler)) { return $null }
    $buildDirectory = Join-Path $workspace "build\performance-windows"
    $oldPath = $env:PATH
    $oldToolchain = $env:RUSTUP_TOOLCHAIN
    try {
        $env:PATH = "$ucrtBin;$env:USERPROFILE\.cargo\bin;$env:PATH"
        $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
        & $cmake -S (Join-Path $workspace "apps\desktop") -B $buildDirectory -G Ninja `
            -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_PREFIX_PATH=$($msysRoot[0])\ucrt64" `
            "-DCMAKE_CXX_COMPILER=$compiler" `
            -DBUILD_TESTING=ON `
            -DYANAMI_ENABLE_DEV_HOOKS=OFF 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration for yanami-desktop-perf-probe failed with code $LASTEXITCODE." }
        & $cmake --build $buildDirectory --target yanami-desktop-perf-probe --parallel 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "Building yanami-desktop-perf-probe failed with code $LASTEXITCODE." }
    }
    finally {
        $env:PATH = $oldPath
        $env:RUSTUP_TOOLCHAIN = $oldToolchain
    }
    $executable = Join-Path $buildDirectory "yanami-desktop-perf-probe.exe"
    if (Test-Path -LiteralPath $executable -PathType Leaf) { return $executable }
    return $null
}

function Test-RustComponentProbePackageExists {
    $crateRoot = Join-Path $workspace "crates"
    if (-not (Test-Path -LiteralPath $crateRoot -PathType Container)) { return $false }
    foreach ($manifest in Get-ChildItem -LiteralPath $crateRoot -Filter Cargo.toml -File -Recurse) {
        if (Select-String -LiteralPath $manifest.FullName -Pattern '^name\s*=\s*"yanami-performance-probe"\s*$' -Quiet) { return $true }
    }
    return $false
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string]$Value)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try { return (($algorithm.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Value)) | ForEach-Object { $_.ToString("x2") }) -join "") }
    finally { $algorithm.Dispose() }
}

function Get-WorkspaceHeadSha {
    try {
        $head = [string](& git -C $workspace rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and $head.Trim() -match '^[0-9a-fA-F]{40,64}$') {
            return $head.Trim().ToLowerInvariant()
        }
    } catch {}
    return "unavailable"
}

function Set-LocalManifestProvenance {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ProbeKind,
        [Parameter(Mandatory = $true)][string]$DiscoverySource,
        [string]$ArtifactPath,
        [AllowNull()][System.Collections.Generic.List[object]]$TrustedProducerLedger
    )
    $artifactSha256 = ""
    if ($ArtifactPath) {
        if (-not (Test-Path -LiteralPath $ArtifactPath -PathType Leaf)) {
            throw "Local performance artifact does not exist: $ArtifactPath"
        }
        $artifactSha256 = (Get-FileHash -LiteralPath $ArtifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $reportedCandidateSha = if ($null -ne $Manifest.PSObject.Properties["candidateSha"]) {
        [string]$Manifest.candidateSha
    } else { "" }
    if (-not [string]::IsNullOrWhiteSpace($reportedCandidateSha) -and
        -not [string]::IsNullOrWhiteSpace([string]$CandidateSha) -and
        $reportedCandidateSha -ne [string]$CandidateSha) {
        throw "Locally executed probe '$($Manifest.runId)' reported candidate SHA '$reportedCandidateSha', which does not match requested candidate SHA '$CandidateSha'."
    }
    $effectiveCandidateSha = if (-not [string]::IsNullOrWhiteSpace([string]$CandidateSha)) {
        [string]$CandidateSha
    } else { $reportedCandidateSha }
    $provenance = [pscustomobject][ordered]@{
        kind = "local-runner-generated"
        producer = "scripts/performance/run-gate.ps1"
        probeKind = $ProbeKind
        discoverySource = $DiscoverySource
        candidateSha = $effectiveCandidateSha
        baseSha = [string]$BaseSha
        workspaceHeadSha = Get-WorkspaceHeadSha
        runnerFingerprint = [string]$env:YANAMI_PERF_RUN_FINGERPRINT
        artifactSha256 = $artifactSha256
    }
    $Manifest | Add-Member -NotePropertyName candidateSha -NotePropertyValue $effectiveCandidateSha -Force
    $Manifest | Add-Member -NotePropertyName baseSha -NotePropertyValue ([string]$BaseSha) -Force
    $Manifest.environment | Add-Member -NotePropertyName runnerProvenance -NotePropertyValue $provenance -Force
    if ($null -ne $TrustedProducerLedger) {
        $TrustedProducerLedger.Add([pscustomobject][ordered]@{
            trustSource = "current-runner-process"
            runId = [string]$Manifest.runId
            probeKind = $ProbeKind
            runnerFingerprint = [string]$env:YANAMI_PERF_RUN_FINGERPRINT
            artifactSha256 = $artifactSha256
        })
    }
    return $Manifest
}

function Add-ManifestArtifactEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ArtifactPath,
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$DiscoverySource,
        [AllowNull()][System.Collections.Generic.List[object]]$TrustedProducerLedger
    )
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $manifest = Set-LocalManifestProvenance `
        -Manifest $manifest `
        -ProbeKind $Role `
        -DiscoverySource $DiscoverySource `
        -ArtifactPath $ArtifactPath `
        -TrustedProducerLedger $TrustedProducerLedger
    $evidence = [pscustomobject][ordered]@{
        role = $Role
        fileName = [System.IO.Path]::GetFileName($ArtifactPath)
        sha256 = (Get-FileHash -LiteralPath $ArtifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
        candidateSha = [string]$manifest.candidateSha
    }
    $existingArtifacts = if ($null -ne $manifest.PSObject.Properties["artifacts"]) {
        @($manifest.artifacts)
    } else { @() }
    $artifacts = @($existingArtifacts) + @($evidence)
    $manifest | Add-Member -NotePropertyName artifacts -NotePropertyValue $artifacts -Force
    $manifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
}

function Add-ManifestLocalProvenance {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ProbeKind,
        [Parameter(Mandatory = $true)][string]$DiscoverySource,
        [string]$ArtifactPath,
        [AllowNull()][System.Collections.Generic.List[object]]$TrustedProducerLedger
    )
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $manifest = Set-LocalManifestProvenance `
        -Manifest $manifest `
        -ProbeKind $ProbeKind `
        -DiscoverySource $DiscoverySource `
        -ArtifactPath $ArtifactPath `
        -TrustedProducerLedger $TrustedProducerLedger
    $manifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
}

function Assert-ImportedManifestCandidate {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [string]$ExpectedCandidateSha
    )
    $manifestCandidateSha = if ($null -ne $Manifest.PSObject.Properties["candidateSha"]) {
        [string]$Manifest.candidateSha
    } else { "" }
    if ([string]::IsNullOrWhiteSpace($manifestCandidateSha)) {
        throw "Imported run manifest '$ManifestPath' must declare a non-empty candidateSha; evaluator arguments cannot relabel imported evidence."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCandidateSha) -and
        $manifestCandidateSha -ne $ExpectedCandidateSha) {
        throw "Imported run manifest '$ManifestPath' candidate SHA '$manifestCandidateSha' does not match requested candidate SHA '$ExpectedCandidateSha'."
    }
}

function Set-RunnerObservationProducerRunIds {
    param([Parameter(Mandatory = $true)][object]$Manifest)

    $runId = [string]$Manifest.runId
    if ([string]::IsNullOrWhiteSpace($runId)) {
        throw "Cannot bind observations from a manifest without a runId."
    }
    foreach ($metric in @($Manifest.metrics)) {
        $metric | Add-Member -NotePropertyName producerRunIds -NotePropertyValue @($runId) -Force
    }
    foreach ($invariant in @($Manifest.invariants)) {
        $invariant | Add-Member -NotePropertyName producerRunIds -NotePropertyValue @($runId) -Force
    }
    return $Manifest
}

function Initialize-RunFingerprint {
    $details = [ordered]@{
        machineName = [Environment]::MachineName
        os = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
        architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        processorCount = [Environment]::ProcessorCount
        windowsBuild = "unavailable"
        cpuModel = "unavailable"
        gpuModel = "unavailable"
        gpuDriver = "unavailable"
        storageModel = "unavailable"
        powerPlan = "unavailable"
        qtVersion = "unavailable"
        rustVersion = "unavailable"
        renderWidthPixels = "unavailable"
        renderHeightPixels = "unavailable"
        displayRefreshHz = "unavailable"
        dpiScalePercent = "unavailable"
        hdrEnabled = "unavailable"
        vrrEnabled = "unavailable"
        windowMode = "unavailable"
        qtRhiRenderer = "unavailable"
        fontSetId = "unavailable"
        fontSetSha256 = "unavailable"
        fontCacheState = "unavailable"
        fontCachePreparationId = "unavailable"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
        try {
            $cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | Sort-Object DeviceID | Select-Object -First 1
            $gpu = Get-CimInstance Win32_VideoController -ErrorAction Stop | Sort-Object PNPDeviceID | Select-Object -First 1
            $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
            $disk = Get-CimInstance Win32_DiskDrive -ErrorAction Stop | Sort-Object DeviceID | Select-Object -First 1
            $details.cpuModel = [string]$cpu.Name
            $details.gpuModel = [string]$gpu.Name
            $details.gpuDriver = [string]$gpu.DriverVersion
            $details.windowsBuild = [string]$os.BuildNumber
            $details.storageModel = [string]$disk.Model
            if ($null -ne $gpu.CurrentHorizontalResolution -and
                $null -ne $gpu.CurrentVerticalResolution) {
                $details.renderWidthPixels = [string]$gpu.CurrentHorizontalResolution
                $details.renderHeightPixels = [string]$gpu.CurrentVerticalResolution
            }
            if ($null -ne $gpu.CurrentRefreshRate) {
                $details.displayRefreshHz = [string]$gpu.CurrentRefreshRate
            }
        } catch {
            $details.hardwareQuery = "unavailable"
        }
        try {
            $desktopMetrics = Get-ItemProperty `
                -LiteralPath "HKCU:\Control Panel\Desktop\WindowMetrics" `
                -Name AppliedDPI `
                -ErrorAction Stop
            if ($null -ne $desktopMetrics.AppliedDPI -and [int]$desktopMetrics.AppliedDPI -gt 0) {
                $details.dpiScalePercent = [string][math]::Round(
                    ([double]$desktopMetrics.AppliedDPI / 96.0) * 100.0,
                    2)
            }
        } catch {}
        try {
            $powerOutput = & powercfg.exe /GETACTIVESCHEME 2>$null
            if ($powerOutput) { $details.powerPlan = ([string]($powerOutput -join " ")).Trim() }
        } catch {}
    }
    $qmakeCandidates = @(
        (Get-Command qmake6 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
        "C:\msys64\ucrt64\bin\qmake6.exe",
        (Join-Path $env:USERPROFILE "scoop\apps\msys2\current\ucrt64\bin\qmake6.exe")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    if ($qmakeCandidates) {
        try { $details.qtVersion = ([string](& $qmakeCandidates -query QT_VERSION 2>$null)).Trim() } catch {}
    }
    $rustcCandidates = @(
        (Get-Command rustc -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
        (Join-Path $env:USERPROFILE ".cargo\bin\rustc.exe")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    if ($rustcCandidates) {
        try { $details.rustVersion = ([string](& $rustcCandidates --version 2>$null)).Trim() } catch {}
    }
    $environmentOverrides = [ordered]@{
        renderWidthPixels = "YANAMI_PERF_RENDER_WIDTH_PIXELS"
        renderHeightPixels = "YANAMI_PERF_RENDER_HEIGHT_PIXELS"
        displayRefreshHz = "YANAMI_PERF_DISPLAY_REFRESH_HZ"
        dpiScalePercent = "YANAMI_PERF_DPI_SCALE_PERCENT"
        hdrEnabled = "YANAMI_PERF_HDR_ENABLED"
        vrrEnabled = "YANAMI_PERF_VRR_ENABLED"
        windowMode = "YANAMI_PERF_WINDOW_MODE"
        qtRhiRenderer = "YANAMI_PERF_QT_RHI_RENDERER"
        fontSetId = "YANAMI_PERF_FONT_SET_ID"
        fontSetSha256 = "YANAMI_PERF_FONT_SET_SHA256"
        fontCacheState = "YANAMI_PERF_FONT_CACHE_STATE"
        fontCachePreparationId = "YANAMI_PERF_FONT_CACHE_PREPARATION_ID"
    }
    foreach ($property in $environmentOverrides.Keys) {
        $value = [Environment]::GetEnvironmentVariable([string]$environmentOverrides[$property])
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            $details[$property] = $value.Trim()
        }
    }
    $canonical = [ordered]@{
        cpumodel = ([string]$details.cpuModel).Trim().ToLowerInvariant()
        dpiscalepercent = ([string]$details.dpiScalePercent).Trim().ToLowerInvariant()
        displayrefreshhz = ([string]$details.displayRefreshHz).Trim().ToLowerInvariant()
        fontcachestate = ([string]$details.fontCacheState).Trim().ToLowerInvariant()
        fontcachepreparationid = ([string]$details.fontCachePreparationId).Trim().ToLowerInvariant()
        fontsetid = ([string]$details.fontSetId).Trim().ToLowerInvariant()
        fontsetsha256 = ([string]$details.fontSetSha256).Trim().ToLowerInvariant()
        gpudriver = ([string]$details.gpuDriver).Trim().ToLowerInvariant()
        gpumodel = ([string]$details.gpuModel).Trim().ToLowerInvariant()
        hdrenabled = ([string]$details.hdrEnabled).Trim().ToLowerInvariant()
        powerplan = ([string]$details.powerPlan).Trim().ToLowerInvariant()
        qtversion = ([string]$details.qtVersion).Trim().ToLowerInvariant()
        qtrhirenderer = ([string]$details.qtRhiRenderer).Trim().ToLowerInvariant()
        renderheightpixels = ([string]$details.renderHeightPixels).Trim().ToLowerInvariant()
        renderwidthpixels = ([string]$details.renderWidthPixels).Trim().ToLowerInvariant()
        rustversion = ([string]$details.rustVersion).Trim().ToLowerInvariant()
        storagemodel = ([string]$details.storageModel).Trim().ToLowerInvariant()
        vrrenabled = ([string]$details.vrrEnabled).Trim().ToLowerInvariant()
        windowmode = ([string]$details.windowMode).Trim().ToLowerInvariant()
        windowsbuild = ([string]$details.windowsBuild).Trim().ToLowerInvariant()
    }
    $computedFingerprint = Get-TextSha256 -Value ($canonical | ConvertTo-Json -Compress -Depth 10)
    $fingerprint = if ($env:YANAMI_PERF_MACHINE_FINGERPRINT) { $env:YANAMI_PERF_MACHINE_FINGERPRINT } else { $computedFingerprint }
    $details.computedFingerprint = $computedFingerprint
    $details.fingerprintSource = if ($env:YANAMI_PERF_MACHINE_FINGERPRINT) { "orchestrator" } else { "runner" }
    $script:runEnvironmentDetails = [pscustomobject]$details
    $env:YANAMI_PERF_MACHINE_FINGERPRINT = $fingerprint
    $env:YANAMI_PERF_RUN_FINGERPRINT = $fingerprint
    return $fingerprint
}

function Initialize-F110KFixture {
    param([string]$RequestedDirectory)
    $fixtureManifestPath = Join-Path $workspace "perf\fixtures\f110k.manifest.json"
    $fixtureContract = Get-Content -LiteralPath $fixtureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $directory = $RequestedDirectory
    if (-not $directory) { $directory = $env:YANAMI_PERF_FIXTURE_DIR }
    $managedDirectory = [string]::IsNullOrWhiteSpace($directory)
    if (-not $directory) {
        $temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
        $directory = Join-Path $temporaryRoot "yanami-f110k-v1"
    }
    $directory = [System.IO.Path]::GetFullPath($directory)
    $missing = @($fixtureContract.files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $directory $_.name) -PathType Leaf) })
    if ($missing.Count -gt 0) {
        if (-not $managedDirectory -and $missing.Count -ne @($fixtureContract.files).Count) {
            throw "Configured F110K directory is incomplete; refusing to overwrite a partial fixture: $directory"
        }
        if ($managedDirectory) {
            & (Join-Path $scriptRoot "New-F110KFixture.ps1") -OutputDirectory $directory -Force | Out-Null
        } else {
            & (Join-Path $scriptRoot "New-F110KFixture.ps1") -OutputDirectory $directory | Out-Null
        }
    }
    $readValidatedHashes = {
        $values = New-Object System.Collections.Generic.List[string]
        foreach ($file in @($fixtureContract.files)) {
            $path = Join-Path $directory $file.name
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            $expected = [string]$file.sha256
            if ($actual -ne $expected) { throw "F110K fixture hash mismatch for '$($file.name)': expected $expected, got $actual." }
            $values.Add($actual)
        }
        return $values.ToArray()
    }
    try {
        $hashes = @(& $readValidatedHashes)
    } catch {
        if (-not $managedDirectory) { throw }
        # The runner owns its versioned temp directory, so a prior contract
        # revision can be regenerated safely. Explicit fixture paths are never
        # overwritten on a hash mismatch.
        & (Join-Path $scriptRoot "New-F110KFixture.ps1") -OutputDirectory $directory -Force | Out-Null
        $hashes = @(& $readValidatedHashes)
    }
    $combined = Get-TextSha256 -Value ($hashes -join ":")
    if ($combined -ne [string]$fixtureContract.fixtureSha256) { throw "F110K combined fixture hash mismatch." }
    $env:YANAMI_PERF_FIXTURE_DIR = $directory
    $env:YANAMI_PERF_F110K_SHA256 = $combined
    Write-Host "Validated F110K-v1 fixture: $combined"
    return [pscustomobject]@{ directory = $directory; sha256 = $combined }
}

function Initialize-DanmakuFixture {
    param([string]$RequestedDirectory)

    $fixtureManifestPath = Join-Path $workspace "perf\fixtures\danmaku-density-v1.manifest.json"
    $fixtureContract = Get-Content -LiteralPath $fixtureManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $directory = $RequestedDirectory
    if (-not $directory) { $directory = $env:YANAMI_PERF_DANMAKU_FIXTURE_DIR }
    $managedDirectory = [string]::IsNullOrWhiteSpace($directory)
    if (-not $directory) {
        $temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
        $directory = Join-Path $temporaryRoot "yanami-danmaku-density-v1"
    }
    $directory = [System.IO.Path]::GetFullPath($directory)
    $missing = @($fixtureContract.files | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $directory $_.name) -PathType Leaf)
    })
    if ($missing.Count -gt 0) {
        if (-not $managedDirectory -and $missing.Count -ne @($fixtureContract.files).Count) {
            throw "Configured DanmakuDensity-v1 directory is incomplete; refusing to overwrite a partial fixture: $directory"
        }
        $generatorArguments = @{
            OutputDirectory = $directory
            CommentCount = [int]$fixtureContract.counts.strictTotal
            DurationSeconds = [int]$fixtureContract.durationSeconds
            Seed = [int]$fixtureContract.seed
        }
        if ($managedDirectory) { $generatorArguments.Force = $true }
        & (Join-Path $scriptRoot "New-DanmakuFixture.ps1") @generatorArguments | Out-Null
    }

    $validate = {
        foreach ($file in @($fixtureContract.files)) {
            $path = Join-Path $directory $file.name
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            $expected = ([string]$file.sha256).ToLowerInvariant()
            if ($actual -ne $expected) {
                throw "DanmakuDensity-v1 fixture hash mismatch for '$($file.name)': expected $expected, got $actual."
            }
        }
    }
    try {
        & $validate
    } catch {
        if (-not $managedDirectory) { throw }
        & (Join-Path $scriptRoot "New-DanmakuFixture.ps1") `
            -OutputDirectory $directory `
            -CommentCount ([int]$fixtureContract.counts.strictTotal) `
            -DurationSeconds ([int]$fixtureContract.durationSeconds) `
            -Seed ([int]$fixtureContract.seed) `
            -Force | Out-Null
        & $validate
    }
    $fixtureSha256 = ([string]$fixtureContract.fixtureSha256).ToLowerInvariant()
    $env:YANAMI_PERF_DANMAKU_FIXTURE_DIR = $directory
    $env:YANAMI_PERF_DANMAKU_SHA256 = $fixtureSha256
    Write-Host "Validated DanmakuDensity-v1 fixture: $fixtureSha256"
    return [pscustomobject]@{ directory = $directory; sha256 = $fixtureSha256 }
}

function Initialize-UpscalingCapabilityFixture {
    $manifestPath = Join-Path $workspace "perf\fixtures\upscaling-capability-v1.manifest.json"
    $fixturePath = Join-Path $workspace "perf\fixtures\upscaling-capability-v1.json"
    $contract = Read-YanamiPerfJson -Path $manifestPath
    if ([string]$contract.id -ne "UpscalingCapability-v1" -or
        [string]$contract.file -ne [System.IO.Path]::GetFileName($fixturePath)) {
        throw "UpscalingCapability-v1 manifest is malformed."
    }
    $actualSha = (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedSha = ([string]$contract.sha256).ToLowerInvariant()
    if ($actualSha -ne $expectedSha) {
        throw "UpscalingCapability-v1 fixture hash mismatch: expected $expectedSha, got $actualSha."
    }
    Write-Host "Validated UpscalingCapability-v1 fixture: $actualSha"
    return [pscustomobject][ordered]@{
        path = $fixturePath
        manifestPath = $manifestPath
        sha256 = $actualSha
    }
}

function Assert-UpscalingFixtureEvidence {
    param(
        [object[]]$Manifests,
        [bool]$StrictRequired
    )

    $capabilityContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\fixtures\upscaling-capability-v1.manifest.json")
    $expectedCapabilitySha = ([string]$capabilityContract.sha256).ToLowerInvariant()
    foreach ($probeManifest in @($Manifests)) {
        foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "UpscalingCapability-v1" })) {
            if (-not [bool]$fixture.validated -or
                ([string]$fixture.sha256).ToLowerInvariant() -ne $expectedCapabilitySha) {
                throw "Probe '$($probeManifest.runId)' reported invalid UpscalingCapability-v1 evidence."
            }
        }
    }
    if (-not $StrictRequired) { return }

    $playbackContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\fixtures\playback-media-v1.manifest.json")
    $modelContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\fixtures\upscaling-model-pack-v1.manifest.json")
    if ([string]$playbackContract.state -ne "provisioned") {
        throw "Strict upscaling requires provisioned PlaybackMedia-v1; the pinned policy is currently '$($playbackContract.state)'."
    }
    if ([string]$modelContract.state -ne "provisioned") {
        throw "Strict upscaling requires provisioned UpscalingModelPack-v1; the pinned policy is currently '$($modelContract.state)'."
    }
    foreach ($requiredId in @("UpscalingCapability-v1", "PlaybackMedia-v1", "UpscalingModelPack-v1")) {
        $matches = @($Manifests.fixtures | Where-Object { [string]$_.id -eq $requiredId -and [bool]$_.validated })
        if ($matches.Count -eq 0) {
            throw "Strict upscaling evidence requires validated fixture '$requiredId'."
        }
    }
    foreach ($scenarioManifest in @($Manifests | Where-Object {
        [string]$_.profile -ne "PullRequest" -and "upscaling" -in @($_.suites)
    })) {
        $scenarioDetails = $scenarioManifest.environment.details
        $modelMatches = @($modelContract.requiredModelPacks | Where-Object {
            [string]$_.provider -eq [string]$scenarioDetails.provider -and
            [string]$_.preset -eq [string]$scenarioDetails.preset -and
            [string]$_.sha256 -match '^[0-9a-fA-F]{64}$' -and
            [string]$_.sha256 -eq [string]$scenarioDetails.modelPackSha256 -and
            [long]$_.sizeBytes -gt 0 -and
            -not [string]::IsNullOrWhiteSpace([string]$_.version) -and
            -not [string]::IsNullOrWhiteSpace([string]$_.licenseId)
        })
        if ($modelMatches.Count -ne 1) {
            throw "Strict upscaling scenario '$([string]$scenarioDetails.upscalingScenarioId)' is not bound to exactly one fully pinned model-pack entry."
        }
    }

    $scenarioManifests = @($Manifests | Where-Object {
        [string]$_.profile -ne "PullRequest" -and
        "upscaling" -in @($_.suites | ForEach-Object { ([string]$_).ToLowerInvariant() }) -and
        @($_.metrics | Where-Object {
            [string]$_.id -like "upscaling.*" -and
            -not ([string]$_.id).StartsWith("upscaling.hosted_smoke.", [System.StringComparison]::Ordinal)
        }).Count -gt 0
    })
    if ($scenarioManifests.Count -ne 3) {
        throw "Strict upscaling matrix requires exactly three independently normalized scenario manifests; found $($scenarioManifests.Count)."
    }
    $presets = @($scenarioManifests | ForEach-Object {
        [string]$_.environment.details.preset
    } | Sort-Object -Unique)
    if (($presets -join ",") -ne "balanced,performance,quality") {
        throw "Strict upscaling matrix requires exactly one performance, balanced, and quality scenario."
    }
    $scenarioIds = @($scenarioManifests | ForEach-Object {
        [string]$_.environment.details.upscalingScenarioId
    } | Sort-Object -Unique)
    if ($scenarioIds.Count -ne 3) {
        throw "Strict upscaling matrix scenario IDs must be non-empty and distinct."
    }
    foreach ($field in @("candidateSha")) {
        $values = @($scenarioManifests | ForEach-Object { [string]$_.$field } | Sort-Object -Unique)
        if ($values.Count -ne 1) { throw "Strict upscaling matrix has inconsistent '$field'." }
    }
    $fingerprints = @($scenarioManifests | ForEach-Object {
        [string]$_.environment.fingerprint
    } | Sort-Object -Unique)
    if ($fingerprints.Count -ne 1) {
        throw "Strict upscaling matrix scenarios must use the same environment fingerprint."
    }
    foreach ($field in @("upscalingMatrixId", "provider", "providerRuntimeVersion")) {
        $values = @($scenarioManifests | ForEach-Object {
            [string]$_.environment.details.$field
        } | Sort-Object -Unique)
        if ($values.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$values[0])) {
            throw "Strict upscaling matrix has inconsistent environment.details.$field."
        }
    }
    $playbackHashes = @($scenarioManifests | ForEach-Object {
        @($_.fixtures | Where-Object { [string]$_.id -eq "PlaybackMedia-v1" }) |
            ForEach-Object { [string]$_.sha256 }
    } | Sort-Object -Unique)
    if ($playbackHashes.Count -ne 1 -or $playbackHashes[0] -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Strict upscaling matrix scenarios must share one pinned PlaybackMedia-v1 hash."
    }
    $rawIdentities = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($scenarioManifest in $scenarioManifests) {
        foreach ($artifact in @($scenarioManifest.artifacts | Where-Object {
            [bool]$_.runnerValidated -and [string]$_.role -ne "upscaling-model-pack-index"
        })) {
            $identity = "$([string]$artifact.fileName)|$(([string]$artifact.sha256).ToLowerInvariant())"
            if (-not $rawIdentities.Add($identity)) {
                throw "Strict upscaling raw evidence '$identity' was reused across preset scenarios."
            }
        }
    }

    $matrixId = [string]$scenarioManifests[0].environment.details.upscalingMatrixId
    $scenarioSummary = @($scenarioManifests | ForEach-Object {
        [pscustomobject][ordered]@{
            scenarioId = [string]$_.environment.details.upscalingScenarioId
            preset = [string]$_.environment.details.preset
            modelPackSha256 = [string]$_.environment.details.modelPackSha256
        }
    })
    foreach ($scenarioManifest in $scenarioManifests) {
        $scenarioManifest.invariants = @($scenarioManifest.invariants | Where-Object {
            [string]$_.id -ne "upscaling.strict_matrix_complete"
        })
    }
    $scenarioManifests[0].invariants += [pscustomobject][ordered]@{
        id = "upscaling.strict_matrix_complete"
        passed = $true
        details = [pscustomobject][ordered]@{
            evidence = "upscaling-strict-scenario-index"
            rawDerived = $true
            runnerGenerated = $true
            matrixId = $matrixId
            scenarios = $scenarioSummary
        }
    }
}

function Assert-DanmakuFixtureEvidence {
    param([object[]]$Manifests)

    $contractPath = Join-Path $workspace "perf\fixtures\danmaku-density-v1.manifest.json"
    $contract = Read-YanamiPerfJson -Path $contractPath
    $expectedHash = ([string]$contract.fixtureSha256).ToLowerInvariant()
    foreach ($probeManifest in @($Manifests)) {
        $fixtures = @($probeManifest.fixtures | Where-Object {
            [string]$_.id -eq "DanmakuDensity-v1"
        })
        if ($fixtures.Count -gt 1) {
            throw "Probe '$($probeManifest.runId)' reported DanmakuDensity-v1 more than once."
        }
        if ($fixtures.Count -eq 1) {
            $actualHash = ([string]$fixtures[0].sha256).ToLowerInvariant()
            if ($actualHash -ne $expectedHash) {
                throw "Probe '$($probeManifest.runId)' reported an unrecognized DanmakuDensity-v1 hash: expected $expectedHash, got $actualHash."
            }
        }
    }
}

function Get-DanmakuEvidenceArtifactRoles {
    param([string]$Evidence)

    switch ($Evidence) {
        "external-present" { return @("presentmon-trace") }
        "external-pixel-present" { return @("presentmon-trace", "pixel-capture-index") }
        "external-pixel-oracle" { return @("presentmon-trace", "pixel-capture-index") }
        "mpv-paired-telemetry" { return @("mpv-telemetry", "paired-baseline-index") }
        "etw-paired-process" { return @("etw-process-counters", "paired-baseline-index") }
        "external-present-mpv-correlated" { return @("presentmon-trace", "mpv-telemetry", "frame-correlation-index") }
        "etw-gpu-process" { return @("etw-gpu-engine-counters") }
        "dxgi-video-memory" { return @("dxgi-video-memory-counters") }
        "yanami-upscaling-runtime-trace" { return @("yanami-upscaling-runtime-trace") }
        "pinned-upscaling-model-pack" { return @("upscaling-model-pack-index") }
        "upscaling-strict-scenario-index" { return @("upscaling-scenario-index") }
        default { return @() }
    }
}

function Copy-ValidatedEvidenceFile {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256
    )

    $destination = Join-Path $resolvedOutput $FileName
    if ([System.IO.Path]::GetFullPath($SourcePath).Equals(
            [System.IO.Path]::GetFullPath($destination),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return
    }
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $destinationSha = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($destinationSha -ne $ExpectedSha256) {
            throw "Strict evidence output collision for '$FileName'."
        }
        return
    }
    Copy-Item -LiteralPath $SourcePath -Destination $destination
}

function Assert-DanmakuStrictEvidenceArtifacts {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    if ([string]$Manifest.profile -eq "PullRequest") { return $Manifest }
    $requiredRoles = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($metric in @($Manifest.metrics)) {
        if ([string]$metric.id -notlike "danmaku.*") { continue }
        $evidence = if ($null -ne $metric.PSObject.Properties["attributes"] -and
            $null -ne $metric.attributes.PSObject.Properties["evidence"]) {
            [string]$metric.attributes.evidence
        } else { "" }
        foreach ($role in @(Get-DanmakuEvidenceArtifactRoles -Evidence $evidence)) {
            [void]$requiredRoles.Add($role)
        }
    }
    if ($requiredRoles.Count -eq 0) { return $Manifest }

    $rendering = if ($null -ne $Manifest.environment.PSObject.Properties["rendering"]) {
        $Manifest.environment.rendering
    } else { $null }
    if ($null -eq $rendering) {
        throw "Strict danmaku evidence must declare environment.rendering; a machine fingerprint alone is insufficient."
    }
    $referenceContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\environments\windows-reference-v1.json")
    $danmakuContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\fixtures\danmaku-density-v1.manifest.json")
    $expectedRendering = $referenceContract.strictRendering
    foreach ($field in @(
        "renderWidthPixels",
        "renderHeightPixels",
        "displayRefreshHz",
        "dpiScalePercent",
        "hdrEnabled",
        "vrrEnabled",
        "windowMode",
        "qtRhiRenderer",
        "fontCacheState",
        "fontCachePreparationId"
    )) {
        $actualValue = if ($null -ne $rendering.PSObject.Properties[$field]) {
            [string]$rendering.$field
        } else { "<missing>" }
        $expectedValue = [string]$expectedRendering.$field
        if (-not $actualValue.Equals($expectedValue, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Strict danmaku rendering field '$field' must be '$expectedValue', got '$actualValue'."
        }
    }
    if ([string]$rendering.fontSetId -ne [string]$expectedRendering.fontSet.id) {
        throw "Strict danmaku fontSetId must be '$($expectedRendering.fontSet.id)'."
    }
    if ([string]$rendering.fontSetSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Strict danmaku fontSetSha256 must identify the lab-approved font set."
    }
    $actualFamilies = @($rendering.fontFamilies | ForEach-Object {
        ([string]$_).Trim().ToLowerInvariant()
    } | Sort-Object -Unique)
    $expectedFamilies = @($expectedRendering.fontSet.families | ForEach-Object {
        ([string]$_).Trim().ToLowerInvariant()
    } | Sort-Object -Unique)
    if (($actualFamilies -join "|") -ne ($expectedFamilies -join "|")) {
        throw "Strict danmaku fontFamilies do not match WindowsDanmakuFonts-v1."
    }

    $manifestDirectory = [System.IO.Path]::GetDirectoryName(
        [System.IO.Path]::GetFullPath($ManifestPath))
    $validatedRoles = New-Object System.Collections.Generic.List[string]
    foreach ($role in @($requiredRoles | Sort-Object)) {
        $matches = @($Manifest.artifacts | Where-Object {
            [string]$_.role -eq $role
        })
        if ($matches.Count -ne 1) {
            throw "Strict danmaku evidence '$([System.IO.Path]::GetFileName($ManifestPath))' requires exactly one '$role' artifact; found $($matches.Count)."
        }
        $artifact = $matches[0]
        $fileName = [string]$artifact.fileName
        if ([string]::IsNullOrWhiteSpace($fileName) -or
            [System.IO.Path]::GetFileName($fileName) -ne $fileName) {
            throw "Strict danmaku artifact '$role' must use a flat, portable fileName next to its run manifest."
        }
        $artifactPath = Join-Path $manifestDirectory $fileName
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            throw "Strict danmaku artifact '$role' does not exist next to the run manifest: $artifactPath"
        }
        $fileInfo = Get-Item -LiteralPath $artifactPath
        if ($fileInfo.Length -le 0) {
            throw "Strict danmaku artifact '$role' is empty: $artifactPath"
        }
        $expectedSha = ([string]$artifact.sha256).ToLowerInvariant()
        if ($expectedSha -notmatch '^[0-9a-f]{64}$') {
            throw "Strict danmaku artifact '$role' has an invalid SHA-256."
        }
        $collector = if ($null -ne $artifact.PSObject.Properties["collector"]) {
            $artifact.collector
        } else { $null }
        if ($null -eq $collector -or
            [string]::IsNullOrWhiteSpace([string]$collector.name) -or
            [string]::IsNullOrWhiteSpace([string]$collector.version) -or
            [string]::IsNullOrWhiteSpace([string]$collector.kind) -or
            -not ([string]$collector.clockDomain).Equals("QPC", [System.StringComparison]::OrdinalIgnoreCase) -or
            [string]$collector.executableSha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Strict danmaku artifact '$role' requires a versioned QPC collector with an executable SHA-256."
        }
        $actualSha = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualSha -ne $expectedSha) {
            throw "Strict danmaku artifact '$role' hash mismatch: expected $expectedSha, got $actualSha."
        }
        $artifactCandidate = if ($null -ne $artifact.PSObject.Properties["candidateSha"]) {
            [string]$artifact.candidateSha
        } else { "" }
        if ([string]::IsNullOrWhiteSpace($artifactCandidate) -or
            $artifactCandidate -ne [string]$Manifest.candidateSha) {
            throw "Strict danmaku artifact '$role' must bind to manifest candidate SHA '$($Manifest.candidateSha)'."
        }
        if ($role -in @("pixel-capture-index", "paired-baseline-index")) {
            try { $index = Get-Content -LiteralPath $artifactPath -Raw -Encoding UTF8 | ConvertFrom-Json }
            catch { throw "Strict danmaku artifact '$role' must be valid JSON: $($_.Exception.Message)" }
            $requiredIndexFields = if ($role -eq "pixel-capture-index") {
                @($danmakuContract.strictEvidenceBundle.pixelCaptureIndexRequiredFields)
            } else {
                @($danmakuContract.strictEvidenceBundle.pairedBaselineIndexRequiredFields)
            }
            foreach ($field in $requiredIndexFields) {
                $indexProperty = $index.PSObject.Properties[[string]$field]
                if ($null -eq $indexProperty -or
                    $null -eq $indexProperty.Value -or
                    ($indexProperty.Value -is [string] -and
                        [string]::IsNullOrWhiteSpace([string]$indexProperty.Value))) {
                    throw "Strict danmaku artifact '$role' is missing index field '$field'."
                }
            }
            if ([string]$index.schemaVersion -ne "1.0" -or
                [string]$index.candidateSha -ne [string]$Manifest.candidateSha -or
                [string]$index.fixtureSha256 -ne [string]$danmakuContract.fixtureSha256 -or
                [string]$index.environmentFingerprint -ne [string]$Manifest.environment.fingerprint) {
                throw "Strict danmaku artifact '$role' index is not bound to this schema, candidate, fixture, and environment."
            }
            if ($role -eq "pixel-capture-index" -and @($index.captures).Count -eq 0) {
                throw "Strict danmaku pixel-capture-index must list at least one capture."
            }
            if ($role -eq "pixel-capture-index") {
                foreach ($capture in @($index.captures)) {
                    $captureName = [string]$capture.fileName
                    $captureSha = ([string]$capture.sha256).ToLowerInvariant()
                    if ([string]::IsNullOrWhiteSpace($captureName) -or
                        [System.IO.Path]::GetFileName($captureName) -ne $captureName -or
                        $captureSha -notmatch '^[0-9a-f]{64}$') {
                        throw "Strict danmaku pixel capture entries require a flat fileName and SHA-256."
                    }
                    $capturePath = Join-Path $manifestDirectory $captureName
                    if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf) -or
                        (Get-Item -LiteralPath $capturePath).Length -le 0) {
                        throw "Strict danmaku pixel capture is missing or empty: $capturePath"
                    }
                    $actualCaptureSha = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash.ToLowerInvariant()
                    if ($actualCaptureSha -ne $captureSha) {
                        throw "Strict danmaku pixel capture '$captureName' hash mismatch."
                    }
                    Copy-ValidatedEvidenceFile `
                        -SourcePath $capturePath `
                        -FileName $captureName `
                        -ExpectedSha256 $captureSha
                }
            }
            if ($role -eq "paired-baseline-index" -and
                [string]$index.overlayEnabledRunId -eq [string]$index.overlayDisabledRunId) {
                throw "Strict danmaku paired baseline must use distinct overlay-enabled and overlay-disabled run IDs."
            }
        }
        $artifact | Add-Member -NotePropertyName runnerValidated -NotePropertyValue $true -Force
        $artifact | Add-Member -NotePropertyName validatedBytes -NotePropertyValue ([long]$fileInfo.Length) -Force
        Copy-ValidatedEvidenceFile `
            -SourcePath $artifactPath `
            -FileName $fileName `
            -ExpectedSha256 $actualSha
        $validatedRoles.Add($role)
    }
    $attestation = [pscustomobject][ordered]@{
        validator = "scripts/performance/run-gate.ps1"
        validatedAtUtc = [DateTime]::UtcNow.ToString("o")
        validatedRoles = $validatedRoles.ToArray()
    }
    $Manifest.environment | Add-Member `
        -NotePropertyName strictEvidenceValidation `
        -NotePropertyValue $attestation `
        -Force
    return $Manifest
}

function Assert-UpscalingStrictEvidenceArtifacts {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    if ([string]$Manifest.profile -eq "PullRequest") { return $Manifest }
    $upscalingObservations = @($Manifest.metrics | Where-Object { [string]$_.id -like "upscaling.*" }) +
        @($Manifest.invariants | Where-Object { [string]$_.id -like "upscaling.*" })
    if ($upscalingObservations.Count -eq 0) { return $Manifest }

    $details = if ($null -ne $Manifest.environment.PSObject.Properties["details"]) {
        $Manifest.environment.details
    } else { $null }
    $scenarioId = if ($null -ne $details) { [string]$details.upscalingScenarioId } else { "" }
    if ([string]::IsNullOrWhiteSpace($scenarioId)) {
        throw "Strict upscaling evidence requires environment.details.upscalingScenarioId."
    }
    if (-not [bool]$details.gpuCertified -or -not [bool]$details.presentCertified) {
        throw "Strict upscaling evidence must declare gpuCertified=true and presentCertified=true; hosted/offscreen evidence cannot be promoted."
    }
    foreach ($field in @("upscalingMatrixId", "provider", "preset", "providerRuntimeVersion", "modelPackSha256")) {
        $value = if ($null -ne $details.PSObject.Properties[$field]) { [string]$details.$field } else { "" }
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "Strict upscaling evidence is missing environment.details.$field."
        }
    }
    if (-not ([string]$details.provider).Equals(
            "anime4k", [System.StringComparison]::Ordinal)) {
        throw "Strict upscaling certification permits only provider='anime4k'."
    }
    if ([string]$details.preset -notin @("performance", "balanced", "quality")) {
        throw "Strict upscaling preset must be performance, balanced, or quality."
    }
    if ([string]$details.modelPackSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Strict upscaling modelPackSha256 is invalid."
    }
    $rendering = if ($null -ne $Manifest.environment.PSObject.Properties["rendering"]) {
        $Manifest.environment.rendering
    } else { $null }
    if ($null -eq $rendering) {
        throw "Strict upscaling evidence must declare the native rendering environment."
    }
    $referenceContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\environments\windows-reference-v1.json")
    $expectedRendering = $referenceContract.upscalingStrictRendering
    foreach ($field in @(
        "renderWidthPixels",
        "renderHeightPixels",
        "displayRefreshHz",
        "dpiScalePercent",
        "hdrEnabled",
        "vrrEnabled",
        "windowMode",
        "qtRhiRenderer"
    )) {
        $actual = if ($null -ne $rendering.PSObject.Properties[$field]) { [string]$rendering.$field } else { "<missing>" }
        $expected = [string]$expectedRendering.$field
        if (-not $actual.Equals($expected, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Strict upscaling rendering field '$field' must be '$expected', got '$actual'."
        }
    }
    if (-not ([string]$rendering.mpvRenderApi).Equals(
            [string]$expectedRendering.mpvRenderApi,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Strict upscaling evidence must use the production '$($expectedRendering.mpvRenderApi)' video-render path."
    }
    $openGlMajor = [int]$rendering.openGlMajor
    $openGlMinor = [int]$rendering.openGlMinor
    $minimumOpenGlMajor = [int]$expectedRendering.minimumOpenGlMajor
    $minimumOpenGlMinor = [int]$expectedRendering.minimumOpenGlMinor
    if ($openGlMajor -lt $minimumOpenGlMajor -or
        ($openGlMajor -eq $minimumOpenGlMajor -and $openGlMinor -lt $minimumOpenGlMinor)) {
        throw "Strict upscaling evidence requires OpenGL $minimumOpenGlMajor.$minimumOpenGlMinor or newer."
    }
    if ([int]$rendering.maximumTextureSize -lt [int]$expectedRendering.minimumTextureSize) {
        throw "Strict upscaling evidence requires maximumTextureSize >= $($expectedRendering.minimumTextureSize)."
    }

    $modelPackContract = Read-YanamiPerfJson -Path (
        Join-Path $workspace "perf\fixtures\upscaling-model-pack-v1.manifest.json")
    $normalizerContract = $modelPackContract.strictEvidenceBundle.measurementNormalizer
    $declaredNormalizers = @($normalizerContract.approvedExecutables |
        Where-Object { $null -ne $_ })
    if ($null -eq $normalizerContract -or
        [string]$normalizerContract.state -ne "provisioned" -or
        $declaredNormalizers.Count -eq 0) {
        throw "Strict upscaling requires a provisioned, hash-pinned measurement normalizer; imported samples and indexes are never trusted."
    }
    $normalizerPath = [string]$env:YANAMI_PERF_UPSCALING_NORMALIZER
    if ([string]::IsNullOrWhiteSpace($normalizerPath) -or
        -not [System.IO.Path]::IsPathFullyQualified($normalizerPath) -or
        -not (Test-Path -LiteralPath $normalizerPath -PathType Leaf)) {
        throw "Strict upscaling requires YANAMI_PERF_UPSCALING_NORMALIZER to name the provisioned absolute normalizer executable."
    }
    $normalizerPath = [System.IO.Path]::GetFullPath($normalizerPath)
    $normalizerSha = (Get-FileHash -LiteralPath $normalizerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $approvedNormalizers = @($normalizerContract.approvedExecutables | Where-Object {
        ([string]$_.sha256).ToLowerInvariant() -eq $normalizerSha -and
        -not [string]::IsNullOrWhiteSpace([string]$_.name) -and
        -not [string]::IsNullOrWhiteSpace([string]$_.version) -and
        -not [string]::IsNullOrWhiteSpace([string]$_.kind)
    })
    if ($approvedNormalizers.Count -ne 1) {
        throw "Strict upscaling normalizer executable SHA-256 '$normalizerSha' is not uniquely approved by UpscalingModelPack-v1."
    }
    $approvedNormalizer = $approvedNormalizers[0]

    $requiredRoles = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($observation in $upscalingObservations) {
        $evidence = ""
        if ($null -ne $observation.PSObject.Properties["attributes"] -and
            $null -ne $observation.attributes.PSObject.Properties["evidence"]) {
            $evidence = [string]$observation.attributes.evidence
            if ([string]$observation.attributes.scenarioId -ne $scenarioId) {
                throw "Strict upscaling metric '$($observation.id)' is not bound to scenario '$scenarioId'."
            }
        } elseif ($null -ne $observation.PSObject.Properties["details"] -and
            $observation.details -isnot [string] -and
            $null -ne $observation.details.PSObject.Properties["evidence"]) {
            $evidence = [string]$observation.details.evidence
            if ([string]$observation.details.scenarioId -ne $scenarioId) {
                throw "Strict upscaling invariant '$($observation.id)' is not bound to scenario '$scenarioId'."
            }
        }
        foreach ($role in @(Get-DanmakuEvidenceArtifactRoles -Evidence $evidence)) {
            [void]$requiredRoles.Add($role)
        }
    }
    if ($requiredRoles.Count -eq 0) {
        throw "Strict upscaling observations did not declare a recognized external evidence contract."
    }

    $manifestDirectory = [System.IO.Path]::GetDirectoryName(
        [System.IO.Path]::GetFullPath($ManifestPath))
    foreach ($artifact in @($Manifest.artifacts)) {
        $artifact | Add-Member -NotePropertyName runnerValidated -NotePropertyValue $false -Force
    }
    $validatedRoles = New-Object System.Collections.Generic.List[string]
    foreach ($role in @($requiredRoles | Sort-Object)) {
        $matches = @($Manifest.artifacts | Where-Object {
            [string]$_.role -eq $role -and [string]$_.scenarioId -eq $scenarioId
        })
        if ($matches.Count -ne 1) {
            throw "Strict upscaling scenario '$scenarioId' requires exactly one '$role' artifact; found $($matches.Count)."
        }
        $artifact = $matches[0]
        $fileName = [string]$artifact.fileName
        if ([string]::IsNullOrWhiteSpace($fileName) -or
            [System.IO.Path]::GetFileName($fileName) -ne $fileName) {
            throw "Strict upscaling artifact '$role' must use a flat fileName."
        }
        $artifactPath = Join-Path $manifestDirectory $fileName
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf) -or
            (Get-Item -LiteralPath $artifactPath).Length -le 0) {
            throw "Strict upscaling artifact '$role' is missing or empty: $artifactPath"
        }
        $expectedSha = ([string]$artifact.sha256).ToLowerInvariant()
        if ($expectedSha -notmatch '^[0-9a-f]{64}$') {
            throw "Strict upscaling artifact '$role' has an invalid SHA-256."
        }
        $actualSha = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualSha -ne $expectedSha) {
            throw "Strict upscaling artifact '$role' hash mismatch."
        }
        if ([string]$artifact.candidateSha -ne [string]$Manifest.candidateSha) {
            throw "Strict upscaling artifact '$role' is not bound to candidate '$($Manifest.candidateSha)'."
        }
        $collector = $artifact.collector
        if ($null -eq $collector -or
            [string]::IsNullOrWhiteSpace([string]$collector.name) -or
            [string]::IsNullOrWhiteSpace([string]$collector.version) -or
            [string]::IsNullOrWhiteSpace([string]$collector.kind) -or
            -not ([string]$collector.clockDomain).Equals("QPC", [System.StringComparison]::OrdinalIgnoreCase) -or
            [string]$collector.executableSha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Strict upscaling artifact '$role' requires a versioned QPC collector with executable SHA-256."
        }

        if ($role -in @(
            "pixel-capture-index",
            "paired-baseline-index",
            "frame-correlation-index",
            "upscaling-model-pack-index",
            "upscaling-scenario-index"
        )) {
            try { $index = Get-Content -LiteralPath $artifactPath -Raw -Encoding UTF8 | ConvertFrom-Json }
            catch { throw "Strict upscaling artifact '$role' must be valid JSON: $($_.Exception.Message)" }
            if ([string]$index.schemaVersion -ne "1.0" -or
                [string]$index.candidateSha -ne [string]$Manifest.candidateSha -or
                [string]$index.environmentFingerprint -ne [string]$Manifest.environment.fingerprint -or
                [string]$index.scenarioId -ne $scenarioId) {
                throw "Strict upscaling artifact '$role' index is not bound to this schema, candidate, environment, and scenario."
            }
            $fixtureIds = @($index.fixtureIds | ForEach-Object { [string]$_ })
            foreach ($requiredFixtureId in @("PlaybackMedia-v1", "UpscalingModelPack-v1")) {
                if ($requiredFixtureId -notin $fixtureIds) {
                    throw "Strict upscaling artifact '$role' is not bound to fixture '$requiredFixtureId'."
                }
            }
            if ($role -eq "pixel-capture-index") {
                if (@($index.captures).Count -eq 0) {
                    throw "Strict upscaling pixel-capture-index must list at least one capture."
                }
                foreach ($capture in @($index.captures)) {
                    $captureName = [string]$capture.fileName
                    $captureSha = ([string]$capture.sha256).ToLowerInvariant()
                    $capturePath = Join-Path $manifestDirectory $captureName
                    if ([System.IO.Path]::GetFileName($captureName) -ne $captureName -or
                        $captureSha -notmatch '^[0-9a-f]{64}$' -or
                        -not (Test-Path -LiteralPath $capturePath -PathType Leaf) -or
                        (Get-Item -LiteralPath $capturePath).Length -le 0 -or
                        (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $captureSha) {
                        throw "Strict upscaling pixel capture '$captureName' is missing, empty, or hash-invalid."
                    }
                    Copy-ValidatedEvidenceFile -SourcePath $capturePath -FileName $captureName -ExpectedSha256 $captureSha
                }
            }
            if ($role -eq "paired-baseline-index" -and
                ([string]$index.upscalingEnabledRunId -eq [string]$index.upscalingDisabledRunId -or
                 [string]::IsNullOrWhiteSpace([string]$index.upscalingEnabledRunId) -or
                 [string]::IsNullOrWhiteSpace([string]$index.upscalingDisabledRunId))) {
                throw "Strict upscaling paired baseline requires distinct enabled and disabled run IDs."
            }
            if ($role -eq "frame-correlation-index" -and @($index.frames).Count -eq 0) {
                throw "Strict upscaling frame-correlation-index must contain source-PTS to native-Present rows."
            }
            if ($role -eq "upscaling-model-pack-index" -and
                [string]$index.modelPackSha256 -ne [string]$details.modelPackSha256) {
                throw "Strict upscaling model-pack index does not match the certified model hash."
            }
            if ($role -in @("upscaling-model-pack-index", "upscaling-scenario-index") -and
                [string]$index.provider -ne [string]$details.provider) {
                throw "Strict upscaling artifact '$role' is not bound to certified provider '$($details.provider)'."
            }
            if ($role -eq "upscaling-scenario-index") {
                if ([string]$index.preset -ne [string]$details.preset -or
                    [string]$index.matrixId -ne [string]$details.upscalingMatrixId) {
                    throw "Strict upscaling scenario index must bind exactly its own preset and matrix ID."
                }
            }
        }

        $artifact | Add-Member -NotePropertyName runnerValidated -NotePropertyValue $true -Force
        $artifact | Add-Member -NotePropertyName validatedBytes -NotePropertyValue ([long](Get-Item -LiteralPath $artifactPath).Length) -Force
        Copy-ValidatedEvidenceFile -SourcePath $artifactPath -FileName $fileName -ExpectedSha256 $actualSha
        $validatedRoles.Add($role)
    }

    # Imported measurements are deliberately ignored. A hash-pinned tool on
    # the fixed runner must regenerate them from the validated raw artifacts.
    $measurementIndexName = "upscaling-measurement-$([guid]::NewGuid().ToString('N')).json"
    $measurementIndexPath = Join-Path $resolvedOutput $measurementIndexName
    & $normalizerPath `
        --schema-version ([string]$normalizerContract.requiredIndexSchema) `
        --manifest $ManifestPath `
        --output $measurementIndexPath
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $measurementIndexPath -PathType Leaf) -or
        (Get-Item -LiteralPath $measurementIndexPath).Length -le 0) {
        throw "Strict upscaling measurement normalizer failed to produce a non-empty index (exit=$LASTEXITCODE)."
    }
    try {
        $measurementIndex = Get-Content -LiteralPath $measurementIndexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "Strict upscaling measurement normalizer produced invalid JSON: $($_.Exception.Message)"
    }
    foreach ($field in @($normalizerContract.requiredBindings)) {
        $property = $measurementIndex.PSObject.Properties[[string]$field]
        if ($null -eq $property -or $null -eq $property.Value -or
            ($property.Value -is [string] -and [string]::IsNullOrWhiteSpace([string]$property.Value))) {
            throw "Strict upscaling measurement index is missing binding '$field'."
        }
    }
    if ([string]$measurementIndex.schemaVersion -ne [string]$normalizerContract.requiredIndexSchema -or
        [string]$measurementIndex.candidateSha -ne [string]$Manifest.candidateSha -or
        [string]$measurementIndex.environmentFingerprint -ne [string]$Manifest.environment.fingerprint -or
        [string]$measurementIndex.scenarioId -ne $scenarioId -or
        [string]$measurementIndex.matrixId -ne [string]$details.upscalingMatrixId -or
        [string]$measurementIndex.provider -ne [string]$details.provider -or
        [string]$measurementIndex.preset -ne [string]$details.preset -or
        [string]$measurementIndex.providerRuntimeVersion -ne [string]$details.providerRuntimeVersion -or
        [string]$measurementIndex.modelPackSha256 -ne [string]$details.modelPackSha256) {
        throw "Strict upscaling measurement index is not bound to this candidate, environment, matrix, scenario, provider, preset, runtime, and model pack."
    }
    $playbackFixtures = @($Manifest.fixtures | Where-Object {
        [string]$_.id -eq "PlaybackMedia-v1" -and [bool]$_.validated -and
        [string]$_.sha256 -match '^[0-9a-fA-F]{64}$'
    })
    if ($playbackFixtures.Count -ne 1 -or
        [string]$measurementIndex.playbackFixtureSha256 -ne [string]$playbackFixtures[0].sha256) {
        throw "Strict upscaling measurement index is not bound to exactly one validated PlaybackMedia-v1 hash."
    }
    try {
        $captureStartQpc = [long]$measurementIndex.captureStartQpc
        $captureEndQpc = [long]$measurementIndex.captureEndQpc
        $measurementProcessId = [long]$measurementIndex.processId
    } catch {
        throw "Strict upscaling measurement index has invalid PID/QPC bindings."
    }
    if ($measurementProcessId -le 0 -or $captureStartQpc -lt 0 -or $captureEndQpc -le $captureStartQpc) {
        throw "Strict upscaling measurement index requires a positive PID and a non-empty QPC capture window."
    }

    $sourceArtifactReferences = @($measurementIndex.sourceArtifacts)
    $sourceReferenceRoles = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($role in @($requiredRoles)) {
        $sourceMatches = @($sourceArtifactReferences | Where-Object { [string]$_.role -eq $role })
        $artifactMatches = @($Manifest.artifacts | Where-Object {
            [string]$_.role -eq $role -and [string]$_.scenarioId -eq $scenarioId -and
            [bool]$_.runnerValidated
        })
        if ($sourceMatches.Count -ne 1 -or $artifactMatches.Count -ne 1 -or
            [string]$sourceMatches[0].fileName -ne [string]$artifactMatches[0].fileName -or
            ([string]$sourceMatches[0].sha256).ToLowerInvariant() -ne
                ([string]$artifactMatches[0].sha256).ToLowerInvariant()) {
            throw "Strict upscaling measurement index does not bind validated raw role '$role' exactly once."
        }
        [void]$sourceReferenceRoles.Add($role)
    }
    if ($sourceArtifactReferences.Count -ne $sourceReferenceRoles.Count) {
        throw "Strict upscaling measurement index contains an unrecognized or duplicate raw artifact reference."
    }

    $effectiveWindows = @($measurementIndex.effectiveProfileWindows)
    if ($effectiveWindows.Count -eq 0) {
        throw "Strict upscaling measurement index must prove at least one effective-profile window."
    }
    foreach ($window in $effectiveWindows) {
        try {
            $windowStartQpc = [long]$window.startQpc
            $windowEndQpc = [long]$window.endQpc
        } catch {
            throw "Strict upscaling effective-profile window has invalid QPC values."
        }
        if ($windowStartQpc -lt $captureStartQpc -or $windowEndQpc -gt $captureEndQpc -or
            $windowEndQpc -le $windowStartQpc -or
            [string]$window.requestedProvider -ne [string]$details.provider -or
            [string]$window.effectiveProvider -ne [string]$details.provider -or
            [string]$window.requestedPreset -ne [string]$details.preset -or
            [string]$window.effectivePreset -ne [string]$details.preset -or
            [string]$window.requestedShaderSetSha256 -ne [string]$details.modelPackSha256 -or
            [string]$window.effectiveShaderSetSha256 -ne [string]$details.modelPackSha256 -or
            [bool]$window.fallbackObserved -or [bool]$window.downgradeObserved) {
            throw "Strict upscaling timed window changed effective provider, preset, shader set, or fallback state."
        }
    }

    $sloContract = Read-YanamiPerfJson -Path (Join-Path $workspace "perf\slo\slo-v1.json")
    $normalizedMetrics = New-Object System.Collections.Generic.List[object]
    $metricIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($metric in @($measurementIndex.metrics)) {
        $metricId = [string]$metric.id
        $definitions = @($sloContract.metrics | Where-Object {
            [string]$_.id -eq $metricId -and [string]$_.suite -eq "upscaling" -and
            -not ([string]$_.id).StartsWith("upscaling.hosted_smoke.", [System.StringComparison]::Ordinal)
        })
        if (-not $metricIds.Add($metricId) -or $definitions.Count -ne 1 -or
            [string]$metric.unit -ne [string]$definitions[0].unit -or
            @($metric.samples).Count -eq 0) {
            throw "Strict upscaling normalizer emitted an unknown, duplicate, unit-invalid, or empty metric '$metricId'."
        }
        [void](Get-YanamiStatistics -Samples @($metric.samples))
        $normalizedMetrics.Add([pscustomobject][ordered]@{
            id = $metricId
            unit = [string]$metric.unit
            samples = @($metric.samples | ForEach-Object { [double]$_ })
            attributes = [pscustomobject][ordered]@{
                evidence = [string]$definitions[0].evidence
                scenarioId = $scenarioId
                preset = [string]$details.preset
                rawDerived = $true
                normalizerSha256 = $normalizerSha
            }
            scenarioMeasurements = @([pscustomobject][ordered]@{
                scenarioId = $scenarioId
                preset = [string]$details.preset
                samples = @($metric.samples | ForEach-Object { [double]$_ })
            })
        })
    }
    $normalizedInvariants = New-Object System.Collections.Generic.List[object]
    $invariantIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($invariant in @($measurementIndex.invariants)) {
        $invariantId = [string]$invariant.id
        if ($invariantId -eq "upscaling.strict_matrix_complete") {
            throw "Strict matrix completeness is runner-generated and must not be supplied by the normalizer."
        }
        $definitions = @($sloContract.invariants | Where-Object {
            [string]$_.id -eq $invariantId -and [string]$_.suite -eq "upscaling" -and
            $_.requiredProfiles -contains [string]$Manifest.profile
        })
        if (-not $invariantIds.Add($invariantId) -or $definitions.Count -ne 1) {
            throw "Strict upscaling normalizer emitted an unknown or duplicate invariant '$invariantId'."
        }
        $normalizedInvariants.Add([pscustomobject][ordered]@{
            id = $invariantId
            passed = [bool]$invariant.passed
            details = [pscustomobject][ordered]@{
                evidence = [string]$definitions[0].evidence
                scenarioId = $scenarioId
                preset = [string]$details.preset
                rawDerived = $true
                normalizerSha256 = $normalizerSha
            }
        })
    }
    $Manifest.metrics = @($Manifest.metrics | Where-Object {
        [string]$_.id -notlike "upscaling.*" -or
        ([string]$_.id).StartsWith("upscaling.hosted_smoke.", [System.StringComparison]::Ordinal)
    }) + $normalizedMetrics.ToArray()
    $Manifest.invariants = @($Manifest.invariants | Where-Object {
        [string]$_.id -notlike "upscaling.*"
    }) + $normalizedInvariants.ToArray()

    $measurementIndexSha = (Get-FileHash -LiteralPath $measurementIndexPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $Manifest.artifacts += [pscustomobject][ordered]@{
        role = "upscaling-measurement-index"
        fileName = $measurementIndexName
        sha256 = $measurementIndexSha
        candidateSha = [string]$Manifest.candidateSha
        scenarioId = $scenarioId
        runnerValidated = $true
        validatedBytes = [long](Get-Item -LiteralPath $measurementIndexPath).Length
        collector = [pscustomobject][ordered]@{
            name = [string]$approvedNormalizer.name
            version = [string]$approvedNormalizer.version
            kind = [string]$approvedNormalizer.kind
            clockDomain = "QPC"
            executableSha256 = $normalizerSha
        }
    }
    $validatedRoles.Add("upscaling-measurement-index")
    $Manifest.environment | Add-Member -NotePropertyName upscalingStrictEvidenceValidation -NotePropertyValue ([pscustomobject][ordered]@{
        validator = "scripts/performance/run-gate.ps1"
        normalizerPath = $normalizerPath
        normalizerSha256 = $normalizerSha
        scenarioId = $scenarioId
        validatedAtUtc = [DateTime]::UtcNow.ToString("o")
        validatedRoles = $validatedRoles.ToArray()
    }) -Force
    if ($env:YANAMI_PERF_REFERENCE_MATCH -ne "1") {
        throw "Strict upscaling evidence requires the current fixed runner to attest YANAMI_PERF_REFERENCE_MATCH=1."
    }
    if ([string]$Manifest.environment.fingerprint -ne [string]$env:YANAMI_PERF_RUN_FINGERPRINT) {
        throw "Strict upscaling evidence fingerprint does not match the current fixed runner."
    }
    return $Manifest
}

function Assert-PlaybackFixtureEvidence {
    param([object[]]$Manifests)

    $playbackFixtures = @(
        foreach ($probeManifest in @($Manifests)) {
            foreach ($fixture in @($probeManifest.fixtures)) {
                if ([string]$fixture.id -eq "PlaybackMedia-v1") {
                    [pscustomobject]@{ runId = [string]$probeManifest.runId; fixture = $fixture }
                }
            }
        }
    )
    if ($playbackFixtures.Count -eq 0) { return }

    $contractPath = Join-Path $workspace "perf\fixtures\playback-media-v1.manifest.json"
    $contract = Read-YanamiPerfJson -Path $contractPath
    if ([string]$contract.state -ne "provisioned") {
        throw "PlaybackMedia-v1 evidence was supplied, but the fixed-media policy is '$($contract.state)'. A dedicated performance-policy change must pin every required asset hash before playback evidence can be accepted."
    }

    $expectedAssets = [ordered]@{}
    foreach ($asset in @($contract.requiredAssets)) {
        $assetId = [string]$asset.id
        $assetHash = ([string]$asset.sha256).ToLowerInvariant()
        if ([string]::IsNullOrWhiteSpace($assetId) -or $assetHash -notmatch '^[0-9a-f]{64}$') {
            throw "PlaybackMedia-v1 policy is provisioned but required asset '$assetId' does not have a valid pinned SHA-256."
        }
        if ($expectedAssets.Contains($assetId)) { throw "PlaybackMedia-v1 policy contains duplicate asset id '$assetId'." }
        $expectedAssets[$assetId] = $assetHash
    }
    if ($expectedAssets.Count -eq 0) { throw "PlaybackMedia-v1 policy contains no required assets." }

    foreach ($entry in $playbackFixtures) {
        $fixture = $entry.fixture
        if (-not [bool]$fixture.validated) {
            throw "Playback fixture reported by probe '$($entry.runId)' is not validated."
        }
        $actualAssets = @($fixture.details.assets)
        foreach ($expected in $expectedAssets.GetEnumerator()) {
            $matches = @($actualAssets | Where-Object { [string]$_.id -eq [string]$expected.Key })
            if ($matches.Count -ne 1) {
                throw "Playback fixture from probe '$($entry.runId)' must report exactly one hash for required asset '$($expected.Key)'."
            }
            $actualHash = ([string]$matches[0].sha256).ToLowerInvariant()
            if ($actualHash -ne [string]$expected.Value) {
                throw "Playback fixture hash mismatch for '$($expected.Key)' in probe '$($entry.runId)': expected $($expected.Value), got $actualHash."
            }
        }
        if ($null -ne $contract.fixtureSha256) {
            $expectedFixtureHash = ([string]$contract.fixtureSha256).ToLowerInvariant()
            $actualFixtureHash = ([string]$fixture.sha256).ToLowerInvariant()
            if ($expectedFixtureHash -notmatch '^[0-9a-f]{64}$' -or $actualFixtureHash -ne $expectedFixtureHash) {
                throw "Playback fixture aggregate hash mismatch in probe '$($entry.runId)'."
            }
        }
    }
}

function Get-PerfFixtureSignature {
    param([object]$Result)
    $entries = @(
        foreach ($fixture in @($Result.fixtures)) {
            $assets = @(
                foreach ($asset in @($fixture.details.assets)) {
                    "$([string]$asset.id):$(([string]$asset.sha256).ToLowerInvariant())"
                }
            ) | Sort-Object
            "$([string]$fixture.id)@$([string]$fixture.version):$(([string]$fixture.sha256).ToLowerInvariant()):$([bool]$fixture.validated):$($assets -join ',')"
        }
    ) | Sort-Object
    return $entries -join "|"
}

function Convert-PerfResultToRunManifest {
    param([Parameter(Mandatory = $true)][object]$Result)
    $metrics = @(
        foreach ($metric in @($Result.metrics)) {
            [pscustomobject][ordered]@{
                id = [string]$metric.id
                unit = [string]$metric.unit
                samples = @($metric.samples | ForEach-Object { [double]$_ })
                attributes = if ($null -ne $metric.PSObject.Properties["attributes"]) { $metric.attributes } else { [pscustomobject]@{} }
            }
        }
    )
    $invariants = @(
        foreach ($invariant in @($Result.invariants)) {
            [pscustomobject][ordered]@{
                id = [string]$invariant.id
                passed = [bool]$invariant.passed
                details = $invariant.details
            }
        }
    )
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        runId = [string]$Result.runId
        profile = [string]$Result.profile
        mode = [string]$Result.mode
        startedAtUtc = [string]$Result.generatedAtUtc
        finishedAtUtc = [string]$Result.generatedAtUtc
        candidateSha = [string]$Result.candidateSha
        baseSha = [string]$Result.baseSha
        environment = $Result.environment
        fixtures = @($Result.fixtures)
        artifacts = if ($null -ne $Result.PSObject.Properties["artifacts"]) {
            @($Result.artifacts)
        } else { @() }
        suites = @($Result.suites)
        metrics = $metrics
        invariants = $invariants
    }
}

function Resolve-AbabComparisonEvidence {
    param(
        [Parameter(Mandatory = $true)][string[]]$Paths,
        [Parameter(Mandatory = $true)][object]$Slo,
        [Parameter(Mandatory = $true)][object]$Policy,
        [Parameter(Mandatory = $true)][string[]]$RequestedSuites,
        [Parameter(Mandatory = $true)][string]$RequestedBaseSha,
        [Parameter(Mandatory = $true)][string]$RequestedCandidateSha
    )
    if ($Paths.Count -ne 4) { throw "A-B-A-B relative comparison requires exactly four ordered PerfResult paths; received $($Paths.Count)." }
    if ([string]$Policy.comparisonOrder -ne "A-B-A-B") { throw "The active performance policy does not authorize A-B-A-B comparison." }
    if ([string]::IsNullOrWhiteSpace($RequestedBaseSha) -or [string]::IsNullOrWhiteSpace($RequestedCandidateSha)) {
        throw "A-B-A-B relative comparison requires both -BaseSha and -CandidateSha."
    }

    $expectedRoles = @("A", "B", "A", "B")
    $expectedShas = @($RequestedBaseSha, $RequestedCandidateSha, $RequestedBaseSha, $RequestedCandidateSha)
    $results = New-Object System.Collections.Generic.List[object]
    $sourceManifests = New-Object System.Collections.Generic.List[object]
    $runIds = @{}
    $fingerprint = ""
    $suiteSignature = ""
    $fixtureSignature = ""
    $previousGeneratedAt = [DateTimeOffset]::MinValue

    for ($index = 0; $index -lt 4; $index++) {
        $path = [System.IO.Path]::GetFullPath($Paths[$index])
        Assert-JsonFileSchema -Path $path -SchemaPath $perfResultSchemaPath -Label "A-B-A-B $($expectedRoles[$index])$([math]::Floor($index / 2) + 1) PerfResult"
        $source = Read-YanamiPerfJson -Path $path
        if ([string]$source.profile -ne $Profile) { throw "A-B-A-B source '$path' uses profile '$($source.profile)', expected '$Profile'." }
        if ([string]$source.contractVersion -ne [string]$Slo.id) { throw "A-B-A-B source '$path' does not use active contract '$($Slo.id)'." }
        if ([string]$source.status -eq "infra-invalid") { throw "A-B-A-B source '$path' is infrastructure-invalid and cannot be comparison evidence." }
        if ([string]$source.candidateSha -ne $expectedShas[$index]) {
            throw "A-B-A-B source '$path' has candidate SHA '$($source.candidateSha)', expected '$($expectedShas[$index])' for role $($expectedRoles[$index])."
        }
        $runId = [string]$source.runId
        if ($runIds.ContainsKey($runId)) { throw "A-B-A-B evidence repeats runId '$runId'." }
        $runIds[$runId] = $true

        $generatedAt = [DateTimeOffset]::MinValue
        if (-not [DateTimeOffset]::TryParse([string]$source.generatedAtUtc, [ref]$generatedAt)) {
            throw "A-B-A-B source '$path' has an invalid generatedAtUtc."
        }
        if ($index -gt 0 -and $generatedAt -le $previousGeneratedAt) {
            throw "A-B-A-B PerfResults are not in strictly increasing completion order at role $($expectedRoles[$index])."
        }
        $previousGeneratedAt = $generatedAt

        $sourceFingerprint = [string]$source.environment.fingerprint
        if ([string]::IsNullOrWhiteSpace($sourceFingerprint)) { throw "A-B-A-B source '$path' has no machine fingerprint." }
        if ($index -eq 0) { $fingerprint = $sourceFingerprint }
        elseif ($sourceFingerprint -ne $fingerprint) { throw "A-B-A-B sources were not collected on the same machine fingerprint." }

        $sourceSuiteSignature = @($source.suites | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique) -join ","
        if ($index -eq 0) { $suiteSignature = $sourceSuiteSignature }
        elseif ($sourceSuiteSignature -ne $suiteSignature) { throw "A-B-A-B sources do not cover the same suite set." }
        foreach ($suite in $RequestedSuites) {
            if ($suite -notin @($source.suites | ForEach-Object { ([string]$_).ToLowerInvariant() })) {
                throw "A-B-A-B source '$path' does not contain requested suite '$suite'."
            }
        }

        $sourceFixtureSignature = Get-PerfFixtureSignature -Result $source
        if ($index -eq 0) { $fixtureSignature = $sourceFixtureSignature }
        elseif ($sourceFixtureSignature -ne $fixtureSignature) { throw "A-B-A-B sources do not use identical fixture versions and hashes." }
        $sourceManifest = Convert-PerfResultToRunManifest -Result $source
        $sourceEvidenceCheck = Invoke-YanamiPerfEvaluation -Manifest $sourceManifest -Slo $Slo -Policy $Policy -Mode enforce -Suites $RequestedSuites -CandidateSha $expectedShas[$index]
        if ($sourceEvidenceCheck.status -eq "infra-invalid") {
            throw "A-B-A-B source '$path' is incomplete or invalid under hard evidence rules: $(@($sourceEvidenceCheck.reasons) -join ' ')"
        }
        $results.Add($source)
        $sourceManifests.Add($sourceManifest)
    }

    $previousRunFingerprint = $env:YANAMI_PERF_RUN_FINGERPRINT
    try {
        # Merge-YanamiRunManifests normally stamps the current runner's unified
        # probe fingerprint. Imported A-B-A-B results must retain the machine
        # that actually produced their samples, even if comparison happens on
        # another host.
        $env:YANAMI_PERF_RUN_FINGERPRINT = $fingerprint
        $baseManifest = Merge-YanamiRunManifests -Manifests @(
            $sourceManifests[0],
            $sourceManifests[2]
        )
        $headManifest = Merge-YanamiRunManifests -Manifests @(
            $sourceManifests[1],
            $sourceManifests[3]
        )
        if ($null -ne $results[0].environment.PSObject.Properties["details"]) {
            $baseManifest.environment | Add-Member -NotePropertyName details -NotePropertyValue $results[0].environment.details -Force
        }
        if ($null -ne $results[1].environment.PSObject.Properties["details"]) {
            $headManifest.environment | Add-Member -NotePropertyName details -NotePropertyValue $results[1].environment.details -Force
        }
    }
    finally {
        $env:YANAMI_PERF_RUN_FINGERPRINT = $previousRunFingerprint
    }
    Assert-DanmakuFixtureEvidence -Manifests @($baseManifest, $headManifest)
    Assert-PlaybackFixtureEvidence -Manifests @($baseManifest, $headManifest)
    $baseResult = Invoke-YanamiPerfEvaluation -Manifest $baseManifest -Slo $Slo -Policy $Policy -Mode collect -Suites $RequestedSuites -CandidateSha $RequestedBaseSha
    if ($baseResult.status -eq "infra-invalid") {
        throw "A-B-A-B base aggregate is invalid: $(@($baseResult.reasons) -join ' ')"
    }
    return [pscustomobject][ordered]@{
        validated = $true
        sequence = "A-B-A-B"
        sourceRunIds = @($results | ForEach-Object { [string]$_.runId })
        sourceGeneratedAtUtc = @($results | ForEach-Object { [string]$_.generatedAtUtc })
        fingerprint = $fingerprint
        baseResult = $baseResult
        headManifest = $headManifest
    }
}

function Invoke-DesktopProbe {
    param([string]$Executable, [string]$Destination, [string]$ProbeMode, [string[]]$RequestedSuites)
    $oldSuites = $env:YANAMI_PERF_SUITES
    $oldWeekly = $env:YANAMI_PERF_WEEKLY
    try {
        if ($RequestedSuites.Count -gt 0) { $env:YANAMI_PERF_SUITES = $RequestedSuites -join "," }
        if ($Profile -eq "Weekly") { $env:YANAMI_PERF_WEEKLY = "1" }
        & $Executable --profile (Get-ProbeProfileName $Profile) --output $Destination --mode $ProbeMode 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "Desktop performance probe exited with code $LASTEXITCODE." }
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) { throw "Desktop performance probe did not write $Destination." }
    }
    finally {
        $env:YANAMI_PERF_SUITES = $oldSuites
        $env:YANAMI_PERF_WEEKLY = $oldWeekly
    }
}

function Invoke-DanmakuProbe {
    param(
        [string]$Executable,
        [string]$Destination,
        [string]$ProbeMode,
        [Parameter(Mandatory = $true)][object]$Fixture
    )

    $oldFixtureDirectory = $env:YANAMI_PERF_DANMAKU_FIXTURE_DIR
    $oldFixtureSha = $env:YANAMI_PERF_DANMAKU_SHA256
    $oldQpaPlatform = $env:QT_QPA_PLATFORM
    $oldQuickBackend = $env:QT_QUICK_BACKEND
    try {
        $env:YANAMI_PERF_DANMAKU_FIXTURE_DIR = [string]$Fixture.directory
        $env:YANAMI_PERF_DANMAKU_SHA256 = [string]$Fixture.sha256
        $env:QT_QPA_PLATFORM = "offscreen"
        $env:QT_QUICK_BACKEND = "software"
        & $Executable `
            --profile pull-request `
            --output $Destination `
            --mode $ProbeMode `
            --fixture-dir ([string]$Fixture.directory) 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Danmaku hosted performance probe exited with code $LASTEXITCODE."
        }
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
            throw "Danmaku hosted performance probe did not write $Destination."
        }
    }
    finally {
        $env:YANAMI_PERF_DANMAKU_FIXTURE_DIR = $oldFixtureDirectory
        $env:YANAMI_PERF_DANMAKU_SHA256 = $oldFixtureSha
        $env:QT_QPA_PLATFORM = $oldQpaPlatform
        $env:QT_QUICK_BACKEND = $oldQuickBackend
    }
}

function Invoke-UpscalingHostedProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ProbeMode,
        [Parameter(Mandatory = $true)][object]$Fixture
    )

    & $Executable `
        --fixture ([string]$Fixture.path) `
        --fixture-manifest ([string]$Fixture.manifestPath) `
        --output $Destination `
        --mode $ProbeMode 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "Native upscaling production probe exited with code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        throw "Native upscaling production probe did not write $Destination."
    }
}

function Invoke-RustComponentProbe {
    param([string]$Destination, [string]$ProbeMode, [string[]]$RequestedSuites)
    $oldSuites = $env:YANAMI_PERF_SUITES
    $oldWeekly = $env:YANAMI_PERF_WEEKLY
    $oldMode = $env:YANAMI_PERF_GATE_MODE
    $oldPath = $env:PATH
    $oldToolchain = $env:RUSTUP_TOOLCHAIN
    try {
        if ($RequestedSuites.Count -gt 0) { $env:YANAMI_PERF_SUITES = $RequestedSuites -join "," }
        if ($Profile -eq "Weekly") { $env:YANAMI_PERF_WEEKLY = "1" }
        $env:YANAMI_PERF_GATE_MODE = $ProbeMode
        $cargoCommand = Get-Command cargo -ErrorAction SilentlyContinue
        $cargoExecutable = if ($cargoCommand) { $cargoCommand.Source } else { Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe" }
        if (-not (Test-Path -LiteralPath $cargoExecutable -PathType Leaf)) { throw "Cargo is unavailable; cannot build yanami-performance-probe." }
        if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
            $ucrtCandidates = @("C:\msys64\ucrt64\bin", (Join-Path $env:USERPROFILE "scoop\apps\msys2\current\ucrt64\bin"))
            $ucrtBin = @($ucrtCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ "gcc.exe") } | Select-Object -First 1)
            if ($ucrtBin.Count -gt 0) { $env:PATH = "$($ucrtBin[0]);$env:PATH" }
            $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
        }
        & $cargoExecutable run --release --locked -p yanami-performance-probe -- --profile (Get-ProbeProfileName $Profile) --output $Destination --mode $ProbeMode 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "Rust component performance probe exited with code $LASTEXITCODE." }
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) { throw "Rust component performance probe did not write $Destination." }
    }
    finally {
        $env:YANAMI_PERF_SUITES = $oldSuites
        $env:YANAMI_PERF_WEEKLY = $oldWeekly
        $env:YANAMI_PERF_GATE_MODE = $oldMode
        $env:PATH = $oldPath
        $env:RUSTUP_TOOLCHAIN = $oldToolchain
    }
}

function Invoke-DesktopRuntimeTrace {
    param(
        [string]$Executable,
        [string]$TracePath,
        [string[]]$Arguments,
        [int]$TimeoutSeconds,
        [string]$CompletionMilestone = "startup_settled",
        [string]$RunId,
        [bool]$HostedSmoke = $false
    )
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Desktop executable does not exist: $Executable" }
    if ([string]::IsNullOrWhiteSpace($RunId)) { throw "Desktop runtime trace requires a non-empty runId." }
    if (Test-Path -LiteralPath $TracePath) { throw "Desktop runtime trace path already exists: $TracePath" }
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = [System.IO.Path]::GetFullPath($Executable)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    if ($HostedSmoke) {
        $startInfo.Environment["QT_QPA_PLATFORM"] = "offscreen"
        $startInfo.Environment["QT_QUICK_BACKEND"] = "software"
    }
    $startInfo.Environment["YANAMI_PERF_RUN_ID"] = $RunId
    $allArguments = @("--performance-trace", $TracePath) + @($Arguments)
    if ($null -ne $startInfo.ArgumentList) {
        foreach ($argument in $allArguments) { [void]$startInfo.ArgumentList.Add([string]$argument) }
    } else {
        $quoted = foreach ($argument in $allArguments) { '"' + ([string]$argument).Replace('"', '\"') + '"' }
        $startInfo.Arguments = $quoted -join " "
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Unable to start desktop runtime smoke process." }
    $processId = [long]$process.Id
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $completed = $false
    $exitCode = $null
    try {
        while ([DateTime]::UtcNow -lt $deadline) {
            if (-not $completed -and (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
                foreach ($line in Get-Content -LiteralPath $TracePath -Encoding UTF8 -ErrorAction SilentlyContinue) {
                    if ([string]::IsNullOrWhiteSpace($line)) { continue }
                    try { $event = $line | ConvertFrom-Json -ErrorAction Stop } catch { continue }
                    # A bootstrap entry point owns the launched PID while the
                    # forwarded desktop process owns the main trace. The
                    # unique runId/path pair prevents stale evidence here;
                    # launcher/child PID binding is verified against the
                    # bootstrap sidecar after both processes exit.
                    if ([string]$event.runId -eq $RunId -and
                        [string]$event.milestone -eq $CompletionMilestone) {
                        $completed = $true
                        break
                    }
                }
            }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds 100
        }
        if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) { throw "Desktop runtime smoke timed out without producing a trace." }
        if (-not $completed) {
            throw "Desktop runtime smoke did not produce the expected '$CompletionMilestone' completion for run '$RunId' and process '$processId'."
        }
        if (-not $process.HasExited) {
            throw "Desktop runtime smoke produced '$CompletionMilestone' but did not exit normally before the timeout."
        }
        $exitCode = [int]$process.ExitCode
        if ($exitCode -ne 0) {
            throw "Desktop runtime smoke produced '$CompletionMilestone' but exited with code $exitCode."
        }
    }
    finally {
        try {
            if (-not $process.HasExited) {
                $process.Kill()
                [void]$process.WaitForExit(5000)
            }
        } catch {
            # Preserve the original completion/exit failure if the process
            # races cleanup or refuses termination.
        }
        $process.Dispose()
    }
    return [pscustomobject][ordered]@{
        processId = $processId
        exitCode = $exitCode
        completionMilestone = $CompletionMilestone
    }
}

function New-ValidationResult {
    param([string]$EffectiveMode, [string[]]$RequestedSuites)
    if (-not $RequestedSuites -or $RequestedSuites.Count -eq 0) {
        $RequestedSuites = @(Get-YanamiPerfSuites)
    }
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"; contractVersion = "SLO-v1"; runId = "contract-validation-$([guid]::NewGuid().ToString('N'))"
        profile = $Profile; mode = $EffectiveMode; status = "pass"; generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        candidateSha = $CandidateSha; baseSha = $BaseSha
        environment = [pscustomobject]@{ fingerprint = "contract-only"; referenceMatch = $false }
        fixtures = @(); suites = @($RequestedSuites); metrics = @(); invariants = @()
        reasons = @("SLO, policy, schemas, and evaluator configuration are internally valid.")
    }
}

function Apply-RelativeEvidenceState {
    param(
        [Parameter(Mandatory = $true)][object]$GateResult,
        [AllowNull()][object]$BaseResult,
        [string]$BaseResultFile,
        [string]$EffectiveMode,
        [string]$RequestedBaseSha,
        [AllowNull()][object]$AbabEvidence
    )
    $baseFileSupplied = -not [string]::IsNullOrWhiteSpace($BaseResultFile)
    $sequenceValidated = $null -ne $AbabEvidence -and [bool]$AbabEvidence.validated -and [string]$AbabEvidence.sequence -eq "A-B-A-B"
    $baseEvidenceProperty = $GateResult.PSObject.Properties["baseComparisonEvidence"]
    $baseEvidenceInvalid = $null -ne $BaseResult -and $null -ne $baseEvidenceProperty -and -not [bool]$baseEvidenceProperty.Value.valid
    $comparedMetrics = @($GateResult.metrics | Where-Object { $null -ne $_.comparison }).Count
    if ($null -eq $BaseResult -or -not $sequenceValidated -or $baseEvidenceInvalid -or $comparedMetrics -eq 0) {
        $relativeReason = if ($baseEvidenceInvalid) {
            "The supplied base/head evidence failed the relative comparison contract; no relative metric comparison was evaluated."
        } elseif ($sequenceValidated -and $comparedMetrics -eq 0) {
            "The validated A-B-A-B sources produced zero comparable relative metrics; no relative gate was claimed."
        } elseif ($baseFileSupplied -and -not $sequenceValidated) {
            "A base PerfResult was supplied, but a base file alone is not A-B-A-B evidence. Provide four ordered PerfResults with -ComparisonResultPath."
        } elseif ($RequestedBaseSha) {
            "Relative comparison requested for base '$RequestedBaseSha', but four ordered A-B-A-B PerfResults were not supplied; no relative gate was claimed."
        } else {
            "Relative comparison was not requested; only absolute SLO and correctness gates were evaluated. For a Lab PR, supply four ordered A-B-A-B PerfResults."
        }
        $GateResult.reasons = @($GateResult.reasons) + @($relativeReason)
        if ($RequestedBaseSha -or $baseFileSupplied -or $null -ne $AbabEvidence) {
            if ($EffectiveMode -eq "collect") {
                if ($GateResult.status -eq "pass") { $GateResult.status = "debt" }
            } else {
                $GateResult.status = "infra-invalid"
            }
        }
        $GateResult | Add-Member -NotePropertyName relativeComparison -NotePropertyValue ([pscustomobject]@{
            state = "not-evaluated"
            requiredSequence = "A-B-A-B"
            evidenceKind = "four-ordered-perf-results"
            sourceRunIds = if ($null -ne $AbabEvidence) { @($AbabEvidence.sourceRunIds) } else { @() }
            baseSha = $RequestedBaseSha
            evidenceValid = if ($null -ne $baseEvidenceProperty) { [bool]$baseEvidenceProperty.Value.valid } else { $false }
            comparedMetrics = $comparedMetrics
        }) -Force
    } else {
        $GateResult | Add-Member -NotePropertyName relativeComparison -NotePropertyValue ([pscustomobject]@{
            state = "evaluated"
            requiredSequence = "A-B-A-B"
            evidenceKind = "four-ordered-perf-results"
            sourceRunIds = @($AbabEvidence.sourceRunIds)
            sourceGeneratedAtUtc = @($AbabEvidence.sourceGeneratedAtUtc)
            baseResultPath = [System.IO.Path]::GetFullPath($BaseResultFile)
            baseSha = $RequestedBaseSha
            evidenceValid = $true
            comparedMetrics = $comparedMetrics
        }) -Force
    }
    return $GateResult
}

try {
    Import-Module $modulePath -Force
    $slo = Read-YanamiPerfJson -Path $SloPath
    $policy = Read-YanamiPerfJson -Path $PolicyPath
    $configurationErrors = @(Test-YanamiPerfConfiguration -Slo $slo -Policy $policy)
    if ($configurationErrors.Count -gt 0) { throw "Performance contract validation failed: $($configurationErrors -join ' ')" }
    $profileModeOverride = if ($null -ne $policy.profileModeOverrides) {
        [string]$policy.profileModeOverrides.$Profile
    } else { "" }
    $effectiveMode = if ($Mode -eq "Auto") {
        if ([string]::IsNullOrWhiteSpace($profileModeOverride)) { [string]$policy.currentMode }
        else { $profileModeOverride.ToLowerInvariant() }
    } else { $Mode.ToLowerInvariant() }
    $requestedSuites = @(Get-RequestedSuites -Csv $Suites)
    foreach ($suite in $requestedSuites) {
        if ($suite -notin @(Get-YanamiPerfSuites)) { throw "Unknown performance suite '$suite'." }
    }

    if ($ValidateOnly) {
        $result = New-ValidationResult -EffectiveMode $effectiveMode -RequestedSuites $requestedSuites
    } else {
        $manifestPaths = New-Object System.Collections.Generic.List[string]
        $trustedProducerAttestations = New-Object System.Collections.Generic.List[object]
        $explicitManifestPaths = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        $desktopProbeUsed = $null
        $desktopProbeDiscoveryUsed = $null
        $danmakuProbeUsed = $null
        $danmakuProbeDiscoveryUsed = $null
        $upscalingProbeUsed = $null
        $rustProbeUsed = $false
        $ababEvidence = $null
        $originalBaseResultPath = $BaseResultPath
        $comparisonPaths = @($ComparisonResultPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $hadExplicitInput = @($InputPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -gt 0 -or $comparisonPaths.Count -gt 0
        $hadExplicitRetryInput = @($RetryInputPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -gt 0
        if (-not $hadExplicitInput -and -not $SkipProbeDiscovery -and
            -not [string]::IsNullOrWhiteSpace([string]$CandidateSha)) {
            $workspaceHeadSha = Get-WorkspaceHeadSha
            if ($workspaceHeadSha -eq "unavailable") {
                throw "The current workspace HEAD could not be resolved for local performance evidence."
            }
            if ($workspaceHeadSha -ne ([string]$CandidateSha).Trim().ToLowerInvariant()) {
                throw "The requested candidate SHA '$CandidateSha' does not match the current workspace HEAD '$workspaceHeadSha'."
            }
        }
        if ($comparisonPaths.Count -gt 0) {
            if (@($InputPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -gt 0 -or $BaseResultPath -or @($RetryInputPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -gt 0 -or $DesktopExecutable) {
                throw "-ComparisonResultPath cannot be combined with -InputPath, -BaseResultPath, -RetryInputPath, or -DesktopExecutable."
            }
            $ababEvidence = Resolve-AbabComparisonEvidence -Paths $comparisonPaths -Slo $slo -Policy $policy -RequestedSuites $requestedSuites -RequestedBaseSha $BaseSha -RequestedCandidateSha $CandidateSha
            $headManifestPath = Join-Path $resolvedOutput "abab-head-run-manifest.json"
            $ababEvidence.headManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $headManifestPath -Encoding UTF8
            $manifestPaths.Add($headManifestPath)
            $BaseResultPath = Join-Path $resolvedOutput "abab-base-result.json"
            $ababEvidence.baseResult | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $BaseResultPath -Encoding UTF8
        }
        foreach ($path in @($InputPath)) {
            if ([string]::IsNullOrWhiteSpace($path)) { continue }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Input run manifest does not exist: $path" }
            $fullInputPath = [System.IO.Path]::GetFullPath($path)
            $manifestPaths.Add($fullInputPath)
            [void]$explicitManifestPaths.Add($fullInputPath)
        }

        $fixtureSuitesRequested = $requestedSuites.Count -eq 0 -or "search" -in $requestedSuites -or "backend" -in $requestedSuites
        $danmakuRequested = $requestedSuites.Count -eq 0 -or "danmaku" -in $requestedSuites
        $upscalingRequested = $requestedSuites.Count -eq 0 -or "upscaling" -in $requestedSuites
        [void](Initialize-RunFingerprint)
        $f110k = if ($fixtureSuitesRequested) { Initialize-F110KFixture -RequestedDirectory $FixtureDirectory } else { $null }
        $danmakuFixture = if ($danmakuRequested) { Initialize-DanmakuFixture -RequestedDirectory $DanmakuFixtureDirectory } else { $null }
        $upscalingFixture = if ($upscalingRequested) { Initialize-UpscalingCapabilityFixture } else { $null }

        if ($manifestPaths.Count -eq 0 -and -not $SkipProbeDiscovery) {
            $desktopProbeDiscovery = Find-DesktopProbe -ExplicitPath $ProbePath
            $desktopProbe = if ($desktopProbeDiscovery) { [string]$desktopProbeDiscovery.path } else { $null }
            if ($desktopProbe) {
                $rebuiltDesktopProbe = Build-DesktopProbeBesideArtifact -DesktopProbePath $desktopProbe
                if ($rebuiltDesktopProbe) {
                    $desktopProbe = $rebuiltDesktopProbe
                    $desktopProbeDiscovery.source = "$($desktopProbeDiscovery.source):cmake-rebuilt"
                } elseif (-not [bool]$desktopProbeDiscovery.explicit) {
                    # An automatically discovered opaque binary has no current
                    # build provenance and must not satisfy this run.
                    $desktopProbe = $null
                    $desktopProbeDiscovery = $null
                } else {
                    $desktopProbeDiscovery.source = "$($desktopProbeDiscovery.source):explicit-prebuilt-artifact"
                }
            }
            if (-not $desktopProbe) {
                $desktopProbe = Build-DesktopProbeIfAvailable
                if ($desktopProbe) {
                    $desktopProbeDiscovery = [pscustomobject][ordered]@{
                        path = [System.IO.Path]::GetFullPath($desktopProbe)
                        source = "configured-and-built-current-run"
                        explicit = $false
                    }
                }
            }
            if ($desktopProbe) {
                $desktopOutput = Join-Path $resolvedOutput "desktop-run-manifest.json"
                Invoke-DesktopProbe -Executable $desktopProbe -Destination $desktopOutput -ProbeMode $effectiveMode -RequestedSuites $requestedSuites
                Add-ManifestArtifactEvidence `
                    -ManifestPath $desktopOutput `
                    -ArtifactPath $desktopProbe `
                    -Role "desktop-component-probe" `
                    -DiscoverySource ([string]$desktopProbeDiscovery.source) `
                    -TrustedProducerLedger $trustedProducerAttestations
                $manifestPaths.Add($desktopOutput)
                $desktopProbeUsed = $desktopProbe
                $desktopProbeDiscoveryUsed = $desktopProbeDiscovery
            }
            if ($Profile -eq "PullRequest" -and $danmakuRequested) {
                $danmakuProbeDiscovery = Find-DanmakuProbe -ExplicitPath $DanmakuProbePath
                $danmakuProbe = if ($danmakuProbeDiscovery) {
                    [string]$danmakuProbeDiscovery.path
                } else { $null }
                if ($danmakuProbe) {
                    $rebuiltDanmakuProbe = Build-DanmakuProbeBesideArtifact -ArtifactPath $danmakuProbe
                    if ($rebuiltDanmakuProbe) {
                        $danmakuProbe = $rebuiltDanmakuProbe
                        $danmakuProbeDiscovery.source = "$($danmakuProbeDiscovery.source):cmake-rebuilt"
                    } elseif (-not [bool]$danmakuProbeDiscovery.explicit) {
                        $danmakuProbe = $null
                        $danmakuProbeDiscovery = $null
                    } else {
                        $danmakuProbeDiscovery.source = "$($danmakuProbeDiscovery.source):explicit-prebuilt-artifact"
                    }
                }
                if (-not $danmakuProbe -and $desktopProbeUsed) {
                    $danmakuProbe = Build-DanmakuProbeBesideArtifact -ArtifactPath $desktopProbeUsed
                    if ($danmakuProbe) {
                        $danmakuProbeDiscovery = [pscustomobject][ordered]@{
                            path = [System.IO.Path]::GetFullPath($danmakuProbe)
                            source = "desktop-probe-build-tree:cmake-built-current-run"
                            explicit = $false
                        }
                    }
                }
                if ($danmakuProbe) {
                    $danmakuOutput = Join-Path $resolvedOutput "danmaku-hosted-run-manifest.json"
                    Invoke-DanmakuProbe `
                        -Executable $danmakuProbe `
                        -Destination $danmakuOutput `
                        -ProbeMode $effectiveMode `
                        -Fixture $danmakuFixture
                    Add-ManifestArtifactEvidence `
                        -ManifestPath $danmakuOutput `
                        -ArtifactPath $danmakuProbe `
                        -Role "danmaku-hosted-render-probe" `
                        -DiscoverySource ([string]$danmakuProbeDiscovery.source) `
                        -TrustedProducerLedger $trustedProducerAttestations
                    $manifestPaths.Add($danmakuOutput)
                    $danmakuProbeUsed = $danmakuProbe
                    $danmakuProbeDiscoveryUsed = $danmakuProbeDiscovery
                }
            }
            if ($Profile -eq "PullRequest" -and $upscalingRequested) {
                $upscalingProbe = if ($desktopProbeUsed) {
                    Build-UpscalingProbeBesideArtifact -ArtifactPath $desktopProbeUsed
                } else { $null }
                if (-not $upscalingProbe) {
                    foreach ($candidate in @(
                        (Join-Path $workspace "build\desktop-windows\yanami-upscaling-perf-probe.exe"),
                        (Join-Path $workspace "build\ci-windows\yanami-upscaling-perf-probe.exe"),
                        (Join-Path $workspace "build\performance-windows\yanami-upscaling-perf-probe.exe")
                    )) {
                        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                            $upscalingProbe = Build-UpscalingProbeBesideArtifact -ArtifactPath $candidate
                            if ($upscalingProbe) { break }
                        }
                    }
                }
                if (-not $upscalingProbe) {
                    throw "The native yanami-upscaling-perf-probe could not be built from the current checkout."
                }
                $upscalingOutput = Join-Path $resolvedOutput "upscaling-hosted-run-manifest.json"
                Invoke-UpscalingHostedProbe `
                    -Executable $upscalingProbe `
                    -Destination $upscalingOutput `
                    -ProbeMode $effectiveMode `
                    -Fixture $upscalingFixture
                Add-ManifestArtifactEvidence `
                    -ManifestPath $upscalingOutput `
                    -ArtifactPath $upscalingProbe `
                    -Role "native-upscaling-production-probe" `
                    -DiscoverySource "desktop-build-tree:cmake-built-current-run" `
                    -TrustedProducerLedger $trustedProducerAttestations
                $manifestPaths.Add($upscalingOutput)
                $upscalingProbeUsed = $upscalingProbe
            }
            # The Rust probe owns both the loopback backend observations and
            # the canonical production MediaCatalog Search component path.
            # A Search-only path classification must therefore launch it too.
            $rustProbeRequested = $requestedSuites.Count -eq 0 -or
                "backend" -in $requestedSuites -or "search" -in $requestedSuites
            if ($rustProbeRequested -and (Test-RustComponentProbePackageExists)) {
                $rustOutput = Join-Path $resolvedOutput "rust-component-run-manifest.json"
                Invoke-RustComponentProbe -Destination $rustOutput -ProbeMode $effectiveMode -RequestedSuites $requestedSuites
                $rustArtifactName = if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
                    "yanami-performance-probe.exe"
                } else { "yanami-performance-probe" }
                $rustArtifact = Join-Path $workspace "target\release\$rustArtifactName"
                Add-ManifestLocalProvenance `
                    -ManifestPath $rustOutput `
                    -ProbeKind "rust-production-component-probe" `
                    -DiscoverySource "cargo-release-current-run" `
                    -ArtifactPath $(if (Test-Path -LiteralPath $rustArtifact -PathType Leaf) { $rustArtifact } else { "" }) `
                    -TrustedProducerLedger $trustedProducerAttestations
                $manifestPaths.Add($rustOutput)
                $rustProbeUsed = $true
            }
        }

        $startupRequested = $requestedSuites.Count -eq 0 -or "startup" -in $requestedSuites
        $interactionRequested = $requestedSuites.Count -eq 0 -or "interaction" -in $requestedSuites
        $runtimeTraceRequested = $startupRequested -or $interactionRequested
        if (-not $DesktopExecutable -and -not $hadExplicitInput -and
            -not $SkipProbeDiscovery -and $runtimeTraceRequested) {
            if ($desktopProbeUsed) {
                $DesktopExecutable = Build-DesktopExecutableBesideProbe -DesktopProbePath $desktopProbeUsed
            }
        }

        if ($DesktopExecutable) {
            $traceRunId = "runtime-smoke-$([guid]::NewGuid().ToString('N'))"
            $tracePath = Join-Path $resolvedOutput "desktop-runtime-trace-$traceRunId.jsonl"
            $bootstrapTracePath = "$tracePath.bootstrap.jsonl"
            $runtimeArguments = @($DesktopArguments)
            if ("--performance-runtime-auto-exit" -notin $runtimeArguments) {
                $runtimeArguments += "--performance-runtime-auto-exit"
            }
            $completionMilestone = "startup_settled"
            if ($interactionRequested) {
                $completionMilestone = "interaction_probe_complete"
                if ("--performance-runtime-probe" -notin $runtimeArguments) {
                    $runtimeArguments += "--performance-runtime-probe"
                }
            }
            $traceProcess = Invoke-DesktopRuntimeTrace -Executable $DesktopExecutable -TracePath $tracePath -Arguments $runtimeArguments -TimeoutSeconds $StartupTimeoutSeconds -CompletionMilestone $completionMilestone -RunId $traceRunId -HostedSmoke ($Profile -eq "PullRequest" -and -not $UseNativeWindowForRuntimeTrace)
            Assert-JsonLinesSchema -Path $tracePath -SchemaPath $perfEventSchemaPath -Label "Desktop PerfEvent trace"
            $bootstrapTracePresent = Test-Path -LiteralPath $bootstrapTracePath -PathType Leaf
            if ($RequireBootstrapSidecar -and -not $bootstrapTracePresent) {
                throw "Bootstrap runtime smoke did not produce the required sidecar '$bootstrapTracePath'."
            }
            $bootstrapManifest = $null
            $expectedDesktopProcessId = [long]$traceProcess.processId
            if ($bootstrapTracePresent) {
                Assert-JsonLinesSchema -Path $bootstrapTracePath -SchemaPath $perfEventSchemaPath -Label "Bootstrap PerfEvent sidecar"
                $bootstrapManifest = Convert-YanamiBootstrapTraceToManifest `
                    -TracePath $bootstrapTracePath `
                    -Profile $Profile `
                    -RunId $traceRunId `
                    -ExpectedProcessId ([long]$traceProcess.processId) `
                    -Mode $effectiveMode `
                    -EnvironmentFingerprint $env:YANAMI_PERF_RUN_FINGERPRINT `
                    -ReferenceMatch ($env:YANAMI_PERF_REFERENCE_MATCH -eq "1")
                $bootstrapInvariant = @($bootstrapManifest.invariants |
                    Where-Object id -eq "startup.bootstrap_handoff_valid")[0]
                $sidecarChildProcessId = [long]$bootstrapInvariant.details.childProcessId
                if ($sidecarChildProcessId -gt 0) {
                    $expectedDesktopProcessId = $sidecarChildProcessId
                }
            }
            $traceManifest = Convert-YanamiTraceToManifest `
                -TracePath $tracePath `
                -Profile $Profile `
                -RunId $traceRunId `
                -ExpectedProcessId $expectedDesktopProcessId `
                -Mode $effectiveMode `
                -EnvironmentFingerprint $env:YANAMI_PERF_RUN_FINGERPRINT `
                -ReferenceMatch ($env:YANAMI_PERF_REFERENCE_MATCH -eq "1") `
                -RequireBootstrapHandshake:$bootstrapTracePresent
            if ($bootstrapManifest) {
                $traceManifest = Merge-YanamiRunManifests -Manifests @(
                    $traceManifest,
                    $bootstrapManifest
                )
            }
            $traceManifest = Set-LocalManifestProvenance `
                -Manifest $traceManifest `
                -ProbeKind "desktop-runtime" `
                -DiscoverySource $(if ($DesktopExecutable -and $desktopProbeDiscoveryUsed) {
                    "$([string]$desktopProbeDiscoveryUsed.source):desktop-cmake-build"
                } elseif ($DesktopExecutable) { "explicit-desktop-executable" } else { "unavailable" }) `
                -ArtifactPath $DesktopExecutable `
                -TrustedProducerLedger $trustedProducerAttestations
            $runtimeArtifacts = New-Object System.Collections.Generic.List[object]
            $runtimeArtifacts.Add([pscustomobject][ordered]@{
                    role = "desktop-runtime-entrypoint"
                    fileName = [System.IO.Path]::GetFileName($DesktopExecutable)
                    sha256 = (Get-FileHash -LiteralPath $DesktopExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
                    candidateSha = [string]$traceManifest.candidateSha
                    processId = [long]$traceProcess.processId
                    processExitCode = [int]$traceProcess.exitCode
                    completionMilestone = [string]$traceProcess.completionMilestone
                })
            $desktopTraceFile = Get-Item -LiteralPath $tracePath
            $runtimeArtifacts.Add([pscustomobject][ordered]@{
                    role = "desktop-runtime-trace"
                    fileName = $desktopTraceFile.Name
                    sha256 = (Get-FileHash -LiteralPath $desktopTraceFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                    runnerValidated = $true
                    validatedBytes = [long]$desktopTraceFile.Length
                    processId = $expectedDesktopProcessId
                })
            if ($bootstrapTracePresent) {
                $bootstrapTraceFile = Get-Item -LiteralPath $bootstrapTracePath
                $runtimeArtifacts.Add([pscustomobject][ordered]@{
                        role = "bootstrap-runtime-trace"
                        fileName = $bootstrapTraceFile.Name
                        sha256 = (Get-FileHash -LiteralPath $bootstrapTraceFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                        runnerValidated = $true
                        validatedBytes = [long]$bootstrapTraceFile.Length
                        processId = [long]$traceProcess.processId
                    })
            }
            $traceManifest | Add-Member -NotePropertyName artifacts -NotePropertyValue $runtimeArtifacts.ToArray() -Force
            $traceManifestPath = Join-Path $resolvedOutput "runtime-smoke-run-manifest.json"
            $traceManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $traceManifestPath -Encoding UTF8
            Assert-JsonFileSchema -Path $traceManifestPath -SchemaPath $runManifestSchemaPath -Label "Runtime smoke run manifest"
            $manifestPaths.Add($traceManifestPath)
        }

        if ($manifestPaths.Count -eq 0) {
            $missingStatus = [string]$policy.missingProbeStatus.$Profile
            $fallbackSuites = if ($requestedSuites.Count -gt 0) { $requestedSuites } else { @(Get-YanamiPerfSuites) }
            $result = New-YanamiUnavailableResult -Profile $Profile -Status $missingStatus -Suites $fallbackSuites -Reason "No performance probe or run manifest was available. Contract validation succeeded, but product performance was not measured." -Mode $effectiveMode -CandidateSha $CandidateSha -BaseSha $BaseSha
        } else {
            $manifests = @(
                foreach ($manifestPath in $manifestPaths) {
                    Assert-JsonFileSchema -Path $manifestPath -SchemaPath $runManifestSchemaPath -Label "Run manifest '$manifestPath'"
                    $loadedManifest = Read-YanamiPerfJson -Path $manifestPath
                    if ($explicitManifestPaths.Contains([System.IO.Path]::GetFullPath($manifestPath))) {
                        Assert-ImportedManifestCandidate `
                            -Manifest $loadedManifest `
                            -ManifestPath $manifestPath `
                            -ExpectedCandidateSha $CandidateSha
                    }
                    $loadedManifest = Assert-DanmakuStrictEvidenceArtifacts `
                        -Manifest $loadedManifest `
                        -ManifestPath $manifestPath
                    $loadedManifest = Assert-UpscalingStrictEvidenceArtifacts `
                        -Manifest $loadedManifest `
                        -ManifestPath $manifestPath
                    Set-RunnerObservationProducerRunIds -Manifest $loadedManifest
                }
            )
            if (-not $hadExplicitInput) {
                foreach ($probeManifest in $manifests) {
                    if ([string]$probeManifest.environment.fingerprint -ne [string]$env:YANAMI_PERF_RUN_FINGERPRINT) {
                        throw "Locally executed probe '$($probeManifest.runId)' reported a fingerprint that differs from the current runner host."
                    }
                }
            }
            if ($null -ne $f110k) {
                foreach ($probeManifest in $manifests) {
                    foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "F110K-v1" })) {
                        if ([string]$fixture.sha256 -ne [string]$f110k.sha256) { throw "Probe '$($probeManifest.runId)' did not report the validated F110K-v1 hash." }
                    }
                }
            }
            if ($null -ne $danmakuFixture) {
                foreach ($probeManifest in $manifests) {
                    foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "DanmakuDensity-v1" })) {
                        if ([string]$fixture.sha256 -ne [string]$danmakuFixture.sha256) { throw "Probe '$($probeManifest.runId)' did not report the validated DanmakuDensity-v1 hash." }
                    }
                }
            }
            if ($null -ne $upscalingFixture) {
                foreach ($probeManifest in $manifests) {
                    foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "UpscalingCapability-v1" })) {
                        if ([string]$fixture.sha256 -ne [string]$upscalingFixture.sha256) {
                            throw "Probe '$($probeManifest.runId)' did not report the validated UpscalingCapability-v1 hash."
                        }
                    }
                }
            }
            Assert-DanmakuFixtureEvidence -Manifests $manifests
            Assert-PlaybackFixtureEvidence -Manifests $manifests
            Assert-UpscalingFixtureEvidence `
                -Manifests $manifests `
                -StrictRequired ($requestedSuites.Count -gt 0 -and "upscaling" -in $requestedSuites -and $Profile -ne "PullRequest")
            $manifest = if ($manifests.Count -eq 1) { $manifests[0] } else { Merge-YanamiRunManifests -Manifests $manifests }
            if ($null -ne $ababEvidence) {
                $manifest.environment | Add-Member -NotePropertyName comparisonEvaluator -NotePropertyValue $script:runEnvironmentDetails -Force
            } elseif ($hadExplicitInput) {
                $manifest.environment | Add-Member -NotePropertyName evaluator -NotePropertyValue $script:runEnvironmentDetails -Force
            } else {
                $manifest.environment | Add-Member -NotePropertyName details -NotePropertyValue $script:runEnvironmentDetails -Force
            }
            if ($manifest.profile -ne $Profile) { throw "Run manifest profile '$($manifest.profile)' does not match requested profile '$Profile'." }
            $baseResult = if ($BaseResultPath) { Assert-JsonFileSchema -Path $BaseResultPath -SchemaPath $perfResultSchemaPath -Label "Base PerfResult"; Read-YanamiPerfJson -Path $BaseResultPath } else { $null }
            $comparisonBaseResult = if ($null -ne $baseResult -and $null -ne $ababEvidence -and [bool]$ababEvidence.validated) { $baseResult } else { $null }
            $evaluationSuites = if ($requestedSuites.Count -gt 0) {
                $requestedSuites
            } else {
                @(Get-YanamiPerfSuites)
            }
            $result = Invoke-YanamiPerfEvaluation -Manifest $manifest -Slo $slo -Policy $policy -BaseResult $comparisonBaseResult -Mode $effectiveMode -Suites $evaluationSuites -CandidateSha $CandidateSha -BaseSha $BaseSha -TrustedProducerAttestations $trustedProducerAttestations.ToArray()
            $hardFailure = $result.status -eq "infra-invalid" -or @($result.invariants | Where-Object { $_.status -eq "fail" }).Count -gt 0 -or @($result.metrics | Where-Object { $_.status -eq "fail" -and $_.category -eq "correctness" }).Count -gt 0
            $continuousFailure = @($result.metrics | Where-Object { $_.status -in @("fail", "debt") -and $_.category -ne "correctness" }).Count -gt 0
            $effectiveRetryPaths = @($RetryInputPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
            if ($effectiveRetryPaths.Count -eq 0 -and $continuousFailure -and -not $hardFailure -and -not $hadExplicitInput -and ($desktopProbeUsed -or $danmakuProbeUsed -or $upscalingProbeUsed -or $rustProbeUsed)) {
                if ($desktopProbeUsed) {
                    $desktopRetryOutput = Join-Path $resolvedOutput "desktop-run-manifest-retry.json"
                    Invoke-DesktopProbe -Executable $desktopProbeUsed -Destination $desktopRetryOutput -ProbeMode $effectiveMode -RequestedSuites $requestedSuites
                    Add-ManifestArtifactEvidence `
                        -ManifestPath $desktopRetryOutput `
                        -ArtifactPath $desktopProbeUsed `
                        -Role "desktop-component-probe" `
                        -DiscoverySource ([string]$desktopProbeDiscoveryUsed.source) `
                        -TrustedProducerLedger $trustedProducerAttestations
                    $effectiveRetryPaths += $desktopRetryOutput
                }
                if ($danmakuProbeUsed) {
                    $danmakuRetryOutput = Join-Path $resolvedOutput "danmaku-hosted-run-manifest-retry.json"
                    Invoke-DanmakuProbe `
                        -Executable $danmakuProbeUsed `
                        -Destination $danmakuRetryOutput `
                        -ProbeMode $effectiveMode `
                        -Fixture $danmakuFixture
                    Add-ManifestArtifactEvidence `
                        -ManifestPath $danmakuRetryOutput `
                        -ArtifactPath $danmakuProbeUsed `
                        -Role "danmaku-hosted-render-probe" `
                        -DiscoverySource ([string]$danmakuProbeDiscoveryUsed.source) `
                        -TrustedProducerLedger $trustedProducerAttestations
                    $effectiveRetryPaths += $danmakuRetryOutput
                }
                if ($upscalingProbeUsed) {
                    $upscalingRetryOutput = Join-Path $resolvedOutput "upscaling-hosted-run-manifest-retry.json"
                    Invoke-UpscalingHostedProbe `
                        -Executable $upscalingProbeUsed `
                        -Destination $upscalingRetryOutput `
                        -ProbeMode $effectiveMode `
                        -Fixture $upscalingFixture
                    Add-ManifestArtifactEvidence `
                        -ManifestPath $upscalingRetryOutput `
                        -ArtifactPath $upscalingProbeUsed `
                        -Role "native-upscaling-production-probe" `
                        -DiscoverySource "desktop-build-tree:cmake-built-current-run:retry" `
                        -TrustedProducerLedger $trustedProducerAttestations
                    $effectiveRetryPaths += $upscalingRetryOutput
                }
                if ($rustProbeUsed) {
                    $rustRetryOutput = Join-Path $resolvedOutput "rust-component-run-manifest-retry.json"
                    Invoke-RustComponentProbe -Destination $rustRetryOutput -ProbeMode $effectiveMode -RequestedSuites $requestedSuites
                    $rustArtifactName = if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
                        "yanami-performance-probe.exe"
                    } else { "yanami-performance-probe" }
                    $rustArtifact = Join-Path $workspace "target\release\$rustArtifactName"
                    Add-ManifestLocalProvenance `
                        -ManifestPath $rustRetryOutput `
                        -ProbeKind "rust-production-component-probe" `
                        -DiscoverySource "cargo-release-current-run" `
                        -ArtifactPath $(if (Test-Path -LiteralPath $rustArtifact -PathType Leaf) { $rustArtifact } else { "" }) `
                        -TrustedProducerLedger $trustedProducerAttestations
                    $effectiveRetryPaths += $rustRetryOutput
                }
            }
            if ($effectiveRetryPaths.Count -gt 0 -and $continuousFailure -and -not $hardFailure) {
                $retryManifests = @(
                    foreach ($retryManifestPath in $effectiveRetryPaths) {
                        Assert-JsonFileSchema -Path $retryManifestPath -SchemaPath $runManifestSchemaPath -Label "Retry run manifest '$retryManifestPath'"
                        $loadedRetryManifest = Read-YanamiPerfJson -Path $retryManifestPath
                        if ($hadExplicitRetryInput) {
                            Assert-ImportedManifestCandidate `
                                -Manifest $loadedRetryManifest `
                                -ManifestPath $retryManifestPath `
                                -ExpectedCandidateSha $CandidateSha
                        }
                        $loadedRetryManifest = Assert-DanmakuStrictEvidenceArtifacts `
                            -Manifest $loadedRetryManifest `
                            -ManifestPath $retryManifestPath
                        $loadedRetryManifest = Assert-UpscalingStrictEvidenceArtifacts `
                            -Manifest $loadedRetryManifest `
                            -ManifestPath $retryManifestPath
                        Set-RunnerObservationProducerRunIds -Manifest $loadedRetryManifest
                    }
                )
                if (-not $hadExplicitRetryInput) {
                    foreach ($probeManifest in $retryManifests) {
                        if ([string]$probeManifest.environment.fingerprint -ne [string]$env:YANAMI_PERF_RUN_FINGERPRINT) {
                            throw "Locally executed retry probe '$($probeManifest.runId)' reported a fingerprint that differs from the current runner host."
                        }
                    }
                }
                if ($null -ne $f110k) {
                    foreach ($probeManifest in $retryManifests) {
                        foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "F110K-v1" })) {
                            if ([string]$fixture.sha256 -ne [string]$f110k.sha256) { throw "Retry probe '$($probeManifest.runId)' did not report the validated F110K-v1 hash." }
                        }
                    }
                }
                if ($null -ne $danmakuFixture) {
                    foreach ($probeManifest in $retryManifests) {
                        foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "DanmakuDensity-v1" })) {
                            if ([string]$fixture.sha256 -ne [string]$danmakuFixture.sha256) { throw "Retry probe '$($probeManifest.runId)' did not report the validated DanmakuDensity-v1 hash." }
                        }
                    }
                }
                if ($null -ne $upscalingFixture) {
                    foreach ($probeManifest in $retryManifests) {
                        foreach ($fixture in @($probeManifest.fixtures | Where-Object { [string]$_.id -eq "UpscalingCapability-v1" })) {
                            if ([string]$fixture.sha256 -ne [string]$upscalingFixture.sha256) {
                                throw "Retry probe '$($probeManifest.runId)' did not report the validated UpscalingCapability-v1 hash."
                            }
                        }
                    }
                }
                Assert-DanmakuFixtureEvidence -Manifests $retryManifests
                Assert-PlaybackFixtureEvidence -Manifests $retryManifests
                Assert-UpscalingFixtureEvidence `
                    -Manifests $retryManifests `
                    -StrictRequired ($requestedSuites.Count -gt 0 -and "upscaling" -in $requestedSuites -and $Profile -ne "PullRequest")
                $retryManifest = if ($retryManifests.Count -eq 1) { $retryManifests[0] } else { Merge-YanamiRunManifests -Manifests $retryManifests }
                $retryCandidateSha = if ($null -ne $retryManifest.PSObject.Properties["candidateSha"]) {
                    [string]$retryManifest.candidateSha
                } else { "" }
                $firstAttemptCandidateSha = if ($null -ne $manifest.PSObject.Properties["candidateSha"]) {
                    [string]$manifest.candidateSha
                } else { "" }
                if ($retryCandidateSha -ne $firstAttemptCandidateSha) {
                    throw "Retry evidence candidate SHA must match the first attempt candidate SHA."
                }
                if ($hadExplicitRetryInput) {
                    $retryManifest.environment | Add-Member -NotePropertyName evaluator -NotePropertyValue $script:runEnvironmentDetails -Force
                } else {
                    $retryManifest.environment | Add-Member -NotePropertyName details -NotePropertyValue $script:runEnvironmentDetails -Force
                }
                $retryResult = Invoke-YanamiPerfEvaluation -Manifest $retryManifest -Slo $slo -Policy $policy -BaseResult $comparisonBaseResult -Mode $effectiveMode -Suites $evaluationSuites -CandidateSha $CandidateSha -BaseSha $BaseSha -TrustedProducerAttestations $trustedProducerAttestations.ToArray()
                $firstAttempt = [pscustomobject]@{ runId = $result.runId; status = $result.status; reasons = @($result.reasons) }
                $secondAttempt = [pscustomobject]@{ runId = $retryResult.runId; status = $retryResult.status; reasons = @($retryResult.reasons) }
                if ($retryResult.status -eq "pass") {
                    $retryResult.reasons = @($retryResult.reasons) + @("The first continuous-metric failure was not reproduced; product failure requires two failing attempts.")
                }
                $result = $retryResult
                $result | Add-Member -NotePropertyName attempts -NotePropertyValue @($firstAttempt, $secondAttempt) -Force
            }
            $result = Apply-RelativeEvidenceState -GateResult $result -BaseResult $comparisonBaseResult -BaseResultFile $BaseResultPath -EffectiveMode $effectiveMode -RequestedBaseSha $BaseSha -AbabEvidence $ababEvidence
            $commandParts = @(".\scripts\performance\run-gate.ps1", "-Profile", $Profile, "-OutputDirectory", "<artifact-dir>")
            if ($Suites) { $commandParts += @("-Suites", $Suites) }
            if ($originalBaseResultPath) { $commandParts += @("-BaseResultPath", $originalBaseResultPath) }
            if ($comparisonPaths.Count -gt 0) { $commandParts += @("-ComparisonResultPath", "<A1>,<B1>,<A2>,<B2>") }
            $result | Add-Member -NotePropertyName reproductionCommand -NotePropertyValue ($commandParts -join " ") -Force
        }
    }
}
catch {
    $message = $_.Exception.Message
    if (Get-Command New-YanamiUnavailableResult -ErrorAction SilentlyContinue) {
        $fallbackMode = if ($Mode -eq "Auto") {
            if ($policy -and $policy.profileModeOverrides -and $policy.profileModeOverrides.$Profile) {
                ([string]$policy.profileModeOverrides.$Profile).ToLowerInvariant()
            } elseif ($policy) { [string]$policy.currentMode }
            else { "collect" }
        } else { $Mode.ToLowerInvariant() }
        $knownSuites = @(Get-YanamiPerfSuites)
        $fallbackSuites = @(Get-RequestedSuites -Csv $Suites | Where-Object { $_ -in $knownSuites })
        if ($fallbackSuites.Count -eq 0) { $fallbackSuites = $knownSuites }
        $result = New-YanamiUnavailableResult -Profile $Profile -Status "infra-invalid" -Suites $fallbackSuites -Reason $message -Mode $fallbackMode -CandidateSha $CandidateSha -BaseSha $BaseSha
    } else {
        $result = [pscustomobject][ordered]@{
            schemaVersion = "1.0"; contractVersion = "SLO-v1"; runId = "infra-$([guid]::NewGuid().ToString('N'))"
            profile = $Profile; mode = "collect"; status = "infra-invalid"; generatedAtUtc = [DateTime]::UtcNow.ToString("o")
            candidateSha = $CandidateSha; baseSha = $BaseSha; environment = [pscustomobject]@{ fingerprint = "unavailable"; referenceMatch = $false }
            fixtures = @(); suites = @("search", "backend", "interaction", "playback", "danmaku", "upscaling", "startup"); metrics = @(); invariants = @(); reasons = @($message)
        }
    }
    if ($BaseSha -or $BaseResultPath -or @($ComparisonResultPath).Count -gt 0) {
        $result | Add-Member -NotePropertyName relativeComparison -NotePropertyValue ([pscustomobject]@{
            state = "not-evaluated"
            requiredSequence = "A-B-A-B"
            evidenceKind = "four-ordered-perf-results"
            sourceRunIds = @()
            baseSha = $BaseSha
            evidenceValid = $false
            comparedMetrics = 0
        }) -Force
    }
}
finally {
    if ($null -eq $result) {
        $result = [pscustomobject][ordered]@{
            schemaVersion = "1.0"; contractVersion = "SLO-v1"; runId = "infra-$([guid]::NewGuid().ToString('N'))"
            profile = $Profile; mode = "collect"; status = "infra-invalid"; generatedAtUtc = [DateTime]::UtcNow.ToString("o")
            candidateSha = $CandidateSha; baseSha = $BaseSha; environment = [pscustomobject]@{ fingerprint = "unavailable"; referenceMatch = $false }
            fixtures = @(); suites = @("search", "backend", "interaction", "playback", "danmaku", "upscaling", "startup"); metrics = @(); invariants = @(); reasons = @("Runner ended without a result.")
        }
    }
    try {
        $resultJson = $result | ConvertTo-Json -Depth 100
        Assert-JsonSchemaText -Json $resultJson -SchemaPath $perfResultSchemaPath -Label "Final PerfResult"
        $resultJson | Set-Content -LiteralPath $jsonOutput -Encoding UTF8
    }
    catch {
        $schemaFailure = "Final PerfResult contract validation failed: $($_.Exception.Message)"
        if (Get-Command New-YanamiUnavailableResult -ErrorAction SilentlyContinue) {
            $knownSuites = @(Get-YanamiPerfSuites)
            $fallbackSuites = @(Get-RequestedSuites -Csv $Suites | Where-Object { $_ -in $knownSuites })
            if ($fallbackSuites.Count -eq 0) { $fallbackSuites = $knownSuites }
            $result = New-YanamiUnavailableResult -Profile $Profile -Status "infra-invalid" -Suites $fallbackSuites -Reason $schemaFailure -Mode "collect" -CandidateSha $CandidateSha -BaseSha $BaseSha
        } else {
            $result = [pscustomobject][ordered]@{
                schemaVersion = "1.0"; contractVersion = "SLO-v1"; runId = "infra-$([guid]::NewGuid().ToString('N'))"
                profile = $Profile; mode = "collect"; status = "infra-invalid"; generatedAtUtc = [DateTime]::UtcNow.ToString("o")
                candidateSha = $CandidateSha; baseSha = $BaseSha; environment = [pscustomobject]@{ fingerprint = "unavailable"; referenceMatch = $false }
                fixtures = @(); suites = @(); metrics = @(); invariants = @(); reasons = @($schemaFailure)
            }
        }
        ($result | ConvertTo-Json -Depth 100) | Set-Content -LiteralPath $jsonOutput -Encoding UTF8
    }
    try { Write-YanamiJUnit -Result $result -Path $junitOutput }
    catch { '<testsuite name="Yanami.Performance" tests="1" errors="1"><testcase name="runner"><error message="Unable to write detailed JUnit output."/></testcase></testsuite>' | Set-Content -LiteralPath $junitOutput -Encoding UTF8 }
    $summary = @(
        "# Yanami Performance Gate",
        "",
        "- Profile: $Profile",
        "- Mode: $($result.mode)",
        "- Status: $($result.status)",
        "- Metrics: $(@($result.metrics).Count)",
        "- Invariants: $(@($result.invariants).Count)",
        "",
        "## Reasons",
        ""
    ) + @(if (@($result.reasons).Count -gt 0) { $result.reasons | ForEach-Object { "- $_" } } else { "- None" })
    $summary | Set-Content -LiteralPath $summaryOutput -Encoding UTF8
    $exitCode = if ($policy -and $policy.exitCodes.PSObject.Properties[$result.status]) { [int]$policy.exitCodes.PSObject.Properties[$result.status].Value } else { switch ($result.status) { "pass" { 0 } "debt" { 0 } "fail" { 1 } default { 2 } } }
}

Write-Host "Yanami performance gate: $($result.status) ($Profile/$($result.mode))"
Write-Host "JSON: $jsonOutput"
Write-Host "JUnit: $junitOutput"
if ($NoExit) { Write-Output $result; return }
exit $exitCode
