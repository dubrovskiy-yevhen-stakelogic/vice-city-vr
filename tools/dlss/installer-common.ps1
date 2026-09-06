Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:DlssNames = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll',
    'sl.pcl.dll', 'sl.dlss_nr.dll', 'nvngx_dlss.dll', 'nvngx_dlssnr.dll')

function Get-DlssHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-DlssPlainPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Substring([IO.Path]::GetPathRoot($full).Length).Contains(':')) {
        throw "Alternate data stream paths are not allowed: $Path"
    }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if (((Get-Item -LiteralPath $cursor -Force).Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Choose a normal folder, not a link or junction: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Assert-DlssPe64([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($stream.Length -lt 128 -or $reader.ReadUInt16() -ne 0x5a4d) {
            throw "Not a Windows executable: $Path"
        }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 26) {
            throw "Invalid executable header: $Path"
        }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "A Windows x64 file is required: $Path"
        }
        $stream.Position = $offset + 24
        if ($reader.ReadUInt16() -ne 0x20b) { throw "Not a PE32+ executable: $Path" }
    } finally { $reader.Dispose() }
}

function Assert-DlssGameClosed {
    if (@(Get-Process -Name reVC -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Close Vice City VR before installing or restoring files. Setup will not stop the game for you.'
    }
}

function Get-DlssGameDirectory([string]$GameDir) {
    $root = Assert-DlssPlainPath $GameDir
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Game folder does not exist: $root"
    }
    if (Test-Path -LiteralPath (Join-Path $root 'patch-manifest.json')) {
        throw 'Select your installed game, not the patch-only source kit.'
    }
    foreach ($name in @('reVC.exe', 'nvngx.dll_dlssnr.dll')) {
        $path = Assert-DlssPlainPath (Join-Path $root $name)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing $name. Select the folder containing the complete Vice City VR player build. This installer does not build the game."
        }
        Assert-DlssPe64 $path
    }
    return $root
}

function Assert-DlssPayload([string]$Path, $Spec) {
    if ($script:DlssNames -notcontains [string]$Spec.name) { throw 'Unexpected runtime filename.' }
    [void](Assert-DlssPlainPath $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-Item -LiteralPath $Path).Length -ne [long]$Spec.size -or
        (Get-DlssHash $Path) -cne [string]$Spec.sha256) {
        throw "File verification failed: $($Spec.name). Nothing from this download will be installed."
    }
    Assert-DlssPe64 $Path
    if ($Spec.signature -eq 'NVIDIA') {
        $signature = Get-AuthenticodeSignature -LiteralPath $Path
        if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=NVIDIA Corporation(,|$)') {
            throw "NVIDIA signature validation failed: $($Spec.name). Check Windows date, certificates and internet access. Signature checks cannot be disabled."
        }
    } elseif ($Spec.signature -ne 'ModifiedPinned') {
        throw 'Unknown signature policy.'
    }
}

function Get-DlssDownload($Package, [string]$CacheDir) {
    $uri = [uri]$Package.url
    if ($uri.Scheme -ne 'https' -or $uri.Host -notin @('github.com', 'raw.githubusercontent.com') -or
        $Package.sha256 -cnotmatch '^[a-f0-9]{64}$' -or [long]$Package.size -le 0 -or
        [long]$Package.size -gt 250000000) { throw 'Invalid pinned download entry.' }
    $cache = Assert-DlssPlainPath $CacheDir
    [void][IO.Directory]::CreateDirectory($cache)
    $path = Assert-DlssPlainPath (Join-Path $cache ($Package.sha256 + '.download'))
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        if ((Get-Item -LiteralPath $path).Length -eq [long]$Package.size -and
            (Get-DlssHash $path) -ceq $Package.sha256) { return $path }
        throw "A cached download failed verification. Remove this one file and retry: $path"
    }
    $partial = Join-Path $cache ([guid]::NewGuid().ToString('N') + '.partial')
    Write-Host ("Downloading {0} ({1:N1} MB)..." -f $Package.label, ($Package.size / 1MB))
    Add-Type -AssemblyName System.Net.Http
    $handler = New-Object Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $client = New-Object Net.Http.HttpClient($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(10)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('ViceCityVR-Setup/0.5.5')
    $response = $null
    $output = $null
    $inputStream = $null
    try {
        for ($redirect = 0; $redirect -le 6; $redirect++) {
            if ($uri.Scheme -ne 'https' -or $uri.Host -notin @(
                    'github.com', 'raw.githubusercontent.com', 'release-assets.githubusercontent.com',
                    'objects.githubusercontent.com', 'media.githubusercontent.com')) {
                throw 'Download redirected outside the permitted HTTPS hosts.'
            }
            $response = $client.GetAsync($uri, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
            if ([int]$response.StatusCode -in @(301, 302, 303, 307, 308)) {
                if ($null -eq $response.Headers.Location -or $redirect -eq 6) { throw 'Invalid download redirect.' }
                $uri = New-Object uri($uri, $response.Headers.Location)
                $response.Dispose()
                $response = $null
                continue
            }
            [void]$response.EnsureSuccessStatusCode()
            break
        }
        if ($response.Content.Headers.ContentLength -and
            $response.Content.Headers.ContentLength -ne [long]$Package.size) {
            throw 'The download size differs from the pinned release.'
        }
        $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $output = [IO.File]::Open($partial, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $buffer = New-Object byte[] 1048576
        $total = 0L
        $deadline = [DateTime]::UtcNow.AddMinutes(10)
        while ($true) {
            $readTask = $inputStream.ReadAsync($buffer, 0, $buffer.Length)
            if (-not $readTask.Wait(30000) -or [DateTime]::UtcNow -gt $deadline) { throw 'Download timed out. Please retry.' }
            $count = $readTask.GetAwaiter().GetResult()
            if ($count -eq 0) { break }
            $total += $count
            if ($total -gt [long]$Package.size) { throw 'Download exceeded the pinned size.' }
            $output.Write($buffer, 0, $count)
            Write-Progress -Activity $Package.label -Status ("{0:N1} / {1:N1} MB" -f ($total/1MB), ($Package.size/1MB)) -PercentComplete ([int](100*$total/$Package.size))
        }
        $output.Dispose(); $output = $null
        if ($total -ne [long]$Package.size -or (Get-DlssHash $partial) -cne $Package.sha256) {
            throw 'Download checksum mismatch. It may be damaged or the remote asset may have changed.'
        }
        [IO.File]::Move($partial, $path)
        return $path
    } finally {
        if ($output) { $output.Dispose() }
        if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() }
        $client.Dispose()
        if (Test-Path -LiteralPath $partial -PathType Leaf) { Remove-Item -LiteralPath $partial -Force }
        Write-Progress -Activity $Package.label -Completed
    }
}

function Expand-DlssPackage([string]$ArchivePath, $Package, [string]$StageDir) {
    $stage = Assert-DlssPlainPath $StageDir
    [void][IO.Directory]::CreateDirectory($stage)
    if ($Package.format -eq 'dll') {
        if (@($Package.files).Count -ne 1) { throw 'Invalid raw DLL package.' }
        $spec = $Package.files[0]
        if ($script:DlssNames -notcontains [string]$spec.name) { throw 'Unexpected runtime filename.' }
        $target = Assert-DlssPlainPath (Join-Path $stage $spec.name)
        [IO.File]::Copy($ArchivePath, $target, $false)
        Assert-DlssPayload $target $spec
        return
    }
    if ($Package.format -ne 'zip') { throw 'Unsupported package format.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $relative = $entry.FullName.Replace('\', '/')
            if ($relative.StartsWith('/') -or $relative.Contains(':') -or
                @($relative.Split('/') | Where-Object { $_ -eq '..' -or $_ -eq '.' }).Count -gt 0) {
                throw 'Unsafe archive entry.'
            }
        }
        foreach ($spec in $Package.files) {
            if ($script:DlssNames -notcontains [string]$spec.name) { throw 'Unexpected runtime filename.' }
            $entries = @($archive.Entries | Where-Object { $_.FullName -ceq $spec.archivePath })
            if ($entries.Count -ne 1 -or $entries[0].Length -ne [long]$spec.size) {
                throw "Archive contents differ from the pinned package: $($spec.name)"
            }
            $target = Assert-DlssPlainPath (Join-Path $stage $spec.name)
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $target, $false)
            Assert-DlssPayload $target $spec
        }
    } finally { $archive.Dispose() }
}

function Set-DlssFileAtomic([string]$Source, [string]$Target, [string]$ExpectedSha256) {
    [void](Assert-DlssPlainPath $Target)
    $temporary = Join-Path ([IO.Path]::GetDirectoryName($Target)) ('.dlss5-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::Copy($Source, $temporary, $false)
        if ($ExpectedSha256 -cnotmatch '^[a-f0-9]{64}$' -or
            (Get-DlssHash $temporary) -cne $ExpectedSha256) {
            throw 'The replacement file changed or its temporary copy failed verification.'
        }
        if (Test-Path -LiteralPath $Target -PathType Leaf) {
            [IO.File]::Replace($temporary, $Target, [System.Management.Automation.Language.NullString]::Value)
        } else {
            [IO.File]::Move($temporary, $Target)
        }
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Restore-DlssBackup([string]$GameDir, [string]$BackupDir) {
    $game = Get-DlssGameDirectory $GameDir
    Assert-DlssGameClosed
    $backup = Assert-DlssPlainPath $BackupDir
    $prefix = (Join-Path $game 'dlss5-backups') + [IO.Path]::DirectorySeparatorChar
    if (-not $backup.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetDirectoryName($backup) -ine $prefix.TrimEnd('\')) {
        throw 'Select a backup created inside this game folder.'
    }
    $journalPath = Assert-DlssPlainPath (Join-Path $backup 'manifest.json')
    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
    if ($journal.schema -ne 1 -or $journal.gameDir -ine $game) { throw 'This backup belongs to a different installation.' }
    $seen = @{}
    $pending = @()
    foreach ($entry in $journal.files) {
        if ($script:DlssNames -notcontains [string]$entry.name -or $seen.ContainsKey($entry.name) -or
            $entry.installedSha256 -cnotmatch '^[a-f0-9]{64}$' -or $entry.existed -isnot [bool]) {
            throw 'Invalid backup manifest.'
        }
        $seen[$entry.name] = $true
        $target = Assert-DlssPlainPath (Join-Path $game $entry.name)
        $exists = Test-Path -LiteralPath $target -PathType Leaf
        $currentHash = if ($exists) { Get-DlssHash $target } else { '' }
        if ($entry.existed) {
            $source = Assert-DlssPlainPath (Join-Path $backup $entry.name)
            if ($entry.previousSha256 -cnotmatch '^[a-f0-9]{64}$' -or
                (Get-DlssHash $source) -cne $entry.previousSha256) { throw 'Backup data is missing or changed.' }
            if ($currentHash -ceq $entry.previousSha256) { continue }
        } elseif (-not $exists) { continue }
        if ($currentHash -cne $entry.installedSha256) {
            throw "Changed since installation: $($entry.name). Restore stopped to preserve your newer files."
        }
        $pending += $entry
    }
    foreach ($entry in $pending) {
        Assert-DlssGameClosed
        $target = Assert-DlssPlainPath (Join-Path $game $entry.name)
        if ((Get-DlssHash $target) -cne $entry.installedSha256) { throw 'A runtime file changed during restore.' }
        if ($entry.existed) {
            Set-DlssFileAtomic (Join-Path $backup $entry.name) $target $entry.previousSha256
            if ((Get-DlssHash $target) -cne $entry.previousSha256) { throw 'Restored file verification failed.' }
        } else { Remove-Item -LiteralPath $target -Force }
    }
    Write-Host 'Previous runtime restored. Backup copies have been kept.'
}

function Install-DlssFiles([string]$GameDir, [string]$StageDir, [object[]]$Files, [string]$Profile) {
    $game = Get-DlssGameDirectory $GameDir
    Assert-DlssGameClosed
    $names = @($Files | ForEach-Object { $_.name })
    if ($names.Count -ne $script:DlssNames.Count -or
        @(Compare-Object $script:DlssNames $names).Count -ne 0) { throw 'A complete, unique runtime set is required.' }
    $changes = @()
    foreach ($spec in $Files) {
        Assert-DlssPayload (Join-Path $StageDir $spec.name) $spec
        $target = Assert-DlssPlainPath (Join-Path $game $spec.name)
        if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath $target -PathType Leaf)) {
            throw "Expected a file, found a directory: $target"
        }
        $exists = Test-Path -LiteralPath $target -PathType Leaf
        $oldHash = if ($exists) { Get-DlssHash $target } else { '' }
        if ($oldHash -ceq $spec.sha256) { continue }
        $changes += [pscustomobject]@{ name=$spec.name; existed=[bool]$exists; previousSha256=$oldHash; installedSha256=$spec.sha256 }
    }
    if ($changes.Count -eq 0) { Write-Host 'This exact runtime set is already installed.'; return $null }
    $backupRoot = Assert-DlssPlainPath (Join-Path $game 'dlss5-backups')
    [void][IO.Directory]::CreateDirectory($backupRoot)
    $backup = Join-Path $backupRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    [void][IO.Directory]::CreateDirectory($backup)
    foreach ($entry in $changes) {
        if ($entry.existed) {
            $source = Join-Path $game $entry.name
            $saved = Join-Path $backup $entry.name
            [IO.File]::Copy($source, $saved, $false)
            if ((Get-DlssHash $saved) -cne $entry.previousSha256) { throw 'Runtime changed while backing up. No runtime files were replaced.' }
        }
    }
    $journal = [pscustomobject]@{ schema=1; gameDir=$game; profile=$Profile; files=@($changes) }
    [IO.File]::WriteAllText((Join-Path $backup 'manifest.json'), ($journal | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
    $changed = @()
    try {
        foreach ($entry in $changes) {
            Assert-DlssGameClosed
            $target = Assert-DlssPlainPath (Join-Path $game $entry.name)
            $currentHash = if (Test-Path -LiteralPath $target -PathType Leaf) { Get-DlssHash $target } else { '' }
            if ($currentHash -cne $entry.previousSha256) { throw 'A runtime file changed during installation.' }
            Set-DlssFileAtomic (Join-Path $StageDir $entry.name) $target $entry.installedSha256
            $changed += $entry
            if ((Get-DlssHash $target) -cne $entry.installedSha256) { throw 'Installed file verification failed.' }
        }
    } catch {
        $failure = $_.Exception.Message
        foreach ($entry in $changed) {
            $target = Assert-DlssPlainPath (Join-Path $game $entry.name)
            try {
                if ((Get-DlssHash $target) -cne $entry.installedSha256) { throw 'The file changed again; automatic restore refused.' }
                if ($entry.existed) { Set-DlssFileAtomic (Join-Path $backup $entry.name) $target $entry.previousSha256 }
                else { Remove-Item -LiteralPath $target -Force }
            } catch { Write-Warning "Could not restore $($entry.name): $($_.Exception.Message). Backup: $backup" }
        }
        throw "Installation failed: $failure. Original files are preserved in $backup"
    }
    Write-Host "Backup: $backup"
    return $backup
}
