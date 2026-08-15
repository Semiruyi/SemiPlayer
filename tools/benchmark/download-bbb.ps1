param(
    [string]$OutputPath = ".tmp/benchmark-media/big-buck-bunny-1080p.mp4"
)

$ErrorActionPreference = "Stop"

$sourceUrl = "https://video.blender.org/object-storage/web_videos/6402b77c-b61f-4a06-96ca-c8420a2becf4-1080.mp4"
$expectedSize = 276266905
$expectedSha256 = "91768E73427FB1E4A3D3A419CB173E7A9D97340190734C361AC56FE2BB6C8A0D"

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $resolvedOutput
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$existing = Get-Item $resolvedOutput -ErrorAction SilentlyContinue
if ($null -eq $existing -or $existing.Length -ne $expectedSize) {
    if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
        throw "curl.exe was not found on PATH"
    }

    & curl.exe -L --fail --retry 3 --output $resolvedOutput $sourceUrl
    if ($LASTEXITCODE -ne 0) {
        throw "download failed with exit code $LASTEXITCODE"
    }
}

$actualHash = (Get-FileHash -Algorithm SHA256 $resolvedOutput).Hash
if ($actualHash -ne $expectedSha256) {
    throw "SHA-256 mismatch: expected $expectedSha256, got $actualHash"
}

if (-not (Get-Command ffprobe -ErrorAction SilentlyContinue)) {
    throw "ffprobe was not found on PATH"
}

& ffprobe -v error `
    -show_entries format=duration,size `
    -show_entries stream=index,codec_name,profile,level,width,height,pix_fmt,r_frame_rate,sample_rate,channels `
    -of json $resolvedOutput

Write-Output "Verified: $resolvedOutput"
Write-Output "SHA-256: $actualHash"
