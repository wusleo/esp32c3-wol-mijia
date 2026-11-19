# Wait for the ESP32-C3 USB-JTAG port to appear, then flash the firmware.
#
# Why this exists: the C3 shuts down the USB PHY in light sleep, so the port
# is gone until the chip actually resets. Instead of guessing the right moment,
# this script polls for the port and flashes the instant it shows up.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\flash-when-ready.ps1
#
# Then press RESET on the board (or unplug and replug USB).
#
# Python selection order:
#   1. -PythonExe "C:\path\to\python.exe"
#   2. $env:IDF_PYTHON_ENV_PATH\Scripts\python.exe   (set by export.ps1)
#   3. python / py on PATH (needs: pip install esptool)

param(
    [string]$PythonExe,
    [int]$TimeoutMinutes = 60
)

$ErrorActionPreference = 'Stop'

$root   = Split-Path -Parent $PSScriptRoot
$build  = Join-Path $root 'build'

$binBoot = Join-Path $build 'bootloader\bootloader.bin'
$binPart = Join-Path $build 'partition_table\partition-table.bin'

# app binary name follows the project directory name in ESP-IDF
$appName = Split-Path -Leaf $root
$binApp  = Join-Path $build "$appName.bin"

foreach ($f in @($binBoot, $binPart, $binApp)) {
    if (-not (Test-Path $f)) {
        Write-Output "MISSING IMAGE: $f"
        Write-Output 'Run "idf.py build" first.'
        exit 3
    }
}

# ---- locate python -------------------------------------------------------
if (-not $PythonExe) {
    if ($env:IDF_PYTHON_ENV_PATH) {
        $cand = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
        if (Test-Path $cand) { $PythonExe = $cand }
    }
}
if (-not $PythonExe) {
    foreach ($n in @('python', 'py')) {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { $PythonExe = $c.Source; break }
    }
}
if (-not $PythonExe) {
    Write-Output 'No python found. Pass -PythonExe, or run IDF export.ps1 first.'
    exit 4
}

Write-Output "python : $PythonExe"

# ---- wait for the port ---------------------------------------------------
function Get-EspPorts {
    $devs = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.DeviceID -like 'USB\VID_303A*' -and $_.Name -match '\(COM\d+\)' }
    $ports = @()
    foreach ($d in $devs) {
        if ($d.Name -match '\((COM\d+)\)') { $ports += $Matches[1] }
    }
    return $ports
}

Write-Output '== waiting for ESP32-C3 USB port =='
Write-Output '== press RESET on the board, or unplug / replug USB =='

$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$target   = $null
$waited   = 0

while ((Get-Date) -lt $deadline) {
    $ports = Get-EspPorts
    if (-not $ports -or $ports.Count -eq 0) {
        $ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    }

    foreach ($p in $ports) {
        if ([string]::IsNullOrWhiteSpace($p)) { continue }
        $ts  = Get-Date -Format 'HH:mm:ss'
        $out = & $PythonExe -m esptool --port $p --before default_reset --after no_reset --no-stub chip_id 2>&1
        $txt = ($out | Out-String)
        if ($txt -match 'Chip is ESP32-C3') {
            $target = $p
            Write-Output "[$ts] chip found on $p"
            break
        }
    }

    if ($target) { break }

    if ($waited -ge 10) {
        $waited = 0
        & pnputil /scan-devices 2>&1 | Out-Null
        Write-Output ("[{0}] rescan, ports: {1}" -f (Get-Date -Format 'HH:mm:ss'), (($ports -join ',') -replace '^$','none'))
    }

    Start-Sleep -Milliseconds 400
    $waited += 0.4
}

if (-not $target) {
    Write-Output 'TIMEOUT: no ESP32-C3 port appeared.'
    exit 1
}

# ---- flash ---------------------------------------------------------------
Write-Output "== flashing on $target =="

$out = & $PythonExe -m esptool --port $target --before default_reset --after hard_reset `
        -b 921600 write_flash `
        --flash_mode dio --flash_size 4MB --flash_freq 80m `
        0x0     $binBoot `
        0x8000  $binPart `
        0x10000 $binApp 2>&1

$txt = ($out | Out-String)
Write-Output $txt

if ($txt -match 'Hash of data verified') {
    Write-Output '=== FLASH OK ==='
    exit 0
} else {
    Write-Output '=== FLASH FAILED ==='
    exit 2
}
