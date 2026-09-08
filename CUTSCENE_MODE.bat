@echo off
setlocal DisableDelayedExpansion
title Vice City VR - Cutscene Mode
set "VCVR_CUTSCENE_SELF=%~f0"
set "VCVR_CUTSCENE_MODE=%~1"
powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -Command "$text = [IO.File]::ReadAllText($env:VCVR_CUTSCENE_SELF); $parts = $text -split '(?m)^# POWERSHELL_PAYLOAD\r?$', 2; if ($parts.Count -ne 2) { exit 1 }; & ([ScriptBlock]::Create($parts[1]))"
set "VCVR_CUTSCENE_EXIT=%ERRORLEVEL%"
echo.
if not "%VCVR_CUTSCENE_EXIT%"=="0" echo No success was reported. Read the error above.
if "%VCVR_CUTSCENE_MODE%"=="" pause
exit /b %VCVR_CUTSCENE_EXIT%
# POWERSHELL_PAYLOAD
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

try {
    $mode = $env:VCVR_CUTSCENE_MODE
    if ($mode -and $mode -notin @('stereo', 'cinema')) {
        throw 'Usage: CUTSCENE_MODE.bat [stereo|cinema]'
    }
    $gameDir = [IO.Path]::GetDirectoryName($env:VCVR_CUTSCENE_SELF)
    $gameExe = Join-Path $gameDir 'reVC.exe'
    if (-not [IO.File]::Exists($gameExe)) {
        if ($mode) {
            throw 'Place CUTSCENE_MODE.bat next to reVC.exe, or double-click it to select the game.'
        }
        Add-Type -AssemblyName System.Windows.Forms
        $picker = New-Object System.Windows.Forms.OpenFileDialog
        try {
            $picker.Title = 'Select Vice City VR 0.5.5 or newer: reVC.exe'
            $picker.Filter = 'Vice City VR (reVC.exe)|reVC.exe'
            $picker.CheckFileExists = $true
            if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) {
                Write-Host 'Cancelled. Settings were not changed.'
                exit 0
            }
            $gameExe = $picker.FileName
            if ([IO.Path]::GetFileName($gameExe) -ine 'reVC.exe') {
                throw 'Select the Vice City VR executable named reVC.exe.'
            }
            $gameDir = [IO.Path]::GetDirectoryName($gameExe)
        } finally { $picker.Dispose() }
    }
    $ini = Join-Path $gameDir 'vr_settings.ini'
    Write-Host ''
    Write-Host 'Vice City VR - Cutscene Mode (0.5.5 or newer)'
    Write-Host "Game: $gameExe"
    Write-Host ''
    Write-Host '1 - STEREO: try this if cutscenes are black in the headset.'
    Write-Host '2 - CINEMA SCREEN: restore cutscenes on a flat virtual screen.'
    Write-Host '0 - Exit without changes.'
    Write-Host ''
    if (-not $mode) {
        do { $choice = Read-Host 'Choose 1, 2 or 0' } while ($choice -notin @('1', '2', '0'))
        if ($choice -eq '0') { exit 0 }
        $mode = if ($choice -eq '1') { 'stereo' } else { 'cinema' }
    }

    # The game saves its own settings; do not let it overwrite this choice on exit.
    foreach ($process in @(Get-Process -Name reVC, reVC_VR -ErrorAction SilentlyContinue)) {
        $processPath = $process.Path
        if (-not $processPath -or
            [IO.Path]::GetDirectoryName($processPath) -ieq $gameDir) {
            throw 'Close Vice City VR first, then run this switch again.'
        }
    }

    # Use the Windows INI API, as the game does, without rewriting other settings.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class VcCutsceneIni {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern uint GetPrivateProfileStringW(string section, string key,
        string defaultValue, StringBuilder value, uint size, string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WritePrivateProfileStringW(string section, string key,
        string value, string path);
}
'@
    function Read-CutsceneMode {
        $value = New-Object Text.StringBuilder 32
        [void][VcCutsceneIni]::GetPrivateProfileStringW('VR', 'CutsceneMode',
            'missing', $value, 32, $ini)
        return $value.ToString()
    }
    $wanted = if ($mode -eq 'stereo') { '1' } else { '0' }
    if ((Read-CutsceneMode) -eq $wanted) {
        Write-Host "Already set to $mode. Settings were not changed."
        exit 0
    }
    $backup = $null
    if ([IO.File]::Exists($ini)) {
        if (([IO.File]::GetAttributes($ini) -band [IO.FileAttributes]::ReadOnly) -ne 0) {
            throw 'vr_settings.ini is read-only. Remove its read-only attribute and try again.'
        }
        $backup = $ini + '.cutscene-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') +
            '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.bak'
        [IO.File]::Copy($ini, $backup, $false)
        Write-Host "Backup: $backup"
    }
    try {
        if (-not [VcCutsceneIni]::WritePrivateProfileStringW('VR', 'CutsceneMode', $wanted, $ini)) {
            $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "Windows could not save the setting (error $code). Check folder write access."
        }
        if ((Read-CutsceneMode) -ne $wanted) {
            throw 'The saved setting could not be verified.'
        }
    } catch {
        if ($backup) { [IO.File]::Copy($backup, $ini, $true) }
        throw
    }
    Write-Host ''
    Write-Host "Saved: [VR] CutsceneMode=$wanted ($mode)" -ForegroundColor Green
    Write-Host "Settings: $ini"
    Write-Host 'Start the game normally. Run this switch again to change the mode back.'
    exit 0
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
