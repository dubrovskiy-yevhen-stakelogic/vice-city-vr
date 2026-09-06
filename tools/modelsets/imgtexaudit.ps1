# Audits every TXD inside an IMG v1 archive and reports which entries contain
# textures that stream badly: uncompressed 24/32-bit, or any texture at 64px+
# with a single mip level. CandidateList is configurable so release builds can
# keep generated state in their private staging directory instead of modifying
# the read-only tools folder or colliding with another build.
param(
  [Parameter(Mandatory=$true)][string]$Img,
  [string]$CandidateList = (Join-Path $PSScriptRoot "txd_candidates.txt")
)
$SECT=2048
$db=[System.IO.File]::ReadAllBytes([System.IO.Path]::ChangeExtension($Img,".dir"))
$n=$db.Length/32
$fs=[System.IO.File]::OpenRead($Img)
$bad=@()
for($i=0;$i -lt $n;$i++){
  $b=$i*32
  $z=[Array]::IndexOf($db,[byte]0,$b+8)-($b+8); if($z -lt 0 -or $z -gt 24){$z=24}
  $name=[System.Text.Encoding]::ASCII.GetString($db,$b+8,$z)
  if($name -notmatch '\.txd$'){continue}
  $off=[int64][BitConverter]::ToUInt32($db,$b)*$SECT
  $len=[int64][BitConverter]::ToUInt32($db,$b+4)*$SECT
  if($len -le 0 -or $len -gt 80MB){continue}
  $fs.Position=$off
  $buf=New-Object byte[] $len
  $r=$fs.Read($buf,0,[int]$len)
  # walk texture natives
  $uncmp=0; $nomip=0; $texs=0; $wasted=[int64]0
  $stack=New-Object System.Collections.Stack
  $stack.Push(@(12,[int64]$r,0))   # skip outer header: pos, end, parent
  $pos=12
  function ScanChunks([byte[]]$data,[int]$start,[long]$end,[uint32]$parent){
    $p=$start
    while($p+12 -le $end){
      $t=[BitConverter]::ToUInt32($data,$p); $s=[BitConverter]::ToUInt32($data,$p+4)
      $de=$p+12+$s
      if($de -gt $end -or $s -eq 0){break}
      if($t -eq 0x01 -and $parent -eq 0x15){
        $w=[BitConverter]::ToUInt16($data,$p+12+80)
        $h=[BitConverter]::ToUInt16($data,$p+12+82)
        $depth=$data[$p+12+84]; $lev=$data[$p+12+85]; $comp=$data[$p+12+87]
        $script:texs++
        $dim=[Math]::Max($w,$h)
        if($comp -eq 0 -and $depth -ge 24){
          $script:uncmp++
          $script:wasted += [int64]$w*$h*($depth/8)
        } elseif($lev -le 1 -and $dim -ge 64){
          $script:nomip++
        }
      } elseif($t -in 0x16,0x15){ ScanChunks $data ($p+12) $de $t }
      $p=$de
    }
  }
  $script:texs=0;$script:uncmp=0;$script:nomip=0;$script:wasted=[int64]0
  ScanChunks $buf 0 $r 0
  if($script:uncmp -gt 0 -or $script:nomip -gt 0){
    $bad += [pscustomobject]@{Name=$name;KB=[math]::Round($len/1KB);Texs=$script:texs;Uncompressed=$script:uncmp;NoMip=$script:nomip;UncmpMB=[math]::Round($script:wasted/1MB,1)}
  }
}
$fs.Close()
Write-Output ("txd entries with problems: {0}" -f $bad.Count)
$bad | Sort-Object UncmpMB -Descending | Select-Object -First 25 | Format-Table -AutoSize
Write-Output ("total uncompressed pixel data: {0:N0} MB" -f (($bad | Measure-Object UncmpMB -Sum).Sum))
Write-Output ("total no-mip textures: {0}" -f (($bad | Measure-Object NoMip -Sum).Sum))
# dump the full list for the extraction step
$bad | ForEach-Object { $_.Name } | Sort-Object -Unique |
  Set-Content -LiteralPath $CandidateList -Encoding ASCII
Write-Output "candidate list written to $CandidateList"
