[CmdletBinding()]
param(
    [string]$PcGameDirectory,
    [string]$AdbPath,
    [string]$Serial,
    [string]$LogPath = (Join-Path $env:TEMP "ViceCityVR-PC-Saves-To-Quest.log")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$script:transcriptStarted = $false

function Write-Heading([string]$Text) {
    Write-Host ""
    Write-Host $Text -ForegroundColor Cyan
}

function Select-Folder([string]$Description) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = $Description
        $dialog.ShowNewFolderButton = $false
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.SelectedPath
        }
    } catch {
        Write-Warning "The folder picker could not open: $($_.Exception.Message)"
    }
    return (Read-Host $Description)
}

function Resolve-PcSaveDirectory([string]$RequestedPath) {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $candidates.Add($RequestedPath)
        $candidates.Add((Join-Path $RequestedPath "userfiles"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $candidates.Add((Join-Path $env:USERPROFILE "Documents\GTA Vice City User Files"))
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate) -or
            -not (Test-Path -LiteralPath $candidate -PathType Container)) { continue }
        if (Get-ChildItem -LiteralPath $candidate -Filter "GTAVCsf*.b" -File `
                -ErrorAction SilentlyContinue | Select-Object -First 1) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $selected = Select-Folder "Select your PC Vice City VR folder, or its userfiles folder"
    if ([string]::IsNullOrWhiteSpace($selected)) {
        throw "No PC folder was selected."
    }
    $selected = [IO.Path]::GetFullPath($selected)
    $selectedCandidates = @($selected, (Join-Path $selected "userfiles"))
    foreach ($candidate in $selectedCandidates) {
        if ((Test-Path -LiteralPath $candidate -PathType Container) -and
            (Get-ChildItem -LiteralPath $candidate -Filter "GTAVCsf*.b" -File `
                -ErrorAction SilentlyContinue | Select-Object -First 1)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "No GTAVCsf1.b ... GTAVCsf8.b files were found in '$selected' or its userfiles folder. Start PC Vice City VR and create a save first."
}

function Resolve-OrInstall-Adb([string]$RequestedPath) {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) { $candidates.Add($RequestedPath) }
    $candidates.Add((Join-Path $PSScriptRoot "saves\platform-tools\adb.exe"))
    $candidates.Add("C:\VCVRBuild\.android-sdk\platform-tools\adb.exe")
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $candidates.Add((Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"))
        $candidates.Add((Join-Path $env:LOCALAPPDATA "Programs\SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $candidates.Add((Join-Path $env:APPDATA "SideQuest\platform-tools\adb.exe"))
    }
    foreach ($root in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT)) {
        if (-not [string]::IsNullOrWhiteSpace($root)) {
            $candidates.Add((Join-Path $root "platform-tools\adb.exe"))
        }
    }
    $command = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { $candidates.Add($command.Source) }
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    Write-Host "ADB was not found. The wizard can download Google's official Android platform-tools (about 8 MB)." -ForegroundColor Yellow
    $answer = Read-Host "Download it now? [Y/n]"
    if (-not [string]::IsNullOrWhiteSpace($answer) -and $answer -notmatch '^[Yy]') {
        throw "ADB is required to transfer saves to the Quest."
    }
    $toolRoot = Join-Path $PSScriptRoot "saves"
    $zip = Join-Path $toolRoot "platform-tools-latest-windows.zip"
    $extract = Join-Path $toolRoot ".platform-tools-download"
    New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
    if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) {
        Invoke-WebRequest -UseBasicParsing `
            -Uri "https://dl.google.com/android/repository/platform-tools-latest-windows.zip" `
            -OutFile $zip
    }
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
    $downloaded = Join-Path $extract "platform-tools\adb.exe"
    if (-not (Test-Path -LiteralPath $downloaded -PathType Leaf)) {
        throw "The official platform-tools archive had an unexpected layout."
    }
    $final = Join-Path $toolRoot "platform-tools"
    if (Test-Path -LiteralPath $final) { Remove-Item -LiteralPath $final -Recurse -Force }
    Move-Item -LiteralPath (Join-Path $extract "platform-tools") -Destination $final
    Remove-Item -LiteralPath $extract -Recurse -Force
    return (Join-Path $final "adb.exe")
}

try {
    Start-Transcript -LiteralPath $LogPath -Force | Out-Null
    $script:transcriptStarted = $true
    Write-Host "===============================================================" -ForegroundColor Cyan
    Write-Host " VICE CITY VR - COPY ALL PC SAVES TO QUEST" -ForegroundColor Cyan
    Write-Host "===============================================================" -ForegroundColor Cyan
    Write-Host "This converts compatible Win64 Vice City VR saves, backs up any Quest slots that will be replaced, and verifies every written slot."
    Write-Host "Close Vice City VR on both PC and Quest. Connect the Quest by USB and accept the USB debugging prompt inside the headset." -ForegroundColor Yellow

    $transferScript = Join-Path $PSScriptRoot "saves\transfer-vr-save.ps1"
    if (-not (Test-Path -LiteralPath $transferScript -PathType Leaf)) {
        throw "Required transfer engine is missing: $transferScript"
    }
    $pcSaveDirectory = Resolve-PcSaveDirectory $PcGameDirectory
    $adb = Resolve-OrInstall-Adb $AdbPath

    Write-Heading "PC saves found in: $pcSaveDirectory"
    $slots = [System.Collections.Generic.List[int]]::new()
    for ($slot = 1; $slot -le 8; $slot++) {
        $save = Join-Path $pcSaveDirectory "GTAVCsf$slot.b"
        if (Test-Path -LiteralPath $save -PathType Leaf) {
            $slots.Add($slot)
            $item = Get-Item -LiteralPath $save
            Write-Host ("  Slot {0}: {1} bytes, {2}" -f $slot, $item.Length, $item.LastWriteTime)
        }
    }
    if ($slots.Count -eq 0) { throw "No PC save slots were found." }

    Write-Heading "Quest connection"
    $adbArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($Serial)) { $adbArguments += @("-s", $Serial) }
    $deviceState = & $adb @adbArguments get-state 2>&1
    if ($LASTEXITCODE -ne 0 -or ($deviceState | Out-String).Trim() -ne "device") {
        throw "No authorized Quest was detected. Reconnect USB, put on the headset, accept 'Allow USB debugging', then run this wizard again. ADB said: $($deviceState -join ' ')"
    }
    Write-Host "Authorized Quest detected." -ForegroundColor Green

    Write-Host ""
    $answer = Read-Host "Copy all listed PC slots to the Quest now? Existing Quest slots will be backed up first [Y/n]"
    if (-not [string]::IsNullOrWhiteSpace($answer) -and $answer -notmatch '^[Yy]') {
        throw "Transfer cancelled by the user before any save was written."
    }

    foreach ($slot in $slots) {
        Write-Heading "Transferring slot $slot of $($slots.Count)"
        $arguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $transferScript,
            "-Mode", "Import", "-Slot", $slot,
            "-PcSaveDirectory", $pcSaveDirectory, "-AdbPath", $adb
        )
        if (-not [string]::IsNullOrWhiteSpace($Serial)) { $arguments += @("-Serial", $Serial) }
        & powershell.exe @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Slot $slot failed. Earlier verified slots were kept; the failed slot was not silently accepted."
        }
    }

    Write-Host ""
    Write-Host "SUCCESS: all $($slots.Count) PC save slot(s) were converted, copied and verified on the Quest." -ForegroundColor Green
    Write-Host "Quest backups were created by the transfer engine before replacing occupied slots."
    Write-Host "You can disconnect USB and start Vice City VR on the headset."
    Write-Host "Diagnostic log: $LogPath"
    exit 0
} catch {
    Write-Host ""
    Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
    Write-Host "No failure was treated as success. Fix the reported item and run the wizard again."
    Write-Host "Diagnostic log: $LogPath"
    exit 1
} finally {
    if ($script:transcriptStarted) {
        try { Stop-Transcript | Out-Null } catch {}
    }
}
