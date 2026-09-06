@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\source-kit\assemble.ps1" %*
exit /b %errorlevel%
