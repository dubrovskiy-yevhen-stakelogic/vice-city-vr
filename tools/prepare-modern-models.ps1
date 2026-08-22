# One-click preparation of the optional Modern overlay for the PC build from a
# legal GTA Vice City installation and two externally hosted source packs.
# The packs are downloaded for the individual user and are never bundled here.
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$WorkDir = "C:\VCVRBuild\modern-assets",
    [string]$HdArchive,
    [string]$ModsArchive,
    [switch]$AcceptDownloads,
    [switch]$NonInteractive,
    [switch]$DryRun,
    [string]$LogPath = (Join-Path $env:TEMP "ViceCityVR-Prepare-Modern-Models-PC.log")
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$wizardVersion = "0.5.2-pc-models-1"
$hdUrl = "https://drive.usercontent.google.com/download?id=1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj&export=download&confirm=t"
$modsUrl = "https://drive.usercontent.google.com/download?id=1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d&export=download&confirm=t"
$hdSize = 1878280127L
$modsSize = 2377186981L
$hdSha256 = "81A7962479752F3A07004A2E12964815435CAF4B0A0F637EE982460171A5D94C"
$modsSha256 = "F33031091DBE50A3BEDCEEFAF3FDE6F6DDD96F9841AABE1ECD3713818DED9725"
$builder = Join-Path $PSScriptRoot "modelsets\build-modern-modelset.ps1"
$script:transcriptStarted = $false

try {
    $logDirectory = Split-Path -Parent $LogPath
    if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    }
    Start-Transcript -Path $LogPath -Force | Out-Null
    $script:transcriptStarted = $true
} catch {
    Write-Host "Warning: diagnostic logging could not start: $($_.Exception.Message)" -ForegroundColor Yellow
}

function Stop-DiagnosticLog {
    if (-not $script:transcriptStarted) { return }
    try { Stop-Transcript | Out-Null } catch { }
    $script:transcriptStarted = $false
}

function Invoke-NativeChecked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage = "Command failed"
    )
    # Windows PowerShell 5.1 can convert ordinary native stderr (including
    # curl's progress meter) into ErrorRecords. Native exit codes are the
    # authoritative success signal here.
    $previousErrorAction = $ErrorActionPreference
    $nativeExitCode = -1
    try {
        $ErrorActionPreference = "Continue"
        & $FilePath @Arguments
        $nativeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($nativeExitCode -ne 0) {
        throw "$FailureMessage (exit code $nativeExitCode)."
    }
}

function Resolve-GameFolder {
    param([string]$Requested)
    if ([string]::IsNullOrWhiteSpace($Requested)) {
        if ($NonInteractive) { throw "-GameDir is required in non-interactive mode." }

        $pickerError = $null
        $dialog = $null
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = "Select your GTA Vice City / Vice City VR installation folder"
            $dialog.ShowNewFolderButton = $false
            $result = $dialog.ShowDialog()
            if ($result -eq [System.Windows.Forms.DialogResult]::OK) {
                $Requested = $dialog.SelectedPath
            } else {
                throw "No GTA Vice City folder was selected."
            }
        } catch {
            $pickerError = $_.Exception.Message
        } finally {
            if ($null -ne $dialog) { $dialog.Dispose() }
        }

        if ([string]::IsNullOrWhiteSpace($Requested)) {
            if ($pickerError -eq "No GTA Vice City folder was selected.") {
                throw $pickerError
            }
            Write-Host "Folder picker was unavailable. Paste the GTA Vice City folder path." -ForegroundColor Yellow
            $Requested = Read-Host "GTA Vice City folder"
        }
    }

    if ([string]::IsNullOrWhiteSpace($Requested) -or
        -not (Test-Path -LiteralPath $Requested -PathType Container)) {
        throw "The GTA Vice City folder does not exist: $Requested"
    }
    $resolved = (Resolve-Path -LiteralPath $Requested).Path.TrimEnd('\','/')
    foreach ($relative in @("models\gta3.img", "models\gta3.dir", "models\generic.txd")) {
        if (-not (Test-Path -LiteralPath (Join-Path $resolved $relative) -PathType Leaf)) {
            throw "The selected folder is not a complete GTA Vice City installation. Missing: $relative"
        }
    }
    return $resolved
}

function Assert-FreeSpace {
    param([string]$Game, [string]$Work)
    $workRoot = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Work))
    $gameRoot = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Game))
    $workDrive = New-Object IO.DriveInfo($workRoot)
    $gameDrive = New-Object IO.DriveInfo($gameRoot)
    if ($workRoot -eq $gameRoot) {
        if ($workDrive.AvailableFreeSpace -lt 24GB) {
            throw ("At least 24 GB free is required on {0} for downloads, extraction and the staged build; only {1:N1} GB is available." -f $workRoot, ($workDrive.AvailableFreeSpace / 1GB))
        }
    } else {
        if ($workDrive.AvailableFreeSpace -lt 14GB) {
            throw ("At least 14 GB free is required on {0} for downloads and extraction; only {1:N1} GB is available." -f $workRoot, ($workDrive.AvailableFreeSpace / 1GB))
        }
        if ($gameDrive.AvailableFreeSpace -lt 12GB) {
            throw ("At least 12 GB free is required on {0} for the staged model build; only {1:N1} GB is available." -f $gameRoot, ($gameDrive.AvailableFreeSpace / 1GB))
        }
    }
}

function Test-FileIdentity {
    param([string]$Path, [long]$Size, [string]$Sha256)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -ne $Size) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Sha256
}

function Ensure-Download {
    param(
        [string]$Name,
        [string]$Url,
        [string]$Destination,
        [long]$Size,
        [string]$Sha256,
        [string]$Provided
    )
    if (-not [string]::IsNullOrWhiteSpace($Provided)) {
        $providedPath = [IO.Path]::GetFullPath($Provided)
        Write-Host "Verifying supplied $Name archive..." -ForegroundColor Cyan
        if (-not (Test-FileIdentity -Path $providedPath -Size $Size -Sha256 $Sha256)) {
            throw "The supplied $Name archive does not match the tested size/SHA256: $providedPath"
        }
        return $providedPath
    }
    if (Test-FileIdentity -Path $Destination -Size $Size -Sha256 $Sha256) {
        Write-Host "Reusing verified download: $Destination" -ForegroundColor Green
        return $Destination
    }
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Force
    }
    $partial = "$Destination.partial"
    if ((Test-Path -LiteralPath $partial) -and
        (Get-Item -LiteralPath $partial).Length -gt $Size) {
        Remove-Item -LiteralPath $partial -Force
    }
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($null -eq $curl) {
        throw "Windows curl.exe was not found. Install current Windows updates, then rerun."
    }
    Write-Host "Downloading $Name ($([Math]::Round($Size / 1GB, 2)) GB). Interrupted downloads resume automatically..." -ForegroundColor Cyan
    Invoke-NativeChecked -FilePath $curl.Source -Arguments @(
        "--fail", "--location", "--retry", "5", "--retry-all-errors",
        "--continue-at", "-", "--output", $partial, $Url
    ) -FailureMessage "$Name download failed"
    Write-Host "Verifying $Name SHA256..." -ForegroundColor Cyan
    if (-not (Test-FileIdentity -Path $partial -Size $Size -Sha256 $Sha256)) {
        # A successful HTTP response can still be a Google error/HTML page.
        # Do not leave that file as a poisoned resume point for the next run.
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        throw "$Name download completed but failed the pinned size/SHA256 check. The invalid partial file was removed; rerun to retry cleanly."
    }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
    return $Destination
}

function Test-ExtractedContent {
    param([string]$Root, [string]$RelativeMarker)
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return $false }
    if (Test-Path -LiteralPath (Join-Path $Root $RelativeMarker)) { return $true }
    foreach ($folder in Get-ChildItem -LiteralPath $Root -Directory -Recurse -ErrorAction SilentlyContinue) {
        if (Test-Path -LiteralPath (Join-Path $folder.FullName $RelativeMarker)) {
            return $true
        }
    }
    return $false
}

function Ensure-Extracted {
    param(
        [string]$Name,
        [string]$Archive,
        [string]$Destination,
        [string]$Sha256,
        [string]$ContentMarker
    )
    $marker = Join-Path $Destination ".vcvr-source-sha256"
    if ((Test-Path -LiteralPath $marker -PathType Leaf) -and
        ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $Sha256) -and
        (Test-ExtractedContent -Root $Destination -RelativeMarker $ContentMarker)) {
        Write-Host "Reusing verified extraction: $Destination" -ForegroundColor Green
        return $Destination
    }
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $parent,$Destination -Force | Out-Null
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($null -eq $tar) {
        throw "Windows tar.exe was not found; it is required to extract the verified ZIP archives."
    }
    Write-Host "Extracting $Name..." -ForegroundColor Cyan
    Invoke-NativeChecked -FilePath $tar.Source -Arguments @(
        "-xf", $Archive, "-C", $Destination
    ) -FailureMessage "$Name extraction failed"
    if (-not (Test-ExtractedContent -Root $Destination -RelativeMarker $ContentMarker)) {
        throw "$Name was extracted but the expected '$ContentMarker' content was not found."
    }
    [IO.File]::WriteAllText($marker, $Sha256, (New-Object Text.UTF8Encoding($false)))
    return $Destination
}

function Get-ExpectedBuilderVersion {
    $builderText = Get-Content -LiteralPath $builder -Raw
    if ($builderText -notmatch '\$BuildScriptVersion\s*=\s*"([^"]+)"') {
        throw "Could not read the bundled Modern builder version from: $builder"
    }
    return $Matches[1]
}

function Test-CompletedOverlay {
    param([string]$Path, [string]$ExpectedBuilderVersion)
    try {
        if ([string]::IsNullOrWhiteSpace($Path) -or
            -not (Test-Path -LiteralPath $Path -PathType Container)) {
            return $false
        }
        $required = @(
            "vegetation_models.txt", "BUILD_INFO.txt",
            "models\gta3.img", "models\gta3.dir", "models\generic.txd",
            "models\generic\wheels.dff", "models\generic\wheels.txd"
        )
        foreach ($relative in $required) {
            $candidate = Join-Path $Path $relative
            if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -or
                (Get-Item -LiteralPath $candidate).Length -le 0) {
                return $false
            }
        }

        $img = Get-Item -LiteralPath (Join-Path $Path "models\gta3.img")
        $dir = Get-Item -LiteralPath (Join-Path $Path "models\gta3.dir")
        if ($img.Length -lt 1GB -or $dir.Length -lt 32 -or
            ($dir.Length % 32) -ne 0) {
            return $false
        }
        $vegetation = @(
            Get-Content -LiteralPath (Join-Path $Path "vegetation_models.txt") |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
        )
        if ($vegetation.Count -lt 20 -or $vegetation.Count -gt 512) {
            return $false
        }

        $buildInfo = Get-Content -LiteralPath (Join-Path $Path "BUILD_INFO.txt") -Raw
        if ($buildInfo -notmatch "(?m)^BuilderVersion=$([regex]::Escape($ExpectedBuilderVersion))\s*$" -or
            $buildInfo -notmatch "(?m)^ModernVegetationGeometry=REMOVED\s*$") {
            return $false
        }

        # Do not trust a stale marker alone. Verify the hashes written by the
        # builder before deciding that a multi-gigabyte active overlay is safe
        # to reuse without rebuilding.
        foreach ($relative in @(
            "models/gta3.img", "models/gta3.dir",
            "models/generic.txd", "vegetation_models.txt"
        )) {
            $match = [regex]::Match(
                $buildInfo,
                "(?m)^$([regex]::Escape($relative))=([A-Fa-f0-9]{64})\s*$")
            if (-not $match.Success) { return $false }
            $localPath = Join-Path $Path ($relative.Replace('/', '\'))
            $actual = (Get-FileHash -LiteralPath $localPath -Algorithm SHA256).Hash
            if ($actual -ne $match.Groups[1].Value) { return $false }
        }
        return $true
    } catch {
        return $false
    }
}

try {
    Write-Host "Vice City VR PC - one-click Modern model installer ($wizardVersion)" -ForegroundColor Green
    Write-Host "Required input: only a legal GTA Vice City installation."
    Write-Host "Two external source packs are downloaded, verified and cached locally."
    Write-Host "HD vegetation/palms are not downloaded or installed; they remain Classic."

    if (-not (Test-Path -LiteralPath $builder -PathType Leaf)) {
        throw "Required builder is missing: $builder"
    }

    $game = Resolve-GameFolder -Requested $GameDir
    $output = Join-Path $game "modelsets\modern"
    $expectedBuilderVersion = Get-ExpectedBuilderVersion

    Write-Host "Checking the installed Modern overlay..." -ForegroundColor Cyan
    $overlayReady = Test-CompletedOverlay -Path $output `
        -ExpectedBuilderVersion $expectedBuilderVersion

    if ($DryRun) {
        $work = [IO.Path]::GetFullPath($WorkDir)
        Assert-FreeSpace -Game $game -Work $work
        $downloads = Join-Path $work "downloads"
        $sources = Join-Path $work "sources"
        $hdDestination = if ([string]::IsNullOrWhiteSpace($HdArchive)) {
            Join-Path $downloads "GTA VC HD + Weapons.zip"
        } else { [IO.Path]::GetFullPath($HdArchive) }
        $modsDestination = if ([string]::IsNullOrWhiteSpace($ModsArchive)) {
            Join-Path $downloads "Mods.zip"
        } else { [IO.Path]::GetFullPath($ModsArchive) }
        $hdCached = Test-FileIdentity -Path $hdDestination -Size $hdSize -Sha256 $hdSha256
        $modsCached = Test-FileIdentity -Path $modsDestination -Size $modsSize -Sha256 $modsSha256
        $hdSource = Join-Path $sources "hd-pack"
        $modsSource = Join-Path $sources "mods-pack"
        $hdMarker = Join-Path $hdSource ".vcvr-source-sha256"
        $modsMarker = Join-Path $modsSource ".vcvr-source-sha256"
        $hdExtracted = (Test-Path -LiteralPath $hdMarker -PathType Leaf) -and
            ((Get-Content -LiteralPath $hdMarker -Raw).Trim() -eq $hdSha256) -and
            (Test-ExtractedContent -Root $hdSource -RelativeMarker "models\gta3.img")
        $modsExtracted = (Test-Path -LiteralPath $modsMarker -PathType Leaf) -and
            ((Get-Content -LiteralPath $modsMarker -Raw).Trim() -eq $modsSha256) -and
            (Test-ExtractedContent -Root $modsSource -RelativeMarker "Vehicles\gta3.img")

        Write-Host ""
        Write-Host "DRY RUN - no download, extraction, build, or game-file change was performed." -ForegroundColor Green
        Write-Host ("Game folder:          {0}" -f $game)
        Write-Host ("Modern overlay:       {0}" -f $(if ($overlayReady) { "READY" } else { "MISSING / REBUILD NEEDED" }))
        Write-Host ("HD archive:           {0}" -f $(if ($hdCached) { "VERIFIED" } else { "DOWNLOAD NEEDED" }))
        Write-Host ("Atmosphere archive:   {0}" -f $(if ($modsCached) { "VERIFIED" } else { "DOWNLOAD NEEDED" }))
        Write-Host ("HD extraction cache:  {0}" -f $(if ($hdExtracted) { "REUSABLE" } else { "EXTRACTION NEEDED" }))
        Write-Host ("Mods extraction cache:{0}" -f $(if ($modsExtracted) { " REUSABLE" } else { " EXTRACTION NEEDED" }))
        Write-Host "Diagnostic log: $LogPath"
        Stop-DiagnosticLog
        exit 0
    }

    if ($overlayReady) {
        Write-Host "Reusing the complete verified Modern overlay: $output" -ForegroundColor Green
        Write-Host "No download, extraction or rebuild is needed."
    } else {
        if (Test-Path -LiteralPath $output) {
            Write-Host "The existing Modern overlay is incomplete or from an older builder; a safe replacement will be prepared." -ForegroundColor Yellow
        }

        $work = [IO.Path]::GetFullPath($WorkDir)
        New-Item -ItemType Directory -Path $work -Force | Out-Null
        Assert-FreeSpace -Game $game -Work $work

        $downloads = Join-Path $work "downloads"
        $sources = Join-Path $work "sources"
        New-Item -ItemType Directory -Path $downloads,$sources -Force | Out-Null
        $hdDestination = Join-Path $downloads "GTA VC HD + Weapons.zip"
        $modsDestination = Join-Path $downloads "Mods.zip"

        $needHdDownload = [string]::IsNullOrWhiteSpace($HdArchive) -and
            -not (Test-FileIdentity -Path $hdDestination -Size $hdSize -Sha256 $hdSha256)
        $needModsDownload = [string]::IsNullOrWhiteSpace($ModsArchive) -and
            -not (Test-FileIdentity -Path $modsDestination -Size $modsSize -Sha256 $modsSha256)
        if (($needHdDownload -or $needModsDownload) -and -not $AcceptDownloads) {
            if ($NonInteractive) {
                throw "-AcceptDownloads is required when the verified download cache is incomplete."
            }
            Write-Host ""
            Write-Host "Missing source archives will download about 4.0 GB from the two external project links." -ForegroundColor Yellow
            Write-Host "The cache and staged build can temporarily use about 20-24 GB." -ForegroundColor Yellow
            $answer = Read-Host "Download, verify and install the Modern overlay now? [Y/n]"
            if (-not [string]::IsNullOrWhiteSpace($answer) -and $answer -notmatch '^[Yy]') {
                throw "Modern model installation was cancelled; the current game files were not changed."
            }
        }

        $hdZip = Ensure-Download -Name "GTA VC HD + Weapons" -Url $hdUrl `
            -Destination $hdDestination -Size $hdSize -Sha256 $hdSha256 `
            -Provided $HdArchive
        $modsZip = Ensure-Download -Name "Mods / Atmosphere" -Url $modsUrl `
            -Destination $modsDestination -Size $modsSize -Sha256 $modsSha256 `
            -Provided $ModsArchive

        $hdSource = Ensure-Extracted -Name "GTA VC HD + Weapons" -Archive $hdZip `
            -Destination (Join-Path $sources "hd-pack") -Sha256 $hdSha256 `
            -ContentMarker "models\gta3.img"
        $modsSource = Ensure-Extracted -Name "Mods / Atmosphere" -Archive $modsZip `
            -Destination (Join-Path $sources "mods-pack") -Sha256 $modsSha256 `
            -ContentMarker "Vehicles\gta3.img"

        Write-Host "Building the optimized PC Modern overlay. This can take several minutes..." -ForegroundColor Cyan
        $builderArguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $builder,
            "-GameDir", $game,
            "-HdPack", $hdSource,
            "-AtmospherePack", $modsSource,
            "-Out", $output,
            "-Force"
        )
        Invoke-NativeChecked -FilePath "powershell.exe" -Arguments $builderArguments `
            -FailureMessage "Modern overlay build failed"

        Write-Host "Verifying the installed overlay..." -ForegroundColor Cyan
        if (-not (Test-CompletedOverlay -Path $output -ExpectedBuilderVersion $expectedBuilderVersion)) {
            throw "The builder returned success, but final manifest/hash validation failed: $output"
        }
    }

    Write-Host ""
    Write-Host "MODERN MODELS ARE READY: $output" -ForegroundColor Green
    Write-Host "Fully restart Vice City VR, then choose the Modern categories in VR Menu > Model Assets."
    Write-Host "Vegetation / Palms stays Classic because the heavy HD palm pack is deliberately excluded."
    Write-Host "Downloads and extracted sources remain cached in: $WorkDir"
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 0
} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Verified downloads and extracted sources are kept for the next run."
    Write-Host "An existing Modern overlay is not removed until a complete replacement has passed validation."
    Write-Host "Diagnostic log: $LogPath"
    Stop-DiagnosticLog
    exit 1
}

