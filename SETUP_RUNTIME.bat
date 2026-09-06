@echo off
setlocal
title Vice City VR - Required Runtime Setup
if exist "%~dp0reVC.exe" (
    powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\dlss\install-dlss5.ps1" -Profile BASE -GameDir "%~dp0." %*
) else (
    powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\dlss\install-dlss5.ps1" -Profile BASE %*
)
set "VCVR_SETUP_EXIT=%ERRORLEVEL%"
echo.
if not "%VCVR_SETUP_EXIT%"=="0" echo Setup did not complete. Read the message above before starting the game.
pause
exit /b %VCVR_SETUP_EXIT%
