[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'common.ps1')

$source = New-Object System.Collections.Hashtable ([System.StringComparer]::Ordinal)
$source['Path'] = 'D:\tools\compiler;D:\tools\git'
$source['PATH'] = 'D:\tools\runtime;D:\TOOLS\GIT'
$source['TEMP'] = 'D:\temporary'
$source['temp'] = 'D:\temporary'
$destination = New-Object System.Collections.Specialized.StringDictionary
$destination['OLD_VALUE'] = 'must be removed'
Set-NormalizedBuildEnvironment -Destination $destination -Environment $source

$paths = @($destination['Path'].Split(';'))
if ($paths.Count -ne 3 -or $paths -inotcontains 'D:\tools\compiler' -or
    $paths -inotcontains 'D:\tools\git' -or $paths -inotcontains 'D:\tools\runtime') {
    throw 'PATH entries were dropped or duplicated.'
}
if (@($destination.Keys | Where-Object { $_ -ieq 'Path' }).Count -ne 1 -or
    @($destination.Keys | Where-Object { $_ -ieq 'TEMP' }).Count -ne 1 -or
    $destination.ContainsKey('OLD_VALUE')) {
    throw 'Environment variable normalization failed.'
}
Write-Host 'BUILD ENVIRONMENT TEST: PASS'
