# Merges TXD dictionaries: keeps every texture from -Base, and appends only the
# textures from -Extra whose names are not already in the base. Used to pull a
# few missing textures out of a pack without inheriting its worse versions of
# the ones we already have.
param(
    [Parameter(Mandatory=$true)][string]$Base,
    [Parameter(Mandatory=$true)][string]$Extra,
    [Parameter(Mandatory=$true)][string]$Out
)

function Read-Txd([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    # outer chunk: type, size, version
    $type = [BitConverter]::ToUInt32($b,0)
    if ($type -ne 0x16) { throw "$path is not a texture dictionary (type $type)" }
    $outerSize = [BitConverter]::ToUInt32($b,4)
    $version   = [BitConverter]::ToUInt32($b,8)
    $pos = 12
    # struct chunk
    $sType = [BitConverter]::ToUInt32($b,$pos)
    $sSize = [BitConverter]::ToUInt32($b,$pos+4)
    $sVer  = [BitConverter]::ToUInt32($b,$pos+8)
    if ($sType -ne 0x01) { throw "$path has no struct chunk" }
    $numTex   = [BitConverter]::ToUInt16($b,$pos+12)
    $deviceId = [BitConverter]::ToUInt16($b,$pos+14)
    $pos += 12 + $sSize
    $end = 12 + $outerSize

    $tex = @(); $trailing = @()
    while ($pos + 12 -le $end) {
        $cType = [BitConverter]::ToUInt32($b,$pos)
        $cSize = [BitConverter]::ToUInt32($b,$pos+4)
        $total = 12 + $cSize
        if ($cType -eq 0x15) {
            # name lives at struct payload +8 (after platformId and filter/addr)
            $nameOff = $pos + 12 + 12 + 8
            $z = [Array]::IndexOf($b, [byte]0, $nameOff) - $nameOff
            if ($z -lt 0 -or $z -gt 32) { $z = 32 }
            $name = [System.Text.Encoding]::ASCII.GetString($b,$nameOff,$z)
            $chunk = New-Object byte[] $total
            [Array]::Copy($b,$pos,$chunk,0,$total)
            $tex += [pscustomobject]@{ Name=$name; Bytes=$chunk }
        } else {
            $chunk = New-Object byte[] $total
            [Array]::Copy($b,$pos,$chunk,0,$total)
            # The comma keeps the byte[] intact as one element; += without it
            # flattens the chunk into individual bytes.
            $trailing += ,$chunk
        }
        $pos += $total
    }
    [pscustomobject]@{ Version=$version; SVer=$sVer; NumTex=$numTex; DeviceId=$deviceId; Tex=$tex; Trailing=$trailing }
}

$bd = Read-Txd $Base
$ed = Read-Txd $Extra
Write-Output ("base : {0} textures (declared {1})" -f $bd.Tex.Count, $bd.NumTex)
Write-Output ("extra: {0} textures (declared {1})" -f $ed.Tex.Count, $ed.NumTex)

$have = @{}; foreach ($t in $bd.Tex) { $have[$t.Name.ToLower()] = $true }
# @() so that zero or one appended texture still yields an array: the caller
# may run under Set-StrictMode, where .Count on $null or a scalar throws.
$add = @($ed.Tex | Where-Object { -not $have.ContainsKey($_.Name.ToLower()) })
Write-Output ("adding {0} textures absent from base:" -f $add.Count)
$add | ForEach-Object { "    $($_.Name)" }

$all = @($bd.Tex) + @($add)
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
# struct chunk
$bw.Write([uint32]0x01); $bw.Write([uint32]4); $bw.Write([uint32]$bd.SVer)
$bw.Write([uint16]$all.Count); $bw.Write([uint16]$bd.DeviceId)
foreach ($t in $all) { $bw.Write($t.Bytes,0,$t.Bytes.Length) }
foreach ($t in $bd.Trailing) { $bw.Write($t,0,$t.Length) }
$bw.Flush()
$body = $ms.ToArray()
$bw.Close()

$fs = [System.IO.File]::Create($Out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([uint32]0x16); $w.Write([uint32]$body.Length); $w.Write([uint32]$bd.Version)
$w.Write($body,0,$body.Length)
$w.Close(); $fs.Close()
Write-Output ("wrote {0} ({1:N1} MB, {2} textures)" -f $Out, ((Get-Item -LiteralPath $Out).Length/1MB), $all.Count)
