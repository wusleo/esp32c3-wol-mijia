# Flash the prebuilt images in .\dist to an ESP32-C3.
#
#   .\tools\flash.ps1 -Port COM11              # plain flash
#   .\tools\flash.ps1 -Port COM11 -Erase       # wipe everything first (NVS included)
#
# Why a script: idf.py needs the whole IDF environment exported (PATH, IDF_PATH,
# python venv). esptool alone only needs the venv, so this keeps the setup short.

param(
    [string]$Port  = "COM11",
    [switch]$Erase
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"

foreach ($f in @("bootloader.bin", "partition-table.bin", "app.bin")) {
    if (-not (Test-Path (Join-Path $dist $f))) {
        throw "missing $f in $dist -- build first, then copy the three images there"
    }
}

# Locate the python that has esptool installed (the IDF virtualenv).
$idfTools = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "D:\Espressif" }
$py = Get-ChildItem -Path (Join-Path $idfTools "python_env") -Filter "python.exe" -Recurse -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match 'Scripts\\python\.exe$' } |
      Select-Object -First 1 -ExpandProperty FullName

if (-not $py) {
    throw "cannot find IDF python under $idfTools -- set IDF_TOOLS_PATH"
}

Write-Host "python : $py"
Write-Host "port   : $Port"
Write-Host "images : $dist"

$common = @("--chip", "esp32c3", "-p", $Port, "-b", "460800",
            "--before", "default_reset", "--after", "hard_reset")

if ($Erase) {
    Write-Host "`n=== erase_flash (wipes NVS, so the board boots into provisioning) ==="
    & $py -m esptool @common erase_flash
    if ($LASTEXITCODE -ne 0) { throw "erase_flash failed" }
}

Write-Host "`n=== write_flash ==="
& $py -m esptool @common write_flash `
    --flash_mode dio --flash_size 4MB --flash_freq 80m `
    0x0     (Join-Path $dist "bootloader.bin") `
    0x8000  (Join-Path $dist "partition-table.bin") `
    0x10000 (Join-Path $dist "app.bin")

if ($LASTEXITCODE -ne 0) { throw "write_flash failed" }

Write-Host "`ndone -- the board resets itself and starts running." -ForegroundColor Green
