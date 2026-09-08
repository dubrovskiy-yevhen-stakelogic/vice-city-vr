[CmdletBinding()]
param([string]$Work)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$kit = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = Join-Path $kit 'CUTSCENE_MODE.bat'
if (-not $Work) { $Work = Join-Path ([IO.Path]::GetTempPath()) ('vc-cutscene-' + [Guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $Work) { throw 'Test output must be a new directory.' }
[void][IO.Directory]::CreateDirectory($Work)
$utf8 = New-Object Text.UTF8Encoding($false)
$passed = 0

function Assert-True($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function New-Fixture([string]$Name, [byte[]]$Bytes, [bool]$WithExe = $true) {
    $dir = Join-Path $Work $Name
    [void][IO.Directory]::CreateDirectory($dir)
    [IO.File]::Copy($source, (Join-Path $dir 'CUTSCENE_MODE.bat'))
    if ($WithExe) { [IO.File]::WriteAllBytes((Join-Path $dir 'reVC.exe'), [byte[]]@()) }
    if ($null -ne $Bytes) { [IO.File]::WriteAllBytes((Join-Path $dir 'vr_settings.ini'), $Bytes) }
    return $dir
}
function Run-Switch([string]$Dir, [string]$Mode, [bool]$Success = $true) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /v:off /c ""%VCVR_TEST_BAT%" ' + $Mode + '"'
    $start.EnvironmentVariables['VCVR_TEST_BAT'] = Join-Path $Dir 'CUTSCENE_MODE.bat'
    $start.WorkingDirectory = $Work
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($start)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    [IO.File]::AppendAllText((Join-Path $Work 'output.log'), "$Dir $Mode`r`n$stdout`r`n$stderr`r`n")
    Assert-True (($p.ExitCode -eq 0) -eq $Success) "Unexpected exit $($p.ExitCode): $stdout $stderr"
    $p.Dispose()
}
function Pass([string]$Name) { $script:passed++; Write-Host "PASS: $Name" }

$parts = [IO.File]::ReadAllText($source) -split '(?m)^# POWERSHELL_PAYLOAD\r?$', 2
Assert-True ($parts.Count -eq 2) 'Missing embedded payload.'
$tokens = $null; $errors = $null
[void][Management.Automation.Language.Parser]::ParseInput($parts[1], [ref]$tokens, [ref]$errors)
Assert-True ($errors.Count -eq 0) "Payload parse failed: $errors"
Pass 'PowerShell payload parses'

$dir = New-Fixture 'missing ini' $null
Run-Switch $dir stereo
$ini = Join-Path $dir 'vr_settings.ini'
Assert-True ([IO.File]::ReadAllText($ini) -match '(?m)^CutsceneMode=1\r?$') 'Missing stereo setting.'
$hash = (Get-FileHash -LiteralPath $ini).Hash
Run-Switch $dir stereo
Assert-True ((Get-FileHash -LiteralPath $ini).Hash -eq $hash) 'Repeated mode rewrote the INI.'
Assert-True (@(Get-ChildItem -LiteralPath $dir -Filter '*.bak').Count -eq 0) 'No-op created a backup.'
Run-Switch $dir cinema
Assert-True ([IO.File]::ReadAllText($ini) -match '(?m)^CutsceneMode=0\r?$') 'Cinema restore failed.'
Assert-True (@(Get-ChildItem -LiteralPath $dir -Filter '*.bak').Count -eq 1) 'Restore backup missing.'
Pass 'Missing file, stereo, no-op and cinema restore'

$original = "; keep this comment`r`n[Audio]`r`nVolume=71`r`n[VR]`r`nFlatMode=0`r`nCutsceneMode=0`r`nRenderScale=88`r`n[Other]`r`nCutsceneMode=7`r`n"
$encodings = @([Text.Encoding]::ASCII, $utf8, [Text.Encoding]::Unicode)
foreach ($encoding in $encodings) {
    $bytes = [byte[]]($encoding.GetPreamble() + $encoding.GetBytes($original))
    $dir = New-Fixture ('encoding-' + $encoding.WebName) $bytes
    Run-Switch $dir stereo
    $backup = @(Get-ChildItem -LiteralPath $dir -Filter '*.bak')
    Assert-True ($backup.Count -eq 1) 'Expected one exact backup.'
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backup[0].FullName)) -eq [Convert]::ToBase64String($bytes)) 'Backup changed original bytes.'
    $actual = [IO.File]::ReadAllText((Join-Path $dir 'vr_settings.ini'))
    $expected = $original.Replace("[VR]`r`nFlatMode=0`r`nCutsceneMode=0", "[VR]`r`nFlatMode=0`r`nCutsceneMode=1")
    Assert-True ($actual -ceq $expected) 'Other settings, sections or comments changed.'
    Pass ('Preserves settings and backup: ' + $encoding.WebName)
}
$special = "spaces & brackets [1] ! apostrophe ' " + [char]0x0418 + [char]0x0433
$dir = New-Fixture $special ($utf8.GetBytes("[Audio]`r`nVolume=42`r`n"))
Run-Switch $dir stereo
Assert-True ([IO.File]::ReadAllText((Join-Path $dir 'vr_settings.ini')).Contains('Volume=42')) 'Missing-section update lost data.'
Pass 'Special-character path, different working directory and missing VR section'

$dir = New-Fixture 'read only' ($utf8.GetBytes($original))
$ini = Join-Path $dir 'vr_settings.ini'
$hash = (Get-FileHash -LiteralPath $ini).Hash
[IO.File]::SetAttributes($ini, [IO.FileAttributes]::ReadOnly)
try { Run-Switch $dir stereo $false } finally { [IO.File]::SetAttributes($ini, [IO.FileAttributes]::Normal) }
Assert-True ((Get-FileHash -LiteralPath $ini).Hash -eq $hash) 'Read-only settings changed.'
Pass 'Read-only failure preserves settings'

$dir = New-Fixture 'missing exe' $null $false
Run-Switch $dir stereo $false
Assert-True (-not [IO.File]::Exists((Join-Path $dir 'vr_settings.ini'))) 'Wrong directory was modified.'
Pass 'Missing game is refused in command-line mode'
$dir = New-Fixture 'invalid mode' $null
Run-Switch $dir invalid $false
Assert-True (-not [IO.File]::Exists((Join-Path $dir 'vr_settings.ini'))) 'Invalid mode changed settings.'
Pass 'Invalid mode is refused'
Write-Host "$passed checks passed. Fixtures and output: $Work"
