# Rebuilds a GTA IMG version 1 archive (gta3.img + gta3.dir), replacing entries
# whose name matches a file supplied on disk. Entries keep their dir order;
# offsets and sizes are recomputed.
param(
    [Parameter(Mandatory=$true)][string]$Img,
    [Parameter(Mandatory=$true)][string[]]$From,
    [string[]]$Exclude = @()
)

# Make .NET I/O failures terminating before archive replacement.
$ErrorActionPreference = "Stop"

$SECTOR = 2048
$dirPath = [System.IO.Path]::ChangeExtension($Img, ".dir")

# collect replacements, later sources win
$repl = @{}
foreach ($src in $From) {
    # -LiteralPath throughout: a game folder named like "GTA Vice City [Rus]"
    # is a wildcard to these cmdlets, and they quietly match nothing.
    if (-not (Test-Path -LiteralPath $src)) { Write-Output "  missing source: $src"; continue }
    foreach ($f in Get-ChildItem -LiteralPath $src -File) {
        if ($f.Extension -notmatch '^\.(dff|txd)$') { continue }
        $repl[$f.Name.ToLower()] = $f.FullName
    }
}
Write-Output ("replacement files offered: {0}" -f $repl.Count)

$db = [System.IO.File]::ReadAllBytes($dirPath)
if (($db.Length % 32) -ne 0) {
    throw "The IMG directory '$dirPath' is truncated (its size is not a multiple of 32 bytes)."
}
$imgLength = (Get-Item -LiteralPath $Img).Length
$n = $db.Length / 32
$entries = @()
for ($i = 0; $i -lt $n; $i++) {
    $b = $i*32
    $z = [Array]::IndexOf($db, [byte]0, $b+8) - ($b+8); if ($z -lt 0 -or $z -gt 24) { $z = 24 }
    $entryOffset = [int64][BitConverter]::ToUInt32($db, $b) * $SECTOR
    $entrySize = [int64][BitConverter]::ToUInt32($db, $b+4) * $SECTOR
    if ($entryOffset -gt $imgLength -or $entrySize -gt ($imgLength - $entryOffset)) {
        throw "IMG directory entry $i points outside '$Img'."
    }
    $entries += [pscustomobject]@{
        Name   = [System.Text.Encoding]::ASCII.GetString($db, $b+8, $z)
        Offset = $entryOffset
        Size   = $entrySize
    }
}
Write-Output ("archive entries: {0}" -f $entries.Count)

# Exact-name pruning removes expensive vegetation geometry from the optional
# Modern archive. Those missing entries fall back to Classic game data.
$excludeSet = @{}
foreach ($name in $Exclude) {
    if (-not [string]::IsNullOrWhiteSpace($name)) {
        $excludeSet[$name.ToLowerInvariant()] = $true
    }
}
if ($excludeSet.Count -gt 0) {
    $beforeExclude = $entries.Count
    $entries = @($entries | Where-Object {
        -not $excludeSet.ContainsKey($_.Name.ToLowerInvariant())
    })
    Write-Output ("archive entries excluded: {0}" -f ($beforeExclude - $entries.Count))
}

$outImg = "$Img.new"

# The rebuilt archive is a full sibling copy; verify required free space first.
$needBytes = (Get-Item -LiteralPath $Img).Length
$freeBytes = $null
try {
    $root = [System.IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $Img).ProviderPath)
    $freeBytes = (New-Object System.IO.DriveInfo($root)).AvailableFreeSpace
} catch {
    $freeBytes = $null
}
if ($freeBytes -ne $null -and $freeBytes -lt $needBytes) {
    throw ("Rebuilding '{0}' needs {1:N1} GB free on that drive; {2:N1} GB is available." -f
        $Img, ($needBytes / 1GB), ($freeBytes / 1GB))
}

$in = $null
$out = $null
$newDir = New-Object byte[] ($entries.Count * 32)
$replaced = 0
$used = @{}
$failure = $null
try {
    $in  = [System.IO.File]::OpenRead($Img)
    $out = [System.IO.File]::Create($outImg)
    $buf = New-Object byte[] (8*1024*1024)
    $pad = New-Object byte[] $SECTOR
    $nextSector = [int64]0

    for ($i = 0; $i -lt $entries.Count; $i++) {
        $e = $entries[$i]
        $key = $e.Name.ToLower()
        $written = [int64]0

        if ($repl.ContainsKey($key)) {
            $bytes = [System.IO.File]::ReadAllBytes($repl[$key])
            $out.Write($bytes, 0, $bytes.Length)
            $written = $bytes.Length
            $replaced++
            $used[$key] = $true
        } else {
            $in.Position = $e.Offset
            $left = $e.Size
            while ($left -gt 0) {
                $take = [int][math]::Min($buf.Length, $left)
                $got = $in.Read($buf, 0, $take)
                if ($got -le 0) {
                    throw "Unexpected end of '$Img' while copying entry '$($e.Name)'."
                }
                $out.Write($buf, 0, $got)
                $written += $got
                $left -= $got
            }
        }

        $tail = $written % $SECTOR
        if ($tail -ne 0) { $out.Write($pad, 0, $SECTOR - $tail); $written += ($SECTOR - $tail) }
        if (($written / $SECTOR) -gt [uint32]::MaxValue -or
            $nextSector -gt [uint32]::MaxValue) {
            throw "The rebuilt IMG exceeds the version 1 directory format limits."
        }
        $sizeSectors = [uint32]($written / $SECTOR)

        $b = $i*32
        [BitConverter]::GetBytes([uint32]$nextSector).CopyTo($newDir, $b)
        [BitConverter]::GetBytes($sizeSectors).CopyTo($newDir, $b+4)
        [System.Text.Encoding]::ASCII.GetBytes($e.Name).CopyTo($newDir, $b+8)
        $nextSector += $sizeSectors
    }
} catch {
    $failure = $_
} finally {
    if ($in) { $in.Close() }
    if ($out) { $out.Close() }
}

if ($failure) {
    # Remove a partial output; the original archive has not been swapped yet.
    if (Test-Path -LiteralPath $outImg) {
        Remove-Item -LiteralPath $outImg -Force -ErrorAction SilentlyContinue
    }
    throw ("Rebuilding '{0}' failed and the original was left untouched: {1}" -f
        $Img, $failure.Exception.Message)
}

[System.IO.File]::WriteAllBytes("$dirPath.new", $newDir)


# Install IMG and DIR as one transaction. A failure on either half restores
# both originals, so the archive and its directory cannot be left mismatched.
function Install-ArchivePair([string]$NewImg, [string]$TargetImg,
                             [string]$NewDir, [string]$TargetDir) {
    $pairs = @(
        [pscustomobject]@{ New = $NewImg; Target = $TargetImg; Backup = $null },
        [pscustomobject]@{ New = $NewDir; Target = $TargetDir; Backup = $null }
    )
    $transaction = [guid]::NewGuid().ToString("N")
    foreach ($pair in $pairs) {
        if (-not [System.IO.File]::Exists($pair.New)) {
            throw ("'{0}' was written but is no longer there. Antivirus or a folder-sync client may have removed it; the originals were not changed." -f $pair.New)
        }
        if (-not [System.IO.File]::Exists($pair.Target)) {
            throw "Original archive component is missing: $($pair.Target)"
        }
        $pair.Backup = "$($pair.Target).imginject-backup.$transaction"
    }
    $movedOriginals = [Collections.Generic.List[object]]::new()
    $installed = [Collections.Generic.List[object]]::new()
    try {
        foreach ($pair in $pairs) {
            [System.IO.File]::Move($pair.Target, $pair.Backup)
            $movedOriginals.Add($pair)
        }
        foreach ($pair in $pairs) {
            [System.IO.File]::Move($pair.New, $pair.Target)
            $installed.Add($pair)
        }
    } catch {
        $installError = $_.Exception.Message
        $rollbackErrors = [Collections.Generic.List[string]]::new()
        for ($index = $installed.Count - 1; $index -ge 0; $index--) {
            $pair = $installed[$index]
            try {
                if ([System.IO.File]::Exists($pair.Target)) {
                    [System.IO.File]::Delete($pair.Target)
                }
            } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        for ($index = $movedOriginals.Count - 1; $index -ge 0; $index--) {
            $pair = $movedOriginals[$index]
            try {
                if ([System.IO.File]::Exists($pair.Backup) -and
                    -not [System.IO.File]::Exists($pair.Target)) {
                    [System.IO.File]::Move($pair.Backup, $pair.Target)
                }
            } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        if ($rollbackErrors.Count -ne 0) {
            throw ("Archive install failed: {0}. Rollback also reported: {1}. Preserve every *.imginject-backup.* file for manual recovery." -f
                $installError, ($rollbackErrors -join '; '))
        }
        throw "Archive install failed and both originals were restored: $installError"
    }
    foreach ($pair in $pairs) {
        [System.IO.File]::Delete($pair.Backup)
    }
}

if ((Get-Item -LiteralPath $outImg).Length -lt $SECTOR) {
    throw "The rebuilt archive '$outImg' is empty; the original was left untouched."
}
Install-ArchivePair $outImg $Img "$dirPath.new" $dirPath

Write-Output ("entries replaced in archive: {0}" -f $replaced)
# Force array semantics for zero or one unused names under StrictMode.
$unused = @($repl.Keys | Where-Object { -not $used.ContainsKey($_) })
Write-Output ("supplied files with no matching entry: {0}" -f $unused.Count)
$unused | Sort-Object | Select-Object -First 20 | ForEach-Object { Write-Output "    $_" }
