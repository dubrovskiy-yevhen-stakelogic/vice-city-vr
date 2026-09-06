@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\source-kit\audit-source-kit.ps1" %*
exit /b %errorlevel%
