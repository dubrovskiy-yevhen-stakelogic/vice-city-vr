#pragma once

class CEntity;

// The game submits semantic draw commands here instead of invoking the
// RenderWare-backed CEntity::Render() implementation directly.  The current
// backend executes commands immediately to preserve the original ordering and
// render state.  A modern backend can later consume the same command boundary.
namespace GameRender
{
enum eEntityDrawRole
{
	ENTITY_DRAW_WORLD,
	ENTITY_DRAW_VEHICLE_OCCUPANT
};

struct EntityDrawCommand
{
	CEntity *entity;
	eEntityDrawRole role;
};

enum eObjectDrawRole
{
	OBJECT_DRAW_ENTITY,
	OBJECT_DRAW_ATTACHMENT,
	OBJECT_DRAW_VR_WEAPON,
	OBJECT_DRAW_PLAYER_PREVIEW,
	OBJECT_DRAW_SHADOW,
	OBJECT_DRAW_SPECIAL_FX,
	OBJECT_DRAW_WATER
};

struct AtomicDrawCommand
{
	RpAtomic *atomic;
	eObjectDrawRole role;
};

struct ClumpDrawCommand
{
	RpClump *clump;
	eObjectDrawRole role;
};

enum eImmediate3DDrawRole
{
	IMMEDIATE3D_DRAW_WORLD,
	IMMEDIATE3D_DRAW_EFFECT,
	IMMEDIATE3D_DRAW_WATER,
	IMMEDIATE3D_DRAW_GLASS,
	IMMEDIATE3D_DRAW_WEATHER,
	IMMEDIATE3D_DRAW_VEHICLE,
	IMMEDIATE3D_DRAW_VR_HAND,
	IMMEDIATE3D_DRAW_VR_LASER,
	IMMEDIATE3D_DRAW_LINES,
	IMMEDIATE3D_DRAW_REFLECTIVE_WATER
};

struct Immediate3DBeginCommand
{
	RwIm3DVertex *vertices;
	uint32 vertexCount;
	RwMatrix *transform;
	uint32 flags;
	eImmediate3DDrawRole role;
};

struct Immediate3DIndexedCommand
{
	RwPrimitiveType primitiveType;
	RwImVertexIndex *indices;
	int32 indexCount;
};

struct Immediate3DLineCommand
{
	int32 firstVertex;
	int32 secondVertex;
};

enum eImmediate2DDrawRole
{
	IMMEDIATE2D_DRAW_SPRITE,
	IMMEDIATE2D_DRAW_RADAR,
	IMMEDIATE2D_DRAW_POST_FX,
	IMMEDIATE2D_DRAW_DEBUG,
	IMMEDIATE2D_DRAW_SHADOW
};

struct Immediate2DCommand
{
	RwPrimitiveType primitiveType;
	RwIm2DVertex *vertices;
	int32 vertexCount;
	eImmediate2DDrawRole role;
};

struct Immediate2DIndexedCommand
{
	RwPrimitiveType primitiveType;
	RwIm2DVertex *vertices;
	int32 vertexCount;
	RwImVertexIndex *indices;
	int32 indexCount;
	eImmediate2DDrawRole role;
};

class IBackend
{
public:
	virtual ~IBackend() {}
	virtual void DrawEntity(const EntityDrawCommand &command) = 0;
	virtual void DrawAtomic(const AtomicDrawCommand &command) = 0;
	virtual void DrawClump(const ClumpDrawCommand &command) = 0;
	virtual bool BeginImmediate3D(const Immediate3DBeginCommand &command) = 0;
	virtual bool DrawImmediate3DIndexed(const Immediate3DIndexedCommand &command) = 0;
	virtual bool DrawImmediate3DLine(const Immediate3DLineCommand &command) = 0;
	virtual void EndImmediate3D(void) = 0;
	virtual bool DrawImmediate2D(const Immediate2DCommand &command) = 0;
	virtual bool DrawImmediate2DIndexed(const Immediate2DIndexedCommand &command) = 0;
};

// Passing nil restores the built-in RenderWare compatibility backend.
void SetBackend(IBackend *backend);
IBackend *GetBackend(void);
void SubmitEntity(CEntity *entity, eEntityDrawRole role = ENTITY_DRAW_WORLD);
void SubmitAtomic(RpAtomic *atomic, eObjectDrawRole role = OBJECT_DRAW_ENTITY);
void SubmitClump(RpClump *clump, eObjectDrawRole role = OBJECT_DRAW_ENTITY);
bool BeginImmediate3D(RwIm3DVertex *vertices, uint32 vertexCount, RwMatrix *transform,
	uint32 flags, eImmediate3DDrawRole role = IMMEDIATE3D_DRAW_WORLD);
bool SubmitImmediate3DIndexed(RwPrimitiveType primitiveType, RwImVertexIndex *indices, int32 indexCount);
bool SubmitImmediate3DLine(int32 firstVertex, int32 secondVertex);
void EndImmediate3D(void);
bool SubmitImmediate2D(RwPrimitiveType primitiveType, RwIm2DVertex *vertices, int32 vertexCount,
	eImmediate2DDrawRole role = IMMEDIATE2D_DRAW_SPRITE);
bool SubmitImmediate2DIndexed(RwPrimitiveType primitiveType, RwIm2DVertex *vertices, int32 vertexCount,
	RwImVertexIndex *indices, int32 indexCount,
	eImmediate2DDrawRole role = IMMEDIATE2D_DRAW_SPRITE);
}
