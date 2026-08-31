$ErrorActionPreference = "Stop"
$workflowPath = Join-Path $PSScriptRoot "..\workflows\release.yml"
$workflow = Get-Content -LiteralPath $workflowPath -Raw
$assertions = 0

function Assert-Match {
    param([string]$Label, [string]$Pattern)
    $script:assertions++
    if ($workflow -notmatch $Pattern) {
        throw "$Label is missing from the release workflow."
    }
}

function Assert-Count {
    param([string]$Label, [string]$Pattern, [int]$Expected)
    $script:assertions++
    $actual = [regex]::Matches($workflow, $Pattern).Count
    if ($actual -ne $Expected) {
        throw "$Label expected $Expected matches, got $actual."
    }
}

function Assert-Absent {
    param([string]$Label, [string]$Pattern)
    $script:assertions++
    if ($workflow -match $Pattern) {
        throw "$Label must not appear in the release workflow."
    }
}

Assert-Match "license inventory job" `
    '(?ms)^  rust-licenses:\r?\n    name:.*?\r?\n    needs: metadata\r?\n.*?retention-days: 30\s*$'
Assert-Count "platform jobs consume the license job" `
    '(?m)^    needs: \[metadata, rust-licenses(?:, linux-package)?\]\s*$' 3
Assert-Count "platform jobs gate on license generation" `
    "needs\.rust-licenses\.result == 'success'" 3
Assert-Count "one upload and three downloads share the run-scoped artifact" `
    'name: rust-license-\$\{\{ github\.run_id \}\}' 4
Assert-Count "all platform builds receive the generated inventory" `
    'YANAMI_RUST_LICENSE_INVENTORY' 3
Assert-Count "all platform audits require the packaged HTML report" `
    'licenses[/\\]rust[/\\]THIRD_PARTY_LICENSES\.html' 3
Assert-Absent "removed tracked dependency snapshot" `
    'licenses[/\\]RUST_DEPENDENCIES\.md'
Assert-Absent "stale checked-in inventory comparison" `
    'checked-in Rust license report is stale|Get-FileHash licenses[/\\]rust[/\\]THIRD_PARTY_LICENSES\.html'
Assert-Count "Linux loader smokes scope LD_DEBUG to the tested entrypoint" `
    '(?m)^\s*LD_DEBUG=libs timeout 12s "\$entrypoint"' 2
Assert-Absent "Linux loader debugging is not exported to unrelated commands" `
    '(?m)^\s*export LD_DEBUG='
Assert-Match "Linux first-screen auto-exit has an explicit trace" `
    '(?ms)LD_DEBUG=libs timeout 12s "\$entrypoint" \\\r?\n\s*--performance-trace "\$first_screen_trace" \\\r?\n\s*--performance-runtime-auto-exit \\$'
Assert-Match "Linux first-screen smoke verifies the settled milestone" `
    'grep -q ''"milestone":"startup_settled"'' "\$first_screen_trace"'
Assert-Match "macOS resolves PowerShell before isolating PATH" `
    '(?m)^\s*pwsh_path="\$\(command -v pwsh\)"\r?\n\s*test -x "\$pwsh_path"$'
Assert-Match "macOS bootstrap smoke uses the resolved PowerShell path" `
    '(?ms)PATH=/usr/bin:/bin:/usr/sbin:/sbin \\\r?\n\s*QT_QUICK_BACKEND=software \\\r?\n\s*"\$pwsh_path" -NoLogo -NoProfile -File \\'
Assert-Absent "macOS isolated bootstrap smoke does not search for bare PowerShell" `
    '(?ms)PATH=/usr/bin:/bin:/usr/sbin:/sbin \\\r?\n\s*QT_QUICK_BACKEND=software \\\r?\n\s*pwsh -NoLogo -NoProfile -File \\'

Assert-Match "Velopack libc archive is version and hash pinned" `
    '(?s)velopack_libc_1\.2\.0\.zip.*?547262ed7a1ab1ff62f580aa53851ede2f1a451ac61b8974eb7bc01117488835'
Assert-Match "Velopack libc DLL is independently hash pinned" `
    'c36d8b984639a8af9d3397088d3ffb8213fe1bd0917f555cf0c6e33f014403ec'
Assert-Match "release CMake receives the verified Velopack runtime" `
    '-DYANAMI_VELOPACK_RUNTIME=\$env:YANAMI_VELOPACK_RUNTIME'
Assert-Match "Windows package invokes the pinned Velopack packager" `
    'scripts/package-windows-velopack\.ps1'
Assert-Match "Windows packager receives the branded installer shell" `
    'InstallerStubPath\s*=\s*\$installerStubPath'
Assert-Match "published Setup verifies its embedded backend" `
    'ArgumentList "--verify-payload"'
Assert-Match "published Setup asset has the stable versioned name" `
    'Yanami-\$\{\{ needs\.metadata\.outputs\.version \}\}-Windows-x86_64-Setup\.exe'
Assert-Match "preview full package has an exact application id name" `
    'io\.github\.TwooSix\.Yanami-\$YANAMI_RELEASE_VERSION-preview-full\.nupkg'
Assert-Match "preview feed is a public release asset" `
    'build/release-windows/releases\.preview\.json'
Assert-Match "published full baseline is verified before delta generation" `
    'Downloaded Velopack baseline failed its published size/SHA-256 check'
Assert-Match "Setup smoke checks the per-user uninstall key" `
    'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\io\.github\.TwooSix\.Yanami'
Assert-Match "Setup smoke uses the public wizard automation contract" `
    '"--install-dir", \$installRoot,\s*\r?\n\s*"--start-menu", "yes", "--desktop", "no", "--no-launch"'
Assert-Match "Setup smoke verifies an unselected desktop shortcut stays absent" `
    'Setup created a desktop shortcut even though it was disabled'
Assert-Count "selected shortcut smokes verify their exact installed targets" `
    'does not target the installed candidate' 2
Assert-Match "Windows packaging runs the Unicode shortcut reader regression" `
    '(?ms)^  windows-package:.*?run: \.\\\.github\\scripts\\Test-WindowsShortcut\.ps1'
Assert-Count "both shortcut smokes load the Unicode-safe reader" `
    '(?m)^\s*\. \./\.github/scripts/WindowsShortcut\.ps1$' 2
Assert-Count "both shortcut smokes read through the Unicode-safe helper" `
    'Get-WindowsShortcut -LiteralPath \$(?:shortcutPath|desktopShortcutPath)' 2
Assert-Count "both shortcut targets retain exact case-insensitive comparisons" `
    '\$actualTarget -ine \$expectedTarget' 2
Assert-Count "both shortcut working directories retain exact case-insensitive comparisons" `
    '\$actualWorkingDirectory -ine \$expectedWorkingDirectory' 2
Assert-Count "shortcut mismatch diagnostics include expected and actual targets" `
    'Expected TargetPath=''\$expectedTarget''; actual TargetPath=''\$actualTarget''' 2
Assert-Count "shortcut mismatch diagnostics include expected and actual working directories" `
    'Expected WorkingDirectory=''\$expectedWorkingDirectory''; actual WorkingDirectory=''\$actualWorkingDirectory''' 2
Assert-Absent "shortcut smokes do not use the ANSI WScript shortcut reader" `
    'WScript\.Shell|\.CreateShortcut\('
Assert-Match "Setup smoke exercises the alternate shortcut selection" `
    '"--start-menu", "no", "--desktop", "yes", "--no-launch"'
Assert-Match "alternate shortcut smoke retains its Unicode installation directory" `
    '"Yanami 安装 desktop smoke \$env:GITHUB_RUN_ID-\$env:GITHUB_RUN_ATTEMPT"'
Assert-Match "alternate shortcut uninstall checks for orphaned desktop links" `
    'Alternate shortcut uninstall left state behind'
Assert-Match "aggregate manifest names the optional delta exactly" `
    'delta_file="io\.github\.TwooSix\.Yanami-\$YANAMI_RELEASE_VERSION-preview-delta\.nupkg"'
Assert-Absent "aggregate does not classify arbitrary zip files as portable packages" `
    "-name '\*\.zip'"

function Assert-Baseline {
    param([string]$Label, [bool]$Condition)
    $script:assertions++
    if (-not $Condition) { throw "Baseline behavior failed: $Label" }
}

# Execute the workflow's actual selector and downloaded-payload validation,
# excluding only filesystem/output setup and the no-baseline exit statement.
# Every HTTP and payload-file operation below is mocked in the invocation scope.
$baselineSteps = [regex]::Matches($workflow,
    '(?ms)^      - name: Download the latest published preview full package\r?\n(?<step>.*?)(?=^      - name:|\z)')
Assert-Baseline "one published-baseline step" ($baselineSteps.Count -eq 1)
$runBody = [regex]::Match($baselineSteps[0].Groups['step'].Value,
    '(?ms)^        run: \|\r?\n(?<body>.*)$')
Assert-Baseline "baseline step has an inline PowerShell body" $runBody.Success
$baselineSource = (@($runBody.Groups['body'].Value -split '\r?\n' | ForEach-Object {
    if ([string]::IsNullOrWhiteSpace($_)) { '' }
    elseif ($_.StartsWith('          ')) { $_.Substring(10) }
    else { throw 'Unexpected indentation in the baseline workflow step.' }
}) -join "`n")
$baselineTokens = $null
$baselineErrors = $null
$baselineAst = [Management.Automation.Language.Parser]::ParseInput(
    $baselineSource, [ref]$baselineTokens, [ref]$baselineErrors)
Assert-Baseline "baseline PowerShell parses" ($baselineErrors.Count -eq 0)

function Get-BaselineAssignment {
    param([string]$Name)
    $statements = @($baselineAst.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
        $_.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $_.Left.VariablePath.UserPath -ceq $Name
    })
    if ($statements.Count -ne 1) { throw "Expected one baseline assignment: $Name" }
    return $statements[0]
}

$selectorStart = (Get-BaselineAssignment 'api').Extent.StartOffset
$selectorEnd = (Get-BaselineAssignment 'baselineRoot').Extent.StartOffset
$baselineSelector = $baselineSource.Substring($selectorStart, $selectorEnd - $selectorStart)
$baselineSelection = (Get-BaselineAssignment 'selected').Extent.Text
$verificationStart = (Get-BaselineAssignment 'baselineName').Extent.StartOffset
$successOutput = @($baselineAst.EndBlock.Statements | Where-Object {
    $_.Extent.Text -match '^"available=true"\s*>>'
})
Assert-Baseline "one verified-baseline success output" ($successOutput.Count -eq 1)
$baselineVerification = $baselineSource.Substring($verificationStart,
    $successOutput[0].Extent.StartOffset - $verificationStart)

function New-TestFullEntry {
    param([string]$Version)
    [pscustomobject]@{
        PackageId = 'io.github.TwooSix.Yanami'
        Version = $Version
        Type = 'Full'
        FileName = "io.github.TwooSix.Yanami-$Version-preview-full.nupkg"
        SHA256 = 'a' * 64
        Size = 123
    }
}

function New-TestRelease {
    param([string]$Version, [bool]$Draft = $false,
        [bool]$HasFeed = $true, [bool]$HasFull = $true)
    $entry = New-TestFullEntry $Version
    [pscustomobject]@{
        draft = $Draft
        assets = @(
            if ($HasFeed) {
                [pscustomobject]@{ name = 'releases.preview.json'
                    browser_download_url = "https://releases.invalid/$Version/feed" }
            }
            if ($HasFull) {
                [pscustomobject]@{ name = $entry.FileName; size = $entry.Size
                    browser_download_url = "https://releases.invalid/$Version/full" }
            }
        )
    }
}

function Invoke-BaselineFixture {
    param([object[]]$Releases, [hashtable]$Feeds,
        [long]$PayloadSize = 123, [string]$PayloadHash = ('a' * 64),
        [string]$Selector = $baselineSelector)

    $requests = [Collections.Generic.List[string]]::new()
    $downloads = [Collections.Generic.List[string]]::new()
    $allowedDownloads = @($Releases | ForEach-Object { $_.assets } |
        Where-Object { $_.name -like '*-full.nupkg' } |
        ForEach-Object { [string]$_.browser_download_url })
    $repositoryBefore = $env:YANAMI_REPOSITORY
    $versionBefore = $env:YANAMI_RELEASE_VERSION
    try {
        $env:YANAMI_REPOSITORY = 'Yanami/ReleaseWorkflowTests'
        $env:YANAMI_RELEASE_VERSION = '0.2.0-dev.18'
        $headers = @{}
        $baselineRoot = Join-Path ([IO.Path]::GetTempPath()) 'Yanami-baseline-fixture-not-created'

        function Invoke-RestMethod {
            [CmdletBinding()]
            param([string]$Uri, [hashtable]$Headers)
            $requests.Add($Uri)
            if ($Uri -ceq 'https://api.github.com/repos/Yanami/ReleaseWorkflowTests/releases?per_page=10') {
                # IRM returns a top-level JSON array as ONE pipeline object.
                # Enumerating the mock's output would hide the production bug.
                $PSCmdlet.WriteObject([object[]]$Releases, $false)
            } elseif ($Feeds.ContainsKey($Uri)) {
                $PSCmdlet.WriteObject($Feeds[$Uri], $false)
            } else { throw "Unexpected fixture HTTP request: $Uri" }
        }
        function Invoke-WebRequest {
            param([string]$Uri, [hashtable]$Headers, [string]$OutFile)
            if ($Uri -cnotin $allowedDownloads) { throw "Unexpected fixture download: $Uri" }
            $downloads.Add($OutFile)
        }
        function Get-Item {
            param([string]$LiteralPath)
            if ($LiteralPath -cnotin $downloads) { throw 'Unexpected fixture file inspection.' }
            [pscustomobject]@{ Length = $PayloadSize }
        }
        function Get-FileHash {
            param([string]$LiteralPath, [string]$Algorithm)
            if ($LiteralPath -cnotin $downloads -or $Algorithm -cne 'SHA256') {
                throw 'Unexpected fixture hash inspection.'
            }
            [pscustomobject]@{ Hash = $PayloadHash }
        }

        $errorText = ''
        $name = ''
        try {
            . ([scriptblock]::Create($Selector))
            . ([scriptblock]::Create($baselineSelection))
            if ($selected.Count -gt 0) {
                . ([scriptblock]::Create($baselineVerification))
                $name = [string]$selected[0].entry.FileName
            }
        } catch { $errorText = $_.Exception.Message }
        [pscustomobject]@{ Name = $name; Error = $errorText
            Downloads = $downloads.Count; Requests = $requests.Count }
    } finally {
        $env:YANAMI_REPOSITORY = $repositoryBefore
        $env:YANAMI_RELEASE_VERSION = $versionBefore
    }
}

function Assert-BaselineResult {
    param([string]$Label, [object]$Result, [string]$ExpectedName = '',
        [string]$ExpectedError = '', [int]$Downloads = 0)
    Assert-Baseline "$Label download count" ($Result.Downloads -eq $Downloads)
    Assert-Baseline "$Label selected name" ($Result.Name -ceq $ExpectedName)
    if ($ExpectedError) {
        Assert-Baseline "$Label error" ($Result.Error -match $ExpectedError)
    } else {
        Assert-Baseline "$Label succeeds" ([string]::IsNullOrEmpty($Result.Error))
    }
}

$full17 = (New-TestFullEntry '0.2.0-dev.17').FileName
$feed16 = [pscustomobject]@{ Assets = @((New-TestFullEntry '0.2.0-dev.16')) }
$feed17 = [pscustomobject]@{ Assets = @((New-TestFullEntry '0.2.0-dev.17')) }
$feeds = @{
    'https://releases.invalid/0.2.0-dev.16/feed' = $feed16
    'https://releases.invalid/0.2.0-dev.17/feed' = $feed17
}
$multipleReleases = @((New-TestRelease '0.2.0-dev.99' -Draft $true),
    (New-TestRelease '0.2.0-dev.16'), (New-TestRelease '0.2.0-dev.17'))
Assert-BaselineResult 'multiple releases skip drafts and select newest full' `
    (Invoke-BaselineFixture $multipleReleases $feeds) -ExpectedName $full17 -Downloads 1
Assert-BaselineResult 'single release JSON array' `
    (Invoke-BaselineFixture @((New-TestRelease '0.2.0-dev.17')) $feeds) `
    -ExpectedName $full17 -Downloads 1
Assert-BaselineResult 'empty release JSON array' (Invoke-BaselineFixture @() @{})
Assert-BaselineResult 'published release without preview feed' `
    (Invoke-BaselineFixture @((New-TestRelease '0.2.0-dev.17' -HasFeed $false)) @{})
Assert-BaselineResult 'historical feed entry without an attached full is ignored' `
    (Invoke-BaselineFixture @((New-TestRelease '0.2.0-dev.17' -HasFull $false)) @{
        'https://releases.invalid/0.2.0-dev.17/feed' = $feed16
    })

$wrongSizeRelease = New-TestRelease '0.2.0-dev.17'
($wrongSizeRelease.assets | Where-Object name -CEQ $full17).size = 124
Assert-BaselineResult 'GitHub/feed size mismatch' `
    (Invoke-BaselineFixture @($wrongSizeRelease) $feeds) `
    -ExpectedError 'GitHub and releases.preview.json disagree on baseline size'
Assert-BaselineResult 'downloaded size mismatch' `
    (Invoke-BaselineFixture @((New-TestRelease '0.2.0-dev.17')) $feeds -PayloadSize 124) `
    -Downloads 1 -ExpectedError 'Downloaded Velopack baseline failed its published size/SHA-256 check'
Assert-BaselineResult 'downloaded SHA-256 mismatch' `
    (Invoke-BaselineFixture @((New-TestRelease '0.2.0-dev.17')) $feeds -PayloadHash ('b' * 64)) `
    -Downloads 1 -ExpectedError 'Downloaded Velopack baseline failed its published size/SHA-256 check'

# A negative control reintroduces the exact original regression in memory. The
# same acceptance check must reject it; no workflow/source file is modified.
$fixedAssignment = '$releases = Invoke-RestMethod -Uri $api -Headers $headers'
$oldAssignment = '$releases = @(Invoke-RestMethod -Uri $api -Headers $headers)'
Assert-Baseline 'one direct IRM release assignment for the negative control' `
    ([regex]::Matches($baselineSelector, [regex]::Escape($fixedAssignment)).Count -eq 1)
$oldResult = Invoke-BaselineFixture $multipleReleases $feeds `
    -Selector $baselineSelector.Replace($fixedAssignment, $oldAssignment)
Assert-BaselineResult 'old nested-array code silently loses every baseline' $oldResult
$oldCodeRejected = $false
try {
    Assert-BaselineResult 'old code must meet the same published-full acceptance' `
        $oldResult -ExpectedName $full17 -Downloads 1
} catch {
    $oldCodeRejected = $_.Exception.Message -like 'Baseline behavior failed:*'
}
Assert-Baseline 'old code fails the published-full acceptance' $oldCodeRejected

Write-Host "Release workflow contract tests passed ($assertions assertions)."
