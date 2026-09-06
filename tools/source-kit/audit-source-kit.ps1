[CmdletBinding()]
param([string]$KitRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'common.ps1')

if ([string]::IsNullOrWhiteSpace($KitRoot)) {
    $KitRoot = Get-KitRoot -ScriptDirectory $PSScriptRoot
}
$root = Get-CanonicalDirectoryPath -Path $KitRoot
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Source-kit root does not exist: $root"
}

$findings = New-Object 'System.Collections.Generic.List[string]'
function Add-Finding {
    param([Parameter(Mandatory = $true)][string]$Message)
    $script:findings.Add($Message)
}

function Invoke-AuditCheck {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )
    try {
        & $Action
    } catch {
        Add-Finding -Message ("{0}: {1}" -f $Label, $_.Exception.Message)
    }
}

$allowedRootFiles = @(
    '.gitattributes', '.gitignore', 'ASSEMBLE_SOURCE.bat',
    'AUDIT_SOURCE_KIT.bat', 'BUILD_PC.bat', 'BUILDING.md', 'INSTALL_DLSS5.bat', 'LICENSE',
    'NOTICE', 'NOTICE.md', 'patch-manifest.json', 'README.md',
    'REGENERATE_PATCH_KIT.bat', 'RELEASING.md', 'SOURCE_KIT.md',
    'SOURCE_MANIFEST.sha256', 'THIRD_PARTY_NOTICES.md',
    'TRANSFER_PC_SAVES_TO_QUEST.bat', 'VERSION'
)
$allowedRootDirectories = @(
    '.git', 'docs', 'librw', 'overlay', 'patches', 'third_party', 'tools'
)
$forbiddenRootDirectories = @(
    'bin', 'build', 'cmake', 'codewarrior', 'dist', 'gamefiles', 'models',
    'src', 'utils', 'vendor'
)

foreach ($item in Get-ChildItem -LiteralPath $root -Force) {
    if ($item.Name -eq '.git') {
        continue
    }
    if ($item.PSIsContainer) {
        if ($allowedRootDirectories -notcontains $item.Name) {
            Add-Finding -Message "Unexpected root directory: $($item.Name)"
        }
    } elseif ($allowedRootFiles -notcontains $item.Name) {
        Add-Finding -Message "Unexpected root file: $($item.Name)"
    }
}
foreach ($directory in $forbiddenRootDirectories) {
    if (Test-Path -LiteralPath (Join-Path $root $directory)) {
        Add-Finding -Message "Full-tree directory is forbidden at kit root: $directory"
    }
}

$rootPrefixLength = $root.Length + 1
$files = @(Get-ChildItem -LiteralPath $root -Recurse -Force -File |
    Where-Object {
        $relative = $_.FullName.Substring($rootPrefixLength).Replace('\', '/')
        -not ($relative -eq '.git' -or $relative.StartsWith('.git/'))
    })
$directories = @(Get-ChildItem -LiteralPath $root -Recurse -Force -Directory |
    Where-Object {
        $relative = $_.FullName.Substring($rootPrefixLength).Replace('\', '/')
        -not ($relative -eq '.git' -or $relative.StartsWith('.git/'))
    })

$forbiddenDirectoryNames = @('.vs', '__pycache__')
foreach ($directory in $directories) {
    if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Add-Finding -Message "Reparse-point directory is forbidden: $($directory.FullName)"
    }
    if ($forbiddenDirectoryNames -contains $directory.Name) {
        Add-Finding -Message "Generated directory is forbidden: $($directory.FullName)"
    }
}

$allowedExtensions = @(
    '.bat', '.c', '.cmake', '.cmd', '.cpp', '.def', '.dsm', '.err',
    '.frag', '.gitattributes', '.gitignore', '.h', '.hpp', '.hlsl', '.in',
    '.inc', '.json', '.license', '.lua', '.md', '.patch', '.ps1', '.py',
    '.rc', '.sha256', '.sh', '.txt', '.vcxproj', '.vert', '.xml', '.yml',
    '.yaml'
)
$allowedExtensionlessNames = @(
    'COPYING', 'COPYING-CMAKE-SCRIPTS', 'LICENSE', 'Makefile', 'NOTICE',
    'VERSION'
)
$forbiddenExtensions = @(
    '.7z', '.apk', '.asi', '.bin', '.cso', '.dff', '.dll', '.exe', '.fon',
    '.ico', '.img', '.jar', '.jpeg', '.jpg', '.jks', '.key', '.keystore',
    '.lib', '.mp3', '.obj', '.ogg', '.pak', '.pdb', '.pem', '.pfx', '.png',
    '.rar', '.scm', '.so', '.tga', '.txd', '.wav', '.zip'
)
$secretFilePattern = '(?i)(^|[._-])(credential|password|secret|token)s?([._-]|$)|' +
    '(^id_(rsa|dsa|ecdsa|ed25519)$)|(^\.env($|\.))'
$aiNames = @(
    ('Chat' + 'GPT'), ('Open' + 'AI'), ('Co' + 'dex'),
    ('Clau' + 'de'), ('Gem' + 'ini')
)
$aiPattern = '(?i)\b(?:' + (($aiNames | ForEach-Object {
            [regex]::Escape($_)
        }) -join '|') + ')\b|generated\s+by\s+(?:an?\s+)?a[iI]\b|' +
    'as\s+an\s+a[iI]\s+language\s+model'
$privatePathPattern = '(?i)(?:[A-Z]:\\' + 'Users\\[^\\\r\n]+|' +
    '/ho' + 'me/[^/\s]+/|/Us' + 'ers/[^/\s]+/)'
$privateKeyMarker = ('-----BEGIN ' + 'PRIVATE KEY-----')
$secretContentPatterns = @(
    [regex]::Escape($privateKeyMarker),
    '\bAKIA[0-9A-Z]{16}\b',
    '\bgh[pousr]_[A-Za-z0-9]{30,255}\b',
    ('(?im)^\s*(?:password|passwd|api[_-]?key|secret|token)\s*[:=]' +
        '\s*["'']?[A-Za-z0-9+/=_-]{16,}')
)
$binarySignatures = @(
    [byte[]]@(0x4D, 0x5A),
    [byte[]]@(0x7F, 0x45, 0x4C, 0x46),
    [byte[]]@(0x50, 0x4B, 0x03, 0x04),
    [byte[]]@(0x89, 0x50, 0x4E, 0x47),
    [byte[]]@(0xFF, 0xD8, 0xFF),
    [byte[]]@(0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C),
    [byte[]]@(0x52, 0x61, 0x72, 0x21),
    [byte[]]@(0x4F, 0x67, 0x67, 0x53)
)

foreach ($file in $files) {
    $relative = $file.FullName.Substring($rootPrefixLength).Replace('\', '/')
    if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Add-Finding -Message "Reparse-point file is forbidden: $relative"
        continue
    }

    $extension = $file.Extension.ToLowerInvariant()
    if ($forbiddenExtensions -contains $extension) {
        Add-Finding -Message "Binary or game-asset extension is forbidden: $relative"
    } elseif ([string]::IsNullOrEmpty($extension)) {
        if ($allowedExtensionlessNames -notcontains $file.Name) {
            Add-Finding -Message "Unapproved extensionless file: $relative"
        }
    } elseif ($allowedExtensions -notcontains $extension) {
        Add-Finding -Message "Unapproved source-kit extension '$extension': $relative"
    }

    if ($file.Name -match $secretFilePattern) {
        Add-Finding -Message "Secret-like filename is forbidden: $relative"
    }

    try {
        $streams = @(Get-Item -LiteralPath $file.FullName -Stream * -ErrorAction Stop |
            Where-Object { $_.Stream -ne ':$DATA' })
        if ($streams.Count -ne 0) {
            Add-Finding -Message "Alternate data stream is forbidden: $relative"
        }
    } catch {
        Add-Finding -Message "Could not inspect alternate streams for $relative"
    }

    $headerLength = [Math]::Min(8, [int]$file.Length)
    if ($headerLength -gt 0) {
        $stream = [System.IO.File]::OpenRead($file.FullName)
        try {
            $header = New-Object byte[] $headerLength
            [void]$stream.Read($header, 0, $headerLength)
        } finally {
            $stream.Dispose()
        }
        foreach ($signature in $binarySignatures) {
            if ($header.Length -lt $signature.Length) {
                continue
            }
            $matches = $true
            for ($index = 0; $index -lt $signature.Length; $index++) {
                if ($header[$index] -ne $signature[$index]) {
                    $matches = $false
                    break
                }
            }
            if ($matches) {
                Add-Finding -Message "Binary signature is forbidden: $relative"
                break
            }
        }
    }

    try {
        $content = [System.IO.File]::ReadAllText($file.FullName)
    } catch {
        Add-Finding -Message "File is not readable as source text: $relative"
        continue
    }
    if ($content -match $aiPattern) {
        Add-Finding -Message "AI-authorship marker found: $relative"
    }
    if ($content -match $privatePathPattern) {
        Add-Finding -Message "Private workstation path found: $relative"
    }
    if ($content -match '[\u202A-\u202E\u2066-\u2069]') {
        Add-Finding -Message "Bidirectional control character found: $relative"
    }
    foreach ($pattern in $secretContentPatterns) {
        if ($content -match $pattern) {
            Add-Finding -Message "High-confidence secret marker found: $relative"
            break
        }
    }
}

foreach ($scriptFile in $files | Where-Object { $_.Extension -ieq '.ps1' }) {
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $scriptFile.FullName, [ref]$tokens, [ref]$parseErrors)
    foreach ($parseError in @($parseErrors)) {
        Add-Finding -Message ("PowerShell parse error in {0}: {1}" -f
            $scriptFile.Name, $parseError.Message)
    }
}
foreach ($jsonFile in $files | Where-Object { $_.Extension -ieq '.json' }) {
    try {
        [void](Get-Content -LiteralPath $jsonFile.FullName -Raw | ConvertFrom-Json)
    } catch {
        Add-Finding -Message ("JSON parse error in {0}: {1}" -f
            $jsonFile.Name, $_.Exception.Message)
    }
}

Invoke-AuditCheck -Label 'Kit version' -Action {
    $manifest = Read-PatchManifest -KitRoot $root
    $versionPath = Join-Path $root 'VERSION'
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        throw "Missing VERSION file: $versionPath"
    }
    $version = ([System.IO.File]::ReadAllText($versionPath)).Trim()
    if ($version -cne [string]$manifest.kitVersion) {
        throw "VERSION '$version' differs from manifest kitVersion '$($manifest.kitVersion)'."
    }
}

Invoke-AuditCheck -Label 'Source manifest' -Action {
    $sourceManifestPath = Join-Path $root 'SOURCE_MANIFEST.sha256'
    if (-not (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) {
        throw "Missing source manifest: $sourceManifestPath"
    }
    $sourceEntries = New-Object 'System.Collections.Generic.List[object]'
    $seenSourcePaths = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($sourceManifestPath)) {
        if ($line -notmatch '^([0-9A-Fa-f]{64}) [ *](.+)$') {
            throw "Invalid SOURCE_MANIFEST line: $line"
        }
        $relative = ConvertTo-SafeRelativePath -Path $matches[2]
        if ($relative -ieq 'SOURCE_MANIFEST.sha256') {
            throw 'SOURCE_MANIFEST.sha256 must not hash itself.'
        }
        $key = $relative.ToLowerInvariant()
        if ($seenSourcePaths.ContainsKey($key)) {
            throw "Duplicate source-manifest path: $relative"
        }
        $seenSourcePaths[$key] = $true
        $sourceEntries.Add([pscustomobject]@{
                path = $relative
                sha256 = $matches[1].ToLowerInvariant()
            })
    }

    $expectedSourcePaths = @($files | ForEach-Object {
            $_.FullName.Substring($rootPrefixLength).Replace('\', '/')
        } | Where-Object { $_ -ne 'SOURCE_MANIFEST.sha256' } | Sort-Object)
    $manifestSourcePaths = @($sourceEntries | ForEach-Object {
            [string]$_.path
        })
    $sourceDifference = @(Compare-Object -ReferenceObject $expectedSourcePaths `
        -DifferenceObject $manifestSourcePaths)
    if ($sourceDifference.Count -ne 0) {
        $details = ($sourceDifference | ForEach-Object {
                '{0} ({1})' -f $_.InputObject, $_.SideIndicator
            }) -join ', '
        throw "SOURCE_MANIFEST file set differs from the kit: $details"
    }
    foreach ($entry in $sourceEntries) {
        $entryPath = Resolve-SafeChildPath -Root $root `
            -RelativePath ([string]$entry.path)
        Assert-FileHash -Path $entryPath -ExpectedHash ([string]$entry.sha256) `
            -Label "SOURCE_MANIFEST/$($entry.path)"
    }
}

Invoke-AuditCheck -Label 'Patch manifest' -Action {
    $manifest = Read-PatchManifest -KitRoot $root
    $patches = @($manifest.patches)
    $patchTargets = @($patches | ForEach-Object {
            if ($null -eq $_.PSObject.Properties['targets']) {
                throw "Patch entry has no targets array: $($_.path)"
            }
            @($_.targets)
        })
    $overlayFiles = @($manifest.overlay)
    $librwFiles = @($manifest.librw)
    if ($patches.Count -eq 0 -or $patchTargets.Count -eq 0) {
        throw 'At least one patch and one patch target are required.'
    }
    if ($overlayFiles.Count -eq 0 -or $librwFiles.Count -eq 0) {
        throw 'Overlay and librw manifests must not be empty.'
    }

    $patchRoot = Join-Path $root 'patches'
    $overlayRoot = Join-Path $root 'overlay'
    $librwRoot = Join-Path $root 'librw'
    Assert-ManifestFilesAndHashes -Root $patchRoot -Entries $patches `
        -CollectionName 'patches'
    Assert-ManifestFilesAndHashes -Root $overlayRoot -Entries $overlayFiles `
        -CollectionName 'overlay'
    Assert-ManifestFilesAndHashes -Root $librwRoot -Entries $librwFiles `
        -CollectionName 'librw'

    Assert-UniqueManifestPaths -Entries $patchTargets `
        -CollectionName 'patch targets'
    foreach ($target in $patchTargets) {
        $targetPath = ConvertTo-SafeRelativePath -Path ([string]$target.path)
        foreach ($property in @('preimageSha256', 'postimageSha256')) {
            if ($null -eq $target.PSObject.Properties[$property] -or
                -not (Test-Sha256Text -Hash ([string]$target.$property))) {
                throw "patchTargets/$targetPath has an invalid $property."
            }
        }
        if ($targetPath -match '(?i)^(gamefiles|vendor)/' -or
            $targetPath -match '(?i)^utils/gxt/' -or
            $targetPath -match '(?i)^src/audio/eax/' -or
            $targetPath -ieq 'src/extras/GitSHA1.cpp') {
            throw "Forbidden patch target: $targetPath"
        }
    }

    foreach ($entry in $overlayFiles) {
        $overlayPath = ConvertTo-SafeRelativePath -Path ([string]$entry.path)
        if ($overlayPath -match '(?i)^(gamefiles|vendor|utils/gxt|src/audio/eax)/' -or
            $overlayPath -ieq 'src/extras/GitSHA1.cpp') {
            throw "Forbidden overlay path: $overlayPath"
        }
    }

    $parsedTargets = New-Object 'System.Collections.Generic.List[string]'
    foreach ($patchEntry in $patches) {
        $patchPath = Resolve-SafeChildPath -Root $patchRoot `
            -RelativePath ([string]$patchEntry.path)
        $patchText = [System.IO.File]::ReadAllText($patchPath)
        if ($patchText -match '(?im)^(GIT binary patch|Binary files )' -or
            $patchText -match '(?im)^(new file mode|deleted file mode) ') {
            throw "Patch must contain text modifications only: $($patchEntry.path)"
        }
        foreach ($line in [System.IO.File]::ReadAllLines($patchPath)) {
            if (-not $line.StartsWith('diff --git ')) {
                continue
            }
            if ($line -notmatch '^diff --git a/([^\s]+) b/([^\s]+)$') {
                throw "Unsupported diff header in $($patchEntry.path): $line"
            }
            $oldPath = ConvertTo-SafeRelativePath -Path $matches[1]
            $newPath = ConvertTo-SafeRelativePath -Path $matches[2]
            if ($oldPath -ne $newPath) {
                throw "Rename patch is forbidden: $line"
            }
            $parsedTargets.Add($oldPath)
        }
    }
    $parsedTargetArray = @($parsedTargets | Sort-Object -Unique)
    if ($parsedTargetArray.Count -ne $parsedTargets.Count) {
        throw 'A patch target occurs more than once across patch files.'
    }
    $manifestTargetArray = @($patchTargets | ForEach-Object {
            [string]$_.path
        } | Sort-Object -Unique)
    $difference = @(Compare-Object -ReferenceObject $manifestTargetArray `
        -DifferenceObject $parsedTargetArray)
    if ($difference.Count -ne 0) {
        $details = ($difference | ForEach-Object {
                '{0} ({1})' -f $_.InputObject, $_.SideIndicator
            }) -join ', '
        throw "Patch target list differs from the manifest: $details"
    }
}

if ($findings.Count -ne 0) {
    Write-Host 'SOURCE-KIT AUDIT: FAILED'
    foreach ($finding in $findings) {
        Write-Host ("  - {0}" -f $finding)
    }
    throw "Source-kit audit failed with $($findings.Count) finding(s)."
}

Write-Host ("SOURCE-KIT AUDIT: PASS ({0} files)" -f $files.Count)
Write-Host "Upstream base: $script:ExpectedCommit"
