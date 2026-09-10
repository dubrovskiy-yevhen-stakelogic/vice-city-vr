#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "../../../overlay/src/vr/DlssNrScenePolicy.h"
#include "../../../overlay/src/vr/DlssNrModelScale.h"
#include "../../../overlay/src/vr/DlssNrFoveation.h"
#include "../../../overlay/src/vr/DlssNrStereoShared.h"

static unsigned checks = 0;
static void Require(bool condition, const char *message)
{
    if(!condition) throw std::runtime_error(message);
    ++checks;
}

static void TestScenePolicy()
{
    for(int saved = -2; saved <= 5; ++saved) {
        const int valid = saved >= 0 && saved <= 3 ? saved : 0;
        Require(DlssNrScenePolicy::ResolveQualityMode(saved, false) == valid,
            "Ordinary DLSS quality changed while neural rendering is disabled");
        Require(DlssNrScenePolicy::ResolveQualityMode(saved, true) == 0,
            "Neural rendering must retain full scene resolution");
        Require(DlssNrScenePolicy::ModelScaleDefault(saved, false) == 0,
            "Ordinary DLSS preferences must not select neural model scaling");
        Require(DlssNrScenePolicy::ModelScaleDefault(saved, true) == valid,
            "Legacy neural scale migration lost the saved selection");
    }
}

static void TestModelDimensions()
{
    const uint32_t sizes[] = {0, 1, 63, 64, 65, 127, 129, 1024, 1776, 1921,
        std::numeric_limits<uint32_t>::max()};
    for(uint32_t width : sizes) for(uint32_t height : sizes) {
        for(int mode = -1; mode <= 4; ++mode) {
            const auto dimensions = DlssNrModelScale::Resolve(width, height, mode);
            if(mode < 1 || mode > 3 || width < 64 || height < 64) {
                Require(dimensions.width == width && dimensions.height == height,
                    "Full, invalid and small inputs must preserve model dimensions");
            } else {
                const uint32_t numerator = mode == 1 ? 3 : mode == 2 ? 2 : 1;
                const uint32_t denominator = mode == 1 ? 4 : mode == 2 ? 3 : 2;
                uint32_t expectedWidth = static_cast<uint32_t>(uint64_t(width) * numerator / denominator) & ~15u;
                uint32_t expectedHeight = static_cast<uint32_t>(uint64_t(height) * numerator / denominator) & ~15u;
                if(expectedWidth < 64) expectedWidth = 64;
                if(expectedHeight < 64) expectedHeight = 64;
                Require(dimensions.width == expectedWidth && dimensions.height == expectedHeight,
                    "Model scaling ratio or alignment changed");
                Require(dimensions.width <= width && dimensions.height <= height,
                    "Model scale enlarged its input");
            }
        }
    }
}

static void TestFoveation()
{
    const uint32_t sizes[] = {0, 1, 63, 64, 65, 127, 129, 1024, 1776, 1921};
    const float centers[] = {-1.f, 0.f, .5f, 1.f, 2.f,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    for(uint32_t width : sizes) for(uint32_t height : sizes) {
        for(int mode = -1; mode <= 4; ++mode) for(float center : centers) {
            const auto region = DlssNrFoveation::Resolve(width, height, mode, center, center);
            Require(uint64_t(region.left) + region.width <= width &&
                uint64_t(region.top) + region.height <= height, "Foveated crop escaped the eye");
            Require(std::isfinite(region.featherPixels) && region.featherPixels >= 0.f,
                "Invalid crop feather");
            if(mode < 1 || mode > 3 || width < 64 || height < 64) {
                Require(!DlssNrFoveation::IsCropped(width, height, region),
                    "Off, invalid or small inputs must preserve the whole eye");
            } else {
                Require(region.width >= 64 && region.height >= 64 &&
                    region.width % 16 == 0 && region.height % 16 == 0, "Invalid crop alignment");
                if(!std::isfinite(center)) {
                    const auto middle = DlssNrFoveation::Resolve(width, height, mode);
                    Require(region.left == middle.left && region.top == middle.top,
                        "Invalid centers must fall back to the eye center");
                }
            }
        }
    }
}

static void CompileShader(const char *source, const char *name, const char *entry)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(source, strlen(source), name, nullptr, nullptr, entry,
        "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
    if(FAILED(result) && errors)
        std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
            static_cast<const char *>(errors->GetBufferPointer()));
    Require(SUCCEEDED(result) && code && code->GetBufferSize() > 0, "Production shader compilation failed");
}

int main() try
{
    TestScenePolicy();
    TestModelDimensions();
    TestFoveation();
    CompileShader(DlssNrModelScale::kShader, "model-scale", "Downsample");
    CompileShader(DlssNrModelScale::kShader, "model-scale", "Compose");
    CompileShader(DlssNrFoveation::kShader, "foveation", "main");
    CompileShader(DlssNrStereoShared::kShader, "shared-stereo", "main");
    std::printf("NR OPTIMIZATIONS: PASS (%u checks; host policies and shader compilation only)\n", checks);
    return 0;
}
catch(const std::exception &error)
{
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
