#pragma once

#include "DlssNrFoveation.h"

namespace Dlaa
{
class FoveationBackend
{
	ID3D12RootSignature *root;
	ID3D12PipelineState *pipeline;
	ID3D12DescriptorHeap *descriptors;
	ID3D12Resource *boundColor, *boundModel;
	DlssNrFoveation::Constants constants;

	void ReleaseBindings()
	{
		if(descriptors) rw::d3d12::deferRelease(descriptors);
		if(boundColor) rw::d3d12::deferRelease(boundColor);
		if(boundModel) rw::d3d12::deferRelease(boundModel);
		descriptors = nullptr;
		boundColor = boundModel = nullptr;
	}
	bool CreatePipeline(ID3D12Device *device)
	{
		if(root && pipeline) return true;
		ID3DBlob *shader = nullptr, *errors = nullptr, *signature = nullptr;
		HRESULT hr = D3DCompile(DlssNrFoveation::kShader, strlen(DlssNrFoveation::kShader),
			"vice_city_nr_foveation", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
		if(errors) errors->Release();
		if(FAILED(hr)){
			if(shader) shader->Release();
			return false;
		}
		D3D12_DESCRIPTOR_RANGE ranges[2] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 2;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].OffsetInDescriptorsFromTableStart = 2;
		D3D12_ROOT_PARAMETER parameters[2] = {};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants.Num32BitValues = sizeof(constants)/sizeof(uint32_t);
		parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[1].DescriptorTable.NumDescriptorRanges = 2;
		parameters[1].DescriptorTable.pDescriptorRanges = ranges;
		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = 2;
		desc.pParameters = parameters;
		hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr);
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
	bool CreateTarget(ID3D12Device *device, UINT width, UINT height)
	{
		if(output && output->GetDesc().Width == width && output->GetDesc().Height == height) return true;
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
		state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			&desc, state, nullptr, IID_PPV_ARGS(&output)));
	}
	bool CreateBindings(ID3D12Device *device, ID3D12Resource *color, ID3D12Resource *model)
	{
		if(descriptors && boundColor == color && boundModel == model) return true;
		ReleaseBindings();
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 3;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if(FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptors)))) return false;
		const UINT size = device->GetDescriptorHandleIncrementSize(desc.Type);
		D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptors->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		device->CreateShaderResourceView(color, &srv, handle);
		handle.ptr += size;
		device->CreateShaderResourceView(model, &srv, handle);
		handle.ptr += size;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		device->CreateUnorderedAccessView(output, nullptr, &uav, handle);
		boundColor = color; boundModel = model;
		boundColor->AddRef(); boundModel->AddRef();
		return true;
	}
	static bool ValidSurface(ID3D12Resource *resource)
	{
		if(!resource) return false;
		const D3D12_RESOURCE_DESC d = resource->GetDesc();
		return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
			d.SampleDesc.Count == 1 && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
			(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
	}

public:
	ID3D12Resource *output;
	D3D12_RESOURCE_STATES state;
	EyeInput input;
	DlssNrFoveation::Region region;
	bool prepared, composed;
	const char *failure;

	FoveationBackend() : root(nullptr), pipeline(nullptr), descriptors(nullptr),
		boundColor(nullptr), boundModel(nullptr), constants{}, output(nullptr), state{},
		input{}, region{}, prepared(false), composed(false), failure(nullptr) {}
	void ResetFrame() { prepared = composed = false; }
	void ReleaseTargets()
	{
		ResetFrame();
		ReleaseBindings();
		if(output) rw::d3d12::deferRelease(output);
		output = nullptr;
	}
	void Release()
	{
		ReleaseTargets();
		if(pipeline) rw::d3d12::deferRelease(pipeline);
		if(root) rw::d3d12::deferRelease(root);
		pipeline = nullptr; root = nullptr;
	}
	bool Prepare(ID3D12Device *device, const EyeInput &original,
		ID3D12Resource *model, const DlssNrFoveation::Region &crop)
	{
		ResetFrame();
		failure = "invalid foveation inputs";
		ID3D12Resource *color = static_cast<ID3D12Resource *>(original.color);
		if(!device || !original.width || !original.height || color == output || model == output || !ValidSurface(color) ||
		   !ValidSurface(model) || !crop.width || !crop.height ||
		   (uint64_t)crop.left + crop.width > original.width ||
		   (uint64_t)crop.top + crop.height > original.height ||
		   !std::isfinite(crop.featherPixels) || crop.featherPixels < 0.f ||
		   crop.featherPixels > .5f * (crop.width < crop.height ? crop.width : crop.height)) return false;
		const D3D12_RESOURCE_DESC sourceDesc = color->GetDesc(), modelDesc = model->GetDesc();
		if((uint64_t)original.sourceLeft + original.width > sourceDesc.Width ||
		   original.height > sourceDesc.Height || modelDesc.Width != crop.width ||
		   modelDesc.Height != crop.height) return false;
		failure = "foveation GPU resource creation failed";
		if(!CreatePipeline(device) || !CreateTarget(device, original.width, original.height) ||
		   !CreateBindings(device, color, model)) return false;
		input = original;
		region = crop;
		constants.width = original.width; constants.height = original.height;
		constants.sourceLeft = original.sourceLeft;
		constants.cropLeft = crop.left; constants.cropTop = crop.top;
		constants.cropWidth = crop.width; constants.cropHeight = crop.height;
		constants.featherPixels = crop.featherPixels;
		prepared = true;
		failure = nullptr;
		return true;
	}
	bool Compose(ID3D12GraphicsCommandList *list, ID3D12Resource *model,
		ID3D12DescriptorHeap *gameHeap)
	{
		failure = "foveation frame not prepared";
		if(!prepared || !list || !gameHeap || !descriptors || model != boundModel) return false;
		if(state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = output;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = state;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			list->ResourceBarrier(1, &b);
		}
		ID3D12DescriptorHeap *heaps[] = {descriptors};
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(root);
		list->SetPipelineState(pipeline);
		list->SetComputeRoot32BitConstants(0, sizeof(constants)/sizeof(uint32_t), &constants, 0);
		list->SetComputeRootDescriptorTable(1, descriptors->GetGPUDescriptorHandleForHeapStart());
		list->Dispatch((constants.width+7)/8, (constants.height+7)/8, 1);
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = output;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		list->ResourceBarrier(1, &b);
		state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		heaps[0] = gameHeap;
		list->SetDescriptorHeaps(1, heaps);
		rw::d3d12::resetWorldDrawState();
		composed = true;
		failure = nullptr;
		return true;
	}
};
}
