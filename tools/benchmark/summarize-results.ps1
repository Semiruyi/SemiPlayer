param(
    [Parameter(Mandatory = $true)]
    [string]$CsvPath
)

$ErrorActionPreference = "Stop"

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $rank = [math]::Ceiling($Percentile * $sorted.Count) - 1
    $rank = [math]::Max(0, [math]::Min($rank, $sorted.Count - 1))
    return [double]$sorted[$rank]
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
}
