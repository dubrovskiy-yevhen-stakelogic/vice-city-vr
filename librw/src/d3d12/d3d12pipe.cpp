#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <unordered_map>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

#ifdef RW_D3D12

struct Vertex
{
	V3d position;
	V3d normal;
	RGBA color;
	TexCoords texCoords;
	float32 weights[4];
	uint8 boneIndices[4];
};

struct MeshDraw
{
	uint32 numIndices;
	uint32 startIndex;
	Material *material;
	bool32 vertexAlpha;
};

enum { DYNAMIC_VERTEX_FRAME_COUNT = 3 };

struct D3D12InstanceDataHeader : InstanceDataHeader
{
	uint32 serialNumber;
	uint32 numMeshes;
	uint32 numVertices;
	uint32 numIndices;
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12Resource *vertexBuffer;
	ID3D12Resource *vertexBuffers[DYNAMIC_VERTEX_FRAME_COUNT];
	ID3D12Resource *indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	MeshDraw *meshes;
};

static ID3D12RootSignature *rootSignature;
static ID3D12RootSignature *stereoRootSignature;
enum WorldBlendMode {
	WORLD_BLEND_OPAQUE,
	WORLD_BLEND_ALPHA,
	WORLD_BLEND_ADD_ONE,
	WORLD_BLEND_ADD_ALPHA,
	WORLD_BLEND_SHADOW,
	WORLD_BLEND_INVERSE_DEST,
	WORLD_BLEND_REPLACE,
	WORLD_BLEND_KEEP_DESTINATION,
	WORLD_BLEND_MODULATE_DESTINATION,
	WORLD_BLEND_ALPHA_INVERSE_DEST_ALPHA,
	WORLD_BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA,
	WORLD_BLEND_COUNT
};
enum WorldDepthMode {
	WORLD_DEPTH_DISABLED,
	WORLD_DEPTH_WRITE_ONLY,
	WORLD_DEPTH_TEST_ONLY,
	WORLD_DEPTH_TEST_WRITE,
	WORLD_DEPTH_COUNT
};
enum WorldCullMode {
	WORLD_CULL_NONE,
	WORLD_CULL_BACK,
	WORLD_CULL_FRONT,
	WORLD_CULL_COUNT
};
static ID3D12PipelineState *worldPipelines[WORLD_BLEND_COUNT][WORLD_DEPTH_COUNT][WORLD_CULL_COUNT];
static ID3D12PipelineState *stereoWorldPipelines[WORLD_BLEND_COUNT][WORLD_DEPTH_COUNT][WORLD_CULL_COUNT];
static ID3D12PipelineState *worldMotionPipelines[WORLD_BLEND_COUNT][WORLD_DEPTH_COUNT][WORLD_CULL_COUNT];
static ID3D12PipelineState *stereoWorldMotionPipelines[WORLD_BLEND_COUNT][WORLD_DEPTH_COUNT][WORLD_CULL_COUNT];
static Raster *whiteRaster;
static bool32 pipelineReady;

enum {
	BONE_FRAME_COUNT = 3,
	MAX_SKIN_BONES = 64,
	MAX_WORLD_LIGHTS = 8,
	// A VR frame records the complete world twice before submission. The old
	// one-view arena could run out midway through the right eye, silently making
	// the remaining atomics (often vehicles and buildings) disappear.
	BONE_UPLOAD_SIZE = 16*1024*1024
};

struct BoneArena
{
	ID3D12Resource *resource;
	uint8 *mapped;
	uint32 offset;
};

struct LightingConstants
{
	float ambient[4];
	float surface[4];
	float colorRadius[MAX_WORLD_LIGHTS][4];
	float positionCos[MAX_WORLD_LIGHTS][4];
	float directionClamp[MAX_WORLD_LIGHTS][4];
	// Per-mesh material colour shares the lighting CBV; SSR uses the remaining
	// mono-root constants.
	float materialColor[4];
	// x = MatFX environment coefficient, restored from the PC-quarter value;
	// y = the game-tagged water draw. z/w are reserved.
	float reflectionMaterial[4];
	// Previous rigid transform plus motionParams.x validity. Object motion is
	// reconstructed per vertex in the world shader and stored as a complete
	// previous-to-current NDC delta for the DLAA compute pass.
	float previousWorld[16];
	float motionParams[4];
};

static BoneArena boneArenas[BONE_FRAME_COUNT];
static uint32 activeBoneArena = UINT32_MAX;
static WorldRenderProfile worldRenderProfile;

enum { STEREO_ATOMIC_CACHE_COUNT = 8192 };

struct StereoAtomicCacheEntry
{
	Atomic *atomic;
	uint32 generation;
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	bool32 isSkinned;
	bool32 previousSkinValid;
	bool32 motionEnabled;
	LightingConstants lighting;
};

static StereoAtomicCacheEntry stereoAtomicCache[STEREO_ATOMIC_CACHE_COUNT];
static uint32 stereoCacheGeneration = 1;
static int32 stereoWorldEye = -1;
static bool32 stereoSinglePassActive;
static uint32 currentWorldStage;

struct AtomicMotionRecord
{
	float32 model[16];
	float32 previousModelThisFrame[16];
	float32 paramsThisFrame[4];
	uint64 serial;
	bool32 initialized;
};

static std::unordered_map<Atomic*, AtomicMotionRecord> atomicMotionHistory;
struct SkinMotionRecord
{
	float32 matrices[MAX_SKIN_BONES*16];
	float32 previousMatricesThisFrame[MAX_SKIN_BONES*16];
	uint64 serial;
	bool32 initialized;
	bool32 previousValidThisFrame;
};
static std::unordered_map<Atomic*, SkinMotionRecord> skinMotionHistory;
static uint64 motionFrameSerial;
static float32 previousStereoCameraConstants[2][32];
static bool32 previousStereoCameraValid[2];

// Redundant-state filter for the world pass. GPU timestamps put scene
// rasterization at 10ms while the pixel cost of the same frame is under a
// tenth of that, and cutting resolution four-fold moved it not at all: the
// cost is in per-draw state, not shading. Vice City submits thousands of small
// meshes that overwhelmingly repeat the previous draw's pipeline, buffers,
// texture and sampler, so each of those calls is skipped when nothing changed.
// Reset at the start of every command list, because a fresh list has no state.
struct WorldDrawState
{
	ID3D12RootSignature *rootSignature;
	ID3D12PipelineState *pipeline;
	D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer;
	D3D12_GPU_VIRTUAL_ADDRESS indexBuffer;
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	D3D12_GPU_VIRTUAL_ADDRESS lightingAddress;
	D3D12_GPU_VIRTUAL_ADDRESS stereoCamera;
	D3D12_GPU_VIRTUAL_ADDRESS reflectionConstants;
	UINT64 texture;
	UINT64 sampler;
	UINT64 reflectionTexture;
	UINT64 reflectionSampler;
	uint32 topology;
	ID3D12GraphicsCommandList *list;
};
static WorldDrawState worldDrawState;
static bool32 distanceFogEnabled = 1;
static uint32 maskedMipBias;
static bool32 vehicleAlphaPass;

void
setDistanceFogEnabled(bool32 enabled)
{
	distanceFogEnabled = enabled != 0;
}

void
setMaskedMipBias(uint32 halfLevels)
{
	maskedMipBias = halfLevels > 3 ? 3 : halfLevels;
}

void
setVehicleAlphaPass(bool32 enabled)
{
	vehicleAlphaPass = enabled != 0;
}

void
resetWorldDrawState(void)
{
	memset(&worldDrawState, 0, sizeof(worldDrawState));
}

void
setWorldRenderStage(uint32 stage)
{
	currentWorldStage = stage < WORLD_STAGE_COUNT ? stage : WORLD_STAGE_OTHER;
}
static uint32 stereoRightCameraFrame = UINT32_MAX;
static uint32 stereoRightCameraUploadFrame = UINT32_MAX;
static float stereoRightCameraConstants[32];
static D3D12_GPU_VIRTUAL_ADDRESS stereoRightCameraAddress;
static float stereoTemporalCameraConstants[2][32];
static bool32 stereoTemporalCameraValid[2];
// Clip-space jitter applied to the current eye projection. Object motion is
// exported as non-jittered motion (matching Streamline's resource tag), so the
// world shader removes this offset from the current clip position before it
// compares against the previous non-jittered camera.
static float32 stereoTemporalJitterClip[2][2];
// Desktop (mono window) world camera snapshot for the flat-mode temporal AA
// path. Same convention as the stereo capture: non-jittered devView/devProj.
static float desktopTemporalCameraConstants[32];
static bool32 desktopTemporalCameraValid;

struct ScreenSpaceReflectionConstants
{
	float previousView[16];
	float previousProjection[16];
	// Previous non-jittered cameras for exact per-vertex temporal motion. Keep
	// both eyes here because this CBV is frame-global and already bound to every
	// world draw, including the non-SPS fallback.
	float temporalPreviousView[2][16];
	float temporalPreviousProjection[2][16];
	float temporalMotionValid[4];
	// xy = left-eye current clip jitter, zw = right-eye current clip jitter.
	float temporalJitterClip[4];
	// x = car strength, y = water strength, z = animation time, w = valid.
	float params[4];
	// xy = full double-wide dimensions, zw = reciprocal dimensions.
	float textureInfo[4];
	// x = accumulated wetness, y = current rainfall, z = surface mode.
	float rainParams[4];
	// x = puddle coverage, y = edge softness, z = reflection, w = ripple.
	float puddleParams[4];
	// x = ocean wave strength, y = speed, z = mirror distortion.
	float waterParams[4];
	// x/y/z = sky sheen, sun glint, dynamic-light sparks.
	float waterExtraParams[4];
	float waterSunDirection[4];
	float waterSunColour[4];
	float waterSkyColour[4];
	// x = live car reflection multiplier, y = distance (zero unlimited).
	float carSsrParams[4];
	// Frame-global GTA point lights in world space.
	float dynamicPositionRadius[MAX_DYNAMIC_POINT_LIGHTS][4];
	float dynamicColourSpot[MAX_DYNAMIC_POINT_LIGHTS][4];
	float dynamicDirection[MAX_DYNAMIC_POINT_LIGHTS][4];
	// x = count, y = air-glow strength; z/w reserved.
	float dynamicParams[4];
};

static Raster *screenSpaceReflectionHistory;
static bool32 screenSpaceReflectionHistoryValid;
static bool32 im3DWaterDraw;
static float32 screenSpaceReflectionCarPercent;
static float32 screenSpaceReflectionWaterPercent;
static float32 screenSpaceReflectionOceanWavePercent = 100.0f;
static float32 screenSpaceReflectionTime;
static bool32 effectsTimeExternal;
static float32 waterSpeedPercent = 100.0f;
static float32 waterDistortionPercent = 100.0f;
static float32 waterExtraPercent[3];
static float32 waterSunDirection[3] = { 0.0f, 0.0f, 1.0f };
static float32 waterSunColour[3];
static float32 waterSkyColour[3];
static float32 carSsrStrengthPercent = 100.0f;
static float32 carSsrDistanceMetres;
static float32 rainSurfaceWetness;
static float32 rainSurfaceRainfall;
static int32 rainSurfaceMode;
static float32 rainSurfaceCoveragePercent = 100.0f;
static float32 rainSurfaceEdgeSoftnessPercent = 150.0f;
static float32 rainSurfaceRipplePercent = 20.0f;
static float32 rainSurfaceReflectionPercent = 75.0f;
static DynamicPointLight dynamicPointLights[MAX_DYNAMIC_POINT_LIGHTS];
static uint32 dynamicPointLightCount;
static float32 dynamicPointLightGlowStrength;
static float screenSpaceReflectionPreviousCamera[32];
// Left-eye matrices used to produce the SSR history colour.
static float screenSpaceReflectionCurrentLeftCamera[32];
static bool32 screenSpaceReflectionCurrentLeftCameraValid;
static D3D12_GPU_VIRTUAL_ADDRESS screenSpaceReflectionConstantAddress[BONE_FRAME_COUNT];
static uint32 screenSpaceReflectionConstantGeneration[BONE_FRAME_COUNT];
static uint32 boneArenaGeneration[BONE_FRAME_COUNT];

enum { STEREO_WORLD_DRAW_CAPACITY = 16384 };

struct StereoWorldDraw
{
	D3D12_PRIMITIVE_TOPOLOGY topology;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	ID3D12PipelineState *pipeline;
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	D3D12_GPU_VIRTUAL_ADDRESS lightingAddress;
	D3D12_GPU_DESCRIPTOR_HANDLE texture;
	D3D12_GPU_DESCRIPTOR_HANDLE sampler;
	D3D12_GPU_VIRTUAL_ADDRESS reflectionConstants;
	D3D12_GPU_DESCRIPTOR_HANDLE reflectionTexture;
	D3D12_GPU_DESCRIPTOR_HANDLE reflectionSampler;
	float constants[54];
	uint32 numIndices;
	uint32 startIndex;
};

struct StereoWorldRange
{
	uint32 first;
	uint32 count;
	bool32 complete;
};

static StereoWorldDraw stereoWorldDraws[STEREO_WORLD_DRAW_CAPACITY];
static StereoWorldRange stereoWorldRanges[STEREO_WORLD_SEGMENT_COUNT];
static uint32 stereoWorldDrawCount;
static int32 stereoCaptureSegment = -1;
static bool32 stereoWorldPacketValid;
static uint32 stereoWorldPacketGeneration;

struct StereoBundleFrame
{
	ID3D12CommandAllocator *allocator;
	ID3D12GraphicsCommandList *segments[STEREO_WORLD_SEGMENT_COUNT];
};

static StereoBundleFrame stereoBundleFrames[BONE_FRAME_COUNT];
static HANDLE stereoBundleWorkerThread;
static HANDLE stereoBundleWorkEvent;
static HANDLE stereoBundleDoneEvent;
static HANDLE stereoBundleStopEvent;
static volatile LONG stereoBundlePending;
static volatile LONG stereoBundleSucceeded;
static uint32 stereoBundleTaskFrame;
static uint32 stereoBundleTaskGeneration;
static float stereoBundleTaskView[16];
static float stereoBundleTaskProjection[16];
static uint32 stereoBundleCompletedFrame;
static uint32 stereoBundleCompletedGeneration;
static float32 stereoBundleCompletedBuildMs;
static bool32 stereoBundleResourcesReady;

static bool32 waitForStereoWorldBundleBuild(void);
static bool32 createStereoBundleResources(void);
static void shutdownStereoBundleResources(void);

static double
profileNowMs(void)
{
	static LARGE_INTEGER frequency = {};
	if(frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart*1000.0/frequency.QuadPart;
}

void
resetWorldRenderProfile(void)
{
	memset(&worldRenderProfile, 0, sizeof(worldRenderProfile));
	// Start every frame unattributed so a label left over from the previous
	// frame cannot be blamed for the next one's opening draws.
	currentWorldStage = WORLD_STAGE_OTHER;
}

void
getWorldRenderProfile(WorldRenderProfile *profile)
{
	if(profile)
		*profile = worldRenderProfile;
}

void
beginStereoWorldFrame(void)
{
	for(int32 previousEye = 0; previousEye < 2; previousEye++){
		previousStereoCameraValid[previousEye] =
			stereoTemporalCameraValid[previousEye];
		if(stereoTemporalCameraValid[previousEye])
			memcpy(previousStereoCameraConstants[previousEye],
			       stereoTemporalCameraConstants[previousEye],
			       sizeof(previousStereoCameraConstants[previousEye]));
	}
	motionFrameSerial++;
	if(motionFrameSerial == 0){
		atomicMotionHistory.clear();
		skinMotionHistory.clear();
		motionFrameSerial = 1;
	}
	// A cancelled OpenXR frame may omit right-eye replay; wait before reuse.
	waitForStereoWorldBundleBuild();
	stereoCacheGeneration++;
	if(stereoCacheGeneration == 0){
		memset(stereoAtomicCache, 0, sizeof(stereoAtomicCache));
		stereoCacheGeneration = 1;
	}
	stereoWorldDrawCount = 0;
	stereoCaptureSegment = -1;
	stereoWorldPacketValid = 1;
	stereoWorldPacketGeneration++;
	if(stereoWorldPacketGeneration == 0)
		stereoWorldPacketGeneration = 1;
	memset(stereoWorldRanges, 0, sizeof(stereoWorldRanges));
	stereoTemporalCameraValid[0] = 0;
	stereoTemporalCameraValid[1] = 0;
	memset(stereoTemporalJitterClip, 0, sizeof(stereoTemporalJitterClip));
	screenSpaceReflectionCurrentLeftCameraValid = 0;
}

void
forgetAtomicMotion(Atomic *atomic)
{
	if(atomic){
		atomicMotionHistory.erase(atomic);
		skinMotionHistory.erase(atomic);
	}
}

void
setStereoWorldEye(int32 eye)
{
	if(eye < 0 && stereoSinglePassActive)
		endStereoSinglePass();
	stereoWorldEye = eye;
}

void
captureStereoWorldCamera(int32 eye, float32 jitterClipX, float32 jitterClipY)
{
	if(eye < 0 || eye > 1 || engine == nil || engine->currentCamera == nil)
		return;
	// Streamline consumes non-jittered camera matrices. Capture them before
	// applying the sub-pixel offset used by the actual DLAA render.
	memcpy(stereoTemporalCameraConstants[eye],
	       &engine->currentCamera->devView, 16*sizeof(float));
	memcpy(stereoTemporalCameraConstants[eye] + 16,
	       &engine->currentCamera->devProj, 16*sizeof(float));
	stereoTemporalCameraValid[eye] = 1;
	// The Halton offset is frame-global (GetTemporalJitterClip has no eye
	// argument). Mirror it immediately so a frame-global world CBV created by
	// the first left-eye sky draw already contains the right-eye value too.
	for(int32 jitterEye = 0; jitterEye < 2; jitterEye++){
		stereoTemporalJitterClip[jitterEye][0] = jitterClipX;
		stereoTemporalJitterClip[jitterEye][1] = jitterClipY;
	}
	float *projection = (float*)&engine->currentCamera->devProj;
	projection[8] += jitterClipX;
	projection[9] += jitterClipY;
	if(eye == 0){
		memcpy(screenSpaceReflectionCurrentLeftCamera,
		       &engine->currentCamera->devView, 16*sizeof(float));
		memcpy(screenSpaceReflectionCurrentLeftCamera + 16,
		       &engine->currentCamera->devProj, 16*sizeof(float));
		screenSpaceReflectionCurrentLeftCameraValid = 1;
	}else{
		memcpy(stereoRightCameraConstants,
		       &engine->currentCamera->devView, 16*sizeof(float));
		memcpy(stereoRightCameraConstants + 16,
		       &engine->currentCamera->devProj, 16*sizeof(float));
		stereoRightCameraFrame = getFrameIndex() % BONE_FRAME_COUNT;
		stereoRightCameraUploadFrame = UINT32_MAX;
		stereoRightCameraAddress = 0;
	}
}

bool32
getStereoWorldCamera(int32 eye, float32 view[16], float32 projection[16])
{
	if(eye < 0 || eye > 1 || view == nil || projection == nil ||
	   !stereoTemporalCameraValid[eye])
		return 0;
	memcpy(view, stereoTemporalCameraConstants[eye], 16*sizeof(float));
	memcpy(projection, stereoTemporalCameraConstants[eye] + 16, 16*sizeof(float));
	return 1;
}

static bool32
isPotentiallyDynamicAtomic(bool32 isSkinned)
{
	return isSkinned || currentWorldStage == WORLD_STAGE_ENTITIES ||
		currentWorldStage == WORLD_STAGE_BOATS ||
		currentWorldStage == WORLD_STAGE_FADING ||
		currentWorldStage == WORLD_STAGE_FX_MOVING ||
		currentWorldStage == WORLD_STAGE_FX_FIRSTPERSON ||
		currentWorldStage == WORLD_STAGE_FX_HANDS;
}

static bool32
atomicModelMoved(const float32 previous[16], const float32 current[16])
{
	// The entity pass contains static buildings as well as genuinely moving
	// objects. Treating every entry as dynamic replaced the depth-derived,
	// per-pixel camera reprojection with one nointerpolation vector measured at
	// the atomic's origin. That approximation is especially wrong across a
	// large facade and makes a stationary building walk inside DLAA history.
	for(uint32 component = 0; component < 16; component++)
		if(fabsf(previous[component]-current[component]) > 1.0e-5f)
			return 1;
	return 0;
}

static void
getAtomicMotion(Atomic *atomic, const float32 model[16], bool32 potentiallyDynamic,
	            bool32 isSkinned, bool32 previousSkinValid,
	            float32 previousWorld[16], float32 params[4])
{
	memcpy(previousWorld, model, 16*sizeof(float32));
	memset(params, 0, 4*sizeof(float32));
	if(!potentiallyDynamic || stereoWorldEye < 0 || stereoWorldEye > 1)
		return;
	AtomicMotionRecord &record = atomicMotionHistory[atomic];
	if(record.serial == motionFrameSerial){
		memcpy(previousWorld, record.previousModelThisFrame,
		       sizeof(record.previousModelThisFrame));
		memcpy(params, record.paramsThisFrame, sizeof(record.paramsThisFrame));
		return;
	}
	const bool32 consecutive = record.initialized &&
		record.serial+1 == motionFrameSerial;
	const bool32 rigidMoved = consecutive &&
		atomicModelMoved(record.model, model);
	const bool32 skinMoved = consecutive && isSkinned && previousSkinValid;
	if(rigidMoved || skinMoved){
		memcpy(previousWorld, record.model, sizeof(record.model));
		params[0] = 1.0f;
		params[1] = skinMoved ? 1.0f : 0.0f;
	}
	memcpy(record.model, model, sizeof(record.model));
	memcpy(record.previousModelThisFrame, previousWorld,
	       sizeof(record.previousModelThisFrame));
	memcpy(record.paramsThisFrame, params, sizeof(record.paramsThisFrame));
	record.serial = motionFrameSerial;
	record.initialized = 1;
}

void
captureDesktopWorldCamera(void)
{
	if(engine == nil || engine->currentCamera == nil)
		return;
	memcpy(desktopTemporalCameraConstants,
	       &engine->currentCamera->devView, 16*sizeof(float));
	memcpy(desktopTemporalCameraConstants + 16,
	       &engine->currentCamera->devProj, 16*sizeof(float));
	desktopTemporalCameraValid = 1;
}

bool32
getDesktopWorldCamera(float32 view[16], float32 projection[16])
{
	if(view == nil || projection == nil || !desktopTemporalCameraValid)
		return 0;
	memcpy(view, desktopTemporalCameraConstants, 16*sizeof(float));
	memcpy(projection, desktopTemporalCameraConstants + 16, 16*sizeof(float));
	return 1;
}

void
beginStereoWorldCapture(uint32 segment)
{
	if(stereoWorldEye != 0 || !stereoWorldPacketValid ||
	   segment >= STEREO_WORLD_SEGMENT_COUNT){
		stereoCaptureSegment = -1;
		return;
	}
	StereoWorldRange &range = stereoWorldRanges[segment];
	range.first = stereoWorldDrawCount;
	range.count = 0;
	range.complete = 0;
	stereoCaptureSegment = (int32)segment;
}

void
endStereoWorldCapture(uint32 segment)
{
	if(stereoCaptureSegment != (int32)segment ||
	   segment >= STEREO_WORLD_SEGMENT_COUNT){
		stereoCaptureSegment = -1;
		return;
	}
	StereoWorldRange &range = stereoWorldRanges[segment];
	range.count = stereoWorldDrawCount - range.first;
	range.complete = stereoWorldPacketValid;
	stereoCaptureSegment = -1;
}

void
cancelStereoWorldPacket(void)
{
	waitForStereoWorldBundleBuild();
	stereoCaptureSegment = -1;
	stereoWorldPacketValid = 0;
	memset(stereoWorldRanges, 0, sizeof(stereoWorldRanges));
}

void
queueStereoWorldBundleBuild(const float32 *view, const float32 *projection)
{
	if(!stereoBundleResourcesReady || stereoWorldEye != 0 ||
	   view == nil || projection == nil ||
	   !stereoWorldPacketValid || stereoBundleWorkEvent == nil)
		return;
	for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++)
		if(!stereoWorldRanges[segment].complete)
			return;
	waitForStereoWorldBundleBuild();
	stereoBundleTaskFrame = getFrameIndex() % BONE_FRAME_COUNT;
	stereoBundleTaskGeneration = stereoWorldPacketGeneration;
	memcpy(stereoBundleTaskView, view,
	       sizeof(stereoBundleTaskView));
	memcpy(stereoBundleTaskProjection, projection,
	       sizeof(stereoBundleTaskProjection));
	stereoBundleCompletedBuildMs = 0.0f;
	InterlockedExchange(&stereoBundleSucceeded, 0);
	InterlockedExchange(&stereoBundlePending, 1);
	ResetEvent(stereoBundleDoneEvent);
	if(!SetEvent(stereoBundleWorkEvent)){
		InterlockedExchange(&stereoBundlePending, 0);
		SetEvent(stereoBundleDoneEvent);
	}
}

static StereoAtomicCacheEntry*
findStereoAtomicCache(Atomic *atomic, bool32 create)
{
	if(atomic == nil || stereoWorldEye < 0)
		return nil;
	const uintptr_t key = (uintptr_t)atomic;
	uint32 slot = (uint32)(((key >> 4) * 2654435761u) &
	                       (STEREO_ATOMIC_CACHE_COUNT - 1));
	for(uint32 probe = 0; probe < STEREO_ATOMIC_CACHE_COUNT; probe++){
		StereoAtomicCacheEntry *entry =
			&stereoAtomicCache[(slot + probe) & (STEREO_ATOMIC_CACHE_COUNT - 1)];
		if(entry->generation != stereoCacheGeneration){
			if(!create)
				return nil;
			entry->generation = stereoCacheGeneration;
			entry->atomic = atomic;
			return entry;
		}
		if(entry->atomic == atomic)
			return entry;
	}
	return nil;
}

template<class T>
static void
releaseCom(T *&object)
{
	if(object){
		object->Release();
		object = nil;
	}
}

static bool32
recordStereoWorldBundles(uint32 frame)
{
	if(frame >= BONE_FRAME_COUNT || !stereoWorldPacketValid)
		return 0;
	StereoBundleFrame &bundleFrame = stereoBundleFrames[frame];
	if(bundleFrame.allocator == nil ||
	   FAILED(bundleFrame.allocator->Reset()))
		return 0;
	ID3D12DescriptorHeap *heaps[2] = {
		getShaderResourceHeap(), getSamplerHeap()
	};
	if(heaps[0] == nil || heaps[1] == nil)
		return 0;
	for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++){
		const StereoWorldRange &range = stereoWorldRanges[segment];
		ID3D12GraphicsCommandList *list = bundleFrame.segments[segment];
		if(list == nil || !range.complete ||
		   range.first + range.count > stereoWorldDrawCount ||
		   FAILED(list->Reset(bundleFrame.allocator, nil)))
			return 0;
		list->SetDescriptorHeaps(2, heaps);
		list->SetGraphicsRootSignature(rootSignature);
		for(uint32 i = 0; i < range.count; i++){
			const StereoWorldDraw &draw = stereoWorldDraws[range.first + i];
			float constants[54];
			memcpy(constants, draw.constants, sizeof(constants));
			// drawFlags.y packs skin in bit 0 and the mono eye in bit 1.
			constants[49] += 2.0f;
			memcpy(constants + 16, stereoBundleTaskView,
			       sizeof(stereoBundleTaskView));
			memcpy(constants + 32, stereoBundleTaskProjection,
			       sizeof(stereoBundleTaskProjection));
			list->IASetPrimitiveTopology(draw.topology);
			list->IASetVertexBuffers(0, 1, &draw.vertexView);
			list->IASetIndexBuffer(&draw.indexView);
			list->SetPipelineState(draw.pipeline);
			list->SetGraphicsRoot32BitConstants(0, 54, constants, 0);
			list->SetGraphicsRootDescriptorTable(1, draw.texture);
			list->SetGraphicsRootConstantBufferView(2, draw.boneAddress);
			list->SetGraphicsRootConstantBufferView(3, draw.lightingAddress);
			list->SetGraphicsRootDescriptorTable(4, draw.sampler);
			list->SetGraphicsRootDescriptorTable(5, draw.reflectionTexture);
			list->SetGraphicsRootConstantBufferView(6, draw.reflectionConstants);
			list->SetGraphicsRootDescriptorTable(7, draw.reflectionSampler);
			list->DrawIndexedInstanced(draw.numIndices, 1,
			                           draw.startIndex, 0, 0);
		}
		if(FAILED(list->Close()))
			return 0;
	}
	return 1;
}

static DWORD WINAPI
stereoBundleWorkerProc(void*)
{
	HANDLE events[2] = { stereoBundleStopEvent, stereoBundleWorkEvent };
	for(;;){
		DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if(result == WAIT_OBJECT_0)
			break;
		if(result != WAIT_OBJECT_0 + 1)
			continue;
		const uint32 frame = stereoBundleTaskFrame;
		const uint32 generation = stereoBundleTaskGeneration;
		const bool32 profiling = isDetailedProfilingEnabled();
		const double start = profiling ? profileNowMs() : 0.0;
		const bool32 ok = recordStereoWorldBundles(frame);
		stereoBundleCompletedBuildMs = profiling ?
			(float32)(profileNowMs() - start) : 0.0f;
		stereoBundleCompletedFrame = frame;
		stereoBundleCompletedGeneration = generation;
		InterlockedExchange(&stereoBundleSucceeded, ok ? 1 : 0);
		SetEvent(stereoBundleDoneEvent);
	}
	return 0;
}

static bool32
waitForStereoWorldBundleBuild(void)
{
	if(InterlockedCompareExchange(&stereoBundlePending, 0, 0) == 0)
		return 0;
	if(stereoBundleDoneEvent == nil){
		InterlockedExchange(&stereoBundlePending, 0);
		return 0;
	}
	const bool32 profiling = isDetailedProfilingEnabled();
	const double start = profiling ? profileNowMs() : 0.0;
	const DWORD result = WaitForSingleObject(stereoBundleDoneEvent, INFINITE);
	if(profiling)
		worldRenderProfile.stereoBundleWaitMs +=
			(float32)(profileNowMs() - start);
	if(profiling)
		worldRenderProfile.stereoBundleBuildMs +=
			stereoBundleCompletedBuildMs;
	stereoBundleCompletedBuildMs = 0.0f;
	InterlockedExchange(&stereoBundlePending, 0);
	return result == WAIT_OBJECT_0 &&
	       InterlockedCompareExchange(&stereoBundleSucceeded, 0, 0) != 0;
}

static void
shutdownStereoBundleResources(void)
{
	if(stereoBundleWorkerThread){
		if(stereoBundleStopEvent)
			SetEvent(stereoBundleStopEvent);
		WaitForSingleObject(stereoBundleWorkerThread, INFINITE);
		CloseHandle(stereoBundleWorkerThread);
		stereoBundleWorkerThread = nil;
	}
	if(stereoBundleWorkEvent){ CloseHandle(stereoBundleWorkEvent); stereoBundleWorkEvent = nil; }
	if(stereoBundleDoneEvent){ CloseHandle(stereoBundleDoneEvent); stereoBundleDoneEvent = nil; }
	if(stereoBundleStopEvent){ CloseHandle(stereoBundleStopEvent); stereoBundleStopEvent = nil; }
	for(uint32 frame = 0; frame < BONE_FRAME_COUNT; frame++){
		for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++)
			releaseCom(stereoBundleFrames[frame].segments[segment]);
		releaseCom(stereoBundleFrames[frame].allocator);
	}
	InterlockedExchange(&stereoBundlePending, 0);
	InterlockedExchange(&stereoBundleSucceeded, 0);
	stereoBundleResourcesReady = 0;
}

static bool32
createStereoBundleResources(void)
{
	if(stereoBundleResourcesReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;
	for(uint32 frame = 0; frame < BONE_FRAME_COUNT; frame++){
		if(FAILED(device->CreateCommandAllocator(
		       D3D12_COMMAND_LIST_TYPE_BUNDLE,
		       IID_PPV_ARGS(&stereoBundleFrames[frame].allocator)))){
			shutdownStereoBundleResources();
			return 0;
		}
		for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++){
			ID3D12GraphicsCommandList *&list =
				stereoBundleFrames[frame].segments[segment];
			if(FAILED(device->CreateCommandList(
			       0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
			       stereoBundleFrames[frame].allocator, nil,
			       IID_PPV_ARGS(&list))) || FAILED(list->Close())){
				shutdownStereoBundleResources();
				return 0;
			}
		}
	}
	stereoBundleWorkEvent = CreateEventA(nil, FALSE, FALSE, nil);
	stereoBundleDoneEvent = CreateEventA(nil, TRUE, TRUE, nil);
	stereoBundleStopEvent = CreateEventA(nil, TRUE, FALSE, nil);
	if(stereoBundleWorkEvent == nil || stereoBundleDoneEvent == nil ||
	   stereoBundleStopEvent == nil){
		shutdownStereoBundleResources();
		return 0;
	}
	stereoBundleWorkerThread = CreateThread(
		nil, 0, stereoBundleWorkerProc, nil, 0, nil);
	if(stereoBundleWorkerThread == nil){
		shutdownStereoBundleResources();
		return 0;
	}
	stereoBundleResourcesReady = 1;
	return 1;
}

static D3D12_HEAP_PROPERTIES
uploadHeapProperties(void)
{
	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.Type = D3D12_HEAP_TYPE_UPLOAD;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

static D3D12_RESOURCE_DESC
bufferDesc(uint64 size)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return desc;
}

static bool32
createUploadBuffer(const void *data, uint64 size, ID3D12Resource **resource)
{
	ID3D12Device *device = getDevice();
	if(device == nil || data == nil || size == 0 || resource == nil)
		return 0;
	const bool32 profiling = isDetailedProfilingEnabled();
	const double startedMs = profiling ? profileNowMs() : 0.0;
	D3D12_HEAP_PROPERTIES props = uploadHeapProperties();
	D3D12_RESOURCE_DESC desc = bufferDesc(size);
	if(FAILED(device->CreateCommittedResource(
	       &props, D3D12_HEAP_FLAG_NONE, &desc,
	       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
	       IID_PPV_ARGS(resource))))
		return 0;
	void *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED((*resource)->Map(0, &readRange, &mapped))){
		releaseCom(*resource);
		return 0;
	}
	memcpy(mapped, data, (size_t)size);
	(*resource)->Unmap(0, nil);
	if(profiling){
		worldRenderProfile.bufferUploadMs += (float32)(profileNowMs()-startedMs);
		worldRenderProfile.bufferBytes += size;
	}
	return 1;
}

static bool32
compileShader(const char *source, const char *entry, const char *target,
	          ID3DBlob **shader, const D3D_SHADER_MACRO *defines = nil)
{
	ID3DBlob *errors = nil;
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
	HRESULT hr = D3DCompile(source, strlen(source), "librw_d3d12_default",
	                        defines, nil, entry, target, flags, 0, shader, &errors);
	if(FAILED(hr) && errors)
		fprintf(stderr, "librw D3D12 shader: %s\n",
		        (const char*)errors->GetBufferPointer());
	releaseCom(errors);
	return SUCCEEDED(hr);
}

static void
getWorldPipelineBlend(uint32 mode, D3D12_BLEND *source,
                      D3D12_BLEND *destination,
                      D3D12_BLEND *sourceAlpha,
                      D3D12_BLEND *destinationAlpha)
{
	switch(mode){
	case WORLD_BLEND_ADD_ONE:
		*source = D3D12_BLEND_ONE;
		*destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_ONE;
		*destinationAlpha = D3D12_BLEND_ONE;
		break;
	case WORLD_BLEND_ADD_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA;
		*destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA;
		*destinationAlpha = D3D12_BLEND_ONE;
		break;
	case WORLD_BLEND_SHADOW:
		*source = D3D12_BLEND_ZERO;
		*destination = D3D12_BLEND_INV_SRC_COLOR;
		*sourceAlpha = D3D12_BLEND_ZERO;
		*destinationAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		break;
	case WORLD_BLEND_INVERSE_DEST:
		*source = D3D12_BLEND_INV_DEST_COLOR;
		*destination = D3D12_BLEND_ZERO;
		*sourceAlpha = D3D12_BLEND_INV_DEST_ALPHA;
		*destinationAlpha = D3D12_BLEND_ZERO;
		break;
	case WORLD_BLEND_REPLACE:
		*source = D3D12_BLEND_ONE;
		*destination = D3D12_BLEND_ZERO;
		*sourceAlpha = D3D12_BLEND_ONE;
		*destinationAlpha = D3D12_BLEND_ZERO;
		break;
	case WORLD_BLEND_KEEP_DESTINATION:
		*source = D3D12_BLEND_ZERO;
		*destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_ZERO;
		*destinationAlpha = D3D12_BLEND_ONE;
		break;
	case WORLD_BLEND_MODULATE_DESTINATION:
		*source = D3D12_BLEND_ZERO;
		*destination = D3D12_BLEND_SRC_COLOR;
		*sourceAlpha = D3D12_BLEND_ZERO;
		*destinationAlpha = D3D12_BLEND_SRC_ALPHA;
		break;
	case WORLD_BLEND_ALPHA_INVERSE_DEST_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA;
		*destination = D3D12_BLEND_INV_DEST_ALPHA;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA;
		*destinationAlpha = D3D12_BLEND_INV_DEST_ALPHA;
		break;
	case WORLD_BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA:
		*source = D3D12_BLEND_DEST_ALPHA;
		*destination = D3D12_BLEND_INV_DEST_ALPHA;
		*sourceAlpha = D3D12_BLEND_DEST_ALPHA;
		*destinationAlpha = D3D12_BLEND_INV_DEST_ALPHA;
		break;
	case WORLD_BLEND_ALPHA:
	default:
		*source = D3D12_BLEND_SRC_ALPHA;
		*destination = D3D12_BLEND_INV_SRC_ALPHA;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA;
		*destinationAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		break;
	}
}

static uint32
getWorldBlendMode(void)
{
	void *source = getRenderState(SRCBLEND);
	void *destination = getRenderState(DESTBLEND);
	if(source == (void*)BLENDONE && destination == (void*)BLENDONE)
		return WORLD_BLEND_ADD_ONE;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDONE)
		return WORLD_BLEND_ADD_ALPHA;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDINVSRCCOLOR)
		return WORLD_BLEND_SHADOW;
	if(source == (void*)BLENDINVDESTCOLOR && destination == (void*)BLENDZERO)
		return WORLD_BLEND_INVERSE_DEST;
	if(source == (void*)BLENDONE && destination == (void*)BLENDZERO)
		return WORLD_BLEND_REPLACE;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDONE)
		return WORLD_BLEND_KEEP_DESTINATION;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDSRCCOLOR)
		return WORLD_BLEND_MODULATE_DESTINATION;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDINVDESTALPHA)
		return WORLD_BLEND_ALPHA_INVERSE_DEST_ALPHA;
	if(source == (void*)BLENDDESTALPHA && destination == (void*)BLENDINVDESTALPHA)
		return WORLD_BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA;
	return WORLD_BLEND_ALPHA;
}

static bool32
createPipelineResources(void)
{
	if(pipelineReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_DESCRIPTOR_RANGE ranges[4];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[2] = ranges[0];
	ranges[2].BaseShaderRegister = 1;
	ranges[3] = ranges[1];
	ranges[3].BaseShaderRegister = 1;

	D3D12_ROOT_PARAMETER params[8];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 54;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor.ShaderRegister = 1;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[3].Descriptor.ShaderRegister = 2;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable.NumDescriptorRanges = 1;
	params[4].DescriptorTable.pDescriptorRanges = &ranges[1];
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[5].DescriptorTable.NumDescriptorRanges = 1;
	params[5].DescriptorTable.pDescriptorRanges = &ranges[2];
	params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[6].Descriptor.ShaderRegister = 4;
	params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[7].DescriptorTable.NumDescriptorRanges = 1;
	params[7].DescriptorTable.pDescriptorRanges = &ranges[3];
	params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 8;
	signature.pParameters = params;
	signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	// The stereo shader receives the ordinary per-draw block through a CBV
	// instead of 54 root constants. That leaves ample root-signature space for
	// the right-eye camera while preserving all material/bone/light bindings.
	D3D12_ROOT_PARAMETER stereoParams[9];
	memset(stereoParams, 0, sizeof(stereoParams));
	stereoParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	stereoParams[0].Descriptor.ShaderRegister = 0;
	stereoParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	for(uint32 i = 1; i < 5; i++)
		stereoParams[i] = params[i];
	stereoParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	stereoParams[5].Descriptor.ShaderRegister = 3;
	stereoParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	stereoParams[6] = params[5];
	stereoParams[7] = params[6];
	stereoParams[8] = params[7];
	signature.NumParameters = 9;
	signature.pParameters = stereoParams;
	serialized = nil;
	errors = nil;
	hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 stereo root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&stereoRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer DrawConstants : register(b0) {"
		" row_major float4x4 world; row_major float4x4 view;"
		" row_major float4x4 projection; float4 drawFlags;"
		" float fogEnd; float fogRange; };\n"
		"#ifdef STEREO_VERTEX\n"
		"cbuffer RightCameraConstants : register(b3) {"
		" row_major float4x4 rightView; row_major float4x4 rightProjection; };\n"
		"#endif\n"
		"cbuffer SkinConstants : register(b1) { row_major float4x4 bones[128]; };"
		"cbuffer LightingConstants : register(b2) { float4 ambientLight;"
		" float4 surfaceProps; float4 lightColorRadius[8];"
		" float4 lightPositionCos[8]; float4 lightDirectionClamp[8];"
		" float4 materialColor; float4 reflectionMaterial;"
		" row_major float4x4 previousWorld; float4 motionParams; };"
		"cbuffer ReflectionConstants : register(b4) {"
		" row_major float4x4 previousView;"
		" row_major float4x4 previousProjection;"
		" row_major float4x4 temporalPreviousView[2];"
		" row_major float4x4 temporalPreviousProjection[2];"
		" float4 temporalMotionValid;"
		" float4 temporalJitterClip;"
		" float4 reflectionParams; float4 reflectionTextureInfo;"
		" float4 rainParams; float4 puddleParams; float4 waterParams;"
		" float4 waterExtraParams; float4 waterSunDirection;"
		" float4 waterSunColour; float4 waterSkyColour; float4 carSsrParams;"
		" float4 dynamicLightPositionRadius[8];"
		" float4 dynamicLightColourSpot[8];"
		" float4 dynamicLightDirection[8];"
		" float4 dynamicLightParams; };"
		"Texture2D diffuseTexture : register(t0);"
		"Texture2D previousScene : register(t1);"
		"SamplerState diffuseSampler : register(s0);"
		"SamplerState reflectionSampler : register(s1);"
		"struct VSIn { float3 position : POSITION; float3 normal : NORMAL;"
		" float4 color : COLOR0; float2 uv : TEXCOORD0;"
		" float4 weights : BLENDWEIGHT0; uint4 indices : BLENDINDICES0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; float fogFactor : TEXCOORD1;"
		" float3 worldPosition : TEXCOORD2; float3 worldNormal : TEXCOORD3;"
		" float3 worldViewRay : TEXCOORD4;\n"
		"#ifdef OBJECT_MOTION\n"
		" float4 currentMotionClip : TEXCOORD5;"
		" float4 previousMotionClip : TEXCOORD6;"
		" nointerpolation float motionValid : TEXCOORD7;\n"
		"#endif\n"
		" nointerpolation float stereoEye : TEXCOORD8;\n"
		"#ifdef STEREO_VERTEX\n"
		" float2 clipDistance : SV_ClipDistance0;\n"
		"#endif\n"
		"};"
		"VSOut BuildVS(VSIn input, uint stereoEye) { VSOut output;"
		" uint drawMode = (uint)(drawFlags.y + 0.5);"
		" float4 localPosition = float4(input.position, 1.0);"
		"\n#ifdef OBJECT_MOTION\n"
		" float4 previousLocalPosition = localPosition;\n"
		"#endif\n"
		" float3 localNormal = input.normal;"
		" if((drawMode & 1u) != 0u) { localPosition = float4(0.0, 0.0, 0.0, 0.0);"
		"\n#ifdef OBJECT_MOTION\n"
		" previousLocalPosition = float4(0.0, 0.0, 0.0, 0.0);\n"
		"#endif\n"
		" localNormal = float3(0.0, 0.0, 0.0);"
		" [unroll] for(int i = 0; i < 4; i++)"
		" { localPosition += mul(float4(input.position, 1.0), bones[input.indices[i]]) * input.weights[i];"
		"\n#ifdef OBJECT_MOTION\n"
		" previousLocalPosition += mul(float4(input.position, 1.0),"
		"  bones[64+input.indices[i]]) * input.weights[i];\n"
		"#endif\n"
		" localNormal += mul(float4(input.normal, 0.0), bones[input.indices[i]]).xyz * input.weights[i]; } }"
		" float4 worldPosition = mul(localPosition, world);"
		" float4 p = mul(worldPosition, view); output.position = mul(p, projection);"
		"\n#ifdef OBJECT_MOTION\n"
		" float4 previousWorldPosition = mul(previousLocalPosition, previousWorld);"
		" output.currentMotionClip = output.position;"
		" output.previousMotionClip = mul(mul(previousWorldPosition,"
		"  temporalPreviousView[stereoEye]),temporalPreviousProjection[stereoEye]);\n"
		"#endif\n"
		" float3 worldViewRay = mul(p.xyz, transpose((float3x3)view));\n"
		"#ifdef STEREO_VERTEX\n"
		" if(stereoEye != 0) { p = mul(worldPosition, rightView);"
		" output.position = mul(p, rightProjection);"
		"\n#ifdef OBJECT_MOTION\n"
		" output.currentMotionClip = output.position;\n"
		"#endif\n"
		" worldViewRay = mul(p.xyz, transpose((float3x3)rightView)); }\n"
		"#endif\n"
		// projection[8/9] adds jitter * clip.w. Remove it only from the
		// temporal position; rasterization remains jittered.
		"#ifdef OBJECT_MOTION\n"
		" float2 currentJitter = stereoEye == 0 ?"
		"  temporalJitterClip.xy : temporalJitterClip.zw;"
		" output.currentMotionClip.xy -= currentJitter*output.currentMotionClip.w;\n"
		"#endif\n"
		" output.fogFactor = fogRange < 0.0 ?"
		" saturate((p.z - fogEnd) * fogRange) : 1.0;"
		" float3 normal = normalize(mul(float4(localNormal, 0.0), world).xyz);"
		" float3 litColor = input.color.rgb + ambientLight.rgb * surfaceProps.x;"
		" [unroll] for(int lightIndex = 0; lightIndex < 8; lightIndex++) {"
		"  float4 light = lightColorRadius[lightIndex]; if(light.w == 0.0) break;"
		"  float diffuse = 0.0; float attenuation = 1.0;"
		"  if(light.w < 0.0) {"
		"   diffuse = max(0.0, dot(normal, -lightDirectionClamp[lightIndex].xyz));"
		"  } else {"
		"   float3 toVertex = worldPosition.xyz - lightPositionCos[lightIndex].xyz;"
		"   float distanceToLight = length(toVertex);"
		"   float3 ray = distanceToLight > 0.0001 ? toVertex / distanceToLight : float3(0.0, 0.0, 0.0);"
		"   attenuation = max(0.0, 1.0 - distanceToLight / light.w);"
		"   diffuse = max(0.0, dot(normal, -ray));"
		"   float falloffClamp = lightDirectionClamp[lightIndex].w;"
		"   if(falloffClamp >= 0.0) {"
		"    float pointCos = dot(ray, lightDirectionClamp[lightIndex].xyz);"
		"    float coneCos = -lightPositionCos[lightIndex].w;"
		"    float falloff = (pointCos - coneCos) / max(0.0001, 1.0 - coneCos);"
		"    if(falloff < 0.0) diffuse = 0.0;"
		"    diffuse *= max(falloff, falloffClamp);"
		"   }"
		"  }"
		"  litColor += diffuse * light.rgb * attenuation * surfaceProps.z;"
		" }"
		" output.color = float4(saturate(litColor), input.color.a) * materialColor;"
		" output.uv = input.uv; output.worldPosition = worldPosition.xyz;"
		" output.worldNormal = normal; output.worldViewRay = worldViewRay;"
		"\n#ifdef OBJECT_MOTION\n"
		" output.motionValid = motionParams.x*temporalMotionValid[stereoEye];\n"
		"#endif\n"
		" output.stereoEye = (float)stereoEye;"
		" return output; }"
		"VSOut VSMain(VSIn input) { return BuildVS(input,"
		" ((uint)(drawFlags.y + 0.5) >> 1) & 1u); }\n"
		"#ifdef STEREO_VERTEX\n"
		"VSOut VSMainStereo(VSIn input, uint instanceId : SV_InstanceID) {"
		" VSOut output = BuildVS(input, instanceId);"
		" output.clipDistance = float2(output.position.x + output.position.w,"
		"  output.position.w - output.position.x);"
		" output.position.x = output.position.x * 0.5 +"
		"  (instanceId == 0 ? -0.5 : 0.5) * output.position.w;"
		" return output; }\n"
		"#endif\n"
		"float ReflectionFade(float4 clip) {"
		" if(clip.w <= 0.1) return 0.0;"
		" float2 ndc = clip.xy/clip.w;"
		" return saturate((1.0-max(abs(ndc.x),abs(ndc.y)))*3.0) *"
		" saturate(clip.w*0.5); }"
		"float2 ClampPreviousEyeUV(float2 uv, uint eye) {"
		" float2 halfTexel = reflectionTextureInfo.zw*0.5;"
		" float left=(float)eye*0.5;"
		" return clamp(uv,float2(left+halfTexel.x,halfTexel.y),"
		"  float2(left+0.5-halfTexel.x,1.0-halfTexel.y)); }"
		"float2 PreviousEyeUV(float4 clip, float2 distortion, uint eye) {"
		" float2 uv = clip.xy/clip.w*float2(0.5,-0.5)+0.5;"
		" uv.x = uv.x*0.5+(float)eye*0.5; uv += distortion;"
		" return ClampPreviousEyeUV(uv,eye); }"
		"float3 SamplePreviousWater(float4 clip, float2 slope, uint eye) {"
		" float2 uv = PreviousEyeUV(clip,"
		" float2(slope.x*0.0075,slope.y*0.015),eye);"
		" float2 px = reflectionTextureInfo.zw*0.6;"
		" float3 seen = previousScene.SampleLevel(reflectionSampler,"
		"  ClampPreviousEyeUV(uv+px,eye),0.0).rgb;"
		" seen += previousScene.SampleLevel(reflectionSampler,"
		"  ClampPreviousEyeUV(uv-px,eye),0.0).rgb;"
		" seen += previousScene.SampleLevel(reflectionSampler,"
		" ClampPreviousEyeUV(uv+float2(px.x,-px.y),eye),0.0).rgb;"
		" seen += previousScene.SampleLevel(reflectionSampler,"
		" ClampPreviousEyeUV(uv-float2(px.x,-px.y),eye),0.0).rgb;"
		" return seen*0.25; }"
		"float RainHash(float2 p) {"
		" p=frac(p*float2(123.34,456.21));p+=dot(p,p+45.32);"
		" return frac(p.x*p.y); }"
		"float RainNoise(float2 p) {"
		" float2 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);"
		" return lerp(lerp(RainHash(i),RainHash(i+float2(1,0)),f.x),"
		"  lerp(RainHash(i+float2(0,1)),RainHash(i+1.0),f.x),f.y); }"
		"float RainHash3(float3 p) {"
		" p=frac(p*float3(0.1031,0.1030,0.0973));"
		" p+=dot(p,p.yxz+33.33);return frac((p.x+p.y)*p.z); }"
		"float RainNoise3(float3 p) {"
		" float3 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);"
		" float n000=RainHash3(i),n100=RainHash3(i+float3(1,0,0));"
		" float n010=RainHash3(i+float3(0,1,0)),n110=RainHash3(i+float3(1,1,0));"
		" float n001=RainHash3(i+float3(0,0,1)),n101=RainHash3(i+float3(1,0,1));"
		" float n011=RainHash3(i+float3(0,1,1)),n111=RainHash3(i+1.0);"
		" return lerp(lerp(lerp(n000,n100,f.x),lerp(n010,n110,f.x),f.y),"
		"  lerp(lerp(n001,n101,f.x),lerp(n011,n111,f.x),f.y),f.z); }"
		"struct PSOut { float4 color : SV_TARGET0;\n"
		"#ifdef OBJECT_MOTION\n"
		" float2 motion : SV_TARGET1;\n"
		"#endif\n"
		"};"
		"PSOut PSMain(VSOut input) { PSOut output;"
		" float4 color = input.color;"
		" float4 textureColor = float4(1.0,1.0,1.0,1.0);"
		" if(drawFlags.x > 0.5) { textureColor = diffuseTexture.Sample(diffuseSampler, input.uv);"
		"  color *= textureColor; }"
		" if(drawFlags.w > 1.5) clip(drawFlags.z - color.a - 0.000001);"
		" else if(drawFlags.w > 0.5) clip(color.a - drawFlags.z);"
		" if(dynamicLightParams.x > 0.5) {"
		"  float3 eyeRay = input.worldViewRay;"
		"  float eyeDistance = length(eyeRay);"
		"  float3 eyeDirection = eyeDistance > 0.0001 ? eyeRay/eyeDistance : float3(0.0,0.0,0.0);"
		"  float3 eyePosition = input.worldPosition-eyeRay;"
		"  float3 normal = normalize(input.worldNormal);"
		"  if(reflectionMaterial.z < 0.5) {"
		"   normal = cross(ddx(input.worldPosition),ddy(input.worldPosition));"
		"   float normalLength = length(normal);"
		"   normal = normalLength > 0.0001 ? normal/normalLength : float3(0.0,0.0,1.0);"
		"   if(dot(normal,-eyeRay) < 0.0) normal = -normal; }"
		"  float3 dynamicSurface = float3(0.0,0.0,0.0);"
		"  float3 dynamicGlow = float3(0.0,0.0,0.0);"
		"  [loop] for(int dynamicIndex = 0; dynamicIndex < (int)dynamicLightParams.x; dynamicIndex++) {"
		"   float4 lightPositionRadius = dynamicLightPositionRadius[dynamicIndex];"
		"   float radius = lightPositionRadius.w;"
		"   float3 toLight = lightPositionRadius.xyz-input.worldPosition;"
		"   float distanceSquared = dot(toLight,toLight);"
		"   if(distanceSquared < radius*radius && distanceSquared > 0.000001) {"
		"    float distanceToLight = sqrt(distanceSquared);"
		"    float3 lightDirection = toLight/distanceToLight;"
		"    float strength = saturate((1.0-distanceToLight/radius)*2.0);"
		"    if(dynamicLightColourSpot[dynamicIndex].w > 0.5)"
		"     strength *= max((dot(-lightDirection,dynamicLightDirection[dynamicIndex].xyz)-0.5)*2.0,0.0);"
		"    strength *= saturate((dot(normal,lightDirection)+0.5)/1.5);"
		"    dynamicSurface += dynamicLightColourSpot[dynamicIndex].rgb*strength; }"
		"   if(dynamicLightParams.y > 0.0 && eyeDistance > 0.0001) {"
		"    float3 eyeToLight = lightPositionRadius.xyz-eyePosition;"
		"    float alongRay = clamp(dot(eyeToLight,eyeDirection),0.0,eyeDistance);"
		"    float3 offAxis = eyeToLight-eyeDirection*alongRay;"
		"    float glowRadius = max(radius*0.4,0.0001);"
		"    float glow = 1.0-dot(offAxis,offAxis)/(glowRadius*glowRadius);"
		"    if(glow > 0.0) {"
		"     if(dynamicLightColourSpot[dynamicIndex].w > 0.5) {"
		"      float3 fromLight = normalize(eyePosition+eyeDirection*alongRay-lightPositionRadius.xyz);"
		"      glow *= max((dot(fromLight,dynamicLightDirection[dynamicIndex].xyz)-0.5)*2.0,0.0); }"
		"     dynamicGlow += dynamicLightColourSpot[dynamicIndex].rgb*(glow*glow*dynamicLightParams.y); }"
		"   }"
		"  }"
		"  color.rgb += dynamicSurface*surfaceProps.z*materialColor.rgb*textureColor.rgb;"
		"  color.rgb += dynamicGlow;"
		" }"
		" uint reflectionEye=(uint)(input.stereoEye+0.5);"
		" if(reflectionMaterial.y > 0.5 &&"
		"   ((reflectionParams.w > 0.5 && reflectionParams.y > 0.0) ||"
		"    any(waterExtraParams.xyz > 0.0))) {"
		"   float3 viewDirection = input.worldViewRay/"
		"    max(length(input.worldViewRay),0.001);"
		"   float t = reflectionParams.z*waterParams.y; float2 wp = input.worldPosition.xy;"
		"   float phaseA = dot(wp,float2(0.061,0.027))+t*0.85;"
		"   float phaseB = dot(wp,float2(-0.019,0.047))-t*0.63;"
		"   float2 slope = float2(sin(phaseA)+cos(phaseB)*0.7,"
		"    cos(phaseA*0.83)+sin(phaseB*1.13)*0.7)*0.65*waterParams.x;"
		"   float3 waveNormal = normalize(float3(slope.x*0.38,slope.y*0.38,1.0));"
		"   float facing = saturate(dot(waveNormal,-viewDirection));"
		"   float fresnel = pow(1.0-facing,5.0);"
		"   float3 mirrorRay = float3(viewDirection.x,viewDirection.y,"
		"    max(-viewDirection.z,0.02));"
		"   float march = clamp(8.0/max(mirrorRay.z,0.04),12.0,260.0);"
		"   float seenWeight = 0.0;"
		"   if(reflectionParams.w > 0.5 && reflectionParams.y > 0.0) {"
		"   float3 probePosition = input.worldPosition+mirrorRay*march;"
		"   float4 previousClip = mul(mul(float4(probePosition,1.0),"
		"    temporalPreviousView[reflectionEye]),"
		"    temporalPreviousProjection[reflectionEye]);"
		"   float fade = ReflectionFade(previousClip);"
		"   if(fade > 0.0) {"
		"    float3 seen = SamplePreviousWater(previousClip,slope*waterParams.z,reflectionEye);"
		"    seenWeight = fade*saturate((fresnel*1.5+0.12)*reflectionParams.y);"
		"    color.rgb = lerp(color.rgb,seen,seenWeight); } }"
		"   if(waterExtraParams.x > 0.0) {"
		"    uint waterFog = asuint(surfaceProps.w);"
		"    float3 fog = float3(waterFog&255u,(waterFog>>8)&255u,(waterFog>>16)&255u)/255.0;"
		"    float3 sheen = lerp(fog,waterSkyColour.rgb,saturate(mirrorRay.z*2.0));"
		"    color.rgb += sheen*((1.0-seenWeight)*(fresnel*0.30+0.03)*waterExtraParams.x); }"
		"   if(waterExtraParams.y > 0.0 || waterExtraParams.z > 0.0) {"
		"    float3 reflected = reflect(viewDirection,waveNormal); reflected.z = abs(reflected.z);"
		"    if(waterExtraParams.y > 0.0) {"
		"     float glint = pow(saturate(dot(reflected,waterSunDirection.xyz)),96.0);"
		"     color.rgb += waterSunColour.rgb*(glint*1.4*waterExtraParams.y); }"
		"    if(waterExtraParams.z > 0.0) {"
		"     [loop] for(int light=0; light<(int)dynamicLightParams.x; ++light) {"
		"      float3 toLight = dynamicLightPositionRadius[light].xyz-input.worldPosition;"
		"      float lightDist = length(toLight), reach = dynamicLightPositionRadius[light].w*2.0;"
		"      if(lightDist > 0.0001 && lightDist < reach) {"
		"       float spark = pow(saturate(dot(reflected,toLight/lightDist)),64.0);"
		"       color.rgb += dynamicLightColourSpot[light].rgb*"
		"        (spark*(1.0-lightDist/reach)*1.5*waterExtraParams.z); } } } }"
		"  } else if(reflectionParams.w > 0.5 && reflectionMaterial.x > 0.0 &&"
		"            reflectionParams.x > 0.0 && carSsrParams.x > 0.0) {"
		"   float eyeDistance = max(length(input.worldViewRay),0.001);"
		"   if(eyeDistance > 1.1) {"
		"    float3 viewDirection = input.worldViewRay/eyeDistance;"
		"    float nearFade = saturate(eyeDistance*1.25-1.375);"
		"    float distanceFade = carSsrParams.y > 0.0 ?"
		"     saturate((carSsrParams.y-eyeDistance)/max(carSsrParams.y*0.25,0.001)) : 1.0;"
		"    float3 reflected = reflect(viewDirection,normalize(input.worldNormal));"
		"   float march = 10.0+reflected.z*(reflected.z > 0.0 ? 18.0 : 7.0);"
		"   float3 probePosition = input.worldPosition+reflected*march;"
		"   float4 previousClip = mul(mul(float4(probePosition,1.0),"
		"    temporalPreviousView[reflectionEye]),"
		"    temporalPreviousProjection[reflectionEye]);"
		"   float fade = ReflectionFade(previousClip)*nearFade*distanceFade;"
		"   if(fade > 0.0) {"
		"    float2 uv = PreviousEyeUV(previousClip,float2(0.0,0.0),reflectionEye);"
		"    float3 seen = previousScene.SampleLevel(reflectionSampler,uv,0.0).rgb;"
		"    color.rgb += seen*(1.2*fade*reflectionParams.x*carSsrParams.x*"
		"     saturate(reflectionMaterial.x)); }"
		"   }"
		"  }"
		" if(rainParams.x > 0.001 && reflectionMaterial.y < 0.5) {"
		"  float3 wetNormal=normalize(input.worldNormal);"
		"  float3 geometricNormal=cross(ddx(input.worldPosition),ddy(input.worldPosition));"
		"  float geometricLength=length(geometricNormal);"
		"  geometricNormal=geometricLength>0.0001?geometricNormal/geometricLength:wetNormal;"
		"  float surfaceUp=abs(geometricNormal.z);"
		"  float wetHorizontal=smoothstep(0.45,0.86,surfaceUp);"
		"  float puddleHorizontal=smoothstep(0.88,0.975,surfaceUp);"
		"  float wet=rainParams.x*wetHorizontal;"
		"  color.rgb*=1.0-wet*0.22;"
		"  float3 wetView=input.worldViewRay/max(length(input.worldViewRay),0.001);"
		"  float grazing=pow(1.0-saturate(abs(dot(wetNormal,wetView))),4.0);"
		"  color.rgb+=float3(0.10,0.13,0.16)*wet*(0.10+grazing*0.32);"
		"  if(rainParams.z > 1.5 && reflectionMaterial.x <= 0.001 &&"
		"     reflectionParams.w > 0.5 &&"
		"     temporalMotionValid[reflectionEye] > 0.5) {"
		"   float2 wp=input.worldPosition.xy;float t=reflectionParams.z;"
		"   float height=input.worldPosition.z;"
		"   float basin=RainNoise3(float3(wp*0.035,height*4.0))*0.68+"
		"    RainNoise3(float3(wp*0.13,height*9.0+17.0))*0.32;"
		"   float fill=saturate(rainParams.x*puddleParams.x);"
		"   float threshold=lerp(0.82,0.48,fill);"
		"   float edgeWidth=lerp(0.06,0.24,saturate(puddleParams.y*0.5));"
		"   float puddle=smoothstep(threshold-edgeWidth*0.5,"
		"    threshold+edgeWidth*0.5,basin)*"
		"    rainParams.x*puddleHorizontal;"
		"   float dropPhase=frac(RainHash(floor(wp*0.7))*0.73+t*0.85);"
		"   float2 dropCell=frac(wp*0.7)-0.5;float dropRadius=length(dropCell);"
		"   float ring=sin((dropRadius-dropPhase*0.72)*42.0)*"
		"    (1.0-smoothstep(0.08,0.48,dropRadius))*"
		"    (1.0-smoothstep(0.18,0.72,dropPhase));"
		"   float2 ripple=normalize(dropCell+0.0001)*ring*0.0035*"
		"    rainParams.y*puddleParams.w;"
		"   float3 puddleNormal=normalize(float3(ripple,1.0));"
		"   float3 mirrorRay=reflect(wetView,puddleNormal);"
		"   mirrorRay.z=max(mirrorRay.z,0.04);"
		"   float3 probe=input.worldPosition+mirrorRay*"
		"    clamp(10.0/mirrorRay.z,10.0,180.0);"
		"   float4 puddleClip=mul(mul(float4(probe,1.0),"
		"    temporalPreviousView[reflectionEye]),"
		"    temporalPreviousProjection[reflectionEye]);"
		"   float fade=ReflectionFade(puddleClip);"
		"   if(fade > 0.0) {"
		"    float2 uv=PreviousEyeUV(puddleClip,ripple*0.35,reflectionEye);"
		"    float3 reflectedScene=previousScene.SampleLevel(reflectionSampler,uv,0).rgb;"
		"    float fresnel=0.18+0.62*pow(1.0-saturate(abs(dot(puddleNormal,wetView))),5.0);"
		"    color.rgb=lerp(color.rgb,reflectedScene,"
		"     saturate(puddle*fade*fresnel*puddleParams.z)); }"
		"  }"
		" }"
		" uint packedFogColor = asuint(surfaceProps.w);"
		" float3 fogColor = float3(packedFogColor & 255u,"
		" (packedFogColor >> 8) & 255u, (packedFogColor >> 16) & 255u) / 255.0;"
		" color.rgb = lerp(fogColor, color.rgb, input.fogFactor);"
		" output.color = color;\n"
		"#ifdef OBJECT_MOTION\n"
		" output.motion = float2(32768.0,32768.0);"
		" if(input.motionValid > 0.5 && input.currentMotionClip.w > 0.000001 &&"
		"  input.previousMotionClip.w > 0.000001)"
		"  output.motion = input.previousMotionClip.xy/input.previousMotionClip.w"
		"   -input.currentMotionClip.xy/input.currentMotionClip.w;\n"
		"#endif\n"
		" return output; }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *stereoVertexShader = nil;
	ID3DBlob *pixelShader = nil;
	ID3DBlob *motionVertexShader = nil;
	ID3DBlob *stereoMotionVertexShader = nil;
	ID3DBlob *motionPixelShader = nil;
	const D3D_SHADER_MACRO stereoDefines[] = {
		{ "STEREO_VERTEX", "1" }, { nil, nil }
	};
	const D3D_SHADER_MACRO motionDefines[] = {
		{ "OBJECT_MOTION", "1" }, { nil, nil }
	};
	const D3D_SHADER_MACRO stereoMotionDefines[] = {
		{ "STEREO_VERTEX", "1" }, { "OBJECT_MOTION", "1" }, { nil, nil }
	};
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "VSMainStereo", "vs_5_0", &stereoVertexShader,
	                  stereoDefines) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader) ||
	   !compileShader(shaderSource, "VSMain", "vs_5_0", &motionVertexShader,
	                  motionDefines) ||
	   !compileShader(shaderSource, "VSMainStereo", "vs_5_0", &stereoMotionVertexShader,
	                  stereoMotionDefines) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &motionPixelShader,
	                  motionDefines)){
		releaseCom(vertexShader);
		releaseCom(stereoVertexShader);
		releaseCom(pixelShader);
		releaseCom(motionVertexShader);
		releaseCom(stereoMotionVertexShader);
		releaseCom(motionPixelShader);
		return 0;
	}

	D3D12_INPUT_ELEMENT_DESC input[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
		  (UINT)offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
		  (UINT)offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
		  (UINT)offsetof(Vertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
		  (UINT)offsetof(Vertex, texCoords), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
		  (UINT)offsetof(Vertex, weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0,
		  (UINT)offsetof(Vertex, boneIndices), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso;
	memset(&pso, 0, sizeof(pso));
	pso.pRootSignature = rootSignature;
	pso.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	pso.VS.BytecodeLength = vertexShader->GetBufferSize();
	pso.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	pso.PS.BytecodeLength = pixelShader->GetBufferSize();
	pso.InputLayout.pInputElementDescs = input;
	pso.InputLayout.NumElements = (UINT)nelem(input);
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.FrontCounterClockwise = TRUE;
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pso.SampleMask = UINT_MAX;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pso.SampleDesc.Count = 1;
	for(uint32 blend = 0; blend < WORLD_BLEND_COUNT && SUCCEEDED(hr); blend++){
		pso.BlendState.RenderTarget[0].BlendEnable =
			blend != WORLD_BLEND_OPAQUE;
		getWorldPipelineBlend(blend,
			&pso.BlendState.RenderTarget[0].SrcBlend,
			&pso.BlendState.RenderTarget[0].DestBlend,
			&pso.BlendState.RenderTarget[0].SrcBlendAlpha,
			&pso.BlendState.RenderTarget[0].DestBlendAlpha);
		for(uint32 depth = 0; depth < WORLD_DEPTH_COUNT && SUCCEEDED(hr); depth++){
			const bool depthTest = depth == WORLD_DEPTH_TEST_ONLY ||
			                       depth == WORLD_DEPTH_TEST_WRITE;
			const bool depthWrite = depth == WORLD_DEPTH_WRITE_ONLY ||
			                        depth == WORLD_DEPTH_TEST_WRITE;
			pso.DepthStencilState.DepthEnable = depthTest || depthWrite;
			pso.DepthStencilState.DepthWriteMask =
				depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL :
				D3D12_DEPTH_WRITE_MASK_ZERO;
			pso.DepthStencilState.DepthFunc = depthTest ?
				D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
			for(uint32 cull = 0; cull < WORLD_CULL_COUNT && SUCCEEDED(hr); cull++){
				pso.RasterizerState.CullMode = cull == WORLD_CULL_BACK ?
					D3D12_CULL_MODE_BACK : cull == WORLD_CULL_FRONT ?
					D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
				pso.pRootSignature = rootSignature;
				pso.VS.pShaderBytecode = vertexShader->GetBufferPointer();
				pso.VS.BytecodeLength = vertexShader->GetBufferSize();
				pso.PS.pShaderBytecode = pixelShader->GetBufferPointer();
				pso.PS.BytecodeLength = pixelShader->GetBufferSize();
				hr = device->CreateGraphicsPipelineState(
					&pso, IID_PPV_ARGS(&worldPipelines[blend][depth][cull]));
				if(SUCCEEDED(hr)){
					pso.pRootSignature = stereoRootSignature;
					pso.VS.pShaderBytecode = stereoVertexShader->GetBufferPointer();
					pso.VS.BytecodeLength = stereoVertexShader->GetBufferSize();
					hr = device->CreateGraphicsPipelineState(
						&pso, IID_PPV_ARGS(&stereoWorldPipelines[blend][depth][cull]));
				}
				if(SUCCEEDED(hr)){
					pso.NumRenderTargets = 2;
					pso.BlendState.IndependentBlendEnable = TRUE;
					pso.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
					pso.BlendState.RenderTarget[1].BlendEnable = FALSE;
					pso.BlendState.RenderTarget[1].RenderTargetWriteMask =
						D3D12_COLOR_WRITE_ENABLE_ALL;
					pso.pRootSignature = rootSignature;
					pso.VS.pShaderBytecode = motionVertexShader->GetBufferPointer();
					pso.VS.BytecodeLength = motionVertexShader->GetBufferSize();
					pso.PS.pShaderBytecode = motionPixelShader->GetBufferPointer();
					pso.PS.BytecodeLength = motionPixelShader->GetBufferSize();
					hr = device->CreateGraphicsPipelineState(
						&pso, IID_PPV_ARGS(&worldMotionPipelines[blend][depth][cull]));
				}
				if(SUCCEEDED(hr)){
					pso.pRootSignature = stereoRootSignature;
					pso.VS.pShaderBytecode = stereoMotionVertexShader->GetBufferPointer();
					pso.VS.BytecodeLength = stereoMotionVertexShader->GetBufferSize();
					hr = device->CreateGraphicsPipelineState(
						&pso, IID_PPV_ARGS(&stereoWorldMotionPipelines[blend][depth][cull]));
				}
				pso.NumRenderTargets = 1;
				pso.BlendState.IndependentBlendEnable = FALSE;
				pso.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
			}
		}
	}
	releaseCom(vertexShader);
	releaseCom(stereoVertexShader);
	releaseCom(pixelShader);
	releaseCom(motionVertexShader);
	releaseCom(stereoMotionVertexShader);
	releaseCom(motionPixelShader);
	if(FAILED(hr)){
		for(uint32 blend = 0; blend < WORLD_BLEND_COUNT; blend++)
			for(uint32 depth = 0; depth < WORLD_DEPTH_COUNT; depth++)
				for(uint32 cull = 0; cull < WORLD_CULL_COUNT; cull++)
				{
					releaseCom(worldPipelines[blend][depth][cull]);
					releaseCom(stereoWorldPipelines[blend][depth][cull]);
					releaseCom(worldMotionPipelines[blend][depth][cull]);
					releaseCom(stereoWorldMotionPipelines[blend][depth][cull]);
				}
		releaseCom(stereoRootSignature);
		releaseCom(rootSignature);
		return 0;
	}

	D3D12_HEAP_PROPERTIES boneProps = uploadHeapProperties();
	D3D12_RESOURCE_DESC boneBuffer = bufferDesc(BONE_UPLOAD_SIZE);
	for(uint32 i = 0; i < BONE_FRAME_COUNT; i++){
		if(FAILED(device->CreateCommittedResource(
		       &boneProps, D3D12_HEAP_FLAG_NONE, &boneBuffer,
		       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
		       IID_PPV_ARGS(&boneArenas[i].resource))))
			return 0;
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(boneArenas[i].resource->Map(
		       0, &readRange, (void**)&boneArenas[i].mapped)))
			return 0;
	}
	activeBoneArena = UINT32_MAX;

	Image *white = Image::create(1, 1, 32);
	white->allocate();
	white->pixels[0] = white->pixels[1] = white->pixels[2] =
		white->pixels[3] = 0xFF;
	whiteRaster = Raster::createFromImage(white, PLATFORM_D3D12);
	white->destroy();
	if(whiteRaster == nil)
		return 0;
	// Replay immutable packets directly; bundle-worker recording is disabled.
	pipelineReady = 1;
	return 1;
}

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry == nil || geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D12)
		return;
	D3D12InstanceDataHeader *header =
		(D3D12InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	for(uint32 i = 0; i < DYNAMIC_VERTEX_FRAME_COUNT; i++){
		deferRelease(header->vertexBuffers[i]);
		header->vertexBuffers[i] = nil;
	}
	deferRelease(header->indexBuffer);
	header->vertexBuffer = nil;
	header->indexBuffer = nil;
	rwFree(header->meshes);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static void
fillInstanceVertices(Geometry *geometry, Vertex *vertices)
{
	Skin *skin = Skin::get(geometry);
	for(uint32 i = 0; i < (uint32)geometry->numVertices; i++){
		vertices[i].position = geometry->morphTargets[0].vertices[i];
		if((geometry->flags & Geometry::NORMALS) &&
		   geometry->morphTargets[0].normals)
			vertices[i].normal = geometry->morphTargets[0].normals[i];
		else
			vertices[i].normal.set(0.0f, 0.0f, 1.0f);
		if((geometry->flags & Geometry::PRELIT) && geometry->colors)
			vertices[i].color = geometry->colors[i];
		else if(geometry->flags & Geometry::LIGHT)
			// Lit non-prelit geometry has no emissive vertex contribution. White
			// saturated the shader before ambient/directional lighting, so the
			// original scorched-vehicle lighting could never darken wrecks.
			vertices[i].color = makeRGBA(0, 0, 0, 255);
		else
			vertices[i].color = makeRGBA(255, 255, 255, 255);
		if(geometry->numTexCoordSets > 0 && geometry->texCoords[0])
			vertices[i].texCoords = geometry->texCoords[0][i];
		else{
			vertices[i].texCoords.u = 0.0f;
			vertices[i].texCoords.v = 0.0f;
		}
		vertices[i].weights[0] = 1.0f;
		vertices[i].weights[1] = vertices[i].weights[2] =
			vertices[i].weights[3] = 0.0f;
		memset(vertices[i].boneIndices, 0, sizeof(vertices[i].boneIndices));
		if(skin && skin->weights && skin->indices){
			memcpy(vertices[i].weights, skin->weights + i*4,
			       sizeof(vertices[i].weights));
			memcpy(vertices[i].boneIndices, skin->indices + i*4,
			       sizeof(vertices[i].boneIndices));
		}
	}
}

static void
refreshInstanceMeshes(Geometry *geometry, D3D12InstanceDataHeader *header,
	                  uint16 *indices)
{
	Mesh *mesh = geometry->meshHeader->getMeshes();
	uint32 indexOffset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		header->meshes[i].numIndices = mesh[i].numIndices;
		header->meshes[i].startIndex = indexOffset;
		header->meshes[i].material = mesh[i].material;
		header->meshes[i].vertexAlpha = 0;
		for(uint32 j = 0; j < mesh[i].numIndices; j++){
			const uint16 vertex = mesh[i].indices[j];
			if(indices)
				indices[indexOffset + j] = vertex;
			if(geometry->colors && geometry->colors[vertex].alpha != 0xFF)
				header->meshes[i].vertexAlpha = 1;
		}
		indexOffset += mesh[i].numIndices;
	}
}

static bool32
updateDynamicVertices(Geometry *geometry, D3D12InstanceDataHeader *header,
	                  const Vertex *vertices)
{
	const uint32 frame = getFrameIndex() % DYNAMIC_VERTEX_FRAME_COUNT;
	const uint64 size = header->numVertices*sizeof(Vertex);
	ID3D12Resource *&buffer = header->vertexBuffers[frame];
	if(buffer == nil){
		if(!createUploadBuffer(vertices, size, &buffer))
			return 0;
	}else{
		void *mapped = nil;
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(buffer->Map(0, &readRange, &mapped)))
			return 0;
		memcpy(mapped, vertices, (size_t)size);
		buffer->Unmap(0, nil);
	}
	header->vertexBuffer = buffer;
	header->vertexView.BufferLocation = buffer->GetGPUVirtualAddress();
	header->vertexView.SizeInBytes = (UINT)size;
	header->vertexView.StrideInBytes = sizeof(Vertex);
	return 1;
}

static bool32
instanceGeometry(Geometry *geometry)
{
	if(geometry == nil || geometry->meshHeader == nil ||
	   geometry->numVertices <= 0 || geometry->morphTargets == nil ||
	   geometry->morphTargets[0].vertices == nil)
		return 0;
	if(geometry->flags & Geometry::NATIVE)
		return 0;

	if(geometry->instData){
		D3D12InstanceDataHeader *existing =
			(D3D12InstanceDataHeader*)geometry->instData;
		if(existing->platform == PLATFORM_D3D12 &&
		   existing->serialNumber == geometry->meshHeader->serialNum){
			if(geometry->lockedSinceInst == 0)
				return 1;
			const bool32 profiling = isDetailedProfilingEnabled();
			const double instanceStartedMs = profiling ? profileNowMs() : 0.0;
			Vertex *vertices = rwNewT(Vertex, existing->numVertices,
			                              MEMDUR_EVENT | ID_GEOMETRY);
			fillInstanceVertices(geometry, vertices);
			const bool32 ok = updateDynamicVertices(geometry, existing, vertices);
			rwFree(vertices);
			if(!ok)
				return 0;
			refreshInstanceMeshes(geometry, existing, nil);
			geometry->lockedSinceInst = 0;
			if(profiling){
				worldRenderProfile.geometryInstanceMs +=
					(float32)(profileNowMs()-instanceStartedMs);
				worldRenderProfile.geometryInstances++;
			}
			return 1;
		}
		freeInstanceData(geometry);
	}
	const bool32 profiling = isDetailedProfilingEnabled();
	const double instanceStartedMs = profiling ? profileNowMs() : 0.0;

	D3D12InstanceDataHeader *header = rwNewT(
		D3D12InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header, 0, sizeof(*header));
	header->platform = PLATFORM_D3D12;
	header->serialNumber = geometry->meshHeader->serialNum;
	header->numMeshes = geometry->meshHeader->numMeshes;
	header->numVertices = geometry->numVertices;
	header->numIndices = geometry->meshHeader->totalIndices;
	header->topology = geometry->meshHeader->flags == MeshHeader::TRISTRIP ?
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP :
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	header->meshes = rwNewT(MeshDraw, header->numMeshes,
	                            MEMDUR_EVENT | ID_GEOMETRY);

	Vertex *vertices = rwNewT(Vertex, header->numVertices,
	                          MEMDUR_EVENT | ID_GEOMETRY);
	fillInstanceVertices(geometry, vertices);
	uint16 *indices = rwNewT(uint16, header->numIndices,
	                         MEMDUR_EVENT | ID_GEOMETRY);
	refreshInstanceMeshes(geometry, header, indices);

	const uint32 frame = getFrameIndex() % DYNAMIC_VERTEX_FRAME_COUNT;
	bool32 ok = createUploadBuffer(vertices,
		header->numVertices*sizeof(Vertex), &header->vertexBuffers[frame]) &&
		createUploadBuffer(indices,
		header->numIndices*sizeof(uint16), &header->indexBuffer);
	rwFree(vertices);
	rwFree(indices);
	if(!ok){
		releaseCom(header->vertexBuffers[frame]);
		releaseCom(header->indexBuffer);
		rwFree(header->meshes);
		rwFree(header);
		return 0;
	}
	header->vertexBuffer = header->vertexBuffers[frame];
	header->vertexView.BufferLocation =
		header->vertexBuffer->GetGPUVirtualAddress();
	header->vertexView.SizeInBytes = header->numVertices*sizeof(Vertex);
	header->vertexView.StrideInBytes = sizeof(Vertex);
	header->indexView.BufferLocation =
		header->indexBuffer->GetGPUVirtualAddress();
	header->indexView.SizeInBytes = header->numIndices*sizeof(uint16);
	header->indexView.Format = DXGI_FORMAT_R16_UINT;
	geometry->instData = header;
	geometry->lockedSinceInst = 0;
	if(profiling){
		worldRenderProfile.geometryInstanceMs +=
			(float32)(profileNowMs()-instanceStartedMs);
		worldRenderProfile.geometryInstances++;
	}
	return 1;
}

static void
fillMatrix(float *dst, const Matrix *matrix)
{
	dst[0] = matrix->right.x; dst[1] = matrix->right.y;
	dst[2] = matrix->right.z; dst[3] = 0.0f;
	dst[4] = matrix->up.x; dst[5] = matrix->up.y;
	dst[6] = matrix->up.z; dst[7] = 0.0f;
	dst[8] = matrix->at.x; dst[9] = matrix->at.y;
	dst[10] = matrix->at.z; dst[11] = 0.0f;
	dst[12] = matrix->pos.x; dst[13] = matrix->pos.y;
	dst[14] = matrix->pos.z; dst[15] = 1.0f;
}

static bool32
allocateArenaConstants(const void *data, uint32 dataSize,
	                  D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(activeBoneArena != frame){
		activeBoneArena = frame;
		boneArenas[frame].offset = 0;
		boneArenaGeneration[frame]++;
		if(boneArenaGeneration[frame] == 0)
			boneArenaGeneration[frame] = 1;
	}
	BoneArena &arena = boneArenas[frame];
	uint32 offset = (arena.offset + 255u) & ~255u;
	uint32 size = (dataSize + 255u) & ~255u;
	if(data == nil || address == nil || arena.mapped == nil ||
	   offset + size > BONE_UPLOAD_SIZE)
		return 0;
	memset(arena.mapped + offset, 0, size);
	memcpy(arena.mapped + offset, data, dataSize);
	*address = arena.resource->GetGPUVirtualAddress() + offset;
	arena.offset = offset + size;
	return 1;
}

static void
resetScreenSpaceReflectionConstantCache(void)
{
	memset(screenSpaceReflectionConstantAddress, 0,
	       sizeof(screenSpaceReflectionConstantAddress));
	memset(screenSpaceReflectionConstantGeneration, 0,
	       sizeof(screenSpaceReflectionConstantGeneration));
}

void
setDynamicPointLights(const DynamicPointLight *lights, uint32 count,
	float32 glowStrength)
{
	if(lights == nil)
		count = 0;
	if(count > MAX_DYNAMIC_POINT_LIGHTS)
		count = MAX_DYNAMIC_POINT_LIGHTS;
	if(count > 0)
		memcpy(dynamicPointLights, lights,
		       count*sizeof(DynamicPointLight));
	dynamicPointLightCount = count;
	dynamicPointLightGlowStrength = glowStrength > 0.0f ?
		glowStrength : 0.0f;
	resetScreenSpaceReflectionConstantCache();
}

void
setEffectsTime(float32 seconds)
{
	if(!(seconds >= 0.0f) || seconds > 3600000.0f)
		seconds = 0.0f;
	effectsTimeExternal = 1;
	if(screenSpaceReflectionTime == seconds)
		return;
	screenSpaceReflectionTime = seconds;
	resetScreenSpaceReflectionConstantCache();
}

static float32
boundEffectValue(float32 value, float32 maximum)
{
	// This also rejects NaN, which ordinary less/greater clamps leave intact.
	return !(value >= 0.0f) ? 0.0f : value > maximum ? maximum : value;
}

void
setWaterSurfaceSettings(float32 speedPercent, float32 distortionPercent,
                       float32 sheenPercent, float32 glintPercent,
                       float32 sparksPercent)
{
	speedPercent = boundEffectValue(speedPercent, 200.0f);
	distortionPercent = boundEffectValue(distortionPercent, 200.0f);
	const float32 extra[3] = { boundEffectValue(sheenPercent, 200.0f),
		boundEffectValue(glintPercent, 200.0f), boundEffectValue(sparksPercent, 200.0f) };
	if(waterSpeedPercent == speedPercent && waterDistortionPercent == distortionPercent &&
	   memcmp(waterExtraPercent, extra, sizeof(extra)) == 0)
		return;
	waterSpeedPercent = speedPercent;
	waterDistortionPercent = distortionPercent;
	memcpy(waterExtraPercent, extra, sizeof(extra));
	resetScreenSpaceReflectionConstantCache();
}

void
setWaterLighting(const float32 sunDirectionWorld[3],
                 const float32 sunColour[3], const float32 skyColour[3])
{
	if(waterExtraPercent[0] <= 0.0f && waterExtraPercent[1] <= 0.0f)
		return;
	float32 direction[3] = { 0.0f, 0.0f, 1.0f }, sun[3] = {}, sky[3] = {};
	if(sunDirectionWorld){
		float32 lengthSq = 0.0f;
		for(int i = 0; i < 3; i++){
			const float32 component = sunDirectionWorld[i];
			direction[i] = component >= -1000000.0f && component <= 1000000.0f ? component : 0.0f;
			lengthSq += direction[i]*direction[i];
		}
		if(lengthSq > 0.000001f){
			const float32 inverse = 1.0f/sqrtf(lengthSq);
			for(int i = 0; i < 3; i++) direction[i] *= inverse;
		}else{
			direction[0] = direction[1] = 0.0f; direction[2] = 1.0f;
		}
	}
	for(int i = 0; i < 3; i++){
		sun[i] = sunColour ? boundEffectValue(sunColour[i], 1.0f) : 0.0f;
		sky[i] = skyColour ? boundEffectValue(skyColour[i], 1.0f) : 0.0f;
	}
	if(memcmp(direction, waterSunDirection, sizeof(direction)) == 0 &&
	   memcmp(sun, waterSunColour, sizeof(sun)) == 0 &&
	   memcmp(sky, waterSkyColour, sizeof(sky)) == 0)
		return;
	memcpy(waterSunDirection, direction, sizeof(direction));
	memcpy(waterSunColour, sun, sizeof(sun));
	memcpy(waterSkyColour, sky, sizeof(sky));
	resetScreenSpaceReflectionConstantCache();
}

void
setCarReflectionSsrParams(float32 strengthPercent, float32 distanceMetres)
{
	strengthPercent = boundEffectValue(strengthPercent, 200.0f);
	distanceMetres = boundEffectValue(distanceMetres, 10000.0f);
	if(carSsrStrengthPercent == strengthPercent && carSsrDistanceMetres == distanceMetres)
		return;
	carSsrStrengthPercent = strengthPercent;
	carSsrDistanceMetres = distanceMetres;
	resetScreenSpaceReflectionConstantCache();
}

void
setScreenSpaceReflectionSettings(float32 carPercent, float32 waterPercent,
	float32 oceanWavePercent)
{
	if(carPercent < 0.0f) carPercent = 0.0f;
	if(waterPercent < 0.0f) waterPercent = 0.0f;
	if(carPercent > 200.0f) carPercent = 200.0f;
	if(waterPercent > 200.0f) waterPercent = 200.0f;
	if(oceanWavePercent < 0.0f) oceanWavePercent = 0.0f;
	if(oceanWavePercent > 200.0f) oceanWavePercent = 200.0f;
	if(screenSpaceReflectionCarPercent == carPercent &&
	   screenSpaceReflectionWaterPercent == waterPercent &&
	   screenSpaceReflectionOceanWavePercent == oceanWavePercent)
		return;
	screenSpaceReflectionCarPercent = carPercent;
	screenSpaceReflectionWaterPercent = waterPercent;
	screenSpaceReflectionOceanWavePercent = oceanWavePercent;
	resetScreenSpaceReflectionConstantCache();
	if(carPercent == 0.0f && waterPercent == 0.0f)
		screenSpaceReflectionHistoryValid = 0;
}

void
setRainSurfaceSettings(float32 wetness, float32 rainfall, int32 mode,
	float32 coveragePercent, float32 edgeSoftnessPercent,
	float32 ripplePercent, float32 reflectionPercent)
{
	if(wetness < 0.0f) wetness = 0.0f;
	if(wetness > 1.0f) wetness = 1.0f;
	if(rainfall < 0.0f) rainfall = 0.0f;
	if(rainfall > 2.0f) rainfall = 2.0f;
	if(mode < 0) mode = 0;
	if(mode > 3) mode = 3;
	if(coveragePercent < 25.0f) coveragePercent = 25.0f;
	if(coveragePercent > 200.0f) coveragePercent = 200.0f;
	if(edgeSoftnessPercent < 25.0f) edgeSoftnessPercent = 25.0f;
	if(edgeSoftnessPercent > 200.0f) edgeSoftnessPercent = 200.0f;
	if(ripplePercent < 0.0f) ripplePercent = 0.0f;
	if(ripplePercent > 200.0f) ripplePercent = 200.0f;
	if(reflectionPercent < 0.0f) reflectionPercent = 0.0f;
	if(reflectionPercent > 200.0f) reflectionPercent = 200.0f;
	if(rainSurfaceWetness == wetness && rainSurfaceRainfall == rainfall &&
	   rainSurfaceMode == mode && rainSurfaceCoveragePercent == coveragePercent &&
	   rainSurfaceEdgeSoftnessPercent == edgeSoftnessPercent &&
	   rainSurfaceRipplePercent == ripplePercent &&
	   rainSurfaceReflectionPercent == reflectionPercent)
		return;
	rainSurfaceWetness = wetness;
	rainSurfaceRainfall = rainfall;
	rainSurfaceMode = mode;
	rainSurfaceCoveragePercent = coveragePercent;
	rainSurfaceEdgeSoftnessPercent = edgeSoftnessPercent;
	rainSurfaceRipplePercent = ripplePercent;
	rainSurfaceReflectionPercent = reflectionPercent;
	resetScreenSpaceReflectionConstantCache();
	if(mode == 0)
		rainSurfaceWetness = rainSurfaceRainfall = 0.0f;
}

void
invalidateScreenSpaceReflectionHistory(void)
{
	screenSpaceReflectionHistoryValid = 0;
	resetScreenSpaceReflectionConstantCache();
}

void
setIm3DWater(bool32 water)
{
	im3DWaterDraw = water != 0;
}

bool32
isIm3DWater(void)
{
	return im3DWaterDraw;
}

static void
releaseScreenSpaceReflectionHistory(void)
{
	if(screenSpaceReflectionHistory){
		screenSpaceReflectionHistory->destroy();
		screenSpaceReflectionHistory = nil;
	}
	screenSpaceReflectionHistoryValid = 0;
	resetScreenSpaceReflectionConstantCache();
}

bool32
captureScreenSpaceReflectionFrame(Raster *source)
{
	// At zero strength, release history and skip allocation, copy, and shading.
	if((screenSpaceReflectionCarPercent <= 0.0f || carSsrStrengthPercent <= 0.0f) &&
	   screenSpaceReflectionWaterPercent <= 0.0f &&
	   (rainSurfaceMode < 2 || rainSurfaceWetness <= 0.001f ||
	    rainSurfaceReflectionPercent <= 0.0f)){
		if(rainSurfaceMode < 2 || rainSurfaceReflectionPercent <= 0.0f)
			releaseScreenSpaceReflectionHistory();
		else
			invalidateScreenSpaceReflectionHistory();
		return 1;
	}
	if(source)
		source = source->parent;
	if(source == nil || source->platform != PLATFORM_D3D12 ||
	   source->type != Raster::CAMERATEXTURE ||
	   source->width <= 0 || source->height <= 0){
		invalidateScreenSpaceReflectionHistory();
		return 0;
	}
	if(screenSpaceReflectionHistory == nil ||
	   screenSpaceReflectionHistory->width != source->width ||
	   screenSpaceReflectionHistory->height != source->height){
		releaseScreenSpaceReflectionHistory();
		screenSpaceReflectionHistory = Raster::create(
			source->width, source->height, 32,
			Raster::CAMERATEXTURE | Raster::C8888, PLATFORM_D3D12);
		if(screenSpaceReflectionHistory == nil)
			return 0;
	}
	ID3D12Resource *sourceResource = nil;
	ID3D12Resource *historyResource = nil;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil ||
	   !getRasterResource(source, &sourceResource) ||
	   !getRasterResource(screenSpaceReflectionHistory, &historyResource) ||
	   sourceResource == historyResource){
		invalidateScreenSpaceReflectionHistory();
		return 0;
	}
	const D3D12_RESOURCE_DESC sourceDesc = sourceResource->GetDesc();
	const D3D12_RESOURCE_DESC historyDesc = historyResource->GetDesc();
	if(sourceDesc.Width != historyDesc.Width ||
	   sourceDesc.Height != historyDesc.Height ||
	   sourceDesc.Format != historyDesc.Format ||
	   sourceDesc.SampleDesc.Count != historyDesc.SampleDesc.Count ||
	   sourceDesc.MipLevels != historyDesc.MipLevels){
		invalidateScreenSpaceReflectionHistory();
		return 0;
	}
	if(!transitionRaster(source, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
	   !transitionRaster(screenSpaceReflectionHistory,
	                     D3D12_RESOURCE_STATE_COPY_DEST)){
		transitionRaster(source, D3D12_RESOURCE_STATE_RENDER_TARGET);
		transitionRaster(screenSpaceReflectionHistory,
		                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		invalidateScreenSpaceReflectionHistory();
		return 0;
	}
	list->CopyResource(historyResource, sourceResource);
	const bool32 historyRestored =
		transitionRaster(screenSpaceReflectionHistory,
		                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	const bool32 sourceRestored =
		transitionRaster(source, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if(!historyRestored || !sourceRestored){
		invalidateScreenSpaceReflectionHistory();
		return 0;
	}
	// This is the post-jitter matrix used to draw the copied left half. The
	// non-jittered Streamline snapshot remains independent.
	if(screenSpaceReflectionCurrentLeftCameraValid){
		memcpy(screenSpaceReflectionPreviousCamera,
		       screenSpaceReflectionCurrentLeftCamera,
		       sizeof(screenSpaceReflectionPreviousCamera));
		screenSpaceReflectionHistoryValid = 1;
	}else
		screenSpaceReflectionHistoryValid = 0;
	if(!effectsTimeExternal)
		screenSpaceReflectionTime += 1.0f/60.0f;
	resetScreenSpaceReflectionConstantCache();
	return 1;
}

bool32
getScreenSpaceReflectionBindings(
	D3D12_GPU_DESCRIPTOR_HANDLE *view,
	D3D12_GPU_DESCRIPTOR_HANDLE *sampler,
	D3D12_GPU_VIRTUAL_ADDRESS *constantsAddress)
{
	if(view == nil || sampler == nil || constantsAddress == nil ||
	   whiteRaster == nil)
		return 0;
	bool32 valid = screenSpaceReflectionHistoryValid &&
		((screenSpaceReflectionCarPercent > 0.0f && carSsrStrengthPercent > 0.0f) ||
		 screenSpaceReflectionWaterPercent > 0.0f ||
		 (rainSurfaceMode >= 2 && rainSurfaceWetness > 0.001f &&
		  rainSurfaceReflectionPercent > 0.0f)) &&
		screenSpaceReflectionHistory != nil;
	if(valid && !getTextureView(screenSpaceReflectionHistory, view, nil)){
		valid = 0;
		screenSpaceReflectionHistoryValid = 0;
		resetScreenSpaceReflectionConstantCache();
	}
	if(!valid && !getTextureView(whiteRaster, view, nil))
		return 0;
	if(!getSamplerView(Texture::LINEAR, Texture::CLAMP, Texture::CLAMP,
	                   sampler))
		return 0;
	const uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(activeBoneArena != frame ||
	   screenSpaceReflectionConstantAddress[frame] == 0 ||
	   screenSpaceReflectionConstantGeneration[frame] !=
	       boneArenaGeneration[frame]){
		ScreenSpaceReflectionConstants reflection = {};
		memcpy(reflection.previousView,
		       screenSpaceReflectionPreviousCamera, 16*sizeof(float));
		memcpy(reflection.previousProjection,
		       screenSpaceReflectionPreviousCamera + 16, 16*sizeof(float));
		for(int32 eye = 0; eye < 2; eye++){
			reflection.temporalJitterClip[eye*2] =
				stereoTemporalJitterClip[eye][0];
			reflection.temporalJitterClip[eye*2+1] =
				stereoTemporalJitterClip[eye][1];
			if(previousStereoCameraValid[eye]){
				memcpy(reflection.temporalPreviousView[eye],
				       previousStereoCameraConstants[eye], 16*sizeof(float));
				memcpy(reflection.temporalPreviousProjection[eye],
				       previousStereoCameraConstants[eye]+16, 16*sizeof(float));
				reflection.temporalMotionValid[eye] = 1.0f;
			}
		}
		reflection.params[0] = screenSpaceReflectionCarPercent/100.0f;
		reflection.params[1] = screenSpaceReflectionWaterPercent/100.0f;
		reflection.params[2] = screenSpaceReflectionTime;
		reflection.params[3] = valid ? 1.0f : 0.0f;
		reflection.rainParams[0] = rainSurfaceWetness;
		reflection.rainParams[1] = rainSurfaceRainfall;
		reflection.rainParams[2] = (float32)rainSurfaceMode;
		reflection.puddleParams[0] = rainSurfaceCoveragePercent/100.0f;
		reflection.puddleParams[1] = rainSurfaceEdgeSoftnessPercent/100.0f;
		reflection.puddleParams[2] = rainSurfaceReflectionPercent/100.0f;
		reflection.puddleParams[3] = rainSurfaceRipplePercent/100.0f;
		reflection.waterParams[0] = screenSpaceReflectionOceanWavePercent/100.0f;
		reflection.waterParams[1] = waterSpeedPercent/100.0f;
		reflection.waterParams[2] = waterDistortionPercent/100.0f;
		for(int i = 0; i < 3; i++){
			reflection.waterExtraParams[i] = waterExtraPercent[i]/100.0f;
			reflection.waterSunDirection[i] = waterSunDirection[i];
			reflection.waterSunColour[i] = waterSunColour[i];
			reflection.waterSkyColour[i] = waterSkyColour[i];
		}
		reflection.carSsrParams[0] = carSsrStrengthPercent/100.0f;
		reflection.carSsrParams[1] = carSsrDistanceMetres;
		if(valid){
			reflection.textureInfo[0] =
				(float32)screenSpaceReflectionHistory->width;
			reflection.textureInfo[1] =
				(float32)screenSpaceReflectionHistory->height;
			reflection.textureInfo[2] =
				1.0f/reflection.textureInfo[0];
			reflection.textureInfo[3] =
				1.0f/reflection.textureInfo[1];
		}else{
			reflection.textureInfo[0] = reflection.textureInfo[1] = 1.0f;
			reflection.textureInfo[2] = reflection.textureInfo[3] = 1.0f;
		}
		for(uint32 i = 0; i < dynamicPointLightCount; i++){
			const DynamicPointLight &light = dynamicPointLights[i];
			reflection.dynamicPositionRadius[i][0] = light.position[0];
			reflection.dynamicPositionRadius[i][1] = light.position[1];
			reflection.dynamicPositionRadius[i][2] = light.position[2];
			reflection.dynamicPositionRadius[i][3] = light.radius;
			reflection.dynamicColourSpot[i][0] = light.colour[0];
			reflection.dynamicColourSpot[i][1] = light.colour[1];
			reflection.dynamicColourSpot[i][2] = light.colour[2];
			reflection.dynamicColourSpot[i][3] = light.spot ? 1.0f : 0.0f;
			reflection.dynamicDirection[i][0] = light.direction[0];
			reflection.dynamicDirection[i][1] = light.direction[1];
			reflection.dynamicDirection[i][2] = light.direction[2];
		}
		reflection.dynamicParams[0] = (float32)dynamicPointLightCount;
		reflection.dynamicParams[1] = dynamicPointLightGlowStrength;
		if(!allocateArenaConstants(&reflection, sizeof(reflection),
		       &screenSpaceReflectionConstantAddress[frame]))
			return 0;
		screenSpaceReflectionConstantGeneration[frame] =
			boneArenaGeneration[frame];
	}
	*constantsAddress = screenSpaceReflectionConstantAddress[frame];
	return 1;
}

bool32
beginStereoSinglePass(void)
{
	const uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(!pipelineReady || stereoSinglePassActive || stereoWorldEye != 0 ||
	   stereoRightCameraFrame != frame){
		if(isDetailedProfilingEnabled())
			worldRenderProfile.stereoSinglePassFallbacks++;
		return 0;
	}
	if(stereoRightCameraUploadFrame != frame || stereoRightCameraAddress == 0){
		if(!allocateArenaConstants(stereoRightCameraConstants,
		       sizeof(stereoRightCameraConstants), &stereoRightCameraAddress)){
			if(isDetailedProfilingEnabled())
				worldRenderProfile.stereoSinglePassFallbacks++;
			return 0;
		}
		stereoRightCameraUploadFrame = frame;
	}
	if(!setStereoWideViewport(1)){
		if(isDetailedProfilingEnabled())
			worldRenderProfile.stereoSinglePassFallbacks++;
		return 0;
	}
	stereoSinglePassActive = 1;
	const bool32 profiling = isDetailedProfilingEnabled();
	if(profiling)
		worldRenderProfile.stereoSinglePassBegins++;
	FixedFoveatedRenderingInfo foveatedInfo;
	getFixedFoveatedRenderingInfo(&foveatedInfo);
	if(foveatedInfo.supported && foveatedInfo.enabled){
		if(beginFixedFoveatedRendering()){
			if(profiling)
				worldRenderProfile.fixedFoveatedBegins++;
		}
		else if(profiling)
			worldRenderProfile.fixedFoveatedFailures++;
	}
	return 1;
}

void
endStereoSinglePass(void)
{
	if(!stereoSinglePassActive)
		return;
	endFixedFoveatedRendering();
	stereoSinglePassActive = 0;
	setStereoWideViewport(0);
}

static bool32
allocateBoneConstants(const float *matrices,
	                  D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	return allocateArenaConstants(matrices,
	       MAX_SKIN_BONES*16*sizeof(float), address);
}

static bool32
allocateLightingConstants(const LightingConstants *constants,
	                     D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	return allocateArenaConstants(constants, sizeof(*constants), address);
}

static void
collectLighting(Atomic *atomic, LightingConstants *constants)
{
	memset(constants, 0, sizeof(*constants));
	constants->ambient[3] = 1.0f;
	if(atomic == nil || atomic->geometry == nil ||
	   (atomic->geometry->flags & Geometry::LIGHT) == 0 ||
	   engine->currentWorld == nil)
		return;

	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	memset(&lightData, 0, sizeof(lightData));
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;
	((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	constants->ambient[0] = lightData.ambient.red;
	constants->ambient[1] = lightData.ambient.green;
	constants->ambient[2] = lightData.ambient.blue;
	int32 count = 0;
	for(int32 i = 0; i < lightData.numDirectionals &&
	    count < MAX_WORLD_LIGHTS; i++){
		Light *light = lightData.directionals[i];
		if(light == nil || light->getFrame() == nil)
			continue;
		V3d direction = light->getFrame()->getLTM()->at;
		constants->colorRadius[count][0] = light->color.red;
		constants->colorRadius[count][1] = light->color.green;
		constants->colorRadius[count][2] = light->color.blue;
		constants->colorRadius[count][3] = -1.0f;
		constants->directionClamp[count][0] = direction.x;
		constants->directionClamp[count][1] = direction.y;
		constants->directionClamp[count][2] = direction.z;
		constants->directionClamp[count][3] = -1.0f;
		count++;
	}
	for(int32 i = 0; i < lightData.numLocals &&
	    count < MAX_WORLD_LIGHTS; i++){
		Light *light = lightData.locals[i];
		if(light == nil || light->getFrame() == nil || light->radius <= 0.0f)
			continue;
		Matrix *matrix = light->getFrame()->getLTM();
		constants->colorRadius[count][0] = light->color.red;
		constants->colorRadius[count][1] = light->color.green;
		constants->colorRadius[count][2] = light->color.blue;
		constants->colorRadius[count][3] = light->radius;
		constants->positionCos[count][0] = matrix->pos.x;
		constants->positionCos[count][1] = matrix->pos.y;
		constants->positionCos[count][2] = matrix->pos.z;
		if(light->getType() == Light::POINT){
			constants->directionClamp[count][3] = -1.0f;
		}else{
			constants->positionCos[count][3] = light->minusCosAngle;
			constants->directionClamp[count][0] = matrix->at.x;
			constants->directionClamp[count][1] = matrix->at.y;
			constants->directionClamp[count][2] = matrix->at.z;
			constants->directionClamp[count][3] =
				light->getType() == Light::SOFTSPOT ? 0.0f : 1.0f;
		}
		count++;
	}
}

static bool32
uploadSkinMatrices(Atomic *atomic, bool32 motionEnabled,
	               D3D12_GPU_VIRTUAL_ADDRESS *address,
	               bool32 *isSkinned, bool32 *previousSkinValid)
{
	*previousSkinValid = 0;
	Skin *skin = atomic && atomic->geometry ? Skin::get(atomic->geometry) : nil;
	*isSkinned = skin != nil && skin->numBones > 0 &&
		skin->inverseMatrices != nil;
	if(!*isSkinned){
		uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
		if(boneArenas[frame].resource == nil)
			return 0;
		*address = boneArenas[frame].resource->GetGPUVirtualAddress();
		return 1;
	}
	float matrices[MAX_SKIN_BONES*2*16];
	const uint32 currentMatrixBytes = MAX_SKIN_BONES*16*sizeof(float);
	memset(matrices, 0, currentMatrixBytes);
	for(int32 i = 0; i < MAX_SKIN_BONES; i++){
		matrices[i*16] = 1.0f;
		matrices[i*16+5] = 1.0f;
		matrices[i*16+10] = 1.0f;
		matrices[i*16+15] = 1.0f;
	}

	HAnimHierarchy *hierarchy = Skin::getHierarchy(atomic);
	int32 count = skin->numBones < MAX_SKIN_BONES ?
		skin->numBones : MAX_SKIN_BONES;
	if(hierarchy && hierarchy->matrices && atomic->getFrame()){
		if(hierarchy->numNodes < count)
			count = hierarchy->numNodes;
		Matrix *inverseMatrices = (Matrix*)skin->inverseMatrices;
		if(hierarchy->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(int32 i = 0; i < count; i++){
				Matrix result;
				Matrix::mult(&result, &inverseMatrices[i],
				             &hierarchy->matrices[i]);
				fillMatrix(matrices + i*16, &result);
			}
		}else{
			Matrix inverseAtomic;
			Matrix::invert(&inverseAtomic, atomic->getFrame()->getLTM());
			for(int32 i = 0; i < count; i++){
				Matrix local, result;
				Matrix::mult(&local, &hierarchy->matrices[i],
				             &inverseAtomic);
				Matrix::mult(&result, &inverseMatrices[i], &local);
				fillMatrix(matrices + i*16, &result);
			}
		}
	}
	if(!motionEnabled)
		return allocateArenaConstants(matrices, currentMatrixBytes, address);
	SkinMotionRecord &motion = skinMotionHistory[atomic];
	if(motion.serial != motionFrameSerial){
		motion.previousValidThisFrame = motion.initialized &&
			motion.serial+1 == motionFrameSerial;
		if(motion.previousValidThisFrame)
			memcpy(motion.previousMatricesThisFrame, motion.matrices,
			       sizeof(motion.previousMatricesThisFrame));
		else
			memcpy(motion.previousMatricesThisFrame, matrices,
			       sizeof(motion.previousMatricesThisFrame));
		memcpy(motion.matrices, matrices, sizeof(motion.matrices));
		motion.serial = motionFrameSerial;
		motion.initialized = 1;
	}
	memcpy(matrices + MAX_SKIN_BONES*16,
	       motion.previousMatricesThisFrame,
	       sizeof(motion.previousMatricesThisFrame));
	*previousSkinValid = motion.previousValidThisFrame;
	return allocateArenaConstants(matrices, sizeof(matrices), address);
}

enum MeshSelection {
	MESH_ALL,
	MESH_OPAQUE_ONLY,
	MESH_TRANSPARENT_ONLY
};

static bool32
renderGeometry(Atomic *atomic, MeshSelection selection, uint8 fadeAlpha)
{
	if(!pipelineReady || atomic == nil || atomic->geometry == nil ||
	   !instanceGeometry(atomic->geometry))
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(list == nil || camera == nil || atomic->getFrame() == nil)
		return 0;
	const bool32 motionTargets = bindCurrentWorldTargets();
	if(!motionTargets)
		bindCurrentColorTarget();
	D3D12InstanceDataHeader *header =
		(D3D12InstanceDataHeader*)atomic->geometry->instData;
	const bool32 stereoDraw = stereoSinglePassActive &&
		stereoRightCameraAddress != 0;
	// Streamline records its own work into this command list and other passes
	// swap descriptor heaps, so the cache can only be trusted while the list
	// belongs to us. Anchor it to the list identity as a backstop for any
	// state setter that forgets to invalidate.
	if(worldDrawState.list != list)
		resetWorldDrawState();
	worldDrawState.list = list;
	ID3D12RootSignature *wantedRoot = stereoDraw ? stereoRootSignature : rootSignature;
	if(worldDrawState.rootSignature != wantedRoot){
		list->SetGraphicsRootSignature(wantedRoot);
		worldDrawState.rootSignature = wantedRoot;
		// A new root signature invalidates every bound root argument.
		worldDrawState.pipeline = nil;
		worldDrawState.boneAddress = 0;
		worldDrawState.lightingAddress = 0;
		worldDrawState.stereoCamera = 0;
		worldDrawState.reflectionConstants = 0;
		worldDrawState.texture = 0;
		worldDrawState.sampler = 0;
		worldDrawState.reflectionTexture = 0;
		worldDrawState.reflectionSampler = 0;
	}
	if(stereoDraw && worldDrawState.stereoCamera != stereoRightCameraAddress){
		list->SetGraphicsRootConstantBufferView(5, stereoRightCameraAddress);
		worldDrawState.stereoCamera = stereoRightCameraAddress;
	}
	D3D12_GPU_DESCRIPTOR_HANDLE reflectionTexture;
	D3D12_GPU_DESCRIPTOR_HANDLE reflectionSampler;
	D3D12_GPU_VIRTUAL_ADDRESS reflectionConstants;
	if(!getScreenSpaceReflectionBindings(&reflectionTexture,
	       &reflectionSampler, &reflectionConstants))
		return 0;
	const uint32 reflectionTextureRoot = stereoDraw ? 6 : 5;
	const uint32 reflectionConstantsRoot = stereoDraw ? 7 : 6;
	const uint32 reflectionSamplerRoot = stereoDraw ? 8 : 7;
	if(worldDrawState.reflectionTexture != reflectionTexture.ptr){
		list->SetGraphicsRootDescriptorTable(
			reflectionTextureRoot, reflectionTexture);
		worldDrawState.reflectionTexture = reflectionTexture.ptr;
	}
	if(worldDrawState.reflectionConstants != reflectionConstants){
		list->SetGraphicsRootConstantBufferView(
			reflectionConstantsRoot, reflectionConstants);
		worldDrawState.reflectionConstants = reflectionConstants;
	}
	if(worldDrawState.reflectionSampler != reflectionSampler.ptr){
		list->SetGraphicsRootDescriptorTable(
			reflectionSamplerRoot, reflectionSampler);
		worldDrawState.reflectionSampler = reflectionSampler.ptr;
	}
	if(worldDrawState.topology != (uint32)header->topology){
		list->IASetPrimitiveTopology(header->topology);
		worldDrawState.topology = (uint32)header->topology;
	}
	if(worldDrawState.vertexBuffer != header->vertexView.BufferLocation){
		list->IASetVertexBuffers(0, 1, &header->vertexView);
		worldDrawState.vertexBuffer = header->vertexView.BufferLocation;
	}
	if(worldDrawState.indexBuffer != header->indexView.BufferLocation){
		list->IASetIndexBuffer(&header->indexView);
		worldDrawState.indexBuffer = header->indexView.BufferLocation;
	}

	float constants[54];
	memset(constants, 0, sizeof(constants));
	fillMatrix(constants, atomic->getFrame()->getLTM());
	memcpy(constants + 16, &camera->devView, 16*sizeof(float));
	memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	bool32 isSkinned;
	bool32 previousSkinValid;
	LightingConstants lighting;
	StereoAtomicCacheEntry *cached = findStereoAtomicCache(atomic, 0);
	if(cached && cached->motionEnabled == motionTargets){
		boneAddress = cached->boneAddress;
		isSkinned = cached->isSkinned;
		previousSkinValid = cached->previousSkinValid;
		lighting = cached->lighting;
	}else{
		if(!uploadSkinMatrices(atomic, motionTargets, &boneAddress, &isSkinned,
		       &previousSkinValid))
			return 0;
		collectLighting(atomic, &lighting);
		// Only the left eye populates the cache.  A right-eye-only edge object
		// simply follows the normal path instead of contaminating this frame's
		// left-eye snapshot.
		if(stereoWorldEye == 0){
			StereoAtomicCacheEntry *entry = findStereoAtomicCache(atomic, 1);
			if(entry){
				entry->boneAddress = boneAddress;
				entry->isSkinned = isSkinned;
				entry->previousSkinValid = previousSkinValid;
				entry->motionEnabled = motionTargets;
				entry->lighting = lighting;
			}
		}
	}
	// drawFlags.y packs skin in bit 0 and the mono eye in bit 1. The stereo
	// shader selects its eye from SV_InstanceID and only consumes the skin bit.
	constants[49] = (isSkinned ? 1.0f : 0.0f) +
		(stereoWorldEye == 1 ? 2.0f : 0.0f);
	if(motionTargets)
		getAtomicMotion(atomic, constants,
			isPotentiallyDynamicAtomic(isSkinned), isSkinned,
			previousSkinValid, lighting.previousWorld, lighting.motionParams);
	const float32 alphaTestReference =
		(uint32)(uintptr_t)getRenderState(ALPHATESTREF)/255.0f;
	const float32 alphaTestFunction =
		(float32)(uint32)(uintptr_t)getRenderState(ALPHATESTFUNC);
	constants[50] = alphaTestReference;
	constants[51] = alphaTestFunction;
	// World atomics are submitted from the renderer's fog-enabled passes, but
	// later compatibility draws can change the global RW state. Use the camera
	// range directly here so the modern backend cannot inherit a stale FALSE.
	if(distanceFogEnabled && camera->fogPlane < camera->farPlane){
		constants[52] = camera->farPlane;
		constants[53] = 1.0f/(camera->fogPlane - camera->farPlane);
	}
	uint32 packedFog = (uint32)(uintptr_t)getRenderState(FOGCOLOR);
	if(worldDrawState.boneAddress != boneAddress){
		list->SetGraphicsRootConstantBufferView(2, boneAddress);
		worldDrawState.boneAddress = boneAddress;
	}
	D3D12_GPU_DESCRIPTOR_HANDLE fallback;
	getTextureView(whiteRaster, &fallback, nil);
	bool32 hasTransparent = 0;
	LightingConstants previousLighting;
	D3D12_GPU_VIRTUAL_ADDRESS previousLightingAddress = 0;
	memset(&previousLighting, 0, sizeof(previousLighting));
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *material = header->meshes[i].material;
		// Match the established D3D/GL/Vulkan RenderWare contract: material
		// colour only modulates a mesh when the geometry explicitly requests it.
		// Several replacement vegetation models carry a legacy 150-grey material
		// but omit Geometry::MODULATE; applying that colour unconditionally turns
		// their textured crowns into a flat grey/dark mass on the D3D12 backend.
		RGBA color = material &&
			(atomic->geometry->flags & Geometry::MODULATE) ?
			material->color : makeRGBA(255, 255, 255, 255);
		color.alpha = (uint8)((color.alpha*fadeAlpha)/255);
		lighting.materialColor[0] = color.red/255.0f;
		lighting.materialColor[1] = color.green/255.0f;
		lighting.materialColor[2] = color.blue/255.0f;
		lighting.materialColor[3] = color.alpha/255.0f;
		memset(lighting.reflectionMaterial, 0,
		       sizeof(lighting.reflectionMaterial));
		lighting.reflectionMaterial[1] = im3DWaterDraw ? 1.0f : 0.0f;
		lighting.reflectionMaterial[2] =
			(atomic->geometry->flags & Geometry::NORMALS) ? 1.0f : 0.0f;
		if(!im3DWaterDraw && screenSpaceReflectionCarPercent > 0.0f &&
		   material != nil &&
		   (atomic->geometry->flags & Geometry::NORMALS)){
			MatFX *matfx = MatFX::get(material);
			if(matfx != nil){
				const int32 slot = matfx->getEffectIndex(MatFX::ENVMAP);
				if(slot >= 0){
					float32 coefficient =
						matfx->fx[slot].env.coefficient*4.0f;
					if(coefficient < 0.0f) coefficient = 0.0f;
					if(coefficient > 1.0f) coefficient = 1.0f;
					lighting.reflectionMaterial[0] = coefficient;
				}
			}
		}
		D3D12_GPU_DESCRIPTOR_HANDLE texture = fallback;
		D3D12_GPU_DESCRIPTOR_HANDLE sampler;
		getSamplerView(Texture::LINEAR, Texture::WRAP, Texture::WRAP, &sampler);
		bool32 textured = 0;
		bool32 textureAlpha = 0;
		if(material && material->texture && material->texture->raster){
			textured = getTextureView(material->texture->raster,
			                          &texture, &textureAlpha);
			uint32 textureFilter = material->texture->getFilter();
			if(textureFilter == Texture::LINEAR &&
			   rasterHasGeneratedMips(material->texture->raster))
				textureFilter = Texture::LINEARMIPLINEAR;
			const bool32 blendedMask = !vehicleAlphaPass &&
				alphaTestFunction != (float32)ALPHAALWAYS &&
				(fadeAlpha != 0xFF || header->meshes[i].vertexAlpha ||
				 color.alpha != 0xFF || (textured && textureAlpha));
			getSamplerView(textureFilter,
			               material->texture->getAddressU(),
			               material->texture->getAddressV(), &sampler,
			               blendedMask ? maskedMipBias : 0);
		}
		bool32 transparent = fadeAlpha != 0xFF ||
			header->meshes[i].vertexAlpha || color.alpha != 0xFF ||
			(textured && textureAlpha);
		hasTransparent = hasTransparent || transparent;
		if(selection == MESH_OPAQUE_ONLY && transparent)
			continue;
		if(selection == MESH_TRANSPARENT_ONLY && !transparent)
			continue;
		if(material){
			lighting.surface[0] = material->surfaceProps.ambient;
			lighting.surface[1] = material->surfaceProps.specular;
			lighting.surface[2] = material->surfaceProps.diffuse;
			lighting.surface[3] = 0.0f;
		}else{
			lighting.surface[0] = 1.0f;
			lighting.surface[1] = 0.0f;
			lighting.surface[2] = 1.0f;
			lighting.surface[3] = 0.0f;
		}
		memcpy(&lighting.surface[3], &packedFog, sizeof(packedFog));
		// Consecutive meshes of an object almost always share their lighting
		// block; reuse the previous upload instead of burning arena space and
		// a root binding on an identical copy.
		D3D12_GPU_VIRTUAL_ADDRESS lightingAddress;
		if(previousLightingAddress != 0 &&
		   memcmp(&previousLighting, &lighting, sizeof(lighting)) == 0)
			lightingAddress = previousLightingAddress;
		else{
			if(!allocateLightingConstants(&lighting, &lightingAddress))
				return hasTransparent;
			previousLighting = lighting;
			previousLightingAddress = lightingAddress;
		}
		if(worldDrawState.lightingAddress != lightingAddress){
			list->SetGraphicsRootConstantBufferView(3, lightingAddress);
			worldDrawState.lightingAddress = lightingAddress;
		}
		uint32 blend = WORLD_BLEND_OPAQUE;
		if(transparent)
			blend = getWorldBlendMode();
		const bool depthTest = getRenderState(ZTESTENABLE) != nil;
		const bool depthWrite = getRenderState(ZWRITEENABLE) != nil;
		uint32 depth = depthTest ?
			(depthWrite ? WORLD_DEPTH_TEST_WRITE : WORLD_DEPTH_TEST_ONLY) :
			(depthWrite ? WORLD_DEPTH_WRITE_ONLY : WORLD_DEPTH_DISABLED);
		uint32 cullState = (uint32)(uintptr_t)getRenderState(CULLMODE);
		uint32 cull = cullState == CULLBACK ? WORLD_CULL_BACK :
			cullState == CULLFRONT ? WORLD_CULL_FRONT : WORLD_CULL_NONE;
		// Match librw's D3D9/GL emulation of the PS2 GS FB_ONLY alpha test.
		// Alpha that passes the GS threshold writes color and depth; failed
		// alpha still blends into the framebuffer but must not occlude later
		// geometry. D3D12 needs two draws because depth writes cannot be
		// selected per pixel in this shader.
		const bool32 splitGSAlpha = transparent && depthWrite &&
			getRenderState(GSALPHATEST) != nil;
		const uint32 numPasses = splitGSAlpha ? 2 : 1;
		for(uint32 pass = 0; pass < numPasses; pass++){
			uint32 passDepth = depth;
			constants[50] = alphaTestReference;
			constants[51] = alphaTestFunction;
			if(splitGSAlpha){
				constants[50] =
					(uint32)(uintptr_t)getRenderState(GSALPHATESTREF)/255.0f;
				constants[51] = (float32)(pass == 0 ?
					ALPHAGREATEREQUAL : ALPHALESS);
				if(pass != 0)
					passDepth = depthTest ?
						WORLD_DEPTH_TEST_ONLY : WORLD_DEPTH_DISABLED;
			}
			ID3D12PipelineState *pipeline = motionTargets ?
				(stereoDraw ? stereoWorldMotionPipelines[blend][passDepth][cull] :
				 worldMotionPipelines[blend][passDepth][cull]) :
				(stereoDraw ? stereoWorldPipelines[blend][passDepth][cull] :
				 worldPipelines[blend][passDepth][cull]);
			if(worldDrawState.pipeline != pipeline){
				list->SetPipelineState(pipeline);
				worldDrawState.pipeline = pipeline;
			}
			constants[48] = textured ? 1.0f : 0.0f;
			if(stereoDraw){
				D3D12_GPU_VIRTUAL_ADDRESS drawAddress;
				if(!allocateArenaConstants(constants, sizeof(constants), &drawAddress))
					return hasTransparent;
				list->SetGraphicsRootConstantBufferView(0, drawAddress);
			}else
				list->SetGraphicsRoot32BitConstants(0, 54, constants, 0);
			if(worldDrawState.texture != texture.ptr){
				list->SetGraphicsRootDescriptorTable(1, texture);
				worldDrawState.texture = texture.ptr;
			}
			if(worldDrawState.sampler != sampler.ptr){
				list->SetGraphicsRootDescriptorTable(4, sampler);
				worldDrawState.sampler = sampler.ptr;
			}
			list->DrawIndexedInstanced(header->meshes[i].numIndices,
			                           stereoDraw ? 2 : 1,
			                           header->meshes[i].startIndex, 0, 0);
			if(stereoCaptureSegment >= 0 && stereoWorldPacketValid){
				if(stereoWorldDrawCount < STEREO_WORLD_DRAW_CAPACITY){
					StereoWorldDraw &draw = stereoWorldDraws[stereoWorldDrawCount++];
					draw.topology = header->topology;
					draw.vertexView = header->vertexView;
					draw.indexView = header->indexView;
					draw.pipeline = pipeline;
					draw.boneAddress = boneAddress;
					draw.lightingAddress = lightingAddress;
					draw.texture = texture;
					draw.sampler = sampler;
					draw.reflectionConstants = reflectionConstants;
					draw.reflectionTexture = reflectionTexture;
					draw.reflectionSampler = reflectionSampler;
					memcpy(draw.constants, constants, sizeof(draw.constants));
					draw.numIndices = header->meshes[i].numIndices;
					draw.startIndex = header->meshes[i].startIndex;
				}else{
					// Never replay a truncated world. The right eye will use the
					// original renderer for every segment when this safety limit is hit.
					stereoWorldPacketValid = 0;
				}
			}
			if(isDetailedProfilingEnabled()){
				worldRenderProfile.drawCalls++;
				worldRenderProfile.submittedIndices += header->meshes[i].numIndices *
					(stereoDraw ? 2 : 1);
				if(stereoDraw){
					worldRenderProfile.stereoSinglePassDrawCalls++;
					worldRenderProfile.stereoSinglePassIndices +=
						header->meshes[i].numIndices * 2;
				}else{
					worldRenderProfile.monoDrawCalls++;
					if(currentWorldStage < WORLD_STAGE_COUNT)
						worldRenderProfile.stageMonoDrawCalls[currentWorldStage]++;
				}
			}
		}
	}
	return hasTransparent;
}

bool32
replayStereoWorldSegment(uint32 segment)
{
	if(stereoWorldEye != 1 || !stereoWorldPacketValid ||
	   segment >= STEREO_WORLD_SEGMENT_COUNT || !pipelineReady)
		return 0;
	const StereoWorldRange &range = stereoWorldRanges[segment];
	if(!range.complete || range.first + range.count > stereoWorldDrawCount)
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(list == nil || camera == nil)
		return 0;
	if(!bindCurrentWorldTargets())
		return 0;

	resetWorldDrawState();
	list->SetGraphicsRootSignature(rootSignature);
	if(InterlockedCompareExchange(&stereoBundlePending, 0, 0) != 0)
		waitForStereoWorldBundleBuild();
	const uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	const bool32 bundleReady = stereoBundleResourcesReady &&
		InterlockedCompareExchange(&stereoBundleSucceeded, 0, 0) != 0 &&
		stereoBundleCompletedGeneration == stereoWorldPacketGeneration &&
		stereoBundleCompletedFrame == frame &&
		stereoBundleFrames[frame].segments[segment] != nil;
	if(bundleReady){
		list->ExecuteBundle(stereoBundleFrames[frame].segments[segment]);
		resetWorldDrawState();
		if(isDetailedProfilingEnabled()){
			worldRenderProfile.drawCalls += range.count;
			worldRenderProfile.stereoBundleDrawCalls += range.count;
			for(uint32 i = 0; i < range.count; i++)
				worldRenderProfile.submittedIndices +=
					stereoWorldDraws[range.first + i].numIndices;
		}
		return 1;
	}
	if(stereoBundleResourcesReady && isDetailedProfilingEnabled())
		worldRenderProfile.stereoBundleFallbacks++;
	const bool32 profiling = isDetailedProfilingEnabled();
	for(uint32 i = 0; i < range.count; i++){
		const StereoWorldDraw &draw = stereoWorldDraws[range.first + i];
		float constants[54];
		memcpy(constants, draw.constants, sizeof(constants));
		constants[49] += 2.0f;
		memcpy(constants + 16, &camera->devView, 16*sizeof(float));
		memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
		list->IASetPrimitiveTopology(draw.topology);
		list->IASetVertexBuffers(0, 1, &draw.vertexView);
		list->IASetIndexBuffer(&draw.indexView);
		list->SetPipelineState(draw.pipeline);
		list->SetGraphicsRoot32BitConstants(0, 54, constants, 0);
		list->SetGraphicsRootDescriptorTable(1, draw.texture);
		list->SetGraphicsRootConstantBufferView(2, draw.boneAddress);
		list->SetGraphicsRootConstantBufferView(3, draw.lightingAddress);
		list->SetGraphicsRootDescriptorTable(4, draw.sampler);
		list->SetGraphicsRootDescriptorTable(5, draw.reflectionTexture);
		list->SetGraphicsRootConstantBufferView(6, draw.reflectionConstants);
		list->SetGraphicsRootDescriptorTable(7, draw.reflectionSampler);
		list->DrawIndexedInstanced(draw.numIndices, 1,
		                           draw.startIndex, 0, 0);
		if(profiling){
			worldRenderProfile.drawCalls++;
			worldRenderProfile.replayDrawCalls++;
			worldRenderProfile.submittedIndices += draw.numIndices;
		}
	}
	return 1;
}

bool32
renderAtomicFirstPass(Atomic *atomic)
{
	return renderGeometry(atomic, MESH_OPAQUE_ONLY, 255);
}

void
renderAtomicBlendPass(Atomic *atomic, uint8 fadeAlpha)
{
	renderGeometry(atomic, MESH_TRANSPARENT_ONLY, fadeAlpha);
}

static void
pipelineInstance(ObjPipeline*, Atomic *atomic)
{
	if(atomic && atomic->geometry)
		instanceGeometry(atomic->geometry);
}

static void
pipelineUninstance(ObjPipeline*, Atomic *atomic)
{
	if(atomic)
		freeInstanceData(atomic->geometry);
}

static void
pipelineRender(ObjPipeline*, Atomic *atomic)
{
	renderGeometry(atomic, MESH_ALL, 255);
}

#else

void *destroyNativeData(void *object, int32, int32) { return object; }

#endif

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipeline = ObjPipeline::create();
#ifdef RW_D3D12
	pipeline->init(PLATFORM_D3D12);
	pipeline->impl.instance = pipelineInstance;
	pipeline->impl.uninstance = pipelineUninstance;
	pipeline->impl.render = pipelineRender;
	createPipelineResources();
#endif
	return pipeline;
}

void
shutdownDefaultPipeline(void)
{
#ifdef RW_D3D12
	pipelineReady = 0;
	shutdownStereoBundleResources();
	atomicMotionHistory.clear();
	skinMotionHistory.clear();
	motionFrameSerial = 0;
	releaseScreenSpaceReflectionHistory();
	im3DWaterDraw = 0;
	screenSpaceReflectionCarPercent = 0.0f;
	screenSpaceReflectionWaterPercent = 0.0f;
	screenSpaceReflectionTime = 0.0f;
	memset(screenSpaceReflectionPreviousCamera, 0,
	       sizeof(screenSpaceReflectionPreviousCamera));
	memset(screenSpaceReflectionCurrentLeftCamera, 0,
	       sizeof(screenSpaceReflectionCurrentLeftCamera));
	screenSpaceReflectionCurrentLeftCameraValid = 0;
	memset(boneArenaGeneration, 0, sizeof(boneArenaGeneration));
	if(whiteRaster){
		whiteRaster->destroy();
		whiteRaster = nil;
	}
	for(uint32 i = 0; i < BONE_FRAME_COUNT; i++){
		if(boneArenas[i].resource && boneArenas[i].mapped)
			boneArenas[i].resource->Unmap(0, nil);
		boneArenas[i].mapped = nil;
		boneArenas[i].offset = 0;
		releaseCom(boneArenas[i].resource);
	}
	activeBoneArena = UINT32_MAX;
	for(uint32 blend = 0; blend < WORLD_BLEND_COUNT; blend++)
		for(uint32 depth = 0; depth < WORLD_DEPTH_COUNT; depth++)
			for(uint32 cull = 0; cull < WORLD_CULL_COUNT; cull++)
			{
				releaseCom(worldPipelines[blend][depth][cull]);
				releaseCom(stereoWorldPipelines[blend][depth][cull]);
				releaseCom(worldMotionPipelines[blend][depth][cull]);
				releaseCom(stereoWorldMotionPipelines[blend][depth][cull]);
			}
	stereoSinglePassActive = 0;
	stereoRightCameraFrame = UINT32_MAX;
	stereoRightCameraUploadFrame = UINT32_MAX;
	stereoRightCameraAddress = 0;
	stereoTemporalCameraValid[0] = 0;
	stereoTemporalCameraValid[1] = 0;
	releaseCom(stereoRootSignature);
	releaseCom(rootSignature);
#endif
}

}
}
