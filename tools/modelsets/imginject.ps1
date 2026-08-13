# Rebuilds a GTA IMG version 1 archive (gta3.img + gta3.dir), replacing entries
# whose name matches a file supplied on disk. Entries keep their dir order;
# offsets and sizes are recomputed.
param(
    [Parameter(Mandatory=$true)][string]$Img,
    [Parameter(Mandatory=$true)][string[]]$From
)

$SECTOR = 2048
$dirPath = [System.IO.Path]::ChangeExtension($Img, ".dir")

# collect replacements, later sources win
$repl = @{}
foreach ($src in $From) {
    if (-not (Test-Path $src)) { Write-Output "  missing source: $src"; continue }
    foreach ($f in Get-ChildItem $src -File) {
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

$outImg = "$Img.new"
$in  = [System.IO.File]::OpenRead($Img)
$out = [System.IO.File]::Create($outImg)
$newDir = New-Object byte[] ($entries.Count * 32)
$buf = New-Object byte[] (8*1024*1024)
$pad = New-Object byte[] $SECTOR
$nextSector = [int64]0
$replaced = 0
$used = @{}

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

$in.Close(); $out.Close()
[System.IO.File]::WriteAllBytes("$dirPath.new", $newDir)

Remove-Item $Img -Force; Rename-Item $outImg $Img
Remove-Item $dirPath -Force; Rename-Item "$dirPath.new" $dirPath

Write-Output ("entries replaced in archive: {0}" -f $replaced)
# @() so that zero or one leftover name still yields an array: the caller may
# run under Set-StrictMode, where .Count on $null or a scalar string throws.
$unused = @($repl.Keys | Where-Object { -not $used.ContainsKey($_) })
Write-Output ("supplied files with no matching entry: {0}" -f $unused.Count)
$unused | Sort-Object | Select-Object -First 20 | ForEach-Object { Write-Output "    $_" }
