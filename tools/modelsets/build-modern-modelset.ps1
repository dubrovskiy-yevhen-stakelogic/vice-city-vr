# Builds the optional MODERN asset overlay from files downloaded by the user.
# No game or third-party assets are distributed with Vice City VR.
# See ..\..\MODERN_MODELS.md for download links and folder examples.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameDir,
    [Parameter(Mandatory=$true)][string]$HdPack,
    [Parameter(Mandatory=$true)][string]$AtmospherePack,
    [Parameter(Mandatory=$true)][string]$HdVegetation,
    [string]$Out,
    [switch]$VerifyOnly,
    [switch]$Force
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"
$BuildScriptVersion = "0.5.0-1"
$MinimumFreeBytes = 12GB

$SourceUrls = [ordered]@{
    "GTA VC HD + Weapons" = "https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view"
    "Mods / Atmosphere"   = "https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view"
    "Vegetation in HD"    = "https://libertycity.net/files/gta-vice-city/126557-vegetation-in-hd.html"
}

# These signatures identify the downloads used to prepare and test 0.5.0.
# A mismatch is a warning, not a hard failure: an author may update a pack at
# the same URL while retaining the folder layout expected by this builder.
$TestedSignatures = @{
    "HD pack gta3.img"             = "C48CA4E66E743B270D8848A4E405087E696C8A63D71B03853CE20F3BBC5B3CC2"
    "HD pack gta3.dir"             = "55C20A60446B0382DD85689DF72EA58B7806E8E315C2D6F75ABC2C960546A104"
    "Atmosphere vehicles.col"      = "64233744C532423ECDCAEFF088696E76BCFA4758A70CADA2FD6EDDA258911DF9"
    "Atmosphere wheels.dff"        = "3FAB043194EB5016D3F96D14090831D1ABCDDCB4B3793B465D30A581643D22DA"
    "HD Vegetation generic.txd"    = "705BD993DCC6B1B5C369D4CE2DFCF1F826E4AC3693D0E4F31366CB4F4594FBB9"
    "HD Vegetation gtatrees.txd"   = "E0BE3987B7C9F95335D5AE22955BFA298CDB6E2B4C054AD25318DF56AE6A5C72"
}

function Step([string]$Text) {
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Full-Path([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\','/')
}

function Resolve-ExistingDirectory([string]$Path, [string]$What) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$What folder not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path.TrimEnd('\','/')
}

function Require-File([string]$Path, [string]$What) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$What not found: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "$What is empty: $Path"
    }
}

function Require-Directory([string]$Path, [string]$What) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$What folder not found: $Path"
    }
}

function Require-MinimumFiles(
    [string]$Path,
    [string]$Filter,
    [int]$Minimum,
    [string]$What,
    [switch]$Recurse
) {
    Require-Directory $Path $What
    $items = if ($Recurse) {
        @(Get-ChildItem -LiteralPath $Path -Recurse -File -Filter $Filter)
    } else {
        @(Get-ChildItem -LiteralPath $Path -File -Filter $Filter)
    }
    if ($items.Count -lt $Minimum) {
        throw "$What contains only $($items.Count) '$Filter' files; expected at least $Minimum. Check that the correct archive was fully extracted."
    }
    return $items.Count
}

# Users may select the outer directory made by 7-Zip. Find the shallowest
# descendant which contains the pack-specific marker.
function Find-Marker([string]$Root, [string]$Marker, [string]$What) {
    $resolvedRoot = Resolve-ExistingDirectory $Root $What
    $candidates = @($resolvedRoot) + @(
        Get-ChildItem -LiteralPath $resolvedRoot -Directory -Recurse -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    )
    $matches = @($candidates |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ $Marker) } |
        Sort-Object { $_.Length }, { $_ })
    if ($matches.Count -eq 0) {
        throw "${What}: could not find '$Marker' anywhere under '$resolvedRoot'. Check the expected folder tree in MODERN_MODELS.md."
    }
    if ($matches.Count -gt 1) {
        Write-Warning "${What}: found more than one candidate; using the shallowest: $($matches[0])"
    }
    return $matches[0]
}

function Validate-ImgPair([string]$Img, [string]$What) {
    $Dir = [System.IO.Path]::ChangeExtension($Img, ".dir")
    Require-File $Img "$What IMG"
    Require-File $Dir "$What DIR"
    $imgLength = (Get-Item -LiteralPath $Img).Length
    $dirBytes = [System.IO.File]::ReadAllBytes($Dir)
    if (($dirBytes.Length % 32) -ne 0) {
        throw "$What DIR length is not a multiple of 32 bytes: $Dir"
    }
    $entryCount = [int]($dirBytes.Length / 32)
    if ($entryCount -lt 5000) {
        throw "$What has only $entryCount directory entries; expected a complete Vice City gta3 archive."
    }
    for ($index = 0; $index -lt $entryCount; $index++) {
        $offset = $index * 32
        $sector = [int64][BitConverter]::ToUInt32($dirBytes, $offset)
        $count = [int64][BitConverter]::ToUInt32($dirBytes, $offset + 4)
        if ((($sector + $count) * 2048) -gt $imgLength) {
            throw "$What entry $index points outside its IMG; the archive is incomplete or corrupt."
        }
    }
    return [pscustomobject]@{ Entries=$entryCount; ImgBytes=$imgLength; DirBytes=$dirBytes.Length }
}

function File-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Copy-DirectoryContents([string]$Source, [string]$Destination) {
    Require-Directory $Source "Copy source"
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force |
        Copy-Item -Destination $Destination -Recurse -Force
}

function Paths-Overlap([string]$Left, [string]$Right) {
    $leftPath = (Full-Path $Left) + [System.IO.Path]::DirectorySeparatorChar
    $rightPath = (Full-Path $Right) + [System.IO.Path]::DirectorySeparatorChar
    return $leftPath.StartsWith($rightPath, [System.StringComparison]::OrdinalIgnoreCase) -or
        $rightPath.StartsWith($leftPath, [System.StringComparison]::OrdinalIgnoreCase)
}

function Invoke-ScriptChecked([string]$Path, [hashtable]$Parameters) {
    & $Path @Parameters
    if (-not $?) {
        throw "Tool failed: $Path"
    }
}

function Invoke-NativeChecked([string]$Path, [string[]]$Arguments) {
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Tool failed with exit code ${LASTEXITCODE}: $Path"
    }
}

# ---- resolve and validate every input before writing anything ----
$game = Resolve-ExistingDirectory $GameDir "Vice City VR game"
$hd = Find-Marker $HdPack "models\gta3.img" "GTA VC HD + Weapons"
$mods = Find-Marker $AtmospherePack "Vehicles\gta3.img" "Mods / Atmosphere"
$hvRoot = Find-Marker $HdVegetation "MOD\modloader\MODS" "Vegetation in HD"
$hv = Join-Path $hvRoot "MOD\modloader\MODS"

if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $game "modelsets\modern"
}
$Out = Full-Path $Out

$gameImg = Join-Path $game "models\gta3.img"
$gameDirFile = Join-Path $game "models\gta3.dir"
$gameGeneric = Join-Path $game "models\generic.txd"
Require-File $gameImg "Original game models\gta3.img"
Require-File $gameDirFile "Original game models\gta3.dir"
Require-File $gameGeneric "Original game models\generic.txd"

$hdImg = Join-Path $hd "models\gta3.img"
$hdDir = Join-Path $hd "models\gta3.dir"
$hdGeneric = Join-Path $hd "models\generic.txd"
$hdTxd = Join-Path $hd "txd"
Require-File $hdGeneric "HD pack models\generic.txd"
Require-Directory $hdTxd "HD pack txd"
$hdArchive = Validate-ImgPair $hdImg "GTA VC HD + Weapons"

$vehicleFiles = Join-Path $mods "Vehicles\gta3.img"
$vehicleColl = Join-Path $mods "Vehicles\coll\vehicles.col"
$vehicleGeneric = Join-Path $mods "Vehicles\generic"
$atmosVegetation = Join-Path $mods "Vegitation" # spelling used by the download
Require-File $vehicleColl "Atmosphere Vehicles\coll\vehicles.col"
Require-File (Join-Path $vehicleGeneric "nowheel.DFF") "Atmosphere nowheel.DFF"
Require-File (Join-Path $vehicleGeneric "wheels.dff") "Atmosphere wheels.dff"
Require-File (Join-Path $vehicleGeneric "wheels.txd") "Atmosphere wheels.txd"
$vehicleDffCount = Require-MinimumFiles $vehicleFiles "*.dff" 90 "Atmosphere vehicle models"
$vehicleTxdCount = Require-MinimumFiles $vehicleFiles "*.txd" 90 "Atmosphere vehicle textures"
$atmosVegetationCount = Require-MinimumFiles $atmosVegetation "*.dff" 20 "Atmosphere vegetation" -Recurse

$hvGeneric = Join-Path $hv "generic.txd"
$hvVegetation = Join-Path $hv "Vice Vegetation"
$hvTreesTxd = Join-Path $hvVegetation "trees\gtatrees.txd"
Require-File $hvGeneric "HD Vegetation generic.txd"
Require-File $hvTreesTxd "HD Vegetation gtatrees.txd"
$hdVegetationCount = Require-MinimumFiles $hvVegetation "*.dff" 20 "HD Vegetation models" -Recurse

$tools = $PSScriptRoot
$requiredTools = @(
    "imginject.ps1", "imgtexaudit.ps1", "imgextract.ps1",
    "txdmerge.ps1", "txdcompress.exe"
)
foreach ($tool in $requiredTools) {
    Require-File (Join-Path $tools $tool) "Required model-set tool '$tool'"
}

# Reject paths which could recurse into a downloaded pack or replace game data.
$gameModels = Join-Path $game "models"
if ((Full-Path $Out) -eq (Full-Path $game) -or
    (Paths-Overlap $Out $gameModels)) {
    throw "Unsafe output path '$Out'. Use the default modelsets\modern folder, never the game root or models folder."
}
foreach ($sourceRoot in @($hd, $mods, $hvRoot)) {
    if (Paths-Overlap $Out $sourceRoot) {
        throw "Unsafe output path '$Out': it overlaps source folder '$sourceRoot'."
    }
}
$outRoot = [System.IO.Path]::GetPathRoot($Out)
if ((Full-Path $Out) -eq (Full-Path $outRoot)) {
    throw "Refusing to use a drive root as output: $Out"
}

Write-Host ""
Write-Host "Vice City VR Modern asset builder $BuildScriptVersion"
Write-Host "Game:          $game"
Write-Host "HD + Weapons:  $hd"
Write-Host "Atmosphere:    $mods"
Write-Host "HD Vegetation: $hvRoot"
Write-Host "Output:        $Out"
Write-Host ""
Write-Host ("Validated: {0} HD archive entries; {1} vehicle DFFs; {2} vehicle TXDs; {3}+{4} vegetation DFFs." -f
    $hdArchive.Entries, $vehicleDffCount, $vehicleTxdCount,
    $atmosVegetationCount, $hdVegetationCount)

Step "Hashing source anchors"
$sourceHashes = [ordered]@{
    "Original game generic.txd"     = File-Sha256 $gameGeneric
    "HD pack gta3.img"              = File-Sha256 $hdImg
    "HD pack gta3.dir"              = File-Sha256 $hdDir
    "Atmosphere vehicles.col"       = File-Sha256 $vehicleColl
    "Atmosphere wheels.dff"         = File-Sha256 (Join-Path $vehicleGeneric "wheels.dff")
    "HD Vegetation generic.txd"     = File-Sha256 $hvGeneric
    "HD Vegetation gtatrees.txd"    = File-Sha256 $hvTreesTxd
}
foreach ($name in $sourceHashes.Keys) {
    Write-Host ("    {0}: {1}" -f $name, $sourceHashes[$name])
    if ($TestedSignatures.ContainsKey($name) -and
        $sourceHashes[$name] -ne $TestedSignatures[$name]) {
        Write-Warning "$name differs from the pack tested for 0.5.0. The build may still work, but it will not be byte-identical to the tested source set."
    }
}

if ($VerifyOnly) {
    Write-Host ""
    Write-Host "Verification passed. No files were written." -ForegroundColor Green
    return
}

if (Test-Path -LiteralPath $Out) {
    if (-not (Test-Path -LiteralPath $Out -PathType Container)) {
        throw "Output exists and is not a directory: $Out"
    }
    if (-not $Force) {
        throw "Output already exists: $Out`nRe-run with -Force to replace it only after a complete staged build succeeds."
    }
}

$drive = $null
$availableBytes = $null
try {
    $drive = New-Object System.IO.DriveInfo([System.IO.Path]::GetPathRoot($Out))
    $availableBytes = $drive.AvailableFreeSpace
} catch {
    Write-Warning "Could not query free space for '$Out'; the build may need about 12 GB temporarily."
}
if ($drive -and $availableBytes -lt $MinimumFreeBytes) {
    throw ("At least {0:N0} GB free is required on {1}; only {2:N1} GB is available." -f
        ($MinimumFreeBytes / 1GB), $drive.Name, ($availableBytes / 1GB))
}

$outParent = Split-Path -Parent $Out
$outLeaf = Split-Path -Leaf $Out
New-Item -ItemType Directory -Force -Path $outParent | Out-Null
$unique = "{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
$stage = Join-Path $outParent (".{0}.build-{1}" -f $outLeaf, $unique)
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("vcvr-modern-build-{0}" -f $unique)
$committed = $false

try {
    New-Item -ItemType Directory -Path $stage,$work | Out-Null
    $stageModels = Join-Path $stage "models"
    $stageTxd = Join-Path $stage "txd"

    # 1. The HD pack supplies the complete base archive and loose TXDs.
    Step "Copying the HD pack base (about 3.5 GB)"
    Copy-DirectoryContents (Join-Path $hd "models") $stageModels
    Copy-DirectoryContents $hdTxd $stageTxd

    # 2. The Atmosphere pack supplies loose vehicle collision and wheel files.
    Step "Copying Atmosphere vehicle collision and wheel files"
    $stageColl = Join-Path $stageModels "coll"
    $stageGenericDir = Join-Path $stageModels "generic"
    New-Item -ItemType Directory -Force -Path $stageColl,$stageGenericDir | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $mods "Vehicles\coll") -File -Filter "*.col" |
        Copy-Item -Destination $stageColl -Force
    Get-ChildItem -LiteralPath $vehicleGeneric -File |
        Copy-Item -Destination $stageGenericDir -Force

    # 3. Inject the Atmosphere vehicles and vegetation. Later sources win.
    Step "Injecting Atmosphere vehicles and vegetation"
    Invoke-ScriptChecked (Join-Path $tools "imginject.ps1") @{
        Img = Join-Path $stageModels "gta3.img"
        From = @(
            $vehicleFiles,
            $vehicleGeneric,
            $atmosVegetation,
            (Join-Path $atmosVegetation "pots"),
            (Join-Path $atmosVegetation "trees")
        )
    }

    # 4. HD Vegetation is included as an optional category. The runtime keeps
    # this category CLASSIC by default because these palms are extremely heavy.
    Step "Injecting optional HD Vegetation"
    Invoke-ScriptChecked (Join-Path $tools "imginject.ps1") @{
        Img = Join-Path $stageModels "gta3.img"
        From = @(
            $hv,
            $hvVegetation,
            (Join-Path $hvVegetation "pots"),
            (Join-Path $hvVegetation "trees")
        )
    }

    # Record exact vegetation membership for category-selective streaming.
    $vegetationNames = @(
        Get-ChildItem -LiteralPath $atmosVegetation,$hvVegetation -Recurse -File -Filter "*.dff" |
            ForEach-Object { $_.BaseName.ToLowerInvariant() } |
            Sort-Object -Unique
    )
    if ($vegetationNames.Count -lt 20) {
        throw "Only $($vegetationNames.Count) vegetation models were found; refusing to create an incomplete manifest."
    }
    if ($vegetationNames.Count -gt 512) {
        throw "The vegetation manifest has $($vegetationNames.Count) names; the runtime supports at most 512."
    }
    $tooLong = @($vegetationNames | Where-Object { $_.Length -gt 23 })
    if ($tooLong.Count -gt 0) {
        throw "Vegetation model names exceed the runtime's 23-character limit: $($tooLong -join ', ')"
    }
    $vegetationManifest = Join-Path $stage "vegetation_models.txt"
    [System.IO.File]::WriteAllLines(
        $vegetationManifest,
        [string[]]$vegetationNames,
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("    vegetation category: {0} models (CLASSIC by default)" -f $vegetationNames.Count)

    # 5. Start from the user's original generic.txd, adding only leaf textures
    # absent from it. This avoids the pack's full uncompressed dictionary.
    Step "Rebuilding generic.txd (original + missing HD leaf textures)"
    $mergedGeneric = Join-Path $work "generic_merged.txd"
    Invoke-ScriptChecked (Join-Path $tools "txdmerge.ps1") @{
        Base = $gameGeneric
        Extra = $hvGeneric
        Out = $mergedGeneric
    }
    Copy-Item -LiteralPath $mergedGeneric -Destination (Join-Path $stageModels "generic.txd") -Force
    $mergedInject = Join-Path $work "inject_merged"
    New-Item -ItemType Directory -Path $mergedInject | Out-Null
    Copy-Item -LiteralPath $mergedGeneric -Destination (Join-Path $mergedInject "Generic.txd") -Force
    Invoke-ScriptChecked (Join-Path $tools "imginject.ps1") @{
        Img = Join-Path $stageModels "gta3.img"
        From = @($mergedInject)
    }

    # 6. Recompress uncompressed or no-mip texture dictionaries for VR.
    Step "Auditing the archive for expensive texture dictionaries"
    $candidateList = Join-Path $work "txd_candidates.txt"
    Invoke-ScriptChecked (Join-Path $tools "imgtexaudit.ps1") @{
        Img = Join-Path $stageModels "gta3.img"
        CandidateList = $candidateList
    }
    $candidates = if (Test-Path -LiteralPath $candidateList) {
        @(Get-Content -LiteralPath $candidateList |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Sort-Object -Unique)
    } else { @() }
    if ($candidates.Count -gt 0) {
        Step ("Extracting and recompressing {0} texture dictionaries" -f $candidates.Count)
        $imageTxd = Join-Path $work "imgtxd"
        Invoke-ScriptChecked (Join-Path $tools "imgextract.ps1") @{
            Img = Join-Path $stageModels "gta3.img"
            Entries = $candidates
            Out = $imageTxd
        }
        $extracted = @(Get-ChildItem -LiteralPath $imageTxd -File -Filter "*.txd")
        if ($extracted.Count -ne $candidates.Count) {
            throw "Requested $($candidates.Count) TXDs but extracted $($extracted.Count); refusing a partial recompression pass."
        }
        foreach ($txd in $extracted) {
            Invoke-NativeChecked (Join-Path $tools "txdcompress.exe") @($txd.FullName)
        }
        Step "Injecting recompressed texture dictionaries"
        Invoke-ScriptChecked (Join-Path $tools "imginject.ps1") @{
            Img = Join-Path $stageModels "gta3.img"
            From = @($imageTxd)
        }
    } else {
        Write-Host "    no archive texture dictionaries require recompression"
    }
    Step "Recompressing loose generic.txd"
    Invoke-NativeChecked (Join-Path $tools "txdcompress.exe") @((Join-Path $stageModels "generic.txd"))

    # Validate the complete staged result before replacing any prior overlay.
    Step "Validating and hashing the completed overlay"
    $resultArchive = Validate-ImgPair (Join-Path $stageModels "gta3.img") "Built Modern overlay"
    Require-File (Join-Path $stageModels "generic.txd") "Built Modern generic.txd"
    Require-File $vegetationManifest "Built vegetation manifest"
    $outputHashes = [ordered]@{
        "models/gta3.img"       = File-Sha256 (Join-Path $stageModels "gta3.img")
        "models/gta3.dir"       = File-Sha256 (Join-Path $stageModels "gta3.dir")
        "models/generic.txd"    = File-Sha256 (Join-Path $stageModels "generic.txd")
        "vegetation_models.txt" = File-Sha256 $vegetationManifest
    }

    $info = New-Object System.Collections.Generic.List[string]
    $info.Add("Vice City VR Modern asset overlay")
    $info.Add("BuilderVersion=$BuildScriptVersion")
    $info.Add("BuiltUtc=$([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))")
    $info.Add("ArchiveEntries=$($resultArchive.Entries)")
    $info.Add("DefaultVegetation=CLASSIC")
    $info.Add("")
    $info.Add("SOURCE URLS (assets are not redistributed by Vice City VR)")
    foreach ($name in $SourceUrls.Keys) { $info.Add("$name=$($SourceUrls[$name])") }
    $info.Add("")
    $info.Add("SOURCE SHA256")
    foreach ($name in $sourceHashes.Keys) { $info.Add("$name=$($sourceHashes[$name])") }
    $info.Add("")
    $info.Add("OUTPUT SHA256")
    foreach ($name in $outputHashes.Keys) { $info.Add("$name=$($outputHashes[$name])") }
    $info.Add("")
    $info.Add("Do not redistribute this built folder; it contains original game and third-party mod assets.")
    [System.IO.File]::WriteAllLines(
        (Join-Path $stage "BUILD_INFO.txt"),
        $info.ToArray(),
        (New-Object System.Text.UTF8Encoding($false)))

    # Commit only after all operations and validation have succeeded. With
    # -Force, the old output is moved aside and restored if the final rename
    # fails. This avoids turning a failed build into a broken installation.
    $backup = $null
    if (Test-Path -LiteralPath $Out) {
        $backup = Join-Path $outParent (".{0}.previous-{1}" -f $outLeaf, $unique)
        Move-Item -LiteralPath $Out -Destination $backup
    }
    try {
        Move-Item -LiteralPath $stage -Destination $Out
        $committed = $true
    } catch {
        if ($backup -and -not (Test-Path -LiteralPath $Out) -and
            (Test-Path -LiteralPath $backup)) {
            Move-Item -LiteralPath $backup -Destination $Out
        }
        throw
    }
    if ($backup -and (Test-Path -LiteralPath $backup)) {
        try {
            Remove-Item -LiteralPath $backup -Recurse -Force
        } catch {
            Write-Warning "The new overlay is installed, but the previous copy could not be removed: $backup"
        }
    }

    $imgMB = [math]::Round((Get-Item -LiteralPath (Join-Path $Out "models\gta3.img")).Length / 1MB)
    Write-Host ""
    Write-Host "Done. The Modern overlay is ready (gta3.img: $imgMB MB)." -ForegroundColor Green
    Write-Host "In VR MENU > MODEL ASSETS, enable the Modern categories you want, then restart."
    Write-Host "VEGETATION / PALMS defaults to CLASSIC for performance; enable it only if desired."
} finally {
    if (Test-Path -LiteralPath $work) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (-not $committed -and (Test-Path -LiteralPath $stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
