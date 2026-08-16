param(
    [Parameter(Mandatory = $true)]
    [string]$MediaPath,
    [int]$Runs = 5,
    [int]$Warmups = 1,
    [int]$SteadySeconds = 60,
    [string]$MsysUcrt64Bin = "C:\msys64\ucrt64\bin"
)

$ErrorActionPreference = "Stop"

$repository = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$media = (Resolve-Path $MediaPath).Path

if ($Runs -le 0 -or $Warmups -lt 0 -or $SteadySeconds -le 0) {
    throw "Runs and SteadySeconds must be positive; Warmups must be non-negative"
}

if (Test-Path $MsysUcrt64Bin) {
    $env:Path = "$MsysUcrt64Bin;$env:Path"
}

Push-Location $repository
try {
    cmake --preset windows-benchmark
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    cmake --build --preset windows-benchmark
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $resultDirectory = Join-Path $repository "benchmarks/results/$timestamp"
    New-Item -ItemType Directory -Force -Path $resultDirectory | Out-Null

    $csvPath = Join-Path $resultDirectory "raw.csv"
    $syncCsvPath = Join-Path $resultDirectory "sync.csv"
    $logPath = Join-Path $resultDirectory "logs/semi_player.log"
    $consoleLogPath = Join-Path $resultDirectory "benchmark.log"
    $stdoutLogPath = Join-Path $resultDirectory "benchmark.stdout.log"
    $summaryPath = Join-Path $resultDirectory "summary.md"
    $benchmark = Join-Path $repository "build-windows-benchmark/bin/semi_player_benchmark.exe"
    if (-not (Test-Path $benchmark)) {
        throw "Benchmark executable not found: $benchmark"
    }

    $commit = "unknown"
    try {
        $gitSafeDirectory = $repository.Replace('\', '/')
        $commit = (git -c "safe.directory=$gitSafeDirectory" rev-parse HEAD).Trim()
    } catch {
        # A source archive may not contain Git metadata.
    }

    $hash = (Get-FileHash -Algorithm SHA256 $media).Hash
    $system = [ordered]@{
        cpu = "unknown"
        physical_memory_bytes = $null
        os = "unknown"
        os_version = "unknown"
        collection_status = "unavailable"
    }
    try {
        $processor = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
        $computer = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
        $operatingSystem = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        $system.cpu = $processor.Name.Trim()
        $system.physical_memory_bytes = $computer.TotalPhysicalMemory
        $system.os = $operatingSystem.Caption
        $system.os_version = $operatingSystem.Version
        $system.collection_status = "ok"
    } catch {
        # System metadata is useful context but must not prevent measurements.
    }
    [ordered]@{
        git_commit = $commit
        build_preset = "windows-benchmark"
        build_type = "Release"
        media_path = $media
        media_sha256 = $hash
        runs = $Runs
        warmups = $Warmups
        steady_seconds = $SteadySeconds
        system = $system
        log_path = $logPath
        stdout_log_path = $stdoutLogPath
        sync_csv_path = $syncCsvPath
        summary_path = $summaryPath
        measured_at = (Get-Date).ToString("o")
    } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $resultDirectory "metadata.json")

    Push-Location $resultDirectory
    try {
        $commandLine = '""{0}" --media "{1}" --scenario all --runs {2} --warmups {3} --steady-seconds {4} --output "{5}" 1> "{6}" 2> "{7}""' -f `
            $benchmark,
            $media,
            $Runs,
            $Warmups,
            $SteadySeconds,
            $csvPath,
            $stdoutLogPath,
            $consoleLogPath
        & cmd.exe /d /s /c $commandLine

        if ($LASTEXITCODE -ne 0) {
            throw "Benchmark execution failed with exit code $LASTEXITCODE"
        }
        if (Test-Path $stdoutLogPath) {
            Get-Content -LiteralPath $stdoutLogPath
        }
    } finally {
        Pop-Location
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $repository "tools/benchmark/parse-sync-log.ps1") `
        -LogPath $logPath `
        -OutputPath $syncCsvPath `
        -Scenario all `
        -Runs $Runs `
        -Warmups $Warmups
    if ($LASTEXITCODE -ne 0) { throw "Sync log parsing failed" }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $repository "tools/benchmark/summarize-results.ps1") `
        -CsvPath $csvPath `
        -SyncCsvPath $syncCsvPath | Set-Content -Encoding utf8 $summaryPath

    Write-Output "Results: $resultDirectory"
} finally {
    Pop-Location
}
