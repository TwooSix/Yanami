[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$PackageRoot,

    [Parameter(Mandatory)]
    [ValidatePattern('^(?!0\.0\.0(?:-|$))(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$')]
    [string]$Version,

    [Parameter(Mandatory)]
    [string]$OutputDirectory,

    [string]$PreviousReleaseDirectory,

    [ValidatePattern('^[a-z0-9][a-z0-9._-]*$')]
    [string]$Channel = "preview",

    [ValidateSet("BestSpeed", "BestSize")]
    [string]$DeltaMode = "BestSpeed",

    [string]$IconPath,

    [string]$ToolCacheDirectory,

    [string]$VpkPackagePath,

    [string]$InstallerStubPath,

    [switch]$PlanOnly,

    [switch]$KeepStagingDirectory
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$vpkVersion = "1.2.0"
$vpkPackageUri =
    "https://github.com/velopack/velopack/releases/download/$vpkVersion/vpk.$vpkVersion.nupkg"
$vpkPackageSha256 =
    "3e458a676be46d1122e522312db18411f36ea8c70e586f81a676695d43f89dbc"
$vpkDllSha256 =
    "d3d23db82ef3e24991656a8b821ebcb8301fad47ff456b4336e42eee62f70a1f"
$dotnetRuntimeVersion = "10.0.0"
$dotnetRuntimeUri =
    "https://builds.dotnet.microsoft.com/dotnet/Runtime/$dotnetRuntimeVersion/dotnet-runtime-$dotnetRuntimeVersion-win-x64.zip"
$dotnetRuntimeSha512 =
    "4f785ed52d49545e328d7526fd6fe80e2d0c84bc90c5f905df631ef87b9d3efd211d10670aa87d5e7b87cd2f54d3dedb6e5be78027d5533153670e1186230825"
$dotnetExeSha256 =
    "b74c001a6f6f5c979f88eda19a8f782346ee17ecea6add3647ad05f5c6e2b8bd"
$packageId = "io.github.TwooSix.Yanami"
$mainExe = "Yanami.exe"
$installerFooterMagic = "YANAMI_SETUP_V1`0"
$installerFooterFormat = [uint32]1
$installerFooterSize = [uint32]64
$installerVerifyArgument = '--verify-payload'
$requiredRootPayload = @(
    'Yanami.exe',
    'yanami-updater.exe',
    'yanami-desktop.exe',
    'yanami_desktop_bridge.dll',
    'velopack_libc.dll'
)

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($IconPath)) {
    $IconPath = [IO.Path]::Combine(
        $workspace, 'apps', 'desktop', 'resources', 'windows', 'yanami.ico')
}
if ([string]::IsNullOrWhiteSpace($ToolCacheDirectory)) {
    $ToolCacheDirectory = [IO.Path]::Combine(
        $workspace, 'build', 'tools', 'velopack', $vpkVersion)
}

function Resolve-RequiredDirectory {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label does not exist or is not a directory: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label does not exist or is not a file: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Test-PeFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label
    )

    $stream = [IO.File]::OpenRead($Path)
    try {
        if ($stream.Length -lt 2 -or $stream.ReadByte() -ne 0x4d -or
            $stream.ReadByte() -ne 0x5a) {
            throw "$Label is not a Windows PE executable: $Path"
        }
    } finally {
        $stream.Dispose()
    }
}

function Set-UInt64LittleEndian {
    param(
        [Parameter(Mandatory)][byte[]]$Buffer,
        [Parameter(Mandatory)][int]$Offset,
        [Parameter(Mandatory)][uint64]$Value
    )

    for ($index = 0; $index -lt 8; $index++) {
        $Buffer[$Offset + $index] = [byte](
            ($Value -shr (8 * $index)) -band 0xff)
    }
}

function Set-UInt32LittleEndian {
    param(
        [Parameter(Mandatory)][byte[]]$Buffer,
        [Parameter(Mandatory)][int]$Offset,
        [Parameter(Mandatory)][uint32]$Value
    )

    for ($index = 0; $index -lt 4; $index++) {
        $Buffer[$Offset + $index] = [byte](
            ($Value -shr (8 * $index)) -band 0xff)
    }
}

function New-YanamiInstallerWrapper {
    param(
        [Parameter(Mandatory)][string]$StubPath,
        [Parameter(Mandatory)][string]$BackendPath,
        [Parameter(Mandatory)][string]$OutputPath
    )

    if (Test-Path -LiteralPath $OutputPath) {
        throw "Refusing to overwrite an existing installer wrapper: $OutputPath"
    }
    $outputParent = Split-Path -Parent $OutputPath
    if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
        throw "Installer wrapper output directory does not exist: $outputParent"
    }

    Test-PeFile -Path $StubPath -Label 'Yanami installer stub'
    Test-PeFile -Path $BackendPath -Label 'Canonical Velopack Setup.exe backend'

    $magicBytes = [Text.Encoding]::ASCII.GetBytes($installerFooterMagic)
    if ($magicBytes.Length -ne 16 -or $installerFooterSize -ne 64) {
        throw 'Internal Yanami installer footer constants are invalid.'
    }

    $temporaryPath = Join-Path $outputParent (
        ".$(Split-Path -Leaf $OutputPath).$([Guid]::NewGuid().ToString('N')).tmp")
    $published = $false
    $backendStream = $null
    $stubStream = $null
    $outputStream = $null
    $sha256 = $null
    try {
        $backendStream = [IO.File]::Open(
            $BackendPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::Read)
        $payloadSize = [uint64]$backendStream.Length
        $sha256 = [Security.Cryptography.SHA256]::Create()
        $payloadHashBytes = $sha256.ComputeHash($backendStream)
        $payloadHash = [Convert]::ToHexString(
            $payloadHashBytes).ToLowerInvariant()
        $backendStream.Position = 0

        $stubStream = [IO.File]::Open(
            $StubPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::Read)
        $outputStream = [IO.File]::Open(
            $temporaryPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        $stubStream.CopyTo($outputStream)
        $backendStream.CopyTo($outputStream)

        $footer = [byte[]]::new($installerFooterSize)
        [Array]::Copy($magicBytes, 0, $footer, 0, $magicBytes.Length)
        Set-UInt64LittleEndian -Buffer $footer -Offset 16 -Value $payloadSize
        [Array]::Copy($payloadHashBytes, 0, $footer, 24,
            $payloadHashBytes.Length)
        Set-UInt32LittleEndian -Buffer $footer -Offset 56 `
            -Value $installerFooterFormat
        Set-UInt32LittleEndian -Buffer $footer -Offset 60 `
            -Value $installerFooterSize
        $outputStream.Write($footer, 0, $footer.Length)
        $outputStream.Flush($true)
        $outputStream.Dispose()
        $outputStream = $null

        Test-PeFile -Path $temporaryPath -Label 'Yanami installer wrapper'
        [IO.File]::Move($temporaryPath, $OutputPath)
        $published = $true
        return [pscustomobject]@{
            payloadSize = [int64]$payloadSize
            payloadSha256 = $payloadHash
            footerSize = [int]$installerFooterSize
            format = [int]$installerFooterFormat
        }
    } finally {
        if ($null -ne $outputStream) {
            $outputStream.Dispose()
        }
        if ($null -ne $stubStream) {
            $stubStream.Dispose()
        }
        if ($null -ne $backendStream) {
            $backendStream.Dispose()
        }
        if ($null -ne $sha256) {
            $sha256.Dispose()
        }
        if (-not $published -and (Test-Path -LiteralPath $temporaryPath)) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Invoke-InstallerWrapperVerification {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$BackendPath,
        [int]$TimeoutMilliseconds = 30000
    )

    $backend = Get-Item -LiteralPath $BackendPath
    $expectedHash = (Get-FileHash -LiteralPath $backend.FullName `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Path
    $startInfo.ArgumentList.Add($installerVerifyArgument)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw 'Installer wrapper verification process did not start.'
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            try {
                $process.Kill($true)
            } catch {
                $process.Kill()
            }
            $process.WaitForExit()
            throw "Installer wrapper verification timed out after $TimeoutMilliseconds ms."
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) {
            throw "Installer wrapper verification failed with exit code $($process.ExitCode): $stderr"
        }
        try {
            $verification = $stdout | ConvertFrom-Json
        } catch {
            throw "Installer wrapper verification returned invalid JSON: $stdout"
        }
        $sizeProperty = $verification.PSObject.Properties['payloadSize']
        $hashProperty = $verification.PSObject.Properties['payloadSha256']
        if ($null -eq $sizeProperty -or $null -eq $hashProperty) {
            throw 'Installer wrapper verification JSON must contain payloadSize and payloadSha256.'
        }
        if ([int64]$sizeProperty.Value -ne $backend.Length -or
            ([string]$hashProperty.Value).ToLowerInvariant() -cne $expectedHash) {
            throw 'Installer wrapper verification hash/size does not match the canonical Velopack backend.'
        }
        return $verification
    } finally {
        $process.Dispose()
    }
}

function Remove-ExactGeneratedInstaller {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$OutputRoot,
        [Parameter(Mandatory)][string]$ExpectedName
    )

    $resolvedTarget = [IO.Path]::GetFullPath($Path)
    $expectedTarget = [IO.Path]::GetFullPath(
        (Join-Path $OutputRoot $ExpectedName))
    if (-not $resolvedTarget.Equals($expectedTarget,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an unexpected installer path: $resolvedTarget"
    }
    if (Test-Path -LiteralPath $resolvedTarget -PathType Leaf) {
        Remove-Item -LiteralPath $resolvedTarget -Force
    }
}

function Resolve-PackageLayout {
    param([Parameter(Mandatory)][string]$Root)

    $candidates = [Collections.Generic.List[object]]::new()
    $candidates.Add([pscustomobject]@{ root = $Root; wrapped = $false })
    $children = @(Get-ChildItem -LiteralPath $Root -Force)
    if ($children.Count -eq 1 -and $children[0].PSIsContainer) {
        $candidates.Add([pscustomobject]@{
            root = $children[0].FullName
            wrapped = $true
        })
    }

    foreach ($candidate in $candidates) {
        $flatMain = Join-Path $candidate.root $mainExe
        if (Test-Path -LiteralPath $flatMain -PathType Leaf) {
            foreach ($required in $requiredRootPayload) {
                $requiredPath = Join-Path $candidate.root $required
                if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
                    throw "Windows package payload is missing: $required"
                }
                Test-PeFile -Path $requiredPath `
                    -Label "Windows package payload '$required'"
            }
            $velopackLicense = [IO.Path]::Combine(
                $candidate.root, 'licenses', 'velopack', 'MIT.txt')
            if (-not (Test-Path -LiteralPath $velopackLicense -PathType Leaf)) {
                throw "Windows package payload is missing: licenses/velopack/MIT.txt"
            }
            return [pscustomobject]@{
                root = $candidate.root
                mainExe = $flatMain
                flattenBin = $false
                wrapped = $candidate.wrapped
            }
        }

        $binRoot = Join-Path $candidate.root 'bin'
        $nestedMain = Join-Path $binRoot $mainExe
        if (Test-Path -LiteralPath $nestedMain -PathType Leaf) {
            foreach ($required in $requiredRootPayload) {
                $requiredPath = Join-Path $binRoot $required
                if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
                    throw "Windows package payload is missing: bin\$required"
                }
                Test-PeFile -Path $requiredPath `
                    -Label "Windows package payload 'bin\$required'"
            }
            $velopackLicense = [IO.Path]::Combine(
                $candidate.root, 'licenses', 'velopack', 'MIT.txt')
            if (-not (Test-Path -LiteralPath $velopackLicense -PathType Leaf)) {
                throw "Windows package payload is missing: licenses/velopack/MIT.txt"
            }
            return [pscustomobject]@{
                root = $candidate.root
                mainExe = $nestedMain
                flattenBin = $true
                wrapped = $candidate.wrapped
            }
        }
    }

    throw "PackageRoot must contain $mainExe or bin\$mainExe (optionally under one CPack wrapper directory): $Root"
}

function Get-AssetNames {
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$ReleaseVersion,
        [Parameter(Mandatory)][string]$ReleaseChannel
    )

    $packageSuffix = if ($ReleaseChannel -eq "win") { "" } else {
        "-$ReleaseChannel"
    }
    return [pscustomobject]@{
        vpkSetup = "$Id-$ReleaseChannel-Setup.exe"
        setup = "Yanami-$ReleaseVersion-Windows-x86_64-Setup.exe"
        full = "$Id-$ReleaseVersion$packageSuffix-full.nupkg"
        delta = "$Id-$ReleaseVersion$packageSuffix-delta.nupkg"
        feed = "releases.$ReleaseChannel.json"
        assetManifest = "assets.$ReleaseChannel.json"
        legacyFeed = if ($ReleaseChannel -eq "win") {
            "RELEASES"
        } else {
            "RELEASES-$ReleaseChannel"
        }
        hashes = "SHA256SUMS.velopack-$ReleaseChannel.txt"
    }
}

function Assert-SeparateDirectoryTrees {
    param(
        [Parameter(Mandatory)][string]$FirstPath,
        [Parameter(Mandatory)][string]$SecondPath,
        [Parameter(Mandatory)][string]$FirstLabel,
        [Parameter(Mandatory)][string]$SecondLabel
    )

    $firstFull = [IO.Path]::GetFullPath($FirstPath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $secondFull = [IO.Path]::GetFullPath($SecondPath).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $firstPrefix = $firstFull + [IO.Path]::DirectorySeparatorChar
    $secondPrefix = $secondFull + [IO.Path]::DirectorySeparatorChar
    if ($firstFull.Equals($secondFull,
            [StringComparison]::OrdinalIgnoreCase) -or
        $secondFull.StartsWith($firstPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        $firstFull.StartsWith($secondPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$FirstLabel and $SecondLabel must be separate trees."
    }
}

function Read-NupkgMetadata {
    param([Parameter(Mandatory)][string]$Path)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $nuspecEntries = @($archive.Entries | Where-Object {
            $_.FullName -notmatch '[/\\]' -and
            $_.FullName.EndsWith('.nuspec', [StringComparison]::OrdinalIgnoreCase)
        })
        if ($nuspecEntries.Count -ne 1) {
            throw "Expected one root .nuspec in $Path, found $($nuspecEntries.Count)."
        }

        $reader = [IO.StreamReader]::new($nuspecEntries[0].Open())
        try {
            [xml]$nuspec = $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }

        $metadata = $nuspec.SelectSingleNode(
            "/*[local-name()='package']/*[local-name()='metadata']")
        if ($null -eq $metadata) {
            throw "The package has no NuGet metadata: $Path"
        }

        $readValue = {
            param([string]$Name)
            $node = $metadata.SelectSingleNode("*[local-name()='$Name']")
            if ($null -eq $node) { return "" }
            return $node.InnerText
        }

        $channelValue = & $readValue "channel"
        if ([string]::IsNullOrWhiteSpace($channelValue)) {
            $channelValue = "win"
        }

        return [pscustomobject]@{
            id = & $readValue "id"
            version = & $readValue "version"
            channel = $channelValue
            mainExe = & $readValue "mainExe"
        }
    } finally {
        $archive.Dispose()
    }
}

function Get-MatchingReleasePackages {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$ReleaseChannel,
        [string]$BeforeVersion
    )

    $matches = [Collections.Generic.List[object]]::new()
    foreach ($package in Get-ChildItem -LiteralPath $Directory -File -Filter '*.nupkg') {
        if (-not ($package.Name.EndsWith('-full.nupkg',
                    [StringComparison]::OrdinalIgnoreCase) -or
                $package.Name.EndsWith('-delta.nupkg',
                    [StringComparison]::OrdinalIgnoreCase))) {
            continue
        }
        $metadata = Read-NupkgMetadata -Path $package.FullName
        if ($metadata.id -ceq $Id -and
            $metadata.channel -ceq $ReleaseChannel) {
            if (-not [string]::IsNullOrWhiteSpace($BeforeVersion)) {
                $candidateVersion =
                    [Management.Automation.SemanticVersion]::new(
                        $metadata.version)
                $targetVersion =
                    [Management.Automation.SemanticVersion]::new(
                        $BeforeVersion)
                if ($candidateVersion -ge $targetVersion) {
                    throw "Velopack baseline '$($package.Name)' must be older than $BeforeVersion."
                }
            }
            $matches.Add([pscustomobject]@{
                file = $package
                metadata = $metadata
                isFull = $package.Name.EndsWith('-full.nupkg',
                    [StringComparison]::OrdinalIgnoreCase)
            })
        }
    }
    return @($matches)
}

function Copy-PreviousReleases {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$ReleaseChannel,
        [Parameter(Mandatory)][string]$CurrentVersion
    )

    $packages = @(Get-MatchingReleasePackages -Directory $Source -Id $Id `
        -ReleaseChannel $ReleaseChannel -BeforeVersion $CurrentVersion)
    if (@($packages | Where-Object isFull).Count -eq 0) {
        throw "PreviousReleaseDirectory contains no $Id/$ReleaseChannel full nupkg: $Source"
    }

    $sourcePath = [IO.Path]::GetFullPath($Source).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $destinationPath = [IO.Path]::GetFullPath($Destination).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    if ($sourcePath.Equals($destinationPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        return $packages
    }

    foreach ($package in $packages) {
        $target = Join-Path $Destination $package.file.Name
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $sourceHash = (Get-FileHash -LiteralPath $package.file.FullName `
                -Algorithm SHA256).Hash
            $targetHash = (Get-FileHash -LiteralPath $target `
                -Algorithm SHA256).Hash
            if ($sourceHash -cne $targetHash) {
                throw "A different baseline asset already exists: $target"
            }
            continue
        }
        Copy-Item -LiteralPath $package.file.FullName -Destination $target
    }
    return $packages
}

function Assert-ReleaseDirectoryIsolation {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$ReleaseChannel,
        [Parameter(Mandatory)][string]$CurrentVersion
    )

    foreach ($package in Get-ChildItem -LiteralPath $Directory -File `
        -Filter '*.nupkg') {
        $metadata = Read-NupkgMetadata -Path $package.FullName
        if ($metadata.id -cne $Id -or
            $metadata.channel -cne $ReleaseChannel) {
            throw "Velopack output contains an unsafe cross-app/channel baseline '$($package.Name)' ($($metadata.id)/$($metadata.channel)); vpk 1.2.0 does not isolate delta baselines by packId."
        }
        $candidateVersion = [Management.Automation.SemanticVersion]::new(
            $metadata.version)
        $targetVersion = [Management.Automation.SemanticVersion]::new(
            $CurrentVersion)
        if ($candidateVersion -ge $targetVersion) {
            throw "Velopack output baseline '$($package.Name)' must be older than $CurrentVersion."
        }
    }
}

function Copy-PackageToVelopackStage {
    param(
        [Parameter(Mandatory)][pscustomobject]$Layout,
        [Parameter(Mandatory)][string]$Destination
    )

    New-Item -ItemType Directory -Path $Destination | Out-Null
    if (-not $Layout.flattenBin) {
        foreach ($item in Get-ChildItem -LiteralPath $Layout.root -Force) {
            Copy-Item -LiteralPath $item.FullName -Destination $Destination `
                -Recurse
        }
    } else {
        foreach ($item in Get-ChildItem -LiteralPath $Layout.root -Force) {
            if ($item.Name.Equals('bin', [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            Copy-Item -LiteralPath $item.FullName -Destination $Destination `
                -Recurse
        }

        $binRoot = Join-Path $Layout.root 'bin'
        foreach ($item in Get-ChildItem -LiteralPath $binRoot -Force) {
            $target = Join-Path $Destination $item.Name
            if (Test-Path -LiteralPath $target) {
                throw "Cannot flatten bin because the package root already contains '$($item.Name)'."
            }
            Copy-Item -LiteralPath $item.FullName -Destination $target -Recurse
        }
    }

    foreach ($required in $requiredRootPayload) {
        $stagedPath = Join-Path $Destination $required
        if (-not (Test-Path -LiteralPath $stagedPath -PathType Leaf)) {
            throw "Velopack staging did not place $required at the package root."
        }
        Test-PeFile -Path $stagedPath `
            -Label "Velopack staged payload '$required'"
        $nestedPath = Join-Path (Join-Path $Destination 'bin') $required
        if (Test-Path -LiteralPath $nestedPath) {
            throw "Velopack staging retained a nested bin\$required entry."
        }
    }
    $stagedVelopackLicense = [IO.Path]::Combine(
        $Destination, 'licenses', 'velopack', 'MIT.txt')
    if (-not (Test-Path -LiteralPath $stagedVelopackLicense -PathType Leaf)) {
        throw "Velopack staging is missing licenses/velopack/MIT.txt."
    }
}

function Write-PackageManifest {
    param([Parameter(Mandatory)][string]$Root)

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $manifestPath = Join-Path $rootPath 'SHA256SUMS.txt'
    $files = @(Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force |
        Where-Object {
            $_.FullName -cne $manifestPath -and
            -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)
        } |
        ForEach-Object {
            $relative = $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
            [pscustomobject]@{ file = $_; relative = $relative }
        } |
        Sort-Object relative)
    if ($files.Count -eq 0) {
        throw "Velopack staging contains no payload files: $Root"
    }

    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('# SHA-256  Size  Relative path')
    foreach ($entry in $files) {
        $hash = (Get-FileHash -LiteralPath $entry.file.FullName `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines.Add("$hash  $($entry.file.Length)  $($entry.relative)")
    }
    $content = [string]::Join("`n", $lines) + "`n"
    [IO.File]::WriteAllText($manifestPath, $content,
        [Text.UTF8Encoding]::new($false))
}

function Assert-PackageManifest {
    param([Parameter(Mandatory)][string]$Root)

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $rootPrefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    $manifestPath = Join-Path $rootPath 'SHA256SUMS.txt'
    $manifestFiles = [Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $manifestPath) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('#')) {
            continue
        }
        if ($line -notmatch '^([0-9a-fA-F]{64})  ([0-9]+)  (.+)$') {
            throw "Malformed staged package manifest entry: $line"
        }
        $relative = $Matches[3]
        if ([IO.Path]::IsPathRooted($relative) -or
            $relative.Contains('\') -or
            $relative -match '(^|/)\.\.(/|$)') {
            throw "Unsafe staged package manifest path: $relative"
        }
        $fullPath = [IO.Path]::GetFullPath((Join-Path $rootPath `
            $relative.Replace('/', [IO.Path]::DirectorySeparatorChar)))
        if (-not $fullPath.StartsWith($rootPrefix,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            throw "Staged package manifest path is missing or escapes its root: $relative"
        }
        $file = Get-Item -LiteralPath $fullPath
        $actualHash = (Get-FileHash -LiteralPath $fullPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -cne $Matches[1].ToLowerInvariant() -or
            $file.Length -ne [int64]$Matches[2]) {
            throw "Native Velopack hook smoke modified the staged payload: $relative"
        }
        if ($manifestFiles.Contains($relative)) {
            throw "Duplicate staged package manifest path: $relative"
        }
        $manifestFiles.Add($relative)
    }

    $payloadFiles = @(Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force |
        Where-Object {
            $_.FullName -cne $manifestPath -and
            -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)
        } |
        ForEach-Object {
            $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
        } |
        Sort-Object)
    $manifestArray = @($manifestFiles | Sort-Object)
    if ([string]::Join("`n", $payloadFiles) -cne
        [string]::Join("`n", $manifestArray)) {
        throw "Native Velopack hook smoke changed staged payload coverage."
    }
}

function Get-StagingProcesses {
    param([Parameter(Mandatory)][string]$Root)

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar)
    $rootPrefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    $matches = [Collections.Generic.List[Diagnostics.Process]]::new()
    foreach ($process in Get-Process -Name 'Yanami', 'yanami-desktop', `
        'yanami-updater', 'Update' `
        -ErrorAction SilentlyContinue) {
        try {
            $processPath = $process.Path
            if (-not [string]::IsNullOrWhiteSpace($processPath) -and
                [IO.Path]::GetFullPath($processPath).StartsWith(
                    $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                $matches.Add($process)
            }
        } catch {
            # An unrelated protected process is outside the unique staging
            # directory and cannot be part of this smoke invocation.
        }
    }
    return @($matches)
}

function Stop-StagingProcesses {
    param([Parameter(Mandatory)][string]$Root)

    foreach ($process in Get-StagingProcesses -Root $Root) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-NativeVelopackHookSmoke {
    param(
        [Parameter(Mandatory)][string]$StagingRoot,
        [Parameter(Mandatory)][string]$ReleaseVersion
    )

    $entry = Join-Path $StagingRoot $mainExe
    $existing = @(Get-StagingProcesses -Root $StagingRoot)
    if ($existing.Count -ne 0) {
        throw "Velopack staging already has a running Yanami process."
    }

    $profileParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $profileRoot = Join-Path $profileParent `
        ("yanami-velopack-hook-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $profileRoot | Out-Null
    $process = $null
    try {
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $entry
        $startInfo.WorkingDirectory = $StagingRoot
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.ArgumentList.Add('--veloapp-install')
        $startInfo.ArgumentList.Add($ReleaseVersion)
        $startInfo.Environment['YANAMI_ISOLATED_PROFILE_ROOT'] =
            (Join-Path $profileRoot 'Profile')
        $startInfo.Environment['APPDATA'] = Join-Path $profileRoot 'Roaming'
        $startInfo.Environment['LOCALAPPDATA'] = Join-Path $profileRoot 'Local'

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Could not start the native Velopack hook smoke."
        }
        if (-not $process.WaitForExit(15000)) {
            try { $process.Kill($true) } catch {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
            $process.WaitForExit()
            throw "$mainExe --veloapp-install did not exit within 15 seconds."
        }
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0) {
            throw "$mainExe --veloapp-install exited $($process.ExitCode). stdout: $stdout stderr: $stderr"
        }

        Start-Sleep -Milliseconds 250
        $leftovers = @(Get-StagingProcesses -Root $StagingRoot)
        if ($leftovers.Count -ne 0) {
            Stop-StagingProcesses -Root $StagingRoot
            throw "$mainExe --veloapp-install launched a persistent desktop process instead of handling the hook."
        }
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
        Stop-StagingProcesses -Root $StagingRoot
        if (Test-Path -LiteralPath $profileRoot) {
            $profileFull = [IO.Path]::GetFullPath($profileRoot)
            $profilePrefix = $profileParent.TrimEnd(
                [IO.Path]::DirectorySeparatorChar) +
                [IO.Path]::DirectorySeparatorChar
            if ($profileFull.StartsWith($profilePrefix,
                    [StringComparison]::OrdinalIgnoreCase) -and
                (Split-Path -Leaf $profileFull).StartsWith(
                    'yanami-velopack-hook-',
                    [StringComparison]::Ordinal)) {
                Remove-Item -LiteralPath $profileFull -Recurse -Force
            }
        }
    }
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][ValidateSet('SHA256', 'SHA512')]
        [string]$Algorithm,
        [Parameter(Mandatory)][string]$ExpectedHash,
        [Parameter(Mandatory)][string]$Label
    )

    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $actual = (Get-FileHash -LiteralPath $Destination `
            -Algorithm $Algorithm).Hash.ToLowerInvariant()
        if ($actual -cne $ExpectedHash) {
            throw "$Label cache hash mismatch; refusing to use or replace: $Destination"
        }
        return $Destination
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) `
        -Force | Out-Null
    $temporary = "$Destination.download-$([Guid]::NewGuid().ToString('N'))"
    try {
        Write-Host "Downloading pinned $Label..."
        Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $temporary
        $actual = (Get-FileHash -LiteralPath $temporary `
            -Algorithm $Algorithm).Hash.ToLowerInvariant()
        if ($actual -cne $ExpectedHash) {
            throw "$Label download hash mismatch: expected $ExpectedHash, got $actual"
        }
        Move-Item -LiteralPath $temporary -Destination $Destination
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    return $Destination
}

function Expand-ToolArchive {
    param(
        [Parameter(Mandatory)][string]$Archive,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$RequiredFile,
        [Parameter(Mandatory)][string]$RequiredSha256,
        [Parameter(Mandatory)][string]$Label
    )

    $requiredPath = Join-Path $Destination $RequiredFile
    if (Test-Path -LiteralPath $requiredPath -PathType Leaf) {
        $actual = (Get-FileHash -LiteralPath $requiredPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -cne $RequiredSha256) {
            throw "$Label extracted cache hash mismatch: $requiredPath"
        }
        return $requiredPath
    }
    if (Test-Path -LiteralPath $Destination) {
        throw "$Label cache is incomplete; remove this versioned cache and retry: $Destination"
    }
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $temporary = "$Destination.extract-$([Guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Path $temporary | Out-Null
        Expand-Archive -LiteralPath $Archive -DestinationPath $temporary
        $temporaryRequired = Join-Path $temporary $RequiredFile
        if (-not (Test-Path -LiteralPath $temporaryRequired -PathType Leaf)) {
            throw "$Label archive did not contain $RequiredFile."
        }
        $actual = (Get-FileHash -LiteralPath $temporaryRequired `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -cne $RequiredSha256) {
            throw "$Label extracted executable hash mismatch."
        }
        Move-Item -LiteralPath $temporary -Destination $Destination
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            $temporaryFull = [IO.Path]::GetFullPath($temporary)
            $parentPrefix = [IO.Path]::GetFullPath($parent).TrimEnd(
                [IO.Path]::DirectorySeparatorChar) +
                [IO.Path]::DirectorySeparatorChar
            if ($temporaryFull.StartsWith($parentPrefix,
                    [StringComparison]::OrdinalIgnoreCase) -and
                (Split-Path -Leaf $temporaryFull).StartsWith(
                    ((Split-Path -Leaf $Destination) + '.extract-'),
                    [StringComparison]::Ordinal)) {
                Remove-Item -LiteralPath $temporaryFull -Recurse -Force
            }
        }
    }
    return $requiredPath
}

function Resolve-VpkTool {
    param(
        [Parameter(Mandatory)][string]$CacheRoot,
        [string]$SuppliedPackage
    )

    $cachePath = [IO.Path]::GetFullPath($CacheRoot)
    New-Item -ItemType Directory -Path $cachePath -Force | Out-Null
    if ([string]::IsNullOrWhiteSpace($SuppliedPackage)) {
        $packagePath = Join-Path $cachePath "vpk.$vpkVersion.nupkg"
        $packagePath = Get-VerifiedDownload -Uri $vpkPackageUri `
            -Destination $packagePath -Algorithm SHA256 `
            -ExpectedHash $vpkPackageSha256 -Label "vpk $vpkVersion"
    } else {
        $packagePath = Resolve-RequiredFile -Path $SuppliedPackage `
            -Label "VpkPackagePath"
        $actual = (Get-FileHash -LiteralPath $packagePath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -cne $vpkPackageSha256) {
            throw "VpkPackagePath is not the pinned vpk $vpkVersion package (SHA-256 mismatch)."
        }
    }

    $vpkDll = Expand-ToolArchive -Archive $packagePath `
        -Destination (Join-Path $cachePath 'package') `
        -RequiredFile 'tools/net10.0/any/vpk.dll' `
        -RequiredSha256 $vpkDllSha256 `
        -Label "vpk $vpkVersion"
    $productVersion = (Get-Item -LiteralPath $vpkDll).VersionInfo.ProductVersion
    if ($productVersion -notmatch '^1\.2\.0(?:\+|$)') {
        throw "Extracted vpk has unexpected product version '$productVersion'."
    }

    $runtimeArchive = Join-Path $cachePath `
        "dotnet-runtime-$dotnetRuntimeVersion-win-x64.zip"
    $runtimeArchive = Get-VerifiedDownload -Uri $dotnetRuntimeUri `
        -Destination $runtimeArchive -Algorithm SHA512 `
        -ExpectedHash $dotnetRuntimeSha512 `
        -Label ".NET runtime $dotnetRuntimeVersion"
    $dotnetExe = Expand-ToolArchive -Archive $runtimeArchive `
        -Destination (Join-Path $cachePath "dotnet-$dotnetRuntimeVersion-win-x64") `
        -RequiredFile 'dotnet.exe' -RequiredSha256 $dotnetExeSha256 `
        -Label ".NET runtime $dotnetRuntimeVersion"

    & $dotnetExe $vpkDll -h *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Pinned vpk $vpkVersion cache failed its executable smoke check."
    }

    return [pscustomobject]@{
        dotnet = $dotnetExe
        vpk = $vpkDll
        version = $productVersion
        packageSha256 = $vpkPackageSha256
    }
}

function Assert-ZipEntry {
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)][string]$Entry,
        [switch]$Absent
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $found = $null -ne $archive.GetEntry($Entry)
    } finally {
        $archive.Dispose()
    }
    if ($Absent -and $found) {
        throw "Archive unexpectedly contains '$Entry': $ArchivePath"
    }
    if (-not $Absent -and -not $found) {
        throw "Archive is missing '$Entry': $ArchivePath"
    }
}

function Assert-ReleasePackage {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$ReleaseVersion,
        [Parameter(Mandatory)][string]$ReleaseChannel,
        [switch]$VerifyMainEntry
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "Velopack release asset is missing or empty: $Path"
    }
    $metadata = Read-NupkgMetadata -Path $Path
    if ($metadata.id -cne $Id -or $metadata.version -cne $ReleaseVersion -or
        $metadata.channel -cne $ReleaseChannel) {
        throw "Unexpected nupkg metadata in $Path."
    }
    if ($VerifyMainEntry) {
        if ($metadata.mainExe -cne $mainExe) {
            throw "Velopack mainExe must be the file name '$mainExe', got '$($metadata.mainExe)'."
        }
        Assert-ZipEntry -ArchivePath $Path -Entry "lib/app/$mainExe"
        foreach ($required in $requiredRootPayload) {
            Assert-ZipEntry -ArchivePath $Path -Entry "lib/app/$required"
            Assert-ZipEntry -ArchivePath $Path `
                -Entry "lib/app/bin/$required" -Absent
        }
        Assert-ZipEntry -ArchivePath $Path `
            -Entry 'lib/app/licenses/velopack/MIT.txt'
    }
}

function Assert-FeedAsset {
    param(
        [Parameter(Mandatory)][object]$Feed,
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$ReleaseVersion,
        [Parameter(Mandatory)][ValidateSet('Full', 'Delta')]
        [string]$Type
    )

    $file = Get-Item -LiteralPath $FilePath
    $entries = @($Feed.Assets | Where-Object { $_.FileName -ceq $file.Name })
    if ($entries.Count -ne 1) {
        throw "Feed must contain exactly one '$($file.Name)' entry."
    }
    if ([string]$entries[0].PackageId -cne $Id -or
        [string]$entries[0].Version -cne $ReleaseVersion -or
        [string]$entries[0].Type -cne $Type) {
        throw "Feed identity/type mismatch for '$($file.Name)'."
    }
    $actualHash = (Get-FileHash -LiteralPath $file.FullName `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $feedHash = ([string]$entries[0].SHA256).ToLowerInvariant()
    if ($feedHash -cne $actualHash -or
        [int64]$entries[0].Size -ne $file.Length) {
        throw "Feed hash/size mismatch for '$($file.Name)'."
    }
}

function Write-AssetHashes {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string[]]$Files
    )

    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('# SHA-256  Size  Asset name')
    foreach ($filePath in @($Files | Sort-Object)) {
        $file = Get-Item -LiteralPath $filePath
        $hash = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines.Add("$hash  $($file.Length)  $($file.Name)")
    }
    [IO.File]::WriteAllText($Path,
        [string]::Join("`n", $lines) + "`n",
        [Text.UTF8Encoding]::new($false))
}

function Update-VpkAssetManifestSetupAsset {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$OriginalName,
        [Parameter(Mandatory)][string]$PublishedName,
        [Parameter(Mandatory)][string]$PublishedFilePath
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Velopack upload asset manifest is missing: $Path"
    }
    if (-not (Test-Path -LiteralPath $PublishedFilePath -PathType Leaf)) {
        throw "Published installer wrapper is missing: $PublishedFilePath"
    }

    $entries = @(Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
    $setupEntries = @($entries | Where-Object {
        $_.RelativeFileName -ceq $OriginalName
    })
    if ($setupEntries.Count -ne 1) {
        throw "Expected one canonical Setup.exe entry in the Velopack asset manifest, found $($setupEntries.Count)."
    }
    $setupEntry = $setupEntries[0]
    if ([string]$setupEntry.Type -cne 'Installer') {
        throw 'Velopack upload asset manifest Setup.exe entry is not an Installer asset.'
    }

    $published = Get-Item -LiteralPath $PublishedFilePath
    $publishedHash = (Get-FileHash -LiteralPath $published.FullName `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $setupEntry.RelativeFileName = $PublishedName
    $sizeProperty = $setupEntry.PSObject.Properties['Size']
    $hashProperty = $setupEntry.PSObject.Properties['SHA256']
    if ($null -ne $sizeProperty) {
        $sizeProperty.Value = [int64]$published.Length
    }
    if ($null -ne $hashProperty) {
        $hashProperty.Value = $publishedHash
    }
    if ($null -eq $sizeProperty -and $null -eq $hashProperty) {
        $unexpectedIntegrityFields = @($setupEntry.PSObject.Properties.Name |
            Where-Object { $_ -match '(?i)(size|sha|hash)' })
        if ($unexpectedIntegrityFields.Count -ne 0) {
            throw "Velopack upload asset manifest contains unsupported installer integrity fields: $($unexpectedIntegrityFields -join ', ')"
        }
        if (@($setupEntry.PSObject.Properties.Name).Count -ne 2) {
            throw 'Velopack upload asset manifest name-only Installer schema changed unexpectedly.'
        }
    }

    $updated = ConvertTo-Json -InputObject @($entries) -Depth 10 -Compress
    [IO.File]::WriteAllText($Path, $updated + "`n",
        [Text.UTF8Encoding]::new($false))

    $verifiedEntries = @(Get-Content -LiteralPath $Path -Raw |
        ConvertFrom-Json)
    $verifiedSetup = @($verifiedEntries | Where-Object {
        $_.RelativeFileName -ceq $PublishedName -and
        [string]$_.Type -ceq 'Installer'
    })
    if ($verifiedSetup.Count -ne 1) {
        throw 'Velopack upload asset manifest did not retain the published installer wrapper.'
    }
    $verifiedSize = $verifiedSetup[0].PSObject.Properties['Size']
    $verifiedHash = $verifiedSetup[0].PSObject.Properties['SHA256']
    if (($null -ne $verifiedSize -and
            [int64]$verifiedSize.Value -ne $published.Length) -or
        ($null -ne $verifiedHash -and
            ([string]$verifiedHash.Value).ToLowerInvariant() -cne
                $publishedHash)) {
        throw 'Velopack upload asset manifest installer hash/size is stale.'
    }
}

$packageRootPath = Resolve-RequiredDirectory -Path $PackageRoot `
    -Label "PackageRoot"
$packageLayout = Resolve-PackageLayout -Root $packageRootPath
$iconFile = Resolve-RequiredFile -Path $IconPath -Label "IconPath"
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$toolCachePath = [IO.Path]::GetFullPath($ToolCacheDirectory)
$installerStubFullPath = if ([string]::IsNullOrWhiteSpace(
        $InstallerStubPath)) {
    $null
} else {
    [IO.Path]::GetFullPath($InstallerStubPath)
}
Assert-SeparateDirectoryTrees -FirstPath $packageLayout.root `
    -SecondPath $outputPath -FirstLabel 'PackageRoot' `
    -SecondLabel 'OutputDirectory'
Assert-SeparateDirectoryTrees -FirstPath $packageLayout.root `
    -SecondPath $toolCachePath -FirstLabel 'PackageRoot' `
    -SecondLabel 'ToolCacheDirectory'
Assert-SeparateDirectoryTrees -FirstPath $outputPath `
    -SecondPath $toolCachePath -FirstLabel 'OutputDirectory' `
    -SecondLabel 'ToolCacheDirectory'
$assetNames = Get-AssetNames -Id $packageId -ReleaseVersion $Version `
    -ReleaseChannel $Channel

$previousPath = $null
$previousPackages = @()
if (-not [string]::IsNullOrWhiteSpace($PreviousReleaseDirectory)) {
    $previousPath = Resolve-RequiredDirectory `
        -Path $PreviousReleaseDirectory -Label "PreviousReleaseDirectory"
    $previousPackages = @(Get-MatchingReleasePackages -Directory $previousPath `
        -Id $packageId -ReleaseChannel $Channel -BeforeVersion $Version)
    if (@($previousPackages | Where-Object isFull).Count -eq 0) {
        throw "PreviousReleaseDirectory contains no $packageId/$Channel full nupkg: $previousPath"
    }
}

$plannedArguments = @(
    '--skip-updates',
    'pack',
    '--outputDir', $outputPath,
    '--channel', $Channel,
    '--runtime', 'win-x64',
    '--packId', $packageId,
    '--packVersion', $Version,
    '--packDir', '<flattened-staging>',
    '--mainExe', $mainExe,
    '--packAuthors', 'TwooSix',
    '--packTitle', 'Yanami',
    '--icon', $iconFile,
    '--delta', $DeltaMode,
    '--instLocation', 'PerUser',
    '--shortcuts', 'None',
    '--noPortable',
    '--skipVeloAppCheck'
)

if ($PlanOnly) {
    [pscustomobject]@{
        packageRoot = $packageLayout.root
        mainExe = $packageLayout.mainExe
        flattenBin = $packageLayout.flattenBin
        wrappedCpackRoot = $packageLayout.wrapped
        version = $Version
        channel = $Channel
        outputDirectory = $outputPath
        previousPackages = @($previousPackages | ForEach-Object {
            $_.file.FullName
        })
        expectedAssets = $assetNames
        vpkVersion = $vpkVersion
        vpkPackageUri = $vpkPackageUri
        vpkPackageSha256 = $vpkPackageSha256
        installerStubPath = $installerStubFullPath
        installerContract = [pscustomobject]@{
            byteOrder = @('stub', 'canonicalVelopackSetup', 'footer')
            magic = 'YANAMI_SETUP_V1\0'
            payload = 'canonicalVelopackSetup'
            payloadSizeEncoding = 'uint64-little-endian'
            payloadSha256Encoding = '32-raw-bytes'
            format = [int]$installerFooterFormat
            footerSize = [int]$installerFooterSize
            verifyArgument = $installerVerifyArgument
            verifyJsonSizeField = 'payloadSize'
            verifyJsonHashField = 'payloadSha256'
            backendPublished = $false
        }
        arguments = $plannedArguments
    }
    return
}

if ($null -eq $installerStubFullPath) {
    throw 'InstallerStubPath is required for a non-PlanOnly Windows package.'
}
$installerStubFullPath = Resolve-RequiredFile -Path $installerStubFullPath `
    -Label 'InstallerStubPath'
Test-PeFile -Path $installerStubFullPath -Label 'Yanami installer stub'

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
foreach ($targetName in @($assetNames.vpkSetup, $assetNames.setup,
        $assetNames.full,
        $assetNames.delta, $assetNames.feed, $assetNames.assetManifest,
        $assetNames.legacyFeed,
        $assetNames.hashes)) {
    $target = Join-Path $outputPath $targetName
    if (Test-Path -LiteralPath $target) {
        throw "Refusing to overwrite an existing Velopack target asset: $target"
    }
}

if ($null -ne $previousPath) {
    $previousPackages = @(Copy-PreviousReleases -Source $previousPath `
        -Destination $outputPath -Id $packageId -ReleaseChannel $Channel `
        -CurrentVersion $Version)
}
Assert-ReleaseDirectoryIsolation -Directory $outputPath -Id $packageId `
    -ReleaseChannel $Channel -CurrentVersion $Version

$outputBaselines = @(Get-MatchingReleasePackages -Directory $outputPath `
    -Id $packageId -ReleaseChannel $Channel -BeforeVersion $Version |
    Where-Object isFull)
$expectDelta = $outputBaselines.Count -gt 0

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$stagingPath = Join-Path $temporaryRoot `
    ("yanami-velopack-stage-" + [Guid]::NewGuid().ToString('N'))
$vpkSetupPath = Join-Path $outputPath $assetNames.vpkSetup
$setupPath = Join-Path $outputPath $assetNames.setup
$fullPath = Join-Path $outputPath $assetNames.full
$deltaPath = Join-Path $outputPath $assetNames.delta
$feedPath = Join-Path $outputPath $assetNames.feed
$assetManifestPath = Join-Path $outputPath $assetNames.assetManifest
$legacyFeedPath = Join-Path $outputPath $assetNames.legacyFeed
$hashPath = Join-Path $outputPath $assetNames.hashes
$completed = $false
try {
    Copy-PackageToVelopackStage -Layout $packageLayout `
        -Destination $stagingPath
    Write-PackageManifest -Root $stagingPath
    Write-Host "Running native Velopack install-hook smoke..."
    Invoke-NativeVelopackHookSmoke -StagingRoot $stagingPath `
        -ReleaseVersion $Version
    Assert-PackageManifest -Root $stagingPath

    $tool = Resolve-VpkTool -CacheRoot $toolCachePath `
        -SuppliedPackage $VpkPackagePath
    $arguments = @($plannedArguments)
    $packDirIndex = [Array]::IndexOf($arguments, '<flattened-staging>')
    $arguments[$packDirIndex] = $stagingPath

    Write-Host "Packaging $packageId $Version for channel '$Channel' with vpk $($tool.version)..."
    & $tool.dotnet $tool.vpk @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "vpk pack failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path -LiteralPath $vpkSetupPath -PathType Leaf) -or
        (Get-Item -LiteralPath $vpkSetupPath).Length -eq 0) {
        throw "Velopack Setup.exe is missing or empty: $vpkSetupPath"
    }
    Test-PeFile -Path $vpkSetupPath -Label "Velopack per-user Setup.exe"
    Test-PeFile -Path $installerStubFullPath -Label 'Yanami installer stub'
    $wrapper = New-YanamiInstallerWrapper `
        -StubPath $installerStubFullPath -BackendPath $vpkSetupPath `
        -OutputPath $setupPath
    Test-PeFile -Path $setupPath -Label 'Published Yanami Setup.exe wrapper'
    $verifiedWrapper = Invoke-InstallerWrapperVerification -Path $setupPath `
        -BackendPath $vpkSetupPath
    if ([int64]$verifiedWrapper.payloadSize -ne $wrapper.payloadSize -or
        ([string]$verifiedWrapper.payloadSha256).ToLowerInvariant() -cne
            $wrapper.payloadSha256) {
        throw 'Installer wrapper self-verification disagrees with the packaging footer.'
    }
    Update-VpkAssetManifestSetupAsset -Path $assetManifestPath `
        -OriginalName $assetNames.vpkSetup -PublishedName $assetNames.setup `
        -PublishedFilePath $setupPath
    Remove-ExactGeneratedInstaller -Path $vpkSetupPath `
        -OutputRoot $outputPath -ExpectedName $assetNames.vpkSetup
    if (Test-Path -LiteralPath $vpkSetupPath) {
        throw 'Canonical Velopack Setup.exe backend remained as a public asset.'
    }
    Assert-ReleasePackage -Path $fullPath -Id $packageId `
        -ReleaseVersion $Version -ReleaseChannel $Channel -VerifyMainEntry
    if ($expectDelta) {
        Assert-ReleasePackage -Path $deltaPath -Id $packageId `
            -ReleaseVersion $Version -ReleaseChannel $Channel
    } elseif (Test-Path -LiteralPath $deltaPath) {
        throw "vpk unexpectedly generated a delta without a previous full release."
    }

    if (-not (Test-Path -LiteralPath $feedPath -PathType Leaf)) {
        throw "Velopack preview feed is missing: $feedPath"
    }
    if (-not (Test-Path -LiteralPath $legacyFeedPath -PathType Leaf)) {
        throw "Velopack legacy preview feed is missing: $legacyFeedPath"
    }
    $feed = Get-Content -LiteralPath $feedPath -Raw | ConvertFrom-Json
    Assert-FeedAsset -Feed $feed -FilePath $fullPath -Id $packageId `
        -ReleaseVersion $Version -Type Full
    if ($expectDelta) {
        Assert-FeedAsset -Feed $feed -FilePath $deltaPath -Id $packageId `
            -ReleaseVersion $Version -Type Delta
    }

    $hashInputs = [Collections.Generic.List[string]]::new()
    foreach ($file in @($setupPath, $fullPath, $feedPath, $assetManifestPath,
            $legacyFeedPath)) {
        $hashInputs.Add($file)
    }
    if ($expectDelta) {
        $hashInputs.Add($deltaPath)
    }
    Write-AssetHashes -Path $hashPath -Files @($hashInputs)

    $result = [pscustomobject]@{
        setup = $setupPath
        full = $fullPath
        delta = if ($expectDelta) { $deltaPath } else { $null }
        feed = $feedPath
        assetManifest = $assetManifestPath
        legacyFeed = $legacyFeedPath
        sha256Manifest = $hashPath
        mainExe = $mainExe
        installLocation = 'PerUser'
        shortcutLocations = 'None'
        installerStub = $installerStubFullPath
        installerPayloadSha256 = $wrapper.payloadSha256
        installerPayloadSize = $wrapper.payloadSize
        channel = $Channel
        vpkVersion = $tool.version
        stagingDirectory = if ($KeepStagingDirectory) {
            $stagingPath
        } else {
            $null
        }
    }
    $completed = $true
    $result
} finally {
    Remove-ExactGeneratedInstaller -Path $vpkSetupPath `
        -OutputRoot $outputPath -ExpectedName $assetNames.vpkSetup
    if (-not $completed) {
        Remove-ExactGeneratedInstaller -Path $setupPath `
            -OutputRoot $outputPath -ExpectedName $assetNames.setup
    }
    if ((-not $KeepStagingDirectory -or -not $completed) -and
        (Test-Path -LiteralPath $stagingPath)) {
        $stagingFull = [IO.Path]::GetFullPath($stagingPath)
        $temporaryPrefix = $temporaryRoot.TrimEnd(
            [IO.Path]::DirectorySeparatorChar) +
            [IO.Path]::DirectorySeparatorChar
        if ($stagingFull.StartsWith($temporaryPrefix,
                [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $stagingFull).StartsWith(
                'yanami-velopack-stage-',
                [StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $stagingFull -Recurse -Force
        }
    }
}
