#include "common.h"

#include "Entity.h"
#include "GameRenderCommand.h"

#ifdef RW_D3D12
namespace rw { namespace d3d12 {
void setImmediate2DStrictDepth(bool32 enabled);
void setIm3DWater(bool32 water);
} }
#endif

namespace GameRender
{
namespace
{
class RenderWareBackend : public IBackend
{
public:
	void DrawEntity(const EntityDrawCommand &command)
	{
		if(command.entity == nil || command.entity->m_rwObject == nil)
			return;
		command.entity->Render();
	}

	void DrawAtomic(const AtomicDrawCommand &command)
	{
		if(command.atomic){
#ifdef RW_D3D12
			rw::d3d12::setIm3DWater(
				command.role == OBJECT_DRAW_WATER);
#endif
			RpAtomicRender(command.atomic);
#ifdef RW_D3D12
			rw::d3d12::setIm3DWater(0);
#endif
		}
	}

	void DrawClump(const ClumpDrawCommand &command)
	{
		if(command.clump)
			RpClumpRender(command.clump);
	}

	bool BeginImmediate3D(const Immediate3DBeginCommand &command)
	{
#ifdef RW_D3D12
		rw::d3d12::setIm3DWater(
			command.role == IMMEDIATE3D_DRAW_REFLECTIVE_WATER);
#endif
		const bool began = RwIm3DTransform(command.vertices,
			command.vertexCount, command.transform, command.flags) != nil;
#ifdef RW_D3D12
		if(!began)
			rw::d3d12::setIm3DWater(0);
#endif
		return began;
	}

	bool DrawImmediate3DIndexed(const Immediate3DIndexedCommand &command)
	{
		return RwIm3DRenderIndexedPrimitive(command.primitiveType,
			command.indices, command.indexCount) != FALSE;
	}

	bool DrawImmediate3DLine(const Immediate3DLineCommand &command)
	{
		return RwIm3DRenderLine(command.firstVertex, command.secondVertex) != FALSE;
	}

	void EndImmediate3D(void)
	{
		RwIm3DEnd();
#ifdef RW_D3D12
		rw::d3d12::setIm3DWater(0);
#endif
	}

	bool DrawImmediate2D(const Immediate2DCommand &command)
	{
#ifdef RW_D3D12
		rw::d3d12::setImmediate2DStrictDepth(
			command.role == IMMEDIATE2D_DRAW_RADAR);
#endif
		return RwIm2DRenderPrimitive(command.primitiveType, command.vertices,
			command.vertexCount) != FALSE;
	}

	bool DrawImmediate2DIndexed(const Immediate2DIndexedCommand &command)
	{
#ifdef RW_D3D12
		rw::d3d12::setImmediate2DStrictDepth(
			command.role == IMMEDIATE2D_DRAW_RADAR);
#endif
		return RwIm2DRenderIndexedPrimitive(command.primitiveType, command.vertices,
			command.vertexCount, command.indices, command.indexCount) != FALSE;
	}
};

RenderWareBackend gRenderWareBackend;
IBackend *gBackend = &gRenderWareBackend;
}

void
SetBackend(IBackend *backend)
{
	gBackend = backend ? backend : &gRenderWareBackend;
}

IBackend *
GetBackend(void)
{
	return gBackend;
}

void
SubmitEntity(CEntity *entity, eEntityDrawRole role)
{
	EntityDrawCommand command = { entity, role };
	gBackend->DrawEntity(command);
}

void
SubmitAtomic(RpAtomic *atomic, eObjectDrawRole role)
{
	AtomicDrawCommand command = { atomic, role };
	gBackend->DrawAtomic(command);
}

void
SubmitClump(RpClump *clump, eObjectDrawRole role)
{
	ClumpDrawCommand command = { clump, role };
	gBackend->DrawClump(command);
}

bool
BeginImmediate3D(RwIm3DVertex *vertices, uint32 vertexCount, RwMatrix *transform,
	uint32 flags, eImmediate3DDrawRole role)
{
	Immediate3DBeginCommand command = { vertices, vertexCount, transform, flags, role };
	return gBackend->BeginImmediate3D(command);
}

bool
SubmitImmediate3DIndexed(RwPrimitiveType primitiveType, RwImVertexIndex *indices, int32 indexCount)
{
	Immediate3DIndexedCommand command = { primitiveType, indices, indexCount };
	return gBackend->DrawImmediate3DIndexed(command);
}

bool
SubmitImmediate3DLine(int32 firstVertex, int32 secondVertex)
{
	Immediate3DLineCommand command = { firstVertex, secondVertex };
	return gBackend->DrawImmediate3DLine(command);
}

void
EndImmediate3D(void)
{
	gBackend->EndImmediate3D();
}

bool
SubmitImmediate2D(RwPrimitiveType primitiveType, RwIm2DVertex *vertices, int32 vertexCount,
	eImmediate2DDrawRole role)
{
	Immediate2DCommand command = { primitiveType, vertices, vertexCount, role };
	return gBackend->DrawImmediate2D(command);
}

bool
SubmitImmediate2DIndexed(RwPrimitiveType primitiveType, RwIm2DVertex *vertices, int32 vertexCount,
	RwImVertexIndex *indices, int32 indexCount, eImmediate2DDrawRole role)
{
	Immediate2DIndexedCommand command = {
		primitiveType, vertices, vertexCount, indices, indexCount, role
	};
	return gBackend->DrawImmediate2DIndexed(command);
}
}
