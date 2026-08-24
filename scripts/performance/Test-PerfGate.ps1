[CmdletBinding()]
param([switch]$KeepArtifacts)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$modulePath = Join-Path $PSScriptRoot "PerfGate.psm1"
Import-Module $modulePath -Force -ErrorAction Stop
$assertions = 0

function Assert-Equal {
    param([object]$Actual, [object]$Expected, [string]$Because)
    $script:assertions++
    if ([string]$Actual -ne [string]$Expected) { throw "Assertion failed: expected '$Expected', got '$Actual'. $Because" }
}

function Assert-True {
    param([bool]$Condition, [string]$Because)
    $script:assertions++
    if (-not $Condition) { throw "Assertion failed: $Because" }
}

function New-TestSlo {
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        id = "SLO-test"
        percentileMethod = "linear-r7"
        thresholdSetByProfile = [pscustomobject]@{ PullRequest = "hosted"; Lab = "strict"; Nightly = "strict"; Weekly = "strict"; Release = "strict" }
        defaultMinimumSamples = [pscustomobject]@{ hosted = 1; strict = 1 }
        relativeRules = [pscustomobject]@{ search_component = [pscustomobject]@{ percent = 10; absolute = 0.5 } }
        metrics = @(
            [pscustomobject][ordered]@{
                id = "test.latency"; suite = "search"; category = "search_component"; unit = "ms"; priority = "P0"; direction = "upper"; relativeStatistic = "p95"
                requiredProfiles = @("PullRequest", "Lab", "Nightly", "Weekly", "Release")
                absolute = [pscustomobject]@{
                    hosted = [pscustomobject]@{ p95 = [pscustomobject]@{ op = "<="; value = 10 } }
                    strict = [pscustomobject]@{ p95 = [pscustomobject]@{ op = "<="; value = 10 } }
                }
            },
            [pscustomobject][ordered]@{
                id = "test.correctness"; suite = "search"; category = "correctness"; unit = "ratio"; priority = "P0"; direction = "higher"; enforcement = "correctness"
                requiredProfiles = @("PullRequest", "Lab", "Nightly", "Weekly", "Release")
                absolute = [pscustomobject]@{
                    hosted = [pscustomobject]@{ min = [pscustomobject]@{ op = ">="; value = 1 } }
                    strict = [pscustomobject]@{ min = [pscustomobject]@{ op = ">="; value = 1 } }
                }
            }
        )
        invariants = @([pscustomobject]@{ id = "test.invariant"; suite = "search"; requiredProfiles = @("PullRequest", "Lab", "Nightly", "Weekly", "Release") })
    }
}

function New-TestPolicy {
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"; id = "policy-test"; currentMode = "collect"; allowedModes = @("collect", "debt", "enforce")
        modeRules = [pscustomobject]@{
            collect = [pscustomobject]@{ absolutePerformanceFailure = "debt"; relativePerformanceFailure = "debt"; correctnessFailure = "fail"; infraFailure = "infra-invalid" }
            debt = [pscustomobject]@{ absolutePerformanceFailure = "debt"; relativePerformanceFailure = "fail"; correctnessFailure = "fail"; infraFailure = "infra-invalid" }
            enforce = [pscustomobject]@{ absolutePerformanceFailure = "fail"; relativePerformanceFailure = "fail"; correctnessFailure = "fail"; infraFailure = "infra-invalid" }
        }
        releaseP0DebtIsFailure = $true; comparisonOrder = "A-B-A-B"; lockedMetrics = @(); referenceEnvironmentRequired = @("Lab", "Nightly", "Weekly", "Release")
        fixtureValidationRequired = @(); requiredFixturesBySuite = [pscustomobject]@{}
        missingProbeStatus = [pscustomobject]@{ PullRequest = "debt"; Lab = "infra-invalid"; Nightly = "infra-invalid"; Weekly = "infra-invalid"; Release = "infra-invalid" }
        exitCodes = [pscustomobject]@{ pass = 0; debt = 0; fail = 1; "infra-invalid" = 2 }
    }
}

function New-TestManifest {
    param(
        [double]$Latency = 5,
        [double]$Correctness = 1,
        [bool]$Invariant = $true,
        [string]$Profile = "PullRequest",
        [bool]$ReferenceMatch = $true,
        [string]$CandidateSha = "head-test"
    )
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"; runId = "test-$([guid]::NewGuid().ToString('N'))"; profile = $Profile; mode = "collect"; startedAtUtc = [DateTime]::UtcNow.ToString("o"); candidateSha = $CandidateSha
        environment = [pscustomobject]@{ fingerprint = "test-machine"; referenceMatch = $ReferenceMatch; mismatchReasons = @("self-test mismatch") }
        fixtures = @(); suites = @("search")
        metrics = @(
            [pscustomobject]@{ id = "test.latency"; unit = "ms"; samples = @($Latency, $Latency, $Latency, $Latency, $Latency) },
            [pscustomobject]@{ id = "test.correctness"; unit = "ratio"; samples = @($Correctness) }
        )
        invariants = @([pscustomobject]@{ id = "test.invariant"; passed = $Invariant; details = "self-test" })
    }
}

function Add-TestRunnerProvenance {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ProbeKind
    )
    $Manifest.environment | Add-Member -NotePropertyName runnerProvenance -NotePropertyValue ([pscustomobject][ordered]@{
        kind = "local-runner-generated"
        producer = "scripts/performance/run-gate.ps1"
        probeKind = $ProbeKind
        discoverySource = "self-test"
        candidateSha = [string]$Manifest.candidateSha
        runnerFingerprint = [string]$Manifest.environment.fingerprint
        artifactSha256 = ("a" * 64)
    }) -Force
    return $Manifest
}

function New-TestTrustedProducerAttestation {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ProbeKind
    )
    return [pscustomobject][ordered]@{
        trustSource = "current-runner-process"
        runId = [string]$Manifest.runId
        probeKind = $ProbeKind
        runnerFingerprint = [string]$Manifest.environment.fingerprint
        artifactSha256 = ("a" * 64)
    }
}

function Set-TestObservationProducerRunIds {
    param([Parameter(Mandatory = $true)][object]$Manifest)
    foreach ($metric in @($Manifest.metrics)) {
        $metric | Add-Member -NotePropertyName producerRunIds -NotePropertyValue @([string]$Manifest.runId) -Force
    }
    foreach ($invariant in @($Manifest.invariants)) {
        $invariant | Add-Member -NotePropertyName producerRunIds -NotePropertyValue @([string]$Manifest.runId) -Force
    }
    return $Manifest
}

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("yanami-perf-self-test-" + [guid]::NewGuid().ToString("N"))
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    foreach ($json in Get-ChildItem -LiteralPath (Join-Path $workspace "perf") -Filter *.json -File -Recurse) {
        [void](Read-YanamiPerfJson -Path $json.FullName)
    }
    $productionSlo = Read-YanamiPerfJson -Path (Join-Path $workspace "perf\slo\slo-v1.json")
    $productionPolicy = Read-YanamiPerfJson -Path (Join-Path $workspace "perf\policy\calibration-v1.json")
    $configurationErrors = @(Test-YanamiPerfConfiguration -Slo $productionSlo -Policy $productionPolicy)
    Assert-Equal $configurationErrors.Count 0 "Production SLO and policy must be internally valid."
    foreach ($metricId in @("search.index_open_ms", "search.index_rebuild_110k_ms", "search.incremental_1000_ms")) {
        $definition = @($productionSlo.metrics | Where-Object { [string]$_.id -eq $metricId })[0]
        Assert-Equal ([string]$definition.reproducibilityRule) "searchLatency" "Homogeneous search operation rounds must opt into the strict CV rule explicitly."
    }
    foreach ($metricId in @("search.query.hot_ms", "search.query.cold_ms", "search.query_during_incremental_ms")) {
        $definition = @($productionSlo.metrics | Where-Object { [string]$_.id -eq $metricId })[0]
        Assert-True ($null -eq $definition.PSObject.Properties["reproducibilityRule"]) "Heterogeneous per-request search samples must not be treated as repeated identical-workload CV evidence."
    }
    foreach ($definition in @($productionSlo.metrics | Where-Object { [string]$_.suite -eq "search" })) {
        Assert-True (-not [string]::IsNullOrWhiteSpace([string]$definition.evidence)) "Canonical Search metric '$($definition.id)' must declare its exact evidence class."
        Assert-True (-not [string]::IsNullOrWhiteSpace([string]$definition.producerProbeKind)) "Canonical Search metric '$($definition.id)' must require a runner-attested producer probe."
    }
    foreach ($definition in @($productionSlo.invariants | Where-Object { [string]$_.suite -eq "search" })) {
        Assert-True (-not [string]::IsNullOrWhiteSpace([string]$definition.evidence)) "Canonical Search invariant '$($definition.id)' must declare its exact evidence class."
        Assert-True (-not [string]::IsNullOrWhiteSpace([string]$definition.producerProbeKind)) "Canonical Search invariant '$($definition.id)' must require a runner-attested producer probe."
    }
    $expectedSuites = @("search", "backend", "interaction", "playback", "danmaku", "startup")
    Assert-Equal (@(Get-YanamiPerfSuites) -join ",") ($expectedSuites -join ",") "The canonical default suite list must contain all six suites in stable order."

    $danmakuMetricDefinitions = @($productionSlo.metrics | Where-Object { [string]$_.suite -eq "danmaku" })
    $danmakuInvariantDefinitions = @($productionSlo.invariants | Where-Object { [string]$_.suite -eq "danmaku" })
    foreach ($metricId in @(
        "danmaku.hosted_smoke.timeline_prepare_20k_ms",
        "danmaku.hosted_smoke.dense_seek_to_frame_candidate_ms",
        "danmaku.hosted_smoke.frame_interval_candidate_ms",
        "danmaku.cached_100k_to_first_pixel_present_ms",
        "danmaku.frame_interval_4k60_ms",
        "danmaku.eligible_presented_ratio",
        "danmaku.duplicate_early_or_stale_pixel_count"
    )) {
        Assert-Equal @($danmakuMetricDefinitions | Where-Object { [string]$_.id -eq $metricId }).Count 1 "Danmaku SLO metric '$metricId' must remain canonical and unique."
    }
    $hostedDanmakuMetrics = @($danmakuMetricDefinitions | Where-Object { [string]$_.id -like "danmaku.hosted_smoke.*" })
    Assert-True ($hostedDanmakuMetrics.Count -gt 0) "The PullRequest suite must retain hosted-only danmaku catastrophe metrics."
    Assert-Equal @($hostedDanmakuMetrics | Where-Object { @($_.requiredProfiles).Count -ne 1 -or "PullRequest" -notin @($_.requiredProfiles) }).Count 0 "Hosted danmaku metrics must never be required by a strict profile."
    $strictDanmakuMetrics = @($danmakuMetricDefinitions | Where-Object { "PullRequest" -notin @($_.requiredProfiles) })
    Assert-True ($strictDanmakuMetrics.Count -gt 0) "The fixed-machine danmaku contract must contain strict metrics."
    Assert-Equal @($strictDanmakuMetrics | Where-Object { [string]::IsNullOrWhiteSpace([string]$_.evidence) }).Count 0 "Every strict danmaku metric must declare its evidence class."

    $expectedDanmakuInvariants = @(
        "danmaku.fixture_hash_and_oracle_valid",
        "danmaku.measurement_hooks_valid",
        "danmaku.timeline_semantics_valid",
        "danmaku.no_stale_generation",
        "danmaku.pause_buffer_rate_sync_valid",
        "danmaku.style_filter_updates_stable",
        "danmaku.no_unbounded_queue_or_delegate_growth",
        "danmaku.video_playback_uninterrupted",
        "danmaku.external_pixel_evidence_valid",
        "danmaku.strict_matrix_complete"
    )
    foreach ($invariantId in $expectedDanmakuInvariants) {
        Assert-Equal @($danmakuInvariantDefinitions | Where-Object { [string]$_.id -eq $invariantId }).Count 1 "Danmaku invariant '$invariantId' must remain canonical and unique."
    }
    foreach ($invariantId in $expectedDanmakuInvariants[0..6]) {
        $definition = @($danmakuInvariantDefinitions | Where-Object { [string]$_.id -eq $invariantId })[0]
        Assert-True ("PullRequest" -in @($definition.requiredProfiles)) "Hosted correctness invariant '$invariantId' must be required on PullRequest."
    }
    foreach ($invariantId in $expectedDanmakuInvariants[7..9]) {
        $definition = @($danmakuInvariantDefinitions | Where-Object { [string]$_.id -eq $invariantId })[0]
        Assert-True ("PullRequest" -notin @($definition.requiredProfiles) -and "Lab" -in @($definition.requiredProfiles)) "Strict-only invariant '$invariantId' must not be satisfiable by the hosted probe."
    }

    Assert-Equal (@(Get-YanamiRequiredFixtureIds -Policy $productionPolicy -Suite danmaku -Profile PullRequest) -join ",") "DanmakuDensity-v1" "PullRequest danmaku uses only the deterministic density corpus."
    foreach ($strictProfile in @("Lab", "Nightly", "Weekly", "Release")) {
        Assert-Equal (@(Get-YanamiRequiredFixtureIds -Policy $productionPolicy -Suite danmaku -Profile $strictProfile) -join ",") "DanmakuDensity-v1,PlaybackMedia-v1" "Strict danmaku profile '$strictProfile' must pair the density corpus with pinned playback media."
    }
    $fallbackFixturePolicy = [pscustomobject]@{
        requiredFixturesBySuite = [pscustomobject]@{
            danmaku = [pscustomobject]@{
                PullRequest = "DanmakuDensity-v1"
                default = @("DanmakuDensity-v1", "PlaybackMedia-v1")
            }
        }
    }
    Assert-Equal (@(Get-YanamiRequiredFixtureIds -Policy $fallbackFixturePolicy -Suite danmaku -Profile PullRequest) -join ",") "DanmakuDensity-v1" "A profile-specific fixture mapping must take precedence over default."
    Assert-Equal (@(Get-YanamiRequiredFixtureIds -Policy $fallbackFixturePolicy -Suite danmaku -Profile Nightly) -join ",") "DanmakuDensity-v1,PlaybackMedia-v1" "The fixture resolver must use the default mapping when a profile is absent."

    $invalidMetricSuiteSlo = $productionSlo | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $invalidMetricSuiteSlo.metrics[0].suite = "danmakuu"
    $invalidMetricSuiteErrors = @(Test-YanamiPerfConfiguration -Slo $invalidMetricSuiteSlo -Policy $productionPolicy)
    Assert-True (@($invalidMetricSuiteErrors | Where-Object { $_ -match "unknown suite 'danmakuu'" }).Count -eq 1) "Configuration validation must reject an unknown metric suite."
    $invalidInvariantSuiteSlo = $productionSlo | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $invalidInvariantSuiteSlo.invariants[0].suite = "danmakuu"
    $invalidInvariantSuiteErrors = @(Test-YanamiPerfConfiguration -Slo $invalidInvariantSuiteSlo -Policy $productionPolicy)
    Assert-True (@($invalidInvariantSuiteErrors | Where-Object { $_ -match "unknown suite 'danmakuu'" }).Count -eq 1) "Configuration validation must reject an unknown invariant suite."
    $invalidFixtureSuitePolicy = $productionPolicy | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $invalidFixtureSuitePolicy.requiredFixturesBySuite | Add-Member -NotePropertyName danmakuu -NotePropertyValue "DanmakuDensity-v1"
    $invalidFixtureSuiteErrors = @(Test-YanamiPerfConfiguration -Slo $productionSlo -Policy $invalidFixtureSuitePolicy)
    Assert-True (@($invalidFixtureSuiteErrors | Where-Object { $_ -match "Fixture policy has unknown suite 'danmakuu'" }).Count -eq 1) "Configuration validation must reject an unknown fixture-policy suite."
    $coldStartupMetrics = @($productionSlo.metrics | Where-Object { [string]$_.id -like "startup.cold.*" })
    Assert-True ($coldStartupMetrics.Count -gt 0 -and @($coldStartupMetrics | Where-Object { [int]$_.minimumSamplesByProfile.Nightly -lt 20 }).Count -eq 0) "Nightly cold-start percentiles require at least 20 accumulated samples."
    Assert-True (@($coldStartupMetrics | Where-Object { [int]$_.minimumSamplesByStatistic.p99 -lt 50 }).Count -eq 0) "Cold-start p99 must remain deferred until 50 accumulated samples exist."

    Assert-Equal ([math]::Round((Get-YanamiPercentile -Values @(1, 2, 3, 4, 5) -Probability 0.95), 6)) 4.8 "Percentiles must use linear R-7 interpolation."
    $slo = New-TestSlo
    $policy = New-TestPolicy

    $deferredStatisticSlo = New-TestSlo
    $deferredStatisticSlo.metrics[0].absolute.hosted | Add-Member -NotePropertyName p99 -NotePropertyValue ([pscustomobject]@{ op = "<="; value = 10 })
    $deferredStatisticSlo.metrics[0] | Add-Member -NotePropertyName minimumSamplesByStatistic -NotePropertyValue ([pscustomobject]@{ p99 = 10 })
    $deferredStatisticResult = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Latency 5) -Slo $deferredStatisticSlo -Policy $policy -Mode collect -Suites search
    $deferredLatencyMetric = @($deferredStatisticResult.metrics | Where-Object id -eq "test.latency")[0]
    Assert-True ($null -eq $deferredLatencyMetric.absoluteTarget.PSObject.Properties["p99"]) "A percentile must not appear as evaluated before its statistic-specific sample floor is reached."
    Assert-Equal @($deferredLatencyMetric.deferredStatistics).Count 1 "Deferred percentile evidence must be explicit in PerfResult."

    $slow = New-TestManifest -Latency 20
    $collect = Invoke-YanamiPerfEvaluation -Manifest $slow -Slo $slo -Policy $policy -Mode collect -Suites search
    Assert-Equal $collect.status "debt" "Collect mode records absolute performance misses without blocking."
    $debt = Invoke-YanamiPerfEvaluation -Manifest $slow -Slo $slo -Policy $policy -Mode debt -Suites search
    Assert-Equal $debt.status "debt" "Debt mode preserves existing absolute debt when there is no relative regression."
    $enforce = Invoke-YanamiPerfEvaluation -Manifest $slow -Slo $slo -Policy $policy -Mode enforce -Suites search
    Assert-Equal $enforce.status "fail" "Enforce mode blocks absolute performance misses."

    $incorrect = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Correctness 0) -Slo $slo -Policy $policy -Mode collect -Suites search
    Assert-Equal $incorrect.status "fail" "Correctness metrics fail immediately even during collection."
    $invariantFailure = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Invariant $false) -Slo $slo -Policy $policy -Mode collect -Suites search
    Assert-Equal $invariantFailure.status "fail" "Correctness invariants fail immediately even during collection."

    $fixtureObservationManifest = New-TestManifest
    $fixtureObservationManifest.metrics = @(
        [pscustomobject]@{
            id = "search.component.fixture.query_hot_ms"; unit = "ms"; samples = @(1, 1, 1)
            attributes = [pscustomobject]@{ evidence = "fixture-component-observation"; enforcement = "observation" }
        },
        [pscustomobject]@{
            id = "search.component.fixture.exact_title_rank1_ratio"; unit = "ratio"; samples = @(0.75)
            attributes = [pscustomobject]@{ evidence = "fixture-component-observation"; enforcement = "observation" }
        }
    )
    $fixtureObservationManifest.invariants = @([pscustomobject]@{
        id = "search.component.fixture.final_query_matches_snapshot"; passed = $false
        details = [pscustomobject]@{ evidence = "fixture-component-observation"; enforcement = "observation" }
    })
    $observationOnlySlo = New-TestSlo
    $observationOnlySlo.metrics = @()
    $observationOnlySlo.invariants = @()
    $fixtureObservationResult = Invoke-YanamiPerfEvaluation -Manifest $fixtureObservationManifest -Slo $observationOnlySlo -Policy $policy -Mode collect -Suites search
    Assert-Equal $fixtureObservationResult.status "pass" "A failed fixture-component observation must not become a product correctness failure."
    Assert-Equal @($fixtureObservationResult.metrics).Count 2 "Fixture-component metrics must be retained for diagnostics."
    Assert-Equal @($fixtureObservationResult.metrics | Where-Object { $_.suite -ne "observation" -or $_.category -ne "observation" }).Count 0 "Uncontracted fixture-component metrics must remain observations."

    $canonicalSearchSlo = New-TestSlo
    $canonicalSearchSlo.metrics = @($canonicalSearchSlo.metrics[0])
    $canonicalSearchSlo.metrics[0].id = "search.query.hot_ms"
    $canonicalSearchSlo.invariants = @([pscustomobject]@{
        id = "search.final_query_complete"; suite = "search"
        requiredProfiles = @("PullRequest", "Lab", "Nightly", "Weekly", "Release")
    })
    $canonicalSearchPolicy = New-TestPolicy
    $canonicalSearchPolicy | Add-Member -NotePropertyName missingMeasurementStatusByMode -NotePropertyValue ([pscustomobject]@{ collect = "debt"; debt = "infra-invalid"; enforce = "infra-invalid" })
    $canonicalSearchResult = Invoke-YanamiPerfEvaluation -Manifest $fixtureObservationManifest -Slo $canonicalSearchSlo -Policy $canonicalSearchPolicy -Mode enforce -Suites search
    Assert-Equal $canonicalSearchResult.status "infra-invalid" "Fixture-component names must not satisfy canonical Search evidence requirements."
    Assert-True (@($canonicalSearchResult.reasons | Where-Object { $_ -match "Required metric 'search.query.hot_ms' is missing" }).Count -eq 1) "The canonical hot-query metric must remain explicitly missing."
    Assert-True (@($canonicalSearchResult.reasons | Where-Object { $_ -match "Required invariant 'search.final_query_complete' is missing" }).Count -eq 1) "The canonical final-query invariant must remain explicitly missing."

    $attestedSearchSlo = New-TestSlo
    $attestedSearchSlo.metrics = @($attestedSearchSlo.metrics[0])
    $attestedSearchSlo.metrics[0].id = "search.index_rebuild_110k_ms"
    $attestedSearchSlo.metrics[0] | Add-Member -NotePropertyName evidence -NotePropertyValue "production-media-catalog" -Force
    $attestedSearchSlo.metrics[0] | Add-Member -NotePropertyName producerProbeKind -NotePropertyValue "rust-production-component-probe" -Force
    $attestedSearchSlo.invariants = @()
    $forgedSearchManifest = New-TestManifest
    $forgedSearchManifest.metrics = @([pscustomobject]@{
        id = "search.index_rebuild_110k_ms"
        unit = "ms"
        samples = @(1, 1, 1, 1, 1)
        attributes = [pscustomobject]@{ evidence = "fixture-component-observation" }
    })
    $forgedSearchManifest.invariants = @()
    $forgedEvidenceResult = Invoke-YanamiPerfEvaluation -Manifest $forgedSearchManifest -Slo $attestedSearchSlo -Policy $policy -Mode enforce -Suites search
    Assert-Equal $forgedEvidenceResult.status "infra-invalid" "A canonical Search ID with fixture-only evidence must fail closed."
    Assert-True (@($forgedEvidenceResult.reasons | Where-Object { $_ -match "requires 'production-media-catalog' evidence" }).Count -eq 1) "Canonical Search forgery rejection must name the missing production evidence class."
    $forgedSearchManifest.metrics[0].attributes.evidence = "production-media-catalog"
    $unattestedSearchResult = Invoke-YanamiPerfEvaluation -Manifest $forgedSearchManifest -Slo $attestedSearchSlo -Policy $policy -Mode enforce -Suites search
    Assert-Equal $unattestedSearchResult.status "infra-invalid" "An evidence label without local runner provenance must not certify a canonical Search metric."
    Assert-True (@($unattestedSearchResult.reasons | Where-Object { $_ -match "requires trusted current-process attestation for producer probe 'rust-production-component-probe'" }).Count -eq 1) "Search provenance rejection must name the required production probe."
    [void](Add-TestRunnerProvenance -Manifest $forgedSearchManifest -ProbeKind "rust-production-component-probe")
    [void](Set-TestObservationProducerRunIds -Manifest $forgedSearchManifest)
    $selfDescribedSearchResult = Invoke-YanamiPerfEvaluation -Manifest $forgedSearchManifest -Slo $attestedSearchSlo -Policy $policy -Mode enforce -Suites search
    Assert-Equal $selfDescribedSearchResult.status "infra-invalid" "A fully self-described provenance object and hash-shaped value must remain untrusted without the run-gate process ledger."
    $trustedRustAttestation = New-TestTrustedProducerAttestation -Manifest $forgedSearchManifest -ProbeKind "rust-production-component-probe"
    $attestedSearchResult = Invoke-YanamiPerfEvaluation -Manifest $forgedSearchManifest -Slo $attestedSearchSlo -Policy $policy -Mode enforce -Suites search -TrustedProducerAttestations @($trustedRustAttestation)
    Assert-Equal $attestedSearchResult.status "pass" "Matching evidence plus runner-attested production probe provenance must satisfy the canonical Search source contract."

    $externalSearchIds = @(
        [pscustomobject]@{ id = "search.input_to_model_ms"; unit = "ms"; statistic = "p95" },
        [pscustomobject]@{ id = "search.input_to_present_ms"; unit = "ms"; statistic = "p95" },
        [pscustomobject]@{ id = "search.frame_over_20_ratio"; unit = "ratio"; statistic = "max" },
        [pscustomobject]@{ id = "search.frame_over_33_ratio"; unit = "ratio"; statistic = "max" }
    )
    $externalGraftSlo = New-TestSlo
    $externalGraftSlo.metrics = @(
        foreach ($externalMetric in $externalSearchIds) {
            $target = [pscustomobject]@{}
            $target | Add-Member -NotePropertyName $externalMetric.statistic -NotePropertyValue ([pscustomobject]@{ op = "<="; value = 10 })
            [pscustomobject][ordered]@{
                id = $externalMetric.id
                suite = "search"
                category = $(if ($externalMetric.unit -eq "ms") { "ui_present" } else { "ratio" })
                unit = $externalMetric.unit
                priority = "P0"
                direction = "upper"
                relativeStatistic = $externalMetric.statistic
                evidence = "external-present"
                producerProbeKind = "desktop-runtime"
                minimumSamples = [pscustomobject]@{ hosted = 1 }
                requiredProfiles = @("PullRequest")
                absolute = [pscustomobject]@{ hosted = $target }
            }
        }
    )
    $externalGraftSlo.invariants = @()
    $forgedExternalManifest = New-TestManifest
    $forgedExternalManifest.metrics = @(
        foreach ($externalMetric in $externalSearchIds) {
            [pscustomobject]@{
                id = $externalMetric.id
                unit = $externalMetric.unit
                samples = @(1)
                attributes = [pscustomobject]@{ evidence = "external-present" }
            }
        }
    )
    $forgedExternalManifest.invariants = @()
    [void](Set-TestObservationProducerRunIds -Manifest $forgedExternalManifest)
    $realDesktopRuntimeManifest = New-TestManifest
    $realDesktopRuntimeManifest.metrics = @([pscustomobject]@{
        id = "desktop.runtime.startup_observation_ms"
        unit = "ms"
        samples = @(2)
        attributes = [pscustomobject]@{ evidence = "runtime-observation" }
    })
    $realDesktopRuntimeManifest.invariants = @()
    [void](Add-TestRunnerProvenance -Manifest $realDesktopRuntimeManifest -ProbeKind "desktop-runtime")
    [void](Set-TestObservationProducerRunIds -Manifest $realDesktopRuntimeManifest)
    $externalGraftManifest = Merge-YanamiRunManifests -Manifests @($forgedExternalManifest, $realDesktopRuntimeManifest)
    $trustedDesktopRuntimeAttestation = New-TestTrustedProducerAttestation -Manifest $realDesktopRuntimeManifest -ProbeKind "desktop-runtime"
    $externalGraftResult = Invoke-YanamiPerfEvaluation -Manifest $externalGraftManifest -Slo $externalGraftSlo -Policy $policy -Mode enforce -Suites search -TrustedProducerAttestations @($trustedDesktopRuntimeAttestation)
    Assert-Equal $externalGraftResult.status "infra-invalid" "Forged external Search observations must not borrow another source manifest's trusted desktop-runtime attestation."
    Assert-Equal @($externalGraftResult.reasons | Where-Object { $_ -match "requires trusted current-process attestation for producer probe 'desktop-runtime'" }).Count 4 "All four forged external Search metrics must fail source-bound attestation independently."
    Assert-Equal @($externalGraftResult.metrics | Where-Object id -eq "desktop.runtime.startup_observation_ms").Count 1 "The genuine desktop runtime source's own observation must remain available when grafted Search metrics are rejected."
    $runIdCollisionManifest = New-TestManifest
    $runIdCollisionManifest.runId = $realDesktopRuntimeManifest.runId
    $runIdCollisionRejected = $false
    try { [void](Merge-YanamiRunManifests -Manifests @($realDesktopRuntimeManifest, $runIdCollisionManifest)) }
    catch { $runIdCollisionRejected = $true }
    Assert-True $runIdCollisionRejected "A forged source manifest cannot collide with a locally attested runId during merge."

    $graftDesktopExecutable = if ($env:YANAMI_TEST_DESKTOP_EXECUTABLE) {
        [System.IO.Path]::GetFullPath($env:YANAMI_TEST_DESKTOP_EXECUTABLE)
    } else {
        Join-Path $workspace "build\desktop-windows\yanami-desktop.exe"
    }
    if (Test-Path -LiteralPath $graftDesktopExecutable -PathType Leaf) {
        $graftRunnerSloPath = Join-Path $testRoot "graft-runner-slo.json"
        $graftRunnerPolicyPath = Join-Path $testRoot "graft-runner-policy.json"
        $graftRunnerManifestPath = Join-Path $testRoot "graft-runner-manifest.json"
        $graftRunnerOutput = Join-Path $testRoot "graft-runner"
        $externalGraftSlo | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $graftRunnerSloPath -Encoding UTF8
        $policy | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $graftRunnerPolicyPath -Encoding UTF8
        $graftRunnerManifest = $forgedExternalManifest | ConvertTo-Json -Depth 100 | ConvertFrom-Json
        $graftRunnerManifest.mode = "enforce"
        $graftRunnerManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $graftRunnerManifestPath -Encoding UTF8
        $graftRunnerResult = & (Join-Path $PSScriptRoot "run-gate.ps1") `
            -Profile PullRequest `
            -OutputDirectory $graftRunnerOutput `
            -Suites search `
            -InputPath $graftRunnerManifestPath `
            -DesktopExecutable $graftDesktopExecutable `
            -CandidateSha "head-test" `
            -Mode enforce `
            -SloPath $graftRunnerSloPath `
            -PolicyPath $graftRunnerPolicyPath `
            -SkipProbeDiscovery `
            -NoExit
        Assert-Equal $graftRunnerResult.status "infra-invalid" "A real locally attested desktop runtime must not lend trust to four forged Search external metrics imported from another run."
        Assert-Equal @($graftRunnerResult.reasons | Where-Object { $_ -match "requires trusted current-process attestation for producer probe 'desktop-runtime'" }).Count 4 "The public run-gate graft path must reject all four forged external metrics by their bound source runId."
        Assert-True (@($graftRunnerResult.metrics | Where-Object { [string]$_.id -like "startup.*" -or [string]$_.id -like "interaction.*" }).Count -gt 0) "The genuine desktop runtime's own observations must remain present in the same graft-negative result."
    }

    $forgedInputSloPath = Join-Path $testRoot "forged-input-slo.json"
    $forgedInputPolicyPath = Join-Path $testRoot "forged-input-policy.json"
    $forgedInputManifestPath = Join-Path $testRoot "forged-input-manifest.json"
    $forgedInputOutput = Join-Path $testRoot "forged-input-runner"
    $attestedSearchSlo | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $forgedInputSloPath -Encoding UTF8
    $policy | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $forgedInputPolicyPath -Encoding UTF8
    $forgedSearchManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $forgedInputManifestPath -Encoding UTF8
    $forgedInputResult = & (Join-Path $PSScriptRoot "run-gate.ps1") `
        -Profile PullRequest `
        -OutputDirectory $forgedInputOutput `
        -Suites search `
        -InputPath $forgedInputManifestPath `
        -CandidateSha "head-test" `
        -Mode enforce `
        -SloPath $forgedInputSloPath `
        -PolicyPath $forgedInputPolicyPath `
        -SkipProbeDiscovery `
        -NoExit
    Assert-Equal $forgedInputResult.status "infra-invalid" "Public run-gate InputPath must reject a fully self-described canonical Search producer forgery."
    Assert-True (@($forgedInputResult.reasons | Where-Object { $_ -match "self-described manifest provenance is insufficient" }).Count -eq 1) "The public InputPath forgery rejection must prove that only the in-process trusted producer ledger establishes trust."

    $schedulerInvariantIds = @(
        "search.no_stale_commit",
        "search.final_query_complete",
        "search.queue_bounded",
        "search.no_duplicates_or_cross_session_results"
    )
    $storageProbeManifest = New-TestManifest
    $coordinatorProbeManifest = New-TestManifest
    $coordinatorProbeManifest.metrics = @()
    $coordinatorProbeManifest.invariants = @(
        foreach ($invariantId in $schedulerInvariantIds) {
            [pscustomobject]@{
                id = $invariantId
                passed = $true
                details = [pscustomobject]@{
                    evidence = "production-search-coordinator-delayed-fake"
                    submitted = 4
                    published = 1
                    maxQueueDepth = 1
                }
            }
        }
    )
    [void](Add-TestRunnerProvenance -Manifest $storageProbeManifest -ProbeKind "rust-production-component-probe")
    [void](Add-TestRunnerProvenance -Manifest $coordinatorProbeManifest -ProbeKind "desktop-component-probe")
    [void](Set-TestObservationProducerRunIds -Manifest $storageProbeManifest)
    [void](Set-TestObservationProducerRunIds -Manifest $coordinatorProbeManifest)
    $mergedSearchProbeManifest = Merge-YanamiRunManifests -Manifests @($storageProbeManifest, $coordinatorProbeManifest)
    foreach ($invariantId in $schedulerInvariantIds) {
        Assert-Equal @($mergedSearchProbeManifest.invariants | Where-Object { [string]$_.id -eq $invariantId -and [bool]$_.passed }).Count 1 "A desktop production-path scheduler invariant must merge into the Rust Search manifest without being fabricated by storage evidence."
    }
    $schedulerOnlySlo = New-TestSlo
    $schedulerOnlySlo.metrics = @()
    $schedulerOnlySlo.invariants = @(
        foreach ($invariantId in $schedulerInvariantIds) {
            [pscustomobject]@{
                id = $invariantId
                suite = "search"
                evidence = "production-search-coordinator-delayed-fake"
                producerProbeKind = "desktop-component-probe"
                requiredProfiles = @("PullRequest")
            }
        }
    )
    $trustedSearchProbeAttestations = @(
        New-TestTrustedProducerAttestation -Manifest $storageProbeManifest -ProbeKind "rust-production-component-probe"
        New-TestTrustedProducerAttestation -Manifest $coordinatorProbeManifest -ProbeKind "desktop-component-probe"
    )
    $mergedSchedulerResult = Invoke-YanamiPerfEvaluation -Manifest $mergedSearchProbeManifest -Slo $schedulerOnlySlo -Policy $policy -Mode collect -Suites search -TrustedProducerAttestations $trustedSearchProbeAttestations
    Assert-Equal $mergedSchedulerResult.status "pass" "Four directly observed SearchCoordinator invariants must satisfy the canonical scheduler contract after manifest merge."

    $badEnvironment = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Profile Lab -ReferenceMatch $false) -Slo $slo -Policy $policy -Mode collect -Suites search
    Assert-Equal $badEnvironment.status "infra-invalid" "Strict profiles reject a mismatched reference environment."
    $evidenceSlo = New-TestSlo
    $evidenceSlo.metrics[0] | Add-Member -NotePropertyName evidence -NotePropertyValue "controlled-network"
    $wrongEvidenceManifest = New-TestManifest -Profile Lab -ReferenceMatch $true
    $wrongEvidenceManifest.metrics[0] | Add-Member -NotePropertyName attributes -NotePropertyValue ([pscustomobject]@{ evidence = "in-process-server-delay" })
    $wrongEvidenceResult = Invoke-YanamiPerfEvaluation -Manifest $wrongEvidenceManifest -Slo $evidenceSlo -Policy $policy -Mode collect -Suites search
    Assert-Equal $wrongEvidenceResult.status "infra-invalid" "A synthetic delay cannot satisfy a strict controlled-network evidence contract."

    $strictDanmakuSlo = New-TestSlo
    $strictDanmakuSlo.metrics = @(
        $productionSlo.metrics |
            Where-Object { [string]$_.id -eq "danmaku.cached_100k_to_first_pixel_present_ms" } |
            ConvertTo-Json -Depth 100 |
            ConvertFrom-Json
    )
    $strictDanmakuSlo.invariants = @()
    $offscreenDanmakuManifest = New-TestManifest -Profile Lab -ReferenceMatch $true
    $offscreenDanmakuManifest.suites = @("danmaku")
    $offscreenDanmakuManifest.metrics = @([pscustomobject]@{
        id = "danmaku.cached_100k_to_first_pixel_present_ms"
        unit = "ms"
        samples = @(100, 100, 100, 100, 100, 100, 100, 100, 100, 100)
        attributes = [pscustomobject]@{
            evidence = "qt-offscreen-software"
            enforcement = "hosted-catastrophe-only"
            strictAbsoluteEvidence = $false
            qpaPlatform = "offscreen"
            quickBackendEnvironment = "software"
            rendererApi = "Software"
        }
    })
    $offscreenDanmakuManifest.invariants = @()
    $offscreenDanmakuResult = Invoke-YanamiPerfEvaluation -Manifest $offscreenDanmakuManifest -Slo $strictDanmakuSlo -Policy $policy -Mode collect -Suites danmaku
    Assert-Equal $offscreenDanmakuResult.status "infra-invalid" "Qt offscreen/software samples cannot satisfy a strict external pixel-Present metric."
    Assert-True (@($offscreenDanmakuResult.reasons | Where-Object { $_ -match "requires 'external-pixel-present' evidence; 'qt-offscreen-software' was reported" }).Count -eq 1) "Strict danmaku evidence rejection must name both the required and supplied evidence classes."
    $firstMachineManifest = New-TestManifest -Profile Lab -ReferenceMatch $true
    $secondMachineManifest = New-TestManifest -Profile Lab -ReferenceMatch $true
    $secondMachineManifest.environment.fingerprint = "different-test-machine"
    $crossMachineMerge = Merge-YanamiRunManifests -Manifests @($firstMachineManifest, $secondMachineManifest)
    Assert-Equal $crossMachineMerge.environment.referenceMatch $false "Accumulated percentile samples from different machine fingerprints are invalid."
    $crossMachineResult = Invoke-YanamiPerfEvaluation -Manifest $crossMachineMerge -Slo $slo -Policy $policy -Mode collect -Suites search
    Assert-Equal $crossMachineResult.status "infra-invalid" "A cross-machine aggregate cannot make a strict product decision."

    $duplicateRunRejected = $false
    try { [void](Merge-YanamiRunManifests -Manifests @($firstMachineManifest, $firstMachineManifest)) }
    catch { $duplicateRunRejected = $true }
    Assert-True $duplicateRunRejected "A duplicated run manifest must not be counted twice toward a sample floor."

    $candidateA = New-TestManifest
    $candidateB = New-TestManifest
    $candidateA.candidateSha = "candidate-a"
    $candidateB.candidateSha = "candidate-b"
    $candidateMismatchRejected = $false
    try { [void](Merge-YanamiRunManifests -Manifests @($candidateA, $candidateB)) }
    catch { $candidateMismatchRejected = $true }
    Assert-True $candidateMismatchRejected "Samples from different candidate commits must not be merged."

    $attributeA = New-TestManifest
    $attributeB = New-TestManifest
    $attributeA.metrics[0] | Add-Member -NotePropertyName attributes -NotePropertyValue ([pscustomobject][ordered]@{ evidence = "controlled"; scope = "same" })
    $attributeB.metrics[0] | Add-Member -NotePropertyName attributes -NotePropertyValue ([pscustomobject][ordered]@{ scope = "same"; evidence = "controlled" })
    $attributeMerge = Merge-YanamiRunManifests -Manifests @($attributeA, $attributeB)
    Assert-Equal @($attributeMerge.metrics | Where-Object id -eq "test.latency")[0].samples.Count 10 "Equivalent metric attributes must compare canonically rather than by JSON property order."
    $attributeB.metrics[0].attributes.scope = "different"
    $attributeMismatchRejected = $false
    try { [void](Merge-YanamiRunManifests -Manifests @($attributeA, $attributeB)) }
    catch { $attributeMismatchRejected = $true }
    Assert-True $attributeMismatchRejected "Samples with different measurement provenance must not be merged."

    $stabilitySlo = New-TestSlo
    $stabilitySlo | Add-Member -NotePropertyName reproducibility -NotePropertyValue ([pscustomobject]@{
        searchLatency = [pscustomobject]@{ maximumCv = 0.05; minimumSamples = 10 }
        warmStartup = [pscustomobject]@{ maximumCv = 0.05; minimumSamples = 30 }
        coldStartup = [pscustomobject]@{ maximumCv = 0.10; minimumSamples = 20 }
    }) -Force
    $stabilitySlo.metrics[0] | Add-Member -NotePropertyName minimumSamplesByProfile -NotePropertyValue ([pscustomobject]@{ Lab = 10 }) -Force
    $unstableManifest = New-TestManifest -Profile Lab
    $unstableManifest.metrics[0].samples = @(1, 9, 1, 9, 1, 9, 1, 9, 1, 9)
    $heterogeneousResult = Invoke-YanamiPerfEvaluation -Manifest $unstableManifest -Slo $stabilitySlo -Policy $policy -Mode collect -Suites search
    Assert-Equal $heterogeneousResult.status "pass" "Heterogeneous request samples without an explicit reproducibility rule must still be judged by their absolute SLO, not raw-sample CV."
    $stabilitySlo.metrics[0] | Add-Member -NotePropertyName reproducibilityRule -NotePropertyValue "searchLatency" -Force
    $unstableResult = Invoke-YanamiPerfEvaluation -Manifest $unstableManifest -Slo $stabilitySlo -Policy $policy -Mode collect -Suites search
    Assert-Equal $unstableResult.status "infra-invalid" "A strict search run above the 5% CV limit is too unstable for a product decision."
    $undersampledManifest = New-TestManifest -Profile Lab
    $undersampledResult = Invoke-YanamiPerfEvaluation -Manifest $undersampledManifest -Slo $stabilitySlo -Policy $policy -Mode collect -Suites search
    Assert-Equal $undersampledResult.status "infra-invalid" "Profile-specific sample counts must be enforced before percentile decisions."
    $stabilitySlo.metrics[0].minimumSamplesByProfile.Lab = 5
    $deferredStabilityResult = Invoke-YanamiPerfEvaluation -Manifest $undersampledManifest -Slo $stabilitySlo -Policy $policy -Mode collect -Suites search
    Assert-Equal $deferredStabilityResult.status "debt" "A valid absolute measurement with too few homogeneous rounds must defer reproducibility as measurement debt rather than silently pass."
    $stabilitySlo.metrics[0].minimumSamplesByProfile | Add-Member -NotePropertyName Release -NotePropertyValue 3 -Force
    $releaseStabilityManifest = New-TestManifest -Profile Release
    $releaseStabilityManifest.metrics[0].samples = @(5, 5, 5)
    $releaseStabilityResult = Invoke-YanamiPerfEvaluation -Manifest $releaseStabilityManifest -Slo $stabilitySlo -Policy $policy -Mode enforce -Suites search
    $releaseStabilityMetric = @($releaseStabilityResult.metrics | Where-Object id -eq "test.latency")[0]
    Assert-Equal $releaseStabilityMetric.status "fail" "Release P0 reproducibility evidence below its homogeneous sample floor must be a blocking metric failure."
    Assert-Equal $releaseStabilityResult.status "fail" "Release/enforce must never greenlight a P0 metric with deferred reproducibility evidence."

    $base = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Latency 5) -Slo $slo -Policy $policy -Mode enforce -Suites search
    $regression = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Latency 6) -Slo $slo -Policy $policy -BaseResult $base -Mode debt -Suites search
    Assert-Equal $regression.status "fail" "Debt mode blocks only when both the 10% and 0.5ms relative limits are exceeded."
    $smallChange = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Latency 5.4) -Slo $slo -Policy $policy -BaseResult $base -Mode debt -Suites search
    Assert-Equal $smallChange.status "pass" "Relative comparison does not fail when neither strict greater-than limit is exceeded."

    $fixtureSlo = New-TestSlo
    $fixtureSlo.metrics = @()
    $fixtureSlo.invariants = @()
    $fixturePolicy = New-TestPolicy
    $fixturePolicy.fixtureValidationRequired = @("PullRequest")
    $fixturePolicy.requiredFixturesBySuite = [pscustomobject]@{ playback = "PlaybackMedia-v1" }
    $fixturePolicy | Add-Member -NotePropertyName missingMeasurementStatusByMode -NotePropertyValue ([pscustomobject]@{ collect = "debt"; debt = "infra-invalid"; enforce = "infra-invalid" }) -Force
    $missingFixtureManifest = New-TestManifest
    $missingFixtureManifest.suites = @("playback")
    $missingFixtureManifest.metrics = @()
    $missingFixtureManifest.invariants = @()
    $missingFixture = Invoke-YanamiPerfEvaluation -Manifest $missingFixtureManifest -Slo $fixtureSlo -Policy $fixturePolicy -Mode collect -Suites playback
    Assert-Equal $missingFixture.status "debt" "A fixture absent during collect is measurement debt rather than false infrastructure evidence."
    $missingFixtureManifest.fixtures = @([pscustomobject]@{ id = "PlaybackMedia-v1"; version = "1"; sha256 = ("0" * 64); validated = $false })
    $invalidFixture = Invoke-YanamiPerfEvaluation -Manifest $missingFixtureManifest -Slo $fixtureSlo -Policy $fixturePolicy -Mode collect -Suites playback
    Assert-Equal $invalidFixture.status "infra-invalid" "A provided but unvalidated fixture is always infrastructure-invalid."

    $junitPath = Join-Path $testRoot "self-test.xml"
    Write-YanamiJUnit -Result $regression -Path $junitPath
    [xml]$junit = Get-Content -LiteralPath $junitPath -Raw
    Assert-True ($null -ne $junit.testsuite) "JUnit output must be well-formed XML."

    $traceOrders = @(
        @("main_entered", "qt_app_ready", "logger_ready", "backend_services_ready", "view_models_ready", "qml_root_ready", "event_loop_ready", "window_exposed", "first_shell_present", "startup_settled"),
        @("main_entered", "qt_app_ready", "logger_ready", "backend_services_ready", "view_models_ready", "qml_root_ready", "window_exposed", "first_shell_present", "event_loop_ready", "startup_settled")
    )
    for ($traceIndex = 0; $traceIndex -lt $traceOrders.Count; $traceIndex++) {
        $tracePath = Join-Path $testRoot "legal-trace-$traceIndex.jsonl"
        $traceLines = New-Object System.Collections.Generic.List[string]
        $timestamp = [long]1000
        foreach ($milestone in $traceOrders[$traceIndex]) {
            $traceEvent = [ordered]@{ schemaVersion = "1.0"; runId = "trace-$traceIndex"; suite = "startup"; scenarioId = "desktop.runtime"; milestone = $milestone; monotonicNs = $timestamp; generation = 0; processId = 1234; attributes = [pscustomobject]@{} }
            $traceLines.Add(($traceEvent | ConvertTo-Json -Compress -Depth 10))
            $timestamp += 1000
        }
        $traceLines.ToArray() | Set-Content -LiteralPath $tracePath -Encoding UTF8
        $traceManifest = Convert-YanamiTraceToManifest -TracePath $tracePath -Profile PullRequest -RunId "trace-$traceIndex"
        Assert-Equal $traceManifest.invariants[0].passed $true "Both legal event-loop/presentation interleavings must satisfy the startup partial order."
    }
    $pidMismatchManifest = Convert-YanamiTraceToManifest -TracePath $tracePath -Profile PullRequest -RunId "trace-1" -ExpectedProcessId 9999
    Assert-Equal $pidMismatchManifest.invariants[0].passed $false "A trace from a different process must not satisfy the launched runtime invariant."
    $runMismatchManifest = Convert-YanamiTraceToManifest -TracePath $tracePath -Profile PullRequest -RunId "trace-stale"
    Assert-Equal $runMismatchManifest.invariants[0].passed $false "A stale trace runId must not be relabeled as the current runtime run."
    $enforceTraceManifest = Convert-YanamiTraceToManifest -TracePath $tracePath -Profile PullRequest -RunId "trace-1" -Mode enforce
    $enforceTraceManifest | Add-Member -NotePropertyName candidateSha -NotePropertyValue "head-test"
    $enforceComponentManifest = New-TestManifest
    $enforceComponentManifest.mode = "enforce"
    $enforceMerge = Merge-YanamiRunManifests -Manifests @($enforceComponentManifest, $enforceTraceManifest)
    Assert-Equal $enforceMerge.mode "enforce" "Runtime and component evidence from an enforce run must remain merge-compatible."

    $interactionTracePath = Join-Path $testRoot "interaction-trace.jsonl"
    $interactionTraceLines = New-Object System.Collections.Generic.List[string]
    $timestamp = [long]1000
    foreach ($milestone in $traceOrders[0]) {
        $traceEvent = [ordered]@{ schemaVersion = "1.0"; runId = "trace-interaction"; suite = "startup"; scenarioId = "desktop.runtime"; milestone = $milestone; monotonicNs = $timestamp; generation = 0; processId = 1234; attributes = [pscustomobject]@{} }
        $interactionTraceLines.Add(($traceEvent | ConvertTo-Json -Compress -Depth 10))
        $timestamp += 1000
    }
    foreach ($traceEvent in @(
        [ordered]@{ schemaVersion = "1.0"; runId = "trace-interaction"; suite = "interaction"; scenarioId = "desktop.runtime"; milestone = "interaction_input_received"; monotonicNs = $timestamp; generation = 1; processId = 1234; attributes = [pscustomobject]@{ generation = 1; synthetic = $true } },
        [ordered]@{ schemaVersion = "1.0"; runId = "trace-interaction"; suite = "interaction"; scenarioId = "desktop.runtime"; milestone = "interaction_next_frame"; monotonicNs = ($timestamp + 1000); generation = 1; processId = 1234; attributes = [pscustomobject]@{ generation = 1; synthetic = $true; latencyMs = 1.0 } },
        [ordered]@{ schemaVersion = "1.0"; runId = "trace-interaction"; suite = "interaction"; scenarioId = "desktop.runtime"; milestone = "interaction_probe_complete"; monotonicNs = ($timestamp + 2000); generation = 0; processId = 1234; attributes = [pscustomobject]@{ syntheticSubmitted = 1; syntheticPresented = 1; syntheticDropped = 0; syntheticPending = 0; dispatchCount = 5; longTaskOver50Count = 0; maxDispatchMs = 2.5; qpaPlatform = "offscreen"; quickBackendEnvironment = "software"; rendererApi = 1; windowExposed = $true; inputModalityBefore = 0; inputModalityAfter = 0; inputModalityChanges = 0 } }
    )) {
        $interactionTraceLines.Add(($traceEvent | ConvertTo-Json -Compress -Depth 10))
    }
    $interactionTraceLines.ToArray() | Set-Content -LiteralPath $interactionTracePath -Encoding UTF8
    $interactionTraceManifest = Convert-YanamiTraceToManifest -TracePath $interactionTracePath -Profile PullRequest -RunId "trace-interaction"
    Assert-True ("interaction" -in @($interactionTraceManifest.suites)) "A completed runtime interaction probe must advertise the interaction suite."
    Assert-Equal @($interactionTraceManifest.invariants | Where-Object id -eq "interaction.measurement_hooks_valid")[0].passed $true "The runtime probe must pair one synthetic input with exactly one later frame."
    Assert-Equal @($interactionTraceManifest.metrics | Where-Object id -eq "interaction.long_task_over_50_count")[0].samples[0] 0 "The bounded PR dispatch window must emit its canonical long-task count."
    $strictInteractionTraceManifest = Convert-YanamiTraceToManifest -TracePath $interactionTracePath -Profile Lab -RunId "trace-interaction"
    Assert-Equal @($strictInteractionTraceManifest.metrics | Where-Object id -eq "interaction.long_task_over_50_count").Count 0 "A bounded smoke must not impersonate a strict-lab interaction workload."
    $mismatchedInteractionTraceManifest = Convert-YanamiTraceToManifest -TracePath $interactionTracePath -Profile PullRequest -RunId "trace-interaction" -ExpectedProcessId 9999
    Assert-Equal @($mismatchedInteractionTraceManifest.invariants | Where-Object id -eq "interaction.measurement_hooks_valid")[0].passed $false "Interaction hook evidence must share the launched process identity."
    Assert-Equal @($mismatchedInteractionTraceManifest.metrics | Where-Object id -eq "interaction.long_task_over_50_count").Count 0 "Invalid interaction provenance must not emit a canonical metric."

    foreach ($backendCase in @(
        [pscustomobject]@{ name = "unknown"; qpa = "offscreen"; backend = "software"; renderer = 0 },
        [pscustomobject]@{ name = "null"; qpa = "offscreen"; backend = "software"; renderer = 7 },
        [pscustomobject]@{ name = "out-of-range"; qpa = "offscreen"; backend = "software"; renderer = 999 },
        [pscustomobject]@{ name = "non-hosted-tuple"; qpa = "windows"; backend = ""; renderer = 3 }
    )) {
        $backendTracePath = Join-Path $testRoot "interaction-backend-$($backendCase.name).jsonl"
        $backendTraceLines = @(
            foreach ($line in Get-Content -LiteralPath $interactionTracePath -Encoding UTF8) {
                $backendEvent = $line | ConvertFrom-Json
                if ([string]$backendEvent.milestone -eq "interaction_probe_complete") {
                    $backendEvent.attributes.qpaPlatform = $backendCase.qpa
                    $backendEvent.attributes.quickBackendEnvironment = $backendCase.backend
                    $backendEvent.attributes.rendererApi = $backendCase.renderer
                }
                $backendEvent | ConvertTo-Json -Compress -Depth 10
            }
        )
        $backendTraceLines | Set-Content -LiteralPath $backendTracePath -Encoding UTF8
        $backendManifest = Convert-YanamiTraceToManifest -TracePath $backendTracePath -Profile PullRequest -RunId "trace-interaction"
        Assert-Equal @($backendManifest.invariants | Where-Object id -eq "interaction.measurement_hooks_valid")[0].passed $false "PR interaction evidence must reject backend case '$($backendCase.name)'."
        Assert-Equal @($backendManifest.metrics | Where-Object id -eq "interaction.long_task_over_50_count").Count 0 "An invalid PR renderer tuple must not emit the canonical long-task metric."
    }
    $nativeLabManifest = Convert-YanamiTraceToManifest -TracePath (Join-Path $testRoot "interaction-backend-non-hosted-tuple.jsonl") -Profile Lab -RunId "trace-interaction"
    Assert-Equal @($nativeLabManifest.invariants | Where-Object id -eq "interaction.measurement_hooks_valid")[0].passed $true "A reviewed native renderer remains valid for the strict lab; only PR is pinned to software/offscreen."

    $invalidTracePath = Join-Path $testRoot "invalid-trace.jsonl"
    $invalidTraceLines = New-Object System.Collections.Generic.List[string]
    $timestamp = [long]1000
    foreach ($milestone in $traceOrders[0]) {
        $traceEvent = [ordered]@{ schemaVersion = "1.0"; runId = "trace-invalid"; suite = "startup"; scenarioId = "desktop.runtime"; milestone = $milestone; monotonicNs = $timestamp; generation = 0; processId = 1234; attributes = [pscustomobject]@{} }
        $invalidTraceLines.Add(($traceEvent | ConvertTo-Json -Compress -Depth 10))
        $timestamp += 1000
    }
    $duplicateMilestone = [ordered]@{ schemaVersion = "1.0"; runId = "trace-other"; suite = "interaction"; scenarioId = "wrong.scenario"; milestone = "window_exposed"; monotonicNs = $timestamp; generation = 1; processId = 5678; attributes = [pscustomobject]@{} }
    $invalidTraceLines.Add(($duplicateMilestone | ConvertTo-Json -Compress -Depth 10))
    $invalidTraceLines.ToArray() | Set-Content -LiteralPath $invalidTracePath -Encoding UTF8
    $invalidTraceManifest = Convert-YanamiTraceToManifest -TracePath $invalidTracePath -Profile PullRequest -RunId "trace-invalid"
    Assert-Equal $invalidTraceManifest.invariants[0].passed $false "Mixed provenance and duplicate required milestones must not satisfy the startup invariant."
    Assert-Equal @($invalidTraceManifest.invariants[0].details.duplicate).Count 1 "Duplicate startup milestones must be reported explicitly."
    Assert-True (@($invalidTraceManifest.invariants[0].details.metadataErrors).Count -gt 0) "Invalid startup metadata must be reported explicitly."

    $runnerOutput = Join-Path $testRoot "runner"
    $runnerResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $runnerOutput -Suites search -ValidateOnly -NoExit
    Assert-Equal $runnerResult.status "pass" "Runner contract validation must succeed without a product probe."
    Assert-True (Test-Path -LiteralPath (Join-Path $runnerOutput "performance-result.json")) "Runner must always write JSON output."
    Assert-True (Test-Path -LiteralPath (Join-Path $runnerOutput "perf-results.xml")) "Runner must always write JUnit output."
    $invalidSuiteOutput = Join-Path $testRoot "runner-invalid-suite"
    $invalidSuiteResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $invalidSuiteOutput -Suites danmakuu -ValidateOnly -NoExit
    Assert-Equal $invalidSuiteResult.status "infra-invalid" "The runner must fail closed on an unknown requested suite."
    Assert-True (@($invalidSuiteResult.reasons | Where-Object { $_ -match "Unknown performance suite 'danmakuu'" }).Count -eq 1) "Unknown suite rejection must preserve the runner's original diagnostic."
    Assert-Equal @($invalidSuiteResult.reasons | Where-Object { $_ -match "Final PerfResult contract validation failed|enum at '/suites/0'" }).Count 0 "An invalid requested suite must not cause a second schema failure that masks the root cause."
    $releaseValidationOutput = Join-Path $testRoot "release-validation"
    $releaseValidation = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile Release -OutputDirectory $releaseValidationOutput -Suites playback -ValidateOnly -NoExit
    Assert-Equal $releaseValidation.mode "enforce" "Release must override the calibration-wide collect mode and can never greenlight P0 debt."
    $missingProbeOutput = Join-Path $testRoot "missing-explicit-probe"
    $missingProbePath = Join-Path $testRoot "does-not-exist\yanami-desktop-perf-probe.exe"
    $missingProbeResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $missingProbeOutput -Suites interaction -ProbePath $missingProbePath -NoExit
    Assert-Equal $missingProbeResult.status "infra-invalid" "A missing explicit ProbePath must fail closed rather than falling back to discovery."
    Assert-True (@($missingProbeResult.reasons | Where-Object { $_ -match "Explicit desktop performance probe does not exist" }).Count -eq 1) "The explicit ProbePath failure must retain its discovery provenance in the reason."

    $testSloPath = Join-Path $testRoot "test-slo.json"
    $testPolicyPath = Join-Path $testRoot "test-policy.json"
    $primaryPath = Join-Path $testRoot "primary-run.json"
    $retryPath = Join-Path $testRoot "retry-run.json"
    $slo | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $testSloPath -Encoding UTF8
    $policy | Add-Member -NotePropertyName missingMeasurementStatusByMode -NotePropertyValue ([pscustomobject]@{ collect = "debt"; debt = "infra-invalid"; enforce = "infra-invalid" }) -Force
    $policy | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $testPolicyPath -Encoding UTF8
    New-TestManifest -Latency 20 | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $primaryPath -Encoding UTF8
    New-TestManifest -Latency 5 | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $retryPath -Encoding UTF8
    $missingCandidatePath = Join-Path $testRoot "missing-candidate-run.json"
    $missingCandidateManifest = New-TestManifest
    $missingCandidateManifest.PSObject.Properties.Remove("candidateSha")
    $missingCandidateManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $missingCandidatePath -Encoding UTF8
    $missingCandidateOutput = Join-Path $testRoot "missing-candidate"
    $missingCandidateResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $missingCandidateOutput -Suites search -InputPath $missingCandidatePath -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $missingCandidateResult.status "infra-invalid" "Imported evidence without candidateSha must not be relabeled by the evaluator."
    Assert-True (@($missingCandidateResult.reasons | Where-Object { $_ -match "must declare a non-empty candidateSha" }).Count -eq 1) "Missing imported candidate attribution must be diagnosed explicitly."
    $mismatchedCandidatePath = Join-Path $testRoot "mismatched-candidate-run.json"
    New-TestManifest -CandidateSha "stale-candidate" | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $mismatchedCandidatePath -Encoding UTF8
    $mismatchedCandidateOutput = Join-Path $testRoot "mismatched-candidate"
    $mismatchedCandidateResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $mismatchedCandidateOutput -Suites search -InputPath $mismatchedCandidatePath -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $mismatchedCandidateResult.status "infra-invalid" "Imported evidence from another candidate must not be accepted under the requested SHA."
    foreach ($retryMode in @("collect", "debt", "enforce")) {
        $retryOutput = Join-Path $testRoot "retry-$retryMode"
        $retryGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $retryOutput -Suites search -InputPath $primaryPath -RetryInputPath $retryPath -BaseSha "base-test" -Mode $retryMode -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
        $expectedRetryStatus = if ($retryMode -eq "collect") { "debt" } else { "infra-invalid" }
        Assert-Equal $retryGate.status $expectedRetryStatus "Retry must not erase missing base evidence in $retryMode mode."
        Assert-Equal @($retryGate.attempts).Count 2 "The continuous failure and retry must both be recorded."
        Assert-Equal $retryGate.relativeComparison.state "not-evaluated" "No relative comparison may be claimed without BaseResultPath."
    }

    $absoluteOnlyOutput = Join-Path $testRoot "absolute-only-debt-mode"
    $absoluteOnlyGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $absoluteOnlyOutput -Suites search -InputPath $retryPath -CandidateSha "head-test" -Mode debt -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $absoluteOnlyGate.status "pass" "A hosted absolute-only gate must remain valid after calibration when no relative comparison was requested."
    Assert-Equal $absoluteOnlyGate.relativeComparison.state "not-evaluated" "Absolute-only evaluation must be explicit without becoming infrastructure-invalid."

    $comparisonBasePath = Join-Path $testRoot "comparison-base-result.json"
    $comparisonHeadPath = Join-Path $testRoot "comparison-head-run.json"
    $comparisonResultPaths = New-Object System.Collections.Generic.List[string]
    $comparisonRoles = @(
        [pscustomobject]@{ name = "A1"; latency = 5; sha = "base-test" },
        [pscustomobject]@{ name = "B1"; latency = 6; sha = "head-test" },
        [pscustomobject]@{ name = "A2"; latency = 5; sha = "base-test" },
        [pscustomobject]@{ name = "B2"; latency = 6; sha = "head-test" }
    )
    $sequenceStart = [DateTimeOffset]::Parse("2026-08-23T00:00:00Z")
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonRoles.Count; $comparisonIndex++) {
        $role = $comparisonRoles[$comparisonIndex]
        $sourceResult = Invoke-YanamiPerfEvaluation -Manifest (New-TestManifest -Latency $role.latency) -Slo $slo -Policy $policy -Mode collect -Suites search -CandidateSha $role.sha
        $sourceResult.runId = "comparison-$($role.name)"
        $sourceResult.generatedAtUtc = $sequenceStart.AddMinutes($comparisonIndex).ToString("o")
        $sourcePath = Join-Path $testRoot "comparison-$($role.name)-result.json"
        $sourceResult | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $sourcePath -Encoding UTF8
        $comparisonResultPaths.Add($sourcePath)
    }
    Copy-Item -LiteralPath $comparisonResultPaths[0] -Destination $comparisonBasePath
    New-TestManifest -Latency 6 | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $comparisonHeadPath -Encoding UTF8

    $unattestedOutput = Join-Path $testRoot "comparison-unattested"
    $unattestedGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $unattestedOutput -Suites search -InputPath $comparisonHeadPath -BaseResultPath $comparisonBasePath -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $unattestedGate.status "debt" "An ordinary base file cannot make a relative decision."
    Assert-Equal $unattestedGate.relativeComparison.state "not-evaluated" "An ordinary base file must remain not-evaluated."
    Assert-True ($null -eq @($unattestedGate.metrics | Where-Object id -eq "test.latency")[0].comparison) "The evaluator must not receive a base result without four source runs."

    $attestedOutput = Join-Path $testRoot "comparison-attested"
    $attestedGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $attestedOutput -Suites search -ComparisonResultPath $comparisonResultPaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $attestedGate.relativeComparison.state "evaluated" "Four attributed, ordered A-B-A-B results may be evaluated."
    Assert-Equal $attestedGate.relativeComparison.sourceRunIds.Count 4 "The comparison decision must retain all four source run IDs."
    Assert-Equal $attestedGate.relativeComparison.comparedMetrics 1 "At least one real metric comparison must back an evaluated state."
    Assert-Equal $attestedGate.status "debt" "Collect mode records the measured 20% and 1ms regression as debt."
    Assert-Equal @($attestedGate.metrics | Where-Object { $_.id -eq "test.latency" -and $null -ne $_.comparison -and $_.comparison.failed }).Count 1 "The A-B-A-B relative regression must be present in metric evidence."

    $attestedEnforceOutput = Join-Path $testRoot "comparison-attested-enforce"
    $attestedEnforceGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $attestedEnforceOutput -Suites search -ComparisonResultPath $comparisonResultPaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode enforce -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $attestedEnforceGate.status "fail" "The same complete relative regression must block when enforcement is active."
    Assert-Equal $attestedEnforceGate.relativeComparison.state "evaluated" "An enforced failure must retain its complete A-B-A-B evidence."

    $wrongBaseOutput = Join-Path $testRoot "comparison-wrong-base"
    $wrongBaseGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $wrongBaseOutput -Suites search -ComparisonResultPath $comparisonResultPaths.ToArray() -BaseSha "wrong-base" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $wrongBaseGate.status "infra-invalid" "Both A source candidate SHAs must match the requested merge-base SHA."
    Assert-Equal $wrongBaseGate.relativeComparison.state "not-evaluated" "Invalid base attribution must never be described as an evaluated comparison."

    $wrongHeadPaths = New-Object System.Collections.Generic.List[string]
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonResultPaths.Count; $comparisonIndex++) {
        $headSource = Get-Content -LiteralPath $comparisonResultPaths[$comparisonIndex] -Raw | ConvertFrom-Json
        if ($comparisonIndex -eq 1) { $headSource.candidateSha = "wrong-head" }
        $headPath = Join-Path $testRoot "comparison-head-sha-$comparisonIndex.json"
        $headSource | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $headPath -Encoding UTF8
        $wrongHeadPaths.Add($headPath)
    }
    $wrongHeadOutput = Join-Path $testRoot "comparison-wrong-head"
    $wrongHeadGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $wrongHeadOutput -Suites search -ComparisonResultPath $wrongHeadPaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $wrongHeadGate.status "infra-invalid" "Both B source candidate SHAs must match the requested head SHA."
    Assert-Equal $wrongHeadGate.relativeComparison.state "not-evaluated" "A mismatched head SHA must remain not-evaluated."

    $missingFingerprintPaths = New-Object System.Collections.Generic.List[string]
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonResultPaths.Count; $comparisonIndex++) {
        $fingerprintSource = Get-Content -LiteralPath $comparisonResultPaths[$comparisonIndex] -Raw | ConvertFrom-Json
        if ($comparisonIndex -eq 0) { $fingerprintSource.environment.PSObject.Properties.Remove("fingerprint") }
        $fingerprintPath = Join-Path $testRoot "comparison-fingerprint-$comparisonIndex.json"
        $fingerprintSource | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $fingerprintPath -Encoding UTF8
        $missingFingerprintPaths.Add($fingerprintPath)
    }
    $missingFingerprintOutput = Join-Path $testRoot "comparison-missing-fingerprint"
    $missingFingerprintGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $missingFingerprintOutput -Suites search -ComparisonResultPath $missingFingerprintPaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $missingFingerprintGate.status "infra-invalid" "Every A-B-A-B source requires a non-empty machine fingerprint."
    Assert-Equal $missingFingerprintGate.relativeComparison.state "not-evaluated" "Missing machine identity must remain not-evaluated."

    $badTimePaths = New-Object System.Collections.Generic.List[string]
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonResultPaths.Count; $comparisonIndex++) {
        $timeSource = Get-Content -LiteralPath $comparisonResultPaths[$comparisonIndex] -Raw | ConvertFrom-Json
        if ($comparisonIndex -eq 2) { $timeSource.generatedAtUtc = $sequenceStart.AddSeconds(30).ToString("o") }
        $timePath = Join-Path $testRoot "comparison-time-$comparisonIndex.json"
        $timeSource | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $timePath -Encoding UTF8
        $badTimePaths.Add($timePath)
    }
    $badTimeOutput = Join-Path $testRoot "comparison-bad-time"
    $badTimeGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $badTimeOutput -Suites search -ComparisonResultPath $badTimePaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $badTimeGate.status "infra-invalid" "A-B-A-B source completion times must be strictly increasing."
    Assert-Equal $badTimeGate.relativeComparison.state "not-evaluated" "Out-of-order completion evidence must remain not-evaluated."

    $reorderedOutput = Join-Path $testRoot "comparison-reordered"
    $reorderedPaths = @($comparisonResultPaths[0], $comparisonResultPaths[2], $comparisonResultPaths[1], $comparisonResultPaths[3])
    $reorderedGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $reorderedOutput -Suites search -ComparisonResultPath $reorderedPaths -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $reorderedGate.status "infra-invalid" "A-A-B-B inputs cannot masquerade as A-B-A-B evidence."
    Assert-Equal $reorderedGate.relativeComparison.state "not-evaluated" "A reordered sequence must remain not-evaluated."

    $zeroComparisonPaths = New-Object System.Collections.Generic.List[string]
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonResultPaths.Count; $comparisonIndex++) {
        $zeroSource = Get-Content -LiteralPath $comparisonResultPaths[$comparisonIndex] -Raw | ConvertFrom-Json
        $zeroSource.metrics = @($zeroSource.metrics | Where-Object id -ne "test.latency")
        $zeroPath = Join-Path $testRoot "comparison-zero-$comparisonIndex.json"
        $zeroSource | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $zeroPath -Encoding UTF8
        $zeroComparisonPaths.Add($zeroPath)
    }
    $zeroOutput = Join-Path $testRoot "comparison-zero"
    $zeroGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $zeroOutput -Suites search -ComparisonResultPath $zeroComparisonPaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $zeroGate.status "infra-invalid" "A supplied A-B-A-B evidence set must be complete in every round, even during collection."
    Assert-Equal $zeroGate.relativeComparison.state "not-evaluated" "Zero metric comparisons can never produce an evaluated relative gate."
    Assert-Equal $zeroGate.relativeComparison.comparedMetrics 0 "Zero comparison evidence must be explicit."

    $asymmetricCoveragePaths = New-Object System.Collections.Generic.List[string]
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonResultPaths.Count; $comparisonIndex++) {
        $asymmetricSource = Get-Content -LiteralPath $comparisonResultPaths[$comparisonIndex] -Raw | ConvertFrom-Json
        if ($comparisonIndex -in @(0, 3)) { $asymmetricSource.metrics = @($asymmetricSource.metrics | Where-Object id -ne "test.latency") }
        $asymmetricPath = Join-Path $testRoot "comparison-asymmetric-$comparisonIndex.json"
        $asymmetricSource | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $asymmetricPath -Encoding UTF8
        $asymmetricCoveragePaths.Add($asymmetricPath)
    }
    $asymmetricOutput = Join-Path $testRoot "comparison-asymmetric-coverage"
    $asymmetricGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $asymmetricOutput -Suites search -ComparisonResultPath $asymmetricCoveragePaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode enforce -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $asymmetricGate.status "infra-invalid" "A1/B2 gaps cannot be hidden by A2/B1 during aggregate merge."
    Assert-Equal $asymmetricGate.relativeComparison.state "not-evaluated" "Per-round missing evidence must prevent aggregate comparison."
    Assert-Equal $asymmetricGate.relativeComparison.comparedMetrics 0 "No aggregate metric may be compared after a per-round evidence failure."

    $missingBaseMetricPaths = New-Object System.Collections.Generic.List[string]
    for ($comparisonIndex = 0; $comparisonIndex -lt $comparisonResultPaths.Count; $comparisonIndex++) {
        $coverageSource = Get-Content -LiteralPath $comparisonResultPaths[$comparisonIndex] -Raw | ConvertFrom-Json
        if ($comparisonIndex -in @(0, 2)) { $coverageSource.metrics = @($coverageSource.metrics | Where-Object id -ne "test.latency") }
        $coveragePath = Join-Path $testRoot "comparison-coverage-$comparisonIndex.json"
        $coverageSource | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $coveragePath -Encoding UTF8
        $missingBaseMetricPaths.Add($coveragePath)
    }
    $coverageOutput = Join-Path $testRoot "comparison-missing-base-metric"
    $coverageGate = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $coverageOutput -Suites search -ComparisonResultPath $missingBaseMetricPaths.ToArray() -BaseSha "base-test" -CandidateSha "head-test" -Mode collect -SloPath $testSloPath -PolicyPath $testPolicyPath -NoExit
    Assert-Equal $coverageGate.status "infra-invalid" "Every candidate relative metric requires matching base statistics."
    Assert-Equal $coverageGate.relativeComparison.state "not-evaluated" "Missing base metric coverage must remain not-evaluated."
    Assert-True ($null -eq @($coverageGate.metrics | Where-Object id -eq "test.latency")[0].comparison) "Missing base statistics must not produce a partial comparison."

    $malformedPath = Join-Path $testRoot "malformed-run.json"
    '{"schemaVersion":"0"}' | Set-Content -LiteralPath $malformedPath -Encoding UTF8
    $malformedOutput = Join-Path $testRoot "malformed"
    $malformedResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $malformedOutput -Suites startup -InputPath $malformedPath -SkipProbeDiscovery -NoExit
    Assert-Equal $malformedResult.status "infra-invalid" "A raw manifest contract mismatch must be infrastructure-invalid."
    Assert-True (Test-Path -LiteralPath (Join-Path $malformedOutput "performance-result.json")) "Malformed input must still produce a JSON result."
    Assert-True (Test-Path -LiteralPath (Join-Path $malformedOutput "perf-results.xml")) "Malformed input must still produce JUnit."

    $unprovisionedPlaybackPath = Join-Path $testRoot "unprovisioned-playback-run.json"
    $unprovisionedPlayback = New-TestManifest
    $unprovisionedPlayback.suites = @("playback")
    $unprovisionedPlayback.metrics = @()
    $unprovisionedPlayback.invariants = @()
    $unprovisionedPlayback.fixtures = @([pscustomobject]@{
        id = "PlaybackMedia-v1"; version = "1"; sha256 = ("0" * 64); validated = $true
        details = [pscustomobject]@{ assets = @([pscustomobject]@{ id = "h264-4k60"; sha256 = ("0" * 64) }) }
    })
    $unprovisionedPlayback | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $unprovisionedPlaybackPath -Encoding UTF8
    $unprovisionedPlaybackOutput = Join-Path $testRoot "unprovisioned-playback"
    $unprovisionedPlaybackResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile PullRequest -OutputDirectory $unprovisionedPlaybackOutput -Suites playback -InputPath $unprovisionedPlaybackPath -SkipProbeDiscovery -NoExit
    Assert-Equal $unprovisionedPlaybackResult.status "infra-invalid" "A probe cannot self-certify playback media before the policy pins every asset hash."
    Assert-True (@($unprovisionedPlaybackResult.reasons | Where-Object { $_ -match "fixed-media policy is 'not-provisioned'" }).Count -eq 1) "Playback rejection must identify the unprovisioned fixed-media policy."

    $releaseMissingPlaybackPath = Join-Path $testRoot "release-missing-playback-run.json"
    $releaseMissingPlayback = New-TestManifest -Profile Release -ReferenceMatch $true
    $releaseMissingPlayback.suites = @("playback")
    $releaseMissingPlayback.metrics = @()
    $releaseMissingPlayback.invariants = @()
    $releaseMissingPlayback | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $releaseMissingPlaybackPath -Encoding UTF8
    $releaseMissingPlaybackOutput = Join-Path $testRoot "release-missing-playback"
    $releaseMissingPlaybackResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile Release -OutputDirectory $releaseMissingPlaybackOutput -Suites playback -InputPath $releaseMissingPlaybackPath -SkipProbeDiscovery -NoExit
    Assert-Equal $releaseMissingPlaybackResult.mode "enforce" "Release evidence evaluation must use enforce mode by default."
    Assert-Equal $releaseMissingPlaybackResult.status "infra-invalid" "A Release cannot pass when the required playback fixture and P0 evidence are absent."

    $defaultFullPath = Join-Path $testRoot "default-full-suite-run.json"
    $defaultFullManifest = New-TestManifest -Profile Release -ReferenceMatch $true
    $defaultFullManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $defaultFullPath -Encoding UTF8
    $defaultFullOutput = Join-Path $testRoot "default-full-suite"
    $defaultFullResult = & (Join-Path $PSScriptRoot "run-gate.ps1") -Profile Release -OutputDirectory $defaultFullOutput -InputPath $defaultFullPath -SkipProbeDiscovery -NoExit
    Assert-Equal @($defaultFullResult.suites).Count 6 "Omitting -Suites must evaluate the complete six-suite contract, not only manifest.suites."
    Assert-True ("playback" -in @($defaultFullResult.suites)) "The default complete evaluation must include playback even when the imported manifest omits that suite."
    Assert-True ("danmaku" -in @($defaultFullResult.suites)) "The default complete evaluation must include danmaku even when the imported manifest omits that suite."
    Assert-True (@($defaultFullResult.reasons | Where-Object { $_ -match "PlaybackMedia-v1" }).Count -gt 0) "Default full-suite evaluation must report the missing playback fixture."
    Assert-True (@($defaultFullResult.reasons | Where-Object { $_ -match "DanmakuDensity-v1" }).Count -gt 0) "Default full-suite evaluation must report the missing danmaku fixture."
    Assert-True (@($defaultFullResult.reasons | Where-Object { $_ -match "playback\.telemetry_complete" }).Count -gt 0) "Default full-suite evaluation must report missing playback invariants."

    $fixtureA = Join-Path $testRoot "fixture-a"
    $fixtureB = Join-Path $testRoot "fixture-b"
    & (Join-Path $PSScriptRoot "New-F110KFixture.ps1") -OutputDirectory $fixtureA -TitleCount 25 -EpisodeCount 100 -QueryCount 48 | Out-Null
    & (Join-Path $PSScriptRoot "New-F110KFixture.ps1") -OutputDirectory $fixtureB -TitleCount 25 -EpisodeCount 100 -QueryCount 48 | Out-Null
    Assert-Equal (Get-FileHash (Join-Path $fixtureA "f110k-items.jsonl") -Algorithm SHA256).Hash (Get-FileHash (Join-Path $fixtureB "f110k-items.jsonl") -Algorithm SHA256).Hash "Fixture generation must be deterministic."
    Assert-Equal (Get-FileHash (Join-Path $fixtureA "search-queries-v1.jsonl") -Algorithm SHA256).Hash (Get-FileHash (Join-Path $fixtureB "search-queries-v1.jsonl") -Algorithm SHA256).Hash "Query corpus generation must be deterministic."
    $fixtureItems = @(Get-Content -LiteralPath (Join-Path $fixtureA "f110k-items.jsonl") | ForEach-Object { $_ | ConvertFrom-Json })
    $titleKinds = @{}
    foreach ($item in @($fixtureItems | Where-Object { $_.kind -ne "Episode" })) { $titleKinds[[string]$item.id] = [string]$item.kind }
    $invalidEpisodeParents = @($fixtureItems | Where-Object { $_.kind -eq "Episode" -and $titleKinds[[string]$_.seriesId] -ne "Series" })
    Assert-Equal $invalidEpisodeParents.Count 0 "Every generated Episode must belong to a Series rather than a Movie."
    $episodeKeys = @{}
    $duplicateEpisodeKeys = 0
    foreach ($episodeItem in @($fixtureItems | Where-Object { $_.kind -eq "Episode" })) {
        $episodeKey = "$($episodeItem.seriesId):$($episodeItem.season):$($episodeItem.episode)"
        if ($episodeKeys.ContainsKey($episodeKey)) { $duplicateEpisodeKeys++ }
        else { $episodeKeys[$episodeKey] = [string]$episodeItem.id }
    }
    Assert-Equal $duplicateEpisodeKeys 0 "Every generated series/season/episode oracle key must identify exactly one Episode."
    $fixtureQueries = @(Get-Content -LiteralPath (Join-Path $fixtureA "search-queries-v1.jsonl") | ForEach-Object { $_ | ConvertFrom-Json })
    $seasonQuery = @($fixtureQueries | Where-Object { $_.category -eq "season" } | Select-Object -First 1)
    $episodeNumberQuery = @($fixtureQueries | Where-Object { $_.category -eq "episode-number" } | Select-Object -First 1)
    $fullWidthQuery = @($fixtureQueries | Where-Object { $_.category -eq "full-width" } | Select-Object -First 1)
    $unicodeQuery = @($fixtureQueries | Where-Object { $_.category -eq "unicode-normalized" } | Select-Object -First 1)
    Assert-Equal $seasonQuery.Count 1 "The query corpus must exercise independently searchable Season results."
    Assert-Equal $seasonQuery[0].expectation.kind "Season" "Season queries must carry an independent derived-entity oracle expectation."
    Assert-Equal $episodeNumberQuery[0].expectation.kind "Episode" "The query corpus must exercise season/episode-number lookup."
    $expectedEpisode = @($fixtureItems | Where-Object { [string]$_.id -eq [string]$episodeNumberQuery[0].expectation.rank1 })
    Assert-Equal $expectedEpisode.Count 1 "An episode-number oracle rank-1 target must exist exactly once in the item corpus."
    $expectedEpisodeKey = "$($expectedEpisode[0].seriesId):$($expectedEpisode[0].season):$($expectedEpisode[0].episode)"
    Assert-Equal $episodeKeys[$expectedEpisodeKey] ([string]$expectedEpisode[0].id) "An episode-number oracle key must resolve uniquely to its declared rank-1 target."
    Assert-True ([string]$fullWidthQuery[0].query -match '[\uFF01-\uFF5E]') "The query corpus must contain full-width ASCII normalization cases."
    $unicodeQueryText = [string]$unicodeQuery[0].query
    Assert-True (-not [string]::Equals($unicodeQueryText, $unicodeQueryText.Normalize([Text.NormalizationForm]::FormC), [StringComparison]::Ordinal)) "The query corpus must contain canonically decomposed Unicode input."

    $danmakuFixtureContract = Read-YanamiPerfJson -Path (Join-Path $workspace "perf\fixtures\danmaku-density-v1.manifest.json")
    $danmakuFixtureA = Join-Path $testRoot "danmaku-fixture-a"
    $danmakuFixtureB = Join-Path $testRoot "danmaku-fixture-b"
    foreach ($fixtureDirectory in @($danmakuFixtureA, $danmakuFixtureB)) {
        & (Join-Path $PSScriptRoot "New-DanmakuFixture.ps1") `
            -OutputDirectory $fixtureDirectory `
            -CommentCount ([int]$danmakuFixtureContract.counts.strictTotal) `
            -DurationSeconds ([int]$danmakuFixtureContract.durationSeconds) `
            -Seed ([int]$danmakuFixtureContract.seed) | Out-Null
    }
    $danmakuCommentsA = Join-Path $danmakuFixtureA "danmaku-comments-v1.jsonl"
    $danmakuCommentsB = Join-Path $danmakuFixtureB "danmaku-comments-v1.jsonl"
    $danmakuHashA = (Get-FileHash -LiteralPath $danmakuCommentsA -Algorithm SHA256).Hash.ToLowerInvariant()
    $danmakuHashB = (Get-FileHash -LiteralPath $danmakuCommentsB -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Equal $danmakuHashA $danmakuHashB "Danmaku fixture generation must be deterministic for the same version, seed, count, and duration."
    Assert-Equal $danmakuHashA ([string]$danmakuFixtureContract.fixtureSha256).ToLowerInvariant() "The generated 100K danmaku corpus must match the policy-pinned fixture hash."
    $danmakuInfo = Read-YanamiPerfJson -Path (Join-Path $danmakuFixtureA "fixture-info.json")
    Assert-Equal $danmakuInfo.commentCount ([int]$danmakuFixtureContract.counts.strictTotal) "Danmaku fixture metadata must report the complete strict corpus size."
    Assert-Equal $danmakuInfo.hostedSliceCount ([int]$danmakuFixtureContract.counts.hostedSlice) "Danmaku fixture metadata must identify the deterministic hosted slice."
    Assert-Equal ([int]$danmakuInfo.modeCounts.scroll + [int]$danmakuInfo.modeCounts.top + [int]$danmakuInfo.modeCounts.bottom) ([int]$danmakuFixtureContract.counts.strictTotal) "Danmaku fixture mode counts must cover every generated record exactly once."

    $strictRenderingContract = (Read-YanamiPerfJson -Path (Join-Path $workspace "perf\environments\windows-reference-v1.json")).strictRendering
    $strictOffscreenManifest = $offscreenDanmakuManifest | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $strictOffscreenManifest.metrics[0].attributes.evidence = "external-pixel-present"
    $strictOffscreenManifest.fixtures = @([pscustomobject]@{
        id = "DanmakuDensity-v1"
        version = "1"
        sha256 = [string]$danmakuFixtureContract.fixtureSha256
        validated = $true
    })
    $strictOffscreenManifest.environment | Add-Member -NotePropertyName rendering -NotePropertyValue ([pscustomobject]@{
        renderWidthPixels = [int]$strictRenderingContract.renderWidthPixels
        renderHeightPixels = [int]$strictRenderingContract.renderHeightPixels
        displayRefreshHz = [int]$strictRenderingContract.displayRefreshHz
        dpiScalePercent = [int]$strictRenderingContract.dpiScalePercent
        hdrEnabled = [bool]$strictRenderingContract.hdrEnabled
        vrrEnabled = [bool]$strictRenderingContract.vrrEnabled
        windowMode = [string]$strictRenderingContract.windowMode
        qtRhiRenderer = "Software"
        fontCacheState = [string]$strictRenderingContract.fontCacheState
        fontCachePreparationId = [string]$strictRenderingContract.fontCachePreparationId
        fontSetId = [string]$strictRenderingContract.fontSet.id
        fontSetSha256 = ("2" * 64)
        fontFamilies = @($strictRenderingContract.fontSet.families)
        qpaPlatform = "offscreen"
    })
    $strictOffscreenPath = Join-Path $testRoot "strict-offscreen-danmaku-run.json"
    $strictOffscreenManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $strictOffscreenPath -Encoding UTF8
    $strictDanmakuSloPath = Join-Path $testRoot "strict-danmaku-slo.json"
    $strictDanmakuPolicyPath = Join-Path $testRoot "strict-danmaku-policy.json"
    $strictDanmakuSlo | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $strictDanmakuSloPath -Encoding UTF8
    $policy | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $strictDanmakuPolicyPath -Encoding UTF8
    $strictOffscreenOutput = Join-Path $testRoot "strict-offscreen-danmaku"
    $strictOffscreenRunnerResult = & (Join-Path $PSScriptRoot "run-gate.ps1") `
        -Profile Lab `
        -OutputDirectory $strictOffscreenOutput `
        -Suites danmaku `
        -InputPath $strictOffscreenPath `
        -DanmakuFixtureDirectory $danmakuFixtureB `
        -SkipProbeDiscovery `
        -CandidateSha "head-test" `
        -SloPath $strictDanmakuSloPath `
        -PolicyPath $strictDanmakuPolicyPath `
        -NoExit
    Assert-Equal $strictOffscreenRunnerResult.status "infra-invalid" "The runner must reject strict danmaku evidence collected with Qt's software renderer."
    Assert-True (@($strictOffscreenRunnerResult.reasons | Where-Object { $_ -match "qtRhiRenderer.*d3d11.*Software" }).Count -eq 1) "Strict renderer rejection must identify the lab-required and supplied renderer backends."

    $validEvidenceDirectory = Join-Path $testRoot "strict-valid-evidence"
    [void](New-Item -ItemType Directory -Path $validEvidenceDirectory)
    $presentmonPath = Join-Path $validEvidenceDirectory "presentmon.csv"
    $capturePath = Join-Path $validEvidenceDirectory "danmaku-capture.bin"
    [System.IO.File]::WriteAllText(
        $presentmonPath,
        "qpc,present`n1,1`n",
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllBytes(
        $capturePath,
        [System.Text.Encoding]::UTF8.GetBytes("deterministic-pixel-capture"))
    $captureSha = (Get-FileHash -LiteralPath $capturePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $pixelIndexPath = Join-Path $validEvidenceDirectory "pixel-capture-index.json"
    [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        runId = [string]$strictOffscreenManifest.runId
        candidateSha = "head-test"
        fixtureSha256 = [string]$danmakuFixtureContract.fixtureSha256
        environmentFingerprint = "test-machine"
        captures = @([pscustomobject][ordered]@{
            fileName = [System.IO.Path]::GetFileName($capturePath)
            sha256 = $captureSha
        })
    } | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $pixelIndexPath -Encoding UTF8

    $validStrictManifest = $strictOffscreenManifest | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $validStrictManifest.environment.rendering.qtRhiRenderer = [string]$strictRenderingContract.qtRhiRenderer
    $collector = [pscustomobject][ordered]@{
        name = "self-test-collector"
        version = "1.0"
        kind = "deterministic-fixture"
        clockDomain = "QPC"
        executableSha256 = ("3" * 64)
    }
    $validStrictManifest | Add-Member -NotePropertyName artifacts -NotePropertyValue @(
        [pscustomobject][ordered]@{
            role = "presentmon-trace"
            fileName = [System.IO.Path]::GetFileName($presentmonPath)
            sha256 = (Get-FileHash -LiteralPath $presentmonPath -Algorithm SHA256).Hash.ToLowerInvariant()
            candidateSha = "head-test"
            collector = $collector
        },
        [pscustomobject][ordered]@{
            role = "pixel-capture-index"
            fileName = [System.IO.Path]::GetFileName($pixelIndexPath)
            sha256 = (Get-FileHash -LiteralPath $pixelIndexPath -Algorithm SHA256).Hash.ToLowerInvariant()
            candidateSha = "head-test"
            collector = $collector
        }
    ) -Force
    $validStrictPath = Join-Path $validEvidenceDirectory "strict-valid-danmaku-run.json"
    $validStrictManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $validStrictPath -Encoding UTF8
    $validStrictOutput = Join-Path $testRoot "strict-valid-danmaku"
    $validStrictResult = & (Join-Path $PSScriptRoot "run-gate.ps1") `
        -Profile Lab `
        -OutputDirectory $validStrictOutput `
        -Suites danmaku `
        -InputPath $validStrictPath `
        -DanmakuFixtureDirectory $danmakuFixtureB `
        -SkipProbeDiscovery `
        -CandidateSha "head-test" `
        -SloPath $strictDanmakuSloPath `
        -PolicyPath $strictDanmakuPolicyPath `
        -NoExit
    Assert-Equal $validStrictResult.status "pass" "A complete hash-validated external pixel/Present evidence bundle must be able to satisfy a strict danmaku gate."
    Assert-Equal @($validStrictResult.artifacts | Where-Object { [bool]$_.runnerValidated }).Count 2 "The strict PerfResult must retain both runner-validated raw artifacts for later A-B-A-B checks."
    Assert-True (Test-Path -LiteralPath (Join-Path $validStrictOutput "presentmon.csv")) "The runner must retain strict raw evidence in the gate output bundle."
    Assert-True (Test-Path -LiteralPath (Join-Path $validStrictOutput "danmaku-capture.bin")) "The runner must retain the pixel capture referenced by the validated index."

    [System.IO.File]::AppendAllText(
        $danmakuCommentsA,
        "`n",
        [System.Text.UTF8Encoding]::new($false))
    Assert-True (((Get-FileHash -LiteralPath $danmakuCommentsA -Algorithm SHA256).Hash.ToLowerInvariant()) -ne [string]$danmakuFixtureContract.fixtureSha256) "A byte-level danmaku fixture mutation must change its content hash."
    $tamperedDanmakuManifest = New-TestManifest
    $tamperedDanmakuManifest.suites = @("danmaku")
    $tamperedDanmakuManifest.metrics = @()
    $tamperedDanmakuManifest.invariants = @()
    $tamperedDanmakuManifest.fixtures = @([pscustomobject]@{
        id = "DanmakuDensity-v1"
        version = "1"
        sha256 = [string]$danmakuFixtureContract.fixtureSha256
        validated = $true
    })
    $tamperedDanmakuPath = Join-Path $testRoot "tampered-danmaku-run.json"
    $tamperedDanmakuManifest | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $tamperedDanmakuPath -Encoding UTF8
    $tamperedDanmakuOutput = Join-Path $testRoot "tampered-danmaku"
    $tamperedDanmakuResult = & (Join-Path $PSScriptRoot "run-gate.ps1") `
        -Profile PullRequest `
        -OutputDirectory $tamperedDanmakuOutput `
        -Suites danmaku `
        -InputPath $tamperedDanmakuPath `
        -DanmakuFixtureDirectory $danmakuFixtureA `
        -SkipProbeDiscovery `
        -CandidateSha "head-test" `
        -NoExit
    Assert-Equal $tamperedDanmakuResult.status "infra-invalid" "A physically modified danmaku corpus must fail closed before its manifest can self-certify the pinned hash."
    Assert-True (@($tamperedDanmakuResult.reasons | Where-Object { $_ -match "DanmakuDensity-v1 fixture hash mismatch" }).Count -eq 1) "A fixture mutation must be diagnosed as a physical danmaku hash mismatch."

    $forgedOffscreenManifest = $offscreenDanmakuManifest | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $forgedOffscreenManifest.metrics[0].attributes.evidence = "external-pixel-present"
    $forgedOffscreenManifest | Add-Member -NotePropertyName artifacts -NotePropertyValue @(
        [pscustomobject]@{ role = "presentmon-trace"; sha256 = ("0" * 64); runnerValidated = $false },
        [pscustomobject]@{ role = "pixel-capture-index"; sha256 = ("1" * 64); runnerValidated = $false }
    )
    $forgedOffscreenResult = Invoke-YanamiPerfEvaluation -Manifest $forgedOffscreenManifest -Slo $strictDanmakuSlo -Policy $policy -Mode collect -Suites danmaku
    Assert-Equal $forgedOffscreenResult.status "infra-invalid" "Relabeling an offscreen/software sample as external pixel-Present evidence must not satisfy a strict danmaku metric."
    Assert-True (@($forgedOffscreenResult.reasons | Where-Object { $_ -match "requires runner-validated.*raw evidence; an evidence label alone is insufficient" }).Count -gt 0) "A forged evidence label and self-declared artifacts must remain invalid until the runner validates their bytes and provenance."

    Write-Host "PASS: Yanami PerfGate self-test completed with $assertions assertions."
}
finally {
    if ($KeepArtifacts) { Write-Host "Self-test artifacts retained at $testRoot" }
    else {
        $resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
        $resolvedTest = [System.IO.Path]::GetFullPath($testRoot)
        if (-not $resolvedTest.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to remove non-temporary self-test path: $resolvedTest" }
        [System.IO.Directory]::Delete($resolvedTest, $true)
    }
}
