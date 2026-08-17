param(
    [Parameter(Mandatory = $true)]
    [string]$CsvPath,
    [string]$SyncCsvPath
)

$ErrorActionPreference = "Stop"

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $rank = [math]::Ceiling($Percentile * $sorted.Count) - 1
    $rank = [math]::Max(0, [math]::Min($rank, $sorted.Count - 1))
    return [double]$sorted[$rank]
}

function Get-Stats([object[]]$Rows, [string]$Property) {
    $values = @($Rows | ForEach-Object { [double]($_.$Property) })
    if ($values.Count -eq 0) {
        return $null
    }
    return [pscustomobject]@{
        samples = $values.Count
        median = Get-Percentile $values 0.50
        p95 = Get-Percentile $values 0.95
        minimum = [double]($values | Measure-Object -Minimum).Minimum
        maximum = [double]($values | Measure-Object -Maximum).Maximum
    }
}

$rows = Import-Csv $CsvPath
$labelledRows = $rows | ForEach-Object {
    $label = if ($_.scenario -eq "paused_seek") {
        "paused_seek@$($_.seek_fraction)"
    } else {
        $_.scenario
    }
    [pscustomobject]@{
        label = $label
        row = $_
    }
}
$groups = $labelledRows | Group-Object -Property label

Write-Output "| Scenario | Samples | Metric median | P95 |"
Write-Output "|---|---:|---:|---:|"

foreach ($group in $groups) {
    $values = switch -Regex ($group.Name) {
        "^startup$" { @($group.Group.row.open_to_first_frame_ms | ForEach-Object { [double]$_ }); break }
        "^paused_seek@" { @($group.Group.row.seek_to_first_frame_ms | ForEach-Object { [double]$_ }); break }
        "^steady_playback$" { @($group.Group.row.cpu_average_percent | ForEach-Object { [double]$_ }); break }
    }
    $metric = switch -Regex ($group.Name) {
        "^startup$" { "open_to_first_frame_ms"; break }
        "^paused_seek@" { "seek_to_first_frame_ms"; break }
        "^steady_playback$" { "cpu_average_percent"; break }
    }
    $median = Get-Percentile $values 0.50
    $p95 = Get-Percentile $values 0.95
    Write-Output ("| {0} ({1}) | {2} | {3:N3} | {4:N3} |" -f `
        $group.Name, $metric, $values.Count, $median, $p95)
    if ($group.Name -eq "steady_playback") {
        [double[]]$frameRateValues = @($group.Group.row | ForEach-Object {
            [double]$_.frames / [double]$_.elapsed_ms * 1000.0
        })
        $frameRateMedian = Get-Percentile $frameRateValues 0.50
        $frameRateP95 = Get-Percentile $frameRateValues 0.95
        Write-Output ("| {0} ({1}) | {2} | {3:N3} | {4:N3} |" -f `
            $group.Name, "callback_fps", $frameRateValues.Count,
            $frameRateMedian, $frameRateP95)

        [double[]]$memoryValues = @($group.Group.row.peak_working_set_bytes |
            ForEach-Object { [double]$_ / 1MB })
        $memoryMedian = Get-Percentile $memoryValues 0.50
        $memoryP95 = Get-Percentile $memoryValues 0.95
        Write-Output ("| {0} ({1}) | {2} | {3:N3} | {4:N3} |" -f `
            $group.Name, "peak_working_set_mib", $memoryValues.Count,
            $memoryMedian, $memoryP95)
    }
}

$resolvedSyncPath = $SyncCsvPath
if ([string]::IsNullOrWhiteSpace($resolvedSyncPath)) {
    $resolvedSyncPath = Join-Path (Split-Path -Parent $CsvPath) "sync.csv"
}

if (Test-Path $resolvedSyncPath) {
    $syncRows = @(Import-Csv $resolvedSyncPath)
    $measuredSyncRows = @($syncRows | Where-Object { $_.warmup -eq "0" })
    $steadyRows = @($measuredSyncRows |
        Where-Object { $_.scenario -eq "steady_playback" })

    Write-Output ""
    Write-Output "## VideoSync steady-playback telemetry (warmups excluded)"
    Write-Output ""
    Write-Output "| Samples | FPS median | FPS minimum | Catch-up drops median | Catch-up drops P95 | Wait overshoot avg median (ms) | Wait overshoot avg P95 (ms) | Wait overshoot max P95 (ms) | Wakeup late max P95 (ms) |"
    Write-Output "|---:|---:|---:|---:|---:|---:|---:|---:|---:|"

    if ($steadyRows.Count -gt 0) {
        $fps = Get-Stats $steadyRows "fps"
        $drops = Get-Stats $steadyRows "catchup_dropped"
        $overshootAverage = Get-Stats $steadyRows "wait_overshoot_avg_us"
        $overshootMaximum = Get-Stats $steadyRows "wait_overshoot_max_us"
        $wakeupLateMaximum = Get-Stats $steadyRows "wakeup_late_max_us"
        Write-Output ("| {0} | {1:N3} | {2:N3} | {3:N1} | {4:N1} | {5:N3} | {6:N3} | {7:N3} | {8:N3} |" -f `
            $fps.samples,
            $fps.median,
            $fps.minimum,
            $drops.median,
            $drops.p95,
            ($overshootAverage.median / 1000.0),
            ($overshootAverage.p95 / 1000.0),
            ($overshootMaximum.p95 / 1000.0),
            ($wakeupLateMaximum.p95 / 1000.0))
    } else {
        Write-Output "| 0 | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |"
    }

    Write-Output ""
    Write-Output "| Samples | Presented-late median | Presented lateness median (ms) | Callback avg median (us) | Callback max P95 (us) | Busy-wait avg median (us) |"
    Write-Output "|---:|---:|---:|---:|---:|---:|"
    if ($steadyRows.Count -gt 0) {
        $presentedLate = Get-Stats $steadyRows "presented_late"
        $presentedLateness = Get-Stats $steadyRows "presented_lateness_avg_us"
        $callbackAverage = Get-Stats $steadyRows "callback_avg_us"
        $callbackMaximum = Get-Stats $steadyRows "callback_max_us"
        $busyWaitAverage = Get-Stats $steadyRows "busy_wait_avg_us"
        Write-Output ("| {0} | {1:N1} | {2:N3} | {3:N3} | {4:N3} | {5:N3} |" -f `
            $presentedLate.samples,
            $presentedLate.median,
            ($presentedLateness.median / 1000.0),
            $callbackAverage.median,
            $callbackMaximum.p95,
            $busyWaitAverage.median)
    } else {
        Write-Output "| 0 | n/a | n/a | n/a | n/a | n/a |"
    }

    Write-Output ""
    Write-Output "Source: ``$resolvedSyncPath``. Telemetry rows include warmups; reported steady-playback statistics exclude them."
} else {
    Write-Output ""
    Write-Output "_VideoSync telemetry unavailable: ``$resolvedSyncPath`` was not found._"
}
