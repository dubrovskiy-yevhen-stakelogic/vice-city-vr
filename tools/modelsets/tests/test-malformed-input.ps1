[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $ToolPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedTool = (Resolve-Path -LiteralPath $ToolPath).Path
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryBase ('txdcompress-tests-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$passed = [Collections.Generic.List[string]]::new()

function New-Chunk {
    param(
        [Parameter(Mandatory = $true)][uint32] $Type,
        [Parameter(Mandatory = $true)][byte[]] $Payload,
        [uint32] $Version = 402915327
    )

    $result = [byte[]]::new(12 + $Payload.Length)
    [BitConverter]::GetBytes($Type).CopyTo($result, 0)
    [BitConverter]::GetBytes([uint32]$Payload.Length).CopyTo($result, 4)
    [BitConverter]::GetBytes($Version).CopyTo($result, 8)
    $Payload.CopyTo($result, 12)
    return ,$result
}

function New-TextureNative {
    param([Parameter(Mandatory = $true)][byte[]] $TextureStruct)

    [byte[]] $structChunk = New-Chunk -Type 1 -Payload $TextureStruct
    [byte[]] $nativeChunk = New-Chunk -Type 0x15 -Payload $structChunk
    return ,(New-Chunk -Type 0x16 -Payload $nativeChunk)
}

function New-ValidTextureStruct {
    $pixelsLength = 8 * 8 * 4
    $result = [byte[]]::new(88 + 4 + $pixelsLength)
    [BitConverter]::GetBytes([uint32]8).CopyTo($result, 0)
    [Text.Encoding]::ASCII.GetBytes('audit_texture').CopyTo($result, 8)
    [BitConverter]::GetBytes([uint32]0).CopyTo($result, 72)
    [BitConverter]::GetBytes([uint32]0).CopyTo($result, 76)
    [BitConverter]::GetBytes([uint16]8).CopyTo($result, 80)
    [BitConverter]::GetBytes([uint16]8).CopyTo($result, 82)
    $result[84] = 32
    $result[85] = 1
    $result[86] = 4
    $result[87] = 0
    [BitConverter]::GetBytes([uint32]$pixelsLength).CopyTo($result, 88)
    for($pixel = 0; $pixel -lt 64; $pixel++) {
        $offset = 92 + $pixel * 4
        $result[$offset + 0] = [byte](16 + ($pixel % 32))
        $result[$offset + 1] = [byte](32 + ($pixel % 32))
        $result[$offset + 2] = [byte](64 + ($pixel % 32))
        $result[$offset + 3] = 255
    }
    return ,$result
}

function New-TruncatedLevelTextureStruct {
    $result = [byte[]]::new(88 + 4 + 16)
    [BitConverter]::GetBytes([uint32]8).CopyTo($result, 0)
    [BitConverter]::GetBytes([uint16]8).CopyTo($result, 80)
    [BitConverter]::GetBytes([uint16]8).CopyTo($result, 82)
    $result[84] = 32
    $result[85] = 1
    $result[86] = 4
    $result[87] = 0
    [BitConverter]::GetBytes([uint32]256).CopyTo($result, 88)
    return ,$result
}

function New-TruncatedCompressedTextureStruct {
    $result = [byte[]]::new(88 + 4 + 8)
    [BitConverter]::GetBytes([uint32]8).CopyTo($result, 0)
    [BitConverter]::GetBytes([uint16]16).CopyTo($result, 80)
    [BitConverter]::GetBytes([uint16]16).CopyTo($result, 82)
    $result[84] = 16
    $result[85] = 2
    $result[86] = 4
    $result[87] = 1
    [BitConverter]::GetBytes([uint32]128).CopyTo($result, 88)
    return ,$result
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)][string] $InputPath,
        [string[]] $PrefixArguments = @()
    )

    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = (& $resolvedTool @PrefixArguments $InputPath 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

function Assert-NoTemporaryFiles {
    param([Parameter(Mandatory = $true)][string] $InputPath)

    $directory = Split-Path -Parent $InputPath
    $leaf = Split-Path -Leaf $InputPath
    $leftovers = @(Get-ChildItem -LiteralPath $directory -Filter ($leaf + '.txdcompress.tmp.*') -Force)
    if($leftovers.Count -ne 0) {
        throw "temporary file was not cleaned up for $leaf"
    }
}

function Test-RejectedAndPreserved {
    param(
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][byte[]] $Bytes,
        [string[]] $PrefixArguments = @()
    )

    $path = Join-Path $testRoot ($Name + '.txd')
    [IO.File]::WriteAllBytes($path, $Bytes)
    $before = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $result = Invoke-Tool -InputPath $path -PrefixArguments $PrefixArguments
    $after = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if($result.ExitCode -eq 0) {
        throw "$Name was accepted unexpectedly. Output: $($result.Output)"
    }
    if($after -ne $before) {
        throw "$Name changed after a rejected parse"
    }
    Assert-NoTemporaryFiles -InputPath $path
    $passed.Add($Name)
}

try {
    Test-RejectedAndPreserved -Name 'truncated-root' -Bytes ([byte[]](0x16, 0, 0))

    $oversizedRoot = [byte[]]::new(12)
    [BitConverter]::GetBytes([uint32]0x16).CopyTo($oversizedRoot, 0)
    [BitConverter]::GetBytes([uint32]100).CopyTo($oversizedRoot, 4)
    Test-RejectedAndPreserved -Name 'oversized-root' -Bytes $oversizedRoot

    [byte[]] $partialChildRoot = New-Chunk -Type 0x16 -Payload ([byte[]](1, 2, 3, 4))
    Test-RejectedAndPreserved -Name 'truncated-child-header' -Bytes $partialChildRoot

    $oversizedChild = [byte[]]::new(12)
    [BitConverter]::GetBytes([uint32]0x15).CopyTo($oversizedChild, 0)
    [BitConverter]::GetBytes([uint32]::MaxValue).CopyTo($oversizedChild, 4)
    [byte[]] $oversizedChildRoot = New-Chunk -Type 0x16 -Payload $oversizedChild
    Test-RejectedAndPreserved -Name 'oversized-child' -Bytes $oversizedChildRoot

    [byte[]] $shortStruct = [BitConverter]::GetBytes([uint32]8)
    Test-RejectedAndPreserved -Name 'short-d3d8-struct' -Bytes (New-TextureNative -TextureStruct $shortStruct)

    Test-RejectedAndPreserved -Name 'truncated-level-zero' -Bytes (
        New-TextureNative -TextureStruct (New-TruncatedLevelTextureStruct))

    [byte[]] $validNativeRoot = New-TextureNative -TextureStruct (New-ValidTextureStruct)
    [byte[]] $validNativeBody = [byte[]]::new($validNativeRoot.Length - 12 + 4)
    [Array]::Copy($validNativeRoot, 12, $validNativeBody, 0, $validNativeRoot.Length - 12)
    $validNativeBody[$validNativeBody.Length - 4] = 1
    $validNativeBody[$validNativeBody.Length - 3] = 2
    $validNativeBody[$validNativeBody.Length - 2] = 3
    $validNativeBody[$validNativeBody.Length - 1] = 4
    [byte[]] $validThenMalformed = New-Chunk -Type 0x16 -Payload $validNativeBody
    Test-RejectedAndPreserved -Name 'valid-then-truncated-child' -Bytes $validThenMalformed

    Test-RejectedAndPreserved -Name 'truncated-compressed-level' -Bytes (
        New-TextureNative -TextureStruct (New-TruncatedCompressedTextureStruct)) -PrefixArguments @('--cap', '8')

    $positivePath = Join-Path $testRoot 'valid-conversion.txd'
    [IO.File]::WriteAllBytes($positivePath, $validNativeRoot)
    $positiveBefore = (Get-FileHash -LiteralPath $positivePath -Algorithm SHA256).Hash
    $positiveResult = Invoke-Tool -InputPath $positivePath
    $positiveAfter = (Get-FileHash -LiteralPath $positivePath -Algorithm SHA256).Hash
    if($positiveResult.ExitCode -ne 0) {
        throw "valid conversion failed. Output: $($positiveResult.Output)"
    }
    if($positiveAfter -eq $positiveBefore) {
        throw 'valid conversion did not change the fixture'
    }
    [byte[]] $convertedBytes = [IO.File]::ReadAllBytes($positivePath)
    if($convertedBytes.Length -lt 12 -or [BitConverter]::ToUInt32($convertedBytes, 4) -ne $convertedBytes.Length - 12) {
        throw 'converted fixture has an inconsistent root size'
    }
    Assert-NoTemporaryFiles -InputPath $positivePath
    $passed.Add('valid-conversion')

    $idempotentBefore = (Get-FileHash -LiteralPath $positivePath -Algorithm SHA256).Hash
    $idempotentResult = Invoke-Tool -InputPath $positivePath
    $idempotentAfter = (Get-FileHash -LiteralPath $positivePath -Algorithm SHA256).Hash
    if($idempotentResult.ExitCode -ne 0 -or $idempotentAfter -ne $idempotentBefore) {
        throw "second pass was not an unchanged success. Output: $($idempotentResult.Output)"
    }
    $passed.Add('idempotent-second-pass')

    $lockedPath = Join-Path $testRoot 'replace-denied.txd'
    [IO.File]::WriteAllBytes($lockedPath, $validNativeRoot)
    $lockedBefore = (Get-FileHash -LiteralPath $lockedPath -Algorithm SHA256).Hash
    $guard = [IO.File]::Open($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $lockedResult = Invoke-Tool -InputPath $lockedPath
    } finally {
        $guard.Dispose()
    }
    $lockedAfter = (Get-FileHash -LiteralPath $lockedPath -Algorithm SHA256).Hash
    if($lockedResult.ExitCode -eq 0) {
        throw 'replacement succeeded despite a delete-sharing lock'
    }
    if($lockedAfter -ne $lockedBefore) {
        throw 'replacement failure changed the original file'
    }
    Assert-NoTemporaryFiles -InputPath $lockedPath
    $passed.Add('replace-failure-preserves-original')

    Write-Host ("PASS: {0} cases" -f $passed.Count)
    foreach($name in $passed) { Write-Host ("  {0}" -f $name) }
} finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if($resolvedTestRoot.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
       (Split-Path -Leaf $resolvedTestRoot).StartsWith('txdcompress-tests-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
