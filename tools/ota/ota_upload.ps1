# ==============================================================================
# One-Click ESP32 OTA Build & Wireless Upload Script
# ==============================================================================
param(
    [string]$IpAddress = "",
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
Write-Host " ESP32 GARAGE DOOR CONTROLLER - OTA BUILD AND UPLOAD " -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# 1. Target device IP
$IpAddress = Get-Esp32TargetIp -ProjectRoot $projectRoot -IpAddress $IpAddress -SkipPrompts:$SkipPrompts

# 2. Resolve Wi-Fi configuration & build flags
Resolve-Esp32Credentials -ProjectRoot $projectRoot `
    -SkipPrompts:$SkipPrompts `
    -WifiSsid $WifiSsid `
    -WifiPass $WifiPass `
    -WifiBackupSsid $WifiBackupSsid `
    -WifiBackupPass $WifiBackupPass `
    -AllowBackup

# 3. Locate PlatformIO
$pio = Get-PlatformIOExecutable

Push-Location $projectRoot
try {
    # 4. Compile ESP32 firmware binary
    Write-Host "`n[1/3] Compiling ESP32 firmware binary..." -ForegroundColor Yellow
    & $pio run -e esp32dev
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nBuild failed! Aborting upload." -ForegroundColor Red
        exit 1
    }

    # 5. Export and verify binary size for OTA partition
    $exported = Export-Esp32FirmwareBinary -ProjectRoot $projectRoot
    $destBin = $exported.Path
    Write-Host "`nFirmware binary updated: $destBin ($($exported.SizeKb) KB)" -ForegroundColor Green

    # Sanity check: Ensure binary fits within target OTA partition (0x1E0000 / ~1.9 MB)
    Assert-OtaBinarySize -BinaryPath $destBin

    # Calculate MD5 checksum for end-to-end payload verification
    $md5Hash = (Get-FileHash -Path $destBin -Algorithm MD5).Hash.ToLower()
    Write-Host "Firmware MD5: $md5Hash" -ForegroundColor Gray

    # 6. Upload via HTTP Web OTA endpoint
    Write-Host "`n[2/3] Uploading firmware to ESP32 (http://${IpAddress}/update)..." -ForegroundColor Yellow

    $uploadSuccess = $false
    try {
        $uri = "http://${IpAddress}/update"
        $response = & curl.exe -s -S --fail --connect-timeout 6 --max-time 60 -H "x-MD5: $md5Hash" -F "update=@$destBin" $uri 2>&1
        
        if ($LASTEXITCODE -eq 0 -and $response -match "success") {
            Write-Host "Web OTA Upload Successful! Response: $response" -ForegroundColor Green
            $uploadSuccess = $true
        } else {
            Write-Host "Web OTA response / error: $response" -ForegroundColor Yellow
            if ($response -match "409" -or $response -match "motion") {
                Write-Host "`nUpload aborted: Door is currently moving. Firmware update is locked for physical safety." -ForegroundColor Red
                exit 1
            }
        }
    } catch {
        Write-Host "HTTP OTA request to http://${IpAddress}/update failed: $_" -ForegroundColor Yellow
    }

    if ($uploadSuccess) {
        Write-Host "`n[3/3] ESP32 is rebooting with new firmware!" -ForegroundColor Cyan
        exit 0
    }

    # 7. Fallback to ArduinoOTA wireless upload
    Write-Host "`n[3/3] Trying ArduinoOTA wireless upload..." -ForegroundColor Yellow
    & $pio run -t upload -e esp32dev_ota --upload-port $IpAddress
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`nArduinoOTA Upload Successful!" -ForegroundColor Green
    } else {
        Write-Host "`nOTA Upload failed. Please check Wi-Fi connection and ESP32 IP (${IpAddress})." -ForegroundColor Red
        exit 1
    }
} finally {
    Clear-Esp32BuildFlags
    Pop-Location
}
