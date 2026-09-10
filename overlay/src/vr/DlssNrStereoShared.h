#pragma once

#include <stdint.h>

namespace DlssNrStereoShared
{
struct Constants
{
	float rightClipToLeftClip[16];
	float leftClipToRightClip[16];
	float leftDepthUnproject[4];
	float rightDepthUnproject[4];
	uint32_t width, height, leftOffset, rightOffset;
	float jitterX, jitterY;
	uint32_t targetEye, pad;
	float depthAbsoluteTolerance, depthRelativeTolerance;
	float colorTolerance, nearFadeDistance;
};
static_assert(sizeof(Constants) == 52 * sizeof(uint32_t), "Stereo shared constants layout");

// Color and depth contain both eyes; the neural texture and output are eye-local.
// Matrices use unjittered D3D clip coordinates and row-vector multiplication.
// Jitter is the displacement of the rasterized image in pixels, positive down/right.
static const char *const kShader = R"hlsl(
cbuffer StereoConstants : register(b0)
{
	row_major float4x4 rightClipToLeftClip;
	row_major float4x4 leftClipToRightClip;
	float4 leftDepthUnproject;
	float4 rightDepthUnproject;
	uint width, height, leftOffset, rightOffset;
	float jitterX, jitterY;
	uint targetEye, pad;
	float depthAbsoluteTolerance, depthRelativeTolerance;
	float colorTolerance, nearFadeDistance;
};
Texture2D<float4> originalColor : register(t0);
Texture2D<float> stereoDepth : register(t1);
Texture2D<float4> leftNeural : register(t2);
RWTexture2D<float4> composite : register(u0);

bool Finite(float v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }
bool2 Finite(float2 v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }
bool3 Finite(float3 v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }
bool4 Finite(float4 v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }

uint EyeOffset(uint eye) { return eye == 0 ? leftOffset : rightOffset; }
float4 ReadColor(int2 p, uint eye)
{
	return originalColor.Load(int3(p + int2(EyeOffset(eye), 0), 0));
}
float ReadDepth(int2 p, uint eye)
{
	return stereoDepth.Load(int3(p + int2(EyeOffset(eye), 0), 0));
}
bool LinearDepth(float depth, uint eye, out float result)
{
	result = 0.0;
	if (!Finite(depth) || depth <= 0.000001 || depth >= 0.999999)
		return false;
	float4 u = eye == 0 ? leftDepthUnproject : rightDepthUnproject;
	float denominator = depth * u.z + u.w;
	if (!Finite(denominator) || abs(denominator) < 0.0000001)
		return false;
	result = abs((depth * u.x + u.y) / denominator);
	return Finite(result) && result > 0.00001;
}
float DepthConfidence(float a, float b)
{
	float tolerance = max(0.00001,
		max(depthAbsoluteTolerance, 0.0) + max(depthRelativeTolerance, 0.0) * min(a, b));
	return 1.0 - smoothstep(0.25 * tolerance, tolerance, abs(a - b));
}
float EdgeConfidence(int2 p, uint eye, float centerDepth)
{
	float confidence = 1.0;
	const int2 neighbors[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
	[unroll] for (uint i = 0; i < 4; ++i) {
		int2 q = clamp(p + neighbors[i], int2(0, 0), int2(width - 1, height - 1));
		float z;
		if (!LinearDepth(ReadDepth(q, eye), eye, z))
			return 0.0;
		confidence = min(confidence, DepthConfidence(z, centerDepth));
	}
	return confidence;
}
float4 ClipPosition(float2 pixel, float depth)
{
	float2 uv = (pixel + 0.5 - float2(jitterX, jitterY)) / float2(width, height);
	return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
}
float3 Blend4(float3 a, float3 b, float3 c, float3 d, float2 f)
{
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= width || tid.y >= height)
		return;
	int2 p = int2(tid.xy);
	float4 baseline = ReadColor(p, targetEye);
	composite[p] = baseline;
	if (targetEye > 1 || !all(Finite(baseline)))
		return;
	uint otherEye = 1 - targetEye;
	float rawDepth = ReadDepth(p, targetEye);
	float referenceDepth;
	if (!LinearDepth(rawDepth, targetEye, referenceDepth))
		return;
	float4 referenceClip = ClipPosition(float2(p), rawDepth);
	float4 otherClip;
	if (targetEye == 0)
		otherClip = mul(referenceClip, leftClipToRightClip);
	else
		otherClip = mul(referenceClip, rightClipToLeftClip);
	if (!all(Finite(otherClip)) || otherClip.w <= 0.000001)
		return;
	float3 otherNdc = otherClip.xyz / otherClip.w;
	float expectedDepth;
	if (!LinearDepth(otherNdc.z, otherEye, expectedDepth))
		return;
	float2 otherPixel = float2(otherNdc.x * 0.5 + 0.5, 0.5 - otherNdc.y * 0.5)
		* float2(width, height) + float2(jitterX, jitterY) - 0.5;
	if (!all(Finite(otherPixel)) || any(otherPixel < -0.001)
		|| any(otherPixel > float2(width - 1, height - 1) + 0.001))
		return;
	otherPixel = clamp(otherPixel, 0.0, float2(width - 1, height - 1));
	int2 q = int2(floor(otherPixel));
	float2 f = frac(otherPixel);
	int2 taps[4] = { q, q + int2(1, 0), q + int2(0, 1), q + int2(1, 1) };
	float3 colors[4];
	float confidence = EdgeConfidence(p, targetEye, referenceDepth);
	[unroll] for (uint i = 0; i < 4; ++i) {
		taps[i] = min(taps[i], int2(width - 1, height - 1));
		float z;
		if (!LinearDepth(ReadDepth(taps[i], otherEye), otherEye, z))
			return;
		float4 color = ReadColor(taps[i], otherEye);
		if (!all(Finite(color)))
			return;
		colors[i] = color.rgb;
		confidence = min(confidence, DepthConfidence(z, expectedDepth));
		confidence = min(confidence, EdgeConfidence(taps[i], otherEye, z));
	}
	float3 otherColor = Blend4(colors[0], colors[1], colors[2], colors[3], f);
	float3 colorError = abs(otherColor - baseline.rgb);
	float difference = max(colorError.x, max(colorError.y, colorError.z));
	float tolerance = max(colorTolerance, 0.00001);
	confidence *= 1.0 - smoothstep(tolerance * 0.25, tolerance, difference);
	if (nearFadeDistance > 0.0)
		confidence *= smoothstep(nearFadeDistance, nearFadeDistance * 2.0,
			min(referenceDepth, expectedDepth));
	if (!Finite(confidence) || confidence <= 0.0)
		return;
	float3 residual;
	if (targetEye == 0) {
		float3 neural = leftNeural.Load(int3(p, 0)).rgb;
		if (!all(Finite(neural)))
			return;
		residual = neural - baseline.rgb;
	} else {
		float3 n[4];
		[unroll] for (uint i = 0; i < 4; ++i) {
			n[i] = leftNeural.Load(int3(taps[i], 0)).rgb;
			if (!all(Finite(n[i])))
				return;
		}
		residual = Blend4(n[0] - colors[0], n[1] - colors[1],
			n[2] - colors[2], n[3] - colors[3], f);
	}
	float3 result = baseline.rgb + confidence * residual;
	if (all(Finite(result)))
		composite[p] = float4(saturate(result), baseline.a);
}
)hlsl";
}
