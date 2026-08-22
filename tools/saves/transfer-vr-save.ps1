[CmdletBinding()]
param(
    [ValidateSet("Import", "Export")]
    [string]$Mode,

    [ValidateRange(1, 8)]
    [int]$Slot,

    [string]$PcSaveDirectory,

    [string]$AdbPath,

    [string]$Serial,

    [switch]$Help
)

$ErrorActionPreference = "Stop"

trap {
    Write-Host ("Error: " + $_.Exception.Message) -ForegroundColor Red
    exit 1
}

$packageId = "com.miamivr.quest"
$providerRoot = "content://$packageId.saves/slot"

function Show-Usage {
    Write-Host @"
Transfer Vice City VR saves between the PC and Quest builds.

Usage:
  .\transfer-vr-save.ps1 -Mode Import -Slot 1 [-PcSaveDirectory PATH]
  .\transfer-vr-save.ps1 -Mode Export -Slot 1 [-PcSaveDirectory PATH]

Modes:
  Import  Convert GTAVCsfN.b from PC format and write it to Quest slot N.
  Export  Read Quest slot N, convert it, and write PC file GTAVCsfN.b.

Options:
  -Slot 1..8             Save slot to transfer.
  -PcSaveDirectory PATH  Directory containing the PC GTAVCsfN.b files.
  -AdbPath PATH          Optional explicit path to adb.exe.
  -Serial SERIAL         Select a Quest when multiple devices are connected.

If the slot or PC save directory is omitted, the script tries common local
locations and then asks for the missing value. Existing saves are backed up
under MiamiVR-save-backups before they are overwritten.
"@
}

function Resolve-AdbExecutable {
    param([string]$RequestedPath)

    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $candidates.Add($RequestedPath)
    }

    $candidates.Add((Join-Path $PSScriptRoot "platform-tools\adb.exe"))
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $candidates.Add((Join-Path $env:APPDATA "SideQuest\platform-tools\adb.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $candidates.Add((Join-Path $env:LOCALAPPDATA "Programs\SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe"))
        $candidates.Add((Join-Path $env:LOCALAPPDATA "Programs\SideQuest\resources\app.asar.unpacked\resources\app\platform-tools\adb.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        $candidates.Add((Join-Path $env:ANDROID_SDK_ROOT "platform-tools\adb.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        $candidates.Add((Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"))
    }

    $adbCommand = Get-Command "adb.exe" -ErrorAction SilentlyContinue
    if ($null -ne $adbCommand) {
        $candidates.Add($adbCommand.Source)
    }

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "adb.exe was not found. Install Android platform-tools or SideQuest, put adb.exe on PATH, or pass -AdbPath."
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [switch]$CaptureOutput
    )

    $fullArguments = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        $fullArguments.Add("-s")
        $fullArguments.Add($Serial)
    }
    foreach ($argument in $Arguments) {
        $fullArguments.Add($argument)
    }

    if ($CaptureOutput) {
        $output = & $script:adbExecutable @fullArguments 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "adb failed: adb $($fullArguments -join ' ')`n$($output -join [Environment]::NewLine)"
        }
        return $output
    }

    & $script:adbExecutable @fullArguments
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed: adb $($fullArguments -join ' ')"
    }
}

function Read-QuestSlotToFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Uri,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    Invoke-Adb -Arguments @(
        "shell",
        "content read --uri '$Uri' > '$script:remoteTemporarySave'"
    )
    Invoke-Adb -Arguments @("pull", $script:remoteTemporarySave, $Destination)
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf) -or
        (Get-Item -LiteralPath $Destination).Length -eq 0) {
        throw "Quest returned an empty save for $Uri."
    }
}

function Write-QuestSlotFromFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Uri,

        [Parameter(Mandatory = $true)]
        [string]$Source
    )

    Invoke-Adb -Arguments @("push", $Source, $script:remoteTemporarySave)
    Invoke-Adb -Arguments @(
        "shell",
        "content write --uri '$Uri' < '$script:remoteTemporarySave'"
    )
}

function Convert-Save {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("pc-to-quest", "quest-to-pc")]
        [string]$Direction,

        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $saveSize = 201828
    $garagePayloadSize = 7876
    $fixedGaragePrefix = 44
    $storedCarCount = 48
    $garageCount = 32
    $garageSize = 184
    $garageStoredCarOffset = 144
    $currentWin64InnerBlocks = @(
        [PSCustomObject]@{ Index = 7; Name = "Cranes"; Size = 1160 },
        [PSCustomObject]@{ Index = 8; Name = "Pickups"; Size = 21588 },
        [PSCustomObject]@{ Index = 9; Name = "Phone"; Size = 4408 },
        [PSCustomObject]@{ Index = 18; Name = "PlayerInfo"; Size = 416 }
    )

    [byte[]]$data = [IO.File]::ReadAllBytes($Source)
    if ($data.Length -ne $saveSize) {
        throw "Unsupported save size $($data.Length); expected $saveSize bytes."
    }

    [uint64]$calculatedChecksum = 0
    for ($index = 0; $index -lt $data.Length - 4; $index++) {
        $calculatedChecksum = ($calculatedChecksum + $data[$index]) -band [uint64]4294967295
    }
    [uint32]$storedChecksum = [BitConverter]::ToUInt32($data, $data.Length - 4)
    if ([uint32]$calculatedChecksum -ne $storedChecksum) {
        throw ("Save checksum mismatch: file={0:X8}, calculated={1:X8}." -f
            $storedChecksum, [uint32]$calculatedChecksum)
    }

    $checksumOffset = $data.Length - 4
    $outerOffset = 0
    $blockCount = 0
    $garagePayloadOffset = -1
    $innerBlockSizes = @{}
    while ($outerOffset + 4 -le $checksumOffset -and $blockCount -lt 40) {
        [uint32]$declaredSize = [BitConverter]::ToUInt32($data, $outerOffset)
        $payloadOffset = $outerOffset + 4
        $alignedSize = ([int64]$declaredSize + 3) -band -4
        $blockEnd = $payloadOffset + $alignedSize
        if ($blockEnd -gt $checksumOffset) {
            throw "Save block $blockCount overruns the file."
        }
        if ($declaredSize -ge 4) {
            $innerBlockSizes[$blockCount] = [BitConverter]::ToUInt32($data, $payloadOffset)
        }
        if ($declaredSize -ge 4 -and
            [BitConverter]::ToUInt32($data, $payloadOffset) -eq $garagePayloadSize) {
            if ($garagePayloadOffset -ne -1) {
                throw "The save contains more than one garages block."
            }
            $garagePayloadOffset = $payloadOffset + 4
        }
        $outerOffset = $blockEnd
        $blockCount++
        if ($outerOffset -eq $checksumOffset) {
            break
        }
    }
    if ($outerOffset -ne $checksumOffset -or $garagePayloadOffset -lt 0) {
        throw "Unsupported save signature; expected the current Vice City VR save format."
    }

    if ($Direction -eq "pc-to-quest") {
        $signatureMismatches = [System.Collections.Generic.List[string]]::new()
        foreach ($signature in $currentWin64InnerBlocks) {
            if (-not $innerBlockSizes.ContainsKey($signature.Index)) {
                $signatureMismatches.Add(
                    "$($signature.Name) block $($signature.Index): missing (expected inner size $($signature.Size))")
                continue
            }
            [uint32]$observedSize = $innerBlockSizes[$signature.Index]
            if ($observedSize -ne $signature.Size) {
                $signatureMismatches.Add(
                    "$($signature.Name) block $($signature.Index): expected inner size $($signature.Size), got $observedSize")
            }
        }
        if ($signatureMismatches.Count -gt 0) {
            throw ("Not a current Win64 Vice City VR save; original retail and legacy x86 saves " +
                "cannot be imported safely (" + ($signatureMismatches -join "; ") + ").")
        }
    }

    [byte[]]$converted = New-Object byte[] $garagePayloadSize
    [Buffer]::BlockCopy($data, $garagePayloadOffset, $converted, 0, $fixedGaragePrefix)
    $sourceCursor = $fixedGaragePrefix
    $destinationCursor = $fixedGaragePrefix

    if ($Direction -eq "pc-to-quest") {
        for ($car = 0; $car -lt $storedCarCount; $car++) {
            [Buffer]::BlockCopy($data, $garagePayloadOffset + $sourceCursor,
                $converted, $destinationCursor, 29)
            [Buffer]::BlockCopy($data, $garagePayloadOffset + $sourceCursor + 32,
                $converted, $destinationCursor + 29, 6)
            $sourceCursor += 40
            $destinationCursor += 36
        }
        for ($garage = 0; $garage -lt $garageCount; $garage++) {
            $sourceGarage = $garagePayloadOffset + $sourceCursor
            [Buffer]::BlockCopy($data, $sourceGarage,
                $converted, $destinationCursor, $garageStoredCarOffset)
            [Buffer]::BlockCopy($data, $sourceGarage + $garageStoredCarOffset,
                $converted, $destinationCursor + $garageStoredCarOffset, 29)
            [Buffer]::BlockCopy($data, $sourceGarage + $garageStoredCarOffset + 32,
                $converted, $destinationCursor + $garageStoredCarOffset + 29, 6)
            $sourceCursor += $garageSize
            $destinationCursor += $garageSize
        }
        if ($sourceCursor + 24 -ne $garagePayloadSize) {
            throw "Unexpected Win64 garages layout."
        }
    } else {
        for ($car = 0; $car -lt $storedCarCount; $car++) {
            [Buffer]::BlockCopy($data, $garagePayloadOffset + $sourceCursor,
                $converted, $destinationCursor, 29)
            [Buffer]::BlockCopy($data, $garagePayloadOffset + $sourceCursor + 29,
                $converted, $destinationCursor + 32, 6)
            $sourceCursor += 36
            $destinationCursor += 40
        }
        for ($garage = 0; $garage -lt $garageCount; $garage++) {
            $sourceGarage = $garagePayloadOffset + $sourceCursor
            [Buffer]::BlockCopy($data, $sourceGarage,
                $converted, $destinationCursor, $garageStoredCarOffset)
            [Buffer]::BlockCopy($data, $sourceGarage + $garageStoredCarOffset,
                $converted, $destinationCursor + $garageStoredCarOffset, 29)
            [Buffer]::BlockCopy($data, $sourceGarage + $garageStoredCarOffset + 29,
                $converted, $destinationCursor + $garageStoredCarOffset + 32, 6)
            $sourceCursor += $garageSize
            $destinationCursor += $garageSize
        }
        if ($sourceCursor + 216 -ne $garagePayloadSize) {
            throw "Unexpected Quest garages layout."
        }
    }

    [Buffer]::BlockCopy($converted, 0, $data, $garagePayloadOffset, $garagePayloadSize)
    [uint64]$newChecksum = 0
    for ($index = 0; $index -lt $data.Length - 4; $index++) {
        $newChecksum = ($newChecksum + $data[$index]) -band [uint64]4294967295
    }
    [byte[]]$checksumBytes = [BitConverter]::GetBytes([uint32]$newChecksum)
    [Buffer]::BlockCopy($checksumBytes, 0, $data, $data.Length - 4, 4)

    $destinationDirectory = Split-Path -Parent $Destination
    if (-not [string]::IsNullOrWhiteSpace($destinationDirectory)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    [IO.File]::WriteAllBytes($Destination, $data)
    Write-Host "Converted $Source -> $Destination ($Direction, $($data.Length) bytes)"
}

function Resolve-PcDirectory {
    param(
        [string]$RequestedPath,
        [string]$TransferMode,
        [int]$SaveSlot
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Container)) {
            throw "PC save directory does not exist: $RequestedPath"
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    $candidates.Add((Join-Path $PSScriptRoot "..\userfiles"))
    $candidates.Add((Join-Path $PSScriptRoot "..\..\userfiles"))
    $candidates.Add((Join-Path $PSScriptRoot "..\..\..\userfiles"))
    $candidates.Add((Join-Path (Get-Location).Path "userfiles"))
    $candidates.Add((Get-Location).Path)
    $documents = [Environment]::GetFolderPath("MyDocuments")
    if (-not [string]::IsNullOrWhiteSpace($documents)) {
        $candidates.Add((Join-Path $documents "GTA Vice City User Files"))
    }

    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
            continue
        }
        $resolved = (Resolve-Path -LiteralPath $candidate).Path
        $requestedSaveExists = Test-Path -LiteralPath (Join-Path $resolved "GTAVCsf$SaveSlot.b") -PathType Leaf
        $looksLikePcSaveDirectory =
            (Split-Path -Leaf $resolved).Equals("userfiles", [StringComparison]::OrdinalIgnoreCase) -or
            $null -ne (Get-ChildItem -LiteralPath $resolved -Filter "GTAVCsf*.b" -File -ErrorAction SilentlyContinue | Select-Object -First 1)
        if (($TransferMode -eq "Import" -and $requestedSaveExists) -or
            ($TransferMode -eq "Export" -and $looksLikePcSaveDirectory)) {
            return $resolved
        }
    }

    $entered = Read-Host "Enter the PC save directory containing GTAVCsfN.b"
    if ([string]::IsNullOrWhiteSpace($entered) -or
        -not (Test-Path -LiteralPath $entered -PathType Container)) {
        throw "PC save directory does not exist: $entered"
    }
    return (Resolve-Path -LiteralPath $entered).Path
}

function Get-QuestSlotSize {
    param([string]$Uri)

    $output = @(Invoke-Adb -Arguments @(
        "shell", "content", "query", "--uri", $Uri,
        "--projection", "_size"
    ) -CaptureOutput)
    $text = $output -join "`n"
    if ($text -match "_size=(\d+)") {
        return [int64]$Matches[1]
    }
    return 0L
}

function New-BackupDirectory {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $path = Join-Path $script:pcDirectory "MiamiVR-save-backups\$timestamp-$($Mode.ToLowerInvariant())-slot$Slot"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

if ($Help) {
    Show-Usage
    exit 0
}
if ([string]::IsNullOrWhiteSpace($Mode)) {
    Show-Usage
    throw "-Mode Import or -Mode Export is required."
}
if ($Slot -eq 0) {
    $enteredSlot = Read-Host "Enter the save slot to transfer (1-8)"
    $parsedSlot = 0
    if (-not [int]::TryParse($enteredSlot, [ref]$parsedSlot) -or
        $parsedSlot -lt 1 -or $parsedSlot -gt 8) {
        throw "Save slot must be between 1 and 8."
    }
    $Slot = $parsedSlot
}
$script:pcDirectory = Resolve-PcDirectory -RequestedPath $PcSaveDirectory -TransferMode $Mode -SaveSlot $Slot
$pcSaveName = "GTAVCsf$Slot.b"
$pcSavePath = Join-Path $script:pcDirectory $pcSaveName
$slotUri = "$providerRoot/$Slot"
$script:adbExecutable = Resolve-AdbExecutable -RequestedPath $AdbPath
$script:temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("MiamiVR-save-transfer-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $script:temporaryRoot | Out-Null
$script:remoteTemporarySave = "/data/local/tmp/miamivr-save-$Slot-$([Guid]::NewGuid().ToString('N')).b"

try {
    Write-Host "Using adb: $script:adbExecutable"
    Write-Host "PC saves: $script:pcDirectory"
    Write-Host "Quest URI: $slotUri"

    $deviceOutput = @(Invoke-Adb -Arguments @("get-state") -CaptureOutput)
    if (($deviceOutput | Select-Object -Last 1).ToString().Trim() -ne "device") {
        throw "Quest is not ready. Put on the headset, accept USB debugging, and retry."
    }
    $packageOutput = @(Invoke-Adb -Arguments @("shell", "pm", "path", $packageId) -CaptureOutput)
    if (-not ($packageOutput | Where-Object { $_.ToString().Trim().StartsWith("package:") })) {
        throw "Vice City VR is not installed on the connected Quest."
    }

    Write-Host "Stopping Vice City VR before save transfer..."
    Invoke-Adb -Arguments @("shell", "am", "force-stop", $packageId)

    if ($Mode -eq "Import") {
        if (-not (Test-Path -LiteralPath $pcSavePath -PathType Leaf)) {
            throw "PC save was not found: $pcSavePath"
        }

        $questInput = Join-Path $script:temporaryRoot "$Slot.quest.b"
        Convert-Save -Direction "pc-to-quest" -Source $pcSavePath -Destination $questInput

        $remoteSize = Get-QuestSlotSize -Uri $slotUri
        if ($remoteSize -gt 0) {
            $backupDirectory = New-BackupDirectory
            $nativeBackup = Join-Path $backupDirectory "$Slot.quest-native.b"
            $pcBackup = Join-Path $backupDirectory $pcSaveName
            Write-Host "Backing up existing Quest slot $Slot to $backupDirectory"
            Read-QuestSlotToFile -Uri $slotUri -Destination $nativeBackup
            Convert-Save -Direction "quest-to-pc" -Source $nativeBackup -Destination $pcBackup
        }

        Write-Host "Writing PC save to Quest slot $Slot..."
        Write-QuestSlotFromFile -Uri $slotUri -Source $questInput

        $verificationCopy = Join-Path $script:temporaryRoot "$Slot.verify.b"
        Read-QuestSlotToFile -Uri $slotUri -Destination $verificationCopy
        $sourceHash = (Get-FileHash -LiteralPath $questInput -Algorithm SHA256).Hash
        $destinationHash = (Get-FileHash -LiteralPath $verificationCopy -Algorithm SHA256).Hash
        if ($sourceHash -ne $destinationHash) {
            throw "Quest save verification failed after writing slot $Slot."
        }

        Write-Host "Import complete: $pcSavePath -> Quest slot $Slot" -ForegroundColor Green
    } else {
        $questOutput = Join-Path $script:temporaryRoot "$Slot.quest.b"
        $convertedOutput = Join-Path $script:temporaryRoot $pcSaveName
        Write-Host "Reading Quest slot $Slot..."
        Read-QuestSlotToFile -Uri $slotUri -Destination $questOutput
        Convert-Save -Direction "quest-to-pc" -Source $questOutput -Destination $convertedOutput

        if (Test-Path -LiteralPath $pcSavePath -PathType Leaf) {
            $backupDirectory = New-BackupDirectory
            Write-Host "Backing up existing PC slot $Slot to $backupDirectory"
            Copy-Item -LiteralPath $pcSavePath -Destination (Join-Path $backupDirectory $pcSaveName)
        }

        Copy-Item -LiteralPath $convertedOutput -Destination $pcSavePath -Force
        Write-Host "Export complete: Quest slot $Slot -> $pcSavePath" -ForegroundColor Green
    }
} finally {
    if (-not [string]::IsNullOrWhiteSpace($script:remoteTemporarySave) -and
        -not [string]::IsNullOrWhiteSpace($script:adbExecutable)) {
        try {
            Invoke-Adb -Arguments @("shell", "rm", "-f", $script:remoteTemporarySave)
        } catch {
            Write-Warning "Could not remove temporary Quest file: $script:remoteTemporarySave"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($script:temporaryRoot) -and
        (Test-Path -LiteralPath $script:temporaryRoot -PathType Container)) {
        Remove-Item -LiteralPath $script:temporaryRoot -Recurse -Force
    }
}

