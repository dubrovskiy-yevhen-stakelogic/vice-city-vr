[CmdletBinding()]
param(
    [string]$GameDir,
    [ValidateSet('BASE', 'RTX50', 'RTX40')][string]$Profile,
    [string]$CacheDir,
    [string]$RestoreBackup,
    [switch]$VerifyOnly,
    [switch]$NoPrompt,
    [switch]$AcceptCommunityRuntime,
    [switch]$AcceptModifiedModel
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'installer-common.ps1')
$stage = $null

function Select-GameFolder {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select reVC.exe in your installed Vice City VR game folder'
    $dialog.Filter = 'Vice City VR (reVC.exe)|reVC.exe'
    $dialog.CheckFileExists = $true
    try {
        if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { throw 'Cancelled. No runtime files were changed.' }
        return [IO.Path]::GetDirectoryName($dialog.FileName)
    } finally { $dialog.Dispose() }
}

try {
    if ($VerifyOnly -and -not [string]::IsNullOrWhiteSpace($RestoreBackup)) {
        throw '-VerifyOnly cannot be combined with -RestoreBackup.'
    }
    Write-Host 'Vice City VR 0.5.5 - DLSS 5 Setup' -ForegroundColor Cyan
    Write-Host 'No compiler, Git, administrator rights or VR headset is needed for setup.'
    Write-Host 'The game is never launched and VR/settings files are not changed.'
    Write-Host ''
    $restore = -not [string]::IsNullOrWhiteSpace($RestoreBackup)
    if (-not $NoPrompt -and -not $VerifyOnly -and -not $restore) {
        Write-Host '1 - Install base runtime or optional DLSS 5 files'
        Write-Host '2 - Restore a previous runtime backup'
        Write-Host 'Anything else - Cancel'
        $choice = Read-Host 'Choose 1 or 2'
        if ($choice -eq '2') { $restore = $true }
        elseif ($choice -ne '1') { Write-Host 'Cancelled.'; exit 0 }
    }
    if ($restore -or -not $VerifyOnly) {
        if ([string]::IsNullOrWhiteSpace($GameDir)) {
            if ($NoPrompt) { throw '-GameDir is required without prompts.' }
            $GameDir = Select-GameFolder
        }
        $GameDir = Get-DlssGameDirectory $GameDir
        Assert-DlssGameClosed
        Write-Host "Game folder: $GameDir"
    }
    if ($restore) {
        if ([string]::IsNullOrWhiteSpace($RestoreBackup)) {
            $backupRoot = Assert-DlssPlainPath (Join-Path $GameDir 'dlss5-backups')
            $backups = @(Get-ChildItem -LiteralPath $backupRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'manifest.json') -PathType Leaf } |
                Sort-Object Name -Descending)
            if ($backups.Count -eq 0) { throw 'No installer backups were found in this game folder.' }
            for ($i=0; $i -lt $backups.Count; $i++) { Write-Host ("{0} - {1}" -f ($i+1), $backups[$i].Name) }
            $number = 0
            if (-not [int]::TryParse((Read-Host 'Select a backup number'), [ref]$number) -or
                $number -lt 1 -or $number -gt $backups.Count) { throw 'Cancelled: no valid backup selected.' }
            $RestoreBackup = $backups[$number-1].FullName
        }
        if (-not $NoPrompt -and (Read-Host 'Type RESTORE to restore the selected backup') -cne 'RESTORE') {
            Write-Host 'Cancelled.'; exit 0
        }
        Restore-DlssBackup $GameDir $RestoreBackup
        exit 0
    }
    if ([string]::IsNullOrWhiteSpace($Profile)) {
        if ($NoPrompt) { throw '-Profile BASE, RTX50 or RTX40 is required without prompts.' }
        try {
            $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name })
            Write-Host ('Detected graphics: ' + ($gpus -join ', '))
        } catch { Write-Host 'Graphics card could not be detected; choose it below.' }
        Write-Host '0 - BASE: required runtime and ordinary DLSS/DLAA, without the DLSS 5 model'
        Write-Host '1 - RTX 50 series: NVIDIA-signed NR model from a community mirror'
        Write-Host '2 - RTX 40 series: modified experimental NR model (not NVIDIA-signed)'
        Write-Host 'For other GPUs, choose BASE to supply the required loader; DLSS/DLAA still needs a supported NVIDIA GPU.'
        switch (Read-Host 'Choose 0, 1 or 2; anything else cancels') {
            '0' { $Profile = 'BASE' }
            '1' { $Profile = 'RTX50' }
            '2' { $Profile = 'RTX40' }
            default { Write-Host 'Cancelled.'; exit 0 }
        }
    }
    $catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.json') -Raw | ConvertFrom-Json
    $packages = @(Get-DlssProfilePackages $catalog $Profile)
    $files = @($packages | ForEach-Object { $_.files })
    Write-Host ''
    Write-Host 'Sources and pinned versions:' -ForegroundColor Cyan
    foreach ($package in $packages) { Write-Host $package.label; Write-Host $package.url }
    Write-Host ''
    if ($Profile -eq 'BASE') {
        Write-Host 'DLSS SR is downloaded directly from NVIDIA. The required Streamline runtime uses RankFTW/rhi-repo, a third-party mirror.' -ForegroundColor Yellow
        Write-Host 'BASE installs five runtime files. No NR model is downloaded; existing DLSS 5 plugin/model files are left untouched.'
    } else {
        Write-Host 'Only DLSS SR is downloaded directly from NVIDIA. Streamline and NR use RankFTW/rhi-repo, a third-party mirror.' -ForegroundColor Yellow
    }
    Write-Host 'Checksums pin these exact files; they do not guarantee safety, licensing permission or in-game compatibility.'
    Write-Host 'Review the NVIDIA RTX SDK terms and the linked releases before accepting.'
    Write-Host 'https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/LICENSE.txt'
    Write-Host 'Setup verifies and installs files only. It does not certify in-game compatibility on your GPU.'
    if (-not $AcceptCommunityRuntime) {
        if ($NoPrompt) { throw 'Explicit -AcceptCommunityRuntime is required for third-party downloads.' }
        if ((Read-Host 'Type INSTALL to accept these sources and continue') -cne 'INSTALL') {
            Write-Host 'Cancelled.'; exit 0
        }
    }
    if ($Profile -eq 'RTX40') {
        Write-Host 'RTX40 WARNING: this exact model is modified and its NVIDIA signature is invalid.' -ForegroundColor Yellow
        Write-Host 'It differs from the locally tested development model. Vice City VR runtime compatibility is not yet confirmed.'
        if (-not $AcceptModifiedModel) {
            if ($NoPrompt) { throw 'Explicit -AcceptModifiedModel is required for the modified RTX40 model.' }
            if ((Read-Host 'Type RTX40 to accept this additional risk') -cne 'RTX40') {
                Write-Host 'Cancelled.'; exit 0
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($CacheDir)) {
        $localData = [Environment]::GetFolderPath('LocalApplicationData')
        if ([string]::IsNullOrWhiteSpace($localData)) { throw 'Local application data folder is unavailable.' }
        $CacheDir = Join-Path $localData 'ViceCityVR\DLSS5Cache'
    }
    $CacheDir = Assert-DlssPlainPath $CacheDir
    $kitRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\')
    if ($CacheDir.TrimEnd('\') -ieq $kitRoot -or
        $CacheDir.StartsWith($kitRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Download caches must stay outside the source kit. Omit -CacheDir to use the normal local application data folder.'
    }
    [void][IO.Directory]::CreateDirectory($CacheDir)
    $stage = Join-Path $CacheDir ('stage-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($stage)
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    foreach ($package in $packages) {
        $download = Get-DlssDownload $package $CacheDir
        Expand-DlssPackage $download $package $stage
    }
    if ($VerifyOnly) {
        Write-Host ("VERIFIED: all {0} pinned runtime files passed the selected checks. No game files were changed." -f $files.Count) -ForegroundColor Green
    } else {
        [void](Install-DlssFiles $GameDir $stage $files $Profile)
        Write-Host 'INSTALLED: runtime files verified and placed beside reVC.exe.' -ForegroundColor Green
        if ($Profile -eq 'BASE') {
            Write-Host 'Start Vice City VR using its VR launcher. Your existing graphics and VR settings have not been changed.'
            Write-Host 'DLSS 5 is optional and was not installed by BASE. You can choose an NR profile later if wanted.'
        } else {
            Write-Host 'Start the game yourself. VR Settings > Graphics > Temporal AA: DLAA; Neural DLSS 5: ON; Passes: 1X.'
            Write-Host 'Confirm ACTIVE and compare against the baseline. Installation success alone is not proof of neural rendering.'
        }
        Write-Host 'To undo this installation, run INSTALL_DLSS5.bat again and choose Restore.'
    }
    Write-Host "Verified downloads are cached for reuse: $CacheDir"
} catch {
    Write-Host ''
    Write-Host ('SETUP STOPPED: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Do not download a replacement with the same filename from a random site to bypass verification.'
    Write-Host 'If Windows blocked access, extract the game to a writable folder; do not disable antivirus or certificate checks.'
    exit 1
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage -PathType Container)) {
        [void](Assert-DlssPlainPath $stage)
        foreach ($name in $script:DlssNames) {
            $path = Assert-DlssPlainPath (Join-Path $stage $name)
            if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
        }
        if (@(Get-ChildItem -LiteralPath $stage -Force).Count -eq 0) { Remove-Item -LiteralPath $stage }
    }
}
