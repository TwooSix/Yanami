[CmdletBinding()]
param(
    [string]$PackageRoot,
    [string]$LabRoot,
    [switch]$RefreshPackage,
    [switch]$Trace,
    [switch]$PrepareOnly,
    [switch]$ForcePreviousLab,
    [ValidateRange(1, 20)]
    [int]$RetainRuns = 2
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$workspace = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $workspace "build\desktop-windows"
if (-not $PackageRoot) {
    $PackageRoot = Join-Path $buildDirectory "pr-install-smoke\Release"
}
if (-not $LabRoot) {
    $LabRoot = Join-Path ([System.IO.Path]::GetTempPath()) "Yanami-ColdStartLab"
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-PathInside {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Candidate
    )
    $rootPath = (Get-FullPath $Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $candidatePath = Get-FullPath $Candidate
    return $candidatePath.StartsWith(
        $rootPath,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeLabRoot {
    param([Parameter(Mandatory = $true)][string]$Path)
    $fullPath = Get-FullPath $Path
    $driveRoot = [System.IO.Path]::GetPathRoot($fullPath)
    $tempRoot = (Get-FullPath ([System.IO.Path]::GetTempPath())).TrimEnd('\', '/')
    if ($fullPath.TrimEnd('\', '/') -ieq $driveRoot.TrimEnd('\', '/') -or
        $fullPath.TrimEnd('\', '/') -ieq (Get-FullPath $workspace).TrimEnd('\', '/') -or
        $fullPath.TrimEnd('\', '/') -ieq $tempRoot) {
        throw "Unsafe cold-start lab root: $fullPath"
    }
    return $fullPath
}

function Initialize-LabRoot {
    param([Parameter(Mandatory = $true)][string]$Path)
    $marker = Join-Path $Path ".yanami-cold-start-lab.json"
    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
            $firstEntry = Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1
            if ($firstEntry) {
                throw "The lab root is not empty and has no Yanami ownership marker: $Path"
            }
        }
    } else {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        [ordered]@{
            schemaVersion = "1.0"
            owner = "Yanami cold-start lab"
            createdUtc = [DateTime]::UtcNow.ToString("o")
        } | ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding UTF8
    }
}

function Get-YanamiProcesses {
    $results = @()
    foreach ($process in @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessName -ieq "Yanami" -or $_.ProcessName -ieq "yanami-desktop"
    })) {
        $path = $null
        $startedUtc = $null
        try { $path = $process.Path } catch { }
        try { $startedUtc = $process.StartTime.ToUniversalTime().ToString("o") } catch { }
        $results += [pscustomobject]@{
            Process = $process
            Id = $process.Id
            Name = $process.ProcessName
            Path = $path
            StartedUtc = $startedUtc
        }
    }
    return $results
}

function Test-OwnedLabProcess {
    param(
        [Parameter(Mandatory = $true)]$ProcessInfo,
        [Parameter(Mandatory = $true)][string]$Root
    )
    if (-not $ProcessInfo.Path -or -not (Test-PathInside $Root $ProcessInfo.Path)) {
        return $false
    }
    $relative = (Get-FullPath $ProcessInfo.Path).Substring(
        ((Get-FullPath $Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar).Length)
    $runName = $relative.Split([char[]]@('\', '/'))[0]
    if (-not $runName.StartsWith("run-", [System.StringComparison]::Ordinal)) {
        return $false
    }
    $runRoot = Join-Path $Root $runName
    return Test-Path -LiteralPath (Join-Path $runRoot ".yanami-cold-start-run.json") -PathType Leaf
}

function Stop-PreviousLabInstance {
    param(
        [Parameter(Mandatory = $true)]$ProcessInfo,
        [Parameter(Mandatory = $true)][string]$Root,
        [switch]$Force
    )
    if (-not (Test-OwnedLabProcess $ProcessInfo $Root)) {
        throw "Refusing to close a process that is not owned by this lab."
    }
    if ($ProcessInfo.Name -ieq "Yanami") {
        throw "A Yanami cold-start launcher is still handing off. Wait a moment and retry."
    }

    Write-Host "Closing previous cold-start run (PID $($ProcessInfo.Id))..."
    $closed = $ProcessInfo.Process.CloseMainWindow()
    if ($closed -and $ProcessInfo.Process.WaitForExit(10000)) {
        return
    }
    if (-not $Force) {
        throw "The previous lab window did not close in 10 seconds. Close it manually, or rerun with -ForcePreviousLab."
    }

    $current = [System.Diagnostics.Process]::GetProcessById($ProcessInfo.Id)
    $currentPath = $null
    try { $currentPath = $current.Path } catch { }
    $refreshed = [pscustomobject]@{
        Process = $current
        Id = $current.Id
        Name = $current.ProcessName
        Path = $currentPath
        StartedUtc = $ProcessInfo.StartedUtc
    }
    if (-not (Test-OwnedLabProcess $refreshed $Root) -or
        $currentPath -ine $ProcessInfo.Path) {
        throw "The previous process identity changed; refusing to force-stop it."
    }
    Write-Host "Force-stopping the unresponsive previous lab process (PID $($ProcessInfo.Id))."
    $current.Kill()
    if (-not $current.WaitForExit(5000)) {
        throw "The previous lab process did not exit after force-stop."
    }
}

function Test-YanamiInstanceMutex {
    try {
        $existing = [System.Threading.Mutex]::OpenExisting("Local\Yanami.Desktop.Instance.v1")
        $existing.Dispose()
        return $true
    } catch [System.Threading.WaitHandleCannotBeOpenedException] {
        return $false
    }
}

function Assert-YanamiAvailable {
    param([Parameter(Mandatory = $true)][string]$Root)
    $processes = @(Get-YanamiProcesses)
    $foreign = @($processes | Where-Object { -not (Test-OwnedLabProcess $_ $Root) })
    if ($foreign.Count -gt 0) {
        $details = ($foreign | ForEach-Object {
            $displayPath = if ($_.Path) { $_.Path } else { "<unavailable>" }
            "PID $($_.Id): $displayPath"
        }) -join [Environment]::NewLine
        throw "Another Yanami instance is running outside the cold-start lab. Close it normally and retry:`n$details"
    }
    foreach ($process in $processes) {
        Stop-PreviousLabInstance $process $Root -Force:$ForcePreviousLab
    }

    $remaining = @(Get-YanamiProcesses)
    if ($remaining.Count -gt 0) {
        throw "A Yanami process is still running after the lab close request."
    }
    if (Test-YanamiInstanceMutex) {
        throw "The Yanami single-instance mutex is still held. Wait a moment and retry."
    }
}

function Remove-OldLabRuns {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][int]$KeepBeforeCreating
    )
    $runs = @(Get-ChildItem -LiteralPath $Root -Directory -Force |
        Where-Object {
            $_.Name.StartsWith("run-", [System.StringComparison]::Ordinal) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName ".yanami-cold-start-run.json") -PathType Leaf)
        } |
        Sort-Object LastWriteTimeUtc -Descending)
    $activePaths = @((Get-YanamiProcesses) | Where-Object Path | ForEach-Object Path)
    foreach ($run in @($runs | Select-Object -Skip $KeepBeforeCreating)) {
        if (-not (Test-PathInside $Root $run.FullName)) {
            throw "Unsafe run selected for cleanup: $($run.FullName)"
        }
        if (@($activePaths | Where-Object { Test-PathInside $run.FullName $_ }).Count -gt 0) {
            continue
        }
        Remove-Item -LiteralPath $run.FullName -Recurse -Force
    }
}

function Assert-PackageTree {
    param([Parameter(Mandatory = $true)][string]$Root)
    foreach ($relative in @(
        "bin\Yanami.exe",
        "bin\yanami-desktop.exe",
        "bin\qt.conf",
        "BUILD_INFO.json",
        "SHA256SUMS.txt"
    )) {
        $path = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "The package tree is missing: $path"
        }
    }
}

function Refresh-PackageTree {
    $runWindows = Join-Path $PSScriptRoot "run-windows.ps1"
    & $runWindows -BuildOnly
    if ($LASTEXITCODE -ne 0) {
        throw "Yanami build failed while refreshing the cold-start source package."
    }
    & cmake --build $buildDirectory --target yanami-pr-install-smoke --config Release --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Yanami install-smoke package refresh failed."
    }
}

function New-ColdStartRun {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $guid = [Guid]::NewGuid().ToString("N")
    $runId = "run-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))-$($guid.Substring(0, 8))"
    $stagingRoot = Join-Path $Root ".staging-$guid"
    $finalRoot = Join-Path $Root $runId
    if (-not (Test-PathInside $Root $stagingRoot) -or -not (Test-PathInside $Root $finalRoot)) {
        throw "Unsafe cold-start run path."
    }

    try {
        New-Item -ItemType Directory -Path $stagingRoot | Out-Null
        $stagingPackage = Join-Path $stagingRoot "package"
        Write-Host "Preparing a fresh executable path with unbuffered copy..."
        & robocopy.exe $SourceRoot $stagingPackage /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /J /NFL /NDL /NJH /NJS /NP | Out-Null
        $copyExitCode = $LASTEXITCODE
        if ($copyExitCode -ge 8) {
            throw "robocopy failed with exit code $copyExitCode."
        }
        Assert-PackageTree $stagingPackage
        New-Item -ItemType Directory -Path (Join-Path $stagingRoot "profile\temp") -Force | Out-Null
        Move-Item -LiteralPath $stagingRoot -Destination $finalRoot

        $package = Join-Path $finalRoot "package"
        $profile = Join-Path $finalRoot "profile"
        $launcher = Join-Path $package "bin\Yanami.exe"
        $desktop = Join-Path $package "bin\yanami-desktop.exe"
        $manifestPath = Join-Path $finalRoot ".yanami-cold-start-run.json"
        [ordered]@{
            schemaVersion = "1.0"
            runId = $runId
            createdUtc = [DateTime]::UtcNow.ToString("o")
            sourcePackageRoot = $SourceRoot
            runRoot = $finalRoot
            profileRoot = $profile
            launcher = [ordered]@{
                path = $launcher
                length = (Get-Item -LiteralPath $launcher).Length
                lastWriteUtc = (Get-Item -LiteralPath $launcher).LastWriteTimeUtc.ToString("o")
            }
            desktop = [ordered]@{
                path = $desktop
                length = (Get-Item -LiteralPath $desktop).Length
                lastWriteUtc = (Get-Item -LiteralPath $desktop).LastWriteTimeUtc.ToString("o")
            }
            simulation = [ordered]@{
                freshExecutablePath = $true
                freshApplicationProfile = $true
                isolatedQSettings = $true
                isolatedCredentials = $true
                unbufferedPackageCopy = $true
                osPageCacheFlushed = $false
                markOfTheWebApplied = $false
                smartScreenIncluded = $false
                freshWindowsUserProfile = $false
            }
            traceEnabled = [bool]$Trace
        } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

        return [pscustomobject]@{
            RunId = $runId
            RunRoot = $finalRoot
            PackageRoot = $package
            ProfileRoot = $profile
            Launcher = $launcher
            Desktop = $desktop
            Manifest = $manifestPath
        }
    } catch {
        if ((Test-Path -LiteralPath $stagingRoot) -and
            (Test-PathInside $Root $stagingRoot) -and
            (Split-Path -Leaf $stagingRoot).StartsWith(".staging-", [System.StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force
        }
        throw
    }
}

function Start-ColdRun {
    param([Parameter(Mandatory = $true)]$Run)
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Run.Launcher
    $startInfo.WorkingDirectory = Split-Path -Parent $Run.Launcher
    $startInfo.UseShellExecute = $false

    foreach ($key in @($startInfo.EnvironmentVariables.Keys)) {
        if ($key -like "YANAMI_DEV_*" -or
            $key -like "YANAMI_PERF_*" -or
            $key -in @(
                "QML_IMPORT_PATH",
                "QML2_IMPORT_PATH",
                "QT_PLUGIN_PATH",
                "QT_QPA_PLATFORM",
                "QT_QUICK_BACKEND",
                "QT_OPENGL"
            )) {
            $startInfo.EnvironmentVariables.Remove($key)
        }
    }

    $tempPath = Join-Path $Run.ProfileRoot "temp"
    $startInfo.EnvironmentVariables["YANAMI_ISOLATED_PROFILE_ROOT"] = $Run.ProfileRoot
    $startInfo.EnvironmentVariables["APPDATA"] = Join-Path $Run.ProfileRoot "roaming"
    $startInfo.EnvironmentVariables["LOCALAPPDATA"] = Join-Path $Run.ProfileRoot "local"
    $startInfo.EnvironmentVariables["TEMP"] = $tempPath
    $startInfo.EnvironmentVariables["TMP"] = $tempPath
    $startInfo.EnvironmentVariables["QML_DISK_CACHE_PATH"] = Join-Path $Run.ProfileRoot "qt\qml-cache"

    $systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
    $windowsDirectory = [Environment]::GetEnvironmentVariable("SystemRoot")
    $startInfo.EnvironmentVariables["PATH"] = @(
        (Split-Path -Parent $Run.Launcher),
        $systemDirectory,
        $windowsDirectory
    ) -join ";"

    if ($Trace) {
        $startInfo.EnvironmentVariables["YANAMI_PERF_RUN_ID"] = $Run.RunId
        $startInfo.EnvironmentVariables["YANAMI_PERF_TRACE"] = Join-Path $Run.RunRoot "startup-trace.jsonl"
    }

    Write-Host ""
    Write-Host "=== YANAMI COLD-START BOUNDARY ===" -ForegroundColor Cyan
    Write-Host "The splash-to-first-screen experience starts now."
    $process = [System.Diagnostics.Process]::Start($startInfo)
    if (-not $process) {
        throw "Windows did not create the Yanami launcher process."
    }
    Write-Host "Launcher PID: $($process.Id)"
}

$scriptMutex = New-Object System.Threading.Mutex($false, "Local\Yanami.ColdStartLab.Script.v1")
$ownsScriptMutex = $false
try {
    try {
        $ownsScriptMutex = $scriptMutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        $ownsScriptMutex = $true
    }
    if (-not $ownsScriptMutex) {
        throw "Another Yanami cold-start preparation is already running."
    }

    $LabRoot = Assert-SafeLabRoot $LabRoot
    Initialize-LabRoot $LabRoot
    Assert-YanamiAvailable $LabRoot

    if ($RefreshPackage) {
        Refresh-PackageTree
    }
    $PackageRoot = Get-FullPath $PackageRoot
    Assert-PackageTree $PackageRoot
    $builtLauncher = Join-Path $buildDirectory "Yanami.exe"
    $packageLauncher = Join-Path $PackageRoot "bin\Yanami.exe"
    if ((Test-Path -LiteralPath $builtLauncher -PathType Leaf) -and
        (Get-Item -LiteralPath $builtLauncher).LastWriteTimeUtc -gt
            (Get-Item -LiteralPath $packageLauncher).LastWriteTimeUtc.AddSeconds(1)) {
        throw "The install-smoke package is older than the current build. Rerun with -RefreshPackage."
    }

    Remove-OldLabRuns $LabRoot ([Math]::Max(0, $RetainRuns - 1))
    $run = New-ColdStartRun $PackageRoot $LabRoot
    Write-Host "Prepared cold-start run: $($run.RunRoot)"
    Write-Host "Profile isolation: settings, data/cache, logs, and credentials"
    Write-Host "Boundary: this does not flush the Windows page cache or emulate SmartScreen/MOTW."
    if (-not $PrepareOnly) {
        Start-ColdRun $run
    } else {
        Write-Host "Prepare-only mode: no process was launched."
    }
} finally {
    if ($ownsScriptMutex) {
        $scriptMutex.ReleaseMutex()
    }
    $scriptMutex.Dispose()
}
