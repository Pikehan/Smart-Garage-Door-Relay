# ==============================================================================
# ESP32 Standalone Firmware Binary (.bin) Generation Script
# ==============================================================================
param(
    [switch]$SkipPrompts = $false,
    [string]$WifiSsid = "",
    [string]$WifiPass = "",
    [string]$WifiBackupSsid = "",
    [string]$WifiBackupPass = ""
)

$ErrorActionPreference = "Stop"

$commonScript = if (Test-Path "$PSScriptRoot\Common-Esp32.ps1") { "$PSScriptRoot\Common-Esp32.ps1" } else { "$PSScriptRoot\..\Common-Esp32.ps1" }
. $commonScript

$projectRoot = (Resolve-Path "$PSScriptRoot\..\..").Path

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " ESP32 GARAGE CONTROLLER - FIRMWARE BINARY BUILDER    " -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 1. Resolve Wi-Fi configuration & build flags
Resolve-Esp32Credentials -ProjectRoot $projectRoot `
    -SkipPrompts:$SkipPrompts `
    -WifiSsid $WifiSsid `
    -WifiPass $WifiPass `
    -WifiBackupSsid $WifiBackupSsid `
    -WifiBackupPass $WifiBackupPass `
    -AllowBackup

# 2. Locate PlatformIO
$pio = Get-PlatformIOExecutable

# 3. Compile Binary
Write-Host "`nCompiling ESP32 firmware binary..." -ForegroundColor Yellow
Push-Location $projectRoot
try {
    & $pio run -e esp32dev
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Build failed!" -ForegroundColor Red
        exit 1
    }
} finally {
    Clear-Esp32BuildFlags
    Pop-Location
}

# 4. Export to bin/firmware.bin
$exported = Export-Esp32FirmwareBinary -ProjectRoot $projectRoot
Write-Host "`n======================================================" -ForegroundColor Green
Write-Host " BUILD SUCCESSFUL!" -ForegroundColor Green
Write-Host " Firmware Binary: $($exported.Path) ($($exported.SizeKb) KB)" -ForegroundColor Green
Write-Host "======================================================" -ForegroundColor Green
