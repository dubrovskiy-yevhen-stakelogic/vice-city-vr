#pragma once

#include <stdint.h>

namespace Dlaa
{
// Native D3D12 resources are kept opaque here so the public VR header does not
// force D3D12 headers into non-D3D builds.
struct EyeInput
{
	void *color;
	void *depth;
	uint64_t depthShaderResourceView;
	uint64_t objectMotionShaderResourceView;
	uint32_t width;
	uint32_t height;
	uint32_t sourceLeft;
	// Reconstruction target size. Equal to width/height for DLAA; larger for
	// the DLSS upscaling modes, where the scene renders at a reduced size and
	// the network reconstructs the full-resolution image.
	uint32_t outputWidth;
	uint32_t outputHeight;
	float view[16];
	float projection[16];
	float nearPlane;
	float farPlane;
};

// 0 = DLAA (native), 1 = Quality, 2 = Balanced, 3 = Performance.
void SetQualityMode(int mode);
float GetQualityModeRenderRatio(int mode);

// Experimental DLSS neural-rendering integration. Mode 0 is the DLAA baseline;
// modes 1..4 select NATURAL, DETAIL, CINEMATIC, and MAX DETAIL parameters.
// VR displays the model output at 1.00x without split-eye diagnostics.
void SetNeuralRenderingEnabled(bool enabled);
bool IsNeuralRenderingEnabled();
void SetNeuralRenderingMode(int mode);
int GetNeuralRenderingMode();
void CycleNeuralRenderingMode();
const char *GetNeuralRenderingModeName();
const char *GetNeuralRenderingModeName(int mode);
// Sequential model passes. Pass 1 consumes the game render; passes 2 and 3
// consume the previous neural output. DLSS SR/DLAA reconstructs only after
// the final neural pass.
void SetNeuralRenderingPassCount(int passes);
int GetNeuralRenderingPassCount();
// VR-only approximation: 0 evaluates both eyes, 1 shares the left-eye residual.
void SetNeuralRenderingStereoMode(int mode);
int GetNeuralRenderingStereoMode();
const char *GetNeuralRenderingStereoModeName();
// VR-only fixed central NR region; peripheral pixels retain the baseline.
// 0 = off, 1 = quality, 2 = balanced, 3 = performance.
void SetNeuralRenderingFoveationMode(int mode);
int GetNeuralRenderingFoveationMode();
const char *GetNeuralRenderingFoveationModeName();
bool HasNeuralFoveationFailed();
// VR-only NR proxy scale, independent of scene resolution and foveation.
// 0 = full, 1 = 75%, 2 = 67%, 3 = 50%; full bypasses proxy reconstruction.
void SetNeuralRenderingModelScaleMode(int mode);
int GetNeuralRenderingModelScaleMode();
const char *GetNeuralRenderingModelScaleModeName();
bool HasNeuralModelScaleFailed();
// Called with both current eye inputs before either eye is evaluated.
bool PrepareNeuralStereoPair(const EyeInput &left, const EyeInput &right,
	float rasterJitterX, float rasterJitterY);
bool HasNeuralStereoSharingFailed();
// Reject the complete pair if reconstruction or presentation of either eye fails.
void RejectNeuralStereoPair();
// True once after the desktop F11 panel changes the pass count.
bool ConsumeNeuralRenderingPassCountChanged();

// Diagnostic names for the scene resolution; NR uses full-resolution DLAA.
const char *GetNeuralRenderingRegionModeName();
const char *GetNeuralRenderingRegionModeName(int mode);

// Live model controls. Percent values map directly to the DLSS-NR option
// strengths and are applied independently to every selected pass.
void SetNeuralRenderingIntensityPercent(int percent);
int GetNeuralRenderingIntensityPercent();
void SetNeuralRenderingStructurePercent(int percent);
int GetNeuralRenderingStructurePercent();
void SetNeuralRenderingLocalTonePercent(int percent);
int GetNeuralRenderingLocalTonePercent();
void SetNeuralRenderingGlobalTonePercent(int percent);
int GetNeuralRenderingGlobalTonePercent();
void SetNeuralRenderingStyle(int style);
int GetNeuralRenderingStyle();
void SetNeuralRenderingAutoMask(bool enabled);
bool IsNeuralRenderingAutoMaskEnabled();
void ResetNeuralRenderingTuning();
bool IsNeuralRenderingTuningCustom();

// EvaluateEye slot for the flat-mode desktop pass (slots 0 and 1 are the VR
// eyes). Owns its own temporal history and Streamline viewport.
enum { kDesktopSlot = 2 };

struct EyeOutput
{
	void *color;
	uint64_t shaderResourceView;
	uint32_t width;
	uint32_t height;
};

// Streamline must be initialized before the first D3D/DXGI call.
void InitializeEarly();

// Called once RenderWare has created the D3D12 device.
void AttachDevice();

bool BeginFrame(float jitterX, float jitterY);
bool EvaluateEye(int eye, const EyeInput &input, EyeOutput *output);
void ResetHistory();
void ReleaseResources();

void Shutdown();
bool IsInitialized();
bool IsSupported();
bool IsNeuralRenderingActive();
bool IsNeuralRenderingStereoActive();
bool HasNeuralRenderingFailed();
// Live A/B switches between the baseline DLSS/DLAA chain and NR-before-DLSS.
// Histories are reset because the reconstruction input changes.
void SetNeuralRenderingOutputVisible(bool visible);
bool IsNeuralRenderingOutputVisible();
bool IsNeuralRenderingSplitView();
bool IsNeuralRenderingOverlayVisible();
int GetNeuralRenderingOverlaySelection();
const char *GetNeuralRenderingEffectName();
const char *GetNeuralRenderingViewName();
bool IsNeuralRenderingFloatTuningVerified();
bool WasLastEvaluationSuccessful();
const char *GetStatus();
}
