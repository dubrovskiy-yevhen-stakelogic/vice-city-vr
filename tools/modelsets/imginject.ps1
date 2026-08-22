# Rebuilds a GTA IMG version 1 archive (gta3.img + gta3.dir), replacing entries
# whose name matches a file supplied on disk. Entries keep their dir order;
# offsets and sizes are recomputed.
param(
    [Parameter(Mandatory=$true)][string]$Img,
    [Parameter(Mandatory=$true)][string[]]$From,
    [string[]]$Exclude = @()
)

# A .NET method that throws -- a full disk, a file the antivirus is holding --
# otherwise ends only its own statement, and the run limps on to the archive
# swap and reports a missing .new file instead of the real cause.
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
$n = $db.Length / 32
$entries = @()
for ($i = 0; $i -lt $n; $i++) {
    $b = $i*32
    $z = [Array]::IndexOf($db, [byte]0, $b+8) - ($b+8); if ($z -lt 0 -or $z -gt 24) { $z = 24 }
    $entries += [pscustomobject]@{
        Name   = [System.Text.Encoding]::ASCII.GetString($db, $b+8, $z)
        Offset = [int64][BitConverter]::ToUInt32($db, $b) * $SECTOR
        Size   = [int64][BitConverter]::ToUInt32($db, $b+4) * $SECTOR
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

# The rebuild writes a full second copy of the archive beside the original, so
# the drive has to have room for it. Running out halfway is what turns into an
# unhelpful "gta3.img.new does not exist" three statements later.
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
                if ($got -le 0) { break }
                $out.Write($buf, 0, $got)
                $written += $got
                $left -= $got
            }
        }

        $tail = $written % $SECTOR
        if ($tail -ne 0) { $out.Write($pad, 0, $SECTOR - $tail); $written += ($SECTOR - $tail) }
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
    # Nothing has been swapped yet, so the archive on disk is still the one we
    # started from. Clear the half-written copy so a retry starts clean.
    if (Test-Path -LiteralPath $outImg) {
        Remove-Item -LiteralPath $outImg -Force -ErrorAction SilentlyContinue
    }
    throw ("Rebuilding '{0}' failed and the original was left untouched: {1}" -f
        $Img, $failure.Exception.Message)
}

[System.IO.File]::WriteAllBytes("$dirPath.new", $newDir)


# The swap goes through a backup, and through .NET rather than Remove-Item and
# Rename-Item. Two reasons, both seen in the wild: those cmdlets treat a path
# as a wildcard, so a folder named like "C:\Users\Bob [PC]" makes them report a
# file that plainly exists as missing; and deleting the original first means
# that if anything removes the rebuilt file between writing it and moving it --
# security software scanning a fresh multi-gigabyte archive is the usual
# suspect -- the archive that was there is gone too, and a 3.5 GB copy has to
# be redone. Here nothing is deleted until its replacement is in place.
function Swap-InPlace([string]$New, [string]$Target) {
    if (-not [System.IO.File]::Exists($New)) {
        throw ("'{0}' was written but is no longer there. Antivirus or a folder-sync client is the usual cause; build outside Downloads and Documents, or exclude the build folder from real-time scanning." -f $New)
    }
    $backup = "$Target.old"
    if ([System.IO.File]::Exists($backup)) { [System.IO.File]::Delete($backup) }
    [System.IO.File]::Move($Target, $backup)
    try {
        [System.IO.File]::Move($New, $Target)
    } catch {
        [System.IO.File]::Move($backup, $Target)
        throw ("Could not put the rebuilt '{0}' in place, the original is back: {1}" -f
            $Target, $_.Exception.Message)
    }
    [System.IO.File]::Delete($backup)
}

if ((Get-Item -LiteralPath $outImg).Length -lt $SECTOR) {
    throw "The rebuilt archive '$outImg' is empty; the original was left untouched."
}
Swap-InPlace $outImg $Img
Swap-InPlace "$dirPath.new" $dirPath

Write-Output ("entries replaced in archive: {0}" -f $replaced)
# @() so that zero or one leftover name still yields an array: the caller may
# run under Set-StrictMode, where .Count on $null or a scalar string throws.
$unused = @($repl.Keys | Where-Object { -not $used.ContainsKey($_) })
Write-Output ("supplied files with no matching entry: {0}" -f $unused.Count)
$unused | Sort-Object | Select-Object -First 20 | ForEach-Object { Write-Output "    $_" }
