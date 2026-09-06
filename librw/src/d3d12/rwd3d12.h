#pragma once

namespace rw {

#ifdef RW_D3D12
struct EngineOpenParams
{
	void *window;
};
#endif

namespace d3d12 {

void registerPlatformPlugins(void);
void registerNativeRaster(void);
extern int32 nativeRasterOffset;

// Selects the strict depth comparison used only by the radar tile pass.
// The default Im2D path remains LESS_EQUAL for fonts, icons and menus.
void setImmediate2DStrictDepth(bool32 enabled);

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode);
void rasterUnlock(Raster *raster, int32 level);
int32 rasterNumLevels(Raster *raster);
bool32 rasterHasGeneratedMips(Raster *raster);
bool32 imageFindRasterFormat(Image *image, int32 type,
                             int32 *width, int32 *height,
                             int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);
void setRasterHasAlpha(Raster *raster, bool32 hasAlpha);
bool32 allocateDXT(Raster *raster, int32 dxt, int32 numLevels,
                   bool32 hasAlpha);

ObjPipeline *makeDefaultPipeline(void);
void shutdownDefaultPipeline(void);
void *destroyNativeData(void *object, int32 offset, int32 size);

struct Im3DVertex
{
	V3d position;
	uint32 color;
	float32 u, v;

	void setX(float32 value) { position.x = value; }
	void setY(float32 value) { position.y = value; }
	void setZ(float32 value) { position.z = value; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		color = ((uint32)a << 24) | ((uint32)r << 16) |
		        ((uint32)g << 8) | (uint32)b;
	}
	void setU(float32 value) { u = value; }
	void setV(float32 value) { v = value; }

	float32 getX(void) { return position.x; }
	float32 getY(void) { return position.y; }
	float32 getZ(void) { return position.z; }
	RGBA getColor(void) {
		return makeRGBA((color >> 16) & 0xFF, (color >> 8) & 0xFF,
		                color & 0xFF, (color >> 24) & 0xFF);
	}
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

// Common RenderWare immediate-mode vertex. The packed RGBA layout is kept
// backend-owned so the D3D12 input layout can be defined without borrowing
// the D3D9 ABI.
struct Im2DVertex
{
	float32 x, y, z, w;
	uint32 color;
	float32 u, v;

	void setScreenX(float32 value) { x = value; }
	void setScreenY(float32 value) { y = value; }
	void setScreenZ(float32 value) { z = value; }
	void setCameraZ(float32 value) { w = value; }
	void setRecipCameraZ(float32 value) { w = 1.0f/value; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		color = ((uint32)a << 24) | ((uint32)r << 16) |
		        ((uint32)g << 8) | (uint32)b;
	}
	void setU(float32 value, float32) { u = value; }
	void setV(float32 value, float32) { v = value; }

	float32 getScreenX(void) { return x; }
	float32 getScreenY(void) { return y; }
	float32 getScreenZ(void) { return z; }
	float32 getCameraZ(void) { return w; }
	float32 getRecipCameraZ(void) { return 1.0f/w; }
	RGBA getColor(void) {
		return makeRGBA((color >> 16) & 0xFF, (color >> 8) & 0xFF,
		                color & 0xFF, (color >> 24) & 0xFF);
	}
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

// Stage 5 core-device diagnostics. Rendering resources and pipelines are
// deliberately added in later slices of the backend.
bool32 isInitialized(void);
bool32 isPresentationReady(void);
const char *getAdapterName(void);
bool32 waitForGpu(void);

extern Device renderdevice;

}
}
