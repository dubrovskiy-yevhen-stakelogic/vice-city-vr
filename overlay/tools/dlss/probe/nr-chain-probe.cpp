// Runs the game's unmodified DLAA.cpp against a headless D3D12 host.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vector>
#include <algorithm>
#include "probe-gpu.h"
#include "../../../src/vr/DLAA.h"

namespace {
Gpu* activeGpu;
ComPtr<ID3D12DescriptorHeap> descriptors;
unsigned nextDescriptor;
std::vector<IUnknown*> retired;
void Drain()
{
    activeGpu->Submit();
    for(auto object : retired) object->Release();
    retired.clear();
}
}

// The only RenderWare services used by DLAA.cpp: a real device/command list,
// a shader-visible descriptor heap, and retirement after GPU completion.
namespace rw { namespace d3d12 {
ID3D12Device* getDevice() { return activeGpu->device.Get(); }
ID3D12GraphicsCommandList* getCommandList() { return activeGpu->list.Get(); }
ID3D12DescriptorHeap* getShaderResourceHeap() { return descriptors.Get(); }
void resetWorldDrawState() {}
int32_t waitForGpu() { Drain(); return 1; }
void deferRelease(IUnknown* object) { if(object) retired.push_back(object); }
void deferDescriptorRelease(uint32_t, uint32_t, uint32_t) {}
int32_t allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
    D3D12_GPU_DESCRIPTOR_HANDLE* gpu, uint32_t* index)
{
    if(nextDescriptor >= 8192) return 0;
    const unsigned stride = activeGpu->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    *index = nextDescriptor++;
    *cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
    *gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += size_t(*index)*stride; gpu->ptr += UINT64(*index)*stride;
    return 1;
}
}}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int mode = argc > 1 ? _wtoi(argv[1]) : 1;
    const int passes = argc > 2 ? _wtoi(argv[2]) : 2;
    const unsigned outputWidth = argc > 3 ? unsigned(_wtoi(argv[3])) : 3612;
    const unsigned outputHeight = argc > 4 ? unsigned(_wtoi(argv[4])) : 3976;
    if(mode < 0 || mode > 3 || passes < 1 || passes > 3 ||
        outputWidth < 256 || outputHeight < 256 || outputWidth > 4096 || outputHeight > 4096) return 2;
    Dlaa::SetQualityMode(mode);
    Dlaa::SetNeuralRenderingEnabled(true);
    Dlaa::SetNeuralRenderingMode(1);
    Dlaa::SetNeuralRenderingPassCount(passes);
    Dlaa::SetNeuralRenderingOutputVisible(false);
    Dlaa::InitializeEarly();
    Gpu gpu; activeGpu = &gpu;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)), "device");
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    Check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)), "queue");
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)), "allocator");
    Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)), "list");
    Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 8192; heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors)), "descriptors");
    Dlaa::AttachDevice();
    if(!Dlaa::IsSupported()) { std::printf("FAIL attach: %s\n", Dlaa::GetStatus()); TerminateProcess(GetCurrentProcess(), 2); }
    const float ratio = Dlaa::GetQualityModeRenderRatio(mode);
    unsigned width = unsigned(outputWidth*ratio+0.5f), height = unsigned(outputHeight*ratio+0.5f);
    if(mode) { width = std::max(256u, width & ~15u); height = std::max(256u, height & ~15u); }
    auto color = Texture(gpu, width*2, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto depth = Texture(gpu, width*2, height, DXGI_FORMAT_R32_FLOAT);
    D3D12_CPU_DESCRIPTOR_HANDLE depthCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE depthGpu;
    uint32_t depthIndex;
    rw::d3d12::allocateShaderResourceDescriptor(&depthCpu, &depthGpu, &depthIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    gpu.device->CreateShaderResourceView(depth.Get(), &srv, depthCpu);
    std::printf("CHAIN mode=%d passes=%d work=%ux%u output=%ux%u\n", mode, passes, width, height, outputWidth, outputHeight);
    unsigned failures = 0, activeFrames = 0;
    uint64_t baselineHash[2] = {};
    for(unsigned phase = 0; phase < 4 && failures == 0; ++phase) {
        const bool neural = (phase&1) != 0;
        Dlaa::SetNeuralRenderingOutputVisible(neural);
        const unsigned frames = neural ? unsigned(passes+4) : 2;
        for(unsigned frame = 0; frame < frames && failures == 0; ++frame) {
            if(!Dlaa::BeginFrame(0.0f, 0.0f)) { ++failures; break; }
            for(int eye = 0; eye < 2; ++eye) {
                Dlaa::EyeInput input = {};
                input.color = color.Get(); input.depth = depth.Get();
                input.depthShaderResourceView = depthGpu.ptr;
                input.width = width; input.height = height; input.sourceLeft = eye*width;
                input.outputWidth = outputWidth; input.outputHeight = outputHeight;
                input.nearPlane = 0.1f; input.farPlane = 1000.0f;
                input.view[0] = input.view[5] = input.view[10] = input.view[15] = 1.0f;
                input.projection[0] = float(height)/width; input.projection[5] = 1.0f;
                input.projection[10] = input.farPlane/(input.farPlane-input.nearPlane);
                input.projection[11] = 1.0f;
                input.projection[14] = -input.nearPlane*input.projection[10];
                Dlaa::EyeOutput output = {};
                const bool result = Dlaa::EvaluateEye(eye, input, &output);
                std::printf("phase=%u frame=%u eye=%d EvaluateEye=%d\n", phase, frame, eye, result);
                if(!result || !output.color || output.width != outputWidth || output.height != outputHeight) {
                    std::printf("FAIL %s\n", Dlaa::GetStatus()); ++failures; break;
                }
                if(frame+1 == frames) {
                    auto texture = static_cast<ID3D12Resource*>(output.color);
                    Barrier(gpu, texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    std::printf("resolved phase=%u eye=%d", phase, eye);
                    uint64_t hash = 0;
                    failures += !Inspect(gpu, texture, &hash);
                    if(phase == 0) baselineHash[eye] = hash;
                    else if(neural && hash == baselineHash[eye]) {
                        std::puts("FAIL reconstructed NR output equals baseline"); ++failures;
                    }else if(!neural && hash != baselineHash[eye]) {
                        std::puts("FAIL baseline did not restore after NR"); ++failures;
                    }
                    Barrier(gpu, texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
            }
            Drain();
            if(!failures && (!Dlaa::WasLastEvaluationSuccessful() || Dlaa::HasNeuralRenderingFailed())) ++failures;
            const bool active = Dlaa::IsNeuralRenderingStereoActive();
            if(neural && active) ++activeFrames;
            if((!neural && active) || (neural && frame+1 == frames && !active)) ++failures;
            std::printf("phase=%u frame=%u stereoNR=%d failures=%u\n", phase, frame, active, failures);
        }
    }
    Drain(); Dlaa::ReleaseResources(); Drain();
    std::printf("CHAIN_COMPLETE failures=%u activeFrames=%u\n", failures, activeFrames);
    TerminateProcess(GetCurrentProcess(), failures ? 1 : 0);
    return 0;
}
