[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateRange(1, 1000000)]
    [int]$TitleCount = 10000,

    [ValidateRange(1, 5000000)]
    [int]$EpisodeCount = 100000,

    [ValidateRange(1, 1000000)]
    [int]$QueryCount = 20000,

    [int]$Seed = 20260823,

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

function ConvertTo-FullWidth {
    param([Parameter(Mandatory = $true)][string]$Value)

    $builder = [System.Text.StringBuilder]::new($Value.Length)
    foreach ($character in $Value.ToCharArray()) {
        $codePoint = [int]$character
        if ($codePoint -eq 0x20) {
            [void]$builder.Append([char]0x3000)
        } elseif ($codePoint -ge 0x21 -and $codePoint -le 0x7e) {
            [void]$builder.Append([char]($codePoint + 0xfee0))
        } else {
            [void]$builder.Append($character)
        }
    }
    return $builder.ToString()
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $resolvedOutput)) {
    [void](New-Item -ItemType Directory -Path $resolvedOutput)
}

$itemsPath = Join-Path $resolvedOutput "f110k-items.jsonl"
$queriesPath = Join-Path $resolvedOutput "search-queries-v1.jsonl"
$infoPath = Join-Path $resolvedOutput "fixture-info.json"
foreach ($path in @($itemsPath, $queriesPath, $infoPath)) {
    if ((Test-Path -LiteralPath $path) -and -not $Force) {
        throw "Output already exists: $path. Pass -Force to replace this generated fixture."
    }
}

$titlePatterns = @(
    @{ title = "星河档案"; pinyin = "xinghedangan"; initials = "xhda" },
    @{ title = "午夜食堂"; pinyin = "wuyeshitang"; initials = "wyst" },
    @{ title = "山海旅人"; pinyin = "shanhailvren"; initials = "shlr" },
    @{ title = "The Archive"; pinyin = "thearchive"; initials = "ta" },
    @{ title = "銀河鉄道"; pinyin = "yinhetiedao"; initials = "yhtd" },
    @{ title = "Café Archive"; pinyin = "cafearchive"; initials = "ca" }
)

$itemsWriter = New-Utf8Writer -Path $itemsPath
try {
    for ($i = 1; $i -le $TitleCount; $i++) {
        $pattern = $titlePatterns[($i + $Seed) % $titlePatterns.Count]
        $kind = if (($i % 4) -eq 0) { "Movie" } else { "Series" }
        $id = "title-{0:d6}" -f $i
        $ordinal = "{0:d6}" -f $i
        $line = '{{"id":"{0}","kind":"{1}","title":"{2} {3}","aliases":["{2} {3}","{4}{3}","{5}{3}"],"ordinal":{6}}}' -f $id, $kind, $pattern.title, $ordinal, $pattern.pinyin, $pattern.initials, $i
        $itemsWriter.WriteLine($line)
    }

    $seriesCount = $TitleCount - [math]::Floor($TitleCount / 4)
    for ($i = 1; $i -le $EpisodeCount; $i++) {
        $seriesPosition = (($i - 1) % $seriesCount) + 1
        $parentOrdinal = [int](([math]::Floor(($seriesPosition - 1) / 3) * 4) + (($seriesPosition - 1) % 3) + 1)
        $parentId = "title-{0:d6}" -f $parentOrdinal
        $id = "episode-{0:d7}" -f $i
        $occurrenceWithinSeries = [int][math]::Floor(($i - 1) / $seriesCount)
        # Every series/season/episode tuple is unique. Four episodes per
        # synthetic season provide multiple seasons in the balanced 110K
        # corpus without cycling back to an ambiguous key.
        $season = [int]([math]::Floor($occurrenceWithinSeries / 4) + 1)
        $episode = [int](($occurrenceWithinSeries % 4) + 1)
        $nameKind = ($i + $Seed) % 4
        $episodeName = @("归航", "新的旅程", "风暴之后", "Final Signal")[$nameKind]
        $episodePinyin = @("guihang", "xindelvcheng", "fengbaozhihou", "finalsignal")[$nameKind]
        $line = '{{"id":"{0}","kind":"Episode","title":"第{1}集 {2}","aliases":["S{3:d2}E{1:d2}","{4}"],"seriesId":"{5}","season":{3},"episode":{1},"ordinal":{6}}}' -f $id, $episode, $episodeName, $season, $episodePinyin, $parentId, $i
        $itemsWriter.WriteLine($line)
    }
}
finally {
    $itemsWriter.Dispose()
}

$queryKinds = @("exact", "prefix", "substring", "case-fold", "full-width", "unicode-normalized", "pinyin-full", "pinyin-initials", "season", "episode-number", "one-han", "two-han", "no-result", "backspace", "middle-insert", "paste", "rapid-rewrite")
$seriesCount = $TitleCount - [math]::Floor($TitleCount / 4)
$queriesWriter = New-Utf8Writer -Path $queriesPath
try {
    for ($i = 1; $i -le $QueryCount; $i++) {
        $targetOrdinal = ((($i * 7919) + $Seed) % $TitleCount) + 1
        $pattern = $titlePatterns[($targetOrdinal + $Seed) % $titlePatterns.Count]
        $ordinal = "{0:d6}" -f $targetOrdinal
        $targetId = "title-{0}" -f $ordinal
        $kind = $queryKinds[($i - 1) % $queryKinds.Count]
        $episodeOrdinal = ((($i * 3571) + $Seed) % $EpisodeCount) + 1
        $seriesPosition = (($episodeOrdinal - 1) % $seriesCount) + 1
        $episodeParentOrdinal = [int](([math]::Floor(($seriesPosition - 1) / 3) * 4) + (($seriesPosition - 1) % 3) + 1)
        $episodeParentText = "{0:d6}" -f $episodeParentOrdinal
        $episodeParentId = "title-{0}" -f $episodeParentText
        $episodeParentPattern = $titlePatterns[($episodeParentOrdinal + $Seed) % $titlePatterns.Count]
        $episodeOccurrenceWithinSeries = [int][math]::Floor(
            ($episodeOrdinal - 1) / $seriesCount)
        $seasonNumber = [int]([math]::Floor($episodeOccurrenceWithinSeries / 4) + 1)
        $episodeNumber = [int](($episodeOccurrenceWithinSeries % 4) + 1)
        $episodeId = "episode-{0:d7}" -f $episodeOrdinal
        $unicodeTargetOrdinal = $targetOrdinal
        do {
            $unicodeTargetPattern = $titlePatterns[($unicodeTargetOrdinal + $Seed) % $titlePatterns.Count]
            if ($unicodeTargetPattern.title -eq "Café Archive") { break }
            $unicodeTargetOrdinal = ($unicodeTargetOrdinal % $TitleCount) + 1
        } while ($unicodeTargetOrdinal -ne $targetOrdinal)
        $unicodeOrdinal = "{0:d6}" -f $unicodeTargetOrdinal
        $unicodeTargetId = "title-{0}" -f $unicodeOrdinal
        $expectedRank1 = $targetId
        $query = switch ($kind) {
            "exact" { "$($pattern.title) $ordinal" }
            "prefix" { $pattern.title.Substring(0, [math]::Min(2, $pattern.title.Length)) }
            "substring" { if ($pattern.title.Length -gt 2) { $pattern.title.Substring(1, 2) } else { $pattern.title } }
            "case-fold" { ("$($pattern.title) $ordinal").ToUpperInvariant() }
            "full-width" { ConvertTo-FullWidth "$($pattern.pinyin)$ordinal" }
            "unicode-normalized" {
                $expectedRank1 = $unicodeTargetId
                "Cafe$([char]0x0301) Archive $unicodeOrdinal"
            }
            "pinyin-full" { "$($pattern.pinyin)$ordinal" }
            "pinyin-initials" { "$($pattern.initials)$ordinal" }
            "season" { "$($episodeParentPattern.title) $episodeParentText 第$($seasonNumber)季" }
            "episode-number" { "$($episodeParentPattern.title) $episodeParentText S$('{0:d2}' -f $seasonNumber)E$('{0:d2}' -f $episodeNumber)" }
            "one-han" { "星" }
            "two-han" { "午夜" }
            "no-result" { "不存在-$ordinal" }
            "backspace" { "$($pattern.title) $($ordinal.Substring(0, 5))" }
            "middle-insert" { "$($pattern.title) $ordinal" }
            "paste" { "$($pattern.title) $ordinal" }
            default { "$($pattern.initials)$ordinal" }
        }
        $expectation = if ($kind -in @("exact", "case-fold", "full-width", "unicode-normalized", "pinyin-full", "pinyin-initials", "middle-insert", "paste", "rapid-rewrite")) {
            '{{"rank1":"{0}"}}' -f $expectedRank1
        } elseif ($kind -eq "season") {
            '{{"rank1":"season:{0}:{1}","kind":"Season","seriesId":"{0}","season":{1}}}' -f $episodeParentId, $seasonNumber
        } elseif ($kind -eq "episode-number") {
            '{{"rank1":"{0}","kind":"Episode","seriesId":"{1}","season":{2},"episode":{3}}}' -f $episodeId, $episodeParentId, $seasonNumber, $episodeNumber
        } elseif ($kind -eq "no-result") {
            '{"matchCount":0}'
        } else {
            '{"oracle":"scan-normalized-fixture"}'
        }
        $line = '{{"id":"query-{0:d6}","category":"{1}","query":"{2}","imeCommitted":true,"expectation":{3}}}' -f $i, $kind, $query, $expectation
        $queriesWriter.WriteLine($line)
    }
}
finally {
    $queriesWriter.Dispose()
}

$info = [ordered]@{
    schemaVersion = "1.0"
    fixtureId = "F110K-v1"
    generatorVersion = "1.3"
    seed = $Seed
    titleCount = $TitleCount
    episodeCount = $EpisodeCount
    totalItemCount = $TitleCount + $EpisodeCount
    queryCount = $QueryCount
    lineEndings = "LF"
    encoding = "UTF-8-no-BOM"
    files = @(
        [ordered]@{ name = [System.IO.Path]::GetFileName($itemsPath); sha256 = Get-FileSha256 -Path $itemsPath },
        [ordered]@{ name = [System.IO.Path]::GetFileName($queriesPath); sha256 = Get-FileSha256 -Path $queriesPath }
    )
}
$info | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $infoPath -Encoding utf8

Write-Host "Generated $($info.totalItemCount) F110K items and $QueryCount search queries in $resolvedOutput"
Write-Output $info
