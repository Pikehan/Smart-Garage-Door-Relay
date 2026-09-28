@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0build_bin.ps1" %*
pause
