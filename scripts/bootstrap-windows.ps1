[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Get-MsysRoot {
    $candidates = @()
    $scoop = Get-Command scoop -ErrorAction SilentlyContinue
    if ($scoop) {
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
    return $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ "usr\bin\bash.exe")) } | Select-Object -First 1
}

function Invoke-Msys([string]$Root, [string]$Command, [switch]$AllowFailure) {
    $bash = Join-Path $Root "usr\bin\bash.exe"
    & $bash -lc "export LANG=C; $Command"
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) {
        throw "MSYS2 command failed with exit code $LASTEXITCODE"
    }
}

$msysRoot = Get-MsysRoot
if (-not $msysRoot) {
    if (Get-Command scoop -ErrorAction SilentlyContinue) {
        Write-Host "Installing MSYS2 with Scoop..."
        & scoop install msys2
        if ($LASTEXITCODE -ne 0) {
            throw "Scoop could not install MSYS2."
        }
    } elseif (Get-Command winget -ErrorAction SilentlyContinue) {
        Write-Host "Installing MSYS2 with winget..."
        & winget install --id MSYS2.MSYS2 --exact --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) {
            throw "winget could not install MSYS2."
        }
    } else {
        throw "Install Scoop or winget, then run this script again."
    }
    $msysRoot = Get-MsysRoot
}

if (-not $msysRoot) {
    throw "MSYS2 was installed but its root directory could not be located."
}

Write-Host "Initializing and updating MSYS2 at $msysRoot..."
Invoke-Msys $msysRoot "true" -AllowFailure

# A first MSYS2 runtime update intentionally terminates its own shell. Retry in
# a fresh process to finish the remaining updates.
Invoke-Msys $msysRoot "pacman -Syu --noconfirm" -AllowFailure
Start-Sleep -Seconds 2
Invoke-Msys $msysRoot "pacman -Syu --noconfirm"

$packages = @(
    "mingw-w64-ucrt-x86_64-gcc",
    "mingw-w64-ucrt-x86_64-cmake",
    "mingw-w64-ucrt-x86_64-ninja",
    "mingw-w64-ucrt-x86_64-pkgconf",
    "mingw-w64-ucrt-x86_64-qt6-base",
    "mingw-w64-ucrt-x86_64-qt6-declarative",
    "mingw-w64-ucrt-x86_64-qt6-tools",
    "mingw-w64-ucrt-x86_64-sdl3",
    "mingw-w64-ucrt-x86_64-mpv"
) -join " "
Write-Host "Installing Qt, SDL3, libmpv, and the native compiler..."
Invoke-Msys $msysRoot "pacman -S --needed --noconfirm $packages"

$cargoBin = Join-Path $env:USERPROFILE ".cargo\bin"
$rustup = Join-Path $cargoBin "rustup.exe"
if (-not (Test-Path $rustup)) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "Rustup is missing and winget is unavailable. Install rustup from https://rustup.rs and run this script again."
    }
    Write-Host "Installing the official Rust toolchain manager..."
    & winget install --id Rustlang.Rustup --exact --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rustup)) {
        throw "Rustup installation did not complete. Open a new terminal and run this script again."
    }
}

Write-Host "Installing the Rust GNU toolchain used with MSYS2 Qt..."
$installedToolchains = & $rustup toolchain list
if ($LASTEXITCODE -ne 0) {
    throw "Unable to inspect the installed Rust toolchains."
}
if (-not ($installedToolchains -match "^stable-x86_64-pc-windows-gnu(?:\s|$)")) {
    & $rustup toolchain install stable-x86_64-pc-windows-gnu --profile minimal --component clippy --component rustfmt
    if ($LASTEXITCODE -ne 0) {
        throw "Rust GNU toolchain installation failed."
    }
} else {
    Write-Host "Rust GNU toolchain is already installed."
}

Write-Host "Windows development environment is ready."
Write-Host "Run: .\scripts\run-windows.ps1"
