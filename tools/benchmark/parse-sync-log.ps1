param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [ValidateSet("all", "startup", "paused-seek", "steady")]
    [string]$Scenario = "all",
    [int]$Runs = 1,
    [int]$Warmups = 0
)

$ErrorActionPreference = "Stop"

if ($Runs -le 0 -or $Warmups -lt 0) {
    throw "Runs must be positive; Warmups must be non-negative"
}

$resolvedLog = (Resolve-Path $LogPath -ErrorAction Stop).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $resolvedOutput
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$definitions = @()
if ($Scenario -eq "all" -or $Scenario -eq "startup") {
    $definitions += [pscustomobject]@{
        scenario = "startup"
        seek_fraction = ""
    }
}
if ($Scenario -eq "all" -or $Scenario -eq "paused-seek") {
    foreach ($fraction in @("0.250", "0.500", "0.750")) {
        $definitions += [pscustomobject]@{
            scenario = "paused_seek"
            seek_fraction = $fraction
        }
    }
}
if ($Scenario -eq "all" -or $Scenario -eq "steady") {
    $definitions += [pscustomobject]@{
        scenario = "steady_playback"
        seek_fraction = ""
    }
}

$expectedSessions = ($Runs + $Warmups) * $definitions.Count
$metricLines = @(
    Get-Content -LiteralPath $resolvedLog -Encoding UTF8 |
        Where-Object { $_ -match "\[video_sync_metrics\].*presentation stats reason=unconfigure" }
)
if ($metricLines.Count -ne $expectedSessions) {
    throw "Expected $expectedSessions video sync sessions in $resolvedLog, found $($metricLines.Count)"
}

function Get-LogFields([string]$Line) {
    $fields = @{}
    foreach ($match in [regex]::Matches(
        $Line,
        "(?<key>[A-Za-z_]+)=(?<value>-?[0-9]+(?:\.[0-9]+)?)")) {
        $fields[$match.Groups["key"].Value] = $match.Groups["value"].Value
    }
    return $fields
}

$requiredFields = @(
    "elapsed_ms",
    "fps",
    "popped",
    "presented",
    "catchup_dropped",
    "stale_dropped",
    "empty_pop",
    "audio_clock_unavailable",
    "wait_events",
    "wait_target_avg_us",
    "wait_target_max_us",
    "wait_overshoot_avg_us",
    "wait_overshoot_max_us",
    "presented_late",
    "presented_lateness_avg_us",
    "presented_lateness_max_us",
    "wakeup_events",
    "wakeup_error_avg_us",
    "wakeup_late_max_us",
    "wakeup_early_max_us",
    "wakeup_compensation_us",
    "busy_wait_avg_us",
    "busy_wait_max_us",
    "callback_avg_us",
    "callback_max_us"
)

$rows = @()
$lineIndex = 0
$sessionIndex = 0
foreach ($definition in $definitions) {
    for ($iteration = 1; $iteration -le ($Warmups + $Runs); ++$iteration) {
        $line = $metricLines[$lineIndex]
        ++$lineIndex
        ++$sessionIndex

        $fields = Get-LogFields $line
        foreach ($requiredField in $requiredFields) {
            if (-not $fields.ContainsKey($requiredField)) {
                throw "Missing '$requiredField' in video sync log session $sessionIndex"
            }
        }

        $timestampMatch = [regex]::Match(
            $line,
            "^(?<timestamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})")
        $isWarmup = $iteration -le $Warmups
        $measuredRun = if ($isWarmup) { 0 } else { $iteration - $Warmups }

        $rows += [pscustomobject][ordered]@{
            session_index = $sessionIndex
            log_timestamp = if ($timestampMatch.Success) {
                $timestampMatch.Groups["timestamp"].Value
            } else {
                ""
            }
            scenario = $definition.scenario
            run = $measuredRun
            warmup = if ($isWarmup) { 1 } else { 0 }
            seek_fraction = $definition.seek_fraction
            elapsed_ms = $fields["elapsed_ms"]
            fps = $fields["fps"]
            popped = $fields["popped"]
            presented = $fields["presented"]
            catchup_dropped = $fields["catchup_dropped"]
            stale_dropped = $fields["stale_dropped"]
            empty_pop = $fields["empty_pop"]
            audio_clock_unavailable = $fields["audio_clock_unavailable"]
            wait_events = $fields["wait_events"]
            wait_target_avg_us = $fields["wait_target_avg_us"]
            wait_target_max_us = $fields["wait_target_max_us"]
            wait_overshoot_avg_us = $fields["wait_overshoot_avg_us"]
            wait_overshoot_max_us = $fields["wait_overshoot_max_us"]
            presented_late = $fields["presented_late"]
            presented_lateness_avg_us = $fields["presented_lateness_avg_us"]
            presented_lateness_max_us = $fields["presented_lateness_max_us"]
            wakeup_events = $fields["wakeup_events"]
            wakeup_error_avg_us = $fields["wakeup_error_avg_us"]
            wakeup_late_max_us = $fields["wakeup_late_max_us"]
            wakeup_early_max_us = $fields["wakeup_early_max_us"]
            wakeup_compensation_us = $fields["wakeup_compensation_us"]
            busy_wait_avg_us = $fields["busy_wait_avg_us"]
            busy_wait_max_us = $fields["busy_wait_max_us"]
            callback_avg_us = $fields["callback_avg_us"]
            callback_max_us = $fields["callback_max_us"]
        }
    }
}

$rows | Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
Write-Output "Sync telemetry: $resolvedOutput"
