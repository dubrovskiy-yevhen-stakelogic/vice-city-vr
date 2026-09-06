#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* operation)
{
    if(FAILED(hr)) { std::printf("FAIL %s: 0x%08X\n", operation, unsigned(hr)); std::exit(2); }
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 serial = 0;
    void Submit() {
        Check(list->Close(), "close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++serial), "signal");
        Check(fence->SetEventOnCompletion(serial, event), "fence event");
        if(WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) {
            std::puts("FAIL GPU fence timed out"); std::exit(3);
        }
        Check(device->GetDeviceRemovedReason(), "device status");
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
    }
    ~Gpu() { CloseHandle(event); }
};

static void Barrier(Gpu& gpu, ID3D12Resource* texture,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    gpu.list->ResourceBarrier(1, &barrier);
}

static ComPtr<ID3D12Resource> Buffer(Gpu& gpu, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "buffer");
    return result;
}

static ComPtr<ID3D12Resource> Texture(Gpu& gpu, unsigned width, unsigned height,
    DXGI_FORMAT format, bool output = false)
{
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = format;
    desc.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> texture;
    Check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; UINT64 bytes;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = Buffer(gpu, bytes, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char* mapped = nullptr;
    Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "upload map");
    memset(mapped, 0, size_t(bytes));
    for(unsigned y = 0; y < height; ++y) for(unsigned x = 0; x < width; ++x) {
        auto pixel = mapped + size_t(y)*footprint.Footprint.RowPitch + x*4;
        if(format == DXGI_FORMAT_R32_FLOAT) { float depth = 0.75f; memcpy(pixel, &depth, 4); }
        else if(format == DXGI_FORMAT_R8G8B8A8_UNORM && !output) {
            pixel[0] = 32 + (x/16 % 8)*24; pixel[1] = 48 + (y/16 % 8)*20;
            pixel[2] = 96; pixel[3] = 255;
        }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint; dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(gpu, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Submit();
    return texture;
}

static bool Inspect(Gpu& gpu, ID3D12Resource* output, uint64_t* outputHash = nullptr)
{
    const auto desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; UINT64 bytes;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = Buffer(gpu, bytes, D3D12_HEAP_TYPE_READBACK);
    Barrier(gpu, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = output; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(gpu, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Submit();
    unsigned char* mapped = nullptr;
    Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "readback map");
    UINT64 nonzero = 0, sum = 0, changed = 0;
    uint64_t hash = 14695981039346656037ull;
    for(unsigned y = 0; y < desc.Height; ++y) for(unsigned x = 0; x < desc.Width; ++x) {
        const auto pixel = mapped + size_t(y)*footprint.Footprint.RowPitch + x*4;
        const unsigned rgb = pixel[0] + pixel[1] + pixel[2];
        nonzero += rgb != 0; sum += rgb;
        changed += pixel[0] != 32 + (x/16 % 8)*24 ||
            pixel[1] != 48 + (y/16 % 8)*20 || pixel[2] != 96;
        for(unsigned component = 0; component < 3; ++component)
            hash = (hash ^ pixel[component])*1099511628211ull;
    }
    readback->Unmap(0, nullptr);
    if(outputHash) *outputHash = hash;
    std::printf(" output_nonzero_pixels=%llu changed_pixels=%llu rgb_sum=%llu rgb_hash=%016llX\n", nonzero, changed, sum, hash);
    return nonzero != 0 && changed != 0;
}
