#pragma once

#include <cstdint>

namespace VrVehicleViewSettings
{
inline int ClampProfileInt(std::uint32_t encoded, int minimum, int maximum)
{
	// Win32 returns UINT even for negative INI values. Decode before comparing
	// with signed bounds, otherwise every ordinary value selects the upper limit.
	const int value = static_cast<std::int32_t>(encoded);
	return value < minimum ? minimum : value > maximum ? maximum : value;
}

template<class ReadInt>
int ReadDefaultSeatHeight(ReadInt read, const char *key, const char *path)
{
	const int legacyHeight = ClampProfileInt(
		read("VR", "DrivingYOffsetCm", 15, path), -100, 150);
	return ClampProfileInt(read("VR", key, legacyHeight, path), -100, 150);
}

template<class ReadInt>
int ReadDefaultSeatDistance(ReadInt read, const char *key, const char *path)
{
	return ClampProfileInt(read("VR", key, 0, path), -100, 100);
}

template<class ReadInt>
int ReadWheelHandPullBack(ReadInt read, const char *path)
{
	return ClampProfileInt(read("VR", "WheelHandPullBackMm", 0, path), -80, 80);
}
}
