#pragma once

#include <stdint.h>

namespace DlssNrModelScale
{
struct Dimensions
{
	uint32_t width, height;
};

inline Dimensions Resolve(uint32_t width, uint32_t height, int mode)
{
	Dimensions result = {width, height};
	if(mode < 1 || mode > 3 || width < 64 || height < 64) return result;
	const uint32_t numerator = mode == 1 ? 3 : mode == 2 ? 2 : 1;
	const uint32_t denominator = mode == 1 ? 4 : mode == 2 ? 3 : 2;
	result.width = static_cast<uint32_t>((uint64_t)width * numerator / denominator) & ~15u;
	result.height = static_cast<uint32_t>((uint64_t)height * numerator / denominator) & ~15u;
	if(result.width < 64) result.width = 64;
	if(result.height < 64) result.height = 64;
	return result;
}

struct Constants
{
	uint32_t width, height, sourceLeft, sourceTop;
	uint32_t modelWidth, modelHeight, pad0, pad1;
};
static_assert(sizeof(Constants) == 8 * sizeof(uint32_t), "Model scale constants layout");

// Both sides of the neural residual use the same proxy grid and interpolation.
// The original crop is retained at full resolution, including its alpha.
static const char *const kShader = R"hlsl(
cbuffer ModelScaleConstants : register(b0)
{
	uint width, height, sourceLeft, sourceTop;
	uint modelWidth, modelHeight, pad0, pad1;
};
Texture2D<float4> originalColor : register(t0);
Texture2D<float4> proxyColor : register(t1);
Texture2D<float4> neuralColor : register(t2);
RWTexture2D<float4> destination : register(u0);

bool3 Finite(float3 value) { return (asuint(value) & 0x7f800000u) != 0x7f800000u; }
bool4 Finite(float4 value) { return (asuint(value) & 0x7f800000u) != 0x7f800000u; }

[numthreads(8, 8, 1)]
void Downsample(uint3 tid : SV_DispatchThreadID)
{
	if(tid.x >= modelWidth || tid.y >= modelHeight) return;
	float2 scale = float2(width, height) / float2(modelWidth, modelHeight);
	float2 low = float2(tid.xy) * scale;
	float2 high = min(float2(tid.xy + 1) * scale, float2(width, height));
	uint2 first = uint2(floor(low));
	uint2 end = min(uint2(ceil(high)), uint2(width, height));
	float4 sum = 0.0;
	float total = 0.0;
	[loop] for(uint y = first.y; y < end.y; ++y) {
		float wy = max(0.0, min(high.y, y + 1.0) - max(low.y, float(y)));
		[loop] for(uint x = first.x; x < end.x; ++x) {
			float wx = max(0.0, min(high.x, x + 1.0) - max(low.x, float(x)));
			float4 color = originalColor.Load(int3(uint2(x + sourceLeft, y + sourceTop), 0));
			if(!all(Finite(color))) { destination[tid.xy] = 0.0; return; }
			float weight = wx * wy;
			sum += color * weight;
			total += weight;
		}
	}
	destination[tid.xy] = total > 0.0 ? sum / total : 0.0;
}

float LimitChannel(float original, float residual)
{
	if(residual > 0.0) return min(1.0, (1.0 - original) / residual);
	if(residual < 0.0) return min(1.0, -original / residual);
	return 1.0;
}

[numthreads(8, 8, 1)]
void Compose(uint3 tid : SV_DispatchThreadID)
{
	if(tid.x >= width || tid.y >= height) return;
	float4 original = originalColor.Load(int3(tid.xy + uint2(sourceLeft, sourceTop), 0));
	destination[tid.xy] = original;
	if(!all(Finite(original)) || any(original.rgb < 0.0) || any(original.rgb > 1.0)) return;
	float2 modelPixel = (float2(tid.xy) + .5) * float2(modelWidth, modelHeight)
		/ float2(width, height) - .5;
	modelPixel = clamp(modelPixel, 0.0, float2(modelWidth - 1, modelHeight - 1));
	uint2 q = uint2(floor(modelPixel));
	float2 f = frac(modelPixel);
	uint2 taps[4] = {q, q + uint2(1, 0), q + uint2(0, 1), q + uint2(1, 1)};
	float3 residuals[4];
	[unroll] for(uint i = 0; i < 4; ++i) {
		uint2 p = min(taps[i], uint2(modelWidth - 1, modelHeight - 1));
		float3 proxy = proxyColor.Load(int3(p, 0)).rgb;
		float3 neural = neuralColor.Load(int3(p, 0)).rgb;
		if(!all(Finite(proxy)) || !all(Finite(neural))) return;
		residuals[i] = neural - proxy;
	}
	float3 residual = lerp(lerp(residuals[0], residuals[1], f.x),
		lerp(residuals[2], residuals[3], f.x), f.y);
	float amount = min(LimitChannel(original.r, residual.r),
		min(LimitChannel(original.g, residual.g), LimitChannel(original.b, residual.b)));
	float3 result = original.rgb + residual * saturate(amount);
	if(all(Finite(result))) destination[tid.xy] = float4(saturate(result), original.a);
}
)hlsl";
}
