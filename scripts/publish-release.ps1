[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [long]$RunId,

    [string]$OutputDirectory,

    [switch]$Publish
)

$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$workspace = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $workspace "build\manual-release\$RunId"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

foreach ($commandName in @("gh", "git")) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Required command is missing: $commandName"
    }
}

$repository = (& gh repo view --json nameWithOwner --jq .nameWithOwner).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($repository)) {
    throw "Unable to resolve the current GitHub repository."
}
$run = & gh run view $RunId `
    --json databaseId,workflowName,conclusion,event,headBranch,headSha,url |
    ConvertFrom-Json
if ($LASTEXITCODE -ne 0) {
    throw "Unable to inspect workflow run $RunId."
}
if ($run.workflowName -ne "Desktop packages" -or
    $run.conclusion -ne "success" -or
    $run.headBranch -ne "main" -or
    $run.event -notin @("workflow_run", "workflow_dispatch")) {
    throw "Run $RunId is not a successful main-branch Desktop packages run."
}

$downloadCandidate = $true
if (Test-Path -LiteralPath $OutputDirectory) {
    $existingItems = @(Get-ChildItem -LiteralPath $OutputDirectory -Force)
    if ($existingItems.Count -gt 0) {
        $existingManifests = @(Get-ChildItem -LiteralPath $OutputDirectory `
            -Filter release-manifest.json -File -Recurse)
        if ($existingManifests.Count -ne 1) {
            throw "Output directory is not empty and is not a reusable candidate: $OutputDirectory"
        }
        $downloadCandidate = $false
        Write-Host "Reusing the downloaded candidate in $OutputDirectory."
    }
} else {
    New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
}

if ($downloadCandidate) {
    & gh run download $RunId --pattern "Yanami-*-all-platforms" `
        --dir $OutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to download the aggregate release candidate."
    }
}

$manifests = @(Get-ChildItem -LiteralPath $OutputDirectory `
    -Filter release-manifest.json -File -Recurse)
if ($manifests.Count -ne 1) {
    throw "Expected exactly one release-manifest.json, found $($manifests.Count)."
}
$candidateRoot = $manifests[0].Directory.FullName
$manifest = Get-Content -LiteralPath $manifests[0].FullName -Raw |
    ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or
    $manifest.repository -ne $repository -or
    $manifest.packageWorkflowRunId -ne "$RunId" -or
    $manifest.releasePolicy -ne "manual-prerelease-only" -or
    $manifest.intendedTag -ne "v$($manifest.version)") {
    throw "The release manifest does not match workflow run $RunId."
}
# workflow_run itself is associated with the latest default-branch commit, not
# necessarily the earlier CI commit recorded in its event payload. The package
# manifest is the authoritative source commit for that event. A manual dispatch
# does package its own head SHA and can be checked directly.
if ($run.event -eq "workflow_dispatch" -and $manifest.commit -ne $run.headSha) {
    throw "The manually dispatched run does not match its packaged commit."
}
if ($manifest.sourceVersion -notmatch '^([0-9]+\.[0-9]+\.[0-9]+-dev)\.0$' -or
    $manifest.version -notmatch "^$([regex]::Escape($Matches[1]))\.[1-9][0-9]*$") {
    throw "The release manifest does not contain a valid development candidate version."
}

$expectedNames = @(
    "Yanami-$($manifest.version)-Windows-x86_64.zip",
    "Yanami-$($manifest.version)-Linux-x86_64.AppImage",
    "Yanami-$($manifest.version)-macOS-arm64.dmg",
    "Yanami-$($manifest.version)-macOS-x86_64.dmg"
) | Sort-Object
$manifestNames = @($manifest.files.name) | Sort-Object
if (Compare-Object $expectedNames $manifestNames) {
    throw "The candidate does not contain the expected four platform packages."
}

foreach ($entry in $manifest.files) {
    $assetPath = Join-Path $candidateRoot $entry.name
    if (-not (Test-Path -LiteralPath $assetPath -PathType Leaf)) {
        throw "Candidate asset is missing: $($entry.name)"
    }
    $asset = Get-Item -LiteralPath $assetPath
    $hash = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($asset.Length -ne [int64]$entry.size -or $hash -ne $entry.sha256) {
        throw "Candidate asset does not match the release manifest: $($entry.name)"
    }
    $checksumPath = "$assetPath.sha256"
    if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
        throw "Per-platform checksum is missing: $($entry.name).sha256"
    }
    $recordedHash = ((Get-Content -LiteralPath $checksumPath -Raw) -split '\s+')[0]
    if ($recordedHash.ToLowerInvariant() -ne $hash) {
        throw "Per-platform checksum does not match: $($entry.name)"
    }
}

$combinedChecksum = Join-Path $candidateRoot "SHA256SUMS.txt"
if (-not (Test-Path -LiteralPath $combinedChecksum -PathType Leaf)) {
    throw "The combined SHA256SUMS.txt file is missing."
}
$combinedEntries = @{}
foreach ($line in Get-Content -LiteralPath $combinedChecksum) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
        throw "Malformed combined checksum entry: $line"
    }
    if ($combinedEntries.ContainsKey($Matches[2])) {
        throw "Duplicate combined checksum entry: $($Matches[2])"
    }
    $combinedEntries[$Matches[2]] = $Matches[1].ToLowerInvariant()
}
if (Compare-Object $expectedNames @($combinedEntries.Keys)) {
    throw "The combined checksum does not cover the expected four packages."
}
foreach ($entry in $manifest.files) {
    if ($combinedEntries[$entry.name] -ne $entry.sha256) {
        throw "The combined checksum disagrees with the manifest: $($entry.name)"
    }
}

Push-Location $workspace
try {
    & git fetch --no-tags origin main
    if ($LASTEXITCODE -ne 0) { throw "Unable to fetch origin/main." }
    & git merge-base --is-ancestor $manifest.commit refs/remotes/origin/main
    if ($LASTEXITCODE -ne 0) {
        throw "Candidate commit is no longer contained in origin/main."
    }

    $tag = $manifest.intendedTag
    $remoteTagLines = @(& git ls-remote --tags origin "refs/tags/$tag" `
        "refs/tags/$tag^{}") | Where-Object { $_ }
    $remoteTagLine = $remoteTagLines |
        Where-Object { $_ -match '\^\{\}$' } | Select-Object -First 1
    if (-not $remoteTagLine) {
        $remoteTagLine = $remoteTagLines | Select-Object -First 1
    }
    if ($remoteTagLine) {
        $remoteTagSha = ($remoteTagLine -split '\s+')[0]
        if ($remoteTagSha -ne $manifest.commit) {
            throw "Remote tag $tag already points to another commit."
        }
    }

    $localTagSha = (& git rev-parse --verify --quiet "$tag^{}" 2>$null)
    $localTagStatus = $LASTEXITCODE
    if ($localTagStatus -eq 0 -and $localTagSha.Trim() -ne $manifest.commit) {
        throw "Local tag $tag already points to another commit."
    }

    & gh release view $tag *> $null
    if ($LASTEXITCODE -eq 0) {
        throw "GitHub Release $tag already exists; this script will not overwrite it."
    }

    Write-Host "Verified Yanami $($manifest.version) from $($manifest.commit)."
    Write-Host "Candidate directory: $candidateRoot"
    Write-Host "Workflow: $($run.url)"
    if (-not $Publish) {
        Write-Host "Preparation only. Re-run with -Publish after reviewing the files."
        return
    }

    if (-not $remoteTagLine) {
        if ($localTagStatus -ne 0) {
            & git tag -a $tag $manifest.commit -m "Yanami $($manifest.version)"
            if ($LASTEXITCODE -ne 0) { throw "Unable to create local tag $tag." }
        }
        & git push origin "refs/tags/$tag"
        if ($LASTEXITCODE -ne 0) { throw "Unable to push tag $tag." }
    }

    $assets = @($manifest.files | ForEach-Object {
        Join-Path $candidateRoot $_.name
        Join-Path $candidateRoot "$($_.name).sha256"
    })
    $assets += @(
        (Join-Path $candidateRoot "SHA256SUMS.txt"),
        (Join-Path $candidateRoot "release-manifest.json")
    )
    $warning = @"
This is an early development preview. Windows and Linux packages are unsigned;
macOS packages are ad-hoc signed but not Developer ID signed or notarized.
Linux and macOS have not yet been validated on real user hardware. Verify the
published SHA-256 checksums before running a downloaded package.
"@
    & gh release create $tag @assets --verify-tag --prerelease --generate-notes `
        --title "Yanami $tag" --notes $warning
    if ($LASTEXITCODE -ne 0) {
        throw "The tag exists, but creating GitHub Release $tag failed."
    }
    Write-Host "Published prerelease $tag."
} finally {
    Pop-Location
}
