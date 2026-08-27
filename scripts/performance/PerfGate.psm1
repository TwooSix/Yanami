Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Read-YanamiPerfJson {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "JSON file does not exist: $Path"
    }
    try {
        return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
    }
    catch {
        throw "Invalid JSON in '$Path': $($_.Exception.Message)"
    }
}

function Get-ObjectPropertyValue {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Get-YanamiPerfSuites {
    return @("search", "backend", "interaction", "playback", "danmaku", "upscaling", "startup")
}

function Get-YanamiRequiredFixtureIds {
    param(
        [Parameter(Mandatory = $true)][object]$Policy,
        [Parameter(Mandatory = $true)][string]$Suite,
        [Parameter(Mandatory = $true)][string]$Profile
    )

    $requiredBySuite = Get-ObjectPropertyValue $Policy "requiredFixturesBySuite"
    $requirement = Get-ObjectPropertyValue $requiredBySuite $Suite
    if ($null -eq $requirement) { return @() }
    if ($requirement -is [System.Management.Automation.PSCustomObject] -or
        $requirement -is [System.Collections.IDictionary]) {
        $profileRequirement = Get-ObjectPropertyValue $requirement $Profile
        if ($null -eq $profileRequirement) {
            $profileRequirement = Get-ObjectPropertyValue $requirement "default"
        }
        $requirement = $profileRequirement
    }
    return @(
        @($requirement) |
            ForEach-Object { [string]$_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -Unique
    )
}

function Get-YanamiEvidenceArtifactRoles {
    param([string]$Evidence)

    switch ($Evidence) {
        "external-present" { return @("presentmon-trace") }
        "external-pixel-present" { return @("presentmon-trace", "pixel-capture-index") }
        "external-pixel-oracle" { return @("presentmon-trace", "pixel-capture-index") }
        "mpv-paired-telemetry" { return @("mpv-telemetry", "paired-baseline-index") }
        "etw-paired-process" { return @("etw-process-counters", "paired-baseline-index") }
        "external-present-mpv-correlated" { return @("presentmon-trace", "mpv-telemetry", "frame-correlation-index") }
        "etw-gpu-process" { return @("etw-gpu-engine-counters") }
        "dxgi-video-memory" { return @("dxgi-video-memory-counters") }
        "yanami-upscaling-runtime-trace" { return @("yanami-upscaling-runtime-trace") }
        "pinned-upscaling-model-pack" { return @("upscaling-model-pack-index") }
        "upscaling-strict-scenario-index" { return @("upscaling-scenario-index") }
        default { return @() }
    }
}

function Test-YanamiProducerProvenance {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ProbeKind,
        [string[]]$ObservationProducerRunIds = @(),
        [object[]]$TrustedProducerAttestations = @()
    )

    $environment = Get-ObjectPropertyValue $Manifest "environment" ([pscustomobject]@{})
    $candidates = New-Object System.Collections.Generic.List[object]
    $directProvenance = Get-ObjectPropertyValue $environment "runnerProvenance"
    if ($null -ne $directProvenance) {
        $candidates.Add([pscustomobject][ordered]@{
            runId = [string](Get-ObjectPropertyValue $Manifest "runId" "")
            fingerprint = [string](Get-ObjectPropertyValue $environment "fingerprint" "")
            provenance = $directProvenance
        })
    }
    foreach ($source in @(Get-ObjectPropertyValue $environment "sourceEnvironments" @())) {
        $sourceProvenance = Get-ObjectPropertyValue $source "runnerProvenance"
        if ($null -eq $sourceProvenance) { continue }
        $candidates.Add([pscustomobject][ordered]@{
            runId = [string](Get-ObjectPropertyValue $source "runId" "")
            fingerprint = [string](Get-ObjectPropertyValue $source "fingerprint" "")
            provenance = $sourceProvenance
        })
    }

    $requiredRunIds = @($ObservationProducerRunIds | ForEach-Object { [string]$_ } | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    } | Select-Object -Unique)
    if ($requiredRunIds.Count -eq 0) { return $false }
    foreach ($requiredRunId in $requiredRunIds) {
        $runTrusted = $false
        foreach ($candidate in @($candidates | Where-Object { [string]$_.runId -eq $requiredRunId })) {
            $provenance = $candidate.provenance
            $fingerprint = [string]$candidate.fingerprint
            if ([string](Get-ObjectPropertyValue $provenance "kind" "") -ne "local-runner-generated") { continue }
            if ([string](Get-ObjectPropertyValue $provenance "producer" "") -ne "scripts/performance/run-gate.ps1") { continue }
            if ([string](Get-ObjectPropertyValue $provenance "probeKind" "") -ne $ProbeKind) { continue }
            if ([string](Get-ObjectPropertyValue $provenance "artifactSha256" "") -notmatch '^[0-9a-fA-F]{64}$') { continue }
            $runnerFingerprint = [string](Get-ObjectPropertyValue $provenance "runnerFingerprint" "")
            if ([string]::IsNullOrWhiteSpace($fingerprint) -or $runnerFingerprint -ne $fingerprint) { continue }
            $artifactSha256 = [string](Get-ObjectPropertyValue $provenance "artifactSha256" "")
            $trustedMatches = @($TrustedProducerAttestations | Where-Object {
                [string](Get-ObjectPropertyValue $_ "trustSource" "") -eq "current-runner-process" -and
                [string](Get-ObjectPropertyValue $_ "runId" "") -eq $requiredRunId -and
                [string](Get-ObjectPropertyValue $_ "probeKind" "") -eq $ProbeKind -and
                [string](Get-ObjectPropertyValue $_ "runnerFingerprint" "") -eq $runnerFingerprint -and
                [string](Get-ObjectPropertyValue $_ "artifactSha256" "") -eq $artifactSha256
            })
            if ($trustedMatches.Count -eq 1) {
                $runTrusted = $true
                break
            }
        }
        if (-not $runTrusted) { return $false }
    }
    return $true
}

function ConvertTo-YanamiCanonicalValue {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Collections.IDictionary]) {
        $ordered = [ordered]@{}
        foreach ($key in @($Value.Keys | ForEach-Object { [string]$_ } | Sort-Object)) {
            $ordered[$key] = ConvertTo-YanamiCanonicalValue $Value[$key]
        }
        return [pscustomobject]$ordered
    }
    if ($Value -is [System.Management.Automation.PSCustomObject]) {
        $ordered = [ordered]@{}
        foreach ($property in @($Value.PSObject.Properties | Sort-Object Name)) {
            $ordered[$property.Name] = ConvertTo-YanamiCanonicalValue $property.Value
        }
        return [pscustomobject]$ordered
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        $items = New-Object System.Collections.Generic.List[object]
        foreach ($item in $Value) { $items.Add((ConvertTo-YanamiCanonicalValue $item)) }
        return ,$items.ToArray()
    }
    return $Value
}

function ConvertTo-YanamiCanonicalJson {
    param([AllowNull()][object]$Value)
    return (ConvertTo-YanamiCanonicalValue $Value | ConvertTo-Json -Compress -Depth 100)
}

function Get-YanamiPercentile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][ValidateRange(0, 1)][double]$Probability
    )

    if ($Values.Count -eq 0) { throw "A percentile requires at least one sample." }
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 1) { return [double]$sorted[0] }
    $position = ($sorted.Count - 1) * $Probability
    $lower = [int][math]::Floor($position)
    $upper = [int][math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$sorted[$lower] }
    $weight = $position - $lower
    return ([double]$sorted[$lower] + (([double]$sorted[$upper] - [double]$sorted[$lower]) * $weight))
}

function Get-YanamiStatistics {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object[]]$Samples)

    if ($Samples.Count -eq 0) { throw "A metric requires at least one sample." }
    $values = New-Object System.Collections.Generic.List[double]
    foreach ($sample in $Samples) {
        try { $value = [double]$sample } catch { throw "Metric sample '$sample' is not numeric." }
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) {
            throw "Metric samples must be finite numbers."
        }
        $values.Add($value)
    }
    $array = $values.ToArray()
    $mean = ($array | Measure-Object -Average).Average
    $sumSquares = 0.0
    foreach ($value in $array) { $sumSquares += [math]::Pow($value - $mean, 2) }
    $standardDeviation = if ($array.Count -gt 1) { [math]::Sqrt($sumSquares / ($array.Count - 1)) } else { 0.0 }
    $coefficient = if ([math]::Abs($mean) -gt [double]::Epsilon) { $standardDeviation / [math]::Abs($mean) } else { $null }
    return [pscustomobject][ordered]@{
        count = $array.Count
        min = [double]($array | Measure-Object -Minimum).Minimum
        p50 = Get-YanamiPercentile -Values $array -Probability 0.50
        p95 = Get-YanamiPercentile -Values $array -Probability 0.95
        p99 = Get-YanamiPercentile -Values $array -Probability 0.99
        max = [double]($array | Measure-Object -Maximum).Maximum
        mean = [double]$mean
        standardDeviation = [double]$standardDeviation
        coefficientOfVariation = $coefficient
    }
}

function Test-YanamiPerfConfiguration {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Slo,
        [Parameter(Mandatory = $true)][object]$Policy
    )

    $errors = New-Object System.Collections.Generic.List[string]
    if ((Get-ObjectPropertyValue $Slo "schemaVersion") -ne "1.0") { $errors.Add("SLO schemaVersion must be 1.0.") }
    if ((Get-ObjectPropertyValue $Policy "schemaVersion") -ne "1.0") { $errors.Add("Policy schemaVersion must be 1.0.") }
    if ((Get-ObjectPropertyValue $Slo "percentileMethod") -ne "linear-r7") { $errors.Add("Only linear-r7 percentileMethod is supported.") }

    $profiles = @("PullRequest", "Lab", "Nightly", "Weekly", "Release")
    $validSuites = @(Get-YanamiPerfSuites)
    $statistics = @("min", "p50", "p95", "p99", "max", "mean", "standardDeviation", "coefficientOfVariation")
    $ids = @{}
    $aliases = @{}
    foreach ($metric in @($Slo.metrics)) {
        $id = [string](Get-ObjectPropertyValue $metric "id" "")
        if ([string]::IsNullOrWhiteSpace($id)) { $errors.Add("A metric has no id."); continue }
        if ($ids.ContainsKey($id)) { $errors.Add("Duplicate metric id: $id") } else { $ids[$id] = $true }
        $metricSuite = [string](Get-ObjectPropertyValue $metric "suite" "")
        if ($metricSuite -notin $validSuites) { $errors.Add("Metric '$id' has unknown suite '$metricSuite'.") }
        foreach ($alias in @(Get-ObjectPropertyValue $metric "aliases" @())) {
            $aliasString = [string]$alias
            if ($ids.ContainsKey($aliasString) -or $aliases.ContainsKey($aliasString)) { $errors.Add("Duplicate metric alias: $aliasString") }
            else { $aliases[$aliasString] = $id }
        }
        if ((Get-ObjectPropertyValue $metric "direction") -notin @("upper", "higher")) { $errors.Add("Metric '$id' has invalid direction.") }
        $producerProbeKind = [string](Get-ObjectPropertyValue $metric "producerProbeKind" "")
        if ($producerProbeKind -and [string]::IsNullOrWhiteSpace([string](Get-ObjectPropertyValue $metric "evidence" ""))) {
            $errors.Add("Metric '$id' declares producerProbeKind without an evidence contract.")
        }
        foreach ($profile in @(Get-ObjectPropertyValue $metric "requiredProfiles" @())) {
            if ($profile -notin $profiles) { $errors.Add("Metric '$id' has unknown profile '$profile'.") }
        }
        $profileSampleRules = Get-ObjectPropertyValue $metric "minimumSamplesByProfile"
        if ($null -ne $profileSampleRules) {
            foreach ($sampleProperty in @($profileSampleRules.PSObject.Properties)) {
                if ($sampleProperty.Name -notin $profiles) { $errors.Add("Metric '$id' has a sample rule for unknown profile '$($sampleProperty.Name)'.") }
                if ([int]$sampleProperty.Value -lt 1) { $errors.Add("Metric '$id' minimum samples for '$($sampleProperty.Name)' must be positive.") }
            }
        }
        $statisticSampleRules = Get-ObjectPropertyValue $metric "minimumSamplesByStatistic"
        if ($null -ne $statisticSampleRules) {
            foreach ($sampleProperty in @($statisticSampleRules.PSObject.Properties)) {
                if ($sampleProperty.Name -notin $statistics) { $errors.Add("Metric '$id' has a sample rule for unsupported statistic '$($sampleProperty.Name)'.") }
                if ([int]$sampleProperty.Value -lt 1) { $errors.Add("Metric '$id' minimum samples for statistic '$($sampleProperty.Name)' must be positive.") }
            }
        }
        $absolute = Get-ObjectPropertyValue $metric "absolute"
        foreach ($setProperty in @($absolute.PSObject.Properties)) {
            foreach ($thresholdProperty in @($setProperty.Value.PSObject.Properties)) {
                if ($thresholdProperty.Name -notin $statistics) { $errors.Add("Metric '$id' uses unsupported statistic '$($thresholdProperty.Name)'.") }
                $op = Get-ObjectPropertyValue $thresholdProperty.Value "op"
                if ($op -notin @("<=", ">=", "<", ">", "==")) { $errors.Add("Metric '$id' has unsupported operator '$op'.") }
                $value = Get-ObjectPropertyValue $thresholdProperty.Value "value"
                if ($null -eq $value) { $errors.Add("Metric '$id' threshold '$($thresholdProperty.Name)' has no value.") }
            }
        }
    }
    foreach ($alias in $aliases.Keys) {
        if ($ids.ContainsKey($alias)) { $errors.Add("Alias '$alias' collides with a canonical metric id.") }
    }

    $invariantIds = @{}
    $invariantAliases = @{}
    foreach ($invariant in @($Slo.invariants)) {
        $id = [string](Get-ObjectPropertyValue $invariant "id" "")
        if ([string]::IsNullOrWhiteSpace($id)) { $errors.Add("An invariant has no id.") }
        elseif ($invariantIds.ContainsKey($id)) { $errors.Add("Duplicate invariant id: $id") }
        else { $invariantIds[$id] = $true }
        $invariantSuite = [string](Get-ObjectPropertyValue $invariant "suite" "")
        if ($invariantSuite -notin $validSuites) { $errors.Add("Invariant '$id' has unknown suite '$invariantSuite'.") }
        $producerProbeKind = [string](Get-ObjectPropertyValue $invariant "producerProbeKind" "")
        if ($producerProbeKind -and [string]::IsNullOrWhiteSpace([string](Get-ObjectPropertyValue $invariant "evidence" ""))) {
            $errors.Add("Invariant '$id' declares producerProbeKind without an evidence contract.")
        }
        foreach ($alias in @(Get-ObjectPropertyValue $invariant "aliases" @())) {
            $aliasString = [string]$alias
            if ($invariantIds.ContainsKey($aliasString) -or $invariantAliases.ContainsKey($aliasString)) { $errors.Add("Duplicate invariant alias: $aliasString") }
            else { $invariantAliases[$aliasString] = $id }
        }
    }
    $currentMode = [string](Get-ObjectPropertyValue $Policy "currentMode" "")
    if ($currentMode -notin @($Policy.allowedModes)) { $errors.Add("Policy currentMode '$currentMode' is not allowed.") }
    if ([string](Get-ObjectPropertyValue $Policy "comparisonOrder" "") -ne "A-B-A-B") { $errors.Add("Policy comparisonOrder must be A-B-A-B.") }
    $profileModeOverrides = Get-ObjectPropertyValue $Policy "profileModeOverrides"
    if ($null -ne $profileModeOverrides) {
        foreach ($override in @($profileModeOverrides.PSObject.Properties)) {
            if ($override.Name -notin $profiles) { $errors.Add("Policy has a mode override for unknown profile '$($override.Name)'.") }
            if ([string]$override.Value -notin @($Policy.allowedModes)) { $errors.Add("Policy profile '$($override.Name)' has unsupported mode '$($override.Value)'.") }
        }
    }
    $requiredFixturesBySuite = Get-ObjectPropertyValue $Policy "requiredFixturesBySuite"
    if ($null -ne $requiredFixturesBySuite) {
        foreach ($fixturePolicy in @($requiredFixturesBySuite.PSObject.Properties)) {
            if ($fixturePolicy.Name -notin $validSuites) {
                $errors.Add("Fixture policy has unknown suite '$($fixturePolicy.Name)'.")
                continue
            }
            $requirement = $fixturePolicy.Value
            if ($requirement -is [System.Management.Automation.PSCustomObject] -or
                $requirement -is [System.Collections.IDictionary]) {
                foreach ($profileRequirement in @($requirement.PSObject.Properties)) {
                    if ($profileRequirement.Name -notin @($profiles + "default")) {
                        $errors.Add("Fixture policy '$($fixturePolicy.Name)' has unknown profile '$($profileRequirement.Name)'.")
                    }
                    $fixtureIds = @($profileRequirement.Value) |
                        ForEach-Object { [string]$_ } |
                        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
                    if (@($fixtureIds).Count -eq 0) {
                        $errors.Add("Fixture policy '$($fixturePolicy.Name)/$($profileRequirement.Name)' is empty.")
                    }
                }
            } else {
                $fixtureIds = @($requirement) |
                    ForEach-Object { [string]$_ } |
                    Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
                if (@($fixtureIds).Count -eq 0) {
                    $errors.Add("Fixture policy '$($fixturePolicy.Name)' is empty.")
                }
            }
        }
    }
    $reproducibility = Get-ObjectPropertyValue $Slo "reproducibility"
    if ($null -ne $reproducibility) {
        foreach ($ruleName in @("searchLatency", "warmStartup", "coldStartup")) {
            $rule = Get-ObjectPropertyValue $reproducibility $ruleName
            if ($null -eq $rule) { $errors.Add("Reproducibility rule '$ruleName' is missing."); continue }
            $maximumCv = Get-ObjectPropertyValue $rule "maximumCv"
            $minimumSamples = Get-ObjectPropertyValue $rule "minimumSamples"
            if ($null -eq $maximumCv -or [double]$maximumCv -le 0) { $errors.Add("Reproducibility rule '$ruleName' must have a positive maximumCv.") }
            if ($null -eq $minimumSamples -or [int]$minimumSamples -lt 1) { $errors.Add("Reproducibility rule '$ruleName' must have positive minimumSamples.") }
        }
        foreach ($metric in @($Slo.metrics)) {
            $metricRuleName = [string](Get-ObjectPropertyValue $metric "reproducibilityRule" "")
            if (-not [string]::IsNullOrWhiteSpace($metricRuleName) -and
                $null -eq (Get-ObjectPropertyValue $reproducibility $metricRuleName)) {
                $errors.Add("Metric '$([string]$metric.id)' references unknown reproducibility rule '$metricRuleName'.")
            }
        }
    }

    return @($errors)
}

function Merge-YanamiRunManifests {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object[]]$Manifests)

    if ($Manifests.Count -eq 0) { throw "At least one run manifest is required." }
    $first = $Manifests[0]
    $profile = [string]$first.profile
    $metrics = [ordered]@{}
    $invariants = [ordered]@{}
    $fixtures = [ordered]@{}
    $suites = New-Object System.Collections.Generic.List[string]
    $referenceMatch = $true
    $fingerprints = New-Object System.Collections.Generic.List[string]
    $mismatchReasons = New-Object System.Collections.Generic.List[string]
    $probeDiagnostics = New-Object System.Collections.Generic.List[string]
    $sourceEnvironments = New-Object System.Collections.Generic.List[object]
    $artifacts = New-Object System.Collections.Generic.List[object]
    $runIds = New-Object 'System.Collections.Generic.HashSet[string]'
    $expectedMode = [string](Get-ObjectPropertyValue $first "mode" "")
    $expectedCandidateSha = [string](Get-ObjectPropertyValue $first "candidateSha" "")
    $expectedBaseSha = [string](Get-ObjectPropertyValue $first "baseSha" "")

    foreach ($manifest in $Manifests) {
        if ([string](Get-ObjectPropertyValue $manifest "schemaVersion") -ne "1.0") { throw "Run manifest schemaVersion must be 1.0." }
        if ([string]$manifest.profile -ne $profile) { throw "Cannot merge run manifests with different profiles." }
        $runId = [string](Get-ObjectPropertyValue $manifest "runId" "")
        if ([string]::IsNullOrWhiteSpace($runId) -or -not $runIds.Add($runId)) {
            throw "Cannot merge run manifests with a missing or duplicate runId '$runId'."
        }
        if ([string](Get-ObjectPropertyValue $manifest "mode" "") -ne $expectedMode) {
            throw "Cannot merge run manifests with different modes."
        }
        if ([string](Get-ObjectPropertyValue $manifest "candidateSha" "") -ne $expectedCandidateSha) {
            throw "Cannot merge run manifests with different candidate SHAs."
        }
        if ([string](Get-ObjectPropertyValue $manifest "baseSha" "") -ne $expectedBaseSha) {
            throw "Cannot merge run manifests with different base SHAs."
        }
        foreach ($suite in @($manifest.suites)) { if (-not $suites.Contains([string]$suite)) { $suites.Add([string]$suite) } }
        $environment = $manifest.environment
        $fingerprint = [string](Get-ObjectPropertyValue $environment "fingerprint" "unknown")
        $sourceEnvironments.Add([pscustomobject][ordered]@{
            runId = $runId
            fingerprint = $fingerprint
            referenceMatch = [bool](Get-ObjectPropertyValue $environment "referenceMatch" $false)
            details = Get-ObjectPropertyValue $environment "details"
            runnerProvenance = Get-ObjectPropertyValue $environment "runnerProvenance"
        })
        if (-not $fingerprints.Contains($fingerprint)) { $fingerprints.Add($fingerprint) }
        if (-not [bool](Get-ObjectPropertyValue $environment "referenceMatch" $false)) { $referenceMatch = $false }
        foreach ($reason in @(Get-ObjectPropertyValue $environment "mismatchReasons" @())) { $mismatchReasons.Add([string]$reason) }
        foreach ($reason in @(Get-ObjectPropertyValue $manifest "reasons" @())) {
            if (-not [string]::IsNullOrWhiteSpace([string]$reason)) { $probeDiagnostics.Add([string]$reason) }
        }
        foreach ($artifact in @(Get-ObjectPropertyValue $manifest "artifacts" @())) {
            $artifacts.Add($artifact)
        }

        foreach ($fixture in @($manifest.fixtures)) {
            $key = "$($fixture.id)@$($fixture.version)"
            if ($fixtures.Contains($key)) {
                $existing = $fixtures[$key]
                if ((Get-ObjectPropertyValue $existing "sha256") -and (Get-ObjectPropertyValue $fixture "sha256") -and $existing.sha256 -ne $fixture.sha256) {
                    $referenceMatch = $false
                    $mismatchReasons.Add("Fixture hash mismatch while merging $key.")
                }
                $existing.validated = [bool]$existing.validated -and [bool]$fixture.validated
            } else { $fixtures[$key] = $fixture }
        }

        foreach ($metric in @($manifest.metrics)) {
            $id = [string]$metric.id
            if ($metrics.Contains($id)) {
                if ([string]$metrics[$id].unit -ne [string]$metric.unit) { throw "Metric '$id' has inconsistent units across manifests." }
                $existingAttributes = ConvertTo-YanamiCanonicalJson $metrics[$id].attributes
                $incomingAttributes = ConvertTo-YanamiCanonicalJson (Get-ObjectPropertyValue $metric "attributes" ([pscustomobject]@{}))
                $isStrictUpscalingScenario = $id -like "upscaling.*" -and
                    -not $id.StartsWith("upscaling.hosted_smoke.", [System.StringComparison]::Ordinal)
                if ($isStrictUpscalingScenario) {
                    $existingEvidence = [string](Get-ObjectPropertyValue $metrics[$id].attributes "evidence" "")
                    $incomingMetricAttributes = Get-ObjectPropertyValue $metric "attributes" ([pscustomobject]@{})
                    $incomingEvidence = [string](Get-ObjectPropertyValue $incomingMetricAttributes "evidence" "")
                    if (-not $existingEvidence -or $existingEvidence -ne $incomingEvidence) {
                        throw "Metric '$id' has incompatible strict upscaling evidence across manifests."
                    }
                    $scenarioMeasurements = New-Object System.Collections.Generic.List[object]
                    foreach ($measurement in @(Get-ObjectPropertyValue $metrics[$id] "scenarioMeasurements" @())) {
                        $scenarioMeasurements.Add($measurement)
                    }
                    if ($scenarioMeasurements.Count -eq 0) {
                        $scenarioMeasurements.Add([pscustomobject][ordered]@{
                            scenarioId = [string](Get-ObjectPropertyValue $metrics[$id].attributes "scenarioId" "")
                            preset = [string](Get-ObjectPropertyValue $metrics[$id].attributes "preset" "")
                            samples = @($metrics[$id].samples)
                        })
                    }
                    $incomingScenarioId = [string](Get-ObjectPropertyValue $incomingMetricAttributes "scenarioId" "")
                    if ([string]::IsNullOrWhiteSpace($incomingScenarioId) -or
                        @($scenarioMeasurements | Where-Object { [string]$_.scenarioId -eq $incomingScenarioId }).Count -gt 0) {
                        throw "Metric '$id' has a missing or duplicate strict upscaling scenario ID '$incomingScenarioId'."
                    }
                    $scenarioMeasurements.Add([pscustomobject][ordered]@{
                        scenarioId = $incomingScenarioId
                        preset = [string](Get-ObjectPropertyValue $incomingMetricAttributes "preset" "")
                        samples = @($metric.samples)
                    })
                    $metrics[$id] | Add-Member -NotePropertyName scenarioMeasurements -NotePropertyValue $scenarioMeasurements.ToArray() -Force
                    $metrics[$id].attributes = [pscustomobject][ordered]@{
                        evidence = $existingEvidence
                        rawDerived = $true
                        scenarioAggregated = $true
                        scenarioIds = @($scenarioMeasurements | ForEach-Object { [string]$_.scenarioId })
                    }
                } elseif ($existingAttributes -ne $incomingAttributes) {
                    throw "Metric '$id' has incompatible measurement attributes across manifests."
                }
                $metrics[$id].samples = @($metrics[$id].samples) + @($metric.samples)
                $metrics[$id].producerRunIds = @(
                    @($metrics[$id].producerRunIds) + @(Get-ObjectPropertyValue $metric "producerRunIds" @()) |
                        ForEach-Object { [string]$_ } |
                        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                        Select-Object -Unique
                )
            } else {
                $attributes = Get-ObjectPropertyValue $metric "attributes" ([pscustomobject]@{})
                $scenarioMeasurements = if ($id -like "upscaling.*" -and
                    -not $id.StartsWith("upscaling.hosted_smoke.", [System.StringComparison]::Ordinal) -and
                    -not [string]::IsNullOrWhiteSpace([string](Get-ObjectPropertyValue $attributes "scenarioId" ""))) {
                    @([pscustomobject][ordered]@{
                        scenarioId = [string](Get-ObjectPropertyValue $attributes "scenarioId" "")
                        preset = [string](Get-ObjectPropertyValue $attributes "preset" "")
                        samples = @($metric.samples)
                    })
                } else { @() }
                $metrics[$id] = [pscustomobject][ordered]@{
                    id = $id
                    unit = [string]$metric.unit
                    samples = @($metric.samples)
                    attributes = $attributes
                    scenarioMeasurements = $scenarioMeasurements
                    producerRunIds = @((Get-ObjectPropertyValue $metric "producerRunIds" @()) | ForEach-Object { [string]$_ } | Select-Object -Unique)
                }
            }
        }
        foreach ($invariant in @($manifest.invariants)) {
            $id = [string]$invariant.id
            if ($invariants.Contains($id)) {
                $invariants[$id].passed = [bool]$invariants[$id].passed -and [bool]$invariant.passed
                if ($id -like "upscaling.*") {
                    $existingDetails = Get-ObjectPropertyValue $invariants[$id] "details" ([pscustomobject]@{})
                    $incomingDetails = Get-ObjectPropertyValue $invariant "details" ([pscustomobject]@{})
                    $existingEvidence = [string](Get-ObjectPropertyValue $existingDetails "evidence" "")
                    $incomingEvidence = [string](Get-ObjectPropertyValue $incomingDetails "evidence" "")
                    if (-not $existingEvidence -or $existingEvidence -ne $incomingEvidence) {
                        throw "Invariant '$id' has incompatible strict upscaling evidence across manifests."
                    }
                    $scenarioResults = @((Get-ObjectPropertyValue $existingDetails "scenarios" @()))
                    if ($scenarioResults.Count -eq 0) { $scenarioResults = @($existingDetails) }
                    $scenarioResults += @($incomingDetails)
                    $invariants[$id].details = [pscustomobject][ordered]@{
                        evidence = $existingEvidence
                        rawDerived = $true
                        scenarios = $scenarioResults
                    }
                } else {
                    $invariants[$id].details = @($invariants[$id].details) + @(Get-ObjectPropertyValue $invariant "details")
                }
                $invariants[$id].producerRunIds = @(
                    @($invariants[$id].producerRunIds) + @(Get-ObjectPropertyValue $invariant "producerRunIds" @()) |
                        ForEach-Object { [string]$_ } |
                        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                        Select-Object -Unique
                )
            } else {
                $invariants[$id] = [pscustomobject][ordered]@{
                    id = $id
                    passed = [bool]$invariant.passed
                    details = Get-ObjectPropertyValue $invariant "details"
                    producerRunIds = @((Get-ObjectPropertyValue $invariant "producerRunIds" @()) | ForEach-Object { [string]$_ } | Select-Object -Unique)
                }
            }
        }
    }

    if ($fingerprints.Count -gt 1) {
        $referenceMatch = $false
        $mismatchReasons.Add("Probe-local fingerprints differed and were retained for diagnostics: $($fingerprints -join ', ').")
    }
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        runId = (@($Manifests | ForEach-Object { $_.runId }) -join "+")
        profile = $profile
        mode = $expectedMode
        startedAtUtc = [string]$first.startedAtUtc
        candidateSha = $expectedCandidateSha
        baseSha = $expectedBaseSha
        environment = [pscustomobject][ordered]@{
            fingerprint = [string]$fingerprints[0]
            referenceMatch = $referenceMatch
            mismatchReasons = $mismatchReasons.ToArray()
            probeFingerprints = $fingerprints.ToArray()
            sourceEnvironments = $sourceEnvironments.ToArray()
        }
        fixtures = @($fixtures.Values)
        suites = $suites.ToArray()
        metrics = @($metrics.Values)
        invariants = @($invariants.Values)
        probeDiagnostics = $probeDiagnostics.ToArray()
        artifacts = $artifacts.ToArray()
    }
}

function Test-Threshold {
    param([double]$Actual, [object]$Rule)
    $expected = [double]$Rule.value
    switch ([string]$Rule.op) {
        "<=" { return $Actual -le $expected }
        ">=" { return $Actual -ge $expected }
        "<" { return $Actual -lt $expected }
        ">" { return $Actual -gt $expected }
        "==" { return $Actual -eq $expected }
        default { throw "Unsupported threshold operator '$($Rule.op)'." }
    }
}

function Get-BaseMetricStatistic {
    param(
        [AllowNull()][object]$Base,
        [Parameter(Mandatory = $true)][string]$MetricId,
        [Parameter(Mandatory = $true)][string]$Statistic
    )

    if ($null -eq $Base) { return $null }
    $baseMetrics = Get-ObjectPropertyValue $Base "metrics"
    if ($null -eq $baseMetrics) { return $null }
    if ($baseMetrics -is [array] -or $baseMetrics -is [System.Collections.IList]) {
        $metric = @($baseMetrics | Where-Object { [string]$_.id -eq $MetricId } | Select-Object -First 1)
        if ($metric.Count -eq 0) { return $null }
        $statistics = Get-ObjectPropertyValue $metric[0] "statistics"
        if ($null -ne $statistics) { return Get-ObjectPropertyValue $statistics $Statistic }
        $samples = @(Get-ObjectPropertyValue $metric[0] "samples" @())
        if ($samples.Count -gt 0) { return Get-ObjectPropertyValue (Get-YanamiStatistics -Samples $samples) $Statistic }
        return $null
    }
    $metricProperty = $baseMetrics.PSObject.Properties[$MetricId]
    if ($null -eq $metricProperty) { return $null }
    return Get-ObjectPropertyValue $metricProperty.Value $Statistic
}

function Get-FailureDisposition {
    param(
        [string]$Mode,
        [string]$FailureKind,
        [string]$MetricId,
        [string]$Priority,
        [string]$Profile,
        [object]$Policy
    )

    if ($FailureKind -eq "correctness") { return "fail" }
    if ($MetricId -in @(Get-ObjectPropertyValue $Policy "lockedMetrics" @())) { return "fail" }
    if ($Profile -eq "Release" -and $Priority -eq "P0" -and [bool](Get-ObjectPropertyValue $Policy "releaseP0DebtIsFailure" $true)) { return "fail" }
    $modeRule = Get-ObjectPropertyValue $Policy.modeRules $Mode
    if ($FailureKind -eq "relative") { return [string](Get-ObjectPropertyValue $modeRule "relativePerformanceFailure" "fail") }
    return [string](Get-ObjectPropertyValue $modeRule "absolutePerformanceFailure" "fail")
}

function Invoke-YanamiPerfEvaluation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][object]$Slo,
        [Parameter(Mandatory = $true)][object]$Policy,
        [AllowNull()][object]$BaseResult = $null,
        [ValidateSet("collect", "debt", "enforce")][string]$Mode,
        [string[]]$Suites,
        [string]$CandidateSha,
        [string]$BaseSha,
        [object[]]$TrustedProducerAttestations = @()
    )

    $configurationErrors = @(Test-YanamiPerfConfiguration -Slo $Slo -Policy $Policy)
    if ($configurationErrors.Count -gt 0) { throw "Invalid performance configuration: $($configurationErrors -join ' ')" }
    $profile = [string]$Manifest.profile
    if ($profile -notin @("PullRequest", "Lab", "Nightly", "Weekly", "Release")) { throw "Unknown run profile '$profile'." }
    if ([string]::IsNullOrWhiteSpace($Mode)) {
        $manifestMode = [string](Get-ObjectPropertyValue $Manifest "mode" "")
        $Mode = if ($manifestMode -in @("collect", "debt", "enforce")) { $manifestMode } else { [string]$Policy.currentMode }
    }
    if (-not $Suites -or $Suites.Count -eq 0) { $Suites = @($Manifest.suites) }
    $Suites = @($Suites | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ } | Select-Object -Unique)
    $validSuites = @(Get-YanamiPerfSuites)
    foreach ($suite in $Suites) { if ($suite -notin $validSuites) { throw "Unknown suite '$suite'." } }

    $resultReasons = New-Object System.Collections.Generic.List[string]
    $infraReasons = New-Object System.Collections.Generic.List[string]
    $baseComparisonInfraReasons = New-Object System.Collections.Generic.List[string]
    $measurementDebtReasons = New-Object System.Collections.Generic.List[string]
    if ($profile -in @($Policy.referenceEnvironmentRequired) -and -not [bool]$Manifest.environment.referenceMatch) {
        $infraReasons.Add("Reference environment fingerprint did not match: $(@(Get-ObjectPropertyValue $Manifest.environment 'mismatchReasons' @()) -join '; ').")
    }
    if ($profile -in @($Policy.fixtureValidationRequired)) {
        foreach ($fixture in @($Manifest.fixtures)) {
            if (-not [bool]$fixture.validated) { $infraReasons.Add("Fixture '$($fixture.id)@$($fixture.version)' is not validated.") }
        }
        foreach ($suite in $Suites) {
            foreach ($requiredFixtureId in @(Get-YanamiRequiredFixtureIds -Policy $Policy -Suite $suite -Profile $profile)) {
                $fixtureMatch = @($Manifest.fixtures | Where-Object { [string]$_.id -eq $requiredFixtureId -and [bool]$_.validated })
                if ($fixtureMatch.Count -eq 0) {
                    $providedFixture = @($Manifest.fixtures | Where-Object { [string]$_.id -eq $requiredFixtureId })
                    $missingMessage = "Suite '$suite' requires validated fixture '$requiredFixtureId'."
                    if ($providedFixture.Count -gt 0) {
                        $infraReasons.Add($missingMessage)
                    } else {
                        $missingDisposition = [string](Get-ObjectPropertyValue $Policy.missingMeasurementStatusByMode $Mode "infra-invalid")
                        if ($missingDisposition -eq "debt") { $measurementDebtReasons.Add($missingMessage) } else { $infraReasons.Add($missingMessage) }
                    }
                }
            }
        }
    }
    if ($null -ne $BaseResult) {
        if ([string](Get-ObjectPropertyValue $BaseResult "profile" "") -ne $profile) {
            $baseComparisonInfraReasons.Add("Base/head profiles differ; relative comparison is invalid.")
        }
        if ([string](Get-ObjectPropertyValue $BaseResult "contractVersion" "") -ne [string]$Slo.id) {
            $baseComparisonInfraReasons.Add("Base PerfResult does not use the active SLO contract '$($Slo.id)'.")
        }
        if ($BaseSha -and [string](Get-ObjectPropertyValue $BaseResult "candidateSha" "") -ne $BaseSha) {
            $baseComparisonInfraReasons.Add("Base PerfResult candidate SHA does not match requested base '$BaseSha'.")
        }
        $baseFingerprint = [string](Get-ObjectPropertyValue (Get-ObjectPropertyValue $BaseResult "environment") "fingerprint" "")
        $headFingerprint = [string](Get-ObjectPropertyValue $Manifest.environment "fingerprint" "")
        if (-not $baseFingerprint -or -not $headFingerprint) {
            $baseComparisonInfraReasons.Add("Base/head environment fingerprints are required for relative comparison.")
        } elseif ($baseFingerprint -ne $headFingerprint) {
            $baseComparisonInfraReasons.Add("Base/head environment fingerprints differ; relative comparison is invalid.")
        }
        foreach ($suite in $Suites) {
            foreach ($requiredFixtureId in @(Get-YanamiRequiredFixtureIds -Policy $Policy -Suite $suite -Profile $profile)) {
                $baseFixtures = @($BaseResult.fixtures | Where-Object { [string]$_.id -eq $requiredFixtureId -and [bool]$_.validated })
                $headFixtures = @($Manifest.fixtures | Where-Object { [string]$_.id -eq $requiredFixtureId -and [bool]$_.validated })
                if ($baseFixtures.Count -ne 1 -or $headFixtures.Count -ne 1) {
                    $baseComparisonInfraReasons.Add("Base/head comparison requires exactly one validated '$requiredFixtureId' fixture.")
                    continue
                }
                $baseFixtureHash = [string](Get-ObjectPropertyValue $baseFixtures[0] "sha256" "")
                $headFixtureHash = [string](Get-ObjectPropertyValue $headFixtures[0] "sha256" "")
                if (-not $baseFixtureHash -or $baseFixtureHash -ne $headFixtureHash) {
                    $baseComparisonInfraReasons.Add("Base/head fixture hash differs for '$requiredFixtureId'.")
                }
            }
        }
    }
    $rawById = @{}
    foreach ($rawMetric in @($Manifest.metrics)) {
        $rawId = [string]$rawMetric.id
        if ($rawById.ContainsKey($rawId)) { $infraReasons.Add("Duplicate raw metric id '$rawId'.") }
        else { $rawById[$rawId] = $rawMetric }
    }
    if ($null -ne $BaseResult -and $baseComparisonInfraReasons.Count -eq 0) {
        foreach ($metricDefinition in @($Slo.metrics)) {
            if ([string]$metricDefinition.suite -notin $Suites) { continue }
            $relativeStatistic = [string](Get-ObjectPropertyValue $metricDefinition "relativeStatistic" "")
            $relativeRule = Get-ObjectPropertyValue $Slo.relativeRules ([string]$metricDefinition.category)
            if (-not $relativeStatistic -or $null -eq $relativeRule) { continue }
            $candidateIds = @([string]$metricDefinition.id) + @((Get-ObjectPropertyValue $metricDefinition "aliases" @()) | ForEach-Object { [string]$_ })
            if (@($candidateIds | Where-Object { $rawById.ContainsKey($_) }).Count -eq 0) { continue }
            if ($null -eq (Get-BaseMetricStatistic -Base $BaseResult -MetricId ([string]$metricDefinition.id) -Statistic $relativeStatistic)) {
                $baseComparisonInfraReasons.Add("Base metric '$($metricDefinition.id)' is missing statistic '$relativeStatistic' required by the candidate comparison.")
            }
        }
    }
    foreach ($reason in $baseComparisonInfraReasons) { $infraReasons.Add($reason) }
    $baseComparisonEligible = $null -ne $BaseResult -and $baseComparisonInfraReasons.Count -eq 0

    $metricResults = New-Object System.Collections.Generic.List[object]
    $consumedRawIds = @{}
    $thresholdSet = [string](Get-ObjectPropertyValue $Slo.thresholdSetByProfile $profile)
    foreach ($metricDefinition in @($Slo.metrics)) {
        if ([string]$metricDefinition.suite -notin $Suites) { continue }
        $isRequired = $profile -in @($metricDefinition.requiredProfiles)
        $candidateIds = @([string]$metricDefinition.id) + @((Get-ObjectPropertyValue $metricDefinition "aliases" @()) | ForEach-Object { [string]$_ })
        $matches = @($candidateIds | Where-Object { $rawById.ContainsKey($_) })
        if ($matches.Count -eq 0) {
            if ($isRequired) {
                $missingMessage = "Required metric '$($metricDefinition.id)' is missing for $profile/$($metricDefinition.suite)."
                $missingDisposition = [string](Get-ObjectPropertyValue $Policy.missingMeasurementStatusByMode $Mode "infra-invalid")
                if ($missingDisposition -eq "debt") { $measurementDebtReasons.Add($missingMessage) } else { $infraReasons.Add($missingMessage) }
            }
            continue
        }
        if ($matches.Count -gt 1) { $infraReasons.Add("Metric '$($metricDefinition.id)' was reported under multiple canonical/alias ids."); continue }
        $raw = $rawById[$matches[0]]
        $consumedRawIds[$matches[0]] = $true
        if ([string]$raw.unit -ne [string]$metricDefinition.unit) {
            $infraReasons.Add("Metric '$($metricDefinition.id)' unit '$($raw.unit)' does not match '$($metricDefinition.unit)'.")
            continue
        }
        $samples = @($raw.samples)
        $minimumSamplesByProfile = Get-ObjectPropertyValue $metricDefinition "minimumSamplesByProfile"
        $minimumSamples = Get-ObjectPropertyValue $minimumSamplesByProfile $profile
        $minimumSamplesObject = Get-ObjectPropertyValue $metricDefinition "minimumSamples"
        if ($null -eq $minimumSamples) { $minimumSamples = Get-ObjectPropertyValue $minimumSamplesObject $thresholdSet }
        if ($null -eq $minimumSamples) { $minimumSamples = Get-ObjectPropertyValue $Slo.defaultMinimumSamples $thresholdSet 1 }
        $scenarioStatistics = New-Object System.Collections.Generic.List[object]
        $scenarioMeasurements = @((Get-ObjectPropertyValue $raw "scenarioMeasurements" @()))
        if ($thresholdSet -eq "strict" -and
            [string]$metricDefinition.suite -eq "upscaling" -and
            -not ([string]$metricDefinition.id).StartsWith("upscaling.hosted_smoke.", [System.StringComparison]::Ordinal)) {
            if ($scenarioMeasurements.Count -eq 0) {
                $rawAttributes = Get-ObjectPropertyValue $raw "attributes" ([pscustomobject]@{})
                $rawScenarioId = [string](Get-ObjectPropertyValue $rawAttributes "scenarioId" "")
                if (-not [string]::IsNullOrWhiteSpace($rawScenarioId)) {
                    $scenarioMeasurements = @([pscustomobject][ordered]@{
                        scenarioId = $rawScenarioId
                        preset = [string](Get-ObjectPropertyValue $rawAttributes "preset" "")
                        samples = $samples
                    })
                }
            }
            if ($scenarioMeasurements.Count -eq 0) {
                $infraReasons.Add("Metric '$($metricDefinition.id)' has no runner-derived strict scenario measurements.")
                continue
            }
            $seenScenarioIds = [System.Collections.Generic.HashSet[string]]::new(
                [System.StringComparer]::Ordinal)
            $scenarioInvalid = $false
            foreach ($scenarioMeasurement in $scenarioMeasurements) {
                $scenarioId = [string](Get-ObjectPropertyValue $scenarioMeasurement "scenarioId" "")
                $preset = [string](Get-ObjectPropertyValue $scenarioMeasurement "preset" "")
                $scenarioSamples = @((Get-ObjectPropertyValue $scenarioMeasurement "samples" @()))
                if ([string]::IsNullOrWhiteSpace($scenarioId) -or
                    -not $seenScenarioIds.Add($scenarioId) -or
                    $preset -notin @("performance", "balanced", "quality")) {
                    $infraReasons.Add("Metric '$($metricDefinition.id)' has an invalid or duplicate strict scenario '$scenarioId'/'$preset'.")
                    $scenarioInvalid = $true
                    continue
                }
                if ($scenarioSamples.Count -lt [int]$minimumSamples) {
                    $infraReasons.Add("Metric '$($metricDefinition.id)' scenario '$scenarioId' has $($scenarioSamples.Count) samples; at least $minimumSamples are required per preset.")
                    $scenarioInvalid = $true
                    continue
                }
                try { $oneScenarioStatistics = Get-YanamiStatistics -Samples $scenarioSamples }
                catch {
                    $infraReasons.Add("Metric '$($metricDefinition.id)' scenario '$scenarioId' is invalid: $($_.Exception.Message)")
                    $scenarioInvalid = $true
                    continue
                }
                $scenarioStatistics.Add([pscustomobject][ordered]@{
                    scenarioId = $scenarioId
                    preset = $preset
                    sampleCount = $scenarioSamples.Count
                    statistics = $oneScenarioStatistics
                })
            }
            if ($scenarioInvalid) { continue }
        }
        if ($samples.Count -lt [int]$minimumSamples) {
            $infraReasons.Add("Metric '$($metricDefinition.id)' has $($samples.Count) samples; at least $minimumSamples are required.")
            continue
        }
        try { $statistics = Get-YanamiStatistics -Samples $samples } catch { $infraReasons.Add("Metric '$($metricDefinition.id)' is invalid: $($_.Exception.Message)"); continue }
        $stabilityDeferredReason = ""
        if ($thresholdSet -eq "strict" -and [string]$metricDefinition.unit -eq "ms") {
            $reproducibility = Get-ObjectPropertyValue $Slo "reproducibility"
            $stabilityRuleName = if ([string]$metricDefinition.suite -eq "search") {
                [string](Get-ObjectPropertyValue $metricDefinition "reproducibilityRule" "")
            } elseif ([string]$metricDefinition.id -like "startup.warm.*") {
                "warmStartup"
            } elseif ([string]$metricDefinition.id -like "startup.cold.*") {
                "coldStartup"
            } else { "" }
            $stabilityRule = if ([string]::IsNullOrWhiteSpace($stabilityRuleName)) {
                $null
            } else {
                Get-ObjectPropertyValue $reproducibility $stabilityRuleName
            }
            if ($null -ne $stabilityRule) {
                $stabilityMinimumSamples = [int](Get-ObjectPropertyValue $stabilityRule "minimumSamples" 1)
                $stabilityMaximumCv = [double](Get-ObjectPropertyValue $stabilityRule "maximumCv" 0)
                if ($samples.Count -lt $stabilityMinimumSamples) {
                    $stabilityDeferredReason = "Homogeneous stability has only $($samples.Count) samples; $stabilityMinimumSamples are required by reproducibility rule '$stabilityRuleName'."
                } elseif ($null -ne $statistics.coefficientOfVariation -and
                    [double]$statistics.coefficientOfVariation -gt $stabilityMaximumCv) {
                    $infraReasons.Add(
                        "Metric '$($metricDefinition.id)' CV=$([math]::Round([double]$statistics.coefficientOfVariation, 6)) exceeds the reproducibility limit $($stabilityRule.maximumCv); the run is too unstable for a product decision.")
                }
            }
        }
        $requiredEvidence = [string](Get-ObjectPropertyValue $metricDefinition "evidence" "")
        if ($requiredEvidence) {
            $actualEvidence = [string](Get-ObjectPropertyValue (Get-ObjectPropertyValue $raw "attributes") "evidence" "")
            if ($actualEvidence -ne $requiredEvidence) {
                $infraReasons.Add("Metric '$($metricDefinition.id)' requires '$requiredEvidence' evidence; '$actualEvidence' was reported.")
                continue
            }
            if ([string]$metricDefinition.suite -in @("danmaku", "upscaling")) {
                foreach ($artifactRole in @(Get-YanamiEvidenceArtifactRoles -Evidence $requiredEvidence)) {
                    $validatedArtifacts = @((Get-ObjectPropertyValue $Manifest "artifacts" @()) | Where-Object {
                        [string](Get-ObjectPropertyValue $_ "role" "") -eq $artifactRole -and
                        [bool](Get-ObjectPropertyValue $_ "runnerValidated" $false) -and
                        [string](Get-ObjectPropertyValue $_ "sha256" "") -match '^[0-9a-fA-F]{64}$'
                    })
                    if ($validatedArtifacts.Count -eq 0) {
                        $infraReasons.Add("Metric '$($metricDefinition.id)' requires runner-validated '$artifactRole' raw evidence; an evidence label alone is insufficient.")
                    }
                }
                if (@(Get-YanamiEvidenceArtifactRoles -Evidence $requiredEvidence).Count -gt 0 -and
                    @($infraReasons | Where-Object { $_ -like "Metric '$($metricDefinition.id)' requires runner-validated*" }).Count -gt 0) {
                    continue
                }
            }
        }
        $producerProbeKind = [string](Get-ObjectPropertyValue $metricDefinition "producerProbeKind" "")
        if ($producerProbeKind -and -not (Test-YanamiProducerProvenance -Manifest $Manifest -ProbeKind $producerProbeKind -ObservationProducerRunIds @(Get-ObjectPropertyValue $raw "producerRunIds" @()) -TrustedProducerAttestations $TrustedProducerAttestations)) {
            $infraReasons.Add("Metric '$($metricDefinition.id)' requires trusted current-process attestation for producer probe '$producerProbeKind'; self-described manifest provenance is insufficient.")
            continue
        }

        $target = Get-ObjectPropertyValue $metricDefinition.absolute $thresholdSet
        $metricReasons = New-Object System.Collections.Generic.List[string]
        if ($stabilityDeferredReason) { $metricReasons.Add($stabilityDeferredReason) }
        $absoluteFailed = $false
        $evaluatedTarget = [ordered]@{}
        $deferredStatistics = New-Object System.Collections.Generic.List[object]
        $minimumSamplesByStatistic = Get-ObjectPropertyValue $metricDefinition "minimumSamplesByStatistic"
        $targetProperties = if ($null -eq $target) { @() } else { @($target.PSObject.Properties) }
        foreach ($targetProperty in $targetProperties) {
            $statisticMinimum = Get-ObjectPropertyValue $minimumSamplesByStatistic $targetProperty.Name
            if ($null -ne $statisticMinimum -and $samples.Count -lt [int]$statisticMinimum) {
                $deferredStatistics.Add([pscustomobject][ordered]@{
                    statistic = $targetProperty.Name
                    requiredSamples = [int]$statisticMinimum
                    actualSamples = $samples.Count
                })
                $metricReasons.Add("$($targetProperty.Name) is not evaluated until $statisticMinimum accumulated samples are available; this result has $($samples.Count).")
                continue
            }
            $evaluatedTarget[$targetProperty.Name] = $targetProperty.Value
            $actual = [double](Get-ObjectPropertyValue $statistics $targetProperty.Name)
            if (-not (Test-Threshold -Actual $actual -Rule $targetProperty.Value)) {
                $absoluteFailed = $true
                $metricReasons.Add("$($targetProperty.Name)=$([math]::Round($actual, 6)) violates $($targetProperty.Value.op)$($targetProperty.Value.value) $($metricDefinition.unit).")
            }
            foreach ($scenarioResult in $scenarioStatistics) {
                $scenarioActual = [double](Get-ObjectPropertyValue $scenarioResult.statistics $targetProperty.Name)
                if (-not (Test-Threshold -Actual $scenarioActual -Rule $targetProperty.Value)) {
                    $absoluteFailed = $true
                    $metricReasons.Add("scenario '$($scenarioResult.scenarioId)'/$($scenarioResult.preset) $($targetProperty.Name)=$([math]::Round($scenarioActual, 6)) violates $($targetProperty.Value.op)$($targetProperty.Value.value) $($metricDefinition.unit).")
                }
            }
        }

        $comparison = $null
        $relativeFailed = $false
        $relativeStatistic = [string](Get-ObjectPropertyValue $metricDefinition "relativeStatistic" "")
        $relativeRule = Get-ObjectPropertyValue $Slo.relativeRules ([string]$metricDefinition.category)
        if ($relativeStatistic -and $null -ne $relativeRule -and $baseComparisonEligible) {
            $baseValueObject = Get-BaseMetricStatistic -Base $BaseResult -MetricId ([string]$metricDefinition.id) -Statistic $relativeStatistic
            if ($null -ne $baseValueObject) {
                $headValue = [double](Get-ObjectPropertyValue $statistics $relativeStatistic)
                $baseValue = [double]$baseValueObject
                $degradation = if ([string]$metricDefinition.direction -eq "higher") { $baseValue - $headValue } else { $headValue - $baseValue }
                if ([math]::Abs($baseValue) -le [double]::Epsilon) { $degradationPercent = if ($degradation -gt 0) { [double]::PositiveInfinity } else { 0.0 } }
                else { $degradationPercent = ($degradation / [math]::Abs($baseValue)) * 100.0 }
                $relativeFailed = $degradation -gt [double]$relativeRule.absolute -and $degradationPercent -gt [double]$relativeRule.percent
                $comparison = [pscustomobject][ordered]@{
                    statistic = $relativeStatistic
                    base = $baseValue
                    candidate = $headValue
                    degradationAbsolute = $degradation
                    degradationPercent = $degradationPercent
                    allowedAbsolute = [double]$relativeRule.absolute
                    allowedPercent = [double]$relativeRule.percent
                    failed = $relativeFailed
                }
                if ($relativeFailed) { $metricReasons.Add("Relative regression is $([math]::Round($degradationPercent, 3))% and $([math]::Round($degradation, 6)) $($metricDefinition.unit); both limits were exceeded.") }
            }
        }

        $metricStatus = "pass"
        if ($absoluteFailed) {
            $failureKind = if ([string](Get-ObjectPropertyValue $metricDefinition "enforcement" "") -eq "correctness") { "correctness" } else { "absolute" }
            $metricStatus = Get-FailureDisposition -Mode $Mode -FailureKind $failureKind -MetricId ([string]$metricDefinition.id) -Priority ([string]$metricDefinition.priority) -Profile $profile -Policy $Policy
        }
        if ($relativeFailed) {
            $relativeStatus = Get-FailureDisposition -Mode $Mode -FailureKind "relative" -MetricId ([string]$metricDefinition.id) -Priority ([string]$metricDefinition.priority) -Profile $profile -Policy $Policy
            if ($relativeStatus -eq "fail" -or $metricStatus -eq "pass") { $metricStatus = $relativeStatus }
        }
        if ($stabilityDeferredReason) {
            $stabilityStatus = Get-FailureDisposition -Mode $Mode -FailureKind "reproducibility-deferred" -MetricId ([string]$metricDefinition.id) -Priority ([string]$metricDefinition.priority) -Profile $profile -Policy $Policy
            if ($stabilityStatus -eq "fail" -or $metricStatus -eq "pass") { $metricStatus = $stabilityStatus }
        }
        $metricResults.Add([pscustomobject][ordered]@{
            id = [string]$metricDefinition.id
            sourceId = [string]$raw.id
            suite = [string]$metricDefinition.suite
            category = [string]$metricDefinition.category
            priority = [string]$metricDefinition.priority
            unit = [string]$metricDefinition.unit
            samples = @($samples | ForEach-Object { [double]$_ })
            statistics = $statistics
            scenarioStatistics = $scenarioStatistics.ToArray()
            absoluteTarget = [pscustomobject]$evaluatedTarget
            deferredStatistics = $deferredStatistics.ToArray()
            comparison = $comparison
            attributes = Get-ObjectPropertyValue $raw "attributes" ([pscustomobject]@{})
            status = $metricStatus
            reasons = $metricReasons.ToArray()
        })
    }

    foreach ($rawMetric in @($Manifest.metrics)) {
        if ($consumedRawIds.ContainsKey([string]$rawMetric.id)) { continue }
        try { $statistics = Get-YanamiStatistics -Samples @($rawMetric.samples) } catch { $infraReasons.Add("Observation metric '$($rawMetric.id)' is invalid: $($_.Exception.Message)"); continue }
        $metricResults.Add([pscustomobject][ordered]@{
            id = [string]$rawMetric.id
            sourceId = [string]$rawMetric.id
            suite = "observation"
            category = "observation"
            priority = "P2"
            unit = [string]$rawMetric.unit
            samples = @($rawMetric.samples | ForEach-Object { [double]$_ })
            statistics = $statistics
            absoluteTarget = [pscustomobject]@{}
            comparison = $null
            attributes = Get-ObjectPropertyValue $rawMetric "attributes" ([pscustomobject]@{})
            status = "pass"
            reasons = @("No SLO-v1 threshold; retained as an observation.")
        })
    }

    $rawInvariants = @{}
    foreach ($invariant in @($Manifest.invariants)) {
        if ($rawInvariants.ContainsKey([string]$invariant.id)) { $infraReasons.Add("Duplicate invariant id '$($invariant.id)'.") }
        else { $rawInvariants[[string]$invariant.id] = $invariant }
    }
    $invariantResults = New-Object System.Collections.Generic.List[object]
    foreach ($definition in @($Slo.invariants)) {
        if ([string]$definition.suite -notin $Suites -or $profile -notin @($definition.requiredProfiles)) { continue }
        $id = [string]$definition.id
        $candidateInvariantIds = @($id) + @((Get-ObjectPropertyValue $definition "aliases" @()) | ForEach-Object { [string]$_ })
        $invariantMatches = @($candidateInvariantIds | Where-Object { $rawInvariants.ContainsKey($_) })
        if ($invariantMatches.Count -eq 0) {
            $missingMessage = "Required invariant '$id' is missing for $profile/$($definition.suite)."
            $missingDisposition = [string](Get-ObjectPropertyValue $Policy.missingMeasurementStatusByMode $Mode "infra-invalid")
            if ($missingDisposition -eq "debt") { $measurementDebtReasons.Add($missingMessage) } else { $infraReasons.Add($missingMessage) }
            continue
        }
        if ($invariantMatches.Count -gt 1) { $infraReasons.Add("Invariant '$id' was reported under multiple canonical/alias ids."); continue }
        $rawInvariant = $rawInvariants[$invariantMatches[0]]
        $details = Get-ObjectPropertyValue $rawInvariant "details" ([pscustomobject]@{})
        $requiredEvidence = [string](Get-ObjectPropertyValue $definition "evidence" "")
        if ($requiredEvidence) {
            $actualEvidence = [string](Get-ObjectPropertyValue $details "evidence" "")
            if ($actualEvidence -ne $requiredEvidence) {
                $infraReasons.Add("Invariant '$id' requires '$requiredEvidence' evidence; '$actualEvidence' was reported.")
                continue
            }
            if ([string]$definition.suite -in @("danmaku", "upscaling")) {
                foreach ($artifactRole in @(Get-YanamiEvidenceArtifactRoles -Evidence $requiredEvidence)) {
                    $validatedArtifacts = @((Get-ObjectPropertyValue $Manifest "artifacts" @()) | Where-Object {
                        [string](Get-ObjectPropertyValue $_ "role" "") -eq $artifactRole -and
                        [bool](Get-ObjectPropertyValue $_ "runnerValidated" $false) -and
                        [string](Get-ObjectPropertyValue $_ "sha256" "") -match '^[0-9a-fA-F]{64}$'
                    })
                    if ($validatedArtifacts.Count -eq 0) {
                        $infraReasons.Add("Invariant '$id' requires runner-validated '$artifactRole' raw evidence; an evidence label alone is insufficient.")
                    }
                }
                if (@(Get-YanamiEvidenceArtifactRoles -Evidence $requiredEvidence).Count -gt 0 -and
                    @($infraReasons | Where-Object { $_ -like "Invariant '$id' requires runner-validated*" }).Count -gt 0) {
                    continue
                }
            }
        }
        $producerProbeKind = [string](Get-ObjectPropertyValue $definition "producerProbeKind" "")
        if ($producerProbeKind -and -not (Test-YanamiProducerProvenance -Manifest $Manifest -ProbeKind $producerProbeKind -ObservationProducerRunIds @(Get-ObjectPropertyValue $rawInvariant "producerRunIds" @()) -TrustedProducerAttestations $TrustedProducerAttestations)) {
            $infraReasons.Add("Invariant '$id' requires trusted current-process attestation for producer probe '$producerProbeKind'; self-described manifest provenance is insufficient.")
            continue
        }
        $passed = [bool]$rawInvariant.passed
        $invariantResults.Add([pscustomobject][ordered]@{
            id = $id
            passed = $passed
            status = if ($passed) { "pass" } else { "fail" }
            details = $details
        })
    }

    foreach ($reason in $infraReasons) { $resultReasons.Add($reason) }
    foreach ($reason in $measurementDebtReasons) { $resultReasons.Add("Calibration measurement debt: $reason") }
    foreach ($metric in $metricResults) { foreach ($reason in @($metric.reasons)) { if ($metric.status -ne "pass") { $resultReasons.Add("$($metric.id): $reason") } } }
    foreach ($invariant in $invariantResults) { if (-not $invariant.passed) { $resultReasons.Add("Invariant failed: $($invariant.id).") } }
    $probeDiagnostics = @((Get-ObjectPropertyValue $Manifest "probeDiagnostics" @())) + @((Get-ObjectPropertyValue $Manifest "reasons" @()))
    foreach ($diagnostic in $probeDiagnostics) {
        if (-not [string]::IsNullOrWhiteSpace([string]$diagnostic)) { $resultReasons.Add("Probe diagnostic: $diagnostic") }
    }
    $status = "pass"
    if ($infraReasons.Count -gt 0) { $status = "infra-invalid" }
    elseif (@($invariantResults | Where-Object { $_.status -eq "fail" }).Count -gt 0 -or @($metricResults | Where-Object { $_.status -eq "fail" }).Count -gt 0) { $status = "fail" }
    elseif ($measurementDebtReasons.Count -gt 0 -or @($metricResults | Where-Object { $_.status -eq "debt" }).Count -gt 0) { $status = "debt" }

    $effectiveCandidateSha = if ($CandidateSha) { $CandidateSha } else { [string](Get-ObjectPropertyValue $Manifest "candidateSha" "") }
    $effectiveBaseSha = if ($BaseSha) { $BaseSha } else { [string](Get-ObjectPropertyValue $Manifest "baseSha" "") }
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        contractVersion = [string]$Slo.id
        runId = [string]$Manifest.runId
        profile = $profile
        mode = $Mode
        status = $status
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        candidateSha = $effectiveCandidateSha
        baseSha = $effectiveBaseSha
        environment = $Manifest.environment
        fixtures = @($Manifest.fixtures)
        artifacts = @((Get-ObjectPropertyValue $Manifest "artifacts" @()))
        suites = @($Suites)
        baseComparisonEvidence = [pscustomobject][ordered]@{
            supplied = $null -ne $BaseResult
            valid = $baseComparisonEligible
            reasons = $baseComparisonInfraReasons.ToArray()
        }
        metrics = $metricResults.ToArray()
        invariants = $invariantResults.ToArray()
        reasons = $resultReasons.ToArray()
    }
}

function Convert-YanamiTraceToManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TracePath,
        [Parameter(Mandatory = $true)][ValidateSet("PullRequest", "Lab", "Nightly", "Weekly", "Release")][string]$Profile,
        [Parameter(Mandatory = $true)][string]$RunId,
        [long]$ExpectedProcessId = -1,
        [ValidateSet("collect", "debt", "enforce")][string]$Mode = "collect",
        [string]$EnvironmentFingerprint = "runtime-smoke-unclassified",
        [bool]$ReferenceMatch = $false,
        [switch]$RequireBootstrapHandshake
    )

    if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) { throw "Performance trace was not produced: $TracePath" }
    $events = New-Object System.Collections.Generic.List[object]
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $TracePath -Encoding UTF8) {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $event = $line | ConvertFrom-Json } catch { throw "Invalid JSONL at ${TracePath}:${lineNumber}: $($_.Exception.Message)" }
        if ([string]$event.schemaVersion -ne "1.0") { throw "Unsupported PerfEvent schemaVersion at ${TracePath}:$lineNumber." }
        $events.Add($event)
    }
    if ($events.Count -eq 0) { throw "Performance trace is empty: $TracePath" }
    $byMilestone = @{}
    $milestoneCounts = @{}
    $monotonic = $true
    $previousNs = [long]-1
    foreach ($event in $events) {
        $timestamp = [long]$event.monotonicNs
        if ($timestamp -lt $previousNs) { $monotonic = $false }
        $previousNs = $timestamp
        $milestone = [string]$event.milestone
        if (-not $milestoneCounts.ContainsKey($milestone)) { $milestoneCounts[$milestone] = 0 }
        $milestoneCounts[$milestone]++
        if (-not $byMilestone.ContainsKey($milestone)) { $byMilestone[$milestone] = $event }
    }
    $requiredMilestones = @("main_entered", "qt_app_ready", "logger_ready", "backend_services_ready", "view_models_ready", "qml_root_ready", "window_exposed", "first_shell_present", "event_loop_ready", "startup_settled")
    if ($RequireBootstrapHandshake) { $requiredMilestones += "desktop_ready" }
    $orderValid = $monotonic
    $missing = New-Object System.Collections.Generic.List[string]
    $duplicate = New-Object System.Collections.Generic.List[string]
    $metadataErrors = New-Object System.Collections.Generic.List[string]
    $runIds = @($events | ForEach-Object { [string]$_.runId } | Select-Object -Unique)
    $processIds = @($events | ForEach-Object {
        if ($null -ne $_.PSObject.Properties["processId"]) { [string]$_.processId } else { "<missing>" }
    } | Select-Object -Unique)
    if ($runIds.Count -ne 1 -or $runIds[0] -ne $RunId) {
        $metadataErrors.Add("Trace must contain exactly the expected runId '$RunId'.")
        $orderValid = $false
    }
    if ($processIds.Count -ne 1 -or $processIds[0] -eq "<missing>") {
        $metadataErrors.Add("Trace must contain exactly one non-missing processId.")
        $orderValid = $false
    } elseif ($ExpectedProcessId -ge 0 -and [long]$processIds[0] -ne $ExpectedProcessId) {
        $metadataErrors.Add("Trace processId must equal the launched process '$ExpectedProcessId'.")
        $orderValid = $false
    }
    $traceIdentityValid = $orderValid
    foreach ($milestone in $requiredMilestones) {
        if (-not $byMilestone.ContainsKey($milestone)) {
            $missing.Add($milestone)
            $orderValid = $false
            continue
        }
        if ([int]$milestoneCounts[$milestone] -ne 1) {
            $duplicate.Add($milestone)
            $orderValid = $false
        }
        $event = $byMilestone[$milestone]
        if ([string]$event.scenarioId -ne "desktop.runtime") {
            $metadataErrors.Add("Milestone '$milestone' must use scenarioId 'desktop.runtime'.")
            $orderValid = $false
        }
        if ([string]$event.suite -ne "startup") {
            $metadataErrors.Add("Milestone '$milestone' must use suite 'startup'.")
            $orderValid = $false
        }
        if ([long]$event.generation -ne 0) {
            $metadataErrors.Add("Milestone '$milestone' must use generation 0.")
            $orderValid = $false
        }
    }
    $orderingEdges = @(
        @("main_entered", "qt_app_ready"),
        @("qt_app_ready", "logger_ready"),
        @("logger_ready", "backend_services_ready"),
        @("backend_services_ready", "view_models_ready"),
        @("view_models_ready", "qml_root_ready"),
        @("qml_root_ready", "event_loop_ready"),
        @("qml_root_ready", "window_exposed"),
        @("window_exposed", "first_shell_present"),
        @("first_shell_present", "startup_settled")
    )
    if ($RequireBootstrapHandshake) {
        $orderingEdges += ,@("first_shell_present", "desktop_ready")
        $orderingEdges += ,@("desktop_ready", "startup_settled")
        if ($byMilestone.ContainsKey("handoff_complete")) {
            $metadataErrors.Add("Desktop runtime trace must not emit 'handoff_complete'; that boundary belongs to the launcher after its handoff animation finishes.")
            $orderValid = $false
        }
    }
    foreach ($edge in $orderingEdges) {
        if (-not $byMilestone.ContainsKey($edge[0]) -or -not $byMilestone.ContainsKey($edge[1])) { continue }
        if ([long]$byMilestone[$edge[0]].monotonicNs -gt [long]$byMilestone[$edge[1]].monotonicNs) { $orderValid = $false }
    }
    $metrics = New-Object System.Collections.Generic.List[object]
    if ($byMilestone.ContainsKey("main_entered") -and $byMilestone.ContainsKey("first_shell_present")) {
        $metrics.Add([pscustomobject][ordered]@{
            id = "startup.internal.main_to_shell_candidate_ms"
            unit = "ms"
            samples = @(([long]$byMilestone["first_shell_present"].monotonicNs - [long]$byMilestone["main_entered"].monotonicNs) / 1000000.0)
            attributes = [pscustomobject]@{ evidence = "qt-frame-candidate"; strictPresent = $false }
        })
    }
    if ($byMilestone.ContainsKey("window_exposed") -and $byMilestone.ContainsKey("first_shell_present")) {
        $metrics.Add([pscustomobject][ordered]@{
            id = "startup.internal.exposed_to_shell_candidate_ms"
            unit = "ms"
            samples = @(([long]$byMilestone["first_shell_present"].monotonicNs - [long]$byMilestone["window_exposed"].monotonicNs) / 1000000.0)
            attributes = [pscustomobject]@{ evidence = "qt-frame-candidate"; strictPresent = $false }
        })
    }
    $invariants = New-Object System.Collections.Generic.List[object]
    $invariants.Add([pscustomobject][ordered]@{
        id = "startup.milestone_order_valid"
        passed = $orderValid
        details = [pscustomobject][ordered]@{
            missing = $missing.ToArray()
            duplicate = $duplicate.ToArray()
            metadataErrors = $metadataErrors.ToArray()
            runIds = $runIds
            processIds = $processIds
            monotonic = $monotonic
            eventCount = $events.Count
            bootstrapHandshakeRequired = [bool]$RequireBootstrapHandshake
            evidence = "internal-jsonl"
        }
    })
    $suites = New-Object System.Collections.Generic.List[string]
    $suites.Add("startup")
    $interactionEvents = @($events | Where-Object { [string]$_.milestone -like "interaction_*" })
    if ($interactionEvents.Count -gt 0) {
        $suites.Add("interaction")
        $interactionMetadataErrors = New-Object System.Collections.Generic.List[string]
        foreach ($event in $interactionEvents) {
            if ([string]$event.suite -ne "interaction") {
                $interactionMetadataErrors.Add("Interaction milestone '$($event.milestone)' must use suite 'interaction'.")
            }
            if ([string]$event.scenarioId -ne "desktop.runtime") {
                $interactionMetadataErrors.Add("Interaction milestone '$($event.milestone)' must use scenarioId 'desktop.runtime'.")
            }
        }
        $inputEvents = @($interactionEvents | Where-Object {
            [string]$_.milestone -eq "interaction_input_received" -and
            [bool](Get-ObjectPropertyValue $_.attributes "synthetic" $false)
        })
        $frameEvents = @($interactionEvents | Where-Object {
            [string]$_.milestone -eq "interaction_next_frame" -and
            [bool](Get-ObjectPropertyValue $_.attributes "synthetic" $false)
        })
        $completionEvents = @($interactionEvents | Where-Object { [string]$_.milestone -eq "interaction_probe_complete" })
        $interactionValid = $traceIdentityValid -and
            $interactionMetadataErrors.Count -eq 0 -and
            $inputEvents.Count -eq 1 -and
            $frameEvents.Count -eq 1 -and
            $completionEvents.Count -eq 1
        $inputGeneration = if ($inputEvents.Count -eq 1) { [long]$inputEvents[0].generation } else { [long]0 }
        $frameGeneration = if ($frameEvents.Count -eq 1) { [long]$frameEvents[0].generation } else { [long]0 }
        if ($inputGeneration -le 0 -or $inputGeneration -ne $frameGeneration) { $interactionValid = $false }
        if ($inputEvents.Count -eq 1 -and $frameEvents.Count -eq 1 -and
            [long]$inputEvents[0].monotonicNs -gt [long]$frameEvents[0].monotonicNs) {
            $interactionValid = $false
        }
        if ($frameEvents.Count -eq 1 -and $completionEvents.Count -eq 1 -and
            [long]$frameEvents[0].monotonicNs -gt [long]$completionEvents[0].monotonicNs) {
            $interactionValid = $false
        }
        $completionAttributes = if ($completionEvents.Count -eq 1) { $completionEvents[0].attributes } else { [pscustomobject]@{} }
        $syntheticSubmitted = [long](Get-ObjectPropertyValue $completionAttributes "syntheticSubmitted" -1)
        $syntheticPresented = [long](Get-ObjectPropertyValue $completionAttributes "syntheticPresented" -1)
        $syntheticDropped = [long](Get-ObjectPropertyValue $completionAttributes "syntheticDropped" -1)
        $syntheticPending = [long](Get-ObjectPropertyValue $completionAttributes "syntheticPending" -1)
        $dispatchCount = [long](Get-ObjectPropertyValue $completionAttributes "dispatchCount" -1)
        $longTaskCount = [long](Get-ObjectPropertyValue $completionAttributes "longTaskOver50Count" -1)
        $maxDispatchMs = [double](Get-ObjectPropertyValue $completionAttributes "maxDispatchMs" -1)
        $qpaPlatform = [string](Get-ObjectPropertyValue $completionAttributes "qpaPlatform" "")
        $quickBackendEnvironment = [string](Get-ObjectPropertyValue $completionAttributes "quickBackendEnvironment" "")
        $rendererApi = [long](Get-ObjectPropertyValue $completionAttributes "rendererApi" -1)
        $windowExposed = [bool](Get-ObjectPropertyValue $completionAttributes "windowExposed" $false)
        $inputModalityBefore = [long](Get-ObjectPropertyValue $completionAttributes "inputModalityBefore" -1)
        $inputModalityAfter = [long](Get-ObjectPropertyValue $completionAttributes "inputModalityAfter" -1)
        $inputModalityChanges = [long](Get-ObjectPropertyValue $completionAttributes "inputModalityChanges" -1)
        # QSGRendererInterface::GraphicsApi values accepted by this contract:
        # Software(1), OpenVG(2), OpenGL(3), D3D11(4), Vulkan(5), Metal(6),
        # and D3D12(8). Unknown(0), Null(7), and future/unreviewed values are
        # deliberately invalid evidence.
        $rendererApiValid = $rendererApi -in @(1, 2, 3, 4, 5, 6, 8)
        $hostedBackendValid = $Profile -ne "PullRequest" -or (
            [string]::Equals($qpaPlatform, "offscreen", [System.StringComparison]::OrdinalIgnoreCase) -and
            [string]::Equals($quickBackendEnvironment, "software", [System.StringComparison]::OrdinalIgnoreCase) -and
            $rendererApi -eq 1)
        if (-not $rendererApiValid) {
            $interactionMetadataErrors.Add("Interaction rendererApi '$rendererApi' is Unknown, Null, or outside the reviewed Qt renderer set.")
        }
        if (-not $hostedBackendValid) {
            $interactionMetadataErrors.Add("PullRequest interaction smoke must use qpaPlatform=offscreen, QT_QUICK_BACKEND=software, and rendererApi=Software(1).")
        }
        if ($syntheticSubmitted -ne 1 -or $syntheticPresented -ne 1 -or
            $syntheticDropped -ne 0 -or $syntheticPending -ne 0 -or
            $dispatchCount -lt 1 -or $longTaskCount -lt 0 -or $maxDispatchMs -lt 0 -or
            [string]::IsNullOrWhiteSpace($qpaPlatform) -or -not $rendererApiValid -or
            -not $hostedBackendValid -or
            -not $windowExposed -or $inputModalityBefore -lt 0 -or
            $inputModalityBefore -ne $inputModalityAfter -or
            $inputModalityChanges -ne 0) {
            $interactionValid = $false
        }
        $invariants.Add([pscustomobject][ordered]@{
            id = "interaction.measurement_hooks_valid"
            passed = $interactionValid
            details = [pscustomobject][ordered]@{
                inputEventCount = $inputEvents.Count
                frameEventCount = $frameEvents.Count
                completionEventCount = $completionEvents.Count
                inputGeneration = $inputGeneration
                frameGeneration = $frameGeneration
                syntheticSubmitted = $syntheticSubmitted
                syntheticPresented = $syntheticPresented
                syntheticDropped = $syntheticDropped
                syntheticPending = $syntheticPending
                dispatchCount = $dispatchCount
                qpaPlatform = $qpaPlatform
                quickBackendEnvironment = $quickBackendEnvironment
                rendererApi = $rendererApi
                rendererApiValid = $rendererApiValid
                hostedBackendValid = $hostedBackendValid
                windowExposed = $windowExposed
                inputModalityBefore = $inputModalityBefore
                inputModalityAfter = $inputModalityAfter
                inputModalityChanges = $inputModalityChanges
                metadataErrors = $interactionMetadataErrors.ToArray()
                evidence = "bounded-runtime-hook-smoke"
            }
        })
        if ($Profile -eq "PullRequest" -and $interactionValid) {
            $metrics.Add([pscustomobject][ordered]@{
                id = "interaction.long_task_over_50_count"
                unit = "count"
                samples = @($longTaskCount)
                attributes = [pscustomobject][ordered]@{
                    evidence = "qt-gui-top-level-dispatch"
                    scope = "bounded-runtime-hook-smoke"
                    dispatchCount = $dispatchCount
                    maxDispatchMs = $maxDispatchMs
                    qpaPlatform = $qpaPlatform
                    quickBackendEnvironment = $quickBackendEnvironment
                    rendererApi = $rendererApi
                    windowExposed = $windowExposed
                    inputModalityBefore = $inputModalityBefore
                    inputModalityAfter = $inputModalityAfter
                    inputModalityChanges = $inputModalityChanges
                }
            })
        }
    }
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        runId = $RunId
        profile = $Profile
        mode = $Mode
        startedAtUtc = [DateTime]::UtcNow.ToString("o")
        environment = [pscustomobject][ordered]@{ fingerprint = $EnvironmentFingerprint; referenceMatch = $ReferenceMatch; mismatchReasons = @("Qt frameSwapped and bootstrap ready-file signals are internal candidates; runtime JSONL does not prove an external compositor Present.") }
        fixtures = @()
        suites = $suites.ToArray()
        metrics = $metrics.ToArray()
        invariants = $invariants.ToArray()
    }
}

function Convert-YanamiBootstrapTraceToManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TracePath,
        [Parameter(Mandatory = $true)][ValidateSet("PullRequest", "Lab", "Nightly", "Weekly", "Release")][string]$Profile,
        [Parameter(Mandatory = $true)][string]$RunId,
        [long]$ExpectedProcessId = -1,
        [ValidateSet("collect", "debt", "enforce")][string]$Mode = "collect",
        [string]$EnvironmentFingerprint = "runtime-smoke-unclassified",
        [bool]$ReferenceMatch = $false
    )

    if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf)) {
        throw "Bootstrap performance sidecar was not produced: $TracePath"
    }
    $events = New-Object System.Collections.Generic.List[object]
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $TracePath -Encoding UTF8) {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $event = $line | ConvertFrom-Json }
        catch { throw "Invalid bootstrap JSONL at ${TracePath}:${lineNumber}: $($_.Exception.Message)" }
        if ([string]$event.schemaVersion -ne "1.0") {
            throw "Unsupported bootstrap PerfEvent schemaVersion at ${TracePath}:$lineNumber."
        }
        $events.Add($event)
    }
    if ($events.Count -eq 0) { throw "Bootstrap performance sidecar is empty: $TracePath" }

    $byMilestone = @{}
    $milestoneCounts = @{}
    $previousNs = [long]-1
    $monotonic = $true
    foreach ($event in $events) {
        $timestamp = [long]$event.monotonicNs
        if ($timestamp -lt $previousNs) { $monotonic = $false }
        $previousNs = $timestamp
        $milestone = [string]$event.milestone
        if (-not $milestoneCounts.ContainsKey($milestone)) {
            $milestoneCounts[$milestone] = 0
        }
        $milestoneCounts[$milestone]++
        if (-not $byMilestone.ContainsKey($milestone)) {
            $byMilestone[$milestone] = $event
        }
    }

    $requiredMilestones = @(
        "bootstrap_first_visible",
        "bootstrap_desktop_spawned",
        "desktop_ready",
        "handoff_complete"
    )
    $missing = New-Object System.Collections.Generic.List[string]
    $duplicate = New-Object System.Collections.Generic.List[string]
    $metadataErrors = New-Object System.Collections.Generic.List[string]
    $runIds = @($events | ForEach-Object { [string]$_.runId } | Select-Object -Unique)
    $processIds = @($events | ForEach-Object {
        if ($null -ne $_.PSObject.Properties["processId"]) {
            [string]$_.processId
        } else {
            "<missing>"
        }
    } | Select-Object -Unique)
    $valid = $monotonic
    if ($runIds.Count -ne 1 -or $runIds[0] -ne $RunId) {
        $metadataErrors.Add("Bootstrap sidecar must contain exactly the expected runId '$RunId'.")
        $valid = $false
    }
    if ($processIds.Count -ne 1 -or $processIds[0] -eq "<missing>") {
        $metadataErrors.Add("Bootstrap sidecar must contain exactly one launcher processId.")
        $valid = $false
    } elseif ($ExpectedProcessId -ge 0 -and [long]$processIds[0] -ne $ExpectedProcessId) {
        $metadataErrors.Add("Bootstrap sidecar processId must equal the launched process '$ExpectedProcessId'.")
        $valid = $false
    }
    foreach ($event in $events) {
        if ([string]$event.suite -ne "startup") {
            $metadataErrors.Add("Bootstrap milestone '$($event.milestone)' must use suite 'startup'.")
            $valid = $false
        }
        if ([string]$event.scenarioId -ne "desktop.bootstrap") {
            $metadataErrors.Add("Bootstrap milestone '$($event.milestone)' must use scenarioId 'desktop.bootstrap'.")
            $valid = $false
        }
        if ([long]$event.generation -ne 0) {
            $metadataErrors.Add("Bootstrap milestone '$($event.milestone)' must use generation 0.")
            $valid = $false
        }
    }
    foreach ($milestone in $requiredMilestones) {
        if (-not $byMilestone.ContainsKey($milestone)) {
            $missing.Add($milestone)
            $valid = $false
        } elseif ([int]$milestoneCounts[$milestone] -ne 1) {
            $duplicate.Add($milestone)
            $valid = $false
        }
    }
    foreach ($optionalMilestone in @("bootstrap_entered")) {
        if ($milestoneCounts.ContainsKey($optionalMilestone) -and
            [int]$milestoneCounts[$optionalMilestone] -ne 1) {
            $duplicate.Add($optionalMilestone)
            $valid = $false
        }
    }

    $orderingEdges = @(
        @("bootstrap_first_visible", "desktop_ready"),
        @("desktop_ready", "handoff_complete")
    )
    if ($byMilestone.ContainsKey("bootstrap_entered")) {
        $orderingEdges += ,@("bootstrap_entered", "bootstrap_first_visible")
    }
    if ($byMilestone.ContainsKey("bootstrap_desktop_spawned")) {
        $orderingEdges += ,@("bootstrap_first_visible", "bootstrap_desktop_spawned")
        $orderingEdges += ,@("bootstrap_desktop_spawned", "desktop_ready")
    }
    foreach ($edge in $orderingEdges) {
        if (-not $byMilestone.ContainsKey($edge[0]) -or
            -not $byMilestone.ContainsKey($edge[1])) { continue }
        if ([long]$byMilestone[$edge[0]].monotonicNs -gt
            [long]$byMilestone[$edge[1]].monotonicNs) {
            $valid = $false
        }
    }

    $firstVisibleAttributes = if ($byMilestone.ContainsKey("bootstrap_first_visible")) {
        $byMilestone["bootstrap_first_visible"].attributes
    } else { [pscustomobject]@{} }
    $spawnedAttributes = if ($byMilestone.ContainsKey("bootstrap_desktop_spawned")) {
        $byMilestone["bootstrap_desktop_spawned"].attributes
    } else { [pscustomobject]@{} }
    $desktopReadyAttributes = if ($byMilestone.ContainsKey("desktop_ready")) {
        $byMilestone["desktop_ready"].attributes
    } else { [pscustomobject]@{} }
    $handoffAttributes = if ($byMilestone.ContainsKey("handoff_complete")) {
        $byMilestone["handoff_complete"].attributes
    } else { [pscustomobject]@{} }

    $firstReadiness = Get-ObjectPropertyValue $firstVisibleAttributes "readiness" $null
    $desktopReadiness = Get-ObjectPropertyValue $desktopReadyAttributes "readiness" $null
    $handoffReadiness = Get-ObjectPropertyValue $handoffAttributes "readiness" $null
    $progressSemantic = [string](Get-ObjectPropertyValue $firstVisibleAttributes "progressSemantic" "")
    $childProcessId = [long]-1
    $desktopReadyChildProcessId = [long]-1
    $handoffChildProcessId = [long]-1
    $childProcessIdValid = [long]::TryParse(
        [string](Get-ObjectPropertyValue $spawnedAttributes "childProcessId" ""),
        [ref]$childProcessId) -and $childProcessId -gt 0
    $desktopReadyChildProcessIdValid = [long]::TryParse(
        [string](Get-ObjectPropertyValue $desktopReadyAttributes "childProcessId" ""),
        [ref]$desktopReadyChildProcessId) -and
        $desktopReadyChildProcessId -gt 0
    $handoffChildProcessIdValid = [long]::TryParse(
        [string](Get-ObjectPropertyValue $handoffAttributes "childProcessId" ""),
        [ref]$handoffChildProcessId) -and $handoffChildProcessId -gt 0
    $launcherProcessId = [long]-1
    $launcherProcessBindingValid =
        $processIds.Count -eq 1 -and
        $processIds[0] -ne "<missing>" -and
        [long]::TryParse([string]$processIds[0], [ref]$launcherProcessId) -and
        $launcherProcessId -gt 0 -and
        ($ExpectedProcessId -lt 0 -or $launcherProcessId -eq $ExpectedProcessId)
    $spawnedEvent = if ($byMilestone.ContainsKey("bootstrap_desktop_spawned")) {
        $byMilestone["bootstrap_desktop_spawned"]
    } else { $null }
    $spawnedMetadataBindingValid =
        $null -ne $spawnedEvent -and
        [string]$spawnedEvent.suite -eq "startup" -and
        [string]$spawnedEvent.scenarioId -eq "desktop.bootstrap" -and
        [long]$spawnedEvent.generation -eq 0
    $childProcessBindingValid =
        $runIds.Count -eq 1 -and $runIds[0] -eq $RunId -and
        $launcherProcessBindingValid -and
        $milestoneCounts.ContainsKey("bootstrap_desktop_spawned") -and
        [int]$milestoneCounts["bootstrap_desktop_spawned"] -eq 1 -and
        $spawnedMetadataBindingValid -and $childProcessIdValid
    $readinessSemanticsValid =
        $firstReadiness -is [bool] -and -not [bool]$firstReadiness -and
        $desktopReadiness -is [bool] -and [bool]$desktopReadiness -and
        $handoffReadiness -is [bool] -and [bool]$handoffReadiness -and
        $progressSemantic -eq "indeterminate" -and
        $childProcessIdValid -and $desktopReadyChildProcessIdValid -and
        $handoffChildProcessIdValid -and
        $childProcessId -eq $desktopReadyChildProcessId -and
        $childProcessId -eq $handoffChildProcessId
    if (-not $readinessSemanticsValid) { $valid = $false }

    $falseProgressFields = New-Object System.Collections.Generic.List[string]
    foreach ($event in $events) {
        foreach ($property in @($event.attributes.PSObject.Properties)) {
            if ($property.Name -match '(?i)(percent(age)?|progress(percent|percentage|value|ratio))' -or
                ($property.Value -is [string] -and [string]$property.Value -match '%')) {
                $falseProgressFields.Add("$($event.milestone).$($property.Name)")
            }
        }
    }
    $noFalseProgress = $progressSemantic -eq "indeterminate" -and
        $falseProgressFields.Count -eq 0

    $metrics = New-Object System.Collections.Generic.List[object]
    function Add-BootstrapDurationMetric {
        param([string]$Id, [string]$Start, [string]$End, [string]$Boundary)
        if (-not $byMilestone.ContainsKey($Start) -or
            -not $byMilestone.ContainsKey($End)) { return }
        $durationNs = [long]$byMilestone[$End].monotonicNs -
            [long]$byMilestone[$Start].monotonicNs
        if ($durationNs -lt 0) { return }
        $metrics.Add([pscustomobject][ordered]@{
            id = $Id
            unit = "ms"
            samples = @($durationNs / 1000000.0)
            attributes = [pscustomobject][ordered]@{
                evidence = "launcher-internal-clock"
                strictPresent = $false
                boundary = $Boundary
            }
        })
    }
    Add-BootstrapDurationMetric -Id "startup.internal.bootstrap_visible_to_desktop_ready_candidate_ms" -Start "bootstrap_first_visible" -End "desktop_ready" -Boundary "content-ready-candidate"
    Add-BootstrapDurationMetric -Id "startup.internal.desktop_ready_to_handoff_animation_ms" -Start "desktop_ready" -End "handoff_complete" -Boundary "handoff-animation-only"
    Add-BootstrapDurationMetric -Id "startup.internal.bootstrap_visible_to_handoff_ms" -Start "bootstrap_first_visible" -End "handoff_complete" -Boundary "diagnostic-total-not-ttfp"
    Add-BootstrapDurationMetric -Id "startup.internal.bootstrap_entered_to_first_visible_candidate_ms" -Start "bootstrap_entered" -End "bootstrap_first_visible" -Boundary "launcher-first-visible-candidate"

    $invariants = @(
        [pscustomobject][ordered]@{
            id = "startup.bootstrap_handoff_valid"
            passed = $valid
            details = [pscustomobject][ordered]@{
                missing = $missing.ToArray()
                duplicate = $duplicate.ToArray()
                metadataErrors = $metadataErrors.ToArray()
                runIds = $runIds
                processIds = $processIds
                monotonic = $monotonic
                eventCount = $events.Count
                childProcessId = $childProcessId
                childProcessBindingValid = $childProcessBindingValid
                desktopReadyChildProcessId = $desktopReadyChildProcessId
                handoffChildProcessId = $handoffChildProcessId
                readinessSemanticsValid = $readinessSemanticsValid
                desktopReadyIsContentBoundary = $true
                handoffIsAnimationBoundary = $true
                strictExternalPresent = $false
                evidence = "bootstrap-ready-file-sidecar"
            }
        },
        [pscustomobject][ordered]@{
            id = "startup.no_false_progress_indicator"
            passed = $noFalseProgress
            details = [pscustomobject][ordered]@{
                progressSemantic = $progressSemantic
                forbiddenFields = $falseProgressFields.ToArray()
                bootstrapVisibleIsReadiness = $false
                evidence = "bootstrap-ready-file-sidecar"
            }
        }
    )
    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        # The sidecar and desktop JSONL intentionally share the external trace
        # runId, but merge provenance requires each producer manifest to keep a
        # distinct identity.
        runId = "$RunId-bootstrap"
        profile = $Profile
        mode = $Mode
        startedAtUtc = [DateTime]::UtcNow.ToString("o")
        environment = [pscustomobject][ordered]@{
            fingerprint = $EnvironmentFingerprint
            referenceMatch = $ReferenceMatch
            mismatchReasons = @(
                "Bootstrap visibility and desktop ready-file events are internal candidates, not DWM, WindowServer, or compositor Present evidence.",
                "SmartScreen, Gatekeeper, quarantine, download, extraction, and installer delays are outside this process trace."
            )
        }
        fixtures = @()
        suites = @("startup")
        metrics = $metrics.ToArray()
        invariants = $invariants
    }
}

function New-YanamiUnavailableResult {
    [CmdletBinding()]
    param(
        [ValidateSet("PullRequest", "Lab", "Nightly", "Weekly", "Release")][string]$Profile,
        [ValidateSet("debt", "infra-invalid")][string]$Status,
        [string[]]$Suites,
        [string]$Reason,
        [string]$Mode,
        [string]$CandidateSha,
        [string]$BaseSha
    )

    if (-not $Suites -or $Suites.Count -eq 0) { $Suites = @(Get-YanamiPerfSuites) }

    return [pscustomobject][ordered]@{
        schemaVersion = "1.0"
        contractVersion = "SLO-v1"
        runId = "unavailable-$([guid]::NewGuid().ToString('N'))"
        profile = $Profile
        mode = $Mode
        status = $Status
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        candidateSha = $CandidateSha
        baseSha = $BaseSha
        environment = [pscustomobject]@{ fingerprint = "unavailable"; referenceMatch = $false }
        fixtures = @()
        suites = @($Suites)
        metrics = @()
        invariants = @()
        reasons = @($Reason)
    }
}

function Write-YanamiJUnit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Result,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $document = New-Object System.Xml.XmlDocument
    $suite = $document.CreateElement("testsuite")
    [void]$document.AppendChild($suite)
    $cases = @($Result.metrics).Count + @($Result.invariants).Count
    if ($cases -eq 0) { $cases = 1 }
    $failures = @($Result.metrics | Where-Object { $_.status -eq "fail" }).Count + @($Result.invariants | Where-Object { $_.status -eq "fail" }).Count
    $errors = if ($Result.status -eq "infra-invalid") { 1 } else { 0 }
    $skipped = @($Result.metrics | Where-Object { $_.status -eq "debt" }).Count
    $suite.SetAttribute("name", "Yanami.Performance.$($Result.profile)")
    $suite.SetAttribute("tests", [string]$cases)
    $suite.SetAttribute("failures", [string]$failures)
    $suite.SetAttribute("errors", [string]$errors)
    $suite.SetAttribute("skipped", [string]$skipped)

    foreach ($metric in @($Result.metrics)) {
        $case = $document.CreateElement("testcase")
        $case.SetAttribute("classname", "performance.$($metric.suite)")
        $case.SetAttribute("name", [string]$metric.id)
        if ($metric.status -eq "fail") {
            $node = $document.CreateElement("failure"); $node.SetAttribute("message", (@($metric.reasons) -join " ")); $node.InnerText = ($metric | ConvertTo-Json -Depth 20); [void]$case.AppendChild($node)
        } elseif ($metric.status -eq "debt") {
            $node = $document.CreateElement("skipped"); $node.SetAttribute("message", "Performance debt during calibration."); $node.InnerText = (@($metric.reasons) -join " "); [void]$case.AppendChild($node)
        }
        [void]$suite.AppendChild($case)
    }
    foreach ($invariant in @($Result.invariants)) {
        $case = $document.CreateElement("testcase")
        $case.SetAttribute("classname", "performance.invariant")
        $case.SetAttribute("name", [string]$invariant.id)
        if ($invariant.status -eq "fail") {
            $node = $document.CreateElement("failure"); $node.SetAttribute("message", "Correctness invariant failed."); $node.InnerText = ($invariant | ConvertTo-Json -Depth 20); [void]$case.AppendChild($node)
        }
        [void]$suite.AppendChild($case)
    }
    if (@($Result.metrics).Count + @($Result.invariants).Count -eq 0) {
        $case = $document.CreateElement("testcase"); $case.SetAttribute("classname", "performance.contract"); $case.SetAttribute("name", "probe-availability")
        if ($Result.status -eq "infra-invalid") { $node = $document.CreateElement("error"); $node.SetAttribute("message", (@($Result.reasons) -join " ")); [void]$case.AppendChild($node) }
        elseif ($Result.status -eq "debt") { $node = $document.CreateElement("skipped"); $node.SetAttribute("message", (@($Result.reasons) -join " ")); [void]$case.AppendChild($node) }
        [void]$suite.AppendChild($case)
    }
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) { [void](New-Item -ItemType Directory -Path $directory) }
    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Indent = $true
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.Xml.XmlWriter]::Create($Path, $settings)
    try { $document.Save($writer) } finally { $writer.Dispose() }
}

Export-ModuleMember -Function @(
    "Read-YanamiPerfJson",
    "Get-YanamiPerfSuites",
    "Get-YanamiRequiredFixtureIds",
    "Get-YanamiPercentile",
    "Get-YanamiStatistics",
    "Test-YanamiPerfConfiguration",
    "Merge-YanamiRunManifests",
    "Invoke-YanamiPerfEvaluation",
    "Convert-YanamiTraceToManifest",
    "Convert-YanamiBootstrapTraceToManifest",
    "New-YanamiUnavailableResult",
    "Write-YanamiJUnit"
)
