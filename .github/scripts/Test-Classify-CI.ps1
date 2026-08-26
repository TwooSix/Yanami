$ErrorActionPreference = "Stop"
$classifier = Join-Path $PSScriptRoot "Classify-CI.ps1"
$assertions = 0

function Assert-Equal {
    param($Actual, $Expected, [string]$Label)
    $script:assertions++
    if ($Actual -cne $Expected) {
        throw "$Label expected '$Expected', got '$Actual'."
    }
}

function Invoke-Case {
    param([string[]]$Path)
    & $classifier -ChangedPath $Path -OutputPath "" -SummaryPath ""
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

$qml = Invoke-Case -Path @("apps/desktop/qml/components/StatusToast.qml")
Assert-Equal $qml.rust $false "qml rust"
Assert-Equal $qml.desktop $true "qml desktop"
Assert-Equal $qml.nativeAnalysis $false "qml native analysis"
Assert-Equal $qml.package $true "qml package"
Assert-Equal $qml.performanceSuites "Interaction,Startup" "qml suites"

$native = Invoke-Case -Path @("apps/desktop/native/WindowController.cpp")
Assert-Equal $native.desktop $true "native desktop"
Assert-Equal $native.nativeAnalysis $true "native analysis"

$perf = Invoke-Case -Path @("perf/policy/calibration-v1.json")
Assert-Equal $perf.rust $false "performance contract rust"
Assert-Equal $perf.desktop $false "performance contract desktop"
Assert-Equal $perf.performanceSuites "Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup" "performance contract suites"

$rustProbe = Invoke-Case -Path @("crates/yanami-performance-probe/src/main.rs")
Assert-Equal $rustProbe.rust $true "rust performance probe quality"
Assert-Equal $rustProbe.desktop $false "rust performance probe desktop"
Assert-Equal $rustProbe.performance $true "rust performance probe gate"
Assert-Equal $rustProbe.package $false "rust performance probe package"

$dependabot = Invoke-Case -Path @(".github/dependabot.yml")
Assert-Equal $dependabot.workflows $true "dependabot workflow lint"
Assert-Equal $dependabot.rust $false "dependabot rust"
Assert-Equal $dependabot.desktop $false "dependabot desktop"
Assert-Equal $dependabot.performance $false "dependabot performance"
Assert-Equal $dependabot.package $false "dependabot package"

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

$unknown = Invoke-Case -Path @("tooling/new-input.json")
Assert-Equal $unknown.rust $true "unknown rust"
Assert-Equal $unknown.desktop $true "unknown desktop"
Assert-Equal $unknown.nativeAnalysis $true "unknown native analysis"
Assert-Equal $unknown.performanceSuites "Search,Backend,Interaction,Playback,Danmaku,Upscaling,Startup" "unknown suites"

Write-Host "CI classifier self-test passed ($assertions assertions)."
