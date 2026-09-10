#pragma once

#include "DlssNrStereoShared.h"

namespace Dlaa
{
class SharedStereoBackend
{
	ID3D12RootSignature *root;
	ID3D12PipelineState *pipeline;
	ID3D12DescriptorHeap *descriptors;
	ID3D12Resource *boundColor, *boundDepth, *boundModel;
	UINT descriptorSize;
	DlssNrStereoShared::Constants constants;

	void ReleaseBindings()
	{
		if(descriptors) rw::d3d12::deferRelease(descriptors);
		if(boundColor) rw::d3d12::deferRelease(boundColor);
		if(boundDepth) rw::d3d12::deferRelease(boundDepth);
		if(boundModel) rw::d3d12::deferRelease(boundModel);
		descriptors = nullptr;
		boundColor = boundDepth = boundModel = nullptr;
	}

	bool CreatePipeline(ID3D12Device *device)
	{
		if(root && pipeline) return true;
		ID3DBlob *shader = nullptr, *errors = nullptr, *signature = nullptr;
		HRESULT hr = D3DCompile(DlssNrStereoShared::kShader,
			strlen(DlssNrStereoShared::kShader), "vice_city_nr_shared_stereo",
			nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0, &shader, &errors);
		if(errors) errors->Release();
		if(FAILED(hr)){
			if(shader) shader->Release();
			return false;
		}
		D3D12_DESCRIPTOR_RANGE ranges[2] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 3;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		D3D12_ROOT_PARAMETER parameters[3] = {};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants.Num32BitValues = sizeof(constants)/sizeof(uint32_t);
		for(int i = 0; i < 2; ++i){
			parameters[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[i+1].DescriptorTable.NumDescriptorRanges = 1;
			parameters[i+1].DescriptorTable.pDescriptorRanges = &ranges[i];
		}
		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = 3;
		desc.pParameters = parameters;
		hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
			&signature, nullptr);
		if(SUCCEEDED(hr))
			hr = device->CreateRootSignature(0, signature->GetBufferPointer(),
				signature->GetBufferSize(), IID_PPV_ARGS(&root));
		if(signature) signature->Release();
		if(SUCCEEDED(hr)){
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
			pso.pRootSignature = root;
			pso.CS.pShaderBytecode = shader->GetBufferPointer();
			pso.CS.BytecodeLength = shader->GetBufferSize();
			hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
		}
		shader->Release();
		if(FAILED(hr)){
			if(root) root->Release();
			root = nullptr;
			return false;
		}
		return true;
	}

	static bool Invert(const float *values, sl::float4x4 &inverse)
	{
		for(int i = 0; i < 16; ++i)
			if(!isfinite(values[i])) return false;
		sl::float4x4 matrix, product;
		memcpy(&matrix[0].x, values, sizeof(matrix));
		sl::matrixFullInvert(inverse, matrix);
		sl::matrixMul(product, matrix, inverse);
		const float *check = &product[0].x;
		for(int i = 0; i < 16; ++i)
			if(!isfinite(check[i]) || fabsf(check[i] - (i%5 == 0 ? 1.f : 0.f)) > 0.01f)
				return false;
		return true;
	}

	static bool ValidView(const float *m)
	{
		for(int i = 0; i < 3; ++i){
			if(fabsf(m[i*4+3]) > 0.001f) return false;
			for(int j = 0; j < 3; ++j){
				float dot = 0.f;
				for(int k = 0; k < 3; ++k) dot += m[i*4+k]*m[j*4+k];
				if(fabsf(dot - (i == j ? 1.f : 0.f)) > 0.01f) return false;
			}
		}
		return fabsf(m[15]-1.f) < 0.001f;
	}

	bool BuildConstants(const EyeInput &left, const EyeInput &right, float jitterX, float jitterY)
	{
		sl::float4x4 camera[2], clipToView[2], projection[2];
		const EyeInput *eyes[2] = { &left, &right };
		for(int i = 0; i < 2; ++i){
			if(!Invert(eyes[i]->view, camera[i]) || !ValidView(eyes[i]->view) ||
			   !Invert(eyes[i]->projection, clipToView[i])) return false;
			memcpy(&projection[i][0].x, eyes[i]->projection, sizeof(projection[i]));
			// Standard asymmetric perspective is supported; oblique depth is not.
			const float *inv = &clipToView[i][0].x;
			if(fabsf(inv[2])+fabsf(inv[3])+fabsf(inv[6])+fabsf(inv[7]) > 0.0001f)
				return false;
			float *depth = i == 0 ? constants.leftDepthUnproject : constants.rightDepthUnproject;
			depth[0] = inv[10]; depth[1] = inv[14];
			depth[2] = inv[11]; depth[3] = inv[15];
		}
		for(int eye = 0; eye < 2; ++eye){
			sl::float4x4 viewToOther, clipToOtherView, clipToOtherClip;
			// Camera-centred multiplication avoids subtracting large world positions in the shader.
			sl::calcCameraToPrevCamera(viewToOther, camera[eye], camera[1-eye]);
			sl::matrixMul(clipToOtherView, clipToView[eye], viewToOther);
			sl::matrixMul(clipToOtherClip, clipToOtherView, projection[1-eye]);
			float *target = eye == 0 ? constants.leftClipToRightClip : constants.rightClipToLeftClip;
			memcpy(target, &clipToOtherClip[0].x, sizeof(clipToOtherClip));
			for(int i = 0; i < 16; ++i) if(!isfinite(target[i])) return false;
		}
		constants.width = left.width; constants.height = left.height;
		constants.leftOffset = left.sourceLeft; constants.rightOffset = right.sourceLeft;
		constants.jitterX = jitterX; constants.jitterY = jitterY;
		constants.depthAbsoluteTolerance = 0.035f;
		constants.depthRelativeTolerance = 0.01f;
		constants.colorTolerance = 0.20f;
		constants.nearFadeDistance = 0.5f;
		return isfinite(jitterX) && isfinite(jitterY);
	}

	bool CreateTargets(ID3D12Device *device, UINT width, UINT height)
	{
		if(output[0] && output[1] && output[0]->GetDesc().Width == width &&
		   output[0]->GetDesc().Height == height) return true;
		ReleaseTargets();
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap.CreationNodeMask = heap.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width; desc.Height = height;
		desc.DepthOrArraySize = desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		for(int eye = 0; eye < 2; ++eye){
			state[eye] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			   &desc, state[eye], nullptr, IID_PPV_ARGS(&output[eye])))){
				ReleaseTargets();
				return false;
			}
		}
		return true;
	}

	bool CreateBindings(ID3D12Device *device, ID3D12Resource *color,
		ID3D12Resource *depth, ID3D12Resource *model)
	{
		if(descriptors && boundColor == color && boundDepth == depth && boundModel == model)
			return true;
		ReleaseBindings();
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 5;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if(FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptors)))) return false;
		descriptorSize = device->GetDescriptorHandleIncrementSize(desc.Type);
		D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptors->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		device->CreateShaderResourceView(color, &srv, handle);
		handle.ptr += descriptorSize;
		srv.Format = depth->GetDesc().Format == DXGI_FORMAT_R24G8_TYPELESS ?
			DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
		device->CreateShaderResourceView(depth, &srv, handle);
		handle.ptr += descriptorSize;
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		device->CreateShaderResourceView(model, &srv, handle);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		for(int eye = 0; eye < 2; ++eye){
			handle.ptr += descriptorSize;
			device->CreateUnorderedAccessView(output[eye], nullptr, &uav, handle);
		}
		boundColor = color; boundDepth = depth; boundModel = model;
		boundColor->AddRef(); boundDepth->AddRef(); boundModel->AddRef();
		return true;
	}

	static bool ValidSurface(ID3D12Resource *resource, const EyeInput &eye, bool depth)
	{
		if(!resource) return false;
		const D3D12_RESOURCE_DESC d = resource->GetDesc();
		return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
			d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 &&
			(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
			(uint64_t)eye.sourceLeft + eye.width <= d.Width && eye.height <= d.Height &&
			(depth ? (d.Format == DXGI_FORMAT_R24G8_TYPELESS ||
			 d.Format == DXGI_FORMAT_R32_TYPELESS || d.Format == DXGI_FORMAT_R32_FLOAT) :
			 d.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
	}

public:
	ID3D12Resource *output[2];
	D3D12_RESOURCE_STATES state[2];
	EyeInput input[2];
	bool prepared, composed;
	const char *failure;

	SharedStereoBackend() : root(nullptr), pipeline(nullptr), descriptors(nullptr),
		boundColor(nullptr), boundDepth(nullptr), boundModel(nullptr), descriptorSize(0),
		constants{}, output{}, state{}, input{}, prepared(false), composed(false), failure(nullptr) {}

	void ResetFrame() { prepared = composed = false; }
	void ReleaseTargets()
	{
		ResetFrame();
		ReleaseBindings();
		for(int eye = 0; eye < 2; ++eye){
			if(output[eye]) rw::d3d12::deferRelease(output[eye]);
			output[eye] = nullptr;
		}
	}
	void Release()
	{
		ReleaseTargets();
		if(pipeline) rw::d3d12::deferRelease(pipeline);
		if(root) rw::d3d12::deferRelease(root);
		pipeline = nullptr; root = nullptr;
	}
	bool Matches(int eye, const EyeInput &value) const
	{
		if(!prepared || eye < 0 || eye > 1) return false;
		const EyeInput &saved = input[eye];
		return saved.color == value.color && saved.depth == value.depth &&
			saved.width == value.width && saved.height == value.height &&
			saved.sourceLeft == value.sourceLeft &&
			saved.outputWidth == value.outputWidth && saved.outputHeight == value.outputHeight &&
			memcmp(saved.view, value.view, sizeof(saved.view)) == 0 &&
			memcmp(saved.projection, value.projection, sizeof(saved.projection)) == 0;
	}
	bool Prepare(ID3D12Device *device, const EyeInput &left, const EyeInput &right,
		ID3D12Resource *model, float jitterX, float jitterY)
	{
		ResetFrame();
		failure = "invalid stereo inputs";
		if(!device || !model || !left.width || !left.height || left.color != right.color ||
		   left.depth != right.depth || left.width != right.width || left.height != right.height ||
		   left.outputWidth != right.outputWidth || left.outputHeight != right.outputHeight ||
		   (uint64_t)left.sourceLeft + left.width > right.sourceLeft) return false;
		ID3D12Resource *color = static_cast<ID3D12Resource*>(left.color);
		ID3D12Resource *depth = static_cast<ID3D12Resource*>(left.depth);
		if(!ValidSurface(color, left, false) || !ValidSurface(color, right, false) ||
		   !ValidSurface(depth, left, true) || !ValidSurface(depth, right, true)) return false;
		const D3D12_RESOURCE_DESC modelDesc = model->GetDesc();
		if(modelDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		   modelDesc.Width != left.width || modelDesc.Height != left.height ||
		   modelDesc.DepthOrArraySize != 1 || modelDesc.SampleDesc.Count != 1 ||
		   modelDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
		   (modelDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) return false;
		failure = "unsupported stereo matrices";
		if(!BuildConstants(left, right, jitterX, jitterY)) return false;
		failure = "shared stereo GPU resource creation failed";
		if(!CreatePipeline(device) || !CreateTargets(device, left.width, left.height) ||
		   !CreateBindings(device, color, depth, model)) return false;
		input[0] = left; input[1] = right;
		prepared = true;
		failure = nullptr;
		return true;
	}
	bool Compose(ID3D12GraphicsCommandList *list, ID3D12Resource *model,
		ID3D12DescriptorHeap *gameHeap)
	{
		failure = "shared stereo pair not prepared";
		if(!prepared || !list || !gameHeap || !descriptors || model != boundModel) return false;
		ID3D12DescriptorHeap *heaps[] = { descriptors };
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(root);
		list->SetPipelineState(pipeline);
		const D3D12_GPU_DESCRIPTOR_HANDLE srv = descriptors->GetGPUDescriptorHandleForHeapStart();
		for(int eye = 0; eye < 2; ++eye){
			if(state[eye] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
				D3D12_RESOURCE_BARRIER b = {};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource = output[eye];
				b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				b.Transition.StateBefore = state[eye];
				b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				list->ResourceBarrier(1, &b);
			}
			constants.targetEye = eye;
			list->SetComputeRoot32BitConstants(0, sizeof(constants)/sizeof(uint32_t), &constants, 0);
			list->SetComputeRootDescriptorTable(1, srv);
			D3D12_GPU_DESCRIPTOR_HANDLE uav = { srv.ptr + (uint64_t)(3+eye)*descriptorSize };
			list->SetComputeRootDescriptorTable(2, uav);
			list->Dispatch((constants.width+7)/8, (constants.height+7)/8, 1);
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = output[eye];
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			list->ResourceBarrier(1, &b);
			state[eye] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		}
		heaps[0] = gameHeap;
		list->SetDescriptorHeaps(1, heaps);
		rw::d3d12::resetWorldDrawState();
		composed = true;
		failure = nullptr;
		return true;
	}
};
}
