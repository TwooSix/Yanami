[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateRange(1, 1000000)]
    [int]$CommentCount = 100000,

    [ValidateRange(901, 86400)]
    [int]$DurationSeconds = 1800,

    [int]$Seed = 20260824,

    [switch]$Force
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function New-Utf8Writer {
    param([Parameter(Mandatory = $true)][string]$Path)

    $encoding = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($Path, $false, $encoding)
    $writer.NewLine = "`n"
    return $writer
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $resolvedOutput)) {
    [void](New-Item -ItemType Directory -Path $resolvedOutput)
}

$commentsPath = Join-Path $resolvedOutput "danmaku-comments-v1.jsonl"
$infoPath = Join-Path $resolvedOutput "fixture-info.json"
foreach ($path in @($commentsPath, $infoPath)) {
    if ((Test-Path -LiteralPath $path) -and -not $Force) {
        throw "Output already exists: $path. Pass -Force to replace this generated fixture."
    }
}

$modeCounts = [ordered]@{ scroll = 0; top = 0; bottom = 0 }
$textClassCounts = [ordered]@{
    cjk = 0
    ascii = 0
    japanese = 0
    emoji = 0
    blocked = 0
    long = 0
}
$colors = @(0xffffff, 0xff7aa2, 0x64d8ff, 0xffd166, 0xa7f3d0)
$perSecondCounts = [int[]]::new($DurationSeconds)
$writer = New-Utf8Writer -Path $commentsPath
try {
    for ($index = 0; $index -lt $CommentCount; ++$index) {
        $ordinal = $index + 1
        $modeSelector = ($index + $Seed) % 10
        $mode = if ($modeSelector -eq 0) {
            "top"
        } elseif ($modeSelector -eq 1) {
            "bottom"
        } else {
            "scroll"
        }
        $modeCounts[$mode]++

        # The first 20K records are a deterministic hosted-runner slice and
        # deliberately contain both dense windows. The complete 100K corpus
        # is used by the fixed 4K60 lab. File order is intentionally not time
        # order so a probe must exercise the product's timeline preparation.
        if ($index -lt 7200) {
            $time = 600.0 + (($index % 7200) / 120.0)
        } elseif ($index -lt 7440) {
            $time = 900.0 + (($index - 7200) / 240.0)
        } else {
            $timeTicks = (($index * 7919L) + $Seed) % ($DurationSeconds * 100L)
            $time = $timeTicks / 100.0
        }
        $perSecondCounts[[int][math]::Floor($time)]++

        $textSelector = ($index + $Seed) % 6
        $textClass = switch ($textSelector) {
            0 { "cjk" }
            1 { "ascii" }
            2 { "japanese" }
            3 { "emoji" }
            4 { "blocked" }
            default { "long" }
        }
        $text = switch ($textClass) {
            "cjk" { "海风吹过第 $ordinal 次" }
            "ascii" { "Real-time comment $ordinal" }
            "japanese" { "銀河鉄道の夜 $ordinal" }
            "emoji" { "弹幕性能 🚀 $ordinal" }
            "blocked" { "屏蔽样本 $ordinal" }
            default { "长文本$("内容" * 24) $ordinal" }
        }
        $textClassCounts[$textClass]++
        $color = $colors[($index * 17 + $Seed) % $colors.Count]
        $timeText = $time.ToString(
            "0.0000", [System.Globalization.CultureInfo]::InvariantCulture)
        $line = '{{"id":"dm-{0:d6}","time":{1},"mode":"{2}","color":{3},"text":"{4}"}}' -f `
            $ordinal, $timeText, $mode, $color, $text
        $writer.WriteLine($line)
    }
}
finally {
    $writer.Dispose()
}

$commentsSha256 = Get-FileSha256 -Path $commentsPath
$sustainedRates = @(600..659 | ForEach-Object { $perSecondCounts[$_] } | Sort-Object)
$peakRate = $perSecondCounts[900]
$info = [ordered]@{
    schemaVersion = "1.0"
    fixtureId = "DanmakuDensity-v1"
    generatorVersion = "1.0"
    seed = $Seed
    commentCount = $CommentCount
    durationSeconds = $DurationSeconds
    hostedSliceCount = [math]::Min(20000, $CommentCount)
    modeCounts = $modeCounts
    textClassCounts = $textClassCounts
    burstWindows = @(
        [ordered]@{
            id = "sustained-120-per-second"
            startSeconds = 600
            durationSeconds = 60
            injectedCommentsPerSecond = 120
            commentCount = [math]::Min(7200, $CommentCount)
            observedCommentsPerSecond = [ordered]@{
                minimum = $sustainedRates[0]
                median = $sustainedRates[[int][math]::Floor(($sustainedRates.Count - 1) / 2)]
                maximum = $sustainedRates[-1]
            }
        },
        [ordered]@{
            id = "peak-240-per-second"
            startSeconds = 900
            durationSeconds = 1
            injectedCommentsPerSecond = 240
            commentCount = [math]::Max(0, [math]::Min(240, $CommentCount - 7200))
            observedCommentsPerSecond = $peakRate
        }
    )
    lineEndings = "LF"
    encoding = "UTF-8-no-BOM"
    files = @(
        [ordered]@{
            name = [System.IO.Path]::GetFileName($commentsPath)
            sha256 = $commentsSha256
        }
    )
    fixtureSha256 = $commentsSha256
}
$info | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $infoPath -Encoding utf8

Write-Host "Generated $CommentCount deterministic danmaku comments in $resolvedOutput"
Write-Output $info
