# ==============================================================================
# Common Helper Functions for ESP32 Tooling & Build Scripts
# ==============================================================================

function Get-PlatformIOExecutable {
    $pio = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
    if (-not (Test-Path $pio)) {
        $pioCmd = Get-Command "platformio" -ErrorAction SilentlyContinue
        if ($pioCmd) {
            $pio = "platformio"
        } else {
            $pioCmdShort = Get-Command "pio" -ErrorAction SilentlyContinue
            if ($pioCmdShort) {
                $pio = "pio"
            } else {
                Write-Host "`n[ERROR] PlatformIO is not installed on this machine!" -ForegroundColor Red
                Write-Host "To set it up on a new PC, either:" -ForegroundColor Yellow
                Write-Host "  1. Install the 'PlatformIO IDE' extension in VS Code, OR" -ForegroundColor Cyan
                Write-Host "  2. Install Python and run: pip install platformio`n" -ForegroundColor Cyan
                exit 1
            }
        }
    }
    return $pio
}

function Read-SecurePassword {
    param([string]$Prompt = "Password")
    $secPass = Read-Host $Prompt -AsSecureString
    $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secPass)
    try {
        return [System.Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
    } finally {
        [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

function Resolve-Esp32Credentials {
    param(
        [string]$ProjectRoot,
        [switch]$SkipPrompts = $false,
        [string]$WifiSsid = "",
        [string]$WifiPass = "",
        [string]$WifiBackupSsid = "",
        [string]$WifiBackupPass = "",
        [switch]$AllowBackup = $false
    )

    $secretsFile = Join-Path $ProjectRoot "include\secrets.h"
    $hasSecrets = Test-Path $secretsFile

    # If parameters were already supplied via CLI, use them directly; otherwise prompt interactively
    if ([string]::IsNullOrWhiteSpace($WifiSsid) -and -not $SkipPrompts) {
        Write-Host "`n------------------------------------------------------" -ForegroundColor Cyan
        Write-Host " WI-FI NETWORK CONFIGURATION" -ForegroundColor Cyan
        Write-Host "------------------------------------------------------" -ForegroundColor Cyan
        if ($hasSecrets) {
            Write-Host "Found include/secrets.h on this machine." -ForegroundColor Gray
            $askWifiManual = Read-Host "Enter Wi-Fi credentials manually? [y/N]"
            $enterWifiManual = ($askWifiManual -match "^[yY]")
            if (-not $enterWifiManual) {
                Write-Host "Using Wi-Fi credentials from include/secrets.h." -ForegroundColor Green
            }
        } else {
            $askWifiManual = Read-Host "Enter Wi-Fi credentials manually? [Y/n]"
            $enterWifiManual = ($askWifiManual -notmatch "^[nN]")
            if (-not $enterWifiManual) {
                Write-Host "No Wi-Fi credentials entered. Device will boot using existing NVS Wi-Fi or SoftAP portal." -ForegroundColor Yellow
            }
        }

        if ($enterWifiManual) {
            $inputSsid = Read-Host "Primary Wi-Fi SSID"
            while ([string]::IsNullOrWhiteSpace($inputSsid)) {
                $inputSsid = Read-Host "SSID cannot be blank. Enter Primary Wi-Fi SSID"
            }
            $WifiSsid = $inputSsid
            $WifiPass = Read-SecurePassword "Primary Wi-Fi Password"

            if ($AllowBackup) {
                $askBackup = Read-Host "`nConfigure backup Wi-Fi hotspot? [y/N]"
                if ($askBackup -match "^[yY]") {
                    $WifiBackupSsid = Read-Host "Backup Wi-Fi SSID"
                    $WifiBackupPass = Read-SecurePassword "Backup Wi-Fi Password"
                }
            }
        }
    }

    # Always construct flags if values exist, regardless of how they were provided
    $customFlags = @()
    if (-not [string]::IsNullOrWhiteSpace($WifiSsid)) {
        $escapedSsid = $WifiSsid.Replace('"', '\"')
        $escapedPass = $WifiPass.Replace('"', '\"')
        $customFlags += "`"-DWIFI_SSID_PRIMARY=\`"$escapedSsid\`"`""
        $customFlags += "`"-DWIFI_PASS_PRIMARY=\`"$escapedPass\`"`""
    }
    if (-not [string]::IsNullOrWhiteSpace($WifiBackupSsid)) {
        $escapedBakSsid = $WifiBackupSsid.Replace('"', '\"')
        $escapedBakPass = $WifiBackupPass.Replace('"', '\"')
        $customFlags += "`"-DWIFI_SSID_BACKUP=\`"$escapedBakSsid\`"`""
        $customFlags += "`"-DWIFI_PASS_BACKUP=\`"$escapedBakPass\`"`""
    }

    if ($customFlags.Count -gt 0) {
        $env:PLATFORMIO_BUILD_FLAGS = $customFlags -join " "
    }

    return [PSCustomObject]@{
        WifiSsid       = $WifiSsid
        WifiPass       = $WifiPass
        WifiBackupSsid = $WifiBackupSsid
        WifiBackupPass = $WifiBackupPass
        CustomFlags    = $customFlags
    }
}

function Clear-Esp32BuildFlags {
    Remove-Item Env:\PLATFORMIO_BUILD_FLAGS -ErrorAction SilentlyContinue
}

function Get-Esp32TargetIp {
    param(
        [string]$ProjectRoot,
        [string]$IpAddress = "",
        [switch]$SkipPrompts = $false
    )

    if (-not [string]::IsNullOrWhiteSpace($IpAddress)) {
        return $IpAddress.Trim()
    }

    $defaultIp = "192.168.1.33"
    $configFile = Join-Path $ProjectRoot "include\config.h"
    if (Test-Path $configFile) {
        $ipLine = Get-Content $configFile | Where-Object { $_ -match "LOCAL_IP\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)" } | Select-Object -First 1
        if ($ipLine -match "LOCAL_IP\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)") {
            $defaultIp = "$($matches[1]).$($matches[2]).$($matches[3]).$($matches[4])"
        }
    }

    if ($SkipPrompts) {
        return $defaultIp
    }

    $inputIp = Read-Host "`nEnter target ESP32 IP Address [Default: $defaultIp]"
    if ([string]::IsNullOrWhiteSpace($inputIp)) {
        return $defaultIp
    }
    return $inputIp.Trim()
}

function Export-Esp32FirmwareBinary {
    param([string]$ProjectRoot)

    $builtBin = Join-Path $ProjectRoot ".pio\build\esp32dev\firmware.bin"
    $destBin = Join-Path $ProjectRoot "bin\firmware.bin"

    if (Test-Path $builtBin) {
        New-Item -ItemType Directory -Force -Path (Split-Path $destBin) | Out-Null
        Copy-Item -Path $builtBin -Destination $destBin -Force
        $binSize = (Get-Item $destBin).Length
        return [PSCustomObject]@{
            Path   = $destBin
            Size   = $binSize
            SizeKb = [math]::Round($binSize / 1024, 1)
        }
    } else {
        Write-Host "`n[ERROR] Could not find compiled binary at $builtBin" -ForegroundColor Red
        exit 1
    }
}

function Assert-OtaBinarySize {
    param(
        [string]$BinaryPath,
        [int]$MaxAllowedBytes = 0x1E0000 # 1,966,080 bytes (~1.9 MB min_spiffs limit)
    )

    if (-not (Test-Path $BinaryPath)) {
        Write-Host "`n[ERROR] Could not find binary at $BinaryPath" -ForegroundColor Red
        exit 1
    }

    $binSize = (Get-Item $BinaryPath).Length
    $sizeKb = [math]::Round($binSize / 1024, 1)
    $maxKb = [math]::Round($MaxAllowedBytes / 1024, 1)

    if ($binSize -gt $MaxAllowedBytes) {
        Write-Host "`n[ERROR] Firmware binary size ($sizeKb KB) exceeds OTA partition limit ($maxKb KB)!" -ForegroundColor Red
        Write-Host "Flashing this binary over-the-air will fail or corrupt the partition. Aborting upload." -ForegroundColor Red
        exit 1
    }

    return $binSize
}
