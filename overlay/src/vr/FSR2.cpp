#include "common.h"
#include "FSR2.h"

#if defined(GTA_VR_OPENXR) && defined(RW_D3D12)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sl_matrix_helpers.h>
#include <ffx_fsr2.h>
#include <dx12/ffx_fsr2_dx12.h>

#include "../../vendor/librw/src/d3d12/rwd3d12impl.h"

namespace Fsr2
{
namespace
{
enum { EYE_COUNT = 2 };

struct EyeState
{
	FfxFsr2Context context;
	void *scratch;
	bool contextCreated;
	ID3D12Resource *inputColor;
	ID3D12Resource *localDepth;
	ID3D12Resource *motion;
	ID3D12Resource *output;
	D3D12_CPU_DESCRIPTOR_HANDLE depthUavCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE depthUavGpu;
	D3D12_CPU_DESCRIPTOR_HANDLE motionUavCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE motionUavGpu;
	D3D12_GPU_DESCRIPTOR_HANDLE outputSrv;
	uint32 depthUavIndex;
	uint32 motionUavIndex;
	uint32 outputSrvIndex;
	D3D12_RESOURCE_STATES inputColorState;
	D3D12_RESOURCE_STATES localDepthState;
	D3D12_RESOURCE_STATES motionState;
	D3D12_RESOURCE_STATES outputState;
	uint32 width;
	uint32 height;
	DXGI_FORMAT inputColorFormat;
	bool historyValid;
	bool resetPending;
	float previousView[16];
	float previousProjection[16];

	EyeState() : scratch(nil), contextCreated(false), inputColor(nil),
		localDepth(nil), motion(nil), output(nil), depthUavIndex(UINT32_MAX),
		motionUavIndex(UINT32_MAX), outputSrvIndex(UINT32_MAX),
		inputColorState(D3D12_RESOURCE_STATE_COMMON),
		localDepthState(D3D12_RESOURCE_STATE_COMMON),
		motionState(D3D12_RESOURCE_STATE_COMMON),
		outputState(D3D12_RESOURCE_STATE_COMMON), width(0), height(0),
		inputColorFormat(DXGI_FORMAT_UNKNOWN), historyValid(false),
		resetPending(true)
	{
		memset(&context, 0, sizeof(context));
		depthUavCpu.ptr = depthUavGpu.ptr = 0;
		motionUavCpu.ptr = motionUavGpu.ptr = 0;
		outputSrv.ptr = 0;
		memset(previousView, 0, sizeof(previousView));
		memset(previousProjection, 0, sizeof(previousProjection));
	}
};

bool gInitialized;
bool gDeviceAttached;
bool gSupported;
bool gFrameReady;
bool gLastEvaluationSucceeded;
bool gRuntimeFailed;
float gJitterX;
float gJitterY;
float gFrameTimeMs = 11.111f;
LARGE_INTEGER gPreviousFrameCounter;
LARGE_INTEGER gCounterFrequency;
char gStatus[192] = "FSR2 not initialized";
EyeState gEye[EYE_COUNT];
ID3D12RootSignature *gDepthMotionRootSignature;
ID3D12PipelineState *gDepthMotionPipeline;

void WriteLog(const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message)-1] = '\0';
	FILE *file = fopen("fsr2.log", "a");
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
	WriteLog("[FSR2] %s", gStatus);
}

void FsrMessage(FfxFsr2MsgType type, const wchar_t *message)
{
	char text[1536] = {};
	if(message)
		WideCharToMultiByte(CP_UTF8, 0, message, -1, text,
			sizeof(text)-1, nil, nil);
	WriteLog("[FSR2/%s] %s",
		type == FFX_FSR2_MESSAGE_TYPE_ERROR ? "ERROR" : "WARN", text);
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
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	D3D12_HEAP_PROPERTIES heap = DefaultHeap();
	return SUCCEEDED(device->CreateCommittedResource(&heap,
		D3D12_HEAP_FLAG_NONE, &desc, initialState, nil,
		IID_PPV_ARGS(resource)));
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

bool CreateDepthMotionPipeline()
{
	if(gDepthMotionRootSignature && gDepthMotionPipeline)
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
		" uint sourceLeft; uint width; uint height; uint historyValid; };"
		"Texture2D<float> sourceDepth : register(t0);"
		"RWTexture2D<float> localDepth : register(u0);"
		"RWTexture2D<float2> motionOutput : register(u1);"
		"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
		" if(id.x >= width || id.y >= height) return;"
		" float depth = sourceDepth.Load(int3(id.x+sourceLeft,id.y,0));"
		" localDepth[id.xy] = depth;"
		" if(historyValid == 0) { motionOutput[id.xy]=0.0; return; }"
		" float2 pixel = float2(id.xy)+0.5;"
		" float2 currentPixel = pixel-jitterPixels;"
		" float2 currentNdc = float2(currentPixel.x*2.0*inverseSize.x-1.0,"
		"  1.0-currentPixel.y*2.0*inverseSize.y);"
		" float4 currentView = mul(float4(currentNdc,depth,1.0),"
		"  currentClipToView);"
		" if(abs(currentView.w) < 1e-6) { motionOutput[id.xy]=0.0; return; }"
		" currentView /= currentView.w;"
		" float4 previousView = mul(currentView,currentViewToPreviousView);"
		" float4 previousClip = mul(previousView,previousViewToClip);"
		" if(previousClip.w <= 1e-6) { motionOutput[id.xy]=0.0; return; }"
		" float2 previousNdc = previousClip.xy/previousClip.w;"
		" float2 previousPixel = float2((previousNdc.x+1.0)*0.5*width,"
		"  (1.0-previousNdc.y)*0.5*height);"
		" motionOutput[id.xy] = clamp(previousPixel-currentPixel,"
		"  float2(-32768.0,-32768.0),float2(32768.0,32768.0)); }";
	ID3DBlob *shader = nil;
	ID3DBlob *errors = nil;
	HRESULT result = D3DCompile(shaderSource, strlen(shaderSource),
		"vice_city_vr_fsr2_depth_motion", nil, nil, "main", "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
	if(FAILED(result)){
		if(errors)
			WriteLog("[FSR2] Depth/motion shader error: %s",
				(const char*)errors->GetBufferPointer());
		if(errors) errors->Release();
		if(shader) shader->Release();
		return false;
	}
	if(errors) errors->Release();

	D3D12_DESCRIPTOR_RANGE ranges[3] = {};
	for(uint32 index = 0; index < ARRAY_SIZE(ranges); index++){
		ranges[index].RangeType = index == 0 ?
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV :
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[index].NumDescriptors = 1;
		ranges[index].BaseShaderRegister = index == 0 ? 0 : index-1;
		ranges[index].OffsetInDescriptorsFromTableStart = 0;
	}
	D3D12_ROOT_PARAMETER parameters[4] = {};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.ShaderRegister = 0;
	parameters[0].Constants.Num32BitValues = 56;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	for(uint32 index = 0; index < ARRAY_SIZE(ranges); index++){
		parameters[index+1].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[index+1].DescriptorTable.NumDescriptorRanges = 1;
		parameters[index+1].DescriptorTable.pDescriptorRanges =
			&ranges[index];
		parameters[index+1].ShaderVisibility =
			D3D12_SHADER_VISIBILITY_ALL;
	}
	D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
	rootDesc.NumParameters = ARRAY_SIZE(parameters);
	rootDesc.pParameters = parameters;
	ID3DBlob *serialized = nil;
	ID3DBlob *rootErrors = nil;
	result = D3D12SerializeRootSignature(&rootDesc,
		D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
	if(FAILED(result) || !serialized){
		if(rootErrors)
			WriteLog("[FSR2] Depth/motion root signature error: %s",
				(const char*)rootErrors->GetBufferPointer());
		if(rootErrors) rootErrors->Release();
		if(serialized) serialized->Release();
		shader->Release();
		return false;
	}
	if(rootErrors) rootErrors->Release();
	result = device->CreateRootSignature(0, serialized->GetBufferPointer(),
		serialized->GetBufferSize(),
		IID_PPV_ARGS(&gDepthMotionRootSignature));
	serialized->Release();
	if(FAILED(result)){
		shader->Release();
		return false;
	}
	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
	pipelineDesc.pRootSignature = gDepthMotionRootSignature;
	pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
	pipelineDesc.CS.BytecodeLength = shader->GetBufferSize();
	result = device->CreateComputePipelineState(&pipelineDesc,
		IID_PPV_ARGS(&gDepthMotionPipeline));
	shader->Release();
	if(FAILED(result)){
		gDepthMotionRootSignature->Release();
		gDepthMotionRootSignature = nil;
		return false;
	}
	gDepthMotionRootSignature->SetName(
		L"Vice City VR FSR2 depth/motion root signature");
	gDepthMotionPipeline->SetName(
		L"Vice City VR FSR2 depth/motion pipeline");
	return true;
}

void ReleaseDepthMotionPipeline()
{
	if(gDepthMotionPipeline)
		rw::d3d12::deferRelease(gDepthMotionPipeline);
	if(gDepthMotionRootSignature)
		rw::d3d12::deferRelease(gDepthMotionRootSignature);
	gDepthMotionPipeline = nil;
	gDepthMotionRootSignature = nil;
}

void ReleaseEye(int eye)
{
	if(eye < 0 || eye >= EYE_COUNT)
		return;
	EyeState &state = gEye[eye];
	if(state.contextCreated){
		try {
			ffxFsr2ContextDestroy(&state.context);
		} catch(...) {
			WriteLog("[FSR2] Context destroy raised an exception for eye %d",
				eye);
		}
	}
	if(state.scratch)
		free(state.scratch);
	if(state.inputColor)
		rw::d3d12::deferRelease(state.inputColor);
	if(state.localDepth)
		rw::d3d12::deferRelease(state.localDepth);
	if(state.motion)
		rw::d3d12::deferRelease(state.motion);
	if(state.output)
		rw::d3d12::deferRelease(state.output);
	if(state.depthUavIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.depthUavIndex,
			UINT32_MAX, UINT32_MAX);
	if(state.motionUavIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.motionUavIndex,
			UINT32_MAX, UINT32_MAX);
	if(state.outputSrvIndex != UINT32_MAX)
		rw::d3d12::deferDescriptorRelease(state.outputSrvIndex,
			UINT32_MAX, UINT32_MAX);
	state = EyeState();
}

bool CreateEyeResources(int eye, const EyeInput &input)
{
	if(eye < 0 || eye >= EYE_COUNT || !input.color ||
	   input.width == 0 || input.height == 0)
		return false;
	// FSR2 keeps several full-resolution history surfaces for each eye. Very
	// high supersampling values can otherwise overcommit VRAM and remove the
	// D3D12 device before the SDK has a chance to report an allocation error.
	const uint64 pixelCount = (uint64)input.width*(uint64)input.height;
	if(input.width > 8192 || input.height > 8192 ||
	   pixelCount > 20ull*1024ull*1024ull){
		SetStatus("FSR2 safe memory limit exceeded for eye %d (%ux%u); reduce render scale",
			eye, input.width, input.height);
		return false;
	}
	EyeState &state = gEye[eye];
	ID3D12Resource *sourceColor = (ID3D12Resource*)input.color;
	const DXGI_FORMAT sourceFormat = sourceColor->GetDesc().Format;
	if(state.contextCreated && state.inputColor && state.localDepth &&
	   state.motion && state.output && state.width == input.width &&
	   state.height == input.height &&
	   state.inputColorFormat == sourceFormat)
		return true;
	// Stereo target replacement releases FSR2 before changing dimensions.
	// Refuse an unsafe live resize rather than destroying history resources
	// still referenced by the active command queue.
	if(state.contextCreated || state.inputColor){
		SetStatus("FSR2 eye %d changed size without a synchronized release",
			eye);
		return false;
	}

	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device)
		return false;
	if(!CreateTexture(device, input.width, input.height, sourceFormat,
	   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
	   &state.inputColor) ||
	   !CreateTexture(device, input.width, input.height,
	   DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.localDepth) ||
	   !CreateTexture(device, input.width, input.height,
	   DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.motion) ||
	   !CreateTexture(device, input.width, input.height,
	   DXGI_FORMAT_R8G8B8A8_UNORM,
	   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &state.output)){
		ReleaseEye(eye);
		return false;
	}
	state.inputColorState = D3D12_RESOURCE_STATE_COPY_DEST;
	state.localDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.motionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	state.width = input.width;
	state.height = input.height;
	state.inputColorFormat = sourceFormat;

	D3D12_CPU_DESCRIPTOR_HANDLE outputSrvCpu = {};
	if(!rw::d3d12::allocateShaderResourceDescriptor(&state.depthUavCpu,
	   &state.depthUavGpu, &state.depthUavIndex) ||
	   !rw::d3d12::allocateShaderResourceDescriptor(&state.motionUavCpu,
	   &state.motionUavGpu, &state.motionUavIndex) ||
	   !rw::d3d12::allocateShaderResourceDescriptor(&outputSrvCpu,
	   &state.outputSrv, &state.outputSrvIndex)){
		ReleaseEye(eye);
		return false;
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC depthUav = {};
	depthUav.Format = DXGI_FORMAT_R32_FLOAT;
	depthUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(state.localDepth, nil, &depthUav,
		state.depthUavCpu);
	D3D12_UNORDERED_ACCESS_VIEW_DESC motionUav = {};
	motionUav.Format = DXGI_FORMAT_R16G16_FLOAT;
	motionUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(state.motion, nil, &motionUav,
		state.motionUavCpu);
	D3D12_SHADER_RESOURCE_VIEW_DESC outputSrv = {};
	outputSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	outputSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	outputSrv.Shader4ComponentMapping =
		D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	outputSrv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(state.output, &outputSrv, outputSrvCpu);

	wchar_t name[80];
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR FSR2 input eye %d", eye);
	state.inputColor->SetName(name);
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR FSR2 depth eye %d", eye);
	state.localDepth->SetName(name);
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR FSR2 motion eye %d", eye);
	state.motion->SetName(name);
	swprintf(name, ARRAY_SIZE(name), L"Vice City VR FSR2 output eye %d", eye);
	state.output->SetName(name);

	const size_t scratchSize = ffxFsr2GetScratchMemorySizeDX12();
	state.scratch = malloc(scratchSize);
	if(!state.scratch){
		ReleaseEye(eye);
		return false;
	}
	FfxFsr2ContextDescription description = {};
	FfxErrorCode error = ffxFsr2GetInterfaceDX12(&description.callbacks,
		device, state.scratch, scratchSize);
	if(error != FFX_OK){
		SetStatus("FSR2 DX12 interface failed for eye %d (%d)", eye,
			(int)error);
		ReleaseEye(eye);
		return false;
	}
	description.device = ffxGetDeviceDX12(device);
	description.maxRenderSize.width = input.width;
	description.maxRenderSize.height = input.height;
	description.displaySize.width = input.width;
	description.displaySize.height = input.height;
	description.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE;
#ifdef _DEBUG
	description.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
	description.fpMessage = FsrMessage;
#else
	description.fpMessage = nil;
#endif
	try {
		error = ffxFsr2ContextCreate(&state.context, &description);
	} catch(...) {
		error = FFX_ERROR_BACKEND_API_ERROR;
	}
	if(error != FFX_OK){
		SetStatus("FSR2 context creation failed for eye %d (%d)", eye,
			(int)error);
		// A failed ContextCreate can already have created backend resources
		// through the callbacks, and ReleaseEye skips the destroy because
		// contextCreated never went true. The context memory was zeroed, so
		// a defensive destroy only releases whatever the failed create
		// actually made.
		try {
			ffxFsr2ContextDestroy(&state.context);
		} catch(...) {
			WriteLog("[FSR2] Destroy of failed context raised for eye %d",
				eye);
		}
		ReleaseEye(eye);
		return false;
	}
	state.contextCreated = true;
	state.resetPending = true;
	return true;
}

void CopyMatrix(sl::float4x4 &destination, const float source[16])
{
	memcpy(&destination[0].x, source, 16*sizeof(float));
}

void SetIdentity(float matrix[16])
{
	static const float identity[16] = {
		1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
	};
	memcpy(matrix, identity, sizeof(identity));
}

bool GenerateDepthAndMotion(const EyeInput &input, EyeState &state,
	ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *heap)
{
	if(!gDepthMotionRootSignature || !gDepthMotionPipeline || !list ||
	   !heap || input.depthShaderResourceView == 0)
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
		uint32 historyValid;
	} constants = {};
	static_assert(sizeof(MotionConstants) == 56*sizeof(uint32),
		"FSR2 depth/motion constants must match HLSL layout");
	if(state.historyValid){
		sl::float4x4 currentView, currentProjection, currentClipToView;
		sl::float4x4 currentCameraToWorld, previousView;
		sl::float4x4 previousProjection, previousCameraToWorld;
		sl::float4x4 currentViewToPreviousView;
		CopyMatrix(currentView, input.view);
		CopyMatrix(currentProjection, input.projection);
		CopyMatrix(previousView, state.previousView);
		CopyMatrix(previousProjection, state.previousProjection);
		sl::matrixFullInvert(currentClipToView, currentProjection);
		sl::matrixFullInvert(currentCameraToWorld, currentView);
		sl::matrixFullInvert(previousCameraToWorld, previousView);
		sl::calcCameraToPrevCamera(currentViewToPreviousView,
			currentCameraToWorld, previousCameraToWorld);
		memcpy(constants.currentClipToView, &currentClipToView[0].x,
			sizeof(constants.currentClipToView));
		memcpy(constants.currentViewToPreviousView,
			&currentViewToPreviousView[0].x,
			sizeof(constants.currentViewToPreviousView));
		memcpy(constants.previousViewToClip, &previousProjection[0].x,
			sizeof(constants.previousViewToClip));
		constants.historyValid = 1;
	}else{
		SetIdentity(constants.currentClipToView);
		SetIdentity(constants.currentViewToPreviousView);
		SetIdentity(constants.previousViewToClip);
	}
	constants.jitterPixels[0] = gJitterX;
	constants.jitterPixels[1] = gJitterY;
	constants.inverseSize[0] = 1.0f/(float)input.width;
	constants.inverseSize[1] = 1.0f/(float)input.height;
	constants.sourceLeft = input.sourceLeft;
	constants.width = input.width;
	constants.height = input.height;

	Transition(list, state.localDepth, state.localDepthState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(list, state.motion, state.motionState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12DescriptorHeap *heaps[] = { heap };
	rw::d3d12::resetWorldDrawState();
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(gDepthMotionRootSignature);
	list->SetPipelineState(gDepthMotionPipeline);
	list->SetComputeRoot32BitConstants(0, 56, &constants, 0);
	D3D12_GPU_DESCRIPTOR_HANDLE depthView = {
		input.depthShaderResourceView
	};
	list->SetComputeRootDescriptorTable(1, depthView);
	list->SetComputeRootDescriptorTable(2, state.depthUavGpu);
	list->SetComputeRootDescriptorTable(3, state.motionUavGpu);
	list->Dispatch((input.width+7)/8, (input.height+7)/8, 1);
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	for(uint32 index = 0; index < ARRAY_SIZE(barriers); index++)
		barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barriers[0].UAV.pResource = state.localDepth;
	barriers[1].UAV.pResource = state.motion;
	list->ResourceBarrier(ARRAY_SIZE(barriers), barriers);
	Transition(list, state.localDepth, state.localDepthState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(list, state.motion, state.motionState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	return true;
}

bool EvaluateEye(int eye, const EyeInput &input, EyeOutput *output)
{
	if(output){
		output->color = nil;
		output->shaderResourceView = 0;
		output->width = output->height = 0;
	}
	if(!gFrameReady || !output || eye < 0 || eye >= EYE_COUNT ||
	   !input.color || !input.depth || input.width == 0 ||
	   input.height == 0)
		return false;
	if(!CreateEyeResources(eye, input))
		return false;
	EyeState &state = gEye[eye];
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	ID3D12DescriptorHeap *heap = rw::d3d12::getShaderResourceHeap();
	if(!list || !heap)
		return false;

	Transition(list, state.inputColor, state.inputColorState,
		D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = state.inputColor;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = (ID3D12Resource*)input.color;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_BOX sourceBox = {
		input.sourceLeft, 0, 0,
		input.sourceLeft+input.width, input.height, 1
	};
	list->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);
	Transition(list, state.inputColor, state.inputColorState,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if(!GenerateDepthAndMotion(input, state, list, heap)){
		SetStatus("FSR2 depth/motion generation failed for eye %d", eye);
		return false;
	}
	Transition(list, state.output, state.outputState,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	FfxFsr2DispatchDescription dispatch = {};
	dispatch.commandList = ffxGetCommandListDX12(list);
	dispatch.color = ffxGetResourceDX12(&state.context, state.inputColor,
		L"FSR2_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.depth = ffxGetResourceDX12(&state.context, state.localDepth,
		L"FSR2_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.motionVectors = ffxGetResourceDX12(&state.context, state.motion,
		L"FSR2_InputMotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.exposure = ffxGetResourceDX12(&state.context, nil,
		L"FSR2_AutoExposure");
	dispatch.reactive = ffxGetResourceDX12(&state.context, nil,
		L"FSR2_EmptyReactive");
	dispatch.transparencyAndComposition =
		ffxGetResourceDX12(&state.context, nil,
			L"FSR2_EmptyTransparencyComposition");
	dispatch.output = ffxGetResourceDX12(&state.context, state.output,
		L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	dispatch.jitterOffset.x = gJitterX;
	dispatch.jitterOffset.y = gJitterY;
	// The local shader stores previous-current displacement directly in
	// pixels, so one texture value already represents one display pixel.
	dispatch.motionVectorScale.x = 1.0f;
	dispatch.motionVectorScale.y = 1.0f;
	dispatch.renderSize.width = input.width;
	dispatch.renderSize.height = input.height;
	dispatch.enableSharpening = false;
	dispatch.sharpness = 0.0f;
	dispatch.frameTimeDelta = gFrameTimeMs;
	dispatch.preExposure = 1.0f;
	dispatch.reset = state.resetPending || !state.historyValid;
	dispatch.cameraNear = input.nearPlane;
	dispatch.cameraFar = input.farPlane;
	dispatch.cameraFovAngleVertical =
		2.0f*atanf(1.0f/fabsf(input.projection[5]));
	dispatch.viewSpaceToMetersFactor = 1.0f;

	FfxErrorCode error = FFX_ERROR_BACKEND_API_ERROR;
	try {
		error = ffxFsr2ContextDispatch(&state.context, &dispatch);
	} catch(...) {
		WriteLog("[FSR2] Dispatch raised an exception for eye %d", eye);
	}
	if(error != FFX_OK){
		SetStatus("FSR2 dispatch failed for eye %d (%d)", eye,
			(int)error);
		state.historyValid = false;
		state.resetPending = true;
		return false;
	}
	Transition(list, state.output, state.outputState,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	memcpy(state.previousView, input.view, sizeof(state.previousView));
	memcpy(state.previousProjection, input.projection,
		sizeof(state.previousProjection));
	state.historyValid = true;
	state.resetPending = false;
	output->color = state.output;
	output->shaderResourceView = state.outputSrv.ptr;
	output->width = state.width;
	output->height = state.height;
	return true;
}
}

void Initialize()
{
	if(gInitialized)
		return;
	gInitialized = true;
	QueryPerformanceFrequency(&gCounterFrequency);
	gPreviousFrameCounter.QuadPart = 0;
	SetStatus("FSR2 initialized; waiting for D3D12 device");
}

void AttachDevice()
{
	if(!gInitialized)
		Initialize();
	if(gDeviceAttached)
		return;
	if(!rw::d3d12::getDevice()){
		SetStatus("FSR2 initialized, but D3D12 device is unavailable");
		return;
	}
	gDeviceAttached = true;
	gSupported = CreateDepthMotionPipeline();
	if(gSupported)
		SetStatus("FSR2 Native AA ready");
	else
		SetStatus("FSR2 depth/motion pipeline creation failed");
}

bool BeginFrame(float jitterX, float jitterY)
{
	gFrameReady = false;
	gLastEvaluationSucceeded = false;
	gJitterX = jitterX;
	gJitterY = jitterY;
	if(!gInitialized || !gDeviceAttached || !gSupported ||
	   gRuntimeFailed)
		return false;
	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	if(gPreviousFrameCounter.QuadPart != 0 &&
	   gCounterFrequency.QuadPart > 0){
		const double elapsed =
			(double)(now.QuadPart-gPreviousFrameCounter.QuadPart)*
			1000.0/(double)gCounterFrequency.QuadPart;
		gFrameTimeMs = (float)clamp(elapsed, 1.0, 100.0);
	}
	gPreviousFrameCounter = now;
	gFrameReady = true;
	return true;
}

bool EvaluateFrame(const EyeInput inputs[EYE_COUNT],
	EyeOutput outputs[EYE_COUNT])
{
	if(!gFrameReady || !inputs || !outputs)
		return false;
	for(int eye = 0; eye < EYE_COUNT; eye++)
		outputs[eye] = EyeOutput();
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(!EvaluateEye(eye, inputs[eye], &outputs[eye])){
			ResetHistory();
			gRuntimeFailed = true;
			return false;
		}
	}
	gLastEvaluationSucceeded = true;
	return true;
}

void ResetHistory()
{
	for(int eye = 0; eye < EYE_COUNT; eye++){
		gEye[eye].historyValid = false;
		gEye[eye].resetPending = true;
	}
	gFrameReady = false;
	gLastEvaluationSucceeded = false;
	gRuntimeFailed = false;
	gPreviousFrameCounter.QuadPart = 0;
}

void ReleaseResources()
{
	if(gEye[0].contextCreated || gEye[1].contextCreated)
		rw::d3d12::waitForGpu();
	for(int eye = 0; eye < EYE_COUNT; eye++)
		ReleaseEye(eye);
	gFrameReady = false;
	gLastEvaluationSucceeded = false;
	gRuntimeFailed = false;
	gPreviousFrameCounter.QuadPart = 0;
}

void Shutdown()
{
	ReleaseResources();
	ReleaseDepthMotionPipeline();
	gInitialized = false;
	gDeviceAttached = false;
	gSupported = false;
	gLastEvaluationSucceeded = false;
	strcpy(gStatus, "FSR2 shut down");
}

bool IsSupported() { return gSupported && !gRuntimeFailed; }
bool WasLastEvaluationSuccessful() { return gLastEvaluationSucceeded; }
const char *GetStatus() { return gStatus; }
}

#else

namespace Fsr2
{
void Initialize() {}
void AttachDevice() {}
bool BeginFrame(float, float) { return false; }
bool EvaluateFrame(const EyeInput[2], EyeOutput[2]) { return false; }
void ResetHistory() {}
void ReleaseResources() {}
void Shutdown() {}
bool IsSupported() { return false; }
bool WasLastEvaluationSuccessful() { return false; }
const char *GetStatus() { return "FSR2 requires OpenXR and D3D12"; }
}

#endif
