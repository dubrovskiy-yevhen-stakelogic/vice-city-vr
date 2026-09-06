param(
    [string]$Output = (Join-Path $PSScriptRoot 'txdcompress.exe')
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'txdcompress.cpp'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Missing source file: $source"
}
$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
    $vswhereCandidates = @()
    if (${env:ProgramFiles(x86)}) {
        $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} `
            'Microsoft Visual Studio\Installer\vswhere.exe')
    }
    if ($env:ProgramFiles) {
        $vswhereCandidates += (Join-Path $env:ProgramFiles `
            'Microsoft Visual Studio\Installer\vswhere.exe')
    }
    $vswhere = $vswhereCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1
    if ($vswhere) {
        $installation = (& $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1)
        if ($installation) {
            $developerEnvironment = Join-Path $installation `
                'Common7\Tools\VsDevCmd.bat'
            if (Test-Path -LiteralPath $developerEnvironment -PathType Leaf) {
                $environmentLines = & $env:ComSpec /d /s /c `
                    ('call "{0}" -arch=x64 -host_arch=x64 >nul && set' -f `
                        $developerEnvironment)
                if ($LASTEXITCODE -ne 0) {
                    throw "Visual Studio developer environment failed with exit code $LASTEXITCODE"
                }
                $seenEnvironmentNames = @{}
                foreach ($line in $environmentLines) {
                    if ($line -match '^([^=]+)=(.*)$') {
                        $name = $matches[1]
                        if (-not $seenEnvironmentNames.ContainsKey($name)) {
                            Set-Item -Path ('Env:' + $name) -Value $matches[2]
                            $seenEnvironmentNames[$name] = $true
                        }
                    }
                }
                $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
            }
        }
    }
}
if ($null -ne $compiler) {
    $temporaryObject = Join-Path ([IO.Path]::GetTempPath()) `
        ('txdcompress-' + [guid]::NewGuid().ToString('N') + '.obj')
    try {
        & $compiler.Source /nologo /O2 /EHsc /std:c++17 `
            "/Fe:$outputPath" "/Fo:$temporaryObject" $source
    } finally {
        Remove-Item -LiteralPath $temporaryObject -Force -ErrorAction SilentlyContinue
    }
} else {
    $compiler = Get-Command clang++.exe -ErrorAction SilentlyContinue
    if ($null -eq $compiler) {
        $compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
    }
    if ($null -eq $compiler) {
        throw 'No C++ compiler found. Install Visual Studio Build Tools with Desktop C++ or add clang++/g++ to PATH.'
    }
    & $compiler.Source -O2 -std=c++17 -o $outputPath $source
}

if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
    throw "Compiler failed with exit code $LASTEXITCODE"
}
Write-Host "Built $outputPath"
