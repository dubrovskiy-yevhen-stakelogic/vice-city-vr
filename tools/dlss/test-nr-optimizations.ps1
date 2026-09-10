[CmdletBinding()]
param(
    [string]$MSBuild,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$kitRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $kitRoot 'tools\source-kit\common.ps1')
& (Join-Path $kitRoot 'tools\source-kit\test-kit-version.ps1')
& (Join-Path $PSScriptRoot 'tests\test-nr-settings.ps1')

if ([string]::IsNullOrWhiteSpace($MSBuild)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $candidates = @(& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe')
        if ($LASTEXITCODE -eq 0 -and $candidates.Count -gt 0) { $MSBuild = $candidates[0] }
    }
    if ([string]::IsNullOrWhiteSpace($MSBuild)) {
        $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
        if ($null -ne $command) { $MSBuild = $command.Source }
    }
}
if ([string]::IsNullOrWhiteSpace($MSBuild) -or -not (Test-Path -LiteralPath $MSBuild -PathType Leaf)) {
    throw 'Install Visual Studio 2022 C++ build tools and a Windows SDK, or supply -MSBuild.'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('vice-city-nr-tests-' + [guid]::NewGuid().ToString('N'))
}
$outputPath = Get-FullPath -Path $OutputDirectory
if (Test-PathInsideRoot -Root $kitRoot -Candidate $outputPath -AllowRoot) {
    throw 'Test outputs must be outside the source kit so they cannot enter its manifest.'
}
if (Test-Path -LiteralPath $outputPath) { throw "Output directory already exists: $outputPath" }
New-Item -ItemType Directory -Path $outputPath | Out-Null

$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = (Resolve-Path -LiteralPath $MSBuild).Path
$start.Arguments = ('"{0}" /t:Build /p:Configuration=Release /p:Platform=x64 /p:TestOutput="{1}" /m /v:minimal /nologo' -f `
    (Join-Path $PSScriptRoot 'tests\nr-optimizations-test.vcxproj'), $outputPath)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
Set-NormalizedBuildEnvironment -Destination $start.EnvironmentVariables `
    -Environment ([Environment]::GetEnvironmentVariables('Process'))
$process = [Diagnostics.Process]::Start($start)
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "NR test build failed: $($process.ExitCode). Outputs: $outputPath" }
& (Join-Path $outputPath 'bin\nr-optimizations-test.exe')
if ($LASTEXITCODE -ne 0) { throw "NR tests failed. Outputs: $outputPath" }
Write-Host "Test outputs: $outputPath"
