param(
    [Parameter(Mandatory=$true)][string]$OutputImg,
    [Parameter(Mandatory=$true)][string[]]$From,
    [string]$DffManifest
)

$ErrorActionPreference = "Stop"
$sectorSize = 2048
$outputDir = [System.IO.Path]::ChangeExtension($OutputImg, ".dir")
$entries = @{}

foreach ($source in $From) {
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Overlay source directory does not exist: $source"
    }
    foreach ($file in Get-ChildItem -LiteralPath $source -File) {
        if ($file.Extension -notmatch '^\.(dff|txd|col|ifp)$') { continue }
        $name = $file.Name.ToLowerInvariant()
        if ([Text.Encoding]::ASCII.GetByteCount($name) -gt 23) {
            throw "IMG v1 entry name is longer than 23 bytes: $name"
        }
        if ($entries.ContainsKey($name)) {
            throw "Duplicate overlay entry name: $name"
        }
        $entries[$name] = $file.FullName
    }
}

if ($entries.Count -eq 0) { throw "No DFF/TXD/COL/IFP files were found." }
$ordered = @($entries.Keys | Sort-Object)
$parent = Split-Path -Parent $OutputImg
if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
$tempId = [guid]::NewGuid().ToString("N")
$tempImg = "$OutputImg.new.$tempId"
$tempDir = "$outputDir.new.$tempId"
$stream = $null

try {
    $stream = [IO.File]::Create($tempImg)
    $directoryBytes = New-Object byte[] ($ordered.Count * 32)
    $padding = New-Object byte[] $sectorSize
    $nextSector = [uint32]0
    for ($index = 0; $index -lt $ordered.Count; $index++) {
        $name = $ordered[$index]
        $bytes = [IO.File]::ReadAllBytes($entries[$name])
        $stream.Write($bytes, 0, $bytes.Length)
        $sectors = [uint32][Math]::Ceiling($bytes.Length / [double]$sectorSize)
        $paddedLength = [int64]$sectors * $sectorSize
        if ($paddedLength -gt $bytes.Length) {
            $stream.Write($padding, 0, [int]($paddedLength - $bytes.Length))
        }
        $offset = $index * 32
        [BitConverter]::GetBytes($nextSector).CopyTo($directoryBytes, $offset)
        [BitConverter]::GetBytes($sectors).CopyTo($directoryBytes, $offset + 4)
        [Text.Encoding]::ASCII.GetBytes($name).CopyTo($directoryBytes, $offset + 8)
        $nextSector += $sectors
    }
    $stream.Close()
    $stream = $null
    [IO.File]::WriteAllBytes($tempDir, $directoryBytes)

    Move-Item -LiteralPath $tempImg -Destination $OutputImg -Force
    Move-Item -LiteralPath $tempDir -Destination $outputDir -Force
} finally {
    if ($stream) { $stream.Close() }
    if (Test-Path -LiteralPath $tempImg) { Remove-Item -LiteralPath $tempImg -Force }
    if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Force }
}

Write-Output ("Built IMG v1 overlay: {0} entries, {1:N1} MiB" -f
    $ordered.Count, ((Get-Item -LiteralPath $OutputImg).Length / 1MB))
if ($DffManifest) {
    $manifestParent = Split-Path -Parent $DffManifest
    if ($manifestParent) {
        New-Item -ItemType Directory -Path $manifestParent -Force | Out-Null
    }
    $dffNames = @($ordered | Where-Object { $_ -like '*.dff' } |
        ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
    [IO.File]::WriteAllLines($DffManifest, $dffNames,
        [Text.UTF8Encoding]::new($false))
    Write-Output ("Wrote DFF manifest: {0} names" -f $dffNames.Count)
}
