#pragma once

#include <stdint.h>
#include <cmath>

namespace DlssNrFoveation
{
struct Region
{
	uint32_t left, top, width, height;
	float featherPixels;
};

inline bool IsCropped(uint32_t width, uint32_t height, const Region &region)
{
	return region.left != 0 || region.top != 0 || region.width != width || region.height != height;
}

inline Region Resolve(uint32_t width, uint32_t height, int mode, float centerU = .5f, float centerV = .5f)
{
	Region region = {0, 0, width, height, 0.f};
	if(mode < 1 || mode > 3 || width < 64 || height < 64) return region;
	const uint32_t percent = mode == 1 ? 85 : mode == 2 ? 75 : 65;
	uint32_t cropWidth = static_cast<uint32_t>((uint64_t)width * percent / 100) & ~15u;
	uint32_t cropHeight = static_cast<uint32_t>((uint64_t)height * percent / 100) & ~15u;
	if(cropWidth < 64) cropWidth = 64;
	if(cropHeight < 64) cropHeight = 64;
	if(cropWidth == width && cropHeight == height) return region;
	if(!std::isfinite(centerU)) centerU = .5f;
	if(!std::isfinite(centerV)) centerV = .5f;
	centerU = centerU < 0.f ? 0.f : centerU > 1.f ? 1.f : centerU;
	centerV = centerV < 0.f ? 0.f : centerV > 1.f ? 1.f : centerV;
	const double left = std::floor((double)width * centerU - .5 * cropWidth + .5);
	const double top = std::floor((double)height * centerV - .5 * cropHeight + .5);
	region.left = left <= 0 ? 0 : left >= width - cropWidth ? width - cropWidth : (uint32_t)left;
	region.top = top <= 0 ? 0 : top >= height - cropHeight ? height - cropHeight : (uint32_t)top;
	region.width = cropWidth;
	region.height = cropHeight;
	const float shortest = (float)(cropWidth < cropHeight ? cropWidth : cropHeight);
	region.featherPixels = shortest * .08f;
	if(region.featherPixels < 8.f) region.featherPixels = 8.f;
	if(region.featherPixels > shortest * .5f) region.featherPixels = shortest * .5f;
	return region;
}

struct Constants
{
	uint32_t width, height, sourceLeft, cropLeft;
	uint32_t cropTop, cropWidth, cropHeight;
	float featherPixels;
};
static_assert(sizeof(Constants) == 8 * sizeof(uint32_t), "Foveation constants layout");

// The crop retains the original pixel density. Only its neural residual is
// feathered into the full eye; pixels outside it are copied without changes.
static const char *const kShader = R"hlsl(
cbuffer FoveationConstants : register(b0)
{
	uint width, height, sourceLeft, cropLeft;
	uint cropTop, cropWidth, cropHeight;
	float featherPixels;
};
Texture2D<float4> originalColor : register(t0);
Texture2D<float4> croppedNeural : register(t1);
RWTexture2D<float4> composite : register(u0);

bool3 Finite(float3 v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if(tid.x >= width || tid.y >= height) return;
	uint2 p = tid.xy;
	float4 baseline = originalColor.Load(int3(p + uint2(sourceLeft, 0), 0));
	composite[p] = baseline;
	if(p.x < cropLeft || p.y < cropTop) return;
	uint2 q = p - uint2(cropLeft, cropTop);
	if(q.x >= cropWidth || q.y >= cropHeight) return;
	float3 neural = croppedNeural.Load(int3(q, 0)).rgb;
	if(!all(Finite(neural)) || !all(Finite(baseline.rgb))) return;
	float edge = min(min(q.x + .5, q.y + .5),
		min(cropWidth - q.x - .5, cropHeight - q.y - .5));
	float weight = featherPixels > 0.f ? smoothstep(0.f, featherPixels, edge) : 1.f;
	float3 result = baseline.rgb + (neural - baseline.rgb) * weight;
	if(all(Finite(result))) composite[p] = float4(saturate(result), baseline.a);
}
)hlsl";
}
