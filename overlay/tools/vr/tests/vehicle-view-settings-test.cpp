#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include "../../../src/vr/VrVehicleViewSettings.h"

static int checks;
static void Check(bool value, const char *name)
{
	++checks;
	if(!value){ std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}

struct Profile
{
	char path[MAX_PATH];
	explicit Profile(const char *directory)
	{
		Check(GetTempFileNameA(directory, "vcv", 0, path) != 0, "create private INI fixture");
	}
	~Profile()
	{
		WritePrivateProfileStringA(nullptr, nullptr, nullptr, path);
		DeleteFileA(path);
	}
	void Set(const char *key, const char *value)
	{
		Check(WritePrivateProfileStringA("VR", key, value, path) != 0, "write private INI fixture");
	}
};

static int OldUnsignedClamp(unsigned int value, int minimum, int maximum)
{
	const unsigned int lower = static_cast<unsigned int>(minimum);
	const unsigned int upper = static_cast<unsigned int>(maximum);
	const unsigned int aboveLower = value > lower ? value : lower;
	return static_cast<int>(aboveLower < upper ? aboveLower : upper);
}

int main(int argc, char **argv)
{
	Check(argc == 2, "pass the private test-output directory");
	Profile ini(argv[1]);
	using namespace VrVehicleViewSettings;
	Check(OldUnsignedClamp(GetPrivateProfileIntA("VR", "MissingHeight", 15, ini.path), -100, 150) == 150,
		"reproduce former 150cm lift with absent key");
	Check(OldUnsignedClamp(GetPrivateProfileIntA("VR", "MissingDistance", 0, ini.path), -100, 100) == 100,
		"reproduce former 100cm forward shift with absent key");
	Check(OldUnsignedClamp(GetPrivateProfileIntA("VR", "WheelHandPullBackMm", 0, ini.path), -80, 80) == 80,
		"reproduce former forced 80mm hand offset");
	for(const char *category : {"Car", "Bike", "Boat", "Heli"}){
		char heightKey[64], distanceKey[64];
		std::snprintf(heightKey, sizeof(heightKey), "Default%sSeatHeightCm", category);
		std::snprintf(distanceKey, sizeof(distanceKey), "Default%sSeatDistanceCm", category);
		Check(ReadDefaultSeatHeight(GetPrivateProfileIntA, heightKey, ini.path) == 15, "absent height stays 15cm");
		Check(ReadDefaultSeatDistance(GetPrivateProfileIntA, distanceKey, ini.path) == 0, "absent distance stays zero");
		for(int value : {-1000, -100, -32, -1, 0, 1, 15, 100, 150, 1000}){
			char text[32];
			std::snprintf(text, sizeof(text), "%d", value);
			ini.Set(heightKey, text);
			ini.Set(distanceKey, text);
			const int height = value < -100 ? -100 : value > 150 ? 150 : value;
			const int distance = value < -100 ? -100 : value > 100 ? 100 : value;
			Check(ReadDefaultSeatHeight(GetPrivateProfileIntA, heightKey, ini.path) == height,
				"signed saved height and limits");
			Check(ReadDefaultSeatDistance(GetPrivateProfileIntA, distanceKey, ini.path) == distance,
				"signed saved distance and limits");
		}
		ini.Set(heightKey, nullptr);
		ini.Set(distanceKey, nullptr);
		for(int value : {-1000, -32, 0, 25, 1000}){
			char text[32];
			std::snprintf(text, sizeof(text), "%d", value);
			ini.Set("DrivingYOffsetCm", text);
			const int height = value < -100 ? -100 : value > 150 ? 150 : value;
			Check(ReadDefaultSeatHeight(GetPrivateProfileIntA, heightKey, ini.path) == height,
				"missing per-category height inherits signed legacy value");
			Check(ReadDefaultSeatDistance(GetPrivateProfileIntA, distanceKey, ini.path) == 0,
				"legacy height never alters seat distance");
			ini.Set(heightKey, "0");
			Check(ReadDefaultSeatHeight(GetPrivateProfileIntA, heightKey, ini.path) == 0,
				"explicit zero overrides legacy height");
			ini.Set(heightKey, nullptr);
		}
		ini.Set("DrivingYOffsetCm", nullptr);
	}
	Check(ReadWheelHandPullBack(GetPrivateProfileIntA, ini.path) == 0, "absent hand pullback stays zero");
	for(int value : {-1000, -80, -40, -1, 0, 1, 40, 80, 1000}){
		char text[32];
		std::snprintf(text, sizeof(text), "%d", value);
		ini.Set("WheelHandPullBackMm", text);
		const int expected = value < -80 ? -80 : value > 80 ? 80 : value;
		Check(ReadWheelHandPullBack(GetPrivateProfileIntA, ini.path) == expected,
			"signed saved hand pullback and limits");
	}
	Check(ClampProfileInt(static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::min()), -100, 150) == -100,
		"INT_MIN clamps at signed lower bound");
	Check(ClampProfileInt(static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()), -100, 150) == 150,
		"INT_MAX clamps at upper bound");
	std::printf("vehicle view settings: PASS (%d checks, production loaders and Win32 INI API)\n", checks);
}
