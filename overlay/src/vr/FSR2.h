#pragma once

#include <stdint.h>

namespace Fsr2
{
// Native resources stay opaque here so non-D3D12 builds do not inherit the
// FidelityFX or Direct3D headers.
struct EyeInput
{
	void *color;
	void *depth;
	uint64_t depthShaderResourceView;
	uint32_t width;
	uint32_t height;
	uint32_t sourceLeft;
	float view[16];
	float projection[16];
	float nearPlane;
	float farPlane;
};

struct EyeOutput
{
	void *color;
	uint64_t shaderResourceView;
	uint32_t width;
	uint32_t height;
};

void Initialize();
void AttachDevice();
bool BeginFrame(float jitterX, float jitterY);
bool EvaluateFrame(const EyeInput inputs[2], EyeOutput outputs[2]);
void ResetHistory();
void ReleaseResources();
void Shutdown();

bool IsSupported();
bool WasLastEvaluationSuccessful();
const char *GetStatus();
}
