#include "common.h"

#ifdef RW_D3D12
#include "custompipes.h"

namespace rw {
namespace d3d12 {
bool32 renderAtomicFirstPass(Atomic *atomic);
void renderAtomicBlendPass(Atomic *atomic, uint8 fadeAlpha);
}
}

#ifdef EXTENDED_PIPELINES

// Stage 5 compatibility bridge. The new backend already owns geometry,
// textures and draw submission; the Neo material variants are introduced
// after the first complete D3D12 frame. Until then every custom attachment
// deliberately routes through the real D3D12 default world pipeline.
namespace CustomPipes {

static rw::ObjPipeline*
defaultPipeline(void)
{
	return rw::engine->driver[rw::PLATFORM_D3D12]->defaultPipeline;
}

void CreateVehiclePipe(void) { vehiclePipe = defaultPipeline(); }
void DestroyVehiclePipe(void) { vehiclePipe = nil; }
void CreateWorldPipe(void) { worldPipe = defaultPipeline(); }
void DestroyWorldPipe(void) { worldPipe = nil; }
void CreateGlossPipe(void) { glossPipe = defaultPipeline(); }
void DestroyGlossPipe(void) { glossPipe = nil; }
void CreateRimLightPipes(void)
{
	rimPipe = defaultPipeline();
	rimSkinPipe = defaultPipeline();
}
void DestroyRimLightPipes(void)
{
	rimPipe = nil;
	rimSkinPipe = nil;
}

}
#endif

#ifdef NEW_RENDERER
namespace WorldRender {

enum { MAX_BLEND_ATOMICS = 2000 };

struct BlendAtomic
{
	RpAtomic *atomic;
	rw::uint8 fadeAlpha;
};

int numBlendInsts[3];
static BlendAtomic blendAtomics[3][MAX_BLEND_ATOMICS];

static void
QueueBlendAtomic(RpAtomic *atomic, int pass, int fadeAlpha)
{
	if(atomic == nil || pass < 0 || pass >= (int)nelem(numBlendInsts))
		return;
	int &count = numBlendInsts[pass];
	if(count >= MAX_BLEND_ATOMICS)
		return;
	blendAtomics[pass][count].atomic = atomic;
	blendAtomics[pass][count].fadeAlpha = (rw::uint8)fadeAlpha;
	count++;
}

void
AtomicFirstPass(RpAtomic *atomic, int pass)
{
	if(atomic && rw::d3d12::renderAtomicFirstPass(atomic))
		QueueBlendAtomic(atomic, pass, 255);
}

void
AtomicFullyTransparent(RpAtomic *atomic, int pass, int fadeAlpha)
{
	QueueBlendAtomic(atomic, pass, fadeAlpha);
}

void
RenderBlendPass(int pass)
{
	if(pass < 0 || pass >= (int)nelem(numBlendInsts))
		return;
	for(int i = 0; i < numBlendInsts[pass]; i++){
		BlendAtomic &entry = blendAtomics[pass][i];
		if(entry.atomic)
			rw::d3d12::renderAtomicBlendPass(entry.atomic, entry.fadeAlpha);
	}
}

}
#endif

#endif
