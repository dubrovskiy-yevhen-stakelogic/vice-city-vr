[CmdletBinding()]
param(
    [ValidateSet("PCToQuest", "QuestToPC")]
    [string]$Direction = "PCToQuest",
    [string]$PcGameDirectory,
    [string]$AdbPath,
    [string]$Serial,
    [string]$LogPath
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$script:transcriptStarted = $false
$exportToPc = $Direction -eq "QuestToPC"
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $env:TEMP "ViceCityVR-$Direction.log"
}

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

function Resolve-PcExportDirectory([string]$RequestedPath) {
    $selected = $RequestedPath
    if ([string]::IsNullOrWhiteSpace($selected)) {
        $besideWizard = Split-Path -Parent $PSScriptRoot
        if (Test-Path -LiteralPath (Join-Path $besideWizard "reVC.exe") -PathType Leaf) {
            $selected = $besideWizard
        } else {
            $selected = Select-Folder "Select your installed PC Vice City VR folder (containing reVC.exe), or its save folder"
        }
    }
    if ([string]::IsNullOrWhiteSpace($selected) -or
        -not (Test-Path -LiteralPath $selected -PathType Container)) {
        throw "No existing PC game or save folder was selected."
    }
    $selected = (Resolve-Path -LiteralPath $selected).Path
    if (Test-Path -LiteralPath (Join-Path $selected "reVC.exe") -PathType Leaf) {
        $selected = Join-Path $selected "userfiles"
    } elseif ((Split-Path -Leaf $selected) -notin @("userfiles", "GTA Vice City User Files") -and
        -not (Get-ChildItem -LiteralPath $selected -Filter "GTAVCsf*.b" -File | Select-Object -First 1)) {
        throw "Select the installed game folder containing reVC.exe, or an actual PC save folder."
    }
    return $selected
}

function Get-OccupiedQuestSlots([string]$Adb, [string[]]$DeviceArguments) {
    for ($slot = 1; $slot -le 8; $slot++) {
        $output = & $Adb @DeviceArguments shell content query --uri "content://com.miamivr.quest.saves/slot/$slot" --projection _size 2>&1
        $response = $output -join "`n"
        if ($LASTEXITCODE -ne 0 -or $response -notmatch '(?m)^Row: \d+ _size=(\d+)\s*$') {
            throw "Cannot read Quest slot $slot. Check that standalone Vice City VR is installed and supports save transfer. Update the standalone app if necessary. ADB said: $response"
        }
        $size = [int64]$Matches[1]
        if ($size -gt 0) {
            Write-Host "  Quest slot ${slot}: $size bytes"
            $slot
        }
    }
}

function Assert-OfficialDownloadedAdb([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $subject = if ($null -ne $signature.SignerCertificate) {
        $signature.SignerCertificate.Subject
    } else { "" }
    if ($signature.Status -ne "Valid" -or
        $subject -notmatch '(?i)(^|,\s*)(CN|O)=Google LLC(,|$)') {
        throw "Downloaded adb.exe does not have a valid Google LLC Authenticode signature (status '$($signature.Status)', subject '$subject'). Delete the cached ZIP and try again, or supply a trusted -AdbPath."
    }
    $versionOutput = & $Path version 2>&1
    if ($LASTEXITCODE -ne 0 -or
        ($versionOutput | Out-String) -notmatch 'Android Debug Bridge version') {
        throw "The signed downloaded executable did not identify itself as Android Debug Bridge."
    }
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
    $extract = Join-Path $toolRoot `
        (".platform-tools-download." + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
    if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) {
        Invoke-WebRequest -UseBasicParsing `
            -Uri "https://dl.google.com/android/repository/platform-tools-latest-windows.zip" `
            -OutFile $zip
    }
    try {
        Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
        $downloaded = Join-Path $extract "platform-tools\adb.exe"
        if (-not (Test-Path -LiteralPath $downloaded -PathType Leaf)) {
            throw "The official platform-tools archive had an unexpected layout."
        }
        Assert-OfficialDownloadedAdb $downloaded
        $final = Join-Path $toolRoot "platform-tools"
        if (Test-Path -LiteralPath $final) {
            Remove-Item -LiteralPath $final -Recurse -Force
        }
        Move-Item -LiteralPath (Join-Path $extract "platform-tools") -Destination $final
    } finally {
        if (Test-Path -LiteralPath $extract) {
            Remove-Item -LiteralPath $extract -Recurse -Force
        }
    }
    return (Join-Path $final "adb.exe")
}

try {
    Start-Transcript -LiteralPath $LogPath -Force | Out-Null
    $script:transcriptStarted = $true
    Write-Host "===============================================================" -ForegroundColor Cyan
    Write-Host " VICE CITY VR - SAVE TRANSFER: $Direction" -ForegroundColor Cyan
    Write-Host "===============================================================" -ForegroundColor Cyan
    Write-Host "This converts compatible Vice City VR saves, backs up destination slots before replacing them, and verifies each written slot."
    Write-Host "Close Vice City VR on both PC and Quest. Connect the Quest by USB and accept the USB debugging prompt inside the headset." -ForegroundColor Yellow

    $transferScript = Join-Path $PSScriptRoot "saves\transfer-vr-save.ps1"
    if (-not (Test-Path -LiteralPath $transferScript -PathType Leaf)) {
        throw "Required transfer engine is missing: $transferScript"
    }
    if ($exportToPc) {
        $pcSaveDirectory = Resolve-PcExportDirectory $PcGameDirectory
    } else {
        $pcSaveDirectory = Resolve-PcSaveDirectory $PcGameDirectory
    }
    $adb = Resolve-OrInstall-Adb $AdbPath

    Write-Heading "PC save folder: $pcSaveDirectory"
    $slots = [System.Collections.Generic.List[int]]::new()
    for ($slot = 1; -not $exportToPc -and $slot -le 8; $slot++) {
        $save = Join-Path $pcSaveDirectory "GTAVCsf$slot.b"
        if (Test-Path -LiteralPath $save -PathType Leaf) {
            $slots.Add($slot)
            $item = Get-Item -LiteralPath $save
            Write-Host ("  Slot {0}: {1} bytes, {2}" -f $slot, $item.Length, $item.LastWriteTime)
        }
    }
    if (-not $exportToPc -and $slots.Count -eq 0) { throw "No PC save slots were found." }

    Write-Heading "Quest connection"
    $adbArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($Serial)) { $adbArguments += @("-s", $Serial) }
    $deviceState = & $adb @adbArguments get-state 2>&1
    if ($LASTEXITCODE -ne 0 -or ($deviceState | Out-String).Trim() -ne "device") {
        throw "No authorized Quest was detected. Reconnect USB, put on the headset, accept 'Allow USB debugging', then run this wizard again. ADB said: $($deviceState -join ' ')"
    }
    Write-Host "Authorized Quest detected." -ForegroundColor Green

    if ($exportToPc) {
        foreach ($occupied in @(Get-OccupiedQuestSlots $adb $adbArguments)) {
            $slots.Add($occupied)
            if (Test-Path -LiteralPath (Join-Path $pcSaveDirectory "GTAVCsf$occupied.b")) {
                Write-Host "    PC slot $occupied will be backed up and replaced." -ForegroundColor Yellow
            }
        }
        if ($slots.Count -eq 0) { throw "No occupied Quest save slots were found. Save your game on standalone first." }
        Write-Host "Destination: $pcSaveDirectory" -ForegroundColor Yellow
        Write-Host "If PCVR saves to Documents instead, cancel and select that save folder using -PcGameDirectory."
    }

    Write-Host ""
    $destination = if ($exportToPc) { "PC" } else { "Quest" }
    $answer = Read-Host "Copy all listed slots to $destination now? Close both games first. Existing destination slots will be backed up [y/N]"
    if ($answer -notmatch '^(?i)y(es)?$') {
        throw "Transfer cancelled by the user before any save was written."
    }

    if ($exportToPc) {
        if (Get-Process -Name reVC -ErrorAction SilentlyContinue) {
            throw "Close reVC before importing saves to PC, then run the wizard again."
        }
        New-Item -ItemType Directory -Path $pcSaveDirectory -Force | Out-Null
    }

    $childPowerShell = Join-Path $env:SystemRoot `
        "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $childPowerShell -PathType Leaf)) {
        throw "Windows PowerShell was not found at the expected system path."
    }
    foreach ($slot in $slots) {
        Write-Heading "Transferring slot $slot of $($slots.Count)"
        $arguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $transferScript,
            "-Mode", $(if ($exportToPc) { "Export" } else { "Import" }), "-Slot", $slot,
            "-PcSaveDirectory", $pcSaveDirectory, "-AdbPath", $adb
        )
        if (-not [string]::IsNullOrWhiteSpace($Serial)) { $arguments += @("-Serial", $Serial) }
        & $childPowerShell @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Slot $slot failed. Earlier verified slots were kept; the failed slot was not silently accepted."
        }
    }

    Write-Host ""
    Write-Host "SUCCESS: all $($slots.Count) save slot(s) were converted, copied and verified on $destination." -ForegroundColor Green
    Write-Host "Backups of replaced slots: $pcSaveDirectory\MiamiVR-save-backups"
    if ($exportToPc) {
        Write-Host "Quest saves were not replaced. Start PCVR and load the transferred slot."
    } else {
        Write-Host "You can disconnect USB and start Vice City VR on the headset."
    }
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
