@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0usb_upload.ps1" %*
pause
