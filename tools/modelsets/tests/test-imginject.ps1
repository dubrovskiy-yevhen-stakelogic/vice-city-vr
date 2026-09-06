param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$injector = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\imginject.ps1')).Path
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('vice-city-imginject-' + [guid]::NewGuid().ToString('N'))
$sectorSize = 2048

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-BytesEqual([byte[]]$Actual, [byte[]]$Expected, [string]$Message) {
    if ($Actual.Length -ne $Expected.Length) { throw $Message }
    for ($i = 0; $i -lt $Actual.Length; $i++) {
        if ($Actual[$i] -ne $Expected[$i]) { throw $Message }
    }
}

function New-DirEntry([uint32]$Offset, [uint32]$Size, [string]$Name) {
    $entry = New-Object byte[] 32
    [BitConverter]::GetBytes($Offset).CopyTo($entry, 0)
    [BitConverter]::GetBytes($Size).CopyTo($entry, 4)
    $nameBytes = [System.Text.Encoding]::ASCII.GetBytes($Name)
    if ($nameBytes.Length -gt 24) { throw "Test entry name is too long: $Name" }
    $nameBytes.CopyTo($entry, 8)
    return $entry
}

function Assert-InjectorFails([string]$ImgPath, [string]$FromPath, [string]$ExpectedText) {
    $failed = $false
    try {
        & $injector -Img $ImgPath -From $FromPath | Out-Null
    } catch {
        $failed = $true
        Assert-True ($_.Exception.Message -like "*$ExpectedText*") "Unexpected failure: $($_.Exception.Message)"
    }
    Assert-True $failed "imginject accepted malformed input: $ImgPath"
}

New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $sourceDir = Join-Path $testRoot 'replacements'
    New-Item -ItemType Directory -Path $sourceDir | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $sourceDir 'a.dff'), [byte[]](1, 2, 3))

    $imgPath = Join-Path $testRoot 'sample.img'
    $dirPath = Join-Path $testRoot 'sample.dir'
    $img = New-Object byte[] ($sectorSize * 2)
    for ($i = 0; $i -lt $sectorSize; $i++) {
        $img[$i] = 0x41
        $img[$sectorSize + $i] = 0x42
    }
    [System.IO.File]::WriteAllBytes($imgPath, $img)
    $dir = New-Object byte[] 64
    (New-DirEntry 0 1 'a.dff').CopyTo($dir, 0)
    (New-DirEntry 1 1 'b.txd').CopyTo($dir, 32)
    [System.IO.File]::WriteAllBytes($dirPath, $dir)

    & $injector -Img $imgPath -From $sourceDir | Out-Null
    $rebuilt = [System.IO.File]::ReadAllBytes($imgPath)
    Assert-True ($rebuilt.Length -eq $sectorSize * 2) 'Rebuilt IMG has the wrong length.'
    Assert-BytesEqual $rebuilt[0..2] ([byte[]](1, 2, 3)) 'Replacement payload was not installed.'
    for ($i = 3; $i -lt $sectorSize; $i++) {
        Assert-True ($rebuilt[$i] -eq 0) 'Replacement sector padding is not zero-filled.'
    }
    for ($i = $sectorSize; $i -lt $sectorSize * 2; $i++) {
        Assert-True ($rebuilt[$i] -eq 0x42) 'Unmodified archive entry changed.'
    }
    Assert-True (-not (Test-Path -LiteralPath "$imgPath.new")) 'Temporary IMG remains after success.'
    Assert-True (-not (Test-Path -LiteralPath "$dirPath.new")) 'Temporary DIR remains after success.'
    Assert-True (@(Get-ChildItem -LiteralPath $testRoot -Filter '*.imginject-backup.*' -File).Count -eq 0) 'Backup remains after success.'

    $badDirImg = Join-Path $testRoot 'bad-length.img'
    $badDirPath = Join-Path $testRoot 'bad-length.dir'
    [System.IO.File]::WriteAllBytes($badDirImg, (New-Object byte[] $sectorSize))
    [System.IO.File]::WriteAllBytes($badDirPath, [byte[]](0))
    $badDirBefore = [System.IO.File]::ReadAllBytes($badDirPath)
    Assert-InjectorFails $badDirImg $sourceDir 'multiple of 32 bytes'
    Assert-BytesEqual ([System.IO.File]::ReadAllBytes($badDirPath)) $badDirBefore 'Malformed DIR changed after rejection.'

    $outsideImg = Join-Path $testRoot 'outside.img'
    $outsideDir = Join-Path $testRoot 'outside.dir'
    [System.IO.File]::WriteAllBytes($outsideImg, (New-Object byte[] $sectorSize))
    [System.IO.File]::WriteAllBytes($outsideDir, (New-DirEntry 2 1 'a.dff'))
    $outsideImgBefore = [System.IO.File]::ReadAllBytes($outsideImg)
    $outsideDirBefore = [System.IO.File]::ReadAllBytes($outsideDir)
    Assert-InjectorFails $outsideImg $sourceDir "points outside"
    Assert-BytesEqual ([System.IO.File]::ReadAllBytes($outsideImg)) $outsideImgBefore 'Out-of-bounds IMG changed after rejection.'
    Assert-BytesEqual ([System.IO.File]::ReadAllBytes($outsideDir)) $outsideDirBefore 'Out-of-bounds DIR changed after rejection.'

    Write-Output 'PASS: imginject replacement, preservation, cleanup, and malformed-input rejection.'
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
