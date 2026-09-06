@echo off
setlocal
title Vice City VR - PC Saves to Quest

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\transfer-pc-saves-to-quest.ps1" %*
set "VCVR_EXIT=%ERRORLEVEL%"

echo.
if not "%VCVR_EXIT%"=="0" (
    echo FAILED. Read the error above. Nothing was silently ignored.
) else (
    echo Save transfer wizard finished successfully.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %VCVR_EXIT%
