// Headless regression probe for the installed NR forwarder and stereo inputs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "sl.h"
#include "../../../src/vr/DlssNrColorInput.h"

using Microsoft::WRL::ComPtr;
using CreateNR = void*(__cdecl*)(const wchar_t*, const wchar_t*, ID3D12Device*,
    ID3D12GraphicsCommandList*, void*, unsigned, unsigned, int, float, int,
    float, float, float, float, int, int);
using EvalNR = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*,
    ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
    unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, int, int, float, int, float, float, float,
    float, int, float, float);

#include "probe-gpu.h"

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if(argc < 2) { std::puts("Usage: nr-subrect-probe <runtime-directory> [width height]"); return 2; }
    const std::wstring dir = argv[1];
    const unsigned width = argc > 2 ? _wtoi(argv[2]) : 768;
    const unsigned height = argc > 3 ? _wtoi(argv[3]) : 768;
    if(width < 128 || height < 128 || width > 4096 || height > 4096) return 2;
    SetDllDirectoryW(dir.c_str());
    auto stream = LoadLibraryW((dir + L"\\sl.interposer.dll").c_str());
    auto forwarder = LoadLibraryW((dir + L"\\nvngx.dll_dlssnr.dll").c_str());
    if(!stream || !forwarder) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    auto init = reinterpret_cast<PFun_slInit*>(GetProcAddress(stream, "slInit"));
    auto attach = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(stream, "slSetD3DDevice"));
    auto shutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(stream, "slShutdown"));
    auto create = reinterpret_cast<CreateNR>(GetProcAddress(forwarder, "dlssnr_call_create"));
    auto eval = reinterpret_cast<EvalNR>(GetProcAddress(forwarder, "dlssnr_call_evaluate"));
    auto release = reinterpret_cast<void(__cdecl*)(void*)>(GetProcAddress(forwarder, "dlssnr_call_release"));
    if(!init || !attach || !shutdown || !create || !eval || !release) return 2;
    const wchar_t* paths[] = { dir.c_str() };
    const sl::Feature features[] = { sl::kFeatureDLSS };
    sl::Preferences preferences{};
    preferences.pathsToPlugins = paths; preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features; preferences.numFeaturesToLoad = 1;
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.engine = sl::EngineType::eCustom; preferences.engineVersion = "0.5.5-probe";
    preferences.projectId = "4557d919-aaf5-4797-8baa-c2acc5950bf1";
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText;
    preferences.logLevel = sl::LogLevel::eOff;
    const auto initResult = init(preferences, sl::kSDKVersion);
    std::printf("slInit=%d\n", int(initResult));
    if(initResult != sl::Result::eOk) return 2;
    Gpu gpu;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)), "device");
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    Check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)), "queue");
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)), "allocator");
    Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)), "list");
    Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");
    const auto attached = attach(gpu.device.Get());
    std::printf("slSetD3DDevice=%d\n", int(attached));
    if(attached != sl::Result::eOk) return 2;
    auto core = GetModuleHandleW(L"nvngx.dll");
    if(!core) core = GetModuleHandleW(L"_nvngx.dll");
    auto getParams = reinterpret_cast<int(__cdecl*)(void**)>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    void* params = nullptr;
    if(!getParams || getParams(&params) != 1 || !params) { std::puts("No capability block"); return 2; }
    auto wideColor = Texture(gpu, width*2, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto localColor = Texture(gpu, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto wideDepth = Texture(gpu, width*2, height, DXGI_FORMAT_R32_FLOAT);
    auto localDepth = Texture(gpu, width, height, DXGI_FORMAT_R32_FLOAT);
    auto motion = Texture(gpu, width, height, DXGI_FORMAT_R16G16_FLOAT);
    const std::wstring model = dir + L"\\nvngx_dlssnr.dll";
    struct Case { const char* name; ID3D12Resource* color; ID3D12Resource* depth; unsigned colorX, depthX; bool stage; };
    const Case cases[] = {
        { "wide-left", wideColor.Get(), wideDepth.Get(), 0, 0, false },
        { "wide-right", wideColor.Get(), wideDepth.Get(), width, width, false },
        { "local-color-right-depth", localColor.Get(), wideDepth.Get(), 0, width, false },
        { "local-color-local-depth", localColor.Get(), localDepth.Get(), 0, 0, false },
        { "staged-right", wideColor.Get(), wideDepth.Get(), width, width, true }
    };
    unsigned failures = 0;
    for(const auto& test : cases) {
        auto output = Texture(gpu, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        auto feature = create(model.c_str(), dir.c_str(), gpu.device.Get(), gpu.list.Get(),
            params, width, height, 0, 1.0f, 1, 1.25f, 0.5f, 0.25f, -1.0f, 1, 1);
        std::printf("case=%s size=%ux%u create=%p\n", test.name, width, height, feature);
        gpu.Submit();
        if(!feature) return 2;
        auto color = test.color;
        unsigned colorX = test.colorX;
        if(test.stage) {
            D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if(!Dlaa::CopyNeuralColorInput(gpu.list.Get(), test.color, localColor.Get(),
                inputState, test.colorX, width, height)) return 2;
            color = localColor.Get(); colorX = 0;
        }
        int result = eval(gpu.list.Get(), feature, params, color, test.depth,
            motion.Get(), output.Get(), width, height, width, height,
            colorX, 0, test.depthX, 0, 0, 0, 0, 1,
            1.0f, 1, 1.25f, 0.5f, 0.25f, -1.0f, 1, 1.0f, 1.0f);
        std::printf("case=%s evaluate=0x%08X", test.name, unsigned(result));
        gpu.Submit();
        const bool changed = Inspect(gpu, output.Get());
        const bool expectFailure = test.colorX != 0 && !test.stage;
        failures += expectFailure ? unsigned(result) != 0xBAD00005u :
            result != 1 || !changed;
        release(feature);
    }
    // Exercise simultaneous left/right handles and recurrent multi-pass output.
    // All six handles share the same capability block, as in the game.
    const unsigned passes = argc > 4 ? unsigned(_wtoi(argv[4])) : 1;
    if(passes < 1 || passes > 3) return 2;
    if(passes > 1) {
        void* handles[2][3] = {};
        ComPtr<ID3D12Resource> ping[2][2];
        D3D12_RESOURCE_STATES states[2][2] = {
            { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
            { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS }
        };
        for(unsigned eye = 0; eye < 2; ++eye) {
            for(auto& texture : ping[eye])
                texture = Texture(gpu, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true);
            for(unsigned pass = 0; pass < passes; ++pass) {
                handles[eye][pass] = create(model.c_str(), dir.c_str(), gpu.device.Get(), gpu.list.Get(),
                    params, width, height, 0, 1.0f, 1, 1.25f, 0.5f, 0.25f, -1.0f, 1, 1);
                gpu.Submit();
                if(!handles[eye][pass]) return 2;
            }
        }
        for(unsigned frame = 0; frame < 3; ++frame) for(unsigned eye = 0; eye < 2; ++eye) {
            D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if(!Dlaa::CopyNeuralColorInput(gpu.list.Get(), wideColor.Get(), localColor.Get(),
                inputState, eye*width, width, height)) return 2;
            ID3D12Resource* source = localColor.Get();
            for(unsigned pass = 0; pass < passes; ++pass) {
                const unsigned target = pass & 1;
                if(pass > 0) {
                    const unsigned previous = (pass-1)&1;
                    Barrier(gpu, source, states[eye][previous], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    states[eye][previous] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
                if(states[eye][target] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    Barrier(gpu, ping[eye][target].Get(), states[eye][target], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    states[eye][target] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
                const int result = eval(gpu.list.Get(), handles[eye][pass], params, source, wideDepth.Get(),
                    motion.Get(), ping[eye][target].Get(), width, height, width, height,
                    0, 0, eye*width, 0, 0, 0, 0, frame == 0,
                    1.0f, 1, 1.25f, 0.5f, 0.25f, -1.0f, 1, 1.0f, 1.0f);
                gpu.Submit();
                std::printf("stereo frame=%u eye=%u pass=%u evaluate=0x%08X",
                    frame, eye, pass+1, unsigned(result));
                failures += result != 1 || !Inspect(gpu, ping[eye][target].Get());
                source = ping[eye][target].Get();
            }
        }
        for(auto& eye : handles) for(auto handle : eye) if(handle) release(handle);
    }
    gpu.Submit();
    // The model is initialized directly and is not owned by Streamline.
    // All GPU work and feature releases have completed; avoid asking SL to
    // tear down a shared NGX core underneath that separately loaded model.
    std::printf("PROBE_COMPLETE failures=%u\n", failures);
    TerminateProcess(GetCurrentProcess(), failures ? 1 : 0);
    return 0;
}
