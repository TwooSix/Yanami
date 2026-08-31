$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$workspace = [IO.Path]::GetFullPath(
    (Join-Path (Join-Path $PSScriptRoot '..') '..'))
$scriptPath = [IO.Path]::Combine(
    $workspace, 'scripts', 'package-windows-velopack.ps1')
$assertions = 0

function Assert-True {
    param([bool]$Condition, [string]$Label)
    $script:assertions++
    if (-not $Condition) {
        throw "Assertion failed: $Label"
    }
}

function Assert-Equal {
    param($Actual, $Expected, [string]$Label)
    $script:assertions++
    if ($Actual -cne $Expected) {
        throw "Assertion failed: $Label (expected '$Expected', got '$Actual')"
    }
}

function Assert-Contains {
    param([string]$Actual, [string]$Expected, [string]$Label)
    $script:assertions++
    if (-not $Actual.Contains($Expected, [StringComparison]::Ordinal)) {
        throw "Assertion failed: $Label"
    }
}

function Assert-Throws {
    param(
        [Parameter(Mandatory)][scriptblock]$Action,
        [Parameter(Mandatory)][string]$Expected,
        [Parameter(Mandatory)][string]$Label
    )

    $script:assertions++
    try {
        & $Action
    } catch {
        if (-not $_.Exception.Message.Contains(
                $Expected, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Assertion failed: $Label (unexpected error '$($_.Exception.Message)')"
        }
        return
    }
    throw "Assertion failed: $Label (no exception was thrown)"
}

function Write-TestPe {
    param([Parameter(Mandatory)][string]$Path)
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) `
        -Force | Out-Null
    [IO.File]::WriteAllBytes($Path, [byte[]]@(0x4d, 0x5a, 0x00, 0x00))
}

function Read-UInt64LittleEndian {
    param([byte[]]$Buffer, [int]$Offset)
    [uint64]$value = 0
    for ($index = 0; $index -lt 8; $index++) {
        $value = $value -bor (
            [uint64]$Buffer[$Offset + $index] -shl (8 * $index))
    }
    return $value
}

function Read-UInt32LittleEndian {
    param([byte[]]$Buffer, [int]$Offset)
    [uint32]$value = 0
    for ($index = 0; $index -lt 4; $index++) {
        $value = $value -bor (
            [uint32]$Buffer[$Offset + $index] -shl (8 * $index))
    }
    return $value
}

function New-TestNupkg {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$Channel
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $source = "$Path.source"
    New-Item -ItemType Directory -Path $source | Out-Null
    $nuspec = @"
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2012/06/nuspec.xsd">
  <metadata>
    <id>$Id</id>
    <version>$Version</version>
    <channel>$Channel</channel>
    <mainExe>Yanami.exe</mainExe>
  </metadata>
</package>
"@
    [IO.File]::WriteAllText((Join-Path $source "$Id.nuspec"), $nuspec,
        [Text.UTF8Encoding]::new($false))
    [IO.Compression.ZipFile]::CreateFromDirectory($source, $Path)
}

$testParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $testParent `
    ("yanami-velopack-test-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null

try {
    $wrappedRoot = Join-Path $testRoot 'wrapped'
    $packageRoot = Join-Path $wrappedRoot `
        'Yanami-0.2.0-dev.15-Windows-x86_64'
    $packageBin = Join-Path $packageRoot 'bin'
    foreach ($required in @('Yanami.exe', 'yanami-updater.exe',
            'yanami-desktop.exe', 'yanami_desktop_bridge.dll',
            'velopack_libc.dll')) {
        Write-TestPe -Path (Join-Path $packageBin $required)
    }
    [IO.File]::WriteAllText((Join-Path $packageRoot 'BUILD_INFO.json'), '{}')
    $fixtureLicense = [IO.Path]::Combine(
        $packageRoot, 'licenses', 'velopack', 'MIT.txt')
    New-Item -ItemType Directory -Path (Split-Path -Parent $fixtureLicense) `
        -Force | Out-Null
    [IO.File]::WriteAllText($fixtureLicense, 'Velopack MIT fixture')

    $plannedOutput = Join-Path $testRoot 'planned-output'
    $plannedToolCache = Join-Path $testRoot 'planned-tools'
    $plannedStub = Join-Path $testRoot 'not-yet-built/yanami-installer.exe'
    $plan = & $scriptPath -PackageRoot $wrappedRoot `
        -Version '0.2.0-dev.15' -OutputDirectory $plannedOutput `
        -ToolCacheDirectory $plannedToolCache `
        -InstallerStubPath $plannedStub -PlanOnly

    Assert-True $plan.wrappedCpackRoot `
        'a single CPack wrapper directory is resolved'
    Assert-True $plan.flattenBin 'the CPack bin directory is flattened'
    Assert-Equal (Split-Path -Leaf $plan.mainExe) 'Yanami.exe' `
        'the main entry is the user-facing bootstrap'
    Assert-Equal $plan.expectedAssets.setup `
        'Yanami-0.2.0-dev.15-Windows-x86_64-Setup.exe' `
        'the published installer name is versioned'
    Assert-Equal $plan.expectedAssets.vpkSetup `
        'io.github.TwooSix.Yanami-preview-Setup.exe' `
        'the canonical vpk installer name is explicit'
    Assert-Equal $plan.expectedAssets.full `
        'io.github.TwooSix.Yanami-0.2.0-dev.15-preview-full.nupkg' `
        'the preview full package name is exact'
    Assert-Equal $plan.expectedAssets.delta `
        'io.github.TwooSix.Yanami-0.2.0-dev.15-preview-delta.nupkg' `
        'the preview delta package name is exact'
    Assert-Equal $plan.expectedAssets.feed 'releases.preview.json' `
        'the modern preview feed name is exact'
    Assert-Equal $plan.expectedAssets.assetManifest 'assets.preview.json' `
        'the upload asset manifest name is exact'
    Assert-Equal $plan.vpkVersion '1.2.0' 'vpk is version-pinned'
    Assert-Equal $plan.vpkPackageSha256 `
        '3e458a676be46d1122e522312db18411f36ea8c70e586f81a676695d43f89dbc' `
        'the official GitHub vpk package is hash-pinned'
    Assert-Equal $plan.installerStubPath `
        ([IO.Path]::GetFullPath($plannedStub)) `
        'PlanOnly reports the explicit installer stub path without resolving it'
    Assert-True (-not (Test-Path -LiteralPath $plannedStub)) `
        'PlanOnly does not require the installer stub to exist'
    Assert-Equal $plan.installerContract.magic 'YANAMI_SETUP_V1\0' `
        'the wrapper footer magic is planned explicitly'
    Assert-Equal $plan.installerContract.footerSize 64 `
        'the wrapper footer is exactly 64 bytes'
    Assert-Equal $plan.installerContract.format 1 `
        'the wrapper format version is fixed'
    Assert-Equal $plan.installerContract.verifyArgument '--verify-payload' `
        'the wrapper self-verification interface is planned'
    Assert-True (-not $plan.installerContract.backendPublished) `
        'the canonical Velopack Setup backend is not a public asset'
    Assert-True (-not (Test-Path -LiteralPath $plannedOutput)) `
        'PlanOnly does not create the release directory'
    Assert-True (-not (Test-Path -LiteralPath $plannedToolCache)) `
        'PlanOnly does not download tools'

    Assert-Throws -Expected 'Version' `
        -Label 'build metadata is rejected because vpk removes it from asset names' `
        -Action {
            & $scriptPath -PackageRoot $wrappedRoot `
                -Version '1.2.3+ci.9' `
                -OutputDirectory (Join-Path $testRoot 'metadata-output') `
                -PlanOnly
        }
    Assert-Throws -Expected 'Version' `
        -Label 'vpk-invalid version 0.0.0 is rejected before packaging' `
        -Action {
            & $scriptPath -PackageRoot $wrappedRoot -Version '0.0.0' `
                -OutputDirectory (Join-Path $testRoot 'zero-output') `
                -PlanOnly
        }

    $arguments = @($plan.arguments)
    Assert-Equal $arguments[0] '--skip-updates' `
        'vpk self-update checks are disabled before the command'
    Assert-Equal $arguments[1] 'pack' 'the vpk pack command is selected'
    $packIdIndex = [Array]::IndexOf($arguments, '--packId')
    Assert-Equal $arguments[$packIdIndex + 1] 'io.github.TwooSix.Yanami' `
        'the stable application id is fixed'
    $mainExeIndex = [Array]::IndexOf($arguments, '--mainExe')
    Assert-Equal $arguments[$mainExeIndex + 1] 'Yanami.exe' `
        'mainExe is a file name, not bin/Yanami.exe'
    $installLocationIndex = [Array]::IndexOf(
        $arguments, '--instLocation')
    Assert-Equal $arguments[$installLocationIndex + 1] 'PerUser' `
        'Setup.exe is constrained to a per-user install'
    $shortcutsIndex = [Array]::IndexOf($arguments, '--shortcuts')
    Assert-Equal $arguments[$shortcutsIndex + 1] 'None' `
        'the wrapper owns optional shortcut creation'
    Assert-True ($arguments -ccontains '--noPortable') `
        'the Windows release does not create an extra portable bundle'
    Assert-True ($arguments -ccontains '--skipVeloAppCheck') `
        'the native C++ package bypasses only the .NET IL scanner'

    $flatRoot = Join-Path $testRoot 'already-flat'
    foreach ($required in @('Yanami.exe', 'yanami-updater.exe',
            'yanami-desktop.exe', 'yanami_desktop_bridge.dll',
            'velopack_libc.dll')) {
        Write-TestPe -Path (Join-Path $flatRoot $required)
    }
    $flatLicense = [IO.Path]::Combine(
        $flatRoot, 'licenses', 'velopack', 'MIT.txt')
    New-Item -ItemType Directory -Path (Split-Path -Parent $flatLicense) `
        -Force | Out-Null
    [IO.File]::WriteAllText($flatLicense, 'Velopack MIT fixture')
    $flatPlan = & $scriptPath -PackageRoot $flatRoot -Version '1.0.0' `
        -OutputDirectory (Join-Path $testRoot 'flat-output') -PlanOnly
    Assert-True (-not $flatPlan.flattenBin) `
        'an already-flat Velopack packDir remains flat'

    $previous = Join-Path $testRoot 'previous'
    New-Item -ItemType Directory -Path $previous | Out-Null
    $previousNupkg = Join-Path $previous `
        'io.github.TwooSix.Yanami-0.2.0-dev.14-preview-full.nupkg'
    New-TestNupkg -Path $previousNupkg `
        -Id 'io.github.TwooSix.Yanami' -Version '0.2.0-dev.14' `
        -Channel 'preview'
    $deltaPlan = & $scriptPath -PackageRoot $wrappedRoot `
        -Version '0.2.0-dev.15' `
        -OutputDirectory (Join-Path $testRoot 'delta-output') `
        -PreviousReleaseDirectory $previous -PlanOnly
    Assert-Equal @($deltaPlan.previousPackages).Count 1 `
        'a matching previous full nupkg is accepted as the delta baseline'
    Assert-Equal (Split-Path -Leaf $deltaPlan.previousPackages[0]) `
        (Split-Path -Leaf $previousNupkg) `
        'the exact previous release asset is planned'

    $sameVersionPrevious = Join-Path $testRoot 'same-version-previous'
    New-Item -ItemType Directory -Path $sameVersionPrevious | Out-Null
    New-TestNupkg -Path (Join-Path $sameVersionPrevious `
        'io.github.TwooSix.Yanami-0.2.0-dev.15-preview-full.nupkg') `
        -Id 'io.github.TwooSix.Yanami' -Version '0.2.0-dev.15' `
        -Channel 'preview'
    Assert-Throws -Expected 'must be older than 0.2.0-dev.15' `
        -Label 'a same-version full package cannot become a delta baseline' `
        -Action {
            & $scriptPath -PackageRoot $wrappedRoot `
                -Version '0.2.0-dev.15' `
                -OutputDirectory (Join-Path $testRoot 'same-output') `
                -PreviousReleaseDirectory $sameVersionPrevious -PlanOnly
        }

    $wrongPrevious = Join-Path $testRoot 'wrong-previous'
    New-Item -ItemType Directory -Path $wrongPrevious | Out-Null
    New-TestNupkg -Path (Join-Path $wrongPrevious 'Other-1.0.0-full.nupkg') `
        -Id 'Other.App' -Version '1.0.0' -Channel 'preview'
    Assert-Throws -Expected 'contains no io.github.TwooSix.Yanami/preview full nupkg' `
        -Label 'a cross-application delta baseline is rejected' -Action {
            & $scriptPath -PackageRoot $wrappedRoot -Version '0.2.0-dev.15' `
                -OutputDirectory (Join-Path $testRoot 'wrong-output') `
                -PreviousReleaseDirectory $wrongPrevious -PlanOnly
        }

    $unsafeOutput = Join-Path $testRoot 'unsafe-output'
    New-Item -ItemType Directory -Path $unsafeOutput | Out-Null
    $fixtureInstallerStub = Join-Path $testRoot 'yanami-installer-stub.exe'
    Write-TestPe -Path $fixtureInstallerStub
    Copy-Item -LiteralPath (Join-Path $wrongPrevious `
        'Other-1.0.0-full.nupkg') -Destination $unsafeOutput
    Assert-Throws -Expected 'unsafe cross-app/channel baseline' `
        -Label 'an unrelated nupkg already in outputDir is rejected before vpk scans it' `
        -Action {
            & $scriptPath -PackageRoot $wrappedRoot -Version '0.2.0-dev.15' `
                -OutputDirectory $unsafeOutput `
                -InstallerStubPath $fixtureInstallerStub `
                -ToolCacheDirectory (Join-Path $testRoot 'unsafe-tools')
        }
    Assert-True (-not (Test-Path -LiteralPath `
            (Join-Path $testRoot 'unsafe-tools'))) `
        'unsafe output isolation fails before any tool download'

    $missingStubOutput = Join-Path $testRoot 'missing-stub-output'
    Assert-Throws -Expected 'InstallerStubPath is required' `
        -Label 'a real package requires an explicit installer stub' -Action {
            & $scriptPath -PackageRoot $wrappedRoot -Version '1.0.0' `
                -OutputDirectory $missingStubOutput
        }
    Assert-True (-not (Test-Path -LiteralPath $missingStubOutput)) `
        'a missing installer stub fails before creating release output'

    $invalidInstallerStub = Join-Path $testRoot 'invalid-installer-stub.exe'
    [IO.File]::WriteAllText($invalidInstallerStub, 'not a PE')
    Assert-Throws -Expected 'not a Windows PE executable' `
        -Label 'a non-PE installer stub fails before packaging' -Action {
            & $scriptPath -PackageRoot $wrappedRoot -Version '1.0.0' `
                -OutputDirectory (Join-Path $testRoot 'invalid-stub-output') `
                -InstallerStubPath $invalidInstallerStub
        }

    Assert-Throws -Expected 'must be separate trees' `
        -Label 'output cannot be nested inside the CPack package root' -Action {
            & $scriptPath -PackageRoot $wrappedRoot -Version '0.2.0-dev.15' `
                -OutputDirectory (Join-Path $packageRoot 'Releases') -PlanOnly
        }
    Assert-Throws -Expected 'must be separate trees' `
        -Label 'tool cache cannot be nested inside the CPack package root' `
        -Action {
            & $scriptPath -PackageRoot $wrappedRoot `
                -Version '0.2.0-dev.15' `
                -OutputDirectory (Join-Path $testRoot 'cache-output') `
                -ToolCacheDirectory (Join-Path $packageRoot '.tools') `
                -PlanOnly
        }

    $invalidRoot = Join-Path $testRoot 'invalid'
    New-Item -ItemType Directory -Path (Join-Path $invalidRoot 'bin') `
        -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path (Join-Path $invalidRoot 'bin') `
            'Yanami.exe'),
        'not a PE')
    Assert-Throws -Expected 'not a Windows PE executable' `
        -Label 'a non-PE main entry is rejected before any download' -Action {
            & $scriptPath -PackageRoot $invalidRoot -Version '1.0.0' `
                -OutputDirectory (Join-Path $testRoot 'invalid-output') `
                -PlanOnly
        }

    $importedPlan = . $scriptPath -PackageRoot $wrappedRoot `
        -Version '0.2.0-dev.15' `
        -OutputDirectory (Join-Path $testRoot 'import-output') `
        -InstallerStubPath (Join-Path $testRoot 'import-stub.exe') -PlanOnly
    Assert-Equal $importedPlan.installerContract.footerSize 64 `
        'dot-sourcing exposes the wrapper helper under the planned contract'

    $footerStub = Join-Path $testRoot 'footer-stub.exe'
    $footerBackend = Join-Path $testRoot 'footer-backend.exe'
    $footerWrapper = Join-Path $testRoot 'footer-wrapper.exe'
    $stubBytes = [byte[]]@(0x4d, 0x5a, 0x11, 0x22, 0x33)
    $backendBytes = [byte[]]@(0x4d, 0x5a, 0xaa, 0xbb, 0xcc, 0xdd, 0xee)
    [IO.File]::WriteAllBytes($footerStub, $stubBytes)
    [IO.File]::WriteAllBytes($footerBackend, $backendBytes)
    $wrapperContract = New-YanamiInstallerWrapper `
        -StubPath $footerStub -BackendPath $footerBackend `
        -OutputPath $footerWrapper
    $wrapperBytes = [IO.File]::ReadAllBytes($footerWrapper)
    Assert-Equal $wrapperBytes.Length `
        ($stubBytes.Length + $backendBytes.Length + 64) `
        'wrapper bytes are stub, backend, then the fixed footer'
    Assert-Equal ([Convert]::ToHexString(
            [byte[]]$wrapperBytes[0..($stubBytes.Length - 1)])) `
        ([Convert]::ToHexString($stubBytes)) `
        'the wrapper preserves the stub prefix byte-for-byte'
    $backendStart = $stubBytes.Length
    $backendEnd = $backendStart + $backendBytes.Length - 1
    Assert-Equal ([Convert]::ToHexString(
            [byte[]]$wrapperBytes[$backendStart..$backendEnd])) `
        ([Convert]::ToHexString($backendBytes)) `
        'the canonical backend immediately follows the stub'
    $footerOffset = $wrapperBytes.Length - 64
    Assert-Equal ([Text.Encoding]::ASCII.GetString(
            $wrapperBytes, $footerOffset, 16)) "YANAMI_SETUP_V1`0" `
        'footer bytes 0..15 contain the exact NUL-terminated ASCII magic'
    Assert-Equal (Read-UInt64LittleEndian -Buffer $wrapperBytes `
            -Offset ($footerOffset + 16)) ([uint64]$backendBytes.Length) `
        'footer bytes 16..23 contain the little-endian payload size'
    $footerHash = [Convert]::ToHexString(
        [byte[]]$wrapperBytes[($footerOffset + 24)..($footerOffset + 55)]
    ).ToLowerInvariant()
    $backendHash = (Get-FileHash -LiteralPath $footerBackend `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Equal $footerHash $backendHash `
        'footer bytes 24..55 contain the raw backend SHA-256'
    Assert-Equal (Read-UInt32LittleEndian -Buffer $wrapperBytes `
            -Offset ($footerOffset + 56)) ([uint32]1) `
        'footer bytes 56..59 contain format version 1'
    Assert-Equal (Read-UInt32LittleEndian -Buffer $wrapperBytes `
            -Offset ($footerOffset + 60)) ([uint32]64) `
        'footer bytes 60..63 contain footer size 64'
    Assert-Equal $wrapperContract.payloadSize ([int64]$backendBytes.Length) `
        'the wrapper helper reports the canonical backend size'
    Assert-Equal $wrapperContract.payloadSha256 $backendHash `
        'the wrapper helper reports the canonical backend hash'
    $wrapperBeforeOverwrite = (Get-FileHash -LiteralPath $footerWrapper `
        -Algorithm SHA256).Hash
    Assert-Throws -Expected 'Refusing to overwrite' `
        -Label 'the wrapper helper never overwrites an existing public asset' `
        -Action {
            New-YanamiInstallerWrapper -StubPath $footerStub `
                -BackendPath $footerBackend -OutputPath $footerWrapper
        }
    Assert-Equal (Get-FileHash -LiteralPath $footerWrapper `
            -Algorithm SHA256).Hash $wrapperBeforeOverwrite `
        'a refused overwrite leaves the existing wrapper unchanged'

    $nameOnlyManifest = Join-Path $testRoot 'assets-name-only.json'
    [IO.File]::WriteAllText($nameOnlyManifest,
        '[{"RelativeFileName":"canonical-Setup.exe","Type":"Installer"}]',
        [Text.UTF8Encoding]::new($false))
    Update-VpkAssetManifestSetupAsset -Path $nameOnlyManifest `
        -OriginalName 'canonical-Setup.exe' `
        -PublishedName 'Yanami-versioned-Setup.exe' `
        -PublishedFilePath $footerWrapper
    $nameOnlyEntry = @(Get-Content -LiteralPath $nameOnlyManifest -Raw |
        ConvertFrom-Json)[0]
    Assert-Equal $nameOnlyEntry.RelativeFileName `
        'Yanami-versioned-Setup.exe' `
        'a name-only vpk Installer manifest points to the public wrapper'
    Assert-True ($null -eq $nameOnlyEntry.PSObject.Properties['Size'] -and
        $null -eq $nameOnlyEntry.PSObject.Properties['SHA256']) `
        'the current name-only vpk Installer schema remains name-only'

    $integrityManifest = Join-Path $testRoot 'assets-integrity.json'
    [IO.File]::WriteAllText($integrityManifest,
        '[{"RelativeFileName":"canonical-Setup.exe","Type":"Installer","Size":1,"SHA256":"00"}]',
        [Text.UTF8Encoding]::new($false))
    Update-VpkAssetManifestSetupAsset -Path $integrityManifest `
        -OriginalName 'canonical-Setup.exe' `
        -PublishedName 'Yanami-versioned-Setup.exe' `
        -PublishedFilePath $footerWrapper
    $integrityEntry = @(Get-Content -LiteralPath $integrityManifest -Raw |
        ConvertFrom-Json)[0]
    Assert-Equal ([int64]$integrityEntry.Size) `
        ([IO.FileInfo]::new($footerWrapper).Length) `
        'an optional vpk Installer size is updated to the wrapper size'
    Assert-Equal ([string]$integrityEntry.SHA256) `
        $wrapperBeforeOverwrite.ToLowerInvariant() `
        'an optional vpk Installer SHA-256 is updated to the wrapper hash'

    $source = Get-Content -LiteralPath $scriptPath -Raw
    $updaterSource = Get-Content -LiteralPath ([IO.Path]::Combine(
            $workspace, 'apps', 'desktop', 'updater',
            'VelopackUpdater.cpp')) -Raw
    Assert-Contains $source `
        "Invoke-NativeVelopackHookSmoke -StagingRoot `$stagingPath" `
        'the native install-hook smoke is a mandatory pre-pack step'
    Assert-Contains $source "ArgumentList.Add('--veloapp-install')" `
        'the native smoke invokes the official install hook argument'
    Assert-Contains $source 'Assert-PackageManifest -Root $stagingPath' `
        'the hook smoke cannot mutate staged package bytes'
    Assert-Contains $source 'Assert-ReleaseDirectoryIsolation' `
        'the vpk 1.2.0 cross-app delta baseline hazard is gated'
    Assert-Contains $source 'Update-VpkAssetManifestSetupAsset' `
        'wrapping Setup.exe keeps name and optional integrity fields coherent'
    Assert-Contains $source '[IO.File]::Move($temporaryPath, $OutputPath)' `
        'the completed wrapper is atomically published from the same directory'
    Assert-Contains $source 'Invoke-InstallerWrapperVerification' `
        'the public wrapper must self-verify its embedded backend'
    Assert-Contains $source 'Remove-ExactGeneratedInstaller -Path $vpkSetupPath' `
        'the canonical backend is removed from public release output'
    Assert-Contains $source "'velopack_libc.dll'" `
        'the native updater runtime DLL is a required root payload'
    Assert-Contains $source "'lib/app/licenses/velopack/MIT.txt'" `
        'the Velopack runtime license is required inside the nupkg'
    Assert-Contains $updaterSource 'JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE' `
        'cancelling a delta download terminates the inherited patch tree'
    Assert-Contains $updaterSource `
        'AssignProcessToJobObject(job, GetCurrentProcess())' `
        'the update helper joins its process-tree job before downloading'
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        $testFull = [IO.Path]::GetFullPath($testRoot)
        $testPrefix = $testParent.TrimEnd(
            [IO.Path]::DirectorySeparatorChar) +
            [IO.Path]::DirectorySeparatorChar
        if ($testFull.StartsWith($testPrefix,
                [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $testFull).StartsWith(
                'yanami-velopack-test-',
                [StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $testFull -Recurse -Force
        }
    }
}

Write-Host "Windows Velopack packaging tests passed ($assertions assertions)."
