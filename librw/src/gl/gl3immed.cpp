#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#ifdef RW_OPENGL
#include "rwgl3.h"
#include "rwgl3impl.h"
#include "rwgl3shader.h"

namespace rw {
namespace gl3 {

uint32 im2DVbo, im2DIbo;
#ifdef RW_GL_USE_VAOS
uint32 im2DVao;
#endif

Shader *im2dOverrideShader;

static int32 u_xform;

#define STARTINDICES 10000
#define STARTVERTICES 10000

static int32 im2DVertexCapacity = STARTVERTICES;
static int32 im2DIndexCapacity = STARTINDICES;
static int32 im2DVertexCursor;
static int32 im2DIndexCursor;

static int32
uploadStreamRange(uint32 target, uint32 buffer, const void *data, int32 count,
	int32 elementSize, int32 &capacity, int32 &cursor)
{
	if(data == nil || count <= 0)
		return -1;
	glBindBuffer(target, buffer);
	if(count > capacity){
		while(capacity < count)
			capacity *= 2;
		glBufferData(target, capacity*elementSize, nil, GL_STREAM_DRAW);
		cursor = 0;
	}else if(cursor + count > capacity){
		// The old storage can still be consumed by the GPU. Orphan it only
		// when the ring wraps instead of once for every immediate draw.
		glBufferData(target, capacity*elementSize, nil, GL_STREAM_DRAW);
		cursor = 0;
	}
	const int32 first = cursor;
	glBufferSubData(target, first*elementSize, count*elementSize, data);
	cursor += count;
	return first;
}

static Shader *im2dShader;
static AttribDesc im2dattribDesc[3] = {
	{ ATTRIB_POS,        GL_FLOAT,         GL_FALSE, 4,
		sizeof(Im2DVertex), 0 },
	{ ATTRIB_COLOR,      GL_UNSIGNED_BYTE, GL_TRUE,  4,
		sizeof(Im2DVertex), offsetof(Im2DVertex, r) },
	{ ATTRIB_TEXCOORDS0, GL_FLOAT,         GL_FALSE, 2,
		sizeof(Im2DVertex), offsetof(Im2DVertex, u) },
};

static int primTypeMap[] = {
	GL_POINTS,	// invalid
	GL_LINES,
	GL_LINE_STRIP,
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_TRIANGLE_FAN,
	GL_POINTS
};

void
openIm2D(void)
{
	u_xform = registerUniform("u_xform");

#include "shaders/im2d_gl.inc"
#include "shaders/simple_fs_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, im2d_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, simple_frag_src, nil };
	im2dShader = Shader::create(vs, fs);
	assert(im2dShader);

	glGenBuffers(1, &im2DIbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, im2DIbo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, STARTINDICES*2, nil, GL_STREAM_DRAW);
	im2DIndexCapacity = STARTINDICES;
	im2DIndexCursor = 0;

	glGenBuffers(1, &im2DVbo);
	glBindBuffer(GL_ARRAY_BUFFER, im2DVbo);
	glBufferData(GL_ARRAY_BUFFER, STARTVERTICES*sizeof(Im2DVertex), nil, GL_STREAM_DRAW);
	im2DVertexCapacity = STARTVERTICES;
	im2DVertexCursor = 0;

#ifdef RW_GL_USE_VAOS
	glGenVertexArrays(1, &im2DVao);
	glBindVertexArray(im2DVao);
	setAttribPointers(im2dattribDesc, 3);
#endif
}

void
closeIm2D(void)
{
	glDeleteBuffers(1, &im2DIbo);
	glDeleteBuffers(1, &im2DVbo);
#ifdef RW_GL_USE_VAOS
	glDeleteVertexArrays(1, &im2DVao);
#endif
	im2dShader->destroy();
	im2dShader = nil;
}

static Im2DVertex tmpprimbuf[3];

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	im2DRenderPrimitive(PRIMTYPELINELIST, tmpprimbuf, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	tmpprimbuf[2] = verts[vert3];
	im2DRenderPrimitive(PRIMTYPETRILIST, tmpprimbuf, 3);
}

void
im2DSetXform(void)
{
	GLfloat xform[4];
	Camera *cam;
	cam = (Camera*)engine->currentCamera;
	xform[0] = 2.0f/cam->frameBuffer->width;
	xform[1] = -2.0f/cam->frameBuffer->height;
	xform[2] = -1.0f;
	xform[3] = 1.0f;
	glUniform4fv(currentShader->uniformLocations[u_xform], 1, xform);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
#ifdef RW_GL_USE_VAOS
	glBindVertexArray(im2DVao);
#endif

	const int32 firstVertex = uploadStreamRange(GL_ARRAY_BUFFER, im2DVbo, vertices,
		numVertices, sizeof(Im2DVertex), im2DVertexCapacity, im2DVertexCursor);
	if(firstVertex < 0)
		return;

	if(im2dOverrideShader)
		im2dOverrideShader->use();
	else
		im2dShader->use();
#ifndef RW_GL_USE_VAOS
	setAttribPointers(im2dattribDesc, 3);
#endif

	im2DSetXform();

	flushCache();
	glDrawArrays(primTypeMap[primType], firstVertex, numVertices);
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(im2dattribDesc, 3);
#endif
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
#ifdef RW_GL_USE_VAOS
	glBindVertexArray(im2DVao);
#endif

	const int32 firstIndex = uploadStreamRange(GL_ELEMENT_ARRAY_BUFFER, im2DIbo, indices,
		numIndices, sizeof(uint16), im2DIndexCapacity, im2DIndexCursor);
	const int32 firstVertex = uploadStreamRange(GL_ARRAY_BUFFER, im2DVbo, vertices,
		numVertices, sizeof(Im2DVertex), im2DVertexCapacity, im2DVertexCursor);
	if(firstIndex < 0 || firstVertex < 0)
		return;

	if(im2dOverrideShader)
		im2dOverrideShader->use();
	else
		im2dShader->use();
#ifndef RW_GL_USE_VAOS
	setAttribPointers(im2dattribDesc, 3);
#endif

	im2DSetXform();

	flushCache();
	glDrawElementsBaseVertex(primTypeMap[primType], numIndices, GL_UNSIGNED_SHORT,
		(void*)(uintptr)(firstIndex*sizeof(uint16)), firstVertex);
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(im2dattribDesc, 3);
#endif
}


// Im3D


static Shader *im3dShader;
static AttribDesc im3dattribDesc[3] = {
	{ ATTRIB_POS,        GL_FLOAT,         GL_FALSE, 3,
		sizeof(Im3DVertex), 0 },
	{ ATTRIB_COLOR,      GL_UNSIGNED_BYTE, GL_TRUE,  4,
		sizeof(Im3DVertex), offsetof(Im3DVertex, r) },
	{ ATTRIB_TEXCOORDS0, GL_FLOAT,         GL_FALSE, 2,
		sizeof(Im3DVertex), offsetof(Im3DVertex, u) },
};
static uint32 im3DVbo, im3DIbo;
#ifdef RW_GL_USE_VAOS
static uint32 im3DVao;
#endif
static int32 num3DVertices;	// not actually needed here
static int32 im3DVertexCapacity = STARTVERTICES;
static int32 im3DIndexCapacity = STARTINDICES;
static int32 im3DVertexCursor;
static int32 im3DIndexCursor;
static int32 im3DVertexBase;

void
openIm3D(void)
{
#include "shaders/im3d_gl.inc"
#include "shaders/simple_fs_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, im3d_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, simple_frag_src, nil };
	im3dShader = Shader::create(vs, fs);
	assert(im3dShader);

	glGenBuffers(1, &im3DIbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, im3DIbo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, STARTINDICES*2, nil, GL_STREAM_DRAW);
	im3DIndexCapacity = STARTINDICES;
	im3DIndexCursor = 0;

	glGenBuffers(1, &im3DVbo);
	glBindBuffer(GL_ARRAY_BUFFER, im3DVbo);
	glBufferData(GL_ARRAY_BUFFER, STARTVERTICES*sizeof(Im3DVertex), nil, GL_STREAM_DRAW);
	im3DVertexCapacity = STARTVERTICES;
	im3DVertexCursor = 0;
	im3DVertexBase = 0;

#ifdef RW_GL_USE_VAOS
	glGenVertexArrays(1, &im3DVao);
	glBindVertexArray(im3DVao);
	setAttribPointers(im3dattribDesc, 3);
#endif
}

void
closeIm3D(void)
{
	glDeleteBuffers(1, &im3DIbo);
	glDeleteBuffers(1, &im3DVbo);
#ifdef RW_GL_USE_VAOS
	glDeleteVertexArrays(1, &im3DVao);
#endif
	im3dShader->destroy();
	im3dShader = nil;
}

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	if(world == nil){
		static Matrix ident;
		ident.setIdentity();
		world = &ident;
	}
	setWorldMatrix(world);
	im3dShader->use();

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

#ifdef RW_GL_USE_VAOS
	glBindVertexArray(im3DVao);
#endif

	im3DVertexBase = uploadStreamRange(GL_ARRAY_BUFFER, im3DVbo, vertices,
		numVertices, sizeof(Im3DVertex), im3DVertexCapacity, im3DVertexCursor);
	if(im3DVertexBase < 0){
		num3DVertices = 0;
		return;
	}
#ifndef RW_GL_USE_VAOS
	setAttribPointers(im3dattribDesc, 3);
#endif
	num3DVertices = numVertices;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, im3DIbo);

	flushCache();
	glDrawArrays(primTypeMap[primType], im3DVertexBase, num3DVertices);
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	const int32 firstIndex = uploadStreamRange(GL_ELEMENT_ARRAY_BUFFER, im3DIbo, indices,
		numIndices, sizeof(uint16), im3DIndexCapacity, im3DIndexCursor);
	if(firstIndex < 0 || im3DVertexBase < 0)
		return;

	flushCache();
	glDrawElementsBaseVertex(primTypeMap[primType], numIndices, GL_UNSIGNED_SHORT,
		(void*)(uintptr)(firstIndex*sizeof(uint16)), im3DVertexBase);
}

void
im3DEnd(void)
{
#ifndef RW_GL_USE_VAOS
	disableAttribPointers(im3dattribDesc, 3);
#endif
}

}
}

#endif
