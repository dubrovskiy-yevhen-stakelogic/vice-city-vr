Set-StrictMode -Version 2.0

$script:ExpectedRepository = 'https://github.com/mrxenginner/reVC.git'
$script:ExpectedBranch = 'miami'
$script:ExpectedCommit = '026cd10f3fdbd92c089830e5067c4457c53c1b51'
$script:ExpectedTree = '603e9d0ef59a86e508309b74cf0a9f9c5fa13676'

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Get-CanonicalDirectoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $full = Get-FullPath -Path $Path
    return $full.TrimEnd([char[]]@([System.IO.Path]::DirectorySeparatorChar,
                [System.IO.Path]::AltDirectorySeparatorChar))
}

function Test-PathInsideRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Candidate,
        [switch]$AllowRoot
    )

    $rootFull = Get-CanonicalDirectoryPath -Path $Root
    $candidateFull = Get-FullPath -Path $Candidate
    if ($AllowRoot -and $candidateFull.Equals($rootFull,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }

    $prefix = $rootFull + [System.IO.Path]::DirectorySeparatorChar
    return $candidateFull.StartsWith($prefix,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function ConvertTo-SafeRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'A manifest path is empty.'
    }
    if ($Path.Contains('\')) {
        throw "Manifest paths must use forward slashes: $Path"
    }
    if ($Path.StartsWith('/') -or $Path.Contains(':') -or
        [System.IO.Path]::IsPathRooted($Path)) {
        throw "Manifest path is absolute: $Path"
    }
    if ($Path -match '(^|/)\.\.(/|$)' -or $Path -match '(^|/)\.(/|$)' -or
        $Path.EndsWith('/')) {
        throw "Manifest path is not normalized: $Path"
    }
    if ($Path.IndexOfAny([char[]]@([char]0, [char]10, [char]13)) -ge 0) {
        throw 'Manifest path contains a control character.'
    }

    return $Path
}

function Resolve-SafeChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $safePath = ConvertTo-SafeRelativePath -Path $RelativePath
    $nativePath = $safePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    $candidate = Get-FullPath -Path (Join-Path $Root $nativePath)
    if (-not (Test-PathInsideRoot -Root $Root -Candidate $candidate)) {
        throw "Path escapes its root: $RelativePath"
    }
    return $candidate
}

function Get-KitRoot {
    param([Parameter(Mandatory = $true)][string]$ScriptDirectory)

    $toolsDirectory = Split-Path -Parent $ScriptDirectory
    return Get-CanonicalDirectoryPath -Path (Split-Path -Parent $toolsDirectory)
}

function Test-KitVersion {
    param([AllowEmptyString()][string]$Version)

    return $Version -cmatch '\A[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:[-+][0-9A-Za-z.-]+)?\z'
}

function Read-PatchManifest {
    param([Parameter(Mandatory = $true)][string]$KitRoot)

    $manifestPath = Join-Path $KitRoot 'patch-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Missing patch manifest: $manifestPath"
    }
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw |
            ConvertFrom-Json
    } catch {
        throw "Invalid patch manifest: $($_.Exception.Message)"
    }

    foreach ($name in @('schemaVersion', 'kitVersion', 'base', 'patches',
            'overlay', 'librw')) {
        if ($null -eq $manifest.PSObject.Properties[$name]) {
            throw "Patch manifest is missing '$name'."
        }
    }
    if ([int]$manifest.schemaVersion -ne 1) {
        throw "Unsupported patch manifest schema: $($manifest.schemaVersion)"
    }
    if (-not (Test-KitVersion -Version ([string]$manifest.kitVersion))) {
        throw "Invalid source-kit version: $($manifest.kitVersion)"
    }
    foreach ($name in @('repository', 'branch', 'commit', 'tree')) {
        if ($null -eq $manifest.base.PSObject.Properties[$name]) {
            throw "Patch manifest has an incomplete base object: $name"
        }
    }
    if ([string]$manifest.base.repository -ne $script:ExpectedRepository) {
        throw "Unexpected upstream repository: $($manifest.base.repository)"
    }
    if ([string]$manifest.base.branch -ne $script:ExpectedBranch) {
        throw "Unexpected upstream branch: $($manifest.base.branch)"
    }
    if ([string]$manifest.base.commit -ne $script:ExpectedCommit) {
        throw "Unexpected upstream commit: $($manifest.base.commit)"
    }
    if ([string]$manifest.base.tree -ne $script:ExpectedTree) {
        throw "Unexpected upstream tree: $($manifest.base.tree)"
    }

    return $manifest
}

function Test-Sha256Text {
    param([AllowNull()][string]$Hash)

    return $null -ne $Hash -and $Hash -match '^[0-9A-Fa-f]{64}$'
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-FileHash {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedHash,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Label`: $Path"
    }
    if (-not (Test-Sha256Text -Hash $ExpectedHash)) {
        throw "Invalid SHA-256 for $Label`: $ExpectedHash"
    }
    $actualHash = Get-Sha256 -Path $Path
    if (-not $actualHash.Equals($ExpectedHash,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "SHA-256 mismatch for $Label`: expected $ExpectedHash, got $actualHash"
    }
}

function Assert-UniqueManifestPaths {
    param(
        [Parameter(Mandatory = $true)][object[]]$Entries,
        [Parameter(Mandatory = $true)][string]$CollectionName
    )

    $seen = @{}
    foreach ($entry in $Entries) {
        if ($null -eq $entry -or $null -eq $entry.PSObject.Properties['path']) {
            throw "$CollectionName contains an entry without a path."
        }
        $path = ConvertTo-SafeRelativePath -Path ([string]$entry.path)
        $key = $path.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            throw "$CollectionName contains a duplicate path: $path"
        }
        $seen[$key] = $true
    }
}

function Get-RelativeFileList {
    param([Parameter(Mandatory = $true)][string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return @()
    }
    $rootFull = Get-CanonicalDirectoryPath -Path $Root
    $prefixLength = $rootFull.Length + 1
    return @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -File |
        ForEach-Object {
            $_.FullName.Substring($prefixLength).Replace('\', '/')
        } | Sort-Object)
}

function Assert-ExactManifestFileSet {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object[]]$Entries,
        [Parameter(Mandatory = $true)][string]$CollectionName
    )

    Assert-UniqueManifestPaths -Entries $Entries -CollectionName $CollectionName
    $expected = @($Entries | ForEach-Object {
            ConvertTo-SafeRelativePath -Path ([string]$_.path)
        } | Sort-Object)
    $actual = @(Get-RelativeFileList -Root $Root)
    $difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
    if ($difference.Count -ne 0) {
        $details = ($difference | ForEach-Object {
                '{0} ({1})' -f $_.InputObject, $_.SideIndicator
            }) -join ', '
        throw "$CollectionName does not match the files under $Root`: $details"
    }
}

function Assert-ManifestFilesAndHashes {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object[]]$Entries,
        [Parameter(Mandatory = $true)][string]$CollectionName
    )

    Assert-ExactManifestFileSet -Root $Root -Entries $Entries `
        -CollectionName $CollectionName
    foreach ($entry in $Entries) {
        if ($null -eq $entry.PSObject.Properties['sha256']) {
            throw "$CollectionName entry has no SHA-256: $($entry.path)"
        }
        $path = Resolve-SafeChildPath -Root $Root -RelativePath ([string]$entry.path)
        Assert-FileHash -Path $path -ExpectedHash ([string]$entry.sha256) `
            -Label "$CollectionName/$($entry.path)"
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    if ($exitCode -ne 0) {
        $message = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        throw "$Label failed with exit code $exitCode.$([Environment]::NewLine)$message"
    }
    return $output
}

function Set-NormalizedBuildEnvironment {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Specialized.StringDictionary]$Destination,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Environment
    )

    $Destination.Clear()
    $pathParts = New-Object 'System.Collections.Generic.List[string]'
    $seenPaths = New-Object 'System.Collections.Generic.HashSet[string]' `
        ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($key in @($Environment.Keys | Sort-Object -CaseSensitive)) {
        $name = [string]$key
        if ($name -ieq 'Path') {
            # Some hosts supply both Path and PATH. Merge their entries into
            # one variable rather than deleting the tool search path.
            foreach ($part in ([string]$Environment[$key]).Split(';')) {
                if (-not [string]::IsNullOrWhiteSpace($part) -and $seenPaths.Add($part)) {
                    $pathParts.Add($part)
                }
            }
        } elseif (-not $Destination.ContainsKey($name)) {
            $Destination[$name] = [string]$Environment[$key]
        }
    }
    if ($pathParts.Count -ne 0) {
        $Destination['Path'] = $pathParts -join ';'
    }
}

function Copy-ManifestFiles {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [Parameter(Mandatory = $true)][object[]]$Entries,
        [Parameter(Mandatory = $true)][string]$CollectionName,
        [switch]$RefuseCollision
    )

    foreach ($entry in $Entries) {
        $relative = ConvertTo-SafeRelativePath -Path ([string]$entry.path)
        $source = Resolve-SafeChildPath -Root $SourceRoot -RelativePath $relative
        $destination = Resolve-SafeChildPath -Root $DestinationRoot -RelativePath $relative
        if ($RefuseCollision -and (Test-Path -LiteralPath $destination)) {
            throw "$CollectionName would overwrite an existing path: $relative"
        }
        $destinationDirectory = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $source -Destination $destination
        Assert-FileHash -Path $destination -ExpectedHash ([string]$entry.sha256) `
            -Label "assembled $CollectionName/$relative"
    }
}
