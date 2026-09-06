#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <limits>

#define nil nullptr
#define FIX_RADAR
#define PROPER_SCALING
#ifndef _MSC_VER
#define _finite std::isfinite
#endif
#define DEGTORAD(value) ((value)*0.01745329251994329577f)
enum { EYE_COUNT = 2, VR_HUD_WIDTH = 1920, VR_HUD_HEIGHT = 1080, MAX_PATH = 260,
       XR_TYPE_COMPOSITION_LAYER_QUAD = 1, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT = 1,
       XR_EYE_VISIBILITY_BOTH = 0, rwCAMERACLEARIMAGE = 1, rwCAMERACLEARZ = 2, GL_NEAREST = 0 };

// Renderer and tracking adapters; the runner includes the production HUD,
// camera restore, hand guard, HUD update and submit ordering below.
struct CVector {
	float x, y, z;
	CVector() = default;
	CVector(float x, float y, float z) : x(x), y(y), z(z) {}
	float MagnitudeSqr() const { return x*x+y*y+z*z; }
	float Magnitude() const { return std::sqrt(MagnitudeSqr()); }
	void Normalise() { const float scale = 1.0f/Magnitude(); x*=scale; y*=scale; z*=scale; }
	CVector &operator-=(const CVector &b) { x-=b.x; y-=b.y; z-=b.z; return *this; }
	CVector &operator*=(float scale) { x*=scale; y*=scale; z*=scale; return *this; }
};
static CVector operator+(const CVector &a, const CVector &b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static CVector operator-(const CVector &a, const CVector &b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static CVector operator*(const CVector &a, float scale) { return {a.x*scale,a.y*scale,a.z*scale}; }
static float DotProduct(const CVector &a, const CVector &b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static CVector CrossProduct(const CVector &a, const CVector &b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
struct CMatrix {
	CVector position = {0,0,0}, right = {1,0,0}, up = {0,0,1}, forward = {0,1,0};
	void SetUnity() { *this = CMatrix(); }
	CVector &GetPosition() { return position; }
	CVector &GetRight() { return right; }
	CVector &GetUp() { return up; }
	CVector &GetForward() { return forward; }
	const CVector &GetPosition() const { return position; }
	const CVector &GetRight() const { return right; }
	const CVector &GetUp() const { return up; }
	const CVector &GetForward() const { return forward; }
};
struct XrVector3f { float x, y, z; };
struct XrQuaternionf { float x, y, z, w; };
struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
struct XrView { XrPosef pose; };
struct Swapchain { uintptr_t handle = 0; };
struct XrCompositionLayerQuad {
	int type, layerFlags;
	uintptr_t space;
	int eyeVisibility;
	struct { uintptr_t swapchain; struct { struct { int x, y; } offset; struct { int width, height; } extent; } imageRect; } subImage;
	XrPosef pose;
	struct { float width, height; } size;
};
static bool NormalizeXrQuaternion(const XrQuaternionf &q, XrQuaternionf *out)
{
	const float norm = q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
	if(!std::isfinite(norm) || norm < 0.0001f) return false;
	const float scale = 1.0f/std::sqrt(norm);
	*out = {q.x*scale,q.y*scale,q.z*scale,q.w*scale};
	return true;
}

#include "wrist-hud-settings-extracted.inc"
static WristHudSettings gWristHud;
static unsigned int gWristHudLayerMask;
static bool gWristHudFailed, gGameplayHudVisible, gVrHudMenuVisible, gFramePrepared, gVrHandsEnabled;
static bool gTrackedHandPoseValid[EYE_COUNT], scopeActive, routeHud, immersiveDriving, failAnchor;
static CMatrix gBaseCamera, handMatrices[EYE_COUNT], vehicleMatrices[EYE_COUNT];
static XrView gLocatedViews[EYE_COUNT];
static XrCompositionLayerQuad gWristHudLayers[WristHudSettings::PANEL_COUNT];
static uintptr_t gGameplaySpace = 17;
static Swapchain gWristHudSwapchain, gHudSwapchain;
static unsigned int handCalls, vehicleCalls, atlasCreates, copies, hudRenders, atlasRenders;
static unsigned int failCopyNumber;
static bool failCreate, failBegin;
static int invalidRectPanel = -1;
static struct { int width, height; } RsGlobal;
static float cachedAspectRatio = 4.0f/3.0f;
namespace CDraw {
	static bool ms_bFixRadar = true, ms_bProperScaling = true;
	static float GetAspectRatio() { return cachedAspectRatio; }
	static void SetAspectRatio(float value) { cachedAspectRatio = value; }
}
static bool &fixRadar = CDraw::ms_bFixRadar, &properScaling = CDraw::ms_bProperScaling;
static bool classicFixRadar, classicProperScaling;
static bool IsTrackedScopeActive() { return scopeActive; }
static bool ShouldRouteGameplayHudToVr() { return routeHud; }
static bool IsImmersiveDrivingActive() { return immersiveDriving; }
static void VrLog(const char *, ...) {}

#include "wrist-hud-tracking-extracted.inc"

struct CVehicle { bool bike; bool IsBike() const { return bike; } };
using CBike = CVehicle;
static CVehicle car = {false}, bike = {true};
static CVehicle *playerVehicle;
static CVehicle *FindPlayerVehicle() { return playerVehicle; }
struct BikeHandlePose { CVector center, right, up, forward; };
static bool BuildBikeHandlePose(CBike *, BikeHandlePose *pose)
{
	++vehicleCalls;
	if(!gFramePrepared || failAnchor) return false;
	*pose = {vehicleMatrices[0].position,vehicleMatrices[0].right,
		vehicleMatrices[0].up,vehicleMatrices[0].forward};
	return true;
}
static bool BuildBikeHandleMatrixInternal(int hand, CMatrix *matrix, bool)
{
	++vehicleCalls;
	if(!gFramePrepared || failAnchor) return false;
	*matrix = vehicleMatrices[hand];
	return true;
}
static bool BuildCarWheelMatrixInternal(int hand, CMatrix *matrix, bool)
{
	++vehicleCalls;
	if(!gFramePrepared || failAnchor) return false;
	*matrix = vehicleMatrices[hand];
	return true;
}
static bool CreateSwapchain(Swapchain &swapchain, int width, int height)
{
	assert(width == VR_HUD_WIDTH && height == VR_HUD_HEIGHT);
	++atlasCreates;
	if(failCreate) return false;
	swapchain.handle = 23;
	return true;
}
// Match the PROPER_SCALING/ASPECT_RATIO_SCALE adapters used by common.h;
// the panel rectangle function itself is extracted from production Hud.cpp.
#define SCREEN_SCALE_X(value) ((value)*float(RsGlobal.width)/640.0f*(4.0f/3.0f)/CDraw::GetAspectRatio())
#define SCREEN_SCALE_X_FIX(value) SCREEN_SCALE_X(value)
#define SCREEN_SCALE_Y(value) ((value)*float(RsGlobal.height)/(properScaling ? 480.0f : 448.0f))
#define SCREEN_SCALE_FROM_RIGHT(value) (float(RsGlobal.width)-SCREEN_SCALE_X(value))
#define SCREEN_SCALE_FROM_BOTTOM(value) (float(RsGlobal.height)-SCREEN_SCALE_Y(value))
#define RADAR_WIDTH (fixRadar ? 82.0f : 94.0f)
#define RADAR_HEIGHT (fixRadar ? 82.0f : 76.0f)
#define RADAR_LEFT 40.0f
#define RADAR_BOTTOM 40.0f
#include "wrist-hud-rect-extracted.inc"
static bool GetVrWristHudPanelRect(int panel, float *left, float *top, float *width, float *height)
{
	assert(RsGlobal.width == VR_HUD_WIDTH && RsGlobal.height == VR_HUD_HEIGHT);
	assert(CDraw::GetAspectRatio() == float(VR_HUD_WIDTH)/float(VR_HUD_HEIGHT));
	assert(fixRadar && properScaling);
	return panel != invalidRectPanel && ProductionWristHudPanelRect(panel,left,top,width,height);
}

struct RwRaster { int id; };
struct RwV2d { float x, y; };
using RwMatrix = CMatrix;
struct RwFrame { RwMatrix matrix; };
struct RwCamera { RwRaster *color, *depth; RwV2d window, offset; float nearPlane; RwFrame frame; };
struct RwRGBA { unsigned char red, green, blue, alpha; };
static RwRaster worldColor = {1}, worldDepth = {2}, originalColor = {3}, originalDepth = {4}, hudColor = {5}, hudDepth = {6};
static RwRaster *gOriginalColor = &originalColor, *gOriginalDepth = &originalDepth, *gHudColor = &hudColor, *gHudDepth = &hudDepth;
static RwV2d gOriginalViewWindow = {0.8f,0.6f}, gOriginalViewOffset = {0.1f,0.2f};
static float gOriginalNearPlane = 0.5f, gOriginalDrawNear = 0.7f, drawNear;
static int gOriginalScreenWidth = 640, gOriginalScreenHeight = 480;
static RwMatrix gOriginalFrameMatrix;
namespace CDraw { static void SetNearClipZ(float value) { drawNear = value; } }
static void RwCameraSetRaster(RwCamera *camera, RwRaster *raster) { camera->color = raster; }
static void RwCameraSetZRaster(RwCamera *camera, RwRaster *raster) { camera->depth = raster; }
static void RwCameraSetViewWindow(RwCamera *camera, const RwV2d *value) { camera->window = *value; }
static void RwCameraSetViewOffset(RwCamera *camera, const RwV2d *value) { camera->offset = *value; }
static void RwCameraSetNearClipPlane(RwCamera *camera, float value) { camera->nearPlane = value; }
static RwRaster *RwCameraGetRaster(RwCamera *camera) { return camera->color; }
static RwRaster *RwCameraGetZRaster(RwCamera *camera) { return camera->depth; }
static RwFrame *RwCameraGetFrame(RwCamera *camera) { return &camera->frame; }
static RwMatrix *RwFrameGetMatrix(RwFrame *frame) { return &frame->matrix; }
static void RwMatrixUpdate(RwMatrix *) {}
static void RwFrameUpdateObjects(RwFrame *) {}
static void RwFrameOrthoNormalize(RwFrame *) {}
static void RwCameraClear(RwCamera *, RwRGBA *, int) {}
static RwCamera *RwCameraBeginUpdate(RwCamera *camera) { return failBegin ? nullptr : camera; }
static void RwCameraEndUpdate(RwCamera *) {}
static bool CopyRasterToSwapchain(RwRaster *raster, int width, int height, Swapchain &, int)
{
	assert(raster == gHudColor && width == VR_HUD_WIDTH && height == VR_HUD_HEIGHT);
	return ++copies != failCopyNumber;
}
static void RenderVrGameplayHud()
{
	assert(RsGlobal.width == VR_HUD_WIDTH && RsGlobal.height == VR_HUD_HEIGHT);
	assert(CDraw::GetAspectRatio() == float(VR_HUD_WIDTH)/float(VR_HUD_HEIGHT));
	classicFixRadar = fixRadar; classicProperScaling = properScaling;
	++hudRenders;
}
static void RenderVrWristHudContents(unsigned int mask)
{
	assert(mask && mask == gWristHudLayerMask);
	assert(RsGlobal.width == VR_HUD_WIDTH && RsGlobal.height == VR_HUD_HEIGHT);
	assert(CDraw::GetAspectRatio() == float(VR_HUD_WIDTH)/float(VR_HUD_HEIGHT));
	assert(fixRadar && properScaling);
	assert(std::fabs(SCREEN_SCALE_X(8.0f)-SCREEN_SCALE_Y(8.0f)) < 0.000001f);
	++atlasRenders;
}

#include "wrist-hud-production-extracted.inc"

static RwCamera camera;
static void PrepareCamera()
{
	gFramePrepared = true;
	camera = {&worldColor,&worldDepth,{1,1},{0,0},0.1f,{}};
	RsGlobal = {3840,2160};
}
static void Reset()
{
	CDraw::SetAspectRatio(4.0f/3.0f);
	fixRadar = properScaling = true;
	gWristHud = WristHudSettings();
	for(int panel = 0; panel < WristHudSettings::PANEL_COUNT; panel++){
		gWristHud.enabled[panel] = true;
		for(int context = 0; context < WristHudSettings::CONTEXT_COUNT; context++)
			for(int side = 0; side < 2; side++)
				gWristHud.placement[panel][context][side][WristHudSettings::SIZE] = 100;
	}
	gWristHud.gaze = false;
	gWristHud.inVehicle = true;
	gWristHud.routingMask = gWristHudLayerMask = 0;
	gWristHudFailed = gVrHudMenuVisible = scopeActive = failAnchor = failCreate = failBegin = false;
	gGameplayHudVisible = gVrHandsEnabled = routeHud = immersiveDriving = true;
	gTrackedHandPoseValid[0] = gTrackedHandPoseValid[1] = true;
	gWristHudSwapchain = {}; gHudSwapchain.handle = 11;
	handCalls = vehicleCalls = atlasCreates = copies = hudRenders = atlasRenders = failCopyNumber = 0;
	invalidRectPanel = -1;
	playerVehicle = nullptr;
	gBaseCamera.SetUnity();
	gBaseCamera.position = {100,200,10};
	for(int hand = 0; hand < EYE_COUNT; hand++){
		handMatrices[hand].position = gBaseCamera.position+ToGameVector({hand ? 0.2f : -0.2f,-0.2f,-0.35f});
		handMatrices[hand].right = ToGameVector({1,0,0});
		handMatrices[hand].up = ToGameVector({0,1,0});
		handMatrices[hand].forward = ToGameVector({0,0,-1});
		vehicleMatrices[hand] = handMatrices[hand];
		gLocatedViews[hand].pose = {{0,0,0,1},{hand ? 0.03f : -0.03f,0,0}};
	}
	PrepareCamera();
}
static void CheckRestored()
{
	assert(!gFramePrepared);
	assert(camera.color == gOriginalColor && camera.depth == gOriginalDepth);
	assert(RsGlobal.width == gOriginalScreenWidth && RsGlobal.height == gOriginalScreenHeight);
	assert(camera.window.x == gOriginalViewWindow.x && camera.offset.y == gOriginalViewOffset.y);
	assert(camera.nearPlane == gOriginalNearPlane && drawNear == gOriginalDrawNear);
}
static bool Near(const XrVector3f &a, const CVector &b)
{
	return (CVector(a.x,a.y,a.z)-b).MagnitudeSqr() < 0.000001f;
}
static void TestNormalSubmit()
{
	Reset();
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xF && gWristHudLayerMask == 0xF);
	assert(handCalls == 2 && vehicleCalls == 0 && atlasCreates == 1);
	assert(copies == 2 && hudRenders == 1 && atlasRenders == 1);
	for(const auto &layer : gWristHudLayers) assert(layer.space == gGameplaySpace && layer.size.width > 0);
	assert(Near(gWristHudLayers[0].pose.position,{-0.2f,-0.235f,-0.28f}));
	CheckRestored();
	CMatrix unused;
	assert(!GetTrackedVisualHandMatrix(0,&unused,nullptr,nullptr));
	handMatrices[0].position = handMatrices[0].position+ToGameVector({0.3f,0.1f,0});
	PrepareCamera();
	assert(SubmitHudForTest(&camera));
	assert(Near(gWristHudLayers[0].pose.position,{0.1f,-0.135f,-0.28f}));
	assert(atlasCreates == 1);
	CheckRestored();
}
static void TestVisibilityAndTracking()
{
	Reset(); gWristHud.gaze = true; gWristHud.gazeRangeCm = 20;
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xF && !gWristHudLayerMask);
	assert(copies == 1 && !atlasRenders);
	PrepareCamera(); gVrHudMenuVisible = true;
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xF && gWristHudLayerMask == 0xF);
	Reset(); gTrackedHandPoseValid[0] = false;
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xE && gWristHudLayerMask == 0xE);
	PrepareCamera(); gTrackedHandPoseValid[1] = false;
	assert(SubmitHudForTest(&camera));
	assert(!gWristHud.routingMask && !gWristHudLayerMask);
	Reset(); gVrHandsEnabled = false;
	assert(SubmitHudForTest(&camera));
	assert(!gWristHud.routingMask && !atlasCreates);
	Reset(); gWristHud.hand[0] = 99;
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xE);
	Reset(); handMatrices[0].position.x = std::numeric_limits<float>::quiet_NaN();
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xE);
	Reset(); invalidRectPanel = WristHudSettings::STATUS;
	assert(SubmitHudForTest(&camera));
	assert(gWristHud.routingMask == 0xD);
}
static void TestOffAndScopes()
{
	Reset();
	for(bool &enabled : gWristHud.enabled) enabled = false;
	assert(SubmitHudForTest(&camera));
	assert(!gWristHud.routingMask && !handCalls && !vehicleCalls && !atlasCreates && copies == 1);
	CheckRestored();
	for(int state = 0; state < 4; state++){
		Reset();
		if(state == 0) scopeActive = true;
		if(state == 1) routeHud = false;
		if(state == 2) gWristHudFailed = true;
		if(state == 3) gGameplayHudVisible = false;
		assert(SubmitHudForTest(&camera) == (state != 3));
		assert(!gWristHud.routingMask && !handCalls && !vehicleCalls && !atlasCreates);
		CheckRestored();
	}
	Reset(); RestoreCamera(&camera);
	assert(SubmitHudForTest(&camera));
	assert(!gWristHud.routingMask && !handCalls && !atlasCreates);
	CheckRestored();
}
static void TestVehicles()
{
	for(CVehicle *vehicle : {&car,&bike}){
		Reset(); playerVehicle = vehicle;
		gTrackedHandPoseValid[0] = gTrackedHandPoseValid[1] = false;
		gVrHandsEnabled = false;
		assert(SubmitHudForTest(&camera));
		assert(gWristHud.routingMask == 0xF && gWristHudLayerMask == 0xF);
		assert(!handCalls && vehicleCalls && atlasCreates == 1);
		assert(Near(gWristHudLayers[0].pose.position,{0,-0.2f,-0.35f}));
		CheckRestored();
		PrepareCamera(); playerVehicle = nullptr;
		assert(SubmitHudForTest(&camera));
		assert(!gWristHud.routingMask && !gWristHudLayerMask);
		for(int failure = 0; failure < 3; failure++){
			Reset(); playerVehicle = vehicle;
			if(failure == 0) gWristHud.inVehicle = false;
			if(failure == 1) immersiveDriving = false;
			if(failure == 2) failAnchor = true;
			assert(SubmitHudForTest(&camera));
			assert(!gWristHud.routingMask && !atlasCreates);
		}
	}
}
static void TestAtlasFailures()
{
	Reset(); failCreate = true;
	assert(SubmitHudForTest(&camera));
	assert(gWristHudFailed && !gWristHud.routingMask && !gWristHudLayerMask);
	PrepareCamera(); failCreate = false;
	assert(SubmitHudForTest(&camera));
	assert(!gWristHud.routingMask && atlasCreates == 1);
	Reset(); failCopyNumber = 2;
	assert(SubmitHudForTest(&camera));
	assert(gWristHudFailed && !gWristHudLayerMask);
	PrepareCamera();
	assert(SubmitHudForTest(&camera));
	assert(!gWristHud.routingMask && !gWristHudLayerMask);
	Reset(); failBegin = true;
	assert(!SubmitHudForTest(&camera));
	assert(!copies && !atlasRenders);
	CheckRestored();
}
static void TestPanelProportions()
{
	for(float aspect : {1.0f,4.0f/3.0f,16.0f/9.0f,21.0f/9.0f,32.0f/9.0f})
		for(bool fixed : {false,true})
			for(bool proper : {false,true})
				for(int size : {40,55,100,250}){
					Reset();
					CDraw::SetAspectRatio(aspect);
					fixRadar = fixed; properScaling = proper;
					gWristHud.placement[0][WristHudSettings::FOOT][1][WristHudSettings::SIZE] = size;
					assert(SubmitHudForTest(&camera));
					const auto &map = gWristHudLayers[WristHudSettings::MAP];
					assert(gWristHud.routingMask == 0xF);
					assert(map.size.width == map.size.height);
					assert(std::fabs(map.size.width-0.115f*float(size)*0.01f) < 0.000001f);
					assert(map.subImage.imageRect.extent.width == 212 && map.subImage.imageRect.extent.height == 212);
					const auto &status = gWristHudLayers[WristHudSettings::STATUS];
					const float statusRatio = 84.0f/196.0f;
					assert(std::fabs(status.size.height/status.size.width-statusRatio) < 0.000001f);
					assert(CDraw::GetAspectRatio() == aspect);
					assert(fixRadar == fixed && properScaling == proper);
					assert(classicFixRadar == fixed && classicProperScaling == proper);
					CheckRestored();
				}
	for(CVehicle *vehicle : {&car,&bike}){
		Reset(); playerVehicle = vehicle;
		assert(SubmitHudForTest(&camera));
		assert(gWristHudLayers[0].size.width == gWristHudLayers[0].size.height);
	}
	for(int failure = 0; failure < 4; failure++){
		Reset(); CDraw::SetAspectRatio(2.5f);
		if(failure == 0) failCreate = true;
		if(failure == 1) failCopyNumber = 1;
		if(failure == 2) failCopyNumber = 2;
		if(failure == 3) failBegin = true;
		SubmitHudForTest(&camera);
		assert(CDraw::GetAspectRatio() == 2.5f);
		CheckRestored();
	}
}
#ifndef WRIST_HUD_LEGACY
static void TestSnapshotLifetime()
{
	Reset();
	WristHudFrame frame;
	CaptureWristHudFrame(&frame);
	assert(frame.anchorMask == 3);
	RestoreCamera(&camera);
	gBaseCamera.position = {500,600,700};
	handMatrices[0].position = {900,900,900};
	RsGlobal = {VR_HUD_WIDTH,VR_HUD_HEIGHT};
	CDraw::SetAspectRatio(float(VR_HUD_WIDTH)/float(VR_HUD_HEIGHT));
	PrepareWristHud(frame);
	assert(gWristHud.routingMask == 0xF);
	assert(Near(gWristHudLayers[0].pose.position,{-0.2f,-0.235f,-0.28f}));
	CaptureWristHudFrame(&frame);
	assert(!frame.anchorMask);
	PrepareWristHud(frame);
	assert(!gWristHud.routingMask && !gWristHudLayerMask);
	WristHudFrame empty;
	PrepareWristHud(empty);
	assert(!gWristHud.routingMask && !gWristHudLayerMask);
}
#endif
int main()
{
	TestNormalSubmit();
	TestVisibilityAndTracking();
	TestOffAndScopes();
	TestVehicles();
	TestAtlasFailures();
	TestPanelProportions();
#ifndef WRIST_HUD_LEGACY
	TestSnapshotLifetime();
#endif
	std::puts("wrist HUD lifecycle: PASS (production submit/restore/update/rect, hand routing, gaze, tracking, OFF/scopes, vehicles, failures, snapshot lifetime, 80 panel-aspect/size combinations)");
}
