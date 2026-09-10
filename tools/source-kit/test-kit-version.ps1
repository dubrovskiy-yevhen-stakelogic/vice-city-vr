[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'common.ps1')

$valid = @('0.5.5', '0.5.5-alpha-pc', '0.5.5.1', '0.5.5.2-alpha-pc',
    '1.2.3.4+build.5', '10.20.30')
$invalid = @('', '0.5', 'v0.5.5', '0.5.5.', '0.5.5.1.2', '0.5.5-alpha pc',
    '0.5.5-', '0.5.5+', '0.5.-1', ' 0.5.5', '0.5.5 ', "0.5.5`n")
foreach ($version in $valid) {
    if (-not (Test-KitVersion -Version $version)) {
        throw "Valid kit version rejected: $version"
    }
}
foreach ($version in $invalid) {
    if (Test-KitVersion -Version $version) {
        throw "Invalid kit version accepted: $version"
    }
}
Write-Host ("KIT VERSION TEST: PASS ({0} cases)" -f ($valid.Count + $invalid.Count))
