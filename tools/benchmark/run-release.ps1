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
    $benchmark = Join-Path $repository "build-windows-benchmark/bin/semi_player_benchmark.exe"
    if (-not (Test-Path $benchmark)) {
        throw "Benchmark executable not found: $benchmark"
    }

    $commit = "unknown"
    try {
        $commit = (git -c "safe.directory=$repository" rev-parse HEAD).Trim()
    } catch {
        # A source archive may not contain Git metadata.
    }

    $hash = (Get-FileHash -Algorithm SHA256 $media).Hash
    $processor = Get-CimInstance Win32_Processor | Select-Object -First 1
    $computer = Get-CimInstance Win32_ComputerSystem
    $operatingSystem = Get-CimInstance Win32_OperatingSystem
    [ordered]@{
        git_commit = $commit
        build_preset = "windows-benchmark"
        build_type = "Release"
        media_path = $media
        media_sha256 = $hash
        runs = $Runs
        warmups = $Warmups
        steady_seconds = $SteadySeconds
        system = [ordered]@{
            cpu = $processor.Name
            physical_memory_bytes = $computer.TotalPhysicalMemory
            os = $operatingSystem.Caption
            os_version = $operatingSystem.Version
        }
        measured_at = (Get-Date).ToString("o")
    } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $resultDirectory "metadata.json")

    & $benchmark `
        --media $media `
        --scenario all `
        --runs $Runs `
        --warmups $Warmups `
        --steady-seconds $SteadySeconds `
        --output $csvPath

    if ($LASTEXITCODE -ne 0) { throw "Benchmark execution failed" }

    $summaryPath = Join-Path $resultDirectory "summary.md"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $repository "tools/benchmark/summarize-results.ps1") `
        -CsvPath $csvPath | Set-Content -Encoding utf8 $summaryPath

    Write-Output "Results: $resultDirectory"
} finally {
    Pop-Location
}
