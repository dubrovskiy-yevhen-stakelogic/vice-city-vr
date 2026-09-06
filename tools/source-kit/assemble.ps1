[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Revc,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$KitRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'common.ps1')

if ([string]::IsNullOrWhiteSpace($KitRoot)) {
    $KitRoot = Get-KitRoot -ScriptDirectory $PSScriptRoot
}
$kitFull = Get-CanonicalDirectoryPath -Path $KitRoot
$revcFull = Get-CanonicalDirectoryPath -Path $Revc
$outFull = Get-CanonicalDirectoryPath -Path $Out

if (-not (Test-Path -LiteralPath $kitFull -PathType Container)) {
    throw "Source-kit root does not exist: $kitFull"
}
if (-not (Test-Path -LiteralPath $revcFull -PathType Container)) {
    throw "Upstream checkout does not exist: $revcFull"
}
if (Test-Path -LiteralPath $outFull) {
    throw "Output already exists; refusing to overwrite it: $outFull"
}
if ((Test-PathInsideRoot -Root $revcFull -Candidate $outFull -AllowRoot) -or
    (Test-PathInsideRoot -Root $outFull -Candidate $revcFull -AllowRoot)) {
    throw 'The output and upstream checkout must not contain one another.'
}
if ((Test-PathInsideRoot -Root $kitFull -Candidate $outFull -AllowRoot) -or
    (Test-PathInsideRoot -Root $outFull -Candidate $kitFull -AllowRoot)) {
    throw 'The output and source kit must not contain one another.'
}

$auditScript = Join-Path $kitFull 'tools\source-kit\audit-source-kit.ps1'
if (-not (Test-Path -LiteralPath $auditScript -PathType Leaf)) {
    throw "Source-kit audit script is missing: $auditScript"
}
& $auditScript -KitRoot $kitFull

$manifest = Read-PatchManifest -KitRoot $kitFull
$patches = @($manifest.patches)
$patchTargets = @($patches | ForEach-Object {
        if ($null -eq $_.PSObject.Properties['targets']) {
            throw "Patch entry has no targets array: $($_.path)"
        }
        @($_.targets)
    })
$overlayFiles = @($manifest.overlay)
$librwFiles = @($manifest.librw)

Assert-UniqueManifestPaths -Entries $patches -CollectionName 'patches'
Assert-UniqueManifestPaths -Entries $patchTargets -CollectionName 'patch targets'
Assert-UniqueManifestPaths -Entries $overlayFiles -CollectionName 'overlay'
Assert-UniqueManifestPaths -Entries $librwFiles -CollectionName 'librw'

$patchRoot = Join-Path $kitFull 'patches'
$overlayRoot = Join-Path $kitFull 'overlay'
$librwRoot = Join-Path $kitFull 'librw'
Assert-ManifestFilesAndHashes -Root $patchRoot -Entries $patches `
    -CollectionName 'patches'
Assert-ManifestFilesAndHashes -Root $overlayRoot -Entries $overlayFiles `
    -CollectionName 'overlay'
Assert-ManifestFilesAndHashes -Root $librwRoot -Entries $librwFiles `
    -CollectionName 'librw'

$gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
if ($null -eq $gitCommand) {
    $gitCommand = Get-Command git -ErrorAction SilentlyContinue
}
if ($null -eq $gitCommand) {
    throw 'Git is required to validate the upstream checkout and apply patches.'
}
$git = $gitCommand.Source
$safeDirectory = 'safe.directory=' + $revcFull.Replace('\', '/')
$emptyExcludes = 'core.excludesFile=/dev/null'

if (-not (Test-Path -LiteralPath (Join-Path $revcFull '.git'))) {
    throw "Upstream path is not a Git checkout: $revcFull"
}
if (-not (Test-Path -LiteralPath (Join-Path $revcFull 'src\core\main.cpp') `
        -PathType Leaf)) {
    throw 'Upstream checkout does not contain src/core/main.cpp.'
}

$headOutput = Invoke-NativeCommand -FilePath $git -Arguments @(
    '-c', $safeDirectory, '-c', $emptyExcludes, '-C', $revcFull,
    'rev-parse', '--verify', 'HEAD'
) -Label 'Git HEAD validation'
$head = (($headOutput | ForEach-Object { [string]$_ }) -join '').Trim()
if (-not $head.Equals($script:ExpectedCommit,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Wrong upstream commit: expected $script:ExpectedCommit, got $head"
}
$treeOutput = Invoke-NativeCommand -FilePath $git -Arguments @(
    '-c', $safeDirectory, '-c', $emptyExcludes, '-C', $revcFull,
    'rev-parse', 'HEAD^{tree}'
) -Label 'Git tree validation'
$tree = (($treeOutput | ForEach-Object { [string]$_ }) -join '').Trim()
if (-not $tree.Equals($script:ExpectedTree,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Wrong upstream tree: expected $script:ExpectedTree, got $tree"
}

$statusOutput = Invoke-NativeCommand -FilePath $git -Arguments @(
    '-c', $safeDirectory, '-c', $emptyExcludes, '-C', $revcFull, 'status',
    '--porcelain=v1', '--untracked-files=all'
) -Label 'Git cleanliness validation'
$statusText = ($statusOutput | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
if (-not [string]::IsNullOrWhiteSpace($statusText)) {
    throw "Upstream checkout is not clean.$([Environment]::NewLine)$statusText"
}

$stagedOutput = Invoke-NativeCommand -FilePath $git -Arguments @(
    '-c', $safeDirectory, '-c', $emptyExcludes, '-C', $revcFull,
    'ls-files', '--stage'
) -Label 'Git index validation'
$symlinkEntries = @($stagedOutput | Where-Object {
        ([string]$_) -match '^120000 '
    })
if ($symlinkEntries.Count -ne 0) {
    throw 'The exact upstream checkout unexpectedly contains symlinks.'
}
$gitlinkPaths = @($stagedOutput | ForEach-Object {
        $line = [string]$_
        if ($line -match '^160000 [0-9a-fA-F]{40} [0-3]\t(.+)$') {
            $matches[1].Replace('\', '/')
        }
    } | Sort-Object)
$expectedGitlinks = @(
    'vendor/librw', 'vendor/ogg', 'vendor/opus', 'vendor/opusfile'
)
$gitlinkDifference = @(Compare-Object -ReferenceObject $expectedGitlinks `
    -DifferenceObject $gitlinkPaths)
if ($gitlinkDifference.Count -ne 0) {
    $details = ($gitlinkDifference | ForEach-Object {
            '{0} ({1})' -f $_.InputObject, $_.SideIndicator
        }) -join ', '
    throw "Unexpected upstream gitlink set: $details"
}

$parent = Split-Path -Parent $outFull
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}
$archivePath = Join-Path ([System.IO.Path]::GetTempPath()) `
    (([System.Guid]::NewGuid().ToString('N')) + '.zip')
try {
    Invoke-NativeCommand -FilePath $git -Arguments @(
        '-c', $safeDirectory, '-c', $emptyExcludes,
        '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', '-C', $revcFull,
        'archive', '--format=zip', "--output=$archivePath",
        $script:ExpectedCommit
    ) -Label 'Deterministic upstream export' | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $outFull)
} finally {
    if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
        Remove-Item -LiteralPath $archivePath -Force
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $outFull 'src\core\main.cpp') `
        -PathType Leaf)) {
    throw 'The deterministic upstream export is incomplete.'
}

$temporaryGit = Join-Path $outFull '.git'
try {
    Invoke-NativeCommand -FilePath $git -Arguments @(
        '-C', $outFull, 'init', '--quiet'
    ) -Label 'Temporary Git initialization' | Out-Null

    foreach ($target in $patchTargets) {
        foreach ($property in @('preimageSha256', 'postimageSha256')) {
            if ($null -eq $target.PSObject.Properties[$property] -or
                -not (Test-Sha256Text -Hash ([string]$target.$property))) {
                throw "patchTargets/$($target.path) has an invalid $property."
            }
        }
        $targetPath = Resolve-SafeChildPath -Root $outFull `
            -RelativePath ([string]$target.path)
        Assert-FileHash -Path $targetPath `
            -ExpectedHash ([string]$target.preimageSha256) `
            -Label "upstream preimage $($target.path)"
    }

    foreach ($patchEntry in $patches) {
        $patchPath = Resolve-SafeChildPath -Root $patchRoot `
            -RelativePath ([string]$patchEntry.path)
        Invoke-NativeCommand -FilePath $git -Arguments @(
            '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', '-C', $outFull,
            'apply', '--check', '--whitespace=nowarn', '--', $patchPath
        ) -Label "Patch preflight $($patchEntry.path)" | Out-Null
        Invoke-NativeCommand -FilePath $git -Arguments @(
            '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', '-C', $outFull,
            'apply', '--whitespace=nowarn', '--', $patchPath
        ) -Label "Patch application $($patchEntry.path)" | Out-Null
    }

    foreach ($target in $patchTargets) {
        $targetPath = Resolve-SafeChildPath -Root $outFull `
            -RelativePath ([string]$target.path)
        Assert-FileHash -Path $targetPath `
            -ExpectedHash ([string]$target.postimageSha256) `
            -Label "patched result $($target.path)"
    }
} finally {
    if (Test-Path -LiteralPath $temporaryGit -PathType Container) {
        Remove-Item -LiteralPath $temporaryGit -Recurse -Force
    }
}

Copy-ManifestFiles -SourceRoot $overlayRoot -DestinationRoot $outFull `
    -Entries $overlayFiles -CollectionName 'overlay' -RefuseCollision

$vendorRoot = Join-Path $outFull 'vendor'
if (-not (Test-Path -LiteralPath $vendorRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $vendorRoot | Out-Null
}
$assembledLibrw = Join-Path $vendorRoot 'librw'
if (Test-Path -LiteralPath $assembledLibrw) {
    $existingLibrwEntries = @(Get-ChildItem -LiteralPath $assembledLibrw -Force)
    if ($existingLibrwEntries.Count -ne 0) {
        throw "Upstream gitlink directory is not empty: $assembledLibrw"
    }
} else {
    New-Item -ItemType Directory -Path $assembledLibrw | Out-Null
}
Copy-ManifestFiles -SourceRoot $librwRoot -DestinationRoot $assembledLibrw `
    -Entries $librwFiles -CollectionName 'librw' -RefuseCollision

Write-Host "Assembled source: $outFull"
Write-Host "Upstream commit: $script:ExpectedCommit"
$summaryFormat = 'Applied {0} patch file(s), {1} patched target(s), ' +
    '{2} overlay file(s), and {3} librw file(s).'
Write-Host ($summaryFormat -f $patches.Count, $patchTargets.Count,
    $overlayFiles.Count, $librwFiles.Count)
