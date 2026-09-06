#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include "../../../src/vr/WristHudSettings.h"

// Minimal vector/OpenXR adapters let the production wrist math run without
// creating a renderer or connecting to a headset.
struct CVector {
	float x, y, z;
	CVector(float x, float y, float z) : x(x), y(y), z(z) {}
	float MagnitudeSqr() const { return x*x+y*y+z*z; }
	void Normalise() { const float s = 1.0f/sqrtf(MagnitudeSqr()); x*=s; y*=s; z*=s; }
	CVector &operator-=(const CVector &b) { x-=b.x; y-=b.y; z-=b.z; return *this; }
};
static CVector operator+(const CVector &a, const CVector &b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static CVector operator-(const CVector &a, const CVector &b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static CVector operator*(const CVector &a, float s) { return {a.x*s,a.y*s,a.z*s}; }
static float DotProduct(const CVector &a, const CVector &b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static CVector CrossProduct(const CVector &a, const CVector &b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
struct XrQuaternionf { float x, y, z, w; };
static bool NormalizeXrQuaternion(const XrQuaternionf &q, XrQuaternionf *out)
{
	const float norm = q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
	if(!std::isfinite(norm) || norm < 0.0001f) return false;
	const float s = 1.0f/sqrtf(norm);
	*out = {q.x*s,q.y*s,q.z*s,q.w*s};
	return true;
}
#include "../../../src/vr/WristHudMath.h"

static bool Near(const CVector &a, const CVector &b)
{
	return (a-b).MagnitudeSqr() < 0.000001f;
}
static CVector QuaternionRotate(const XrQuaternionf &q, const CVector &v)
{
	const CVector xyz(q.x,q.y,q.z);
	const CVector t = CrossProduct(xyz,v)*2.0f;
	return v+t*q.w+CrossProduct(xyz,t);
}
static void TestOrientation()
{
	const float pi = 3.14159265358979323846f;
	CVector right(1,0,0), up(0,1,0), back(0,0,1);
	assert(ApplyWristPanelRotation(right,up,back,0,0,0));
	assert(Near(QuaternionRotate(WristQuaternion(right,up),{1,0,0}),right));
	for(int axis = 0; axis < 3; axis++){
		right = {1,0,0}; up = {0,1,0};
		assert(ApplyWristPanelRotation(right,up,back,
			axis == 0 ? pi*0.5f : 0,axis == 1 ? pi*0.5f : 0,axis == 2 ? pi*0.5f : 0));
		if(axis == 0) assert(Near(up,{0,0,1}));
		if(axis == 1) assert(Near(right,{0,0,-1}));
		if(axis == 2) assert(Near(right,{0,1,0}));
	}
	for(float pitch : {-pi,-pi*0.5f,0.0f,pi*0.5f,pi})
		for(float yaw : {-pi,-pi*0.5f,0.0f,pi*0.5f,pi})
			for(float roll : {-pi,-pi*0.5f,0.0f,pi*0.5f,pi}){
				right = {1,0,0}; up = {0,1,0};
				assert(ApplyWristPanelRotation(right,up,back,pitch,yaw,roll));
				assert(fabsf(DotProduct(right,up)) < 0.00001f);
				const XrQuaternionf q = WristQuaternion(right,up);
				assert(fabsf(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1) < 0.00001f);
				assert(Near(QuaternionRotate(q,{1,0,0}),right));
				assert(Near(QuaternionRotate(q,{0,1,0}),up));
				assert(Near(QuaternionRotate(q,{0,0,1}),CrossProduct(right,up)));
			}
	// Invalid anchors must preserve the classic HUD instead of claiming a panel.
	right = {0,0,0}; up = {0,1,0};
	assert(!ApplyWristPanelRotation(right,up,back,0,0,0));
	right = {1,0,0}; up = {2,0,0};
	assert(!ApplyWristPanelRotation(right,up,back,0,0,0));
	right = {std::numeric_limits<float>::infinity(),0,0}; up = {0,1,0};
	assert(!ApplyWristPanelRotation(right,up,back,0,0,0));
	right = {1,0,0}; up = {0,std::numeric_limits<float>::quiet_NaN(),0};
	assert(!ApplyWristPanelRotation(right,up,back,0,0,0));
}

static void TestSettings(const char *path)
{
	using S = WristHudSettings;
	S settings;
	settings.Load(path);
	assert(strcmp(settings.PresetName(),"CLASSIC") == 0);
	assert(!settings.inVehicle && settings.gaze && settings.classicWeapon && settings.classicClock);
	for(int p = 0; p < S::PANEL_COUNT; p++){
		assert(!settings.enabled[p]);
		for(int c = 0; c < S::CONTEXT_COUNT; c++)
			for(int side = 0; side < 2; side++)
				for(int f = 0; f < S::FIELD_COUNT; f++)
					assert(settings.placement[p][c][side][f] == S::DefaultValue(p,c,side,f));
	}
	settings.SetPreset(true);
	S loaded; loaded.Load(path);
	assert(strcmp(loaded.PresetName(),"IMMERSIVE") == 0);
	assert(loaded.inVehicle && loaded.gaze && !loaded.classicWeapon && !loaded.classicClock);
	loaded.SetPreset(false);
	settings.Load(path);
	assert(strcmp(settings.PresetName(),"CLASSIC") == 0);
	assert(!settings.inVehicle && !settings.gaze);
	settings.panel = S::STATUS; settings.context = S::CAR;
	settings.underside[S::STATUS] = false;
	settings.ChangePlacement(S::ALONG,-10000);
	settings.ChangePlacement(S::YAW,10000);
	settings.ChangePlacement(S::SIZE,-10000);
	loaded.Load(path);
	assert(loaded.owned[S::STATUS][S::CAR]);
	assert(loaded.placement[S::STATUS][S::CAR][0][S::ALONG] == -300);
	assert(loaded.placement[S::STATUS][S::CAR][0][S::YAW] == 1800);
	assert(loaded.placement[S::STATUS][S::CAR][0][S::SIZE] == 40);
	assert(loaded.placement[S::STATUS][S::CAR][0][S::LIFT] == S::DefaultValue(S::STATUS,S::CAR,0,S::LIFT));
	settings.ResetPlacement(true);
	loaded.Load(path);
	for(int f = 0; f < S::FIELD_COUNT; f++)
		assert(loaded.placement[S::STATUS][S::CAR][0][f] == loaded.placement[S::STATUS][S::CAR][1][f]);
	settings.ResetPlacement(false);
	loaded.Load(path);
	for(int f = 0; f < S::FIELD_COUNT; f++)
		assert(loaded.placement[S::STATUS][S::CAR][0][f] == S::DefaultValue(S::STATUS,S::CAR,0,f));
	settings.Save("AmmoColourRed",9999);
	settings.Save("AmmoColourGreen",-100);
	settings.Save("WristPanelGazeRangeCm",-40);
	loaded.Load(path);
	assert(loaded.ammoColour[0] == 255 && loaded.ammoColour[1] == 0 && loaded.gazeRangeCm == 20);
	char key[96];
	settings.MakePlacementKey(key,S::MAP,S::FOOT,0,S::ALONG);
	assert(strcmp(key,"WristOuterAlong") == 0);
	settings.MakePlacementKey(key,S::AMMO,S::BIKE,1,S::ROLL);
	assert(strcmp(key,"WristAmmoBikeInnerRoll") == 0);
}

int main()
{
	char directory[MAX_PATH], path[MAX_PATH];
	const DWORD length = GetTempPathA(MAX_PATH,directory);
	assert(length && length < MAX_PATH);
	assert(GetTempFileNameA(directory,"wht",0,path));
	TestOrientation();
	TestSettings(path);
	assert(DeleteFileA(path));
	std::puts("wrist HUD: PASS (125 orientation combinations, axes, invalid bases, defaults, INI roundtrip, bounds, copy/reset)");
}
