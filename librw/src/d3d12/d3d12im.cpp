#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

#ifdef RW_D3D12

enum {
	IMMEDIATE_FRAME_COUNT = 3,
	// Stereo records two full effect/Im2D/Im3D passes into one command list.
	IMMEDIATE_UPLOAD_SIZE = 16*1024*1024,
	RENDER_STATE_COUNT = GSALPHATESTREF + 1
};

struct ImmediateArena
{
	ID3D12Resource *resource;
	uint8 *mapped;
	uint32 offset;
};

static ID3D12RootSignature *immediateRootSignature;
enum {
	PIPELINE_TRIANGLE,
	PIPELINE_LINE,
	PIPELINE_POINT,
	PIPELINE_TOPOLOGY_COUNT
};
enum {
	BLEND_ALPHA,
	BLEND_ADD_ONE,
	BLEND_ADD_ALPHA,
	BLEND_SHADOW,
	BLEND_INVERSE_DEST,
	BLEND_REPLACE,
	BLEND_KEEP_DESTINATION,
	BLEND_MODULATE_DESTINATION,
	BLEND_ALPHA_INVERSE_DEST_ALPHA,
	BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA,
	BLEND_MODE_COUNT
};
enum {
	DEPTH_DISABLED,
	DEPTH_TEST_ONLY,
	DEPTH_TEST_WRITE,
	DEPTH_WRITE_ONLY,
	DEPTH_MODE_COUNT
};
enum {
	DEPTH_COMPARE_LESS_EQUAL,
	DEPTH_COMPARE_LESS,
	DEPTH_COMPARE_COUNT
};
enum {
	STENCIL_DISABLED,
	STENCIL_ALWAYS_REPLACE,
	STENCIL_EQUAL_KEEP,
	STENCIL_NOT_EQUAL_KEEP,
	STENCIL_MODE_COUNT
};

static ID3D12PipelineState *im2DPipelines[BLEND_MODE_COUNT][DEPTH_COMPARE_COUNT][DEPTH_MODE_COUNT][STENCIL_MODE_COUNT][PIPELINE_TOPOLOGY_COUNT];
static ID3D12RootSignature *screenDropletRootSignature;
static ID3D12PipelineState *screenDropletPipeline;
static ID3D12RootSignature *modernRainRootSignature;
static ID3D12PipelineState *modernRainPipeline;
static bool32 modernRainCreationAttempted;
static ID3D12RootSignature *postFXRootSignature;
static ID3D12PipelineState *postFXPipeline;
static ID3D12RootSignature *openXRResolveRootSignature;
static ID3D12PipelineState *openXRResolvePipeline;
static ID3D12RootSignature *cinemaQuadRootSignature;
static ID3D12PipelineState *cinemaQuadPipeline;
static ID3D12RootSignature *im3DRootSignature;
static ID3D12PipelineState *im3DPipelines[BLEND_MODE_COUNT][DEPTH_MODE_COUNT][STENCIL_MODE_COUNT][PIPELINE_TOPOLOGY_COUNT];
static Raster *immediateWhiteRaster;
static ImmediateArena arenas[IMMEDIATE_FRAME_COUNT];
static uint32 activeArena = UINT32_MAX;
static void *renderStates[RENDER_STATE_COUNT];
static bool32 immediateReady;
static D3D12_VERTEX_BUFFER_VIEW im3DVertexView;
static int32 num3DVertices;
static uint32 im3DFlags;
static float im3DWorld[16];
static bool32 immediate2DStrictDepth;

template<class T>
static void
releaseCom(T *&object)
{
	if(object){
		object->Release();
		object = nil;
	}
}

static D3D12_HEAP_PROPERTIES
uploadHeapProperties(void)
{
	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.Type = D3D12_HEAP_TYPE_UPLOAD;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

static D3D12_RESOURCE_DESC
bufferDesc(uint64 size)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return desc;
}

static bool32
compileShader(const char *source, const char *entry, const char *target,
	          ID3DBlob **shader)
{
	ID3DBlob *errors = nil;
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
	HRESULT hr = D3DCompile(source, strlen(source), "librw_d3d12_im2d",
	                        nil, nil, entry, target, flags, 0, shader, &errors);
	if(FAILED(hr) && errors)
		fprintf(stderr, "librw D3D12 Im2D shader: %s\n",
		        (const char*)errors->GetBufferPointer());
	releaseCom(errors);
	return SUCCEEDED(hr);
}

static bool32
getPipelineBlend(uint32 mode, D3D12_BLEND *source, D3D12_BLEND *destination,
	             D3D12_BLEND *sourceAlpha, D3D12_BLEND *destinationAlpha)
{
	switch(mode){
	case BLEND_ADD_ONE:
		*source = D3D12_BLEND_ONE; *destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_ONE; *destinationAlpha = D3D12_BLEND_ONE; break;
	case BLEND_ADD_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA; *destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA; *destinationAlpha = D3D12_BLEND_ONE; break;
	case BLEND_SHADOW:
		*source = D3D12_BLEND_ZERO; *destination = D3D12_BLEND_INV_SRC_COLOR;
		*sourceAlpha = D3D12_BLEND_ZERO; *destinationAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
	case BLEND_INVERSE_DEST:
		*source = D3D12_BLEND_INV_DEST_COLOR; *destination = D3D12_BLEND_ZERO;
		*sourceAlpha = D3D12_BLEND_INV_DEST_ALPHA; *destinationAlpha = D3D12_BLEND_ZERO; break;
	case BLEND_REPLACE:
		*source = D3D12_BLEND_ONE; *destination = D3D12_BLEND_ZERO;
		*sourceAlpha = D3D12_BLEND_ONE; *destinationAlpha = D3D12_BLEND_ZERO; break;
	case BLEND_KEEP_DESTINATION:
		*source = D3D12_BLEND_ZERO; *destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_ZERO; *destinationAlpha = D3D12_BLEND_ONE; break;
	case BLEND_MODULATE_DESTINATION:
		*source = D3D12_BLEND_ZERO; *destination = D3D12_BLEND_SRC_COLOR;
		*sourceAlpha = D3D12_BLEND_ZERO; *destinationAlpha = D3D12_BLEND_SRC_ALPHA; break;
	case BLEND_ALPHA_INVERSE_DEST_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA; *destination = D3D12_BLEND_INV_DEST_ALPHA;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA; *destinationAlpha = D3D12_BLEND_INV_DEST_ALPHA; break;
	case BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA:
		*source = D3D12_BLEND_DEST_ALPHA; *destination = D3D12_BLEND_INV_DEST_ALPHA;
		*sourceAlpha = D3D12_BLEND_DEST_ALPHA; *destinationAlpha = D3D12_BLEND_INV_DEST_ALPHA; break;
	default:
		*source = D3D12_BLEND_SRC_ALPHA; *destination = D3D12_BLEND_INV_SRC_ALPHA;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA; *destinationAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
	}
	return 1;
}

static uint32
getActiveBlendMode(void)
{
	void *source = renderStates[SRCBLEND];
	void *destination = renderStates[DESTBLEND];
	if(source == (void*)BLENDONE && destination == (void*)BLENDONE)
		return BLEND_ADD_ONE;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDONE)
		return BLEND_ADD_ALPHA;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDINVSRCCOLOR)
		return BLEND_SHADOW;
	if(source == (void*)BLENDINVDESTCOLOR && destination == (void*)BLENDZERO)
		return BLEND_INVERSE_DEST;
	if(source == (void*)BLENDONE && destination == (void*)BLENDZERO)
		return BLEND_REPLACE;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDONE)
		return BLEND_KEEP_DESTINATION;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDSRCCOLOR)
		return BLEND_MODULATE_DESTINATION;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDINVDESTALPHA)
		return BLEND_ALPHA_INVERSE_DEST_ALPHA;
	if(source == (void*)BLENDDESTALPHA && destination == (void*)BLENDINVDESTALPHA)
		return BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA;
	return BLEND_ALPHA;
}

static bool32
getActiveSampler(Texture::Addressing fallbackAddress,
                 D3D12_GPU_DESCRIPTOR_HANDLE *sampler)
{
	uint32 filter = (uint32)(uintptr_t)renderStates[TEXTUREFILTER];
	uint32 address = (uint32)(uintptr_t)renderStates[TEXTUREADDRESS];
	uint32 addressU = (uint32)(uintptr_t)renderStates[TEXTUREADDRESSU];
	uint32 addressV = (uint32)(uintptr_t)renderStates[TEXTUREADDRESSV];
	if(filter == 0)
		filter = Texture::LINEAR;
	if(address == 0)
		address = fallbackAddress;
	if(addressU == 0)
		addressU = address;
	if(addressV == 0)
		addressV = address;
	return getSamplerView(filter, addressU, addressV, sampler);
}

static uint32
getActiveStencilMode(void)
{
	if(renderStates[STENCILENABLE] == nil)
		return STENCIL_DISABLED;
	uint32 function = (uint32)(uintptr_t)renderStates[STENCILFUNCTION];
	uint32 pass = (uint32)(uintptr_t)renderStates[STENCILPASS];
	if(function == STENCILALWAYS && pass == STENCILREPLACE)
		return STENCIL_ALWAYS_REPLACE;
	if(function == STENCILEQUAL)
		return STENCIL_EQUAL_KEEP;
	if(function == STENCILNOTEQUAL)
		return STENCIL_NOT_EQUAL_KEEP;
	return STENCIL_DISABLED;
}

static void
setPipelineStencil(D3D12_DEPTH_STENCIL_DESC *state, uint32 mode)
{
	state->StencilEnable = mode != STENCIL_DISABLED;
	state->StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
	state->StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	D3D12_DEPTH_STENCILOP_DESC face;
	memset(&face, 0, sizeof(face));
	face.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	face.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	face.StencilPassOp = mode == STENCIL_ALWAYS_REPLACE ?
		D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP;
	switch(mode){
	case STENCIL_EQUAL_KEEP:
		face.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		break;
	case STENCIL_NOT_EQUAL_KEEP:
		face.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
		break;
	default:
		face.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		break;
	}
	state->FrontFace = face;
	state->BackFace = face;
}

static bool32
createPipeline(ID3D12Device *device, D3D12_PRIMITIVE_TOPOLOGY_TYPE type,
	           const D3D12_SHADER_BYTECODE &vs,
	           const D3D12_SHADER_BYTECODE &ps,
	           const D3D12_INPUT_LAYOUT_DESC &input,
	           uint32 blendMode, bool32 depthTest, bool32 depthWrite,
	           bool32 strictDepth, uint32 stencilMode,
	           ID3D12PipelineState **pipeline)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = immediateRootSignature;
	desc.VS = vs;
	desc.PS = ps;
	desc.InputLayout = input;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	getPipelineBlend(blendMode, &desc.BlendState.RenderTarget[0].SrcBlend,
	                 &desc.BlendState.RenderTarget[0].DestBlend,
	                 &desc.BlendState.RenderTarget[0].SrcBlendAlpha,
	                 &desc.BlendState.RenderTarget[0].DestBlendAlpha);
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = depthTest || depthWrite;
	desc.DepthStencilState.DepthWriteMask = depthWrite ?
		D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = depthTest ?
		(strictDepth ? D3D12_COMPARISON_FUNC_LESS :
		 D3D12_COMPARISON_FUNC_LESS_EQUAL) : D3D12_COMPARISON_FUNC_ALWAYS;
	setPipelineStencil(&desc.DepthStencilState, stencilMode);
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = type;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	return SUCCEEDED(device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(pipeline)));
}

static bool32
createIm3DPipeline(ID3D12Device *device,
	               D3D12_PRIMITIVE_TOPOLOGY_TYPE type,
	               const D3D12_SHADER_BYTECODE &vs,
	               const D3D12_SHADER_BYTECODE &ps,
	               const D3D12_INPUT_LAYOUT_DESC &input,
	               uint32 blendMode, bool32 depthTest, bool32 depthWrite,
	               uint32 stencilMode,
	               ID3D12PipelineState **pipeline)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = im3DRootSignature;
	desc.VS = vs;
	desc.PS = ps;
	desc.InputLayout = input;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	getPipelineBlend(blendMode, &desc.BlendState.RenderTarget[0].SrcBlend,
	                 &desc.BlendState.RenderTarget[0].DestBlend,
	                 &desc.BlendState.RenderTarget[0].SrcBlendAlpha,
	                 &desc.BlendState.RenderTarget[0].DestBlendAlpha);
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = depthTest || depthWrite;
	desc.DepthStencilState.DepthWriteMask = depthWrite ?
		D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = depthTest ?
		D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
	setPipelineStencil(&desc.DepthStencilState, stencilMode);
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = type;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	return SUCCEEDED(device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(pipeline)));
}

static bool32
createScreenDropletResources(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	for(uint32 i = 0; i < 2; i++){
		ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[i].NumDescriptors = 1;
		ranges[i].BaseShaderRegister = i;
		ranges[i].OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	}
	D3D12_ROOT_PARAMETER params[3];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 2;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	for(uint32 i = 0; i < 2; i++){
		params[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[i+1].DescriptorTable.NumDescriptorRanges = 1;
		params[i+1].DescriptorTable.pDescriptorRanges = &ranges[i];
		params[i+1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 3;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	signature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 screen droplets root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&screenDropletRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer ScreenConstants : register(b0) { float2 screenSize; };"
		"Texture2D maskTexture : register(t0);"
		"Texture2D sceneTexture : register(t1);"
		"SamplerState linearSampler : register(s0);"
		"struct VSIn { float4 position : POSITION; uint color : COLOR0;"
		" float2 maskUV : TEXCOORD0; float2 sceneUV : TEXCOORD1; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 maskUV : TEXCOORD0; float2 sceneUV : TEXCOORD1; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" output.position = float4(input.position.x * 2.0 / screenSize.x - 1.0,"
		" 1.0 - input.position.y * 2.0 / screenSize.y, input.position.z, 1.0);"
		" output.color = float4((input.color >> 16) & 255,"
		" (input.color >> 8) & 255, input.color & 255,"
		" (input.color >> 24) & 255) / 255.0;"
		" output.maskUV = input.maskUV; output.sceneUV = input.sceneUV;"
		" return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 mask = maskTexture.Sample(linearSampler, input.maskUV);"
		" float4 scene = sceneTexture.Sample(linearSampler, input.sceneUV);"
		" return float4(input.color.rgb * mask.rgb * scene.rgb,"
		" input.color.a * mask.a); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = screenDropletRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.InputLayout.pInputElementDescs = elements;
	desc.InputLayout.NumElements = (UINT)nelem(elements);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(&screenDropletPipeline));
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	return SUCCEEDED(hr);
}

static bool32
createModernRainResources(ID3D12Device *device)
{
	D3D12_ROOT_PARAMETER parameter;
	memset(&parameter, 0, sizeof(parameter));
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameter.Constants.ShaderRegister = 0;
	parameter.Constants.Num32BitValues = 40;
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 1;
	signature.pParameters = &parameter;
	signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 modern rain root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&modernRainRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	// A single instanced draw creates a camera-centred world-space storm volume.
	// Positions are deterministic for both stereo eyes, while the view/projection
	// matrices provide correct parallax and the scene depth rejects indoor or
	// occluded streak fragments.
	static const char *shaderSource =
		"cbuffer RainConstants : register(b0) {"
		" row_major float4x4 view; row_major float4x4 projection;"
		" float4 cameraIntensity; float4 windTimeQuality; };"
		"struct VSOut { float4 position:SV_POSITION; float2 uv:TEXCOORD0;"
		" float alpha:TEXCOORD1; float brightness:TEXCOORD2; };"
		"uint Hash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;"
		" x*=0x846ca68bu;x^=x>>16;return x;}"
		"float Random01(uint x){return (Hash(x)&0x00ffffffu)/16777215.0;}"
		"float2 Corner(uint vertexId){"
		" if(vertexId==0)return float2(0,0);"
		" if(vertexId==1)return float2(1,0);"
		" if(vertexId==2)return float2(0,1);"
		" if(vertexId==3)return float2(0,1);"
		" if(vertexId==4)return float2(1,0);return float2(1,1); }"
		"VSOut VSMain(uint vertexId:SV_VertexID,uint instanceId:SV_InstanceID){"
		" VSOut o;bool cinematic=windTimeQuality.w>2.5;"
		" uint grid=cinematic?72u:56u;uint baseCount=grid*grid;"
		" uint cellInstance=instanceId%baseCount;uint layer=instanceId/baseCount;"
		" float cellSize=cinematic?1.10:1.25;"
		" int2 cameraCell=int2(floor(cameraIntensity.xy/cellSize));"
		" int2 cell=cameraCell+int2(int(cellInstance%grid)-int(grid/2u),"
		"  int(cellInstance/grid)-int(grid/2u));"
		" uint h=Hash(asuint(cell.x)*73856093u^asuint(cell.y)*19349663u^"
		"  layer*83492791u);"
		" float rx=Random01(h)-0.5;float ry=Random01(h+101u)-0.5;"
		" float rz=Random01(h+211u);float shape=Random01(h+307u);"
		" float speed=24.0+Random01(h+401u)*18.0;float cycle=38.0;"
		" float phase=frac(rz+windTimeQuality.z*speed/cycle);"
		" float3 drop=float3((float2(cell)+float2(rx,ry))*cellSize,"
		"  cameraIntensity.z+20.0-phase*cycle);"
		" float2 wind=windTimeQuality.xy;drop.xy+=wind*phase*7.0;"
		" float3 rainDir=normalize(float3(wind.x*0.34,wind.y*0.34,-1.0));"
		" float3 toCamera=cameraIntensity.xyz-drop;"
		" float3 side=cross(rainDir,toCamera/max(length(toCamera),0.001));"
		" float sideLength=max(length(side),0.001);side/=sideLength;"
		" float distance=length(toCamera);float nearFactor=saturate(1.0-distance/48.0);"
		" float closeFade=smoothstep(1.0,3.0,distance);"
		" bool nearDrop=shape>0.76;"
		" float lengthScale=nearDrop?lerp(0.24,0.62,Random01(h+307u)):"
		"  lerp(0.07,0.24,Random01(h+307u));"
		" if(cinematic)lengthScale*=1.08;"
		" float width=(nearDrop?lerp(0.014,0.026,Random01(h+503u)):"
		"  lerp(0.007,0.014,Random01(h+503u)))*lerp(0.85,1.2,nearFactor);"
		" float2 uv=Corner(vertexId);"
		" float3 worldPos=drop-rainDir*lengthScale*uv.y+"
		"  side*(uv.x-0.5)*width;"
		" o.position=mul(mul(float4(worldPos,1.0),view),projection);"
		" o.uv=uv;o.alpha=cameraIntensity.w*closeFade*"
		"  lerp(0.42,0.92,Random01(h+601u))*lerp(0.65,1.0,nearFactor);"
		" o.brightness=lerp(0.78,1.28,pow(nearFactor,2.0));return o;}"
		"float4 PSMain(VSOut input):SV_TARGET{"
		" float across=1.0-abs(input.uv.x*2.0-1.0);"
		" float core=pow(saturate(across),1.35);"
		" float tail=smoothstep(0.0,0.18,input.uv.y)*"
		"  (1.0-smoothstep(0.58,1.0,input.uv.y));"
		" float glint=pow(core,6.0);float alpha=input.alpha*core*tail*0.68;"
		" float3 color=lerp(float3(0.62,0.67,0.73),"
		"  float3(0.94,0.97,1.0),glint)*input.brightness;"
		" return float4(color,alpha); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = modernRainRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = TRUE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(&modernRainPipeline));
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	return SUCCEEDED(hr);
}

static bool32
createPostFXResources(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE range;
	memset(&range, 0, sizeof(range));
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart =
		D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[2];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 12;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 2;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	signature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 post FX root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&postFXRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer PostFXConstants : register(b0) { float2 screenSize;"
		" uint effectMode; float unusedValue; float4 effect0; float4 effect1; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSIn { float4 position : POSITION; uint color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" output.position = float4(input.position.x * 2.0 / screenSize.x - 1.0,"
		" 1.0 - input.position.y * 2.0 / screenSize.y, input.position.z, 1.0);"
		" output.uv = input.uv; return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 dst = image.Sample(imageSampler, input.uv);"
		" if(effectMode != 0) return float4(dst.rgb * effect0.rgb + effect1.rgb, 1.0);"
		" float a = effect0.a; float4 doublec = saturate(effect0 * 2.0);"
		" float4 previous = dst;"
		" [unroll] for(int i = 0; i < 5; i++) {"
		"  float4 value = dst * (1.0 - a) + previous * doublec * a;"
		"  value += previous * effect0; value += previous * effect0;"
		"  previous = saturate(value); }"
		" return float4(previous.rgb, 1.0); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = postFXRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.InputLayout.pInputElementDescs = elements;
	desc.InputLayout.NumElements = (UINT)nelem(elements);
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(&postFXPipeline));
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	return SUCCEEDED(hr);
}

static bool32
createOpenXRResolveResources(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE range;
	memset(&range, 0, sizeof(range));
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart =
		D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[2];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 24;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 2;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 OpenXR resolve root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&openXRResolveRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer ResolveConstants : register(b0) { float2 uvScale; float2 uvOffset;"
		" float2 inverseSourceSize; uint fxaaEnabled; uint colorMode;"
		" float2 inverseOutputSize; float2 resolvePadding;"
		" float4 blurColor; float4 contrastMult; float4 contrastAdd; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };"
		"VSOut VSMain(uint id : SV_VertexID) { VSOut output;"
		" float2 position = id == 0 ? float2(-1.0, -1.0) :"
		"                   id == 1 ? float2(-1.0,  3.0) : float2(3.0, -1.0);"
		" output.position = float4(position, 0.0, 1.0);"
		" float2 sourceUV = float2((position.x + 1.0) * 0.5,"
		"                          (1.0 - position.y) * 0.5);"
		" output.uv = sourceUV; return output; }"
		"float Luma(float3 color) { return dot(color, float3(0.299, 0.587, 0.114)); }"
		"float3 SampleColor(float2 outputUV) {"
		" float2 sourceUV = outputUV * uvScale + uvOffset;"
		" return image.Sample(imageSampler, clamp(sourceUV, 0.0, 1.0)).rgb; }"
		"float3 SamplePoint(float2 outputUV) {"
		" float2 sourceUV=clamp(outputUV*uvScale+uvOffset,0.0,1.0-inverseSourceSize*0.5);"
		" int2 texel=int2(sourceUV/inverseSourceSize); return image.Load(int3(texel,0)).rgb; }"
		"float3 SampleDownsampled(float2 outputUV) {"
		" float2 footprint=uvScale*inverseOutputSize;"
		" float2 ratio=footprint/max(inverseSourceSize,float2(0.0000001,0.0000001));"
		" if(max(ratio.x,ratio.y)<=1.05) return SampleColor(outputUV);"
		" float2 radius=footprint*0.25;"
		" return 0.25*(SampleColor(outputUV+float2(-radius.x,-radius.y))"
		"             +SampleColor(outputUV+float2( radius.x,-radius.y))"
		"             +SampleColor(outputUV+float2(-radius.x, radius.y))"
		"             +SampleColor(outputUV+float2( radius.x, radius.y))); }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float3 middle = fxaaEnabled!=0?SampleColor(input.uv):SampleDownsampled(input.uv);"
		" float2 outputTexel=inverseSourceSize/max(uvScale,float2(0.00001,0.00001));"
		" float3 nw = SampleColor(input.uv + float2(-1.0, -1.0) * outputTexel);"
		" float3 ne = SampleColor(input.uv + float2( 1.0, -1.0) * outputTexel);"
		" float3 sw = SampleColor(input.uv + float2(-1.0,  1.0) * outputTexel);"
		" float3 se = SampleColor(input.uv + float2( 1.0,  1.0) * outputTexel);"
		" float lm=Luma(middle), lnw=Luma(nw), lne=Luma(ne), lsw=Luma(sw), lse=Luma(se);"
		" float lmin=min(lm,min(min(lnw,lne),min(lsw,lse)));"
		" float lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));"
		" float2 direction=float2(-((lnw+lne)-(lsw+lse)),((lnw+lsw)-(lne+lse)));"
		" float reduce=max((lnw+lne+lsw+lse)*0.03125,0.0078125);"
		" direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduce),-8.0,8.0)*outputTexel;"
		" float3 a=0.5*(SampleColor(input.uv+direction*(1.0/3.0-0.5))+SampleColor(input.uv+direction*(2.0/3.0-0.5)));"
		" float3 b=a*0.5+0.25*(SampleColor(input.uv+direction*-0.5)+SampleColor(input.uv+direction*0.5));"
		" float lb=Luma(b); float3 color=fxaaEnabled!=0?((lb<lmin||lb>lmax)?a:b):middle;"
		" if(colorMode==1) { float alpha=blurColor.a; float3 doubled=saturate(blurColor.rgb*2.0);"
		"  float3 original=color, previous=color; [unroll] for(int i=0;i<5;i++) {"
		"   float3 filtered=original*(1.0-alpha)+previous*doubled*alpha;"
		"   filtered+=previous*blurColor.rgb*2.0; previous=saturate(filtered); } color=previous; }"
		" else if(colorMode==2) color=saturate(color*contrastMult.rgb+contrastAdd.rgb);"
		" return float4(color,1.0); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = openXRResolveRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	// OpenXR exposes its RGBA8 swapchain resources as TYPELESS, allowing this
	// UNORM RTV while the compositor still interprets the declared sRGB format.
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(&openXRResolvePipeline));
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	return SUCCEEDED(hr);
}

// Theater cinema: draws the staged cinema image as a world-anchored quad
// into an eye swapchain image. The quad corners arrive as clip-space
// float4s with w intact, so texture interpolation stays perspective
// correct without any matrix work in the shader.
static bool32
createCinemaQuadResources(ID3D12Device *device)
{
	D3D12_DESCRIPTOR_RANGE range;
	memset(&range, 0, sizeof(range));
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart =
		D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[2];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 16;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler;
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 2;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 cinema quad root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&cinemaQuadRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer QuadConstants : register(b0) { float4 corners[4]; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };"
		"VSOut VSMain(uint id : SV_VertexID) { VSOut output;"
		" output.position = corners[id];"
		" output.uv = float2(id == 1 || id == 3 ? 1.0 : 0.0,"
		"                    id == 2 || id == 3 ? 1.0 : 0.0);"
		" return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" return float4(image.Sample(imageSampler, input.uv).rgb, 1.0); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		return 0;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.pRootSignature = cinemaQuadRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	hr = device->CreateGraphicsPipelineState(
		&desc, IID_PPV_ARGS(&cinemaQuadPipeline));
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	return SUCCEEDED(hr);
}

// Clears an eye swapchain image to black and draws the cinema quad into it.
// Records exclusively into the open frame command list, submitted with the
// standard frame fence -- the exact path stereo gameplay uses every frame.
bool32
drawCinemaQuadToExternal(ID3D12Resource *source, ID3D12Resource *destination,
	                 int32 width, int32 height, const float32 *corners)
{
	if(source == nil || destination == nil || width <= 0 || height <= 0 ||
	   corners == nil || !immediateReady)
		return 0;
	ID3D12Device *device = getDevice();
	ID3D12GraphicsCommandList *list = getCommandList();
	if(device == nil || list == nil)
		return 0;
	if(cinemaQuadPipeline == nil && !createCinemaQuadResources(device))
		return 0;
	D3D12_CPU_DESCRIPTOR_HANDLE sourceCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE sourceView;
	uint32 sourceIndex = UINT32_MAX;
	if(!allocateShaderResourceDescriptor(&sourceCpu, &sourceView, &sourceIndex))
		return 0;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv;
	memset(&srv, 0, sizeof(srv));
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(source, &srv, sourceCpu);
	D3D12_CPU_DESCRIPTOR_HANDLE targetView;
	uint32 targetViewIndex = UINT32_MAX;
	if(!allocateRenderTargetDescriptor(&targetView, &targetViewIndex)){
		deferDescriptorRelease(sourceIndex, UINT32_MAX, UINT32_MAX);
		return 0;
	}
	D3D12_RENDER_TARGET_VIEW_DESC viewDesc;
	memset(&viewDesc, 0, sizeof(viewDesc));
	viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(destination, &viewDesc, targetView);
	// The staging texture rests in RENDER_TARGET (the backbuffer copy
	// expects that); bracket the sampling window with barriers.
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = source;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	list->ResourceBarrier(1, &barrier);
	static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	list->OMSetRenderTargets(1, &targetView, FALSE, nil);
	list->ClearRenderTargetView(targetView, black, 0, nil);
	D3D12_VIEWPORT viewport = {
		0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f
	};
	D3D12_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	ID3D12DescriptorHeap *heap = getShaderResourceHeap();
	if(heap)
		list->SetDescriptorHeaps(1, &heap);
	list->SetGraphicsRootSignature(cinemaQuadRootSignature);
	list->SetPipelineState(cinemaQuadPipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	list->SetGraphicsRoot32BitConstants(0, 16, corners, 0);
	list->SetGraphicsRootDescriptorTable(1, sourceView);
	list->DrawInstanced(4, 1, 0, 0);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	list->ResourceBarrier(1, &barrier);
	deferDescriptorRelease(sourceIndex, targetViewIndex, UINT32_MAX);
	resetWorldDrawState();
	return 1;
}

bool32
initializeImmediate(void)
{
	if(immediateReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_DESCRIPTOR_RANGE ranges[4];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart =
		D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart =
		D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[2] = ranges[0];
	ranges[2].BaseShaderRegister = 1;
	ranges[3] = ranges[1];
	ranges[3].BaseShaderRegister = 1;
	D3D12_ROOT_PARAMETER params[3];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 2;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 3;
	signature.pParameters = params;
	signature.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 Im2D root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&immediateRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer ScreenConstants : register(b0) { float2 screenSize; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSIn { float4 position : POSITION; uint color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" output.position = float4(input.position.x * 2.0 / screenSize.x - 1.0,"
		" 1.0 - input.position.y * 2.0 / screenSize.y, input.position.z, 1.0);"
		" output.color = float4((input.color >> 16) & 255,"
		" (input.color >> 8) & 255, input.color & 255,"
		" (input.color >> 24) & 255) / 255.0;"
		" output.uv = input.uv; return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" return input.color * image.Sample(imageSampler, input.uv); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		shutdownImmediate();
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_INPUT_LAYOUT_DESC input = { elements, (UINT)nelem(elements) };
	D3D12_SHADER_BYTECODE vs = {
		vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()
	};
	D3D12_SHADER_BYTECODE ps = {
		pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()
	};
	static const D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyTypes[] = {
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
	};
	bool32 pipelinesReady = 1;
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 compare = 0; compare < DEPTH_COMPARE_COUNT; compare++)
			for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
				for(uint32 stencil = 0; stencil < STENCIL_MODE_COUNT; stencil++)
					for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++){
						bool32 depthTest = depth == DEPTH_TEST_ONLY || depth == DEPTH_TEST_WRITE;
						bool32 depthWrite = depth == DEPTH_TEST_WRITE || depth == DEPTH_WRITE_ONLY;
						pipelinesReady = pipelinesReady && createPipeline(
							device, topologyTypes[topology], vs, ps, input,
							blend, depthTest, depthWrite,
							compare == DEPTH_COMPARE_LESS, stencil,
							&im2DPipelines[blend][compare][depth][stencil][topology]);
					}
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	if(!pipelinesReady){
		shutdownImmediate();
		return 0;
	}
	if(!createScreenDropletResources(device)){
		shutdownImmediate();
		return 0;
	}
	if(!createPostFXResources(device)){
		shutdownImmediate();
		return 0;
	}
	if(!createOpenXRResolveResources(device)){
		shutdownImmediate();
		return 0;
	}

	D3D12_ROOT_PARAMETER im3DParams[6];
	memset(im3DParams, 0, sizeof(im3DParams));
	im3DParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	im3DParams[0].Constants.ShaderRegister = 0;
	im3DParams[0].Constants.Num32BitValues = 55;
	im3DParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	im3DParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	im3DParams[1].DescriptorTable.NumDescriptorRanges = 1;
	im3DParams[1].DescriptorTable.pDescriptorRanges = &ranges[0];
	im3DParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	im3DParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	im3DParams[2].DescriptorTable.NumDescriptorRanges = 1;
	im3DParams[2].DescriptorTable.pDescriptorRanges = &ranges[1];
	im3DParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	im3DParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	im3DParams[3].DescriptorTable.NumDescriptorRanges = 1;
	im3DParams[3].DescriptorTable.pDescriptorRanges = &ranges[2];
	im3DParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	im3DParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	im3DParams[4].Descriptor.ShaderRegister = 4;
	im3DParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	im3DParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	im3DParams[5].DescriptorTable.NumDescriptorRanges = 1;
	im3DParams[5].DescriptorTable.pDescriptorRanges = &ranges[3];
	im3DParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC im3DSignature = signature;
	im3DSignature.NumParameters = 6;
	im3DSignature.pParameters = im3DParams;
	hr = D3D12SerializeRootSignature(
		&im3DSignature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 Im3D root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		shutdownImmediate();
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&im3DRootSignature));
	releaseCom(serialized);
	if(FAILED(hr)){
		shutdownImmediate();
		return 0;
	}
	static const char *im3DShaderSource =
		"cbuffer DrawConstants : register(b0) {"
		" row_major float4x4 world; row_major float4x4 view;"
		" row_major float4x4 projection; float4 drawFlags;"
		" float fogEnd; float fogRange; uint fogColorPacked; };"
		"cbuffer ReflectionConstants : register(b4) {"
		" row_major float4x4 previousView;"
		" row_major float4x4 previousProjection;"
		" row_major float4x4 temporalPreviousView[2];"
		" row_major float4x4 temporalPreviousProjection[2];"
		" float4 temporalMotionValid; float4 temporalJitterClip;"
		" float4 reflectionParams; float4 reflectionTextureInfo;"
		" float4 rainParams; float4 puddleParams; float4 waterParams; };"
		"Texture2D image : register(t0); Texture2D previousScene : register(t1);"
		"SamplerState imageSampler : register(s0);"
		"SamplerState reflectionSampler : register(s1);"
		"struct VSIn { float3 position : POSITION; uint color : COLOR0;"
		" float2 uv : TEXCOORD0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; float fogFactor : TEXCOORD1;"
		" float3 worldPosition : TEXCOORD2; float3 worldViewRay : TEXCOORD3; };"
		"VSOut VSMain(VSIn input) { VSOut output;"
		" float4 worldPosition = mul(float4(input.position, 1.0), world);"
		" float4 p = worldPosition;"
		" p = mul(p, view); output.position = mul(p, projection);"
		" output.fogFactor = fogRange < 0.0 ?"
		" saturate((p.z - fogEnd) * fogRange) : 1.0;"
		" output.color = float4((input.color >> 16) & 255,"
		" (input.color >> 8) & 255, input.color & 255,"
		" (input.color >> 24) & 255) / 255.0;"
		" output.uv = input.uv; output.worldPosition = worldPosition.xyz;"
		" output.worldViewRay = mul(p.xyz,transpose((float3x3)view));"
		" return output; }"
		"float ReflectionFade(float4 clip) {"
		" if(clip.w <= 0.1) return 0.0; float2 ndc = clip.xy/clip.w;"
		" return saturate((1.0-max(abs(ndc.x),abs(ndc.y)))*3.0) *"
		" saturate(clip.w*0.5); }"
		"float2 ClampPreviousLeftUV(float2 uv) {"
		" float2 halfTexel = reflectionTextureInfo.zw*0.5;"
		" return clamp(uv,halfTexel,"
		"  float2(0.5-halfTexel.x,1.0-halfTexel.y)); }"
		"float2 PreviousLeftUV(float4 clip, float2 distortion) {"
		" float2 uv = clip.xy/clip.w*float2(0.5,-0.5)+0.5;"
		" uv.x *= 0.5; uv += distortion;"
		" return ClampPreviousLeftUV(uv); }"
		"float3 SamplePreviousWater(float4 clip,float2 slope) {"
		" float2 uv = PreviousLeftUV(clip,"
		"  float2(slope.x*0.0075,slope.y*0.015));"
		" float2 px = reflectionTextureInfo.zw*0.6;"
		" float3 seen = previousScene.SampleLevel(reflectionSampler,"
		"  ClampPreviousLeftUV(uv+px),0.0).rgb;"
		" seen += previousScene.SampleLevel(reflectionSampler,"
		"  ClampPreviousLeftUV(uv-px),0.0).rgb;"
		" seen += previousScene.SampleLevel(reflectionSampler,"
		"  ClampPreviousLeftUV(uv+float2(px.x,-px.y)),0.0).rgb;"
		" seen += previousScene.SampleLevel(reflectionSampler,"
		"  ClampPreviousLeftUV(uv-float2(px.x,-px.y)),0.0).rgb;"
		" return seen*0.25; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 color = input.color;"
		" if(drawFlags.x > 0.5) color *= image.Sample(imageSampler, input.uv);"
		" if(drawFlags.z > 1.5) clip(drawFlags.y - color.a - 0.000001);"
		" else if(drawFlags.z > 0.5) clip(color.a - drawFlags.y);"
		" if(drawFlags.w > 0.5 && reflectionParams.y > 0.0 &&"
		"    reflectionParams.w > 0.5) {"
		"  float3 viewDirection = input.worldViewRay/"
		"   max(length(input.worldViewRay),0.001);"
		"  float t = reflectionParams.z; float2 wp = input.worldPosition.xy;"
		"  float phaseA = dot(wp,float2(0.061,0.027))+t*0.85;"
		"  float phaseB = dot(wp,float2(-0.019,0.047))-t*0.63;"
		"  float2 slope = float2(sin(phaseA)+cos(phaseB)*0.7,"
		"   cos(phaseA*0.83)+sin(phaseB*1.13)*0.7)*0.65*waterParams.x;"
		"  float3 waveNormal = normalize(float3(slope.x*0.38,slope.y*0.38,1.0));"
		"  float facing = saturate(dot(waveNormal,-viewDirection));"
		"  float fresnel = pow(1.0-facing,5.0);"
		"  float3 mirrorRay = float3(viewDirection.x,viewDirection.y,"
		"   max(-viewDirection.z,0.02));"
		"  float march = clamp(8.0/max(mirrorRay.z,0.04),12.0,260.0);"
		"  float3 probePosition = input.worldPosition+mirrorRay*march;"
		"  float4 previousClip = mul(mul(float4(probePosition,1.0),"
		"   previousView),previousProjection);"
		"  float fade = ReflectionFade(previousClip);"
		"  if(fade > 0.0) {"
		"   float3 seen = SamplePreviousWater(previousClip,slope);"
		"   float weight = fade*saturate((fresnel*1.5+0.12)*reflectionParams.y);"
		"   color.rgb = lerp(color.rgb,seen,weight); }"
		" }"
		" float3 fogColor = float3(fogColorPacked & 255u,"
		" (fogColorPacked >> 8) & 255u, (fogColorPacked >> 16) & 255u) / 255.0;"
		" color.rgb = lerp(fogColor, color.rgb, input.fogFactor);"
		" return color; }";
	vertexShader = nil;
	pixelShader = nil;
	if(!compileShader(im3DShaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(im3DShaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(pixelShader);
		shutdownImmediate();
		return 0;
	}
	D3D12_INPUT_ELEMENT_DESC im3DElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 12,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_INPUT_LAYOUT_DESC im3DInput = {
		im3DElements, (UINT)nelem(im3DElements)
	};
	vs.pShaderBytecode = vertexShader->GetBufferPointer();
	vs.BytecodeLength = vertexShader->GetBufferSize();
	ps.pShaderBytecode = pixelShader->GetBufferPointer();
	ps.BytecodeLength = pixelShader->GetBufferSize();
	pipelinesReady = 1;
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
			for(uint32 stencil = 0; stencil < STENCIL_MODE_COUNT; stencil++)
				for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++){
					bool32 depthTest = depth == DEPTH_TEST_ONLY || depth == DEPTH_TEST_WRITE;
					bool32 depthWrite = depth == DEPTH_TEST_WRITE || depth == DEPTH_WRITE_ONLY;
					pipelinesReady = pipelinesReady && createIm3DPipeline(
						device, topologyTypes[topology], vs, ps, im3DInput,
						blend, depthTest, depthWrite, stencil,
						&im3DPipelines[blend][depth][stencil][topology]);
				}
	releaseCom(vertexShader);
	releaseCom(pixelShader);
	if(!pipelinesReady){
		shutdownImmediate();
		return 0;
	}

	D3D12_HEAP_PROPERTIES props = uploadHeapProperties();
	D3D12_RESOURCE_DESC buffer = bufferDesc(IMMEDIATE_UPLOAD_SIZE);
	for(uint32 i = 0; i < IMMEDIATE_FRAME_COUNT; i++){
		if(FAILED(device->CreateCommittedResource(
		       &props, D3D12_HEAP_FLAG_NONE, &buffer,
		       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
		       IID_PPV_ARGS(&arenas[i].resource)))){
			shutdownImmediate();
			return 0;
		}
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(arenas[i].resource->Map(
		       0, &readRange, (void**)&arenas[i].mapped))){
			shutdownImmediate();
			return 0;
		}
	}

	Image *white = Image::create(1, 1, 32);
	if(white == nil){
		shutdownImmediate();
		return 0;
	}
	white->allocate();
	white->pixels[0] = white->pixels[1] = white->pixels[2] =
		white->pixels[3] = 0xFF;
	immediateWhiteRaster = Raster::createFromImage(white, PLATFORM_D3D12);
	white->destroy();
	if(immediateWhiteRaster == nil){
		shutdownImmediate();
		return 0;
	}
	memset(renderStates, 0, sizeof(renderStates));
	renderStates[ZTESTENABLE] = (void*)1;
	renderStates[ZWRITEENABLE] = (void*)1;
	renderStates[VERTEXALPHA] = (void*)1;
	renderStates[TEXTUREFILTER] = (void*)Texture::LINEAR;
	renderStates[TEXTUREADDRESS] = (void*)Texture::WRAP;
	renderStates[TEXTUREADDRESSU] = (void*)Texture::WRAP;
	renderStates[TEXTUREADDRESSV] = (void*)Texture::WRAP;
	renderStates[SRCBLEND] = (void*)BLENDSRCALPHA;
	renderStates[DESTBLEND] = (void*)BLENDINVSRCALPHA;
	renderStates[STENCILFAIL] = (void*)STENCILKEEP;
	renderStates[STENCILZFAIL] = (void*)STENCILKEEP;
	renderStates[STENCILPASS] = (void*)STENCILKEEP;
	renderStates[STENCILFUNCTION] = (void*)STENCILALWAYS;
	renderStates[STENCILFUNCTIONMASK] = (void*)0xFF;
	renderStates[STENCILFUNCTIONWRITEMASK] = (void*)0xFF;
	renderStates[GSALPHATESTREF] = (void*)128;
	activeArena = UINT32_MAX;
	immediateReady = 1;
	return 1;
}

void
shutdownImmediate(void)
{
	immediateReady = 0;
	activeArena = UINT32_MAX;
	if(immediateWhiteRaster){
		immediateWhiteRaster->destroy();
		immediateWhiteRaster = nil;
	}
	for(uint32 i = 0; i < IMMEDIATE_FRAME_COUNT; i++){
		if(arenas[i].resource && arenas[i].mapped)
			arenas[i].resource->Unmap(0, nil);
		arenas[i].mapped = nil;
		arenas[i].offset = 0;
		releaseCom(arenas[i].resource);
	}
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 compare = 0; compare < DEPTH_COMPARE_COUNT; compare++)
			for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
				for(uint32 stencil = 0; stencil < STENCIL_MODE_COUNT; stencil++)
					for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++)
						releaseCom(im2DPipelines[blend][compare][depth][stencil][topology]);
	releaseCom(immediateRootSignature);
	releaseCom(screenDropletPipeline);
	releaseCom(screenDropletRootSignature);
	releaseCom(modernRainPipeline);
	releaseCom(modernRainRootSignature);
	modernRainCreationAttempted = 0;
	releaseCom(postFXPipeline);
	releaseCom(postFXRootSignature);
	releaseCom(openXRResolvePipeline);
	releaseCom(openXRResolveRootSignature);
	releaseCom(cinemaQuadPipeline);
	releaseCom(cinemaQuadRootSignature);
	for(uint32 blend = 0; blend < BLEND_MODE_COUNT; blend++)
		for(uint32 depth = 0; depth < DEPTH_MODE_COUNT; depth++)
			for(uint32 stencil = 0; stencil < STENCIL_MODE_COUNT; stencil++)
				for(uint32 topology = 0; topology < PIPELINE_TOPOLOGY_COUNT; topology++)
					releaseCom(im3DPipelines[blend][depth][stencil][topology]);
	releaseCom(im3DRootSignature);
	memset(&im3DVertexView, 0, sizeof(im3DVertexView));
	num3DVertices = 0;
}

void
setRenderState(int32 state, void *value)
{
	if(state >= 0 && state < RENDER_STATE_COUNT){
		renderStates[state] = value;
		if(state == TEXTUREADDRESS){
			renderStates[TEXTUREADDRESSU] = value;
			renderStates[TEXTUREADDRESSV] = value;
		}
	}
}

void
setImmediate2DStrictDepth(bool32 enabled)
{
	immediate2DStrictDepth = enabled;
}

void*
getRenderState(int32 state)
{
	return state >= 0 && state < RENDER_STATE_COUNT ?
		renderStates[state] : nil;
}

static uint32
alignUpload(uint32 value)
{
	return (value + 15u) & ~15u;
}

static bool32
allocateUpload(const void *data, uint32 size, uint64 *gpuAddress)
{
	if(data == nil || size == 0 || gpuAddress == nil)
		return 0;
	uint32 frame = getFrameIndex() % IMMEDIATE_FRAME_COUNT;
	if(activeArena != frame){
		activeArena = frame;
		arenas[frame].offset = 0;
	}
	ImmediateArena &arena = arenas[frame];
	uint32 offset = alignUpload(arena.offset);
	if(arena.mapped == nil || offset + size > IMMEDIATE_UPLOAD_SIZE)
		return 0;
	memcpy(arena.mapped + offset, data, size);
	*gpuAddress = arena.resource->GetGPUVirtualAddress() + offset;
	arena.offset = offset + size;
	return 1;
}

static bool32
uploadImmediate(const void *vertices, uint32 vertexBytes,
	            const void *indices, uint32 indexBytes,
	            D3D12_VERTEX_BUFFER_VIEW *vertexView,
	            D3D12_INDEX_BUFFER_VIEW *indexView)
{
	uint64 vertexAddress;
	if(!allocateUpload(vertices, vertexBytes, &vertexAddress))
		return 0;
	vertexView->BufferLocation = vertexAddress;
	vertexView->SizeInBytes = vertexBytes;
	vertexView->StrideInBytes = sizeof(Im2DVertex);
	if(indexBytes){
		uint64 indexAddress;
		if(!allocateUpload(indices, indexBytes, &indexAddress))
			return 0;
		indexView->BufferLocation = indexAddress;
		indexView->SizeInBytes = indexBytes;
		indexView->Format = DXGI_FORMAT_R16_UINT;
	}
	return 1;
}

static ID3D12PipelineState*
selectPipeline(PrimitiveType type, D3D12_PRIMITIVE_TOPOLOGY *topology)
{
	uint32 blend = getActiveBlendMode();
	bool32 depthTest = renderStates[ZTESTENABLE] != nil;
	bool32 depthWrite = renderStates[ZWRITEENABLE] != nil;
	// The radar writes an invisible depth mask at exactly the same Z as its
	// nine textured tiles. Only that tile pass needs strict LESS; using it for
	// every Im2D draw clips font glyphs and the rest of the HUD.
	uint32 compare = immediate2DStrictDepth && depthTest && !depthWrite ?
		DEPTH_COMPARE_LESS : DEPTH_COMPARE_LESS_EQUAL;
	uint32 depth = depthTest ?
		(depthWrite ? DEPTH_TEST_WRITE : DEPTH_TEST_ONLY) :
		(depthWrite ? DEPTH_WRITE_ONLY : DEPTH_DISABLED);
	uint32 stencil = getActiveStencilMode();
	switch(type){
	case PRIMTYPELINELIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		return im2DPipelines[blend][compare][depth][stencil][PIPELINE_LINE];
	case PRIMTYPEPOLYLINE:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		return im2DPipelines[blend][compare][depth][stencil][PIPELINE_LINE];
	case PRIMTYPEPOINTLIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		return im2DPipelines[blend][compare][depth][stencil][PIPELINE_POINT];
	case PRIMTYPETRISTRIP:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		return im2DPipelines[blend][compare][depth][stencil][PIPELINE_TRIANGLE];
	default:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		return im2DPipelines[blend][compare][depth][stencil][PIPELINE_TRIANGLE];
	}
}

static void
drawImmediate(PrimitiveType type, Im2DVertex *vertices, int32 numVertices,
	          uint16 *indices, int32 numIndices)
{
	if(!immediateReady || vertices == nil || numVertices <= 0)
		return;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return;
	bindCurrentColorTarget();

	uint16 *fanIndices = nil;
	if(type == PRIMTYPETRIFAN && numVertices >= 3){
		int32 sourceCount = indices ? numIndices : numVertices;
		int32 triangleCount = sourceCount - 2;
		fanIndices = rwNewT(uint16, triangleCount*3,
		                    MEMDUR_EVENT | ID_DRIVER);
		for(int32 i = 0; i < triangleCount; i++){
			fanIndices[i*3] = indices ? indices[0] : 0;
			fanIndices[i*3+1] = indices ? indices[i+1] : (uint16)(i+1);
			fanIndices[i*3+2] = indices ? indices[i+2] : (uint16)(i+2);
		}
		indices = fanIndices;
		numIndices = triangleCount*3;
		type = PRIMTYPETRILIST;
	}

	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	memset(&indexView, 0, sizeof(indexView));
	if(!uploadImmediate(vertices, numVertices*sizeof(Im2DVertex), indices,
	                    indices ? numIndices*sizeof(uint16) : 0,
	                    &vertexView, &indexView)){
		rwFree(fanIndices);
		return;
	}
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12PipelineState *pipeline = selectPipeline(type, &topology);
	resetWorldDrawState();
	list->SetGraphicsRootSignature(immediateRootSignature);
	list->SetPipelineState(pipeline);
	list->OMSetStencilRef((UINT)(uintptr_t)renderStates[STENCILFUNCTIONREF] & 0xFFu);
	list->IASetPrimitiveTopology(topology);
	list->IASetVertexBuffers(0, 1, &vertexView);
	if(indices)
		list->IASetIndexBuffer(&indexView);
	int32 width, height;
	getCurrentRenderTargetSize(&width, &height);
	float screen[2] = {
		(float)(width > 0 ? width : 1), (float)(height > 0 ? height : 1)
	};
	list->SetGraphicsRoot32BitConstants(0, 2, screen, 0);
	Raster *raster = (Raster*)renderStates[TEXTURERASTER];
	D3D12_GPU_DESCRIPTOR_HANDLE texture;
	if(!getTextureView(raster, &texture, nil))
		getTextureView(immediateWhiteRaster, &texture, nil);
	D3D12_GPU_DESCRIPTOR_HANDLE sampler;
	if(!getActiveSampler(Texture::CLAMP, &sampler)){
		rwFree(fanIndices);
		return;
	}
	list->SetGraphicsRootDescriptorTable(1, texture);
	list->SetGraphicsRootDescriptorTable(2, sampler);
	if(indices)
		list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	else
		list->DrawInstanced(numVertices, 1, 0, 0);
	rwFree(fanIndices);
}

bool32
renderScreenDroplets(void *vertices, int32 numVertices, int32 vertexStride,
	                 void *indices, int32 numIndices,
	                 Raster *maskRaster, Raster *sceneRaster)
{
	if(!immediateReady || screenDropletPipeline == nil ||
	   vertices == nil || indices == nil || numVertices <= 0 ||
	   numIndices <= 0 || vertexStride < 36)
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return 0;
	D3D12_GPU_DESCRIPTOR_HANDLE maskView, sceneView;
	if(!getTextureView(maskRaster, &maskView, nil) ||
	   !getTextureView(sceneRaster, &sceneView, nil))
		return 0;
	uint64 vertexAddress, indexAddress;
	uint32 vertexBytes = numVertices*vertexStride;
	uint32 indexBytes = numIndices*sizeof(uint16);
	if(!allocateUpload(vertices, vertexBytes, &vertexAddress) ||
	   !allocateUpload(indices, indexBytes, &indexAddress))
		return 0;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	vertexView.BufferLocation = vertexAddress;
	vertexView.SizeInBytes = vertexBytes;
	vertexView.StrideInBytes = vertexStride;
	D3D12_INDEX_BUFFER_VIEW indexView;
	indexView.BufferLocation = indexAddress;
	indexView.SizeInBytes = indexBytes;
	indexView.Format = DXGI_FORMAT_R16_UINT;
	resetWorldDrawState();
	list->SetGraphicsRootSignature(screenDropletRootSignature);
	list->SetPipelineState(screenDropletPipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->IASetVertexBuffers(0, 1, &vertexView);
	list->IASetIndexBuffer(&indexView);
	int32 width, height;
	getCurrentRenderTargetSize(&width, &height);
	float screen[2] = {
		(float)(width > 0 ? width : 1), (float)(height > 0 ? height : 1)
	};
	list->SetGraphicsRoot32BitConstants(0, 2, screen, 0);
	list->SetGraphicsRootDescriptorTable(1, maskView);
	list->SetGraphicsRootDescriptorTable(2, sceneView);
	list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	return 1;
}

bool32
renderModernRain(float intensity, float wind, uint32 timeMilliseconds,
	             int32 quality, int32 densityPercent,
	             const float cameraPosition[3])
{
	if(!immediateReady || cameraPosition == nil || intensity <= 0.0f)
		return 0;
	// This renderer is strictly opt-in. Do not compile shaders or allocate its
	// PSO until modern rain is selected and a rainy frame actually reaches us.
	if(modernRainPipeline == nil || modernRainRootSignature == nil){
		if(modernRainCreationAttempted)
			return 0;
		modernRainCreationAttempted = 1;
		if(!createModernRainResources(getDevice())){
			fprintf(stderr, "librw D3D12 modern rain pipeline unavailable; using classic rain\n");
			return 0;
		}
	}
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(list == nil || camera == nil)
		return 0;
	bindCurrentColorTarget();
	struct RainConstants {
		float view[16];
		float projection[16];
		float cameraIntensity[4];
		float windTimeQuality[4];
	} constants;
	static_assert(sizeof(RainConstants) == 40*sizeof(float),
		"modern rain root constants must match HLSL");
	memcpy(constants.view, &camera->devView, sizeof(constants.view));
	memcpy(constants.projection, &camera->devProj, sizeof(constants.projection));
	constants.cameraIntensity[0] = cameraPosition[0];
	constants.cameraIntensity[1] = cameraPosition[1];
	constants.cameraIntensity[2] = cameraPosition[2];
	constants.cameraIntensity[3] = intensity > 2.0f ? 2.0f : intensity;
	// Vice City's wind has no direction vector. Match its particle convention:
	// rain travels diagonally toward negative X/Y, with stronger hurricane slant.
	constants.windTimeQuality[0] = -wind*0.75f;
	constants.windTimeQuality[1] = -wind*0.75f;
	constants.windTimeQuality[2] = timeMilliseconds/1000.0f;
	constants.windTimeQuality[3] = (float)quality;
	if(densityPercent < 25) densityPercent = 25;
	if(densityPercent > 300) densityPercent = 300;
	const int32 baseInstanceCount = quality >= 3 ? 72*72 : 56*56;
	const int32 instanceCount = baseInstanceCount*densityPercent/100;
	resetWorldDrawState();
	list->SetGraphicsRootSignature(modernRainRootSignature);
	list->SetPipelineState(modernRainPipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->IASetVertexBuffers(0, 0, nil);
	list->IASetIndexBuffer(nil);
	list->SetGraphicsRoot32BitConstants(0, 40, &constants, 0);
	list->DrawInstanced(6, instanceCount, 0, 0);
	return 1;
}

bool32
renderPostFX(void *vertices, int32 numVertices, int32 vertexStride,
	         void *indices, int32 numIndices, Raster *sceneRaster,
	         uint32 effectMode, const float effect0[4], const float effect1[4])
{
	if(!immediateReady || postFXPipeline == nil || vertices == nil ||
	   indices == nil || numVertices <= 0 || numIndices <= 0 ||
	   vertexStride < (int32)sizeof(Im2DVertex) || effect0 == nil ||
	   effect1 == nil)
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return 0;
	D3D12_GPU_DESCRIPTOR_HANDLE sceneView;
	if(!getTextureView(sceneRaster, &sceneView, nil))
		return 0;
	uint64 vertexAddress, indexAddress;
	uint32 vertexBytes = numVertices*vertexStride;
	uint32 indexBytes = numIndices*sizeof(uint16);
	if(!allocateUpload(vertices, vertexBytes, &vertexAddress) ||
	   !allocateUpload(indices, indexBytes, &indexAddress))
		return 0;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	vertexView.BufferLocation = vertexAddress;
	vertexView.SizeInBytes = vertexBytes;
	vertexView.StrideInBytes = vertexStride;
	D3D12_INDEX_BUFFER_VIEW indexView;
	indexView.BufferLocation = indexAddress;
	indexView.SizeInBytes = indexBytes;
	indexView.Format = DXGI_FORMAT_R16_UINT;
	resetWorldDrawState();
	list->SetGraphicsRootSignature(postFXRootSignature);
	list->SetPipelineState(postFXPipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->IASetVertexBuffers(0, 1, &vertexView);
	list->IASetIndexBuffer(&indexView);
	struct PostFXConstants {
		float screenSize[2];
		uint32 effectMode;
		float unusedValue;
		float effect0[4];
		float effect1[4];
	} constants;
	int32 width, height;
	getCurrentRenderTargetSize(&width, &height);
	constants.screenSize[0] = (float)(width > 0 ? width : 1);
	constants.screenSize[1] = (float)(height > 0 ? height : 1);
	constants.effectMode = effectMode;
	constants.unusedValue = 0.0f;
	memcpy(constants.effect0, effect0, sizeof(constants.effect0));
	memcpy(constants.effect1, effect1, sizeof(constants.effect1));
	list->SetGraphicsRoot32BitConstants(0, 12, &constants, 0);
	list->SetGraphicsRootDescriptorTable(1, sceneView);
	list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	return 1;
}

bool32
resolveRasterToExternal(Raster *source, ID3D12Resource *destination,
	                    int32 width, int32 height,
	                    float32 uvScaleX, float32 uvScaleY,
	                    float32 uvOffsetX, float32 uvOffsetY,
	                    bool32 fxaaEnabled, uint32 colorMode,
	                    const float32 blurColor[4],
	                    const float32 contrastMult[3],
	                    const float32 contrastAdd[3])
{
	if(!immediateReady || openXRResolvePipeline == nil ||
	   openXRResolveRootSignature == nil || source == nil ||
	   destination == nil || width <= 0 || height <= 0 ||
	   blurColor == nil || contrastMult == nil || contrastAdd == nil)
		return 0;
	Raster *sourceParent = source->parent ? source->parent : source;
	if(sourceParent->width <= 0 || sourceParent->height <= 0 ||
	   source->width <= 0 || source->height <= 0)
		return 0;
	// The source may be one eye inside a shared double-wide render target.
	// Compose the eye-local OpenXR UV remap with that sub-rectangle before the
	// shader samples the parent SRV.
	const float32 regionScaleX = (float32)source->width/(float32)sourceParent->width;
	const float32 regionScaleY = (float32)source->height/(float32)sourceParent->height;
	const float32 regionOffsetX = (float32)source->offsetX/(float32)sourceParent->width;
	const float32 regionOffsetY = (float32)source->offsetY/(float32)sourceParent->height;
	uvScaleX *= regionScaleX;
	uvScaleY *= regionScaleY;
	uvOffsetX = regionOffsetX + uvOffsetX*regionScaleX;
	uvOffsetY = regionOffsetY + uvOffsetY*regionScaleY;
	ID3D12Device *device = getDevice();
	ID3D12GraphicsCommandList *list = getCommandList();
	if(device == nil || list == nil)
		return 0;
	if(!transitionRaster(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
		return 0;
	D3D12_GPU_DESCRIPTOR_HANDLE sourceView;
	if(!getTextureView(source, &sourceView, nil))
		return 0;
	D3D12_CPU_DESCRIPTOR_HANDLE targetView;
	uint32 targetViewIndex = UINT32_MAX;
	if(!allocateRenderTargetDescriptor(&targetView, &targetViewIndex))
		return 0;
	D3D12_RENDER_TARGET_VIEW_DESC viewDesc;
	memset(&viewDesc, 0, sizeof(viewDesc));
	viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(destination, &viewDesc, targetView);

	// XrSwapchainImageD3D12KHR color images are acquired in RENDER_TARGET and
	// must be released in RENDER_TARGET.  Do not transition them through COMMON:
	// that is undefined by XR_KHR_D3D12_enable and VDXR rejects the session.
	list->OMSetRenderTargets(1, &targetView, FALSE, nil);
	D3D12_VIEWPORT viewport = {
		0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f
	};
	D3D12_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	ID3D12DescriptorHeap *heap = getShaderResourceHeap();
	if(heap)
	resetWorldDrawState();
		list->SetDescriptorHeaps(1, &heap);
	list->SetGraphicsRootSignature(openXRResolveRootSignature);
	list->SetPipelineState(openXRResolvePipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	struct ResolveConstants {
		float uvScale[2];
		float uvOffset[2];
		float inverseSourceSize[2];
		uint32 fxaaEnabled;
		uint32 colorMode;
		float inverseOutputSize[2];
		float resolvePadding[2];
		float blurColor[4];
		float contrastMult[4];
		float contrastAdd[4];
	} constants = {};
	constants.uvScale[0] = uvScaleX;
	constants.uvScale[1] = uvScaleY;
	constants.uvOffset[0] = uvOffsetX;
	constants.uvOffset[1] = uvOffsetY;
	constants.inverseSourceSize[0] = 1.0f/(float)sourceParent->width;
	constants.inverseSourceSize[1] = 1.0f/(float)sourceParent->height;
	constants.inverseOutputSize[0] = 1.0f/(float)width;
	constants.inverseOutputSize[1] = 1.0f/(float)height;
	constants.fxaaEnabled = fxaaEnabled != 0;
	constants.colorMode = colorMode;
	memcpy(constants.blurColor, blurColor, sizeof(constants.blurColor));
	memcpy(constants.contrastMult, contrastMult, 3*sizeof(float));
	memcpy(constants.contrastAdd, contrastAdd, 3*sizeof(float));
	list->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);
	list->SetGraphicsRootDescriptorTable(1, sourceView);
	list->DrawInstanced(3, 1, 0, 0);
	deferDescriptorRelease(UINT32_MAX, targetViewIndex, UINT32_MAX);
	return 1;
}

bool32
resolveTextureToExternal(ID3D12Resource *source,
	                     D3D12_GPU_DESCRIPTOR_HANDLE sourceView,
	                     ID3D12Resource *destination,
	                     int32 sourceWidth, int32 sourceHeight,
	                     int32 width, int32 height,
	                     float32 uvScaleX, float32 uvScaleY,
	                     float32 uvOffsetX, float32 uvOffsetY,
	                     bool32 fxaaEnabled, uint32 colorMode,
	                     const float32 blurColor[4],
	                     const float32 contrastMult[3],
	                     const float32 contrastAdd[3])
{
	if(!immediateReady || openXRResolvePipeline == nil ||
	   openXRResolveRootSignature == nil || source == nil ||
	   sourceView.ptr == 0 || destination == nil ||
	   sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0 ||
	   blurColor == nil || contrastMult == nil || contrastAdd == nil)
		return 0;
	ID3D12Device *device = getDevice();
	ID3D12GraphicsCommandList *list = getCommandList();
	if(device == nil || list == nil)
		return 0;
	D3D12_CPU_DESCRIPTOR_HANDLE targetView;
	uint32 targetViewIndex = UINT32_MAX;
	if(!allocateRenderTargetDescriptor(&targetView, &targetViewIndex))
		return 0;
	D3D12_RENDER_TARGET_VIEW_DESC viewDesc;
	memset(&viewDesc, 0, sizeof(viewDesc));
	viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(destination, &viewDesc, targetView);

	// The OpenXR runtime transfers this image to us in RENDER_TARGET state and
	// requires the same state back when xrReleaseSwapchainImage is called.
	list->OMSetRenderTargets(1, &targetView, FALSE, nil);
	D3D12_VIEWPORT viewport = {
		0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f
	};
	D3D12_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	ID3D12DescriptorHeap *heap = getShaderResourceHeap();
	if(heap)
	resetWorldDrawState();
		list->SetDescriptorHeaps(1, &heap);
	list->SetGraphicsRootSignature(openXRResolveRootSignature);
	list->SetPipelineState(openXRResolvePipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	struct ResolveConstants {
		float uvScale[2];
		float uvOffset[2];
		float inverseSourceSize[2];
		uint32 fxaaEnabled;
		uint32 colorMode;
		float inverseOutputSize[2];
		float resolvePadding[2];
		float blurColor[4];
		float contrastMult[4];
		float contrastAdd[4];
	} constants = {};
	constants.uvScale[0] = uvScaleX;
	constants.uvScale[1] = uvScaleY;
	constants.uvOffset[0] = uvOffsetX;
	constants.uvOffset[1] = uvOffsetY;
	constants.inverseSourceSize[0] = 1.0f/(float)sourceWidth;
	constants.inverseSourceSize[1] = 1.0f/(float)sourceHeight;
	constants.inverseOutputSize[0] = 1.0f/(float)width;
	constants.inverseOutputSize[1] = 1.0f/(float)height;
	constants.fxaaEnabled = fxaaEnabled != 0;
	constants.colorMode = colorMode;
	memcpy(constants.blurColor, blurColor, sizeof(constants.blurColor));
	memcpy(constants.contrastMult, contrastMult, 3*sizeof(float));
	memcpy(constants.contrastAdd, contrastAdd, 3*sizeof(float));
	list->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);
	list->SetGraphicsRootDescriptorTable(1, sourceView);
	list->DrawInstanced(3, 1, 0, 0);
	deferDescriptorRelease(UINT32_MAX, targetViewIndex, UINT32_MAX);
	return 1;
}

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	uint16 indices[2] = { (uint16)vert1, (uint16)vert2 };
	drawImmediate(PRIMTYPELINELIST, (Im2DVertex*)vertices, numVertices,
	              indices, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
	               int32 vert2, int32 vert3)
{
	uint16 indices[3] = {
		(uint16)vert1, (uint16)vert2, (uint16)vert3
	};
	drawImmediate(PRIMTYPETRILIST, (Im2DVertex*)vertices, numVertices,
	              indices, 3);
}

void
im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices)
{
	drawImmediate(type, (Im2DVertex*)vertices, numVertices, nil, 0);
}

void
im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices,
	                       int32 numVertices, void *indices, int32 numIndices)
{
	drawImmediate(type, (Im2DVertex*)vertices, numVertices,
	              (uint16*)indices, numIndices);
}

static void
fillMatrix(float *dst, const Matrix *matrix)
{
	dst[0] = matrix->right.x; dst[1] = matrix->right.y;
	dst[2] = matrix->right.z; dst[3] = 0.0f;
	dst[4] = matrix->up.x; dst[5] = matrix->up.y;
	dst[6] = matrix->up.z; dst[7] = 0.0f;
	dst[8] = matrix->at.x; dst[9] = matrix->at.y;
	dst[10] = matrix->at.z; dst[11] = 0.0f;
	dst[12] = matrix->pos.x; dst[13] = matrix->pos.y;
	dst[14] = matrix->pos.z; dst[15] = 1.0f;
}

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	num3DVertices = 0;
	if(!immediateReady || vertices == nil || numVertices <= 0)
		return;
	uint64 address;
	uint32 size = numVertices*sizeof(Im3DVertex);
	if(!allocateUpload(vertices, size, &address))
		return;
	im3DVertexView.BufferLocation = address;
	im3DVertexView.SizeInBytes = size;
	im3DVertexView.StrideInBytes = sizeof(Im3DVertex);
	num3DVertices = numVertices;
	im3DFlags = flags;
	if(world)
		fillMatrix(im3DWorld, world);
	else{
		memset(im3DWorld, 0, sizeof(im3DWorld));
		im3DWorld[0] = im3DWorld[5] = im3DWorld[10] = im3DWorld[15] = 1.0f;
	}
	if((flags & im3d::VERTEXUV) == 0)
		renderStates[TEXTURERASTER] = nil;
}

static ID3D12PipelineState*
selectIm3DPipeline(PrimitiveType type,
	               D3D12_PRIMITIVE_TOPOLOGY *topology)
{
	uint32 blend = getActiveBlendMode();
	bool32 depthTest = renderStates[ZTESTENABLE] != nil;
	bool32 depthWrite = renderStates[ZWRITEENABLE] != nil;
	uint32 depth = depthTest ?
		(depthWrite ? DEPTH_TEST_WRITE : DEPTH_TEST_ONLY) :
		(depthWrite ? DEPTH_WRITE_ONLY : DEPTH_DISABLED);
	uint32 stencil = getActiveStencilMode();
	switch(type){
	case PRIMTYPELINELIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		return im3DPipelines[blend][depth][stencil][PIPELINE_LINE];
	case PRIMTYPEPOLYLINE:
		*topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		return im3DPipelines[blend][depth][stencil][PIPELINE_LINE];
	case PRIMTYPEPOINTLIST:
		*topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		return im3DPipelines[blend][depth][stencil][PIPELINE_POINT];
	case PRIMTYPETRISTRIP:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		return im3DPipelines[blend][depth][stencil][PIPELINE_TRIANGLE];
	default:
		*topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		return im3DPipelines[blend][depth][stencil][PIPELINE_TRIANGLE];
	}
}

static void
drawIm3D(PrimitiveType type, uint16 *indices, int32 numIndices)
{
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(!immediateReady || list == nil || camera == nil || num3DVertices <= 0)
		return;
	bindCurrentColorTarget();

	uint16 *fanIndices = nil;
	if(type == PRIMTYPETRIFAN){
		int32 sourceCount = indices ? numIndices : num3DVertices;
		if(sourceCount < 3)
			return;
		int32 triangleCount = sourceCount - 2;
		fanIndices = rwNewT(uint16, triangleCount*3,
		                    MEMDUR_EVENT | ID_DRIVER);
		for(int32 i = 0; i < triangleCount; i++){
			fanIndices[i*3] = indices ? indices[0] : 0;
			fanIndices[i*3+1] = indices ? indices[i+1] : (uint16)(i+1);
			fanIndices[i*3+2] = indices ? indices[i+2] : (uint16)(i+2);
		}
		indices = fanIndices;
		numIndices = triangleCount*3;
		type = PRIMTYPETRILIST;
	}

	D3D12_INDEX_BUFFER_VIEW indexView;
	if(indices){
		uint64 address;
		if(!allocateUpload(indices, numIndices*sizeof(uint16), &address)){
			rwFree(fanIndices);
			return;
		}
		indexView.BufferLocation = address;
		indexView.SizeInBytes = numIndices*sizeof(uint16);
		indexView.Format = DXGI_FORMAT_R16_UINT;
	}
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12PipelineState *pipeline = selectIm3DPipeline(type, &topology);
	resetWorldDrawState();
	list->SetGraphicsRootSignature(im3DRootSignature);
	list->SetPipelineState(pipeline);
	list->OMSetStencilRef((UINT)(uintptr_t)renderStates[STENCILFUNCTIONREF] & 0xFFu);
	list->IASetPrimitiveTopology(topology);
	list->IASetVertexBuffers(0, 1, &im3DVertexView);
	if(indices)
		list->IASetIndexBuffer(&indexView);
	float constants[55];
	memset(constants, 0, sizeof(constants));
	memcpy(constants, im3DWorld, 16*sizeof(float));
	memcpy(constants + 16, &camera->devView, 16*sizeof(float));
	memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
	Raster *raster = (Raster*)renderStates[TEXTURERASTER];
	D3D12_GPU_DESCRIPTOR_HANDLE texture;
	bool32 textured = (im3DFlags & im3d::VERTEXUV) &&
		getTextureView(raster, &texture, nil);
	if(!textured)
		getTextureView(immediateWhiteRaster, &texture, nil);
	D3D12_GPU_DESCRIPTOR_HANDLE sampler;
	if(!getActiveSampler(Texture::WRAP, &sampler)){
		rwFree(fanIndices);
		return;
	}
	D3D12_GPU_DESCRIPTOR_HANDLE reflectionTexture;
	D3D12_GPU_DESCRIPTOR_HANDLE reflectionSampler;
	D3D12_GPU_VIRTUAL_ADDRESS reflectionConstants;
	if(!getScreenSpaceReflectionBindings(&reflectionTexture,
	       &reflectionSampler, &reflectionConstants)){
		rwFree(fanIndices);
		return;
	}
	constants[48] = textured ? 1.0f : 0.0f;
	constants[49] = (uint32)(uintptr_t)getRenderState(ALPHATESTREF)/255.0f;
	constants[50] = (float)(uint32)(uintptr_t)getRenderState(ALPHATESTFUNC);
	constants[51] = isIm3DWater() ? 1.0f : 0.0f;
	if(getRenderState(FOGENABLE) != nil &&
	   camera->fogPlane < camera->farPlane){
		constants[52] = camera->farPlane;
		constants[53] = 1.0f/(camera->fogPlane - camera->farPlane);
	}
	uint32 packedFog = (uint32)(uintptr_t)getRenderState(FOGCOLOR);
	memcpy(&constants[54], &packedFog, sizeof(packedFog));
	list->SetGraphicsRoot32BitConstants(0, 55, constants, 0);
	list->SetGraphicsRootDescriptorTable(1, texture);
	list->SetGraphicsRootDescriptorTable(2, sampler);
	list->SetGraphicsRootDescriptorTable(3, reflectionTexture);
	list->SetGraphicsRootConstantBufferView(4, reflectionConstants);
	list->SetGraphicsRootDescriptorTable(5, reflectionSampler);
	if(indices)
		list->DrawIndexedInstanced(numIndices, 1, 0, 0, 0);
	else
		list->DrawInstanced(num3DVertices, 1, 0, 0);
	rwFree(fanIndices);
}

void
im3DRenderPrimitive(PrimitiveType type)
{
	drawIm3D(type, nil, 0);
}

void
im3DRenderIndexedPrimitive(PrimitiveType type, void *indices,
	                       int32 numIndices)
{
	drawIm3D(type, (uint16*)indices, numIndices);
}

void
im3DEnd(void)
{
	num3DVertices = 0;
}

#else

bool32 initializeImmediate(void) { return 0; }
void shutdownImmediate(void) { }
void setRenderState(int32, void*) { }
void *getRenderState(int32) { return nil; }
void setImmediate2DStrictDepth(bool32) { }
void im2DRenderLine(void*, int32, int32, int32) { }
void im2DRenderTriangle(void*, int32, int32, int32, int32) { }
void im2DRenderPrimitive(PrimitiveType, void*, int32) { }
void im2DRenderIndexedPrimitive(PrimitiveType, void*, int32, void*, int32) { }
void im3DTransform(void*, int32, Matrix*, uint32) { }
void im3DRenderPrimitive(PrimitiveType) { }
void im3DRenderIndexedPrimitive(PrimitiveType, void*, int32) { }
void im3DEnd(void) { }

#endif

}
}
