[CmdletBinding()]
param(
    [string]$KitRoot,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

if ([string]::IsNullOrWhiteSpace($KitRoot)) {
    $KitRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$root = (Resolve-Path -LiteralPath $KitRoot).Path.TrimEnd('\')
$manifestPath = Join-Path $root 'SOURCE_MANIFEST.sha256'
$prefixLength = $root.Length + 1

$lines = @(Get-ChildItem -LiteralPath $root -Recurse -Force -File |
    Where-Object {
        $relative = $_.FullName.Substring($prefixLength).Replace('\', '/')
        $relative -ne 'SOURCE_MANIFEST.sha256' -and
        $relative -ne '.git' -and -not $relative.StartsWith('.git/')
    } |
    Sort-Object { $_.FullName.Substring($prefixLength).Replace('\', '/') } |
    ForEach-Object {
        $relative = $_.FullName.Substring($prefixLength).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        '{0}  {1}' -f $hash.ToLowerInvariant(), $relative
    })
$expected = ($lines -join "`n") + "`n"

if ($Verify) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Missing source manifest: $manifestPath"
    }
    $actual = [System.IO.File]::ReadAllText($manifestPath).Replace("`r`n", "`n")
    if ($actual -ne $expected) {
        throw 'SOURCE_MANIFEST.sha256 is stale or does not match the kit tree.'
    }
    Write-Host ("Source manifest verified: {0} file(s)." -f $lines.Count)
    return
}

[System.IO.File]::WriteAllText($manifestPath, $expected,
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Source manifest updated: {0} file(s)." -f $lines.Count)
