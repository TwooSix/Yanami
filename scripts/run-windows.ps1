[CmdletBinding()]
param(
    [switch]$BuildOnly,
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Get-MsysRoot {
    $candidates = @()
    if (Get-Command scoop -ErrorAction SilentlyContinue) {
        $scoopPrefix = (& scoop prefix msys2 2>$null | Select-Object -Last 1)
        if ($scoopPrefix) {
            $candidates += $scoopPrefix
        }
    }
    $candidates += @(
        "C:\msys64",
        (Join-Path $env:USERPROFILE "scoop\apps\msys2\current"),
        (Join-Path $env:LOCALAPPDATA "Programs\MSYS2")
    )
    return $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ "ucrt64\bin\qmake6.exe")) } | Select-Object -First 1
}

$workspace = Split-Path -Parent $PSScriptRoot
$msysRoot = Get-MsysRoot
if (-not $msysRoot) {
    throw "Qt development environment is missing. Run .\scripts\bootstrap-windows.ps1 first."
}

$ucrtBin = Join-Path $msysRoot "ucrt64\bin"
$cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
$env:PATH = "$cargoBin;$ucrtBin;$env:PATH"
$env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"

if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    throw "Cargo is missing. Run .\scripts\bootstrap-windows.ps1 first."
}

$sourceDir = Join-Path $workspace "apps\desktop"
$buildDir = Join-Path $workspace "build\desktop-windows"
$compiler = Join-Path $ucrtBin "g++.exe"
$configureArgs = @(
    "-S", $sourceDir,
    "-B", $buildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_PREFIX_PATH=$($msysRoot)\ucrt64",
    "-DCMAKE_CXX_COMPILER=$compiler"
)
if ($Fresh) {
    $configureArgs = @("--fresh") + $configureArgs
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Desktop build failed."
}

$executable = Join-Path $buildDir "yanami-desktop.exe"
Write-Host "Built $executable"
if (-not $BuildOnly) {
    & $executable
}
