# ==============================================================================
# One-Click ESP32 USB Flash & Build Script
# ==============================================================================
param(
    [string]$Port = "",
    [switch]$Monitor = $false,
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
Write-Host "  ESP32 GARAGE DOOR CONTROLLER - USB FLASH AUTOMATION " -ForegroundColor Cyan
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

# 3. Build and flash via USB
Write-Host "`n[1/2] Compiling and uploading firmware via USB..." -ForegroundColor Yellow
Push-Location $projectRoot
try {
    $uploadArgs = @("run", "-e", "esp32dev", "--target", "upload")
    if ($Port -ne "") {
        $uploadArgs += @("--upload-port", $Port)
        Write-Host "Targeting COM Port: $Port" -ForegroundColor DarkCyan
    }

    & $pio @uploadArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Upload failed! If the ESP32 is stuck connecting, hold down the BOOT button." -ForegroundColor Red
        exit 1
    }
} finally {
    Clear-Esp32BuildFlags
    Pop-Location
}

Write-Host "`n[SUCCESS] ESP32 flashed successfully via USB!" -ForegroundColor Green

# 4. Optional serial monitor
if ($Monitor) {
    Write-Host "`n[2/2] Opening Serial Monitor (115200 baud)... Press Ctrl+C to stop." -ForegroundColor Yellow
    Push-Location $projectRoot
    try {
        $monitorArgs = @("device", "monitor", "-b", "115200")
        if ($Port -ne "") {
            $monitorArgs += @("--port", $Port)
        }
        & $pio @monitorArgs
    } finally {
        Pop-Location
    }
}
