[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$version = "1.28.0"
$archiveName = "mupdf-$version-source.tar.gz"
$expectedHash = "21C7F064903154F1C3A7458BEE81F130FC36F9B5147EA13328F9980E02D2DEA2"
$url = "https://mupdf.com/downloads/archive/$archiveName"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"
$downloadDir = Join-Path $thirdParty ".downloads"
$archive = Join-Path $downloadDir $archiveName
$destination = Join-Path $thirdParty "mupdf-$version-source"

function Get-Sha256([string]$Path) {
    $getFileHash = Get-Command Get-FileHash -ErrorAction SilentlyContinue
    if ($getFileHash) {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
    }

    $lines = & certutil.exe -hashfile $Path SHA256
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to calculate SHA256 for $Path"
    }
    return (($lines | Where-Object { $_ -match '^[0-9A-Fa-f ]{64,}$' } | Select-Object -First 1) -replace ' ', '').ToUpperInvariant()
}

if ((Test-Path (Join-Path $destination "COPYING")) -and -not $Force) {
    Write-Host "MuPDF $version already exists at $destination"
    exit 0
}

New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null

if ($Force -or -not (Test-Path $archive)) {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $archive
}

$actualHash = Get-Sha256 $archive
if ($actualHash -ne $expectedHash) {
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    throw "MuPDF archive checksum mismatch. Expected $expectedHash, received $actualHash."
}

if (Test-Path $destination) {
    Remove-Item -LiteralPath $destination -Recurse -Force
}

Write-Host "Extracting $archiveName"
& tar.exe -xf $archive -C $thirdParty
if ($LASTEXITCODE -ne 0) {
    throw "tar.exe failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path (Join-Path $destination "COPYING"))) {
    throw "MuPDF was extracted, but the expected directory was not created: $destination"
}

Write-Host "MuPDF $version is ready at $destination"
