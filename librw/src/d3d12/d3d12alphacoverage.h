#pragma once

#include <stdint.h>

namespace rw {
namespace d3d12 {

inline uint32_t
countAlphaCoverage(const uint8_t *pixels, uint32_t width, uint32_t height,
                   uint32_t stride, float scale)
{
	uint32_t covered = 0;
	for(uint32_t y = 0; y < height; y++){
		const uint8_t *row = pixels + y*stride;
		for(uint32_t x = 0; x < width; x++)
			if((float)row[x*4+3]*scale >= 128.0f)
				covered++;
	}
	return covered;
}

inline uint32_t
countHistogramCoverage(const uint32_t histogram[256], float scale)
{
	uint32_t covered = 0;
	for(uint32_t alpha = 0; alpha < 256; alpha++)
		if((float)alpha*scale >= 128.0f)
			covered += histogram[alpha];
	return covered;
}

inline void
preserveAlphaCoverage(const uint8_t *source, uint32_t sourceWidth,
                     uint32_t sourceHeight, uint32_t sourceStride,
                     uint8_t *target, uint32_t targetWidth,
                     uint32_t targetHeight, uint32_t targetStride)
{
	const uint32_t sourcePixels = sourceWidth*sourceHeight;
	const uint32_t targetPixels = targetWidth*targetHeight;
	if(sourcePixels == 0 || targetPixels == 0)
		return;
	const uint32_t sourceCovered = countAlphaCoverage(source, sourceWidth,
		sourceHeight, sourceStride, 1.0f);
	const uint32_t wanted = (uint32_t)(((uint64_t)sourceCovered*targetPixels +
		sourcePixels/2)/sourcePixels);
	if(wanted == 0)
		return;

	// The search only depends on alpha values. Count each texel once instead
	// of rescanning the complete mip for each of the 12 candidate scales.
	uint32_t histogram[256] = {};
	const bool useHistogram = targetPixels >= 256;
	if(useHistogram)
		for(uint32_t y = 0; y < targetHeight; y++){
			const uint8_t *row = target + y*targetStride;
			for(uint32_t x = 0; x < targetWidth; x++)
				histogram[row[x*4+3]]++;
		}
	const auto covered = [&](float scale) {
		return useHistogram ? countHistogramCoverage(histogram, scale) :
			countAlphaCoverage(target,targetWidth,targetHeight,targetStride,scale);
	};
	float low = 1.0f;
	float high = 255.0f;
	if(covered(low) >= wanted)
		high = low;
	else{
		for(uint32_t iteration = 0; iteration < 12; iteration++){
			const float middle = (low + high)*0.5f;
			if(covered(middle) >= wanted)
				high = middle;
			else
				low = middle;
		}
	}
	for(uint32_t y = 0; y < targetHeight; y++){
		uint8_t *row = target + y*targetStride;
		for(uint32_t x = 0; x < targetWidth; x++){
			const uint32_t scaled = (uint32_t)(row[x*4+3]*high + 0.5f);
			row[x*4+3] = (uint8_t)(scaled < 255 ? scaled : 255);
		}
	}
}

}
}
