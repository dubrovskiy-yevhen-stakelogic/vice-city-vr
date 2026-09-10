#pragma once

#include <d3d12.h>

namespace Dlaa
{
// This NR runtime rejects a nonzero color X origin even when the source
// subrectangle fits. Give each eye a local color texture before evaluation.
// The caller keeps the shared source in NON_PIXEL_SHADER_RESOURCE state.
inline bool CopyNeuralColorInput(ID3D12GraphicsCommandList *list,
	ID3D12Resource *source, ID3D12Resource *destination,
	D3D12_RESOURCE_STATES &destinationState,
	UINT sourceLeft, UINT width, UINT height, UINT sourceTop = 0)
{
	if(!list || !source || !destination || source == destination ||
	   width == 0 || height == 0)
		return false;
	const D3D12_RESOURCE_DESC src = source->GetDesc();
	const D3D12_RESOURCE_DESC dst = destination->GetDesc();
	if(src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
	   dst.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
	   src.SampleDesc.Count != 1 || dst.SampleDesc.Count != 1 ||
	   (UINT64)sourceLeft+width > src.Width || (UINT64)sourceTop+height > src.Height ||
	   dst.Width != width || dst.Height != height ||
	   src.Format != dst.Format)
		return false;
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = source;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = destination;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[1].Transition.StateBefore = destinationState;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	list->ResourceBarrier(destinationState == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2, barriers);
	D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
	from.pResource = source;
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	to.pResource = destination;
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const D3D12_BOX box = { sourceLeft, sourceTop, 0, sourceLeft+width, sourceTop+height, 1 };
	list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	list->ResourceBarrier(2, barriers);
	destinationState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	return true;
}
}
