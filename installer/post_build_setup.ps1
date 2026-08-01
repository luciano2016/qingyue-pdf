$ErrorActionPreference = "Stop"
$dist = Join-Path (Get-Location) "dist"
$setup = Get-ChildItem -LiteralPath $dist -Filter "*Setup-x64.exe" | Where-Object { $_.Name -ne "QingYuePDF-Setup-x64.exe" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $setup) { throw "setup exe not found" }

$ascii = Join-Path $dist "QingYuePDF-Setup-x64.exe"
Copy-Item -LiteralPath $setup.FullName -Destination $ascii -Force

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ascii).Hash
$asciiHash = $ascii + ".sha256.txt"
Set-Content -LiteralPath $asciiHash -Encoding ASCII -Value ($hash + "  QingYuePDF-Setup-x64.exe")

$sourceHash = Join-Path $dist ($setup.Name + ".sha256.txt")
Set-Content -LiteralPath $sourceHash -Encoding ASCII -Value ($hash + "  " + $setup.Name)

Write-Host ("Installer: " + $setup.FullName)
Write-Host ("ASCII copy: " + $ascii)
Write-Host ("Installer SHA256: " + $hash)
