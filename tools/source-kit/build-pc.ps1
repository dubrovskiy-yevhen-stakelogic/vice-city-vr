[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Revc,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$Premake,
    [Parameter(Mandatory = $true)][string]$OpenXrSdk,
    [Parameter(Mandatory = $true)][string]$StreamlineSdk,
    [Parameter(Mandatory = $true)][string]$Fsr2Sdk,
    [Parameter(Mandatory = $true)][string]$OpenAlSdk,
    [Parameter(Mandatory = $true)][string]$Mpg123Sdk,
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
    [switch]$GenerateOnly,
    [string]$KitRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'common.ps1')

if ([string]::IsNullOrWhiteSpace($KitRoot)) {
    $KitRoot = Get-KitRoot -ScriptDirectory $PSScriptRoot
}
$kitFull = Get-CanonicalDirectoryPath -Path $KitRoot
$workFull = Get-CanonicalDirectoryPath -Path $WorkDir
if (Test-Path -LiteralPath $workFull) {
    throw "Work directory already exists; refusing to overwrite it: $workFull"
}

function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-RequiredDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

$premakePath = Resolve-RequiredFile -Path $Premake -Label 'Premake executable'
$openXrPath = Resolve-RequiredDirectory -Path $OpenXrSdk -Label 'OpenXR SDK'
$streamlinePath = Resolve-RequiredDirectory -Path $StreamlineSdk `
    -Label 'Streamline SDK'
$fsr2Path = Resolve-RequiredDirectory -Path $Fsr2Sdk -Label 'FSR2 SDK'
$openAlPath = Resolve-RequiredDirectory -Path $OpenAlSdk -Label 'OpenAL SDK'
$mpg123Path = Resolve-RequiredDirectory -Path $Mpg123Sdk -Label 'mpg123 SDK'

$requiredSdkFiles = @(
    (Join-Path $openXrPath 'include\openxr\openxr.h'),
    (Join-Path $openXrPath 'x64\lib\openxr_loader.lib'),
    (Join-Path $streamlinePath 'include\sl.h'),
    (Join-Path $streamlinePath 'lib\x64\sl.interposer.lib'),
    (Join-Path $fsr2Path 'src\ffx-fsr2-api\ffx_fsr2.h'),
    (Join-Path $fsr2Path 'src\ffx-fsr2-api\bin\ffx_fsr2_api\ffx_fsr2_api_x64.lib'),
    (Join-Path $fsr2Path 'src\ffx-fsr2-api\bin\ffx_fsr2_api\ffx_fsr2_api_dx12_x64.lib'),
    (Join-Path $openAlPath 'include\AL\al.h'),
    (Join-Path $openAlPath 'libs\Win64\OpenAL32.lib'),
    (Join-Path $mpg123Path 'include\mpg123.h'),
    (Join-Path $mpg123Path 'lib\Win64\libmpg123-0.lib')
)
foreach ($requiredFile in $requiredSdkFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Missing build prerequisite: $requiredFile"
    }
}
if (-not $GenerateOnly) {
    $msbuildPath = Resolve-RequiredFile -Path $MSBuild -Label 'MSBuild executable'
}

$assembledSource = Join-Path $workFull 'source'
$assembleScript = Join-Path $kitFull 'tools\source-kit\assemble.ps1'
& $assembleScript -Revc $Revc -Out $assembledSource -KitRoot $kitFull

$oldGameDirectory = $env:GTA_VC_RE_DIR
$env:GTA_VC_RE_DIR = $null
Push-Location $assembledSource
try {
    & $premakePath vs2019 --with-librw --with-openxr-vr `
        "--openxrsdk=$openXrPath" "--streamlinesdk=$streamlinePath" `
        "--fsr2sdk=$fsr2Path" "--openalsdk=$openAlPath" `
        "--mpg123sdk=$mpg123Path"
    if ($LASTEXITCODE -ne 0) {
        throw "Premake failed with exit code $LASTEXITCODE"
    }

    $generatedProjects = @(Get-ChildItem -LiteralPath (Join-Path $assembledSource 'build') `
        -Filter '*.vcxproj' -File -ErrorAction Stop)
    if ($generatedProjects.Count -eq 0) {
        throw 'Premake did not generate any Visual Studio projects.'
    }
    foreach ($project in $generatedProjects) {
        $content = Get-Content -LiteralPath $project.FullName -Raw
        $updated = $content.Replace('<PlatformToolset>v142</PlatformToolset>',
            '<PlatformToolset>v143</PlatformToolset>')
        if ($updated -ne $content) {
            [System.IO.File]::WriteAllText($project.FullName, $updated,
                (New-Object System.Text.UTF8Encoding($true)))
        }
    }

    $solution = Join-Path $assembledSource 'build\reVC.sln'
    if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) {
        throw "Premake did not generate the expected solution: $solution"
    }
    if ($GenerateOnly) {
        Write-Host "Generated solution: $solution"
        return
    }

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $msbuildPath
    $startInfo.Arguments = ('"{0}" /m /t:Build /p:Configuration=Release ' +
        '/p:Platform=win-amd64-librw_d3d12-oal /v:minimal') -f $solution
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true

    $processEnvironment = $startInfo.EnvironmentVariables
    $environment = [Environment]::GetEnvironmentVariables('Process')
    Set-NormalizedBuildEnvironment -Destination $processEnvironment `
        -Environment $environment

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "MSBuild failed with exit code $($process.ExitCode)"
    }

    $artifact = Join-Path $assembledSource `
        'bin\win-amd64-librw_d3d12-oal\Release\reVC.exe'
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "MSBuild succeeded but the expected artifact is missing: $artifact"
    }
    $artifactItem = Get-Item -LiteralPath $artifact
    $artifactHash = Get-Sha256 -Path $artifact

    $forwarderProject = Join-Path $assembledSource `
        'tools\dlss\forwarder\dlssnr_forwarder.vcxproj'
    if (-not (Test-Path -LiteralPath $forwarderProject -PathType Leaf)) {
        throw "DLSS-NR forwarder project is missing: $forwarderProject"
    }
    $forwarderStartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $forwarderStartInfo.FileName = $msbuildPath
    $forwarderStartInfo.Arguments = ('"{0}" /m /t:Build ' +
        '/p:Configuration=Release /p:Platform=x64 /v:minimal') -f `
        $forwarderProject
    $forwarderStartInfo.UseShellExecute = $false
    $forwarderStartInfo.CreateNoWindow = $true
    $forwarderEnvironment = $forwarderStartInfo.EnvironmentVariables
    Set-NormalizedBuildEnvironment -Destination $forwarderEnvironment `
        -Environment $environment
    $forwarderProcess = [System.Diagnostics.Process]::Start($forwarderStartInfo)
    $forwarderProcess.WaitForExit()
    if ($forwarderProcess.ExitCode -ne 0) {
        throw "DLSS-NR forwarder build failed with exit code $($forwarderProcess.ExitCode)"
    }
    $forwarder = Join-Path $assembledSource `
        'tools\dlss\forwarder\bin\Release\nvngx.dll_dlssnr.dll'
    if (-not (Test-Path -LiteralPath $forwarder -PathType Leaf)) {
        throw "Forwarder build succeeded but the artifact is missing: $forwarder"
    }
    $forwarderItem = Get-Item -LiteralPath $forwarder
    $forwarderHash = Get-Sha256 -Path $forwarder
    Write-Host ("Built {0} ({1} bytes, SHA-256 {2})" -f $artifact,
        $artifactItem.Length, $artifactHash)
    Write-Host ("Built {0} ({1} bytes, SHA-256 {2})" -f $forwarder,
        $forwarderItem.Length, $forwarderHash)
    Write-Host 'Build completed. The game was not installed or launched.'
} finally {
    Pop-Location
    $env:GTA_VC_RE_DIR = $oldGameDirectory
}
