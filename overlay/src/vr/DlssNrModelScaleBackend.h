#pragma once

#include "DlssNrModelScale.h"
#include "DlssNrFoveation.h"

namespace Dlaa
{
class ModelScaleBackend
{
	ID3D12RootSignature *root;
	ID3D12PipelineState *downsamplePipeline, *composePipeline;
	ID3D12DescriptorHeap *descriptors;
	ID3D12Resource *boundColor, *boundProxy, *boundModel;
	UINT descriptorSize;
	DlssNrModelScale::Constants constants;

	void ReleaseBindings()
	{
		if(descriptors) rw::d3d12::deferRelease(descriptors);
		if(boundColor) rw::d3d12::deferRelease(boundColor);
		if(boundProxy) rw::d3d12::deferRelease(boundProxy);
		if(boundModel) rw::d3d12::deferRelease(boundModel);
		descriptors = nullptr;
		boundColor = boundProxy = boundModel = nullptr;
	}
	bool CreatePipeline(ID3D12Device *device)
	{
		if(root && downsamplePipeline && composePipeline) return true;
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
		ID3DBlob *signature = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr);
		if(SUCCEEDED(hr))
			hr = device->CreateRootSignature(0, signature->GetBufferPointer(),
				signature->GetBufferSize(), IID_PPV_ARGS(&root));
		if(signature) signature->Release();
		for(int i = 0; i < 2 && SUCCEEDED(hr); ++i){
			ID3DBlob *shader = nullptr, *errors = nullptr;
			hr = D3DCompile(DlssNrModelScale::kShader, strlen(DlssNrModelScale::kShader),
				"vice_city_nr_model_scale", nullptr, nullptr, i == 0 ? "Downsample" : "Compose",
				"cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
			if(errors) errors->Release();
			if(SUCCEEDED(hr)){
				D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
				pso.pRootSignature = root;
				pso.CS.pShaderBytecode = shader->GetBufferPointer();
				pso.CS.BytecodeLength = shader->GetBufferSize();
				ID3D12PipelineState **target = i == 0 ? &downsamplePipeline : &composePipeline;
				hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(target));
			}
			if(shader) shader->Release();
		}
		if(FAILED(hr)){
			if(composePipeline) composePipeline->Release();
			if(downsamplePipeline) downsamplePipeline->Release();
			if(root) root->Release();
			root = nullptr;
			downsamplePipeline = composePipeline = nullptr;
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
	bool CreateBindings(ID3D12Device *device, ID3D12Resource *color,
		ID3D12Resource *proxy, ID3D12Resource *model)
	{
		if(descriptors && boundColor == color && boundProxy == proxy && boundModel == model) return true;
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
		ID3D12Resource *inputs[] = {color, proxy, model};
		for(int i = 0; i < 3; ++i){
			device->CreateShaderResourceView(inputs[i], &srv, handle);
			handle.ptr += descriptorSize;
		}
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		device->CreateUnorderedAccessView(proxy, nullptr, &uav, handle);
		handle.ptr += descriptorSize;
		device->CreateUnorderedAccessView(output, nullptr, &uav, handle);
		boundColor = color; boundProxy = proxy; boundModel = model;
		boundColor->AddRef(); boundProxy->AddRef(); boundModel->AddRef();
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
	void Bind(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline, int target)
	{
		ID3D12DescriptorHeap *heaps[] = {descriptors};
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(root);
		list->SetPipelineState(pipeline);
		list->SetComputeRoot32BitConstants(0, sizeof(constants)/sizeof(uint32_t), &constants, 0);
		D3D12_GPU_DESCRIPTOR_HANDLE handle = descriptors->GetGPUDescriptorHandleForHeapStart();
		list->SetComputeRootDescriptorTable(1, handle);
		handle.ptr += (uint64_t)target * descriptorSize;
		list->SetComputeRootDescriptorTable(2, handle);
	}
	static void MoveTo(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
		D3D12_RESOURCE_STATES &before, D3D12_RESOURCE_STATES after)
	{
		if(before == after) return;
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = resource;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
		list->ResourceBarrier(1, &b);
		before = after;
	}
	static void Restore(ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *gameHeap)
	{
		ID3D12DescriptorHeap *heaps[] = {gameHeap};
		list->SetDescriptorHeaps(1, heaps);
		rw::d3d12::resetWorldDrawState();
	}

public:
	ID3D12Resource *output;
	D3D12_RESOURCE_STATES state;
	EyeInput input;
	DlssNrFoveation::Region region;
	bool prepared, downsampled, composed;
	const char *failure;

	ModelScaleBackend() : root(nullptr), downsamplePipeline(nullptr), composePipeline(nullptr),
		descriptors(nullptr), boundColor(nullptr), boundProxy(nullptr), boundModel(nullptr),
		descriptorSize(0), constants{}, output(nullptr), state{}, input{}, region{},
		prepared(false), downsampled(false), composed(false), failure(nullptr) {}
	void ResetFrame() { prepared = downsampled = composed = false; }
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
		if(composePipeline) rw::d3d12::deferRelease(composePipeline);
		if(downsamplePipeline) rw::d3d12::deferRelease(downsamplePipeline);
		if(root) rw::d3d12::deferRelease(root);
		root = nullptr;
		downsamplePipeline = composePipeline = nullptr;
	}
	bool Prepare(ID3D12Device *device, const EyeInput &original,
		const DlssNrFoveation::Region &crop, ID3D12Resource *proxy, ID3D12Resource *model)
	{
		ResetFrame();
		failure = "invalid model scale inputs";
		ID3D12Resource *color = static_cast<ID3D12Resource *>(original.color);
		if(!device || !original.width || !original.height || !crop.width || !crop.height ||
		   color == proxy || color == model || proxy == model || color == output || proxy == output ||
		   model == output || !ValidSurface(color) || !ValidSurface(proxy) || !ValidSurface(model) ||
		   (uint64_t)crop.left + crop.width > original.width ||
		   (uint64_t)crop.top + crop.height > original.height) return false;
		const D3D12_RESOURCE_DESC sourceDesc = color->GetDesc(), proxyDesc = proxy->GetDesc(), modelDesc = model->GetDesc();
		if((uint64_t)original.sourceLeft + original.width > sourceDesc.Width || original.height > sourceDesc.Height ||
		   proxyDesc.Width == 0 || proxyDesc.Height == 0 || proxyDesc.Width > crop.width || proxyDesc.Height > crop.height ||
		   proxyDesc.Width != modelDesc.Width || proxyDesc.Height != modelDesc.Height ||
		   (proxyDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 ||
		   (uint64_t)crop.width > proxyDesc.Width * 4 || (uint64_t)crop.height > (uint64_t)proxyDesc.Height * 4) return false;
		failure = "model scale GPU resource creation failed";
		if(!CreatePipeline(device) || !CreateTarget(device, crop.width, crop.height) ||
		   !CreateBindings(device, color, proxy, model)) return false;
		input = original;
		region = crop;
		constants.width = crop.width; constants.height = crop.height;
		constants.sourceLeft = original.sourceLeft + crop.left; constants.sourceTop = crop.top;
		constants.modelWidth = (uint32_t)proxyDesc.Width; constants.modelHeight = proxyDesc.Height;
		prepared = true;
		failure = nullptr;
		return true;
	}
	bool Downsample(ID3D12GraphicsCommandList *list, ID3D12Resource *proxy,
		D3D12_RESOURCE_STATES &proxyState, ID3D12DescriptorHeap *gameHeap)
	{
		failure = "model scale frame not prepared";
		if(!prepared || !list || !gameHeap || !descriptors || proxy != boundProxy) return false;
		MoveTo(list, proxy, proxyState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Bind(list, downsamplePipeline, 3);
		list->Dispatch((constants.modelWidth+7)/8, (constants.modelHeight+7)/8, 1);
		MoveTo(list, proxy, proxyState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Restore(list, gameHeap);
		downsampled = true;
		composed = false;
		failure = nullptr;
		return true;
	}
	bool Compose(ID3D12GraphicsCommandList *list, ID3D12Resource *model,
		ID3D12DescriptorHeap *gameHeap)
	{
		failure = "model scale proxy not ready";
		if(!prepared || !downsampled || !list || !gameHeap || !descriptors || model != boundModel) return false;
		MoveTo(list, output, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Bind(list, composePipeline, 4);
		list->Dispatch((constants.width+7)/8, (constants.height+7)/8, 1);
		MoveTo(list, output, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Restore(list, gameHeap);
		composed = true;
		failure = nullptr;
		return true;
	}
};
}
