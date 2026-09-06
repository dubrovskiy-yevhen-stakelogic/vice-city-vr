#pragma once

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

namespace rw {
struct Atomic;
struct Raster;

namespace d3d12 {

// R16G16_FLOAT motion targets have no spare validity channel. Static geometry
// and untouched pixels carry this out-of-range value so a genuine zero vector
// from a tracked object remains distinguishable from missing object motion.
static const float32 OBJECT_MOTION_INVALID = 32768.0f;

// CPU-side cost accumulated while the current streaming item creates and
// uploads D3D12 textures. The OpenXR profiler snapshots this alongside the
// slowest streamed item so texture stalls can be split into allocator, copy
// and command-recording costs without doing synchronous per-texture logging.
struct TextureUploadProfile
{
	float32 defaultResourceMs;
	float32 descriptorMs;
	float32 footprintMs;
	float32 uploadResourceMs;
	float32 cpuCopyMs;
	float32 queueMs;
	uint64 uploadBytes;
	uint32 textureResources;
	uint32 uploads;
	// defaultResourceMs split open: the driver's allocation-info query, the
	// free-block scan, heap-page creation, and the placed-resource call itself.
	// Added when allocation cost stayed at 4-7ms for near-zero upload sizes.
	float32 allocInfoMs;
	float32 allocScanMs;
	float32 allocHeapMs;
	float32 allocPlaceMs;
};

void resetTextureUploadProfile(void);
void getTextureUploadProfile(TextureUploadProfile *profile);
// Shared between the raster and device translation units so the placed-heap
// allocator can attribute its cost into the same per-frame profile.
extern TextureUploadProfile textureUploadProfile;
double textureProfileNowMs(void);

struct FrameSyncProfile
{
	float32 frameFenceWaitMs;
	float32 fullGpuWaitMs;
};

void resetFrameSyncProfile(void);
void getFrameSyncProfile(FrameSyncProfile *profile);

// Detailed diagnostics are opt-in. OpenXR enables them only while its
// in-headset profiler is visible or an explicit performance capture is active.
// Keep the hot-path test inline: normal Release rendering then pays one cheap
// branch instead of GPU queries, QPC calls, and thousands of counter updates.
extern volatile LONG detailedProfilingEnabled;
inline bool32 isDetailedProfilingEnabled(void)
{
	return detailedProfilingEnabled != 0;
}
void setDetailedProfilingEnabled(bool32 enabled);

// Optional GPU timestamps split frame cost by render stage.
enum eGpuTimestampStage
{
	GPU_STAGE_SCENE,       // world rasterization for both eyes
	GPU_STAGE_TEMPORAL_AA, // DLSS/DLAA or FSR2 reconstruction
	GPU_STAGE_RESOLVE,     // eye resolve into the OpenXR swapchain images
	GPU_STAGE_FRAME_TOTAL, // complete submitted command list
	GPU_STAGE_COUNT
};

struct GpuFrameProfile
{
	float32 stageMs[GPU_STAGE_COUNT];
	float32 totalMs;
	bool32 valid;
};

void beginGpuTimestamp(uint32 stage);
void endGpuTimestamp(uint32 stage);
void getGpuFrameProfile(GpuFrameProfile *profile);

// Work that is normally hidden inside the legacy RenderWare atomic callback.
// In particular, geometry is converted and two upload buffers are allocated
// the first time a newly streamed model is drawn. That can look like an
// unexplained eye-render spike unless it is reported separately.
struct WorldRenderProfile
{
	float32 geometryInstanceMs;
	float32 bufferUploadMs;
	uint64 bufferBytes;
	uint64 submittedIndices;
	uint32 geometryInstances;
	uint32 drawCalls;
	float32 stereoBundleBuildMs;
	float32 stereoBundleWaitMs;
	uint32 stereoBundleDrawCalls;
	uint32 stereoBundleFallbacks;
	uint32 stereoSinglePassBegins;
	uint32 stereoSinglePassDrawCalls;
	uint64 stereoSinglePassIndices;
	uint32 stereoSinglePassFallbacks;
	uint32 fixedFoveatedBegins;
	uint32 fixedFoveatedFailures;
	// Draw calls that did not use instanced stereo, split by where they came
	// from. Their sum against stereoSinglePassDrawCalls says which part of the
	// frame is paying for two submissions per eye.
	uint32 monoDrawCalls;
	uint32 replayDrawCalls;
	// Mono draw calls attributed to the part of the frame that issued them.
	// Reading the render path did not reveal where thousands of per-eye atomic
	// draws come from, so the frame labels itself instead.
	uint32 stageMonoDrawCalls[24];
};

enum eWorldRenderStage
{
	WORLD_STAGE_OTHER = 0,
	WORLD_STAGE_SKY,
	WORLD_STAGE_ROADS,
	WORLD_STAGE_CORONA_REFLECTIONS,
	WORLD_STAGE_ENTITIES,
	WORLD_STAGE_WATER,
	WORLD_STAGE_BOATS,
	WORLD_STAGE_FADING,
	WORLD_STAGE_TRANSPARENT_WATER,
	WORLD_STAGE_EFFECTS,
	// RenderEffects broken down, because it turned out to issue nearly every
	// per-eye atomic draw and it is a list of fifteen unrelated subsystems.
	WORLD_STAGE_FX_GLASS,
	WORLD_STAGE_FX_SPECIAL,
	WORLD_STAGE_FX_SHADOWS,
	WORLD_STAGE_FX_MOVING,
	WORLD_STAGE_FX_FIRSTPERSON,
	WORLD_STAGE_FX_HANDS,
	// Everything outside the stereo frame. With the stage labels fixed, this is
	// where all the per-eye atomic draws turned out to live.
	WORLD_STAGE_DESKTOP,
	WORLD_STAGE_CINEMA,
	WORLD_STAGE_MENUS,
	// The remainder of the frame, so that nothing is left unattributed.
	WORLD_STAGE_GAME_UPDATE,
	WORLD_STAGE_RENDERLIST,
	WORLD_STAGE_SCENESETUP,
	WORLD_STAGE_HUD2D,
	WORLD_STAGE_ENVMAP,
	WORLD_STAGE_COUNT
};

void setWorldRenderStage(uint32 stage);
// Drops the world pass redundant-state cache. Must be called whenever another
// pass binds its own root signature or a fresh command list is started.
void resetWorldDrawState(void);

void resetWorldRenderProfile(void);
void getWorldRenderProfile(WorldRenderProfile *profile);

// The two legacy RenderScene passes see the same animated pose and the same
// world lights.  Let the D3D12 pipeline retain those eye-independent constants
// after the left eye and reuse them while recording the right eye.
void beginStereoWorldFrame(void);
void forgetAtomicMotion(Atomic *atomic);
void setStereoWorldEye(int32 eye);
// Capture the active eye matrices and temporarily address the complete
// double-wide OpenXR target.  World geometry can then use one instanced draw
// to rasterize both eye views while immediate effects keep their sub-viewports.
void captureStereoWorldCamera(int32 eye, float32 jitterClipX, float32 jitterClipY);
bool32 getStereoWorldCamera(int32 eye, float32 view[16], float32 projection[16]);
void captureDesktopWorldCamera(void);
bool32 getDesktopWorldCamera(float32 view[16], float32 projection[16]);
bool32 beginStereoSinglePass(void);
void endStereoSinglePass(void);

// Previous-frame screen-space reflections. OpenXR supplies the complete
// double-wide scene after both eyes have finished; each eye reprojects with its
// own previous matrices and samples only its matching half of the history.
void setScreenSpaceReflectionSettings(float32 carPercent, float32 waterPercent,
                                      float32 oceanWavePercent);
// Independent water multipliers. Sheen/glint/sparks must be zero when the
// effects master is disabled; defaults are 100/100/0/0/0.
void setWaterSurfaceSettings(float32 speedPercent, float32 distortionPercent,
                            float32 sheenPercent, float32 glintPercent,
                            float32 sparksPercent);
void setWaterLighting(const float32 sunDirectionWorld[3],
                      const float32 sunColour[3], const float32 skyColour[3]);
// The legacy material reflection remains separate. Zero distance is unlimited.
void setCarReflectionSsrParams(float32 strengthPercent, float32 distanceMetres);
// Optional game-time override, in seconds. Without it the legacy clock remains.
void setEffectsTime(float32 seconds);
void setRainSurfaceSettings(float32 wetness, float32 rainfall, int32 mode,
	float32 coveragePercent, float32 edgeSoftnessPercent,
	float32 ripplePercent, float32 reflectionPercent);
// Quest-style per-pixel GTA point lights. The game supplies the nearest lights
// in world space; glowStrength zero keeps surface lighting but disables the
// view-ray air-glow term.
enum { MAX_DYNAMIC_POINT_LIGHTS = 8 };
struct DynamicPointLight
{
	float32 position[3];
	float32 radius;
	float32 colour[3];
	bool32 spot;
	float32 direction[3];
};
void setDynamicPointLights(const DynamicPointLight *lights, uint32 count,
	float32 glowStrength);
bool32 captureScreenSpaceReflectionFrame(Raster *source);
void invalidateScreenSpaceReflectionHistory(void);
void setIm3DWater(bool32 water);
bool32 isIm3DWater(void);
bool32 getScreenSpaceReflectionBindings(
	D3D12_GPU_DESCRIPTOR_HANDLE *view,
	D3D12_GPU_DESCRIPTOR_HANDLE *sampler,
	D3D12_GPU_VIRTUAL_ADDRESS *constants);

// Tier 2 D3D12 variable-rate shading profile used by the double-wide OpenXR
// world pass.  Profile zero disables it; the other profiles progressively
// reduce the full-rate central region while retaining an automatic 1x1
// fallback on GPUs without a shading-rate image.
enum FixedFoveatedProfile {
	FIXED_FOVEATED_OFF,
	FIXED_FOVEATED_QUALITY,
	FIXED_FOVEATED_BALANCED,
	FIXED_FOVEATED_PERFORMANCE,
	FIXED_FOVEATED_PROFILE_COUNT
};

struct FixedFoveatedRenderingInfo
{
	bool32 supported;
	bool32 enabled;
	bool32 active;
	bool32 additionalRates;
	uint32 tier;
	uint32 tileSize;
	uint32 profile;
	uint32 imageWidth;
	uint32 imageHeight;
};

void setFixedFoveatedRenderingProfile(uint32 profile);
void getFixedFoveatedRenderingInfo(FixedFoveatedRenderingInfo *info);
bool32 beginFixedFoveatedRendering(void);
void endFixedFoveatedRendering(void);

// Stage 6 stereo frame packet.  The legacy game builds these heavy world
// passes while rendering the left eye.  D3D12 stores the fully resolved draw
// commands (including skinning, lighting and material state), then replays the
// same immutable commands with the right-eye camera matrices.  Immediate-mode
// weather, water and screen effects deliberately stay on the original path.
enum StereoWorldSegment {
	STEREO_WORLD_ROADS,
	STEREO_WORLD_ENTITIES,
	STEREO_WORLD_BOATS,
	STEREO_WORLD_FADING_UNDERWATER,
	STEREO_WORLD_FADING,
	STEREO_WORLD_SEGMENT_COUNT
};
void beginStereoWorldCapture(uint32 segment);
void endStereoWorldCapture(uint32 segment);
void queueStereoWorldBundleBuild(const float32 *view,
                                 const float32 *projection);
bool32 replayStereoWorldSegment(uint32 segment);
void cancelStereoWorldPacket(void);

// Suballocate ordinary sampled textures from large default heaps. Render
// targets and depth buffers keep their dedicated committed-resource path.
bool32 allocatePlacedTextureResource(const D3D12_RESOURCE_DESC *desc,
                                     D3D12_RESOURCE_STATES initialState,
                                     ID3D12Resource **resource,
                                     uint32 *heapPage, uint64 *heapOffset,
                                     uint64 *heapSize);
void deferTextureAllocationRelease(uint32 heapPage, uint64 heapOffset,
                                   uint64 heapSize);

ID3D12Device *getDevice(void);
// Dedicated VRAM of the selected adapter, in bytes; 0 before initialization.
// The streaming system sizes its cache from this instead of system RAM.
uint64 getAdapterDedicatedVideoMemory(void);
// Session-total texture creation failures; polled by streaming to detect GPU
// memory exhaustion the stream-byte accounting cannot see.
uint32 getTextureCreateFailureCount(void);
// Streamline has to receive the final D3D12 device before librw creates the
// command queue and swap chain.  The game registers this callback before RW
// initialization so renderer-independent builds keep no Streamline dependency.
typedef void (*DeviceCreatedCallback)(void);
void setDeviceCreatedCallback(DeviceCreatedCallback callback);
ID3D12CommandQueue *getCommandQueue(void);
ID3D12GraphicsCommandList *getCommandList(void);
ID3D12DescriptorHeap *getShaderResourceHeap(void);
ID3D12DescriptorHeap *getSamplerHeap(void);
uint32 getFrameIndex(void);
void getPresentSize(int32 *width, int32 *height);
void setPresentInterval(uint32 interval);
// Size of the camera raster currently bound by beginUpdate.  Im2D vertices
// are expressed in pixels of that raster, which can differ from the desktop
// swapchain for VR eyes, HUD layers and other camera textures.
void getCurrentRenderTargetSize(int32 *width, int32 *height);
bool32 readPresentedFrame(uint8 *pixels, uint32 stride,
                          int32 width, int32 height);
bool32 prepareForReadback(void);
// Submit the active command list to the graphics queue without stalling the
// CPU. OpenXR owns completion synchronization after xrReleaseSwapchainImage.
bool32 submitForExternal(void);
// Submit the currently open graphics list and wait until resources shared with
// an external compositor (OpenXR) are safe to release.
bool32 submitAndWaitForExternal(void);
bool32 copyCurrentBackBufferToExternal(ID3D12Resource *destination);
// Copies an external texture over the current back buffer on the open frame
// list; sourceState is the resource's current state and is restored after.
bool32 copyExternalToCurrentBackBuffer(ID3D12Resource *source, uint32 sourceState);
// Clears an external swapchain image (RENDER_TARGET state) to opaque black.
bool32 clearExternalTexture(ID3D12Resource *destination);
// Same clear with no CPU-side GPU wait, for callers holding acquired OpenXR
// swapchain images; the work is only submitted, never waited on.
bool32 clearExternalTexturesNoWait(ID3D12Resource **destinations, int32 count);
// Theater cinema mode: opens a frame outside BeginUpdate when needed, a
// staging texture for the cinema image, and the black-clear + anchored-quad
// draw into an eye swapchain image. All draws record into the frame command
// list and ride the standard frame fence.
bool32 ensureFrameOpen(void);
bool32 createStagingColorTexture(int32 width, int32 height, ID3D12Resource **out);
bool32 drawCinemaQuadToExternal(ID3D12Resource *source, ID3D12Resource *destination,
                                int32 width, int32 height, const float32 *corners);
bool32 uploadRgbaToExternal(ID3D12Resource *destination, const uint8 *pixels,
                            uint32 stride, int32 width, int32 height);
void deferRelease(IUnknown *object);
// Pooled persistently-mapped staging buffers for texture uploads; see
// d3d12device.cpp. Returns 0 when the size exceeds the pool's standard buffer
// so the caller can fall back to a dedicated allocation.
bool32 acquirePooledUpload(uint64 size, ID3D12Resource **resource, uint8 **mapped);
void returnPooledUpload(ID3D12Resource *resource, uint8 *mapped);
// Upload command allocators, lists and buffers are submitted before the next
// presented frame. Keep them alive until that frame's fence has completed.
void deferReleaseAfterNextSubmit(IUnknown *object);
// Queue a texture copy for the next regular frame command list. Ownership of
// upload transfers to the queue on success.
bool32 queueTextureUpload(ID3D12Resource *destination,
                          D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after,
                          ID3D12Resource *upload,
                          uint32 firstLevel, uint32 levelCount);

bool32 allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
                                        uint32 *index);
bool32 getSamplerView(uint32 filter, uint32 addressU, uint32 addressV,
                      D3D12_GPU_DESCRIPTOR_HANDLE *gpu, uint32 mipBiasHalfLevels = 0);
void setDistanceFogEnabled(bool32 enabled);
// Startup setting: existing texture allocations are not rebuilt.
void setGenerateMipmaps(bool32 enabled);
// Blended alpha-tested world materials only: 0, 0.5, 1.0 or 1.5 mip levels.
void setMaskedMipBias(uint32 halfLevels);
void setVehicleAlphaPass(bool32 enabled);
bool32 allocateDepthDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                               uint32 *index);
bool32 allocateRenderTargetDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                      uint32 *index);
void deferDescriptorRelease(uint32 srvIndex, uint32 rtvIndex,
                            uint32 dsvIndex);

bool32 getDepthTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
// The D24S8 depth allocation is typeless internally so temporal passes can
// sample its depth plane through an R24_UNORM_X8_TYPELESS SRV.
bool32 transitionDepthRaster(Raster *raster, D3D12_RESOURCE_STATES state);
bool32 getDepthTextureView(Raster *raster, ID3D12Resource **resource,
                           D3D12_GPU_DESCRIPTOR_HANDLE *view);
bool32 getColorTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
// Opt in before the camera begins drawing. Only temporal world inputs need
// an object-motion attachment; ordinary camera textures have none.
bool32 setRasterMotionEnabled(Raster *raster, bool32 enabled);
// Full temporal-backend shutdown, not a per-eye or foreground toggle.
void releaseRasterMotionResources(Raster *raster);
bool32 getMotionTarget(Raster *raster, ID3D12Resource **resource,
                       D3D12_CPU_DESCRIPTOR_HANDLE *view);
bool32 getMotionTextureView(Raster *raster, ID3D12Resource **resource,
                            D3D12_GPU_DESCRIPTOR_HANDLE *view);
bool32 getRasterResource(Raster *raster, ID3D12Resource **resource);
bool32 transitionRaster(Raster *raster, D3D12_RESOURCE_STATES state);
bool32 setStereoWideViewport(bool32 wide);
bool32 bindCurrentColorTarget(void);
bool32 bindCurrentWorldTargets(void);
bool32 getTextureView(Raster *raster, D3D12_GPU_DESCRIPTOR_HANDLE *view,
                      bool32 *hasAlpha);
// Draw a camera texture into an external typeless RGBA8 target while remapping
// its UV rectangle. Used by OpenXR to convert the stable symmetric game view
// into each runtime-provided asymmetric eye frustum.
bool32 resolveRasterToExternal(Raster *source, ID3D12Resource *destination,
                               int32 width, int32 height,
                               float32 uvScaleX, float32 uvScaleY,
                               float32 uvOffsetX, float32 uvOffsetY,
                               bool32 fxaaEnabled, uint32 colorMode,
                               const float32 blurColor[4],
                               const float32 contrastMult[3],
                               const float32 contrastAdd[3]);
// Resolve a native shader-readable texture into an OpenXR swapchain image.
// DLAA owns the source resource and descriptor, so unlike the raster version
// this function does not perform a source state transition.
bool32 resolveTextureToExternal(ID3D12Resource *source,
                                D3D12_GPU_DESCRIPTOR_HANDLE sourceView,
                                ID3D12Resource *destination,
                                int32 sourceWidth, int32 sourceHeight,
                                int32 width, int32 height,
                                float32 uvScaleX, float32 uvScaleY,
                                float32 uvOffsetX, float32 uvOffsetY,
                                bool32 fxaaEnabled, uint32 colorMode,
                                const float32 blurColor[4],
                                const float32 contrastMult[3],
                                const float32 contrastAdd[3]);

bool32 initializeImmediate(void);
void shutdownImmediate(void);
void setRenderState(int32 state, void *value);
void *getRenderState(int32 state);
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
                       int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices,
                               int32 numVertices, void *indices,
                               int32 numIndices);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world,
                   uint32 flags);
void im3DRenderPrimitive(PrimitiveType type);
void im3DRenderIndexedPrimitive(PrimitiveType type, void *indices,
                               int32 numIndices);
void im3DEnd(void);

}
}
#endif
