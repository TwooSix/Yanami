$ErrorActionPreference = "Stop"
$classifierPath = Join-Path $PSScriptRoot "Classify-CI.ps1"
$assertions = 0

function Assert-Equal {
    param($Actual, $Expected, [string]$Label)
    $script:assertions++
    if ($Actual -cne $Expected) {
        throw "$Label expected '$Expected', got '$Actual'."
    }
}

function Assert-Match {
    param([string]$Actual, [string]$Pattern, [string]$Label)
    $script:assertions++
    if ($Actual -notmatch $Pattern) {
        throw "$Label did not match the required CI policy."
    }
}

function Invoke-Case {
    param([string[]]$Path)
    & $classifierPath -ChangedPath $Path -OutputPath "" -SummaryPath ""
}

$docs = Invoke-Case -Path @("docs/ARCHITECTURE.md")
Assert-Equal $docs.rust $false "docs rust"
Assert-Equal $docs.desktop $false "docs desktop"
Assert-Equal $docs.performance $false "docs performance"
Assert-Equal $docs.workflows $false "docs workflows"
Assert-Equal $docs.nativeAnalysis $false "docs native analysis"
Assert-Equal $docs.package $false "docs package"

$readme = Invoke-Case -Path @("README.md")
Assert-Equal $readme.desktop $false "readme desktop"
Assert-Equal $readme.performance $false "readme performance"
Assert-Equal $readme.package $true "readme package"

$empty = Invoke-Case -Path @()
Assert-Equal $empty.rust $false "empty rust"
Assert-Equal $empty.desktop $false "empty desktop"
Assert-Equal $empty.performance $false "empty performance"

$rust = Invoke-Case -Path @("crates/yanami-core/src/lib.rs")
Assert-Equal $rust.rust $true "core rust"
Assert-Equal $rust.desktop $true "core desktop"
Assert-Equal $rust.performance $true "core performance"
Assert-Equal $rust.nativeAnalysis $false "core native analysis"
Assert-Equal $rust.performanceSuites "Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup" "core suites"

$dependencies = Invoke-Case -Path @("Cargo.toml", "Cargo.lock")
Assert-Equal $dependencies.rust $true "dependency rust"
Assert-Equal $dependencies.desktop $true "dependency desktop"
Assert-Equal $dependencies.performance $true "dependency performance"
Assert-Equal $dependencies.performanceSuites "Search,Backend" "dependency suites"
Assert-Equal $dependencies.package $true "dependency package"

$lockOnly = Invoke-Case -Path @("Cargo.lock")
Assert-Equal $lockOnly.performanceSuites "Search,Backend" "lock-only dependency suites"

$botDependencies = Invoke-Case -Path @(
    "Cargo.toml",
    "Cargo.lock"
)
Assert-Equal $botDependencies.performanceSuites "Search,Backend" "bot dependency suites"

$qml = Invoke-Case -Path @("apps/desktop/qml/components/StatusToast.qml")
Assert-Equal $qml.rust $false "qml rust"
Assert-Equal $qml.desktop $true "qml desktop"
Assert-Equal $qml.nativeAnalysis $false "qml native analysis"
Assert-Equal $qml.package $true "qml package"
Assert-Equal $qml.performanceSuites "Interaction,Startup" "qml suites"

$native = Invoke-Case -Path @("apps/desktop/native/WindowController.cpp")
Assert-Equal $native.desktop $true "native desktop"
Assert-Equal $native.nativeAnalysis $true "native analysis"

$installer = Invoke-Case -Path @("apps/desktop/installer/WindowsInstaller.cpp")
Assert-Equal $installer.desktop $true "installer desktop"
Assert-Equal $installer.nativeAnalysis $true "installer native analysis"
Assert-Equal $installer.package $true "installer package"

$perf = Invoke-Case -Path @("perf/policy/calibration-v1.json")
Assert-Equal $perf.rust $false "performance contract rust"
Assert-Equal $perf.desktop $false "performance contract desktop"
Assert-Equal $perf.performanceSuites "Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup" "performance contract suites"

$rustProbe = Invoke-Case -Path @("crates/yanami-performance-probe/src/main.rs")
Assert-Equal $rustProbe.rust $true "rust performance probe quality"
Assert-Equal $rustProbe.desktop $false "rust performance probe desktop"
Assert-Equal $rustProbe.performance $true "rust performance probe gate"
Assert-Equal $rustProbe.performanceSuites "Search,Backend" "rust performance probe suites"
Assert-Equal $rustProbe.package $false "rust performance probe package"

$dependabot = Invoke-Case -Path @(".github/dependabot.yml")
Assert-Equal $dependabot.workflows $true "dependabot workflow lint"
Assert-Equal $dependabot.rust $false "dependabot rust"
Assert-Equal $dependabot.desktop $false "dependabot desktop"
Assert-Equal $dependabot.performance $false "dependabot performance"
Assert-Equal $dependabot.package $false "dependabot package"

$codeowners = Invoke-Case -Path @(".github/CODEOWNERS")
Assert-Equal $codeowners.workflows $true "CODEOWNERS static analysis"
Assert-Equal $codeowners.desktop $false "CODEOWNERS desktop"
Assert-Equal $codeowners.performance $false "CODEOWNERS performance"
Assert-Equal $codeowners.package $false "CODEOWNERS package"

$contributing = Invoke-Case -Path @("CONTRIBUTING.md")
Assert-Equal $contributing.rust $false "contributing rust"
Assert-Equal $contributing.desktop $false "contributing desktop"
Assert-Equal $contributing.performance $false "contributing performance"
Assert-Equal $contributing.package $false "contributing package"

$attributes = Invoke-Case -Path @(".gitattributes")
Assert-Equal $attributes.workflows $true "attributes packaging policy"
Assert-Equal $attributes.desktop $false "attributes desktop"
Assert-Equal $attributes.performance $false "attributes performance"
Assert-Equal $attributes.package $true "attributes package"

foreach ($packageScriptPath in @(
        "scripts/package-windows-velopack.ps1",
        "scripts/collect-linux-runtime-licenses.sh",
        "scripts/package-linux.sh",
        "scripts/package-macos.sh",
        "scripts/verify-macos-bundle.sh"
    )) {
    $packageScript = Invoke-Case -Path @($packageScriptPath)
    Assert-Equal $packageScript.rust $false "$packageScriptPath rust"
    Assert-Equal $packageScript.desktop $false "$packageScriptPath desktop"
    Assert-Equal $packageScript.workflows $true `
        "$packageScriptPath static analysis"
    Assert-Equal $packageScript.performance $false `
        "$packageScriptPath performance"
    Assert-Equal $packageScript.package $true "$packageScriptPath package"
}

foreach ($performancePackagePath in @(
        "scripts/performance/Test-BootstrapPackage.ps1",
        "scripts/performance/run-gate.ps1",
        "scripts/performance/PerfGate.psm1"
    )) {
    $performancePackage = Invoke-Case -Path @($performancePackagePath)
    Assert-Equal $performancePackage.rust $false `
        "$performancePackagePath rust"
    Assert-Equal $performancePackage.desktop $false `
        "$performancePackagePath desktop"
    Assert-Equal $performancePackage.workflows $true `
        "$performancePackagePath static analysis"
    Assert-Equal $performancePackage.performance $true `
        "$performancePackagePath performance"
    Assert-Equal $performancePackage.package $true `
        "$performancePackagePath package"
}

$performanceSelfTest = Invoke-Case -Path @("scripts/performance/Test-PerfGate.ps1")
Assert-Equal $performanceSelfTest.workflows $true `
    "performance self-test static analysis"
Assert-Equal $performanceSelfTest.performance $true `
    "performance self-test performance"
Assert-Equal $performanceSelfTest.package $false `
    "performance self-test package"

$licenseGenerator = Invoke-Case -Path @("scripts/generate-rust-license-inventory.sh")
Assert-Equal $licenseGenerator.rust $true "license generator rust"
Assert-Equal $licenseGenerator.desktop $false "license generator desktop"
Assert-Equal $licenseGenerator.workflows $true "license generator static analysis"
Assert-Equal $licenseGenerator.performance $false "license generator performance"
Assert-Equal $licenseGenerator.package $true "license generator package"

$release = Invoke-Case -Path @(".github/workflows/release.yml")
Assert-Equal $release.workflows $true "release workflow lint"
Assert-Equal $release.desktop $false "release desktop"
Assert-Equal $release.performance $false "release performance"
Assert-Equal $release.package $true "release package"

$router = Invoke-Case -Path @(".github/workflows/core.yml")
Assert-Equal $router.rust $true "router rust"
Assert-Equal $router.desktop $true "router desktop"
Assert-Equal $router.nativeAnalysis $true "router native analysis"
Assert-Equal $router.performanceSuites "Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup" "router suites"
Assert-Equal $router.package $true "router package wiring"

$classifierResult = Invoke-Case -Path @(".github/scripts/Classify-CI.ps1")
Assert-Equal $classifierResult.rust $true "classifier rust"
Assert-Equal $classifierResult.desktop $true "classifier desktop"
Assert-Equal $classifierResult.nativeAnalysis $true "classifier native analysis"
Assert-Equal $classifierResult.performance $false "classifier performance"
Assert-Equal $classifierResult.package $true "classifier package wiring"

$workflow = Get-Content -LiteralPath (Join-Path $PSScriptRoot "..\workflows\core.yml") -Raw
Assert-Match $workflow "(?m)^\s*group:\s*ci-.*github\.event_name == 'pull_request'.*github\.run_id.*$" "push runs use unique concurrency groups"
Assert-Match $workflow "(?m)^\s*cancel-in-progress:\s*\$\{\{ github\.event_name == 'pull_request' \}\}\s*$" "only pull requests cancel stale runs"

$testRepo = Join-Path ([IO.Path]::GetTempPath()) `
    ("yanami-ci-classifier-" + [guid]::NewGuid().ToString("N"))
[void][IO.Directory]::CreateDirectory($testRepo)
Push-Location $testRepo
try {
    git init --quiet
    git config user.name "Yanami CI"
    git config user.email "ci@example.invalid"
    Set-Content -LiteralPath baseline.txt -Value "baseline"
    git add baseline.txt
    git commit --quiet -m baseline
    $commonSha = (git rev-parse HEAD).Trim()

    [void][IO.Directory]::CreateDirectory((Join-Path $testRepo "apps/desktop/native"))
    Set-Content -LiteralPath apps/desktop/native/BaseOnly.cpp -Value "int base_only;"
    git add apps/desktop/native/BaseOnly.cpp
    git commit --quiet -m "base-only product change"
    $baseSha = (git rev-parse HEAD).Trim()

    git checkout --quiet -b feature $commonSha
    [void][IO.Directory]::CreateDirectory((Join-Path $testRepo "docs"))
    Set-Content -LiteralPath docs/PR.md -Value "docs only"
    git add docs/PR.md
    git commit --quiet -m "feature docs change"
    $headSha = (git rev-parse HEAD).Trim()

    $twoPoint = & $classifierPath -BaseSha $baseSha -HeadSha $headSha `
        -OutputPath "" -SummaryPath ""
    Assert-Equal $twoPoint.desktop $true "two-point diff includes base-only product change"

    $mergeBase = & $classifierPath -BaseSha $baseSha -HeadSha $headSha `
        -UseMergeBase -OutputPath "" -SummaryPath ""
    Assert-Equal $mergeBase.desktop $false "PR merge-base excludes base-only product change"
    Assert-Equal $mergeBase.package $false "docs-only PR merge-base package"

    git checkout --quiet -b rename-source $commonSha
    [void][IO.Directory]::CreateDirectory((Join-Path $testRepo "apps/desktop/native"))
    Set-Content -LiteralPath apps/desktop/native/Moved.cpp -Value "int moved;"
    git add apps/desktop/native/Moved.cpp
    git commit --quiet -m "add product file before rename"
    $renameBaseSha = (git rev-parse HEAD).Trim()

    git checkout --quiet -b rename-to-docs
    [void][IO.Directory]::CreateDirectory((Join-Path $testRepo "docs"))
    git mv apps/desktop/native/Moved.cpp docs/Moved.cpp
    git commit --quiet -m "move product file under docs"
    $renameHeadSha = (git rev-parse HEAD).Trim()

    $renameResult = & $classifierPath `
        -BaseSha $renameBaseSha -HeadSha $renameHeadSha `
        -UseMergeBase -OutputPath "" -SummaryPath ""
    Assert-Equal $renameResult.desktop $true "renamed-away product path desktop"
    Assert-Equal $renameResult.package $true "renamed-away product path package"
} finally {
    Pop-Location
    $resolvedTestRepo = [IO.Path]::GetFullPath($testRepo)
    $resolvedTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolvedTestRepo.StartsWith(
        $resolvedTempRoot,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw "Refusing to remove a classifier fixture outside the temp directory."
    }
    Remove-Item -LiteralPath $resolvedTestRepo -Recurse -Force
}

$unknown = Invoke-Case -Path @("tooling/new-input.json")
Assert-Equal $unknown.rust $true "unknown rust"
Assert-Equal $unknown.desktop $true "unknown desktop"
Assert-Equal $unknown.nativeAnalysis $true "unknown native analysis"
Assert-Equal $unknown.performanceSuites "Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup" "unknown suites"

Write-Host "CI classifier self-test passed ($assertions assertions)."
