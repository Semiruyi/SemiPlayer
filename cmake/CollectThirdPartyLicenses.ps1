[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [Parameter(Mandatory = $true)]
    [string]$NoticesPath,

    [Parameter(Mandatory = $true)]
    [string]$PacmanExecutable,

    [Parameter(Mandatory = $true)]
    [string]$ToolchainPrefix,

    [Parameter(Mandatory = $true)]
    [string]$MsysRoot,

    [string[]]$AdditionalPackages = @()
)

$ErrorActionPreference = 'Stop'
$env:LANG = 'C'
$env:LC_ALL = 'C'

function Invoke-Pacman {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $output = @(& $PacmanExecutable @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "pacman $($Arguments -join ' ') failed:`n$($output -join "`n")"
    }
    return $output
}

function Read-PackageField {
    param(
        [string[]]$Metadata,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $line = $Metadata | Where-Object { $_ -match "^$([regex]::Escape($Name))\s*:" } |
        Select-Object -First 1
    if ($null -eq $line) {
        return 'unknown'
    }
    return ($line -replace '^[^:]+:\s*', '').Trim()
}

if (-not (Test-Path -LiteralPath $RuntimeDirectory -PathType Container)) {
    throw "Runtime directory does not exist: $RuntimeDirectory"
}
if (-not (Test-Path -LiteralPath $PacmanExecutable -PathType Leaf)) {
    throw "pacman does not exist: $PacmanExecutable"
}

$runtimeDlls = @(Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File |
    Where-Object { $_.Name -ne 'semi_player.dll' } |
    Sort-Object Name)
if ($runtimeDlls.Count -eq 0) {
    throw "No third-party runtime DLLs found in $RuntimeDirectory"
}

$packagePaths = @($runtimeDlls | ForEach-Object { "$ToolchainPrefix/bin/$($_.Name)" })
$ownerLines = @(Invoke-Pacman -Arguments (@('-Qo') + $packagePaths))
$packages = @{}
foreach ($owner in $ownerLines) {
    if ($owner -notmatch '^(?<path>\S+) is owned by (?<package>\S+) (?<version>\S+)$') {
        throw "Could not parse package owner: $owner"
    }

    $binaryName = $matches.path.Substring($matches.path.LastIndexOf('/') + 1)
    $packageName = $matches.package
    if (-not $packages.ContainsKey($packageName)) {
        $packages[$packageName] = [ordered]@{
            Version = $matches.version
            Binaries = [System.Collections.Generic.List[string]]::new()
        }
    }
    $packages[$packageName].Binaries.Add($binaryName)
}

foreach ($packageName in $AdditionalPackages) {
    if (-not $packages.ContainsKey($packageName)) {
        $packages[$packageName] = [ordered]@{
            Version = 'unknown'
            Binaries = [System.Collections.Generic.List[string]]::new()
        }
        $packages[$packageName].Binaries.Add('compiled into semi_player.dll')
    }
}

$packageNames = @($packages.Keys | Sort-Object)
$metadataByPackage = @{}
$currentPackage = $null
foreach ($line in @(Invoke-Pacman -Arguments (@('-Qi') + $packageNames))) {
    if ($line -match '^Name\s*:\s*(?<package>\S+)') {
        $currentPackage = $matches.package
        $metadataByPackage[$currentPackage] = [System.Collections.Generic.List[string]]::new()
    }
    if ($null -ne $currentPackage) {
        $metadataByPackage[$currentPackage].Add($line)
    }
}

$filesByPackage = @{}
foreach ($line in @(Invoke-Pacman -Arguments (@('-Ql') + $packageNames))) {
    if ($line -notmatch '^(?<package>\S+)\s+(?<path>\S+)$') {
        continue
    }
    if (-not $filesByPackage.ContainsKey($matches.package)) {
        $filesByPackage[$matches.package] = [System.Collections.Generic.List[string]]::new()
    }
    $filesByPackage[$matches.package].Add($matches.path)
}

if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

$notice = [System.Collections.Generic.List[string]]::new()
$notice.Add('# Third-party notices')
$notice.Add('')
$notice.Add('This manifest was generated from the runtime DLLs in this package.')
$notice.Add('Package metadata and license files come from the MSYS2 UCRT64')
$notice.Add('packages used to build this release.')
$notice.Add('')

foreach ($packageName in $packageNames) {
    $record = $packages[$packageName]
    if (-not $metadataByPackage.ContainsKey($packageName)) {
        throw "pacman returned no metadata for $packageName"
    }
    $metadata = @($metadataByPackage[$packageName])
    $record.Version = Read-PackageField -Metadata $metadata -Name 'Version'
    $license = Read-PackageField -Metadata $metadata -Name 'Licenses'
    $homepage = Read-PackageField -Metadata $metadata -Name 'URL'
    $packageFiles = if ($filesByPackage.ContainsKey($packageName)) {
        @($filesByPackage[$packageName])
    } else {
        @()
    }
    $licenseFiles = [System.Collections.Generic.List[string]]::new()
    $shortName = $packageName -replace '^mingw-w64-ucrt-x86_64-', ''

    foreach ($path in $packageFiles) {
        if ($path -notmatch '^(?<path>/\S+/share/licenses/(?<relative>.+))$') {
            continue
        }
        if ($matches.path.EndsWith('/')) {
            continue
        }

        $source = Join-Path $MsysRoot ($matches.path.TrimStart('/') -replace '/', '\')
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            continue
        }

        $relative = $matches.relative -replace '/', '\'
        $destination = Join-Path (Join-Path $OutputDirectory $shortName) $relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
        $licenseFiles.Add("licenses/$shortName/$($matches.relative)")
    }

    $notice.Add("## $packageName $($record.Version)")
    $notice.Add('')
    $notice.Add("- License: $license")
    $notice.Add("- Homepage: $homepage")
    $notice.Add("- Runtime files: $(@($record.Binaries | Sort-Object) -join ', ')")
    if ($licenseFiles.Count -gt 0) {
        $notice.Add("- Included license files: $(@($licenseFiles | Sort-Object) -join ', ')")
    } else {
        $notice.Add('- Included license files: none shipped by this MSYS2 package; see the upstream homepage')
    }
    $notice.Add('')
}

$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllLines($NoticesPath, $notice, $utf8WithoutBom)
