[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Revc,
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$NewPatchPaths = @(),
    [string[]]$NewOverlayPaths = @(),
    [string[]]$NewLibrwPaths = @(),
    [string]$KitRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'common.ps1')
if ([string]::IsNullOrWhiteSpace($KitRoot)) {
    $KitRoot = Get-KitRoot -ScriptDirectory $PSScriptRoot
}
$kitFull = Get-CanonicalDirectoryPath $KitRoot
$sourceFull = Get-CanonicalDirectoryPath $Source
$revcFull = Get-CanonicalDirectoryPath $Revc
$outFull = Get-CanonicalDirectoryPath $Out

function Assert-NoReparseAncestors {
    param([string]$Path)
    $ancestor = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($ancestor)) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse-point path is forbidden: $ancestor"
            }
        }
        $parent = Split-Path -Parent $ancestor
        if ($parent -eq $ancestor) { break }
        $ancestor = $parent
    }
}

Assert-NoReparseAncestors $outFull
foreach ($inputRoot in @($kitFull, $sourceFull, $revcFull)) {
    Assert-NoReparseAncestors $inputRoot
    if (-not (Test-Path -LiteralPath $inputRoot -PathType Container)) {
        throw "Input directory not found: $inputRoot"
    }
    if ((Test-PathInsideRoot -Root $inputRoot -Candidate $outFull -AllowRoot) -or
        (Test-PathInsideRoot -Root $outFull -Candidate $inputRoot -AllowRoot)) {
        throw 'The output and each input directory must not contain one another.'
    }
}
if (Test-Path -LiteralPath $outFull) {
    throw "Output already exists; refusing to overwrite it: $outFull"
}

function Assert-ReleaseTextFile {
    param([string]$Path, [string]$Relative)
    Assert-NoReparseAncestors $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing allowlisted source file; deletion requires a separate review: $Relative"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Reparse-point file is forbidden: $Relative"
    }
    $allowed = @('.bat', '.c', '.cmake', '.cmd', '.cpp', '.def', '.dsm', '.err',
        '.frag', '.gitattributes', '.gitignore', '.h', '.hpp', '.hlsl', '.in',
        '.inc', '.json', '.license', '.lua', '.md', '.patch', '.ps1', '.py',
        '.rc', '.sha256', '.sh', '.txt', '.vcxproj', '.vert', '.xml', '.yml', '.yaml')
    if ($allowed -notcontains $item.Extension.ToLowerInvariant() -and
        $item.Name -notmatch '^(COPYING|COPYING-CMAKE-SCRIPTS|LICENSE|Makefile|NOTICE|VERSION)$') {
        throw "Not a permitted source-file extension: $Relative"
    }
    if ($item.Length -gt 32MB) {
        throw "Source file unexpectedly large: $Relative"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if (($bytes.Length -ge 2 -and $bytes[0] -eq 0x4d -and $bytes[1] -eq 0x5a) -or
        ($bytes.Length -ge 4 -and $bytes[0] -eq 0x7f -and $bytes[1] -eq 0x45 -and
            $bytes[2] -eq 0x4c -and $bytes[3] -eq 0x46) -or
        ($bytes.Length -ge 4 -and $bytes[0] -eq 0x50 -and $bytes[1] -eq 0x4b -and
            $bytes[2] -eq 3 -and $bytes[3] -eq 4) -or $bytes -contains 0) {
        throw "Binary content is forbidden: $Relative"
    }
    [void](New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
}

function Get-ApprovedPaths {
    param([object[]]$Existing, [string[]]$Added, [string]$Label)
    $paths = @($Existing | ForEach-Object { [string]$_.path })
    foreach ($path in $Added) {
        $safe = ConvertTo-SafeRelativePath $path
        if ($paths -icontains $safe) { throw "Already allowlisted in $Label`: $safe" }
        $paths += $safe
    }
    foreach ($path in $paths) {
        [void](ConvertTo-SafeRelativePath $path)
        if ($path -match '(?i)^(gamefiles|vendor|utils/gxt|src/audio/eax)/' -or
            $path -ieq 'src/extras/GitSHA1.cpp' -or
            $path -match '(?i)(^|/)(bin|build|dist|models|\.git|\.vs|__pycache__)/') {
            throw "Forbidden $Label path: $path"
        }
    }
    return @($paths | Sort-Object)
}

$manifest = Read-PatchManifest $kitFull
$patches = @($manifest.patches)
$oldTargets = @($patches | ForEach-Object { @($_.targets) })
Assert-UniqueManifestPaths -Entries $oldTargets -CollectionName 'patch targets'
foreach ($collection in @('patches', 'overlay', 'librw')) {
    Assert-ManifestFilesAndHashes -Root (Join-Path $kitFull $collection) `
        -Entries @($manifest.$collection) -CollectionName $collection
}
$patchPaths = @(Get-ApprovedPaths $oldTargets $NewPatchPaths 'patch')
$overlayPaths = @(Get-ApprovedPaths @($manifest.overlay) $NewOverlayPaths 'overlay')
$librwPaths = @(Get-ApprovedPaths @($manifest.librw) $NewLibrwPaths 'librw')
foreach ($path in $overlayPaths) {
    if ($patchPaths -icontains $path) { throw "Patch/overlay collision: $path" }
}

$git = (Get-Command git -ErrorAction Stop).Source
$gitBase = @('-c', ('safe.directory=' + $revcFull.Replace('\', '/')),
    '-c', 'core.excludesFile=/dev/null', '-C', $revcFull)
$head = ((Invoke-NativeCommand $git ($gitBase + @('rev-parse', 'HEAD')) 'HEAD check') -join '').Trim()
$tree = ((Invoke-NativeCommand $git ($gitBase + @('rev-parse', 'HEAD^{tree}')) 'Tree check') -join '').Trim()
if ($head -cne $script:ExpectedCommit -or $tree -cne $script:ExpectedTree) {
    throw 'The baseline is not the pinned upstream commit and tree.'
}
$status = Invoke-NativeCommand $git ($gitBase + @('status', '--porcelain=v1', '--untracked-files=all')) 'Cleanliness check'
if (-not [string]::IsNullOrWhiteSpace(($status -join "`n"))) {
    throw 'The pinned upstream checkout must be clean.'
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('vice-city-kit-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temporaryRoot)
$baseline = Join-Path $temporaryRoot 'baseline'
$archive = Join-Path $temporaryRoot 'baseline.zip'
try {
    Invoke-NativeCommand $git ($gitBase + @('archive', '--format=zip', "--output=$archive", $script:ExpectedCommit)) 'Baseline export' | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $baseline)
    Invoke-NativeCommand $git @('-C', $baseline, 'init', '--quiet') 'Scratch Git initialization' | Out-Null

    $preimages = @{}
    foreach ($path in $patchPaths) {
        $basePath = Resolve-SafeChildPath $baseline $path
        Assert-ReleaseTextFile $basePath $path
        $preimages[$path] = Get-Sha256 $basePath
        $oldTarget = @($oldTargets | Where-Object { $_.path -ceq $path })
        if ($oldTarget.Count -ne 0 -and $preimages[$path] -cne $oldTarget[0].preimageSha256) {
            throw "Existing manifest preimage differs from pinned baseline: $path"
        }
        $sourcePath = Resolve-SafeChildPath $sourceFull $path
        Assert-ReleaseTextFile $sourcePath $path
    }
    Invoke-NativeCommand $git (@('-c', 'core.autocrlf=false', '-C', $baseline, 'add', '--') + $patchPaths) 'Pin scratch preimages' | Out-Null
    foreach ($path in $patchPaths) {
        $sourcePath = Resolve-SafeChildPath $sourceFull $path
        $targetPath = Resolve-SafeChildPath $baseline $path
        $content = [IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
        [IO.File]::WriteAllText($targetPath, $content, (New-Object Text.UTF8Encoding($false)))
    }

    # Copy only the reviewed kit layout. Development-tree discovery is never used.
    $rootFiles = @('.gitattributes', '.gitignore', 'ASSEMBLE_SOURCE.bat', 'AUDIT_SOURCE_KIT.bat',
        'BUILD_PC.bat', 'BUILDING.md', 'INSTALL_DLSS5.bat', 'LICENSE', 'NOTICE', 'NOTICE.md', 'patch-manifest.json',
        'README.md', 'REGENERATE_PATCH_KIT.bat', 'RELEASING.md', 'SOURCE_KIT.md',
        'SOURCE_MANIFEST.sha256', 'THIRD_PARTY_NOTICES.md', 'TRANSFER_PC_SAVES_TO_QUEST.bat', 'VERSION')
    $rootDirectories = @('docs', 'librw', 'overlay', 'patches', 'third_party', 'tools')
    foreach ($item in Get-ChildItem -LiteralPath $kitFull -Force) {
        if ($item.Name -eq '.git') { continue }
        Assert-NoReparseAncestors $item.FullName
        if (($item.PSIsContainer -and $rootDirectories -notcontains $item.Name) -or
            (-not $item.PSIsContainer -and $rootFiles -notcontains $item.Name)) {
            throw "Unexpected source-kit root item: $($item.Name)"
        }
    }
    $kitDirectories = @(Get-ChildItem -LiteralPath $kitFull -Recurse -Force -Directory | Where-Object {
        -not $_.FullName.Substring($kitFull.Length + 1).StartsWith('.git\') -and $_.Name -ne '.git'
    })
    foreach ($directory in $kitDirectories) { Assert-NoReparseAncestors $directory.FullName }
    $kitFiles = @(Get-ChildItem -LiteralPath $kitFull -Recurse -Force -File | Where-Object {
        -not $_.FullName.Substring($kitFull.Length + 1).StartsWith('.git\')
    })
    foreach ($file in $kitFiles) {
        $relative = $file.FullName.Substring($kitFull.Length + 1).Replace('\', '/')
        Assert-ReleaseTextFile $file.FullName $relative
    }
    foreach ($path in $overlayPaths) {
        if (Test-Path -LiteralPath (Resolve-SafeChildPath $baseline $path)) {
            throw "Overlay is present in pinned upstream; use a patch: $path"
        }
        Assert-ReleaseTextFile (Resolve-SafeChildPath $sourceFull $path) $path
    }
    $sourceLibrw = Join-Path $sourceFull 'vendor\librw'
    foreach ($path in $librwPaths) {
        Assert-ReleaseTextFile (Resolve-SafeChildPath $sourceLibrw $path) "librw/$path"
    }
    [void](New-Item -ItemType Directory -Path $outFull)
    foreach ($file in $kitFiles) {
        $relative = $file.FullName.Substring($kitFull.Length + 1).Replace('\', '/')
        # The old series is replaced with one cumulative patch below.
        if ($relative.StartsWith('patches/')) { continue }
        $destination = Resolve-SafeChildPath $outFull $relative
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
    [void](New-Item -ItemType Directory -Path (Join-Path $outFull 'patches'))
    $patchName = 'vice-city-vr.patch'
    $patchFile = Join-Path $outFull "patches\$patchName"
    Invoke-NativeCommand $git (@('-c', 'core.autocrlf=false', '-C', $baseline,
        'diff', '--no-ext-diff', '--no-textconv', '--src-prefix=a/', '--dst-prefix=b/',
        "--output=$patchFile", '--') + $patchPaths) 'Cumulative patch generation' | Out-Null
    $changedPaths = @(Invoke-NativeCommand $git (@('-c', 'core.autocrlf=false', '-C', $baseline,
        'diff', '--name-only', '--') + $patchPaths) 'Changed-target enumeration')
    if ($changedPaths.Count -eq 0) { throw 'No patch targets differ from the pinned baseline.' }
    $targets = @($changedPaths | Sort-Object | ForEach-Object {
        $path = [string]$_
        [pscustomobject][ordered]@{ path = $path; preimageSha256 = $preimages[$path];
            postimageSha256 = Get-Sha256 (Resolve-SafeChildPath $baseline $path) }
    })
    $manifest.patches = @([pscustomobject][ordered]@{
        path = $patchName; sha256 = Get-Sha256 $patchFile; targets = $targets
    })
    foreach ($collection in @('overlay', 'librw')) {
        $paths = if ($collection -eq 'overlay') { $overlayPaths } else { $librwPaths }
        $fromRoot = if ($collection -eq 'overlay') { $sourceFull } else { $sourceLibrw }
        $toRoot = Join-Path $outFull $collection
        $entries = @($paths | ForEach-Object {
            $path = [string]$_
            $sourceFile = Resolve-SafeChildPath $fromRoot $path
            $destination = Resolve-SafeChildPath $toRoot $path
            [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
            Copy-Item -LiteralPath $sourceFile -Destination $destination -Force
            [pscustomobject][ordered]@{ path = $path; sha256 = Get-Sha256 $destination }
        })
        $manifest.$collection = $entries
    }
    [IO.File]::WriteAllText((Join-Path $outFull 'patch-manifest.json'),
        (($manifest | ConvertTo-Json -Depth 12) + "`n"), (New-Object Text.UTF8Encoding($false)))
    & (Join-Path $outFull 'tools\source-kit\update-source-manifest.ps1') -KitRoot $outFull
    & (Join-Path $outFull 'tools\source-kit\audit-source-kit.ps1') -KitRoot $outFull
    Write-Host "Candidate kit: $outFull"
    Write-Host 'Inputs were not modified. Assemble and compare this candidate before replacing the public kit.'
} finally {
    $tempResolved = [IO.Path]::GetFullPath($temporaryRoot)
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ((Split-Path -Parent $tempResolved) -ieq $tempParent -and
        (Split-Path -Leaf $tempResolved) -match '^vice-city-kit-[0-9a-f]{32}$' -and
        (Test-Path -LiteralPath $tempResolved -PathType Container)) {
        Remove-Item -LiteralPath $tempResolved -Recurse -Force
    }
}
