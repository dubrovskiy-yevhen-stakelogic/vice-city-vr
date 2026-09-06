[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-common.ps1')

$script:FixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('vice-city-dlss-tests-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($script:FixtureRoot)
$script:Passed = 0
$script:Failed = 0
$script:FixtureNumber = 0

function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-TestThrows([scriptblock]$Action, [string]$Pattern) {
    $caught = $null
    try { & $Action | Out-Null } catch { $caught = $_.Exception.Message }
    Assert-Test ($null -ne $caught) 'Expected an exception, but the operation succeeded.'
    Assert-Test ($caught -match $Pattern) "Unexpected exception: $caught"
}

function Invoke-Test([string]$Name, [scriptblock]$Action) {
    try {
        & $Action
        $script:Passed++
        Write-Host "PASS $Name"
    } catch {
        $script:Failed++
        Write-Host "FAIL $Name : $($_.Exception.Message)"
        Write-Host $_.ScriptStackTrace
    }
}

function New-TestDirectory([string]$Name) {
    $script:FixtureNumber++
    $path = Join-Path $script:FixtureRoot ($script:FixtureNumber.ToString('000') + '-' + $Name)
    [void][IO.Directory]::CreateDirectory($path)
    return $path
}

function New-TestPe([string]$Path, [byte]$Marker = 1, [uint16]$Machine = 0x8664) {
    $bytes = New-Object byte[] 256
    [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($bytes, 0)
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3c)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes, 0x80)
    [BitConverter]::GetBytes($Machine).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([uint16]0x20b).CopyTo($bytes, 0x98)
    $bytes[255] = $Marker
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function New-TestSpec([string]$Path, [string]$Name) {
    return [pscustomobject]@{
        name = $Name
        size = (Get-Item -LiteralPath $Path).Length
        sha256 = Get-DlssHash $Path
        signature = 'ModifiedPinned'
        archivePath = 'runtime/' + $Name
    }
}

function New-TestGame {
    $path = New-TestDirectory 'game'
    New-TestPe (Join-Path $path 'reVC.exe') 100
    New-TestPe (Join-Path $path 'nvngx.dll_dlssnr.dll') 101
    [IO.File]::WriteAllText((Join-Path $path 'vr-settings.ini'), 'Keep this user configuration.')
    return $path
}

function Get-TestSnapshot([string]$Root) {
    $result = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -File) {
        $result[$file.Name] = Get-DlssHash $file.FullName
    }
    return $result
}

function Assert-TestSnapshot($Expected, [string]$Root) {
    $actual = Get-TestSnapshot $Root
    Assert-Test ($Expected.Count -eq $actual.Count) 'Root file count changed unexpectedly.'
    foreach ($name in $Expected.Keys) {
        Assert-Test ($actual.ContainsKey($name) -and $actual[$name] -ceq $Expected[$name]) "Unexpected change to $name"
    }
}

function Write-TestManifest([string]$Backup, $Manifest) {
    [IO.File]::WriteAllText((Join-Path $Backup 'manifest.json'), ($Manifest | ConvertTo-Json -Depth 8))
}

function New-TestZip([string]$Path, [string[]]$Entries, [byte[]]$Bytes) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $Entries) {
            $entry = $archive.CreateEntry($name)
            $stream = $entry.Open()
            try { $stream.Write($Bytes, 0, $Bytes.Length) } finally { $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
}

$script:Stage = New-TestDirectory 'stage'
$script:Specs = @()
$marker = 10
foreach ($name in $script:DlssNames) {
    $path = Join-Path $script:Stage $name
    New-TestPe $path ([byte]$marker)
    $script:Specs += New-TestSpec $path $name
    $marker++
}
$script:BaseSpecs = @($script:Specs | Where-Object { $script:DlssBaseNames -contains $_.name })

Invoke-Test 'BASE selects exactly five files without an NR model package or catalog mutation' {
    $catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.json') -Raw | ConvertFrom-Json
    $packages = @(Get-DlssProfilePackages $catalog 'BASE')
    Assert-Test ($packages.Count -eq 2) 'BASE must use only Streamline and DLSS SR downloads.'
    Assert-Test (@($packages[0].files).Count -eq 4) 'BASE did not filter Streamline to four files.'
    Assert-Test (@($catalog.streamline.files).Count -eq 5) 'BASE changed the shared catalog.'
    Assert-DlssRuntimeSet @($packages | ForEach-Object { $_.files }) 'BASE'
    foreach ($profile in @('RTX50', 'RTX40')) {
        $nrPackages = @(Get-DlssProfilePackages $catalog $profile)
        Assert-Test ($nrPackages.Count -eq 3) 'NR profile lost a required package.'
        Assert-DlssRuntimeSet @($nrPackages | ForEach-Object { $_.files }) $profile
    }
}

Invoke-Test 'BASE rejects a catalog missing a required loader file' {
    $catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.json') -Raw | ConvertFrom-Json
    $catalog.streamline.files = @($catalog.streamline.files | Where-Object { $_.name -ne 'sl.interposer.dll' })
    Assert-TestThrows { Get-DlssProfilePackages $catalog 'BASE' } 'complete, unique'
}

Invoke-Test 'x64 PE and complete pinned fixture payload accepted' {
    foreach ($spec in $script:Specs) { Assert-DlssPayload (Join-Path $script:Stage $spec.name) $spec }
}

Invoke-Test 'hash mismatch rejected' {
    $path = Join-Path (New-TestDirectory 'bad-hash') $script:DlssNames[0]
    New-TestPe $path 44
    Assert-TestThrows { Assert-DlssPayload $path $script:Specs[0] } 'verification failed'
}

Invoke-Test 'wrong PE architecture rejected even with matching hash' {
    $path = Join-Path (New-TestDirectory 'x86') $script:DlssNames[0]
    New-TestPe $path 1 0x14c
    $spec = New-TestSpec $path $script:DlssNames[0]
    Assert-TestThrows { Assert-DlssPayload $path $spec } 'x64'
}

Invoke-Test 'unknown signature policy rejected' {
    $spec = New-TestSpec (Join-Path $script:Stage $script:DlssNames[0]) $script:DlssNames[0]
    $spec.signature = 'Skip'
    Assert-TestThrows { Assert-DlssPayload (Join-Path $script:Stage $spec.name) $spec } 'Unknown signature policy'
}

Invoke-Test 'unsigned fixture cannot pass NVIDIA signature policy' {
    $spec = New-TestSpec (Join-Path $script:Stage $script:DlssNames[0]) $script:DlssNames[0]
    $spec.signature = 'NVIDIA'
    Assert-TestThrows { Assert-DlssPayload (Join-Path $script:Stage $spec.name) $spec } 'NVIDIA signature validation failed'
}

Invoke-Test 'missing game executable rejected' {
    $game = New-TestGame
    Remove-Item -LiteralPath (Join-Path $game 'reVC.exe')
    Assert-TestThrows { Get-DlssGameDirectory $game } 'Missing reVC.exe'
}

Invoke-Test 'missing project forwarder rejected' {
    $game = New-TestGame
    Remove-Item -LiteralPath (Join-Path $game 'nvngx.dll_dlssnr.dll')
    Assert-TestThrows { Get-DlssGameDirectory $game } 'Missing nvngx.dll_dlssnr.dll'
}

Invoke-Test 'source kit folder rejected' {
    $game = New-TestGame
    [IO.File]::WriteAllText((Join-Path $game 'patch-manifest.json'), '{}')
    Assert-TestThrows { Get-DlssGameDirectory $game } 'source kit'
}

Invoke-Test 'alternate data stream path rejected' {
    Assert-TestThrows { Assert-DlssPlainPath (Join-Path $script:FixtureRoot 'file.dll:payload') } 'Alternate data stream|format is not supported'
}

Invoke-Test 'running game guard refuses without terminating process' {
    $script:ProcessNameSeen = ''
    function Get-Process {
        param([string]$Name, [string]$ErrorAction)
        $script:ProcessNameSeen = $Name
        return [pscustomobject]@{ Id = 123; ProcessName = 'reVC' }
    }
    Assert-TestThrows { Assert-DlssGameClosed } 'Close Vice City VR'
    Assert-Test ($script:ProcessNameSeen -ceq 'reVC') 'Guard did not inspect the game process.'
}

# Transaction tests operate only on synthetic PE data; no fixture is executable.
$script:ActualGuard = ${function:Assert-DlssGameClosed}
function Assert-DlssGameClosed { }

Invoke-Test 'incomplete and duplicate runtime sets rejected before writes' {
    $game = New-TestGame
    $before = Get-TestSnapshot $game
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $script:Specs[0..5] 'RTX50' } 'complete, unique'
    $duplicate = @($script:Specs[0..5]) + @($script:Specs[0])
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $duplicate 'RTX50' } 'complete, unique'
    Assert-TestSnapshot $before $game
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $game 'dlss5-backups'))) 'Rejected install created a backup.'
}

Invoke-Test 'BASE rejects missing, duplicate, substituted and extra NR files before writes' {
    $game = New-TestGame
    $before = Get-TestSnapshot $game
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $script:BaseSpecs[0..3] 'BASE' } 'complete, unique'
    $duplicate = @($script:BaseSpecs[0..3]) + @($script:BaseSpecs[0])
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $duplicate 'BASE' } 'complete, unique'
    $substituted = @($script:BaseSpecs[0..3]) + @($script:Specs[6])
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $substituted 'BASE' } 'complete, unique'
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $script:Specs 'BASE' } 'complete, unique'
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $script:BaseSpecs 'RTX40' } 'complete, unique'
    Assert-TestSnapshot $before $game
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $game 'dlss5-backups'))) 'Rejected install created a backup.'
}

Invoke-Test 'invalid runtime profile rejected before writes' {
    $game = New-TestGame
    $before = Get-TestSnapshot $game
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $script:Specs 'Fixture' } 'Invalid runtime profile'
    Assert-TestSnapshot $before $game
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $game 'dlss5-backups'))) 'Invalid profile created a backup.'
}

Invoke-Test 'BASE installs exactly five files, is idempotent and restores cleanly' {
    $game = New-TestGame
    New-TestPe (Join-Path $game 'sl.interposer.dll') 210
    $before = Get-TestSnapshot $game
    $backup = Install-DlssFiles $game $script:Stage $script:BaseSpecs 'BASE'
    $after = Get-TestSnapshot $game
    Assert-Test ($after.Count -eq 8) 'BASE wrote unexpected files into the game root.'
    foreach ($spec in $script:BaseSpecs) {
        Assert-Test ($after[$spec.name] -ceq $spec.sha256) "BASE file $($spec.name) was not installed."
    }
    Assert-Test (-not $after.ContainsKey('sl.dlss_nr.dll')) 'BASE installed the NR plugin.'
    Assert-Test (-not $after.ContainsKey('nvngx_dlssnr.dll')) 'BASE installed an NR model.'
    $journal = Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
    Assert-Test ($journal.profile -ceq 'BASE' -and @($journal.files).Count -eq 5) 'BASE backup has the wrong profile or file count.'
    $second = Install-DlssFiles $game $script:Stage $script:BaseSpecs 'BASE'
    Assert-Test ($null -eq $second) 'Identical BASE install was not a no-op.'
    Assert-Test (@(Get-ChildItem -LiteralPath (Join-Path $game 'dlss5-backups') -Directory).Count -eq 1) 'BASE reinstall added a backup.'
    Restore-DlssBackup $game $backup
    Assert-TestSnapshot $before $game
    Restore-DlssBackup $game $backup
    Assert-TestSnapshot $before $game
}

Invoke-Test 'BASE installation and restoration preserve existing and subsequently changed NR files' {
    $game = New-TestGame
    New-TestPe (Join-Path $game 'sl.dlss_nr.dll') 220
    New-TestPe (Join-Path $game 'nvngx_dlssnr.dll') 221
    $before = Get-TestSnapshot $game
    $backup = Install-DlssFiles $game $script:Stage $script:BaseSpecs 'BASE'
    foreach ($name in @('sl.dlss_nr.dll', 'nvngx_dlssnr.dll')) {
        Assert-Test ((Get-DlssHash (Join-Path $game $name)) -ceq $before[$name]) "BASE installation changed $name."
        Assert-Test (-not (Test-Path -LiteralPath (Join-Path $backup $name))) "BASE backed up unrelated $name."
    }
    New-TestPe (Join-Path $game 'nvngx_dlssnr.dll') 222
    $before['nvngx_dlssnr.dll'] = Get-DlssHash (Join-Path $game 'nvngx_dlssnr.dll')
    Restore-DlssBackup $game $backup
    Assert-TestSnapshot $before $game
}

Invoke-Test 'BASE restore rejects NR entries and invalid profiles before writes' {
    $game = New-TestGame
    $backup = Install-DlssFiles $game $script:Stage $script:BaseSpecs 'BASE'
    $journal = Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
    $journal.files[4].name = 'nvngx_dlssnr.dll'
    Write-TestManifest $backup $journal
    $before = Get-TestSnapshot $game
    Assert-TestThrows { Restore-DlssBackup $game $backup } 'Invalid backup manifest'
    Assert-TestSnapshot $before $game
    $journal.profile = 'Unknown'
    Write-TestManifest $backup $journal
    Assert-TestThrows { Restore-DlssBackup $game $backup } 'Invalid runtime profile'
    Assert-TestSnapshot $before $game
}

Invoke-Test 'late corrupt payload prevents all runtime writes' {
    $game = New-TestGame
    $before = Get-TestSnapshot $game
    $specs = @($script:Specs[0..5]) + @(New-TestSpec (Join-Path $script:Stage $script:DlssNames[6]) $script:DlssNames[6])
    $specs[6].sha256 = ('0' * 64)
    Assert-TestThrows { Install-DlssFiles $game $script:Stage $specs 'RTX50' } 'verification failed'
    Assert-TestSnapshot $before $game
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $game 'dlss5-backups'))) 'Rejected install created a backup.'
}

Invoke-Test 'install backs up originals and changes only seven runtime names' {
    $game = New-TestGame
    New-TestPe (Join-Path $game $script:DlssNames[0]) 200
    New-TestPe (Join-Path $game $script:DlssNames[1]) 201
    $before = Get-TestSnapshot $game
    $backup = Install-DlssFiles $game $script:Stage $script:Specs 'RTX50'
    $after = Get-TestSnapshot $game
    Assert-Test ($after.Count -eq 10) 'Installer wrote unexpected game-root files.'
    foreach ($name in $before.Keys) {
        if ($script:DlssNames -notcontains $name) {
            Assert-Test ($after[$name] -ceq $before[$name]) "Installer changed unrelated file $name"
        } else {
            Assert-Test ((Get-DlssHash (Join-Path $backup $name)) -ceq $before[$name]) "Original $name was not preserved."
        }
    }
    foreach ($spec in $script:Specs) {
        Assert-Test ($after[$spec.name] -ceq $spec.sha256) "Runtime $($spec.name) was not installed."
    }
    $journal = Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
    Assert-Test (@($journal.files).Count -eq 7) 'Journal must list all seven changed files.'
}

Invoke-Test 'identical reinstall is a no-op without another backup' {
    $game = New-TestGame
    [void](Install-DlssFiles $game $script:Stage $script:Specs 'RTX50')
    $before = Get-TestSnapshot $game
    $countBefore = @(Get-ChildItem -LiteralPath (Join-Path $game 'dlss5-backups') -Directory).Count
    $second = Install-DlssFiles $game $script:Stage $script:Specs 'RTX50'
    Assert-Test ($null -eq $second) 'Identical install did not report no-op.'
    Assert-TestSnapshot $before $game
    Assert-Test (@(Get-ChildItem -LiteralPath (Join-Path $game 'dlss5-backups') -Directory).Count -eq $countBefore) 'Identical install made a redundant backup.'
}

Invoke-Test 'restore recovers original files and removes newly installed files' {
    $game = New-TestGame
    New-TestPe (Join-Path $game $script:DlssNames[0]) 200
    $before = Get-TestSnapshot $game
    $backup = Install-DlssFiles $game $script:Stage $script:Specs 'RTX50'
    Restore-DlssBackup $game $backup
    Assert-TestSnapshot $before $game
    Restore-DlssBackup $game $backup
    Assert-TestSnapshot $before $game
    Assert-Test (Test-Path -LiteralPath (Join-Path $backup $script:DlssNames[0])) 'Restore removed the backup.'
}

Invoke-Test 'restore preflight preserves all files when a runtime was modified' {
    $game = New-TestGame
    New-TestPe (Join-Path $game $script:DlssNames[0]) 200
    $backup = Install-DlssFiles $game $script:Stage $script:Specs 'RTX50'
    New-TestPe (Join-Path $game $script:DlssNames[6]) 250
    $beforeRestore = Get-TestSnapshot $game
    Assert-TestThrows { Restore-DlssBackup $game $backup } 'Changed since installation'
    Assert-TestSnapshot $beforeRestore $game
}

Invoke-Test 'failed install rolls back previously replaced files' {
    $game = New-TestGame
    New-TestPe (Join-Path $game $script:DlssNames[0]) 200
    $before = Get-TestSnapshot $game
    $script:AtomicOriginal = ${function:Set-DlssFileAtomic}
    $script:AtomicCalls = 0
    function Set-DlssFileAtomic([string]$Source, [string]$Target, [string]$ExpectedSha256) {
        $script:AtomicCalls++
        if ($script:AtomicCalls -eq 2) { throw 'Injected copy failure' }
        & $script:AtomicOriginal $Source $Target $ExpectedSha256
    }
    try {
        Assert-TestThrows { Install-DlssFiles $game $script:Stage $script:Specs 'RTX50' } 'Injected copy failure'
        Assert-Test ($script:AtomicCalls -ge 3) 'Rollback did not restore the first file.'
        Assert-TestSnapshot $before $game
        Assert-Test (@(Get-ChildItem -LiteralPath $game -Filter '.dlss5-*.tmp').Count -eq 0) 'Temporary runtime file was left behind.'
    } finally {
        Set-Item -LiteralPath Function:\Set-DlssFileAtomic -Value $script:AtomicOriginal
    }
}

Invoke-Test 'atomic replacement refuses a source changed after verification' {
    $folder = New-TestDirectory 'atomic-mutation'
    $source = Join-Path $folder 'source.dll'
    $target = Join-Path $folder 'target.dll'
    New-TestPe $source 40
    New-TestPe $target 41
    $expectedSource = Get-DlssHash $source
    $originalTarget = Get-DlssHash $target
    New-TestPe $source 42
    Assert-TestThrows { Set-DlssFileAtomic $source $target $expectedSource } 'changed|verification|checksum'
    Assert-Test ((Get-DlssHash $target) -ceq $originalTarget) 'Changed source was published to the target.'
    Assert-Test (@(Get-ChildItem -LiteralPath $folder -Filter '.dlss5-*.tmp').Count -eq 0) 'Rejected source left a temporary payload.'
}

foreach ($badKind in @('unknown', 'duplicate', 'traversal', 'nonboolean')) {
    Invoke-Test "restore rejects $badKind manifest before writes" {
        $game = New-TestGame
        $backup = Install-DlssFiles $game $script:Stage $script:Specs 'RTX50'
        $journal = Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
        switch ($badKind) {
            'unknown' { $journal.files[6].name = 'reVC.exe' }
            'duplicate' { $journal.files[6].name = $journal.files[0].name }
            'traversal' { $journal.files[6].name = '..\reVC.exe' }
            'nonboolean' { $journal.files[6].existed = 'false' }
        }
        Write-TestManifest $backup $journal
        $before = Get-TestSnapshot $game
        Assert-TestThrows { Restore-DlssBackup $game $backup } 'Invalid backup manifest'
        Assert-TestSnapshot $before $game
    }
}

Invoke-Test 'restore rejects an external backup folder' {
    $game = New-TestGame
    Assert-TestThrows { Restore-DlssBackup $game (New-TestDirectory 'external-backup') } 'inside this game folder'
}

Invoke-Test 'ZIP extracts and verifies only the pinned runtime file' {
    $folder = New-TestDirectory 'zip-valid'
    $zip = Join-Path $folder 'package.zip'
    $spec = $script:Specs[0]
    $bytes = [IO.File]::ReadAllBytes((Join-Path $script:Stage $spec.name))
    New-TestZip $zip @($spec.archivePath, 'unrelated.txt') $bytes
    $stage = Join-Path $folder 'extracted'
    Expand-DlssPackage $zip ([pscustomobject]@{ format='zip'; files=@($spec) }) $stage
    Assert-Test (@(Get-ChildItem -LiteralPath $stage -File).Count -eq 1) 'Unpinned ZIP content was extracted.'
    Assert-Test ((Get-DlssHash (Join-Path $stage $spec.name)) -ceq $spec.sha256) 'ZIP payload did not match.'
}

foreach ($entryName in @('../escape.dll', '..\escape.dll', '/absolute.dll', 'C:/absolute.dll', './hidden.dll')) {
    Invoke-Test "ZIP rejects unsafe entry $entryName before extraction" {
        $folder = New-TestDirectory 'zip-unsafe'
        $zip = Join-Path $folder 'package.zip'
        $spec = $script:Specs[0]
        New-TestZip $zip @($spec.archivePath, $entryName) ([IO.File]::ReadAllBytes((Join-Path $script:Stage $spec.name)))
        $stage = Join-Path $folder 'extracted'
        Assert-TestThrows { Expand-DlssPackage $zip ([pscustomobject]@{ format='zip'; files=@($spec) }) $stage } 'Unsafe archive entry'
        Assert-Test (@(Get-ChildItem -LiteralPath $stage -File -Recurse).Count -eq 0) 'ZIP extracted data before validating all entry paths.'
    }
}

Invoke-Test 'ZIP rejects duplicate pinned payload entries' {
    $folder = New-TestDirectory 'zip-duplicate'
    $zip = Join-Path $folder 'package.zip'
    $spec = $script:Specs[0]
    New-TestZip $zip @($spec.archivePath, $spec.archivePath) ([IO.File]::ReadAllBytes((Join-Path $script:Stage $spec.name)))
    $stage = Join-Path $folder 'extracted'
    Assert-TestThrows { Expand-DlssPackage $zip ([pscustomobject]@{ format='zip'; files=@($spec) }) $stage } 'Archive contents differ'
    Assert-Test (@(Get-ChildItem -LiteralPath $stage -File -Recurse).Count -eq 0) 'Duplicate archive wrote a payload.'
}

Invoke-Test 'CLI requires explicit community-source consent before cache creation' {
    $cli = Join-Path $PSScriptRoot 'install-dlss5.ps1'
    $cache = Join-Path $script:FixtureRoot 'unconsented-community-cache'
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli -NoPrompt -VerifyOnly -Profile RTX40 -CacheDir $cache 2>&1
    Assert-Test ($LASTEXITCODE -eq 1) 'CLI did not reject missing community consent.'
    Assert-Test (($output -join "`n") -match 'Explicit -AcceptCommunityRuntime is required') 'CLI stopped for an unexpected reason.'
    Assert-Test (-not (Test-Path -LiteralPath $cache)) 'CLI created cache before obtaining source consent.'
}

Invoke-Test 'BASE CLI discloses mirror and requires consent before cache creation' {
    $cli = Join-Path $PSScriptRoot 'install-dlss5.ps1'
    $cache = Join-Path $script:FixtureRoot 'unconsented-base-cache'
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli -NoPrompt -VerifyOnly -Profile BASE -CacheDir $cache 2>&1
    Assert-Test ($LASTEXITCODE -eq 1) 'BASE CLI did not reject missing source consent.'
    $text = $output -join "`n"
    Assert-Test ($text -match 'Explicit -AcceptCommunityRuntime is required') 'BASE CLI stopped for an unexpected reason.'
    Assert-Test ($text -match 'RankFTW/rhi-repo, a third-party mirror') 'BASE did not disclose the mirror.'
    Assert-Test ($text -notmatch 'releases/download/dlssnr') 'BASE selected an NR model download.'
    Assert-Test (-not (Test-Path -LiteralPath $cache)) 'BASE created a cache without source consent.'
}

Invoke-Test 'BASE CLI needs no modified-model consent and reaches cache validation without downloading' {
    $cli = Join-Path $PSScriptRoot 'install-dlss5.ps1'
    $cache = Join-Path $PSScriptRoot 'disallowed-base-cache'
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli -NoPrompt -VerifyOnly -Profile BASE -AcceptCommunityRuntime -CacheDir $cache 2>&1
    Assert-Test ($LASTEXITCODE -eq 1) 'BASE accepted a cache inside the source kit.'
    $text = $output -join "`n"
    Assert-Test ($text -match 'Download caches must stay outside') 'BASE failed before reaching cache validation.'
    Assert-Test ($text -notmatch 'AcceptModifiedModel|RTX40 WARNING|releases/download/dlssnr') 'BASE requested or selected a modified NR model.'
    Assert-Test (-not (Test-Path -LiteralPath $cache)) 'BASE created a disallowed cache.'
}

Invoke-Test 'CLI requires additional modified-model consent before cache creation' {
    $cli = Join-Path $PSScriptRoot 'install-dlss5.ps1'
    $cache = Join-Path $script:FixtureRoot 'unconsented-model-cache'
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli -NoPrompt -VerifyOnly -Profile RTX40 -AcceptCommunityRuntime -CacheDir $cache 2>&1
    Assert-Test ($LASTEXITCODE -eq 1) 'CLI did not reject missing modified-model consent.'
    Assert-Test (($output -join "`n") -match 'Explicit -AcceptModifiedModel is required') 'CLI stopped for an unexpected reason.'
    Assert-Test (-not (Test-Path -LiteralPath $cache)) 'CLI created cache before obtaining modified-model consent.'
}

Invoke-Test 'CLI refuses restore combined with verification-only mode' {
    $cli = Join-Path $PSScriptRoot 'install-dlss5.ps1'
    $game = New-TestGame
    $before = Get-TestSnapshot $game
    $output = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli -NoPrompt -VerifyOnly -GameDir $game -RestoreBackup (Join-Path $game 'missing-backup') 2>&1
    Assert-Test ($LASTEXITCODE -eq 1) 'CLI accepted conflicting verify/restore switches.'
    Assert-Test (($output -join "`n") -match 'cannot be combined') 'Conflicting switches stopped for an unexpected reason.'
    Assert-TestSnapshot $before $game
}

Set-Item -LiteralPath Function:\Assert-DlssGameClosed -Value $script:ActualGuard
Write-Host "RESULT passed=$script:Passed failed=$script:Failed"
Write-Host "Synthetic fixtures retained at: $script:FixtureRoot"
if ($script:Failed -gt 0) { exit 1 }
