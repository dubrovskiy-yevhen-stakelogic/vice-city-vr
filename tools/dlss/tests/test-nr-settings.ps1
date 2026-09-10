[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$kitRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$source = Get-Content -LiteralPath (Join-Path $kitRoot 'overlay\src\vr\OpenXRVR.cpp') -Raw
$dlaa = Get-Content -LiteralPath (Join-Path $kitRoot 'overlay\src\vr\DLAA.cpp') -Raw
$checks = 0

function Require-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
    $script:checks++
}
function Require-Absent([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
    $script:checks++
}
function Get-MenuCase([string]$Name) {
    $match = [regex]::Match($source, '(?s)case ' + [regex]::Escape($Name) + ':.*?(?=\s*case VR_GRAPHICS_)')
    if (-not $match.Success) { throw "Menu case missing: $Name" }
    return $match.Value
}

Require-Absent $source 'VR_GRAPHICS_DLSS_NR_REGION|SCENE SCALE' 'The obsolete neural scene-scale control is still exposed.'
Require-Match $source 'ResolveQualityMode\(gDlssQualityMode,\s*Dlaa::IsNeuralRenderingEnabled\(\)\)' 'Scene resolution is not using the neural policy.'
Require-Absent $source 'SetQualityMode\(gDlssQualityMode\)|GetQualityModeRenderRatio\(gDlssQualityMode\)' 'A render path bypasses the scene-resolution policy.'
Require-Match $source '"DLSS5ModelScaleMode",\s*DlssNrScenePolicy::ModelScaleDefault\(gDlssQualityMode,\s*Dlaa::IsNeuralRenderingEnabled\(\)\),\s*path' 'An explicit model-scale setting must override legacy migration.'
Require-Match $source 'GetPrivateProfileIntA\("VR", "DLSSNeuralRendering", 0, path\)' 'Neural rendering must remain opt-in.'
Require-Match $source '"DLSSNeuralPasses", 1, path' 'New players must start with a single neural pass.'
Require-Match $source '"DLSS5StereoMode", 1, path' 'New players must default to SHARED stereo.'
Require-Match $source '"DLSS5FoveationMode", 1, path' 'New players must default to Quality foveation.'

$ordinary = Get-MenuCase 'VR_GRAPHICS_DLSS_MODE'
Require-Match $ordinary 'if\(Dlaa::IsNeuralRenderingEnabled\(\)\)\s*break;' 'Ordinary DLSS scaling must not reduce an active neural scene.'
Require-Match $ordinary 'SaveVrSetting\("DlssMode", gDlssQualityMode\)' 'Ordinary DLSS selection is not saved.'
$enable = Get-MenuCase 'VR_GRAPHICS_DLSS_NR_ENABLE'
Require-Match $enable 'SetQualityMode\(SceneDlssQualityMode\(\)\)' 'Neural toggling must restore the effective scene quality.'
Require-Match $enable 'gRenderScaleChangePending = true;' 'Neural toggling must rebuild targets when the effective resolution changes.'
Require-Match $enable 'RetryDlaaAfterNeuralModeChange\(\);' 'NR toggling must recover DLAA even if scene dimensions do not change.'
Require-Match $source 'SetNeuralRenderingOutputVisible\(showModel\);\s*ResetTemporalAaHistory\(\);\s*RetryDlaaAfterNeuralModeChange\(\);' 'A/B must retry DLAA after a paired failure.'
Require-Match $dlaa 'void SetNeuralRenderingOutputVisible\(bool visible\)[\s\S]*?gNrSharingFailed = gNrSharingLogged = false;' 'A/B must clear the previous shared failure.'

$model = Get-MenuCase 'VR_GRAPHICS_DLSS_NR_MODEL_SCALE'
Require-Match $model 'SaveVrSetting\("DLSS5ModelScaleMode", mode\)' 'Model scaling selection is not saved.'
Require-Absent $model 'gDlssQualityMode|gRenderScaleChangePending|SetQualityMode' 'Changing model scale must not resize or change the original scene.'
Require-Match $model 'ResetTemporalAaHistory\(\);\s*RetryDlaaAfterNeuralModeChange\(\);' 'Model-scale changes must reset history and allow recovery.'
Require-Match $model 'if\(!gFlatModeEnabled\)' 'The current model-scale implementation is VR-only.'
Require-Match $dlaa 'bool wantsNr = gDlssNrEnabled && gDlssNrDisplayModelOutput' 'Neural resource work must remain behind its opt-in guard.'
Require-Match $source 'HasNeuralModelScaleFailed\(\)[\s\S]*?IsNeuralRenderingStereoActive\(\)' 'Model-scale failures must be checked before ACTIVE status.'

Write-Host "NR SETTINGS: PASS ($checks source checks; not a runtime test)"
