#include "common.h"
#include "DLAA.h"

#if defined(GTA_VR_OPENXR) && defined(RW_D3D12)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include "DlssNrColorInput.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <float.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>

#include "../../vendor/librw/src/d3d12/rwd3d12.h"
#include "../../vendor/librw/src/d3d12/rwd3d12impl.h"

namespace Dlaa
{
namespace
{
enum { EYE_COUNT = 2 };
// One extra evaluation slot for the flat-mode desktop pass. It has its own
// Streamline viewport and temporal history, independent of the VR eyes;
// EYE_COUNT keeps meaning "both eyes" for the stereo success bookkeeping.
enum { SLOT_COUNT = EYE_COUNT + 1, MAX_NR_PASSES = 3 };

// Streamline 2.13 DLSS Neural Rendering ABI. The public 2.12 SDK used by this
// tree predates these declarations, so keep the reverse-engineered surface
// deliberately small and assert its exact binary layout. The GUID and field
// layout are consumed by sl.dlss_nr.dll's slDLSSNRSetOptions entry point.
constexpr sl::Feature kFeatureDLSS_NR = 1004;
constexpr sl::BufferType kBufferTypeUpliftInputColor = 70;
constexpr sl::BufferType kBufferTypeUpliftOutputColor = 71;

SL_STRUCT_BEGIN(DlssNrOptionsABI,
	sl::StructType({ 0x29dfdfe0, 0x273a, 0x4e72,
		{ 0xb4, 0x92, 0x2d, 0xc8, 0x23, 0xd5, 0xb1, 0xad } }),
	sl::kStructVersion3)
	uint32 mode = 1; // 0 off, 1 on
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	float globalToneStrength = 1.0f;
	uint32 style = 0;
	uint32 preset = 0;
	sl::Boolean useAutoMask = sl::Boolean::eTrue;
	float skinStructureStrength = 1.0f;
	uint32 performanceMode = 3;
SL_STRUCT_END()

static_assert(sizeof(DlssNrOptionsABI) == 0x48,
	"Streamline 2.13 DLSS-NR options ABI changed");

using PFun_slDLSSNRSetOptions = sl::Result(
	const sl::ViewportHandle&, const DlssNrOptionsABI&);

// Minimal ABI for the driver's NVSDK_NGX_Parameter capability block.  The
// public NGX helper methods are header-only and this tree does not otherwise
// depend on the NGX SDK; only the vtable order matters here.  The direct
// DLSS-NR forwarder writes through the same block after Streamline has
// initialized the system nvngx.dll.
struct NgxParameterABI
{
	virtual void SetULL(const char*, unsigned long long) = 0;
	virtual void SetFloat(const char*, float) = 0;
	virtual void SetDouble(const char*, double) = 0;
	virtual void SetUInt(const char*, unsigned int) = 0;
	virtual void SetInt(const char*, int) = 0;
	virtual void SetD3D11Resource(const char*, void*) = 0;
	virtual void SetD3D12Resource(const char*, ID3D12Resource*) = 0;
	virtual void SetPointer(const char*, void*) = 0;
	virtual int GetULL(const char*, unsigned long long*) = 0;
	virtual int GetFloat(const char*, float*) = 0;
	virtual int GetDouble(const char*, double*) = 0;
	virtual int GetUInt(const char*, unsigned int*) = 0;
	virtual int GetInt(const char*, int*) = 0;
	virtual int GetD3D11Resource(const char*, void**) = 0;
	virtual int GetD3D12Resource(const char*, ID3D12Resource**) = 0;
	virtual int GetPointer(const char*, void**) = 0;
};

using PFun_NgxGetCapabilityParameters = int(__cdecl*)(NgxParameterABI**);
using PFun_NrCreate = void*(__cdecl*)(const wchar_t*, const wchar_t*,
	ID3D12Device*, ID3D12GraphicsCommandList*, void*, unsigned int,
	unsigned int, int, float, int, float, float, float, float, int, int);
using PFun_NrEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*,
	void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
	ID3D12Resource*, unsigned int, unsigned int, unsigned int,
	unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
	unsigned int, unsigned int, int, int, float, int, float, float, float, float,
	int, float, float);
using PFun_NrRelease = void(__cdecl*)(void*);
using PFun_NrSetFloatSlot = void(__cdecl*)(int);
using PFun_NrProbeFloat = void(__cdecl*)(void*, const char*, float, int);
using PFun_NrGetFloat = int(__cdecl*)(void*, const char*, float*, int);

struct EyeState
{
	ID3D12Resource *output;
	ID3D12Resource *nrOutput;
	ID3D12Resource *nrScratch;
	ID3D12Resource *nrInput;
	ID3D12Resource *motion;
	void *nrDirectFeature[MAX_NR_PASSES];
	D3D12_GPU_DESCRIPTOR_HANDLE outputSrv;
	D3D12_GPU_DESCRIPTOR_HANDLE nrOutputSrv;
	D3D12_GPU_DESCRIPTOR_HANDLE nrScratchSrv;
	D3D12_CPU_DESCRIPTOR_HANDLE motionUavCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE motionUavGpu;
	uint32 outputSrvIndex;
	uint32 nrOutputSrvIndex;
	uint32 nrScratchSrvIndex;
	uint32 motionUavIndex;
	D3D12_RESOURCE_STATES outputState;
	D3D12_RESOURCE_STATES nrOutputState;
	D3D12_RESOURCE_STATES nrScratchState;
	D3D12_RESOURCE_STATES nrInputState;
	D3D12_RESOURCE_STATES motionState;
	uint32 width;
	uint32 height;
	uint32 outputWidth;
	uint32 outputHeight;
	uint32 nrRegionLeft;
	uint32 nrRegionTop;
	uint32 nrRegionWidth;
	uint32 nrRegionHeight;
	int nrRegionMode;
	int optionsMode;
	bool optionsSet;
	uint32 nrOptionsSetMask;
	bool nrEnabled;
	int nrPassCount;
	bool historyValid;
	bool dlssUsedNeuralInput;
	bool nrHistoryValid[MAX_NR_PASSES];
	float previousView[16];
	float previousProjection[16];

	EyeState() : output(nil), nrOutput(nil), nrScratch(nil), nrInput(nil),
		motion(nil),
		outputSrvIndex(UINT32_MAX), nrOutputSrvIndex(UINT32_MAX),
		nrScratchSrvIndex(UINT32_MAX),
		motionUavIndex(UINT32_MAX), outputState(D3D12_RESOURCE_STATE_COMMON),
		nrOutputState(D3D12_RESOURCE_STATE_COMMON),
		nrScratchState(D3D12_RESOURCE_STATE_COMMON),
		nrInputState(D3D12_RESOURCE_STATE_COMMON),
		motionState(D3D12_RESOURCE_STATE_COMMON), width(0), height(0),
		outputWidth(0), outputHeight(0), nrRegionLeft(0), nrRegionTop(0),
		nrRegionWidth(0), nrRegionHeight(0), nrRegionMode(0), optionsMode(-1),
		optionsSet(false), nrOptionsSetMask(0), nrEnabled(false),
		nrPassCount(1), historyValid(false), dlssUsedNeuralInput(false)
	{
		outputSrv.ptr = nrOutputSrv.ptr = nrScratchSrv.ptr = 0;
		motionUavCpu.ptr = motionUavGpu.ptr = 0;
		memset(nrDirectFeature, 0, sizeof(nrDirectFeature));
		memset(nrHistoryValid, 0, sizeof(nrHistoryValid));
		memset(previousView, 0, sizeof(previousView));
		memset(previousProjection, 0, sizeof(previousProjection));
	}
};

bool gInitialized;
bool gDeviceAttached;
bool gSupported;
bool gDlssNrProbeRequested;
bool gDlssNrReady;
bool gDlssNrEnabled;
bool gDlssNrRuntimeFailed;
bool gDlssNrActiveLogged;
bool gFrameReady;
bool gLastEvaluationSucceeded;
uint32 gSuccessfulEvaluationsThisFrame;
uint32 gEvaluationLogCount;
uint32 gDlssNrEvaluationLogCount;
char gStatus[192] = "Streamline not initialized";
wchar_t gPluginDirectory[MAX_PATH];
const wchar_t *gPluginPaths[1];
sl::FrameToken *gFrameToken;
float gJitterX;
float gJitterY;
EyeState gEye[SLOT_COUNT];
ID3D12RootSignature *gMotionRootSignature;
ID3D12PipelineState *gMotionPipeline;
PFun_slDLSSNRSetOptions *gDlssNrSetOptions;
HMODULE gDlssNrModule;
uint8 *gDlssNrAdaBranch;
uint8 *gDlssNrMissingCallbackReturns[2];
HMODULE gDlssNrForwarder;
NgxParameterABI *gDlssNrCapabilityParams;
PFun_NrCreate gDlssNrDirectCreate;
PFun_NrEvaluate gDlssNrDirectEvaluate;
PFun_NrRelease gDlssNrDirectRelease;
PFun_NrSetFloatSlot gDlssNrDirectSetFloatSlot;
PFun_NrProbeFloat gDlssNrDirectProbeFloat;
PFun_NrGetFloat gDlssNrDirectGetFloat;
int *gDlssNrDirectLastInit;
int *gDlssNrDirectLastCreate;
bool gDlssNrDirectReady;
bool gDlssNrDirectAttempted;
bool gDlssNrDisplayModelOutput = true;
bool gDlssNrSplitView = false;
bool gDlssNrOverlayVisible = true;
int gDlssNrPassCount = 1;
int gDlssNrRegionMode = 0;
bool gDlssNrPassCountChangedByOverlay;
	// Desktop-only post-composition modes: raw output, two RGB transfer levels,
	// luminance transfer, and an amplified absolute-difference diagnostic.
int gDlssNrEffectMode = 1;
int gDlssNrMode = 2;
	// VR profiles use style values 0..2 and strengths 0..2. A skin value of -1
	// follows local structure. Preset 0 selects the model's default network.
struct DlssNrProfile
{
	const char *name;
	int preset;
	float intensity;
	int style;
	float localStructure;
	float localTone;
	float globalTone;
	float skinStructure;
	int autoMask;
	int uiCorrection;
};

const DlssNrProfile gDlssNrProfiles[5] = {
	{ "DLAA BASELINE", 0, 0.0f, 1, 0.0f, 0.0f, 0.0f, -1.0f, 1, 1 },
	{ "NATURAL",       0, 1.00f, 1, 1.25f, 0.50f, 0.25f, -1.0f, 1, 1 },
	{ "DETAIL",        0, 1.25f, 1, 1.50f, 0.25f, 0.00f, -1.0f, 1, 1 },
	{ "CINEMATIC",     0, 1.10f, 2, 1.35f, 0.50f, 0.50f, -1.0f, 1, 1 },
	{ "MAX DETAIL",    0, 1.50f, 0, 2.00f, 0.25f, 0.00f, -1.0f, 1, 1 },
};

struct DlssNrTuning
{
	float intensity;
	int style;
	float localStructure;
	float localTone;
	float globalTone;
	int autoMask;
};

// gDlssNrMode starts on DETAIL, so initialize the live values to that profile.
// Profile changes restore their defaults; explicit setters mark the result as
// custom and are consumed by both Streamline and the direct feature-18 path.
DlssNrTuning gDlssNrTuning = { 1.25f, 1, 1.50f, 0.25f, 0.00f, 1 };
bool gDlssNrTuningCustom;

const DlssNrProfile &GetDlssNrProfile()
{
	return gDlssNrProfiles[gDlssNrMode >= 0 && gDlssNrMode < 5 ?
		gDlssNrMode : 0];
}

void ApplyDlssNrProfileTuning()
{
	const DlssNrProfile &profile = GetDlssNrProfile();
	gDlssNrTuning.intensity = profile.intensity;
	gDlssNrTuning.style = profile.style;
	gDlssNrTuning.localStructure = profile.localStructure;
	gDlssNrTuning.localTone = profile.localTone;
	gDlssNrTuning.globalTone = profile.globalTone;
	gDlssNrTuning.autoMask = profile.autoMask;
	gDlssNrTuningCustom = false;
}

int StrengthToPercent(float strength)
{
	return (int)(strength*100.0f+0.5f);
}

int ClampTuningPercent(int percent, int minimum, int maximum)
{
	return percent < minimum ? minimum :
		(percent > maximum ? maximum : percent);
}

void FillDlssNrOptions(DlssNrOptionsABI &options)
{
	const DlssNrProfile &profile = GetDlssNrProfile();
	options.mode = gDlssNrMode > 0 ? 1u : 0u;
	options.intensity = gDlssNrTuning.intensity;
	options.localToneStrength = gDlssNrTuning.localTone;
	options.localStructureStrength = gDlssNrTuning.localStructure;
	options.globalToneStrength = gDlssNrTuning.globalTone;
	options.style = (uint32)gDlssNrTuning.style;
	options.preset = (uint32)profile.preset;
	options.useAutoMask = gDlssNrTuning.autoMask ?
		sl::Boolean::eTrue : sl::Boolean::eFalse;
	// -1 makes skin detail follow the live local-structure strength.
	options.skinStructureStrength = -1.0f;
	options.performanceMode = 3;
}

void WriteLog(const char *format, ...);

void LogDlssNrProfile(const char *reason, int slot)
{
	const DlssNrProfile &profile = GetDlssNrProfile();
	WriteLog("[DLSS-NR profile] %s slot=%d mode=%d %s%s preset=%d style=%d "
		"intensity=%.2f structure=%.2f localTone=%.2f globalTone=%.2f "
		"skin=%.2f autoMask=%d uiCorrection=%d", reason, slot,
		gDlssNrMode, profile.name, gDlssNrTuningCustom ? " CUSTOM" : "",
		profile.preset, gDlssNrTuning.style, gDlssNrTuning.intensity,
		gDlssNrTuning.localStructure, gDlssNrTuning.localTone,
		gDlssNrTuning.globalTone, -1.0f, gDlssNrTuning.autoMask,
		profile.uiCorrection);
}

uint32 gDlssNrActiveMask;
bool gDlssNrStereoDisplayReady;
int gDlssNrOverlaySelection;
bool gDlssNrControlKeys[7];
bool gDlssNrFloatTuningVerified;
LARGE_INTEGER gPerfFrequency;
LARGE_INTEGER gPerfWindowStart;
uint32 gPerfFrameCount;
bool gPerfWasNrActive;

void ReleaseDirectNrFeatures(EyeState &state, int slot);
bool InitializeDirectNrBackend(ID3D12Device *device);

uint32 NrViewportId(int slot, int pass)
{
	// NR histories have their own reset state. Never share their constants
	// viewport with the subsequent DLSS reconstruction for the same eye.
	return (uint32)(SLOT_COUNT+slot*MAX_NR_PASSES+pass);
}

const char *ResultName(sl::Result result)
{
	switch(result){
	case sl::Result::eOk: return "OK";
	case sl::Result::eErrorInvalidParameter: return "invalid parameter";
	case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
	case sl::Result::eErrorOSOutOfDate: return "OS out of date";
	case sl::Result::eErrorOSDisabledHWS: return "hardware scheduling disabled";
	case sl::Result::eErrorDeviceNotCreated: return "device not created";
	case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter";
	case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
	case sl::Result::eErrorNoPlugins: return "plugins not found";
	case sl::Result::eErrorNGXFailed: return "NGX initialization failed";
	case sl::Result::eErrorNotInitialized: return "not initialized";
	case sl::Result::eErrorInvalidIntegration: return "invalid integration";
	case sl::Result::eErrorMissingInputParameter: return "missing input";
	case sl::Result::eErrorMissingConstants: return "missing constants";
	case sl::Result::eErrorDuplicatedConstants: return "duplicated frame constants";
	case sl::Result::eErrorCommonConstantsMissing: return "common constants missing";
	case sl::Result::eErrorComputeFailed: return "compute failed";
	case sl::Result::eErrorFeatureMissing: return "DLSS plugin missing";
	case sl::Result::eErrorFeatureNotSupported: return "DLSS not supported";
	case sl::Result::eErrorFeatureFailedToLoad: return "DLSS plugin failed to load";
	default: return "Streamline error";
	}
}

void WriteLog(const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message)-1] = '\0';

	FILE *file = fopen("streamline_dlaa.log", "a");
	if(file){
		SYSTEMTIME now;
		GetLocalTime(&now);
		fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
			now.wSecond, now.wMilliseconds, message);
		fclose(file);
	}
}

void SetStatus(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(gStatus, sizeof(gStatus), format, args);
	va_end(args);
	gStatus[sizeof(gStatus)-1] = '\0';
	WriteLog("[DLAA] %s", gStatus);
}

bool FindPluginDirectory()
{
	DWORD length = GetModuleFileNameW(nil, gPluginDirectory, MAX_PATH);
	if(length == 0 || length >= MAX_PATH)
		return false;
	while(length > 0 && gPluginDirectory[length-1] != L'\\' &&
	      gPluginDirectory[length-1] != L'/')
		length--;
	if(length == 0)
		return false;
	gPluginDirectory[length-1] = L'\0';
	gPluginPaths[0] = gPluginDirectory;
	return true;
}

D3D12_HEAP_PROPERTIES DefaultHeap()
{
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	return heap;
}

bool CreateTexture(ID3D12Device *device, uint32 width, uint32 height,
	DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
	D3D12_RESOURCE_STATES initialState, ID3D12Resource **resource)
{
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Alignment = 0;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	D3D12_HEAP_PROPERTIES heap = DefaultHeap();
	return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
		&desc, initialState, nil, IID_PPV_ARGS(resource)));
}

bool CreateMotionPipeline()
{
	if(gMotionRootSignature && gMotionPipeline)
		return true;
	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device)
		return false;
	static const char *shaderSource =
		"cbuffer MotionConstants : register(b0) {"
		" row_major float4x4 currentClipToView;"
		" row_major float4x4 currentViewToPreviousView;"
		" row_major float4x4 previousViewToClip;"
		" float2 jitterPixels; float2 inverseSize;"
		" uint sourceLeft; uint width; uint height; uint hasObjectMotion; };"
		"Texture2D<float> depthTexture : register(t0);"
		"Texture2D<float2> objectMotionTexture : register(t1);"
		"RWTexture2D<float2> motionOutput : register(u0);"
		"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
		" if(id.x >= width || id.y >= height) return;"
		" float2 pixel = float2(id.xy) + 0.5;"
		" float2 currentPixel = pixel-jitterPixels;"
		" float2 currentNdc = float2(currentPixel.x*2.0*inverseSize.x-1.0,"
		"  1.0-currentPixel.y*2.0*inverseSize.y);"
		" float depth = depthTexture.Load(int3(id.x+sourceLeft,id.y,0));"
		" float4 currentView = mul(float4(currentNdc,depth,1.0),currentClipToView);"
		" if(abs(currentView.w) < 1e-6) { motionOutput[id.xy]=0.0; return; }"
		" currentView /= currentView.w;"
		" float4 previousView = mul(currentView,currentViewToPreviousView);"
		" float4 previousClip = mul(previousView,previousViewToClip);"
		" if(previousClip.w <= 1e-6) { motionOutput[id.xy]=0.0; return; }"
		" float2 previousNdc = previousClip.xy/previousClip.w;"
		" float2 previousPixel = float2((previousNdc.x+1.0)*0.5*width,"
		"  (1.0-previousNdc.y)*0.5*height);"
		" float2 motion = previousPixel-currentPixel;"
		" if(hasObjectMotion != 0) {"
		"  float2 objectMotionNdc = objectMotionTexture.Load(int3(id.x+sourceLeft,id.y,0));"
		"  if(all(abs(objectMotionNdc) < 32768.0))"
		"   motion = float2(objectMotionNdc.x*0.5*width,"
		"    -objectMotionNdc.y*0.5*height); }"
		" motionOutput[id.xy] = clamp(motion,"
		"  float2(-32768.0,-32768.0),float2(32768.0,32768.0)); }";
	ID3DBlob *shader = nil;
	ID3DBlob *errors = nil;
	HRESULT result = D3DCompile(shaderSource, strlen(shaderSource),
		"vice_city_vr_camera_motion", nil, nil, "main", "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
	if(FAILED(result)){
		if(errors)
			WriteLog("[DLAA] Camera motion shader error: %s",
				(const char*)errors->GetBufferPointer());
		if(errors) errors->Release();
		if(shader) shader->Release();
		return false;
	}
	if(errors) errors->Release();

	D3D12_DESCRIPTOR_RANGE ranges[3] = {};
	for(uint32 index = 0; index < 3; index++){
		ranges[index].RangeType = index < 2 ?
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[index].NumDescriptors = 1;
		ranges[index].BaseShaderRegister = index < 2 ? index : 0;
		ranges[index].RegisterSpace = 0;
		ranges[index].OffsetInDescriptorsFromTableStart = 0;
	}
	D3D12_ROOT_PARAMETER parameters[4] = {};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.ShaderRegister = 0;
	parameters[0].Constants.RegisterSpace = 0;
	parameters[0].Constants.Num32BitValues = 56;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	for(uint32 index = 0; index < 3; index++){
		parameters[index+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[index+1].DescriptorTable.NumDescriptorRanges = 1;
		parameters[index+1].DescriptorTable.pDescriptorRanges = &ranges[index];
		parameters[index+1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
	rootDesc.NumParameters = ARRAY_SIZE(parameters);
	rootDesc.pParameters = parameters;
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
	ID3DBlob *serialized = nil;
	ID3DBlob *rootErrors = nil;
	result = D3D12SerializeRootSignature(&rootDesc,
		D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
	if(FAILED(result) || !serialized){
		if(rootErrors)
			WriteLog("[DLAA] Camera motion root signature error: %s",
				(const char*)rootErrors->GetBufferPointer());
		if(rootErrors) rootErrors->Release();
		if(serialized) serialized->Release();
		shader->Release();
		return false;
	}
	if(rootErrors) rootErrors->Release();
	result = device->CreateRootSignature(0, serialized->GetBufferPointer(),
		serialized->GetBufferSize(), IID_PPV_ARGS(&gMotionRootSignature));
	serialized->Release();
	if(FAILED(result)){
		shader->Release();
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
	pipelineDesc.pRootSignature = gMotionRootSignature;
	pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
	pipelineDesc.CS.BytecodeLength = shader->GetBufferSize();
	result = device->CreateComputePipelineState(&pipelineDesc,
		IID_PPV_ARGS(&gMotionPipeline));
	shader->Release();
	if(FAILED(result)){
		gMotionRootSignature->Release();
		gMotionRootSignature = nil;
		return false;
	}
	gMotionRootSignature->SetName(L"Vice City VR camera motion root signature");
	gMotionPipeline->SetName(L"Vice City VR camera motion pipeline");
	return true;
}

void ReleaseMotionPipeline()
{
	if(gMotionPipeline)
		rw::d3d12::deferRelease(gMotionPipeline);
	if(gMotionRootSignature)
		rw::d3d12::deferRelease(gMotionRootSignature);
	gMotionPipeline = nil;
	gMotionRootSignature = nil;
}

void ReleaseEye(int eye)
{
	if(eye < 0 || eye >= SLOT_COUNT)
		return;
	EyeState &state = gEye[eye];
	ReleaseDirectNrFeatures(state, eye);
	if(gInitialized && gDeviceAttached && state.optionsSet){
		sl::ViewportHandle viewport(eye);
		slFreeResources(sl::kFeatureDLSS, viewport);
	}
	if(gInitialized && gDeviceAttached && state.nrOptionsSetMask){
		for(int pass = 0; pass < MAX_NR_PASSES; pass++){
			if((state.nrOptionsSetMask & (1u << pass)) == 0)
				continue;
			sl::ViewportHandle viewport(NrViewportId(eye, pass));
			slFreeResources(kFeatureDLSS_NR, viewport);
		}
	}
	if(state.output)
		rw::d3d12::deferRelease(state.output);
	if(state.nrOutput)
		rw::d3d12::deferRelease(state.nrOutput);
	if(state.nrScratch)
		rw::d3d12::deferRelease(state.nrScratch);
	if(state.nrInput)
		rw::d3d12::deferRelease(state.nrInput);
	if(state.motion)
		rw::d3d12::deferRelease(state.motion);
	if(state.outputSrvIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.outputSrvIndex, UINT32_MAX, UINT32_MAX);
	if(state.nrOutputSrvIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.nrOutputSrvIndex,
			UINT32_MAX, UINT32_MAX);
	if(state.nrScratchSrvIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.nrScratchSrvIndex,
			UINT32_MAX, UINT32_MAX);
	if(state.motionUavIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.motionUavIndex, UINT32_MAX, UINT32_MAX);
	state = EyeState();
}

// 0 DLAA, 1 Quality, 2 Balanced, 3 Performance; mirrored in the VR menu.
int gQualityMode;
const float gQualityRatios[4] = { 1.0f, 2.0f/3.0f, 0.58f, 0.5f };

struct NrRegion
{
	uint32 left;
	uint32 top;
	uint32 width;
	uint32 height;
	int mode;
};

NrRegion ResolveNrRegion(int eye, uint32 width, uint32 height,
	uint32 outputWidth, uint32 outputHeight)
{
	(void)eye;
	(void)outputWidth;
	(void)outputHeight;
	// Feature 18 is a 1:1 neural stage, not a spatial upscaler. Its resources
	// cover the complete game render at the current DLSS work resolution; the
	// following DLSS SR/DLAA evaluation reconstructs the presentation size.
	NrRegion region = { 0, 0, width, height, gQualityMode };
	return region;
}

bool CreateEyeResources(int eye, uint32 width, uint32 height,
	uint32 outputWidth, uint32 outputHeight)
{
	if(eye < 0 || eye >= SLOT_COUNT || width == 0 || height == 0)
		return false;
	if(outputWidth < width) outputWidth = width;
	if(outputHeight < height) outputHeight = height;
	const NrRegion nrRegion = ResolveNrRegion(eye, width, height,
		outputWidth, outputHeight);
	EyeState &state = gEye[eye];
	// Pure DLAA/baseline sessions do not allocate or evaluate NR resources.
	bool wantsNr = gDlssNrEnabled && gDlssNrDisplayModelOutput &&
		gDlssNrMode > 0 &&
		(gDlssNrDirectReady || gDlssNrReady) && !gDlssNrRuntimeFailed;
	if(state.output && state.motion && state.width == width &&
	   state.height == height && state.outputWidth == outputWidth &&
	   state.outputHeight == outputHeight && state.optionsMode == gQualityMode &&
	   state.nrEnabled == wantsNr &&
	   (!wantsNr || (state.nrPassCount == gDlssNrPassCount &&
	    state.nrRegionMode == nrRegion.mode &&
	    state.nrRegionLeft == nrRegion.left && state.nrRegionTop == nrRegion.top &&
	    state.nrRegionWidth == nrRegion.width &&
	    state.nrRegionHeight == nrRegion.height &&
	    state.nrOutput && state.nrInput &&
	    (gDlssNrPassCount == 1 || state.nrScratch))))
		return true;
	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device)
		return false;
	// Replacing live targets: the outgoing ones can still be referenced by
	// command lists in flight, so drain the GPU before releasing them.
	if(state.output || state.nrOutput || state.nrScratch || state.nrInput ||
	   state.motion)
		rw::d3d12::waitForGpu();
	ReleaseEye(eye);
	if(!CreateTexture(device, outputWidth, outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
	   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.output) ||
	   !CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT,
	   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.motion)){
		ReleaseEye(eye);
		return false;
	}
	if(wantsNr &&
	   (!CreateTexture(device, nrRegion.width, nrRegion.height,
	   DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.nrOutput) ||
	   (gDlssNrPassCount > 1 && !CreateTexture(device, nrRegion.width,
	   nrRegion.height,
	   DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.nrScratch)) ||
	   !CreateTexture(device, width, height,
	   DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
	   D3D12_RESOURCE_STATE_COPY_DEST, &state.nrInput))){
		ReleaseEye(eye);
		return false;
	}
	state.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.nrOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.nrScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.nrInputState = D3D12_RESOURCE_STATE_COPY_DEST;
	state.motionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.width = width;
	state.height = height;
	state.outputWidth = outputWidth;
	state.outputHeight = outputHeight;
	state.nrRegionLeft = nrRegion.left;
	state.nrRegionTop = nrRegion.top;
	state.nrRegionWidth = nrRegion.width;
	state.nrRegionHeight = nrRegion.height;
	state.nrRegionMode = nrRegion.mode;
	state.optionsMode = gQualityMode;
	state.nrEnabled = wantsNr;
	state.nrPassCount = gDlssNrPassCount;

	D3D12_CPU_DESCRIPTOR_HANDLE outputSrvCpu = {};
	if(!rw::d3d12::allocateShaderResourceDescriptor(&outputSrvCpu, &state.outputSrv,
	   &state.outputSrvIndex) ||
	   !rw::d3d12::allocateShaderResourceDescriptor(&state.motionUavCpu,
	   &state.motionUavGpu, &state.motionUavIndex)){
		ReleaseEye(eye);
		return false;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE nrOutputSrvCpu = {};
	D3D12_CPU_DESCRIPTOR_HANDLE nrScratchSrvCpu = {};
	if(wantsNr && !rw::d3d12::allocateShaderResourceDescriptor(&nrOutputSrvCpu,
	   &state.nrOutputSrv, &state.nrOutputSrvIndex)){
		ReleaseEye(eye);
		return false;
	}
	if(wantsNr && gDlssNrPassCount > 1 &&
	   !rw::d3d12::allocateShaderResourceDescriptor(&nrScratchSrvCpu,
	   &state.nrScratchSrv, &state.nrScratchSrvIndex)){
		ReleaseEye(eye);
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(state.output, &srv, outputSrvCpu);
	if(wantsNr){
		device->CreateShaderResourceView(state.nrOutput, &srv, nrOutputSrvCpu);
		if(state.nrScratch)
			device->CreateShaderResourceView(state.nrScratch, &srv,
				nrScratchSrvCpu);
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.Format = DXGI_FORMAT_R16G16_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(state.motion, nil, &uav, state.motionUavCpu);

	wchar_t name[64];
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR DLAA output eye %d", eye);
	state.output->SetName(name);
	if(wantsNr){
		swprintf(name, ARRAY_SIZE(name), L"Vice City DLSS-NR output slot %d", eye);
		state.nrOutput->SetName(name);
		if(state.nrScratch){
			swprintf(name, ARRAY_SIZE(name),
				L"Vice City DLSS-NR pass scratch slot %d", eye);
			state.nrScratch->SetName(name);
		}
		swprintf(name, ARRAY_SIZE(name), L"Vice City DLSS-NR local color slot %d", eye);
		state.nrInput->SetName(name);
	}
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR motion vectors eye %d", eye);
	state.motion->SetName(name);

	sl::DLSSOptions options{};
	switch(gQualityMode){
	case 1: options.mode = sl::DLSSMode::eMaxQuality; break;
	case 2: options.mode = sl::DLSSMode::eBalanced; break;
	case 3: options.mode = sl::DLSSMode::eMaxPerformance; break;
	default: options.mode = sl::DLSSMode::eDLAA; break;
	}
	options.outputWidth = outputWidth;
	options.outputHeight = outputHeight;
	options.dlaaPreset = sl::DLSSPreset::ePresetK;
	options.colorBuffersHDR = sl::Boolean::eFalse;
	options.useAutoExposure = sl::Boolean::eTrue;
	options.alphaUpscalingEnabled = sl::Boolean::eFalse;
	sl::ViewportHandle viewport(eye);
	const sl::Result result = slDLSSSetOptions(viewport, options);
	if(result != sl::Result::eOk){
		SetStatus("DLAA options failed for eye %d: %s (%d)", eye,
			ResultName(result), (int)result);
		ReleaseEye(eye);
		return false;
	}
	state.optionsSet = true;
	return true;
}

void ReleaseDirectNrFeatures(EyeState &state, int slot)
{
	for(int pass = 0; pass < MAX_NR_PASSES; pass++){
		if(state.nrDirectFeature[pass] && gDlssNrDirectRelease)
			gDlssNrDirectRelease(state.nrDirectFeature[pass]);
		state.nrDirectFeature[pass] = nil;
		state.nrHistoryValid[pass] = false;
	}
	if(slot >= 0 && slot < SLOT_COUNT)
		gDlssNrActiveMask &= ~(1u << slot);
}

bool InitializeDirectNrBackend(ID3D12Device *device)
{
	if(gDlssNrDirectReady)
		return true;
	if(gDlssNrDirectAttempted || !gDlssNrEnabled || !device || !gPluginDirectory[0])
		return false;
	gDlssNrDirectAttempted = true;

	// Streamline initialized the system NGX core in slSetD3DDevice.  Reuse its
	// capability block: unlike a freshly allocated parameter block it contains
	// the snippet and preset callbacks required when feature 18 is created.
	HMODULE ngxCore = GetModuleHandleW(L"nvngx.dll");
	if(!ngxCore)
		ngxCore = GetModuleHandleW(L"_nvngx.dll");
	if(!ngxCore){
		WriteLog("[DLSS-NR direct] system NGX core is not loaded");
		return false;
	}
	PFun_NgxGetCapabilityParameters getCapability =
		reinterpret_cast<PFun_NgxGetCapabilityParameters>(GetProcAddress(
			ngxCore, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
	if(!getCapability){
		WriteLog("[DLSS-NR direct] NGX capability export is unavailable");
		return false;
	}
	const int capabilityResult = getCapability(&gDlssNrCapabilityParams);
	if(capabilityResult != 1 || !gDlssNrCapabilityParams){
		WriteLog("[DLSS-NR direct] capability parameters failed: 0x%08X",
			(uint32)capabilityResult);
		gDlssNrCapabilityParams = nil;
		return false;
	}

	wchar_t forwarderPath[MAX_PATH];
	swprintf(forwarderPath, MAX_PATH, L"%s\\nvngx.dll_dlssnr.dll",
		gPluginDirectory);
	gDlssNrForwarder = LoadLibraryW(forwarderPath);
	if(!gDlssNrForwarder){
		WriteLog("[DLSS-NR direct] forwarder missing or unloadable (%lu): %ls",
			GetLastError(), forwarderPath);
		gDlssNrCapabilityParams = nil;
		return false;
	}

	gDlssNrDirectCreate = reinterpret_cast<PFun_NrCreate>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_create"));
	gDlssNrDirectEvaluate = reinterpret_cast<PFun_NrEvaluate>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_evaluate"));
	gDlssNrDirectRelease = reinterpret_cast<PFun_NrRelease>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_release"));
	gDlssNrDirectSetFloatSlot = reinterpret_cast<PFun_NrSetFloatSlot>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_set_float_slot"));
	gDlssNrDirectProbeFloat = reinterpret_cast<PFun_NrProbeFloat>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_probe_float"));
	gDlssNrDirectGetFloat = reinterpret_cast<PFun_NrGetFloat>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_get_float"));
	gDlssNrDirectLastInit = reinterpret_cast<int*>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_last_init"));
	gDlssNrDirectLastCreate = reinterpret_cast<int*>(GetProcAddress(
		gDlssNrForwarder, "dlssnr_call_last_create"));
	if(!gDlssNrDirectCreate || !gDlssNrDirectEvaluate ||
	   !gDlssNrDirectRelease || !gDlssNrDirectSetFloatSlot ||
	   !gDlssNrDirectProbeFloat){
		WriteLog("[DLSS-NR direct] forwarder exports are incomplete");
		FreeLibrary(gDlssNrForwarder);
		gDlssNrForwarder = nil;
		gDlssNrCapabilityParams = nil;
		return false;
	}

	// This capability-block ABI exposes the float setter at slot 6 and its
	// matching getter at slot 14. Verify both slots before enabling tuning.
	const float expected = 0.375f;
	const int floatSlot = 6;
	const int floatGetterSlot = 14;
	float readBack = 0.0f;
	gDlssNrDirectProbeFloat(gDlssNrCapabilityParams,
		"DLSSNR.ViceCityFloatProbe", expected, floatSlot);
	const int readResult = gDlssNrDirectGetFloat ?
		gDlssNrDirectGetFloat(gDlssNrCapabilityParams,
		"DLSSNR.ViceCityFloatProbe", &readBack, floatGetterSlot) : 0;
	gDlssNrFloatTuningVerified = readResult == 1 && readBack == expected;
	gDlssNrDirectSetFloatSlot(floatSlot);
	WriteLog("[DLSS-NR direct] float ABI set=%d get=%d -> result 0x%08X, "
		"value %.6f (%s)", floatSlot, floatGetterSlot, (uint32)readResult,
		readBack, gDlssNrFloatTuningVerified ? "verified" : "unverified");

	gDlssNrDirectReady = true;
	WriteLog("[DLSS-NR direct] backend ready; capability block %p, float slot %d",
		gDlssNrCapabilityParams, floatSlot);
	return true;
}

void ShutdownDirectNrBackend()
{
	for(int slot = 0; slot < SLOT_COUNT; slot++)
		ReleaseDirectNrFeatures(gEye[slot], slot);
	gDlssNrCapabilityParams = nil;
	gDlssNrDirectReady = false;
	gDlssNrDirectAttempted = false;
	gDlssNrDirectCreate = nil;
	gDlssNrDirectEvaluate = nil;
	gDlssNrDirectRelease = nil;
	gDlssNrDirectSetFloatSlot = nil;
	gDlssNrDirectProbeFloat = nil;
	gDlssNrDirectGetFloat = nil;
	gDlssNrDirectLastInit = nil;
	gDlssNrDirectLastCreate = nil;
	gDlssNrFloatTuningVerified = false;
	if(gDlssNrForwarder)
		FreeLibrary(gDlssNrForwarder);
	gDlssNrForwarder = nil;
}

void TickFrameRateTelemetry()
{
	const bool nrActive = gDlssNrActiveLogged && !gDlssNrRuntimeFailed;
	if(!gPerfFrequency.QuadPart)
		QueryPerformanceFrequency(&gPerfFrequency);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if(!gPerfWindowStart.QuadPart || nrActive != gPerfWasNrActive){
		gPerfWindowStart = now;
		gPerfFrameCount = 0;
		gPerfWasNrActive = nrActive;
		return;
	}
	gPerfFrameCount++;
	const double seconds = (double)(now.QuadPart-gPerfWindowStart.QuadPart) /
		(double)gPerfFrequency.QuadPart;
	if(seconds >= 5.0){
		WriteLog("[DLSS perf] backend=%s frames=%u seconds=%.3f fps=%.2f",
			nrActive ? "DLSS-NR-direct" : "DLAA", gPerfFrameCount,
			seconds, (double)gPerfFrameCount/seconds);
		gPerfWindowStart = now;
		gPerfFrameCount = 0;
	}
}

void RestoreDlssNrAdaSupportInMemory();

// Preserve the plugin's on-disk Authenticode signature: load the validated
// 2.13 module, then patch only the mapped .text page before slInit reuses it.
bool PatchDlssNrAdaSupportInMemory(const wchar_t *path)
{
	if(!path || gDlssNrModule)
		return gDlssNrModule != nil;
	constexpr DWORD64 kExpectedSize = 401024;
	constexpr LONG kBranchFileOffset = 0x3E70D;
	constexpr uintptr_t kBranchRva = 0x3F30D;
	// Both setDLSSNRScalingRatio variants end in `mov eax, 15`. Patch the
	// immediate to zero when NGX did not register the callback;
	// performanceMode=3 selects the verified 1:1 scaling ratio.
	constexpr LONG kCallbackReturnFileOffsets[2] = { 0x39699, 0x396D1 };
	constexpr uintptr_t kCallbackReturnRvas[2] = { 0x3A299, 0x3A2D1 };

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nil,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nil);
	if(file == INVALID_HANDLE_VALUE){
		WriteLog("[DLSS-NR] cannot open signed plugin for in-memory Ada patch (%lu)",
			GetLastError());
		return false;
	}
	LARGE_INTEGER size = {};
	uint8 fileOpcode = 0;
	uint8 callbackReturnOpcodes[2] = {};
	DWORD bytesRead = 0;
	bool validFile = GetFileSizeEx(file, &size) &&
		size.QuadPart == (LONGLONG)kExpectedSize &&
		SetFilePointer(file, kBranchFileOffset, nil, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
		ReadFile(file, &fileOpcode, 1, &bytesRead, nil) && bytesRead == 1 &&
		fileOpcode == 0x85;
	for(uint32 i = 0; i < ARRAY_SIZE(kCallbackReturnFileOffsets) && validFile; i++){
		bytesRead = 0;
		validFile = SetFilePointer(file, kCallbackReturnFileOffsets[i], nil,
			FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
			ReadFile(file, &callbackReturnOpcodes[i], 1, &bytesRead, nil) &&
			bytesRead == 1 && callbackReturnOpcodes[i] == 0x0F;
	}
	CloseHandle(file);
	if(!validFile){
		WriteLog("[DLSS-NR] signed plugin did not match the bounded Ada patch "
			"(size=%lld support=0x%02x callback=0x%02x/0x%02x)",
			size.QuadPart, fileOpcode, callbackReturnOpcodes[0],
			callbackReturnOpcodes[1]);
		return false;
	}

	HMODULE module = LoadLibraryW(path);
	if(!module){
		WriteLog("[DLSS-NR] preloading signed plugin failed (%lu)", GetLastError());
		return false;
	}
	uint8 *branch = reinterpret_cast<uint8*>(module)+kBranchRva;
	if(*branch != 0x85){
		WriteLog("[DLSS-NR] mapped Ada branch had unexpected opcode 0x%02x", *branch);
		FreeLibrary(module);
		return false;
	}
	DWORD oldProtect = 0;
	if(!VirtualProtect(branch, 1, PAGE_EXECUTE_READWRITE, &oldProtect)){
		WriteLog("[DLSS-NR] Ada branch VirtualProtect failed (%lu)", GetLastError());
		FreeLibrary(module);
		return false;
	}
	*branch = 0x84;
	FlushInstructionCache(GetCurrentProcess(), branch, 1);
	DWORD ignored = 0;
	VirtualProtect(branch, 1, oldProtect, &ignored);
	for(uint32 i = 0; i < ARRAY_SIZE(kCallbackReturnRvas); i++){
		uint8 *callbackReturn = reinterpret_cast<uint8*>(module)+
			kCallbackReturnRvas[i];
		if(*callbackReturn != 0x0F ||
		   !VirtualProtect(callbackReturn, 1, PAGE_EXECUTE_READWRITE,
			&oldProtect)){
			WriteLog("[DLSS-NR] missing-callback return patch %u failed", i);
			// Leave cleanup to the common restore path below.
			gDlssNrModule = module;
			gDlssNrAdaBranch = branch;
			RestoreDlssNrAdaSupportInMemory();
			return false;
		}
		*callbackReturn = 0x00;
		FlushInstructionCache(GetCurrentProcess(), callbackReturn, 1);
		VirtualProtect(callbackReturn, 1, oldProtect, &ignored);
		gDlssNrMissingCallbackReturns[i] = callbackReturn;
	}
	gDlssNrModule = module;
	gDlssNrAdaBranch = branch;
	WriteLog("[DLSS-NR] signed 2.13 plugin preloaded; in-memory Ada branch "
		"RVA 0x%llx flipped 85 -> 84; missing-callback fallback is 1:1",
		(unsigned long long)kBranchRva);
	return true;
}

void RestoreDlssNrAdaSupportInMemory()
{
	if(!gDlssNrModule)
		return;
	if(gDlssNrAdaBranch && *gDlssNrAdaBranch == 0x84){
		DWORD oldProtect = 0;
		if(VirtualProtect(gDlssNrAdaBranch, 1, PAGE_EXECUTE_READWRITE,
		   &oldProtect)){
			*gDlssNrAdaBranch = 0x85;
			FlushInstructionCache(GetCurrentProcess(), gDlssNrAdaBranch, 1);
			DWORD ignored = 0;
			VirtualProtect(gDlssNrAdaBranch, 1, oldProtect, &ignored);
		}
	}
	for(uint32 i = 0; i < ARRAY_SIZE(gDlssNrMissingCallbackReturns); i++){
		uint8 *callbackReturn = gDlssNrMissingCallbackReturns[i];
		if(callbackReturn && *callbackReturn == 0x00){
			DWORD oldProtect = 0;
			if(VirtualProtect(callbackReturn, 1, PAGE_EXECUTE_READWRITE,
			   &oldProtect)){
				*callbackReturn = 0x0F;
				FlushInstructionCache(GetCurrentProcess(), callbackReturn, 1);
				DWORD ignored = 0;
				VirtualProtect(callbackReturn, 1, oldProtect, &ignored);
			}
		}
		gDlssNrMissingCallbackReturns[i] = nil;
	}
	FreeLibrary(gDlssNrModule);
	gDlssNrModule = nil;
	gDlssNrAdaBranch = nil;
}

void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
	D3D12_RESOURCE_STATES &current, D3D12_RESOURCE_STATES next)
{
	if(!list || !resource || current == next)
		return;
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = current;
	barrier.Transition.StateAfter = next;
	list->ResourceBarrier(1, &barrier);
	current = next;
}

void CopyMatrix(sl::float4x4 &destination, const float source[16])
{
	memcpy(&destination[0].x, source, 16*sizeof(float));
}

bool GenerateCameraMotion(const EyeInput &input, EyeState &state,
	ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *heap)
{
	if(!state.historyValid)
		return true;
	if(!gMotionRootSignature || !gMotionPipeline || !list || !heap ||
	   input.depthShaderResourceView == 0)
		return false;
	struct MotionConstants {
		float currentClipToView[16];
		float currentViewToPreviousView[16];
		float previousViewToClip[16];
		float jitterPixels[2];
		float inverseSize[2];
		uint32 sourceLeft;
		uint32 width;
		uint32 height;
		uint32 hasObjectMotion;
	} constants = {};
	static_assert(sizeof(MotionConstants) == 56*sizeof(uint32),
		"Camera motion root constants must match HLSL layout");
	sl::float4x4 currentView, currentProjection, currentClipToView;
	sl::float4x4 currentCameraToWorld, currentViewToPreviousView;
	sl::float4x4 previousView, previousProjection, previousCameraToWorld;
	CopyMatrix(currentView, input.view);
	CopyMatrix(currentProjection, input.projection);
	CopyMatrix(previousView, state.previousView);
	CopyMatrix(previousProjection, state.previousProjection);
	sl::matrixFullInvert(currentClipToView, currentProjection);
	sl::matrixFullInvert(currentCameraToWorld, currentView);
	sl::matrixFullInvert(previousCameraToWorld, previousView);
	// Work in camera-centred space. Multiplying absolute world/view matrices
	// loses the sub-pixel camera motion that DLAA needs once Vice City's world
	// coordinates grow large enough.
	sl::calcCameraToPrevCamera(currentViewToPreviousView,
		currentCameraToWorld, previousCameraToWorld);
	memcpy(constants.currentClipToView, &currentClipToView[0].x,
		sizeof(constants.currentClipToView));
	memcpy(constants.currentViewToPreviousView, &currentViewToPreviousView[0].x,
		sizeof(constants.currentViewToPreviousView));
	memcpy(constants.previousViewToClip, &previousProjection[0].x,
		sizeof(constants.previousViewToClip));
	constants.jitterPixels[0] = gJitterX;
	constants.jitterPixels[1] = gJitterY;
	constants.inverseSize[0] = 1.0f/(float)input.width;
	constants.inverseSize[1] = 1.0f/(float)input.height;
	constants.sourceLeft = input.sourceLeft;
	constants.width = input.width;
	constants.height = input.height;
	constants.hasObjectMotion = input.objectMotionShaderResourceView != 0;
	ID3D12DescriptorHeap *heaps[] = { heap };
	rw::d3d12::resetWorldDrawState();
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(gMotionRootSignature);
	list->SetPipelineState(gMotionPipeline);
	list->SetComputeRoot32BitConstants(0, 56, &constants, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE depthView = { input.depthShaderResourceView };
	D3D12_GPU_DESCRIPTOR_HANDLE objectMotionView = {
		input.objectMotionShaderResourceView ?
		input.objectMotionShaderResourceView : input.depthShaderResourceView
	};
	list->SetComputeRootDescriptorTable(1, depthView);
	list->SetComputeRootDescriptorTable(2, objectMotionView);
	list->SetComputeRootDescriptorTable(3, state.motionUavGpu);
	list->Dispatch((input.width+7)/8, (input.height+7)/8, 1);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = state.motion;
	list->ResourceBarrier(1, &barrier);
	return true;
}

void BuildConstants(const EyeInput &input, EyeState &state, sl::Constants &constants)
{
	sl::float4x4 currentView, currentProjection;
	CopyMatrix(currentView, input.view);
	CopyMatrix(currentProjection, input.projection);
	constants.cameraViewToClip = currentProjection;
	sl::matrixFullInvert(constants.clipToCameraView, currentProjection);

	sl::float4x4 cameraToWorld;
	sl::matrixFullInvert(cameraToWorld, currentView);
	constants.cameraRight = sl::float3(cameraToWorld[0].x, cameraToWorld[0].y,
		cameraToWorld[0].z);
	constants.cameraUp = sl::float3(cameraToWorld[1].x, cameraToWorld[1].y,
		cameraToWorld[1].z);
	constants.cameraFwd = sl::float3(cameraToWorld[2].x, cameraToWorld[2].y,
		cameraToWorld[2].z);
	constants.cameraPos = sl::float3(cameraToWorld[3].x, cameraToWorld[3].y,
		cameraToWorld[3].z);

	if(state.historyValid){
		sl::float4x4 previousView, previousProjection, previousCameraToWorld;
		CopyMatrix(previousView, state.previousView);
		CopyMatrix(previousProjection, state.previousProjection);
		sl::matrixFullInvert(previousCameraToWorld, previousView);
		sl::float4x4 currentViewToPreviousView, currentClipToPreviousView;
		sl::calcCameraToPrevCamera(currentViewToPreviousView,
			cameraToWorld, previousCameraToWorld);
		sl::matrixMul(currentClipToPreviousView, constants.clipToCameraView,
			currentViewToPreviousView);
		sl::matrixMul(constants.clipToPrevClip, currentClipToPreviousView,
			previousProjection);
		sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);
	}else{
		const float identity[16] = {
			1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
		};
		CopyMatrix(constants.clipToPrevClip, identity);
		CopyMatrix(constants.prevClipToClip, identity);
	}

	constants.jitterOffset = sl::float2(gJitterX, gJitterY);
	constants.mvecScale = sl::float2(1.0f/input.width, 1.0f/input.height);
	constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
	constants.cameraNear = input.nearPlane;
	constants.cameraFar = input.farPlane;
	constants.cameraFOV = 2.0f*atanf(1.0f/fabsf(input.projection[0]));
	constants.cameraAspectRatio = (float)input.width/(float)input.height;
	constants.motionVectorsInvalidValue = -32768.0f;
	constants.depthInverted = sl::Boolean::eFalse;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D = sl::Boolean::eFalse;
	constants.reset = state.historyValid ? sl::Boolean::eFalse : sl::Boolean::eTrue;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated = sl::Boolean::eFalse;
	constants.motionVectorsJittered = sl::Boolean::eFalse;
}
}

void SetQualityMode(int mode)
{
	if(mode < 0 || mode > 3)
		mode = 0;
	if(gQualityMode == mode && gDlssNrRegionMode == mode)
		return;
	gQualityMode = mode;
	gDlssNrRegionMode = mode;
	gDlssNrRuntimeFailed = false;
	gDlssNrActiveMask = 0;
	gDlssNrStereoDisplayReady = false;
	gDlssNrActiveLogged = false;
	gDlssNrEvaluationLogCount = 0;
	// Defer eye-resource recreation until evaluation; menu callbacks may run
	// while previous GPU work still references those resources.
	for(int eye = 0; eye < SLOT_COUNT; eye++){
		gEye[eye].historyValid = false;
		memset(gEye[eye].nrHistoryValid, 0,
			sizeof(gEye[eye].nrHistoryValid));
	}
	WriteLog("[DLSS-NR] work scale=%s; neural 1:1 then DLSS reconstruction",
		GetNeuralRenderingRegionModeName());
}

float GetQualityModeRenderRatio(int mode)
{
	if(mode < 0 || mode > 3)
		mode = 0;
	return gQualityRatios[mode];
}

void SetNeuralRenderingEnabled(bool enabled)
{
	if(gDlssNrEnabled == enabled)
		return;
	gDlssNrEnabled = enabled;
	gDlssNrRuntimeFailed = false;
	gDlssNrActiveLogged = false;
	gDlssNrActiveMask = 0;
	gDlssNrStereoDisplayReady = false;
	gDlssNrEvaluationLogCount = 0;
	gDlssNrDirectAttempted = false;
	gDlssNrDisplayModelOutput = true;
	gDlssNrSplitView = false;
	gDlssNrOverlayVisible = true;
	gDlssNrOverlaySelection = 0;
	gDlssNrFloatTuningVerified = false;
	memset(gDlssNrControlKeys, 0, sizeof(gDlssNrControlKeys));
	// Resource creation/release stays deferred to EvaluateEye, where the GPU can
	// be drained safely. Reset both temporal histories now so re-enabling can
	// never inherit stale model state.
	for(int eye = 0; eye < SLOT_COUNT; eye++){
		gEye[eye].historyValid = false;
		memset(gEye[eye].nrHistoryValid, 0,
			sizeof(gEye[eye].nrHistoryValid));
	}
}

bool IsNeuralRenderingEnabled()
{
	return gDlssNrEnabled;
}

void SetNeuralRenderingMode(int mode)
{
	mode = (mode%5+5)%5;
	const int previousMode = gDlssNrMode;
	gDlssNrMode = mode;
	if(previousMode != mode)
		ApplyDlssNrProfileTuning();
	// Preserve the selected A/B side while changing one enhanced profile to
	// another. Legacy mode 0 still selects baseline, and returning from that
	// legacy mode selects the enhanced output as before.
	if(mode == 0)
		gDlssNrDisplayModelOutput = false;
	else if(previousMode == 0)
		gDlssNrDisplayModelOutput = true;
	// Split-screen is useful on a monitor but is actively misleading in a
	// headset: each eye must receive the same complete presentation mode.
	gDlssNrSplitView = false;
	if(mode > 0)
		gDlssNrEffectMode = 0;
	if(previousMode != mode){
		gDlssNrActiveMask = 0;
		gDlssNrStereoDisplayReady = false;
		for(int slot = 0; slot < SLOT_COUNT; slot++)
			memset(gEye[slot].nrHistoryValid, 0,
				sizeof(gEye[slot].nrHistoryValid));
	}
	if(mode > 0)
		LogDlssNrProfile("mode switch", -1);
	else
		WriteLog("[DLSS-NR profile] mode=0 DLAA BASELINE");
	WriteLog("[DLSS-NR] model profile mode=%d (%s), 1:1 neural work scale %s",
		gDlssNrMode, GetNeuralRenderingModeName(),
		GetNeuralRenderingRegionModeName());
}

int GetNeuralRenderingMode()
{
	return gDlssNrMode;
}

void SetNeuralRenderingPassCount(int passes)
{
	passes = passes < 1 ? 1 : (passes > MAX_NR_PASSES ? MAX_NR_PASSES : passes);
	if(gDlssNrPassCount == passes)
		return;
	gDlssNrPassCount = passes;
	gDlssNrActiveMask = 0;
	gDlssNrStereoDisplayReady = false;
	gDlssNrActiveLogged = false;
	for(int slot = 0; slot < SLOT_COUNT; slot++)
		memset(gEye[slot].nrHistoryValid, 0,
			sizeof(gEye[slot].nrHistoryValid));
	WriteLog("[DLSS-NR] sequential model passes=%d", gDlssNrPassCount);
}

int GetNeuralRenderingPassCount()
{
	return gDlssNrPassCount;
}

void SetNeuralRenderingRegionMode(int mode)
{
	mode = mode < 0 ? 0 : (mode > 3 ? 3 : mode);
	SetQualityMode(mode);
}

int GetNeuralRenderingRegionMode()
{
	return gDlssNrRegionMode;
}

const char *GetNeuralRenderingRegionModeName(int mode)
{
	static const char *names[] = { "FULL 100%", "QUALITY 67%",
		"BALANCED 58%", "PERFORMANCE 50%" };
	return names[mode >= 0 && mode < 4 ? mode : 0];
}

const char *GetNeuralRenderingRegionModeName()
{
	return GetNeuralRenderingRegionModeName(gDlssNrRegionMode);
}

bool ConsumeNeuralRenderingPassCountChanged()
{
	const bool changed = gDlssNrPassCountChangedByOverlay;
	gDlssNrPassCountChangedByOverlay = false;
	return changed;
}

void CycleNeuralRenderingMode()
{
	SetNeuralRenderingMode(gDlssNrMode+1);
}

const char *GetNeuralRenderingModeName()
{
	return GetDlssNrProfile().name;
}

const char *GetNeuralRenderingModeName(int mode)
{
	return gDlssNrProfiles[mode >= 0 && mode < 5 ? mode : 0].name;
}

void SetNeuralRenderingIntensityPercent(int percent)
{
	gDlssNrTuning.intensity =
		ClampTuningPercent(percent, 25, 250)/100.0f;
	gDlssNrTuningCustom = true;
}

int GetNeuralRenderingIntensityPercent()
{
	return StrengthToPercent(gDlssNrTuning.intensity);
}

void SetNeuralRenderingStructurePercent(int percent)
{
	gDlssNrTuning.localStructure =
		ClampTuningPercent(percent, 0, 250)/100.0f;
	gDlssNrTuningCustom = true;
}

int GetNeuralRenderingStructurePercent()
{
	return StrengthToPercent(gDlssNrTuning.localStructure);
}

void SetNeuralRenderingLocalTonePercent(int percent)
{
	gDlssNrTuning.localTone =
		ClampTuningPercent(percent, 0, 200)/100.0f;
	gDlssNrTuningCustom = true;
}

int GetNeuralRenderingLocalTonePercent()
{
	return StrengthToPercent(gDlssNrTuning.localTone);
}

void SetNeuralRenderingGlobalTonePercent(int percent)
{
	gDlssNrTuning.globalTone =
		ClampTuningPercent(percent, 0, 200)/100.0f;
	gDlssNrTuningCustom = true;
}

int GetNeuralRenderingGlobalTonePercent()
{
	return StrengthToPercent(gDlssNrTuning.globalTone);
}

void SetNeuralRenderingStyle(int style)
{
	gDlssNrTuning.style = style < 0 ? 0 : (style > 2 ? 2 : style);
	gDlssNrTuningCustom = true;
}

int GetNeuralRenderingStyle()
{
	return gDlssNrTuning.style;
}

void SetNeuralRenderingAutoMask(bool enabled)
{
	gDlssNrTuning.autoMask = enabled ? 1 : 0;
	gDlssNrTuningCustom = true;
}

bool IsNeuralRenderingAutoMaskEnabled()
{
	return gDlssNrTuning.autoMask != 0;
}

void ResetNeuralRenderingTuning()
{
	ApplyDlssNrProfileTuning();
	LogDlssNrProfile("tuning reset", -1);
}

bool IsNeuralRenderingTuningCustom()
{
	return gDlssNrTuningCustom;
}

void ProbeDlssNeuralRendering(const sl::AdapterInfo &adapter)
{
	gDlssNrReady = false;
	gDlssNrSetOptions = nil;
	const sl::Result supportResult =
		slIsFeatureSupported(kFeatureDLSS_NR, adapter);
	WriteLog("[DLSS-NR] adapter support: %s (%d)",
		ResultName(supportResult), (int)supportResult);

	bool loaded = false;
	sl::Result result = slIsFeatureLoaded(kFeatureDLSS_NR, loaded);
	WriteLog("[DLSS-NR] plugin loaded: %s (query: %s, %d)", loaded ? "yes" : "no",
		ResultName(result), (int)result);
	const bool loadedSuccessfully = result == sl::Result::eOk && loaded;

	sl::FeatureVersion version{};
	result = slGetFeatureVersion(kFeatureDLSS_NR, version);
	if(result == sl::Result::eOk)
		WriteLog("[DLSS-NR] version: SL %s, NGX %s", version.versionSL.toStr().c_str(),
			version.versionNGX.toStr().c_str());
	else
		WriteLog("[DLSS-NR] version query failed: %s (%d)", ResultName(result), (int)result);

	sl::FeatureRequirements requirements{};
	result = slGetFeatureRequirements(kFeatureDLSS_NR, requirements);
	if(result == sl::Result::eOk){
		WriteLog("[DLSS-NR] requirements: flags 0x%x, driver detected %s / required %s, "
			"OS detected %s / required %s, viewports %u, cpu threads %u",
			(uint32)requirements.flags,
			requirements.driverVersionDetected.toStr().c_str(),
			requirements.driverVersionRequired.toStr().c_str(),
			requirements.osVersionDetected.toStr().c_str(),
			requirements.osVersionRequired.toStr().c_str(),
			requirements.maxNumViewports, requirements.maxNumCPUThreads);
		for(uint32_t i = 0; i < requirements.numRequiredTags; i++)
			WriteLog("[DLSS-NR] required buffer tag %u of %u: %u", i+1,
				requirements.numRequiredTags, (uint32)requirements.requiredTags[i]);
	}else
		WriteLog("[DLSS-NR] requirements query failed: %s (%d)", ResultName(result), (int)result);

	void *setOptions = nil;
	if(supportResult == sl::Result::eOk && loadedSuccessfully){
		result = slGetFeatureFunction(kFeatureDLSS_NR,
			"slDLSSNRSetOptions", setOptions);
		if(result == sl::Result::eOk && setOptions){
			gDlssNrSetOptions =
				reinterpret_cast<PFun_slDLSSNRSetOptions*>(setOptions);
			gDlssNrReady = true;
			WriteLog("[DLSS-NR] slDLSSNRSetOptions imported; 2D evaluation ready");
		}else{
			WriteLog("[DLSS-NR] options import failed: %s (%d)",
				ResultName(result), (int)result);
		}
	}
}

// Returns 1 when the model wrote nrOutput, 0 on the creation frame (the first
// evaluate is deliberately deferred), and -1 on a terminal runtime failure.
int EvaluateDirectNr(int slot, int pass, EyeState &state,
	const EyeInput &input, ID3D12Resource *source,
	ID3D12Resource *destination, ID3D12GraphicsCommandList *list)
{
	if(!gDlssNrDirectReady || !gDlssNrCapabilityParams ||
	   !gDlssNrDirectCreate || !gDlssNrDirectEvaluate || !source ||
	   !destination || pass < 0 || pass >= MAX_NR_PASSES)
		return -1;

	const DlssNrProfile &profile = GetDlssNrProfile();
	if(!state.nrDirectFeature[pass]){
		wchar_t modelPath[MAX_PATH];
		swprintf(modelPath, MAX_PATH, L"%s\\nvngx_dlssnr.dll",
			gPluginDirectory);
		state.nrDirectFeature[pass] = gDlssNrDirectCreate(modelPath,
			gPluginDirectory, rw::d3d12::getDevice(), list,
			gDlssNrCapabilityParams, state.nrRegionWidth, state.nrRegionHeight,
			profile.preset, gDlssNrTuning.intensity, gDlssNrTuning.style,
			gDlssNrTuning.localStructure, gDlssNrTuning.localTone,
			gDlssNrTuning.globalTone, -1.0f, gDlssNrTuning.autoMask,
			profile.uiCorrection);
		if(!state.nrDirectFeature[pass]){
			const uint32 initResult = gDlssNrDirectLastInit ?
				(uint32)*gDlssNrDirectLastInit : 0;
			const uint32 createResult = gDlssNrDirectLastCreate ?
				(uint32)*gDlssNrDirectLastCreate : 0;
			WriteLog("[DLSS-NR direct] slot %d pass %d create failed: "
				"init 0x%08X, feature 18 create 0x%08X", slot, pass+1,
				initResult, createResult);
			SetStatus("DLSS 5 create failed at %ux%u (0x%08X)",
				state.nrRegionWidth, state.nrRegionHeight, createResult);
			return -1;
		}
		LogDlssNrProfile("feature create", slot);
		WriteLog("[DLSS-NR direct] slot %d pass %d feature 18 created at "
			"%ux%u; first evaluate deferred one frame", slot, pass+1,
			state.nrRegionWidth, state.nrRegionHeight);
		return 0;
	}

	const uint32 colorBaseX = 0;
	const uint32 colorBaseY = 0;
	const uint32 depthBaseX = input.sourceLeft;
	const uint32 depthBaseY = 0;
	const uint32 motionBaseX = 0;
	const uint32 motionBaseY = 0;
	const int result = gDlssNrDirectEvaluate(list, state.nrDirectFeature[pass],
		gDlssNrCapabilityParams, source,
		reinterpret_cast<ID3D12Resource*>(input.depth), state.motion,
		destination, state.nrRegionWidth, state.nrRegionHeight,
		state.nrRegionWidth, state.nrRegionHeight,
		colorBaseX, colorBaseY, depthBaseX, depthBaseY,
		motionBaseX, motionBaseY, 0,
		state.nrHistoryValid[pass] ? 0 : 1,
		gDlssNrTuning.intensity, gDlssNrTuning.style,
		gDlssNrTuning.localStructure, gDlssNrTuning.localTone,
		gDlssNrTuning.globalTone, -1.0f, gDlssNrTuning.autoMask,
		1.0f, 1.0f);
	if(result != 1){
		WriteLog("[DLSS-NR direct] slot %d pass %d evaluate failed: 0x%08X",
			slot, pass+1, (uint32)result);
		SetStatus("DLSS 5 evaluate failed at %ux%u (0x%08X)",
			state.nrRegionWidth, state.nrRegionHeight, (uint32)result);
		return -1;
	}

	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = destination;
	list->ResourceBarrier(1, &uavBarrier);
	state.nrHistoryValid[pass] = true;
	return 1;
}

void InitializeEarly()
{
	if(gInitialized)
		return;
	FILE *file = fopen("streamline_dlaa.log", "w");
	if(file)
		fclose(file);

	sl::Feature features[2] = { sl::kFeatureDLSS, sl::kFeatureDLSS };
	uint32_t featureCount = 1;
	sl::Preferences preferences{};
	preferences.showConsole = false;
	// Release builds surface every actionable Streamline call failure through
	// SetStatus below. Disable SDK chatter here so a repeated driver warning can
	// never perform synchronous fopen/fclose work on the render thread.
	preferences.logLevel = sl::LogLevel::eOff;
	preferences.logMessageCallback = nil;
	preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
		sl::PreferenceFlags::eDisableDebugText;
	// NGX permits custom engines without an NVIDIA-issued application ID when
	// they provide a stable GUID-like project ID and an engine version.
	preferences.engine = sl::EngineType::eCustom;
	preferences.engineVersion = "0.5.5";
	preferences.projectId = "4557d919-aaf5-4797-8baa-c2acc5950bf1";
	preferences.renderAPI = sl::RenderAPI::eD3D12;
	if(FindPluginDirectory()){
		preferences.pathsToPlugins = gPluginPaths;
		preferences.numPathsToPlugins = 1;
		// Drop a file named "sl_verbose" (no extension) next to the executable
		// to route Streamline's own verbose log into sl.log in the same folder.
		// Diagnostic aid for init hangs that never reach our own log lines.
		wchar_t marker[MAX_PATH];
		swprintf(marker, MAX_PATH, L"%s\\sl_verbose", gPluginDirectory);
		if(GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES){
			preferences.logLevel = sl::LogLevel::eVerbose;
			preferences.pathToLogsAndData = gPluginDirectory;
			WriteLog("[DLAA] Streamline verbose logging enabled (sl_verbose marker)");
		}
		// The DLSS-NR plugin ships with Streamline builds newer than the SDK we
		// compile against; its presence next to the executable is the opt-in
		// for the support probe.
		wchar_t nrPlugin[MAX_PATH];
		swprintf(nrPlugin, MAX_PATH, L"%s\\sl.dlss_nr.dll", gPluginDirectory);
		if(GetFileAttributesW(nrPlugin) != INVALID_FILE_ATTRIBUTES){
			if(gDlssNrEnabled)
				PatchDlssNrAdaSupportInMemory(nrPlugin);
			gDlssNrProbeRequested = true;
			features[featureCount++] = kFeatureDLSS_NR;
			// OTA remains disabled because this integration supplies and validates
			// the local plugin set before device creation.
			WriteLog("[DLSS-NR] sl.dlss_nr.dll present; feature %u probe requested",
				(uint32)kFeatureDLSS_NR);
		}
	}
	preferences.featuresToLoad = features;
	preferences.numFeaturesToLoad = featureCount;

	const sl::Result result = slInit(preferences);
	if(result != sl::Result::eOk){
		SetStatus("Streamline init failed: %s (%d)", ResultName(result), (int)result);
		return;
	}
	gInitialized = true;
	SetStatus("Streamline initialized; waiting for D3D12 device");
}

void AttachDevice()
{
	if(!gInitialized || gDeviceAttached)
		return;
	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device){
		SetStatus("Streamline initialized, but D3D12 device is unavailable");
		return;
	}
	// Beacon pair around the attach: a hang inside slSetD3DDevice (feature
	// plugin loading, NGX init, OTA) leaves the first line as the last entry.
	WriteLog("[DLAA] attaching D3D12 device to Streamline...");
	sl::Result result = slSetD3DDevice(device);
	WriteLog("[DLAA] slSetD3DDevice returned: %s (%d)", ResultName(result), (int)result);
	if(result != sl::Result::eOk){
		SetStatus("Streamline device attach failed: %s (%d)", ResultName(result), (int)result);
		return;
	}
	gDeviceAttached = true;
	LUID luid = device->GetAdapterLuid();
	sl::AdapterInfo adapter{};
	adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
	adapter.deviceLUIDSizeInBytes = sizeof(luid);
	WriteLog("[DLAA] querying DLSS adapter support...");
	result = slIsFeatureSupported(sl::kFeatureDLSS, adapter);
	gSupported = result == sl::Result::eOk;
	if(gSupported && !CreateMotionPipeline()){
		gSupported = false;
		SetStatus("DLAA camera motion pipeline creation failed");
	}else if(gSupported)
		SetStatus("DLSS/DLAA supported; Streamline camera motion ready");
	else
		SetStatus("DLSS/DLAA unavailable: %s (%d)", ResultName(result), (int)result);

	if(gDlssNrProbeRequested)
		ProbeDlssNeuralRendering(adapter);
	if(gDlssNrEnabled)
		InitializeDirectNrBackend(device);
}

bool BeginFrame(float jitterX, float jitterY)
{
	// Only expose the neural result to stereo after both independent feature
	// instances completed at least once. This prevents a one-eye activation
	// frame while the second feature is still being created.
	gDlssNrStereoDisplayReady = gDlssNrMode > 0 &&
		(gDlssNrActiveMask & ((1u << EYE_COUNT)-1u)) ==
		((1u << EYE_COUNT)-1u);
	gFrameReady = false;
	gLastEvaluationSucceeded = false;
	gSuccessfulEvaluationsThisFrame = 0;
	gFrameToken = nil;
	gJitterX = jitterX;
	gJitterY = jitterY;
	if(!gInitialized || !gDeviceAttached || !gSupported)
		return false;
	// Let Streamline generate the unique token.  A caller-owned temporal sample
	// counter can legally pause while jitter is disabled, but a frame token may
	// never be reused for a later rendered frame.
	const sl::Result result = slGetNewFrameToken(gFrameToken);
	if(result != sl::Result::eOk || !gFrameToken){
		SetStatus("DLAA frame token failed: %s (%d)", ResultName(result), (int)result);
		return false;
	}
	gFrameReady = true;
	TickFrameRateTelemetry();
	return true;
}

bool NrControlPressed(int virtualKey, uint32 index)
{
	const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
	const bool pressed = down && !gDlssNrControlKeys[index];
	gDlssNrControlKeys[index] = down;
	return pressed;
}

int GetNrViewMode()
{
	if(gDlssNrSplitView)
		return 0;
	return gDlssNrDisplayModelOutput ? 1 : 2;
}

void SetNrViewMode(int mode)
{
	mode = (mode%3+3)%3;
	gDlssNrSplitView = mode == 0;
	gDlssNrDisplayModelOutput = mode != 2;
}

void ProcessNrOverlayInput()
{
	if(!IsNeuralRenderingActive())
		return;
	const bool toggle = NrControlPressed(VK_F11, 0);
	const bool up = NrControlPressed(VK_UP, 1);
	const bool down = NrControlPressed(VK_DOWN, 2);
	const bool left = NrControlPressed(VK_LEFT, 3);
	const bool right = NrControlPressed(VK_RIGHT, 4);
	const bool reset = NrControlPressed(VK_F12, 5);
	if(toggle){
		gDlssNrOverlayVisible = !gDlssNrOverlayVisible;
		WriteLog("[DLSS-NR overlay] panel=%s",
			gDlssNrOverlayVisible ? "open" : "closed");
	}
	if(!gDlssNrOverlayVisible)
		return;
	if(reset){
		gDlssNrEffectMode = 1;
		gDlssNrOverlaySelection = 0;
		SetNrViewMode(0);
		SetNeuralRenderingPassCount(1);
		gDlssNrPassCountChangedByOverlay = true;
		WriteLog("[DLSS-NR overlay] reset: effect=strong-1.5x view=split passes=1");
		return;
	}
	if(up || down){
		gDlssNrOverlaySelection =
			(gDlssNrOverlaySelection+(down ? 1 : -1)+3)%3;
	}
	if(left || right){
		const int direction = right ? 1 : -1;
		if(gDlssNrOverlaySelection == 0){
			gDlssNrEffectMode =
				(gDlssNrEffectMode+direction+5)%5;
			WriteLog("[DLSS-NR overlay] effect mode=%d", gDlssNrEffectMode);
		}else if(gDlssNrOverlaySelection == 1){
			SetNrViewMode(GetNrViewMode()+direction);
			WriteLog("[DLSS-NR overlay] view mode=%d", GetNrViewMode());
		}else{
			SetNeuralRenderingPassCount(1+
				(GetNeuralRenderingPassCount()-1+direction+MAX_NR_PASSES)%MAX_NR_PASSES);
			gDlssNrPassCountChangedByOverlay = true;
		}
	}
}

bool EvaluateEye(int eye, const EyeInput &input, EyeOutput *output)
{
	if(output){
		output->color = nil;
		output->shaderResourceView = 0;
		output->width = output->height = 0;
	}
	if(!gFrameReady || !gFrameToken || !output || eye < 0 || eye >= SLOT_COUNT ||
	   !input.color || !input.depth || input.width == 0 || input.height == 0)
		return false;
	if(gDlssNrEnabled && !gDlssNrDirectReady &&
	   !gDlssNrDirectAttempted)
		InitializeDirectNrBackend(rw::d3d12::getDevice());
	// Process the flat-panel controls before resource validation because changing
	// the pass count may require the scratch target in this same frame.
	if(eye == kDesktopSlot)
		ProcessNrOverlayInput();
	if(!CreateEyeResources(eye, input.width, input.height,
	   input.outputWidth, input.outputHeight)){
		SetStatus("DLAA target allocation failed for eye %d (%ux%u)", eye,
			input.width, input.height);
		return false;
	}
	EyeState &state = gEye[eye];
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	ID3D12DescriptorHeap *heap = rw::d3d12::getShaderResourceHeap();
	if(!list || !heap)
		return false;
	Transition(list, state.motion, state.motionState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12DescriptorHeap *heaps[] = { heap };
	rw::d3d12::resetWorldDrawState();
	list->SetDescriptorHeaps(1, heaps);
	// Streamline's internal camera-motion pass samples depth from x=0 and cannot
	// address the right-eye half of our double-wide depth resource. Generate the
	// same current-to-previous pixel motion locally, including sourceLeft, and
	// tell DLAA that camera motion is already present in this buffer.
	if(state.historyValid){
		if(!GenerateCameraMotion(input, state, list, heap)){
			SetStatus("DLAA camera motion generation failed for eye %d", eye);
			state.historyValid = false;
			return false;
		}
	}else{
		const float zeroMotion[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		list->ClearUnorderedAccessViewFloat(state.motionUavGpu, state.motionUavCpu,
			state.motion, zeroMotion, 0, nil);
		D3D12_RESOURCE_BARRIER uavBarrier = {};
		uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		uavBarrier.UAV.pResource = state.motion;
		list->ResourceBarrier(1, &uavBarrier);
	}
	Transition(list, state.motion, state.motionState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(list, state.output, state.outputState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	sl::ViewportHandle viewport(eye);
	sl::Constants constants{};
	BuildConstants(input, state, constants);

	const uint32 shaderRead = (uint32)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	sl::Resource depth(sl::ResourceType::eTex2d, input.depth, shaderRead);
	sl::Resource motion(sl::ResourceType::eTex2d, state.motion, shaderRead);
	sl::Extent sourceExtent = { 0, input.sourceLeft, input.width, input.height };
	sl::Extent localExtent = { 0, 0, input.width, input.height };
	sl::Extent outputExtent = { 0, 0, state.outputWidth, state.outputHeight };
	ID3D12Resource *dlssInputResource =
		reinterpret_cast<ID3D12Resource*>(input.color);
	sl::Extent *dlssInputExtent = &sourceExtent;
	bool useNeuralInput = false;
	if(gDlssNrMode > 0 && state.nrEnabled && state.nrOutput &&
	   (gDlssNrDirectReady || gDlssNrReady) && !gDlssNrRuntimeFailed){
		ID3D12Resource *modelResource = nil;
		D3D12_RESOURCE_STATES *modelState = nil;
		int completedPasses = 0;
		bool nrFailed = !CopyNeuralColorInput(list,
			reinterpret_cast<ID3D12Resource*>(input.color), state.nrInput,
			state.nrInputState, input.sourceLeft, input.width, input.height);
		if(nrFailed)
			SetStatus("DLSS 5 color input invalid for eye %d: x=%u, %ux%u",
				eye, input.sourceLeft, input.width, input.height);
		for(int pass = 0; !nrFailed && pass < gDlssNrPassCount; pass++){
			ID3D12Resource *source = pass == 0 ?
				state.nrInput : modelResource;
			ID3D12Resource *destinationResource = (pass & 1) == 0 ?
				state.nrOutput : state.nrScratch;
			D3D12_RESOURCE_STATES *destinationState = (pass & 1) == 0 ?
				&state.nrOutputState : &state.nrScratchState;
			if(!source || !destinationResource || !destinationState){
				SetStatus("DLSS 5 target missing for eye %d pass %d", eye, pass+1);
				nrFailed = true;
				break;
			}
			if(pass > 0)
				Transition(list, source, *modelState,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destinationResource, *destinationState,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			bool passSucceeded = false;
			bool creationFrame = false;
			if(gDlssNrDirectReady){
				const int directResult = EvaluateDirectNr(eye, pass, state, input,
					source, destinationResource, list);
				passSucceeded = directResult > 0;
				creationFrame = directResult == 0;
				if(directResult < 0)
					nrFailed = true;
			}else{
				const sl::ViewportHandle nrViewport(NrViewportId(eye, pass));
				sl::Constants nrConstants = constants;
				nrConstants.reset = state.nrHistoryValid[pass] ?
					sl::Boolean::eFalse : sl::Boolean::eTrue;
				const sl::Result constantsResult = slSetConstants(nrConstants,
					*gFrameToken, nrViewport);
				if(constantsResult != sl::Result::eOk){
					if(gDlssNrEvaluationLogCount++ < 8)
						SetStatus("DLSS 5 pass %d constants failed: %s (%d)",
							pass+1, ResultName(constantsResult), (int)constantsResult);
					nrFailed = true;
					break;
				}
				DlssNrOptionsABI nrOptions{};
				FillDlssNrOptions(nrOptions);
				const sl::Result nrOptionsResult = gDlssNrSetOptions ?
					gDlssNrSetOptions(nrViewport, nrOptions) :
					sl::Result::eErrorMissingOrInvalidAPI;
				if(nrOptionsResult != sl::Result::eOk){
					if(gDlssNrEvaluationLogCount++ < 8)
						SetStatus("DLSS 5 pass %d live options failed: %s (%d)",
							pass+1, ResultName(nrOptionsResult), (int)nrOptionsResult);
					nrFailed = true;
					break;
				}
				state.nrOptionsSetMask |= 1u << pass;
				sl::Resource nrInput(sl::ResourceType::eTex2d, source,
					(uint32)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				sl::Resource nrDestination(sl::ResourceType::eTex2d,
					destinationResource,
					(uint32)D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				sl::Extent nrInputExtent = localExtent;
				sl::Extent nrOutputExtent = { 0, 0, state.nrRegionWidth,
					state.nrRegionHeight };
				sl::ResourceTag nrInputTag(&nrInput, kBufferTypeUpliftInputColor,
					sl::ResourceLifecycle::eValidUntilEvaluate, &nrInputExtent);
				sl::ResourceTag nrOutputTag(&nrDestination, kBufferTypeUpliftOutputColor,
					sl::ResourceLifecycle::eValidUntilEvaluate, &nrOutputExtent);
				sl::ResourceTag nrDepthTag(&depth, sl::kBufferTypeDepth,
					sl::ResourceLifecycle::eValidUntilEvaluate, &sourceExtent);
				sl::ResourceTag nrMotionTag(&motion, sl::kBufferTypeMotionVectors,
					sl::ResourceLifecycle::eValidUntilEvaluate, &localExtent);
				const sl::BaseStructure *nrInputs[] = {
					&nrViewport, &nrInputTag, &nrOutputTag, &nrDepthTag, &nrMotionTag
				};
				const sl::Result nrResult = slEvaluateFeature(kFeatureDLSS_NR,
					*gFrameToken, nrInputs, ARRAY_SIZE(nrInputs),
					reinterpret_cast<sl::CommandBuffer*>(list));
				passSucceeded = nrResult == sl::Result::eOk;
				if(!passSucceeded){
					if(gDlssNrEvaluationLogCount++ < 8)
						SetStatus("DLSS 5 pass %d evaluate failed: %s (%d)",
							pass+1, ResultName(nrResult), (int)nrResult);
					nrFailed = true;
				}else{
					D3D12_RESOURCE_BARRIER uavBarrier = {};
					uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					uavBarrier.UAV.pResource = destinationResource;
					list->ResourceBarrier(1, &uavBarrier);
					state.nrHistoryValid[pass] = true;
				}
			}

			if(!passSucceeded){
				if(creationFrame)
					break;
				nrFailed = true;
				break;
			}
			modelResource = destinationResource;
			modelState = destinationState;
			completedPasses++;
		}

		if(nrFailed){
			if(gDlssNrEvaluationLogCount++ < 8)
				WriteLog("[DLSS-NR] failed at %ux%u; using DLSS/DLAA baseline",
					state.nrRegionWidth, state.nrRegionHeight);
			gDlssNrRuntimeFailed = true;
			gDlssNrActiveMask = 0;
			gDlssNrStereoDisplayReady = false;
		}
		const bool nrSucceeded = !nrFailed &&
			completedPasses == gDlssNrPassCount;
		if(nrSucceeded){
			const uint32 slotBit = 1u << eye;
			const bool firstSuccessForSlot =
				(gDlssNrActiveMask & slotBit) == 0;
			gDlssNrActiveMask |= slotBit;
			const bool stereoPresentationReady =
				eye == kDesktopSlot || gDlssNrStereoDisplayReady;
			if(stereoPresentationReady){
				Transition(list, modelResource, *modelState,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				dlssInputResource = modelResource;
				dlssInputExtent = &localExtent;
				useNeuralInput = true;
			}
			if(firstSuccessForSlot)
				WriteLog("[DLSS-NR] slot %d %dx sequential evaluation active: "
					"work %ux%u -> output %ux%u, scale %s", eye,
					gDlssNrPassCount, state.nrRegionWidth, state.nrRegionHeight,
					state.outputWidth, state.outputHeight,
					GetNeuralRenderingRegionModeName(state.nrRegionMode));
			if(!gDlssNrRuntimeFailed && !gDlssNrActiveLogged){
				gDlssNrActiveLogged = true;
				WriteLog("[DLSS-NR] model evaluation active before DLSS reconstruction "
					"with %dx sequential passes", gDlssNrPassCount);
			}
		}else{
			gDlssNrActiveMask &= ~(1u << eye);
		}
	}

	// DLSS-NR is a same-resolution pre-process. DLSS SR (or 1:1 DLAA) is the
	// single reconstruction pass that expands its result to the headset size.
	if(state.dlssUsedNeuralInput != useNeuralInput)
		constants.reset = sl::Boolean::eTrue;
	// Submit once, after the final input and its history reset are known.
	// Streamline rejects changed constants for the same frame and viewport.
	sl::Result result = slSetConstants(constants, *gFrameToken, viewport);
	if(result != sl::Result::eOk){
		SetStatus("DLSS constants update failed for eye %d: %s (%d)", eye,
			ResultName(result), (int)result);
		state.historyValid = false;
		return false;
	}
	sl::Resource color(sl::ResourceType::eTex2d, dlssInputResource, shaderRead);
	sl::Resource destination(sl::ResourceType::eTex2d, state.output,
		(uint32)D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	sl::ResourceTag colorTag(&color, sl::kBufferTypeScalingInputColor,
		sl::ResourceLifecycle::eValidUntilEvaluate, dlssInputExtent);
	sl::ResourceTag depthTag(&depth, sl::kBufferTypeDepth,
		sl::ResourceLifecycle::eValidUntilEvaluate, &sourceExtent);
	sl::ResourceTag motionTag(&motion, sl::kBufferTypeMotionVectors,
		sl::ResourceLifecycle::eValidUntilEvaluate, &localExtent);
	sl::ResourceTag outputTag(&destination, sl::kBufferTypeScalingOutputColor,
		sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent);
	const sl::BaseStructure *inputs[] = {
		&viewport, &colorTag, &outputTag, &depthTag, &motionTag
	};
	result = slEvaluateFeature(sl::kFeatureDLSS, *gFrameToken, inputs,
		ARRAY_SIZE(inputs), reinterpret_cast<sl::CommandBuffer*>(list));
	if(result != sl::Result::eOk){
		if(gEvaluationLogCount++ < 16)
			SetStatus("DLSS/DLAA evaluate failed for eye %d: %s (%d)", eye,
				ResultName(result), (int)result);
		state.historyValid = false;
		return false;
	}
	if(useNeuralInput && !state.dlssUsedNeuralInput)
		WriteLog("[DLSS-NR] eye %d reconstructed local NR color %ux%u -> %ux%u",
			eye, state.width, state.height, state.outputWidth, state.outputHeight);
	state.dlssUsedNeuralInput = useNeuralInput;
	Transition(list, state.output, state.outputState,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	ID3D12Resource *finalColor = state.output;
	D3D12_GPU_DESCRIPTOR_HANDLE finalSrv = state.outputSrv;
	memcpy(state.previousView, input.view, sizeof(state.previousView));
	memcpy(state.previousProjection, input.projection, sizeof(state.previousProjection));
	state.historyValid = true;
	gSuccessfulEvaluationsThisFrame++;
	// The desktop slot evaluates alone in flat mode; the VR eyes report success
	// only when both completed this frame.
	if(eye == kDesktopSlot)
		gLastEvaluationSucceeded = true;
	else
		gLastEvaluationSucceeded = gSuccessfulEvaluationsThisFrame == EYE_COUNT;
	output->color = finalColor;
	output->shaderResourceView = finalSrv.ptr;
	output->width = state.outputWidth;
	output->height = state.outputHeight;
	return true;
}

void ResetHistory()
{
	for(int eye = 0; eye < SLOT_COUNT; eye++){
		gEye[eye].historyValid = false;
		memset(gEye[eye].nrHistoryValid, 0,
			sizeof(gEye[eye].nrHistoryValid));
	}
	gDlssNrActiveMask = 0;
	gDlssNrStereoDisplayReady = false;
	gFrameReady = false;
}

void ReleaseResources()
{
	for(int eye = 0; eye < SLOT_COUNT; eye++)
		ReleaseEye(eye);
	gFrameReady = false;
	gFrameToken = nil;
}

void Shutdown()
{
	ReleaseResources();
	ReleaseMotionPipeline();
	ShutdownDirectNrBackend();
	if(gInitialized){
		WriteLog("[DLAA] Shutting down Streamline");
		slShutdown();
	}
	RestoreDlssNrAdaSupportInMemory();
	gInitialized = false;
	gDeviceAttached = false;
	gSupported = false;
	gDlssNrReady = false;
	gDlssNrRuntimeFailed = false;
	gDlssNrActiveLogged = false;
	gDlssNrActiveMask = 0;
	gDlssNrStereoDisplayReady = false;
	gDlssNrSetOptions = nil;
	gLastEvaluationSucceeded = false;
	strcpy(gStatus, "Streamline shut down");
}

bool IsInitialized() { return gInitialized; }
bool IsSupported() { return gSupported; }
bool IsNeuralRenderingActive()
{
	return gDlssNrEnabled && gDlssNrMode > 0 &&
		(gDlssNrDirectReady || gDlssNrReady) &&
		!gDlssNrRuntimeFailed &&
		gDlssNrActiveMask != 0;
}
bool IsNeuralRenderingStereoActive()
{
	const uint32 stereoMask = (1u << EYE_COUNT)-1u;
	return IsNeuralRenderingActive() &&
		(gDlssNrActiveMask & stereoMask) == stereoMask &&
		gDlssNrStereoDisplayReady &&
		gEye[0].dlssUsedNeuralInput && gEye[1].dlssUsedNeuralInput;
}
bool HasNeuralRenderingFailed()
{
	return gDlssNrRuntimeFailed;
}
void SetNeuralRenderingOutputVisible(bool visible)
{
	if(gDlssNrDisplayModelOutput == visible && !gDlssNrSplitView)
		return;
	gDlssNrSplitView = false;
	gDlssNrDisplayModelOutput = visible;
	gDlssNrRuntimeFailed = false;
	gDlssNrActiveLogged = false;
	gDlssNrActiveMask = 0;
	gDlssNrStereoDisplayReady = false;
	gDlssNrEvaluationLogCount = 0;
	for(int slot = 0; slot < SLOT_COUNT; slot++){
		gEye[slot].historyValid = false;
		memset(gEye[slot].nrHistoryValid, 0,
			sizeof(gEye[slot].nrHistoryValid));
	}
	WriteLog("[DLSS-NR A/B] pipeline=%s; temporal history reset",
		visible ? "NR -> DLSS" : "DLSS/DLAA BASELINE");
}
bool IsNeuralRenderingOutputVisible()
{
	return gDlssNrEnabled &&
		(gDlssNrDisplayModelOutput || gDlssNrSplitView);
}
bool IsNeuralRenderingSplitView()
{
	return IsNeuralRenderingActive() && gDlssNrSplitView;
}
bool IsNeuralRenderingOverlayVisible()
{
	return IsNeuralRenderingActive() && gDlssNrOverlayVisible;
}
int GetNeuralRenderingOverlaySelection()
{
	return gDlssNrOverlaySelection;
}
const char *GetNeuralRenderingEffectName()
{
	static const char *names[] = {
		"RAW MODEL  1.00X",
		"STRONG  1.50X",
		"MAX  2.00X",
		"LUMA ONLY  1.50X",
		"CHANGE MAGNITUDE  20X"
	};
	return names[gDlssNrEffectMode >= 0 && gDlssNrEffectMode < 5 ?
		gDlssNrEffectMode : 0];
}
const char *GetNeuralRenderingViewName()
{
	static const char *names[] = {
		"SPLIT: DLAA LEFT | EFFECT RIGHT",
		"EFFECT FULLSCREEN",
		"DLAA BASELINE"
	};
	return names[GetNrViewMode()];
}
bool IsNeuralRenderingFloatTuningVerified()
{
	return gDlssNrFloatTuningVerified;
}
bool WasLastEvaluationSuccessful() { return gLastEvaluationSucceeded; }
const char *GetStatus() { return gStatus; }
}

#else

namespace Dlaa
{
void InitializeEarly() {}
void AttachDevice() {}
void SetQualityMode(int) {}
float GetQualityModeRenderRatio(int) { return 1.0f; }
void SetNeuralRenderingEnabled(bool) {}
bool IsNeuralRenderingEnabled() { return false; }
void SetNeuralRenderingMode(int) {}
int GetNeuralRenderingMode() { return 0; }
void SetNeuralRenderingPassCount(int) {}
int GetNeuralRenderingPassCount() { return 1; }
void SetNeuralRenderingRegionMode(int) {}
int GetNeuralRenderingRegionMode() { return 0; }
const char *GetNeuralRenderingRegionModeName() { return "FULL"; }
const char *GetNeuralRenderingRegionModeName(int) { return "FULL"; }
bool ConsumeNeuralRenderingPassCountChanged() { return false; }
void CycleNeuralRenderingMode() {}
const char *GetNeuralRenderingModeName() { return "DLAA BASELINE"; }
const char *GetNeuralRenderingModeName(int mode)
{
	switch(mode){
	case 1: return "NATURAL";
	case 2: return "DETAIL";
	case 3: return "CINEMATIC";
	case 4: return "MAX DETAIL";
	default: return "DLAA BASELINE";
	}
}
void SetNeuralRenderingIntensityPercent(int) {}
int GetNeuralRenderingIntensityPercent() { return 100; }
void SetNeuralRenderingStructurePercent(int) {}
int GetNeuralRenderingStructurePercent() { return 100; }
void SetNeuralRenderingLocalTonePercent(int) {}
int GetNeuralRenderingLocalTonePercent() { return 0; }
void SetNeuralRenderingGlobalTonePercent(int) {}
int GetNeuralRenderingGlobalTonePercent() { return 0; }
void SetNeuralRenderingStyle(int) {}
int GetNeuralRenderingStyle() { return 0; }
void SetNeuralRenderingAutoMask(bool) {}
bool IsNeuralRenderingAutoMaskEnabled() { return true; }
void ResetNeuralRenderingTuning() {}
bool IsNeuralRenderingTuningCustom() { return false; }
bool BeginFrame(float, float) { return false; }
bool EvaluateEye(int, const EyeInput&, EyeOutput*) { return false; }
void ResetHistory() {}
void ReleaseResources() {}
void Shutdown() {}
bool IsInitialized() { return false; }
bool IsSupported() { return false; }
bool IsNeuralRenderingActive() { return false; }
bool IsNeuralRenderingStereoActive() { return false; }
bool HasNeuralRenderingFailed() { return false; }
void SetNeuralRenderingOutputVisible(bool) {}
bool IsNeuralRenderingOutputVisible() { return false; }
bool IsNeuralRenderingSplitView() { return false; }
bool IsNeuralRenderingOverlayVisible() { return false; }
int GetNeuralRenderingOverlaySelection() { return 0; }
const char *GetNeuralRenderingEffectName() { return "OFF"; }
const char *GetNeuralRenderingViewName() { return "OFF"; }
bool IsNeuralRenderingFloatTuningVerified() { return false; }
bool WasLastEvaluationSuccessful() { return false; }
const char *GetStatus() { return "DLAA requires OpenXR and D3D12"; }
}

#endif
