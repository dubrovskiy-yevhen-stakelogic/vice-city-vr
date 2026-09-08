[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('vcvr-save-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$checks = 0
function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Parse-Script([string]$Path) {
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    return $ast
}
function Set-U32([byte[]]$Data, [int]$Offset, [uint32]$Value) {
    [Buffer]::BlockCopy([BitConverter]::GetBytes($Value), 0, $Data, $Offset, 4)
}
function Set-Checksum([byte[]]$Data) {
    [uint64]$sum = 0
    for ($i = 0; $i -lt $Data.Length - 4; $i++) { $sum += $Data[$i] }
    Set-U32 $Data ($Data.Length - 4) ([uint32]($sum -band 4294967295))
}

try {
    $engine = Parse-Script (Join-Path $PSScriptRoot 'transfer-vr-save.ps1')
    $wizard = Parse-Script (Join-Path $PSScriptRoot '..\transfer-pc-saves-to-quest.ps1')
    foreach ($ast in @($engine, $wizard)) {
        foreach ($function in $ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst]
        }, $false)) {
            . ([scriptblock]::Create($function.Extent.Text))
        }
    }

    [byte[]]$fixture = New-Object byte[] 201828
    $offset = 0
    $garageStart = 0
    $sizes = @{ 7 = 1160; 8 = 21588; 9 = 4408; 18 = 416; 5 = 7876 }
    for ($block = 0; $block -lt 20; $block++) {
        $inner = if ($sizes.ContainsKey($block)) { $sizes[$block] } else { 4 }
        $outer = $inner + 4
        if ($block -eq 19) { $outer = $fixture.Length - 4 - $offset - 4 }
        Set-U32 $fixture $offset $outer
        Set-U32 $fixture ($offset + 4) $inner
        if ($block -eq 5) { $garageStart = $offset + 8 }
        $offset += 4 + $outer
    }
    for ($i = 0; $i -lt 44; $i++) { $fixture[$garageStart + $i] = $i + 1 }
    for ($car = 0; $car -lt 48; $car++) {
        $start = $garageStart + 44 + $car * 40
        for ($i = 0; $i -lt 29; $i++) { $fixture[$start + $i] = $car + $i + 1 }
        for ($i = 32; $i -lt 38; $i++) { $fixture[$start + $i] = $car + $i + 1 }
    }
    for ($garage = 0; $garage -lt 32; $garage++) {
        $start = $garageStart + 44 + 48 * 40 + $garage * 184
        for ($i = 0; $i -lt 173; $i++) { $fixture[$start + $i] = ($garage + $i + 1) % 256 }
        for ($i = 176; $i -lt 182; $i++) { $fixture[$start + $i] = ($garage + $i + 1) % 256 }
    }
    Set-Checksum $fixture
    $pc = Join-Path $testRoot 'original.b'
    $quest = Join-Path $testRoot 'quest.b'
    $roundTrip = Join-Path $testRoot 'roundtrip.b'
    [IO.File]::WriteAllBytes($pc, $fixture)
    Convert-Save pc-to-quest $pc $quest
    Convert-Save quest-to-pc $quest $roundTrip
    Assert-Test ((Get-FileHash $pc).Hash -eq (Get-FileHash $roundTrip).Hash) 'Canonical PC/Quest round trip changed save data.'

    $Mode = 'Export'
    $Slot = 1
    $script:pcDirectory = Join-Path $testRoot 'destination'
    $script:temporaryRoot = Join-Path $testRoot 'conversion'
    New-Item -ItemType Directory -Path $script:pcDirectory, $script:temporaryRoot | Out-Null
    $pcSaveName = 'GTAVCsf1.b'
    $pcSavePath = Join-Path $script:pcDirectory $pcSaveName
    $slotUri = 'content://com.miamivr.quest.saves/slot/1'
    function Read-QuestSlotToFile([string]$Uri, [string]$Destination) {
        Copy-Item -LiteralPath $quest -Destination $Destination
    }
    $exportBranch = $engine.Find({ param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '$Mode -eq "Import"'
    }, $true).ElseClause
    Assert-Test ($null -ne $exportBranch) 'Export branch was not found.'
    $exportBody = [scriptblock]::Create($exportBranch.Extent.Text.Trim().Substring(1).TrimEnd().TrimEnd('}'))
    & $exportBody
    Assert-Test ((Get-FileHash $pcSavePath).Hash -eq (Get-FileHash $pc).Hash) 'Export to empty PC slot failed.'
    [IO.File]::WriteAllText($pcSavePath, 'previous PC save')
    $before = (Get-FileHash $pcSavePath).Hash
    & $exportBody
    $backups = @(Get-ChildItem (Join-Path $script:pcDirectory 'MiamiVR-save-backups') -Recurse -File)
    Assert-Test ($backups.Count -eq 1 -and (Get-FileHash $backups[0].FullName).Hash -eq $before) 'Existing PC save backup failed.'
    Assert-Test ((New-BackupDirectory) -ne (New-BackupDirectory)) 'Backup directory collision.'
    $goodExportHash = (Get-FileHash $pcSavePath).Hash
    $damaged = [IO.File]::ReadAllBytes($quest)
    $damaged[100] = $damaged[100] -bxor 1
    [IO.File]::WriteAllBytes($quest, $damaged)
    $rejected = $false
    try { & $exportBody } catch { $rejected = $_.Exception.Message -match 'checksum' }
    Assert-Test $rejected 'Corrupt Quest save was accepted.'
    Assert-Test ((Get-FileHash $pcSavePath).Hash -eq $goodExportHash) 'Corrupt input changed the PC save.'
    [IO.File]::WriteAllBytes($quest, [byte[]]@(1, 2, 3))
    $rejected = $false
    try { & $exportBody } catch { $rejected = $_.Exception.Message -match 'Unsupported save size' }
    Assert-Test $rejected 'Wrong-sized Quest save was accepted.'
    Assert-Test ((Get-FileHash $pcSavePath).Hash -eq $goodExportHash) 'Wrong-sized input changed the PC save.'

    $game = Join-Path $testRoot 'game with spaces'
    New-Item -ItemType Directory -Path $game | Out-Null
    New-Item -ItemType File -Path (Join-Path $game 'reVC.exe') | Out-Null
    Assert-Test ((Resolve-PcExportDirectory $game) -eq (Join-Path $game 'userfiles')) 'Fresh game destination resolution failed.'
    Assert-Test (-not (Test-Path (Join-Path $game 'userfiles'))) 'Folder resolution wrote before confirmation.'
    Assert-Test ((Resolve-PcExportDirectory $script:pcDirectory) -eq $script:pcDirectory) 'Explicit save folder resolution failed.'

    function Mock-Adb {
        $global:LASTEXITCODE = 0
        $slot = [int](([string]$args[4]).Split('/')[-1])
        if ($script:mockError) { return 'Error while accessing provider: permission denied' }
        $size = if ($slot -in @(1, 8)) { 201828 } else { 0 }
        "Row: 0 _size=$size"
    }
    $script:mockError = $false
    $occupied = @(Get-OccupiedQuestSlots 'Mock-Adb' @())
    Assert-Test (($occupied -join ',') -eq '1,8') 'Occupied slot discovery failed.'
    $script:mockError = $true
    $rejected = $false
    try { Get-OccupiedQuestSlots 'Mock-Adb' @() | Out-Null } catch { $rejected = $true }
    Assert-Test $rejected 'Provider error was silently treated as an empty slot.'
    Write-Host "PASS: $checks offline save-transfer checks. No headset or player saves were accessed."
} finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedTestRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolvedTestRoot) -notlike 'vcvr-save-test-*') {
        throw 'Refusing to clean an unexpected test directory.'
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
