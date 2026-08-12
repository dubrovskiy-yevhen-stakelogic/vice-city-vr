# Extracts named entries from an IMG v1 archive into a target directory.
param(
    [Parameter(Mandatory=$true)][string]$Img,
    [Parameter(Mandatory=$true)][string[]]$Entries,
    [Parameter(Mandatory=$true)][string]$Out
)
$SECT = 2048
$dirPath = [System.IO.Path]::ChangeExtension($Img, ".dir")
$db = [System.IO.File]::ReadAllBytes($dirPath)
$n = $db.Length / 32
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$fs = [System.IO.File]::OpenRead($Img)
$want = @{}; foreach ($e in $Entries) { $want[$e.ToLower()] = $true }

for ($i = 0; $i -lt $n; $i++) {
    $b = $i*32
    $z = [Array]::IndexOf($db, [byte]0, $b+8) - ($b+8); if ($z -lt 0 -or $z -gt 24) { $z = 24 }
    $name = [System.Text.Encoding]::ASCII.GetString($db, $b+8, $z)
    if (-not $want.ContainsKey($name.ToLower())) { continue }
    $off = [int64][BitConverter]::ToUInt32($db, $b) * $SECT
    $len = [int64][BitConverter]::ToUInt32($db, $b+4) * $SECT
    $fs.Position = $off
    $buf = New-Object byte[] $len
    $null = $fs.Read($buf, 0, [int]$len)
    [System.IO.File]::WriteAllBytes((Join-Path $Out $name), $buf)
    Write-Output ("extracted {0} ({1:N0} bytes)" -f $name, $len)
}
$fs.Close()
