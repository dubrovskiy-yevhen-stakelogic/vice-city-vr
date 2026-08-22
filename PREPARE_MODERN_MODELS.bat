@echo off
setlocal
title Vice City VR PC - Prepare Modern Models
cd /d "%~dp0"

echo ================================================================
echo  VICE CITY VR PC - DOWNLOAD, BUILD AND INSTALL MODERN MODELS
echo ================================================================
echo.
echo This creates a personal Modern overlay from your legal GTA Vice City
echo installation. No GTA data or third-party model pack is bundled here.
echo.

set "INSTALLER=%~dp0tools\prepare-modern-models.ps1"
if not exist "%INSTALLER%" (
    echo ERROR: Required file is missing:
    echo   %INSTALLER%
    echo.
    echo Extract the complete Vice City VR source/release folder, then retry.
    set "RESULT=1"
    goto :finish
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%INSTALLER%" %*
set "RESULT=%ERRORLEVEL%"

:finish
echo.
if not "%RESULT%"=="0" (
    echo FAILED. Read the error above; an existing Modern overlay was not
    echo removed before a complete verified replacement was ready.
) else (
    echo COMPLETE. Fully restart Vice City VR before selecting Modern assets.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RESULT%

