// DLSS Neural Rendering caller-gate forwarder.
//
// Derived from Dagherbou/OptiScaler_DLSSNR commit
// 393e0706b950a0ff1498e9dcf66989a80de72f31 (GPL-3.0-or-later).
// The NVIDIA snippet accepts NGX calls only when the module that owns the
// caller address has "nvngx.dll" in its path.  This small, source-built DLL
// owns those calls and intentionally builds as nvngx.dll_dlssnr.dll.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

namespace
{
constexpr int VT_SET_ULL = 0;
constexpr int VT_SET_UINT = 3;
// NVIDIA driver 616.56's capability block keeps the float overload here,
// rather than in the public header's nominal slot 1. The host verifies it with
// the corresponding raw getter before reporting float tuning as available.
int gFloatSlot = 6;

using PFunSetULL = void(__thiscall*)(void*, const char*, unsigned long long);
using PFunSetFloat = void(__thiscall*)(void*, const char*, float);
using PFunSetUInt = void(__thiscall*)(void*, const char*, unsigned int);
using PFunGetFloat = int(__thiscall*)(void*, const char*, float*);

void SetUInt(void *params, const char *name, unsigned int value)
{
	void **vtable = *reinterpret_cast<void***>(params);
	reinterpret_cast<PFunSetUInt>(vtable[VT_SET_UINT])(params, name, value);
}

void SetFloat(void *params, const char *name, float value)
{
	void **vtable = *reinterpret_cast<void***>(params);
	reinterpret_cast<PFunSetFloat>(vtable[gFloatSlot])(params, name, value);
}

void SetResource(void *params, const char *name, ID3D12Resource *value)
{
	void **vtable = *reinterpret_cast<void***>(params);
	reinterpret_cast<PFunSetULL>(vtable[VT_SET_ULL])(
		params, name, reinterpret_cast<unsigned long long>(value));
}

using PFunInit = int(__cdecl*)(unsigned long long, const wchar_t*,
	ID3D12Device*, int, const void*);
using PFunCreate = int(__cdecl*)(ID3D12GraphicsCommandList*, int,
	const void*, void**);
using PFunEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, const void*,
	const void*, void*);
using PFunRelease = int(__cdecl*)(void*);

struct Model
{
	HMODULE module;
	PFunInit init;
	PFunCreate create;
	PFunEvaluate evaluate;
	PFunRelease release;
	bool initialized;
};

Model gModel = {};

bool LoadModel(const wchar_t *path)
{
	if(gModel.module)
		return gModel.create && gModel.evaluate;
	gModel.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if(!gModel.module)
		return false;
	gModel.init = reinterpret_cast<PFunInit>(GetProcAddress(
		gModel.module, "NVSDK_NGX_D3D12_Init_Ext"));
	gModel.create = reinterpret_cast<PFunCreate>(GetProcAddress(
		gModel.module, "NVSDK_NGX_D3D12_CreateFeature"));
	gModel.evaluate = reinterpret_cast<PFunEvaluate>(GetProcAddress(
		gModel.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
	gModel.release = reinterpret_cast<PFunRelease>(GetProcAddress(
		gModel.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
	return gModel.create && gModel.evaluate;
}
}

extern "C"
{
__declspec(dllexport) int dlssnr_call_last_init = 0;
__declspec(dllexport) int dlssnr_call_last_create = 0;

__declspec(dllexport) void dlssnr_call_set_float_slot(int slot)
{
	if(slot >= 0 && slot < 8)
		gFloatSlot = slot;
}

__declspec(dllexport) void dlssnr_call_probe_float(void *params,
	const char *name, float value, int slot)
{
	if(!params || slot < 0 || slot >= 8)
		return;
	void **vtable = *reinterpret_cast<void***>(params);
	reinterpret_cast<PFunSetFloat>(vtable[slot])(params, name, value);
}

__declspec(dllexport) int dlssnr_call_get_float(void *params,
	const char *name, float *value, int slot)
{
	if(!params || !value || slot < 8 || slot >= 24)
		return 0;
	void **vtable = *reinterpret_cast<void***>(params);
	return reinterpret_cast<PFunGetFloat>(vtable[slot])(params, name, value);
}

__declspec(dllexport) void *dlssnr_call_create(const wchar_t *modelPath,
	const wchar_t *dataPath, ID3D12Device *device,
	ID3D12GraphicsCommandList *commands, void *capabilityParams,
	unsigned int width, unsigned int height, int preset, float intensity,
	int style, float localStructure, float localTone, float globalTone,
	float skinStructure, int autoMask, int uiCorrection)
{
	if(!LoadModel(modelPath) || !capabilityParams)
		return nullptr;
	if(!gModel.initialized && gModel.init){
		dlssnr_call_last_init = gModel.init(0x24480451ull, dataPath,
			device, 0x0000015, capabilityParams);
		gModel.initialized = dlssnr_call_last_init == 1;
		if(!gModel.initialized)
			return nullptr;
	}

	SetUInt(capabilityParams, "DLSSNR.Enabled", 1);
	SetUInt(capabilityParams, "DLSSNR.Width", width);
	SetUInt(capabilityParams, "DLSSNR.Height", height);
	SetUInt(capabilityParams, "CreationNodeMask", 1);
	SetUInt(capabilityParams, "VisibilityNodeMask", 1);
	SetUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", preset);
	SetFloat(capabilityParams, "DLSSNR.Intensity", intensity);
	SetUInt(capabilityParams, "DLSSNR.Style", style);
	SetFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
	SetFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
	SetFloat(capabilityParams, "DLSSNR.GlobalToneStrength", globalTone);
	SetFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
	SetUInt(capabilityParams, "DLSSNR.UseAutoMask", autoMask);
	SetUInt(capabilityParams, "DLSSNR.UICorrection", uiCorrection);

	void *handle = nullptr;
	dlssnr_call_last_create = gModel.create(commands, 18,
		capabilityParams, &handle);
	return handle;
}

__declspec(dllexport) int dlssnr_call_evaluate(
	ID3D12GraphicsCommandList *commands, void *feature,
	void *capabilityParams, ID3D12Resource *color, ID3D12Resource *depth,
	ID3D12Resource *motion, ID3D12Resource *output, unsigned int width,
	unsigned int height, unsigned int guideWidth, unsigned int guideHeight,
	unsigned int colorBaseX, unsigned int colorBaseY,
	unsigned int depthBaseX, unsigned int depthBaseY,
	unsigned int motionBaseX, unsigned int motionBaseY,
	int depthInverted, int reset, float intensity, int style,
	float localStructure, float localTone, float globalTone,
	float skinStructure, int autoMask, float motionScaleX, float motionScaleY)
{
	if(!feature || !capabilityParams || !gModel.evaluate)
		return 0;
	SetResource(capabilityParams, "DLSSNR.Color", color);
	SetResource(capabilityParams, "DLSSNR.Depth", depth);
	SetResource(capabilityParams, "DLSSNR.MVec", motion);
	SetResource(capabilityParams, "DLSSNR.Output", output);
	SetUInt(capabilityParams, "DLSSNR.Enabled", 1);
	SetUInt(capabilityParams, "DLSSNR.Width", width);
	SetUInt(capabilityParams, "DLSSNR.Height", height);
	SetUInt(capabilityParams, "DLSSNR.DepthInverted", depthInverted);
	SetUInt(capabilityParams, "DLSSNR.Reset", reset);

	SetUInt(capabilityParams, "DLSSNR.ColorSubrectBaseX", colorBaseX);
	SetUInt(capabilityParams, "DLSSNR.ColorSubrectBaseY", colorBaseY);
	SetUInt(capabilityParams, "DLSSNR.ColorSubrectWidth", width);
	SetUInt(capabilityParams, "DLSSNR.ColorSubrectHeight", height);
	SetUInt(capabilityParams, "DLSSNR.OutputSubrectBaseX", 0);
	SetUInt(capabilityParams, "DLSSNR.OutputSubrectBaseY", 0);
	SetUInt(capabilityParams, "DLSSNR.OutputSubrectWidth", width);
	SetUInt(capabilityParams, "DLSSNR.OutputSubrectHeight", height);
	const char *guides[] = { "Depth", "MVec" };
	for(const char *guide : guides){
		char key[64];
		const bool isDepth = guide[0] == 'D';
		const unsigned int baseX = isDepth ? depthBaseX : motionBaseX;
		const unsigned int baseY = isDepth ? depthBaseY : motionBaseY;
		wsprintfA(key, "DLSSNR.%sSubrectBaseX", guide); SetUInt(capabilityParams, key, baseX);
		wsprintfA(key, "DLSSNR.%sSubrectBaseY", guide); SetUInt(capabilityParams, key, baseY);
		wsprintfA(key, "DLSSNR.%sSubrectWidth", guide); SetUInt(capabilityParams, key, guideWidth);
		wsprintfA(key, "DLSSNR.%sSubrectHeight", guide); SetUInt(capabilityParams, key, guideHeight);
	}
	SetFloat(capabilityParams, "DLSSNR.MVecScaleX", motionScaleX);
	SetFloat(capabilityParams, "DLSSNR.MVecScaleY", motionScaleY);
	SetFloat(capabilityParams, "DLSSNR.Intensity", intensity);
	SetUInt(capabilityParams, "DLSSNR.Style", style);
	SetFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
	SetFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
	SetFloat(capabilityParams, "DLSSNR.GlobalToneStrength", globalTone);
	SetFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
	SetUInt(capabilityParams, "DLSSNR.UseAutoMask", autoMask);

	// A direct return may be tail-call optimized into a jump, which would make
	// the model see the host executable as its caller and reject it.
	volatile int result = gModel.evaluate(commands, feature,
		capabilityParams, nullptr);
	return result;
}

__declspec(dllexport) void dlssnr_call_release(void *feature)
{
	if(feature && gModel.release){
		volatile int result = gModel.release(feature);
		(void)result;
	}
}
}
