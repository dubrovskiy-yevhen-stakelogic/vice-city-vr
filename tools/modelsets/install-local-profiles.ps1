param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [string]$OptimizedVegetation,
    [string]$XboxVehicles
)

$ErrorActionPreference = "Stop"
$game = (Resolve-Path -LiteralPath $GameRoot).Path
$modelSets = Join-Path $game "modelsets"
$builder = Join-Path $PSScriptRoot "build-img-overlay.ps1"
$compressor = Join-Path $PSScriptRoot "txdcompress.exe"

if ($OptimizedVegetation) {
    $source = (Resolve-Path -LiteralPath $OptimizedVegetation).Path
    $archiveSource = Join-Path $source "models\gta3.img"
    $profile = Join-Path $modelSets "optimized-vegetation"
    & $builder -OutputImg (Join-Path $profile "models\gta3.img") `
        -From $archiveSource -DffManifest (Join-Path $profile "vegetation_models.txt")
    New-Item -ItemType Directory -Path (Join-Path $profile "models\coll") `
        -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source "models\generic.txd") `
        -Destination (Join-Path $profile "models\generic.txd") -Force
    if (-not (Test-Path -LiteralPath $compressor -PathType Leaf)) {
        & (Join-Path $PSScriptRoot "build-txdcompress.ps1")
    }
    & $compressor --repair-alpha-mips Palm_Tree_Leaf `
        (Join-Path $profile "models\generic.txd")
    if ($LASTEXITCODE -ne 0) {
        throw "Palm alpha-mip repair failed with exit code $LASTEXITCODE."
    }
    Copy-Item -LiteralPath (Join-Path $source "models\coll\generic.col") `
        -Destination (Join-Path $profile "models\coll\generic.col") -Force
}

if ($XboxVehicles) {
    $source = (Resolve-Path -LiteralPath $XboxVehicles).Path
    $pack = Join-Path $source "Fixed Xbox Vehicles"
    $profile = Join-Path $modelSets "xbox"
    & $builder -OutputImg (Join-Path $profile "models\gta3.img") `
        -From (Join-Path $pack "gta3.img")
    New-Item -ItemType Directory -Path (Join-Path $profile "models\coll") `
        -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $profile "models\generic") `
        -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $pack "models\coll\vehicles.col") `
        -Destination (Join-Path $profile "models\coll\vehicles.col") -Force
    Copy-Item -LiteralPath (Join-Path $pack "models\generic\wheels.dff") `
        -Destination (Join-Path $profile "models\generic\wheels.dff") -Force
    Copy-Item -LiteralPath (Join-Path $pack "models\generic\wheels.txd") `
        -Destination (Join-Path $profile "models\generic\wheels.txd") -Force
}

Write-Output "Local model profiles installed under $modelSets"
