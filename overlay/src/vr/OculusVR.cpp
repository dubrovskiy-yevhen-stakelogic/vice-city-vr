#include "common.h"

#if defined(GTA_VR_OCULUS) && !defined(GTA_VR_OPENXR)

#include "OculusVR.h"
#include "Camera.h"
#include "ControllerConfig.h"
#include "Matrix.h"
#include "Pad.h"
#include "postfx.h"

#include <OVR_CAPI_GL.h>
#include <Extras/OVR_CAPI_Util.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern void RenderVrGameplayHud(void);

namespace OculusVR
{
namespace
{
struct EyeBuffer
{
	ovrTextureSwapChain chain;
	RwRaster *color;
	RwRaster *depth;
	ovrSizei size;
	ovrSizei renderSize;
};

ovrSession gSession;
ovrHmdDesc gHmd;
ovrEyeRenderDesc gEyeDesc[ovrEye_Count];
ovrFovPort gRenderFov[ovrEye_Count];
ovrPosef gEyePose[ovrEye_Count];
EyeBuffer gEye[ovrEye_Count];
GLuint gCopyFramebuffer;
GLuint gFxaaProgram;
GLuint gFxaaVertexArray;
GLint gFxaaTextureUniform = -1;
GLint gFxaaInverseSizeUniform = -1;
GLint gFxaaEnabledUniform = -1;
GLint gColorModeUniform = -1;
GLint gBlurColorUniform = -1;
GLint gContrastMultUniform = -1;
GLint gContrastAddUniform = -1;
ovrTextureSwapChain gDebugChain;
ovrTextureSwapChain gHudChain;
RwRaster *gHudColor;
RwRaster *gHudDepth;
ovrTextureSwapChain gCinemaChain;
ovrSizei gCinemaSize;
long long gFrameIndex;
double gSensorSampleTime;
int gRetryFrames;
bool gRuntimeInitialized;
bool gFramePrepared;
bool gWasSubmitting;
bool gTouchWasConnected;
bool gTouchHudShortcutDown;
bool gTouchPerfShortcutDown;
bool gReverseStereo;
bool gFirstPersonEnabled = true;
bool gDebugVisible;
bool gTrackingCenterValid;
bool gAntiAliasingEnabled = true;
bool gLightingEnabled = true;
bool gGameplayHudVisible = true;
int gStereoScaleIndex = 4;
ovrVector3f gTrackingCenterOrigin;
bool gPerfSampleValid;
double gPerfSampleTime;
int gPerfSampleAppIndex;
int gPerfSampleVsyncIndex;
int gPerfAppFps;
int gPerfHmdFps;
int gPerfCpuMs;
int gPerfGpuMs;
bool gPerfAswActive;

enum {
	VR_PERF_MAX_SAMPLES = 54000,
	VR_PERF_GPU_QUERY_COUNT = 8
};

struct PerfFrameSample
{
	long long serial;
	double elapsedSeconds;
	float frameMs;
	float phaseMs[PERF_PHASE_COUNT];
	float gpuStereoMs;
	int runtimeCpuMs;
	int runtimeGpuMs;
	int appFps;
	int hmdFps;
	int visibleBuildings;
	int visibleObjects;
	int visiblePeds;
	int visibleVehicles;
	int entityRenderCalls;
	int requestedModels;
	uint64 streamingMemory;
	float slowStreamItemMs;
	int slowStreamItemId;
	int slowStreamItemType;
	float playerX;
	float playerY;
	float playerZ;
	bool aswActive;
};

struct PerfGpuQuery
{
	GLuint id;
	long long serial;
	bool pending;
};

PerfFrameSample gPerfSamples[VR_PERF_MAX_SAMPLES];
PerfFrameSample gPerfCurrent;
PerfGpuQuery gPerfGpuQueries[VR_PERF_GPU_QUERY_COUNT];
double gPerfFrameStartMs;
double gPerfRecordingStartMs;
double gPerfPhaseStartMs[PERF_PHASE_COUNT];
long long gPerfNextSerial;
int gPerfRecordedSamples;
int gPerfGpuWriteIndex;
bool gPerfRecording;
bool gPerfFrameStarted;
bool gPerfGpuQueryActive;
bool gPerfDebugWasVisible;
double gPerfStreamItemStartMs;
int gPerfStreamItemId;
int gPerfStreamItemType;

const float gStereoScales[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
const float VR_RENDER_SCALE = 1.75f;

enum {
	VR_DEBUG_WIDTH = 512,
	VR_DEBUG_HEIGHT = 128,
	VR_HUD_WIDTH = 1920,
	VR_HUD_HEIGHT = 1080
};
uint8 gDebugPixels[VR_DEBUG_WIDTH * VR_DEBUG_HEIGHT * 4];

struct DebugGlyph
{
	char character;
	uint8 rows[7];
};

const DebugGlyph gDebugGlyphs[] = {
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '%', { 0x11, 0x12, 0x02, 0x04, 0x08, 0x09, 0x11 } },
	{ ':', { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } },
	{ '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
	{ '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
	{ '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
	{ '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
	{ '5', { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E } },
	{ '6', { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
	{ '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
	{ '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E } },
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } }
};

RwRaster *gOriginalColor;
RwRaster *gOriginalDepth;
RwV2d gOriginalViewWindow;
RwV2d gOriginalViewOffset;
RwMatrix gOriginalFrameMatrix;
CMatrix gBaseCamera;
int gOriginalScreenWidth;
int gOriginalScreenHeight;
float gOriginalNearPlane;
float gOriginalDrawNear;

void LogOvrError(const char *operation, ovrResult result)
{
	ovrErrorInfo info = {};
	ovr_GetLastErrorInfo(&info);
	debug("[VR] %s failed (%d): %s\n", operation, result, info.ErrorString);
}

double PerfNowMs()
{
	return ovr_GetTimeInSeconds() * 1000.0;
}

int ComparePerfFrameMs(const void *left, const void *right)
{
	const float a = *(const float*)left;
	const float b = *(const float*)right;
	return a < b ? -1 : a > b ? 1 : 0;
}

float PerfPercentile(const float *sorted, int count, double percentile)
{
	int index = (int)(percentile * count + 0.999999) - 1;
	index = clamp(index, 0, count - 1);
	return sorted[index];
}

float PerfSlowestAverage(const float *sorted, int count, double fraction)
{
	int slowCount = Max(1, (int)(count * fraction + 0.999999));
	double total = 0.0;
	for(int i = count - slowCount; i < count; i++)
		total += sorted[i];
	return (float)(total / slowCount);
}

void AssignGpuTime(long long serial, float milliseconds)
{
	for(int i = gPerfRecordedSamples - 1; i >= 0 && i >= gPerfRecordedSamples - 16; i--){
		if(gPerfSamples[i].serial == serial){
			gPerfSamples[i].gpuStereoMs = milliseconds;
			return;
		}
	}
}

void PollPerfGpuQueries()
{
	if(!glGetQueryObjectiv || !glGetQueryObjectui64v)
		return;
	for(int i = 0; i < VR_PERF_GPU_QUERY_COUNT; i++){
		PerfGpuQuery &query = gPerfGpuQueries[i];
		if(!query.id || !query.pending)
			continue;
		GLint available = GL_FALSE;
		glGetQueryObjectiv(query.id, GL_QUERY_RESULT_AVAILABLE, &available);
		if(available == GL_FALSE)
			continue;
		GLuint64 nanoseconds = 0;
		glGetQueryObjectui64v(query.id, GL_QUERY_RESULT, &nanoseconds);
		AssignGpuTime(query.serial, (float)(nanoseconds / 1000000.0));
		query.pending = false;
	}
}

void BeginPerfGpuQuery()
{
	PollPerfGpuQueries();
	if(!gPerfRecording || !gPerfFrameStarted || gPerfGpuQueryActive || !glGenQueries)
		return;
	if(!gPerfGpuQueries[0].id){
		GLuint ids[VR_PERF_GPU_QUERY_COUNT] = {};
		glGenQueries(VR_PERF_GPU_QUERY_COUNT, ids);
		for(int i = 0; i < VR_PERF_GPU_QUERY_COUNT; i++)
			gPerfGpuQueries[i].id = ids[i];
	}
	PerfGpuQuery &query = gPerfGpuQueries[gPerfGpuWriteIndex];
	if(!query.id || query.pending)
		return;
	query.serial = gPerfCurrent.serial;
	glBeginQuery(GL_TIME_ELAPSED, query.id);
	gPerfGpuQueryActive = true;
}

void EndPerfGpuQuery()
{
	if(!gPerfGpuQueryActive)
		return;
	glEndQuery(GL_TIME_ELAPSED);
	gPerfGpuQueries[gPerfGpuWriteIndex].pending = true;
	gPerfGpuWriteIndex = (gPerfGpuWriteIndex + 1) % VR_PERF_GPU_QUERY_COUNT;
	gPerfGpuQueryActive = false;
}

void DumpPerfRecording()
{
	if(gPerfRecordedSamples <= 0){
		debug("[VR PERF] No complete frames recorded\n");
		return;
	}

	time_t wallTime = time(nil);
	struct tm localTime = {};
	localtime_s(&localTime, &wallTime);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &localTime);
	char csvName[96];
	char reportName[96];
	sprintf(csvName, "vr_perf_%s.csv", stamp);
	sprintf(reportName, "vr_perf_%s.txt", stamp);

	FILE *csv = fopen(csvName, "w");
	if(csv){
		fprintf(csv, "frame,elapsed_s,frame_ms,game_ms,stream_ms,slow_stream_item_ms,slow_stream_item_id,slow_stream_item_type,world_list_ms,pre_render_ms,desktop_render_ms,cinema_submit_ms,left_eye_ms,right_eye_ms,submit_ms,gpu_stereo_ms,runtime_cpu_ms,runtime_gpu_ms,app_fps,hmd_fps,asw,visible_buildings,visible_objects,visible_peds,visible_vehicles,entity_render_calls,requested_models,stream_memory_mb,player_x,player_y,player_z\n");
		for(int i = 0; i < gPerfRecordedSamples; i++){
			const PerfFrameSample &s = gPerfSamples[i];
			fprintf(csv, "%d,%.6f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f\n",
				i, s.elapsedSeconds, s.frameMs, s.phaseMs[PERF_PHASE_GAME],
				s.phaseMs[PERF_PHASE_STREAMING], s.slowStreamItemMs,
				s.slowStreamItemId, s.slowStreamItemType, s.phaseMs[PERF_PHASE_WORLD_LIST],
				s.phaseMs[PERF_PHASE_PRE_RENDER], s.phaseMs[PERF_PHASE_DESKTOP_RENDER],
				s.phaseMs[PERF_PHASE_CINEMA_SUBMIT], s.phaseMs[PERF_PHASE_LEFT_EYE],
				s.phaseMs[PERF_PHASE_RIGHT_EYE], s.phaseMs[PERF_PHASE_SUBMIT], s.gpuStereoMs,
				s.runtimeCpuMs, s.runtimeGpuMs, s.appFps, s.hmdFps, s.aswActive ? 1 : 0,
				s.visibleBuildings, s.visibleObjects, s.visiblePeds, s.visibleVehicles,
				s.entityRenderCalls, s.requestedModels, s.streamingMemory / (1024.0 * 1024.0),
				s.playerX, s.playerY, s.playerZ);
		}
		fclose(csv);
	}

	float *sorted = (float*)malloc(sizeof(float) * gPerfRecordedSamples);
	if(!sorted){
		debug("[VR PERF] CSV saved to %s; summary allocation failed\n", csvName);
		return;
	}
	double frameTotal = 0.0;
	double phaseTotal[PERF_PHASE_COUNT] = {};
	float phaseMax[PERF_PHASE_COUNT] = {};
	int over90 = 0;
	int over72 = 0;
	int over45 = 0;
	int worstIndex = 0;
	for(int i = 0; i < gPerfRecordedSamples; i++){
		const PerfFrameSample &s = gPerfSamples[i];
		sorted[i] = s.frameMs;
		frameTotal += s.frameMs;
		if(s.frameMs > gPerfSamples[worstIndex].frameMs)
			worstIndex = i;
		if(s.frameMs > 1000.0f / 90.0f) over90++;
		if(s.frameMs > 1000.0f / 72.0f) over72++;
		if(s.frameMs > 1000.0f / 45.0f) over45++;
		for(int phase = 0; phase < PERF_PHASE_COUNT; phase++){
			phaseTotal[phase] += s.phaseMs[phase];
			phaseMax[phase] = Max(phaseMax[phase], s.phaseMs[phase]);
		}
	}
	qsort(sorted, gPerfRecordedSamples, sizeof(float), ComparePerfFrameMs);
	const float averageMs = (float)(frameTotal / gPerfRecordedSamples);
	const float slowestOnePercent = PerfSlowestAverage(sorted, gPerfRecordedSamples, 0.01);
	const float slowestPointOnePercent = PerfSlowestAverage(sorted, gPerfRecordedSamples, 0.001);
	const PerfFrameSample &worst = gPerfSamples[worstIndex];

	FILE *report = fopen(reportName, "w");
	if(report){
		fprintf(report, "Vice City VR performance capture\n");
		fprintf(report, "Frames: %d  Duration: %.2f s\n", gPerfRecordedSamples,
			gPerfSamples[gPerfRecordedSamples - 1].elapsedSeconds);
		fprintf(report, "Average: %.3f ms (%.1f FPS)\n", averageMs, averageMs > 0.0f ? 1000.0f / averageMs : 0.0f);
		fprintf(report, "1%% low: %.1f FPS (slowest 1%% average %.3f ms)\n",
			slowestOnePercent > 0.0f ? 1000.0f / slowestOnePercent : 0.0f, slowestOnePercent);
		fprintf(report, "0.1%% low: %.1f FPS (slowest 0.1%% average %.3f ms)\n",
			slowestPointOnePercent > 0.0f ? 1000.0f / slowestPointOnePercent : 0.0f, slowestPointOnePercent);
		fprintf(report, "Frame p95/p99/p99.9/worst: %.3f / %.3f / %.3f / %.3f ms\n",
			PerfPercentile(sorted, gPerfRecordedSamples, 0.95),
			PerfPercentile(sorted, gPerfRecordedSamples, 0.99),
			PerfPercentile(sorted, gPerfRecordedSamples, 0.999), worst.frameMs);
		fprintf(report, "Frames slower than 90/72/45 FPS budgets: %.2f%% / %.2f%% / %.2f%%\n",
			over90 * 100.0 / gPerfRecordedSamples, over72 * 100.0 / gPerfRecordedSamples,
			over45 * 100.0 / gPerfRecordedSamples);
		static const char *phaseNames[PERF_PHASE_COUNT] = {
			"game", "audio", "streaming", "world list", "pre-render", "scene setup",
			"desktop", "UI", "cinema submit", "left eye", "right eye", "submit",
			"desktop present"
		};
		fprintf(report, "\nCPU phases (average / maximum ms):\n");
		for(int phase = 0; phase < PERF_PHASE_COUNT; phase++)
			fprintf(report, "  %-12s %.3f / %.3f\n", phaseNames[phase],
				phaseTotal[phase] / gPerfRecordedSamples, phaseMax[phase]);
		fprintf(report, "\nWorst frame #%d at %.2f s, position %.2f %.2f %.2f\n", worstIndex,
			worst.elapsedSeconds, worst.playerX, worst.playerY, worst.playerZ);
		fprintf(report, "  frame %.3f, game %.3f, streaming %.3f, world list %.3f, pre-render %.3f, desktop %.3f, cinema submit %.3f, eyes %.3f + %.3f, submit %.3f, GPU stereo %.3f ms\n",
			worst.frameMs, worst.phaseMs[PERF_PHASE_GAME], worst.phaseMs[PERF_PHASE_STREAMING],
			worst.phaseMs[PERF_PHASE_WORLD_LIST], worst.phaseMs[PERF_PHASE_PRE_RENDER],
			worst.phaseMs[PERF_PHASE_DESKTOP_RENDER], worst.phaseMs[PERF_PHASE_CINEMA_SUBMIT],
			worst.phaseMs[PERF_PHASE_LEFT_EYE], worst.phaseMs[PERF_PHASE_RIGHT_EYE],
			worst.phaseMs[PERF_PHASE_SUBMIT], worst.gpuStereoMs);
		fprintf(report, "  visible B/O/P/V %d/%d/%d/%d, entity render calls %d, requested models %d, stream memory %.3f MB\n",
			worst.visibleBuildings, worst.visibleObjects, worst.visiblePeds, worst.visibleVehicles,
			worst.entityRenderCalls, worst.requestedModels, worst.streamingMemory / (1024.0 * 1024.0));
		fclose(report);
	}
	free(sorted);
	debug("[VR PERF] Saved %d frames to %s and %s\n", gPerfRecordedSamples, csvName, reportName);
}

void StartPerfRecording()
{
	gPerfRecordedSamples = 0;
	gPerfNextSerial = 0;
	gPerfFrameStarted = false;
	gPerfRecordingStartMs = PerfNowMs();
	gPerfRecording = true;
	gPerfDebugWasVisible = gDebugVisible;
	gDebugVisible = true;
	debug("[VR PERF] Recording started; hold both grips + Y again to save\n");
}

void StopPerfRecording()
{
	EndPerfGpuQuery();
	PollPerfGpuQueries();
	gPerfRecording = false;
	gPerfFrameStarted = false;
	DumpPerfRecording();
	gDebugVisible = gPerfDebugWasVisible;
}

void TogglePerfRecording()
{
	if(gPerfRecording)
		StopPerfRecording();
	else
		StartPerfRecording();
}

GLuint CompileFxaaShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nil);
	glCompileShader(shader);
	GLint compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(compiled == GL_TRUE)
		return shader;
	char log[1024] = {};
	glGetShaderInfoLog(shader, sizeof(log), nil, log);
	debug("[VR] FXAA shader compilation failed: %s\n", log);
	glDeleteShader(shader);
	return 0;
}

bool CreateFxaaProgram()
{
	static const char *vertexSource =
		"#version 330 core\n"
		"out vec2 uv;\n"
		"void main(){\n"
		"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"  uv = p;\n"
		"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char *fragmentSource =
		"#version 330 core\n"
		"uniform sampler2D sourceTexture;\n"
		"uniform vec2 inverseScreenSize;\n"
		"uniform int fxaaEnabled;\n"
		"uniform int colorMode;\n"
		"uniform vec4 blurColor;\n"
		"uniform vec3 contrastMult;\n"
		"uniform vec3 contrastAdd;\n"
		"in vec2 uv;\n"
		"out vec4 outColor;\n"
		"float luma(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
		"vec3 sampleAt(vec2 p){ return texture(sourceTexture, clamp(p, vec2(0.0), vec2(1.0))).rgb; }\n"
		"void main(){\n"
		"  vec3 rgbM = sampleAt(uv);\n"
		"  vec3 rgbNW = sampleAt(uv + vec2(-1.0, -1.0) * inverseScreenSize);\n"
		"  vec3 rgbNE = sampleAt(uv + vec2( 1.0, -1.0) * inverseScreenSize);\n"
		"  vec3 rgbSW = sampleAt(uv + vec2(-1.0,  1.0) * inverseScreenSize);\n"
		"  vec3 rgbSE = sampleAt(uv + vec2( 1.0,  1.0) * inverseScreenSize);\n"
		"  float lumaM = luma(rgbM);\n"
		"  float lumaNW = luma(rgbNW); float lumaNE = luma(rgbNE);\n"
		"  float lumaSW = luma(rgbSW); float lumaSE = luma(rgbSE);\n"
		"  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));\n"
		"  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));\n"
		"  vec2 dir;\n"
		"  dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
		"  dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
		"  float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);\n"
		"  float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
		"  dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * inverseScreenSize;\n"
		"  vec3 rgbA = 0.5 * (sampleAt(uv + dir * (1.0/3.0 - 0.5)) + sampleAt(uv + dir * (2.0/3.0 - 0.5)));\n"
		"  vec3 rgbB = rgbA * 0.5 + 0.25 * (sampleAt(uv + dir * -0.5) + sampleAt(uv + dir * 0.5));\n"
		"  float lumaB = luma(rgbB);\n"
		"  vec3 color = fxaaEnabled != 0 ? ((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB) : rgbM;\n"
		"  if(colorMode == 1){\n"
		"    float a = blurColor.a;\n"
		"    vec3 doubled = clamp(blurColor.rgb * 2.0, 0.0, 1.0);\n"
		"    vec3 original = color;\n"
		"    vec3 previous = color;\n"
		"    for(int i = 0; i < 5; ++i){\n"
		"      vec3 filtered = original * (1.0 - a) + previous * doubled * a;\n"
		"      filtered += previous * blurColor.rgb * 2.0;\n"
		"      previous = clamp(filtered, 0.0, 1.0);\n"
		"    }\n"
		"    color = previous;\n"
		"  }else if(colorMode == 2){\n"
		"    color = clamp(color * contrastMult + contrastAdd, 0.0, 1.0);\n"
		"  }\n"
		"  outColor = vec4(color, 1.0);\n"
		"}\n";

	GLuint vertex = CompileFxaaShader(GL_VERTEX_SHADER, vertexSource);
	GLuint fragment = CompileFxaaShader(GL_FRAGMENT_SHADER, fragmentSource);
	if(!vertex || !fragment){
		if(vertex) glDeleteShader(vertex);
		if(fragment) glDeleteShader(fragment);
		return false;
	}
	gFxaaProgram = glCreateProgram();
	glAttachShader(gFxaaProgram, vertex);
	glAttachShader(gFxaaProgram, fragment);
	glLinkProgram(gFxaaProgram);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint linked = GL_FALSE;
	glGetProgramiv(gFxaaProgram, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE){
		char log[1024] = {};
		glGetProgramInfoLog(gFxaaProgram, sizeof(log), nil, log);
		debug("[VR] FXAA program link failed: %s\n", log);
		glDeleteProgram(gFxaaProgram);
		gFxaaProgram = 0;
		return false;
	}
	gFxaaTextureUniform = glGetUniformLocation(gFxaaProgram, "sourceTexture");
	gFxaaInverseSizeUniform = glGetUniformLocation(gFxaaProgram, "inverseScreenSize");
	gFxaaEnabledUniform = glGetUniformLocation(gFxaaProgram, "fxaaEnabled");
	gColorModeUniform = glGetUniformLocation(gFxaaProgram, "colorMode");
	gBlurColorUniform = glGetUniformLocation(gFxaaProgram, "blurColor");
	gContrastMultUniform = glGetUniformLocation(gFxaaProgram, "contrastMult");
	gContrastAddUniform = glGetUniformLocation(gFxaaProgram, "contrastAdd");
	glGenVertexArrays(1, &gFxaaVertexArray);
	return true;
}

void DestroyFxaaProgram()
{
	if(gFxaaVertexArray){
		glDeleteVertexArrays(1, &gFxaaVertexArray);
		gFxaaVertexArray = 0;
	}
	if(gFxaaProgram){
		glDeleteProgram(gFxaaProgram);
		gFxaaProgram = 0;
	}
}

const uint8 *FindDebugGlyph(char character)
{
	for(uint32 i = 0; i < ARRAY_SIZE(gDebugGlyphs); i++)
		if(gDebugGlyphs[i].character == character)
			return gDebugGlyphs[i].rows;
	return gDebugGlyphs[0].rows;
}

void PutDebugPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_DEBUG_WIDTH || y >= VR_DEBUG_HEIGHT)
		return;
	// OpenGL's first upload row is the bottom row. Store our top-down text flipped.
	const int offset = ((VR_DEBUG_HEIGHT - 1 - y) * VR_DEBUG_WIDTH + x) * 4;
	gDebugPixels[offset + 0] = red;
	gDebugPixels[offset + 1] = green;
	gDebugPixels[offset + 2] = blue;
	gDebugPixels[offset + 3] = alpha;
}

void DrawDebugText(const char *text, int centerX, int y, int scale,
	uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale * 6;
	int x = centerX - (int)strlen(text) * advance / 2;
	for(const char *character = text; *character; character++, x += advance){
		const uint8 *rows = FindDebugGlyph(*character);
		for(int row = 0; row < 7; row++)
			for(int column = 0; column < 5; column++)
				if(rows[row] & (1 << (4 - column)))
					for(int py = 0; py < scale; py++)
						for(int px = 0; px < scale; px++)
							PutDebugPixel(x + column*scale + px, y + row*scale + py,
								red, green, blue, 255);
	}
}

bool CreateDebugChain()
{
	ovrTextureSwapChainDesc desc = {};
	desc.Type = ovrTexture_2D;
	desc.ArraySize = 1;
	desc.Width = VR_DEBUG_WIDTH;
	desc.Height = VR_DEBUG_HEIGHT;
	desc.MipLevels = 1;
	desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleCount = 1;
	desc.StaticImage = ovrFalse;

	ovrResult result = ovr_CreateTextureSwapChainGL(gSession, &desc, &gDebugChain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CreateTextureSwapChainGL(debug)", result);
		return false;
	}

	GLint oldTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	int length = 0;
	ovr_GetTextureSwapChainLength(gSession, gDebugChain, &length);
	for(int index = 0; index < length; index++){
		GLuint texture = 0;
		ovr_GetTextureSwapChainBufferGL(gSession, gDebugChain, index, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	return true;
}

void DestroyHudResources()
{
	if(gHudDepth){
		RwRasterDestroy(gHudDepth);
		gHudDepth = nil;
	}
	if(gHudColor){
		RwRasterDestroy(gHudColor);
		gHudColor = nil;
	}
	if(gHudChain){
		ovr_DestroyTextureSwapChain(gSession, gHudChain);
		gHudChain = nil;
	}
}

bool CreateHudResources()
{
	ovrTextureSwapChainDesc desc = {};
	desc.Type = ovrTexture_2D;
	desc.ArraySize = 1;
	desc.Width = VR_HUD_WIDTH;
	desc.Height = VR_HUD_HEIGHT;
	desc.MipLevels = 1;
	desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleCount = 1;
	desc.StaticImage = ovrFalse;
	ovrResult result = ovr_CreateTextureSwapChainGL(gSession, &desc, &gHudChain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CreateTextureSwapChainGL(HUD)", result);
		return false;
	}

	GLint oldTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	int length = 0;
	ovr_GetTextureSwapChainLength(gSession, gHudChain, &length);
	for(int index = 0; index < length; index++){
		GLuint texture = 0;
		ovr_GetTextureSwapChainBufferGL(gSession, gHudChain, index, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, oldTexture);

	gHudColor = RwRasterCreate(VR_HUD_WIDTH, VR_HUD_HEIGHT, 32,
		rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
	gHudDepth = RwRasterCreate(VR_HUD_WIDTH, VR_HUD_HEIGHT, 0, rwRASTERTYPEZBUFFER);
	if(!gHudColor || !gHudDepth){
		debug("[VR] Unable to create the gameplay HUD render target\n");
		return false;
	}
	return true;
}

void DestroyCinemaChain()
{
	if(gCinemaChain){
		ovr_DestroyTextureSwapChain(gSession, gCinemaChain);
		gCinemaChain = nil;
	}
	gCinemaSize.w = 0;
	gCinemaSize.h = 0;
}

bool EnsureCinemaChain(int width, int height)
{
	if(gCinemaChain && gCinemaSize.w == width && gCinemaSize.h == height)
		return true;
	DestroyCinemaChain();

	ovrTextureSwapChainDesc desc = {};
	desc.Type = ovrTexture_2D;
	desc.ArraySize = 1;
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleCount = 1;
	desc.StaticImage = ovrFalse;

	ovrResult result = ovr_CreateTextureSwapChainGL(gSession, &desc, &gCinemaChain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CreateTextureSwapChainGL(cinema)", result);
		return false;
	}
	gCinemaSize.w = width;
	gCinemaSize.h = height;

	GLint oldTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	int length = 0;
	ovr_GetTextureSwapChainLength(gSession, gCinemaChain, &length);
	for(int index = 0; index < length; index++){
		GLuint texture = 0;
		ovr_GetTextureSwapChainBufferGL(gSession, gCinemaChain, index, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	return true;
}

void UpdateRuntimePerfStats()
{
	ovrPerfStats stats = {};
	if(OVR_FAILURE(ovr_GetPerfStats(gSession, &stats)) || stats.FrameStatsCount <= 0)
		return;

	const ovrPerfStatsPerCompositorFrame &frame = stats.FrameStats[0];
	gPerfAswActive = frame.AswIsActive != ovrFalse;
	gPerfCpuMs = frame.AppCpuElapsedTime > 0.0f ?
		(int)(frame.AppCpuElapsedTime * 1000.0f + 0.5f) : 0;
	gPerfGpuMs = frame.AppGpuElapsedTime > 0.0f ?
		(int)(frame.AppGpuElapsedTime * 1000.0f + 0.5f) : 0;
	const double now = ovr_GetTimeInSeconds();
	if(!gPerfSampleValid || frame.AppFrameIndex < gPerfSampleAppIndex ||
	   frame.HmdVsyncIndex < gPerfSampleVsyncIndex){
		gPerfSampleValid = true;
		gPerfSampleTime = now;
		gPerfSampleAppIndex = frame.AppFrameIndex;
		gPerfSampleVsyncIndex = frame.HmdVsyncIndex;
		return;
	}

	const double elapsed = now - gPerfSampleTime;
	if(elapsed >= 0.5){
		gPerfAppFps = (int)((frame.AppFrameIndex - gPerfSampleAppIndex) / elapsed + 0.5);
		gPerfHmdFps = (int)((frame.HmdVsyncIndex - gPerfSampleVsyncIndex) / elapsed + 0.5);
		gPerfSampleTime = now;
		gPerfSampleAppIndex = frame.AppFrameIndex;
		gPerfSampleVsyncIndex = frame.HmdVsyncIndex;
	}
}

bool UpdateDebugChain()
{
	if(!gDebugChain)
		return false;
	UpdateRuntimePerfStats();

	for(int pixel = 0; pixel < VR_DEBUG_WIDTH * VR_DEBUG_HEIGHT; pixel++){
		gDebugPixels[pixel*4 + 0] = 0;
		gDebugPixels[pixel*4 + 1] = 0;
		gDebugPixels[pixel*4 + 2] = 0;
		gDebugPixels[pixel*4 + 3] = 210;
	}

	char text[48];
	sprintf(text, "AA: %s LIGHT: %s HUD: %s", gAntiAliasingEnabled ? "ON" : "OFF",
		gLightingEnabled ? "ON" : "OFF", gGameplayHudVisible ? "ON" : "OFF");
	DrawDebugText(text, VR_DEBUG_WIDTH / 2, 10, 3, 255, 230, 64);
	sprintf(text, "APP: %d HMD: %d", gPerfAppFps, gPerfHmdFps);
	DrawDebugText(text, VR_DEBUG_WIDTH / 2, 48, 3, 96, 220, 255);
	if(gPerfRecording)
		sprintf(text, "REC:%d GPU:%dMS ASW:%s", gPerfRecordedSamples, gPerfGpuMs,
			gPerfAswActive ? "ON" : "OFF");
	else
		sprintf(text, "CPU:%dMS GPU:%dMS ASW:%s", gPerfCpuMs, gPerfGpuMs,
			gPerfAswActive ? "ON" : "OFF");
	DrawDebugText(text, VR_DEBUG_WIDTH / 2, 86, 3, 128, 255, 128);

	int index = 0;
	ovrResult result = ovr_GetTextureSwapChainCurrentIndex(gSession, gDebugChain, &index);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetTextureSwapChainCurrentIndex(debug)", result);
		return false;
	}
	GLuint texture = 0;
	result = ovr_GetTextureSwapChainBufferGL(gSession, gDebugChain, index, &texture);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetTextureSwapChainBufferGL(debug)", result);
		return false;
	}

	GLint oldTexture = 0;
	GLint oldAlignment = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VR_DEBUG_WIDTH, VR_DEBUG_HEIGHT,
		GL_RGBA, GL_UNSIGNED_BYTE, gDebugPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
	glBindTexture(GL_TEXTURE_2D, oldTexture);

	result = ovr_CommitTextureSwapChain(gSession, gDebugChain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CommitTextureSwapChain(debug)", result);
		return false;
	}
	return true;
}

void DestroyEye(EyeBuffer &eye)
{
	if(eye.depth){
		RwRasterDestroy(eye.depth);
		eye.depth = nil;
	}
	if(eye.color){
		RwRasterDestroy(eye.color);
		eye.color = nil;
	}
	if(eye.chain){
		ovr_DestroyTextureSwapChain(gSession, eye.chain);
		eye.chain = nil;
	}
	eye.size.w = 0;
	eye.size.h = 0;
	eye.renderSize.w = 0;
	eye.renderSize.h = 0;
}

void DestroySession()
{
	EndPerfGpuQuery();
	if(gPerfGpuQueries[0].id){
		GLuint ids[VR_PERF_GPU_QUERY_COUNT];
		for(int i = 0; i < VR_PERF_GPU_QUERY_COUNT; i++){
			ids[i] = gPerfGpuQueries[i].id;
			gPerfGpuQueries[i].id = 0;
			gPerfGpuQueries[i].pending = false;
		}
		glDeleteQueries(VR_PERF_GPU_QUERY_COUNT, ids);
	}
	gPerfGpuWriteIndex = 0;
	for(int eye = 0; eye < ovrEye_Count; eye++)
		DestroyEye(gEye[eye]);

	DestroyHudResources();
	DestroyFxaaProgram();
	if(gCopyFramebuffer){
		glDeleteFramebuffers(1, &gCopyFramebuffer);
		gCopyFramebuffer = 0;
	}
	if(gDebugChain){
		ovr_DestroyTextureSwapChain(gSession, gDebugChain);
		gDebugChain = nil;
	}
	DestroyCinemaChain();
	if(gSession){
		ovr_Destroy(gSession);
		gSession = nil;
	}

	gFrameIndex = 0;
	gFramePrepared = false;
	gWasSubmitting = false;
	gTouchWasConnected = false;
	gTouchHudShortcutDown = false;
	gTouchPerfShortcutDown = false;
	gTrackingCenterValid = false;
	gPerfSampleValid = false;
	gPerfAppFps = 0;
	gPerfHmdFps = 0;
	gPerfGpuMs = 0;
	gPerfAswActive = false;
}

bool InitializeRuntime()
{
	if(gRuntimeInitialized)
		return true;

	ovrInitParams params = {};
	params.Flags = ovrInit_RequestVersion;
	params.RequestedMinorVersion = OVR_MINOR_VERSION;
	ovrResult result = ovr_Initialize(&params);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_Initialize", result);
		return false;
	}

	gRuntimeInitialized = true;
	debug("[VR] Oculus runtime initialized\n");
	return true;
}

bool CreateEye(int eye)
{
	EyeBuffer &buffer = gEye[eye];
	const ovrFovPort fov = gRenderFov[eye];
	// Submit a compositor-recommended texture, but render the world at a higher
	// resolution and explicitly downsample into it. This produces real SSAA rather
	// than relying on the headset compositor's distortion pass to hide jagged edges.
	buffer.size = ovr_GetFovTextureSize(gSession, (ovrEyeType)eye, fov, 1.0f);
	buffer.renderSize = ovr_GetFovTextureSize(gSession, (ovrEyeType)eye, fov, VR_RENDER_SCALE);
	gEyeDesc[eye] = ovr_GetRenderDesc(gSession, (ovrEyeType)eye, fov);

	ovrTextureSwapChainDesc desc = {};
	desc.Type = ovrTexture_2D;
	desc.ArraySize = 1;
	desc.Width = buffer.size.w;
	desc.Height = buffer.size.h;
	desc.MipLevels = 1;
	desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleCount = 1;
	desc.StaticImage = ovrFalse;

	ovrResult result = ovr_CreateTextureSwapChainGL(gSession, &desc, &buffer.chain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CreateTextureSwapChainGL", result);
		return false;
	}

	buffer.color = RwRasterCreate(buffer.renderSize.w, buffer.renderSize.h, 32,
		rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
	buffer.depth = RwRasterCreate(buffer.renderSize.w, buffer.renderSize.h, 0, rwRASTERTYPEZBUFFER);
	if(!buffer.color || !buffer.depth){
		debug("[VR] Unable to create RenderWare eye target %d (%dx%d)\n",
			eye, buffer.renderSize.w, buffer.renderSize.h);
		return false;
	}

	return true;
}

bool CreateSession()
{
	if(!InitializeRuntime())
		return false;

	ovrGraphicsLuid luid = {};
	ovrResult result = ovr_Create(&gSession, &luid);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_Create", result);
		gSession = nil;
		return false;
	}

	gHmd = ovr_GetHmdDesc(gSession);
	// Start from a common symmetric projection for both eyes.  This removes all
	// per-eye lens-centre offsets from the RenderWare conversion and gives us an
	// unambiguous zero-stereo calibration image.  Once that image fuses correctly,
	// stereo separation can be added independently with F7.
	float horizontalTan = 0.0f;
	float verticalTan = 0.0f;
	for(int eye = 0; eye < ovrEye_Count; eye++){
		horizontalTan = Max(horizontalTan, Max(gHmd.DefaultEyeFov[eye].LeftTan,
			gHmd.DefaultEyeFov[eye].RightTan));
		verticalTan = Max(verticalTan, Max(gHmd.DefaultEyeFov[eye].UpTan,
			gHmd.DefaultEyeFov[eye].DownTan));
	}
	for(int eye = 0; eye < ovrEye_Count; eye++){
		gRenderFov[eye].UpTan = verticalTan;
		gRenderFov[eye].DownTan = verticalTan;
		gRenderFov[eye].LeftTan = horizontalTan;
		gRenderFov[eye].RightTan = horizontalTan;
	}
	for(int eye = 0; eye < ovrEye_Count; eye++){
		if(!CreateEye(eye)){
			DestroySession();
			return false;
		}
	}
	if(!CreateDebugChain()){
		DestroySession();
		return false;
	}
	if(!CreateHudResources()){
		DestroySession();
		return false;
	}

	glGenFramebuffers(1, &gCopyFramebuffer);
	if(!CreateFxaaProgram())
		debug("[VR] FXAA unavailable; using the original direct eye copy\n");
	ovr_SetTrackingOriginType(gSession, ovrTrackingOrigin_EyeLevel);
	ovr_RecenterTrackingOrigin(gSession);

	debug("[VR] Stereo session ready: %s, submit %dx%d, render %dx%d (%.0f%%)\n",
		gHmd.ProductName,
		gEye[ovrEye_Left].size.w, gEye[ovrEye_Left].size.h,
		gEye[ovrEye_Left].renderSize.w, gEye[ovrEye_Left].renderSize.h,
		VR_RENDER_SCALE * 100.0f);
	return true;
}

bool EnsureSession()
{
	if(gSession)
		return true;

	if(gRetryFrames > 0){
		--gRetryFrames;
		return false;
	}

	if(!CreateSession()){
		gRetryFrames = 300;
		return false;
	}
	return true;
}

bool UpdateSessionStatus()
{
	ovrSessionStatus status = {};
	ovrResult result = ovr_GetSessionStatus(gSession, &status);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetSessionStatus", result);
		DestroySession();
		return false;
	}
	if(status.ShouldQuit || status.DisplayLost){
		DestroySession();
		gRetryFrames = 300;
		return false;
	}
	if(status.ShouldRecenter){
		ovr_RecenterTrackingOrigin(gSession);
		ovr_ClearShouldRecenterFlag(gSession);
		gTrackingCenterValid = false;
	}
	return true;
}

ovrVector3f Rotate(const ovrQuatf &q, const ovrVector3f &v)
{
	const ovrVector3f qv = { q.x, q.y, q.z };
	const ovrVector3f t = {
		2.0f * (qv.y*v.z - qv.z*v.y),
		2.0f * (qv.z*v.x - qv.x*v.z),
		2.0f * (qv.x*v.y - qv.y*v.x)
	};
	const ovrVector3f cross = {
		qv.y*t.z - qv.z*t.y,
		qv.z*t.x - qv.x*t.z,
		qv.x*t.y - qv.y*t.x
	};
	const ovrVector3f result = {
		v.x + q.w*t.x + cross.x,
		v.y + q.w*t.y + cross.y,
		v.z + q.w*t.z + cross.z
	};
	return result;
}

CVector ToGameVector(const ovrVector3f &v)
{
	// Oculus: +X right, +Y up, -Z forward. reVC stores camera left in GetRight().
	return gBaseCamera.GetRight()*(-v.x) +
		gBaseCamera.GetUp()*v.y + gBaseCamera.GetForward()*(-v.z);
}

void RestoreCamera(RwCamera *camera)
{
	if(!gFramePrepared)
		return;

	RwCameraSetRaster(camera, gOriginalColor);
	RwCameraSetZRaster(camera, gOriginalDepth);
	RwCameraSetViewWindow(camera, &gOriginalViewWindow);
	RwCameraSetViewOffset(camera, &gOriginalViewOffset);
	RwCameraSetNearClipPlane(camera, gOriginalNearPlane);
	CDraw::SetNearClipZ(gOriginalDrawNear);
	RsGlobal.width = gOriginalScreenWidth;
	RsGlobal.height = gOriginalScreenHeight;

	RwFrame *frame = RwCameraGetFrame(camera);
	*RwFrameGetMatrix(frame) = gOriginalFrameMatrix;
	RwMatrixUpdate(RwFrameGetMatrix(frame));
	RwFrameUpdateObjects(frame);
	RwFrameOrthoNormalize(frame);
	gFramePrepared = false;
}

int16 AxisValue(float value)
{
	value = clamp(value, -1.0f, 1.0f);
	return (int16)(value * 128.0f);
}

int16 TriggerValue(float value)
{
	value = clamp(value, 0.0f, 1.0f);
	return (int16)(value * 255.0f);
}

void MergeAxis(int16 &destination, int16 value)
{
	if(Abs(value) > Abs(destination))
		destination = value;
}

void MergeButton(int16 &destination, bool pressed)
{
	if(pressed)
		destination = 255;
}
}

void PerfBeginFrame()
{
	if(!gPerfRecording)
		return;
	memset(&gPerfCurrent, 0, sizeof(gPerfCurrent));
	gPerfCurrent.slowStreamItemId = -1;
	gPerfCurrent.slowStreamItemType = -1;
	gPerfStreamItemStartMs = 0.0;
	for(int phase = 0; phase < PERF_PHASE_COUNT; phase++)
		gPerfPhaseStartMs[phase] = -1.0;
	gPerfCurrent.serial = gPerfNextSerial++;
	gPerfCurrent.elapsedSeconds = (PerfNowMs() - gPerfRecordingStartMs) / 1000.0;
	gPerfCurrent.gpuStereoMs = -1.0f;
	gPerfFrameStartMs = PerfNowMs();
	gPerfFrameStarted = true;
}

void PerfAbortFrame()
{
	gPerfFrameStarted = false;
	gPerfStreamItemStartMs = 0.0;
}

void PerfEndFrame(float playerX, float playerY, float playerZ)
{
	if(!gPerfRecording || !gPerfFrameStarted)
		return;
	gPerfCurrent.frameMs = (float)(PerfNowMs() - gPerfFrameStartMs);
	gPerfCurrent.runtimeCpuMs = gPerfCpuMs;
	gPerfCurrent.runtimeGpuMs = gPerfGpuMs;
	gPerfCurrent.appFps = gPerfAppFps;
	gPerfCurrent.hmdFps = gPerfHmdFps;
	gPerfCurrent.aswActive = gPerfAswActive;
	gPerfCurrent.playerX = playerX;
	gPerfCurrent.playerY = playerY;
	gPerfCurrent.playerZ = playerZ;
	if(gPerfRecordedSamples < VR_PERF_MAX_SAMPLES)
		gPerfSamples[gPerfRecordedSamples++] = gPerfCurrent;
	gPerfFrameStarted = false;
	gPerfStreamItemStartMs = 0.0;
	if(gPerfRecordedSamples >= VR_PERF_MAX_SAMPLES)
		StopPerfRecording();
}

void PerfBeginPhase(ePerfPhase phase)
{
	if(gPerfRecording && gPerfFrameStarted && phase >= 0 && phase < PERF_PHASE_COUNT)
		gPerfPhaseStartMs[phase] = PerfNowMs();
}

void PerfEndPhase(ePerfPhase phase)
{
	if(!gPerfRecording || !gPerfFrameStarted || phase < 0 || phase >= PERF_PHASE_COUNT ||
	   gPerfPhaseStartMs[phase] < 0.0)
		return;
	gPerfCurrent.phaseMs[phase] += (float)(PerfNowMs() - gPerfPhaseStartMs[phase]);
	gPerfPhaseStartMs[phase] = -1.0;
}

void PerfSetStreamingStats(int requestedModels, uint64 memoryUsed)
{
	if(!gPerfRecording || !gPerfFrameStarted)
		return;
	gPerfCurrent.requestedModels = requestedModels;
	gPerfCurrent.streamingMemory = memoryUsed;
}

void PerfBeginStreamItem(int streamId, int streamType)
{
	if(!gPerfRecording || !gPerfFrameStarted){ gPerfStreamItemStartMs = 0.0; return; }
	gPerfStreamItemId = streamId;
	gPerfStreamItemType = streamType;
	gPerfStreamItemStartMs = PerfNowMs();
}

void PerfEndStreamItem()
{
	if(gPerfStreamItemStartMs <= 0.0 || !gPerfRecording || !gPerfFrameStarted){
		gPerfStreamItemStartMs = 0.0;
		return;
	}
	const float elapsed = (float)(PerfNowMs() - gPerfStreamItemStartMs);
	if(elapsed > gPerfCurrent.slowStreamItemMs){
		gPerfCurrent.slowStreamItemMs = elapsed;
		gPerfCurrent.slowStreamItemId = gPerfStreamItemId;
		gPerfCurrent.slowStreamItemType = gPerfStreamItemType;
	}
	gPerfStreamItemStartMs = 0.0;
}

void PerfCountVisibleEntity(ePerfVisibleType type, const CEntity *entity)
{
	(void)entity;
	if(!gPerfRecording || !gPerfFrameStarted)
		return;
	switch(type){
	case PERF_VISIBLE_BUILDING: gPerfCurrent.visibleBuildings++; break;
	case PERF_VISIBLE_OBJECT: gPerfCurrent.visibleObjects++; break;
	case PERF_VISIBLE_PED: gPerfCurrent.visiblePeds++; break;
	case PERF_VISIBLE_VEHICLE: gPerfCurrent.visibleVehicles++; break;
	default: break;
	}
}

void PerfCountEntityRender()
{
	if(gPerfRecording && gPerfFrameStarted)
		gPerfCurrent.entityRenderCalls++;
}

bool ApplyTouchInput(CControllerState *state)
{
	// Graphics submission owns session creation.  The initial frontend polls pads
	// before its first rendered frame; creating GL swapchains from that input path
	// can fail and postpone VR until gameplay.  Once the first cinema frame creates
	// the session, Touch input becomes available on the following menu frame.
	if(!state || !gSession || !UpdateSessionStatus())
		return false;

	const unsigned int connected = ovr_GetConnectedControllerTypes(gSession);
	const bool touchConnected = (connected & ovrControllerType_Touch) != 0;
	if(touchConnected != gTouchWasConnected){
		debug("[VR] Touch controllers %s\n", touchConnected ? "connected" : "disconnected");
		if(touchConnected)
			debug("[VR] Shortcuts: both grips + X toggles HUD; both grips + Y toggles performance capture\n");
		gTouchWasConnected = touchConnected;
	}
	if(!touchConnected){
		gTouchHudShortcutDown = false;
		gTouchPerfShortcutDown = false;
		return false;
	}

	ovrInputState input = {};
	ovrResult result = ovr_GetInputState(gSession, ovrControllerType_Touch, &input);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetInputState", result);
		return false;
	}

	MergeAxis(state->LeftStickX, AxisValue(input.Thumbstick[ovrHand_Left].x));
	MergeAxis(state->LeftStickY, AxisValue(-input.Thumbstick[ovrHand_Left].y));
	MergeAxis(state->RightStickX, AxisValue(input.Thumbstick[ovrHand_Right].x));
	MergeAxis(state->RightStickY, AxisValue(-input.Thumbstick[ovrHand_Right].y));

	const bool shortcutModifier = input.HandTrigger[ovrHand_Left] >= 0.75f &&
		input.HandTrigger[ovrHand_Right] >= 0.75f;
	const bool hudShortcut = shortcutModifier && (input.Buttons & ovrButton_X) != 0;
	const bool perfShortcut = shortcutModifier && (input.Buttons & ovrButton_Y) != 0;
	if(hudShortcut && !gTouchHudShortcutDown){
		gGameplayHudVisible = !gGameplayHudVisible;
		debug("[VR] Gameplay HUD: %s\n", gGameplayHudVisible ? "visible" : "hidden");
	}
	if(perfShortcut && !gTouchPerfShortcutDown)
		TogglePerfRecording();
	gTouchHudShortcutDown = hudShortcut;
	gTouchPerfShortcutDown = perfShortcut;

	// Both grips form a service modifier. Do not also deliver them as L1/R1 while
	// a VR shortcut is being selected.
	if(!shortcutModifier){
		state->LeftShoulder1 = Max(state->LeftShoulder1, TriggerValue(input.HandTrigger[ovrHand_Left]));
		state->RightShoulder1 = Max(state->RightShoulder1, TriggerValue(input.HandTrigger[ovrHand_Right]));
	}
	// L2/R2 are camera-look buttons in Vice City. Mapping Touch index triggers to
	// them made the desktop camera cull the forward view while VR still looked ahead.
	// Use the conventional VR driving layout instead: left trigger brakes, right
	// trigger accelerates (Square/Cross in the original controller mode).
	state->Square = Max(state->Square, TriggerValue(input.IndexTrigger[ovrHand_Left]));
	state->Cross = Max(state->Cross, TriggerValue(input.IndexTrigger[ovrHand_Right]));

	MergeButton(state->Cross, (input.Buttons & ovrButton_A) != 0);
	MergeButton(state->Circle, (input.Buttons & ovrButton_B) != 0);
	if(!shortcutModifier){
		MergeButton(state->Square, (input.Buttons & ovrButton_X) != 0);
		MergeButton(state->Triangle, (input.Buttons & ovrButton_Y) != 0);
	}
	MergeButton(state->LeftShock, (input.Buttons & ovrButton_LThumb) != 0);
	MergeButton(state->RightShock, (input.Buttons & ovrButton_RThumb) != 0);
	MergeButton(state->Start, (input.Buttons & ovrButton_Enter) != 0);
	return true;
}

bool BeginStereoFrame(RwCamera *camera, const CMatrix &baseCamera)
{
	if(!camera || !EnsureSession() || !UpdateSessionStatus())
		return false;
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF4)){
		gLightingEnabled = !gLightingEnabled;
		debug("[VR] Vice City color filter: %s\n", gLightingEnabled ? "enabled" : "disabled");
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF5)){
		gAntiAliasingEnabled = !gAntiAliasingEnabled;
		debug("[VR] Anti-aliasing comparison: %s\n", gAntiAliasingEnabled ? "enabled" : "disabled");
	}
	PollPerfGpuQueries();

	gBaseCamera = baseCamera;
	gOriginalColor = RwCameraGetRaster(camera);
	gOriginalDepth = RwCameraGetZRaster(camera);
	gOriginalViewWindow = *RwCameraGetViewWindow(camera);
	gOriginalViewOffset = *RwCameraGetViewOffset(camera);
	gOriginalFrameMatrix = *RwFrameGetMatrix(RwCameraGetFrame(camera));
	gOriginalScreenWidth = RsGlobal.width;
	gOriginalScreenHeight = RsGlobal.height;
	gOriginalNearPlane = RwCameraGetNearClipPlane(camera);
	gOriginalDrawNear = CDraw::GetNearClipZ();
	gFramePrepared = true;
	BeginPerfGpuQuery();

	ovrPosef hmdToEyePose[ovrEye_Count] = {
		gEyeDesc[ovrEye_Left].HmdToEyePose,
		gEyeDesc[ovrEye_Right].HmdToEyePose
	};
	ovr_GetEyePoses(gSession, gFrameIndex, ovrTrue, hmdToEyePose, gEyePose, &gSensorSampleTime);

	// F6 reverses the eye baseline; F7 cycles its strength.
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF6)){
		gReverseStereo = !gReverseStereo;
		debug("[VR] Stereo direction: %s\n", gReverseStereo ? "reversed" : "original");
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF7)){
		gStereoScaleIndex = (gStereoScaleIndex + 1) % ARRAY_SIZE(gStereoScales);
		debug("[VR] Stereo scale: %.0f%%\n", gStereoScales[gStereoScaleIndex] * 100.0f);
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF8)){
		gFirstPersonEnabled = !gFirstPersonEnabled;
		ovr_RecenterTrackingOrigin(gSession);
		gTrackingCenterValid = false;
		debug("[VR] Camera mode: %s\n", gFirstPersonEnabled ? "first person" : "chase");
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF10)){
		gDebugVisible = !gDebugVisible;
		debug("[VR] Calibration HUD: %s\n", gDebugVisible ? "visible" : "hidden");
	}

	// The game camera is the tracking origin for now.  Do not apply the absolute
	// room-space head position to the existing third-person camera: doing so makes
	// that camera orbit its target as the player turns.  Keep only the rotated IPD
	// offset between the two eyes.  Room-scale translation can be reintroduced once
	// the VR camera is attached to the player's head instead of the chase camera.
	ovrVector3f eyeCenter = {
		(gEyePose[ovrEye_Left].Position.x + gEyePose[ovrEye_Right].Position.x) * 0.5f,
		(gEyePose[ovrEye_Left].Position.y + gEyePose[ovrEye_Right].Position.y) * 0.5f,
		(gEyePose[ovrEye_Left].Position.z + gEyePose[ovrEye_Right].Position.z) * 0.5f
	};
	if(gFirstPersonEnabled && !gTrackingCenterValid){
		gTrackingCenterOrigin = eyeCenter;
		gTrackingCenterValid = true;
	}
	const ovrVector3f trackingOffset = {
		gFirstPersonEnabled ? eyeCenter.x - gTrackingCenterOrigin.x : 0.0f,
		gFirstPersonEnabled ? eyeCenter.y - gTrackingCenterOrigin.y : 0.0f,
		gFirstPersonEnabled ? eyeCenter.z - gTrackingCenterOrigin.z : 0.0f
	};
	const float stereoScale = gStereoScales[gStereoScaleIndex] * (gReverseStereo ? -1.0f : 1.0f);
	for(int eye = 0; eye < ovrEye_Count; eye++){
		const ovrVector3f eyeOffset = {
			gEyePose[eye].Position.x - eyeCenter.x,
			gEyePose[eye].Position.y - eyeCenter.y,
			gEyePose[eye].Position.z - eyeCenter.z
		};
		gEyePose[eye].Position.x = eyeOffset.x * stereoScale + trackingOffset.x;
		gEyePose[eye].Position.y = eyeOffset.y * stereoScale + trackingOffset.y;
		gEyePose[eye].Position.z = eyeOffset.z * stereoScale + trackingOffset.z;
	}
	return true;
}

bool BeginEye(RwCamera *camera, int eye, CMatrix *eyeCamera, float *horizontalFov)
{
	if(!gFramePrepared || !camera || !eyeCamera || eye < 0 || eye >= ovrEye_Count)
		return false;

	const ovrFovPort &fov = gRenderFov[eye];
	RwV2d viewWindow = {
		(fov.LeftTan + fov.RightTan) * 0.5f,
		(fov.UpTan + fov.DownTan) * 0.5f
	};
	RwV2d viewOffset = {
		(fov.LeftTan - fov.RightTan) * 0.5f,
		(fov.DownTan - fov.UpTan) * 0.5f
	};

	RwCameraSetRaster(camera, gEye[eye].color);
	RwCameraSetZRaster(camera, gEye[eye].depth);
	RwCameraSetViewWindow(camera, &viewWindow);
	RwCameraSetViewOffset(camera, &viewOffset);
	if(gFirstPersonEnabled){
		RwCameraSetNearClipPlane(camera, 0.05f);
		CDraw::SetNearClipZ(0.05f);
	}
	// Coronas, particles and several other world-space effects are projected into
	// 2D using SCREEN_WIDTH/SCREEN_HEIGHT.  During VR rendering those values must
	// match the active eye raster, otherwise lamp glows are drawn in desktop pixel
	// coordinates and appear to slide through the world as the head turns.
	RsGlobal.width = gEye[eye].renderSize.w;
	RsGlobal.height = gEye[eye].renderSize.h;

	const ovrPosef &pose = gEyePose[eye];
	const ovrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const ovrVector3f localForward = { 0.0f, 0.0f, -1.0f };

	*eyeCamera = gBaseCamera;
	CVector forward = ToGameVector(Rotate(pose.Orientation, localForward));
	CVector up = ToGameVector(Rotate(pose.Orientation, localUp));
	forward.Normalise();
	CVector left = CrossProduct(up, forward);
	left.Normalise();
	up = CrossProduct(forward, left);
	up.Normalise();
	eyeCamera->GetRight() = left;
	eyeCamera->GetUp() = up;
	eyeCamera->GetForward() = forward;
	eyeCamera->GetPosition() = gBaseCamera.GetPosition() + ToGameVector(pose.Position);

	RwFrame *frame = RwCameraGetFrame(camera);
	RwMatrix *matrix = RwFrameGetMatrix(frame);
	// CCamera's CMatrix uses Right/Forward/Up exactly as the RW camera frame does
	// (Right is historically the camera's left vector in reVC).  CMatrix::CopyToRwMatrix
	// is for world entities and remaps Forward->RW Up and Up->RW At, so using it here
	// rotates the VR view around the wrong axes.
	*RwMatrixGetRight(matrix) = eyeCamera->GetRight();
	*RwMatrixGetUp(matrix) = eyeCamera->GetUp();
	*RwMatrixGetAt(matrix) = eyeCamera->GetForward();
	*RwMatrixGetPos(matrix) = eyeCamera->GetPosition();
	RwMatrixUpdate(matrix);
	RwFrameUpdateObjects(frame);
	RwFrameOrthoNormalize(frame);

	if(horizontalFov)
		*horizontalFov = RADTODEG(Atan(fov.LeftTan) + Atan(fov.RightTan));
	return true;
}

int GetStereoScalePercent()
{
	return (int)(gStereoScales[gStereoScaleIndex] * 100.0f + 0.5f);
}

bool IsStereoReversed()
{
	return gReverseStereo;
}

bool IsFirstPersonEnabled()
{
	return gFirstPersonEnabled;
}

bool IsHeadRelativeLocomotionActive()
{
	// Head-relative locomotion is implemented by the OpenXR input path.
	return false;
}

bool IsExperimentalHeadTurningActive()
{
	// Experimental head turning is implemented by the OpenXR input path.
	return false;
}

bool IsHeadDirectedLocomotionActive()
{
	// Head-directed body alignment is implemented by the OpenXR input path.
	return false;
}

bool IsStereoFrameActive()
{
	return false;
}

bool IsHeadBobbingEnabled()
{
	return true;
}

bool CanSkipDesktopGameplayRender()
{
	// Only suppress the fallback monitor render after at least one frame has
	// reached the Oculus compositor. If the session is lost, DestroySession
	// clears gWasSubmitting and the desktop renderer automatically returns.
	return gSession != nil && gWasSubmitting;
}

void ReportDesktopRenderSkip(bool skipping, bool vrReady, bool wideScreen,
	bool cutsceneProcessing, bool cutsceneRunning)
{
	// Detailed render-stage telemetry belongs to the OpenXR backend. Keep the
	// shared game loop link-compatible with the legacy LibOVR configuration.
	(void)skipping;
	(void)vrReady;
	(void)wideScreen;
	(void)cutsceneProcessing;
	(void)cutsceneRunning;
}

bool ShouldRenderImmersiveCarWheel()
{
	// Physical vehicle controls are implemented by the OpenXR input path.
	return false;
}

bool IsVrBikeHorizonLocked()
{
	// Preserve the legacy stable-horizon behavior when the shared camera path
	// asks for the newer OpenXR comfort option.
	return true;
}

bool ShouldRouteGameplayHudToVr()
{
	return gSession != nil && gWasSubmitting;
}

void NotifyTrackedWeaponFired(int hand, int weaponType)
{
	(void)hand;
	(void)weaponType;
}

static bool DrawEyeFxaa(EyeBuffer &eye, GLuint targetTexture)
{
	if(!gFxaaProgram || !gFxaaVertexArray)
		return false;

	int colorMode = 0;
	float blurColor[4] = { 0.0f, 0.0f, 0.0f, 30.0f/255.0f };
	float contrastMult[3] = { 1.0f, 1.0f, 1.0f };
	float contrastAdd[3] = { 0.0f, 0.0f, 0.0f };
#ifdef EXTENDED_COLOURFILTER
	if(gLightingEnabled && TheCamera.m_BlurType != MOTION_BLUR_NONE){
		if(CPostFX::EffectSwitch == CPostFX::POSTFX_NORMAL)
			colorMode = 1;
		else if(CPostFX::EffectSwitch == CPostFX::POSTFX_MOBILE)
			colorMode = 2;
	}
	const float red = (float)TheCamera.m_BlurRed;
	const float green = (float)TheCamera.m_BlurGreen;
	const float blue = (float)TheCamera.m_BlurBlue;
	blurColor[0] = red * CPostFX::Intensity / 255.0f;
	blurColor[1] = green * CPostFX::Intensity / 255.0f;
	blurColor[2] = blue * CPostFX::Intensity / 255.0f;
	contrastMult[0] = (red - 64.0f) / 256.0f + 1.4f;
	contrastMult[1] = (green - 64.0f) / 256.0f + 1.4f;
	contrastMult[2] = (blue - 64.0f) / 256.0f + 1.4f;
	contrastAdd[0] = red / 1536.0f - 0.05f;
	contrastAdd[1] = green / 1536.0f - 0.05f;
	contrastAdd[2] = blue / 1536.0f - 0.05f;
#endif

	rw::Raster *raster = ((rw::Raster*)eye.color)->parent;
	rw::gl3::Gl3Raster *native = PLUGINOFFSET(rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
	GLint oldDrawFramebuffer = 0, oldReadFramebuffer = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebuffer);
		return false;
	}

	GLint oldProgram = 0, oldVertexArray = 0, oldActiveTexture = 0, oldTexture = 0;
	GLint oldViewport[4] = {};
	GLboolean oldColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVertexArray);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
	glGetIntegerv(GL_VIEWPORT, oldViewport);
	glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
	const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
	const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
	const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean srgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glBindTexture(GL_TEXTURE_2D, native->texid);
	GLint oldMinFilter = GL_NEAREST, oldMagFilter = GL_NEAREST;
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &oldMinFilter);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &oldMagFilter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		gAntiAliasingEnabled ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
		gAntiAliasingEnabled ? GL_LINEAR : GL_NEAREST);

	glViewport(0, 0, eye.size.w, eye.size.h);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glUseProgram(gFxaaProgram);
	glBindVertexArray(gFxaaVertexArray);
	glUniform1i(gFxaaTextureUniform, 0);
	glUniform2f(gFxaaInverseSizeUniform,
		1.0f/(float)eye.renderSize.w, 1.0f/(float)eye.renderSize.h);
	glUniform1i(gFxaaEnabledUniform, gAntiAliasingEnabled ? 1 : 0);
	glUniform1i(gColorModeUniform, colorMode);
	glUniform4fv(gBlurColorUniform, 1, blurColor);
	glUniform3fv(gContrastMultUniform, 1, contrastMult);
	glUniform3fv(gContrastAddUniform, 1, contrastAdd);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, oldMinFilter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, oldMagFilter);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	glActiveTexture(oldActiveTexture);
	glBindVertexArray(oldVertexArray);
	glUseProgram(oldProgram);
	glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
	glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
	if(blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if(depthEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if(cullEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if(scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if(srgbEnabled) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebuffer);
	return true;
}

static bool UpdateHudChain(RwCamera *camera)
{
	if(!gGameplayHudVisible || !gHudChain || !gHudColor || !gHudDepth)
		return false;

	int index = 0;
	ovrResult result = ovr_GetTextureSwapChainCurrentIndex(gSession, gHudChain, &index);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetTextureSwapChainCurrentIndex(HUD)", result);
		return false;
	}
	GLuint targetTexture = 0;
	result = ovr_GetTextureSwapChainBufferGL(gSession, gHudChain, index, &targetTexture);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetTextureSwapChainBufferGL(HUD)", result);
		return false;
	}

	GLint oldDrawFramebuffer = 0, oldReadFramebuffer = 0;
	GLint oldViewport[4] = {};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
	glGetIntegerv(GL_VIEWPORT, oldViewport);
	RwRaster *oldColor = RwCameraGetRaster(camera);
	RwRaster *oldDepth = RwCameraGetZRaster(camera);
	const int oldScreenWidth = RsGlobal.width;
	const int oldScreenHeight = RsGlobal.height;
	RwCameraSetRaster(camera, gHudColor);
	RwCameraSetZRaster(camera, gHudDepth);
	RsGlobal.width = VR_HUD_WIDTH;
	RsGlobal.height = VR_HUD_HEIGHT;
	RwRGBA transparent = { 0, 0, 0, 0 };
	RwCameraClear(camera, &transparent, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
	if(!RwCameraBeginUpdate(camera)){
		RwCameraSetRaster(camera, oldColor);
		RwCameraSetZRaster(camera, oldDepth);
		RsGlobal.width = oldScreenWidth;
		RsGlobal.height = oldScreenHeight;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebuffer);
		glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
		return false;
	}
	RenderVrGameplayHud();
	RwCameraEndUpdate(camera);

	rw::Raster *raster = ((rw::Raster*)gHudColor)->parent;
	rw::gl3::Gl3Raster *native = PLUGINOFFSET(rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, native->fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(0, 0, VR_HUD_WIDTH, VR_HUD_HEIGHT,
		0, 0, VR_HUD_WIDTH, VR_HUD_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

	RwCameraSetRaster(camera, oldColor);
	RwCameraSetZRaster(camera, oldDepth);
	RsGlobal.width = oldScreenWidth;
	RsGlobal.height = oldScreenHeight;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebuffer);
	glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
	result = ovr_CommitTextureSwapChain(gSession, gHudChain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CommitTextureSwapChain(HUD)", result);
		return false;
	}
	return true;
}

bool SubmitStereoFrame(RwCamera *camera)
{
	EndPerfGpuQuery();
	if(!gFramePrepared || !gSession)
		return false;

	RestoreCamera(camera);

	const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	if(scissorEnabled)
		glDisable(GL_SCISSOR_TEST);

	for(int eye = 0; eye < ovrEye_Count; eye++){
		GLuint texture = 0;
		ovrResult result = ovr_GetTextureSwapChainBufferGL(gSession, gEye[eye].chain, -1, &texture);
		if(OVR_FAILURE(result)){
			LogOvrError("ovr_GetTextureSwapChainBufferGL", result);
			return false;
		}

		if(!DrawEyeFxaa(gEye[eye], texture)){
			rw::Raster *raster = ((rw::Raster*)gEye[eye].color)->parent;
			rw::gl3::Gl3Raster *native = PLUGINOFFSET(rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, native->fbo);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCopyFramebuffer);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			glBlitFramebuffer(0, 0, gEye[eye].renderSize.w, gEye[eye].renderSize.h,
				0, 0, gEye[eye].size.w, gEye[eye].size.h, GL_COLOR_BUFFER_BIT,
				gAntiAliasingEnabled ? GL_LINEAR : GL_NEAREST);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		}
		result = ovr_CommitTextureSwapChain(gSession, gEye[eye].chain);
		if(OVR_FAILURE(result)){
			LogOvrError("ovr_CommitTextureSwapChain", result);
			return false;
		}
	}
	const bool showHudLayer = UpdateHudChain(camera);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glDrawBuffer(GL_BACK);
	if(scissorEnabled)
		glEnable(GL_SCISSOR_TEST);

	ovrLayerEyeFov layer = {};
	layer.Header.Type = ovrLayerType_EyeFov;
	layer.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;
	for(int eye = 0; eye < ovrEye_Count; eye++){
		layer.ColorTexture[eye] = gEye[eye].chain;
		layer.Viewport[eye].Pos.x = 0;
		layer.Viewport[eye].Pos.y = 0;
		layer.Viewport[eye].Size = gEye[eye].size;
		layer.Fov[eye] = gRenderFov[eye];
		layer.RenderPose[eye] = gEyePose[eye];
	}
	layer.SensorSampleTime = gSensorSampleTime;

	ovrLayerQuad hudLayer = {};
	if(showHudLayer){
		hudLayer.Header.Type = ovrLayerType_Quad;
		hudLayer.Header.Flags = ovrLayerFlag_HeadLocked |
			ovrLayerFlag_TextureOriginAtBottomLeft | ovrLayerFlag_HighQuality;
		hudLayer.ColorTexture = gHudChain;
		hudLayer.Viewport.Pos.x = 0;
		hudLayer.Viewport.Pos.y = 0;
		hudLayer.Viewport.Size.w = VR_HUD_WIDTH;
		hudLayer.Viewport.Size.h = VR_HUD_HEIGHT;
		hudLayer.QuadPoseCenter.Orientation.w = 1.0f;
		hudLayer.QuadPoseCenter.Position.y = -0.22f;
		hudLayer.QuadPoseCenter.Position.z = -1.6f;
		hudLayer.QuadSize.x = 1.8f;
		hudLayer.QuadSize.y = 1.8f * (float)VR_HUD_HEIGHT / (float)VR_HUD_WIDTH;
	}

	if(gPerfRecording && !gDebugVisible)
		UpdateRuntimePerfStats();
	ovrLayerQuad debugLayer = {};
	const bool showDebugLayer = gDebugVisible && UpdateDebugChain();
	if(showDebugLayer){
		debugLayer.Header.Type = ovrLayerType_Quad;
		debugLayer.Header.Flags = ovrLayerFlag_HeadLocked | ovrLayerFlag_TextureOriginAtBottomLeft;
		debugLayer.ColorTexture = gDebugChain;
		debugLayer.Viewport.Pos.x = 0;
		debugLayer.Viewport.Pos.y = 0;
		debugLayer.Viewport.Size.w = VR_DEBUG_WIDTH;
		debugLayer.Viewport.Size.h = VR_DEBUG_HEIGHT;
		debugLayer.QuadPoseCenter.Orientation.w = 1.0f;
		debugLayer.QuadPoseCenter.Position.y = -0.18f;
		debugLayer.QuadPoseCenter.Position.z = -1.5f;
		debugLayer.QuadSize.x = 1.2f;
		debugLayer.QuadSize.y = 0.3f;
	}

	const ovrLayerHeader *layers[3];
	unsigned int layerCount = 0;
	layers[layerCount++] = &layer.Header;
	if(showHudLayer)
		layers[layerCount++] = &hudLayer.Header;
	if(showDebugLayer)
		layers[layerCount++] = &debugLayer.Header;
	ovrResult result = ovr_SubmitFrame(gSession, gFrameIndex++, nil, layers, layerCount);
	if(result == ovrError_DisplayLost){
		DestroySession();
		gRetryFrames = 300;
		return false;
	}
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_SubmitFrame", result);
		return false;
	}

	gWasSubmitting = true;
	return true;
}

bool SubmitCinemaFrame(RwCamera *camera)
{
	if(!camera || !EnsureSession() || !UpdateSessionStatus())
		return false;

	GLint viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, viewport);
	const int sourceWidth = viewport[2];
	const int sourceHeight = viewport[3];
	if(sourceWidth <= 0 || sourceHeight <= 0 || !EnsureCinemaChain(sourceWidth, sourceHeight))
		return false;

	GLuint texture = 0;
	ovrResult result = ovr_GetTextureSwapChainBufferGL(gSession, gCinemaChain, -1, &texture);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_GetTextureSwapChainBufferGL(cinema)", result);
		return false;
	}

	const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	if(scissorEnabled)
		glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + sourceWidth, viewport[1] + sourceHeight,
		0, 0, gCinemaSize.w, gCinemaSize.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glDrawBuffer(GL_BACK);
	if(scissorEnabled)
		glEnable(GL_SCISSOR_TEST);

	result = ovr_CommitTextureSwapChain(gSession, gCinemaChain);
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_CommitTextureSwapChain(cinema)", result);
		return false;
	}

	ovrLayerQuad layer = {};
	layer.Header.Type = ovrLayerType_Quad;
	layer.Header.Flags = ovrLayerFlag_HeadLocked | ovrLayerFlag_TextureOriginAtBottomLeft;
	layer.ColorTexture = gCinemaChain;
	layer.Viewport.Pos.x = 0;
	layer.Viewport.Pos.y = 0;
	layer.Viewport.Size = gCinemaSize;
	layer.QuadPoseCenter.Orientation.w = 1.0f;
	layer.QuadPoseCenter.Position.z = -2.0f;
	layer.QuadSize.x = 3.2f;
	layer.QuadSize.y = 3.2f * (float)gCinemaSize.h / (float)gCinemaSize.w;

	const ovrLayerHeader *layers[] = { &layer.Header };
	result = ovr_SubmitFrame(gSession, gFrameIndex++, nil, layers, 1);
	if(result == ovrError_DisplayLost){
		DestroySession();
		gRetryFrames = 300;
		return false;
	}
	if(OVR_FAILURE(result)){
		LogOvrError("ovr_SubmitFrame(cinema)", result);
		return false;
	}

	gWasSubmitting = true;
	return true;
}

void CancelStereoFrame(RwCamera *camera)
{
	EndPerfGpuQuery();
	RestoreCamera(camera);
}

void SetInactive()
{
	if(!gSession || !gWasSubmitting)
		return;
	ovr_SubmitFrame(gSession, gFrameIndex++, nil, nil, 0);
	gWasSubmitting = false;
}

void PrepareForGameShutdown()
{
	// Legacy LibOVR has no game-owned calibration/model references. Keep the
	// common shutdown hook available so core lifecycle ordering stays identical.
}

void Shutdown()
{
	if(gPerfRecording)
		StopPerfRecording();
	DestroySession();
	if(gRuntimeInitialized){
		ovr_Shutdown();
		gRuntimeInitialized = false;
		debug("[VR] Oculus runtime shut down\n");
	}
}
}

#endif
