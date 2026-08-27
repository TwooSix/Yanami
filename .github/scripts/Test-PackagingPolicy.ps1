$ErrorActionPreference = "Stop"
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$assertions = 0

function Read-WorkspaceFile {
    param([string]$Path)
    Get-Content -LiteralPath (Join-Path $workspace $Path) -Raw
}

function Assert-Contains {
    param([string]$Actual, [string]$Expected, [string]$Label)
    $script:assertions++
    if (-not $Actual.Contains($Expected, [StringComparison]::Ordinal)) {
        throw "$Label is missing from the packaging contract."
    }
}

function Assert-Absent {
    param([string]$Actual, [string]$Unexpected, [string]$Label)
    $script:assertions++
    if ($Actual.Contains($Unexpected, [StringComparison]::Ordinal)) {
        throw "$Label must not appear in the packaging contract."
    }
}

function Assert-PathAbsent {
    param([string]$Path, [string]$Label)
    $script:assertions++
    if (Test-Path -LiteralPath (Join-Path $workspace $Path)) {
        throw "$Label is generated data and must not be tracked."
    }
}

$attributes = Read-WorkspaceFile ".gitattributes"
Assert-Contains $attributes `
    "licenses/msys2-fallback/** -text whitespace=-blank-at-eol,-blank-at-eof,-space-before-tab" `
    "byte-stable fallback licenses"
Assert-Absent $attributes "licenses/rust/THIRD_PARTY_LICENSES.html" `
    "generated Rust inventory attribute"

Assert-PathAbsent "licenses/RUST_DEPENDENCIES.md" `
    "legacy Rust dependency snapshot"
Assert-PathAbsent "licenses/rust/THIRD_PARTY_LICENSES.html" `
    "generated Rust license snapshot"

foreach ($scriptPath in @("scripts/package-linux.sh", "scripts/package-macos.sh")) {
    $script = Read-WorkspaceFile $scriptPath
    Assert-Contains $script `
        'if [[ ! -f "$rust_license_inventory" ]]; then' `
        "$scriptPath rejects a missing supplied inventory"
    Assert-Contains $script `
        'bash "$script_dir/generate-rust-license-inventory.sh" "$rust_license_inventory"' `
        "$scriptPath generates a local inventory when needed"
    Assert-Contains $script `
        '-DYANAMI_RUST_LICENSE_INVENTORY="$rust_license_inventory"' `
        "$scriptPath passes the inventory to CMake"
}

$cmake = Read-WorkspaceFile "apps/desktop/CMakeLists.txt"
Assert-Contains $cmake 'cmake/VerifyRustLicenseInventory.cmake.in' `
    "CMake inventory verifier"
Assert-Contains $cmake 'install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/VerifyRustLicenseInventory.cmake")' `
    "install-time inventory verification"
Assert-Contains $cmake 'install(FILES "${YANAMI_RUST_LICENSE_INVENTORY}"' `
    "generated inventory installation"
Assert-Contains $cmake 'RENAME THIRD_PARTY_LICENSES.html' `
    "stable packaged inventory path"

$generator = Read-WorkspaceFile "scripts/generate-rust-license-inventory.sh"
Assert-Contains $generator 'yanami-license-generator' `
    "pinned inventory generator provenance"
Assert-Contains $generator 'yanami-license-input-sha256' `
    "inventory input hashes"

$inventoryVerifier = Read-WorkspaceFile `
    "apps/desktop/cmake/VerifyRustLicenseInventory.cmake.in"
Assert-Contains $inventoryVerifier 'file(SHA256 "${input}" input_hash)' `
    "install-time inventory input hashing"
Assert-Contains $inventoryVerifier `
    'Generated Rust license inventory is stale for ${relative_input}' `
    "stale inventory rejection"

$macVerifier = Read-WorkspaceFile "scripts/verify-macos-bundle.sh"
Assert-Contains $macVerifier "extract_dependencies()" `
    "macOS dependency extraction"
Assert-Contains $macVerifier "verify_dependency()" `
    "macOS dependency verification"
Assert-Contains $macVerifier "Unresolved or non-portable dylib reference" `
    "macOS non-portable dependency rejection"

$releaseWorkflow = Read-WorkspaceFile ".github/workflows/release.yml"
Assert-Contains $releaseWorkflow `
    'bash scripts/verify-macos-bundle.sh "$app"' `
    "release macOS bundle verification"

$installMacRuntime = Read-WorkspaceFile `
    "apps/desktop/cmake/InstallMacRuntime.cmake.in"
Assert-Contains $installMacRuntime `
    '@YANAMI_WORKSPACE@/scripts/verify-macos-bundle.sh' `
    "install-time macOS bundle verification"

$installSmoke = Read-WorkspaceFile "apps/desktop/tests/InstallTreeSmoke.cmake"
Assert-Contains $installSmoke 'licenses/rust/THIRD_PARTY_LICENSES.html' `
    "install-tree inventory assertion"

$notices = Read-WorkspaceFile "THIRD_PARTY_NOTICES.md"
Assert-Contains $notices "generated from that locked Rust graph with" `
    "third-party inventory provenance"
Assert-Contains $notices "cargo-about during CI and packaging" `
    "third-party inventory generation timing"

Write-Host "Packaging policy tests passed ($assertions assertions)."
