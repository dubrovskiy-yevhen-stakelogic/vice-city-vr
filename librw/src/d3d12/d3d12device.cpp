#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

#ifdef RW_D3D12
static void*
destroyAtomicMotionRecord(void *object, int32, int32)
{
	forgetAtomicMotion((Atomic*)object);
	return object;
}
#endif

static void*
driverOpen(void *object, int32, int32)
{
	Driver *driver = engine->driver[PLATFORM_D3D12];
	driver->rasterNativeOffset = nativeRasterOffset;
	driver->rasterCreate = rasterCreate;
	driver->rasterLock = rasterLock;
	driver->rasterUnlock = rasterUnlock;
	driver->rasterLockPalette = null::rasterLockPalette;
	driver->rasterUnlockPalette = null::rasterUnlockPalette;
	driver->rasterNumLevels = rasterNumLevels;
	driver->imageFindRasterFormat = imageFindRasterFormat;
	driver->rasterFromImage = rasterFromImage;
	driver->rasterToImage = rasterToImage;
	driver->defaultPipeline = makeDefaultPipeline();
#ifdef RW_D3D12
	initializeImmediate();
#endif
	return object;
}

static void*
driverClose(void *object, int32, int32)
{
#ifdef RW_D3D12
	shutdownImmediate();
#endif
	shutdownDefaultPipeline();
	Driver *driver = engine->driver[PLATFORM_D3D12];
	if(driver->defaultPipeline &&
	   driver->defaultPipeline != engine->dummyDefaultPipeline){
		driver->defaultPipeline->destroy();
		driver->defaultPipeline = engine->dummyDefaultPipeline;
	}
	return object;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_D3D12, 0, PLATFORM_D3D12,
	                       driverOpen, driverClose);
#ifdef RW_D3D12
	// Plugin lists are per object type, so the D3D12 platform ID can also own
	// Atomic teardown without adding bytes to every Atomic.
	Atomic::registerPlugin(0, ID_RASTERD3D12, nil,
	                       destroyAtomicMotionRecord, nil);
#endif
	registerNativeRaster();
}

#ifdef RW_D3D12

enum {
	FRAME_COUNT = 3,
	MAX_RENDER_TARGET_DESCRIPTORS = 256,
	MAX_DEPTH_DESCRIPTORS = 128,
	MAX_SHADER_RESOURCE_DESCRIPTORS = 16384,
	MAX_SAMPLER_DESCRIPTORS = 512,
	SAMPLER_BIAS_COUNT = 4,
	SAMPLER_FILTER_COUNT = 7,
	SAMPLER_ADDRESS_COUNT = 5
};

struct PendingTextureUpload
{
	ID3D12Resource *destination;
	ID3D12Resource *upload;
	D3D12_RESOURCE_STATES before;
	D3D12_RESOURCE_STATES after;
	uint32 firstLevel;
	uint32 levelCount;
};

struct TextureHeapBlock
{
	UINT64 offset;
	UINT64 size;
};

struct TextureHeapPage
{
	ID3D12Heap *heap;
	UINT64 size;
	std::vector<TextureHeapBlock> freeBlocks;
};

struct TextureHeapAllocation
{
	uint32 page;
	UINT64 offset;
	UINT64 size;
};

struct D3D12Context
{
	HWND window;
	IDXGIFactory4 *factory;
	IDXGIFactory6 *factory6;
	IDXGIAdapter1 *adapter;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	ID3D12Fence *fence;
	IDXGISwapChain3 *swapChain;
	ID3D12DescriptorHeap *rtvHeap;
	ID3D12DescriptorHeap *dsvHeap;
	ID3D12DescriptorHeap *srvHeap;
	ID3D12DescriptorHeap *samplerHeap;
	ID3D12Resource *backBuffers[FRAME_COUNT];
	ID3D12CommandAllocator *commandAllocators[FRAME_COUNT];
	ID3D12GraphicsCommandList *commandList;
	ID3D12GraphicsCommandList5 *commandList5;
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[FRAME_COUNT];
	HANDLE fenceEvent;
	UINT64 fenceValue;
	UINT64 frameFenceValues[FRAME_COUNT];
	UINT rtvDescriptorSize;
	UINT dsvDescriptorSize;
	UINT srvDescriptorSize;
	UINT samplerDescriptorSize;
	UINT nextRtvDescriptor;
	UINT nextDsvDescriptor;
	UINT nextSrvDescriptor;
	UINT nextSamplerDescriptor;
	UINT frameIndex;
	UINT lastPresentedFrame;
	UINT presentInterval;
	int32 width;
	int32 height;
	int32 currentTargetWidth;
	int32 currentTargetHeight;
	int32 desktopWidth;
	int32 desktopHeight;
	int32 currentVideoMode;
	char adapterName[128];
	uint64 adapterDedicatedVideoMemory;
	bool32 initialized;
	bool32 presentationReady;
	bool32 frameOpen;
	bool32 backBufferRendering;
	bool32 hasPresentedFrame;
	D3D12_GPU_DESCRIPTOR_HANDLE samplerCache[SAMPLER_BIAS_COUNT][SAMPLER_FILTER_COUNT][SAMPLER_ADDRESS_COUNT][SAMPLER_ADDRESS_COUNT];
	Raster *currentColorRaster;
	ID3D12Resource *currentColorResource;
	D3D12_CPU_DESCRIPTOR_HANDLE currentColorView;
	ID3D12Resource *currentMotionResource;
	D3D12_CPU_DESCRIPTOR_HANDLE currentMotionView;
	D3D12_CPU_DESCRIPTOR_HANDLE currentDepthView;
	uint32 currentRenderTargetCount;
	ID3D12Resource *fixedFoveatedImage;
	uint32 fixedFoveatedTier;
	uint32 fixedFoveatedTileSize;
	uint32 fixedFoveatedProfile;
	uint32 fixedFoveatedImageProfile;
	uint32 fixedFoveatedImageWidth;
	uint32 fixedFoveatedImageHeight;
	int32 fixedFoveatedTargetWidth;
	int32 fixedFoveatedTargetHeight;
	bool32 fixedFoveatedAdditionalRates;
	bool32 fixedFoveatedActive;
	bool32 fixedFoveatedCreationFailed;
	std::vector<IUnknown*> deferredReleases[FRAME_COUNT];
	std::vector<IUnknown*> pendingSubmitReleases;
	// Descriptor indices freed between frames; migrated to the per-frame
	// deferred lists under the next submitted fence, like the COM releases.
	std::vector<uint32> pendingSubmitSrvDescriptors;
	std::vector<uint32> pendingSubmitRtvDescriptors;
	std::vector<uint32> pendingSubmitDsvDescriptors;
	std::vector<PendingTextureUpload> pendingTextureUploads;
	std::vector<TextureHeapPage> textureHeapPages;
	std::vector<TextureHeapAllocation> deferredTextureAllocations[FRAME_COUNT];
	std::vector<TextureHeapAllocation> pendingTextureAllocations;
	std::vector<uint32> deferredSrvDescriptors[FRAME_COUNT];
	std::vector<uint32> deferredRtvDescriptors[FRAME_COUNT];
	std::vector<uint32> deferredDsvDescriptors[FRAME_COUNT];
	std::vector<uint32> freeSrvDescriptors;
	std::vector<uint32> freeRtvDescriptors;
	std::vector<uint32> freeDsvDescriptors;
	// GPU timestamps: two per stage, read back when the same backbuffer slot is
	// reused so the query results are guaranteed resolved without stalling.
	ID3D12QueryHeap *timestampHeap;
	ID3D12Resource *timestampReadback[FRAME_COUNT];
	uint64 timestampFrequency;
	uint32 timestampWritten[FRAME_COUNT];
	uint32 timestampResolvedMask[FRAME_COUNT];
	uint8 timestampNextSegment[FRAME_COUNT][GPU_STAGE_COUNT];
	uint32 timestampActiveQuery[FRAME_COUNT][GPU_STAGE_COUNT];
};

static DeviceCreatedCallback deviceCreatedCallback;

void
setDeviceCreatedCallback(DeviceCreatedCallback callback)
{
	deviceCreatedCallback = callback;
}

static D3D12Context context;
static uint32 externalCopyLogCount;
static FrameSyncProfile frameSyncProfile;

static double
frameSyncNowMs(void)
{
	static LARGE_INTEGER frequency = {};
	if(frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart * 1000.0 / frequency.QuadPart;
}

void
resetFrameSyncProfile(void)
{
	memset(&frameSyncProfile, 0, sizeof(frameSyncProfile));
}

void
getFrameSyncProfile(FrameSyncProfile *profile)
{
	if(profile)
		*profile = frameSyncProfile;
}

// ---- GPU timestamps ----

// Temporal reconstruction and resolve can run once per eye, and a failed
// temporal resolve can be followed by the FXAA fallback in the same eye. Keep
// separate query pairs for those submissions and aggregate them on readback;
// reusing one query index several times before ResolveQueryData is invalid.
enum {
	TIMESTAMP_SEGMENTS_PER_STAGE = 4,
	TIMESTAMP_QUERY_COUNT = GPU_STAGE_COUNT*TIMESTAMP_SEGMENTS_PER_STAGE*2
};
static GpuFrameProfile gpuFrameProfile;
volatile LONG detailedProfilingEnabled;

void
setDetailedProfilingEnabled(bool32 enabled)
{
	const LONG wanted = enabled ? 1 : 0;
	const LONG previous = InterlockedExchange(&detailedProfilingEnabled, wanted);
	if(wanted != 0 && previous == 0){
		memset(&gpuFrameProfile, 0, sizeof(gpuFrameProfile));
		memset(context.timestampResolvedMask, 0,
		       sizeof(context.timestampResolvedMask));
		memset(context.timestampWritten, 0,
		       sizeof(context.timestampWritten));
		memset(context.timestampNextSegment, 0,
		       sizeof(context.timestampNextSegment));
		for(uint32 frame = 0; frame < FRAME_COUNT; frame++)
			for(uint32 stage = 0; stage < GPU_STAGE_COUNT; stage++)
				context.timestampActiveQuery[frame][stage] = UINT32_MAX;
	}
}

static bool32
ensureTimestampResources(void)
{
	if(context.device == nil || context.queue == nil)
		return 0;
	if(context.timestampHeap != nil)
		return 1;
	D3D12_QUERY_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count = TIMESTAMP_QUERY_COUNT;
	if(FAILED(context.device->CreateQueryHeap(&heapDesc,
	   IID_PPV_ARGS(&context.timestampHeap))))
		return 0;
	D3D12_HEAP_PROPERTIES readback = {};
	readback.Type = D3D12_HEAP_TYPE_READBACK;
	readback.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	readback.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	readback.CreationNodeMask = 1;
	readback.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = TIMESTAMP_QUERY_COUNT*sizeof(UINT64);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for(uint32 i = 0; i < FRAME_COUNT; i++)
		if(FAILED(context.device->CreateCommittedResource(&readback,
		   D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nil,
		   IID_PPV_ARGS(&context.timestampReadback[i])))){
			for(uint32 j = 0; j < FRAME_COUNT; j++)
				if(context.timestampReadback[j]){
					context.timestampReadback[j]->Release();
					context.timestampReadback[j] = nil;
				}
			if(context.timestampHeap) { context.timestampHeap->Release(); context.timestampHeap = nil; }
			return 0;
		}
	if(FAILED(context.queue->GetTimestampFrequency(&context.timestampFrequency)) ||
	   context.timestampFrequency == 0){
		for(uint32 i = 0; i < FRAME_COUNT; i++)
			if(context.timestampReadback[i]){
				context.timestampReadback[i]->Release();
				context.timestampReadback[i] = nil;
			}
		if(context.timestampHeap) { context.timestampHeap->Release(); context.timestampHeap = nil; }
		return 0;
	}
	return 1;
}

void
beginGpuTimestamp(uint32 stage)
{
	if(!isDetailedProfilingEnabled() || stage >= GPU_STAGE_COUNT || !context.frameOpen ||
	   context.commandList == nil || !ensureTimestampResources())
		return;
	const uint32 frame = context.frameIndex;
	if(context.timestampActiveQuery[frame][stage] != UINT32_MAX)
		return;
	const uint32 segment = context.timestampNextSegment[frame][stage]++;
	if(segment >= TIMESTAMP_SEGMENTS_PER_STAGE)
		return;
	const uint32 query =
		(stage*TIMESTAMP_SEGMENTS_PER_STAGE + segment)*2;
	context.commandList->EndQuery(context.timestampHeap,
		D3D12_QUERY_TYPE_TIMESTAMP, query);
	context.timestampWritten[frame] |= 1u << query;
	context.timestampActiveQuery[frame][stage] = query;
}

void
endGpuTimestamp(uint32 stage)
{
	if(!isDetailedProfilingEnabled() || stage >= GPU_STAGE_COUNT || !context.frameOpen ||
	   context.commandList == nil || context.timestampHeap == nil)
		return;
	const uint32 frame = context.frameIndex;
	const uint32 query = context.timestampActiveQuery[frame][stage];
	if(query == UINT32_MAX)
		return;
	context.commandList->EndQuery(context.timestampHeap,
		D3D12_QUERY_TYPE_TIMESTAMP, query+1);
	context.timestampWritten[frame] |= 1u << (query+1);
	context.timestampActiveQuery[frame][stage] = UINT32_MAX;
}

// Reads the results from the previous use of this backbuffer slot, then queues
// this frame's resolve. Nothing here blocks.
static void
collectGpuTimestamps(void)
{
	if(!isDetailedProfilingEnabled() || context.timestampHeap == nil ||
	   context.commandList == nil)
		return;
	const uint32 frame = context.frameIndex;
	ID3D12Resource *readback = context.timestampReadback[frame];
	const uint32 written = context.timestampWritten[frame];
	const uint32 resolved = context.timestampResolvedMask[frame];
	memset(&gpuFrameProfile, 0, sizeof(gpuFrameProfile));
	UINT64 currentFrequency = 0;
	if(context.queue && SUCCEEDED(context.queue->GetTimestampFrequency(
	   &currentFrequency)) && currentFrequency != 0)
		context.timestampFrequency = currentFrequency;
	if(readback && resolved != 0){
		D3D12_RANGE range = { 0, TIMESTAMP_QUERY_COUNT*sizeof(UINT64) };
		UINT64 *stamps = nil;
		if(SUCCEEDED(readback->Map(0, &range, (void**)&stamps)) && stamps){
			UINT64 earliest = 0, latest = 0;
			bool32 anyValid = 0;
			for(uint32 stage = 0; stage < GPU_STAGE_COUNT; stage++){
				for(uint32 segment = 0;
				    segment < TIMESTAMP_SEGMENTS_PER_STAGE; segment++){
					const uint32 query =
						(stage*TIMESTAMP_SEGMENTS_PER_STAGE + segment)*2;
					const uint32 mask = (1u << query) | (1u << (query+1));
					if((resolved & mask) != mask)
						continue;
					const UINT64 begin = stamps[query];
					const UINT64 end = stamps[query+1];
					if(end <= begin)
						continue;
					gpuFrameProfile.stageMs[stage] += (float32)
						((end-begin)*1000.0/context.timestampFrequency);
					anyValid = 1;
					if(earliest == 0 || begin < earliest) earliest = begin;
					if(end > latest) latest = end;
				}
			}
			gpuFrameProfile.totalMs = latest > earliest ? (float32)
				((latest-earliest)*1000.0/context.timestampFrequency) : 0.0f;
			if(gpuFrameProfile.stageMs[GPU_STAGE_FRAME_TOTAL] > 0.0f)
				gpuFrameProfile.totalMs =
					gpuFrameProfile.stageMs[GPU_STAGE_FRAME_TOTAL];
			gpuFrameProfile.valid = anyValid;
			D3D12_RANGE empty = { 0, 0 };
			readback->Unmap(0, &empty);
		}
	}
	uint32 complete = 0;
	if(written != 0 && readback)
		for(uint32 query = 0; query < TIMESTAMP_QUERY_COUNT; query += 2){
			const uint32 pair = (1u << query) | (1u << (query+1));
			if((written & pair) != pair)
				continue;
			context.commandList->ResolveQueryData(context.timestampHeap,
				D3D12_QUERY_TYPE_TIMESTAMP, query, 2, readback,
				query*sizeof(UINT64));
			complete |= pair;
		}
	context.timestampResolvedMask[frame] = complete;
}

void
getGpuFrameProfile(GpuFrameProfile *profile)
{
	if(profile)
		*profile = gpuFrameProfile;
}

static UINT64
alignTextureHeapOffset(UINT64 value, UINT64 alignment)
{
	return alignment > 1 ? (value + alignment - 1) & ~(alignment - 1) : value;
}

// Profiling showed CreateHeap costing 4-6ms no matter the page size or the
// NOT_ZEROED flag — with Streamline interposed, every driver allocation is
// expensive. Heap creation is free-threaded in D3D12, so a worker keeps a
// couple of standard pages ready and the streaming path picks one up without
// ever calling the driver. Oversized pages for huge textures stay on the
// direct path; they are rare.
enum { SPARE_TEXTURE_PAGES = 2 };
static const UINT64 TEXTURE_HEAP_PAGE_SIZE = 32ull*1024ull*1024ull;
static std::mutex gSpareHeapMutex;
static std::condition_variable gSpareHeapSignal;
static std::vector<ID3D12Heap*> gSpareHeaps;
static std::thread gSpareHeapThread;
static bool gSpareHeapThreadRunning;
static bool gSpareHeapStop;

static ID3D12Heap*
createTextureHeapPage(UINT64 pageSize)
{
	D3D12_HEAP_DESC heapDesc = {};
	heapDesc.SizeInBytes = pageSize;
	heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapDesc.Properties.CreationNodeMask = 1;
	heapDesc.Properties.VisibleNodeMask = 1;
	heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES |
		D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
	ID3D12Heap *heap = nil;
	if(FAILED(context.device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)))){
		heapDesc.Flags &= ~D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
		if(FAILED(context.device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))))
			return nil;
	}
	return heap;
}

static void
spareHeapWorker(void)
{
	for(;;){
		std::unique_lock<std::mutex> lock(gSpareHeapMutex);
		gSpareHeapSignal.wait(lock, []{
			return gSpareHeapStop ||
				gSpareHeaps.size() < SPARE_TEXTURE_PAGES;
		});
		if(gSpareHeapStop)
			return;
		lock.unlock();
		ID3D12Heap *heap = createTextureHeapPage(TEXTURE_HEAP_PAGE_SIZE);
		if(heap == nil){
			// Out of memory or device loss; do not spin. The direct path
			// still works (or fails visibly) on the main thread.
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}
		lock.lock();
		gSpareHeaps.push_back(heap);
	}
}

static void
ensureSpareHeapThread(void)
{
	if(gSpareHeapThreadRunning)
		return;
	gSpareHeapStop = false;
	gSpareHeapThread = std::thread(spareHeapWorker);
	gSpareHeapThreadRunning = true;
}

static void
stopSpareHeapThread(void)
{
	if(!gSpareHeapThreadRunning)
		return;
	{
		std::lock_guard<std::mutex> lock(gSpareHeapMutex);
		gSpareHeapStop = true;
	}
	gSpareHeapSignal.notify_all();
	gSpareHeapThread.join();
	gSpareHeapThreadRunning = false;
	for(size_t i = 0; i < gSpareHeaps.size(); i++)
		if(gSpareHeaps[i])
			gSpareHeaps[i]->Release();
	gSpareHeaps.clear();
}

static void
freeTextureAllocationNow(const TextureHeapAllocation &allocation)
{
	if(allocation.page >= context.textureHeapPages.size() ||
	   allocation.size == 0)
		return;
	std::vector<TextureHeapBlock> &blocks =
		context.textureHeapPages[allocation.page].freeBlocks;
	TextureHeapBlock returned = { allocation.offset, allocation.size };
	size_t position = 0;
	while(position < blocks.size() && blocks[position].offset < returned.offset)
		position++;
	blocks.insert(blocks.begin() + position, returned);
	for(size_t i = 0; i + 1 < blocks.size(); ){
		TextureHeapBlock &left = blocks[i];
		TextureHeapBlock &right = blocks[i+1];
		if(left.offset + left.size >= right.offset){
			const UINT64 rightEnd = right.offset + right.size;
			if(rightEnd > left.offset + left.size)
				left.size = rightEnd - left.offset;
			blocks.erase(blocks.begin() + i + 1);
		}else
			i++;
	}
	// A page whose blocks coalesced back to the full span holds 32MB of VRAM
	// for nothing, and the footprint used to be monotonic to the session's
	// peak. Keep one fully-free page as a warm reserve and give the rest
	// back. Live allocations reference pages by index, so the slot stays
	// behind as a tombstone (heap=nil) and is reused for the next page.
	TextureHeapPage &page = context.textureHeapPages[allocation.page];
	if(page.heap != nil && blocks.size() == 1 &&
	   blocks[0].offset == 0 && blocks[0].size == page.size){
		// Keep several fully-free pages as a warm reserve: streaming while
		// driving through the city empties and refills pages constantly,
		// and releasing eagerly turned that into heap create/release churn.
		uint32 emptyPagesKept = 0;
		for(uint32 i = 0; i < context.textureHeapPages.size(); i++){
			if(i == allocation.page)
				continue;
			TextureHeapPage &other = context.textureHeapPages[i];
			if(other.heap != nil && other.freeBlocks.size() == 1 &&
			   other.freeBlocks[0].offset == 0 &&
			   other.freeBlocks[0].size == other.size)
				emptyPagesKept++;
		}
		if(emptyPagesKept >= 4){
			page.heap->Release();
			page.heap = nil;
			page.size = 0;
			page.freeBlocks.clear();
		}
	}
}

bool32
allocatePlacedTextureResource(const D3D12_RESOURCE_DESC *desc,
	                          D3D12_RESOURCE_STATES initialState,
	                          ID3D12Resource **resource,
	                          uint32 *heapPage, uint64 *heapOffset,
	                          uint64 *heapSize)
{
	if(context.device == nil || desc == nil || resource == nil ||
	   heapPage == nil || heapOffset == nil || heapSize == nil ||
	   desc->Flags != D3D12_RESOURCE_FLAG_NONE)
		return 0;
	// Streamed textures repeat the same handful of dimension/format/mip
	// combinations endlessly, and every GetResourceAllocationInfo call goes to
	// the driver (through the Streamline interposer when DLSS is present).
	// Cache the answer per combination; the map stays tiny.
	const bool32 profiling = isDetailedProfilingEnabled();
	double allocProfileStart = profiling ? textureProfileNowMs() : 0.0;
	struct AllocInfoCacheEntry { uint64 key; D3D12_RESOURCE_ALLOCATION_INFO info; };
	static std::vector<AllocInfoCacheEntry> allocInfoCache;
	const uint64 cacheKey = (uint64)desc->Width |
		((uint64)desc->Height << 20) | ((uint64)desc->MipLevels << 40) |
		((uint64)desc->Format << 48);
	D3D12_RESOURCE_ALLOCATION_INFO info = {};
	bool cached = false;
	for(size_t i = 0; i < allocInfoCache.size(); i++)
		if(allocInfoCache[i].key == cacheKey){
			info = allocInfoCache[i].info;
			cached = true;
			break;
		}
	if(!cached){
		info = context.device->GetResourceAllocationInfo(0, 1, desc);
		if(info.SizeInBytes != 0 && info.SizeInBytes != UINT64_MAX)
			allocInfoCache.push_back({ cacheKey, info });
	}
	if(profiling)
		textureUploadProfile.allocInfoMs +=
			(float32)(textureProfileNowMs() - allocProfileStart);
	if(info.SizeInBytes == 0 || info.SizeInBytes == UINT64_MAX)
		return 0;

	allocProfileStart = profiling ? textureProfileNowMs() : 0.0;
	uint32 selectedPage = UINT32_MAX;
	UINT64 selectedOffset = 0;
	for(uint32 pageIndex = 0; pageIndex < context.textureHeapPages.size();
	    pageIndex++){
		// Reclaimed pages stay behind as tombstones; late deferred frees may
		// even park blocks in them. Their heap is gone — never allocate.
		if(context.textureHeapPages[pageIndex].heap == nil)
			continue;
		std::vector<TextureHeapBlock> &blocks =
			context.textureHeapPages[pageIndex].freeBlocks;
		for(size_t blockIndex = 0; blockIndex < blocks.size(); blockIndex++){
			TextureHeapBlock block = blocks[blockIndex];
			const UINT64 offset = alignTextureHeapOffset(block.offset, info.Alignment);
			if(offset < block.offset || offset + info.SizeInBytes > block.offset + block.size)
				continue;
			blocks.erase(blocks.begin() + blockIndex);
			size_t insertPosition = blockIndex;
			if(offset > block.offset){
				TextureHeapBlock prefix = { block.offset, offset - block.offset };
				blocks.insert(blocks.begin() + insertPosition, prefix);
				insertPosition++;
			}
			const UINT64 end = offset + info.SizeInBytes;
			if(end < block.offset + block.size){
				TextureHeapBlock suffix = { end, block.offset + block.size - end };
				blocks.insert(blocks.begin() + insertPosition, suffix);
			}
			selectedPage = pageIndex;
			selectedOffset = offset;
			break;
		}
		if(selectedPage != UINT32_MAX)
			break;
	}
	if(profiling)
		textureUploadProfile.allocScanMs +=
			(float32)(textureProfileNowMs() - allocProfileStart);

	allocProfileStart = profiling ? textureProfileNowMs() : 0.0;
	if(selectedPage == UINT32_MAX){
		const UINT64 pageSize = alignTextureHeapOffset(
			info.SizeInBytes > TEXTURE_HEAP_PAGE_SIZE ?
				info.SizeInBytes : TEXTURE_HEAP_PAGE_SIZE,
			D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
		TextureHeapPage page = {};
		if(pageSize == TEXTURE_HEAP_PAGE_SIZE){
			// Standard page: take a pre-created one; the worker refills the
			// pool off the render thread. Only the very first page of a
			// session (empty pool, worker not yet started) pays the driver
			// cost on this thread.
			ensureSpareHeapThread();
			{
				std::lock_guard<std::mutex> lock(gSpareHeapMutex);
				if(!gSpareHeaps.empty()){
					page.heap = gSpareHeaps.back();
					gSpareHeaps.pop_back();
				}
			}
			gSpareHeapSignal.notify_one();
		}
		if(page.heap == nil){
			page.heap = createTextureHeapPage(pageSize);
			if(page.heap == nil)
				return 0;
		}
		page.size = pageSize;
		if(info.SizeInBytes < pageSize){
			TextureHeapBlock remainder = { info.SizeInBytes,
			                                   pageSize - info.SizeInBytes };
			page.freeBlocks.push_back(remainder);
		}
		// Tombstone slots are NEVER reused: deferred frees still in flight
		// reference pages by index, and a reused slot would let them inject
		// free blocks into an unrelated new page — the allocator then hands
		// out space overlapping live textures. A dead slot costs ~40 bytes.
		context.textureHeapPages.push_back(page);
		selectedPage = (uint32)context.textureHeapPages.size() - 1;
		selectedOffset = 0;
	}
	if(profiling)
		textureUploadProfile.allocHeapMs +=
			(float32)(textureProfileNowMs() - allocProfileStart);

	allocProfileStart = profiling ? textureProfileNowMs() : 0.0;
	HRESULT result = context.device->CreatePlacedResource(
		context.textureHeapPages[selectedPage].heap, selectedOffset, desc,
		initialState, nil, IID_PPV_ARGS(resource));
	if(FAILED(result)){
		TextureHeapAllocation failed = {
			selectedPage, selectedOffset, info.SizeInBytes
		};
		freeTextureAllocationNow(failed);
		return 0;
	}
	*heapPage = selectedPage;
	*heapOffset = selectedOffset;
	*heapSize = info.SizeInBytes;
	if(profiling)
		textureUploadProfile.allocPlaceMs +=
			(float32)(textureProfileNowMs() - allocProfileStart);
	return 1;
}

void
deferTextureAllocationRelease(uint32 heapPage, uint64 heapOffset,
	                          uint64 heapSize)
{
	if(heapPage == UINT32_MAX || heapSize == 0)
		return;
	TextureHeapAllocation allocation = { heapPage, heapOffset, heapSize };
	// Always associate reuse with the next submitted fence. A freshly streamed
	// texture may still be waiting in pendingTextureUploads when its RW raster
	// is destroyed before the next beginFrame.
	context.pendingTextureAllocations.push_back(allocation);
}

static void
logExternalCopy(const char *message, const D3D12_RESOURCE_DESC *source = nil,
	            const D3D12_RESOURCE_DESC *destination = nil, HRESULT result = S_OK)
{
	if(externalCopyLogCount++ >= 32)
		return;
	FILE *file = fopen("d3d12_external_copy.log", externalCopyLogCount == 1 ? "w" : "a");
	if(file == nil)
		return;
	fprintf(file,
	        "%s hr=%08lX frameOpen=%d hasPresented=%d frame=%u last=%u ready=%d",
	        message, (unsigned long)result, (int)context.frameOpen,
	        (int)context.hasPresentedFrame, (unsigned)context.frameIndex,
	        (unsigned)context.lastPresentedFrame, (int)context.presentationReady);
	if(source)
		fprintf(file, " src=%llux%u fmt=%u samples=%u state=%s",
		        (unsigned long long)source->Width, (unsigned)source->Height,
		        (unsigned)source->Format, (unsigned)source->SampleDesc.Count,
		        context.frameOpen ? "render-target" : "present");
	if(destination)
		fprintf(file, " dst=%llux%u fmt=%u samples=%u",
		        (unsigned long long)destination->Width, (unsigned)destination->Height,
		        (unsigned)destination->Format,
		        (unsigned)destination->SampleDesc.Count);
	fputc('\n', file);
	fclose(file);
}

static D3D12_RESOURCE_BARRIER transitionBarrier(
	ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after);
static void finishFrame(void);
static bool32 beginFrame(Camera *camera);

static bool32
areCopyCompatibleFormats(DXGI_FORMAT source, DXGI_FORMAT destination)
{
	if(source == destination)
		return 1;
	// OpenXR runtimes commonly expose an UNORM swapchain through a typeless
	// ID3D12Resource. D3D12 explicitly permits copies inside the same typeless
	// format family, so do not reject these resources before CopyResource.
	if((source == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	    source == DXGI_FORMAT_R8G8B8A8_UNORM ||
	    source == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
	   (destination == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	    destination == DXGI_FORMAT_R8G8B8A8_UNORM ||
	    destination == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
		return 1;
	return 0;
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

ID3D12Device*
getDevice(void)
{
	return context.device;
}

uint64
getAdapterDedicatedVideoMemory(void)
{
	return context.adapterDedicatedVideoMemory;
}

ID3D12CommandQueue*
getCommandQueue(void)
{
	return context.queue;
}

ID3D12GraphicsCommandList*
getCommandList(void)
{
	return context.frameOpen ? context.commandList : nil;
}

ID3D12DescriptorHeap*
getShaderResourceHeap(void)
{
	return context.srvHeap;
}

ID3D12DescriptorHeap*
getSamplerHeap(void)
{
	return context.samplerHeap;
}

uint32
getFrameIndex(void)
{
	return context.frameIndex;
}

void
getPresentSize(int32 *width, int32 *height)
{
	if(width)
		*width = context.width;
	if(height)
		*height = context.height;
}

void
setPresentInterval(uint32 interval)
{
	// OpenXR already provides the frame pacing for VR.  Waiting for the
	// desktop swapchain as well can serialize two unrelated refresh rates and
	// make scripted camera sequences run below real time.
	context.presentInterval = interval ? 1u : 0u;
}

// Clears an external (OpenXR swapchain) texture to opaque black. Acquired
// images follow the same convention as the external copy below: they sit in
// RENDER_TARGET state, so no barriers are needed. Clear descriptors are
// consumed at record time, which makes one reused RTV slot safe for
// several clears in the same list.
bool32
clearExternalTexture(ID3D12Resource *destination)
{
	if(destination == nil || context.device == nil || context.queue == nil)
		return 0;
	static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	static bool32 rtvAllocated;
	if(!rtvAllocated){
		if(!allocateRenderTargetDescriptor(&rtv, nil))
			return 0;
		rtvAllocated = 1;
	}
	static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	context.device->CreateRenderTargetView(destination, nil, rtv);
	if(context.frameOpen && context.commandList != nil){
		context.commandList->ClearRenderTargetView(rtv, black, 0, nil);
		return 1;
	}
	if(!waitForGpu())
		return 0;
	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(context.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(context.device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
			IID_PPV_ARGS(&list)));
	if(ok){
		list->ClearRenderTargetView(rtv, black, 0, nil);
		ok = SUCCEEDED(list->Close());
	}
	if(ok){
		ID3D12CommandList *lists[] = { list };
		context.queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	releaseCom(list);
	releaseCom(allocator);
	return ok;
}

// Clears external swapchain images with NO CPU-side GPU wait, for callers
// holding acquired OpenXR images: a waitForGpu while swapchain images are
// acquired inside an open XR frame wedged real sessions (Oculus cutscene
// freeze, SteamVR 2.16 first-menu-frame freeze, SteamVR 2.12 slow decay).
// xrReleaseSwapchainImage only requires the clears to be SUBMITTED — the
// runtime synchronizes against the app queue itself. The ad-hoc allocator
// and list stay alive through the fence-gated deferred release.
bool32
clearExternalTexturesNoWait(ID3D12Resource **destinations, int32 count)
{
	if(destinations == nil || count <= 0 || context.device == nil ||
	   context.queue == nil)
		return 0;
	if(context.frameOpen && context.commandList != nil){
		for(int32 i = 0; i < count; i++)
			if(!clearExternalTexture(destinations[i]))
				return 0;
		return 1;
	}
	static D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	static bool32 rtvAllocated;
	if(!rtvAllocated){
		if(!allocateRenderTargetDescriptor(&rtv, nil))
			return 0;
		rtvAllocated = 1;
	}
	static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(context.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(context.device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
			IID_PPV_ARGS(&list)));
	if(ok){
		for(int32 i = 0; i < count; i++){
			// The RTV descriptor is consumed at record time, so one slot
			// serves every clear in the list.
			context.device->CreateRenderTargetView(destinations[i], nil, rtv);
			list->ClearRenderTargetView(rtv, black, 0, nil);
		}
		ok = SUCCEEDED(list->Close());
	}
	if(ok){
		ID3D12CommandList *lists[] = { list };
		context.queue->ExecuteCommandLists(1, lists);
	}
	deferReleaseAfterNextSubmit(list);
	deferReleaseAfterNextSubmit(allocator);
	return ok;
}

void
getCurrentRenderTargetSize(int32 *width, int32 *height)
{
	const int32 targetWidth = context.currentTargetWidth > 0 ?
		context.currentTargetWidth : context.width;
	const int32 targetHeight = context.currentTargetHeight > 0 ?
		context.currentTargetHeight : context.height;
	if(width)
		*width = targetWidth;
	if(height)
		*height = targetHeight;
}

bool32
readPresentedFrame(uint8 *pixels, uint32 stride, int32 width, int32 height)
{
	if(!context.presentationReady || !context.hasPresentedFrame ||
	   pixels == nil || width <= 0 || height <= 0 ||
	   context.lastPresentedFrame >= FRAME_COUNT)
		return 0;
	// Screenshot capture runs after showRaster. Keep this path defensive for
	// callers that request a capture between EndUpdate and presentation.
	if(context.frameOpen)
		finishFrame();
	if(!waitForGpu())
		return 0;

	ID3D12Resource *source = context.backBuffers[context.lastPresentedFrame];
	if(source == nil)
		return 0;
	D3D12_RESOURCE_DESC texture = source->GetDesc();
	UINT copyWidth = (UINT)(width < (int32)texture.Width ? width : texture.Width);
	UINT copyHeight = (UINT)(height < (int32)texture.Height ? height : texture.Height);
	if(copyWidth == 0 || copyHeight == 0 || stride < copyWidth*4)
		return 0;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 bufferSize = 0;
	context.device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint,
	                                      &rows, &rowSize, &bufferSize);
	D3D12_HEAP_PROPERTIES heap;
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = bufferSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *readback = nil;
	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(context.device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
		nil, IID_PPV_ARGS(&readback))) &&
		SUCCEEDED(context.device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(context.device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
			IID_PPV_ARGS(&list)));
	if(ok){
		D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
			source, D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->ResourceBarrier(1, &toCopy);
		D3D12_TEXTURE_COPY_LOCATION dst;
		memset(&dst, 0, sizeof(dst));
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION src;
		memset(&src, 0, sizeof(src));
		src.pResource = source;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box = { 0, 0, 0, copyWidth, copyHeight, 1 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
		D3D12_RESOURCE_BARRIER toPresent = transitionBarrier(
			source, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PRESENT);
		list->ResourceBarrier(1, &toPresent);
		ok = SUCCEEDED(list->Close());
	}
	if(ok){
		ID3D12CommandList *lists[] = { list };
		context.queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	if(ok){
		uint8 *mapped = nil;
		D3D12_RANGE readRange = {
			(SIZE_T)footprint.Offset,
			(SIZE_T)(footprint.Offset + footprint.Footprint.RowPitch*copyHeight)
		};
		ok = SUCCEEDED(readback->Map(0, &readRange, (void**)&mapped));
		if(ok){
			for(UINT row = 0; row < copyHeight; row++)
				memcpy(pixels + row*stride,
				       mapped + footprint.Offset + row*footprint.Footprint.RowPitch,
				       copyWidth*4);
			D3D12_RANGE writtenRange = { 0, 0 };
			readback->Unmap(0, &writtenRange);
		}
	}
	releaseCom(list);
	releaseCom(allocator);
	releaseCom(readback);
	return ok;
}

bool32
prepareForReadback(void)
{
	if(context.device == nil || context.queue == nil)
		return 0;
	if(context.frameOpen)
		finishFrame();
	return waitForGpu();
}

bool32
submitForExternal(void)
{
	if(context.device == nil || context.queue == nil)
		return 0;
	if(context.frameOpen)
		finishFrame();
	return 1;
}

// Use the standard frame command list and fence for cinema draws initiated
// after the normal game frame has already been presented.
bool32
ensureFrameOpen(void)
{
	if(context.device == nil || context.queue == nil)
		return 0;
	if(context.frameOpen)
		return 1;
	return beginFrame(nil) ? 1 : 0;
}

// Committed RGBA8 texture kept in RENDER_TARGET state, used to stage the
// cinema image between the backbuffer copy and the theater eye draws.
bool32
createStagingColorTexture(int32 width, int32 height, ID3D12Resource **out)
{
	if(out == nil || width <= 0 || height <= 0 || context.device == nil)
		return 0;
	*out = nil;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = (UINT64)width;
	desc.Height = (UINT)height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	return SUCCEEDED(context.device->CreateCommittedResource(&heap,
		D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
		nil, IID_PPV_ARGS(out))) ? 1 : 0;
}

bool32
submitAndWaitForExternal(void)
{
	if(!submitForExternal())
		return 0;
	return waitForGpu();
}

bool32
copyCurrentBackBufferToExternal(ID3D12Resource *destination)
{
	if(destination == nil || context.device == nil || context.queue == nil){
		logExternalCopy("missing destination/device/queue");
		return 0;
	}
	const bool32 useOpenFrame = context.frameOpen && context.commandList != nil &&
		context.backBuffers[context.frameIndex] != nil;
	if(!useOpenFrame && (!context.hasPresentedFrame ||
	   context.lastPresentedFrame >= FRAME_COUNT ||
	   context.backBuffers[context.lastPresentedFrame] == nil)){
		logExternalCopy("no open or presented source");
		return 0;
	}
	ID3D12Resource *source = useOpenFrame ?
		context.backBuffers[context.frameIndex] :
		context.backBuffers[context.lastPresentedFrame];
	D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
	D3D12_RESOURCE_DESC destinationDesc = destination->GetDesc();
	if(sourceDesc.Dimension != destinationDesc.Dimension ||
	   !areCopyCompatibleFormats(sourceDesc.Format, destinationDesc.Format) ||
	   sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count){
		logExternalCopy("resource mismatch", &sourceDesc, &destinationDesc);
		return 0;
	}
	if(!useOpenFrame){
		// Menus and startup movies can close/present their regular frame before the
		// OpenXR cinema layer asks for it. Copy the last presented image through a
		// short independent list; this intentionally costs one frame of latency only
		// in the mono theatre path.
		if(!waitForGpu()){
			logExternalCopy("pre-copy wait failed", &sourceDesc, &destinationDesc);
			return 0;
		}
		ID3D12CommandAllocator *allocator = nil;
		ID3D12GraphicsCommandList *list = nil;
		bool32 ok = SUCCEEDED(context.device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
			SUCCEEDED(context.device->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
				IID_PPV_ARGS(&list)));
		if(ok){
			D3D12_RESOURCE_BARRIER barriers[2] = {
				transitionBarrier(source, D3D12_RESOURCE_STATE_PRESENT,
				                  D3D12_RESOURCE_STATE_COPY_SOURCE),
				transitionBarrier(destination, D3D12_RESOURCE_STATE_RENDER_TARGET,
				                  D3D12_RESOURCE_STATE_COPY_DEST)
			};
			list->ResourceBarrier(2, barriers);
			if(sourceDesc.Width == destinationDesc.Width &&
			   sourceDesc.Height == destinationDesc.Height)
				list->CopyResource(destination, source);
			else{
				D3D12_TEXTURE_COPY_LOCATION dst = {};
				dst.pResource = destination;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				D3D12_TEXTURE_COPY_LOCATION src = {};
				src.pResource = source;
				src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				D3D12_BOX box = { 0, 0, 0,
					(UINT)(sourceDesc.Width < destinationDesc.Width ?
					 sourceDesc.Width : destinationDesc.Width),
					(UINT)(sourceDesc.Height < destinationDesc.Height ?
					 sourceDesc.Height : destinationDesc.Height), 1 };
				list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			}
			barriers[0] = transitionBarrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
			                                D3D12_RESOURCE_STATE_PRESENT);
			barriers[1] = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
			                                D3D12_RESOURCE_STATE_RENDER_TARGET);
			list->ResourceBarrier(2, barriers);
			ok = SUCCEEDED(list->Close());
		}
		if(ok){
			ID3D12CommandList *lists[] = { list };
			context.queue->ExecuteCommandLists(1, lists);
			ok = waitForGpu();
		}
		if(!ok)
			logExternalCopy("independent copy failed", &sourceDesc, &destinationDesc);
		releaseCom(list);
		releaseCom(allocator);
		return ok;
	}

	D3D12_RESOURCE_BARRIER barriers[2] = {
		transitionBarrier(source, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                  D3D12_RESOURCE_STATE_COPY_SOURCE),
		transitionBarrier(destination, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                  D3D12_RESOURCE_STATE_COPY_DEST)
	};
	context.commandList->ResourceBarrier(2, barriers);
	if(sourceDesc.Width == destinationDesc.Width &&
	   sourceDesc.Height == destinationDesc.Height)
		context.commandList->CopyResource(destination, source);
	else{
		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource = destination;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.pResource = source;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box = { 0, 0, 0,
			(UINT)(sourceDesc.Width < destinationDesc.Width ?
			 sourceDesc.Width : destinationDesc.Width),
			(UINT)(sourceDesc.Height < destinationDesc.Height ?
			 sourceDesc.Height : destinationDesc.Height), 1 };
		context.commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	}
	barriers[0] = transitionBarrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
	                                D3D12_RESOURCE_STATE_RENDER_TARGET);
	barriers[1] = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
	                                D3D12_RESOURCE_STATE_RENDER_TARGET);
	context.commandList->ResourceBarrier(2, barriers);
	return 1;
}

bool32
copyExternalToCurrentBackBuffer(ID3D12Resource *source, uint32 sourceState)
{
	// Frame-list only: the flat-mode temporal AA path replaces the scene
	// portion of the current back buffer mid-frame, before the HUD draws.
	if(source == nil || context.device == nil || !context.frameOpen ||
	   context.commandList == nil ||
	   context.backBuffers[context.frameIndex] == nil)
		return 0;
	ID3D12Resource *destination = context.backBuffers[context.frameIndex];
	D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
	D3D12_RESOURCE_DESC destinationDesc = destination->GetDesc();
	if(sourceDesc.Dimension != destinationDesc.Dimension ||
	   !areCopyCompatibleFormats(sourceDesc.Format, destinationDesc.Format) ||
	   sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count)
		return 0;
	D3D12_RESOURCE_BARRIER barriers[2] = {
		transitionBarrier(source, (D3D12_RESOURCE_STATES)sourceState,
		                  D3D12_RESOURCE_STATE_COPY_SOURCE),
		transitionBarrier(destination, D3D12_RESOURCE_STATE_RENDER_TARGET,
		                  D3D12_RESOURCE_STATE_COPY_DEST)
	};
	context.commandList->ResourceBarrier(2, barriers);
	if(sourceDesc.Width == destinationDesc.Width &&
	   sourceDesc.Height == destinationDesc.Height)
		context.commandList->CopyResource(destination, source);
	else{
		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource = destination;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.pResource = source;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box = { 0, 0, 0,
			(UINT)(sourceDesc.Width < destinationDesc.Width ?
			 sourceDesc.Width : destinationDesc.Width),
			(UINT)(sourceDesc.Height < destinationDesc.Height ?
			 sourceDesc.Height : destinationDesc.Height), 1 };
		context.commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	}
	barriers[0] = transitionBarrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
	                                (D3D12_RESOURCE_STATES)sourceState);
	barriers[1] = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
	                                D3D12_RESOURCE_STATE_RENDER_TARGET);
	context.commandList->ResourceBarrier(2, barriers);
	return 1;
}

bool32
uploadRgbaToExternal(ID3D12Resource *destination, const uint8 *pixels,
	                 uint32 stride, int32 width, int32 height)
{
	if(destination == nil || pixels == nil || context.device == nil ||
	   context.queue == nil ||
	   width <= 0 || height <= 0 || stride < (uint32)width*4)
		return 0;
	const bool32 standalone = !context.frameOpen;
	ID3D12CommandAllocator *standaloneAllocator = nil;
	ID3D12GraphicsCommandList *commandList = context.commandList;
	if(standalone){
		commandList = nil;
		if(FAILED(context.device->CreateCommandAllocator(
		   D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&standaloneAllocator))) ||
		   FAILED(context.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		   standaloneAllocator, nil, IID_PPV_ARGS(&commandList)))){
			releaseCom(commandList);
			releaseCom(standaloneAllocator);
			return 0;
		}
	}else if(commandList == nil){
		return 0;
	}
	D3D12_RESOURCE_DESC texture = destination->GetDesc();
	if(texture.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
	   texture.Width < (UINT64)width || texture.Height < (UINT)height ||
	   !areCopyCompatibleFormats(DXGI_FORMAT_R8G8B8A8_UNORM, texture.Format)){
		// Every failure exit must give the standalone allocator/list back;
		// this one leaked them once per call on runtimes whose swapchain
		// format fails the compatibility check.
		if(standalone) releaseCom(commandList);
		releaseCom(standaloneAllocator);
		return 0;
	}

	D3D12_RESOURCE_DESC uploadTexture = texture;
	uploadTexture.Width = width;
	uploadTexture.Height = height;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 uploadSize = 0;
	context.device->GetCopyableFootprints(&uploadTexture, 0, 1, 0, &footprint,
	                                     &rows, &rowSize, &uploadSize);
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC buffer = {};
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = uploadSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource *upload = nil;
	if(FAILED(context.device->CreateCommittedResource(
	   &heap, D3D12_HEAP_FLAG_NONE, &buffer,
	   D3D12_RESOURCE_STATE_GENERIC_READ, nil, IID_PPV_ARGS(&upload)))){
		if(standalone) releaseCom(commandList);
		releaseCom(standaloneAllocator);
		return 0;
	}
	uint8 *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED(upload->Map(0, &readRange, (void**)&mapped))){
		releaseCom(upload);
		if(standalone) releaseCom(commandList);
		releaseCom(standaloneAllocator);
		return 0;
	}
	const uint32 copyBytes = (uint32)width*4;
	for(UINT row = 0; row < rows && row < (UINT)height; row++)
		memcpy(mapped + footprint.Offset + row*footprint.Footprint.RowPitch,
		       pixels + row*stride, copyBytes);
	upload->Unmap(0, nil);

	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
		destination, D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = upload;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION target = {};
	target.pResource = destination;
	target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	target.SubresourceIndex = 0;
	commandList->CopyTextureRegion(&target, 0, 0, 0, &source, nil);
	barrier = transitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
	                           D3D12_RESOURCE_STATE_RENDER_TARGET);
	commandList->ResourceBarrier(1, &barrier);
	if(!standalone){
		deferRelease(upload);
		return 1;
	}
	bool32 ok = SUCCEEDED(commandList->Close());
	if(ok){
		ID3D12CommandList *lists[] = { commandList };
		context.queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	releaseCom(upload);
	releaseCom(commandList);
	releaseCom(standaloneAllocator);
	return ok;
}

// CreateCommittedResource for an upload staging buffer costs the same
// milliseconds as any other driver allocation here, so staging buffers are
// pooled: one standard persistently-mapped size, taken on upload, returned to
// the pool when the frame slot that recorded the copy has provably finished
// on the GPU. Uploads larger than the standard size keep the direct path.
struct PooledUploadBuffer
{
	ID3D12Resource *resource;
	uint8 *mapped;
};
static const UINT64 UPLOAD_POOL_BUFFER_SIZE = 8ull*1024ull*1024ull;
static std::vector<PooledUploadBuffer> gUploadPoolFree;
static std::vector<PooledUploadBuffer> gUploadPoolInFlight[FRAME_COUNT];

bool32
acquirePooledUpload(UINT64 size, ID3D12Resource **resource, uint8 **mapped)
{
	if(size > UPLOAD_POOL_BUFFER_SIZE || context.device == nil)
		return 0;
	if(!gUploadPoolFree.empty()){
		PooledUploadBuffer buffer = gUploadPoolFree.back();
		gUploadPoolFree.pop_back();
		*resource = buffer.resource;
		*mapped = buffer.mapped;
		return 1;
	}
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = UPLOAD_POOL_BUFFER_SIZE;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	PooledUploadBuffer buffer = {};
	HRESULT result = context.device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nil,
		IID_PPV_ARGS(&buffer.resource));
	if(FAILED(result))
		result = context.device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nil,
			IID_PPV_ARGS(&buffer.resource));
	if(FAILED(result))
		return 0;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED(buffer.resource->Map(0, &readRange, (void**)&buffer.mapped))){
		buffer.resource->Release();
		return 0;
	}
	*resource = buffer.resource;
	*mapped = buffer.mapped;
	return 1;
}

void
returnPooledUpload(ID3D12Resource *resource, uint8 *mapped)
{
	if(resource == nil)
		return;
	if(context.device == nil){
		resource->Release();
		return;
	}
	PooledUploadBuffer buffer = { resource, mapped };
	gUploadPoolInFlight[context.frameIndex % FRAME_COUNT].push_back(buffer);
}

static void
destroyUploadPool(void)
{
	for(uint32 frame = 0; frame < FRAME_COUNT; frame++){
		for(size_t i = 0; i < gUploadPoolInFlight[frame].size(); i++)
			gUploadPoolInFlight[frame][i].resource->Release();
		gUploadPoolInFlight[frame].clear();
	}
	for(size_t i = 0; i < gUploadPoolFree.size(); i++)
		gUploadPoolFree[i].resource->Release();
	gUploadPoolFree.clear();
}

static void
releaseDeferredFrame(uint32 frame)
{
	if(frame >= FRAME_COUNT)
		return;
	for(size_t i = 0; i < context.deferredReleases[frame].size(); i++)
		context.deferredReleases[frame][i]->Release();
	context.deferredReleases[frame].clear();
	// This frame slot has completed on the GPU; its staging buffers are free
	// to be reused.
	gUploadPoolFree.insert(gUploadPoolFree.end(),
		gUploadPoolInFlight[frame].begin(), gUploadPoolInFlight[frame].end());
	gUploadPoolInFlight[frame].clear();
	// The pool ratchets to the session's worst three-frame burst. A tight cap
	// caused buffer create/release churn during heavy streaming (hitches
	// while running with HD assets), so the working set kept is generous;
	// only clearly excessive surplus is returned.
	enum { UPLOAD_POOL_MAX_FREE = 48 };
	while(gUploadPoolFree.size() > UPLOAD_POOL_MAX_FREE){
		gUploadPoolFree.back().resource->Release();
		gUploadPoolFree.pop_back();
	}
	for(size_t i = 0; i < context.deferredTextureAllocations[frame].size(); i++)
		freeTextureAllocationNow(context.deferredTextureAllocations[frame][i]);
	context.deferredTextureAllocations[frame].clear();
	context.freeSrvDescriptors.insert(context.freeSrvDescriptors.end(),
		context.deferredSrvDescriptors[frame].begin(),
		context.deferredSrvDescriptors[frame].end());
	context.freeRtvDescriptors.insert(context.freeRtvDescriptors.end(),
		context.deferredRtvDescriptors[frame].begin(),
		context.deferredRtvDescriptors[frame].end());
	context.freeDsvDescriptors.insert(context.freeDsvDescriptors.end(),
		context.deferredDsvDescriptors[frame].begin(),
		context.deferredDsvDescriptors[frame].end());
	context.deferredSrvDescriptors[frame].clear();
	context.deferredRtvDescriptors[frame].clear();
	context.deferredDsvDescriptors[frame].clear();
}

static void
releasePendingSubmitObjects(void)
{
	for(size_t i = 0; i < context.pendingSubmitReleases.size(); i++)
		context.pendingSubmitReleases[i]->Release();
	context.pendingSubmitReleases.clear();
}

static void
releasePendingTextureUploads(void)
{
	for(size_t i = 0; i < context.pendingTextureUploads.size(); i++){
		releaseCom(context.pendingTextureUploads[i].upload);
		releaseCom(context.pendingTextureUploads[i].destination);
	}
	context.pendingTextureUploads.clear();
}

static bool32 flushPendingTextureUploadsStandalone(void);

bool32
queueTextureUpload(ID3D12Resource *destination,
	               D3D12_RESOURCE_STATES before,
	               D3D12_RESOURCE_STATES after,
	               ID3D12Resource *upload,
	               uint32 firstLevel, uint32 levelCount)
{
	if(context.device == nil || destination == nil || upload == nil ||
	   levelCount == 0 || levelCount > 16)
		return 0;
	PendingTextureUpload pending;
	pending.destination = destination;
	pending.upload = upload;
	pending.before = before;
	pending.after = after;
	pending.firstLevel = firstLevel;
	pending.levelCount = levelCount;
	destination->AddRef();
	context.pendingTextureUploads.push_back(pending);
	// NOTE: no automatic standalone flush here. waitForGpu inside an open
	// OpenXR frame (runtime queue-waits pending on unreleased swapchain
	// images) can deadlock the render thread; the backlog is instead
	// bounded by the frame ring draining it at every beginFrame, and the
	// wedged-presentation case is accepted until a provably safe flush
	// point exists.
	return 1;
}

static void
recordTextureUploadsInto(ID3D12GraphicsCommandList *list)
{
	for(size_t pendingIndex = 0;
	    pendingIndex < context.pendingTextureUploads.size(); pendingIndex++){
		PendingTextureUpload &pending =
			context.pendingTextureUploads[pendingIndex];
		if(pending.before != D3D12_RESOURCE_STATE_COPY_DEST){
			D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
				pending.destination, pending.before,
				D3D12_RESOURCE_STATE_COPY_DEST);
			list->ResourceBarrier(1, &barrier);
		}

		D3D12_RESOURCE_DESC texture = pending.destination->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[16];
		UINT rows[16] = {};
		UINT64 rowSizes[16] = {};
		UINT64 totalSize = 0;
		context.device->GetCopyableFootprints(
			&texture, pending.firstLevel, pending.levelCount, 0,
			footprints, rows, rowSizes, &totalSize);
		for(uint32 i = 0; i < pending.levelCount; i++){
			D3D12_TEXTURE_COPY_LOCATION destination;
			memset(&destination, 0, sizeof(destination));
			destination.pResource = pending.destination;
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			destination.SubresourceIndex = pending.firstLevel + i;
			D3D12_TEXTURE_COPY_LOCATION source;
			memset(&source, 0, sizeof(source));
			source.pResource = pending.upload;
			source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			source.PlacedFootprint = footprints[i];
			list->CopyTextureRegion(
				&destination, 0, 0, 0, &source, nil);
		}
		if(pending.after != D3D12_RESOURCE_STATE_COPY_DEST){
			D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
				pending.destination, D3D12_RESOURCE_STATE_COPY_DEST,
				pending.after);
			list->ResourceBarrier(1, &barrier);
		}
	}
}

static void
recordPendingTextureUploads(void)
{
	if(context.commandList == nil || context.device == nil)
		return;
	recordTextureUploadsInto(context.commandList);
	for(size_t i = 0; i < context.pendingTextureUploads.size(); i++){
		deferRelease(context.pendingTextureUploads[i].upload);
		deferRelease(context.pendingTextureUploads[i].destination);
	}
	context.pendingTextureUploads.clear();
}

// Bound staging-resource lifetime when no desktop frame opens by flushing a
// large upload backlog through an independent command list.
static bool32
flushPendingTextureUploadsStandalone(void)
{
	if(context.pendingTextureUploads.empty() || context.device == nil ||
	   context.queue == nil || context.frameOpen)
		return 0;
	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(context.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(context.device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
			IID_PPV_ARGS(&list)));
	if(ok){
		recordTextureUploadsInto(list);
		ok = SUCCEEDED(list->Close());
	}
	if(ok){
		ID3D12CommandList *lists[] = { list };
		context.queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	if(ok){
		for(size_t i = 0; i < context.pendingTextureUploads.size(); i++){
			releaseCom(context.pendingTextureUploads[i].upload);
			releaseCom(context.pendingTextureUploads[i].destination);
		}
		context.pendingTextureUploads.clear();
	}
	// Retain failed uploads for retry unless device removal makes submission
	// impossible.
	if(!ok && context.device != nil &&
	   context.device->GetDeviceRemovedReason() != S_OK){
		for(size_t i = 0; i < context.pendingTextureUploads.size(); i++){
			releaseCom(context.pendingTextureUploads[i].upload);
			releaseCom(context.pendingTextureUploads[i].destination);
		}
		context.pendingTextureUploads.clear();
	}
	releaseCom(list);
	releaseCom(allocator);
	return ok;
}

void
deferRelease(IUnknown *object)
{
	if(object == nil)
		return;
	if(context.device == nil){
		object->Release();
		return;
	}
	// Between frames (streaming destroys run in the game-update phase, after
	// present advanced frameIndex) the current slot is drained by the very
	// next beginFrame after waiting only a FRAME_COUNT-old fence, while up
	// to two submitted frames may still reference the object on the GPU —
	// a use-after-free that surfaces as DEVICE_HUNG and wrong-texture
	// flicker. Park it until the next submitted frame's fence instead; the
	// finishFrame migration already exists for exactly this pattern.
	if(!context.frameOpen){
		context.pendingSubmitReleases.push_back(object);
		return;
	}
	context.deferredReleases[context.frameIndex % FRAME_COUNT].push_back(object);
}

void
deferReleaseAfterNextSubmit(IUnknown *object)
{
	if(object == nil)
		return;
	if(context.device == nil){
		object->Release();
		return;
	}
	context.pendingSubmitReleases.push_back(object);
}

void
deferDescriptorRelease(uint32 srvIndex, uint32 rtvIndex, uint32 dsvIndex)
{
	if(context.device == nil)
		return;
	// Same in-flight hazard as deferRelease above, but worse in effect: a
	// recycled SRV slot is immediately reused by the next texture, which
	// rewrites the shader-visible descriptor while a submitted frame still
	// samples through it. Park between-frames releases until the next
	// submitted fence.
	if(!context.frameOpen){
		if(srvIndex != UINT32_MAX)
			context.pendingSubmitSrvDescriptors.push_back(srvIndex);
		if(rtvIndex != UINT32_MAX && rtvIndex >= FRAME_COUNT)
			context.pendingSubmitRtvDescriptors.push_back(rtvIndex);
		if(dsvIndex != UINT32_MAX)
			context.pendingSubmitDsvDescriptors.push_back(dsvIndex);
		return;
	}
	uint32 frame = context.frameIndex % FRAME_COUNT;
	if(srvIndex != UINT32_MAX)
		context.deferredSrvDescriptors[frame].push_back(srvIndex);
	if(rtvIndex != UINT32_MAX && rtvIndex >= FRAME_COUNT)
		context.deferredRtvDescriptors[frame].push_back(rtvIndex);
	if(dsvIndex != UINT32_MAX)
		context.deferredDsvDescriptors[frame].push_back(dsvIndex);
}

bool32
allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
	                             D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
	                             uint32 *allocatedIndex)
{
	if(context.srvHeap == nil || cpu == nil || gpu == nil)
		return 0;
	uint32 index;
	if(!context.freeSrvDescriptors.empty()){
		index = context.freeSrvDescriptors.back();
		context.freeSrvDescriptors.pop_back();
	}else{
		if(context.nextSrvDescriptor >= MAX_SHADER_RESOURCE_DESCRIPTORS)
			return 0;
		index = context.nextSrvDescriptor++;
	}
	*cpu = context.srvHeap->GetCPUDescriptorHandleForHeapStart();
	*gpu = context.srvHeap->GetGPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)index*context.srvDescriptorSize;
	gpu->ptr += (UINT64)index*context.srvDescriptorSize;
	if(allocatedIndex)
		*allocatedIndex = index;
	return 1;
}

bool32
getSamplerView(uint32 filter, uint32 addressU, uint32 addressV,
               D3D12_GPU_DESCRIPTOR_HANDLE *gpu, uint32 mipBiasHalfLevels)
{
	if(context.samplerHeap == nil || context.device == nil || gpu == nil)
		return 0;
	if(filter < 1 || filter >= SAMPLER_FILTER_COUNT)
		filter = 2;
	if(addressU < 1 || addressU >= SAMPLER_ADDRESS_COUNT)
		addressU = 1;
	if(addressV < 1 || addressV >= SAMPLER_ADDRESS_COUNT)
		addressV = 1;
	if(mipBiasHalfLevels >= SAMPLER_BIAS_COUNT)
		mipBiasHalfLevels = SAMPLER_BIAS_COUNT-1;
	D3D12_GPU_DESCRIPTOR_HANDLE &cached =
		context.samplerCache[mipBiasHalfLevels][filter][addressU][addressV];
	if(cached.ptr != 0){
		*gpu = cached;
		return 1;
	}
	if(context.nextSamplerDescriptor >= MAX_SAMPLER_DESCRIPTORS)
		return 0;

	static const D3D12_FILTER filters[SAMPLER_FILTER_COUNT] = {
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_FILTER_MIN_MAG_MIP_POINT,
		D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		D3D12_FILTER_MIN_MAG_MIP_POINT,
		D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
		D3D12_FILTER_MIN_MAG_MIP_LINEAR
	};
	static const D3D12_TEXTURE_ADDRESS_MODE addresses[SAMPLER_ADDRESS_COUNT] = {
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		D3D12_TEXTURE_ADDRESS_MODE_BORDER
	};
	D3D12_SAMPLER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Filter = filters[filter];
	desc.AddressU = addresses[addressU];
	desc.AddressV = addresses[addressV];
	desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	desc.MipLODBias = (float32)mipBiasHalfLevels*0.5f;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = D3D12_FLOAT32_MAX;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu =
		context.samplerHeap->GetCPUDescriptorHandleForHeapStart();
	cached = context.samplerHeap->GetGPUDescriptorHandleForHeapStart();
	cpu.ptr += (SIZE_T)context.nextSamplerDescriptor*context.samplerDescriptorSize;
	cached.ptr += (UINT64)context.nextSamplerDescriptor*context.samplerDescriptorSize;
	context.nextSamplerDescriptor++;
	context.device->CreateSampler(&desc, cpu);
	*gpu = cached;
	return 1;
}

bool32
allocateDepthDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
	                    uint32 *allocatedIndex)
{
	if(context.dsvHeap == nil || cpu == nil)
		return 0;
	uint32 index;
	if(!context.freeDsvDescriptors.empty()){
		index = context.freeDsvDescriptors.back();
		context.freeDsvDescriptors.pop_back();
	}else{
		if(context.nextDsvDescriptor >= MAX_DEPTH_DESCRIPTORS)
			return 0;
		index = context.nextDsvDescriptor++;
	}
	*cpu = context.dsvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)index*context.dsvDescriptorSize;
	if(allocatedIndex)
		*allocatedIndex = index;
	return 1;
}

bool32
allocateRenderTargetDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
	                           uint32 *allocatedIndex)
{
	if(context.rtvHeap == nil || cpu == nil)
		return 0;
	uint32 index;
	if(!context.freeRtvDescriptors.empty()){
		index = context.freeRtvDescriptors.back();
		context.freeRtvDescriptors.pop_back();
	}else{
		if(context.nextRtvDescriptor >= MAX_RENDER_TARGET_DESCRIPTORS)
			return 0;
		index = context.nextRtvDescriptor++;
	}
	*cpu = context.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)index*context.rtvDescriptorSize;
	if(allocatedIndex)
		*allocatedIndex = index;
	return 1;
}

static void
setAdapterName(IDXGIAdapter1 *adapter)
{
	DXGI_ADAPTER_DESC1 desc;
	memset(&desc, 0, sizeof(desc));
	if(adapter == nil || FAILED(adapter->GetDesc1(&desc))){
		strncpy(context.adapterName, "Unknown D3D12 adapter", sizeof(context.adapterName));
		context.adapterName[sizeof(context.adapterName)-1] = '\0';
		return;
	}
	context.adapterDedicatedVideoMemory = desc.DedicatedVideoMemory;

	int written = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
	                                  context.adapterName,
	                                  (int)sizeof(context.adapterName),
	                                  nil, nil);
	if(written == 0){
		strncpy(context.adapterName, "D3D12 adapter", sizeof(context.adapterName));
		context.adapterName[sizeof(context.adapterName)-1] = '\0';
	}
}

static bool32
tryCreateDevice(IDXGIAdapter1 *adapter)
{
	ID3D12Device *device = nil;
	if(FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
	                            IID_PPV_ARGS(&device))))
		return 0;

	context.adapter = adapter;
	context.adapter->AddRef();
	context.device = device;
	setAdapterName(adapter);
	return 1;
}

static bool32
selectAdapter(void)
{
	IDXGIAdapter1 *candidate = nil;

	if(context.factory6){
		for(UINT i = 0;
		    context.factory6->EnumAdapterByGpuPreference(
		        i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
		        IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
		    i++){
			DXGI_ADAPTER_DESC1 desc;
			candidate->GetDesc1(&desc);
			if((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
			   tryCreateDevice(candidate)){
				releaseCom(candidate);
				return 1;
			}
			releaseCom(candidate);
		}
	}else{
		for(UINT i = 0;
		    context.factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND;
		    i++){
			DXGI_ADAPTER_DESC1 desc;
			candidate->GetDesc1(&desc);
			if((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
			   tryCreateDevice(candidate)){
				releaseCom(candidate);
				return 1;
			}
			releaseCom(candidate);
		}
	}

	// WARP keeps the backend testable on machines without a hardware D3D12
	// adapter. Normal gameplay always reaches the high-performance path first.
	if(SUCCEEDED(context.factory->EnumWarpAdapter(IID_PPV_ARGS(&candidate)))){
		bool32 created = tryCreateDevice(candidate);
		releaseCom(candidate);
		return created;
	}
	return 0;
}

bool32
waitForGpu(void)
{
	if(context.queue == nil || context.fence == nil || context.fenceEvent == nil)
		return 0;

	const UINT64 value = ++context.fenceValue;
	if(FAILED(context.queue->Signal(context.fence, value)))
		return 0;
	if(context.fence->GetCompletedValue() < value){
		if(FAILED(context.fence->SetEventOnCompletion(value, context.fenceEvent)))
			return 0;
		const bool32 profiling = isDetailedProfilingEnabled();
		const double waitStart = profiling ? frameSyncNowMs() : 0.0;
		WaitForSingleObject(context.fenceEvent, INFINITE);
		if(profiling)
			frameSyncProfile.fullGpuWaitMs +=
				(float32)(frameSyncNowMs() - waitStart);
	}
	// This signal is ordered after standalone texture-copy command lists.
	// Their temporary upload resources can now be released safely.
	releasePendingSubmitObjects();
	// The GPU is fully idle, so descriptor indices parked between frames can
	// go straight back to their free lists.
	context.freeSrvDescriptors.insert(context.freeSrvDescriptors.end(),
		context.pendingSubmitSrvDescriptors.begin(),
		context.pendingSubmitSrvDescriptors.end());
	context.pendingSubmitSrvDescriptors.clear();
	context.freeRtvDescriptors.insert(context.freeRtvDescriptors.end(),
		context.pendingSubmitRtvDescriptors.begin(),
		context.pendingSubmitRtvDescriptors.end());
	context.pendingSubmitRtvDescriptors.clear();
	context.freeDsvDescriptors.insert(context.freeDsvDescriptors.end(),
		context.pendingSubmitDsvDescriptors.begin(),
		context.pendingSubmitDsvDescriptors.end());
	context.pendingSubmitDsvDescriptors.clear();
	return 1;
}

static bool32
waitForFrame(UINT frameIndex)
{
	const UINT64 value = context.frameFenceValues[frameIndex];
	if(value == 0 || context.fence->GetCompletedValue() >= value)
		return 1;
	if(FAILED(context.fence->SetEventOnCompletion(value, context.fenceEvent)))
		return 0;
	const bool32 profiling = isDetailedProfilingEnabled();
	const double waitStart = profiling ? frameSyncNowMs() : 0.0;
	WaitForSingleObject(context.fenceEvent, INFINITE);
	if(profiling)
		frameSyncProfile.frameFenceWaitMs +=
			(float32)(frameSyncNowMs() - waitStart);
	return 1;
}

static void
destroyFrameResources(void)
{
	context.frameOpen = 0;
	context.backBufferRendering = 0;
	context.currentColorRaster = nil;
	context.currentColorResource = nil;
	context.currentColorView.ptr = 0;
	context.currentDepthView.ptr = 0;
	context.currentTargetWidth = 0;
	context.currentTargetHeight = 0;
	context.fixedFoveatedActive = 0;
	releaseCom(context.commandList5);
	releaseCom(context.commandList);
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		releaseCom(context.commandAllocators[i]);
		releaseCom(context.backBuffers[i]);
		context.frameFenceValues[i] = 0;
		context.rtvHandles[i].ptr = 0;
	}
	releaseCom(context.rtvHeap);
	releaseCom(context.swapChain);
	context.rtvDescriptorSize = 0;
	context.nextRtvDescriptor = FRAME_COUNT;
	context.frameIndex = 0;
	context.lastPresentedFrame = 0;
	context.hasPresentedFrame = 0;
	context.presentationReady = 0;
}

static bool32
createFrameResources(void)
{
	if(context.window == nil)
		return 1;

	DXGI_SWAP_CHAIN_DESC1 swapDesc;
	memset(&swapDesc, 0, sizeof(swapDesc));
	swapDesc.Width = context.width > 0 ? context.width : 1;
	swapDesc.Height = context.height > 0 ? context.height : 1;
	swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapDesc.SampleDesc.Count = 1;
	swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapDesc.BufferCount = FRAME_COUNT;
	swapDesc.Scaling = DXGI_SCALING_STRETCH;
	swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	IDXGISwapChain1 *swapChain1 = nil;
	HRESULT hr = context.factory->CreateSwapChainForHwnd(
		context.queue, context.window, &swapDesc, nil, nil, &swapChain1);
	if(FAILED(hr))
		return 0;
	context.factory->MakeWindowAssociation(context.window, DXGI_MWA_NO_ALT_ENTER);
	hr = swapChain1->QueryInterface(IID_PPV_ARGS(&context.swapChain));
	releaseCom(swapChain1);
	if(FAILED(hr))
		return 0;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc;
	memset(&heapDesc, 0, sizeof(heapDesc));
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heapDesc.NumDescriptors = MAX_RENDER_TARGET_DESCRIPTORS;
	if(FAILED(context.device->CreateDescriptorHeap(
	        &heapDesc, IID_PPV_ARGS(&context.rtvHeap))))
		return 0;

	context.rtvDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	context.nextRtvDescriptor = FRAME_COUNT;
	D3D12_CPU_DESCRIPTOR_HANDLE handle =
		context.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		if(FAILED(context.swapChain->GetBuffer(
		        i, IID_PPV_ARGS(&context.backBuffers[i]))) ||
		   FAILED(context.device->CreateCommandAllocator(
		        D3D12_COMMAND_LIST_TYPE_DIRECT,
		        IID_PPV_ARGS(&context.commandAllocators[i]))))
			return 0;
		context.rtvHandles[i] = handle;
		context.device->CreateRenderTargetView(context.backBuffers[i], nil, handle);
		handle.ptr += context.rtvDescriptorSize;
	}

	if(FAILED(context.device->CreateCommandList(
	        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
	        context.commandAllocators[0], nil,
	        IID_PPV_ARGS(&context.commandList))))
		return 0;
	context.commandList->QueryInterface(IID_PPV_ARGS(&context.commandList5));
	if(FAILED(context.commandList->Close()))
		return 0;

	context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
	context.presentationReady = 1;
	return 1;
}

static bool32
resizeFrameResources(int32 width, int32 height)
{
	if(!context.presentationReady || context.swapChain == nil ||
	   width <= 0 || height <= 0)
		return 0;
	if(width == context.width && height == context.height)
		return 1;
	if(context.frameOpen)
		finishFrame();
	if(!waitForGpu())
		return 0;
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		releaseDeferredFrame(i);
		releaseCom(context.backBuffers[i]);
		context.frameFenceValues[i] = 0;
	}
	context.currentColorRaster = nil;
	context.currentColorResource = nil;
	context.currentColorView.ptr = 0;
	context.currentDepthView.ptr = 0;
	context.backBufferRendering = 0;
	context.hasPresentedFrame = 0;
	HRESULT hr = context.swapChain->ResizeBuffers(
		FRAME_COUNT, (UINT)width, (UINT)height,
		DXGI_FORMAT_R8G8B8A8_UNORM, 0);
	if(FAILED(hr)){
		context.presentationReady = 0;
		return 0;
	}
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		if(FAILED(context.swapChain->GetBuffer(
		       i, IID_PPV_ARGS(&context.backBuffers[i])))){
			context.presentationReady = 0;
			return 0;
		}
		context.device->CreateRenderTargetView(
			context.backBuffers[i], nil, context.rtvHandles[i]);
	}
	context.width = width;
	context.height = height;
	context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
	context.lastPresentedFrame = context.frameIndex;
	return 1;
}

static bool32
refreshFrameSize(void)
{
	if(context.window == nil)
		return 1;
	RECT rect;
	if(!GetClientRect(context.window, &rect))
		return 1;
	int32 width = rect.right - rect.left;
	int32 height = rect.bottom - rect.top;
	if(width <= 0 || height <= 0 ||
	   (width == context.width && height == context.height))
		return 1;
	return resizeFrameResources(width, height);
}

static D3D12_RESOURCE_BARRIER
transitionBarrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
	              D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

void
setFixedFoveatedRenderingProfile(uint32 profile)
{
	if(profile >= FIXED_FOVEATED_PROFILE_COUNT)
		profile = FIXED_FOVEATED_OFF;
	if(context.fixedFoveatedProfile == profile)
		return;

	if(context.fixedFoveatedActive)
		endFixedFoveatedRendering();
	context.fixedFoveatedProfile = profile;
	context.fixedFoveatedCreationFailed = 0;

	// OFF keeps the last map resident so comparison toggles are free. A change
	// between quality profiles needs a new immutable image; retain the old one
	// until the next submitted frame fence makes its GPU references safe.
	if(profile != FIXED_FOVEATED_OFF && context.fixedFoveatedImage &&
	   context.fixedFoveatedImageProfile != profile){
		deferReleaseAfterNextSubmit(context.fixedFoveatedImage);
		context.fixedFoveatedImage = nil;
		context.fixedFoveatedImageWidth = 0;
		context.fixedFoveatedImageHeight = 0;
		context.fixedFoveatedTargetWidth = 0;
		context.fixedFoveatedTargetHeight = 0;
	}
}

void
getFixedFoveatedRenderingInfo(FixedFoveatedRenderingInfo *info)
{
	if(info == nil)
		return;
	memset(info, 0, sizeof(*info));
	info->supported = context.fixedFoveatedTier >=
		D3D12_VARIABLE_SHADING_RATE_TIER_2 &&
		context.fixedFoveatedTileSize != 0 && context.commandList5 != nil;
	info->enabled = context.fixedFoveatedProfile != FIXED_FOVEATED_OFF;
	info->active = context.fixedFoveatedActive;
	info->additionalRates = context.fixedFoveatedAdditionalRates;
	info->tier = context.fixedFoveatedTier;
	info->tileSize = context.fixedFoveatedTileSize;
	info->profile = context.fixedFoveatedProfile;
	info->imageWidth = context.fixedFoveatedImageWidth;
	info->imageHeight = context.fixedFoveatedImageHeight;
}

static uint8
fixedFoveatedRateForTile(uint32 tileX, uint32 tileY, int32 targetWidth,
	                    int32 targetHeight)
{
	const uint32 tileSize = context.fixedFoveatedTileSize;
	const float32 eyeWidth = targetWidth*0.5f;
	const float32 halfEyeWidth = eyeWidth*0.5f;
	const float32 halfHeight = targetHeight*0.5f;
	float32 pixelX = (tileX + 0.5f)*tileSize;
	float32 pixelY = (tileY + 0.5f)*tileSize;
	if(pixelX > targetWidth-0.5f)
		pixelX = targetWidth-0.5f;
	if(pixelY > targetHeight-0.5f)
		pixelY = targetHeight-0.5f;
	const uint32 eye = pixelX >= eyeWidth ? 1u : 0u;
	const float32 eyeCentreX = eye*eyeWidth + halfEyeWidth;
	const float32 nx = (pixelX-eyeCentreX)/halfEyeWidth;
	const float32 ny = (pixelY-halfHeight)/halfHeight;
	const float32 radiusSquared = nx*nx + ny*ny;

	float32 innerRadius = 0.75f;
	float32 middleRadius = 1.15f;
	if(context.fixedFoveatedProfile == FIXED_FOVEATED_BALANCED){
		innerRadius = 0.65f;
		middleRadius = 1.00f;
	}else if(context.fixedFoveatedProfile == FIXED_FOVEATED_PERFORMANCE){
		innerRadius = 0.55f;
		middleRadius = 0.85f;
	}
	if(radiusSquared <= innerRadius*innerRadius)
		return (uint8)D3D12_SHADING_RATE_1X1;
	if(radiusSquared <= middleRadius*middleRadius)
		return (uint8)D3D12_SHADING_RATE_2X2;
	return (uint8)(context.fixedFoveatedAdditionalRates ?
		D3D12_SHADING_RATE_4X4 : D3D12_SHADING_RATE_2X2);
}

static bool32
createFixedFoveatedImage(int32 targetWidth, int32 targetHeight)
{
	if(context.device == nil || context.commandList == nil ||
	   context.fixedFoveatedTileSize == 0 || targetWidth < 2 ||
	   targetHeight < 1 || (targetWidth & 1) != 0)
		return 0;

	const uint32 tileSize = context.fixedFoveatedTileSize;
	const uint32 imageWidth = (targetWidth + tileSize-1)/tileSize;
	const uint32 imageHeight = (targetHeight + tileSize-1)/tileSize;
	if(context.fixedFoveatedImage &&
	   context.fixedFoveatedImageProfile == context.fixedFoveatedProfile &&
	   context.fixedFoveatedTargetWidth == targetWidth &&
	   context.fixedFoveatedTargetHeight == targetHeight)
		return 1;
	if(context.fixedFoveatedCreationFailed)
		return 0;

	if(context.fixedFoveatedImage){
		deferReleaseAfterNextSubmit(context.fixedFoveatedImage);
		context.fixedFoveatedImage = nil;
	}

	D3D12_RESOURCE_DESC textureDesc;
	memset(&textureDesc, 0, sizeof(textureDesc));
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	textureDesc.Width = imageWidth;
	textureDesc.Height = imageHeight;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.MipLevels = 1;
	textureDesc.Format = DXGI_FORMAT_R8_UINT;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	D3D12_HEAP_PROPERTIES defaultHeap;
	memset(&defaultHeap, 0, sizeof(defaultHeap));
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	defaultHeap.CreationNodeMask = 1;
	defaultHeap.VisibleNodeMask = 1;
	ID3D12Resource *image = nil;
	if(FAILED(context.device->CreateCommittedResource(
	       &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
	       D3D12_RESOURCE_STATE_COPY_DEST, nil, IID_PPV_ARGS(&image)))){
		context.fixedFoveatedCreationFailed = 1;
		return 0;
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 uploadSize = 0;
	context.device->GetCopyableFootprints(&textureDesc, 0, 1, 0,
		&footprint, &rows, &rowSize, &uploadSize);
	D3D12_RESOURCE_DESC uploadDesc;
	memset(&uploadDesc, 0, sizeof(uploadDesc));
	uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width = uploadSize;
	uploadDesc.Height = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels = 1;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES uploadHeap;
	memset(&uploadHeap, 0, sizeof(uploadHeap));
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	uploadHeap.CreationNodeMask = 1;
	uploadHeap.VisibleNodeMask = 1;
	ID3D12Resource *upload = nil;
	if(FAILED(context.device->CreateCommittedResource(
	       &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
	       D3D12_RESOURCE_STATE_GENERIC_READ, nil, IID_PPV_ARGS(&upload)))){
		releaseCom(image);
		context.fixedFoveatedCreationFailed = 1;
		return 0;
	}

	uint8 *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED(upload->Map(0, &readRange, (void**)&mapped))){
		releaseCom(upload);
		releaseCom(image);
		context.fixedFoveatedCreationFailed = 1;
		return 0;
	}
	memset(mapped, 0, (size_t)uploadSize);
	for(uint32 y = 0; y < imageHeight; y++){
		uint8 *row = mapped + footprint.Offset +
			y*footprint.Footprint.RowPitch;
		for(uint32 x = 0; x < imageWidth; x++)
			row[x] = fixedFoveatedRateForTile(x, y,
				targetWidth, targetHeight);
	}
	D3D12_RANGE writtenRange = { 0, (SIZE_T)uploadSize };
	upload->Unmap(0, &writtenRange);

	D3D12_TEXTURE_COPY_LOCATION destination;
	memset(&destination, 0, sizeof(destination));
	destination.pResource = image;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION source;
	memset(&source, 0, sizeof(source));
	source.pResource = upload;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint = footprint;
	context.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nil);
	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(image,
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
	context.commandList->ResourceBarrier(1, &barrier);
	deferRelease(upload);

	context.fixedFoveatedImage = image;
	context.fixedFoveatedImageProfile = context.fixedFoveatedProfile;
	context.fixedFoveatedImageWidth = imageWidth;
	context.fixedFoveatedImageHeight = imageHeight;
	context.fixedFoveatedTargetWidth = targetWidth;
	context.fixedFoveatedTargetHeight = targetHeight;
	return 1;
}

bool32
beginFixedFoveatedRendering(void)
{
	FixedFoveatedRenderingInfo info;
	getFixedFoveatedRenderingInfo(&info);
	if(!info.supported || !info.enabled || !context.frameOpen)
		return 0;
	if(context.fixedFoveatedActive)
		return 1;
	if(!createFixedFoveatedImage(context.currentTargetWidth,
	   context.currentTargetHeight))
		return 0;

	D3D12_SHADING_RATE_COMBINER combiners[2] = {
		D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
		D3D12_SHADING_RATE_COMBINER_OVERRIDE
	};
	context.commandList5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
	context.commandList5->RSSetShadingRateImage(context.fixedFoveatedImage);
	context.fixedFoveatedActive = 1;
	return 1;
}

void
endFixedFoveatedRendering(void)
{
	if(!context.fixedFoveatedActive)
		return;
	if(context.commandList5 && context.frameOpen){
		context.commandList5->RSSetShadingRateImage(nil);
		context.commandList5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nil);
	}
	context.fixedFoveatedActive = 0;
}

static bool32
beginFrame(Camera *camera)
{
	if(!context.presentationReady || !refreshFrameSize())
		return 0;
	if(!context.frameOpen){
		context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
		if(!waitForFrame(context.frameIndex))
			return 0;
		releaseDeferredFrame(context.frameIndex);
		context.timestampWritten[context.frameIndex] = 0;
		memset(context.timestampNextSegment[context.frameIndex], 0,
		       sizeof(context.timestampNextSegment[context.frameIndex]));
		for(uint32 stage = 0; stage < GPU_STAGE_COUNT; stage++)
			context.timestampActiveQuery[context.frameIndex][stage] = UINT32_MAX;
		resetWorldDrawState();
		if(FAILED(context.commandAllocators[context.frameIndex]->Reset()) ||
		   FAILED(context.commandList->Reset(
		       context.commandAllocators[context.frameIndex], nil)))
			return 0;
		context.fixedFoveatedActive = 0;

		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			context.backBuffers[context.frameIndex],
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		context.commandList->ResourceBarrier(1, &barrier);
		context.backBufferRendering = 1;
		if(context.srvHeap && context.samplerHeap){
			ID3D12DescriptorHeap *heaps[] = {
				context.srvHeap, context.samplerHeap
			};
			context.commandList->SetDescriptorHeaps(2, heaps);
			resetWorldDrawState();
		}
		context.frameOpen = 1;
		beginGpuTimestamp(GPU_STAGE_FRAME_TOTAL);
		recordPendingTextureUploads();
	}

	Raster *frameBuffer = camera ? camera->frameBuffer : nil;
	Raster *parent = frameBuffer ? frameBuffer->parent : nil;
	Raster *colorRaster = parent && parent->type == Raster::CAMERATEXTURE ?
		parent : nil;
	if(context.currentColorRaster &&
	   context.currentColorRaster != colorRaster)
		transitionRaster(context.currentColorRaster,
		                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	context.currentColorRaster = colorRaster;
	context.currentColorResource = context.backBuffers[context.frameIndex];
	context.currentColorView = context.rtvHandles[context.frameIndex];
	context.currentMotionResource = nil;
	context.currentMotionView.ptr = 0;
	if(colorRaster){
		if(!transitionRaster(colorRaster, D3D12_RESOURCE_STATE_RENDER_TARGET) ||
		   !getColorTarget(colorRaster, &context.currentColorResource,
		                   &context.currentColorView))
			return 0;
		getMotionTarget(colorRaster, &context.currentMotionResource,
		                &context.currentMotionView);
	}
	context.currentDepthView.ptr = 0;
	if(camera && camera->zBuffer)
		getDepthTarget(camera->zBuffer, nil, &context.currentDepthView);
	context.commandList->OMSetRenderTargets(
		1, &context.currentColorView, FALSE,
		context.currentDepthView.ptr ? &context.currentDepthView : nil);
	context.currentRenderTargetCount = 1;

	D3D12_VIEWPORT viewport;
	viewport.TopLeftX = frameBuffer && frameBuffer != parent ?
		(float)frameBuffer->offsetX : 0.0f;
	viewport.TopLeftY = frameBuffer && frameBuffer != parent ?
		(float)frameBuffer->offsetY : 0.0f;
	viewport.Width = (float)(frameBuffer ? frameBuffer->width : context.width);
	viewport.Height = (float)(frameBuffer ? frameBuffer->height : context.height);
	context.currentTargetWidth = frameBuffer ? frameBuffer->width : context.width;
	context.currentTargetHeight = frameBuffer ? frameBuffer->height : context.height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor = {
		(LONG)viewport.TopLeftX, (LONG)viewport.TopLeftY,
		(LONG)(viewport.TopLeftX + viewport.Width),
		(LONG)(viewport.TopLeftY + viewport.Height)
	};
	context.commandList->RSSetViewports(1, &viewport);
	context.commandList->RSSetScissorRects(1, &scissor);
	return 1;
}

bool32
bindCurrentColorTarget(void)
{
	if(!context.frameOpen || context.commandList == nil ||
	   context.currentColorView.ptr == 0)
		return 0;
	if(context.currentRenderTargetCount != 1){
		context.commandList->OMSetRenderTargets(1, &context.currentColorView, FALSE,
			context.currentDepthView.ptr ? &context.currentDepthView : nil);
		context.currentRenderTargetCount = 1;
	}
	return 1;
}

bool32
bindCurrentWorldTargets(void)
{
	if(!context.frameOpen || context.commandList == nil ||
	   context.currentColorView.ptr == 0 || context.currentMotionView.ptr == 0)
		return 0;
	if(context.currentRenderTargetCount != 2){
		D3D12_CPU_DESCRIPTOR_HANDLE views[2] = {
			context.currentColorView, context.currentMotionView
		};
		context.commandList->OMSetRenderTargets(2, views, FALSE,
			context.currentDepthView.ptr ? &context.currentDepthView : nil);
		context.currentRenderTargetCount = 2;
	}
	return 1;
}

bool32
setStereoWideViewport(bool32 wide)
{
	if(!context.frameOpen || engine == nil || engine->currentCamera == nil)
		return 0;
	Raster *frameBuffer = engine->currentCamera->frameBuffer;
	if(frameBuffer == nil)
		return 0;
	Raster *parent = frameBuffer->parent ? frameBuffer->parent : frameBuffer;
	if(wide && frameBuffer == parent)
		return 0;
	D3D12_VIEWPORT viewport;
	viewport.TopLeftX = wide ? 0.0f : (float)frameBuffer->offsetX;
	viewport.TopLeftY = wide ? 0.0f : (float)frameBuffer->offsetY;
	viewport.Width = (float)(wide ? parent->width : frameBuffer->width);
	viewport.Height = (float)(wide ? parent->height : frameBuffer->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor = {
		(LONG)viewport.TopLeftX, (LONG)viewport.TopLeftY,
		(LONG)(viewport.TopLeftX + viewport.Width),
		(LONG)(viewport.TopLeftY + viewport.Height)
	};
	context.commandList->RSSetViewports(1, &viewport);
	context.commandList->RSSetScissorRects(1, &scissor);
	context.currentTargetWidth = wide ? parent->width : frameBuffer->width;
	context.currentTargetHeight = wide ? parent->height : frameBuffer->height;
	return 1;
}

static void
beginUpdate(Camera *camera)
{
	if(camera && camera->getFrame()){
		float view[16], projection[16];
		Matrix inverse;
		Matrix::invert(&inverse, camera->getFrame()->getLTM());
		view[0] = -inverse.right.x; view[1] = inverse.right.y;
		view[2] = inverse.right.z; view[3] = 0.0f;
		view[4] = -inverse.up.x; view[5] = inverse.up.y;
		view[6] = inverse.up.z; view[7] = 0.0f;
		view[8] = -inverse.at.x; view[9] = inverse.at.y;
		view[10] = inverse.at.z; view[11] = 0.0f;
		view[12] = -inverse.pos.x; view[13] = inverse.pos.y;
		view[14] = inverse.pos.z; view[15] = 1.0f;
		memcpy(&camera->devView, view, sizeof(view));

		float32 inverseWidth = 1.0f/camera->viewWindow.x;
		float32 inverseHeight = 1.0f/camera->viewWindow.y;
		float32 inverseDepth = 1.0f/(camera->farPlane-camera->nearPlane);
		memset(projection, 0, sizeof(projection));
		projection[0] = inverseWidth;
		projection[5] = inverseHeight;
		projection[8] = camera->viewOffset.x*inverseWidth;
		projection[9] = camera->viewOffset.y*inverseHeight;
		projection[12] = -projection[8];
		projection[13] = -projection[9];
		if(camera->projection == Camera::PERSPECTIVE){
			projection[10] = camera->farPlane*inverseDepth;
			projection[11] = 1.0f;
			projection[15] = 0.0f;
		}else{
			projection[10] = inverseDepth;
			projection[11] = 0.0f;
			projection[15] = 1.0f;
		}
		projection[14] = -camera->nearPlane*projection[10];
		memcpy(&camera->devProj, projection, sizeof(projection));
	}
	beginFrame(camera);
}

static void
clearCamera(Camera *camera, RGBA *color, uint32 mode)
{
	if(!beginFrame(camera))
		return;
	Raster *frameBuffer = camera ? camera->frameBuffer : nil;
	Raster *zBuffer = camera ? camera->zBuffer : nil;
	D3D12_RECT colorRect;
	D3D12_RECT depthRect;
	const D3D12_RECT *colorRects = nil;
	const D3D12_RECT *depthRects = nil;
	UINT colorRectCount = 0;
	UINT depthRectCount = 0;
	if(frameBuffer && frameBuffer != frameBuffer->parent){
		colorRect.left = frameBuffer->offsetX;
		colorRect.top = frameBuffer->offsetY;
		colorRect.right = colorRect.left + frameBuffer->width;
		colorRect.bottom = colorRect.top + frameBuffer->height;
		colorRects = &colorRect;
		colorRectCount = 1;
	}
	if(zBuffer && zBuffer != zBuffer->parent){
		depthRect.left = zBuffer->offsetX;
		depthRect.top = zBuffer->offsetY;
		depthRect.right = depthRect.left + zBuffer->width;
		depthRect.bottom = depthRect.top + zBuffer->height;
		depthRects = &depthRect;
		depthRectCount = 1;
	}
	if((mode & Camera::CLEARIMAGE) && color){
		const float clearColor[4] = {
			color->red / 255.0f,
			color->green / 255.0f,
			color->blue / 255.0f,
			color->alpha / 255.0f
		};
		context.commandList->ClearRenderTargetView(
			context.currentColorView, clearColor, colorRectCount, colorRects);
		if(context.currentMotionView.ptr){
			const float invalidMotion[4] = {
				OBJECT_MOTION_INVALID, OBJECT_MOTION_INVALID,
				OBJECT_MOTION_INVALID, OBJECT_MOTION_INVALID
			};
			context.commandList->ClearRenderTargetView(
				context.currentMotionView, invalidMotion,
				colorRectCount, colorRects);
		}
	}
	if((mode & Camera::CLEARZ) && context.currentDepthView.ptr)
		context.commandList->ClearDepthStencilView(
			context.currentDepthView,
			D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
			1.0f, 0, depthRectCount, depthRects);
}

static void
endUpdate(Camera*)
{
	// RenderWare can end several auxiliary cameras during one presented frame.
	// The D3D12 list therefore remains open until showRaster submits it.
}

static void
finishFrame(void)
{
	if(!context.frameOpen)
		return;
	endFixedFoveatedRendering();
	if(context.currentColorRaster)
		transitionRaster(context.currentColorRaster,
		                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	context.currentColorRaster = nil;
	context.currentColorResource = nil;
	context.currentColorView.ptr = 0;
	context.currentMotionResource = nil;
	context.currentMotionView.ptr = 0;
	context.currentRenderTargetCount = 0;
	context.currentTargetWidth = 0;
	context.currentTargetHeight = 0;

	if(context.backBufferRendering){
		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			context.backBuffers[context.frameIndex],
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PRESENT);
		context.commandList->ResourceBarrier(1, &barrier);
		context.backBufferRendering = 0;
	}
	endGpuTimestamp(GPU_STAGE_FRAME_TOTAL);
	collectGpuTimestamps();
	if(FAILED(context.commandList->Close())){
		context.frameOpen = 0;
		return;
	}
	ID3D12CommandList *lists[] = { context.commandList };
	context.queue->ExecuteCommandLists(1, lists);
	const UINT64 value = ++context.fenceValue;
	if(SUCCEEDED(context.queue->Signal(context.fence, value))){
		context.frameFenceValues[context.frameIndex] = value;
		// Associate standalone-upload resources with this newly signalled fence,
		// not the fence previously consumed by beginFrame.
		context.deferredReleases[context.frameIndex].insert(
			context.deferredReleases[context.frameIndex].end(),
			context.pendingSubmitReleases.begin(),
			context.pendingSubmitReleases.end());
		context.pendingSubmitReleases.clear();
		context.deferredSrvDescriptors[context.frameIndex].insert(
			context.deferredSrvDescriptors[context.frameIndex].end(),
			context.pendingSubmitSrvDescriptors.begin(),
			context.pendingSubmitSrvDescriptors.end());
		context.pendingSubmitSrvDescriptors.clear();
		context.deferredRtvDescriptors[context.frameIndex].insert(
			context.deferredRtvDescriptors[context.frameIndex].end(),
			context.pendingSubmitRtvDescriptors.begin(),
			context.pendingSubmitRtvDescriptors.end());
		context.pendingSubmitRtvDescriptors.clear();
		context.deferredDsvDescriptors[context.frameIndex].insert(
			context.deferredDsvDescriptors[context.frameIndex].end(),
			context.pendingSubmitDsvDescriptors.begin(),
			context.pendingSubmitDsvDescriptors.end());
		context.pendingSubmitDsvDescriptors.clear();
		context.deferredTextureAllocations[context.frameIndex].insert(
			context.deferredTextureAllocations[context.frameIndex].end(),
			context.pendingTextureAllocations.begin(),
			context.pendingTextureAllocations.end());
		context.pendingTextureAllocations.clear();
	}
	context.frameOpen = 0;
}

// Log the first Present failure and device-removal reason for TDR, driver,
// and out-of-memory diagnosis.
static void
logPresentFailure(HRESULT result)
{
	static bool logged;
	if(logged)
		return;
	logged = true;
	FILE *file = fopen("d3d12_device_removed.log", "w");
	if(file == nil)
		return;
	const HRESULT reason = context.device != nil ?
		context.device->GetDeviceRemovedReason() : S_OK;
	fprintf(file, "Present failed hr=%08lX deviceRemovedReason=%08lX\n",
		(unsigned long)result, (unsigned long)reason);
	fclose(file);
}

static void
showRaster(Raster*, uint32)
{
	if(!context.presentationReady)
		return;
	if(context.frameOpen)
		finishFrame();
	UINT presentedFrame = context.frameIndex;
	HRESULT presentResult = context.swapChain->Present(context.presentInterval, 0);
	if(FAILED(presentResult)){
		logPresentFailure(presentResult);
		return;
	}
	context.lastPresentedFrame = presentedFrame;
	context.hasPresentedFrame = 1;
	context.frameIndex = context.swapChain->GetCurrentBackBufferIndex();
}

static bool32
rasterRenderFast(Raster *source, int32 x, int32 y)
{
	if(!context.frameOpen || source == nil || source->parent == nil ||
	   source->parent->type != Raster::CAMERA)
		return 0;
	Raster *destination = Raster::getCurrentContext();
	if(destination)
		destination = destination->parent;
	ID3D12Resource *destinationResource = nil;
	if(destination == nil ||
	   !getRasterResource(destination, &destinationResource))
		return 0;
	if(x < 0 || y < 0 || x >= destination->width ||
	   y >= destination->height)
		return 0;
	if(!transitionRaster(destination, D3D12_RESOURCE_STATE_COPY_DEST))
		return 0;

	D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
		context.backBuffers[context.frameIndex],
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	context.commandList->ResourceBarrier(1, &toCopy);
	D3D12_TEXTURE_COPY_LOCATION dst;
	memset(&dst, 0, sizeof(dst));
	dst.pResource = destinationResource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src;
	memset(&src, 0, sizeof(src));
	src.pResource = context.backBuffers[context.frameIndex];
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_BOX box;
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = (UINT)((source->width < destination->width-x) ?
		source->width : destination->width-x);
	box.bottom = (UINT)((source->height < destination->height-y) ?
		source->height : destination->height-y);
	box.back = 1;
	context.commandList->CopyTextureRegion(
		&dst, (UINT)x, (UINT)y, 0, &src, &box);
	D3D12_RESOURCE_BARRIER toRender = transitionBarrier(
		context.backBuffers[context.frameIndex],
		D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	context.commandList->ResourceBarrier(1, &toRender);
	transitionRaster(destination,
	                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return 1;
}

static void
destroyCoreDevice(void)
{
	InterlockedExchange(&detailedProfilingEnabled, 0);
	memset(&gpuFrameProfile, 0, sizeof(gpuFrameProfile));
	memset(context.timestampWritten, 0, sizeof(context.timestampWritten));
	memset(context.timestampResolvedMask, 0,
	       sizeof(context.timestampResolvedMask));
	memset(context.timestampNextSegment, 0,
	       sizeof(context.timestampNextSegment));
	for(uint32 frame = 0; frame < FRAME_COUNT; frame++)
		for(uint32 stage = 0; stage < GPU_STAGE_COUNT; stage++)
			context.timestampActiveQuery[frame][stage] = UINT32_MAX;
	context.timestampFrequency = 0;
	if(context.queue && context.fence && context.fenceEvent)
		waitForGpu();
	releasePendingTextureUploads();
	releasePendingSubmitObjects();
	for(uint32 i = 0; i < FRAME_COUNT; i++)
		releaseDeferredFrame(i);
	for(size_t i = 0; i < context.pendingTextureAllocations.size(); i++)
		freeTextureAllocationNow(context.pendingTextureAllocations[i]);
	context.pendingTextureAllocations.clear();
	releaseCom(context.fixedFoveatedImage);
	context.fixedFoveatedImageWidth = 0;
	context.fixedFoveatedImageHeight = 0;
	context.fixedFoveatedTargetWidth = 0;
	context.fixedFoveatedTargetHeight = 0;
	context.fixedFoveatedImageProfile = FIXED_FOVEATED_OFF;
	context.fixedFoveatedActive = 0;
	context.fixedFoveatedCreationFailed = 0;
	destroyFrameResources();
	stopSpareHeapThread();
	destroyUploadPool();
	for(uint32 i = 0; i < FRAME_COUNT; i++)
		releaseCom(context.timestampReadback[i]);
	releaseCom(context.timestampHeap);
	for(size_t i = 0; i < context.textureHeapPages.size(); i++)
		releaseCom(context.textureHeapPages[i].heap);
	context.textureHeapPages.clear();
	// Descriptor indices parked between frames are meaningless across a
	// device recreation; never let them leak into the next device's lists.
	context.pendingSubmitSrvDescriptors.clear();
	context.pendingSubmitRtvDescriptors.clear();
	context.pendingSubmitDsvDescriptors.clear();

	if(context.fenceEvent){
		CloseHandle(context.fenceEvent);
		context.fenceEvent = nil;
	}
	releaseCom(context.fence);
	releaseCom(context.srvHeap);
	releaseCom(context.samplerHeap);
	releaseCom(context.dsvHeap);
	releaseCom(context.queue);
	releaseCom(context.device);
	releaseCom(context.adapter);
	releaseCom(context.factory6);
	releaseCom(context.factory);
	context.fenceValue = 0;
	context.srvDescriptorSize = 0;
	context.samplerDescriptorSize = 0;
	context.dsvDescriptorSize = 0;
	context.fixedFoveatedTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
	context.fixedFoveatedTileSize = 0;
	context.fixedFoveatedAdditionalRates = 0;
	context.nextSrvDescriptor = 0;
	context.nextSamplerDescriptor = 0;
	context.nextDsvDescriptor = 0;
	context.freeSrvDescriptors.clear();
	context.freeRtvDescriptors.clear();
	context.freeDsvDescriptors.clear();
	for(uint32 i = 0; i < FRAME_COUNT; i++){
		context.deferredSrvDescriptors[i].clear();
		context.deferredRtvDescriptors[i].clear();
		context.deferredDsvDescriptors[i].clear();
		context.deferredTextureAllocations[i].clear();
	}
	memset(context.samplerCache, 0, sizeof(context.samplerCache));
	context.initialized = 0;
}

static bool32
createCoreDevice(void)
{
	if(context.initialized)
		return 1;

	UINT factoryFlags = 0;
#ifdef DEBUG
	ID3D12Debug *debug = nil;
	if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))){
		debug->EnableDebugLayer();
		factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		releaseCom(debug);
	}
#endif

	if(FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&context.factory))))
		return 0;
	context.factory->QueryInterface(IID_PPV_ARGS(&context.factory6));

	if(!selectAdapter()){
		destroyCoreDevice();
		return 0;
	}
	// Streamline requires slSetD3DDevice before any hooked device method (most
	// importantly CreateCommandQueue) is invoked.  Waiting until the OpenXR
	// session starts is already too late and leaves the temporal plugin only
	// partially initialized.
	if(deviceCreatedCallback)
		deviceCreatedCallback();
	D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6;
	memset(&options6, 0, sizeof(options6));
	if(SUCCEEDED(context.device->CheckFeatureSupport(
	       D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))){
		context.fixedFoveatedTier = options6.VariableShadingRateTier;
		context.fixedFoveatedTileSize = options6.ShadingRateImageTileSize;
		context.fixedFoveatedAdditionalRates =
			options6.AdditionalShadingRatesSupported != FALSE;
	}
	D3D12_COMMAND_QUEUE_DESC queueDesc;
	memset(&queueDesc, 0, sizeof(queueDesc));
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	if(FAILED(context.device->CreateCommandQueue(&queueDesc,
	                                            IID_PPV_ARGS(&context.queue))) ||
	   FAILED(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
	                                     IID_PPV_ARGS(&context.fence)))){
		destroyCoreDevice();
		return 0;
	}

	context.fenceEvent = CreateEventW(nil, FALSE, FALSE, nil);
	if(context.fenceEvent == nil){
		destroyCoreDevice();
		return 0;
	}
	D3D12_DESCRIPTOR_HEAP_DESC srvDesc;
	memset(&srvDesc, 0, sizeof(srvDesc));
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = MAX_SHADER_RESOURCE_DESCRIPTORS;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc;
	memset(&dsvDesc, 0, sizeof(dsvDesc));
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = MAX_DEPTH_DESCRIPTORS;
	D3D12_DESCRIPTOR_HEAP_DESC samplerDesc;
	memset(&samplerDesc, 0, sizeof(samplerDesc));
	samplerDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	samplerDesc.NumDescriptors = MAX_SAMPLER_DESCRIPTORS;
	samplerDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if(FAILED(context.device->CreateDescriptorHeap(
	       &srvDesc, IID_PPV_ARGS(&context.srvHeap))) ||
	   FAILED(context.device->CreateDescriptorHeap(
	       &samplerDesc, IID_PPV_ARGS(&context.samplerHeap))) ||
	   FAILED(context.device->CreateDescriptorHeap(
	       &dsvDesc, IID_PPV_ARGS(&context.dsvHeap)))){
		destroyCoreDevice();
		return 0;
	}
	context.srvDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	context.samplerDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	context.dsvDescriptorSize = context.device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	if(!createFrameResources()){
		destroyCoreDevice();
		return 0;
	}
	context.initialized = 1;
	return 1;
}

bool32
isInitialized(void)
{
	return context.initialized;
}

bool32
isPresentationReady(void)
{
	return context.presentationReady;
}

const char*
getAdapterName(void)
{
	return context.adapterName[0] ? context.adapterName : "D3D12 not initialized";
}

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	VideoMode *mode;
	switch(req){
	case DEVICEOPEN:
		context.window = (HWND)((EngineOpenParams*)arg)->window;
		context.width = 1280;
		context.height = 720;
		context.desktopWidth = GetSystemMetrics(SM_CXSCREEN);
		context.desktopHeight = GetSystemMetrics(SM_CYSCREEN);
		DEVMODE displayMode;
		memset(&displayMode, 0, sizeof(displayMode));
		displayMode.dmSize = sizeof(displayMode);
		if(EnumDisplaySettings(nil, ENUM_CURRENT_SETTINGS, &displayMode)){
			context.desktopWidth = (int32)displayMode.dmPelsWidth;
			context.desktopHeight = (int32)displayMode.dmPelsHeight;
		}
		if(context.desktopWidth <= 0)
			context.desktopWidth = context.width;
		if(context.desktopHeight <= 0)
			context.desktopHeight = context.height;
		context.currentVideoMode = 0;
		context.presentInterval = 1;
		if(context.window){
			RECT rect;
			if(GetClientRect(context.window, &rect)){
				context.width = rect.right - rect.left;
				context.height = rect.bottom - rect.top;
			}
		}
		return 1;
	case DEVICECLOSE:
		context.window = nil;
		return 1;
	case DEVICEINIT:
		// psSelectDevice changes the HWND after DEVICEOPEN. Read the final client
		// area here so the swap chain matches the RenderWare camera pixel-for-pixel.
		if(context.window){
			RECT rect;
			if(GetClientRect(context.window, &rect) &&
			   rect.right > rect.left && rect.bottom > rect.top){
				context.width = rect.right - rect.left;
				context.height = rect.bottom - rect.top;
			}
		}
		return createCoreDevice();
	case DEVICETERM:
		destroyCoreDevice();
		return 1;
	case DEVICEFINALIZE:
		return context.initialized;
	case DEVICEGETNUMSUBSYSTEMS:
		// RenderWare selects a subsystem before DEVICEINIT. D3D12 performs
		// the real high-performance adapter selection during initialization,
		// but must expose that selectable logical subsystem beforehand.
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return n == 0;
	case DEVICEGETSUBSSYSTEMINFO:
		if(arg == nil || n != 0)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, getAdapterName(),
		        sizeof(SubSystemInfo::name));
		((SubSystemInfo*)arg)->name[sizeof(SubSystemInfo::name)-1] = '\0';
		return 1;
	case DEVICEGETNUMVIDEOMODES:
		// reVC's device selector requires both a windowed mode and at least one
		// exclusive-marked mode. The latter is implemented by the Win32 skeleton
		// as a borderless desktop window; DXGI exclusive fullscreen is deliberately
		// not used because flip-model swap chains work more reliably without it.
		return 2;
	case DEVICEGETCURRENTVIDEOMODE:
		return context.currentVideoMode;
	case DEVICESETVIDEOMODE:
		if(n < 0 || n >= 2)
			return 0;
		context.currentVideoMode = n;
		return 1;
	case DEVICEGETVIDEOMODEINFO:
		if(arg == nil || n < 0 || n >= 2)
			return 0;
		mode = (VideoMode*)arg;
		mode->width = n == 0 ? context.width : context.desktopWidth;
		mode->height = n == 0 ? context.height : context.desktopHeight;
		mode->depth = 32;
		mode->flags = n == 1 ? VIDEOMODEEXCLUSIVE : 0;
		return 1;
	case DEVICEGETMAXMULTISAMPLINGLEVELS:
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		return n <= 1;
	}
	return 0;
}

#else

bool32 isInitialized(void) { return 0; }
bool32 isPresentationReady(void) { return 0; }
const char *getAdapterName(void) { return "D3D12 backend not compiled"; }
bool32 waitForGpu(void) { return 0; }
static int deviceSystem(DeviceReq req, void *arg, int32 n)
{
	return null::deviceSystem(req, arg, n);
}

#endif

// Camera presentation is live. Raster allocation, depth and draw pipelines are
// introduced in the next slices; their device entries remain explicitly null.
Device renderdevice = {
	0.0f, 1.0f,
#ifdef RW_D3D12
	d3d12::beginUpdate,
	d3d12::endUpdate,
	d3d12::clearCamera,
	d3d12::showRaster,
#else
	null::beginUpdate,
	null::endUpdate,
	null::clearCamera,
	null::showRaster,
#endif
#ifdef RW_D3D12
	d3d12::rasterRenderFast,
#else
	null::rasterRenderFast,
#endif
#ifdef RW_D3D12
	d3d12::setRenderState,
	d3d12::getRenderState,
	d3d12::im2DRenderLine,
	d3d12::im2DRenderTriangle,
	d3d12::im2DRenderPrimitive,
	d3d12::im2DRenderIndexedPrimitive,
#else
	null::setRenderState,
	null::getRenderState,
	null::im2DRenderLine,
	null::im2DRenderTriangle,
	null::im2DRenderPrimitive,
	null::im2DRenderIndexedPrimitive,
#endif
#ifdef RW_D3D12
	d3d12::im3DTransform,
	d3d12::im3DRenderPrimitive,
	d3d12::im3DRenderIndexedPrimitive,
	d3d12::im3DEnd,
#else
	null::im3DTransform,
	null::im3DRenderPrimitive,
	null::im3DRenderIndexedPrimitive,
	null::im3DEnd,
#endif
	d3d12::deviceSystem
};

}
}
