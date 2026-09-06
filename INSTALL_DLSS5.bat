@echo off
setlocal
title Vice City VR - DLSS 5 Setup
powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\dlss\install-dlss5.ps1" %*
set "VCVR_SETUP_EXIT=%ERRORLEVEL%"
echo.
if not "%VCVR_SETUP_EXIT%"=="0" echo Setup did not complete. Read the message above; do not launch with a partial installation.
pause
exit /b %VCVR_SETUP_EXIT%
