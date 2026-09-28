@echo off
title ESP32 Garage Door Controller — OTA One-Click Build ^& Upload
powershell -ExecutionPolicy Bypass -File "%~dp0ota_upload.ps1" %*
pause
