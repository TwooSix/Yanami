[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Windows", "Linux", "macOS")]
    [string]$Platform,

    [Parameter(Mandatory = $true)]
    [string]$EntryPointPath,

    [Parameter(Mandatory = $true)]
    [string]$LauncherBinaryPath,

    [Parameter(Mandatory = $true)]
    [string]$DesktopBinaryPath,

    [Parameter(Mandatory = $true)]
    [string]$DependencyToolPath,

    [Parameter(Mandatory = $true)]
    [string]$CompilerPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$CandidateSha = "",

    [ValidateRange(100, 5000)]
    [int]$TimeoutSmokeMilliseconds = 250
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$entryPoint = [IO.Path]::GetFullPath($EntryPointPath)
$launcher = [IO.Path]::GetFullPath($LauncherBinaryPath)
$desktop = [IO.Path]::GetFullPath($DesktopBinaryPath)
$dependencyTool = [IO.Path]::GetFullPath($DependencyToolPath)
$compiler = [IO.Path]::GetFullPath($CompilerPath)
$output = [IO.Path]::GetFullPath($OutputDirectory)

foreach ($requiredPath in @($entryPoint, $launcher, $desktop, $dependencyTool, $compiler)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Bootstrap package smoke input does not exist: $requiredPath"
    }
}
if (-not (Test-Path -LiteralPath $output)) {
    [void](New-Item -ItemType Directory -Path $output)
}

switch ($Platform) {
    "Windows" {
        $dependencyOutput = (& $dependencyTool -p $launcher 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) {
            throw "objdump failed while auditing the bootstrap PE import table."
        }
    }
    "Linux" {
        $dependencyOutput = (& $dependencyTool $launcher 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $dependencyOutput -match 'not found') {
            throw "ldd failed or found an unresolved bootstrap dependency.`n$dependencyOutput"
        }
    }
    "macOS" {
        $dependencyOutput = (& $dependencyTool -L $launcher 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) {
            throw "otool failed while auditing the bootstrap Mach-O dependencies."
        }
    }
}
$forbiddenDependencyPattern =
    '(?i)(Qt[56]|libmpv|\bmpv(?:-[0-9]+)?\.(?:dll|so|dylib)|SDL3|yanami_desktop_bridge|libavcodec|libavformat|libavutil|libplacebo)'
if ($dependencyOutput -match $forbiddenDependencyPattern) {
    throw "Bootstrap must stay independent of Qt, mpv, SDL, the Rust bridge, and media runtime libraries.`n$dependencyOutput"
}
$dependencyAuditPath = Join-Path $output "bootstrap-dependencies.txt"
$dependencyOutput | Set-Content -LiteralPath $dependencyAuditPath -Encoding UTF8

$runtimeOutput = Join-Path $output "runtime"
$gateArguments = @{
    Profile = "PullRequest"
    OutputDirectory = $runtimeOutput
    Suites = "Startup"
    DesktopExecutable = $entryPoint
    StartupTimeoutSeconds = 30
    SkipProbeDiscovery = $true
    RequireBootstrapSidecar = $true
    NoExit = $true
}
if ($Platform -ne "Linux") {
    $gateArguments.UseNativeWindowForRuntimeTrace = $true
}
if (-not [string]::IsNullOrWhiteSpace($CandidateSha)) {
    $gateArguments.CandidateSha = $CandidateSha
}
$gateOutput = @(& (Join-Path $PSScriptRoot "run-gate.ps1") @gateArguments)
$gateResult = @($gateOutput | Where-Object {
    $null -ne $_.PSObject.Properties["status"]
} | Select-Object -Last 1)
if ($gateResult.Count -eq 1) { $gateResult = $gateResult[0] } else { $gateResult = $null }
if (-not $gateResult -or [string]$gateResult.status -ne "pass") {
    $observedStatus = if ($gateResult) { [string]$gateResult.status } else { "missing" }
    throw "Bootstrap startup performance smoke failed with status '$observedStatus'."
}
$performanceResultPath = Join-Path $runtimeOutput "performance-result.json"
$performanceResult = Get-Content -LiteralPath $performanceResultPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ([string]$performanceResult.status -ne "pass") {
    throw "Bootstrap startup contract must pass; observed '$($performanceResult.status)'."
}
foreach ($invariantId in @(
    "startup.milestone_order_valid",
    "startup.bootstrap_handoff_valid",
    "startup.no_false_progress_indicator")) {
    $invariant = @($performanceResult.invariants |
        Where-Object { [string]$_.id -eq $invariantId })
    if ($invariant.Count -ne 1 -or -not [bool]$invariant[0].passed) {
        throw "Bootstrap startup result did not pass invariant '$invariantId'."
    }
}
function Get-StartupObservationSample([string]$MetricId) {
    $metric = @($performanceResult.metrics |
        Where-Object { [string]$_.id -eq $MetricId })
    if ($metric.Count -ne 1 -or @($metric[0].samples).Count -ne 1) {
        throw "Bootstrap startup result must contain one '$MetricId' observation."
    }
    return [double]$metric[0].samples[0]
}
$bootstrapFirstVisibleMs = Get-StartupObservationSample `
    "startup.internal.bootstrap_entered_to_first_visible_candidate_ms"
$bootstrapToDesktopReadyMs = Get-StartupObservationSample `
    "startup.internal.bootstrap_visible_to_desktop_ready_candidate_ms"
$desktopReadyToHandoffMs = Get-StartupObservationSample `
    "startup.internal.desktop_ready_to_handoff_animation_ms"
$bootstrapToHandoffMs = Get-StartupObservationSample `
    "startup.internal.bootstrap_visible_to_handoff_ms"

$timeoutRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "yanami-bootstrap-timeout-" + [guid]::NewGuid().ToString("N"))
[void](New-Item -ItemType Directory -Path $timeoutRoot)
$fakeSource = Join-Path $timeoutRoot "fake-desktop.c"
$fakeDesktop = Join-Path $timeoutRoot ([IO.Path]::GetFileName($desktop))
$copiedLauncher = Join-Path $timeoutRoot ([IO.Path]::GetFileName($launcher))
Copy-Item -LiteralPath $launcher -Destination $copiedLauncher
$compilerPathBeforeSmoke = $env:PATH
$compileExitCode = -1
try {
    $env:PATH = "$(Split-Path -Parent $compiler);$env:PATH"
    if ($Platform -eq "Windows") {
        @'
#include <windows.h>
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    (void)instance; (void)previous; (void)command; (void)show;
    Sleep(5000);
    return 0;
}
'@ | Set-Content -LiteralPath $fakeSource -Encoding UTF8
        & $compiler $fakeSource -Os -s -mwindows -o $fakeDesktop
    } else {
        @'
#include <unistd.h>
int main(void) {
    sleep(5);
    return 0;
}
'@ | Set-Content -LiteralPath $fakeSource -Encoding UTF8
        & $compiler $fakeSource -Os -o $fakeDesktop
    }
    $compileExitCode = $LASTEXITCODE
} finally {
    $env:PATH = $compilerPathBeforeSmoke
}
if ($compileExitCode -ne 0 -or
    -not (Test-Path -LiteralPath $fakeDesktop -PathType Leaf)) {
    throw "Unable to build the staged desktop process for bootstrap timeout testing."
}
if ($Platform -ne "Windows") {
    & chmod +x $copiedLauncher $fakeDesktop
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to make the staged bootstrap timeout binaries executable."
    }
}

$timeoutStart = [Diagnostics.ProcessStartInfo]::new()
$timeoutStart.FileName = $copiedLauncher
$timeoutStart.WorkingDirectory = $timeoutRoot
$timeoutStart.UseShellExecute = $false
$timeoutStart.CreateNoWindow = $true
[void]$timeoutStart.ArgumentList.Add(
    "--yanami-bootstrap-timeout-ms=$TimeoutSmokeMilliseconds")
# A forwarded argument selects command/CI mode, in which the launcher owns a
# bounded timeout and mirrors the child exit code. The staged child ignores it.
[void]$timeoutStart.ArgumentList.Add("--yanami-bootstrap-smoke-child")
$timeoutProcess = [Diagnostics.Process]::new()
$timeoutProcess.StartInfo = $timeoutStart
$timeoutClock = [Diagnostics.Stopwatch]::StartNew()
try {
    if (-not $timeoutProcess.Start()) {
        throw "Unable to start the staged bootstrap timeout smoke."
    }
    $waitMilliseconds = [Math]::Max(5000, $TimeoutSmokeMilliseconds + 3000)
    if (-not $timeoutProcess.WaitForExit($waitMilliseconds)) {
        $timeoutProcess.Kill($true)
        throw "Bootstrap timeout smoke exceeded its bounded wait."
    }
    $timeoutExitCode = [int]$timeoutProcess.ExitCode
} finally {
    $timeoutClock.Stop()
    $timeoutProcess.Dispose()
}
if ($timeoutExitCode -ne 70) {
    throw "Bootstrap timeout smoke must exit 70; observed $timeoutExitCode."
}
if ($timeoutClock.ElapsedMilliseconds -lt $TimeoutSmokeMilliseconds -or
    $timeoutClock.ElapsedMilliseconds -gt ($TimeoutSmokeMilliseconds + 2500)) {
    throw "Bootstrap timeout was not enforced near the requested bound: $($timeoutClock.ElapsedMilliseconds) ms."
}

$report = [pscustomobject][ordered]@{
    schemaVersion = "1.0"
    platform = $Platform
    entryPoint = [IO.Path]::GetFileName($entryPoint)
    launcher = [IO.Path]::GetFileName($launcher)
    desktop = [IO.Path]::GetFileName($desktop)
    launcherSha256 = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash.ToLowerInvariant()
    dependencyAuditSha256 = (Get-FileHash -LiteralPath $dependencyAuditPath -Algorithm SHA256).Hash.ToLowerInvariant()
    startupResultSha256 = (Get-FileHash -LiteralPath $performanceResultPath -Algorithm SHA256).Hash.ToLowerInvariant()
    timeoutExitCode = $timeoutExitCode
    timeoutElapsedMs = [long]$timeoutClock.ElapsedMilliseconds
    observations = [pscustomobject][ordered]@{
        bootstrapEnteredToFirstVisibleCandidateMs = $bootstrapFirstVisibleMs
        bootstrapVisibleToDesktopReadyCandidateMs = $bootstrapToDesktopReadyMs
        desktopReadyToHandoffAnimationMs = $desktopReadyToHandoffMs
        bootstrapVisibleToHandoffMs = $bootstrapToHandoffMs
    }
    bootstrapContract = [pscustomobject][ordered]@{
        progressSemantic = "indeterminate"
        contentReadyMilestone = "desktop_ready"
        animationCompletionMilestone = "handoff_complete"
        strictExternalPresent = $false
        downloadReputationDelayMeasured = $false
    }
}
$reportPath = Join-Path $output "bootstrap-package-smoke.json"
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$report
