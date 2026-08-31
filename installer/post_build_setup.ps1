$ErrorActionPreference = "Stop"
$dist = Join-Path (Get-Location) "dist"
$setup = Get-ChildItem -LiteralPath $dist -Filter "*Setup-x64.exe" |
    Where-Object { $_.Name -ne "QingYuePDF-Setup-x64.exe" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $setup) { throw "setup exe not found" }

$ascii = Join-Path $dist "QingYuePDF-Setup-x64.exe"
Copy-Item -LiteralPath $setup.FullName -Destination $ascii -Force

function Get-Sha256([string]$Path) {
    $getFileHash = Get-Command Get-FileHash -ErrorAction SilentlyContinue
    if ($getFileHash) {
        return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
    }

    $lines = & certutil.exe -hashfile $Path SHA256
    if ($LASTEXITCODE -ne 0) { throw "Unable to calculate installer SHA256" }
    return (($lines | Where-Object { $_ -match '^[0-9A-Fa-f ]{64,}$' } | Select-Object -First 1) -replace ' ', '').ToUpperInvariant()
}

$hash = Get-Sha256 $ascii
$asciiHash = $ascii + ".sha256.txt"
[IO.File]::WriteAllText($asciiHash, $hash + "  QingYuePDF-Setup-x64.exe" + [Environment]::NewLine, [Text.Encoding]::ASCII)

$sourceHash = Join-Path $dist ($setup.Name + ".sha256.txt")
[IO.File]::WriteAllText($sourceHash, $hash + "  " + $setup.Name + [Environment]::NewLine, [Text.Encoding]::ASCII)

Write-Host ("Installer: " + $setup.FullName)
Write-Host ("ASCII copy: " + $ascii)
Write-Host ("Installer SHA256: " + $hash)
