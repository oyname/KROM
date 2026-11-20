@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_sdk.ps1" %*
exit /b %errorlevel%
