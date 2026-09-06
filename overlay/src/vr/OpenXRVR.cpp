#include "common.h"

#ifdef GTA_VR_OPENXR

#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#ifdef RW_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#include <d3dcompiler.h>
#else
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef RW_D3D12
#include "../../vendor/librw/src/d3d12/rwd3d12impl.h"
#endif

#include "OculusVR.h"
#include "DLAA.h"
#include "FSR2.h"
#include "MenuNavigation.h"
#include "VrPadBindings.h"
#include "WristHudSettings.h"
#include "VrRagdoll.h"
#include "Camera.h"
#include "ControllerConfig.h"
#include "CarCtrl.h"
#include "CutsceneMgr.h"
#include "CutsceneObject.h"
#include "Bones.h"
#include "RwHelper.h"
#include "Frontend.h"
#include "Game.h"
#include "IniFile.h"
#include "ModelSets.h"
#include "Hud.h"
#include "Matrix.h"
#include "Pad.h"
#include "PlayerPed.h"
#include "PointLights.h"
#include "Population.h"
#include "Pools.h"
#include "Particle.h"
#include "ParticleObject.h"
#include "Shadows.h"
#include "ProjectileInfo.h"
#include "Renderer.h"
#include "Script.h"
#include "Timer.h"
#include "Timecycle.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Bike.h"
#include "VehicleModelInfo.h"
#include "Weather.h"
#include "World.h"
#include "Streaming.h"
#include "TxdStore.h"
#include "postfx.h"
#include "WeaponInfo.h"
#include "WeaponType.h"
#include "crossplatform.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <float.h>
#include <time.h>
#include <vector>

extern void RenderVrGameplayHud(void);
extern void RenderVrWristHudContents(unsigned int mask);
extern bool GetVrWristHudPanelRect(int panel, float *left, float *top,
	float *width, float *height);
extern bool CaptureMovieFrameRGBA(uint8 *destination, int destinationWidth, int destinationHeight);
extern int GetVrCheatCount(void);
extern const char *GetVrCheatName(int index);
extern bool ActivateVrCheat(int index);
extern bool CycleVrCheatSelection(int index, int direction);
extern int GetVrMissionCategoryCount(void);
extern const char *GetVrMissionCategoryName(int category);
extern int GetVrMissionCount(int category);
extern const char *GetVrMissionName(int category, int item);
extern bool ActivateVrMission(int category, int item);
extern const char *GetVrVehicleModelName(int model);
extern const char *GetVrCurrentWeaponName(void);
extern int GetVrCurrentWeaponType(void);
extern int GetVrWeaponTypeForSlot(int slot);
extern const char *GetVrWeaponName(int weaponType);
extern bool IsVrWeaponSlotOwned(int slot);
class CVehicle;
extern CVehicle *FindPlayerVehicle(void);

namespace OculusVR
{
static void UpdateCutsceneCameraInput(bool leftClick, bool rightClick);
bool IsTrackedDetonatorActiveInternal(int hand);
bool IsTrackedDetonatorHandReservedInternal(int hand);
uint32 GetTrackedDetonatorHandMaskInternal();
void ResetTrackedDetonatorInteraction(bool clearCharges);
bool BuildTrackedWeaponAimInternal(int hand, int weaponType, CVector *source,
	CVector *direction, bool applyOneHandSway);
void UpdateTrackedScopeState();
void ResetTrackedScopeState();
void ClearDroppedWeapon(int hand);
bool IsHandBusyWithReload(int hand);
void ResetTeleportInteraction();

namespace
{
bool IsWeaponSupportHandInternal(int hand);
void ClearWeaponSupportForHand(int hand);

enum eVrHolsterPoint
{
	HOLSTER_WAIST_LEFT = 0,
	HOLSTER_WAIST_RIGHT,
	HOLSTER_CHEST_LEFT,
	HOLSTER_CHEST_RIGHT,
	HOLSTER_CHEST_CENTER,
	HOLSTER_BACK_LEFT,
	HOLSTER_BACK_RIGHT,
	HOLSTER_POINT_COUNT
};

enum {
	EYE_COUNT = 2,
	VR_DEBUG_WIDTH = 1024,
	VR_DEBUG_HEIGHT = 512,
	VR_MENU_WIDTH = 1024,
	VR_MENU_HEIGHT = 768,
	VR_STARTUP_WIDTH = 1280,
	VR_STARTUP_HEIGHT = 720,
	VR_HUD_WIDTH = 1920,
	VR_HUD_HEIGHT = 1080,
	HOLSTER_MENU_ITEM_COUNT = HOLSTER_POINT_COUNT+1
};

// One-at-a-time weapon/holster calibration and arbitrary mission switching are
// developer tools, not player-facing cheats. Keep their indices entirely out
// of Release builds so the remaining cheat list stays dense and navigable.
#if defined(DEBUG) && !defined(FINAL)
enum {
	VR_CHEAT_CAL_HOLSTER = 0,
	VR_CHEAT_CAL_NEXT_WEAPON,
	VR_CHEAT_CAL_FINISH,
	VR_CHEAT_LOCAL_ITEM_COUNT
};
#else
enum { VR_CHEAT_LOCAL_ITEM_COUNT = 0 };
#endif
const float gRenderScaleOptions[] = {
	1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f,
	2.75f, 3.0f, 3.25f, 3.5f, 3.75f, 4.0f
};
// Start at the headset runtime's requested resolution. SteamVR and several
// vendor runtimes already apply their own supersampling, so stacking a 175%
// game-side default on top is too expensive for a safe first launch.
enum { VR_RENDER_SCALE_DEFAULT = 0 };
enum { DLAA_ACTIVATION_WARMUP_FRAMES = 30 };

enum eVrDrivingType
{
	VR_DRIVING_DEFAULT = 0,
	VR_DRIVING_IMMERSIVE,
	VR_DRIVING_MOTION,
	VR_DRIVING_TYPE_COUNT
};

enum eVrVehicleCategory
{
	VR_VEHICLE_CATEGORY_CAR = 0,
	VR_VEHICLE_CATEGORY_BIKE,
	VR_VEHICLE_CATEGORY_BOAT,
	VR_VEHICLE_CATEGORY_HELI,
	VR_VEHICLE_CATEGORY_COUNT,
	VR_VEHICLE_CATEGORY_INVALID = -1
};

enum eTemporalAaBackend
{
	TEMPORAL_AA_OFF = 0,
	TEMPORAL_AA_DLAA,
	TEMPORAL_AA_FSR2,
	TEMPORAL_AA_BACKEND_COUNT
};

enum eTemporalAaReleaseBits
{
	TEMPORAL_AA_RELEASE_DLAA = 1u << 0,
	TEMPORAL_AA_RELEASE_FSR2 = 1u << 1
};

enum eVrMainMenuItem
{
	VR_MAIN_GRAPHICS = 0,
	VR_MAIN_MODEL_SET,
	VR_MAIN_TRAFFIC_SETTINGS,
	VR_MAIN_HUD,
	VR_MAIN_HANDS,
	VR_MAIN_LASER,
	VR_MAIN_WEAPON_HAPTICS,
	VR_MAIN_WEAPON_HAPTICS_STRENGTH,
	VR_MAIN_HOLSTER_HIGHLIGHTS,
	VR_MAIN_MANUAL_RELOAD,
	VR_MAIN_SCOPE_AIM,
	VR_MAIN_AIM_DIRECTION,
	VR_MAIN_VEHICLE_SETTINGS,
	VR_MAIN_LOCOMOTION_SETTINGS,
	VR_MAIN_CHEATS,
	VR_MAIN_DEBUG,
	VR_MAIN_GRIP_LOCK,
	VR_MAIN_CALIBRATION,
	VR_MAIN_HOLSTERS,
	VR_MAIN_ABOUT,
	VR_MENU_ITEM_COUNT
};

enum eVrGraphicsMenuItem
{
	VR_GRAPHICS_RENDER_SCALE = 0,
	VR_GRAPHICS_VRS,
	VR_GRAPHICS_TEMPORAL_AA,
	VR_GRAPHICS_DLSS_MODE,
	VR_GRAPHICS_DLSS_NR_ENABLE,
	VR_GRAPHICS_DLSS_NR,
	VR_GRAPHICS_DLSS_NR_PASSES,
	VR_GRAPHICS_DLSS_NR_REGION,
	VR_GRAPHICS_DLSS_NR_TUNING,
	VR_GRAPHICS_EFFECTS,
	VR_GRAPHICS_JITTER,
	VR_GRAPHICS_FXAA,
	VR_GRAPHICS_COLOR,
	VR_GRAPHICS_SUN_GLARE,
	VR_GRAPHICS_OCCLUSION,
	VR_GRAPHICS_MIPMAPS,
	VR_GRAPHICS_FOLIAGE_SOFTNESS,
	VR_GRAPHICS_BACK,
	VR_GRAPHICS_MENU_ITEM_COUNT
};

enum eVrDlssTuningMenuItem
{
	VR_DLSS_TUNING_INTENSITY = 0,
	VR_DLSS_TUNING_STRUCTURE,
	VR_DLSS_TUNING_LOCAL_TONE,
	VR_DLSS_TUNING_GLOBAL_TONE,
	VR_DLSS_TUNING_STYLE,
	VR_DLSS_TUNING_AUTO_MASK,
	VR_DLSS_TUNING_RESET,
	VR_DLSS_TUNING_BACK,
	VR_DLSS_TUNING_MENU_ITEM_COUNT
};

enum eVrEffectsMenuItem
{
	VR_EFFECTS_MASTER = 0,
	VR_EFFECTS_LIGHTING_MENU,
	VR_EFFECTS_RAIN_MENU,
	VR_EFFECTS_REFLECTION_MENU,
	VR_EFFECTS_FOUNTAINS,
	VR_EFFECTS_BACK,
	VR_EFFECTS_MENU_ITEM_COUNT
};

enum eVrLightingMenuItem
{
	VR_LIGHTING_MODE = 0,
	VR_LIGHTING_INTENSITY,
	VR_LIGHTING_GLOW,
	VR_LIGHTING_MAX_LIGHTS,
	VR_LIGHTING_SHADOWS,
	VR_LIGHTING_DISTANCE_FOG,
	VR_LIGHTING_BACK,
	VR_LIGHTING_MENU_ITEM_COUNT
};

enum eVrRainMenuItem
{
	VR_RAIN_MODE = 0,
	VR_RAIN_INTENSITY,
	VR_RAIN_DENSITY,
	VR_RAIN_SURFACES,
	VR_RAIN_PUDDLE_COVERAGE,
	VR_RAIN_PUDDLE_EDGE,
	VR_RAIN_PUDDLE_RIPPLE,
	VR_RAIN_RESET,
	VR_RAIN_BACK,
	VR_RAIN_MENU_ITEM_COUNT
};

enum eVrReflectionMenuItem
{
	VR_REFLECTION_CARS = 0,
	VR_REFLECTION_OCEAN,
	VR_REFLECTION_PUDDLES,
	VR_REFLECTION_RESET,
	VR_REFLECTION_BACK,
	VR_REFLECTION_MENU_ITEM_COUNT
};

enum eVrWaterMenuItem {
	VR_WATER_ENABLE, VR_WATER_REFLECTION, VR_WATER_WAVES, VR_WATER_SPEED, VR_WATER_DISTORTION,
	VR_WATER_SHEEN, VR_WATER_GLINT, VR_WATER_SPARKS, VR_WATER_BACK, VR_WATER_ITEM_COUNT
};
enum eVrCarReflectionMenuItem {
	VR_CAR_REFLECTION_INTENSITY, VR_CAR_REFLECTION_SSR, VR_CAR_REFLECTION_DISTANCE,
	VR_CAR_REFLECTION_BACK, VR_CAR_REFLECTION_ITEM_COUNT
};

enum eVrModelMenuItem
{
	VR_MODEL_PRESET = 0,
	VR_MODEL_WORLD,
	VR_MODEL_VEGETATION,
	VR_MODEL_VEHICLES,
	VR_MODEL_PEDS,
	VR_MODEL_WEAPONS,
	VR_MODEL_BACK,
	VR_MODEL_MENU_ITEM_COUNT
};

enum eVrTrafficMenuItem
{
	VR_TRAFFIC_PEDESTRIANS = 0,
	VR_TRAFFIC_VEHICLES,
	VR_TRAFFIC_RAGDOLLS,
	VR_TRAFFIC_DEFAULTS,
	VR_TRAFFIC_BACK,
	VR_TRAFFIC_MENU_ITEM_COUNT
};

enum eVrVehicleMenuItem
{
	VR_VEHICLE_CAR_DRIVING_TYPE = 0,
	VR_VEHICLE_BIKE_DRIVING_TYPE,
	VR_VEHICLE_BOAT_DRIVING_TYPE,
	VR_VEHICLE_CAR_THIRD_PERSON,
	VR_VEHICLE_BIKE_THIRD_PERSON,
	VR_VEHICLE_BOAT_THIRD_PERSON,
	VR_VEHICLE_DEFAULT_BODY,
	VR_VEHICLE_GLOBAL_SEAT_HEIGHT,
	VR_VEHICLE_GLOBAL_SEAT_FORWARD,
	VR_VEHICLE_MODEL_SEAT_HEIGHT,
	VR_VEHICLE_MODEL_SEAT_FORWARD,
	VR_VEHICLE_MOTION_HAND,
	VR_VEHICLE_WHEEL_VISIBLE,
	VR_VEHICLE_BOAT_WHEEL_VISIBLE,
	VR_VEHICLE_MODEL_WHEEL_VISIBLE,
	VR_VEHICLE_WHEEL_HAND_PULL_BACK,
	VR_VEHICLE_HANDLE_CUBES,
	VR_VEHICLE_BIKE_LOCK_HORIZON,
	VR_VEHICLE_BIKE_THROTTLE,
	VR_VEHICLE_BIKE_VISUAL_LEAN,
	VR_VEHICLE_BIKE_VIEW_TILT,
	VR_VEHICLE_BIKE_THROW_RIDER,
	VR_VEHICLE_CALIBRATION,
	VR_VEHICLE_BACK,
	VR_VEHICLE_MENU_ITEM_COUNT
};

enum eVrLocomotionMenuItem
{
	VR_LOCOMOTION_MOVEMENT_MODE = 0,
	VR_LOCOMOTION_MOVEMENT_ORIENTATION,
	VR_LOCOMOTION_TURN_MODE,
	VR_LOCOMOTION_TURN_SENSITIVITY,
	VR_LOCOMOTION_SNAP_ANGLE,
	VR_LOCOMOTION_HEAD_BOBBING,
	VR_LOCOMOTION_CUTSCENES,
	VR_LOCOMOTION_CONTROLS,
	VR_LOCOMOTION_BACK,
	VR_LOCOMOTION_MENU_ITEM_COUNT
};

enum eVrControlsMenuItem
{
	VR_CONTROLS_LAYOUT = 0,
	VR_CONTROLS_FIRST_SOURCE,
	VR_CONTROLS_LAST_SOURCE = VR_CONTROLS_FIRST_SOURCE+VrPadBindings::SOURCE_COUNT-1,
	VR_CONTROLS_LOOK_BEHIND,
	VR_CONTROLS_CROUCH,
	VR_CONTROLS_RESET,
	VR_CONTROLS_BACK,
	VR_CONTROLS_ITEM_COUNT
};

enum eVrHudMenuItem
{
	VR_HUD_ENABLED = 0,
	VR_HUD_HORIZONTAL_SCALE,
	VR_HUD_SCALE,
	VR_HUD_OFFSET_X,
	VR_HUD_OFFSET_Y,
	VR_HUD_PRESET,
	VR_HUD_WRIST_PANEL,
	VR_HUD_WRIST_ENABLED,
	VR_HUD_WRIST_HAND,
	VR_HUD_WRIST_SIDE,
	VR_HUD_WRIST_PLACEMENT,
	VR_HUD_WRIST_GAZE,
	VR_HUD_WRIST_RANGE,
	VR_HUD_WRIST_VEHICLE,
	VR_HUD_CLASSIC_WEAPON,
	VR_HUD_CLASSIC_CLOCK,
	VR_HUD_BACK,
	VR_HUD_MENU_ITEM_COUNT
};

enum eVrWristPlacementItem {
	VR_WRIST_CONTEXT, VR_WRIST_SIDE, VR_WRIST_HAND,
	VR_WRIST_ALONG, VR_WRIST_ACROSS, VR_WRIST_LIFT,
	VR_WRIST_PITCH, VR_WRIST_YAW, VR_WRIST_ROLL, VR_WRIST_SIZE,
	VR_WRIST_RED, VR_WRIST_GREEN, VR_WRIST_BLUE,
	VR_WRIST_COPY, VR_WRIST_RESET, VR_WRIST_BACK, VR_WRIST_ITEM_COUNT
};

enum eVrMovementMode
{
	VR_MOVEMENT_SMOOTH = 0,
	VR_MOVEMENT_TELEPORT,
	VR_MOVEMENT_MODE_COUNT
};

enum eVrMovementOrientation
{
	VR_MOVEMENT_ORIENTATION_BODY = 0,
	VR_MOVEMENT_ORIENTATION_HEAD,
	VR_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL,
	VR_MOVEMENT_ORIENTATION_HEAD_DIRECTED,
	VR_MOVEMENT_ORIENTATION_COUNT
};

enum eVrTurnMode
{
	VR_TURN_SMOOTH = 0,
	VR_TURN_SNAP,
	VR_TURN_MODE_COUNT
};

enum eVrCalibrationMenuItem
{
	VR_CAL_AIM_ALIGNED = 0,
	VR_CAL_AIM_OFFSET_X,
	VR_CAL_AIM_OFFSET_Y,
	VR_CAL_AIM_OFFSET_Z,
	VR_CAL_AIM_ROT_X,
	VR_CAL_AIM_ROT_Y,
	VR_CAL_AIM_ROT_Z,
	VR_CAL_WEAPON_OFFSET_X,
	VR_CAL_WEAPON_OFFSET_Y,
	VR_CAL_WEAPON_OFFSET_Z,
	VR_CAL_WEAPON_ROT_X,
	VR_CAL_WEAPON_ROT_Y,
	VR_CAL_WEAPON_ROT_Z,
	VR_CAL_SUPPORT_GRIP_TYPE,
	VR_CAL_SUPPORT_OFFSET_X,
	VR_CAL_SUPPORT_OFFSET_Y,
	VR_CAL_SUPPORT_OFFSET_Z,
	VR_CAL_SUPPORT_ROT_X,
	VR_CAL_SUPPORT_ROT_Y,
	VR_CAL_SUPPORT_ROT_Z,
	VR_CAL_LASER,
	VR_CAL_BACK,
	VR_CALIBRATION_MENU_ITEM_COUNT
};

enum eVrBikeCalibrationMenuItem
{
	VR_BIKE_CAL_HAND = 0,
	VR_BIKE_CAL_OFFSET_X,
	VR_BIKE_CAL_OFFSET_Y,
	VR_BIKE_CAL_OFFSET_Z,
	VR_BIKE_CAL_ROT_X,
	VR_BIKE_CAL_ROT_Y,
	VR_BIKE_CAL_ROT_Z,
	VR_BIKE_CAL_GLOBAL_CENTER_X,
	VR_BIKE_CAL_GLOBAL_CENTER_Y,
	VR_BIKE_CAL_GLOBAL_CENTER_Z,
	VR_BIKE_CAL_GLOBAL_RADIUS,
	VR_BIKE_CAL_MODEL_CENTER_X,
	VR_BIKE_CAL_MODEL_CENTER_Y,
	VR_BIKE_CAL_MODEL_CENTER_Z,
	VR_BIKE_CAL_MODEL_RADIUS,
	VR_CAR_CAL_GLOBAL_PITCH,
	VR_CAR_CAL_GLOBAL_YAW,
	VR_CAR_CAL_GLOBAL_ROLL,
	VR_CAR_CAL_MODEL_PITCH,
	VR_CAR_CAL_MODEL_YAW,
	VR_CAR_CAL_MODEL_ROLL,
	VR_BIKE_CAL_WHEELIE_HEIGHT,
	VR_BIKE_CAL_STAND_HEIGHT,
	VR_BIKE_CAL_BACK,
	VR_BIKE_CALIBRATION_MENU_ITEM_COUNT
};

struct Swapchain
{
	XrSwapchain handle;
#ifdef RW_D3D12
	std::vector<XrSwapchainImageD3D12KHR> images;
#else
	std::vector<XrSwapchainImageOpenGLKHR> images;
#endif
	int width;
	int height;
	uint32_t acquiredIndex;
	bool acquired;

	Swapchain() : handle(XR_NULL_HANDLE), width(0), height(0), acquiredIndex(0), acquired(false) {}
};

struct EyeBuffer
{
	Swapchain swapchain;
	RwRaster *color;
	RwRaster *depth;
	int renderWidth;
	int renderHeight;

	EyeBuffer() : color(nil), depth(nil), renderWidth(0), renderHeight(0) {}
};

struct Actions
{
	XrActionSet set;
	XrPath hands[EYE_COUNT];
	XrAction stick;
	XrAction squeeze;
	XrAction trigger;
	XrAction gripPose;
	XrAction aimPose;
	XrSpace gripSpace[EYE_COUNT];
	XrSpace aimSpace[EYE_COUNT];
	XrAction a;
	XrAction b;
	XrAction x;
	XrAction y;
	XrAction stickClick;
	XrAction menu;
	XrAction haptic;

	Actions() : set(XR_NULL_HANDLE), stick(XR_NULL_HANDLE), squeeze(XR_NULL_HANDLE),
		trigger(XR_NULL_HANDLE), gripPose(XR_NULL_HANDLE), aimPose(XR_NULL_HANDLE), a(XR_NULL_HANDLE), b(XR_NULL_HANDLE), x(XR_NULL_HANDLE),
		y(XR_NULL_HANDLE), stickClick(XR_NULL_HANDLE), menu(XR_NULL_HANDLE),
		haptic(XR_NULL_HANDLE)
	{
		hands[0] = hands[1] = XR_NULL_PATH;
		gripSpace[0] = gripSpace[1] = XR_NULL_HANDLE;
		aimSpace[0] = aimSpace[1] = XR_NULL_HANDLE;
	}
};

XrInstance gInstance = XR_NULL_HANDLE;
XrSystemId gSystemId = XR_NULL_SYSTEM_ID;
XrSession gSession = XR_NULL_HANDLE;
XrSpace gLocalSpace = XR_NULL_HANDLE;
XrSpace gViewSpace = XR_NULL_HANDLE;
XrSpace gGameplaySpace = XR_NULL_HANDLE;
XrSessionState gSessionState = XR_SESSION_STATE_UNKNOWN;
bool gSessionRunning;
bool gExitRequested;
bool gGenericControllerExtensionEnabled;
bool gFrameBegun;
bool gFramePrepared;
bool gWasSubmitting;
bool gTouchWasConnected;
bool gTrackedWeaponTriggerPressed[EYE_COUNT];
bool gTrackedWeaponTriggerJustPressed[EYE_COUNT];
bool gTrackedWeaponTriggerJustReleased[EYE_COUNT];
// The remote-grenade controller is an auxiliary tracked prop rather than a
// Vice City inventory weapon.  Keeping it outside gHeldWeaponSlot preserves the
// projectile in one hand, permits several charges to be thrown in succession,
// and avoids replacing the camera which shares WEAPONSLOT_OTHER.
int gTrackedDetonatorHand = -1;
bool gTrackedDetonatorWasActive[EYE_COUNT];
bool gTrackedDetonatorWaitForTriggerRelease[EYE_COUNT];
bool gTrackedDetonatorTriggerJustPressed[EYE_COUNT];
bool gTrackedThrowablePreviewActive[EYE_COUNT];
struct ManualReloadState
{
	bool active;
	bool requested;
	bool movedAwayFromSpot;
	int weaponHand;
	int magazineHand;
	int slot;
	int weaponType;
	ULONGLONG grabbedAt;

	ManualReloadState() : active(false), requested(false),
		movedAwayFromSpot(false), weaponHand(-1), magazineHand(-1),
		slot(-1), weaponType(-1), grabbedAt(0) {}
};
ManualReloadState gManualReload[EYE_COUNT];
bool gManualReloadGripDown[EYE_COUNT];
uint32 gWeaponHolsterMask;
XrVector3f gHolsterHeadForwardTracking;
bool gHolsterHeadForwardValid;
// Six configurable equipment points plus one dedicated throwable point replace
// the old implicit one-position-per-slot layout. Configurable values are Vice
// City inventory slots, or -1 for an empty point.
int gHolsterPointWeaponSlot[HOLSTER_POINT_COUNT] = {
	WEAPONSLOT_SNIPER, WEAPONSLOT_SUBMACHINEGUN,
	WEAPONSLOT_SHOTGUN, WEAPONSLOT_MELEE,
	WEAPONSLOT_PROJECTILE, WEAPONSLOT_HEAVY, WEAPONSLOT_RIFLE
};
const char *gHolsterPointNames[HOLSTER_POINT_COUNT] = {
	// CCamera::GetRight stores screen-left. The legacy front-point offsets use
	// negative values first, so their old LEFT/RIGHT captions were reversed even
	// though the physical locations were stable. Correct display names only;
	// preserve point indices and INI keys so existing loadouts do not move sides.
	"WAIST RIGHT", "WAIST LEFT", "CHEST RIGHT", "CHEST LEFT",
	"CHEST CENTER - THROWABLE", "BACK LEFT", "BACK RIGHT"
};
const char *gHolsterPointSettingNames[HOLSTER_POINT_COUNT] = {
	"HolsterWaistLeftSlot", "HolsterWaistRightSlot",
	"HolsterChestLeftSlot", "HolsterChestRightSlot",
	nil, "HolsterBackLeftSlot", "HolsterBackRightSlot"
};
const int gHolsterSlotChoices[] = {
	-1, WEAPONSLOT_MELEE, WEAPONSLOT_HANDGUN, WEAPONSLOT_SHOTGUN,
	WEAPONSLOT_SUBMACHINEGUN, WEAPONSLOT_RIFLE, WEAPONSLOT_HEAVY,
	WEAPONSLOT_SNIPER, WEAPONSLOT_OTHER
};
// The calibration preview deliberately excludes inventory pseudo-weapons and
// engine-only projectiles. Every entry below has a physical VR representation
// and can safely live in one of Vice City's native inventory slots.
const int gHolsterCalibrationWeaponTypes[] = {
	WEAPONTYPE_SCREWDRIVER, WEAPONTYPE_GOLFCLUB,
	WEAPONTYPE_NIGHTSTICK, WEAPONTYPE_KNIFE,
	WEAPONTYPE_BASEBALLBAT, WEAPONTYPE_HAMMER,
	WEAPONTYPE_CLEAVER, WEAPONTYPE_MACHETE,
	WEAPONTYPE_KATANA, WEAPONTYPE_CHAINSAW,
	WEAPONTYPE_GRENADE, WEAPONTYPE_DETONATOR_GRENADE,
	WEAPONTYPE_TEARGAS, WEAPONTYPE_MOLOTOV,
	WEAPONTYPE_COLT45, WEAPONTYPE_PYTHON,
	WEAPONTYPE_SHOTGUN, WEAPONTYPE_SPAS12_SHOTGUN,
	WEAPONTYPE_STUBBY_SHOTGUN,
	WEAPONTYPE_TEC9, WEAPONTYPE_UZI,
	WEAPONTYPE_SILENCED_INGRAM, WEAPONTYPE_MP5,
	WEAPONTYPE_M4, WEAPONTYPE_RUGER,
	WEAPONTYPE_SNIPERRIFLE, WEAPONTYPE_LASERSCOPE,
	WEAPONTYPE_ROCKETLAUNCHER, WEAPONTYPE_FLAMETHROWER,
	WEAPONTYPE_M60, WEAPONTYPE_MINIGUN,
	WEAPONTYPE_CAMERA
};
struct HolsterCalibrationCheatState
{
	bool active;
	CEntity *owner;
	int ownerPoolHandle;
	CWeapon originalWeapons[TOTAL_WEAPON_SLOTS];
	int originalCurrentSlot;
	int originalSelectedSlot;
	int selectedPoint;
	int nextWeaponIndex;
	int previewWeaponType;
	int previewSlot;
	bool snapshotModelRefHeld[TOTAL_WEAPON_SLOTS][2];

	HolsterCalibrationCheatState() : active(false), owner(nil),
		ownerPoolHandle(-1),
		originalCurrentSlot(WEAPONSLOT_UNARMED),
		originalSelectedSlot(WEAPONSLOT_UNARMED), selectedPoint(0),
		nextWeaponIndex(0), previewWeaponType(-1), previewSlot(-1) {}
};
HolsterCalibrationCheatState gHolsterCalibrationCheat;
bool FinishHolsterCalibrationLoadout();
void DiscardHolsterCalibrationLoadout(const char *reason);
bool ValidateHolsterCalibrationLifecycle();
int gWeaponHolsterSelection[EYE_COUNT] = { -1, -1 };
bool gWeaponHolsterGripDown[EYE_COUNT];
int gHeldWeaponSlot[EYE_COUNT] = { -1, -1 };
// Scripted mounted-gun sequences (notably Phnom Penh '86) attach Tommy to an
// entity instead of putting him in a normal vehicle seat.  The script owns the
// temporary weapon, while this state only mirrors that exact inventory slot
// into the tracked right hand for the duration of the attachment.
bool gAttachedMissionWeaponForced;
int gAttachedMissionWeaponSlot = -1;
// A supporting hand is deliberately not given a second copy of the inventory
// slot.  The primary hand remains the sole owner of the weapon/trigger while
// this relation only supplies the two-handed pose.
int gWeaponSupportHand[EYE_COUNT] = { -1, -1 };
CMatrix gTrackedWeaponRenderMatrix[EYE_COUNT];
int gTrackedWeaponRenderMatrixSlot[EYE_COUNT] = { -1, -1 };
CMatrix gTrackedWeaponContactMatrix[EYE_COUNT];
int gTrackedWeaponContactMatrixSlot[EYE_COUNT] = { -1, -1 };
int gTrackedWeaponContactMatrixType[EYE_COUNT] = { -1, -1 };
uint32 gTrackedWeaponContactMatrixFrame[EYE_COUNT] = { 0, 0 };
int gDroppedWeaponSlot[EYE_COUNT] = { -1, -1 };
CMatrix gDroppedWeaponMatrix[EYE_COUNT];
CVector gDroppedWeaponStartPosition[EYE_COUNT];
CVector gDroppedWeaponGravityUp[EYE_COUNT];
CVector gDroppedWeaponLinearVelocity[EYE_COUNT];
CVector gDroppedWeaponAngularVelocity[EYE_COUNT];
ULONGLONG gDroppedWeaponStartTime[EYE_COUNT];
int gActiveTrackedFireHand = -1;
int gActiveTrackedFireWeaponType = -1;
bool gActiveTrackedFireAimValid;
CVector gActiveTrackedFireSource;
CVector gActiveTrackedFireDirection;
bool gTrackedAimCacheValid[EYE_COUNT];
uint32 gTrackedAimCacheFrame[EYE_COUNT] = { 0xFFFFFFFFU, 0xFFFFFFFFU };
int gTrackedAimCacheWeaponType[EYE_COUNT] = { -1, -1 };
CVector gTrackedAimCacheSource[EYE_COUNT];
CVector gTrackedAimCacheDirection[EYE_COUNT];
int gTrackedScopeHand = -1;
int gTrackedScopeWeaponType = -1;
int gTrackedScopeCandidateHand = -1;
int gTrackedScopeCandidateWeaponType = -1;
ULONGLONG gTrackedScopeCandidateSince;
ULONGLONG gTrackedScopeInvalidSince;
bool gTrackedScopeReticleTargetValid;
CVector gTrackedScopeReticleTarget;
struct PhysicalMeleeStrike
{
	bool pending;
	int slot;
	int weaponType;
	CVector sweepStart;
	CVector sweepEnd;
	CVector rootStart;
	CVector rootEnd;
	float speed;
	uint32 frame;

	PhysicalMeleeStrike() : pending(false), slot(-1), weaponType(-1),
		speed(0.0f), frame(0) {}
};
PhysicalMeleeStrike gPhysicalMeleeStrike[EYE_COUNT];
struct PhysicalMeleeMotion
{
	bool valid;
	bool armed;
	int slot;
	int weaponType;
	CVector previousWorldPoint;
	CVector previousWorldRoot;
	CVector previousTrackingPoint;
	CVector previousTrackingRoot;
	uint32 lastStrikeTime;
	uint32 calmSinceTime;
	uint32 previousFrame;
	bool usedContactMatrix;
	int supportHand;
	bool strikeInProgress;
	float strikePeakSpeed;
	uint32 strikeContinueUntil;

	PhysicalMeleeMotion() : valid(false), armed(false), slot(-1),
		weaponType(-1), previousWorldPoint(0.0f, 0.0f, 0.0f),
		previousWorldRoot(0.0f, 0.0f, 0.0f),
		previousTrackingPoint(0.0f, 0.0f, 0.0f),
		previousTrackingRoot(0.0f, 0.0f, 0.0f),
		lastStrikeTime(0), calmSinceTime(0), previousFrame(0),
		usedContactMatrix(false), supportHand(-1), strikeInProgress(false),
		strikePeakSpeed(0.0f), strikeContinueUntil(0) {}
};
PhysicalMeleeMotion gPhysicalMeleeMotion[EYE_COUNT];
bool gPhysicalMeleeFreshGrab[EYE_COUNT] = { false, false };
bool gPhysicalGameplayWasAvailable;
bool gTouchPerfShortcutDown;
bool gTouchDebugShortcutDown;
bool gTouchWeatherShortcutDown;
bool gTouchRecenterShortcutDown;
bool gTouchSpsShortcutDown;
bool gTouchVrsShortcutDown;
bool gTouchVrMenuShortcutDown;
bool gRecenterRequested;
bool gVehicleViewStateValid;
bool gVehicleViewWasActive;
bool gFirstPersonEnabled = true;
bool gTrackingCenterValid;
bool gDebugVisible;
ULONGLONG gDlssProfileToastUntil;
bool gDlssProfileToastPresented;
// FXAA is the universal first-run path. Temporal AA remains available from the
// headset menu, but must be explicitly enabled by the player.
int gTemporalAaBackend = TEMPORAL_AA_OFF;
// Streamline owns the D3D12 entry points at link time, so even with DLAA off
// every frame goes through its interposed device once slInit runs. On a small
// number of driver/runtime combinations that delivers black frames with no
// error anywhere. StreamlineEnabled=0 in vr_settings.ini skips slInit, which
// leaves the interposer as a plain passthrough to the system D3D12 -- the
// pre-0.5.0 render path. The cost: DLAA cannot be enabled until the flag is
// restored and the game restarted.
bool gStreamlineEnabled = true;
// FlatMode=1 skips OpenXR initialization and renders the desktop world pass.
// It supports monitor-only render-feature testing and systems without OpenXR.
bool gFlatModeEnabled = false;
// FlatFirstPerson=1 (default) keeps the game's native first-person look-around
// camera engaged permanently while FlatMode is on, movement included. The V
// key toggles it against classic third person at runtime.
bool gFlatFirstPersonEnabled = true;
bool gFlatViewKeyWasDown;
uint32 gTemporalAaReleasePending;
bool gDlaaStereoActivationReady;
bool gDlaaStereoActivationFailed;
bool gFsr2StereoActivationFailed;
uint32 gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
bool gAntiAliasingEnabled = true;
bool gLightingEnabled = true;
int gSunGlarePercent = 100;
bool gEffectsEnabled = false;
bool gGenerateMipmapsRequested = true;
bool gGenerateMipmapsAtStartup = true;
bool gDistanceFogEnabled = true;
int gFoliageSoftness = 0;
int gCarReflectionPercent = 100;
int gWaterReflectionPercent = 80;
bool gModernWaterEnabled = true;
int gWaterSpeedPercent = 100;
int gWaterDistortionPercent = 100;
int gWaterSheenPercent = 0;
int gWaterGlintPercent = 0;
int gWaterSparksPercent = 0;
int gCarReflectionSsrPercent = 100;
int gCarReflectionSsrDistance = 0;
int gOceanWavePercent = 100;
int gPuddleReflectionPercent = 75;
int gPuddleCoveragePercent = 25;
int gPuddleEdgeSoftnessPercent = 25;
int gPuddleRipplePercent = 100;
bool gScreenSpaceReflectionFailed;
int gDynamicLights;
int gDynamicLightIntensityPercent = 100;
int gDynamicLightGlowPercent = 100;
int gDynamicLightMax = 4;
bool gGameplayHudVisible = true;
bool gVrHandsEnabled = true;
bool gWeaponLaserEnabled = false;
bool gWeaponHapticsEnabled = true;
int gWeaponHapticsStrengthPercent = 100;
bool gWeaponHolsterHighlightsEnabled = false;
bool gManualReloadEnabled = false;
bool gPhysicalScopeAimEnabled = true;
bool gHeadAimEnabled;
bool gWeaponGripLockEnabled;
int gCarDrivingType;
int gBikeDrivingType;
int gMotionSteeringHand = 1;
// Calibration markers are intentionally opt-in during normal driving. They are
// always rendered while the vehicle calibration page is open.
bool gBikeHandleHighlightsEnabled;
bool gCarHandleHighlightsEnabled, gBoatHandleHighlightsEnabled;
int gWheelHandPullBackMm;
bool gImmersiveCarWheelVisible = true;
bool gImmersiveBoatWheelVisible = true;
int gBoatDrivingType;
bool gBikeLockHorizonEnabled = true;
bool gFullStereoSinglePass = true;
bool gVrMenuVisible;
bool gVrGraphicsMenuVisible;
bool gVrDlssTuningMenuVisible;
bool gVrEffectsMenuVisible;
bool gVrLightingMenuVisible;
bool gVrRainMenuVisible;
bool gVrReflectionMenuVisible;
bool gVrModelMenuVisible;
bool gVrTrafficMenuVisible;
bool gVrVehicleMenuVisible;
bool gVrLocomotionMenuVisible;
bool gVrHudMenuVisible;
bool gVrHolsterMenuVisible;
bool gVrCalibrationMenuVisible;
bool gVrBikeCalibrationMenuVisible;
bool gVrAboutVisible;
bool gVrAboutFirstRun;
bool gVrAboutFirstRunPending;
bool gVrAboutStepBaselineValid;
CVector gVrAboutStepBaseline;
CVector gVrAboutStepPrevious;
uint32 gVrAboutStepControllableFrames;
uint32 gVrAboutStepLastGameFrame;
bool gVrAboutStepMovementInputSeen;
bool gVrAboutDismissArmed;
bool gVrAboutReleaseGate;
bool gVrAboutWasRendered;
bool gCheatMenuVisible;
bool gCheatMenuOpenedFromVrMenu;
bool gVrMissionMenuVisible;
bool gVrMenuVerticalDown;
bool gVrMenuHorizontalDown;
VrMenuNavigation gVrMenuNavigation;
VrMenuNavigation gCheatMenuHorizontalNavigation;
bool gVrMenuSelectDown;
bool gVrMenuBackDown;
bool gVrMenuDecreaseDown;
bool gVrMenuIncreaseDown;
ULONGLONG gVrMenuDecreaseRepeatAt;
ULONGLONG gVrMenuIncreaseRepeatAt;
ULONGLONG gVrMenuDecreaseHoldStartedAt;
ULONGLONG gVrMenuIncreaseHoldStartedAt;
bool gVrSettingsLoaded;
bool gRenderScaleChangePending;
int gVrMenuSelection;
int gVrGraphicsMenuSelection;
int gVrDlssTuningMenuSelection;
int gVrEffectsMenuSelection;
int gVrLightingMenuSelection;
int gVrRainMenuSelection;
int gVrReflectionMenuSelection;
int gVrReflectionPage = 0; // 0 shared, 1 cars, 2 ocean
int gVrModelMenuSelection;
int gVrTrafficMenuSelection;
int gVrVehicleMenuSelection;
int gVrLocomotionMenuSelection;
int gVrHudMenuSelection;
int gVrHolsterMenuSelection;
int gVrCalibrationMenuSelection;
int gVrBikeCalibrationMenuSelection;
int gCheatMenuSelection;
int gVrMissionCategory = -1;
int gVrMissionCategorySelection;
int gVrMissionMenuSelection;
int gHudWidthPercent = 100;
int gHudScalePercent = 130;
int gHudOffsetXCm = 0;
int gHudOffsetYCm = 0;
WristHudSettings gWristHud;
unsigned int gWristHudLayerMask = 0;
bool gWristHudFailed = false;
XrCompositionLayerQuad gWristHudLayers[WristHudSettings::PANEL_COUNT];
int gTrafficPedPercent = 135;
int gTrafficCarPercent = 135;
// Calibration integers are stored in half-units: 1 = 0.5 cm or 0.5 degree.
// This keeps INI values exact while allowing sub-centimetre/sub-degree tuning.
enum { WEAPON_CALIBRATION_VALUE_SCALE = 2 };
int gWeaponOffsetXCm = 0;
int gWeaponOffsetYCm = 2;
int gWeaponOffsetZCm = -10;
int gWeaponAimOffsetXCm = 0;
int gWeaponAimOffsetYCm = 8;
int gWeaponAimOffsetZCm = 0;
int gWeaponAimRotationXDeg = 0;
int gWeaponAimRotationYDeg = 0;
int gWeaponAimRotationZDeg = 0;
int gWeaponRotationXDeg = 0;
int gWeaponRotationYDeg = 18;
int gWeaponRotationZDeg = 14;
int gMovementMode;
int gMovementOrientation = VR_MOVEMENT_ORIENTATION_HEAD;
int gTurnMode;
int gTurnSensitivityPercent = 100;
int gHeadSteeringSensitivityPercent = 50;
int gSnapTurnAngleDegrees = 30;
bool gHeadBobbingEnabled;
int gCutsceneMode;
bool gStickCrouch;
bool gStickLookBehind;
VrPadBindings::Bindings gVrPadBindings;
bool gVrControlsMenuVisible;
int gVrControlsMenuSelection;
int gCutsceneCamera;
char gCutsceneCameraScene[32];
bool gCutsceneCycleDown, gCutsceneStoreDown;
CEntity *gCutsceneCameraActor;
bool gDefaultDrivingBodyVisible = true;
bool gVehicleThirdPerson[VR_VEHICLE_CATEGORY_COUNT];
bool gBikeManualThrottle;
bool gBikeViewFollowingTilt;
bool gBikeRiderCanBeThrown = true;
int gBikeVisualLeanPercent = 50;
int gDefaultSeatHeightCm[VR_VEHICLE_CATEGORY_COUNT] = { 15, 15, 15, 15 };
int gDefaultSeatDistanceCm[VR_VEHICLE_CATEGORY_COUNT];
float gHeadLocomotionYaw;
bool gHeadLocomotionPoseValid;
bool gSnapTurnStickLatched;
bool gTeleportInputDown;
bool gTeleportPreviewActive;
bool gTeleportTargetValid;
CVector gTeleportTarget;
enum { VR_TELEPORT_MAX_POINTS = 41 };
CVector gTeleportTrajectory[VR_TELEPORT_MAX_POINTS];
int gTeleportTrajectoryCount;
enum { VR_BIKE_MODEL_COUNT = 6 };
const int gVrBikeModels[VR_BIKE_MODEL_COUNT] = {
	MI_ANGEL, MI_FREEWAY, MI_PCJ600,
	MI_FAGGIO, MI_PIZZABOY, MI_SANCHEZ
};
const char *gVrBikeNames[VR_BIKE_MODEL_COUNT] = {
	"ANGEL", "FREEWAY", "PCJ 600", "FAGGIO", "PIZZA BOY", "SANCHEZ"
};
struct BikeHandleCalibration
{
	int offsetX, offsetY, offsetZ;
	int rotationX, rotationY, rotationZ;
	bool valid;
	BikeHandleCalibration() : offsetX(0), offsetY(0), offsetZ(0),
		rotationX(0), rotationY(0), rotationZ(0), valid(false) {}
};
BikeHandleCalibration gBikeHandleCalibration[VR_BIKE_MODEL_COUNT][EYE_COUNT];
enum { VR_CAR_MODEL_COUNT = MI_LAST_VEHICLE-MI_FIRST_VEHICLE+1 };
enum {
	VR_CAR_WHEEL_DEFAULT_RADIUS_CM = 18,
	VR_CAR_WHEEL_MIN_RADIUS_CM = 12,
	VR_CAR_WHEEL_MAX_RADIUS_CM = 32,
	// Wheel orientation is stored in half-degrees, matching the existing VR
	// calibration precision. Global and per-model layers add together, then the
	// effective orientation is limited to +/-90 degrees on each local axis.
	VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG = 180
};
struct VehicleViewCalibration
{
	int seatDistanceCm;
	int seatHeightCm;
	int defaultSeatHeightCm, defaultSeatDistanceCm;
	int wheelCenterXCm, wheelCenterYCm, wheelCenterZCm;
	// Cars use an absolute radius. Zero means that this model inherits the
	// active Classic/Modern car-category radius. Motorcycles retain the legacy
	// additive radius adjustment in wheelRadiusCm below.
	int carWheelRadiusCm;
	int wheelRadiusCm;
	int carWheelPitchHalfDeg, carWheelYawHalfDeg, carWheelRollHalfDeg;
	// -1 inherits the shared global choice; 0 hides this model.
	// This belongs to the model-set-specific VehicleView section.
	int carWheelVisibilityOverride;
	bool valid;
	VehicleViewCalibration() : seatDistanceCm(0), seatHeightCm(0),
		defaultSeatHeightCm(0), defaultSeatDistanceCm(0),
		wheelCenterXCm(0), wheelCenterYCm(0), wheelCenterZCm(0),
		carWheelRadiusCm(0), wheelRadiusCm(0),
		carWheelPitchHalfDeg(0), carWheelYawHalfDeg(0),
		carWheelRollHalfDeg(0), carWheelVisibilityOverride(-1),
		valid(false) {}
};
VehicleViewCalibration gVehicleViewCalibration[VR_CAR_MODEL_COUNT];
struct VehicleCategoryCalibration
{
	int seatDistanceCm;
	int seatHeightCm;
	int wheelCenterXCm, wheelCenterYCm, wheelCenterZCm;
	// Absolute default radius for cars. Other vehicle categories continue to
	// use wheelRadiusCm as an additive handlebar-radius adjustment.
	int carWheelRadiusCm;
	int wheelRadiusCm;
	int carWheelPitchHalfDeg, carWheelYawHalfDeg, carWheelRollHalfDeg;
	bool valid;
	VehicleCategoryCalibration() : seatDistanceCm(0), seatHeightCm(15),
		wheelCenterXCm(0), wheelCenterYCm(0), wheelCenterZCm(0),
		carWheelRadiusCm(VR_CAR_WHEEL_DEFAULT_RADIUS_CM),
		wheelRadiusCm(0), carWheelPitchHalfDeg(0),
		carWheelYawHalfDeg(0), carWheelRollHalfDeg(0), valid(false) {}
};
VehicleCategoryCalibration
	gVehicleCategoryCalibration[VR_VEHICLE_CATEGORY_COUNT];
struct BuiltInVehicleCategoryDefaults
{
	ModelSets::eModelSet modelSet;
	int category;
	int value[10];
};

// Effective category baselines from the captured profile. Field order is seat
// distance/height, wheel center XYZ, car radius, bike-handle radius, rotation XYZ.
static const BuiltInVehicleCategoryDefaults
gBuiltInVehicleCategoryDefaults[] = {
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_CAR,
		{ 0,0, -1,4,8, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_BIKE,
		{ 0,0, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_BOAT,
		{ 0,15, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_HELI,
		{ 0,15, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_CAR,
		{ 0,5, 0,0,0, 17,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_BIKE,
		{ 0,0, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_BOAT,
		{ 0,10, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_HELI,
		{ 0,10, 0,0,0, 18,0, 0,0,0 } },
};

bool GetBuiltInVehicleCategoryCalibration(ModelSets::eModelSet modelSet,
	int category, VehicleCategoryCalibration *calibration)
{
	if(!calibration)
		return false;
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInVehicleCategoryDefaults); i++){
		const BuiltInVehicleCategoryDefaults &entry =
			gBuiltInVehicleCategoryDefaults[i];
		if(entry.modelSet != modelSet || entry.category != category)
			continue;
		calibration->seatDistanceCm = entry.value[0];
		calibration->seatHeightCm = entry.value[1];
		calibration->wheelCenterXCm = entry.value[2];
		calibration->wheelCenterYCm = entry.value[3];
		calibration->wheelCenterZCm = entry.value[4];
		calibration->carWheelRadiusCm = entry.value[5];
		calibration->wheelRadiusCm = entry.value[6];
		calibration->carWheelPitchHalfDeg = entry.value[7];
		calibration->carWheelYawHalfDeg = entry.value[8];
		calibration->carWheelRollHalfDeg = entry.value[9];
		calibration->valid = true;
		return true;
	}
	if(modelSet == ModelSets::MODEL_SET_XBOX)
		return GetBuiltInVehicleCategoryCalibration(
			ModelSets::MODEL_SET_CLASSIC, category, calibration);
	return false;
}
const char *gVehicleCategorySettingPrefixes[VR_VEHICLE_CATEGORY_COUNT] = {
	"Car", "Bike", "Boat", "Heli"
};
const char *gVehicleCategoryNames[VR_VEHICLE_CATEGORY_COUNT] = {
	"CAR", "BIKE", "BOAT", "HELICOPTER"
};

ModelSets::eModelSet GetActiveVehicleModelSet()
{
	return ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES);
}

bool IsModernVehicleModelSetActive()
{
	return GetActiveVehicleModelSet() == ModelSets::MODEL_SET_MODERN;
}

bool IsReplacementVehicleModelSetActive()
{
	return GetActiveVehicleModelSet() != ModelSets::MODEL_SET_CLASSIC;
}

const char *GetVehicleModelSetPrefix(ModelSets::eModelSet modelSet)
{
	if(modelSet == ModelSets::MODEL_SET_XBOX)
		return "Xbox";
	if(modelSet == ModelSets::MODEL_SET_MODERN)
		return "Modern";
	return "";
}

ModelSets::eModelSet GetActiveWeaponModelSet()
{
	return ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_WEAPONS);
}

bool IsModernWeaponModelSetActive()
{
	return GetActiveWeaponModelSet() == ModelSets::MODEL_SET_MODERN;
}

void GetVehicleCategorySettingKey(int category, const char *suffix,
	ModelSets::eModelSet modelSet, char *key)
{
	if(!key)
		return;
	if(category < 0 || category >= VR_VEHICLE_CATEGORY_COUNT || !suffix){
		key[0] = '\0';
		return;
	}
	sprintf(key, "%s%s%s", GetVehicleModelSetPrefix(modelSet),
		gVehicleCategorySettingPrefixes[category], suffix);
}

int ReadVehicleCategorySetting(int category, const char *suffix,
	int fallback, int minimum, int maximum, const char *path)
{
	char key[64];
	const ModelSets::eModelSet modelSet = GetActiveVehicleModelSet();
	if(modelSet != ModelSets::MODEL_SET_CLASSIC){
		GetVehicleCategorySettingKey(category, suffix,
			ModelSets::MODEL_SET_CLASSIC, key);
		fallback = (int)(int32)GetPrivateProfileIntA("VR", key,
			fallback, path);
	}
	GetVehicleCategorySettingKey(category, suffix, modelSet, key);
	return Min(Max((int)(int32)GetPrivateProfileIntA("VR", key,
		fallback, path), minimum), maximum);
}

bool ReadVrProfileInt(const char *section, const char *name,
	const char *path, int *value)
{
	if(!section || !name || !path || !value)
		return false;
	char text[32];
	if(GetPrivateProfileStringA(section, name, "", text,
	   sizeof(text), path) == 0)
		return false;
	*value = atoi(text);
	return true;
}

int ReadCarWheelCategoryRadius(const char *path, int classicDefault,
	int modernDefault)
{
	char classicKey[64], modernKey[64];
	GetVehicleCategorySettingKey(VR_VEHICLE_CATEGORY_CAR,
		"WheelRadiusV2Cm", ModelSets::MODEL_SET_CLASSIC, classicKey);
	int classicRadius;
	bool classicConfigured = ReadVrProfileInt("VR", classicKey, path,
		&classicRadius);
	if(!classicConfigured){
		// The old key was an additive adjustment around the release-default
		// 18 cm radius. Fold only that scalar into the new absolute value; the
		// obsolete per-hand CarWheelV2 sockets are deliberately not migrated.
		char legacyKey[64];
		GetVehicleCategorySettingKey(VR_VEHICLE_CATEGORY_CAR,
			"WheelRadiusCm", ModelSets::MODEL_SET_CLASSIC, legacyKey);
		int legacyAdjustment;
		classicConfigured = ReadVrProfileInt("VR", legacyKey, path,
			&legacyAdjustment);
		classicRadius = classicConfigured ?
			VR_CAR_WHEEL_DEFAULT_RADIUS_CM+legacyAdjustment : classicDefault;
	}
	classicRadius = Min(Max(classicRadius, VR_CAR_WHEEL_MIN_RADIUS_CM),
		VR_CAR_WHEEL_MAX_RADIUS_CM);
	const ModelSets::eModelSet modelSet = GetActiveVehicleModelSet();
	if(modelSet == ModelSets::MODEL_SET_CLASSIC)
		return classicRadius;

	GetVehicleCategorySettingKey(VR_VEHICLE_CATEGORY_CAR,
		"WheelRadiusV2Cm", modelSet, modernKey);
	int modernRadius;
	if(ReadVrProfileInt("VR", modernKey, path, &modernRadius))
		return Min(Max(modernRadius, VR_CAR_WHEEL_MIN_RADIUS_CM),
			VR_CAR_WHEEL_MAX_RADIUS_CM);
	// Modern inherits the new Classic value unless an old Modern-only radius
	// adjustment exists, in which case preserve its previous effective size.
	char legacyModernKey[64];
	GetVehicleCategorySettingKey(VR_VEHICLE_CATEGORY_CAR,
		"WheelRadiusCm", modelSet, legacyModernKey);
	int legacyModernAdjustment;
	if(ReadVrProfileInt("VR", legacyModernKey, path,
	   &legacyModernAdjustment))
		return Min(Max(VR_CAR_WHEEL_DEFAULT_RADIUS_CM+
			legacyModernAdjustment, VR_CAR_WHEEL_MIN_RADIUS_CM),
			VR_CAR_WHEEL_MAX_RADIUS_CM);
	return classicConfigured ? classicRadius :
		(modelSet == ModelSets::MODEL_SET_MODERN ? modernDefault :
		 classicDefault);
}
struct BikeLeanCalibration
{
	int wheelieHeightCm;
	int standHeightCm;
	bool valid;
	BikeLeanCalibration() : wheelieHeightCm(20), standHeightCm(20),
		valid(false) {}
};
BikeLeanCalibration gBikeLeanCalibration[VR_BIKE_MODEL_COUNT];
int gBikeCalibrationEditHand;
bool gBikeHandleGrabbed[EYE_COUNT];
bool gBikeHandleGripDown[EYE_COUNT];
float gImmersiveBikeSteering;
float gImmersiveBikePhysicalAngle;
float gImmersiveBikeDesiredAngle;
float gImmersiveBikeSteeringOverflow;
uint32 gBikeHandleUnavailableMask;
float gImmersiveBikeThrottle;
float gImmersiveBikeLean;
bool gBikeThrottleReferenceValid;
bool gBikeThrottleGestureActive;
XrQuaternionf gBikeThrottleReferenceOrientation;
float gBikeThrottleRawTwistAngle;
bool gBikeThrottleTwistValid;
uint32 gBikeThrottleCaptureCount;
bool gBikeLeanReferenceValid[EYE_COUNT];
float gBikeLeanReferenceTrackingY[EYE_COUNT];
int gBikeLeanGestureState;
float gBikeHandleDistance[EYE_COUNT] = { 1000.0f, 1000.0f };
bool gCarWheelGrabbed[EYE_COUNT];
bool gCarWheelGripDown[EYE_COUNT];
float gCarWheelDistance[EYE_COUNT] = { 1000.0f, 1000.0f };
float gImmersiveCarSteering;
float gImmersiveCarPhysicalAngle;
float gImmersiveCarDesiredAngle;
float gImmersiveCarSteeringOverflow;
uint32 gCarWheelUnavailableMask;
bool gImmersiveCarHornPressed;
bool gCarHornContact[EYE_COUNT];
bool gCarHornArmed[EYE_COUNT];
float gCarHornPreviousDistance[EYE_COUNT] = { 1000.0f, 1000.0f };
bool gVrRadioButtonDown;
bool gVrRadioChangeJustPressed;
CVehicle *gMotionSteeringVehicle;
float gMotionVehicleSteering;
float gMotionVehiclePhysicalAngle;
bool gMotionSteeringReferenceValid;
float gMotionSteeringReferenceHeading;
// Two real hands retain the proven v0.4.1 labelled left-to-right chord. Car
// one-hand steering stores a tangent coordinate. Bike one-hand steering freezes
// the controller and mirrored chord in OpenXR tracking space for the duration of
// the grab, so vehicle lean and handlebar animation cannot feed back into input.
// This shared state only owns transition continuity and hard-stop anti-windup.
struct ImmersiveSteeringChordState
{
	CVehicle *vehicle;
	uint32 realHandMask;
	bool valid;
	bool referenceActive;
	bool rebaseOnNextValid;
	float referenceAngle;
	float oneHandReferenceCoordinate;
	float continuousAngle;
	float lastDelta;
	uint32 pointValidMask;
	uint32 captureCount;
	ImmersiveSteeringChordState() : vehicle(nil), realHandMask(0),
		valid(false), referenceActive(false), rebaseOnNextValid(false),
		referenceAngle(0.0f), oneHandReferenceCoordinate(0.0f),
		continuousAngle(0.0f),
		lastDelta(0.0f), pointValidMask(0), captureCount(0) {}
};
ImmersiveSteeringChordState gCarSteeringChordState;
ImmersiveSteeringChordState gBikeSteeringChordState;
struct BikeOneHandSteeringState
{
	CVehicle *vehicle;
	uint32 realHandMask;
	bool valid;
	CVector referenceHandTracking;
	CVector seedChordTracking;
	CVector rightTracking;
	CVector forwardTracking;
	float referencePhysicalAngle;
	BikeOneHandSteeringState() : vehicle(nil), realHandMask(0), valid(false),
		referenceHandTracking(0.0f, 0.0f, 0.0f),
		seedChordTracking(0.0f, 0.0f, 0.0f),
		rightTracking(0.0f, 0.0f, 0.0f),
		forwardTracking(0.0f, 0.0f, 0.0f),
		referencePhysicalAngle(0.0f) {}
};
BikeOneHandSteeringState gBikeOneHandSteeringState;
bool gNewGameCinemaHold;
ULONGLONG gNewGameCinemaHoldStartedAt;
int gActiveWeaponCalibration = -1;
int gActiveWeaponCalibrationHand = -1;
int gCalibrationEditHand = 1;
// One canonical lock is stored per weapon/model-set section.  RIGHT owns the
// aim pose and LEFT remains its automatic mirror; model placement itself is
// still independent for each hand.
bool gWeaponAimAligned = false;
struct WeaponCalibration
{
	int offsetX, offsetY, offsetZ;
	int aimOffsetX, aimOffsetY, aimOffsetZ;
	int aimRotationX, aimRotationY, aimRotationZ;
	int rotationX, rotationY, rotationZ;
	bool valid;
	WeaponCalibration() : offsetX(0), offsetY(2), offsetZ(-10),
		aimOffsetX(0), aimOffsetY(8), aimOffsetZ(0),
		aimRotationX(0), aimRotationY(0), aimRotationZ(0),
		rotationX(0), rotationY(18), rotationZ(14), valid(false) {}
};
WeaponCalibration gWeaponCalibration[EYE_COUNT][WEAPONTYPE_TOTALWEAPONS];
CMatrix gWeaponAimRelative[ModelSets::MODEL_SET_COUNT]
	[WEAPONTYPE_TOTALWEAPONS];
bool gWeaponAimRelativeLoaded[ModelSets::MODEL_SET_COUNT]
	[WEAPONTYPE_TOTALWEAPONS] = {};
bool gWeaponAimRelativeEnabled[ModelSets::MODEL_SET_COUNT]
	[WEAPONTYPE_TOTALWEAPONS] = {};

// OpenXR exposes the same right-handed aim space for both controllers:
// +X is local right, +Y is local up and -Z is local forward.  Reflecting the
// calibrated RIGHT ray through the controller's local YZ plane therefore
// negates lateral translation plus rotations about Y and Z.  The visible
// weapon pose is deliberately not copied here; replacement models can still
// require independent LEFT offset/rotation values.
void ApplyMirroredWeaponAim(const WeaponCalibration &source,
	WeaponCalibration &mirrored)
{
	mirrored.aimOffsetX = -source.aimOffsetX;
	mirrored.aimOffsetY = source.aimOffsetY;
	mirrored.aimOffsetZ = source.aimOffsetZ;
	mirrored.aimRotationX = source.aimRotationX;
	mirrored.aimRotationY = -source.aimRotationY;
	mirrored.aimRotationZ = -source.aimRotationZ;
}

void CopyWeaponAim(const WeaponCalibration &source, WeaponCalibration &target)
{
	target.aimOffsetX = source.aimOffsetX;
	target.aimOffsetY = source.aimOffsetY;
	target.aimOffsetZ = source.aimOffsetZ;
	target.aimRotationX = source.aimRotationX;
	target.aimRotationY = source.aimRotationY;
	target.aimRotationZ = source.aimRotationZ;
}

bool IsWeaponAimCalibrationValue(const char *name)
{
	return name != nil && strncmp(name, "Aim", 3) == 0;
}

int gSupportGripOffsetXCm = 0;
int gSupportGripOffsetYCm = 60;
int gSupportGripOffsetZCm = -10;
int gSupportGripRotationXDeg = 0;
int gSupportGripRotationYDeg = 180*WEAPON_CALIBRATION_VALUE_SCALE;
int gSupportGripRotationZDeg = 0;
enum {
	// v1/v3 offsets are controller-local. v2 was a short-lived relative wrist
	// correction. v4 first bound the socket to the visible weapon, but selected
	// an axis hemisphere from the live controller pose. v5 is fully deterministic:
	// X=lateral, Y=barrel, Z=weapon top in the calibrated model frame.
	SUPPORT_GRIP_POSE_VERSION = 5,
	SUPPORT_GRIP_MODEL_BOUND_V4 = 4,
	SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION = 3
};
enum eVrSupportGripType
{
	VR_SUPPORT_GRIP_MAGAZINE = 0,
	VR_SUPPORT_GRIP_FROM_BELOW,
	VR_SUPPORT_GRIP_TYPE_COUNT
};
const char *gSupportGripTypeNames[VR_SUPPORT_GRIP_TYPE_COUNT] = {
	"MAGAZINE GRIP", "SUPPORT FROM BELOW"
};
int gSupportGripType = VR_SUPPORT_GRIP_MAGAZINE;
int gSupportGripPoseVersion = SUPPORT_GRIP_POSE_VERSION;
int gActiveSupportGripCalibration = -1;
struct SupportGripCalibration
{
	int offsetX, offsetY, offsetZ;
	int rotationX, rotationY, rotationZ;
	int gripType;
	int poseVersion;
	bool valid;
	SupportGripCalibration() : offsetX(0), offsetY(60), offsetZ(-10),
		rotationX(0), rotationY(180*WEAPON_CALIBRATION_VALUE_SCALE),
		rotationZ(0),
		gripType(VR_SUPPORT_GRIP_MAGAZINE),
		poseVersion(SUPPORT_GRIP_POSE_VERSION), valid(false) {}
};
SupportGripCalibration gSupportGripCalibration[EYE_COUNT][WEAPONTYPE_TOTALWEAPONS];

// The unprefixed/right profile is the sole support-grip calibration. Reflect
// its socket and axial rotations through the weapon's local YZ plane for a
// left-hand primary. Legacy LEFT keys remain readable as a migration source,
// but are never independently edited again.
void ApplyMirroredSupportGrip(const SupportGripCalibration &source,
	SupportGripCalibration &mirrored)
{
	if(source.poseVersion >= SUPPORT_GRIP_POSE_VERSION){
		// V5 position is canonical (X=lateral), while the wrist deliberately keeps
		// the v4-compatible authoring basis so the user's calibrated palms survive
		// migration. Mirror those two contracts independently.
		mirrored.offsetX = -source.offsetX;
		mirrored.offsetY = source.offsetY;
		mirrored.offsetZ = source.offsetZ;
		mirrored.rotationX = -source.rotationX;
		mirrored.rotationY = source.rotationY;
		mirrored.rotationZ = -source.rotationZ;
	}else if(source.poseVersion == SUPPORT_GRIP_MODEL_BOUND_V4){
		// The short-lived v4 contract stored lateral travel in Z and authored its
		// wrist in the historical permuted weapon basis. Keep old profiles stable
		// until they are explicitly migrated to v5.
		mirrored.offsetX = source.offsetX;
		mirrored.offsetY = source.offsetY;
		mirrored.offsetZ = -source.offsetZ;
		mirrored.rotationX = -source.rotationX;
		mirrored.rotationY = -source.rotationY;
		mirrored.rotationZ = source.rotationZ;
	}else{
		// Legacy profiles live in each controller's already mirrored raw frame.
		mirrored.offsetX = source.offsetX;
		mirrored.offsetY = source.offsetY;
		mirrored.offsetZ = source.offsetZ;
		mirrored.rotationX = source.rotationX;
		mirrored.rotationY = -source.rotationY;
		mirrored.rotationZ = -source.rotationZ;
	}
	mirrored.gripType = source.gripType;
	mirrored.poseVersion = source.poseVersion;
	mirrored.valid = source.valid;
}

struct BuiltInHandCalibration
{
	int main[12];
	int support[3];
};

struct BuiltInWeaponDefaults
{
	int weaponType;
	BuiltInHandCalibration hand[EYE_COUNT];
};

struct BuiltInWeaponSetDefaults
{
	int weaponType;
	ModelSets::eModelSet modelSet;
	BuiltInHandCalibration hand[EYE_COUNT];
};

struct BuiltInSupportGripDefaults
{
	int weaponType;
	ModelSets::eModelSet modelSet;
	int offset[3];
	int rotation[3];
	int gripType;
	int poseVersion;
};

struct BuiltInWeaponAimRelativeDefaults
{
	int weaponType;
	ModelSets::eModelSet modelSet;
	int transform[12];
};

// Stable release baselines captured from the project's calibrated
// vr_settings.ini.  All values are absolute half-centimetres / half-degrees;
// user INI values replace individual defaults and are never added to them.
// Hand order is LEFT, RIGHT.
static const BuiltInWeaponDefaults gBuiltInWeaponDefaults[] = {
	{ WEAPONTYPE_UNARMED, {
		{ { -12,0,-8, 0,8,0, 0,0,0, 0,8,12 }, { 0,60,-10 } },
		{ { -12,0,-8, 0,8,0, 0,0,0, 0,8,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_BRASSKNUCKLE, {
		{ { 0,0,-12, 0,0,0, 0,0,0, -2,0,34 }, { 0,60,-10 } },
		{ { 0,0,-12, 0,0,0, 0,0,0, -2,0,34 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SCREWDRIVER, {
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_GOLFCLUB, {
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_NIGHTSTICK, {
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KNIFE, {
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } },
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_BASEBALLBAT, {
		{ { 0,-5,-10, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,-5,-11, 2,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_HAMMER, {
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } },
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CLEAVER, {
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } },
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MACHETE, {
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } },
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KATANA, {
		{ { 24,-4,-10, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 24,-4,-10, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CHAINSAW, {
		{ { 8,0,-13, 0,0,0, 0,0,0, 0,44,-9 }, { 0,60,-10 } },
		{ { 8,0,-13, 0,0,0, 0,0,0, 0,44,-9 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_GRENADE, {
		{ { 0,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_DETONATOR_GRENADE, {
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MOLOTOV, {
		{ { 24,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 24,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_COLT45, {
		{ { 0,-3,-7, 2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } },
		{ { 0,-3,-7, -2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_PYTHON, {
		{ { 0,-6,-11, 2,15,10, 7,-4,-2, 0,0,0 }, { 0,60,-10 } },
		{ { 0,-6,-8, -2,15,10, 7,4,2, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SHOTGUN, {
		{ { 3,1,-10, 4,12,0, 0,0,0, 2,19,11 }, { 0,60,-10 } },
		{ { 3,1,-10, -4,12,0, 0,0,0, 2,19,11 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SPAS12_SHOTGUN, {
		{ { 0,0,-11, 3,18,20, 17,-4,-1, 0,0,8 }, { 11,60,-13 } },
		{ { 0,0,-11, -3,18,20, 17,4,1, 0,0,8 }, { 9,60,-12 } } } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, {
		{ { 0,-5,-10, 10,14,0, -14,-1,0, 2,33,11 }, { 0,60,-16 } },
		{ { 0,-5,-10, -10,14,0, -14,1,0, 2,33,11 }, { 0,60,-17 } } } },
	{ WEAPONTYPE_TEC9, {
		{ { 1,-4,-9, 0,12,0, 0,0,0, -2,8,6 }, { 0,60,-10 } },
		{ { 1,-4,-9, 0,12,0, 0,0,0, -2,8,6 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_UZI, {
		{ { 0,-3,-10, 2,8,0, -10,2,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, -2,8,0, -10,-2,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SILENCED_INGRAM, {
		{ { 0,0,-16, 8,14,0, 5,-2,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,0,-16, -8,14,0, 5,2,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MP5, {
		{ { -4,-7,-8, 5,23,20, 13,-7,-1, 0,0,8 }, { 0,45,-19 } },
		{ { -4,-7,-8, -5,23,20, 13,7,1, 0,0,8 }, { 0,39,-13 } } } },
	{ WEAPONTYPE_M4, {
		{ { 5,0,-12, 6,25,65, 0,0,0, 0,0,11 }, { -1,41,-20 } },
		{ { 5,0,-12, -6,25,65, 0,0,0, 0,0,11 }, { -1,41,-20 } } } },
	{ WEAPONTYPE_RUGER, {
		{ { -1,0,-11, 0,20,0, 0,0,0, 0,8,11 }, { 0,51,-7 } },
		{ { -1,0,-11, 0,20,0, 0,0,0, 0,8,11 }, { 0,50,-6 } } } },
	{ WEAPONTYPE_SNIPERRIFLE, {
		{ { -2,-5,-9, 0,10,100, 0,0,0, 0,10,0 }, { 0,60,-10 } },
		{ { -2,-5,-9, 0,10,100, 0,0,0, 0,10,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_LASERSCOPE, {
		{ { 5,0,-11, 6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } },
		{ { 5,0,-11, -6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_ROCKETLAUNCHER, {
		{ { -2,26,-11, 8,21,71, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { -2,26,-11, -8,21,71, 0,0,0, 0,0,0 }, { 0,45,-10 } } } },
	{ WEAPONTYPE_FLAMETHROWER, {
		{ { 21,1,-10, 0,-7,100, 0,0,0, -3,56,8 }, { 0,60,-10 } },
		{ { 21,1,-10, 0,-7,100, 0,0,0, -3,56,8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_M60, {
		{ { 0,0,-12, 9,18,0, 7,5,-75, -10,8,12 }, { 5,60,0 } },
		{ { 0,0,-12, -9,18,0, 7,-5,75, -10,8,12 }, { 5,60,0 } } } },
	{ WEAPONTYPE_MINIGUN, {
		{ { 25,6,-18, 6,-19,47, -10,-5,0, 0,67,4 }, { 0,60,-10 } },
		{ { 25,6,-18, -6,-19,47, -10,5,0, 0,67,4 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_DETONATOR, {
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CAMERA, {
		{ { -3,0,-39, 0,8,0, 0,0,0, 0,37,12 }, { 0,60,-10 } },
		{ { -3,0,-39, 0,8,0, 0,0,0, 0,37,12 }, { 0,60,-10 } } } },
};

// Model-set-specific weapon/model and laser baselines from the same captured
// profile. Missing Modern entries deliberately fall back to Classic.
static const BuiltInWeaponSetDefaults gBuiltInWeaponSetDefaults[] = {
	{ WEAPONTYPE_SCREWDRIVER, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_GOLFCLUB, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_NIGHTSTICK, ModelSets::MODEL_SET_MODERN, {
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KNIFE, ModelSets::MODEL_SET_MODERN, {
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } },
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_HAMMER, ModelSets::MODEL_SET_MODERN, {
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } },
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CLEAVER, ModelSets::MODEL_SET_MODERN, {
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } },
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MACHETE, ModelSets::MODEL_SET_MODERN, {
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } },
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_COLT45, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-3,-7, 2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } },
		{ { 0,-3,-7, -2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_PYTHON, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-6,-11, 3,13,8, 7,-4,-2, 0,0,0 }, { 0,60,-10 } },
		{ { 0,-6,-11, -3,13,8, 7,4,2, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SHOTGUN, ModelSets::MODEL_SET_MODERN, {
		{ { 3,1,-10, 0,7,0, 0,0,0, 2,19,11 }, { 0,60,-10 } },
		{ { 3,1,-10, 0,7,0, 0,0,0, 2,19,11 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-5,-10, 4,9,0, -15,-6,0, 2,33,11 }, { 0,60,-16 } },
		{ { 0,-5,-10, -4,9,0, -15,6,0, 2,33,11 }, { 0,60,-17 } } } },
	{ WEAPONTYPE_TEC9, ModelSets::MODEL_SET_MODERN, {
		{ { 1,-4,-9, 0,16,0, 7,0,0, -2,8,6 }, { 0,60,-10 } },
		{ { 1,-4,-9, 0,16,0, 7,0,0, -2,8,6 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_UZI, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-3,-10, 2,9,0, -10,-1,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, -2,9,0, -10,1,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SILENCED_INGRAM, ModelSets::MODEL_SET_MODERN, {
		{ { 0,0,-16, 1,10,-10, 5,5,0, 0,0,0 }, { 0,60,-10 } },
		{ { -3,-4,-11, -1,10,-10, 5,-5,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MP5, ModelSets::MODEL_SET_MODERN, {
		{ { -4,-7,-8, 3,14,20, 13,-7,-1, 0,0,8 }, { 0,45,-19 } },
		{ { -4,-7,-8, -3,14,20, 13,7,1, 0,0,8 }, { 0,39,-13 } } } },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_MODERN, {
		{ { 5,0,-12, 2,16,48, 2,0,0, 0,0,11 }, { -1,41,-20 } },
		{ { 1,-5,-8, -2,16,48, 2,0,0, 0,0,11 }, { -1,41,-20 } } } },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_MODERN, {
		{ { -1,0,-11, 2,8,87, 1,-3,0, 0,8,11 }, { 0,51,-7 } },
		{ { 1,-6,-12, -2,8,87, 1,3,0, 0,8,11 }, { 0,50,-6 } } } },
	{ WEAPONTYPE_LASERSCOPE, ModelSets::MODEL_SET_MODERN, {
		{ { 5,0,-11, 6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } },
		{ { -1,-8,-11, -6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_ROCKETLAUNCHER, ModelSets::MODEL_SET_MODERN, {
		{ { -2,26,-11, 4,15,71, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { -2,26,-11, -4,15,71, 0,0,0, 0,0,0 }, { 0,45,-10 } } } },
	{ WEAPONTYPE_M60, ModelSets::MODEL_SET_MODERN, {
		{ { 0,0,-12, 11,11,41, 7,5,-75, -10,8,12 }, { 5,60,0 } },
		{ { 0,0,-12, -11,11,41, 7,-5,75, -10,8,12 }, { 5,60,0 } } } },
};

// Release 0.5.0 support poses captured from the user profile. Authored gun
// sockets are RIGHT-canonical v5 values; the nine pre-v5 Modern placeholder
// profiles retain their effective v3 contract exactly. LEFT is always derived
// by reflection. Modern entries override Classic only where explicitly stored.
static const BuiltInSupportGripDefaults gBuiltInSupportGripDefaults[] = {
	{ WEAPONTYPE_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -13,80,-6 }, { -32,320,-360 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_SPAS12_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -13,79,-8 }, { 0,297,-354 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_STUBBY_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -12,76,-9 }, { -13,339,353 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_MP5, ModelSets::MODEL_SET_CLASSIC,
		{ -7,39,-10 }, { -64,360,32 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_CLASSIC,
		{ -15,75,-6 }, { -66,359,-292 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_CLASSIC,
		{ -14,92,-8 }, { -54,360,-295 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_SNIPERRIFLE, ModelSets::MODEL_SET_CLASSIC,
		{ -15,71,-4 }, { -47,300,-332 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_LASERSCOPE, ModelSets::MODEL_SET_CLASSIC,
		{ -5,47,-8 }, { -70,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_ROCKETLAUNCHER, ModelSets::MODEL_SET_CLASSIC,
		{ -7,12,-1 }, { -75,-360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_FLAMETHROWER, ModelSets::MODEL_SET_CLASSIC,
		{ -30,48,0 }, { 0,234,0 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_M60, ModelSets::MODEL_SET_CLASSIC,
		{ -12,62,-2 }, { 0,360,347 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_MINIGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -37,53,3 }, { -22,268,22 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_SCREWDRIVER, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_GOLFCLUB, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_NIGHTSTICK, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_KNIFE, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_HAMMER, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_CLEAVER, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_MACHETE, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_COLT45, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_PYTHON, ModelSets::MODEL_SET_MODERN,
		{ 0,60,-10 }, { 0,360,0 }, VR_SUPPORT_GRIP_MAGAZINE,
		SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION },
	{ WEAPONTYPE_SPAS12_SHOTGUN, ModelSets::MODEL_SET_MODERN,
		{ -18,79,-8 }, { 0,297,-354 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_MODERN,
		{ -11,98,-4 }, { -29,359,-360 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_MODERN,
		{ -12,70,-6 }, { -74,360,-332 }, VR_SUPPORT_GRIP_FROM_BELOW,
		SUPPORT_GRIP_POSE_VERSION },
};

// Captured model-to-aim relationships for profiles explicitly marked aligned.
// Values use WEAPON_AIM_ALIGNMENT_SCALE and are model-set specific: an absent
// Modern entry stays unaligned rather than silently borrowing Classic geometry.
static const BuiltInWeaponAimRelativeDefaults
gBuiltInWeaponAimRelativeDefaults[] = {
	{ WEAPONTYPE_UNARMED, ModelSets::MODEL_SET_CLASSIC,
		{ 104528,994522,0, 992099,-104274,69756, -69374,7292,997564,
		  268066,11963,148867 } },
	{ WEAPONTYPE_COLT45, ModelSets::MODEL_SET_CLASSIC,
		{ -74559,996618,34560, 987856,69078,139173, -136315,-44517,989665,
		  234192,38643,114832 } },
	{ WEAPONTYPE_PYTHON, ModelSets::MODEL_SET_CLASSIC,
		{ 35895,999239,-15290, 997527,-34899,61011, -60431,17442,998020,
		  349579,23315,119034 } },
	{ WEAPONTYPE_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ 98698,994969,-17213, 981745,-94531,165048, -162590,33189,986135,
		  255859,6790,118008 } },
	{ WEAPONTYPE_SPAS12_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ 105429,994421,-3472, 983571,-103763,147719, -146535,18989,989023,
		  359253,5951,160431 } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ 109327,993888,-15293, 980619,-105325,165205, -162585,33058,986141,
		  264252,9232,142319 } },
	{ WEAPONTYPE_TEC9, ModelSets::MODEL_SET_CLASSIC,
		{ 51112,998541,17410, 996197,-52209,69756, -70564,-13778,997412,
		  290192,28412,106146 } },
	{ WEAPONTYPE_UZI, ModelSets::MODEL_SET_CLASSIC,
		{ 104571,994517,-1217, 992104,-104232,69746, -69236,8501,997564,
		  259521,11566,95398 } },
	{ WEAPONTYPE_SILENCED_INGRAM, ModelSets::MODEL_SET_CLASSIC,
		{ 17436,999848,761, 998896,-17452,43613, -43619,0,999048,
		  271057,36835,107750 } },
	{ WEAPONTYPE_MP5, ModelSets::MODEL_SET_CLASSIC,
		{ 131115,991366,-1760, 985045,-130079,112992, -111788,16548,993594,
		  358337,-11101,175938 } },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_CLASSIC,
		{ 95846,995396,0, 995396,-95846,0, 0,0,1000000,
		  600159,-27634,129997 } },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_CLASSIC,
		{ 95236,995417,8705, 992972,-95612,69756, -70269,-2001,997526,
		  267944,8278,139193 } },
	{ WEAPONTYPE_SNIPERRIFLE, ModelSets::MODEL_SET_CLASSIC,
		{ 95846,995396,0, 991609,-95481,87156, -86754,8354,996195,
		  763123,-43320,162184 } },
	{ WEAPONTYPE_LASERSCOPE, ModelSets::MODEL_SET_CLASSIC,
		{ 113203,993572,0, 993572,-113203,0, 0,0,1000000,
		  772858,-62958,129985 } },
	{ WEAPONTYPE_ROCKETLAUNCHER, ModelSets::MODEL_SET_CLASSIC,
		{ 0,1000000,0, 1000000,0,0, 0,0,1000000,
		  480042,40009,110001 } },
	{ WEAPONTYPE_FLAMETHROWER, ModelSets::MODEL_SET_CLASSIC,
		{ 57473,998080,23113, 880797,-61591,469472, -469994,6624,882645,
		  732849,1740,265564 } },
	{ WEAPONTYPE_M60, ModelSets::MODEL_SET_CLASSIC,
		{ 123377,834399,-537175, 989939,-65699,125316, -69272,547231,834110,
		  262085,22263,160464 } },
	{ WEAPONTYPE_MINIGUN, ModelSets::MODEL_SET_CLASSIC,
		{ 73176,997102,20813, 875924,-74234,476705, -476868,16653,878817,
		  516327,23911,70374 } },
	{ WEAPONTYPE_SCREWDRIVER, ModelSets::MODEL_SET_MODERN,
		{ 121869,992546,0, 980326,-120369,156434, -155268,19065,987689,
		  303955,13016,118408 } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, ModelSets::MODEL_SET_MODERN,
		{ 152069,988333,-8523, 976297,-148862,157115, -154013,32213,987544,
		  276855,-5096,134827 } },
	{ WEAPONTYPE_TEC9, ModelSets::MODEL_SET_MODERN,
		{ 51112,998541,17410, 990031,-52952,130517, -131248,-10565,991293,
		  282593,28259,136536 } },
	{ WEAPONTYPE_UZI, ModelSets::MODEL_SET_MODERN,
		{ 130505,991447,609, 989027,-130229,69754, -69237,8501,997564,
		  283081,3906,104309 } },
	{ WEAPONTYPE_SILENCED_INGRAM, ModelSets::MODEL_SET_MODERN,
		{ -43578,999048,-1903, 998098,43619,43578, -43619,0,999048,
		  242920,55664,100647 } },
	{ WEAPONTYPE_MP5, ModelSets::MODEL_SET_MODERN,
		{ 131115,991366,-1760, 985044,-130079,112992, -111788,16549,993594,
		  398438,-19302,151230 } },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_MODERN,
		{ 95846,995396,0, 995245,-95831,17452, -17372,1673,999848,
		  538940,-21729,112320 } },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_MODERN,
		{ 121789,992554,2054, 989479,-121574,78432, -78098,7520,996917,
		  701294,-38513,129639 } },
	{ WEAPONTYPE_LASERSCOPE, ModelSets::MODEL_SET_MODERN,
		{ 113203,993572,0, 993572,-113203,0, 0,0,1000000,
		  772827,-62897,129990 } },
	{ WEAPONTYPE_ROCKETLAUNCHER, ModelSets::MODEL_SET_MODERN,
		{ 0,1000000,0, 1000000,0,0, 0,0,1000000,
		  500000,34973,115002 } },
	{ WEAPONTYPE_M60, ModelSets::MODEL_SET_MODERN,
		{ 128038,833909,-536845, 988814,-65581,133963, -76506,547992,832978,
		  467285,6226,168579 } },
};

const BuiltInWeaponDefaults *FindBuiltInWeaponDefaults(int weaponType)
{
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInWeaponDefaults); i++)
		if(gBuiltInWeaponDefaults[i].weaponType == weaponType)
			return &gBuiltInWeaponDefaults[i];
	return nil;
}

const BuiltInWeaponSetDefaults *FindBuiltInWeaponSetDefaults(int weaponType,
	ModelSets::eModelSet modelSet)
{
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInWeaponSetDefaults); i++)
		if(gBuiltInWeaponSetDefaults[i].weaponType == weaponType &&
		   gBuiltInWeaponSetDefaults[i].modelSet == modelSet)
			return &gBuiltInWeaponSetDefaults[i];
	return nil;
}

const BuiltInSupportGripDefaults *FindBuiltInSupportGripDefaults(int weaponType,
	ModelSets::eModelSet modelSet)
{
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInSupportGripDefaults); i++)
		if(gBuiltInSupportGripDefaults[i].weaponType == weaponType &&
		   gBuiltInSupportGripDefaults[i].modelSet == modelSet)
			return &gBuiltInSupportGripDefaults[i];
	return nil;
}

const BuiltInWeaponAimRelativeDefaults *
FindBuiltInWeaponAimRelativeDefaults(int weaponType,
	ModelSets::eModelSet modelSet)
{
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInWeaponAimRelativeDefaults); i++)
		if(gBuiltInWeaponAimRelativeDefaults[i].weaponType == weaponType &&
		   gBuiltInWeaponAimRelativeDefaults[i].modelSet == modelSet)
			return &gBuiltInWeaponAimRelativeDefaults[i];
	return nil;
}

bool GetBuiltInWeaponCalibrationForSet(int weaponType, int hand,
	ModelSets::eModelSet modelSet, WeaponCalibration *calibration)
{
	if(!calibration || hand < 0 || hand >= EYE_COUNT)
		return false;
	const BuiltInWeaponSetDefaults *setDefaults =
		FindBuiltInWeaponSetDefaults(weaponType, modelSet);
	const BuiltInWeaponDefaults *classicDefaults = setDefaults ? nil :
		FindBuiltInWeaponDefaults(weaponType);
	if(!setDefaults && !classicDefaults)
		return false;
	const int *value = setDefaults ? setDefaults->hand[hand].main :
		classicDefaults->hand[hand].main;
	calibration->offsetX = value[0];
	calibration->offsetY = value[1];
	calibration->offsetZ = value[2];
	calibration->aimOffsetX = value[3];
	calibration->aimOffsetY = value[4];
	calibration->aimOffsetZ = value[5];
	calibration->aimRotationX = value[6];
	calibration->aimRotationY = value[7];
	calibration->aimRotationZ = value[8];
	calibration->rotationX = value[9];
	calibration->rotationY = value[10];
	calibration->rotationZ = value[11];
	calibration->valid = true;
	return true;
}

bool GetBuiltInWeaponCalibration(int weaponType, int hand,
	WeaponCalibration *calibration)
{
	return GetBuiltInWeaponCalibrationForSet(weaponType, hand,
		GetActiveWeaponModelSet(), calibration);
}

bool GetBuiltInSupportGripCalibrationForSet(int weaponType, int hand,
	ModelSets::eModelSet modelSet, SupportGripCalibration *calibration)
{
	if(!calibration || hand < 0 || hand >= EYE_COUNT)
		return false;
	const BuiltInWeaponSetDefaults *setDefaults =
		FindBuiltInWeaponSetDefaults(weaponType, modelSet);
	const BuiltInWeaponDefaults *classicDefaults = setDefaults ? nil :
		FindBuiltInWeaponDefaults(weaponType);
	if(!setDefaults && !classicDefaults)
		return false;
	const int *value = setDefaults ? setDefaults->hand[hand].support :
		classicDefaults->hand[hand].support;
	calibration->offsetX = value[0];
	calibration->offsetY = value[1];
	calibration->offsetZ = value[2];
	calibration->rotationX = 0;
	// The anatomical support frame fixes the palm normal. The half-turn around
	// that normal is still required to point the fingers/wrist along the weapon
	// instead of presenting the same hand upside-down.
	calibration->rotationY = 180*WEAPON_CALIBRATION_VALUE_SCALE;
	calibration->rotationZ = 0;
	// This helper deliberately preserves the historic controller-local table.
	// Partial v1-v3 profiles depend on the per-hand fallback values above.
	calibration->poseVersion = SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION;
	calibration->valid = true;
	return true;
}

bool GetBuiltInSupportGripCalibration(int weaponType, int hand,
	SupportGripCalibration *calibration)
{
	return GetBuiltInSupportGripCalibrationForSet(weaponType, hand,
		GetActiveWeaponModelSet(), calibration);
}

bool GetBuiltInSupportGripV5CalibrationForSet(int weaponType,
	ModelSets::eModelSet modelSet, SupportGripCalibration *calibration)
{
	if(!calibration)
		return false;
	const BuiltInSupportGripDefaults *authored =
		FindBuiltInSupportGripDefaults(weaponType, modelSet);
	if(!authored && modelSet == ModelSets::MODEL_SET_MODERN)
		authored = FindBuiltInSupportGripDefaults(weaponType,
			ModelSets::MODEL_SET_CLASSIC);
	if(authored){
		calibration->offsetX = authored->offset[0];
		calibration->offsetY = authored->offset[1];
		calibration->offsetZ = authored->offset[2];
		calibration->rotationX = authored->rotation[0];
		calibration->rotationY = authored->rotation[1];
		calibration->rotationZ = authored->rotation[2];
		calibration->gripType = authored->gripType;
		calibration->poseVersion = authored->poseVersion;
	}else{
		// The legacy table already names these components as lateral, forward and
		// vertical. Reuse the canonical RIGHT values in the deterministic v5 frame.
		SupportGripCalibration legacyRight;
		if(!GetBuiltInSupportGripCalibrationForSet(weaponType, 1, modelSet,
		   &legacyRight))
			return false;
		calibration->offsetX = legacyRight.offsetX;
		calibration->offsetY = legacyRight.offsetY;
		calibration->offsetZ = legacyRight.offsetZ;
		calibration->rotationX = legacyRight.rotationX;
		calibration->rotationY = legacyRight.rotationY;
		calibration->rotationZ = legacyRight.rotationZ;
		calibration->gripType = legacyRight.gripType;
		calibration->poseVersion = SUPPORT_GRIP_POSE_VERSION;
	}
	calibration->valid = true;
	return true;
}

bool GetBuiltInSupportGripV5Calibration(int weaponType,
	SupportGripCalibration *calibration)
{
	return GetBuiltInSupportGripV5CalibrationForSet(weaponType,
		GetActiveWeaponModelSet(), calibration);
}

void GetDefaultWeaponCalibrationForSet(int weaponType, int hand,
	ModelSets::eModelSet modelSet, WeaponCalibration &calibration)
{
	calibration = WeaponCalibration();
	if(!GetBuiltInWeaponCalibrationForSet(weaponType, hand, modelSet,
	   &calibration))
		calibration.valid = true;
	if(hand == 0){
		WeaponCalibration right;
		if(GetBuiltInWeaponCalibrationForSet(weaponType, 1, modelSet, &right))
			ApplyMirroredWeaponAim(right, calibration);
	}
}

void GetDefaultWeaponCalibration(int weaponType, int hand,
	WeaponCalibration &calibration)
{
	GetDefaultWeaponCalibrationForSet(weaponType, hand,
		GetActiveWeaponModelSet(), calibration);
}

void GetDefaultSupportGripCalibrationForSet(int weaponType, int hand,
	ModelSets::eModelSet modelSet, SupportGripCalibration &calibration)
{
	calibration = SupportGripCalibration();
	if(hand == 0){
		SupportGripCalibration right;
		if(!GetBuiltInSupportGripV5CalibrationForSet(weaponType, modelSet,
		   &right))
			right.valid = true;
		ApplyMirroredSupportGrip(right, calibration);
	}else if(!GetBuiltInSupportGripV5CalibrationForSet(weaponType, modelSet,
	   &calibration))
		calibration.valid = true;
}

void GetDefaultSupportGripCalibration(int weaponType, int hand,
	SupportGripCalibration &calibration)
{
	GetDefaultSupportGripCalibrationForSet(weaponType, hand,
		GetActiveWeaponModelSet(), calibration);
}

void ApplyCurrentWeaponCalibration(const WeaponCalibration &calibration)
{
	gWeaponOffsetXCm = calibration.offsetX;
	gWeaponOffsetYCm = calibration.offsetY;
	gWeaponOffsetZCm = calibration.offsetZ;
	gWeaponAimOffsetXCm = calibration.aimOffsetX;
	gWeaponAimOffsetYCm = calibration.aimOffsetY;
	gWeaponAimOffsetZCm = calibration.aimOffsetZ;
	gWeaponAimRotationXDeg = calibration.aimRotationX;
	gWeaponAimRotationYDeg = calibration.aimRotationY;
	gWeaponAimRotationZDeg = calibration.aimRotationZ;
	gWeaponRotationXDeg = calibration.rotationX;
	gWeaponRotationYDeg = calibration.rotationY;
	gWeaponRotationZDeg = calibration.rotationZ;
}

void ApplyCurrentSupportGripCalibration(
	const SupportGripCalibration &calibration)
{
	gSupportGripOffsetXCm = calibration.offsetX;
	gSupportGripOffsetYCm = calibration.offsetY;
	gSupportGripOffsetZCm = calibration.offsetZ;
	gSupportGripRotationXDeg = calibration.rotationX;
	gSupportGripRotationYDeg = calibration.rotationY;
	gSupportGripRotationZDeg = calibration.rotationZ;
	gSupportGripType = calibration.gripType;
	gSupportGripPoseVersion = calibration.poseVersion;
}

XrPosef gTrackedHandPose[EYE_COUNT];
bool gTrackedHandPoseValid[EYE_COUNT];
XrPosef gTrackedHandAimPose[EYE_COUNT];
bool gTrackedHandAimPoseValid[EYE_COUNT];
XrVector3f gTrackedHandLinearVelocity[EYE_COUNT];
XrVector3f gTrackedHandAngularVelocity[EYE_COUNT];
float gTrackedHandGrip[EYE_COUNT];
float gTrackedHandTrigger[EYE_COUNT];
int gRenderScaleIndex = VR_RENDER_SCALE_DEFAULT;
// 0 DLAA, 1 Quality, 2 Balanced, 3 Performance. Applied only while the DLAA
// backend is active; the scene renders at the mode ratio and DLSS
// reconstructs the full render-scale image.
int gDlssQualityMode;
// Model profile used by the DLSS-NR versus DLAA A/B switch.
int gDlssComparisonProfile = 1;
// Calibration modes vary Streamline's pixel-space jitter value without
// changing the Halton projection. OFF disables projection jitter.
int gTemporalJitterMode = 1;
float gRenderScale = gRenderScaleOptions[VR_RENDER_SCALE_DEFAULT];
uint32 gTemporalFrameIndex;
float gTemporalJitterX;
float gTemporalJitterY;
float gTemporalJitterClipX;
float gTemporalJitterClipY;
#ifdef RW_D3D12
uint32 gFixedFoveatedProfile = rw::d3d12::FIXED_FOVEATED_QUALITY;
#endif
int gRetryFrames;
// Delay between session creation attempts, in frames. Doubles while the
// runtime keeps refusing (dead/hung SteamVR after a crash produced hundreds
// of retry log lines) and resets once a session comes up again.
int gSessionRetryDelay = 300;
// When the session dropped below VISIBLE (headset taken off / dormant).
// Coming back FOCUSED after a long gap re-latches the head anchors: the
// player has usually turned or moved meanwhile, and a stale anchor makes
// head-relative movement walk off-axis.
ULONGLONG gHeadsetDormantSinceMs;
int64_t gColorFormat;
XrFrameState gFrameState = { XR_TYPE_FRAME_STATE };
XrView gLocatedViews[EYE_COUNT] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
XrPosef gRenderPose[EYE_COUNT];
XrFovf gRenderFov[EYE_COUNT];
float gSourceTanX;
float gSourceTanY;
XrVector3f gTrackingCenterOrigin;
XrViewConfigurationView gViewConfig[EYE_COUNT] = {
	{ XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW }
};
EyeBuffer gEye[EYE_COUNT];
TrackedForegroundRenderCallback gTrackedForegroundRenderCallback;
TrackedForegroundVisibilityCallback gTrackedForegroundVisibilityCallback;
#ifdef RW_D3D12
// Both eyes share one double-wide render target.  The per-eye rasters below
// are lightweight sub-rasters that select their half of this allocation.
// This keeps the existing sequential eye path bit-for-bit useful while also
// providing the target layout required by single-pass stereo.
RwRaster *gStereoColor;
RwRaster *gStereoDepth;
ID3D12RootSignature *gTrackedForegroundResolveRootSignature;
ID3D12PipelineState *gTrackedForegroundResolvePipeline;
bool gTrackedForegroundResolveAvailable;
void DestroyTrackedForegroundResolvePipeline();
bool EnsureTrackedForegroundResolvePipeline();
#endif
Swapchain gHudSwapchain;
Swapchain gWristHudSwapchain;
Swapchain gCinemaSwapchain;
bool gCinemaFrameValid;
int gCinemaContentWidth;
int gCinemaContentHeight;
bool gCinemaAnchorValid;
XrPosef gCinemaAnchorPose;
// Cinema-path telemetry, reported once a second from SubmitCinemaFrame while
// cutscenes/menus are on screen. Player logs then separate three otherwise
// identical-looking "cutscene jitter" complaints: content updating too rarely
// (copies/s low), the screen riding the head (view-fallback frames non-zero),
// and the anchor re-jumping (anchor resets climbing).
uint32 gCinemaViewFallbackFrames;
uint32 gCinemaAnchorResets;
uint32 gConsecutiveStereoFrames;
// Wall time the most recent xrWaitFrame blocked for, always measured. The
// cinema pace line averages it: a ~60ms average is the signature of the
// runtime throttling a session it believes stopped rendering.
float gLastXrWaitFrameMs;
// The keep-alive projection layer must not cost anything per frame: eye
// Clear each cinema eye image once per swapchain rotation. Menu clears use a
// synchronized command list, so they are not repeated every frame.
uint32 gCinemaEyeClearsPending;
bool gCinemaEyesEverCleared;
// True only on SteamVR, the runtime whose quad-only throttling the cinema
// keep-alive projection works around.
bool gRuntimeNeedsCinemaKeepAlive;
// Theater mode renders cinema as a world-anchored stereo projection, avoiding
// SteamVR's 15 Hz quad-only throttle.
bool gCinemaTheaterMode;
// True once the classic quad path has actually released a cinema swapchain
// image this session. Theater-mode sessions never do, and a held quad frame
// must not reference a swapchain with no released image.
bool gCinemaQuadImageValid;
#ifdef RW_D3D12
ID3D12Resource *gCinemaStagingTexture;
int gCinemaStagingWidth;
int gCinemaStagingHeight;
#endif
Swapchain gDebugSwapchain;
Swapchain gVrMenuSwapchain;
RwRaster *gHudColor;
RwRaster *gHudDepth;
#ifndef RW_D3D12
GLuint gCopyFramebuffer;
GLuint gFxaaProgram;
GLuint gFxaaVertexArray;
GLint gFxaaTextureUniform = -1;
GLint gFxaaInverseSizeUniform = -1;
GLint gFxaaUvScaleUniform = -1;
GLint gFxaaUvOffsetUniform = -1;
GLint gFxaaEnabledUniform = -1;
GLint gColorModeUniform = -1;
GLint gBlurColorUniform = -1;
GLint gContrastMultUniform = -1;
GLint gContrastAddUniform = -1;
#endif
Actions gActions;

uint8 gDebugPixels[VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT*4];
uint8 gVrMenuPixels[VR_MENU_WIDTH*VR_MENU_HEIGHT*4];
uint8 gStartupPixels[VR_STARTUP_WIDTH*VR_STARTUP_HEIGHT*4];
struct VrWeaponIconCache
{
	int width;
	int height;
	std::vector<uint8> rgba;
	VrWeaponIconCache() : width(0), height(0) {}
};
VrWeaponIconCache gVrWeaponIconCache[WEAPONTYPE_TOTALWEAPONS];
HDC gStartupCaptureDc;
HBITMAP gStartupCaptureBitmap;
HGDIOBJ gStartupCaptureOldBitmap;
void *gStartupCaptureBits;
double gDebugPreviousFrameMs;
float gDebugSmoothedFrameMs;
int gDebugFps;
bool gVrLogStarted;
int gVrLoggedFrames;
int gVrLoggedRenderableFrames;
bool gRuntimeRecoveryPending;
const char *gCurrentFramePhase = "none";

void VrLog(const char *format, ...)
{
#ifdef RW_D3D12
	FILE *file = fopen("openxr_d3d12.log", gVrLogStarted ? "a" : "w");
	if(!file)
		return;
	gVrLogStarted = true;
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fclose(file);
#else
	(void)format;
#endif
}

struct DebugGlyph { char character; uint8 rows[7]; };
const DebugGlyph gDebugGlyphs[] = {
	{' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'%',{0x11,0x12,0x02,0x04,0x08,0x09,0x11}},
	{'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
	{'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
	{'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
	{':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}}, {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
	{'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}}, {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}}, {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
	{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}}, {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
	{'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J',{0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
	{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x19,0x15,0x13,0x13,0x11}},
	{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
	{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
	{'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
	{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}}
};

void BackupVrSettings(const char *reason);

const char *GetVrSettingsPath()
{
	static char path[MAX_PATH] = {};
	if(path[0] != '\0')
		return path;
	const DWORD length = GetModuleFileNameA(nil, path, ARRAY_SIZE(path));
	if(length == 0 || length >= ARRAY_SIZE(path)){
		strcpy(path, ".\\vr_settings.ini");
		return path;
	}
	char *separator = strrchr(path, '\\');
	if(!separator)
		separator = strrchr(path, '/');
	if(separator)
		strcpy(separator+1, "vr_settings.ini");
	else
		strcpy(path, "vr_settings.ini");
	return path;
}

bool IsAssignableHolsterSlot(int slot)
{
	return slot == WEAPONSLOT_MELEE || slot == WEAPONSLOT_HANDGUN ||
		slot == WEAPONSLOT_SHOTGUN ||
		slot == WEAPONSLOT_SUBMACHINEGUN || slot == WEAPONSLOT_RIFLE ||
		slot == WEAPONSLOT_HEAVY || slot == WEAPONSLOT_SNIPER ||
		 slot == WEAPONSLOT_OTHER;
}

void ApplyTrafficSettings()
{
	CIniFile::PedNumberMultiplier = gTrafficPedPercent/100.0f;
	CIniFile::CarNumberMultiplier = gTrafficCarPercent/100.0f;
	CPopulation::MaxNumberOfPedsInUse =
		(int32)(25.0f*CIniFile::PedNumberMultiplier);
	CPopulation::MaxNumberOfPedsInUseInterior =
		(int32)(40.0f*CIniFile::PedNumberMultiplier);
	CCarCtrl::MaxNumberOfCarsInUse =
		(int32)(12.0f*CIniFile::CarNumberMultiplier);
}

void ApplyGraphicsEffectsSettings()
{
#ifdef RW_D3D12
	const float carPercent = gEffectsEnabled ?
		(float)gCarReflectionPercent : 0.0f;
	const float waterPercent = gEffectsEnabled && gModernWaterEnabled ?
		(float)gWaterReflectionPercent : 0.0f;
	rw::d3d12::setScreenSpaceReflectionSettings(carPercent, waterPercent,
		(float)gOceanWavePercent);
	rw::d3d12::setCarReflectionSsrParams((float)gCarReflectionSsrPercent,
		(float)gCarReflectionSsrDistance);
	rw::d3d12::setWaterSurfaceSettings((float)gWaterSpeedPercent, (float)gWaterDistortionPercent,
		gEffectsEnabled && gModernWaterEnabled ? (float)gWaterSheenPercent : 0.0f,
		gEffectsEnabled && gModernWaterEnabled ? (float)gWaterGlintPercent : 0.0f,
		gEffectsEnabled && gModernWaterEnabled ? (float)gWaterSparksPercent : 0.0f);
	CWeather::ModernRainEffectsEnabled = gEffectsEnabled;
	gScreenSpaceReflectionFailed = false;
#endif
}

void LoadVrSettings()
{
	if(gVrSettingsLoaded)
		return;
	BackupVrSettings("startup");
	const char *path = GetVrSettingsPath();
	const bool welcomeAlreadyShown =
		GetPrivateProfileIntA("VR", "WelcomeShown", 0, path) != 0;
	// Do not persist the first-run marker merely because settings were loaded.
	// It is written only after the player has actually seen and dismissed the
	// welcome card. This also keeps a failed/aborted first launch eligible.
	// First-run About belongs to the live VR world, not the frontend/cinema
	// layer. Keep it pending until Tommy takes a real on-foot step across
	// consecutive controllable gameplay frames.
	gVrAboutVisible = false;
	gVrAboutFirstRun = !welcomeAlreadyShown;
	gVrAboutFirstRunPending = !welcomeAlreadyShown;
	gVrAboutStepBaselineValid = false;
	gVrAboutStepControllableFrames = 0;
	gVrAboutStepLastGameFrame = 0;
	gVrAboutStepMovementInputSeen = false;
	gVrAboutDismissArmed = false;
	gVrAboutReleaseGate = false;
	gVrAboutWasRendered = false;
	gRenderScaleIndex = GetPrivateProfileIntA("VR", "RenderScale", VR_RENDER_SCALE_DEFAULT, path);
	gRenderScaleIndex = Min(Max(gRenderScaleIndex, 0), (int)ARRAY_SIZE(gRenderScaleOptions)-1);
	gRenderScale = gRenderScaleOptions[gRenderScaleIndex];
	gDlssQualityMode = GetPrivateProfileIntA("VR", "DlssMode", 0, path);
	gDlssQualityMode = Min(Max(gDlssQualityMode, 0), 3);
	Dlaa::SetQualityMode(gDlssQualityMode);
	char temporalAaText[16] = {};
	GetPrivateProfileStringA("VR", "TemporalAA", "", temporalAaText,
		sizeof(temporalAaText), path);
	if(temporalAaText[0] != '\0')
		gTemporalAaBackend = atoi(temporalAaText);
	else
		gTemporalAaBackend =
			GetPrivateProfileIntA("VR", "DLAA", 0, path) != 0 ?
				TEMPORAL_AA_DLAA : TEMPORAL_AA_OFF;
	gTemporalAaBackend = Min(Max(gTemporalAaBackend,
		(int)TEMPORAL_AA_OFF), (int)TEMPORAL_AA_BACKEND_COUNT-1);
	gStreamlineEnabled =
		GetPrivateProfileIntA("VR", "StreamlineEnabled", 1, path) != 0;
	gFlatModeEnabled = GetPrivateProfileIntA("VR", "FlatMode", 0, path) != 0;
	gFlatFirstPersonEnabled =
		GetPrivateProfileIntA("VR", "FlatFirstPerson", 1, path) != 0;
	// Keep loading the experimental model behind an explicit opt-in, but allow
	// that opt-in in both flat and OpenXR sessions. Presentation mode 0 is a
	// cheap DLAA baseline; 1..4 are fullscreen feature-18 model profiles.
	Dlaa::SetNeuralRenderingEnabled(
		GetPrivateProfileIntA("VR", "DLSSNeuralRendering", 0, path) != 0);
	Dlaa::SetNeuralRenderingPassCount(Min(Max(GetPrivateProfileIntA("VR",
		"DLSSNeuralPasses", 1, path), 1), 3));
	// Old builds stored a centered NR crop independently. The neural work scale
	// now follows the real DLSS render size so the entire eye remains processed.
	Dlaa::SetNeuralRenderingRegionMode(gDlssQualityMode);
	const int savedNeuralMode = Min(Max(GetPrivateProfileIntA("VR",
		"DLSSNeuralRenderingMode", 1, path), 0), 4);
	gDlssComparisonProfile = Min(Max(GetPrivateProfileIntA("VR",
		"DLSSNeuralRenderingCompareMode",
		savedNeuralMode > 0 ? savedNeuralMode : 1, path), 1), 4);
	Dlaa::SetNeuralRenderingMode(gDlssComparisonProfile);
	if(GetPrivateProfileIntA("VR", "DLSSNeuralTuningCustom", 0, path) != 0){
		Dlaa::SetNeuralRenderingIntensityPercent(GetPrivateProfileIntA("VR",
			"DLSSNeuralIntensity", Dlaa::GetNeuralRenderingIntensityPercent(), path));
		Dlaa::SetNeuralRenderingStructurePercent(GetPrivateProfileIntA("VR",
			"DLSSNeuralStructure", Dlaa::GetNeuralRenderingStructurePercent(), path));
		Dlaa::SetNeuralRenderingLocalTonePercent(GetPrivateProfileIntA("VR",
			"DLSSNeuralLocalTone", Dlaa::GetNeuralRenderingLocalTonePercent(), path));
		Dlaa::SetNeuralRenderingGlobalTonePercent(GetPrivateProfileIntA("VR",
			"DLSSNeuralGlobalTone", Dlaa::GetNeuralRenderingGlobalTonePercent(), path));
		Dlaa::SetNeuralRenderingStyle(GetPrivateProfileIntA("VR",
			"DLSSNeuralStyle", Dlaa::GetNeuralRenderingStyle(), path));
		Dlaa::SetNeuralRenderingAutoMask(GetPrivateProfileIntA("VR",
			"DLSSNeuralAutoMask",
			Dlaa::IsNeuralRenderingAutoMaskEnabled() ? 1 : 0, path) != 0);
	}
	Dlaa::SetNeuralRenderingOutputVisible(GetPrivateProfileIntA("VR",
		"DLSSNeuralRenderingOutputVisible",
		savedNeuralMode > 0 ? 1 : 0, path) != 0);
	// With Streamline never initialised DLAA cannot work; drop a saved DLAA
	// selection to OFF so the menu does not point at an impossible backend.
	if(!gStreamlineEnabled && gTemporalAaBackend == TEMPORAL_AA_DLAA)
		gTemporalAaBackend = TEMPORAL_AA_OFF;
	gTemporalJitterMode = GetPrivateProfileIntA("VR", "TemporalJitter", 1, path);
	gTemporalJitterMode = Min(Max(gTemporalJitterMode, 0), 5);
	gAntiAliasingEnabled = GetPrivateProfileIntA("VR", "AntiAliasing", 1, path) != 0;
	gLightingEnabled = GetPrivateProfileIntA("VR", "ViceCityColor", 1, path) != 0;
	gSunGlarePercent = Min(Max(GetPrivateProfileIntA("VR",
		"SunGlarePercent", 100, path), 0), 100);
	gEffectsEnabled = GetPrivateProfileIntA("VR",
		"EffectsEnabled", 0, path) != 0;
	gGenerateMipmapsRequested = GetPrivateProfileIntA("VR", "GenerateMipmaps", 1, path) != 0;
	gGenerateMipmapsAtStartup = gGenerateMipmapsRequested;
	gDistanceFogEnabled = GetPrivateProfileIntA("VR", "DistanceFog", 1, path) != 0;
	gFoliageSoftness = Min(Max((int)GetPrivateProfileIntA("VR", "FoliageSoftness", 0, path), 0), 3);
#ifdef RW_D3D12
	rw::d3d12::setGenerateMipmaps(gGenerateMipmapsAtStartup);
	rw::d3d12::setDistanceFogEnabled(gDistanceFogEnabled);
	rw::d3d12::setMaskedMipBias(gFoliageSoftness);
#endif
	CShadows::SetRenderEnabled(GetPrivateProfileIntA("VR", "ShadowsEnabled",
		GetPrivateProfileIntA("VR", "Shadows", 1, path), path) != 0);
	CParticleObject::SetVrFountainQuality((int)GetPrivateProfileIntA("VR", "FountainQuality", VR_FOUNTAIN_OPTIMIZED, path));
	gCarReflectionPercent = Min(Max(GetPrivateProfileIntA("VR",
		"CarReflectionIntensity", 100, path), 0), 200);
	gWaterReflectionPercent = Min(Max(GetPrivateProfileIntA("VR",
		"WaterReflection", 80, path), 0), 200);
	gModernWaterEnabled = GetPrivateProfileIntA("VR", "ModernWater", 1, path) != 0;
	gWaterSpeedPercent = Min(Max((int)GetPrivateProfileIntA("VR", "WaterSpeed", 100, path), 0), 200);
	gWaterDistortionPercent = Min(Max((int)GetPrivateProfileIntA("VR", "WaterDistortion", 100, path), 0), 200);
	gWaterSheenPercent = Min(Max((int)GetPrivateProfileIntA("VR", "WaterSheen", 0, path), 0), 200);
	gWaterGlintPercent = Min(Max((int)GetPrivateProfileIntA("VR", "WaterGlint", 0, path), 0), 200);
	gWaterSparksPercent = Min(Max((int)GetPrivateProfileIntA("VR", "WaterSparks", 0, path), 0), 200);
	gCarReflectionSsrPercent = Min(Max((int)GetPrivateProfileIntA("VR", "CarReflectionSsr", 100, path), 0), 200);
	gCarReflectionSsrDistance = Min(Max((int)GetPrivateProfileIntA("VR", "CarReflectionSsrDistance", 0, path), 0), 500);
	gOceanWavePercent = Min(Max(GetPrivateProfileIntA("VR",
		"OceanWaveIntensity", 100, path), 0), 200);
	gPuddleReflectionPercent = Min(Max(GetPrivateProfileIntA("VR",
		"PuddleReflection", 75, path), 0), 200);
	gPuddleCoveragePercent = Min(Max(GetPrivateProfileIntA("VR",
		"PuddleCoverage", 25, path), 25), 200);
	gPuddleEdgeSoftnessPercent = Min(Max(GetPrivateProfileIntA("VR",
		"PuddleEdgeSoftness", 25, path), 25), 200);
	gPuddleRipplePercent = Min(Max(GetPrivateProfileIntA("VR",
		"PuddleRipple", 100, path), 0), 200);
	gDynamicLights = Min(Max(GetPrivateProfileIntA("VR",
		"DynamicLights", 0, path), 0), 2);
	gDynamicLightIntensityPercent = Min(Max(GetPrivateProfileIntA("VR",
		"DynamicLightIntensity", 100, path), 25), 200);
	gDynamicLightGlowPercent = Min(Max(GetPrivateProfileIntA("VR",
		"DynamicLightGlowIntensity", 100, path), 25), 200);
	gDynamicLightMax = Min(Max(GetPrivateProfileIntA("VR",
		"DynamicLightMax", 4, path), 2), 8);
	CWeather::RainGraphicsMode = Min(Max(GetPrivateProfileIntA("VR",
		"RainGraphicsMode", 1, path), 0), 3);
	CWeather::RainGraphicsIntensity = Min(Max(GetPrivateProfileIntA("VR",
		"RainGraphicsIntensity", 200, path), 50), 200);
	CWeather::RainGraphicsDensity = Min(Max(GetPrivateProfileIntA("VR",
		"RainGraphicsDensity", 300, path), 25), 300);
	CWeather::RainSurfaceMode = Min(Max(GetPrivateProfileIntA("VR",
		"RainSurfaceMode", 0, path), 0), 3);
	ApplyGraphicsEffectsSettings();
	CRenderer::SetVrOcclusionCulling(
		GetPrivateProfileIntA("VR", "OcclusionCulling", 1, path) != 0);
	gGameplayHudVisible = GetPrivateProfileIntA("VR", "GameplayHud", 1, path) != 0;
	gWristHud.Load(path);
	VrRagdoll::SetEnabled(GetPrivateProfileIntA("VR", "Ragdolls", 0, path) != 0);
	gHudWidthPercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudWidthPercent", 100, path), 50), 200);
	gHudScalePercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudScalePercent", 130, path), 50), 200);
	// GetPrivateProfileIntA returns UINT. Cast back to signed before applying
	// negative bounds, otherwise every stored HUD offset clamps to +100.
	gHudOffsetXCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetXCm", 0, path), -100), 100);
	gHudOffsetYCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetYCm", 0, path), -100), 100);
	gVrHandsEnabled = GetPrivateProfileIntA("VR", "VrHands", 1, path) != 0;
	gWeaponLaserEnabled = GetPrivateProfileIntA("VR", "WeaponLaser", 0, path) != 0;
	gWeaponHapticsEnabled =
		GetPrivateProfileIntA("VR", "WeaponHaptics", 1, path) != 0;
	gWeaponHapticsStrengthPercent = Min(Max(GetPrivateProfileIntA("VR",
		"WeaponHapticsStrengthPercent", 100, path), 10), 200);
	gWeaponHolsterHighlightsEnabled =
		GetPrivateProfileIntA("VR", "HolsterHighlights", 0, path) != 0;
	gManualReloadEnabled =
		GetPrivateProfileIntA("VR", "ManualReloading", 0, path) != 0;
	gPhysicalScopeAimEnabled =
		GetPrivateProfileIntA("VR", "PhysicalScopeAim", 1, path) != 0;
	gHeadAimEnabled =
		GetPrivateProfileIntA("VR", "HeadAim", 0, path) != 0;
	gWeaponGripLockEnabled =
		GetPrivateProfileIntA("VR", "WeaponGripLock", 0, path) != 0;
	// Migrate the single pre-v0.4.1 choice without rewriting it. Missing new
	// keys inherit the old absolute value; existing per-class choices always win.
	char drivingTypeText[16] = {};
	GetPrivateProfileStringA("VR", "DrivingType", "", drivingTypeText,
		sizeof(drivingTypeText), path);
	int legacyDrivingType = drivingTypeText[0] != '\0' ?
		atoi(drivingTypeText) :
		(GetPrivateProfileIntA("VR", "ImmersiveDriving", 0, path) != 0 ?
			VR_DRIVING_IMMERSIVE : VR_DRIVING_DEFAULT);
	legacyDrivingType = Min(Max(legacyDrivingType, (int)VR_DRIVING_DEFAULT),
		(int)VR_DRIVING_TYPE_COUNT-1);
	char carDrivingTypeText[16] = {};
	GetPrivateProfileStringA("VR", "CarDrivingType", "",
		carDrivingTypeText, sizeof(carDrivingTypeText), path);
	gCarDrivingType = carDrivingTypeText[0] != '\0' ?
		atoi(carDrivingTypeText) : legacyDrivingType;
	gCarDrivingType = Min(Max(gCarDrivingType, (int)VR_DRIVING_DEFAULT),
		(int)VR_DRIVING_TYPE_COUNT-1);
	char bikeDrivingTypeText[16] = {};
	GetPrivateProfileStringA("VR", "BikeDrivingType", "",
		bikeDrivingTypeText, sizeof(bikeDrivingTypeText), path);
	gBikeDrivingType = bikeDrivingTypeText[0] != '\0' ?
		atoi(bikeDrivingTypeText) : legacyDrivingType;
	gBikeDrivingType = Min(Max(gBikeDrivingType, (int)VR_DRIVING_DEFAULT),
		(int)VR_DRIVING_TYPE_COUNT-1);
	gMotionSteeringHand =
		GetPrivateProfileIntA("VR", "MotionSteeringHand", 1, path);
	gMotionSteeringHand = Min(Max(gMotionSteeringHand, 0), EYE_COUNT-1);
	gBikeHandleHighlightsEnabled =
		GetPrivateProfileIntA("VR", "BikeHandleHighlights", 0, path) != 0;
	gCarHandleHighlightsEnabled = GetPrivateProfileIntA("VR", "CarHandleHighlights", 0, path) != 0;
	gBoatHandleHighlightsEnabled = GetPrivateProfileIntA("VR", "BoatHandleHighlights", 0, path) != 0;
	gWheelHandPullBackMm = Min(Max(GetPrivateProfileIntA("VR", "WheelHandPullBackMm", 0, path), -80), 80);
	// This is the master switch across both model sets. Per-model exceptions are
	// namespaced below, but a global HIDE must keep every virtual rim hidden.
	gImmersiveCarWheelVisible =
		GetPrivateProfileIntA("VR", "ImmersiveCarWheelVisible", 1, path) != 0;
	gImmersiveBoatWheelVisible = GetPrivateProfileIntA("VR", "ImmersiveBoatWheelVisible", 1, path) != 0;
	gBoatDrivingType = Min(Max(GetPrivateProfileIntA("VR", "BoatDrivingType", 0, path), 0), (int)VR_DRIVING_TYPE_COUNT-1);
	gBikeLockHorizonEnabled =
		GetPrivateProfileIntA("VR", "BikeLockHorizon", 1, path) != 0;
	// Teleport remains in the code for later work, but is intentionally not a
	// release-selectable locomotion mode until its destination placement and
	// distance controls are production ready.
	gMovementMode = VR_MOVEMENT_SMOOTH;
	gMovementOrientation = Min(Max(GetPrivateProfileIntA("VR",
		"MovementOrientation", VR_MOVEMENT_ORIENTATION_HEAD_DIRECTED, path),
		(int)VR_MOVEMENT_ORIENTATION_BODY),
		(int)VR_MOVEMENT_ORIENTATION_COUNT-1);
	gTurnMode = Min(Max(GetPrivateProfileIntA("VR", "TurnMode",
		VR_TURN_SMOOTH, path), (int)VR_TURN_SMOOTH),
		(int)VR_TURN_MODE_COUNT-1);
	gTurnSensitivityPercent = Min(Max(GetPrivateProfileIntA("VR",
		"TurnSensitivityPercent", 100, path), 25), 300);
	gHeadSteeringSensitivityPercent = Min(Max(GetPrivateProfileIntA("VR",
		"HeadSteeringSensitivityPercent", 50, path), 25), 300);
	gSnapTurnAngleDegrees = Min(Max(GetPrivateProfileIntA("VR",
		"SnapTurnAngleDegrees", 30, path), 15), 90);
	gHeadBobbingEnabled =
		GetPrivateProfileIntA("VR", "HeadBobbing", 0, path) != 0;
	gCutsceneMode = Min(Max(GetPrivateProfileIntA("VR", "CutsceneMode", 0, path), 0), 1);
	gStickCrouch = GetPrivateProfileIntA("VR", "StickCrouch", 0, path) != 0;
	gStickLookBehind = GetPrivateProfileIntA("VR", "StickLookBehind", 0, path) != 0;
	for(int source = 0; source < VrPadBindings::SOURCE_COUNT; source++)
		gVrPadBindings.Set(source, GetPrivateProfileIntA("VR", VrPadBindings::Key(source),
			VrPadBindings::DefaultTarget(source), path));
	gDefaultDrivingBodyVisible = GetPrivateProfileIntA("VR",
		"DefaultDrivingBodyVisible", 1, path) != 0;
	gBikeManualThrottle = GetPrivateProfileIntA("VR", "ImmersiveBikeManualThrottle", 0, path) != 0;
	gBikeViewFollowingTilt = GetPrivateProfileIntA("VR", "BikeViewFollowsTilt", 0, path) != 0;
	gBikeRiderCanBeThrown = GetPrivateProfileIntA("VR", "BikeRiderCanBeThrown", 1, path) != 0;
	gBikeVisualLeanPercent = Min(Max(GetPrivateProfileIntA("VR", "BikeVisualLeanPercent", 50, path), 25), 100);
	for(int category = 0; category < VR_VEHICLE_CATEGORY_COUNT; category++){
		char key[64];
		sprintf(key, "VehicleThirdPerson%s", gVehicleCategorySettingPrefixes[category]);
		gVehicleThirdPerson[category] = GetPrivateProfileIntA("VR", key,
			GetPrivateProfileIntA("VR", "VehicleThirdPerson", 0, path), path) != 0;
		sprintf(key, "Default%sSeatHeightCm", gVehicleCategorySettingPrefixes[category]);
		gDefaultSeatHeightCm[category] = Min(Max(GetPrivateProfileIntA("VR", key,
			GetPrivateProfileIntA("VR", "DrivingYOffsetCm", 15, path), path), -100), 150);
		sprintf(key, "Default%sSeatDistanceCm", gVehicleCategorySettingPrefixes[category]);
		gDefaultSeatDistanceCm[category] = Min(Max(GetPrivateProfileIntA("VR", key, 0, path), -100), 100);
	}
	const int defaultPedTrafficPercent = Min(Max(
		(int)(CIniFile::PedNumberMultiplier*100.0f+0.5f), 50), 300);
	const int defaultCarTrafficPercent = Min(Max(
		(int)(CIniFile::CarNumberMultiplier*100.0f+0.5f), 50), 300);
	gTrafficPedPercent = Min(Max(GetPrivateProfileIntA("VR",
		"PedTrafficPercent", defaultPedTrafficPercent, path), 50), 300);
	gTrafficCarPercent = Min(Max(GetPrivateProfileIntA("VR",
		"CarTrafficPercent", defaultCarTrafficPercent, path), 50), 300);
	ApplyTrafficSettings();
	const int defaultHolsterSlots[HOLSTER_POINT_COUNT] = {
		WEAPONSLOT_SNIPER, WEAPONSLOT_SUBMACHINEGUN,
		WEAPONSLOT_SHOTGUN, WEAPONSLOT_MELEE,
		WEAPONSLOT_PROJECTILE, WEAPONSLOT_HEAVY, WEAPONSLOT_RIFLE
	};
	bool usedHolsterSlots[TOTAL_WEAPON_SLOTS] = {};
	gHolsterPointWeaponSlot[HOLSTER_CHEST_CENTER] = WEAPONSLOT_PROJECTILE;
	usedHolsterSlots[WEAPONSLOT_PROJECTILE] = true;
	for(int point = 0; point < HOLSTER_POINT_COUNT; point++){
		if(point == HOLSTER_CHEST_CENTER)
			continue;
		int slot = (int)(int32)GetPrivateProfileIntA("VR",
			gHolsterPointSettingNames[point], defaultHolsterSlots[point], path);
		if(slot != -1 && (!IsAssignableHolsterSlot(slot) ||
		   usedHolsterSlots[slot]))
			slot = -1;
		gHolsterPointWeaponSlot[point] = slot;
		if(slot >= 0)
			usedHolsterSlots[slot] = true;
	}
	// Existing installations already persisted CHEST RIGHT as EMPTY before
	// physical melee weapons existed.  Migrate only the first genuinely empty
	// point and never displace a loadout chosen by the player.
	if(!usedHolsterSlots[WEAPONSLOT_MELEE]){
		for(int point = 0; point < HOLSTER_POINT_COUNT; point++){
			if(point == HOLSTER_CHEST_CENTER)
				continue;
			if(gHolsterPointWeaponSlot[point] != -1)
				continue;
			gHolsterPointWeaponSlot[point] = WEAPONSLOT_MELEE;
			usedHolsterSlots[WEAPONSLOT_MELEE] = true;
			char slotValue[16];
			sprintf(slotValue, "%d", WEAPONSLOT_MELEE);
			WritePrivateProfileStringA("VR", gHolsterPointSettingNames[point],
				slotValue, path);
			break;
		}
	}
	gWeaponOffsetXCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponOffsetXCm", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetYCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponOffsetYCm", 1, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetZCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponOffsetZCm", -5, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimOffsetXCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimOffsetXCm", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimOffsetYCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimOffsetYCm", 4, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimOffsetZCm = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimOffsetZCm", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimRotationXDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimRotationXDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimRotationYDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimRotationYDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponAimRotationZDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponAimRotationZDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	// Versioned local-axis keys deliberately ignore the short-lived Euler
	// calibration values from earlier builds, whose Y/Z axes collapsed at the
	// legacy weapon frame's 90-degree base rotation.
	gWeaponRotationXDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponLocalRotationXDeg", 0, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponRotationYDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponLocalRotationYDeg", 9, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponRotationZDeg = (int)(int32)GetPrivateProfileIntA("VR", "WeaponLocalRotationZDeg", 7, path)*WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetXCm = Min(Max(gWeaponOffsetXCm, -100), 100);
	gWeaponOffsetYCm = Min(Max(gWeaponOffsetYCm, -100), 100);
	gWeaponOffsetZCm = Min(Max(gWeaponOffsetZCm, -100), 100);
	gWeaponAimOffsetXCm = Min(Max(gWeaponAimOffsetXCm, -100), 100);
	gWeaponAimOffsetYCm = Min(Max(gWeaponAimOffsetYCm, -100), 100);
	gWeaponAimOffsetZCm = Min(Max(gWeaponAimOffsetZCm, -100), 100);
	gWeaponAimRotationXDeg = Min(Max(gWeaponAimRotationXDeg, -360), 360);
	gWeaponAimRotationYDeg = Min(Max(gWeaponAimRotationYDeg, -360), 360);
	gWeaponAimRotationZDeg = Min(Max(gWeaponAimRotationZDeg, -360), 360);
	gWeaponRotationXDeg = Min(Max(gWeaponRotationXDeg, -360), 360);
	gWeaponRotationYDeg = Min(Max(gWeaponRotationYDeg, -360), 360);
	gWeaponRotationZDeg = Min(Max(gWeaponRotationZDeg, -360), 360);
	ReloadVehicleCategoryCalibration();
#ifdef RW_D3D12
	gFixedFoveatedProfile = GetPrivateProfileIntA("VR", "VRS", rw::d3d12::FIXED_FOVEATED_QUALITY, path);
	gFixedFoveatedProfile = Min(Max(gFixedFoveatedProfile, 0U),
		(uint32)rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT-1);
#endif
	gVrSettingsLoaded = true;
}

void SaveVrSetting(const char *name, int value)
{
	char textValue[32];
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA("VR", name, textValue, GetVrSettingsPath());
}

void SaveDlssNrTuningSettings()
{
	SaveVrSetting("DLSSNeuralTuningCustom",
		Dlaa::IsNeuralRenderingTuningCustom() ? 1 : 0);
	SaveVrSetting("DLSSNeuralIntensity",
		Dlaa::GetNeuralRenderingIntensityPercent());
	SaveVrSetting("DLSSNeuralStructure",
		Dlaa::GetNeuralRenderingStructurePercent());
	SaveVrSetting("DLSSNeuralLocalTone",
		Dlaa::GetNeuralRenderingLocalTonePercent());
	SaveVrSetting("DLSSNeuralGlobalTone",
		Dlaa::GetNeuralRenderingGlobalTonePercent());
	SaveVrSetting("DLSSNeuralStyle", Dlaa::GetNeuralRenderingStyle());
	SaveVrSetting("DLSSNeuralAutoMask",
		Dlaa::IsNeuralRenderingAutoMaskEnabled() ? 1 : 0);
}

int GetVrVehicleCategory(CVehicle *vehicle)
{
	if(!vehicle)
		return VR_VEHICLE_CATEGORY_INVALID;
	if(vehicle->IsBike())
		return VR_VEHICLE_CATEGORY_BIKE;
	if(vehicle->IsBoat())
		return VR_VEHICLE_CATEGORY_BOAT;
	if(vehicle->IsHeli() || vehicle->IsRealHeli())
		return VR_VEHICLE_CATEGORY_HELI;
	if(vehicle->IsCar() && !vehicle->IsRealPlane())
		return VR_VEHICLE_CATEGORY_CAR;
	return VR_VEHICLE_CATEGORY_INVALID;
}

VehicleCategoryCalibration *GetVehicleCategoryCalibration(CVehicle *vehicle)
{
	const int category = GetVrVehicleCategory(vehicle);
	if(category < 0 || category >= VR_VEHICLE_CATEGORY_COUNT)
		return nil;
	return &gVehicleCategoryCalibration[category];
}

const char *GetActiveVrVehicleCategoryName()
{
	const int category = GetVrVehicleCategory(FindPlayerVehicle());
	return category >= 0 && category < VR_VEHICLE_CATEGORY_COUNT ?
		gVehicleCategoryNames[category] : "NO VEHICLE";
}

void SaveVehicleCategoryCalibrationValue(int category, const char *suffix,
	int value)
{
	if(category < 0 || category >= VR_VEHICLE_CATEGORY_COUNT || !suffix)
		return;
	char key[64];
	GetVehicleCategorySettingKey(category, suffix,
		GetActiveVehicleModelSet(), key);
	SaveVrSetting(key, value);
}

int GetVrCarModelIndex(int model);

struct BuiltInVehicleViewDefaults
{
	int model;
	ModelSets::eModelSet modelSet;
	int value[11];
};

// Per-model effective view/socket baselines. Field order is seat distance,
// height, center XYZ, absolute car radius, additive bike radius, rotation XYZ,
// then virtual-wheel visibility (-1 inherit, 0 hide).
static const BuiltInVehicleViewDefaults gBuiltInVehicleViewDefaults[] = {
	{ MI_PIZZABOY, ModelSets::MODEL_SET_CLASSIC,
		{ -15,-12, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_PCJ600, ModelSets::MODEL_SET_CLASSIC,
		{ -8,0, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_CLASSIC,
		{ -7,-22, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_SANCHEZ, ModelSets::MODEL_SET_CLASSIC,
		{ -15,-15, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_STINGER, ModelSets::MODEL_SET_MODERN,
		{ 8,5, 0,19,9, 21,0, -42,0,0, 0 } },
	{ MI_INFERNUS, ModelSets::MODEL_SET_MODERN,
		{ 0,0, 1,-5,4, 0,0, -36,0,0, 0 } },
	{ MI_BANSHEE, ModelSets::MODEL_SET_MODERN,
		{ 0,6, 0,2,12, 18,0, -50,0,0, 0 } },
	{ MI_ADMIRAL, ModelSets::MODEL_SET_MODERN,
		{ 0,0, 2,7,7, 25,0, -57,0,0, 0 } },
	{ MI_PIZZABOY, ModelSets::MODEL_SET_MODERN,
		{ 0,-33, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_MODERN,
		{ 0,-38, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_PCJ600, ModelSets::MODEL_SET_MODERN,
		{ -19,-16, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_SANCHEZ, ModelSets::MODEL_SET_MODERN,
		{ -22,-27, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_SPEEDER, ModelSets::MODEL_SET_CLASSIC,
		{ 0,0, 3,4,12, 14,0, -22,0,0, 0 } },
	{ MI_SPEEDER, ModelSets::MODEL_SET_MODERN,
		{ 0,0, 3,4,9, 16,0, -26,6,0, 0 } },
};

const BuiltInVehicleViewDefaults *FindBuiltInVehicleViewDefaults(int model,
	ModelSets::eModelSet modelSet)
{
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInVehicleViewDefaults); i++)
		if(gBuiltInVehicleViewDefaults[i].model == model &&
		   gBuiltInVehicleViewDefaults[i].modelSet == modelSet)
			return &gBuiltInVehicleViewDefaults[i];
	return nil;
}

void GetDefaultVehicleViewCalibration(int model,
	ModelSets::eModelSet modelSet, VehicleViewCalibration &calibration)
{
	calibration = VehicleViewCalibration();
	calibration.seatDistanceCm = model == MI_SANCHEZ ? -23 : 0;
	const BuiltInVehicleViewDefaults *defaults =
		FindBuiltInVehicleViewDefaults(model, modelSet);
	if(!defaults && modelSet != ModelSets::MODEL_SET_CLASSIC)
		defaults = FindBuiltInVehicleViewDefaults(model,
			ModelSets::MODEL_SET_CLASSIC);
	if(!defaults)
		return;
	const int *value = defaults->value;
	calibration.seatDistanceCm = value[0];
	calibration.seatHeightCm = value[1];
	calibration.wheelCenterXCm = value[2];
	calibration.wheelCenterYCm = value[3];
	calibration.wheelCenterZCm = value[4];
	calibration.carWheelRadiusCm = value[5];
	calibration.wheelRadiusCm = value[6];
	calibration.carWheelPitchHalfDeg = value[7];
	calibration.carWheelYawHalfDeg = value[8];
	calibration.carWheelRollHalfDeg = value[9];
	calibration.carWheelVisibilityOverride = value[10];
}

void GetVehicleViewCalibrationSection(int model,
	ModelSets::eModelSet modelSet, char *section)
{
	if(modelSet == ModelSets::MODEL_SET_CLASSIC)
		sprintf(section, "VehicleView_%d", model);
	else
		sprintf(section, "VehicleView%s_%d",
			GetVehicleModelSetPrefix(modelSet), model);
}

int ReadVehicleViewCalibrationValue(const char *section,
	const char *classicSection, const char *name, int fallback)
{
	const char *path = GetVrSettingsPath();
	if(IsReplacementVehicleModelSetActive())
		fallback = (int)(int32)GetPrivateProfileIntA(classicSection, name,
			fallback, path);
	return (int)(int32)GetPrivateProfileIntA(section, name, fallback, path);
}

int ReadCarWheelModelRadius(const char *section,
	const char *classicSection, int categoryRadiusCm, int fallback)
{
	const char *path = GetVrSettingsPath();
	int radius;
	if(IsReplacementVehicleModelSetActive()){
		if(ReadVrProfileInt(section, "WheelRadiusV2Cm", path, &radius))
			return radius == 0 ? 0 : Min(Max(radius,
				VR_CAR_WHEEL_MIN_RADIUS_CM), VR_CAR_WHEEL_MAX_RADIUS_CM);
		// Preserve a model's old Modern-only adjustment before falling back to
		// Classic. Modern inherits Classic only when it has no setting of its own.
		int legacyAdjustment;
		if(ReadVrProfileInt(section, "WheelRadiusCm", path,
		   &legacyAdjustment))
			return legacyAdjustment == 0 ? 0 :
				Min(Max(categoryRadiusCm+legacyAdjustment,
					VR_CAR_WHEEL_MIN_RADIUS_CM),
					VR_CAR_WHEEL_MAX_RADIUS_CM);
		if(ReadVrProfileInt(classicSection, "WheelRadiusV2Cm", path, &radius))
			return radius == 0 ? 0 : Min(Max(radius,
				VR_CAR_WHEEL_MIN_RADIUS_CM), VR_CAR_WHEEL_MAX_RADIUS_CM);
	}
	if(ReadVrProfileInt(classicSection, "WheelRadiusV2Cm", path, &radius))
		return radius == 0 ? 0 : Min(Max(radius,
			VR_CAR_WHEEL_MIN_RADIUS_CM), VR_CAR_WHEEL_MAX_RADIUS_CM);
	int legacyAdjustment;
	if(ReadVrProfileInt(classicSection, "WheelRadiusCm", path,
	   &legacyAdjustment))
		return legacyAdjustment == 0 ? 0 :
			Min(Max(categoryRadiusCm+legacyAdjustment,
				VR_CAR_WHEEL_MIN_RADIUS_CM),
				VR_CAR_WHEEL_MAX_RADIUS_CM);
	return fallback;
}

VehicleViewCalibration *GetVehicleViewCalibration(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return nil;
	VehicleViewCalibration &calibration =
		gVehicleViewCalibration[model-MI_FIRST_VEHICLE];
	if(calibration.valid)
		return &calibration;
	char section[64], classicSection[64];
	GetVehicleViewCalibrationSection(model, GetActiveVehicleModelSet(), section);
	GetVehicleViewCalibrationSection(model, ModelSets::MODEL_SET_CLASSIC,
		classicSection);
	VehicleViewCalibration defaults;
	GetDefaultVehicleViewCalibration(model, GetActiveVehicleModelSet(), defaults);
	calibration.seatDistanceCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "SeatDistanceCm", defaults.seatDistanceCm),
		-100), 100);
	calibration.seatHeightCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "SeatHeightCm", defaults.seatHeightCm), -100), 100);
	calibration.defaultSeatHeightCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "DefaultSeatHeightCm", model == MI_SANCHEZ ? -32 : 0), -100), 100);
	calibration.defaultSeatDistanceCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "DefaultSeatDistanceCm", 0), -100), 100);
	calibration.wheelCenterXCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "WheelCenterXCm", defaults.wheelCenterXCm), -100), 100);
	calibration.wheelCenterYCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "WheelCenterYCm", defaults.wheelCenterYCm), -100), 100);
	calibration.wheelCenterZCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "WheelCenterZCm", defaults.wheelCenterZCm), -100), 100);
	if(GetVrCarModelIndex(model) >= 0){
		const bool boat = ((CVehicleModelInfo*)CModelInfo::GetModelInfo(model))->m_vehicleType == VEHICLE_TYPE_BOAT;
		VehicleCategoryCalibration *category =
			&gVehicleCategoryCalibration[boat ? VR_VEHICLE_CATEGORY_BOAT : VR_VEHICLE_CATEGORY_CAR];
		calibration.carWheelRadiusCm = ReadCarWheelModelRadius(section,
			classicSection, category->carWheelRadiusCm,
			defaults.carWheelRadiusCm);
		calibration.carWheelPitchHalfDeg = Min(Max(
			ReadVehicleViewCalibrationValue(section, classicSection,
				"WheelPitchHalfDeg", defaults.carWheelPitchHalfDeg),
			-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
			VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
		calibration.carWheelYawHalfDeg = Min(Max(
			ReadVehicleViewCalibrationValue(section, classicSection,
				"WheelYawHalfDeg", defaults.carWheelYawHalfDeg),
			-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
			VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
		calibration.carWheelRollHalfDeg = Min(Max(
			ReadVehicleViewCalibrationValue(section, classicSection,
				"WheelRollHalfDeg", defaults.carWheelRollHalfDeg),
			-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
			VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
		// Visibility is deliberately not inherited across model sets. Geometry
		// differs, so a per-model exception for Classic must not silently affect
		// the replacement model with the same numeric ID.
		const int storedVisibility = (int)(int32)GetPrivateProfileIntA(section,
			"VirtualWheelVisibility", defaults.carWheelVisibilityOverride,
			GetVrSettingsPath());
		// A short-lived development build wrote +1 for SHOW. Global HIDE is now
		// deliberately absolute, so normalise that ineffective legacy state to
		// INHERIT while preserving explicit per-model HIDE.
		calibration.carWheelVisibilityOverride =
			storedVisibility == 0 ? 0 : -1;
	}
	calibration.wheelRadiusCm = Min(Max(ReadVehicleViewCalibrationValue(
		section, classicSection, "WheelRadiusCm", defaults.wheelRadiusCm),
		-20), 40);
	calibration.valid = true;
	return &calibration;
}

void SaveVehicleViewCalibrationValue(int model, const char *name, int value)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE || !name)
		return;
	char section[64], textValue[32];
	GetVehicleViewCalibrationSection(model, GetActiveVehicleModelSet(), section);
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, name, textValue,
		GetVrSettingsPath());
}

int GetVrBikeModelIndex(int model)
{
	for(int index = 0; index < VR_BIKE_MODEL_COUNT; index++)
		if(gVrBikeModels[index] == model)
			return index;
	return -1;
}

CBike *GetActivePlayerBike()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && vehicle->IsBike() ? (CBike*)vehicle : nil;
}

CVehicle *GetActivePlayerCar()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && (vehicle->IsBoat() || (vehicle->IsCar() && !vehicle->IsRealHeli() &&
		!vehicle->IsRealPlane())) ? vehicle : nil;
}

int GetDrivingTypeForVehicle(CVehicle *vehicle)
{
	if(!vehicle)
		return VR_DRIVING_DEFAULT;
	if(IsVehicleThirdPersonActive() && vehicle == FindPlayerVehicle())
		return VR_DRIVING_DEFAULT;
	if(vehicle->IsBike())
		return gBikeDrivingType;
	if(vehicle->IsBoat())
		return gBoatDrivingType;
	if(vehicle->IsCar() && !vehicle->IsRealHeli() && !vehicle->IsRealPlane())
		return gCarDrivingType;
	return VR_DRIVING_DEFAULT;
}

void ApplyVehicleViewTranslation(CMatrix &baseCamera, CVehicle *vehicle)
{
	if(!vehicle || IsStereoCutsceneActive() || IsVehicleThirdPersonActive())
		return;
	const int categoryIndex = GetVrVehicleCategory(vehicle);
	if(categoryIndex < 0)
		return;
	VehicleCategoryCalibration *category =
		GetVehicleCategoryCalibration(vehicle);
	VehicleViewCalibration *calibration =
		GetVehicleViewCalibration(vehicle->GetModelIndex());
	if(!category || !calibration)
		return;
	const bool defaultView = GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT &&
		categoryIndex != VR_VEHICLE_CATEGORY_HELI;
	// Translate the shared tracking anchor rather than only the rendered eyes.
	// The HMD, controllers, holsters and immersive controls then move together.
	baseCamera.GetPosition().z +=
		(float)(defaultView ? gDefaultSeatHeightCm[categoryIndex]+calibration->defaultSeatHeightCm :
			category->seatHeightCm+calibration->seatHeightCm)/100.0f;
	baseCamera.GetPosition() += vehicle->GetForward()*
		((float)(defaultView ? gDefaultSeatDistanceCm[categoryIndex]+calibration->defaultSeatDistanceCm :
			category->seatDistanceCm+calibration->seatDistanceCm)/100.0f);
}

bool IsVrDrivingEnvironmentActive()
{
	// Loading, frontend and cinema transitions temporarily expose partially
	// rebuilt player/vehicle state.  Immersive driving must be completely inert
	// there: querying that state from the OpenXR input/render path can race the
	// scripted transition into the opening hotel drive.
	if(IsVehicleThirdPersonActive() || gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   FrontEndMenuManager.m_bWantToRestart ||
	   FrontEndMenuManager.m_bWantToLoad ||
	   CGame::playingIntro ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return false;
	CPlayerPed *player = FindPlayerPed();
	CVehicle *vehicle = FindPlayerVehicle();
	return player && vehicle && vehicle->pDriver == player &&
		GetDrivingTypeForVehicle(vehicle) != VR_DRIVING_DEFAULT;
}

bool IsImmersiveDrivingEnvironmentActive()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_IMMERSIVE &&
		IsVrDrivingEnvironmentActive();
}

bool IsMotionDrivingEnvironmentActive()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_MOTION &&
		IsVrDrivingEnvironmentActive();
}

bool IsImmersiveBikeDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsImmersiveDrivingEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike &&
		(!expected || expected == bike) &&
		GetVrBikeModelIndex(bike->GetModelIndex()) >= 0;
}

bool IsImmersiveCarDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsImmersiveDrivingEnvironmentActive())
		return false;
	CVehicle *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

bool IsImmersiveDrivingActiveInternal(CVehicle *expected = nil)
{
	return IsImmersiveBikeDrivingActiveInternal(expected) ||
		IsImmersiveCarDrivingActiveInternal(expected);
}

bool IsVrBikeDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsVrDrivingEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike && (!expected || expected == bike) &&
		GetVrBikeModelIndex(bike->GetModelIndex()) >= 0;
}

bool IsVrCarDrivingActiveInternal(CVehicle *expected = nil)
{
	if(!IsVrDrivingEnvironmentActive())
		return false;
	CVehicle *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

bool IsVrDrivingActiveInternal(CVehicle *expected = nil)
{
	return IsVrBikeDrivingActiveInternal(expected) ||
		IsVrCarDrivingActiveInternal(expected);
}

const char *GetActiveVrBikeName()
{
	CBike *bike = GetActivePlayerBike();
	const int index = bike ? GetVrBikeModelIndex(bike->GetModelIndex()) : -1;
	return index >= 0 ? gVrBikeNames[index] : "NO BIKE";
}

const char *GetActiveVrVehicleName()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle ? GetVrVehicleModelName(vehicle->GetModelIndex()) :
		"NO VEHICLE";
}

int GetDefaultBikeHandleOffsetX(int bikeIndex, int hand)
{
	static const int halfWidthCm[VR_BIKE_MODEL_COUNT] = {
		36, 36, 32, 26, 26, 32
	};
	if(bikeIndex < 0 || bikeIndex >= VR_BIKE_MODEL_COUNT ||
	   hand < 0 || hand >= EYE_COUNT)
		return 0;
	return halfWidthCm[bikeIndex]*WEAPON_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
}

struct BuiltInBikeHandleDefaults
{
	int model;
	ModelSets::eModelSet modelSet;
	int hand[EYE_COUNT][6];
};

// Absolute release baselines captured from the calibrated project profile.
// Hand order is LEFT, RIGHT; fields are offset XYZ then rotation XYZ.
static const BuiltInBikeHandleDefaults gBuiltInBikeHandleDefaults[] = {
	{ MI_ANGEL, ModelSets::MODEL_SET_CLASSIC, {
		{ -61,-83,82, 137,0,0 },
		{ 61,-82,84, 138,0,0 } } },
	{ MI_FREEWAY, ModelSets::MODEL_SET_CLASSIC, {
		{ -64,-82,85, 135,0,0 },
		{ 64,-82,85, 133,0,0 } } },
	{ MI_PCJ600, ModelSets::MODEL_SET_CLASSIC, {
		{ -66,-27,81, -193,1,-15 },
		{ 66,-27,83, -192,0,0 } } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_CLASSIC, {
		{ -65,-35,84, 156,0,0 },
		{ 66,-33,84, 156,15,0 } } },
	{ MI_PIZZABOY, ModelSets::MODEL_SET_CLASSIC, {
		{ -68,-34,83, -202,0,0 },
		{ 68,-34,83, 164,0,0 } } },
	{ MI_SANCHEZ, ModelSets::MODEL_SET_CLASSIC, {
		{ -64,-23,46, -222,5,0 },
		{ 65,-24,46, 149,0,0 } } },
	{ MI_PIZZABOY, ModelSets::MODEL_SET_MODERN, {
		{ -55,-63,80, -232,-13,-1 },
		{ 55,-63,83, 126,0,0 } } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_MODERN, {
		{ -55,-65,80, 121,0,0 },
		{ 56,-63,80, 134,-10,0 } } },
	{ MI_FREEWAY, ModelSets::MODEL_SET_MODERN, {
		{ -61,-21,-8, 135,0,0 },
		{ 61,-21,-8, 133,0,0 } } },
};

bool GetBuiltInBikeHandleCalibrationForSet(int model, int hand,
	ModelSets::eModelSet modelSet, BikeHandleCalibration *calibration)
{
	if(!calibration || hand < 0 || hand >= EYE_COUNT)
		return false;
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInBikeHandleDefaults); i++){
		if(gBuiltInBikeHandleDefaults[i].model != model ||
		   gBuiltInBikeHandleDefaults[i].modelSet != modelSet)
			continue;
		const int *value = gBuiltInBikeHandleDefaults[i].hand[hand];
		calibration->offsetX = value[0];
		calibration->offsetY = value[1];
		calibration->offsetZ = value[2];
		calibration->rotationX = value[3];
		calibration->rotationY = value[4];
		calibration->rotationZ = value[5];
		calibration->valid = true;
		return true;
	}
	if(modelSet != ModelSets::MODEL_SET_CLASSIC)
		return GetBuiltInBikeHandleCalibrationForSet(model, hand,
			ModelSets::MODEL_SET_CLASSIC, calibration);
	return false;
}

bool GetBuiltInBikeHandleCalibration(int model, int hand,
	BikeHandleCalibration *calibration)
{
	return GetBuiltInBikeHandleCalibrationForSet(model, hand,
		GetActiveVehicleModelSet(), calibration);
}

void GetBikeHandleCalibrationSection(int model, int hand,
	ModelSets::eModelSet modelSet, char *section)
{
	sprintf(section, "BikeHandle%s_%d_%s",
		GetVehicleModelSetPrefix(modelSet), model,
		hand == 0 ? "Left" : "Right");
}

BikeHandleCalibration *GetBikeHandleCalibration(int model, int hand)
{
	const int bikeIndex = GetVrBikeModelIndex(model);
	if(bikeIndex < 0 || hand < 0 || hand >= EYE_COUNT)
		return nil;
	BikeHandleCalibration &calibration =
		gBikeHandleCalibration[bikeIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64], classicSection[64];
	GetBikeHandleCalibrationSection(model, hand,
		GetActiveVehicleModelSet(), section);
	GetBikeHandleCalibrationSection(model, hand,
		ModelSets::MODEL_SET_CLASSIC, classicSection);
	BikeHandleCalibration defaults;
	if(!GetBuiltInBikeHandleCalibration(model, hand, &defaults))
		defaults.offsetX = GetDefaultBikeHandleOffsetX(bikeIndex, hand);
	calibration.offsetX = ReadVehicleViewCalibrationValue(section,
		classicSection, "OffsetX", defaults.offsetX);
	calibration.offsetY = ReadVehicleViewCalibrationValue(section,
		classicSection, "OffsetY", defaults.offsetY);
	calibration.offsetZ = ReadVehicleViewCalibrationValue(section,
		classicSection, "OffsetZ", defaults.offsetZ);
	calibration.rotationX = ReadVehicleViewCalibrationValue(section,
		classicSection, "RotationX", defaults.rotationX);
	calibration.rotationY = ReadVehicleViewCalibrationValue(section,
		classicSection, "RotationY", defaults.rotationY);
	calibration.rotationZ = ReadVehicleViewCalibrationValue(section,
		classicSection, "RotationZ", defaults.rotationZ);
	calibration.offsetX = Min(Max(calibration.offsetX, -300), 300);
	calibration.offsetY = Min(Max(calibration.offsetY, -300), 300);
	calibration.offsetZ = Min(Max(calibration.offsetZ, -300), 300);
	calibration.rotationX = Min(Max(calibration.rotationX, -720), 720);
	calibration.rotationY = Min(Max(calibration.rotationY, -720), 720);
	calibration.rotationZ = Min(Max(calibration.rotationZ, -720), 720);
	calibration.valid = true;
	return &calibration;
}

void SaveBikeHandleCalibrationValue(int model, int hand,
	const char *name, int value)
{
	if(GetVrBikeModelIndex(model) < 0 || hand < 0 || hand >= EYE_COUNT ||
	   !name)
		return;
	char section[64], textValue[32];
	GetBikeHandleCalibrationSection(model, hand,
		GetActiveVehicleModelSet(), section);
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, name, textValue,
		GetVrSettingsPath());
}

int GetVrCarModelIndex(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return -1;
	CBaseModelInfo *base = CModelInfo::GetModelInfo(model);
	if(!base || (((CVehicleModelInfo*)base)->m_vehicleType != VEHICLE_TYPE_CAR &&
		((CVehicleModelInfo*)base)->m_vehicleType != VEHICLE_TYPE_BOAT))
		return -1;
	return model-MI_FIRST_VEHICLE;
}

BikeLeanCalibration *GetBikeLeanCalibration(int model)
{
	const int bikeIndex = GetVrBikeModelIndex(model);
	if(bikeIndex < 0)
		return nil;
	BikeLeanCalibration &calibration = gBikeLeanCalibration[bikeIndex];
	if(calibration.valid)
		return &calibration;
	char section[64], classicSection[64];
	sprintf(section, "BikeControl%s_%d",
		GetVehicleModelSetPrefix(GetActiveVehicleModelSet()), model);
	sprintf(classicSection, "BikeControl_%d", model);
	const int defaultWheelieHeightCm = model == MI_SANCHEZ ? 30 : 20;
	calibration.wheelieHeightCm = Min(Max(
		ReadVehicleViewCalibrationValue(section, classicSection,
			"WheelieHeightCm", defaultWheelieHeightCm), 5), 100);
	calibration.standHeightCm = Min(Max(
		ReadVehicleViewCalibrationValue(section, classicSection,
			"StandHeightCm", 20), 5), 100);
	calibration.valid = true;
	return &calibration;
}

void SaveBikeLeanCalibrationValue(int model, const char *name, int value)
{
	if(GetVrBikeModelIndex(model) < 0 || !name)
		return;
	char section[64], textValue[32];
	sprintf(section, "BikeControl%s_%d",
		GetVehicleModelSetPrefix(GetActiveVehicleModelSet()), model);
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, name, textValue,
		GetVrSettingsPath());
}

void ResetImmersiveBikeInteraction()
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gBikeHandleGrabbed[hand] = false;
		gBikeHandleGripDown[hand] = false;
		gBikeHandleDistance[hand] = 1000.0f;
		gBikeLeanReferenceValid[hand] = false;
		gBikeLeanReferenceTrackingY[hand] = 0.0f;
	}
	gImmersiveBikeSteering = 0.0f;
	gImmersiveBikePhysicalAngle = 0.0f;
	gImmersiveBikeDesiredAngle = 0.0f;
	gImmersiveBikeSteeringOverflow = 0.0f;
	gBikeHandleUnavailableMask = 0;
	gBikeSteeringChordState = ImmersiveSteeringChordState();
	gBikeOneHandSteeringState = BikeOneHandSteeringState();
	gImmersiveBikeThrottle = 0.0f;
	gBikeThrottleGestureActive = false;
	gBikeThrottleReferenceOrientation = {};
	gBikeThrottleReferenceOrientation.w = 1.0f;
	gBikeThrottleRawTwistAngle = 0.0f;
	gBikeThrottleTwistValid = false;
	gBikeThrottleCaptureCount = 0;
	gImmersiveBikeLean = 0.0f;
	gBikeLeanGestureState = 0;
	gBikeThrottleReferenceValid = false;
}

void ResetImmersiveCarInteraction()
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gCarWheelGrabbed[hand] = false;
		gCarWheelGripDown[hand] = false;
		gCarWheelDistance[hand] = 1000.0f;
		gCarHornContact[hand] = false;
		gCarHornArmed[hand] = false;
		gCarHornPreviousDistance[hand] = 1000.0f;
	}
	gImmersiveCarSteering = 0.0f;
	gImmersiveCarPhysicalAngle = 0.0f;
	gImmersiveCarDesiredAngle = 0.0f;
	gImmersiveCarSteeringOverflow = 0.0f;
	gCarWheelUnavailableMask = 0;
	gCarSteeringChordState = ImmersiveSteeringChordState();
	gImmersiveCarHornPressed = false;
}

void InvalidateImmersiveTrackingReferences()
{
	// Recreating gameplay space changes the coordinates of every raw OpenXR
	// pose. Keep the currently applied wheel/handle angle and grab ownership, but
	// force the next held frame to latch a fresh pair in the new space. A full
	// interaction reset here would visibly snap both vehicles back to neutral.
	gCarSteeringChordState = ImmersiveSteeringChordState();
	gBikeSteeringChordState = ImmersiveSteeringChordState();
	gBikeOneHandSteeringState = BikeOneHandSteeringState();
	// A held two-hand bike normally uses the literal absolute v0.4.1 chord.
	// Recreating gameplay space is the one exception: force its first valid
	// observation to preserve the already applied physical bar angle.
	gBikeSteeringChordState.rebaseOnNextValid = true;
	gBikeThrottleGestureActive = false;
	gBikeThrottleReferenceValid = false;
	gImmersiveBikeThrottle = 0.0f;
	gBikeThrottleRawTwistAngle = 0.0f;
	gBikeThrottleTwistValid = false;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gBikeLeanReferenceValid[hand] = false;
		gBikeLeanReferenceTrackingY[hand] = 0.0f;
	}
	gBikeLeanGestureState = 0;
	gImmersiveBikeLean = 0.0f;
}

void ResetMotionSteeringInteraction()
{
	gMotionSteeringVehicle = nil;
	gMotionVehicleSteering = 0.0f;
	gMotionVehiclePhysicalAngle = 0.0f;
	gMotionSteeringReferenceValid = false;
	gMotionSteeringReferenceHeading = 0.0f;
}

void ResetImmersiveDrivingInteraction()
{
	ResetImmersiveBikeInteraction();
	ResetImmersiveCarInteraction();
	ResetMotionSteeringInteraction();
	gVrRadioButtonDown = false;
	gVrRadioChangeJustPressed = false;
}

int GetVehicleCalibrationMenuItemCount()
{
	static const int bikeItems[] = {
		VR_BIKE_CAL_HAND,
		VR_BIKE_CAL_OFFSET_X,
		VR_BIKE_CAL_OFFSET_Y,
		VR_BIKE_CAL_OFFSET_Z,
		VR_BIKE_CAL_ROT_X,
		VR_BIKE_CAL_ROT_Y,
		VR_BIKE_CAL_ROT_Z,
		VR_BIKE_CAL_GLOBAL_CENTER_X,
		VR_BIKE_CAL_GLOBAL_CENTER_Y,
		VR_BIKE_CAL_GLOBAL_CENTER_Z,
		VR_BIKE_CAL_GLOBAL_RADIUS,
		VR_BIKE_CAL_MODEL_CENTER_X,
		VR_BIKE_CAL_MODEL_CENTER_Y,
		VR_BIKE_CAL_MODEL_CENTER_Z,
		VR_BIKE_CAL_MODEL_RADIUS,
		VR_BIKE_CAL_WHEELIE_HEIGHT,
		VR_BIKE_CAL_STAND_HEIGHT,
		VR_BIKE_CAL_BACK
	};
	static const int carWheelItems[] = {
		VR_BIKE_CAL_GLOBAL_CENTER_X,
		VR_BIKE_CAL_GLOBAL_CENTER_Y,
		VR_BIKE_CAL_GLOBAL_CENTER_Z,
		VR_BIKE_CAL_GLOBAL_RADIUS,
		VR_CAR_CAL_GLOBAL_PITCH,
		VR_CAR_CAL_GLOBAL_YAW,
		VR_CAR_CAL_GLOBAL_ROLL,
		VR_BIKE_CAL_MODEL_CENTER_X,
		VR_BIKE_CAL_MODEL_CENTER_Y,
		VR_BIKE_CAL_MODEL_CENTER_Z,
		VR_BIKE_CAL_MODEL_RADIUS,
		VR_CAR_CAL_MODEL_PITCH,
		VR_CAR_CAL_MODEL_YAW,
		VR_CAR_CAL_MODEL_ROLL,
		VR_BIKE_CAL_BACK
	};
	return IsImmersiveCarDrivingActiveInternal() ?
		(int)ARRAY_SIZE(carWheelItems) :
		(int)ARRAY_SIZE(bikeItems);
}

int GetVehicleCalibrationMenuItemForRow(int row)
{
	static const int bikeItems[] = {
		VR_BIKE_CAL_HAND,
		VR_BIKE_CAL_OFFSET_X,
		VR_BIKE_CAL_OFFSET_Y,
		VR_BIKE_CAL_OFFSET_Z,
		VR_BIKE_CAL_ROT_X,
		VR_BIKE_CAL_ROT_Y,
		VR_BIKE_CAL_ROT_Z,
		VR_BIKE_CAL_GLOBAL_CENTER_X,
		VR_BIKE_CAL_GLOBAL_CENTER_Y,
		VR_BIKE_CAL_GLOBAL_CENTER_Z,
		VR_BIKE_CAL_GLOBAL_RADIUS,
		VR_BIKE_CAL_MODEL_CENTER_X,
		VR_BIKE_CAL_MODEL_CENTER_Y,
		VR_BIKE_CAL_MODEL_CENTER_Z,
		VR_BIKE_CAL_MODEL_RADIUS,
		VR_BIKE_CAL_WHEELIE_HEIGHT,
		VR_BIKE_CAL_STAND_HEIGHT,
		VR_BIKE_CAL_BACK
	};
	static const int carWheelItems[] = {
		VR_BIKE_CAL_GLOBAL_CENTER_X,
		VR_BIKE_CAL_GLOBAL_CENTER_Y,
		VR_BIKE_CAL_GLOBAL_CENTER_Z,
		VR_BIKE_CAL_GLOBAL_RADIUS,
		VR_CAR_CAL_GLOBAL_PITCH,
		VR_CAR_CAL_GLOBAL_YAW,
		VR_CAR_CAL_GLOBAL_ROLL,
		VR_BIKE_CAL_MODEL_CENTER_X,
		VR_BIKE_CAL_MODEL_CENTER_Y,
		VR_BIKE_CAL_MODEL_CENTER_Z,
		VR_BIKE_CAL_MODEL_RADIUS,
		VR_CAR_CAL_MODEL_PITCH,
		VR_CAR_CAL_MODEL_YAW,
		VR_CAR_CAL_MODEL_ROLL,
		VR_BIKE_CAL_BACK
	};
	if(IsImmersiveCarDrivingActiveInternal())
		return row >= 0 && row < (int)ARRAY_SIZE(carWheelItems) ?
			carWheelItems[row] : VR_BIKE_CAL_BACK;
	return row >= 0 && row < (int)ARRAY_SIZE(bikeItems) ?
		bikeItems[row] : VR_BIKE_CAL_BACK;
}

int FindHolsterPointForSlot(int slot)
{
	if(gHolsterCalibrationCheat.active &&
	   gHolsterCalibrationCheat.previewSlot >= 0){
		// Put the temporary weapon at the selected calibration point even when
		// its native inventory category is normally assigned elsewhere.
		if(slot == gHolsterCalibrationCheat.previewSlot)
			return gHolsterCalibrationCheat.selectedPoint;
		// Hide the permanent category which normally occupies that point. This
		// prevents two different slots from being rendered on top of each other.
		if(gHolsterCalibrationCheat.selectedPoint >= 0 &&
		   gHolsterCalibrationCheat.selectedPoint < HOLSTER_POINT_COUNT &&
		   gHolsterPointWeaponSlot[
			gHolsterCalibrationCheat.selectedPoint] == slot)
			return -1;
	}
	for(int point = 0; point < HOLSTER_POINT_COUNT; point++)
		if(gHolsterPointWeaponSlot[point] == slot)
			return point;
	return -1;
}

const char *GetHolsterSlotCategoryName(int slot)
{
	switch(slot){
	case WEAPONSLOT_MELEE: return "MELEE";
	case WEAPONSLOT_PROJECTILE: return "THROWABLE";
	case WEAPONSLOT_HANDGUN: return "HANDGUN";
	case WEAPONSLOT_SHOTGUN: return "SHOTGUN";
	case WEAPONSLOT_SUBMACHINEGUN: return "SMG";
	case WEAPONSLOT_RIFLE: return "RIFLE";
	case WEAPONSLOT_HEAVY: return "HEAVY";
	case WEAPONSLOT_SNIPER: return "SCOPED";
	case WEAPONSLOT_OTHER: return "CAMERA";
	default: return "EMPTY";
	}
}

void FormatHolsterSlotDisplayName(int slot, char *buffer)
{
	if(!buffer)
		return;
	if(slot < 0){
		strcpy(buffer, "EMPTY");
		return;
	}
	// A holster point owns an inventory category, while the icon identifies
	// the concrete weapon currently stored in it.
	if(IsVrWeaponSlotOwned(slot))
		sprintf(buffer, "%s", GetHolsterSlotCategoryName(slot));
	else
		sprintf(buffer, "%s SLOT EMPTY", GetHolsterSlotCategoryName(slot));
}

void CycleHolsterPointSlot(int point, int direction)
{
	if(point < 0 || point >= HOLSTER_POINT_COUNT || direction == 0)
		return;
	// Grenades, tear gas and Molotovs always share Vice City's projectile slot;
	// keeping its centre-chest point fixed makes it impossible to lose behind a
	// configurable duplicate assignment.
	if(point == HOLSTER_CHEST_CENTER)
		return;
	int choice = 0;
	for(int i = 0; i < (int)ARRAY_SIZE(gHolsterSlotChoices); i++)
		if(gHolsterSlotChoices[i] == gHolsterPointWeaponSlot[point]){
			choice = i;
			break;
		}
	for(int step = 0; step < (int)ARRAY_SIZE(gHolsterSlotChoices); step++){
		choice = (choice+(direction > 0 ? 1 : -1)+
			ARRAY_SIZE(gHolsterSlotChoices)) % ARRAY_SIZE(gHolsterSlotChoices);
		const int candidate = gHolsterSlotChoices[choice];
		const int occupiedPoint = FindHolsterPointForSlot(candidate);
		if(candidate >= 0 && occupiedPoint >= 0 && occupiedPoint != point)
			continue;
		gHolsterPointWeaponSlot[point] = candidate;
		SaveVrSetting(gHolsterPointSettingNames[point], candidate);
		debug("[OpenXR] Holster %s: %s\n", gHolsterPointNames[point],
			GetHolsterSlotCategoryName(candidate));
		return;
	}
}

bool IsHolsterCalibrationRemoteGrenadeBlocked(int weaponType)
{
	CPlayerPed *player = FindPlayerPed();
	return weaponType == WEAPONTYPE_DETONATOR_GRENADE && player &&
		CProjectileInfo::HasDetonatorProjectile(player);
}

int FindNextHolsterCalibrationWeaponIndex(int start)
{
	const int count = ARRAY_SIZE(gHolsterCalibrationWeaponTypes);
	for(int checked = 0; checked < count; checked++){
		const int index = (start+checked+count) % count;
		if(!IsHolsterCalibrationRemoteGrenadeBlocked(
		   gHolsterCalibrationWeaponTypes[index]))
			return index;
	}
	return -1;
}

int GetNextHolsterCalibrationWeaponType()
{
	const int index = FindNextHolsterCalibrationWeaponIndex(
		gHolsterCalibrationCheat.nextWeaponIndex);
	return index >= 0 ? gHolsterCalibrationWeaponTypes[index] : -1;
}

bool CanStartHolsterCalibration(CPlayerPed *player)
{
	return player && !player->DyingOrDead() && FindPlayerVehicle() == nil &&
		!CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() &&
		CWorld::Players[CWorld::PlayerInFocus].m_WBState == WBSTATE_PLAYING &&
		gGameState == GS_PLAYING_GAME && !CTheScripts::IsPlayerOnAMission();
}

bool IsHolsterCalibrationOwnerValid(CPlayerPed *player)
{
	return gHolsterCalibrationCheat.active && player &&
		gHolsterCalibrationCheat.owner == player &&
		gHolsterCalibrationCheat.ownerPoolHandle >= 0 &&
		CPools::GetPedPool() &&
		CPools::GetPedPool()->GetAt(
			gHolsterCalibrationCheat.ownerPoolHandle) == player;
}

void HoldHolsterCalibrationSnapshotModelRefs()
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		for(int part = 0; part < 2; part++)
			gHolsterCalibrationCheat.snapshotModelRefHeld[slot][part] = false;
		const CWeapon &saved =
			gHolsterCalibrationCheat.originalWeapons[slot];
		CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(saved.m_eWeaponType);
		if(!info)
			continue;
		const int models[2] = { info->m_nModelId, info->m_nModel2Id };
		for(int part = 0; part < 2; part++){
			if(models[part] < 0)
				continue;
			CBaseModelInfo *model = CModelInfo::GetModelInfo(models[part]);
			if(!model)
				continue;
			model->AddRef();
			gHolsterCalibrationCheat.snapshotModelRefHeld[slot][part] = true;
		}
	}
}

void ReleaseHolsterCalibrationSnapshotModelRefs()
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		const CWeapon &saved =
			gHolsterCalibrationCheat.originalWeapons[slot];
		CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(saved.m_eWeaponType);
		const int models[2] = {
			info ? info->m_nModelId : -1,
			info ? info->m_nModel2Id : -1
		};
		for(int part = 0; part < 2; part++){
			if(!gHolsterCalibrationCheat.snapshotModelRefHeld[slot][part])
				continue;
			CBaseModelInfo *model = models[part] >= 0 ?
				CModelInfo::GetModelInfo(models[part]) : nil;
			if(model)
				model->RemoveRef();
			gHolsterCalibrationCheat.snapshotModelRefHeld[slot][part] = false;
		}
	}
}

void ClearHolsterCalibrationWeaponTransients()
{
	// A synthetic physical drop would continue referring to a slot while the
	// preview replaces that slot. Clear both hand and toss representations
	// instead; the exact native inventory object is restored below.
	for(int hand = 0; hand < EYE_COUNT; hand++){
		ClearWeaponSupportForHand(hand);
		gHeldWeaponSlot[hand] = -1;
		gWeaponHolsterSelection[hand] = -1;
		gTrackedWeaponRenderMatrixSlot[hand] = -1;
		gTrackedWeaponContactMatrixSlot[hand] = -1;
		gTrackedWeaponContactMatrixType[hand] = -1;
		ClearDroppedWeapon(hand);
		gManualReload[hand] = ManualReloadState();
		gManualReloadGripDown[hand] = true;
		gWeaponHolsterGripDown[hand] = true;
		gTrackedWeaponTriggerPressed[hand] = false;
		gTrackedWeaponTriggerJustPressed[hand] = false;
		gTrackedWeaponTriggerJustReleased[hand] = false;
		gTrackedThrowablePreviewActive[hand] = false;
		gTrackedAimCacheValid[hand] = false;
		gPhysicalMeleeMotion[hand] = PhysicalMeleeMotion();
		gPhysicalMeleeStrike[hand] = PhysicalMeleeStrike();
		gPhysicalMeleeFreshGrab[hand] = false;
	}
	gAttachedMissionWeaponForced = false;
	gAttachedMissionWeaponSlot = -1;
	gActiveTrackedFireHand = -1;
	gActiveTrackedFireWeaponType = -1;
	gActiveTrackedFireAimValid = false;
	gTrackedScopeReticleTargetValid = false;
	ResetTrackedScopeState();
	ResetTrackedDetonatorInteraction(false);
}

void RestoreHolsterCalibrationSlot(CPlayerPed *player, int slot)
{
	if(!player || slot < 0 || slot >= TOTAL_WEAPON_SLOTS)
		return;
	const CWeapon saved =
		gHolsterCalibrationCheat.originalWeapons[slot];
	const bool currentSlot = player->m_currentWeapon == slot;
	const int selectedSlot = player->m_nSelectedWepSlot;
	if(currentSlot)
		player->RemoveWeaponModel(-1);
	CWeapon &weapon = player->GetWeapon(slot);
	weapon.Shutdown();
	weapon.Initialise(saved.m_eWeaponType, saved.m_nAmmoTotal);
	weapon = saved;
	if(currentSlot && saved.m_eWeaponType != WEAPONTYPE_UNARMED){
		const int model = CWeaponInfo::GetWeaponInfo(
			saved.m_eWeaponType)->m_nModelId;
		if(model >= 0)
			player->AddWeaponModel(model);
	}
	player->m_nSelectedWepSlot = selectedSlot;
}

void RestoreHolsterCalibrationLoadout(CPlayerPed *player)
{
	if(!player)
		return;
	player->RemoveWeaponModel(-1);
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		CWeapon &weapon = player->GetWeapon(slot);
		const CWeapon saved =
			gHolsterCalibrationCheat.originalWeapons[slot];
		weapon.Shutdown();
		weapon.Initialise(saved.m_eWeaponType, saved.m_nAmmoTotal);
		weapon = saved;
	}
	// These came from the same live player immediately before the preview, so
	// restore the byte values verbatim rather than normalising weapon-selection
	// state which may be between two native selection frames.
	player->m_currentWeapon =
		(uint8)gHolsterCalibrationCheat.originalCurrentSlot;
	player->m_nSelectedWepSlot =
		(int8)gHolsterCalibrationCheat.originalSelectedSlot;
	const CWeapon &current = player->GetWeapon(player->m_currentWeapon);
	if(current.m_eWeaponType != WEAPONTYPE_UNARMED){
		const int model = CWeaponInfo::GetWeaponInfo(
			current.m_eWeaponType)->m_nModelId;
		if(model >= 0)
			player->AddWeaponModel(model);
	}
}

bool BeginHolsterCalibrationLoadout(CPlayerPed *player)
{
	if(gHolsterCalibrationCheat.active &&
	   !ValidateHolsterCalibrationLifecycle())
		return false;
	if(!CanStartHolsterCalibration(player)){
		debug("[OpenXR] Holster calibration unavailable: requires alive "
			"on-foot player outside missions\n");
		return false;
	}
	if(gHolsterCalibrationCheat.active)
		return IsHolsterCalibrationOwnerValid(player);
	if(!CPools::GetPedPool())
		return false;
	const int ownerPoolHandle = CPools::GetPedPool()->GetIndex(player);
	if(ownerPoolHandle < 0 ||
	   CPools::GetPedPool()->GetAt(ownerPoolHandle) != player)
		return false;
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
		gHolsterCalibrationCheat.originalWeapons[slot] =
			player->GetWeapon(slot);
	HoldHolsterCalibrationSnapshotModelRefs();
	gHolsterCalibrationCheat.originalCurrentSlot = player->m_currentWeapon;
	gHolsterCalibrationCheat.originalSelectedSlot =
		player->m_nSelectedWepSlot;
	gHolsterCalibrationCheat.owner = player;
	gHolsterCalibrationCheat.ownerPoolHandle = ownerPoolHandle;
	gHolsterCalibrationCheat.previewWeaponType = -1;
	gHolsterCalibrationCheat.previewSlot = -1;
	gHolsterCalibrationCheat.active = true;
	debug("[OpenXR] Holster calibration loadout snapshot captured\n");
	return true;
}

uint32 GetHolsterCalibrationAmmo(int weaponType)
{
	if(weaponType >= WEAPONTYPE_SCREWDRIVER &&
	   weaponType <= WEAPONTYPE_CHAINSAW)
		return 1;
	if(weaponType >= WEAPONTYPE_GRENADE &&
	   weaponType <= WEAPONTYPE_MOLOTOV)
		return 10;
	if(weaponType == WEAPONTYPE_CAMERA)
		return 1;
	if(weaponType == WEAPONTYPE_ROCKETLAUNCHER)
		return 20;
	if(weaponType == WEAPONTYPE_FLAMETHROWER ||
	   weaponType == WEAPONTYPE_MINIGUN)
		return 500;
	return 250;
}

bool RequestHolsterCalibrationWeaponModels(int weaponType)
{
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(
		(eWeaponType)weaponType);
	if(!info)
		return false;
	if(info->m_nModelId >= 0)
		CStreaming::RequestModel(info->m_nModelId,
			STREAMFLAGS_DEPENDENCY);
	if(info->m_nModel2Id >= 0)
		CStreaming::RequestModel(info->m_nModel2Id,
			STREAMFLAGS_DEPENDENCY);
	CStreaming::LoadAllRequestedModels(false);
	return (info->m_nModelId < 0 ||
		CStreaming::HasModelLoaded(info->m_nModelId)) &&
		(info->m_nModel2Id < 0 ||
		 CStreaming::HasModelLoaded(info->m_nModel2Id));
}

bool PutNextHolsterCalibrationWeapon()
{
	CPlayerPed *player = FindPlayerPed();
	if(!BeginHolsterCalibrationLoadout(player) ||
	   gHolsterCalibrationCheat.owner != player)
		return false;
	if(!CanStartHolsterCalibration(player))
		return false;
	ClearHolsterCalibrationWeaponTransients();
	const int oldPreviewSlot = gHolsterCalibrationCheat.previewSlot;
	gHolsterCalibrationCheat.previewWeaponType = -1;
	gHolsterCalibrationCheat.previewSlot = -1;
	if(oldPreviewSlot >= 0)
		RestoreHolsterCalibrationSlot(player, oldPreviewSlot);

	const int index = FindNextHolsterCalibrationWeaponIndex(
		gHolsterCalibrationCheat.nextWeaponIndex);
	if(index < 0)
		return false;
	const int weaponType = gHolsterCalibrationWeaponTypes[index];
	if(IsHolsterCalibrationRemoteGrenadeBlocked(weaponType)){
		debug("[OpenXR] Holster calibration blocked remote grenade: "
			"active C4 projectile exists\n");
		return false;
	}
	if(!RequestHolsterCalibrationWeaponModels(weaponType)){
		debug("[OpenXR] Holster calibration could not load weapon %s\n",
			GetVrWeaponName(weaponType));
		return false;
	}
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(
		(eWeaponType)weaponType);
	const uint32 ammo = GetHolsterCalibrationAmmo(weaponType);
	const int slot = player->GiveWeapon((eWeaponType)weaponType, ammo, true);
	if(slot <= WEAPONSLOT_UNARMED || slot >= TOTAL_WEAPON_SLOTS)
		return false;
	CWeapon &preview = player->GetWeapon(slot);
	preview.m_nAmmoTotal = ammo;
	preview.m_nAmmoInClip = Min((int32)ammo,
		Max(1, info->m_nAmountofAmmunition));
	preview.m_eWeaponState = WEAPONSTATE_READY;
	preview.m_nTimer = 0;
	preview.m_bAddRotOffset = false;
	gHolsterCalibrationCheat.previewWeaponType = weaponType;
	gHolsterCalibrationCheat.previewSlot = slot;
	gHolsterCalibrationCheat.nextWeaponIndex =
		(index+1) % ARRAY_SIZE(gHolsterCalibrationWeaponTypes);
	gWeaponHolsterMask |= 1u << slot;
	debug("[OpenXR] Holster calibration preview: %s in %s (slot %d)\n",
		GetVrWeaponName(weaponType),
		gHolsterPointNames[gHolsterCalibrationCheat.selectedPoint], slot);
	return true;
}

bool FinishHolsterCalibrationLoadout()
{
	if(!gHolsterCalibrationCheat.active)
		return false;
	CPlayerPed *player = FindPlayerPed();
	if(!IsHolsterCalibrationOwnerValid(player)){
		DiscardHolsterCalibrationLoadout("owner changed or stale");
		return false;
	}
	ClearHolsterCalibrationWeaponTransients();
	RestoreHolsterCalibrationLoadout(player);
	// RestoreHolsterCalibrationLoadout has created the live CWeapon references;
	// only now may the independent snapshot ownership be released.
	ReleaseHolsterCalibrationSnapshotModelRefs();
	gHolsterCalibrationCheat.active = false;
	gHolsterCalibrationCheat.owner = nil;
	gHolsterCalibrationCheat.ownerPoolHandle = -1;
	gHolsterCalibrationCheat.previewWeaponType = -1;
	gHolsterCalibrationCheat.previewSlot = -1;
	gHolsterCalibrationCheat.nextWeaponIndex = 0;
	gWeaponHolsterMask = 0;
	debug("[OpenXR] Holster calibration finished: original loadout "
		"restored exactly\n");
	return true;
}

void DiscardHolsterCalibrationLoadout(const char *reason)
{
	if(!gHolsterCalibrationCheat.active)
		return;
	// Never dereference owner here: this path specifically handles a replaced
	// or destroyed player entity. Global tracked-hand state is safe to clear.
	ClearHolsterCalibrationWeaponTransients();
	ReleaseHolsterCalibrationSnapshotModelRefs();
	gHolsterCalibrationCheat.active = false;
	gHolsterCalibrationCheat.owner = nil;
	gHolsterCalibrationCheat.ownerPoolHandle = -1;
	gHolsterCalibrationCheat.previewWeaponType = -1;
	gHolsterCalibrationCheat.previewSlot = -1;
	gHolsterCalibrationCheat.nextWeaponIndex = 0;
	gWeaponHolsterMask = 0;
	debug("[OpenXR] Holster calibration snapshot discarded: %s\n",
		reason ? reason : "lifecycle ended");
}

bool ValidateHolsterCalibrationLifecycle()
{
	if(!gHolsterCalibrationCheat.active)
		return true;
	CPlayerPed *player = FindPlayerPed();
	if(!IsHolsterCalibrationOwnerValid(player)){
		DiscardHolsterCalibrationLoadout("player owner changed or vanished");
		return false;
	}
	if(!CanStartHolsterCalibration(player)){
		debug("[OpenXR] Holster calibration lifecycle became invalid; "
			"restoring original loadout\n");
		FinishHolsterCalibrationLoadout();
		return false;
	}
	return true;
}

void CycleHolsterCalibrationPoint(int direction)
{
	if(direction == 0)
		return;
	gHolsterCalibrationCheat.selectedPoint =
		(gHolsterCalibrationCheat.selectedPoint+HOLSTER_POINT_COUNT+
		 (direction > 0 ? 1 : -1)) % HOLSTER_POINT_COUNT;
	debug("[OpenXR] Holster calibration target: %s\n",
		gHolsterPointNames[gHolsterCalibrationCheat.selectedPoint]);
}

int GetOpenXrCheatMenuCount()
{
	const int cheatCount = VR_CHEAT_LOCAL_ITEM_COUNT+GetVrCheatCount();
#if defined(DEBUG) && !defined(FINAL)
	// The mission browser is deliberately the final item in the flat cheat
	// list, as requested for quick test-build access.
	return cheatCount+1;
#else
	return cheatCount;
#endif
}

#if defined(DEBUG) && !defined(FINAL)
int GetOpenXrMissionMenuIndex()
{
	return VR_CHEAT_LOCAL_ITEM_COUNT+GetVrCheatCount();
}
#endif

const char *GetOpenXrCheatMenuName(int index)
{
#if defined(DEBUG) && !defined(FINAL)
	static char label[112];
	if(index == VR_CHEAT_CAL_HOLSTER){
		sprintf(label, "CAL HOLSTER  < %s >",
			gHolsterPointNames[gHolsterCalibrationCheat.selectedPoint]);
		return label;
	}
	if(index == VR_CHEAT_CAL_NEXT_WEAPON){
		const int weaponType = GetNextHolsterCalibrationWeaponType();
		sprintf(label, "PUT NEXT WEAPON  < %s >",
			weaponType >= 0 ? GetVrWeaponName(weaponType) : "UNAVAILABLE");
		return label;
	}
	if(index == VR_CHEAT_CAL_FINISH)
		return "FINISH TEST / RESTORE LOADOUT";
	if(index == GetOpenXrMissionMenuIndex())
		return "MISSIONS  < OPEN >";
#endif
	return GetVrCheatName(index-VR_CHEAT_LOCAL_ITEM_COUNT);
}

void BackupVrSettings(const char *reason)
{
	static uint32 backupSequence = 0;
	char backupDirectory[MAX_PATH] = {};
	strncpy(backupDirectory, GetVrSettingsPath(),
		ARRAY_SIZE(backupDirectory)-1);
	char *separator = strrchr(backupDirectory, '\\');
	if(!separator)
		separator = strrchr(backupDirectory, '/');
	if(separator)
		strcpy(separator+1, "vr_settings_backups");
	else
		strcpy(backupDirectory, ".\\vr_settings_backups");
	CreateDirectoryA(backupDirectory, nil);
	SYSTEMTIME time;
	GetLocalTime(&time);
	char backupPath[MAX_PATH];
	sprintf(backupPath,
		"%s\\vr_settings_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u_%s.ini",
		backupDirectory,
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
		time.wSecond, time.wMilliseconds, (unsigned long)GetCurrentProcessId(),
		backupSequence++, reason ? reason : "snapshot");
	// CREATE_NEW semantics: an existing snapshot is never overwritten.
	CopyFileA(GetVrSettingsPath(), backupPath, TRUE);
}

bool GetWeaponCalibrationSectionForSet(int weaponType,
	ModelSets::eModelSet modelSet, char *section)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return false;
	sprintf(section, modelSet == ModelSets::MODEL_SET_MODERN ?
		"VRWeaponModern_%02d_%s" : "VRWeapon_%02d_%s",
		weaponType, GetVrWeaponName(weaponType));
	return true;
}

bool GetWeaponCalibrationSection(int weaponType, char *section)
{
	return GetWeaponCalibrationSectionForSet(weaponType,
		GetActiveWeaponModelSet(), section);
}

struct WeaponLaserOverride
{
	bool loaded = false;
	int value = -1;
};
WeaponLaserOverride gWeaponLaserOverrides[2][WEAPONTYPE_TOTALWEAPONS];

int GetWeaponLaserOverride(int weaponType)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return -1;
	const int modelSet = GetActiveWeaponModelSet() == ModelSets::MODEL_SET_MODERN ? 1 : 0;
	WeaponLaserOverride &entry = gWeaponLaserOverrides[modelSet][weaponType];
	if(!entry.loaded){
		char section[128];
		GetWeaponCalibrationSection(weaponType, section);
		const int stored = (int)(int32)GetPrivateProfileIntA(section,
			"Laser", -1, GetVrSettingsPath());
		entry.value = stored < 0 ? -1 : (stored != 0 ? 1 : 0);
		entry.loaded = true;
	}
	return entry.value;
}

void CycleWeaponLaserOverride(int weaponType, int direction)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return;
	const int value = (GetWeaponLaserOverride(weaponType)+1+3+
		(direction < 0 ? -1 : 1))%3-1;
	char section[128], text[16];
	GetWeaponCalibrationSection(weaponType, section);
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section, "Laser", text, GetVrSettingsPath());
	const int modelSet = GetActiveWeaponModelSet() == ModelSets::MODEL_SET_MODERN ? 1 : 0;
	gWeaponLaserOverrides[modelSet][weaponType].value = value;
}

int GetCalibrationWeaponType()
{
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	// The virtual remote uses the stock detonator model but never occupies an
	// inventory slot.  Give it its own per-hand WEAPONTYPE_DETONATOR profile so
	// adjusting the controller cannot disturb the already calibrated C4 charge.
	if(IsTrackedDetonatorHandReservedInternal(hand))
		return WEAPONTYPE_DETONATOR;
	int weaponType = gHeldWeaponSlot[hand] >= 0 ?
		GetVrWeaponTypeForSlot(gHeldWeaponSlot[hand]) : -1;
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		weaponType = GetVrCurrentWeaponType();
	return weaponType;
}

bool GetCurrentWeaponCalibrationSection(char *section)
{
	return GetWeaponCalibrationSection(GetCalibrationWeaponType(), section);
}

const char *GetWeaponCalibrationKey(int hand, const char *name, char *key)
{
	// Existing unprefixed values are the user's carefully calibrated RIGHT-hand
	// profiles.  Never rename or rewrite them during migration.  LEFT lives next
	// to them under an explicit prefix and is seeded from the normalized right
	// profile on first use.
	if(hand == 0){
		sprintf(key, "Left%s", name);
		return key;
	}
	return name;
}

void WriteWeaponCalibrationValue(const char *section, int hand,
	const char *name, int value)
{
	char textValue[32];
	char key[64];
	sprintf(textValue, "%d", value);
	WritePrivateProfileStringA(section, GetWeaponCalibrationKey(hand, name, key),
		textValue, GetVrSettingsPath());
}

int ReadWeaponCalibrationValue(const char *section, int hand,
	const char *name, int fallback)
{
	// GetPrivateProfileInt returns UINT even for negative text. Cast the result
	// back to signed before clamping; mixing UINT with a negative clamp bound is
	// what previously converted every loaded value to its positive maximum.
	char key[64];
	return (int)(int32)GetPrivateProfileIntA(section,
		GetWeaponCalibrationKey(hand, name, key), fallback,
		GetVrSettingsPath());
}

enum {
	WEAPON_AIM_ALIGNMENT_VERSION = 1,
	WEAPON_AIM_ALIGNMENT_SCALE = 1000000
};

bool LoadWeaponAimRelativeForSet(int weaponType,
	ModelSets::eModelSet modelSet, CMatrix *relative)
{
	if(!relative || weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS ||
	   modelSet < ModelSets::MODEL_SET_CLASSIC ||
	   modelSet >= ModelSets::MODEL_SET_COUNT)
		return false;
	const int set = (int)modelSet;
	if(!gWeaponAimRelativeLoaded[set][weaponType]){
		gWeaponAimRelativeLoaded[set][weaponType] = true;
		gWeaponAimRelativeEnabled[set][weaponType] = false;
		char section[96];
		if(!GetWeaponCalibrationSectionForSet(weaponType, modelSet, section))
			return false;
		char alignedText[16];
		const bool hasUserAligned = GetPrivateProfileStringA(section,
			"AimAligned", "", alignedText, sizeof(alignedText),
			GetVrSettingsPath()) != 0;
		const BuiltInWeaponAimRelativeDefaults *builtIn = nil;
		int scale = 0;
		if(hasUserAligned){
			// An explicit user OFF (or an incomplete explicit profile) always wins
			// over the release baseline.
			if(atoi(alignedText) == 0 ||
			   GetPrivateProfileIntA(section, "AimAlignedTransformVersion", 0,
				GetVrSettingsPath()) != WEAPON_AIM_ALIGNMENT_VERSION)
				return false;
			scale = GetPrivateProfileIntA(section, "AimAlignedTransformScale", 0,
				GetVrSettingsPath());
		}else{
			builtIn = FindBuiltInWeaponAimRelativeDefaults(weaponType, modelSet);
			if(!builtIn)
				return false;
			scale = WEAPON_AIM_ALIGNMENT_SCALE;
		}
		if(scale <= 0)
			return false;
		CMatrix loaded;
		loaded.SetUnity();
#define READ_AIM_RELATIVE(component, index, key) \
		loaded.component = (float)(builtIn ? builtIn->transform[index] : \
			ReadWeaponCalibrationValue(section, 1, key, 0))/(float)scale
		READ_AIM_RELATIVE(rx, 0, "AimAlignedRightX");
		READ_AIM_RELATIVE(ry, 1, "AimAlignedRightY");
		READ_AIM_RELATIVE(rz, 2, "AimAlignedRightZ");
		READ_AIM_RELATIVE(fx, 3, "AimAlignedForwardX");
		READ_AIM_RELATIVE(fy, 4, "AimAlignedForwardY");
		READ_AIM_RELATIVE(fz, 5, "AimAlignedForwardZ");
		READ_AIM_RELATIVE(ux, 6, "AimAlignedUpX");
		READ_AIM_RELATIVE(uy, 7, "AimAlignedUpY");
		READ_AIM_RELATIVE(uz, 8, "AimAlignedUpZ");
		READ_AIM_RELATIVE(px, 9, "AimAlignedPositionX");
		READ_AIM_RELATIVE(py, 10, "AimAlignedPositionY");
		READ_AIM_RELATIVE(pz, 11, "AimAlignedPositionZ");
#undef READ_AIM_RELATIVE
		const float determinant = DotProduct(loaded.GetRight(),
			CrossProduct(loaded.GetForward(), loaded.GetUp()));
		if(!_finite(determinant) || fabsf(determinant) < 0.85f ||
		   loaded.GetRight().MagnitudeSqr() < 0.75f ||
		   loaded.GetForward().MagnitudeSqr() < 0.75f ||
		   loaded.GetUp().MagnitudeSqr() < 0.75f ||
		   !_finite(loaded.px) || !_finite(loaded.py) || !_finite(loaded.pz) ||
		   fabsf(loaded.px) > 2.0f || fabsf(loaded.py) > 2.0f ||
		   fabsf(loaded.pz) > 2.0f)
			return false;
		// Preserve the captured determinant instead of assuming a handedness for
		// this persisted relationship. Both the tracked model and OpenXR aim bases
		// are reflected today, but the relative transform must remain version-safe.
		const float handedness = determinant < 0.0f ? -1.0f : 1.0f;
		loaded.GetRight().Normalise();
		loaded.GetForward() -= loaded.GetRight()*DotProduct(
			loaded.GetForward(), loaded.GetRight());
		loaded.GetForward().Normalise();
		loaded.GetUp() = CrossProduct(loaded.GetRight(),
			loaded.GetForward())*handedness;
		gWeaponAimRelative[set][weaponType] = loaded;
		gWeaponAimRelativeEnabled[set][weaponType] = true;
	}
	if(!gWeaponAimRelativeEnabled[set][weaponType])
		return false;
	*relative = gWeaponAimRelative[set][weaponType];
	return true;
}

bool LoadActiveWeaponAimRelative(int weaponType, CMatrix *relative)
{
	return LoadWeaponAimRelativeForSet(weaponType, GetActiveWeaponModelSet(),
		relative);
}

void InvalidateWeaponAimRelative(int weaponType,
	ModelSets::eModelSet modelSet)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS ||
	   modelSet < ModelSets::MODEL_SET_CLASSIC ||
	   modelSet >= ModelSets::MODEL_SET_COUNT)
		return;
	gWeaponAimRelativeLoaded[(int)modelSet][weaponType] = false;
	gWeaponAimRelativeEnabled[(int)modelSet][weaponType] = false;
}

bool IsWeaponCalibrationConfigured(const char *section, int hand)
{
	char key[64];
	return GetPrivateProfileIntA(section,
		GetWeaponCalibrationKey(hand, "Configured", key), 0,
		GetVrSettingsPath()) != 0;
}

bool HasConfiguredClassicWeaponCalibration(int weaponType)
{
	char section[96];
	return GetWeaponCalibrationSectionForSet(weaponType,
		ModelSets::MODEL_SET_CLASSIC, section) &&
		(IsWeaponCalibrationConfigured(section, 0) ||
		 IsWeaponCalibrationConfigured(section, 1));
}

void ReadConfiguredWeaponAim(const char *section, int weaponType, int hand,
	ModelSets::eModelSet modelSet, WeaponCalibration &calibration)
{
	const int storedScale = ReadWeaponCalibrationValue(section, hand,
		"ValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	WeaponCalibration defaults;
	GetDefaultWeaponCalibrationForSet(weaponType, hand, modelSet, defaults);
	calibration.aimOffsetX = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"AimOffsetX", defaults.aimOffsetX/conversion)*conversion, -100), 100);
	calibration.aimOffsetY = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"AimOffsetY", defaults.aimOffsetY/conversion)*conversion, -100), 100);
	calibration.aimOffsetZ = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"AimOffsetZ", defaults.aimOffsetZ/conversion)*conversion, -100), 100);
	calibration.aimRotationX = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"AimRotationX", defaults.aimRotationX/conversion)*conversion,
		-360), 360);
	calibration.aimRotationY = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"AimRotationY", defaults.aimRotationY/conversion)*conversion,
		-360), 360);
	calibration.aimRotationZ = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"AimRotationZ", defaults.aimRotationZ/conversion)*conversion,
		-360), 360);
}

void LoadEffectiveClassicWeaponCalibration(int weaponType, int hand,
	WeaponCalibration &calibration)
{
	char section[96];
	GetWeaponCalibrationSectionForSet(weaponType,
		ModelSets::MODEL_SET_CLASSIC, section);
	int readHand = hand;
	if(!IsWeaponCalibrationConfigured(section, hand)){
		if(hand == 0 && IsWeaponCalibrationConfigured(section, 1))
			readHand = 1;
		else if(hand == 1 && IsWeaponCalibrationConfigured(section, 0)){
			// LEFT-only files predate the canonical profile. Keep their effective
			// ray by reflecting it into an in-memory RIGHT default; do not rewrite
			// the user's INI merely because it was loaded.
			WeaponCalibration legacyLeft;
			GetDefaultWeaponCalibrationForSet(weaponType, 0,
				ModelSets::MODEL_SET_CLASSIC, legacyLeft);
			ReadConfiguredWeaponAim(section, weaponType, 0,
				ModelSets::MODEL_SET_CLASSIC, legacyLeft);
			GetDefaultWeaponCalibrationForSet(weaponType, 1,
				ModelSets::MODEL_SET_CLASSIC, calibration);
			ApplyMirroredWeaponAim(legacyLeft, calibration);
			calibration.valid = true;
			return;
		}
		else{
			GetDefaultWeaponCalibrationForSet(weaponType, hand,
				ModelSets::MODEL_SET_CLASSIC, calibration);
			calibration.valid = true;
			return;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"ValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	WeaponCalibration defaults;
	GetDefaultWeaponCalibrationForSet(weaponType, readHand,
		ModelSets::MODEL_SET_CLASSIC, defaults);
	calibration.offsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"OffsetX", defaults.offsetX/conversion)*conversion, -100), 100);
	calibration.offsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"OffsetY", defaults.offsetY/conversion)*conversion, -100), 100);
	calibration.offsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"OffsetZ", defaults.offsetZ/conversion)*conversion, -100), 100);
	calibration.aimOffsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetX", defaults.aimOffsetX/conversion)*conversion, -100), 100);
	calibration.aimOffsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetY", defaults.aimOffsetY/conversion)*conversion, -100), 100);
	calibration.aimOffsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetZ", defaults.aimOffsetZ/conversion)*conversion, -100), 100);
	calibration.aimRotationX = Min(Max(ReadWeaponCalibrationValue(section,
		readHand, "AimRotationX", defaults.aimRotationX/conversion)*conversion,
		-360), 360);
	calibration.aimRotationY = Min(Max(ReadWeaponCalibrationValue(section,
		readHand, "AimRotationY", defaults.aimRotationY/conversion)*conversion,
		-360), 360);
	calibration.aimRotationZ = Min(Max(ReadWeaponCalibrationValue(section,
		readHand, "AimRotationZ", defaults.aimRotationZ/conversion)*conversion,
		-360), 360);
	calibration.rotationX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"RotationX", defaults.rotationX/conversion)*conversion, -360), 360);
	calibration.rotationY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"RotationY", defaults.rotationY/conversion)*conversion, -360), 360);
	calibration.rotationZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"RotationZ", defaults.rotationZ/conversion)*conversion, -360), 360);
	const bool knownBrokenClampProfile = calibration.offsetX == 100 &&
		calibration.offsetY == 100 && calibration.offsetZ == 100 &&
		calibration.aimOffsetY == 100 && calibration.rotationX == 360 &&
		calibration.rotationY == 360 && calibration.rotationZ == 360;
	if(knownBrokenClampProfile)
		GetDefaultWeaponCalibrationForSet(weaponType, hand,
			ModelSets::MODEL_SET_CLASSIC, calibration);
	// A configured RIGHT profile is the canonical aim calibration. Preserve an
	// old LEFT-only profile for compatibility, but once RIGHT exists its laser
	// is always mirrored automatically while LEFT model placement stays intact.
	if(hand == 0 && IsWeaponCalibrationConfigured(section, 1)){
		WeaponCalibration right;
		LoadEffectiveClassicWeaponCalibration(weaponType, 1, right);
		ApplyMirroredWeaponAim(right, calibration);
	}
	calibration.valid = true;
}

void CaptureCurrentWeaponCalibration(WeaponCalibration &calibration);
const WeaponCalibration *GetWeaponCalibration(int hand, int weaponType);

void ApplyActiveRightWeaponAimToCurrent(int weaponType)
{
	const WeaponCalibration *right = GetWeaponCalibration(1, weaponType);
	if(!right)
		return;
	WeaponCalibration current;
	CaptureCurrentWeaponCalibration(current);
	CopyWeaponAim(*right, current);
	ApplyCurrentWeaponCalibration(current);
}

void WriteCompleteWeaponCalibration(const char *section, int hand,
	const WeaponCalibration &calibration)
{
	WriteWeaponCalibrationValue(section, hand, "Configured", 1);
	WriteWeaponCalibrationValue(section, hand, "ValueScale",
		WEAPON_CALIBRATION_VALUE_SCALE);
	WriteWeaponCalibrationValue(section, hand, "OffsetX", calibration.offsetX);
	WriteWeaponCalibrationValue(section, hand, "OffsetY", calibration.offsetY);
	WriteWeaponCalibrationValue(section, hand, "OffsetZ", calibration.offsetZ);
	WriteWeaponCalibrationValue(section, hand, "AimOffsetX",
		calibration.aimOffsetX);
	WriteWeaponCalibrationValue(section, hand, "AimOffsetY",
		calibration.aimOffsetY);
	WriteWeaponCalibrationValue(section, hand, "AimOffsetZ",
		calibration.aimOffsetZ);
	WriteWeaponCalibrationValue(section, hand, "AimRotationX",
		calibration.aimRotationX);
	WriteWeaponCalibrationValue(section, hand, "AimRotationY",
		calibration.aimRotationY);
	WriteWeaponCalibrationValue(section, hand, "AimRotationZ",
		calibration.aimRotationZ);
	WriteWeaponCalibrationValue(section, hand, "RotationX",
		calibration.rotationX);
	WriteWeaponCalibrationValue(section, hand, "RotationY",
		calibration.rotationY);
	WriteWeaponCalibrationValue(section, hand, "RotationZ",
		calibration.rotationZ);
}

void SaveCompleteWeaponCalibration(const char *section, int hand)
{
	WeaponCalibration calibration;
	CaptureCurrentWeaponCalibration(calibration);
	if(hand == 0){
		WeaponCalibration canonical = calibration;
		ApplyMirroredWeaponAim(canonical, calibration);
	}
	WriteCompleteWeaponCalibration(section, hand, calibration);
}

void SyncCurrentWeaponCalibration()
{
	const int weaponType = GetCalibrationWeaponType();
	const int hand = gCalibrationEditHand == 0 ? 0 : 1;
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS ||
	   (weaponType == gActiveWeaponCalibration &&
	    hand == gActiveWeaponCalibrationHand))
		return;
	if(gActiveWeaponCalibration >= 0){
		char reason[48];
		sprintf(reason, "weapon_%02d_%c_done", gActiveWeaponCalibration,
			gActiveWeaponCalibrationHand == 0 ? 'L' : 'R');
		BackupVrSettings(reason);
	}
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	// Missing or incomplete transform keys deliberately mean OFF, so every
	// existing profile keeps its old independent model/laser behaviour until the
	// player explicitly captures a complete relative binding.
	CMatrix aimRelative;
	gWeaponAimAligned = LoadActiveWeaponAimRelative(weaponType, &aimRelative);
	gActiveWeaponCalibration = weaponType;
	gActiveWeaponCalibrationHand = hand;
	int readHand = hand;
	if(!IsWeaponCalibrationConfigured(section, hand)){
		if(IsModernWeaponModelSetActive()){
			// Existing Classic user data remains the read-only inheritance source.
			// With no user profile at all, use the captured Modern release baseline;
			// either value is copied into Modern only when the player edits it.
			WeaponCalibration inherited;
			if(!HasConfiguredClassicWeaponCalibration(weaponType) &&
			   FindBuiltInWeaponSetDefaults(weaponType,
				ModelSets::MODEL_SET_MODERN))
				GetDefaultWeaponCalibrationForSet(weaponType, hand,
					ModelSets::MODEL_SET_MODERN, inherited);
			else
				LoadEffectiveClassicWeaponCalibration(weaponType, hand,
					inherited);
			if(hand == 1 && IsWeaponCalibrationConfigured(section, 0)){
				WeaponCalibration legacyLeft;
				GetDefaultWeaponCalibrationForSet(weaponType, 0,
					GetActiveWeaponModelSet(), legacyLeft);
				ReadConfiguredWeaponAim(section, weaponType, 0,
					GetActiveWeaponModelSet(), legacyLeft);
				ApplyMirroredWeaponAim(legacyLeft, inherited);
			}
			ApplyCurrentWeaponCalibration(inherited);
			gWeaponCalibration[hand][weaponType] = inherited;
			if(hand == 0){
				ApplyActiveRightWeaponAimToCurrent(weaponType);
				WeaponCalibration current;
				CaptureCurrentWeaponCalibration(current);
				gWeaponCalibration[hand][weaponType] = current;
				ApplyMirroredWeaponAim(current,
					gWeaponCalibration[hand][weaponType]);
			}
			return;
		}else{
			// Use the same effective Classic resolver as the ordinary gameplay cache.
			// The active owner hand also drives the calibration globals outside the
			// menu; resolving it from a different built-in profile made an unconfigured
			// LEFT RPG jump 13 cm back and put its primary and support grips together.
			// Keep the profile read-only until the player actually edits a value.
			WeaponCalibration defaults;
			LoadEffectiveClassicWeaponCalibration(weaponType, hand, defaults);
			ApplyCurrentWeaponCalibration(defaults);
			if(hand == 0)
				ApplyActiveRightWeaponAimToCurrent(weaponType);
			gWeaponCalibration[hand][weaponType] = defaults;
			if(hand == 0){
				WeaponCalibration current;
				CaptureCurrentWeaponCalibration(current);
				gWeaponCalibration[hand][weaponType] = current;
				ApplyMirroredWeaponAim(current,
					gWeaponCalibration[hand][weaponType]);
			}
			return;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"ValueScale", 1);
	const int fallbackDivisor = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	WeaponCalibration defaults;
	GetDefaultWeaponCalibration(weaponType, readHand, defaults);
	const int rawOffsetX = ReadWeaponCalibrationValue(section, readHand,
		"OffsetX", defaults.offsetX/fallbackDivisor);
	const int rawOffsetY = ReadWeaponCalibrationValue(section, readHand,
		"OffsetY", defaults.offsetY/fallbackDivisor);
	const int rawOffsetZ = ReadWeaponCalibrationValue(section, readHand,
		"OffsetZ", defaults.offsetZ/fallbackDivisor);
	const int rawAimOffsetX = ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetX", defaults.aimOffsetX/fallbackDivisor);
	const int rawAimOffsetY = ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetY", defaults.aimOffsetY/fallbackDivisor);
	const int rawAimOffsetZ = ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetZ", defaults.aimOffsetZ/fallbackDivisor);
	const int rawAimRotationX = ReadWeaponCalibrationValue(section, readHand,
		"AimRotationX", defaults.aimRotationX/fallbackDivisor);
	const int rawAimRotationY = ReadWeaponCalibrationValue(section, readHand,
		"AimRotationY", defaults.aimRotationY/fallbackDivisor);
	const int rawAimRotationZ = ReadWeaponCalibrationValue(section, readHand,
		"AimRotationZ", defaults.aimRotationZ/fallbackDivisor);
	const int rawRotationX = ReadWeaponCalibrationValue(section, readHand,
		"RotationX", defaults.rotationX/fallbackDivisor);
	const int rawRotationY = ReadWeaponCalibrationValue(section, readHand,
		"RotationY", defaults.rotationY/fallbackDivisor);
	const int rawRotationZ = ReadWeaponCalibrationValue(section, readHand,
		"RotationZ", defaults.rotationZ/fallbackDivisor);
	const bool knownBrokenClampProfile = rawOffsetX == 50 && rawOffsetY == 50 &&
		rawOffsetZ == 50 && rawAimOffsetY == 50 && rawRotationX == 180 &&
		rawRotationY == 180 && rawRotationZ == 180;
	if(knownBrokenClampProfile){
		// Preserve the complete damaged file before replacing only this known
		// corruption signature with the deterministic release calibration.
		BackupVrSettings("broken_clamp_profile");
		ApplyCurrentWeaponCalibration(defaults);
		if(hand == 0)
			ApplyActiveRightWeaponAimToCurrent(weaponType);
		SaveCompleteWeaponCalibration(section, hand);
		gWeaponCalibration[hand][weaponType].valid = false;
		debug("[OpenXR] Recovered corrupt weapon calibration %s from last safe values\n", section);
		return;
	}
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	gWeaponOffsetXCm = Min(Max(rawOffsetX*conversion, -100), 100);
	gWeaponOffsetYCm = Min(Max(rawOffsetY*conversion, -100), 100);
	gWeaponOffsetZCm = Min(Max(rawOffsetZ*conversion, -100), 100);
	gWeaponAimOffsetXCm = Min(Max(rawAimOffsetX*conversion, -100), 100);
	gWeaponAimOffsetYCm = Min(Max(rawAimOffsetY*conversion, -100), 100);
	gWeaponAimOffsetZCm = Min(Max(rawAimOffsetZ*conversion, -100), 100);
	gWeaponAimRotationXDeg = Min(Max(rawAimRotationX*conversion, -360), 360);
	gWeaponAimRotationYDeg = Min(Max(rawAimRotationY*conversion, -360), 360);
	gWeaponAimRotationZDeg = Min(Max(rawAimRotationZ*conversion, -360), 360);
	gWeaponRotationXDeg = Min(Max(rawRotationX*conversion, -360), 360);
	gWeaponRotationYDeg = Min(Max(rawRotationY*conversion, -360), 360);
	gWeaponRotationZDeg = Min(Max(rawRotationZ*conversion, -360), 360);
	if(hand == 0)
		ApplyActiveRightWeaponAimToCurrent(weaponType);
	gWeaponCalibration[hand][weaponType].valid = false;
	if(storedScale != WEAPON_CALIBRATION_VALUE_SCALE)
		SaveCompleteWeaponCalibration(section, hand);
}

void CaptureCurrentWeaponCalibration(WeaponCalibration &calibration)
{
	calibration.offsetX = gWeaponOffsetXCm;
	calibration.offsetY = gWeaponOffsetYCm;
	calibration.offsetZ = gWeaponOffsetZCm;
	calibration.aimOffsetX = gWeaponAimOffsetXCm;
	calibration.aimOffsetY = gWeaponAimOffsetYCm;
	calibration.aimOffsetZ = gWeaponAimOffsetZCm;
	calibration.aimRotationX = gWeaponAimRotationXDeg;
	calibration.aimRotationY = gWeaponAimRotationYDeg;
	calibration.aimRotationZ = gWeaponAimRotationZDeg;
	calibration.rotationX = gWeaponRotationXDeg;
	calibration.rotationY = gWeaponRotationYDeg;
	calibration.rotationZ = gWeaponRotationZDeg;
	calibration.valid = true;
}

const WeaponCalibration *GetWeaponCalibration(int hand, int weaponType)
{
	if(hand < 0 || hand >= EYE_COUNT ||
	   weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return nil;
	if(hand == gCalibrationEditHand && weaponType == GetCalibrationWeaponType()){
		SyncCurrentWeaponCalibration();
		WeaponCalibration current;
		CaptureCurrentWeaponCalibration(current);
		gWeaponCalibration[hand][weaponType] = current;
		if(hand == 0)
			ApplyMirroredWeaponAim(current,
				gWeaponCalibration[hand][weaponType]);
		return &gWeaponCalibration[hand][weaponType];
	}
	WeaponCalibration &calibration = gWeaponCalibration[hand][weaponType];
	if(calibration.valid)
		return &calibration;
	char section[96];
	if(!GetWeaponCalibrationSection(weaponType, section))
		return nil;
	int readHand = hand;
	if(!IsWeaponCalibrationConfigured(section, hand)){
		if(IsModernWeaponModelSetActive()){
			if(!HasConfiguredClassicWeaponCalibration(weaponType) &&
			   FindBuiltInWeaponSetDefaults(weaponType,
				ModelSets::MODEL_SET_MODERN))
				GetDefaultWeaponCalibrationForSet(weaponType, hand,
					ModelSets::MODEL_SET_MODERN, calibration);
			else
				LoadEffectiveClassicWeaponCalibration(weaponType, hand,
					calibration);
			if(hand == 1 && IsWeaponCalibrationConfigured(section, 0)){
				WeaponCalibration legacyLeft;
				GetDefaultWeaponCalibrationForSet(weaponType, 0,
					GetActiveWeaponModelSet(), legacyLeft);
				ReadConfiguredWeaponAim(section, weaponType, 0,
					GetActiveWeaponModelSet(), legacyLeft);
				ApplyMirroredWeaponAim(legacyLeft, calibration);
			}else if(hand == 0){
				const WeaponCalibration *right =
					GetWeaponCalibration(1, weaponType);
				if(right)
					ApplyMirroredWeaponAim(*right, calibration);
			}
			return &calibration;
		}else if(hand == 0 && IsWeaponCalibrationConfigured(section, 1))
			readHand = 1;
		else{
			GetDefaultWeaponCalibration(weaponType, hand, calibration);
			if(hand == 1 && IsWeaponCalibrationConfigured(section, 0)){
				WeaponCalibration legacyLeft;
				GetDefaultWeaponCalibrationForSet(weaponType, 0,
					GetActiveWeaponModelSet(), legacyLeft);
				ReadConfiguredWeaponAim(section, weaponType, 0,
					GetActiveWeaponModelSet(), legacyLeft);
				ApplyMirroredWeaponAim(legacyLeft, calibration);
			}else if(hand == 0){
				const WeaponCalibration *right =
					GetWeaponCalibration(1, weaponType);
				if(right)
					ApplyMirroredWeaponAim(*right, calibration);
			}
			return &calibration;
		}
	}
	const int storedScale = ReadWeaponCalibrationValue(section, readHand,
		"ValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	const int fallbackDivisor = conversion;
	WeaponCalibration defaults;
	GetDefaultWeaponCalibration(weaponType, readHand, defaults);
	calibration.offsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"OffsetX", defaults.offsetX/fallbackDivisor)*conversion, -100), 100);
	calibration.offsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"OffsetY", defaults.offsetY/fallbackDivisor)*conversion, -100), 100);
	calibration.offsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"OffsetZ", defaults.offsetZ/fallbackDivisor)*conversion, -100), 100);
	calibration.aimOffsetX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetX", defaults.aimOffsetX/fallbackDivisor)*conversion, -100), 100);
	calibration.aimOffsetY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetY", defaults.aimOffsetY/fallbackDivisor)*conversion, -100), 100);
	calibration.aimOffsetZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"AimOffsetZ", defaults.aimOffsetZ/fallbackDivisor)*conversion, -100), 100);
	calibration.aimRotationX = Min(Max(ReadWeaponCalibrationValue(section,
		readHand, "AimRotationX", defaults.aimRotationX/fallbackDivisor)*
		conversion, -360), 360);
	calibration.aimRotationY = Min(Max(ReadWeaponCalibrationValue(section,
		readHand, "AimRotationY", defaults.aimRotationY/fallbackDivisor)*
		conversion, -360), 360);
	calibration.aimRotationZ = Min(Max(ReadWeaponCalibrationValue(section,
		readHand, "AimRotationZ", defaults.aimRotationZ/fallbackDivisor)*
		conversion, -360), 360);
	calibration.rotationX = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"RotationX", defaults.rotationX/fallbackDivisor)*conversion, -360), 360);
	calibration.rotationY = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"RotationY", defaults.rotationY/fallbackDivisor)*conversion, -360), 360);
	calibration.rotationZ = Min(Max(ReadWeaponCalibrationValue(section, readHand,
		"RotationZ", defaults.rotationZ/fallbackDivisor)*conversion, -360), 360);
	if(hand == 0){
		const WeaponCalibration *right = GetWeaponCalibration(1, weaponType);
		if(right)
			ApplyMirroredWeaponAim(*right, calibration);
	}
	calibration.valid = true;
	return &calibration;
}

void EnsureModernProfilesForEdit(int weaponType, int hand);

void SaveCurrentWeaponCalibrationValue(const char *name, int value)
{
	SyncCurrentWeaponCalibration();
	const int weaponType = GetCalibrationWeaponType();
	const int editHand = gCalibrationEditHand == 0 ? 0 : 1;
	const bool aimValue = IsWeaponAimCalibrationValue(name);
	// RIGHT owns the six aim/laser values. Globals stay canonical; the LEFT menu
	// is a read-only mirrored display and LEFT gameplay aim is reflected only
	// when its runtime calibration is read.
	const int storageHand = aimValue ? 1 : editHand;
	const int storageValue = value;
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	const bool rightWasConfigured =
		IsWeaponCalibrationConfigured(section, 1);
	const bool editWasConfigured =
		IsWeaponCalibrationConfigured(section, editHand);
	if(storageHand == 1 && !rightWasConfigured){
		// Backward-safe first RIGHT edit: materialize the complete canonical
		// profile before changing one component. For the uncommon LEFT-only INI,
		// retain its effective aim even when the first RIGHT edit is only a model
		// offset/rotation; otherwise merely positioning the right-hand model would
		// unexpectedly replace the old left laser with built-in defaults. No
		// existing LEFT keys are removed.
		WeaponCalibration right;
		const WeaponCalibration *effectiveRight =
			GetWeaponCalibration(1, weaponType);
		if(effectiveRight)
			right = *effectiveRight;
		else
			GetDefaultWeaponCalibration(weaponType, 1, right);
		// When LEFT is the model being edited, globals contain LEFT model pose plus
		// canonical RIGHT aim. Copy only the aim fields into the effective RIGHT
		// model profile before materialising it.
		if(editHand == 0){
			WeaponCalibration current;
			CaptureCurrentWeaponCalibration(current);
			CopyWeaponAim(current, right);
		}
		BackupVrSettings("right_aim_canonical_seed");
		WriteCompleteWeaponCalibration(section, 1, right);
	}
	if(!aimValue && storageHand == 0 && !editWasConfigured){
		WeaponCalibration canonicalComposite;
		CaptureCurrentWeaponCalibration(canonicalComposite);
		WeaponCalibration left = canonicalComposite;
		ApplyMirroredWeaponAim(canonicalComposite, left);
		BackupVrSettings("left_model_profile_seed");
		WriteCompleteWeaponCalibration(section, 0, left);
	}
	if(IsWeaponCalibrationConfigured(section, storageHand) &&
	   ReadWeaponCalibrationValue(section, storageHand, "ValueScale", 1) !=
		WEAPON_CALIBRATION_VALUE_SCALE){
		// Normalize the complete target profile for every kind of edit. Updating
		// ValueScale beside only one model or aim key would halve all untouched
		// fields on the next load and couple model placement back into the laser.
		WeaponCalibration normalized;
		if(storageHand == editHand)
			CaptureCurrentWeaponCalibration(normalized);
		else{
			const WeaponCalibration *effective =
				GetWeaponCalibration(storageHand, weaponType);
			if(effective)
				normalized = *effective;
			else
				GetDefaultWeaponCalibration(weaponType, storageHand, normalized);
			WeaponCalibration current;
			CaptureCurrentWeaponCalibration(current);
			CopyWeaponAim(current, normalized);
		}
		if(storageHand == 0){
			WeaponCalibration canonical = normalized;
			ApplyMirroredWeaponAim(canonical, normalized);
		}
		BackupVrSettings(storageHand == 0 ? "left_weapon_scale_normalize" :
			"right_weapon_scale_normalize");
		WriteCompleteWeaponCalibration(section, storageHand, normalized);
	}
	EnsureModernProfilesForEdit(weaponType, storageHand);
	if(weaponType >= 0 && weaponType < WEAPONTYPE_TOTALWEAPONS){
		gWeaponCalibration[storageHand][weaponType].valid = false;
		// Creating the first RIGHT profile also changes the canonical aim seen by
		// an already-cached legacy LEFT-only profile, even when this particular
		// edit changed only model placement.
		if(aimValue || (storageHand == 1 && !rightWasConfigured))
			gWeaponCalibration[0][weaponType].valid = false;
		for(int hand = 0; hand < EYE_COUNT; hand++)
			gTrackedAimCacheValid[hand] = false;
	}
	// The complete profile is materialised/normalised above when necessary.
	// Repeating only the changed key avoids three synchronous INI writes for
	// every accelerated calibration step.
	WriteWeaponCalibrationValue(section, storageHand, name, storageValue);
}

void SaveCurrentWeaponAimAligned(bool aligned);
bool GetEffectiveAlignedRightAimCalibration(int weaponType,
	WeaponCalibration *effective);

bool IsSupportGripCalibrationConfigured(const char *section, int hand)
{
	char key[64];
	return GetPrivateProfileIntA(section,
		GetWeaponCalibrationKey(hand, "SupportGripConfigured", key), 0,
		GetVrSettingsPath()) != 0;
}

bool IsSupportGripStyleConfigured(const char *section)
{
	char value[16] = {};
	GetPrivateProfileStringA(section, "SupportGripStyle", "", value,
		ARRAY_SIZE(value), GetVrSettingsPath());
	return value[0] != '\0';
}

int ReadSupportGripStyleForSet(int weaponType, ModelSets::eModelSet modelSet,
	int fallback)
{
	char section[96];
	if(!GetWeaponCalibrationSectionForSet(weaponType, modelSet, section))
		return fallback;
	if(IsSupportGripStyleConfigured(section))
		return Min(Max((int)(int32)GetPrivateProfileIntA(section,
			"SupportGripStyle", fallback, GetVrSettingsPath()), 0),
			VR_SUPPORT_GRIP_TYPE_COUNT-1);
	// An unset Modern style is a read-only view of Classic until the first
	// Modern edit materialises the shared key in its own weapon section.
	if(modelSet == ModelSets::MODEL_SET_MODERN)
		return ReadSupportGripStyleForSet(weaponType,
			ModelSets::MODEL_SET_CLASSIC, fallback);
	return Min(Max(fallback, 0), VR_SUPPORT_GRIP_TYPE_COUNT-1);
}

void WriteSupportGripStyle(const char *section, int gripType)
{
	char value[16];
	sprintf(value, "%d", Min(Max(gripType, 0),
		VR_SUPPORT_GRIP_TYPE_COUNT-1));
	WritePrivateProfileStringA(section, "SupportGripStyle", value,
		GetVrSettingsPath());
}

void ConvertSupportGripV4ToV5(SupportGripCalibration &calibration)
{
	// The released v4 preview used (barrel,-top,-lateral) as its stable authored
	// axes. Preserve the exact socket and wrist while expressing position in v5's
	// canonical (lateral,barrel,top) model frame. Wrist Euler values stay literal
	// because v5 deliberately retains that proven in-headset authoring basis.
	const int oldX = calibration.offsetX;
	const int oldY = calibration.offsetY;
	const int oldZ = calibration.offsetZ;
	calibration.offsetX = -oldZ;
	calibration.offsetY = oldX;
	calibration.offsetZ = -oldY;
	calibration.poseVersion = SUPPORT_GRIP_POSE_VERSION;
}

void ReadConfiguredSupportGripCalibration(const char *section, int weaponType,
	int hand, ModelSets::eModelSet modelSet,
	SupportGripCalibration &calibration)
{
	const int storedScale = ReadWeaponCalibrationValue(section, hand,
		"SupportGripValueScale", 1);
	const int conversion = storedScale == WEAPON_CALIBRATION_VALUE_SCALE ? 1 :
		WEAPON_CALIBRATION_VALUE_SCALE;
	const int poseVersion = ReadWeaponCalibrationValue(section, hand,
		"SupportGripPoseVersion", 1);
	SupportGripCalibration defaults;
	if(poseVersion >= SUPPORT_GRIP_POSE_VERSION){
		GetDefaultSupportGripCalibrationForSet(weaponType, hand, modelSet,
			defaults);
	}else if(poseVersion == SUPPORT_GRIP_MODEL_BOUND_V4){
		// V4 was canonical RIGHT too, but used a permuted model-local socket. Its
		// partial-profile fallback must be built in that exact old contract before
		// the complete effective value is converted below.
		SupportGripCalibration oldRight;
		if(!GetBuiltInSupportGripCalibrationForSet(weaponType, 1, modelSet,
		   &oldRight))
			oldRight.valid = true;
		oldRight.poseVersion = SUPPORT_GRIP_MODEL_BOUND_V4;
		defaults = oldRight;
		if(hand == 0)
			ApplyMirroredSupportGrip(oldRight, defaults);
	}else{
		// Partial legacy profiles need their original per-controller authored
		// fallback. The v4 LEFT default is derived from canonical RIGHT and can
		// differ materially from the historical LEFT socket table.
		defaults = SupportGripCalibration();
		if(!GetBuiltInSupportGripCalibrationForSet(weaponType, hand, modelSet,
		   &defaults))
			defaults.valid = true;
		defaults.poseVersion = SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION;
		if(hand == 0){
			// Legacy LEFT keys describe a LEFT-primary profile. Its axial neutral
			// used the mirrored sign even though raw-frame X itself was copied.
			defaults.rotationY = -defaults.rotationY;
			defaults.rotationZ = -defaults.rotationZ;
		}
	}
	// The short-lived v2 schema stored a correction relative to a zero-degree
	// neutral. v1 and v3 store the absolute wrist rotation. Read a missing v2 Y
	// as zero, then add back the anatomical half-turn below.
	const int rotationYFallback = poseVersion == 2 ? 0 :
		defaults.rotationY/conversion;
	calibration.offsetX = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"SupportGripOffsetX", defaults.offsetX/conversion)*conversion,
		-200), 200);
	calibration.offsetY = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"SupportGripOffsetY", defaults.offsetY/conversion)*conversion,
		-200), 200);
	calibration.offsetZ = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"SupportGripOffsetZ", defaults.offsetZ/conversion)*conversion,
		-200), 200);
	calibration.rotationX = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"SupportGripRotationX", defaults.rotationX/conversion)*conversion,
		-360), 360);
	calibration.rotationY = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"SupportGripRotationY", rotationYFallback)*conversion,
		-360), 360);
	if(poseVersion == 2){
		calibration.rotationY += hand == 0 ? -360 : 360;
		while(calibration.rotationY > 360) calibration.rotationY -= 720;
		while(calibration.rotationY < -360) calibration.rotationY += 720;
	}
	calibration.rotationZ = Min(Max(ReadWeaponCalibrationValue(section, hand,
		"SupportGripRotationZ", defaults.rotationZ/conversion)*conversion,
		-360), 360);
	calibration.gripType = ReadSupportGripStyleForSet(weaponType, modelSet,
		defaults.gripType);
	// v1 and v3 are the same absolute, controller-local coordinate contract. v2
	// is converted above into that contract. V4 is upgraded in memory to the
	// stable deterministic v5 model frame and persisted on the next numeric edit.
	calibration.poseVersion = poseVersion >= SUPPORT_GRIP_POSE_VERSION ?
		SUPPORT_GRIP_POSE_VERSION :
		(poseVersion == SUPPORT_GRIP_MODEL_BOUND_V4 ?
		 SUPPORT_GRIP_MODEL_BOUND_V4 : SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION);
	if(calibration.poseVersion == SUPPORT_GRIP_MODEL_BOUND_V4)
		ConvertSupportGripV4ToV5(calibration);
	calibration.valid = true;
}

void LoadEffectiveSupportGripCalibrationForSet(int weaponType,
	ModelSets::eModelSet modelSet, SupportGripCalibration &calibration)
{
	char section[96];
	GetWeaponCalibrationSectionForSet(weaponType, modelSet, section);
	if(IsSupportGripCalibrationConfigured(section, 1)){
		ReadConfiguredSupportGripCalibration(section, weaponType, 1, modelSet,
			calibration);
		return;
	}
	if(IsSupportGripCalibrationConfigured(section, 0)){
		// Import a LEFT-only legacy socket once as an in-memory RIGHT canonical
		// value. The old keys stay untouched until the player edits this weapon.
		SupportGripCalibration legacyLeft;
		ReadConfiguredSupportGripCalibration(section, weaponType, 0, modelSet,
			legacyLeft);
		ApplyMirroredSupportGrip(legacyLeft, calibration);
		calibration.valid = true;
		return;
	}
	if(modelSet == ModelSets::MODEL_SET_MODERN){
		if(FindBuiltInSupportGripDefaults(weaponType, modelSet)){
			GetDefaultSupportGripCalibrationForSet(weaponType, 1, modelSet,
				calibration);
			calibration.gripType = ReadSupportGripStyleForSet(weaponType,
				modelSet, calibration.gripType);
			calibration.valid = true;
			return;
		}
		LoadEffectiveSupportGripCalibrationForSet(weaponType,
			ModelSets::MODEL_SET_CLASSIC, calibration);
		calibration.gripType = ReadSupportGripStyleForSet(weaponType, modelSet,
			calibration.gripType);
		return;
	}
	GetDefaultSupportGripCalibrationForSet(weaponType, 1, modelSet,
		calibration);
	calibration.gripType = ReadSupportGripStyleForSet(weaponType, modelSet,
		calibration.gripType);
	calibration.valid = true;
}

void CaptureCurrentSupportGripCalibration(SupportGripCalibration &calibration)
{
	calibration.offsetX = gSupportGripOffsetXCm;
	calibration.offsetY = gSupportGripOffsetYCm;
	calibration.offsetZ = gSupportGripOffsetZCm;
	calibration.rotationX = gSupportGripRotationXDeg;
	calibration.rotationY = gSupportGripRotationYDeg;
	calibration.rotationZ = gSupportGripRotationZDeg;
	calibration.gripType = gSupportGripType;
	calibration.poseVersion = gSupportGripPoseVersion;
	calibration.valid = true;
}

void WriteCompleteSupportGripCalibration(const char *section, int hand,
	const SupportGripCalibration &calibration)
{
	WriteWeaponCalibrationValue(section, hand, "SupportGripConfigured", 1);
	WriteWeaponCalibrationValue(section, hand, "SupportGripValueScale",
		WEAPON_CALIBRATION_VALUE_SCALE);
	WriteWeaponCalibrationValue(section, hand, "SupportGripOffsetX",
		calibration.offsetX);
	WriteWeaponCalibrationValue(section, hand, "SupportGripOffsetY",
		calibration.offsetY);
	WriteWeaponCalibrationValue(section, hand, "SupportGripOffsetZ",
		calibration.offsetZ);
	WriteWeaponCalibrationValue(section, hand, "SupportGripRotationX",
		calibration.rotationX);
	WriteWeaponCalibrationValue(section, hand, "SupportGripRotationY",
		calibration.rotationY);
	WriteWeaponCalibrationValue(section, hand, "SupportGripRotationZ",
		calibration.rotationZ);
	WriteSupportGripStyle(section, calibration.gripType);
	// Commit the coordinate contract last. Legacy profiles remain controller-local
	// when merely normalized; fresh/default profiles use deterministic model-local v5.
	WriteWeaponCalibrationValue(section, hand, "SupportGripPoseVersion",
		calibration.poseVersion >= SUPPORT_GRIP_POSE_VERSION ?
		SUPPORT_GRIP_POSE_VERSION :
		(calibration.poseVersion == SUPPORT_GRIP_MODEL_BOUND_V4 ?
		 SUPPORT_GRIP_MODEL_BOUND_V4 : SUPPORT_GRIP_LEGACY_ABSOLUTE_VERSION));
}

void SyncCurrentSupportGripCalibration()
{
	const int weaponType = GetCalibrationWeaponType();
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS ||
	   weaponType == gActiveSupportGripCalibration)
		return;
	gActiveSupportGripCalibration = weaponType;
	SupportGripCalibration right;
	LoadEffectiveSupportGripCalibrationForSet(weaponType,
		GetActiveWeaponModelSet(), right);
	ApplyCurrentSupportGripCalibration(right);
	gSupportGripCalibration[1][weaponType] = right;
	gSupportGripCalibration[0][weaponType].valid = false;
}

const SupportGripCalibration *GetSupportGripCalibration(int hand, int weaponType)
{
	if(hand < 0 || hand >= EYE_COUNT || weaponType < 0 ||
	   weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return nil;
	if(weaponType == GetCalibrationWeaponType()){
		SyncCurrentSupportGripCalibration();
		SupportGripCalibration right;
		CaptureCurrentSupportGripCalibration(right);
		gSupportGripCalibration[hand][weaponType] = right;
		if(hand == 0)
			ApplyMirroredSupportGrip(right,
				gSupportGripCalibration[hand][weaponType]);
		return &gSupportGripCalibration[hand][weaponType];
	}
	SupportGripCalibration &calibration =
		gSupportGripCalibration[hand][weaponType];
	if(calibration.valid)
		return &calibration;
	SupportGripCalibration right;
	LoadEffectiveSupportGripCalibrationForSet(weaponType,
		GetActiveWeaponModelSet(), right);
	calibration = right;
	if(hand == 0)
		ApplyMirroredSupportGrip(right, calibration);
	return &calibration;
}

void EnsureModernProfilesForEdit(int weaponType, int hand)
{
	if(!IsModernWeaponModelSetActive() || hand < 0 || hand >= EYE_COUNT ||
	   weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return;
	char modernSection[96];
	if(!GetWeaponCalibrationSectionForSet(weaponType,
	   ModelSets::MODEL_SET_MODERN, modernSection))
		return;
	const bool needsWeapon =
		!IsWeaponCalibrationConfigured(modernSection, hand);
	if(!needsWeapon)
		return;
	// The first Modern edit creates one self-contained absolute profile. This
	// freezes the effective Classic pose at that moment; subsequent Classic
	// edits never add to or silently move the Modern weapon.
	BackupVrSettings("modern_weapon_profile_seed");
	WeaponCalibration inherited;
	LoadEffectiveClassicWeaponCalibration(weaponType, hand, inherited);
	WriteCompleteWeaponCalibration(modernSection, hand, inherited);
	gWeaponCalibration[hand][weaponType].valid = false;
}

void SaveCurrentSupportGripCalibrationValue(const char *name, int value)
{
	SyncCurrentSupportGripCalibration();
	const int weaponType = GetCalibrationWeaponType();
	const int canonicalHand = 1;
	const bool sharedStyle = strcmp(name, "SupportGripStyle") == 0;
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	if(sharedStyle){
		if(IsModernWeaponModelSetActive() &&
		   !IsSupportGripStyleConfigured(section))
			BackupVrSettings("modern_support_style_seed");
		if(weaponType >= 0 && weaponType < WEAPONTYPE_TOTALWEAPONS){
			gSupportGripCalibration[0][weaponType].valid = false;
			gSupportGripCalibration[1][weaponType].valid = false;
		}
		WriteSupportGripStyle(section, value);
		return;
	}
	const bool canonicalConfigured =
		IsSupportGripCalibrationConfigured(section, canonicalHand);
	const bool legacyScale = canonicalConfigured &&
		ReadWeaponCalibrationValue(section, canonicalHand,
			"SupportGripValueScale", 1) != WEAPON_CALIBRATION_VALUE_SCALE;
	const int storedPoseVersion = canonicalConfigured ?
		ReadWeaponCalibrationValue(section, canonicalHand,
			"SupportGripPoseVersion", 1) : SUPPORT_GRIP_POSE_VERSION;
	// V2 needs wrist normalization; v4 needs an atomic socket-axis migration.
	// V1/v3 already contain absolute values and stay in their raw-controller frame.
	const bool legacyPose = canonicalConfigured &&
		(storedPoseVersion == 2 || storedPoseVersion == SUPPORT_GRIP_MODEL_BOUND_V4);
	if(!canonicalConfigured || legacyScale || legacyPose){
		// Preserve every effective component when the first edit creates the sole
		// canonical profile, or when a legacy profile is normalized. Merely changing
		// ValueScale/frame version beside one key would corrupt the other components.
		SupportGripCalibration right;
		CaptureCurrentSupportGripCalibration(right);
		BackupVrSettings(legacyPose ?
			(storedPoseVersion == SUPPORT_GRIP_MODEL_BOUND_V4 ?
			 "support_pose_v5_normalize" : "support_pose_v2_normalize") :
			(legacyScale ? "right_support_scale_normalize" :
			 "right_support_canonical_seed"));
		WriteCompleteSupportGripCalibration(section, canonicalHand, right);
	}
	if(weaponType >= 0 && weaponType < WEAPONTYPE_TOTALWEAPONS){
		gSupportGripCalibration[0][weaponType].valid = false;
		gSupportGripCalibration[1][weaponType].valid = false;
	}
	// WriteCompleteSupportGripCalibration already commits markers on first use or
	// migration. Subsequent repeats only need the value that actually changed.
	WriteWeaponCalibrationValue(section, canonicalHand, name, value);
}

const char *FoveatedProfileName()
{
#ifdef RW_D3D12
	switch(gFixedFoveatedProfile){
	case rw::d3d12::FIXED_FOVEATED_OFF: return "OFF";
	case rw::d3d12::FIXED_FOVEATED_QUALITY: return "QUALITY";
	case rw::d3d12::FIXED_FOVEATED_BALANCED: return "BALANCED";
	case rw::d3d12::FIXED_FOVEATED_PERFORMANCE: return "PERFORMANCE";
	default: return "NA";
	}
#else
	return "NA";
#endif
}

const char *TemporalJitterModeName()
{
	switch(gTemporalJitterMode){
	case 0: return "OFF";
	case 2: return "X2";
	case 3: return "BOTH2";
	case 4: return "HALF";
	case 5: return "FLIPPED";
	default: return "NORMAL";
	}
}

const char *TemporalAaBackendName()
{
	switch(gTemporalAaBackend){
	case TEMPORAL_AA_DLAA: return "DLAA";
	case TEMPORAL_AA_FSR2: return "FSR2 NATIVE";
	default: return "OFF";
	}
}

bool IsSelectedTemporalAaSupported()
{
#ifdef RW_D3D12
	if(gTemporalAaBackend == TEMPORAL_AA_DLAA)
		return Dlaa::IsSupported();
	if(gTemporalAaBackend == TEMPORAL_AA_FSR2)
		return !gFsr2StereoActivationFailed && Fsr2::IsSupported();
#endif
	return false;
}

void ResetTemporalAaHistory()
{
	Dlaa::ResetHistory();
	Fsr2::ResetHistory();
#ifdef RW_D3D12
	// SSR reprojects a previous world frame too. Camera discontinuities such as
	// teleport, snap turn, recenter and cinema transitions must invalidate it
	// together with the temporal-AA histories.
	rw::d3d12::invalidateScreenSpaceReflectionHistory();
#endif
}

void QueueTemporalAaBackendRelease(int previousBackend, int nextBackend)
{
#ifdef RW_D3D12
	if(previousBackend == TEMPORAL_AA_DLAA &&
	   nextBackend != TEMPORAL_AA_DLAA)
		gTemporalAaReleasePending |= TEMPORAL_AA_RELEASE_DLAA;
	if(previousBackend == TEMPORAL_AA_FSR2 &&
	   nextBackend != TEMPORAL_AA_FSR2)
		gTemporalAaReleasePending |= TEMPORAL_AA_RELEASE_FSR2;
	if(nextBackend == TEMPORAL_AA_DLAA)
		gTemporalAaReleasePending &= ~TEMPORAL_AA_RELEASE_DLAA;
	if(nextBackend == TEMPORAL_AA_FSR2)
		gTemporalAaReleasePending &= ~TEMPORAL_AA_RELEASE_FSR2;
	// Only the selected backend is attached at startup now, so a backend
	// chosen later in the settings menu has to attach on the spot. Both are
	// idempotent and return immediately once attached.
	if(nextBackend == TEMPORAL_AA_DLAA)
		Dlaa::AttachDevice();
	else if(nextBackend == TEMPORAL_AA_FSR2)
		Fsr2::AttachDevice();
#else
	(void)previousBackend;
	(void)nextBackend;
#endif
}

void ApplyPendingTemporalAaRelease()
{
#ifdef RW_D3D12
	const uint32 pending = gTemporalAaReleasePending;
	if(pending == 0)
		return;
	gTemporalAaReleasePending = 0;
	// This runs before xrBeginFrame and before recording the next renderer
	// command list. Waiting here makes FSR2 context destruction safe even when
	// the previous frame was still executing on the GPU.
	rw::d3d12::waitForGpu();
	if((pending & TEMPORAL_AA_RELEASE_DLAA) != 0){
		Dlaa::ReleaseResources();
		rw::d3d12::releaseRasterMotionResources((rw::Raster*)gStereoColor);
		gDlaaStereoActivationReady = false;
		gDlaaStereoActivationFailed = false;
		gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
	}
	if((pending & TEMPORAL_AA_RELEASE_FSR2) != 0)
		Fsr2::ReleaseResources();
#endif
}

const char *DrivingTypeName(int drivingType)
{
	switch(drivingType){
	case VR_DRIVING_IMMERSIVE: return "IMMERSIVE";
	case VR_DRIVING_MOTION: return "MOTION";
	default: return "DEFAULT";
	}
}

const char *MotionSteeringHandName()
{
	return gMotionSteeringHand == 0 ? "LEFT" : "RIGHT";
}

int MaxSupportedRenderScaleIndex()
{
	if(gEye[0].swapchain.width <= 0 || gEye[0].swapchain.height <= 0)
		return (int)ARRAY_SIZE(gRenderScaleOptions)-1;
#ifdef RW_D3D12
	// FULL SPS uses one double-wide texture. D3D12 limits either texture axis
	// to 16384 texels, so the admissible scale also depends on the OpenXR
	// resolution selected in Meta Link.
	const int maxTextureDimension = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	int maximum = 0;
	for(int index = 0; index < (int)ARRAY_SIZE(gRenderScaleOptions); index++){
		const int width = (int)(gEye[0].swapchain.width*gRenderScaleOptions[index]+0.5f)*EYE_COUNT;
		const int height = (int)(gEye[0].swapchain.height*gRenderScaleOptions[index]+0.5f);
		if(width > maxTextureDimension || height > maxTextureDimension)
			break;
		maximum = index;
	}
	return maximum;
#else
	return (int)ARRAY_SIZE(gRenderScaleOptions)-1;
#endif
}

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

enum { VR_PERF_MAX_SAMPLES = 54000 };
struct PerfFrameSample
{
	double elapsedSeconds;
	float frameMs;
	float phaseMs[PERF_PHASE_COUNT];
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
	float textureDefaultResourceMs;
	float textureDescriptorMs;
	float textureFootprintMs;
	float textureUploadResourceMs;
	float textureCpuCopyMs;
	float textureQueueMs;
	uint64 textureUploadBytes;
	int textureUploadCount;
	float xrWaitFrameMs;
	float xrBeginFrameMs;
	float xrAcquireMs;
	float xrSwapchainWaitMs;
	float xrReleaseMs;
	float xrEndFrameMs;
	float xrLocateViewsMs;
	float d3d12ExternalSubmitMs;
	float d3d12FrameFenceWaitMs;
	float d3d12FullGpuWaitMs;
	uint32 gpuProfileValid;
	float gpuSceneMs;
	float gpuTemporalAaMs;
	float gpuResolveMs;
	float gpuTotalMs;
	float geometryInstanceMs;
	float geometryBufferUploadMs;
	uint64 geometryBufferBytes;
	uint64 worldSubmittedIndices;
	uint32 geometryInstances;
	uint32 worldDrawCalls;
	float stereoBundleBuildMs;
	float stereoBundleWaitMs;
	uint32 stereoBundleDrawCalls;
	uint32 stereoBundleFallbacks;
	uint32 stereoSinglePassBegins;
	uint32 stereoSinglePassDrawCalls;
	uint64 stereoSinglePassIndices;
	uint32 stereoSinglePassFallbacks;
	uint32 monoDrawCalls;
	uint32 replayDrawCalls;
	uint32 stageMonoDrawCalls[24];
	uint32 fixedFoveatedBegins;
	uint32 fixedFoveatedFailures;
	uint32 fixedFoveatedProfile;
	uint32 fixedFoveatedTileSize;
	float playerX, playerY, playerZ;
	// Frame time not attributed to any phase or OpenXR call. Kept as a first
	// class number so a future blind spot shows up instead of hiding.
	float otherMs;
	int particleActive;
	int heliDustActive;
};
PerfFrameSample gPerfSamples[VR_PERF_MAX_SAMPLES];
PerfFrameSample gPerfCurrent;
double gPerfFrameStartMs;
double gPerfRecordingStartMs;
double gPerfPhaseStartMs[PERF_PHASE_COUNT];
int gPerfRecordedSamples;
bool gPerfRecording;
bool gPerfFrameStarted;
double gPerfStreamItemStartMs;
int gPerfStreamItemId;
int gPerfStreamItemType;
FILE *gPerfLiveCsv;
const CEntity *gPerfVisibleBuildingEntities[NUMVISIBLEENTITIES];
int gPerfVisibleBuildingEntityCount;

// What the in-headset profiler panel reads. Raw per-frame numbers jitter far
// too much to read while playing, so every field is exponentially smoothed and
// the worst frame time of the last two seconds is kept alongside it.
PerfFrameSample gPerfDisplay;
float gPerfDisplayWorstMs;
float gPerfDisplayWorstWindowMs;
double gPerfDisplayWorstResetMs;

double PerfNowMs();

// Release builds collect detailed phase/GPU counters only while the player is
// actually viewing the profiler or explicitly recording a capture. This keeps
// normal gameplay free of diagnostic timestamps and backend counter resets.
bool PerfCollecting(){ return gDebugVisible || gPerfRecording; }

static void SmoothFloat(float *value, float sample)
{
	*value = *value > 0.0f ? *value*0.9f + sample*0.1f : sample;
}
static void SmoothUint(uint32 *value, uint32 sample)
{
	*value = (uint32)(*value*0.9f + sample*0.1f + 0.5f);
}

void PerfPublishDisplaySample(const PerfFrameSample &s)
{
	SmoothFloat(&gPerfDisplay.frameMs, s.frameMs);
	for(int i = 0; i < PERF_PHASE_COUNT; i++)
		SmoothFloat(&gPerfDisplay.phaseMs[i], s.phaseMs[i]);
	SmoothFloat(&gPerfDisplay.xrWaitFrameMs, s.xrWaitFrameMs);
	SmoothFloat(&gPerfDisplay.xrBeginFrameMs, s.xrBeginFrameMs);
	SmoothFloat(&gPerfDisplay.xrEndFrameMs, s.xrEndFrameMs);
	SmoothFloat(&gPerfDisplay.xrAcquireMs, s.xrAcquireMs);
	SmoothFloat(&gPerfDisplay.xrSwapchainWaitMs, s.xrSwapchainWaitMs);
	SmoothFloat(&gPerfDisplay.d3d12FrameFenceWaitMs, s.d3d12FrameFenceWaitMs);
	SmoothFloat(&gPerfDisplay.d3d12FullGpuWaitMs, s.d3d12FullGpuWaitMs);
	SmoothFloat(&gPerfDisplay.d3d12ExternalSubmitMs, s.d3d12ExternalSubmitMs);
	SmoothFloat(&gPerfDisplay.geometryInstanceMs, s.geometryInstanceMs);
	SmoothFloat(&gPerfDisplay.geometryBufferUploadMs, s.geometryBufferUploadMs);
	SmoothFloat(&gPerfDisplay.stereoBundleBuildMs, s.stereoBundleBuildMs);
	SmoothFloat(&gPerfDisplay.stereoBundleWaitMs, s.stereoBundleWaitMs);
	SmoothFloat(&gPerfDisplay.textureCpuCopyMs, s.textureCpuCopyMs);
	SmoothFloat(&gPerfDisplay.textureQueueMs, s.textureQueueMs);
	SmoothFloat(&gPerfDisplay.textureUploadResourceMs, s.textureUploadResourceMs);
	SmoothFloat(&gPerfDisplay.textureDefaultResourceMs, s.textureDefaultResourceMs);
	SmoothFloat(&gPerfDisplay.slowStreamItemMs, s.slowStreamItemMs);
	SmoothFloat(&gPerfDisplay.otherMs, s.otherMs);
	SmoothUint(&gPerfDisplay.worldDrawCalls, s.worldDrawCalls);
	SmoothUint(&gPerfDisplay.geometryInstances, s.geometryInstances);
	SmoothUint(&gPerfDisplay.stereoSinglePassDrawCalls, s.stereoSinglePassDrawCalls);
	SmoothUint(&gPerfDisplay.monoDrawCalls, s.monoDrawCalls);
	SmoothUint(&gPerfDisplay.replayDrawCalls, s.replayDrawCalls);
	for(int i = 0; i < 24; i++)
		SmoothUint(&gPerfDisplay.stageMonoDrawCalls[i], s.stageMonoDrawCalls[i]);
	gPerfDisplay.worldSubmittedIndices = s.worldSubmittedIndices;
	gPerfDisplay.stereoSinglePassIndices = s.stereoSinglePassIndices;
	gPerfDisplay.stereoBundleFallbacks = s.stereoBundleFallbacks;
	gPerfDisplay.stereoSinglePassFallbacks = s.stereoSinglePassFallbacks;
	gPerfDisplay.fixedFoveatedFailures = s.fixedFoveatedFailures;
	gPerfDisplay.visibleBuildings = s.visibleBuildings;
	gPerfDisplay.visibleObjects = s.visibleObjects;
	gPerfDisplay.visiblePeds = s.visiblePeds;
	gPerfDisplay.visibleVehicles = s.visibleVehicles;
	gPerfDisplay.entityRenderCalls = s.entityRenderCalls;
	gPerfDisplay.requestedModels = s.requestedModels;
	gPerfDisplay.streamingMemory = s.streamingMemory;
	gPerfDisplay.textureUploadBytes = s.textureUploadBytes;
	gPerfDisplay.textureUploadCount = s.textureUploadCount;
	gPerfDisplay.geometryBufferBytes = s.geometryBufferBytes;
	gPerfDisplay.slowStreamItemId = s.slowStreamItemId;
	gPerfDisplay.particleActive = s.particleActive;
	gPerfDisplay.heliDustActive = s.heliDustActive;

	// Rolling worst frame: track the peak, then hand it over every two seconds
	// so a single hitch stays readable without sticking forever.
	const double now = PerfNowMs();
	if(s.frameMs > gPerfDisplayWorstWindowMs)
		gPerfDisplayWorstWindowMs = s.frameMs;
	if(gPerfDisplayWorstResetMs <= 0.0)
		gPerfDisplayWorstResetMs = now;
	if(now-gPerfDisplayWorstResetMs >= 2000.0){
		gPerfDisplayWorstMs = gPerfDisplayWorstWindowMs;
		gPerfDisplayWorstWindowMs = 0.0f;
		gPerfDisplayWorstResetMs = now;
	}
}

bool XrOk(XrResult result, const char *operation)
{
	if(XR_SUCCEEDED(result))
		return true;
	char message[XR_MAX_RESULT_STRING_SIZE] = {};
	if(gInstance)
		xrResultToString(gInstance, result, message);

	// A wedged session repeats the same failure every frame forever. Logging
	// each one buries the interesting part of the file -- the events leading
	// up to it -- under megabytes of identical lines, so a repeat is counted
	// instead of written, and reported once when it stops.
	static const char *lastOperation = nil;
	static XrResult lastResult = XR_SUCCESS;
	static unsigned int repeatCount = 0;
	if(operation == lastOperation && result == lastResult){
		repeatCount++;
		// Occasional heartbeats keep a genuinely stuck session visible.
		if(repeatCount == 10 || repeatCount == 1000 ||
		   (repeatCount % 10000) == 0)
			VrLog("FAIL %s repeated %u times\n", operation, repeatCount);
		return false;
	}
	if(repeatCount > 0){
		VrLog("FAIL %s stopped repeating after %u times\n",
			lastOperation ? lastOperation : "?", repeatCount);
		repeatCount = 0;
	}
	lastOperation = operation;
	lastResult = result;

	debug("[OpenXR] %s failed (%d): %s\n", operation, result, message);
	VrLog("FAIL %s result=%d message=%s\n", operation, result, message);
	return false;
}

void SetFramePhase(const char *phase)
{
	if(!phase)
		phase = "unknown";
	if(strcmp(gCurrentFramePhase, phase) != 0){
		gCurrentFramePhase = phase;
		VrLog("Frame phase=%s\n", phase);
	}
}

void ScheduleRuntimeRecovery(const char *operation, XrResult result)
{
	if(result != XR_ERROR_RUNTIME_FAILURE &&
	   result != XR_ERROR_SESSION_LOST &&
	   result != XR_ERROR_INSTANCE_LOST)
		return;
	if(!gRuntimeRecoveryPending)
		VrLog("OpenXR recovery scheduled phase=%s operation=%s result=%d state=%d\n",
			gCurrentFramePhase, operation ? operation : "?", (int)result,
			(int)gSessionState);
	gRuntimeRecoveryPending = true;
}

#ifndef RW_D3D12
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
	debug("[OpenXR] FXAA shader compilation failed: %s\n", log);
	glDeleteShader(shader);
	return 0;
}

bool CreateFxaaProgram()
{
	static const char *vertexSource =
		"#version 330 core\n"
		"out vec2 uv;\n"
		"void main(){\n"
		" vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
		" uv=p; gl_Position=vec4(p*2.0-1.0,0.0,1.0);\n"
		"}\n";
	static const char *fragmentSource =
		"#version 330 core\n"
		"uniform sampler2D sourceTexture; uniform vec2 inverseScreenSize;\n"
		"uniform vec2 sourceUvScale; uniform vec2 sourceUvOffset;\n"
		"uniform int fxaaEnabled; uniform int colorMode; uniform vec4 blurColor;\n"
		"uniform vec3 contrastMult; uniform vec3 contrastAdd; in vec2 uv; out vec4 outColor;\n"
		"float luma(vec3 c){return dot(c,vec3(0.299,0.587,0.114));}\n"
		"vec3 s(vec2 p){vec2 sourceUv=p*sourceUvScale+sourceUvOffset;\n"
		" return texture(sourceTexture,clamp(sourceUv,vec2(0.0),vec2(1.0))).rgb;}\n"
		"void main(){\n"
		" vec3 m=s(uv),nw=s(uv+vec2(-1,-1)*inverseScreenSize),ne=s(uv+vec2(1,-1)*inverseScreenSize);\n"
		" vec3 sw=s(uv+vec2(-1,1)*inverseScreenSize),se=s(uv+vec2(1,1)*inverseScreenSize);\n"
		" float lm=luma(m),lnw=luma(nw),lne=luma(ne),lsw=luma(sw),lse=luma(se);\n"
		" float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));\n"
		" vec2 d=vec2(-((lnw+lne)-(lsw+lse)),((lnw+lsw)-(lne+lse)));\n"
		" float reduce=max((lnw+lne+lsw+lse)*0.03125,0.0078125);\n"
		" d=clamp(d/(min(abs(d.x),abs(d.y))+reduce),vec2(-8),vec2(8))*inverseScreenSize;\n"
		" vec3 a=.5*(s(uv+d*(1.0/3.0-.5))+s(uv+d*(2.0/3.0-.5)));\n"
		" vec3 b=a*.5+.25*(s(uv+d*-.5)+s(uv+d*.5)); float lb=luma(b);\n"
		" vec3 color=fxaaEnabled!=0?((lb<lmin||lb>lmax)?a:b):m;\n"
		" if(colorMode==1){float alpha=blurColor.a;vec3 doubled=clamp(blurColor.rgb*2.0,0.0,1.0);\n"
		"  vec3 original=color,previous=color;for(int i=0;i<5;i++){vec3 f=original*(1.0-alpha)+previous*doubled*alpha;\n"
		"  f+=previous*blurColor.rgb*2.0;previous=clamp(f,0.0,1.0);}color=previous;}\n"
		" else if(colorMode==2)color=clamp(color*contrastMult+contrastAdd,0.0,1.0);\n"
		" outColor=vec4(color,1.0);}\n";
	GLuint vertex = CompileFxaaShader(GL_VERTEX_SHADER, vertexSource);
	GLuint fragment = CompileFxaaShader(GL_FRAGMENT_SHADER, fragmentSource);
	if(!vertex || !fragment){ if(vertex) glDeleteShader(vertex); if(fragment) glDeleteShader(fragment); return false; }
	gFxaaProgram = glCreateProgram();
	glAttachShader(gFxaaProgram, vertex); glAttachShader(gFxaaProgram, fragment); glLinkProgram(gFxaaProgram);
	glDeleteShader(vertex); glDeleteShader(fragment);
	GLint linked = GL_FALSE; glGetProgramiv(gFxaaProgram, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE){
		char log[1024] = {}; glGetProgramInfoLog(gFxaaProgram, sizeof(log), nil, log);
		debug("[OpenXR] FXAA program link failed: %s\n", log); glDeleteProgram(gFxaaProgram); gFxaaProgram=0; return false;
	}
	gFxaaTextureUniform=glGetUniformLocation(gFxaaProgram,"sourceTexture");
	gFxaaInverseSizeUniform=glGetUniformLocation(gFxaaProgram,"inverseScreenSize");
	gFxaaUvScaleUniform=glGetUniformLocation(gFxaaProgram,"sourceUvScale");
	gFxaaUvOffsetUniform=glGetUniformLocation(gFxaaProgram,"sourceUvOffset");
	gFxaaEnabledUniform=glGetUniformLocation(gFxaaProgram,"fxaaEnabled");
	gColorModeUniform=glGetUniformLocation(gFxaaProgram,"colorMode");
	gBlurColorUniform=glGetUniformLocation(gFxaaProgram,"blurColor");
	gContrastMultUniform=glGetUniformLocation(gFxaaProgram,"contrastMult");
	gContrastAddUniform=glGetUniformLocation(gFxaaProgram,"contrastAdd");
	glGenVertexArrays(1,&gFxaaVertexArray);
	return true;
}

void DestroyFxaaProgram()
{
	if(gFxaaVertexArray){ glDeleteVertexArrays(1,&gFxaaVertexArray); gFxaaVertexArray=0; }
	if(gFxaaProgram){ glDeleteProgram(gFxaaProgram); gFxaaProgram=0; }
}
#endif

double PerfNowMs()
{
	static LARGE_INTEGER frequency = {};
	if(!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart * 1000.0 / frequency.QuadPart;
}

void WritePerfCsvHeader(FILE *csv)
{
	fprintf(csv, "frame,elapsed_s,frame_ms,game_ms,stream_ms,slow_stream_item_ms,slow_stream_item_id,slow_stream_item_type,tex_default_resource_ms,tex_descriptor_ms,tex_footprint_ms,tex_upload_resource_ms,tex_cpu_copy_ms,tex_queue_ms,tex_upload_mb,tex_upload_count,xr_wait_frame_ms,xr_begin_frame_ms,xr_acquire_ms,xr_swapchain_wait_ms,xr_release_ms,xr_end_frame_ms,xr_locate_views_ms,d3d12_external_submit_ms,d3d12_frame_fence_wait_ms,d3d12_full_gpu_wait_ms,gpu_profile_valid,gpu_scene_ms,gpu_temporal_aa_ms,gpu_resolve_ms,gpu_total_ms,world_list_ms,pre_render_ms,desktop_render_ms,cinema_submit_ms,left_eye_ms,right_eye_ms,submit_ms,visible_buildings,visible_objects,visible_peds,visible_vehicles,entity_render_calls,requested_models,stream_memory_mb,player_x,player_y,player_z,audio_ms,scene_setup_ms,ui_ms,desktop_present_ms,geometry_instance_ms,geometry_buffer_upload_ms,geometry_buffer_mb,geometry_instances,world_draw_calls,world_indices,stereo_bundle_build_ms,stereo_bundle_wait_ms,stereo_bundle_draw_calls,stereo_bundle_fallbacks,stereo_single_pass_begins,stereo_single_pass_draw_calls,stereo_single_pass_indices,stereo_single_pass_fallbacks,vrs_begins,vrs_failures,vrs_profile,vrs_tile_size,frame_init_ms,vr_setup_ms,vr_overlays_ms,camera_end_ms,session_upkeep_ms,other_ms,mono_draw_calls,replay_draw_calls,particle_active,heli_dust_active\n");
}

void WritePerfCsvSample(FILE *csv, int index, const PerfFrameSample &s)
{
	fprintf(csv, "%d,%.6f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u,%llu,%.4f,%.4f,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u,%d,%d\n",
		index, s.elapsedSeconds, s.frameMs, s.phaseMs[PERF_PHASE_GAME],
		s.phaseMs[PERF_PHASE_STREAMING], s.slowStreamItemMs,
		s.slowStreamItemId, s.slowStreamItemType,
		s.textureDefaultResourceMs, s.textureDescriptorMs,
		s.textureFootprintMs, s.textureUploadResourceMs,
		s.textureCpuCopyMs, s.textureQueueMs,
		s.textureUploadBytes/(1024.0*1024.0), s.textureUploadCount,
		s.xrWaitFrameMs, s.xrBeginFrameMs, s.xrAcquireMs,
		s.xrSwapchainWaitMs, s.xrReleaseMs, s.xrEndFrameMs,
		s.xrLocateViewsMs, s.d3d12ExternalSubmitMs,
		s.d3d12FrameFenceWaitMs, s.d3d12FullGpuWaitMs,
		s.gpuProfileValid, s.gpuSceneMs, s.gpuTemporalAaMs,
		s.gpuResolveMs, s.gpuTotalMs,
		s.phaseMs[PERF_PHASE_WORLD_LIST],
		s.phaseMs[PERF_PHASE_PRE_RENDER], s.phaseMs[PERF_PHASE_DESKTOP_RENDER],
		s.phaseMs[PERF_PHASE_CINEMA_SUBMIT], s.phaseMs[PERF_PHASE_LEFT_EYE],
		s.phaseMs[PERF_PHASE_RIGHT_EYE], s.phaseMs[PERF_PHASE_SUBMIT],
		s.visibleBuildings, s.visibleObjects, s.visiblePeds, s.visibleVehicles,
		s.entityRenderCalls, s.requestedModels, s.streamingMemory/(1024.0*1024.0),
		s.playerX, s.playerY, s.playerZ,
		s.phaseMs[PERF_PHASE_AUDIO], s.phaseMs[PERF_PHASE_SCENE_SETUP],
		s.phaseMs[PERF_PHASE_UI], s.phaseMs[PERF_PHASE_DESKTOP_PRESENT],
		s.geometryInstanceMs, s.geometryBufferUploadMs,
		s.geometryBufferBytes/(1024.0*1024.0), s.geometryInstances,
		s.worldDrawCalls, (unsigned long long)s.worldSubmittedIndices,
		s.stereoBundleBuildMs, s.stereoBundleWaitMs,
		s.stereoBundleDrawCalls, s.stereoBundleFallbacks,
		s.stereoSinglePassBegins, s.stereoSinglePassDrawCalls,
		(unsigned long long)s.stereoSinglePassIndices,
		s.stereoSinglePassFallbacks, s.fixedFoveatedBegins,
		s.fixedFoveatedFailures, s.fixedFoveatedProfile,
		s.fixedFoveatedTileSize,
		s.phaseMs[PERF_PHASE_FRAME_INIT], s.phaseMs[PERF_PHASE_VR_SETUP],
		s.phaseMs[PERF_PHASE_VR_OVERLAYS], s.phaseMs[PERF_PHASE_CAMERA_END],
		s.phaseMs[PERF_PHASE_SESSION_UPKEEP],
		s.otherMs, s.monoDrawCalls, s.replayDrawCalls,
		s.particleActive, s.heliDustActive);
}

void DumpPerfRecording()
{
	if(gPerfRecordedSamples <= 0){
		debug("[VR PERF] No complete OpenXR frames recorded\n");
		return;
	}
	time_t wallTime = time(nil);
	struct tm localTime = {};
	localtime_s(&localTime, &wallTime);
	char stamp[32], csvName[96], reportName[96];
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &localTime);
	sprintf(csvName, "vr_perf_openxr_%s.csv", stamp);
	sprintf(reportName, "vr_perf_openxr_%s.txt", stamp);
	FILE *csv = fopen(csvName, "w");
	if(csv){
		WritePerfCsvHeader(csv);
		for(int i = 0; i < gPerfRecordedSamples; i++)
			WritePerfCsvSample(csv, i, gPerfSamples[i]);
		fclose(csv);
	}
	double total = 0.0;
	float worst = 0.0f;
	for(int i = 0; i < gPerfRecordedSamples; i++){
		total += gPerfSamples[i].frameMs;
		worst = Max(worst, gPerfSamples[i].frameMs);
	}
	FILE *report = fopen(reportName, "w");
	if(report){
		const float average = (float)(total/gPerfRecordedSamples);
		fprintf(report, "Vice City VR OpenXR performance capture\n");
		fprintf(report, "Frames: %d  Duration: %.2f s\n", gPerfRecordedSamples,
			gPerfSamples[gPerfRecordedSamples-1].elapsedSeconds);
		fprintf(report, "Average: %.3f ms (%.1f FPS)  Worst: %.3f ms\n", average,
			average > 0.0f ? 1000.0f/average : 0.0f, worst);
		fclose(report);
	}
	debug("[VR PERF] Saved %d OpenXR frames to %s and %s\n", gPerfRecordedSamples, csvName, reportName);
}

void TogglePerfRecording()
{
	if(gPerfRecording){
		gPerfRecording = false;
		gPerfFrameStarted = false;
		if(gPerfLiveCsv){ fclose(gPerfLiveCsv); gPerfLiveCsv = nil; }
		DumpPerfRecording();
	}else{
		gPerfRecordedSamples = 0;
		gPerfFrameStarted = false;
		gPerfRecordingStartMs = PerfNowMs();
		gPerfLiveCsv = fopen("vr_perf_openxr_live.csv", "w");
		if(gPerfLiveCsv){ WritePerfCsvHeader(gPerfLiveCsv); fflush(gPerfLiveCsv); }
		gPerfRecording = true;
		debug("[VR PERF] OpenXR recording started; both grips + Y saves it\n");
	}
}

XrPath Path(const char *text)
{
	XrPath result = XR_NULL_PATH;
	XrOk(xrStringToPath(gInstance, text, &result), text);
	return result;
}

bool CreateAction(XrAction &action, XrActionType type, const char *name, const char *localized,
	bool perHand = true)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE-1);
	strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
	if(perHand){
		info.countSubactionPaths = EYE_COUNT;
		info.subactionPaths = gActions.hands;
	}
	return XrOk(xrCreateAction(gActions.set, &info, &action), name);
}

struct BindingDef
{
	XrAction action;
	const char *path;
};

bool SuggestControllerBindings(const char *profilePath, const char *profileName,
	const BindingDef *defs, uint32 count)
{
	std::vector<XrActionSuggestedBinding> bindings;
	for(uint32 i = 0; i < count; i++){
		const XrPath bindingPath = Path(defs[i].path);
		if(bindingPath != XR_NULL_PATH){
			const XrActionSuggestedBinding binding = {
				defs[i].action, bindingPath
			};
			bindings.push_back(binding);
		}
	}
	const XrPath profile = Path(profilePath);
	if(profile == XR_NULL_PATH || bindings.empty())
		return false;
	XrInteractionProfileSuggestedBinding suggestion = {
		XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
	};
	suggestion.interactionProfile = profile;
	suggestion.countSuggestedBindings = (uint32)bindings.size();
	suggestion.suggestedBindings = bindings.data();
	char operation[96];
	sprintf(operation, "xrSuggestInteractionProfileBindings(%s)", profileName);
	const bool suggested = XrOk(
		xrSuggestInteractionProfileBindings(gInstance, &suggestion), operation);
	if(suggested)
		VrLog("Controller bindings enabled: %s\n", profilePath);
	return suggested;
}

bool CreateActions()
{
	gActions.hands[0] = Path("/user/hand/left");
	gActions.hands[1] = Path("/user/hand/right");
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(setInfo.actionSetName, "vice_city");
	strcpy(setInfo.localizedActionSetName, "Vice City VR");
	if(!XrOk(xrCreateActionSet(gInstance, &setInfo, &gActions.set), "xrCreateActionSet"))
		return false;
	if(!CreateAction(gActions.stick, XR_ACTION_TYPE_VECTOR2F_INPUT, "move_look", "Move and look") ||
	   !CreateAction(gActions.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip") ||
	   !CreateAction(gActions.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") ||
	   !CreateAction(gActions.gripPose, XR_ACTION_TYPE_POSE_INPUT, "hand_grip_pose", "Hand grip pose") ||
	   !CreateAction(gActions.aimPose, XR_ACTION_TYPE_POSE_INPUT, "hand_aim_pose", "Hand aim pose") ||
	   !CreateAction(gActions.a, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", false) ||
	   !CreateAction(gActions.b, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", false) ||
	   !CreateAction(gActions.x, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X", false) ||
	   !CreateAction(gActions.y, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y", false) ||
	   !CreateAction(gActions.stickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick click") ||
	   !CreateAction(gActions.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", false) ||
	   !CreateAction(gActions.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT,
		"weapon_haptic", "Weapon haptic"))
		return false;

	const BindingDef touchBindings[] = {
		{ gActions.stick, "/user/hand/left/input/thumbstick" },
		{ gActions.stick, "/user/hand/right/input/thumbstick" },
		{ gActions.squeeze, "/user/hand/left/input/squeeze/value" },
		{ gActions.squeeze, "/user/hand/right/input/squeeze/value" },
		{ gActions.trigger, "/user/hand/left/input/trigger/value" },
		{ gActions.trigger, "/user/hand/right/input/trigger/value" },
		{ gActions.gripPose, "/user/hand/left/input/grip/pose" },
		{ gActions.gripPose, "/user/hand/right/input/grip/pose" },
		{ gActions.aimPose, "/user/hand/left/input/aim/pose" },
		{ gActions.aimPose, "/user/hand/right/input/aim/pose" },
		{ gActions.a, "/user/hand/right/input/a/click" },
		{ gActions.b, "/user/hand/right/input/b/click" },
		{ gActions.x, "/user/hand/left/input/x/click" },
		{ gActions.y, "/user/hand/left/input/y/click" },
		{ gActions.stickClick, "/user/hand/left/input/thumbstick/click" },
		{ gActions.stickClick, "/user/hand/right/input/thumbstick/click" },
		{ gActions.menu, "/user/hand/left/input/menu/click" },
		{ gActions.haptic, "/user/hand/left/output/haptic" },
		{ gActions.haptic, "/user/hand/right/output/haptic" }
	};
	bool anyProfile = SuggestControllerBindings(
		"/interaction_profiles/oculus/touch_controller", "Touch",
		touchBindings, ARRAY_SIZE(touchBindings));

	const BindingDef indexBindings[] = {
		{ gActions.stick, "/user/hand/left/input/thumbstick" },
		{ gActions.stick, "/user/hand/right/input/thumbstick" },
		{ gActions.squeeze, "/user/hand/left/input/squeeze/value" },
		{ gActions.squeeze, "/user/hand/right/input/squeeze/value" },
		{ gActions.trigger, "/user/hand/left/input/trigger/value" },
		{ gActions.trigger, "/user/hand/right/input/trigger/value" },
		{ gActions.gripPose, "/user/hand/left/input/grip/pose" },
		{ gActions.gripPose, "/user/hand/right/input/grip/pose" },
		{ gActions.aimPose, "/user/hand/left/input/aim/pose" },
		{ gActions.aimPose, "/user/hand/right/input/aim/pose" },
		{ gActions.a, "/user/hand/right/input/a/click" },
		{ gActions.b, "/user/hand/right/input/b/click" },
		{ gActions.x, "/user/hand/left/input/a/click" },
		{ gActions.y, "/user/hand/left/input/b/click" },
		{ gActions.stickClick, "/user/hand/left/input/thumbstick/click" },
		{ gActions.stickClick, "/user/hand/right/input/thumbstick/click" },
		{ gActions.haptic, "/user/hand/left/output/haptic" },
		{ gActions.haptic, "/user/hand/right/output/haptic" }
	};
	anyProfile = SuggestControllerBindings(
		"/interaction_profiles/valve/index_controller", "Index",
		indexBindings, ARRAY_SIZE(indexBindings)) || anyProfile;

	const BindingDef viveBindings[] = {
		{ gActions.stick, "/user/hand/left/input/trackpad" },
		{ gActions.stick, "/user/hand/right/input/trackpad" },
		{ gActions.squeeze, "/user/hand/left/input/squeeze/click" },
		{ gActions.squeeze, "/user/hand/right/input/squeeze/click" },
		{ gActions.trigger, "/user/hand/left/input/trigger/value" },
		{ gActions.trigger, "/user/hand/right/input/trigger/value" },
		{ gActions.gripPose, "/user/hand/left/input/grip/pose" },
		{ gActions.gripPose, "/user/hand/right/input/grip/pose" },
		{ gActions.aimPose, "/user/hand/left/input/aim/pose" },
		{ gActions.aimPose, "/user/hand/right/input/aim/pose" },
		{ gActions.a, "/user/hand/right/input/trackpad/click" },
		{ gActions.b, "/user/hand/left/input/trackpad/click" },
		{ gActions.menu, "/user/hand/left/input/menu/click" },
		{ gActions.haptic, "/user/hand/left/output/haptic" },
		{ gActions.haptic, "/user/hand/right/output/haptic" }
	};
	anyProfile = SuggestControllerBindings(
		"/interaction_profiles/htc/vive_controller", "Vive",
		viveBindings, ARRAY_SIZE(viveBindings)) || anyProfile;

	const BindingDef wmrBindings[] = {
		{ gActions.stick, "/user/hand/left/input/thumbstick" },
		{ gActions.stick, "/user/hand/right/input/thumbstick" },
		{ gActions.squeeze, "/user/hand/left/input/squeeze/click" },
		{ gActions.squeeze, "/user/hand/right/input/squeeze/click" },
		{ gActions.trigger, "/user/hand/left/input/trigger/value" },
		{ gActions.trigger, "/user/hand/right/input/trigger/value" },
		{ gActions.gripPose, "/user/hand/left/input/grip/pose" },
		{ gActions.gripPose, "/user/hand/right/input/grip/pose" },
		{ gActions.aimPose, "/user/hand/left/input/aim/pose" },
		{ gActions.aimPose, "/user/hand/right/input/aim/pose" },
		{ gActions.a, "/user/hand/right/input/trackpad/click" },
		{ gActions.b, "/user/hand/left/input/trackpad/click" },
		{ gActions.stickClick, "/user/hand/left/input/thumbstick/click" },
		{ gActions.stickClick, "/user/hand/right/input/thumbstick/click" },
		{ gActions.menu, "/user/hand/left/input/menu/click" },
		{ gActions.haptic, "/user/hand/left/output/haptic" },
		{ gActions.haptic, "/user/hand/right/output/haptic" }
	};
	anyProfile = SuggestControllerBindings(
		"/interaction_profiles/microsoft/motion_controller", "WMR",
		wmrBindings, ARRAY_SIZE(wmrBindings)) || anyProfile;

	if(gGenericControllerExtensionEnabled){
		const BindingDef genericBindings[] = {
			{ gActions.stick, "/user/hand/left/input/thumbstick" },
			{ gActions.stick, "/user/hand/right/input/thumbstick" },
			{ gActions.squeeze, "/user/hand/left/input/squeeze/value" },
			{ gActions.squeeze, "/user/hand/right/input/squeeze/value" },
			{ gActions.trigger, "/user/hand/left/input/trigger/value" },
			{ gActions.trigger, "/user/hand/right/input/trigger/value" },
			{ gActions.gripPose, "/user/hand/left/input/grip/pose" },
			{ gActions.gripPose, "/user/hand/right/input/grip/pose" },
			{ gActions.aimPose, "/user/hand/left/input/aim/pose" },
			{ gActions.aimPose, "/user/hand/right/input/aim/pose" },
			{ gActions.a, "/user/hand/right/input/primary/click" },
			{ gActions.b, "/user/hand/right/input/secondary/click" },
			{ gActions.x, "/user/hand/left/input/primary/click" },
			{ gActions.y, "/user/hand/left/input/secondary/click" },
			{ gActions.stickClick, "/user/hand/left/input/thumbstick/click" },
			{ gActions.stickClick, "/user/hand/right/input/thumbstick/click" },
			{ gActions.haptic, "/user/hand/left/output/haptic" },
			{ gActions.haptic, "/user/hand/right/output/haptic" }
		};
		anyProfile = SuggestControllerBindings(
			"/interaction_profiles/khr/generic_controller", "Generic",
			genericBindings, ARRAY_SIZE(genericBindings)) || anyProfile;
	}
	return anyProfile;
}

// Retain replaced swapchains for several frames so compositor and GPU
// references from the preceding xrEndFrame can retire before destruction.
struct RetiredSwapchain { XrSwapchain handle; int framesLeft; };
std::vector<RetiredSwapchain> gRetiredSwapchains;
enum { SWAPCHAIN_RETIREMENT_FRAMES = 4 };

void RetireSwapchainHandle(XrSwapchain handle)
{
	if(handle == XR_NULL_HANDLE)
		return;
	RetiredSwapchain retired = { handle, SWAPCHAIN_RETIREMENT_FRAMES };
	gRetiredSwapchains.push_back(retired);
}

// Called once per submitted frame; destroys handles whose grace period is up.
void UpdateRetiredSwapchains()
{
	for(size_t i = 0; i < gRetiredSwapchains.size(); ){
		if(--gRetiredSwapchains[i].framesLeft <= 0){
			xrDestroySwapchain(gRetiredSwapchains[i].handle);
			gRetiredSwapchains.erase(gRetiredSwapchains.begin()+i);
		}else
			i++;
	}
}

void DestroyRetiredSwapchainsNow()
{
	for(size_t i = 0; i < gRetiredSwapchains.size(); i++)
		xrDestroySwapchain(gRetiredSwapchains[i].handle);
	gRetiredSwapchains.clear();
}

void DestroySwapchain(Swapchain &swapchain)
{
	if(&swapchain == &gCinemaSwapchain){
		gCinemaFrameValid = false;
		gCinemaContentWidth = 0;
		gCinemaContentHeight = 0;
	}
	// An acquired image must be handed back before the swapchain goes away.
	if(swapchain.acquired && swapchain.handle){
		XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(swapchain.handle, &release);
	}
	if(swapchain.handle)
		RetireSwapchainHandle(swapchain.handle);
	swapchain.handle = XR_NULL_HANDLE;
	swapchain.images.clear();
	swapchain.width = swapchain.height = 0;
	swapchain.acquired = false;
}

bool CreateSwapchain(Swapchain &swapchain, int width, int height)
{
	VrLog("CreateSwapchain begin %dx%d format=%lld\n", width, height, (long long)gColorFormat);
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
#ifdef RW_D3D12
	info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
#endif
	info.format = gColorFormat;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if(!XrOk(xrCreateSwapchain(gSession, &info, &swapchain.handle), "xrCreateSwapchain"))
		return false;
	uint32_t count = 0;
	if(!XrOk(xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nil),
		"xrEnumerateSwapchainImages(count)"))
		return false;
#ifdef RW_D3D12
	swapchain.images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
#else
	swapchain.images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
#endif
	if(!XrOk(xrEnumerateSwapchainImages(swapchain.handle, count, &count,
		(XrSwapchainImageBaseHeader*)swapchain.images.data()), "xrEnumerateSwapchainImages"))
		return false;
	swapchain.width = width;
	swapchain.height = height;
	VrLog("CreateSwapchain ok %dx%d images=%u\n", width, height, count);
	return true;
}

void ReleaseSwapchain(Swapchain &swapchain);

bool AcquireSwapchain(Swapchain &swapchain)
{
	if(swapchain.acquired)
		return true;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	double timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
	XrResult result = xrAcquireSwapchainImage(
		swapchain.handle, &acquire, &swapchain.acquiredIndex);
	if(gPerfFrameStarted)
		gPerfCurrent.xrAcquireMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrAcquireSwapchainImage")){
		ScheduleRuntimeRecovery("xrAcquireSwapchainImage", result);
		return false;
	}
	// Track ownership as soon as acquire succeeds. If the following wait fails,
	// the image is still acquired and must be released during recovery.
	swapchain.acquired = true;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
	result = xrWaitSwapchainImage(swapchain.handle, &wait);
	if(gPerfFrameStarted)
		gPerfCurrent.xrSwapchainWaitMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrWaitSwapchainImage")){
		ScheduleRuntimeRecovery("xrWaitSwapchainImage", result);
		ReleaseSwapchain(swapchain);
		return false;
	}
	return true;
}

void ReleaseSwapchain(Swapchain &swapchain)
{
	if(!swapchain.acquired)
		return;
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	const double timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
	XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
	if(gPerfFrameStarted)
		gPerfCurrent.xrReleaseMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrReleaseSwapchainImage"))
		ScheduleRuntimeRecovery("xrReleaseSwapchainImage", result);
	swapchain.acquired = false;
}

const uint8 *FindDebugGlyph(char character)
{
	if(character >= 'a' && character <= 'z')
		character = character-'a'+'A';
	for(uint32 i=0;i<ARRAY_SIZE(gDebugGlyphs);i++)
		if(gDebugGlyphs[i].character==character)
			return gDebugGlyphs[i].rows;
	return gDebugGlyphs[0].rows;
}

void PutDebugPixel(int x,int y,uint8 red,uint8 green,uint8 blue,uint8 alpha)
{
	if(x<0 || y<0 || x>=VR_DEBUG_WIDTH || y>=VR_DEBUG_HEIGHT) return;
#ifdef RW_D3D12
	const int offset=(y*VR_DEBUG_WIDTH+x)*4;
#else
	const int offset=((VR_DEBUG_HEIGHT-1-y)*VR_DEBUG_WIDTH+x)*4;
#endif
	gDebugPixels[offset+0]=red; gDebugPixels[offset+1]=green;
	gDebugPixels[offset+2]=blue; gDebugPixels[offset+3]=alpha;
}

void DrawDebugText(const char *value,int centreX,int y,int scale,uint8 red,uint8 green,uint8 blue)
{
	const int advance=scale*6;
	int x=centreX-(int)strlen(value)*advance/2;
	for(const char *ch=value;*ch;ch++,x+=advance){
		const uint8 *rows=FindDebugGlyph(*ch);
		for(int row=0;row<7;row++) for(int column=0;column<5;column++)
			if(rows[row]&(1<<(4-column)))
				for(int py=0;py<scale;py++) for(int px=0;px<scale;px++)
					PutDebugPixel(x+column*scale+px,y+row*scale+py,red,green,blue,255);
	}
}

// The profiler panel is a dense table, so everything below writes left aligned
// from a fixed margin and advances the caller's cursor. DrawDebugText centres on
// the x it is given, hence the half-width correction.
enum { DEBUG_TEXT_SCALE = 2, DEBUG_TEXT_ADVANCE = DEBUG_TEXT_SCALE*6,
       DEBUG_TEXT_LINE = 16, DEBUG_TEXT_MARGIN = 8,
       DEBUG_COLUMN_WIDTH = VR_DEBUG_WIDTH/2 };

// The panel is laid out in two columns; everything below writes into whichever
// one is current. One column could not hold the whole table, which left the
// streaming and texture sections off the bottom edge.
int gDebugColumnX = DEBUG_TEXT_MARGIN;

void DrawDebugTextLeft(const char *value,int *y,uint8 red,uint8 green,uint8 blue)
{
	const int centreX = gDebugColumnX +
		(int)strlen(value)*DEBUG_TEXT_ADVANCE/2;
	DrawDebugText(value,centreX,*y,DEBUG_TEXT_SCALE,red,green,blue);
	*y += DEBUG_TEXT_LINE;
}

void DebugSection(const char *title,int *y)
{
	*y += 5;
	char line[80];
	sprintf(line,"- %s",title);
	int pad = (int)strlen(line);
	while(pad < 40 && pad < (int)sizeof(line)-1) line[pad++] = '-';
	line[pad] = '\0';
	DrawDebugTextLeft(line,y,120,150,190);
}

void DebugPair(const char *leftName,float leftMs,const char *rightName,
	float rightMs,int *y)
{
	char line[80];
	sprintf(line,"%-9s %-8.2f %-9s %.2f",leftName,leftMs,rightName,rightMs);
	// Anything eating a large slice of a 13.9ms frame is worth spotting fast.
	const float worst = leftMs > rightMs ? leftMs : rightMs;
	if(worst >= 4.0f)      DrawDebugTextLeft(line,y,255,140,110);
	else if(worst >= 2.0f) DrawDebugTextLeft(line,y,240,220,130);
	else                   DrawDebugTextLeft(line,y,200,235,200);
}

void PutVrMenuPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_MENU_WIDTH || y >= VR_MENU_HEIGHT)
		return;
#ifdef RW_D3D12
	const int offset = (y*VR_MENU_WIDTH+x)*4;
#else
	const int offset = ((VR_MENU_HEIGHT-1-y)*VR_MENU_WIDTH+x)*4;
#endif
	gVrMenuPixels[offset+0] = red;
	gVrMenuPixels[offset+1] = green;
	gVrMenuPixels[offset+2] = blue;
	gVrMenuPixels[offset+3] = alpha;
}

void FillVrMenuRect(int left, int top, int right, int bottom,
	uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	left = Max(left, 0); top = Max(top, 0);
	right = Min(right, VR_MENU_WIDTH); bottom = Min(bottom, VR_MENU_HEIGHT);
	for(int y = top; y < bottom; y++)
		for(int x = left; x < right; x++)
			PutVrMenuPixel(x, y, red, green, blue, alpha);
}

void DrawVrMenuText(const char *value, int centreX, int y, int scale,
	uint8 red, uint8 green, uint8 blue)
{
	// All links use an accent, including nested and dynamically labelled pages.
	// Leave unavailable rows neutral; the OPEN marker is the non-colour cue.
	if(strstr(value, "< OPEN") != nil){
		static const struct { const char *name; uint8 colour[3]; } links[] = {
			{"LIGHTING", {255, 195, 100}},
			{"RAIN AND PUDDLES", {100, 195, 255}},
			{"REFLECTIONS", {100, 235, 190}},
			{"EFFECTS", {200, 155, 255}},
			{"DLSS 5 TUNING", {255, 140, 205}},
			{"CONTROL CALIBRATION", {255, 180, 115}},
			{"CONTROLS", {255, 170, 95}},
			{"PANEL PLACEMENT", {100, 225, 215}},
			{"MISSIONS", {185, 235, 110}},
			{"PLACE AND SIZE", {100, 225, 215}}
		};
		bool matched = false;
		for(const auto &link : links){
			if(strncmp(value, link.name, strlen(link.name)) == 0){
				red = link.colour[0]; green = link.colour[1]; blue = link.colour[2];
				matched = true;
				break;
			}
		}
		if(!matched && ((red == 205 && green == 215 && blue == 225) ||
		   (red == 255 && green == 245 && blue == 110))){
			static const uint8 palette[][3] = {
				{100, 205, 255}, {200, 160, 255}, {115, 235, 155},
				{255, 195, 100}, {255, 145, 190}, {100, 230, 215}
			};
			uint32 hash = 2166136261u;
			for(const char *ch = value; *ch && *ch != '<'; ch++) hash = (hash^(uint8)*ch)*16777619u;
			const uint8 *colour = palette[hash%ARRAY_SIZE(palette)];
			red = colour[0]; green = colour[1]; blue = colour[2];
		}
	}
	const int length = (int)strlen(value);
	if(length == 0)
		return;
	scale = Max(1, Min(scale, (VR_MENU_WIDTH-100)/(length*6)));
	const int advance = scale*6;
	int x = centreX-length*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++) for(int column = 0; column < 5; column++)
			if(rows[row] & (1 << (4-column)))
				for(int py = 0; py < scale; py++) for(int px = 0; px < scale; px++)
					PutVrMenuPixel(x+column*scale+px, y+row*scale+py,
						red, green, blue, 255);
	}
}

void ShowDlssProfileToast()
{
	// Hide the profiler and show the compact status strip for five seconds.
	gDebugVisible = false;
	gDlssProfileToastUntil = GetTickCount64()+5000ULL;
}

bool IsDlssProfileToastVisible()
{
	if(gDlssProfileToastUntil == 0)
		return false;
	if(GetTickCount64() < gDlssProfileToastUntil)
		return true;
	gDlssProfileToastUntil = 0;
	return false;
}

void DrawDlssProfileToast()
{
	char value[96];
	uint8 red = 120, green = 220, blue = 255;
	if(!Dlaa::IsNeuralRenderingEnabled()){
		strcpy(value, "DLSS5: DISABLED");
		red = 255; green = 150; blue = 120;
	}else if(!Dlaa::IsNeuralRenderingOutputVisible()){
		sprintf(value, "DLSS5 %dX %s A/B: BASELINE - NR BYPASSED",
			Dlaa::GetNeuralRenderingPassCount(),
			Dlaa::GetNeuralRenderingRegionModeName());
	}else{
		const char *state;
		if(Dlaa::HasNeuralRenderingFailed()){
			state = "ERROR";
			red = 255; green = 110; blue = 110;
		}else if(Dlaa::IsNeuralRenderingStereoActive()){
			state = "ACTIVE";
			red = 120; green = 255; blue = 150;
		}else{
			state = "PREPARING";
			red = 255; green = 230; blue = 100;
		}
		sprintf(value, "DLSS5 %dX %s A/B: %s%s - %s",
			Dlaa::GetNeuralRenderingPassCount(),
			Dlaa::GetNeuralRenderingRegionModeName(),
			Dlaa::GetNeuralRenderingModeName(),
			Dlaa::IsNeuralRenderingTuningCustom() ? " CUSTOM" : "", state);
	}
	FillVrMenuRect(45, 315, VR_MENU_WIDTH-45, 445, 5, 16, 26, 210);
	DrawVrMenuText(value, VR_MENU_WIDTH/2, 355, 5, red, green, blue);
}

void DrawVrControlsMenu()
{
	DrawVrMenuText("CONTROLS", VR_MENU_WIDTH/2, 116, 4, 255, 180, 105);
	DrawVrMenuText("ON FOOT ONLY - VEHICLES AND SERVICE CHORDS KEEP THEIR LAYOUT",
		VR_MENU_WIDTH/2, 150, 2, 180, 200, 220);
	char rows[VR_CONTROLS_ITEM_COUNT][112];
	const VrPadBindings::Layout layout = gVrPadBindings.GetLayout();
	sprintf(rows[VR_CONTROLS_LAYOUT], "LAYOUT  < %s >", layout == VrPadBindings::DEFAULT ?
		"DEFAULT" : layout == VrPadBindings::SWAPPED_HANDS ? "SWAPPED HANDS" : "CUSTOM");
	for(int source = 0; source < VrPadBindings::SOURCE_COUNT; source++)
		sprintf(rows[VR_CONTROLS_FIRST_SOURCE+source], "%s  < %s >",
			VrPadBindings::SourceName(source),
			gVrPadBindings.target[source] == VrPadBindings::FireTarget(CPad::GetPad(0)->GetMode()) ?
			"RESERVED - PHYSICAL TRIGGER" : VrPadBindings::TargetName(gVrPadBindings.target[source]));
	sprintf(rows[VR_CONTROLS_LOOK_BEHIND], "R3 VEHICLE LOOK BEHIND  < %s >", gStickLookBehind ? "ON" : "OFF");
	sprintf(rows[VR_CONTROLS_CROUCH], "DEFAULT L3 CROUCH  < %s >", gStickCrouch ? "ON" : "OFF");
	strcpy(rows[VR_CONTROLS_RESET], "RESET BINDINGS TO DEFAULTS");
	strcpy(rows[VR_CONTROLS_BACK], "BACK TO LOCOMOTION");
	for(int item = 0; item < VR_CONTROLS_ITEM_COUNT; item++){
		const int y = 184+item*35;
		const bool selected = item == gVrControlsMenuSelection;
		if(selected) FillVrMenuRect(85, y-7, VR_MENU_WIDTH-85, y+27, 25, 95, 135, 245);
		DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
			selected ? 255 : 205, selected ? 245 : 215, selected ? 110 : 225);
	}
	DrawVrMenuText("WEAPON TRIGGERS STAY PHYSICAL - A MAPPED ATTACK CANNOT DOUBLE-FIRE",
		VR_MENU_WIDTH/2, 670, 2, 180, 200, 220);
	DrawVrMenuText("LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

bool GetVrMainMenuCategoryColour(int item, bool selected,
	uint8 *red, uint8 *green, uint8 *blue)
{
	// Only rows that open a settings category get an accent. Ordinary toggles
	// retain the neutral menu colour, so category boundaries remain obvious.
	switch(item){
	case VR_MAIN_GRAPHICS:
		*red = 80; *green = 205; *blue = 255;
		break;
	case VR_MAIN_MODEL_SET:
		*red = 190; *green = 150; *blue = 255;
		break;
	case VR_MAIN_TRAFFIC_SETTINGS:
		*red = 105; *green = 225; *blue = 135;
		break;
	case VR_MAIN_HUD:
		*red = 255; *green = 205; *blue = 90;
		break;
	case VR_MAIN_VEHICLE_SETTINGS:
		*red = 255; *green = 140; *blue = 100;
		break;
	case VR_MAIN_LOCOMOTION_SETTINGS:
		*red = 165; *green = 145; *blue = 255;
		break;
	case VR_MAIN_CHEATS:
		*red = 255; *green = 120; *blue = 105;
		break;
	case VR_MAIN_CALIBRATION:
		*red = 255; *green = 115; *blue = 205;
		break;
	case VR_MAIN_HOLSTERS:
		*red = 80; *green = 225; *blue = 215;
		break;
	case VR_MAIN_ABOUT:
		*red = 145; *green = 215; *blue = 255;
		break;
	default:
		return false;
	}
	if(selected){
		*red = (uint8)Min((int)*red+35, 255);
		*green = (uint8)Min((int)*green+35, 255);
		*blue = (uint8)Min((int)*blue+35, 255);
	}
	return true;
}

RwImage *CreateVrWeaponIconImage(RwRaster *raster)
{
	if(raster == nil || raster->width <= 0 || raster->height <= 0)
		return nil;
#ifdef RW_D3D12
	// Weapon TXDs may retain their original BC compression.  The generic D3D12
	// Raster::toImage path expects BGRA8888, so decode only these tiny menu icons
	// locally rather than changing texture handling for the rest of the renderer.
	ID3D12Resource *resource = nil;
	if(rw::d3d12::getRasterResource(raster, &resource) && resource){
		const DXGI_FORMAT format = resource->GetDesc().Format;
		int dxt = 0;
		switch(format){
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB: dxt = 1; break;
		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB: dxt = 3; break;
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB: dxt = 5; break;
		default: break;
		}
		if(dxt != 0){
			RwImage *image = RwImageCreate(raster->width, raster->height, 32);
			if(image == nil || RwImageAllocatePixels(image) == nil){
				if(image) RwImageDestroy(image);
				return nil;
			}
			uint8 *blocks = raster->lock(0, rw::Raster::LOCKREAD);
			if(blocks == nil){
				RwImageDestroy(image);
				return nil;
			}
			image->setPixelsDXT(dxt, blocks);
			raster->unlock(0);
			return image;
		}
	}
#endif
	// All uncompressed textures produced by the modern backend are 32-bit.  The
	// stride guard keeps an unknown compressed backend format away from toImage.
	if(raster->depth != 32 || raster->stride < raster->width*4)
		return nil;
	return raster->toImage();
}

bool CacheVrWeaponIcon(int weaponType)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return false;
	VrWeaponIconCache &cache = gVrWeaponIconCache[weaponType];
	if(!cache.rgba.empty())
		return true;
	RwTexture *texture = CHud::GetWeaponIconTexture(weaponType);
	RwRaster *raster = texture ? RwTextureGetRaster(texture) : nil;
	RwImage *image = CreateVrWeaponIconImage(raster);
	if(image == nil)
		return false;
	if(image->depth != 32)
		image->convertTo32();
	if(image->pixels == nil || image->width <= 0 || image->height <= 0 ||
	   image->depth != 32){
		RwImageDestroy(image);
		return false;
	}
	cache.width = image->width;
	cache.height = image->height;
	cache.rgba.resize(cache.width*cache.height*4);
	for(int y = 0; y < cache.height; y++)
		memcpy(&cache.rgba[y*cache.width*4], image->pixels+y*image->stride,
			cache.width*4);
	RwImageDestroy(image);
	return true;
}

void BlendVrMenuPixel(int x, int y, const uint8 *source)
{
	if(source == nil || x < 0 || y < 0 || x >= VR_MENU_WIDTH ||
	   y >= VR_MENU_HEIGHT || source[3] == 0)
		return;
#ifdef RW_D3D12
	const int offset = (y*VR_MENU_WIDTH+x)*4;
#else
	const int offset = ((VR_MENU_HEIGHT-1-y)*VR_MENU_WIDTH+x)*4;
#endif
	const uint32 alpha = source[3];
	const uint32 inverseAlpha = 255-alpha;
	for(int channel = 0; channel < 3; channel++)
		gVrMenuPixels[offset+channel] = (uint8)(
			(source[channel]*alpha+gVrMenuPixels[offset+channel]*inverseAlpha+127)/255);
	gVrMenuPixels[offset+3] = (uint8)Min(255U,
		alpha+(gVrMenuPixels[offset+3]*inverseAlpha+127)/255);
}

void DrawVrWeaponIcon(int weaponType, int left, int centreY,
	int maximumWidth, int maximumHeight)
{
	if(!CacheVrWeaponIcon(weaponType))
		return;
	const VrWeaponIconCache &cache = gVrWeaponIconCache[weaponType];
	if(cache.width <= 0 || cache.height <= 0)
		return;
	const float scale = Min((float)maximumWidth/cache.width,
		(float)maximumHeight/cache.height);
	const int width = Max(1, (int)(cache.width*scale+0.5f));
	const int height = Max(1, (int)(cache.height*scale+0.5f));
	const int top = centreY-height/2;
	FillVrMenuRect(left-3, top-3, left+width+3, top+height+3,
		4, 10, 18, 210);
	for(int y = 0; y < height; y++){
		const int sourceY = Min(cache.height-1, y*cache.height/height);
		for(int x = 0; x < width; x++){
			const int sourceX = Min(cache.width-1, x*cache.width/width);
			BlendVrMenuPixel(left+x, top+y,
				&cache.rgba[(sourceY*cache.width+sourceX)*4]);
		}
	}
}

bool UpdateVrMenuSwapchain()
{
	ValidateHolsterCalibrationLifecycle();
	const bool showMenu =
		gVrMenuVisible || gCheatMenuVisible || gVrAboutVisible;
	const bool showDlssToast = IsDlssProfileToastVisible();
	if((!showMenu && !showDlssToast) ||
	   !gVrMenuSwapchain.handle || !AcquireSwapchain(gVrMenuSwapchain))
		return false;
	if(!showMenu){
		FillVrMenuRect(0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT, 0, 0, 0, 0);
		DrawDlssProfileToast();
	}else{
	if(gVrBikeCalibrationMenuVisible &&
	   !IsImmersiveDrivingActiveInternal())
		gVrBikeCalibrationMenuVisible = false;
	FillVrMenuRect(0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT, 5, 10, 20, 238);
	FillVrMenuRect(28, 28, VR_MENU_WIDTH-28, VR_MENU_HEIGHT-28, 10, 22, 38, 245);
	DrawVrMenuText("VICE CITY VR", VR_MENU_WIDTH/2, 55, 7, 255, 120, 205);
	if(gVrAboutVisible){
		DrawVrMenuText(gVrAboutFirstRun ? "WELCOME TO VICE CITY VR" : "ABOUT",
			VR_MENU_WIDTH/2, 112, 5, 100, 225, 255);
		DrawVrMenuText("VERSION V0.5.5 ALPHA PC", VR_MENU_WIDTH/2, 157, 3,
			255, 205, 110);

		FillVrMenuRect(75, 195, VR_MENU_WIDTH-75, 306,
			16, 48, 73, 235);
		DrawVrMenuText("OPEN VR MENU: HOLD BOTH GRIPS AND PRESS MENU",
			VR_MENU_WIDTH/2, 212, 2, 215, 235, 255);
		DrawVrMenuText("FALLBACK: BOTH GRIPS PLUS BOTH TRIGGERS PLUS X",
			VR_MENU_WIDTH/2, 246, 2, 170, 205, 235);
		DrawVrMenuText(Dlaa::IsNeuralRenderingEnabled() ?
			"DLSS 5 A/B: WITH EMPTY HANDS HOLD BOTH GRIPS AND PRESS B" :
			"CHEAT SHORTCUT: WITH EMPTY HANDS HOLD BOTH GRIPS AND PRESS B",
			VR_MENU_WIDTH/2, 280, 2, 255, 180, 225);
		DrawVrMenuText("CHEATS ARE ALWAYS AVAILABLE IN SETTINGS > CHEATS",
			VR_MENU_WIDTH/2, 310, 2, 255, 180, 225);

		DrawVrMenuText("CHOOSE IMMERSIVE DRIVING IN VEHICLE SETTINGS",
			VR_MENU_WIDTH/2, 340, 2, 255, 165, 115);
		DrawVrMenuText("CALIBRATE A WEAPON IF ITS ALIGNMENT HAS SHIFTED",
			VR_MENU_WIDTH/2, 374, 2, 255, 135, 205);

		DrawVrMenuText("THIS MOD IS STILL IN DEVELOPMENT",
			VR_MENU_WIDTH/2, 428, 2, 255, 205, 110);
		DrawVrMenuText("PLAYERS CONFIRM THE FULL STORY CAN BE COMPLETED",
			VR_MENU_WIDTH/2, 462, 2, 165, 235, 175);

		DrawVrMenuText("IF STEREO FLICKER OR GEOMETRY ARTIFACTS APPEAR",
			VR_MENU_WIDTH/2, 502, 2, 255, 155, 125);
		DrawVrMenuText("DISABLE VR MENU > GRAPHICS > OCCLUSION CULLING",
			VR_MENU_WIDTH/2, 532, 2, 255, 190, 130);

		DrawVrMenuText("JOIN FLAT2VR DISCORD", VR_MENU_WIDTH/2, 570, 3,
			125, 205, 255);
		DrawVrMenuText(
			"HTTPS://DISCORD.COM/CHANNELS/747967102895390741/1529621098751197365",
			VR_MENU_WIDTH/2, 606, 2, 205, 215, 230);
		DrawVrMenuText("ABOUT IS ALWAYS AVAILABLE IN THE VR MENU",
			VR_MENU_WIDTH/2, 642, 2, 170, 190, 210);
		FillVrMenuRect(245, 675, VR_MENU_WIDTH-245, 725,
			105, 42, 105, 245);
		DrawVrMenuText("PRESS ANY BUTTON TO CLOSE",
			VR_MENU_WIDTH/2, 689, 3, 255, 245, 110);
	}else if(gCheatMenuVisible){
#if defined(DEBUG) && !defined(FINAL)
		if(gVrMissionMenuVisible){
			DrawVrMenuText("CHEATS / MISSIONS", VR_MENU_WIDTH/2, 105, 4,
				100, 225, 255);
			if(gVrMissionCategory < 0){
				const int count = GetVrMissionCategoryCount();
				for(int item = 0; item < count; item++){
					const int y = 215+item*90;
					if(item == gVrMissionCategorySelection)
						FillVrMenuRect(75, y-15, VR_MENU_WIDTH-75, y+43,
							115, 42, 105, 245);
					char row[112];
					sprintf(row, "%s  < OPEN >",
						GetVrMissionCategoryName(item));
					DrawVrMenuText(row, VR_MENU_WIDTH/2, y, 3,
						item == gVrMissionCategorySelection ? 255 : 205,
						item == gVrMissionCategorySelection ? 245 : 215,
						item == gVrMissionCategorySelection ? 110 : 225);
				}
				DrawVrMenuText("MISSION NUMBERS COME FROM THE ACTIVE MAIN.SCM TABLE",
					VR_MENU_WIDTH/2, 650, 2, 255, 180, 225);
				DrawVrMenuText("UP/DOWN SELECT   A OPEN   B BACK TO CHEATS",
					VR_MENU_WIDTH/2, 724, 2, 170, 190, 210);
			}else{
				DrawVrMenuText(GetVrMissionCategoryName(gVrMissionCategory),
					VR_MENU_WIDTH/2, 129, 2, 255, 180, 225);
				const int count = GetVrMissionCount(gVrMissionCategory);
				const int itemsPerPage = 14;
				const int first =
					(gVrMissionMenuSelection/itemsPerPage)*itemsPerPage;
				for(int row = 0; row < itemsPerPage && first+row < count; row++){
					const int item = first+row;
					const int y = 154+row*34;
					if(item == gVrMissionMenuSelection)
						FillVrMenuRect(65, y-7, VR_MENU_WIDTH-65, y+24,
							115, 42, 105, 245);
					DrawVrMenuText(GetVrMissionName(gVrMissionCategory, item),
						VR_MENU_WIDTH/2, y, 2,
						item == gVrMissionMenuSelection ? 255 : 205,
						item == gVrMissionMenuSelection ? 245 : 215,
						item == gVrMissionMenuSelection ? 110 : 225);
				}
				char page[48];
				sprintf(page, "PAGE %d OF %d", first/itemsPerPage+1,
					(count+itemsPerPage-1)/itemsPerPage);
				DrawVrMenuText(page, VR_MENU_WIDTH/2, 650, 2,
					120, 220, 255);
				DrawVrMenuText("A STARTS THE SELECTED MISSION IN THE CURRENT SAVE STATE",
					VR_MENU_WIDTH/2, 684, 2, 255, 180, 225);
				DrawVrMenuText("UP/DOWN SELECT   A START   B CATEGORIES",
					VR_MENU_WIDTH/2, 724, 2, 170, 190, 210);
			}
		}else
#endif
		{
			DrawVrMenuText("CHEATS", VR_MENU_WIDTH/2, 105, 4, 100, 225, 255);
			const int count = GetOpenXrCheatMenuCount();
			const int itemsPerPage = 14;
			const int first = (gCheatMenuSelection/itemsPerPage)*itemsPerPage;
			for(int row = 0; row < itemsPerPage && first+row < count; row++){
				const int item = first+row;
				const int y = 130+row*37;
				if(item == gCheatMenuSelection)
					FillVrMenuRect(65, y-8, VR_MENU_WIDTH-65, y+27,
						115, 42, 105, 245);
				DrawVrMenuText(GetOpenXrCheatMenuName(item), VR_MENU_WIDTH/2, y, 2,
					item == gCheatMenuSelection ? 255 : 205,
					item == gCheatMenuSelection ? 245 : 215,
					item == gCheatMenuSelection ? 110 : 225);
			}
			char page[48];
			sprintf(page, "PAGE %d OF %d", first/itemsPerPage+1,
				(count+itemsPerPage-1)/itemsPerPage);
			DrawVrMenuText(page, VR_MENU_WIDTH/2, 663, 2, 120, 220, 255);
#if defined(DEBUG) && !defined(FINAL)
			if(gHolsterCalibrationCheat.active){
				char preview[112];
				if(gHolsterCalibrationCheat.previewWeaponType >= 0)
					sprintf(preview, "TESTING %s AT %s - LOADOUT SNAPSHOTTED",
						GetVrWeaponName(
							gHolsterCalibrationCheat.previewWeaponType),
						gHolsterPointNames[
							gHolsterCalibrationCheat.selectedPoint]);
				else
					strcpy(preview, "LOADOUT SNAPSHOTTED - SELECT NEXT WEAPON");
				DrawVrMenuText(preview, VR_MENU_WIDTH/2, 690, 2,
					255, 180, 225);
			}
			DrawVrMenuText(gCheatMenuOpenedFromVrMenu ?
				"UP/DOWN SELECT   LEFT/RIGHT MODEL OR TARGET   A ACTIVATE   B BACK" :
				"UP/DOWN SELECT   LEFT/RIGHT MODEL OR TARGET   A ACTIVATE   B CLOSE",
				VR_MENU_WIDTH/2, 724, 2,
				170, 190, 210);
#else
			DrawVrMenuText(gCheatMenuOpenedFromVrMenu ?
				"UP/DOWN SELECT   LEFT/RIGHT OPTION   A ACTIVATE   B BACK" :
				"UP/DOWN SELECT   LEFT/RIGHT OPTION   A ACTIVATE   B CLOSE",
				VR_MENU_WIDTH/2, 724, 2, 170, 190, 210);
#endif
		}
	}else if(gVrModelMenuVisible){
		DrawVrMenuText("MODEL ASSETS", VR_MENU_WIDTH/2, 112, 5,
			190, 150, 255);
		DrawVrMenuText("MIX CLASSIC AND MODERN ASSETS - RESTART REQUIRED",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_MODEL_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_MODEL_PRESET], "RECOMMENDED PRESET  < %s >",
			ModelSets::GetName(ModelSets::GetRequested()));
		const ModelSets::eModelCategory categories[] = {
			ModelSets::MODEL_CATEGORY_WORLD,
			ModelSets::MODEL_CATEGORY_VEGETATION,
			ModelSets::MODEL_CATEGORY_VEHICLES,
			ModelSets::MODEL_CATEGORY_PEDS,
			ModelSets::MODEL_CATEGORY_WEAPONS,
		};
		for(int row = VR_MODEL_WORLD; row <= VR_MODEL_WEAPONS; row++){
			const ModelSets::eModelCategory category =
				categories[row-VR_MODEL_WORLD];
			if(!ModelSets::IsCategoryAvailable(category))
				sprintf(rows[row], "%s  < UNAVAILABLE >",
					ModelSets::GetCategoryName(category));
			else
				sprintf(rows[row], "%s  < %s%s >",
					ModelSets::GetCategoryName(category),
					ModelSets::GetName(
						ModelSets::GetRequestedForCategory(category)),
					ModelSets::IsCategoryRestartRequired(category) ?
						" - RESTART" : "");
		}
		strcpy(rows[VR_MODEL_BACK], "BACK TO SETTINGS");
		for(int item = 0; item < VR_MODEL_MENU_ITEM_COUNT; item++){
			const int y = 188+item*64;
			if(item == gVrModelMenuSelection)
				FillVrMenuRect(70, y-15, VR_MENU_WIDTH-70, y+43,
					65, 45, 125, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrModelMenuSelection ? 255 : 215,
				item == gVrModelMenuSelection ? 245 : 205,
				item == gVrModelMenuSelection ? 115 : 235);
		}
		if(!ModelSets::HasVegetationManifest())
			DrawVrMenuText("VEGETATION MANIFEST MISSING - SAFE CLASSIC FALLBACK",
				VR_MENU_WIDTH/2, 658, 2, 255, 170, 190);
		else
			DrawVrMenuText("TRY CLASSIC VEGETATION WITH MODERN VEHICLES AND WEAPONS",
				VR_MENU_WIDTH/2, 658, 2, 145, 235, 165);
		DrawVrMenuText("UP/DOWN SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrGraphicsMenuVisible){
		DrawVrMenuText("GRAPHICS SETTINGS", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("RENDERING QUALITY AND ANTI-ALIASING",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_GRAPHICS_MENU_ITEM_COUNT][112];
		const int requestedWidth = gEye[0].swapchain.width > 0 ?
			(int)(gEye[0].swapchain.width*
			gRenderScaleOptions[gRenderScaleIndex]+0.5f) : 0;
		const int requestedHeight = gEye[0].swapchain.height > 0 ?
			(int)(gEye[0].swapchain.height*
			gRenderScaleOptions[gRenderScaleIndex]+0.5f) : 0;
		sprintf(rows[VR_GRAPHICS_RENDER_SCALE],
			"RENDER SCALE  < %d%% >",
			(int)(gRenderScaleOptions[gRenderScaleIndex]*100.0f+0.5f));
		sprintf(rows[VR_GRAPHICS_VRS], "VRS  < %s >",
			FoveatedProfileName());
		const bool temporalError =
			(gTemporalAaBackend == TEMPORAL_AA_DLAA &&
			 gDlaaStereoActivationFailed) ||
			(gTemporalAaBackend == TEMPORAL_AA_FSR2 &&
			 (gFsr2StereoActivationFailed || !Fsr2::IsSupported()));
		sprintf(rows[VR_GRAPHICS_TEMPORAL_AA],
			"TEMPORAL AA  < %s%s >", TemporalAaBackendName(),
			temporalError ? " ERROR" : "");
		{
		static const char *dlssModeNames[4] =
			{ "DLAA", "QUALITY", "BALANCED", "PERFORMANCE" };
		sprintf(rows[VR_GRAPHICS_DLSS_MODE], "DLSS MODE  < %s >",
			dlssModeNames[gDlssQualityMode&3]);
		}
		{
		const char *compareName =
			Dlaa::GetNeuralRenderingModeName(gDlssComparisonProfile);
		if(Dlaa::IsNeuralRenderingEnabled())
			sprintf(rows[VR_GRAPHICS_DLSS_NR_ENABLE],
				"NEURAL DLSS 5  < ON - %s >", compareName);
		else
			sprintf(rows[VR_GRAPHICS_DLSS_NR_ENABLE],
				"NEURAL DLSS 5  < OFF - %s >",
				gDlssQualityMode == 0 ? "PURE DLAA" : "STANDARD DLSS");
		if(!Dlaa::IsNeuralRenderingEnabled())
			strcpy(rows[VR_GRAPHICS_DLSS_NR], "DLSS 5 A/B  < DISABLED >");
		else if(!Dlaa::IsNeuralRenderingOutputVisible())
			sprintf(rows[VR_GRAPHICS_DLSS_NR],
				"DLSS 5 %dX A/B  < BASELINE / %s >",
				Dlaa::GetNeuralRenderingPassCount(), compareName);
		else{
			const char *nrState = Dlaa::HasNeuralRenderingFailed() ?
				"ERROR" : (Dlaa::IsNeuralRenderingStereoActive() ?
				"ACTIVE" : "PREPARING");
				sprintf(rows[VR_GRAPHICS_DLSS_NR],
					"DLSS 5 %dX A/B  < %s - %s >",
					Dlaa::GetNeuralRenderingPassCount(), compareName, nrState);
		}
		}
		sprintf(rows[VR_GRAPHICS_DLSS_NR_PASSES],
			"DLSS 5 PASSES  < %dX%s >",
			Dlaa::GetNeuralRenderingPassCount(),
			Dlaa::GetNeuralRenderingPassCount() > 1 ? " - EXPERIMENTAL" : "");
		sprintf(rows[VR_GRAPHICS_DLSS_NR_REGION],
			"DLSS 5 WORK SCALE  < %s >",
			Dlaa::GetNeuralRenderingRegionModeName());
		sprintf(rows[VR_GRAPHICS_DLSS_NR_TUNING],
			"DLSS 5 TUNING  < OPEN%s >",
			Dlaa::IsNeuralRenderingTuningCustom() ? " - CUSTOM" : "");
#ifdef RW_D3D12
		sprintf(rows[VR_GRAPHICS_EFFECTS], "EFFECTS  < OPEN - %s%s >",
			gEffectsEnabled ? "ON" : "OFF",
			gEffectsEnabled && gScreenSpaceReflectionFailed ? " ERROR" : "");
#else
		strcpy(rows[VR_GRAPHICS_EFFECTS], "EFFECTS  < D3D12 ONLY >");
#endif
		sprintf(rows[VR_GRAPHICS_JITTER], "TEMPORAL JITTER  < %s >",
			TemporalJitterModeName());
		sprintf(rows[VR_GRAPHICS_FXAA], "FXAA FALLBACK  < %s >",
			gAntiAliasingEnabled ? "ON" : "OFF");
		sprintf(rows[VR_GRAPHICS_COLOR], "COLOR FILTER  < %s >",
			gLightingEnabled ? "ON" : "OFF");
		sprintf(rows[VR_GRAPHICS_SUN_GLARE], "SUN GLARE  < %d%% >",
			gSunGlarePercent);
		sprintf(rows[VR_GRAPHICS_OCCLUSION], "OCCLUSION CULLING  < %s >",
			CRenderer::IsVrOcclusionCullingEnabled() ? "ON" : "OFF");
		sprintf(rows[VR_GRAPHICS_MIPMAPS], "GENERATE MIPMAPS  < %s%s >",
			gGenerateMipmapsRequested ? "ON" : "OFF",
			gGenerateMipmapsRequested != gGenerateMipmapsAtStartup ? " - RESTART" : "");
		static const char *foliageNames[] = { "ORIGINAL", "+0.5 MIP", "+1.0 MIP", "+1.5 MIP" };
		sprintf(rows[VR_GRAPHICS_FOLIAGE_SOFTNESS], "FOLIAGE SOFTNESS  < %s >", foliageNames[gFoliageSoftness]);
		strcpy(rows[VR_GRAPHICS_BACK], "BACK TO SETTINGS");
		const int visibleRows = 14;
		const int firstRow = Min(Max(gVrGraphicsMenuSelection-visibleRows/2, 0), VR_GRAPHICS_MENU_ITEM_COUNT-visibleRows);
		for(int item = firstRow; item < Min(firstRow+visibleRows, VR_GRAPHICS_MENU_ITEM_COUNT); item++){
			const int y = 160+(item-firstRow)*29;
			if(item == gVrGraphicsMenuSelection)
				FillVrMenuRect(85, y-6, VR_MENU_WIDTH-85, y+25,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrGraphicsMenuSelection ? 255 : 205,
				item == gVrGraphicsMenuSelection ? 245 : 215,
				item == gVrGraphicsMenuSelection ? 110 : 225);
		}
		char resolution[96];
		// Above-native scales quietly break weak machines: frame misses in
		// cutscenes, runtime demotions, TDR at effect spikes. Make the cost
		// visible right where the player raises it.
		const bool heavyScale =
			gRenderScaleOptions[gRenderScaleIndex] > 1.001f;
		sprintf(resolution, "PER EYE  %d X %d%s", requestedWidth,
			requestedHeight, heavyScale ? "  -  HEAVY GPU LOAD" : "");
		if(heavyScale)
			DrawVrMenuText(resolution, VR_MENU_WIDTH/2, 598, 3,
				255, 170, 90);
		else
			DrawVrMenuText(resolution, VR_MENU_WIDTH/2, 598, 3,
				120, 220, 255);
		char vrsStatus[96] = "VRS NOT SUPPORTED";
#ifdef RW_D3D12
		rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
		rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
		if(foveatedInfo.supported){
			if(foveatedInfo.profile == rw::d3d12::FIXED_FOVEATED_OFF)
				sprintf(vrsStatus, "VRS TIER %u READY - OFF",
					foveatedInfo.tier);
			else
				sprintf(vrsStatus, "VRS TIER %u ACTIVE   TILE %u",
					foveatedInfo.tier, foveatedInfo.tileSize);
		}
#endif
		DrawVrMenuText(vrsStatus, VR_MENU_WIDTH/2, 666, 3,
			125, 255, 145);
		const char *temporalStatus = "TEMPORAL AA DISABLED";
		if(gTemporalAaBackend == TEMPORAL_AA_DLAA)
			temporalStatus = !Dlaa::IsSupported() ?
				"DLAA UNAVAILABLE - FXAA" :
				(gDlaaStereoActivationFailed ?
				"DLAA INIT FAILED - FXAA" :
				(!gDlaaStereoActivationReady ? "DLAA PREPARING" :
				(Dlaa::WasLastEvaluationSuccessful() ?
				"DLAA ACTIVE" : "DLAA READY")));
		else if(gTemporalAaBackend == TEMPORAL_AA_FSR2)
			temporalStatus = gFsr2StereoActivationFailed ?
				"FSR2 INIT FAILED - FXAA" :
				(!Fsr2::IsSupported() ? Fsr2::GetStatus() :
				(Fsr2::WasLastEvaluationSuccessful() ?
					"FSR2 NATIVE AA ACTIVE" :
					"FSR2 NATIVE AA READY"));
		DrawVrMenuText(temporalStatus, VR_MENU_WIDTH/2, 636, 2,
			255, 180, 225);
		DrawVrMenuText(
			"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrDlssTuningMenuVisible){
		DrawVrMenuText("DLSS 5 - MODEL TUNING", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		char subtitle[112];
		sprintf(subtitle, "%s%s - LIVE MODEL PARAMETERS",
			Dlaa::GetNeuralRenderingModeName(),
			Dlaa::IsNeuralRenderingTuningCustom() ? " CUSTOM" : " PROFILE");
		DrawVrMenuText(subtitle, VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_DLSS_TUNING_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_DLSS_TUNING_INTENSITY], "MODEL INTENSITY  < %d%% >",
			Dlaa::GetNeuralRenderingIntensityPercent());
		sprintf(rows[VR_DLSS_TUNING_STRUCTURE], "LOCAL STRUCTURE  < %d%% >",
			Dlaa::GetNeuralRenderingStructurePercent());
		sprintf(rows[VR_DLSS_TUNING_LOCAL_TONE], "LOCAL TONE  < %d%% >",
			Dlaa::GetNeuralRenderingLocalTonePercent());
		sprintf(rows[VR_DLSS_TUNING_GLOBAL_TONE], "GLOBAL TONE  < %d%% >",
			Dlaa::GetNeuralRenderingGlobalTonePercent());
		sprintf(rows[VR_DLSS_TUNING_STYLE], "MODEL STYLE  < %d >",
			Dlaa::GetNeuralRenderingStyle());
		sprintf(rows[VR_DLSS_TUNING_AUTO_MASK], "AUTO MASK  < %s >",
			Dlaa::IsNeuralRenderingAutoMaskEnabled() ? "ON" : "OFF");
		strcpy(rows[VR_DLSS_TUNING_RESET], "RESET TO CURRENT PROFILE");
		strcpy(rows[VR_DLSS_TUNING_BACK], "BACK TO GRAPHICS");
		for(int item = 0; item < VR_DLSS_TUNING_MENU_ITEM_COUNT; item++){
			const int y = 178+item*52;
			if(item == gVrDlssTuningMenuSelection)
				FillVrMenuRect(85, y-11, VR_MENU_WIDTH-85, y+35,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrDlssTuningMenuSelection ? 255 : 205,
				item == gVrDlssTuningMenuSelection ? 245 : 215,
				item == gVrDlssTuningMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("TUNING APPLIES TO EVERY SELECTED MODEL PASS",
			VR_MENU_WIDTH/2, 630, 2, 120, 255, 150);
		DrawVrMenuText("AUTO MASK OFF MAY AFFECT THE WHOLE FRAME",
			VR_MENU_WIDTH/2, 658, 2, 255, 180, 120);
		DrawVrMenuText(
			"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrEffectsMenuVisible){
		DrawVrMenuText("EFFECTS", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("OPTIONAL DX12 LIGHTING AND WEATHER",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_EFFECTS_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_EFFECTS_MASTER], "DX12 EXTRA EFFECTS  < %s >",
			gEffectsEnabled ? "ON" : "OFF");
#ifdef RW_D3D12
		sprintf(rows[VR_EFFECTS_LIGHTING_MENU], "LIGHTING  < OPEN - %s >",
			gDynamicLights ? "ON" : "OFF");
		strcpy(rows[VR_EFFECTS_RAIN_MENU], "RAIN AND PUDDLES  < OPEN >");
		strcpy(rows[VR_EFFECTS_REFLECTION_MENU], "REFLECTIONS  < OPEN >");
#else
		strcpy(rows[VR_EFFECTS_LIGHTING_MENU], "LIGHTING  < D3D12 ONLY >");
		strcpy(rows[VR_EFFECTS_RAIN_MENU], "RAIN AND PUDDLES  < D3D12 ONLY >");
		strcpy(rows[VR_EFFECTS_REFLECTION_MENU], "REFLECTIONS  < D3D12 ONLY >");
#endif
		strcpy(rows[VR_EFFECTS_BACK], "BACK TO GRAPHICS");
		sprintf(rows[VR_EFFECTS_FOUNTAINS], "FOUNTAINS  < %s >", CParticleObject::GetVrFountainQualityName());
		for(int item = 0; item < VR_EFFECTS_MENU_ITEM_COUNT; item++){
			const int y = 178+item*54;
			if(item == gVrEffectsMenuSelection)
				FillVrMenuRect(85, y-11, VR_MENU_WIDTH-85, y+35,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrEffectsMenuSelection ? 255 : 205,
				item == gVrEffectsMenuSelection ? 245 : 215,
				item == gVrEffectsMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LIGHTING, RAIN AND REFLECTIONS HAVE SEPARATE CONTROLS",
			VR_MENU_WIDTH/2, 630, 2, 170, 190, 210);
		DrawVrMenuText("EXTRA EFFECTS OFF USES THE ORIGINAL RENDER PATH",
			VR_MENU_WIDTH/2, 658, 2, 170, 190, 210);
		DrawVrMenuText(
			"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrLightingMenuVisible){
		DrawVrMenuText("LIGHTING", VR_MENU_WIDTH/2, 112, 5,
			255, 195, 100);
		DrawVrMenuText(gEffectsEnabled ? "DYNAMIC LIGHTS PLUS ORIGINAL WORLD SETTINGS" :
			"EXTRA LIGHTS PAUSED - ORIGINAL SHADOWS AND FOG ARE SEPARATE",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_LIGHTING_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_LIGHTING_MODE], "DYNAMIC LIGHTS  < %s >",
			gDynamicLights >= 2 ? "ON + AIR GLOW" :
			(gDynamicLights == 1 ? "ON" : "OFF"));
		sprintf(rows[VR_LIGHTING_INTENSITY], "LIGHT INTENSITY  < %d%% >",
			gDynamicLightIntensityPercent);
		sprintf(rows[VR_LIGHTING_GLOW], "AIR GLOW INTENSITY  < %d%% >",
			gDynamicLightGlowPercent);
		sprintf(rows[VR_LIGHTING_MAX_LIGHTS], "MAX LIGHTS  < %d >", gDynamicLightMax);
		sprintf(rows[VR_LIGHTING_SHADOWS], "ORIGINAL SHADOWS  < %s >", CShadows::IsRenderEnabled() ? "ON" : "OFF");
		sprintf(rows[VR_LIGHTING_DISTANCE_FOG], "DISTANCE FOG  < %s >", gDistanceFogEnabled ? "ON" : "OFF");
		strcpy(rows[VR_LIGHTING_BACK], "BACK TO EFFECTS");
		for(int item = 0; item < VR_LIGHTING_MENU_ITEM_COUNT; item++){
			const int y = 180+item*57;
			if(item == gVrLightingMenuSelection)
				FillVrMenuRect(70, y-13, VR_MENU_WIDTH-70, y+39, 95, 65, 25, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrLightingMenuSelection ? 255 : 205,
				item == gVrLightingMenuSelection ? 245 : 215,
				item == gVrLightingMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("MORE LIGHTS AND AIR GLOW INCREASE GPU COST",
			VR_MENU_WIDTH/2, 630, 2, 255, 190, 120);
		DrawVrMenuText("HOLD STICK TO SCROLL   HOLD TRIGGERS TO ADJUST   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrRainMenuVisible){
		DrawVrMenuText("RAIN AND PUDDLES", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("DROP VOLUME AND GROUND RESPONSE ARE INDEPENDENT",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_RAIN_MENU_ITEM_COUNT][112];
		static const char *rainModeNames[4] = {
			"OFF", "CLASSIC", "MODERN", "CINEMATIC"
		};
		static const char *rainSurfaceNames[4] = {
			"OFF", "WET", "PUDDLES", "CINEMATIC"
		};
		sprintf(rows[VR_RAIN_MODE], "RAIN RENDERER  < %s >",
			rainModeNames[CWeather::RainGraphicsMode]);
		sprintf(rows[VR_RAIN_INTENSITY], "DROP VISIBILITY  < %d%% >",
			CWeather::RainGraphicsIntensity);
		sprintf(rows[VR_RAIN_DENSITY], "DROP DENSITY  < %d%% >",
			CWeather::RainGraphicsDensity);
		sprintf(rows[VR_RAIN_SURFACES], "GROUND SURFACES  < %s >",
			rainSurfaceNames[CWeather::RainSurfaceMode]);
		sprintf(rows[VR_RAIN_PUDDLE_COVERAGE], "PUDDLE COVERAGE  < %d%% >",
			gPuddleCoveragePercent);
		sprintf(rows[VR_RAIN_PUDDLE_EDGE], "PUDDLE EDGE SOFTNESS  < %d%% >",
			gPuddleEdgeSoftnessPercent);
		sprintf(rows[VR_RAIN_PUDDLE_RIPPLE], "PUDDLE RIPPLE  < %d%% >",
			gPuddleRipplePercent);
		strcpy(rows[VR_RAIN_RESET], "APPLY RECOMMENDED RAIN PRESET");
		strcpy(rows[VR_RAIN_BACK], "BACK TO EFFECTS");
		for(int item = 0; item < VR_RAIN_MENU_ITEM_COUNT; item++){
			const int y = 170+item*48;
			if(item == gVrRainMenuSelection)
				FillVrMenuRect(70, y-10, VR_MENU_WIDTH-70, y+33,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrRainMenuSelection ? 255 : 205,
				item == gVrRainMenuSelection ? 245 : 215,
				item == gVrRainMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("PUDDLE MASK NOW FOLLOWS SURFACE HEIGHT AND FEATHERS EDGES",
			VR_MENU_WIDTH/2, 630, 2, 120, 255, 150);
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrReflectionMenuVisible){
		DrawVrMenuText(gVrReflectionPage == 1 ? "CAR REFLECTIONS" :
			gVrReflectionPage == 2 ? "OCEAN SURFACE" : "REFLECTIONS", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("CARS, OCEAN AND PUDDLES USE SEPARATE STRENGTHS",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_WATER_ITEM_COUNT][112];
		const int rowCount = gVrReflectionPage == 1 ? VR_CAR_REFLECTION_ITEM_COUNT :
			gVrReflectionPage == 2 ? VR_WATER_ITEM_COUNT : VR_REFLECTION_MENU_ITEM_COUNT;
		if(gVrReflectionPage == 1){
			sprintf(rows[VR_CAR_REFLECTION_INTENSITY], "CAR REFLECTION INTENSITY  < %d%% >", gCarReflectionPercent);
			sprintf(rows[VR_CAR_REFLECTION_SSR], "SCREEN-SPACE REFLECTION MIX  < %d%% >", gCarReflectionSsrPercent);
			if(gCarReflectionSsrDistance == 0)
				strcpy(rows[VR_CAR_REFLECTION_DISTANCE], "SCREEN-SPACE MAX DISTANCE  < UNLIMITED >");
			else
				sprintf(rows[VR_CAR_REFLECTION_DISTANCE], "SCREEN-SPACE MAX DISTANCE  < %d M >", gCarReflectionSsrDistance);
			strcpy(rows[VR_CAR_REFLECTION_BACK], "BACK TO REFLECTIONS");
		}else if(gVrReflectionPage == 2){
			sprintf(rows[VR_WATER_ENABLE], "OCEAN EFFECTS  < %s >", gModernWaterEnabled ? "ON" : "OFF");
			sprintf(rows[VR_WATER_REFLECTION], "OCEAN REFLECTION INTENSITY  < %d%% >", gWaterReflectionPercent);
			sprintf(rows[VR_WATER_WAVES], "WAVE AMPLITUDE  < %d%% >", gOceanWavePercent);
			sprintf(rows[VR_WATER_SPEED], "WAVE SPEED  < %d%% >", gWaterSpeedPercent);
			sprintf(rows[VR_WATER_DISTORTION], "REFLECTION DISTORTION  < %d%% >", gWaterDistortionPercent);
			sprintf(rows[VR_WATER_SHEEN], "SKY SHEEN  < %d%% >", gWaterSheenPercent);
			sprintf(rows[VR_WATER_GLINT], "SUN GLINT  < %d%% >", gWaterGlintPercent);
			sprintf(rows[VR_WATER_SPARKS], "DYNAMIC LIGHT SPARKS  < %d%% >", gWaterSparksPercent);
			strcpy(rows[VR_WATER_BACK], "BACK TO REFLECTIONS");
		}else{
			sprintf(rows[VR_REFLECTION_CARS], "CAR REFLECTIONS  < OPEN - %d%% >", gCarReflectionPercent);
			sprintf(rows[VR_REFLECTION_OCEAN], "OCEAN SURFACE  < OPEN - %s >", gModernWaterEnabled ? "ON" : "OFF");
		if(gPuddleReflectionPercent == 0)
			strcpy(rows[VR_REFLECTION_PUDDLES], "PUDDLE REFLECTIONS  < OFF >");
		else
			sprintf(rows[VR_REFLECTION_PUDDLES], "PUDDLE REFLECTIONS  < %d%% >",
				gPuddleReflectionPercent);
		strcpy(rows[VR_REFLECTION_RESET], "RESTORE REFLECTION DEFAULTS");
		strcpy(rows[VR_REFLECTION_BACK], "BACK TO EFFECTS");
		}
		for(int item = 0; item < rowCount; item++){
			const int y = 185+item*(gVrReflectionPage == 2 ? 49 : 66);
			if(item == gVrReflectionMenuSelection)
				FillVrMenuRect(70, y-13, VR_MENU_WIDTH-70, y+39,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrReflectionMenuSelection ? 255 : 205,
				item == gVrReflectionMenuSelection ? 245 : 215,
				item == gVrReflectionMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("OCEAN WAVES DO NOT AFFECT PUDDLE RIPPLE",
			VR_MENU_WIDTH/2, 630, 2, 120, 255, 150);
		DrawVrMenuText(gVrReflectionPage == 2 ? "SPARKS REQUIRE DYNAMIC LIGHTS - OCEAN EFFECTS OFF DISABLES THIS PAGE" :
			(gEffectsEnabled && gScreenSpaceReflectionFailed ? "SCREEN-SPACE HISTORY ERROR" :
			"CAR, OCEAN AND PUDDLE REFLECTIONS ARE INDEPENDENT"), VR_MENU_WIDTH/2, 663, 2, 170, 195, 215);
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrTrafficMenuVisible){
		DrawVrMenuText("TRAFFIC SETTINGS", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText(
			"LIVE DENSITY - SAFE ENTITIES CONVERGE WITHOUT HARD DELETION",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_TRAFFIC_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_TRAFFIC_PEDESTRIANS],
			"PEDESTRIANS  < %d%% >", gTrafficPedPercent);
		sprintf(rows[VR_TRAFFIC_VEHICLES],
			"VEHICLES  < %d%% >", gTrafficCarPercent);
		sprintf(rows[VR_TRAFFIC_RAGDOLLS], "RAGDOLLS  < %s >", VrRagdoll::IsEnabled() ? "ON" : "OFF");
		strcpy(rows[VR_TRAFFIC_DEFAULTS], "RESTORE DEFAULTS  < 135% >");
		strcpy(rows[VR_TRAFFIC_BACK], "BACK TO SETTINGS");
		for(int item = 0; item < VR_TRAFFIC_MENU_ITEM_COUNT; item++){
			const int y = 185+item*64;
			if(item == gVrTrafficMenuSelection)
				FillVrMenuRect(85, y-15, VR_MENU_WIDTH-85, y+43,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrTrafficMenuSelection ? 255 : 205,
				item == gVrTrafficMenuSelection ? 245 : 215,
				item == gVrTrafficMenuSelection ? 110 : 225);
		}
		char status[112];
		sprintf(status, "WALKERS %u / %.1f   CAP %d",
			CPopulation::ms_nTotalPeds, CPopulation::VrTargetAmbientPeds,
			CGame::IsInInterior() ?
				CPopulation::MaxNumberOfPedsInUseInterior :
				CPopulation::MaxNumberOfPedsInUse);
		DrawVrMenuText(status, VR_MENU_WIDTH/2, 510, 3,
			125, 255, 145);
		sprintf(status, "CARS %d + %d PROXY   LOCAL %.1f / %.1f",
			CCarCtrl::NumVrEffectiveAmbient,
			CCarCtrl::NumVrActiveProxies,
			CCarCtrl::VrLocalServed, CCarCtrl::VrLocalDesired);
		DrawVrMenuText(status, VR_MENU_WIDTH/2, 555, 3,
			125, 255, 145);
		DrawVrMenuText("RANGES 50-300%   STEP 5%",
			VR_MENU_WIDTH/2, 635, 2, 255, 180, 225);
		DrawVrMenuText(
			"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrBikeCalibrationMenuVisible){
		CVehicle *vehicle = FindPlayerVehicle();
		const bool bike = vehicle && vehicle->IsBike();
		BikeHandleCalibration *calibration = bike ?
			GetBikeHandleCalibration(vehicle->GetModelIndex(),
				gBikeCalibrationEditHand) : nil;
		BikeLeanCalibration *leanCalibration = bike ?
			GetBikeLeanCalibration(vehicle->GetModelIndex()) : nil;
		VehicleCategoryCalibration *categoryCalibration =
			GetVehicleCategoryCalibration(vehicle);
		VehicleViewCalibration *viewCalibration = vehicle ?
			GetVehicleViewCalibration(vehicle->GetModelIndex()) : nil;
		char heading[112];
		sprintf(heading, "VEHICLE CONTROLS - %s",
			GetActiveVrVehicleName());
		DrawVrMenuText(heading, VR_MENU_WIDTH/2, 108, 4,
			100, 225, 255);
		DrawVrMenuText(bike ?
			"HAND VALUES ARE PER MODEL - CENTER HAS GLOBAL AND MODEL LAYERS" :
			"GRIP POINTS FOLLOW THE WHEEL AT 9 AND 3 - RADIUS IS AUTHORITATIVE",
			VR_MENU_WIDTH/2, 143, 2, 170, 190, 210);
		char rows[VR_BIKE_CALIBRATION_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_BIKE_CAL_HAND], "EDIT HANDLE  < %s >",
			gBikeCalibrationEditHand == 0 ? "LEFT" : "RIGHT");
		sprintf(rows[VR_BIKE_CAL_OFFSET_X], "LOCAL X OFFSET  < %+.1f CM >",
			calibration ? (float)calibration->offsetX/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_OFFSET_Y], "LOCAL Y OFFSET  < %+.1f CM >",
			calibration ? (float)calibration->offsetY/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_OFFSET_Z], "LOCAL Z OFFSET  < %+.1f CM >",
			calibration ? (float)calibration->offsetZ/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_ROT_X], "LOCAL ROT X  < %+.1f DEG >",
			calibration ? (float)calibration->rotationX/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_ROT_Y], "LOCAL ROT Y  < %+.1f DEG >",
			calibration ? (float)calibration->rotationY/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_ROT_Z], "LOCAL ROT Z  < %+.1f DEG >",
			calibration ? (float)calibration->rotationZ/
				WEAPON_CALIBRATION_VALUE_SCALE : 0.0f);
		sprintf(rows[VR_BIKE_CAL_GLOBAL_CENTER_X],
			"%s GLOBAL CENTER %s  < %+d CM >", GetActiveVrVehicleCategoryName(),
			bike ? "X" : "SIDE",
			categoryCalibration ? categoryCalibration->wheelCenterXCm : 0);
		sprintf(rows[VR_BIKE_CAL_GLOBAL_CENTER_Y],
			"%s GLOBAL CENTER %s  < %+d CM >", GetActiveVrVehicleCategoryName(),
			bike ? "Y" : "FORWARD",
			categoryCalibration ? categoryCalibration->wheelCenterYCm : 0);
		sprintf(rows[VR_BIKE_CAL_GLOBAL_CENTER_Z],
			"%s GLOBAL CENTER %s  < %+d CM >", GetActiveVrVehicleCategoryName(),
			bike ? "Z" : "HEIGHT",
			categoryCalibration ? categoryCalibration->wheelCenterZCm : 0);
		if(bike)
			sprintf(rows[VR_BIKE_CAL_GLOBAL_RADIUS],
				"%s GLOBAL RADIUS ADJUST  < %+d CM >",
				GetActiveVrVehicleCategoryName(),
				categoryCalibration ? categoryCalibration->wheelRadiusCm : 0);
		else
			sprintf(rows[VR_BIKE_CAL_GLOBAL_RADIUS],
				"GLOBAL DEFAULT RADIUS  < %d CM >",
				categoryCalibration ?
					categoryCalibration->carWheelRadiusCm :
					VR_CAR_WHEEL_DEFAULT_RADIUS_CM);
		sprintf(rows[VR_CAR_CAL_GLOBAL_PITCH],
			"GLOBAL WHEEL PITCH  < %+.1f DEG >",
			categoryCalibration ?
				(float)categoryCalibration->carWheelPitchHalfDeg/2.0f : 0.0f);
		sprintf(rows[VR_CAR_CAL_GLOBAL_YAW],
			"GLOBAL WHEEL YAW  < %+.1f DEG >",
			categoryCalibration ?
				(float)categoryCalibration->carWheelYawHalfDeg/2.0f : 0.0f);
		sprintf(rows[VR_CAR_CAL_GLOBAL_ROLL],
			"GLOBAL WHEEL ROLL  < %+.1f DEG >",
			categoryCalibration ?
				(float)categoryCalibration->carWheelRollHalfDeg/2.0f : 0.0f);
		sprintf(rows[VR_BIKE_CAL_MODEL_CENTER_X],
			"MODEL CENTER %s  < %+d CM >", bike ? "X" : "SIDE",
			viewCalibration ? viewCalibration->wheelCenterXCm : 0);
		sprintf(rows[VR_BIKE_CAL_MODEL_CENTER_Y],
			"MODEL CENTER %s  < %+d CM >", bike ? "Y" : "FORWARD",
			viewCalibration ? viewCalibration->wheelCenterYCm : 0);
		sprintf(rows[VR_BIKE_CAL_MODEL_CENTER_Z],
			"MODEL CENTER %s  < %+d CM >", bike ? "Z" : "HEIGHT",
			viewCalibration ? viewCalibration->wheelCenterZCm : 0);
		if(bike)
			sprintf(rows[VR_BIKE_CAL_MODEL_RADIUS],
				"MODEL RADIUS ADJUST  < %+d CM >",
				viewCalibration ? viewCalibration->wheelRadiusCm : 0);
		else if(viewCalibration && viewCalibration->carWheelRadiusCm != 0)
			sprintf(rows[VR_BIKE_CAL_MODEL_RADIUS],
				"MODEL RADIUS  < %d CM >",
				viewCalibration->carWheelRadiusCm);
		else
			sprintf(rows[VR_BIKE_CAL_MODEL_RADIUS],
				"MODEL RADIUS  < INHERIT %d CM >",
				categoryCalibration ?
					categoryCalibration->carWheelRadiusCm :
					VR_CAR_WHEEL_DEFAULT_RADIUS_CM);
		sprintf(rows[VR_CAR_CAL_MODEL_PITCH],
			"MODEL WHEEL PITCH  < %+.1f DEG >",
			viewCalibration ?
				(float)viewCalibration->carWheelPitchHalfDeg/2.0f : 0.0f);
		sprintf(rows[VR_CAR_CAL_MODEL_YAW],
			"MODEL WHEEL YAW  < %+.1f DEG >",
			viewCalibration ?
				(float)viewCalibration->carWheelYawHalfDeg/2.0f : 0.0f);
		sprintf(rows[VR_CAR_CAL_MODEL_ROLL],
			"MODEL WHEEL ROLL  < %+.1f DEG >",
			viewCalibration ?
				(float)viewCalibration->carWheelRollHalfDeg/2.0f : 0.0f);
		sprintf(rows[VR_BIKE_CAL_WHEELIE_HEIGHT],
			"WHEELIE HAND HEIGHT  < %d CM >",
			leanCalibration ? leanCalibration->wheelieHeightCm : 20);
		sprintf(rows[VR_BIKE_CAL_STAND_HEIGHT],
			"STAND HAND DROP  < %d CM >",
			leanCalibration ? leanCalibration->standHeightCm : 50);
		strcpy(rows[VR_BIKE_CAL_BACK], "BACK TO SETTINGS");
		const int rowCount = GetVehicleCalibrationMenuItemCount();
		for(int row = 0; row < rowCount; row++){
			const int item = GetVehicleCalibrationMenuItemForRow(row);
			const int y = 166+row*29;
			if(row == gVrBikeCalibrationMenuSelection)
				FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85, y+22,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 2,
				row == gVrBikeCalibrationMenuSelection ? 255 : 205,
				row == gVrBikeCalibrationMenuSelection ? 245 : 215,
				row == gVrBikeCalibrationMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrHolsterMenuVisible){
		DrawVrMenuText("HOLSTER LOADOUT", VR_MENU_WIDTH/2, 112, 5, 100, 225, 255);
		DrawVrMenuText("SEVEN BODY POINTS - CENTER THROWABLE SLOT IS FIXED",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		for(int item = 0; item < HOLSTER_MENU_ITEM_COUNT; item++){
			const int y = 180+item*55;
			if(item == gVrHolsterMenuSelection)
				FillVrMenuRect(85, y-13, VR_MENU_WIDTH-85, y+37,
					25, 95, 135, 245);
			char row[112];
			if(item < HOLSTER_POINT_COUNT){
				char weapon[64];
				FormatHolsterSlotDisplayName(gHolsterPointWeaponSlot[item], weapon);
				if(item == HOLSTER_CHEST_CENTER)
					sprintf(row, "%s  [ %s ]", gHolsterPointNames[item], weapon);
				else
					sprintf(row, "%s  < %s >", gHolsterPointNames[item], weapon);
			}else
				strcpy(row, "BACK TO SETTINGS");
			DrawVrMenuText(row, VR_MENU_WIDTH/2, y, 3,
				item == gVrHolsterMenuSelection ? 255 : 205,
				item == gVrHolsterMenuSelection ? 245 : 215,
				item == gVrHolsterMenuSelection ? 110 : 225);
			if(item < HOLSTER_POINT_COUNT){
				const int slot = gHolsterPointWeaponSlot[item];
				if(slot >= 0 && IsVrWeaponSlotOwned(slot))
					DrawVrWeaponIcon(GetVrWeaponTypeForSlot(slot),
						VR_MENU_WIDTH-154, y+10, 48, 48);
			}
		}
		DrawVrMenuText("CLEAR THE OLD POINT BEFORE MOVING AN ASSIGNED SLOT",
			VR_MENU_WIDTH/2, 650, 2, 255, 180, 225);
		DrawVrMenuText("LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrCalibrationMenuVisible){
		SyncCurrentWeaponCalibration();
		SyncCurrentSupportGripCalibration();
		char calibrationHeading[112];
		sprintf(calibrationHeading, "%s WEAPON - %s - %s MODEL HAND",
			ModelSets::GetName(GetActiveWeaponModelSet()),
			GetVrWeaponName(GetCalibrationWeaponType()),
			gCalibrationEditHand == 0 ? "LEFT" : "RIGHT");
		DrawVrMenuText(calibrationHeading, VR_MENU_WIDTH/2, 110, 3,
			100, 225, 255);
		DrawVrMenuText(gWeaponAimAligned ?
			"AIM ATTACHED TO EACH HAND'S WEAPON MODEL - LASER ROWS LOCKED" :
			(gCalibrationEditHand == 0 ?
			 "LEFT AIM/SUPPORT AUTO FROM RIGHT - READ ONLY" :
			 "RIGHT AIM/SUPPORT CANONICAL - EDITS DRIVE BOTH HANDS"),
			VR_MENU_WIDTH/2, 139, 2, 170, 190, 210);

		char rows[VR_CALIBRATION_MENU_ITEM_COUNT][96];
		WeaponCalibration displayedWeapon;
		CaptureCurrentWeaponCalibration(displayedWeapon);
		if(gWeaponAimAligned){
			WeaponCalibration effectiveAim;
			if(GetEffectiveAlignedRightAimCalibration(
			   GetCalibrationWeaponType(), &effectiveAim))
				CopyWeaponAim(effectiveAim, displayedWeapon);
		}
		SupportGripCalibration displayedSupport;
		CaptureCurrentSupportGripCalibration(displayedSupport);
		const char *derivedPrefix = "RIGHT";
		if(gCalibrationEditHand == 0){
			WeaponCalibration canonicalWeapon = displayedWeapon;
			SupportGripCalibration canonicalSupport = displayedSupport;
			ApplyMirroredWeaponAim(canonicalWeapon, displayedWeapon);
			ApplyMirroredSupportGrip(canonicalSupport, displayedSupport);
			derivedPrefix = "LEFT AUTO";
		}
		const char *aimLockSuffix = gWeaponAimAligned ? "  [ LOCKED ]" : "";
		sprintf(rows[VR_CAL_AIM_ALIGNED],
			"AIM ALIGNED - RIGHT CANONICAL  < %s >",
			gWeaponAimAligned ? "ON" : "OFF");
		sprintf(rows[VR_CAL_AIM_OFFSET_X], "%s AIM X OFFSET  < %+.1f CM >%s",
			derivedPrefix, (float)displayedWeapon.aimOffsetX/WEAPON_CALIBRATION_VALUE_SCALE,
			aimLockSuffix);
		sprintf(rows[VR_CAL_AIM_OFFSET_Y], "%s AIM Y OFFSET  < %+.1f CM >%s",
			derivedPrefix, (float)displayedWeapon.aimOffsetY/WEAPON_CALIBRATION_VALUE_SCALE,
			aimLockSuffix);
		sprintf(rows[VR_CAL_AIM_OFFSET_Z], "%s AIM Z OFFSET  < %+.1f CM >%s",
			derivedPrefix, (float)displayedWeapon.aimOffsetZ/WEAPON_CALIBRATION_VALUE_SCALE,
			aimLockSuffix);
		sprintf(rows[VR_CAL_AIM_ROT_X], "%s AIM LOCAL ROT X  < %+.1f DEG >%s",
			derivedPrefix, (float)displayedWeapon.aimRotationX/WEAPON_CALIBRATION_VALUE_SCALE,
			aimLockSuffix);
		sprintf(rows[VR_CAL_AIM_ROT_Y], "%s AIM LOCAL ROT Y  < %+.1f DEG >%s",
			derivedPrefix, (float)displayedWeapon.aimRotationY/WEAPON_CALIBRATION_VALUE_SCALE,
			aimLockSuffix);
		sprintf(rows[VR_CAL_AIM_ROT_Z], "%s AIM LOCAL ROT Z  < %+.1f DEG >%s",
			derivedPrefix, (float)displayedWeapon.aimRotationZ/WEAPON_CALIBRATION_VALUE_SCALE,
			aimLockSuffix);
		sprintf(rows[VR_CAL_WEAPON_OFFSET_X], "WEAPON X OFFSET  < %+.1f CM >",
			(float)gWeaponOffsetXCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_OFFSET_Y], "WEAPON Y OFFSET  < %+.1f CM >",
			(float)gWeaponOffsetYCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_OFFSET_Z], "WEAPON Z OFFSET  < %+.1f CM >",
			(float)gWeaponOffsetZCm/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_ROT_X], "WEAPON LOCAL ROT X  < %+.1f DEG >",
			(float)gWeaponRotationXDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_ROT_Y], "WEAPON LOCAL ROT Y  < %+.1f DEG >",
			(float)gWeaponRotationYDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_WEAPON_ROT_Z], "WEAPON LOCAL ROT Z  < %+.1f DEG >",
			(float)gWeaponRotationZDeg/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_GRIP_TYPE], "%s SUPPORT HAND POSE  < %s >",
			derivedPrefix,
			gSupportGripTypeNames[displayedSupport.gripType]);
		sprintf(rows[VR_CAL_SUPPORT_OFFSET_X], "%s SUPPORT GRIP X  < %+.1f CM >",
			derivedPrefix, (float)displayedSupport.offsetX/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_OFFSET_Y], "%s SUPPORT GRIP Y  < %+.1f CM >",
			derivedPrefix, (float)displayedSupport.offsetY/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_OFFSET_Z], "%s SUPPORT GRIP Z  < %+.1f CM >",
			derivedPrefix, (float)displayedSupport.offsetZ/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_ROT_X], "%s SUPPORT LOCAL ROT X  < %+.1f DEG >",
			derivedPrefix, (float)displayedSupport.rotationX/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_ROT_Y], "%s SUPPORT LOCAL ROT Y  < %+.1f DEG >",
			derivedPrefix, (float)displayedSupport.rotationY/WEAPON_CALIBRATION_VALUE_SCALE);
		sprintf(rows[VR_CAL_SUPPORT_ROT_Z], "%s SUPPORT LOCAL ROT Z  < %+.1f DEG >",
			derivedPrefix, (float)displayedSupport.rotationZ/WEAPON_CALIBRATION_VALUE_SCALE);
		const int laserOverride = GetWeaponLaserOverride(GetCalibrationWeaponType());
		sprintf(rows[VR_CAL_LASER], "THIS WEAPON LASER  < %s >",
			laserOverride < 0 ? "FOLLOW GLOBAL" : (laserOverride ? "ON" : "OFF"));
		strcpy(rows[VR_CAL_BACK], "BACK TO SETTINGS");

		const int visibleRows = Min(VR_CALIBRATION_MENU_ITEM_COUNT, 14);
		const int first = Max(0, Min(gVrCalibrationMenuSelection-6, VR_CALIBRATION_MENU_ITEM_COUNT-visibleRows));
		for(int item = first; item < first+visibleRows; item++){
			const int y = 176+(item-first)*35;
			if(item == gVrCalibrationMenuSelection)
				FillVrMenuRect(85, y-7, VR_MENU_WIDTH-85, y+27,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrCalibrationMenuSelection ? 255 : 205,
				item == gVrCalibrationMenuSelection ? 245 : 215,
				item == gVrCalibrationMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrVehicleMenuVisible){
		DrawVrMenuText("VEHICLE SETTINGS", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("DRIVING CONTROLS AND PER-VEHICLE CALIBRATION",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_VEHICLE_MENU_ITEM_COUNT][112];
		CVehicle *activeVehicle = FindPlayerVehicle();
		const int activeCategory = GetVrVehicleCategory(activeVehicle);
		VehicleCategoryCalibration *categoryCalibration =
			GetVehicleCategoryCalibration(activeVehicle);
		VehicleViewCalibration *viewCalibration = activeVehicle ?
			GetVehicleViewCalibration(activeVehicle->GetModelIndex()) : nil;
		sprintf(rows[VR_VEHICLE_CAR_DRIVING_TYPE],
			"CAR DRIVING TYPE  < %s >", DrivingTypeName(gCarDrivingType));
		sprintf(rows[VR_VEHICLE_BIKE_DRIVING_TYPE],
			"BIKE DRIVING TYPE  < %s >", DrivingTypeName(gBikeDrivingType));
		sprintf(rows[VR_VEHICLE_BOAT_DRIVING_TYPE], "BOAT DRIVING TYPE  < %s >", DrivingTypeName(gBoatDrivingType));
		for(int category = 0; category < 3; category++)
			sprintf(rows[VR_VEHICLE_CAR_THIRD_PERSON+category], "%s VIEW  < %s >",
				gVehicleCategoryNames[category], gVehicleThirdPerson[category] ? "THIRD PERSON" : "FIRST PERSON");
		sprintf(rows[VR_VEHICLE_DEFAULT_BODY], "DEFAULT DRIVING TOMMY  < %s >",
			gDefaultDrivingBodyVisible ? "VISIBLE" : "HIDDEN");
		const bool defaultView = activeVehicle &&
			GetDrivingTypeForVehicle(activeVehicle) == VR_DRIVING_DEFAULT &&
			activeCategory != VR_VEHICLE_CATEGORY_HELI;
		if(categoryCalibration && viewCalibration){
			sprintf(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT],
				"%s %s HEIGHT  < %+d CM >",
				gVehicleCategoryNames[activeCategory],
				defaultView ? "DEFAULT" : "GLOBAL",
				defaultView ? gDefaultSeatHeightCm[activeCategory] : categoryCalibration->seatHeightCm);
			sprintf(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD],
				"%s %s FORWARD  < %+d CM >",
				gVehicleCategoryNames[activeCategory],
				defaultView ? "DEFAULT" : "GLOBAL",
				defaultView ? gDefaultSeatDistanceCm[activeCategory] : categoryCalibration->seatDistanceCm);
			sprintf(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT],
				"MODEL HEIGHT  < %+d CM >", defaultView ? viewCalibration->defaultSeatHeightCm : viewCalibration->seatHeightCm);
			sprintf(rows[VR_VEHICLE_MODEL_SEAT_FORWARD],
				"MODEL FORWARD  < %+d CM >",
				defaultView ? viewCalibration->defaultSeatDistanceCm : viewCalibration->seatDistanceCm);
		}else{
			strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT],
				"GLOBAL HEIGHT  < ENTER VEHICLE >");
			strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD],
				"GLOBAL FORWARD  < ENTER VEHICLE >");
			strcpy(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT],
				"MODEL HEIGHT  < ENTER VEHICLE >");
			strcpy(rows[VR_VEHICLE_MODEL_SEAT_FORWARD],
				"MODEL FORWARD  < ENTER VEHICLE >");
		}
		sprintf(rows[VR_VEHICLE_MOTION_HAND],
			"MOTION STEERING HAND  < %s >", MotionSteeringHandName());
		sprintf(rows[VR_VEHICLE_WHEEL_VISIBLE],
			"ALL CARS VIRTUAL WHEEL  < %s >",
			gImmersiveCarWheelVisible ? "VISIBLE" : "HIDDEN");
		sprintf(rows[VR_VEHICLE_BOAT_WHEEL_VISIBLE], "ALL BOATS VIRTUAL WHEEL  < %s >", gImmersiveBoatWheelVisible ? "VISIBLE" : "HIDDEN");
		if(activeVehicle && (activeVehicle->IsCar() || activeVehicle->IsBoat()) && viewCalibration){
			const char *visibility =
				viewCalibration->carWheelVisibilityOverride == 0 ?
				"HIDE" : "INHERIT";
			sprintf(rows[VR_VEHICLE_MODEL_WHEEL_VISIBLE],
				"THIS MODEL VIRTUAL WHEEL  < %s >", visibility);
		}else
			strcpy(rows[VR_VEHICLE_MODEL_WHEEL_VISIBLE],
				"THIS MODEL VIRTUAL WHEEL  < ENTER CAR OR BOAT >");
		sprintf(rows[VR_VEHICLE_HANDLE_CUBES],
			"GRIP CALIBRATION CUBES  < %s >",
			(activeCategory == VR_VEHICLE_CATEGORY_BIKE ? gBikeHandleHighlightsEnabled :
			 activeCategory == VR_VEHICLE_CATEGORY_BOAT ? gBoatHandleHighlightsEnabled : gCarHandleHighlightsEnabled) ? "ON" : "OFF");
		sprintf(rows[VR_VEHICLE_WHEEL_HAND_PULL_BACK], "WHEEL HAND PULL BACK  < %+d MM >", gWheelHandPullBackMm);
		sprintf(rows[VR_VEHICLE_BIKE_LOCK_HORIZON],
			"BIKE LOCK HORIZON  < %s >",
			gBikeLockHorizonEnabled ? "ON" : "OFF");
		sprintf(rows[VR_VEHICLE_BIKE_THROTTLE], "BIKE THROTTLE  < %s >",
			gBikeManualThrottle ? "WRIST TWIST" : "RIGHT TRIGGER");
		sprintf(rows[VR_VEHICLE_BIKE_VISUAL_LEAN], "BIKE LEAN SHOWN  < %d%% >", gBikeVisualLeanPercent);
		sprintf(rows[VR_VEHICLE_BIKE_VIEW_TILT], "BIKE VIEW FOLLOWS TILT  < %s >", gBikeViewFollowingTilt ? "ON" : "OFF");
		sprintf(rows[VR_VEHICLE_BIKE_THROW_RIDER], "THROWN OFF ON A CRASH  < %s >", gBikeRiderCanBeThrown ? "ON" : "OFF");
		const int activeDrivingType = GetDrivingTypeForVehicle(activeVehicle);
		const bool anyImmersive =
			gCarDrivingType == VR_DRIVING_IMMERSIVE ||
			gBikeDrivingType == VR_DRIVING_IMMERSIVE;
		const char *calibrationState =
			IsImmersiveDrivingActiveInternal() ? "OPEN" :
			(activeVehicle ?
				(activeDrivingType == VR_DRIVING_IMMERSIVE ?
					"UNAVAILABLE" : "IMMERSIVE MODE ONLY") :
				(anyImmersive ? "ENTER VEHICLE" : "IMMERSIVE MODE ONLY"));
		sprintf(rows[VR_VEHICLE_CALIBRATION],
			"CONTROL CALIBRATION  < %s >", calibrationState);
		strcpy(rows[VR_VEHICLE_BACK], "BACK TO SETTINGS");
		const int firstVehicleRow = Max(0, Min(gVrVehicleMenuSelection-6, VR_VEHICLE_MENU_ITEM_COUNT-13));
		for(int item = firstVehicleRow; item < Min(firstVehicleRow+13, (int)VR_VEHICLE_MENU_ITEM_COUNT); item++){
			const int y = 166+(item-firstVehicleRow)*36;
			if(item == gVrVehicleMenuSelection)
				FillVrMenuRect(85, y-8, VR_MENU_WIDTH-85, y+29,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 2,
				item == gVrVehicleMenuSelection ? 255 : 205,
				item == gVrVehicleMenuSelection ? 245 : 215,
				item == gVrVehicleMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrHudMenuVisible){
		const int panel = gWristHud.panel;
		char rows[VR_HUD_MENU_ITEM_COUNT > VR_WRIST_ITEM_COUNT ? VR_HUD_MENU_ITEM_COUNT : VR_WRIST_ITEM_COUNT][112];
		int itemCount = VR_HUD_MENU_ITEM_COUNT;
		DrawVrMenuText(gWristHud.editing ? "WRIST PANEL PLACEMENT" : "HUD SETTINGS",
			VR_MENU_WIDTH/2, 112, 5, 100, 225, 255);
		if(gWristHud.editing){
			itemCount = VR_WRIST_ITEM_COUNT;
			const int *values = gWristHud.placement[panel][gWristHud.context][gWristHud.underside[panel] ? 1 : 0];
			DrawVrMenuText(WristHudSettings::PanelName(panel), VR_MENU_WIDTH/2, 146, 2, 190, 155, 255);
			sprintf(rows[VR_WRIST_CONTEXT], "CALIBRATING FOR  < %s >", WristHudSettings::ContextName(gWristHud.context));
			sprintf(rows[VR_WRIST_SIDE], "SIDE  < %s >", gWristHud.underside[panel] ? "INNER" : "OUTER");
			sprintf(rows[VR_WRIST_HAND], "HAND  < %s >", gWristHud.hand[panel] ? "RIGHT" : "LEFT");
			static const char *const labels[] = {"ALONG", "ACROSS", "LIFT", "PITCH", "YAW", "ROLL", "SIZE"};
			for(int field = 0; field < WristHudSettings::FIELD_COUNT; field++){
				if(field == WristHudSettings::SIZE)
					sprintf(rows[VR_WRIST_ALONG+field], "SIZE  < %d%% >", values[field]);
				else
					sprintf(rows[VR_WRIST_ALONG+field], "%s  < %+.1f %s >", labels[field],
						values[field]*0.1f, field < WristHudSettings::PITCH ? "CM" : "DEG");
			}
			static const char *const colours[] = {"RED", "GREEN", "BLUE"};
			for(int channel = 0; channel < 3; channel++)
				sprintf(rows[VR_WRIST_RED+channel], "AMMO %s  < %d >", colours[channel], gWristHud.ammoColour[channel]);
			strcpy(rows[VR_WRIST_COPY], "COPY FROM OTHER SIDE");
			strcpy(rows[VR_WRIST_RESET], "RESTORE THIS PLACEMENT");
			strcpy(rows[VR_WRIST_BACK], "BACK TO HUD");
		}else{
			DrawVrMenuText("CLASSIC INTERFACE OR OPTIONAL WRIST PANELS",
				VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
			sprintf(rows[VR_HUD_ENABLED], "GAMEPLAY HUD  < %s >", gGameplayHudVisible ? "ON" : "OFF");
			sprintf(rows[VR_HUD_HORIZONTAL_SCALE], "HORIZONTAL WIDTH  < %d%% >", gHudWidthPercent);
			sprintf(rows[VR_HUD_SCALE], "UNIFORM SCALE  < %d%% >", gHudScalePercent);
			sprintf(rows[VR_HUD_OFFSET_X], "HORIZONTAL OFFSET  < %+d CM >", gHudOffsetXCm);
			sprintf(rows[VR_HUD_OFFSET_Y], "VERTICAL OFFSET  < %+d CM >", gHudOffsetYCm);
			sprintf(rows[VR_HUD_PRESET], "HUD PRESET  < %s >", gWristHud.PresetName());
			sprintf(rows[VR_HUD_WRIST_PANEL], "WRIST PANEL  < %s >", WristHudSettings::PanelName(panel));
			sprintf(rows[VR_HUD_WRIST_ENABLED], "SELECTED PANEL  < %s >", gWristHud.enabled[panel] ? "WORN" : "OFF");
			sprintf(rows[VR_HUD_WRIST_HAND], "HAND  < %s >", gWristHud.hand[panel] ? "RIGHT" : "LEFT");
			sprintf(rows[VR_HUD_WRIST_SIDE], "SIDE  < %s >", gWristHud.underside[panel] ? "INNER" : "OUTER");
			strcpy(rows[VR_HUD_WRIST_PLACEMENT], "PANEL PLACEMENT  < OPEN >");
			sprintf(rows[VR_HUD_WRIST_GAZE], "SHOW PANELS  < %s >", gWristHud.gaze ? "WHEN LOOKED AT" : "ALWAYS");
			sprintf(rows[VR_HUD_WRIST_RANGE], "LOOK RANGE  < %d CM >", gWristHud.gazeRangeCm);
			sprintf(rows[VR_HUD_WRIST_VEHICLE], "PANELS WHILE DRIVING  < %s >", gWristHud.inVehicle ? "IMMERSIVE ONLY" : "OFF");
			sprintf(rows[VR_HUD_CLASSIC_WEAPON], "CLASSIC WEAPON PANEL  < %s >", gWristHud.classicWeapon ? "ON" : "OFF");
			sprintf(rows[VR_HUD_CLASSIC_CLOCK], "CLASSIC CLOCK  < %s >", gWristHud.classicClock ? "ON" : "OFF");
			strcpy(rows[VR_HUD_BACK], "BACK TO SETTINGS");
		}
		const int visible = 13;
		const int first = Max(0, Min(gVrHudMenuSelection-visible+1, itemCount-visible));
		for(int item = first; item < Min(first+visible, itemCount); item++){
			const int y = 185+(item-first)*35;
			const bool selected = item == gVrHudMenuSelection;
			const bool submenu = !gWristHud.editing && item == VR_HUD_WRIST_PLACEMENT;
			if(selected) FillVrMenuRect(85, y-6, VR_MENU_WIDTH-85, y+26, 25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				selected ? 255 : submenu ? 195 : 205,
				selected ? 245 : submenu ? 150 : 215,
				selected ? 110 : submenu ? 255 : 225);
		}
		char page[96];
		sprintf(page, "ROWS %d-%d OF %d%s", first+1, Min(first+visible,itemCount), itemCount,
			gWristHudFailed ? "  -  WRIST ATLAS ERROR" : "");
		DrawVrMenuText(page, VR_MENU_WIDTH/2, 674, 2, 170, 190, 210);
		DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else if(gVrLocomotionMenuVisible && gVrControlsMenuVisible){
		DrawVrControlsMenu();
	}else if(gVrLocomotionMenuVisible){
		DrawVrMenuText("LOCOMOTION", VR_MENU_WIDTH/2, 112, 5,
			100, 225, 255);
		DrawVrMenuText("MOVEMENT AND COMFORT TURNING",
			VR_MENU_WIDTH/2, 146, 2, 170, 190, 210);
		char rows[VR_LOCOMOTION_MENU_ITEM_COUNT][112];
		sprintf(rows[VR_LOCOMOTION_MOVEMENT_MODE],
			"MOVEMENT  < %s >",
			gMovementMode == VR_MOVEMENT_TELEPORT ?
				"TELEPORT" : "SMOOTH");
		sprintf(rows[VR_LOCOMOTION_MOVEMENT_ORIENTATION],
			"MOVEMENT DIRECTION  < %s >",
			gMovementOrientation ==
				VR_MOVEMENT_ORIENTATION_HEAD_DIRECTED ?
				"HEAD DIRECTED" :
			gMovementOrientation ==
				VR_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL ?
				"HEAD TURN EXP" :
			gMovementOrientation == VR_MOVEMENT_ORIENTATION_HEAD ?
				"HEAD" : "BODY");
		sprintf(rows[VR_LOCOMOTION_TURN_MODE], "TURNING  < %s >",
			gTurnMode == VR_TURN_SNAP ? "SNAP" : "SMOOTH");
		if(gMovementOrientation ==
		   VR_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL)
			sprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY],
				"HEAD TURN SENSITIVITY  < %d%% >",
				gHeadSteeringSensitivityPercent);
		else
			sprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY],
				"SMOOTH TURN SENSITIVITY  < %d%% >",
				gTurnSensitivityPercent);
		sprintf(rows[VR_LOCOMOTION_SNAP_ANGLE],
			"SNAP TURN ANGLE  < %d DEG >", gSnapTurnAngleDegrees);
		sprintf(rows[VR_LOCOMOTION_HEAD_BOBBING],
			"HEAD BOBBING  < %s >", gHeadBobbingEnabled ? "ON" : "OFF");
		sprintf(rows[VR_LOCOMOTION_CUTSCENES], "CUTSCENES  < %s >",
			gCutsceneMode ? "STEREO - R3 CAMERA / L3 SAVE" : "CINEMA SCREEN");
		strcpy(rows[VR_LOCOMOTION_CONTROLS], "CONTROLS  < OPEN >");
		strcpy(rows[VR_LOCOMOTION_BACK], "BACK TO SETTINGS");
		for(int item = 0; item < VR_LOCOMOTION_MENU_ITEM_COUNT; item++){
			const int y = 176+item*40;
			if(item == gVrLocomotionMenuSelection)
				FillVrMenuRect(85, y-7, VR_MENU_WIDTH-85, y+29,
					25, 95, 135, 245);
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 3,
				item == gVrLocomotionMenuSelection ? 255 : 205,
				item == gVrLocomotionMenuSelection ? 245 : 215,
				item == gVrLocomotionMenuSelection ? 110 : 225);
		}
		DrawVrMenuText("MOVEMENT MODE LOCKED TO SMOOTH FOR THIS RELEASE",
			VR_MENU_WIDTH/2, 660, 2, 255, 180, 225);
		DrawVrMenuText("LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
	}else{
	DrawVrMenuText("SETTINGS", VR_MENU_WIDTH/2, 118, 4, 100, 225, 255);

	char rows[VR_MENU_ITEM_COUNT][96];
	strcpy(rows[VR_MAIN_GRAPHICS], "GRAPHICS SETTINGS  < OPEN >");
	sprintf(rows[VR_MAIN_MODEL_SET], "MODEL ASSETS  < OPEN%s >",
		ModelSets::IsRestartRequired() ? " - RESTART" : "");
	strcpy(rows[VR_MAIN_TRAFFIC_SETTINGS], "TRAFFIC SETTINGS  < OPEN >");
	strcpy(rows[VR_MAIN_HUD], "HUD SETTINGS  < OPEN >");
	sprintf(rows[VR_MAIN_HANDS], "VR HANDS  < %s >",
		gVrHandsEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_LASER], "WEAPON LASER  < %s >",
		gWeaponLaserEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_WEAPON_HAPTICS], "WEAPON HAPTICS  < %s >",
		gWeaponHapticsEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_WEAPON_HAPTICS_STRENGTH],
		"HAPTIC RECOIL STRENGTH  < %d%% >",
		gWeaponHapticsStrengthPercent);
	sprintf(rows[VR_MAIN_HOLSTER_HIGHLIGHTS], "HOLSTER HIGHLIGHTS  < %s >",
		gWeaponHolsterHighlightsEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_MANUAL_RELOAD], "MANUAL RELOADING  < %s >",
		gManualReloadEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_SCOPE_AIM], "PHYSICAL SCOPE AIM  < %s >",
		gPhysicalScopeAimEnabled ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_AIM_DIRECTION], "AIM DIRECTION  < %s >",
		gHeadAimEnabled ? "HEAD" : "CONTROLLER");
	strcpy(rows[VR_MAIN_VEHICLE_SETTINGS], "VEHICLE SETTINGS  < OPEN >");
	strcpy(rows[VR_MAIN_LOCOMOTION_SETTINGS],
		"LOCOMOTION SETTINGS  < OPEN >");
	strcpy(rows[VR_MAIN_CHEATS], "CHEATS  < OPEN >");
	sprintf(rows[VR_MAIN_DEBUG], "DEBUG OVERLAY  < %s >",
		gDebugVisible ? "ON" : "OFF");
	sprintf(rows[VR_MAIN_GRIP_LOCK], "WEAPON GRIP LOCK  < %s >",
		gWeaponGripLockEnabled ? "ON" : "OFF");
	strcpy(rows[VR_MAIN_CALIBRATION], "WEAPON CALIBRATION  < OPEN >");
	strcpy(rows[VR_MAIN_HOLSTERS], "HOLSTER LOADOUT  < OPEN >");
	strcpy(rows[VR_MAIN_ABOUT], "ABOUT  < OPEN >");
	for(int item = 0; item < VR_MENU_ITEM_COUNT; item++){
		const int y = 145+item*22;
		const bool selected = item == gVrMenuSelection;
		if(selected)
			FillVrMenuRect(85, y-2, VR_MENU_WIDTH-85, y+18, 25, 95, 135, 245);
		uint8 red = selected ? 255 : 205;
		uint8 green = selected ? 245 : 215;
		uint8 blue = selected ? 110 : 225;
		GetVrMainMenuCategoryColour(item, selected, &red, &green, &blue);
		DrawVrMenuText(rows[item], VR_MENU_WIDTH/2, y, 2,
			red, green, blue);
	}
	char modelSetStatus[112];
	sprintf(modelSetStatus, "ACTIVE %s   REQUESTED %s%s",
		ModelSets::GetSourceName(ModelSets::GetActive()),
		ModelSets::GetSourceName(ModelSets::GetRequested()),
		ModelSets::IsRestartRequired() ? "   RESTART REQUIRED" : "");
	DrawVrMenuText(modelSetStatus, VR_MENU_WIDTH/2, 680, 2,
		ModelSets::IsRestartRequired() ? 255 : 125,
		ModelSets::IsRestartRequired() ? 180 : 255,
		ModelSets::IsRestartRequired() ? 225 : 145);
	if(!ModelSets::IsAvailable(ModelSets::MODEL_SET_MODERN))
		DrawVrMenuText("MODERN OVERLAY NOT INSTALLED - BASE FILES ARE USED AS-IS",
			VR_MENU_WIDTH/2, 650, 2, 255, 180, 225);
	DrawVrMenuText("LEFT STICK SELECT   A OPEN   B CLOSE", VR_MENU_WIDTH/2, 718, 2,
		170, 190, 210);
	}
	}

#ifdef RW_D3D12
	if(!rw::d3d12::uploadRgbaToExternal(
	   gVrMenuSwapchain.images[gVrMenuSwapchain.acquiredIndex].texture, gVrMenuPixels,
	   VR_MENU_WIDTH*4, VR_MENU_WIDTH, VR_MENU_HEIGHT)){
		ReleaseSwapchain(gVrMenuSwapchain);
		return false;
	}
	if(gVrAboutVisible)
		gVrAboutWasRendered = true;
	return true;
#else
	GLint oldTexture = 0, oldAlignment = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
	glBindTexture(GL_TEXTURE_2D, gVrMenuSwapchain.images[gVrMenuSwapchain.acquiredIndex].image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT,
		GL_RGBA, GL_UNSIGNED_BYTE, gVrMenuPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	ReleaseSwapchain(gVrMenuSwapchain);
	if(gVrAboutVisible)
		gVrAboutWasRendered = true;
	return true;
#endif
}

bool UpdateDebugSwapchain()
{
	if(!gDebugVisible || !gDebugSwapchain.handle || !AcquireSwapchain(gDebugSwapchain))
		return false;
	for(int pixel=0;pixel<VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT;pixel++){
		gDebugPixels[pixel*4+0]=0; gDebugPixels[pixel*4+1]=0;
		gDebugPixels[pixel*4+2]=0; gDebugPixels[pixel*4+3]=210;
	}
	const PerfFrameSample &p = gPerfDisplay;
	char value[80];
	int y = 6;
	gDebugColumnX = DEBUG_TEXT_MARGIN;

	sprintf(value,"FPS %d   FRAME %.1fms   WORST %.1fms",
		gDebugFps, p.frameMs, gPerfDisplayWorstMs);
	DrawDebugText(value,VR_DEBUG_WIDTH/2,y,2,255,230,64); y += 20;

	char vrsMode[8] = "NA";
#ifdef RW_D3D12
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	if(foveatedInfo.supported)
		sprintf(vrsMode, "%u", foveatedInfo.profile);
#endif
	const char *aaMode =
		gTemporalAaBackend == TEMPORAL_AA_DLAA &&
		Dlaa::WasLastEvaluationSuccessful() ? "DLAA" :
		(gTemporalAaBackend == TEMPORAL_AA_FSR2 &&
		 Fsr2::WasLastEvaluationSuccessful() ? "FSR2" :
		(gAntiAliasingEnabled ? "FXAA" : "OFF"));
	sprintf(value,"AA %s  SPS %s  VRS %s  SCALE %.0f%%",aaMode,
		gFullStereoSinglePass?"FULL":"HYB",vrsMode,gRenderScale*100.0f);
	DrawDebugText(value,VR_DEBUG_WIDTH/2,y,2,96,220,255); y += 22;

	DebugSection("CPU PHASES (MS)",&y);
	DebugPair("GAME",p.phaseMs[PERF_PHASE_GAME],
	          "STREAM",p.phaseMs[PERF_PHASE_STREAMING],&y);
	DebugPair("WORLDLIST",p.phaseMs[PERF_PHASE_WORLD_LIST],
	          "SCENE",p.phaseMs[PERF_PHASE_SCENE_SETUP],&y);
	DebugPair("LEFTEYE",p.phaseMs[PERF_PHASE_LEFT_EYE],
	          "RIGHTEYE",p.phaseMs[PERF_PHASE_RIGHT_EYE],&y);
	DebugPair("PRERENDER",p.phaseMs[PERF_PHASE_PRE_RENDER],
	          "SUBMIT",p.phaseMs[PERF_PHASE_SUBMIT],&y);
	DebugPair("AUDIO",p.phaseMs[PERF_PHASE_AUDIO],
	          "UI",p.phaseMs[PERF_PHASE_UI],&y);
	DebugPair("DESKTOP",p.phaseMs[PERF_PHASE_DESKTOP_RENDER],
	          "PRESENT",p.phaseMs[PERF_PHASE_DESKTOP_PRESENT],&y);
	DebugPair("FRAMEINIT",p.phaseMs[PERF_PHASE_FRAME_INIT],
	          "VRSETUP",p.phaseMs[PERF_PHASE_VR_SETUP],&y);
	DebugPair("OVERLAYS",p.phaseMs[PERF_PHASE_VR_OVERLAYS],
	          "CAMEND",p.phaseMs[PERF_PHASE_CAMERA_END],&y);
	// Anything the phases above missed. If this is not near zero the profiler
	// is lying by omission and the next phase to add belongs wherever it grows.
	DebugPair("UPKEEP",p.phaseMs[PERF_PHASE_SESSION_UPKEEP],
	          "CINEMA",p.phaseMs[PERF_PHASE_CINEMA_SUBMIT],&y);
	DebugPair("UNMEASURED",p.otherMs,"TOTALWORK",
		p.frameMs-p.xrWaitFrameMs,&y);

	DebugSection("GPU (MS)",&y);
	{
#ifdef RW_D3D12
		rw::d3d12::GpuFrameProfile gpu = {};
		rw::d3d12::getGpuFrameProfile(&gpu);
		DebugPair("SCENE",gpu.stageMs[rw::d3d12::GPU_STAGE_SCENE],
		          "TEMPAA",gpu.stageMs[rw::d3d12::GPU_STAGE_TEMPORAL_AA],&y);
		DebugPair("RESOLVE",gpu.stageMs[rw::d3d12::GPU_STAGE_RESOLVE],
		          "GPUTOTAL",gpu.totalMs,&y);
#endif
	}

	DebugSection("WAITS (MS)",&y);
	DebugPair("XR WAIT",p.xrWaitFrameMs,"XR END",p.xrEndFrameMs,&y);
	DebugPair("GPU FENCE",p.d3d12FrameFenceWaitMs,
	          "GPU FULL",p.d3d12FullGpuWaitMs,&y);
	DebugPair("SWAPCHAIN",p.xrSwapchainWaitMs,
	          "EXTSUBMIT",p.d3d12ExternalSubmitMs,&y);

	// Second column starts level with the first section, not with the header.
	gDebugColumnX = DEBUG_COLUMN_WIDTH + DEBUG_TEXT_MARGIN;
	y = 48;

	DebugSection("GEOMETRY",&y);
	sprintf(value,"DRAWCALLS %-8u INSTANCES %u",p.worldDrawCalls,p.geometryInstances);
	DrawDebugTextLeft(value,&y,200,235,200);
	sprintf(value,"INDICES   %-8.2fM SPSDRAWS  %u",
		p.worldSubmittedIndices/1000000.0,p.stereoSinglePassDrawCalls);
	DrawDebugTextLeft(value,&y,200,235,200);
	// Everything not drawn with instanced stereo costs a submission per eye.
	// Whichever of these two is large is where the draw call budget is going.
	sprintf(value,"MONODRAWS %-8u REPLAY    %u",p.monoDrawCalls,p.replayDrawCalls);
	DrawDebugTextLeft(value,&y,
		p.monoDrawCalls > p.stereoSinglePassDrawCalls ? 255 : 200,
		p.monoDrawCalls > p.stereoSinglePassDrawCalls ? 140 : 235,
		p.monoDrawCalls > p.stereoSinglePassDrawCalls ? 110 : 200);
	// Name the three parts of the frame issuing the most per-eye draws, so the
	// next thing to convert to instanced stereo is not a guess.
	{
		static const char *stageNames[rw::d3d12::WORLD_STAGE_COUNT] = {
			"OTHER","SKY","ROADS","CORONAREF","ENTITIES","WATER","BOATS",
			"FADING","TRANSWATER","EFFECTS","FX_GLASS","FX_SPECIAL",
			"FX_SHADOWS","FX_MOVING","FX_1STPERSON","FX_HANDS",
			"DESKTOP","CINEMA","MENUS","GAMEUPD","RENDERLIST","SETUP","HUD2D",
			"ENVMAP" };
		int order[rw::d3d12::WORLD_STAGE_COUNT];
		for(int i = 0; i < rw::d3d12::WORLD_STAGE_COUNT; i++) order[i] = i;
		for(int i = 0; i < rw::d3d12::WORLD_STAGE_COUNT; i++)
			for(int j = i+1; j < rw::d3d12::WORLD_STAGE_COUNT; j++)
				if(p.stageMonoDrawCalls[order[j]] > p.stageMonoDrawCalls[order[i]]){
					const int swap = order[i]; order[i] = order[j]; order[j] = swap;
				}
		for(int i = 0; i < 3; i++){
			if(p.stageMonoDrawCalls[order[i]] == 0) break;
			sprintf(value," %-10s %u",stageNames[order[i]],
				p.stageMonoDrawCalls[order[i]]);
			DrawDebugTextLeft(value,&y,255,190,120);
		}
	}
	DebugPair("INSTANCE",p.geometryInstanceMs,"BUFUPLOAD",p.geometryBufferUploadMs,&y);
	DebugPair("BUNDLE",p.stereoBundleBuildMs,"BUNDLEWAIT",p.stereoBundleWaitMs,&y);

	DebugSection("SCENE",&y);
	sprintf(value,"BUILDINGS %-5d OCCL %d/%d  OBJECTS %d",p.visibleBuildings,
		CRenderer::GetVrOcclusionActiveCount(0),
		CRenderer::GetVrOcclusionActiveCount(1),p.visibleObjects);
	DrawDebugTextLeft(value,&y,200,235,200);
	sprintf(value,"PEDS      %-8d VEHICLES  %d",p.visiblePeds,p.visibleVehicles);
	DrawDebugTextLeft(value,&y,200,235,200);
	sprintf(value,"ENTITIES  %-8d REQUESTED %d",p.entityRenderCalls,p.requestedModels);
	DrawDebugTextLeft(value,&y,200,235,200);
	sprintf(value,"PARTICLES %-8d HELIDUST  %d",p.particleActive,p.heliDustActive);
	DrawDebugTextLeft(value,&y,
		p.heliDustActive >= 320 ? 255 : 200,
		p.heliDustActive >= 320 ? 140 : 235,
		p.heliDustActive >= 320 ? 110 : 200);

	DebugSection("STREAMING / TEXTURES",&y);
	sprintf(value,"STREAMMEM %.0fMB  SLOWEST %.1fms ID %d",
		p.streamingMemory/(1024.0*1024.0),p.slowStreamItemMs,p.slowStreamItemId);
	DrawDebugTextLeft(value,&y,200,235,200);
	sprintf(value,"TEXUPLOAD %.2fMB x%d",
		p.textureUploadBytes/(1024.0*1024.0),p.textureUploadCount);
	DrawDebugTextLeft(value,&y,200,235,200);
	DebugPair("TEXCOPY",p.textureCpuCopyMs,"TEXQUEUE",p.textureQueueMs,&y);

	const bool anyFallback = p.stereoBundleFallbacks || p.stereoSinglePassFallbacks ||
		p.fixedFoveatedFailures;
	sprintf(value,"FALLBACK BUNDLE %u SPS %u VRS %u",
		p.stereoBundleFallbacks,p.stereoSinglePassFallbacks,p.fixedFoveatedFailures);
	y += 4;
	DrawDebugTextLeft(value,&y,anyFallback?255:120,anyFallback?110:150,
		anyFallback?110:120);
	if(gPerfRecording){
		sprintf(value,"RECORDING %d FRAMES",gPerfRecordedSamples);
		DrawDebugTextLeft(value,&y,255,140,140);
	}

#ifdef RW_D3D12
	if(!rw::d3d12::uploadRgbaToExternal(
	   gDebugSwapchain.images[gDebugSwapchain.acquiredIndex].texture, gDebugPixels,
	   VR_DEBUG_WIDTH*4, VR_DEBUG_WIDTH, VR_DEBUG_HEIGHT)){
		ReleaseSwapchain(gDebugSwapchain);
		return false;
	}
	// The image is released together with the eye and HUD images after the
	// shared D3D12 command list has completed.
	return true;
#else
	GLint oldTexture=0,oldAlignment=0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT,&oldAlignment);
	glBindTexture(GL_TEXTURE_2D,gDebugSwapchain.images[gDebugSwapchain.acquiredIndex].image);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT,1);
	glTexSubImage2D(GL_TEXTURE_2D,0,0,0,VR_DEBUG_WIDTH,VR_DEBUG_HEIGHT,
		GL_RGBA,GL_UNSIGNED_BYTE,gDebugPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT,oldAlignment);
	glBindTexture(GL_TEXTURE_2D,oldTexture);
	ReleaseSwapchain(gDebugSwapchain);
	return true;
#endif
}

#ifdef RW_D3D12
ID3D12Resource *AcquiredTexture(const Swapchain &swapchain)
{
	return swapchain.images[swapchain.acquiredIndex].texture;
}
#else
GLuint AcquiredTexture(const Swapchain &swapchain)
{
	return swapchain.images[swapchain.acquiredIndex].image;
}
#endif

void DestroyStereoRenderTargets()
{
#ifdef RW_D3D12
	Dlaa::ReleaseResources();
	Fsr2::ReleaseResources();
	rw::d3d12::invalidateScreenSpaceReflectionHistory();
#endif
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(gEye[eye].depth) RwRasterDestroy(gEye[eye].depth);
		if(gEye[eye].color) RwRasterDestroy(gEye[eye].color);
		gEye[eye].depth = gEye[eye].color = nil;
	}
#ifdef RW_D3D12
	if(gStereoDepth) RwRasterDestroy(gStereoDepth);
	if(gStereoColor) RwRasterDestroy(gStereoColor);
	gStereoDepth = gStereoColor = nil;
#endif
}

bool ReplaceStereoRenderTargets(float scale, bool synchronizeOldTargets)
{
	// In the DLSS upscaling modes the scene renders at a fraction of the
	// requested scale and the network reconstructs the rest, which is what
	// makes high render scales affordable: pixel cost follows this size, not
	// the output size.
	float renderRatio = 1.0f;
	if(gTemporalAaBackend == TEMPORAL_AA_DLAA)
		renderRatio = Dlaa::GetQualityModeRenderRatio(gDlssQualityMode);
	int renderWidth[EYE_COUNT], renderHeight[EYE_COUNT];
	for(int eye = 0; eye < EYE_COUNT; eye++){
		renderWidth[eye] = (int)(gEye[eye].swapchain.width*scale*renderRatio+0.5f);
		renderHeight[eye] = (int)(gEye[eye].swapchain.height*scale*renderRatio+0.5f);
		if(gTemporalAaBackend == TEMPORAL_AA_DLAA && gDlssQualityMode != 0){
			renderWidth[eye] = Max(256, renderWidth[eye] & ~15);
			renderHeight[eye] = Max(256, renderHeight[eye] & ~15);
		}
	}
#ifdef RW_D3D12
	if(renderWidth[0] != renderWidth[1] || renderHeight[0] != renderHeight[1]){
		VrLog("Cannot build the double-wide stereo target: eyes are %dx%d and %dx%d\n",
			renderWidth[0], renderHeight[0], renderWidth[1], renderHeight[1]);
		return false;
	}
	RwRaster *newStereoColor = RwRasterCreate(renderWidth[0]*EYE_COUNT, renderHeight[0], 32,
		rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
	RwRaster *newStereoDepth = RwRasterCreate(renderWidth[0]*EYE_COUNT, renderHeight[0], 0,
		rwRASTERTYPEZBUFFER);
	RwRaster *newColor[EYE_COUNT] = {};
	RwRaster *newDepth[EYE_COUNT] = {};
	bool created = newStereoColor && newStereoDepth;
	for(int eye = 0; eye < EYE_COUNT && created; eye++){
		newColor[eye] = RwRasterCreate(0, 0, 0,
			rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888 | rwRASTERDONTALLOCATE);
		newDepth[eye] = RwRasterCreate(0, 0, 0,
			rwRASTERTYPEZBUFFER | rwRASTERDONTALLOCATE);
		created = newColor[eye] && newDepth[eye];
		if(created){
			rw::Rect rect = { eye*renderWidth[eye], 0, renderWidth[eye], renderHeight[eye] };
			((rw::Raster*)newColor[eye])->subRaster((rw::Raster*)newStereoColor, &rect);
			((rw::Raster*)newDepth[eye])->subRaster((rw::Raster*)newStereoDepth, &rect);
		}
	}
	if(!created || (synchronizeOldTargets && !rw::d3d12::submitAndWaitForExternal())){
		for(int eye = 0; eye < EYE_COUNT; eye++){
			if(newDepth[eye]) RwRasterDestroy(newDepth[eye]);
			if(newColor[eye]) RwRasterDestroy(newColor[eye]);
		}
		if(newStereoDepth) RwRasterDestroy(newStereoDepth);
		if(newStereoColor) RwRasterDestroy(newStereoColor);
		return false;
	}
	DestroyStereoRenderTargets();
	gStereoColor = newStereoColor;
	gStereoDepth = newStereoDepth;
	for(int eye = 0; eye < EYE_COUNT; eye++){
		gEye[eye].color = newColor[eye];
		gEye[eye].depth = newDepth[eye];
		gEye[eye].renderWidth = renderWidth[eye];
		gEye[eye].renderHeight = renderHeight[eye];
	}
	gDlaaStereoActivationReady = false;
	gDlaaStereoActivationFailed = false;
	gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
	gFsr2StereoActivationFailed = false;
	VrLog("D3D12 double-wide stereo target %dx%d (%dx%d per eye, %.0f%%)\n",
		renderWidth[0]*EYE_COUNT, renderHeight[0], renderWidth[0], renderHeight[0], scale*100.0f);
#else
	RwRaster *newColor[EYE_COUNT] = {};
	RwRaster *newDepth[EYE_COUNT] = {};
	bool created = true;
	for(int eye = 0; eye < EYE_COUNT && created; eye++){
		newColor[eye] = RwRasterCreate(renderWidth[eye], renderHeight[eye], 32,
			rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
		newDepth[eye] = RwRasterCreate(renderWidth[eye], renderHeight[eye], 0,
			rwRASTERTYPEZBUFFER);
		created = newColor[eye] && newDepth[eye];
	}
	if(!created){
		for(int eye = 0; eye < EYE_COUNT; eye++){
			if(newDepth[eye]) RwRasterDestroy(newDepth[eye]);
			if(newColor[eye]) RwRasterDestroy(newColor[eye]);
		}
		return false;
	}
	DestroyStereoRenderTargets();
	for(int eye = 0; eye < EYE_COUNT; eye++){
		gEye[eye].color = newColor[eye];
		gEye[eye].depth = newDepth[eye];
		gEye[eye].renderWidth = renderWidth[eye];
		gEye[eye].renderHeight = renderHeight[eye];
	}
#endif
	gRenderScale = scale;
	return true;
}

void ApplyPendingRenderScale()
{
	if(!gRenderScaleChangePending || !gSession || gFrameBegun || gFramePrepared)
		return;
	gRenderScaleChangePending = false;
	const float requestedScale = gRenderScaleOptions[gRenderScaleIndex];
	// The TEMPORAL AA and DLSS MODE rows request a rebuild unconditionally,
	// but with DLAA inactive the render ratio is fixed at 1.0 and the target
	// dimensions often come out identical to what already exists. A rebuild
	// is not free: it reallocates the full double-wide target and drains the
	// whole GPU behind a fence, a guaranteed multi-frame hitch. Skip it when
	// nothing about the target would actually change.
	{
		float renderRatio = 1.0f;
		if(gTemporalAaBackend == TEMPORAL_AA_DLAA)
			renderRatio = Dlaa::GetQualityModeRenderRatio(gDlssQualityMode);
		int wantWidth =
			(int)(gEye[0].swapchain.width*requestedScale*renderRatio+0.5f);
		int wantHeight =
			(int)(gEye[0].swapchain.height*requestedScale*renderRatio+0.5f);
		if(gTemporalAaBackend == TEMPORAL_AA_DLAA && gDlssQualityMode != 0){
			wantWidth = Max(256, wantWidth & ~15);
			wantHeight = Max(256, wantHeight & ~15);
		}
		if(gEye[0].color != nil &&
		   wantWidth == gEye[0].renderWidth &&
		   wantHeight == gEye[0].renderHeight){
			gRenderScale = requestedScale;
			SaveVrSetting("RenderScale", gRenderScaleIndex);
			debug("[OpenXR] Render scale: %.0f%% unchanged (%dx%d per eye); "
				"keeping the current stereo targets\n",
				requestedScale*100.0f,
				gEye[0].renderWidth, gEye[0].renderHeight);
			return;
		}
	}
	if(ReplaceStereoRenderTargets(requestedScale, true)){
		SaveVrSetting("RenderScale", gRenderScaleIndex);
		debug("[OpenXR] Render scale: %.0f%% (%dx%d per eye)\n", requestedScale*100.0f,
			gEye[0].renderWidth, gEye[0].renderHeight);
	}else{
		int nearest = 0;
		for(int i = 1; i < (int)ARRAY_SIZE(gRenderScaleOptions); i++)
			if(fabsf(gRenderScaleOptions[i]-gRenderScale) <
			   fabsf(gRenderScaleOptions[nearest]-gRenderScale)) nearest = i;
		gRenderScaleIndex = nearest;
		debug("[OpenXR] Could not allocate the requested render scale; keeping %.0f%%\n",
			gRenderScale*100.0f);
	}
}

void DestroyStartupCapture()
{
	if(gStartupCaptureDc){
		if(gStartupCaptureOldBitmap)
			SelectObject(gStartupCaptureDc, gStartupCaptureOldBitmap);
		if(gStartupCaptureBitmap) DeleteObject(gStartupCaptureBitmap);
		DeleteDC(gStartupCaptureDc);
	}
	gStartupCaptureDc = nil;
	gStartupCaptureBitmap = nil;
	gStartupCaptureOldBitmap = nil;
	gStartupCaptureBits = nil;
}

void DestroySessionResources()
{
	ResetImmersiveDrivingInteraction();
#ifdef RW_D3D12
	// Without an OpenXR session the desktop swapchain owns frame pacing again.
	// Flat mode must still respect the user's VSync setting; forcing interval 1
	// here made every 2D benchmark report exactly the monitor refresh rate.
	rw::d3d12::setPresentInterval(gFlatModeEnabled ?
		(uint32)FrontEndMenuManager.m_PrefsVsync : 1u);
	DestroyTrackedForegroundResolvePipeline();
#endif
	DestroyStereoRenderTargets();
	for(int eye = 0; eye < EYE_COUNT; eye++){
		DestroySwapchain(gEye[eye].swapchain);
	}
	if(gHudDepth) RwRasterDestroy(gHudDepth);
	if(gHudColor) RwRasterDestroy(gHudColor);
	gHudDepth = gHudColor = nil;
	DestroySwapchain(gHudSwapchain);
	DestroySwapchain(gWristHudSwapchain);
	gWristHud.routingMask = gWristHudLayerMask = 0;
	gWristHudFailed = false;
	DestroySwapchain(gCinemaSwapchain);
	DestroySwapchain(gDebugSwapchain);
	DestroySwapchain(gVrMenuSwapchain);
	// The session is going away, so nothing is left to reference the retired
	// handles: release them now rather than leaking them with the session.
	DestroyRetiredSwapchainsNow();
	DestroyStartupCapture();
#ifndef RW_D3D12
	DestroyFxaaProgram();
	if(gCopyFramebuffer){ glDeleteFramebuffers(1, &gCopyFramebuffer); gCopyFramebuffer = 0; }
#endif
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gActions.gripSpace[hand]) xrDestroySpace(gActions.gripSpace[hand]);
		if(gActions.aimSpace[hand]) xrDestroySpace(gActions.aimSpace[hand]);
		gActions.gripSpace[hand] = XR_NULL_HANDLE;
		gActions.aimSpace[hand] = XR_NULL_HANDLE;
		gTrackedHandPoseValid[hand] = false;
		gTrackedHandAimPoseValid[hand] = false;
	}
	if(gGameplaySpace) xrDestroySpace(gGameplaySpace);
	if(gLocalSpace) xrDestroySpace(gLocalSpace);
	if(gViewSpace) xrDestroySpace(gViewSpace);
	gGameplaySpace = gLocalSpace = gViewSpace = XR_NULL_HANDLE;
	gCinemaAnchorValid = false;
	if(gSession){
		if(gSessionRunning) xrEndSession(gSession);
		xrDestroySession(gSession);
	}
	gSession = XR_NULL_HANDLE;
	gSessionRunning = false;
	gFrameBegun = false;
	gFramePrepared = false;
	gWasSubmitting = false;
	gTrackingCenterValid = false;
	gRecenterRequested = false;
	gVehicleViewStateValid = false;
	gVehicleViewWasActive = false;
	gHeadLocomotionPoseValid = false;
	gHeadLocomotionYaw = 0.0f;
	gHolsterHeadForwardValid = false;
}

void DestroyRuntime()
{
	DestroySessionResources();
	if(gActions.set) xrDestroyActionSet(gActions.set);
	gActions = Actions();
	if(gInstance) xrDestroyInstance(gInstance);
	gInstance = XR_NULL_HANDLE;
	gSystemId = XR_NULL_SYSTEM_ID;
	gExitRequested = false;
}

bool RuntimeSupportsExtension(const char *extensionName)
{
	uint32 count = 0;
	if(!XrOk(xrEnumerateInstanceExtensionProperties(nil, 0, &count, nil),
	   "xrEnumerateInstanceExtensionProperties(count)"))
		return false;
	std::vector<XrExtensionProperties> properties(count);
	for(uint32 i = 0; i < count; i++)
		properties[i] = { XR_TYPE_EXTENSION_PROPERTIES };
	if(count > 0 &&
	   !XrOk(xrEnumerateInstanceExtensionProperties(nil, count, &count,
	   properties.data()), "xrEnumerateInstanceExtensionProperties"))
		return false;
	for(uint32 i = 0; i < count; i++)
		if(strcmp(properties[i].extensionName, extensionName) == 0)
			return true;
	return false;
}

bool InitializeRuntime()
{
	LoadVrSettings();
	if(gInstance)
		return true;
	VrLog("InitializeRuntime begin backend=D3D12\n");
#ifdef RW_D3D12
	const char *graphicsExtension = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
#else
	const char *graphicsExtension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#endif
	std::vector<const char*> extensions;
	extensions.push_back(graphicsExtension);
	gGenericControllerExtensionEnabled =
		RuntimeSupportsExtension(XR_KHR_GENERIC_CONTROLLER_EXTENSION_NAME);
	if(gGenericControllerExtensionEnabled)
		extensions.push_back(XR_KHR_GENERIC_CONTROLLER_EXTENSION_NAME);
	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy(info.applicationInfo.applicationName, "Vice City VR");
	info.applicationInfo.applicationVersion = 1;
#ifdef RW_D3D12
	strcpy(info.applicationInfo.engineName, "reVC librw D3D12");
#else
	strcpy(info.applicationInfo.engineName, "reVC librw GL3");
#endif
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	info.enabledExtensionCount = (uint32)extensions.size();
	info.enabledExtensionNames = extensions.data();
	if(!XrOk(xrCreateInstance(&info, &gInstance), "xrCreateInstance"))
		return false;
	XrInstanceProperties runtimeProperties = { XR_TYPE_INSTANCE_PROPERTIES };
	if(XrOk(xrGetInstanceProperties(gInstance, &runtimeProperties),
	   "xrGetInstanceProperties")){
		VrLog("OpenXR runtime=%s version=%u.%u.%u\n",
			runtimeProperties.runtimeName,
			(unsigned)XR_VERSION_MAJOR(runtimeProperties.runtimeVersion),
			(unsigned)XR_VERSION_MINOR(runtimeProperties.runtimeVersion),
			(unsigned)XR_VERSION_PATCH(runtimeProperties.runtimeVersion));
		// SteamVR throttles quad-only sessions, while the Oculus OpenXR runtime
		// can retain the last quad image when gameplay switches to a cutscene.
		// Render cinema through the eye projection swapchains on both runtimes.
		// Read the setting from the game directory because launchers may use an
		// arbitrary working directory.
		gRuntimeNeedsCinemaKeepAlive = false;
		const bool cinemaProjectionRuntime =
			strstr(runtimeProperties.runtimeName, "SteamVR") != nil ||
			strstr(runtimeProperties.runtimeName, "Oculus") != nil;
		gCinemaTheaterMode = cinemaProjectionRuntime &&
			GetPrivateProfileIntA("VR", "CinemaProjection", 1,
				GetVrSettingsPath()) != 0;
		if(gCinemaTheaterMode)
			VrLog("Cinema theater projection enabled for %s "
				"(disable with [VR] CinemaProjection=0 in %s)\n",
				runtimeProperties.runtimeName,
				GetVrSettingsPath());
		else if(cinemaProjectionRuntime)
			VrLog("Cinema theater projection disabled by CinemaProjection=0; "
				"the runtime quad path will be used\n");
	}
	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if(!XrOk(xrGetSystem(gInstance, &systemInfo, &gSystemId), "xrGetSystem")){
		DestroyRuntime();
		return false;
	}
	if(!CreateActions()){
		DestroyRuntime();
		return false;
	}
	debug("[OpenXR] Runtime initialized (loader API 1.1, requested API 1.0, generic controller %s)\n",
		gGenericControllerExtensionEnabled ? "enabled" : "unavailable");
	VrLog("InitializeRuntime ok system=%llu\n", (unsigned long long)gSystemId);
	return true;
}

bool ChooseColorFormat()
{
	uint32_t count = 0;
	if(!XrOk(xrEnumerateSwapchainFormats(gSession, 0, &count, nil),
		"xrEnumerateSwapchainFormats(count)"))
		return false;
	std::vector<int64_t> formats(count);
	if(!XrOk(xrEnumerateSwapchainFormats(gSession, count, &count, formats.data()),
		"xrEnumerateSwapchainFormats"))
		return false;
#ifdef RW_D3D12
	VrLog("Swapchain formats count=%u", count);
	for(uint32 i = 0; i < count; i++)
		VrLog(" %lld", (long long)formats[i]);
	VrLog("\n");
	// The game produces display-referred (sRGB-encoded) colour values. Asking the
	// compositor for a linear UNORM swapchain makes those values look washed out.
	const int64_t preferred[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_R8G8B8A8_UNORM };
#else
	const int64_t preferred[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
#endif
	for(uint32 p = 0; p < ARRAY_SIZE(preferred); p++)
		for(uint32 i = 0; i < count; i++)
			if(formats[i] == preferred[p]){ gColorFormat = preferred[p]; return true; }
	debug("[OpenXR] Runtime exposes no compatible RGBA8 swapchain format\n");
	return false;
}

bool CreateSession()
{
	if(!InitializeRuntime())
		return false;
#ifdef RW_D3D12
	PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nil;
	if(!XrOk(xrGetInstanceProcAddr(gInstance, "xrGetD3D12GraphicsRequirementsKHR",
		(PFN_xrVoidFunction*)&getRequirements), "xrGetD3D12GraphicsRequirementsKHR") || !getRequirements)
		return false;
	XrGraphicsRequirementsD3D12KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
	if(!XrOk(getRequirements(gInstance, gSystemId, &requirements), "D3D12 graphics requirements"))
		return false;
	ID3D12Device *device = rw::d3d12::getDevice();
	ID3D12CommandQueue *queue = rw::d3d12::getCommandQueue();
	if(!device || !queue){
		debug("[OpenXR] librw D3D12 device or queue is unavailable\n");
		return false;
	}
	const LUID deviceLuid = device->GetAdapterLuid();
	VrLog("Graphics requirements feature=%u runtimeLuid=%08X:%08X deviceLuid=%08X:%08X\n",
		(unsigned)requirements.minFeatureLevel,
		(unsigned)requirements.adapterLuid.HighPart, (unsigned)requirements.adapterLuid.LowPart,
		(unsigned)deviceLuid.HighPart, (unsigned)deviceLuid.LowPart);
	if(deviceLuid.HighPart != requirements.adapterLuid.HighPart ||
	   deviceLuid.LowPart != requirements.adapterLuid.LowPart){
		debug("[OpenXR] D3D12 device does not use the runtime-requested adapter\n");
		return false;
	}
	XrGraphicsBindingD3D12KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
	binding.device = device;
	binding.queue = queue;
#else
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nil;
	if(!XrOk(xrGetInstanceProcAddr(gInstance, "xrGetOpenGLGraphicsRequirementsKHR",
		(PFN_xrVoidFunction*)&getRequirements), "xrGetOpenGLGraphicsRequirementsKHR") || !getRequirements)
		return false;
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if(!XrOk(getRequirements(gInstance, gSystemId, &requirements), "OpenGL graphics requirements"))
		return false;
	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if(!binding.hDC || !binding.hGLRC){
		debug("[OpenXR] No current Win32 OpenGL context\n");
		return false;
	}
#endif
	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = gSystemId;
	if(!XrOk(xrCreateSession(gInstance, &sessionInfo, &gSession), "xrCreateSession"))
		return false;
	VrLog("xrCreateSession ok\n");
	if(!ChooseColorFormat()){
		DestroySessionResources();
		return false;
	}
	uint32_t viewCount = 0;
	if(!XrOk(xrEnumerateViewConfigurationViews(gInstance, gSystemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, EYE_COUNT, &viewCount, gViewConfig),
		"xrEnumerateViewConfigurationViews") || viewCount != EYE_COUNT){
		DestroySessionResources();
		return false;
	}
	// Headsets with canted panels report a different recommended image size per
	// eye; Pimax asks for 3136 wide on one eye and 3330 on the other. The D3D12
	// single-pass path packs both eyes into one double-wide target and rejects
	// mismatched eyes, so session creation failed and retried forever with no
	// picture in the headset. Give both eyes the larger of the two sizes, kept
	// within what the runtime accepts. Each eye still renders through its own
	// projection from xrLocateViews; only the image dimensions are shared.
	uint32_t eyeWidth = 0, eyeHeight = 0;
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(gViewConfig[eye].recommendedImageRectWidth > eyeWidth)
			eyeWidth = gViewConfig[eye].recommendedImageRectWidth;
		if(gViewConfig[eye].recommendedImageRectHeight > eyeHeight)
			eyeHeight = gViewConfig[eye].recommendedImageRectHeight;
	}
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(gViewConfig[eye].maxImageRectWidth > 0 &&
		   eyeWidth > gViewConfig[eye].maxImageRectWidth)
			eyeWidth = gViewConfig[eye].maxImageRectWidth;
		if(gViewConfig[eye].maxImageRectHeight > 0 &&
		   eyeHeight > gViewConfig[eye].maxImageRectHeight)
			eyeHeight = gViewConfig[eye].maxImageRectHeight;
	}
	if(gViewConfig[0].recommendedImageRectWidth !=
	   gViewConfig[1].recommendedImageRectWidth ||
	   gViewConfig[0].recommendedImageRectHeight !=
	   gViewConfig[1].recommendedImageRectHeight)
		VrLog("Per-eye recommended sizes differ (%ux%u and %ux%u); using %ux%u for both\n",
			gViewConfig[0].recommendedImageRectWidth,
			gViewConfig[0].recommendedImageRectHeight,
			gViewConfig[1].recommendedImageRectWidth,
			gViewConfig[1].recommendedImageRectHeight,
			eyeWidth, eyeHeight);
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(!CreateSwapchain(gEye[eye].swapchain, (int)eyeWidth, (int)eyeHeight)){
			DestroySessionResources(); return false;
		}
	}
	const int maximumRenderScaleIndex = MaxSupportedRenderScaleIndex();
	if(gRenderScaleIndex > maximumRenderScaleIndex){
		gRenderScaleIndex = maximumRenderScaleIndex;
		gRenderScale = gRenderScaleOptions[gRenderScaleIndex];
		SaveVrSetting("RenderScale", gRenderScaleIndex);
		debug("[OpenXR] Saved render scale exceeded the double-wide D3D12 target limit; capped at %.0f%%\n",
			gRenderScale*100.0f);
	}
	if(!ReplaceStereoRenderTargets(gRenderScale, false)){
		DestroySessionResources(); return false;
	}
#ifdef RW_D3D12
	rw::d3d12::setFixedFoveatedRenderingProfile(gFixedFoveatedProfile);
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	debug("[OpenXR] D3D12 VRS Tier %u: %s, tile %u, profile %u\n",
		foveatedInfo.tier, foveatedInfo.supported ? "fixed foveation ready" : "unsupported",
		foveatedInfo.tileSize, foveatedInfo.profile);
	VrLog("D3D12 VRS tier=%u supported=%d additionalRates=%d tile=%u profile=%u\n",
		foveatedInfo.tier, foveatedInfo.supported, foveatedInfo.additionalRates,
		foveatedInfo.tileSize, foveatedInfo.profile);
#endif
	if(!CreateSwapchain(gHudSwapchain, VR_HUD_WIDTH, VR_HUD_HEIGHT)){
		DestroySessionResources(); return false;
	}
	if(!CreateSwapchain(gDebugSwapchain, VR_DEBUG_WIDTH, VR_DEBUG_HEIGHT)){
		DestroySessionResources(); return false;
	}
	if(!CreateSwapchain(gVrMenuSwapchain, VR_MENU_WIDTH, VR_MENU_HEIGHT)){
		DestroySessionResources(); return false;
	}
	// SteamVR can invalidate the entire session when xrCreateSwapchain is
	// called between submitted frames.  The startup movies and the frontend
	// use different source resolutions, so allocate one cinema swapchain now,
	// before the session starts, and keep it for the complete session.
	int cinemaWidth = VR_STARTUP_WIDTH;
	int cinemaHeight = VR_STARTUP_HEIGHT;
#ifdef RW_D3D12
	int presentWidth = 0, presentHeight = 0;
	rw::d3d12::getPresentSize(&presentWidth, &presentHeight);
	if(presentWidth > cinemaWidth) cinemaWidth = presentWidth;
	if(presentHeight > cinemaHeight) cinemaHeight = presentHeight;
#endif
	if(RsGlobal.maximumWidth > cinemaWidth) cinemaWidth = RsGlobal.maximumWidth;
	if(RsGlobal.maximumHeight > cinemaHeight) cinemaHeight = RsGlobal.maximumHeight;
	if(!CreateSwapchain(gCinemaSwapchain, cinemaWidth, cinemaHeight)){
		DestroySessionResources(); return false;
	}
	VrLog("Cinema swapchain fixed at %dx%d for this session\n",
		cinemaWidth, cinemaHeight);
	gHudColor = RwRasterCreate(VR_HUD_WIDTH, VR_HUD_HEIGHT, 32,
		rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
	gHudDepth = RwRasterCreate(VR_HUD_WIDTH, VR_HUD_HEIGHT, 0, rwRASTERTYPEZBUFFER);
	if(!gHudColor || !gHudDepth){ DestroySessionResources(); return false; }
	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if(!XrOk(xrCreateReferenceSpace(gSession, &spaceInfo, &gLocalSpace), "xrCreateReferenceSpace(local)")){
		DestroySessionResources(); return false;
	}
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if(!XrOk(xrCreateReferenceSpace(gSession, &spaceInfo, &gViewSpace), "xrCreateReferenceSpace(view)")){
		DestroySessionResources(); return false;
	}
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &gActions.set;
	if(!XrOk(xrAttachSessionActionSets(gSession, &attach), "xrAttachSessionActionSets")){
		DestroySessionResources(); return false;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++){
		XrActionSpaceCreateInfo handSpaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		handSpaceInfo.action = gActions.gripPose;
		handSpaceInfo.subactionPath = gActions.hands[hand];
		handSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
		if(!XrOk(xrCreateActionSpace(gSession, &handSpaceInfo, &gActions.gripSpace[hand]),
			"xrCreateActionSpace(hand grip)")){
			DestroySessionResources(); return false;
		}
		handSpaceInfo.action = gActions.aimPose;
		if(!XrOk(xrCreateActionSpace(gSession, &handSpaceInfo, &gActions.aimSpace[hand]),
			"xrCreateActionSpace(hand aim)")){
			DestroySessionResources(); return false;
		}
	}
#ifndef RW_D3D12
	glGenFramebuffers(1, &gCopyFramebuffer);
	if(!CreateFxaaProgram())
		debug("[OpenXR] FXAA unavailable; using linear supersample resolve\n");
#else
	// Compile outside gameplay so the first tracked-hand frame cannot hitch.
	// Failure is deliberately non-fatal: main.cpp keeps the established in-world
	// hand pass whenever this optional post-temporal path is unavailable.
	gTrackedForegroundResolveAvailable =
		EnsureTrackedForegroundResolvePipeline();
	if(!gTrackedForegroundResolveAvailable)
		debug("[OpenXR] Tracked foreground resolve unavailable; using world-pass hands\n");
#endif
#ifdef RW_D3D12
	const char *backendName = "D3D12";
#else
	const char *backendName = "GL3";
#endif
	debug("[OpenXR] %s stereo session created: submit %dx%d, render %dx%d (%.0f%%)\n",
		backendName,
		gEye[0].swapchain.width, gEye[0].swapchain.height,
		gEye[0].renderWidth, gEye[0].renderHeight, gRenderScale*100.0f);
	VrLog("CreateSession ok eye=%dx%d hud=%dx%d\n",
		gEye[0].renderWidth, gEye[0].renderHeight, VR_HUD_WIDTH, VR_HUD_HEIGHT);
#ifdef RW_D3D12
	// xrWaitFrame is the authoritative limiter while VR is active.  Do not
	// block a second time on the desktop window's vertical blank.
	rw::d3d12::setPresentInterval(0);
	// Fresh session: nothing has ever been rendered into the eye images, so
	// the keep-alive cinema projection must clear the full rotation first.
	gCinemaEyesEverCleared = false;
	gCinemaEyeClearsPending = (uint32)Max(gEye[0].swapchain.images.size(),
		gEye[1].swapchain.images.size());
#endif
	return true;
}

void LogCurrentInteractionProfiles()
{
	if(!gSession)
		return;
	const char *handNames[EYE_COUNT] = { "left", "right" };
	for(int hand = 0; hand < EYE_COUNT; hand++){
		XrInteractionProfileState state = {
			XR_TYPE_INTERACTION_PROFILE_STATE
		};
		if(XR_FAILED(xrGetCurrentInteractionProfile(gSession,
		   gActions.hands[hand], &state)))
			continue;
		char profile[XR_MAX_PATH_LENGTH] = "none";
		if(state.interactionProfile != XR_NULL_PATH){
			uint32 written = 0;
			if(XR_FAILED(xrPathToString(gInstance,
			   state.interactionProfile, sizeof(profile), &written, profile)))
				strcpy(profile, "unknown");
		}
		debug("[OpenXR] %s controller profile: %s\n",
			handNames[hand], profile);
		VrLog("%s controller profile: %s\n", handNames[hand], profile);
	}
}

bool PollEvents()
{
	if(!gInstance)
		return false;
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while(xrPollEvent(gInstance, &event) == XR_SUCCESS){
		if(event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){
			const XrEventDataSessionStateChanged *changed =
				(const XrEventDataSessionStateChanged*)&event;
			gSessionState = changed->state;
			VrLog("Session state=%d\n", (int)changed->state);
			if(changed->state <= XR_SESSION_STATE_SYNCHRONIZED &&
			   gSessionRunning && gHeadsetDormantSinceMs == 0)
				gHeadsetDormantSinceMs = GetTickCount64();
			if(changed->state == XR_SESSION_STATE_FOCUSED){
				if(gHeadsetDormantSinceMs != 0 &&
				   GetTickCount64()-gHeadsetDormantSinceMs >= 30000){
					gRecenterRequested = true;
					gCinemaAnchorValid = false;
					VrLog("Headset worn again after a long break; "
						"anchors re-latch\n");
				}
				gHeadsetDormantSinceMs = 0;
			}
			if(changed->state == XR_SESSION_STATE_READY && !gSessionRunning){
				XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if(XrOk(xrBeginSession(gSession, &begin), "xrBeginSession"))
					gSessionRunning = true;
			}else if(changed->state == XR_SESSION_STATE_STOPPING && gSessionRunning){
				xrEndSession(gSession);
				gSessionRunning = false;
				ResetImmersiveDrivingInteraction();
				gWasSubmitting = false;
				gHeadLocomotionPoseValid = false;
				gHeadLocomotionYaw = 0.0f;
				gHolsterHeadForwardValid = false;
			}else if(changed->state == XR_SESSION_STATE_EXITING ||
			          changed->state == XR_SESSION_STATE_LOSS_PENDING){
				gExitRequested = true;
			}
		}else if(event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED){
			LogCurrentInteractionProfiles();
		}else if(event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING){
			const XrEventDataReferenceSpaceChangePending *changed =
				(const XrEventDataReferenceSpaceChangePending*)&event;
			if(changed->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL){
				// Meta's system recenter changes LOCAL underneath the game. Rebuild
				// our head-anchored space on the next stereo frame so the saved
				// positional origin cannot pull the camera away from Tommy.
				gRecenterRequested = true;
				gCinemaAnchorValid = false;
				gHeadLocomotionPoseValid = false;
				gHeadLocomotionYaw = 0.0f;
				gHolsterHeadForwardValid = false;
				VrLog("LOCAL reference space changed; gameplay head anchor reset\n");
			}
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
	if(gExitRequested){
		DestroyRuntime();
		gRetryFrames = 300;
		return false;
	}
	return true;
}

bool EnsureSession()
{
	if(gFlatModeEnabled){
		static bool logged;
		if(!logged){
			logged = true;
			VrLog("Flat mode enabled via vr_settings.ini; VR session disabled\n");
		}
		return false;
	}
	if(!gSession){
		if(gRetryFrames > 0){ --gRetryFrames; return false; }
		if(!CreateSession()){
			VrLog("CreateSession failed; retry delayed\n");
			gRetryFrames = gSessionRetryDelay;
			gSessionRetryDelay = Min(gSessionRetryDelay*2, 18000);
			return false;
		}
		gSessionRetryDelay = 300;
	}
	return PollEvents();
}

bool BeginXrFrame()
{
	if(gFrameBegun)
		return false;
	if(gRuntimeRecoveryPending){
		VrLog("OpenXR recovery begin\n");
		DestroyRuntime();
		// Desktop present is vsynced again by DestroySessionResources, so this is
		// a real, bounded pause rather than the previous hot retry loop.
		gRetryFrames = 120;
		gRuntimeRecoveryPending = false;
		gCurrentFramePhase = "recovery";
		return false;
	}
	// Event polling plus the two deferred rebuilds. ApplyPendingTemporalAaRelease
	// flushes the GPU completely before destroying DLAA or FSR2 resources, which
	// is exactly the kind of stall that was showing up as unattributed frame time.
	PerfBeginPhase(PERF_PHASE_SESSION_UPKEEP);
	if(!EnsureSession()){
		PerfEndPhase(PERF_PHASE_SESSION_UPKEEP);
		return false;
	}
	ApplyPendingTemporalAaRelease();
	ApplyPendingRenderScale();
	PerfEndPhase(PERF_PHASE_SESSION_UPKEEP);
	if(!gSessionRunning)
		return false;
	XrFrameWaitInfo wait = { XR_TYPE_FRAME_WAIT_INFO };
	gFrameState = { XR_TYPE_FRAME_STATE };
	double timingStart = PerfNowMs();
	XrResult result = xrWaitFrame(gSession, &wait, &gFrameState);
	gLastXrWaitFrameMs = (float)(PerfNowMs() - timingStart);
	if(gPerfFrameStarted)
		gPerfCurrent.xrWaitFrameMs += gLastXrWaitFrameMs;
	if(!XrOk(result, "xrWaitFrame")){
		ScheduleRuntimeRecovery("xrWaitFrame", result);
		return false;
	}
	XrFrameBeginInfo begin = { XR_TYPE_FRAME_BEGIN_INFO };
	timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
	result = xrBeginFrame(gSession, &begin);
	if(gPerfFrameStarted)
		gPerfCurrent.xrBeginFrameMs += (float)(PerfNowMs() - timingStart);
	if(!XrOk(result, "xrBeginFrame")){
		ScheduleRuntimeRecovery("xrBeginFrame", result);
		return false;
	}
	if(gDebugVisible){
		const double now=PerfNowMs();
		if(gDebugPreviousFrameMs>0.0){
			const float frameMs=(float)(now-gDebugPreviousFrameMs);
			gDebugSmoothedFrameMs=gDebugSmoothedFrameMs>0.0f ?
				gDebugSmoothedFrameMs*0.9f+frameMs*0.1f : frameMs;
			gDebugFps=gDebugSmoothedFrameMs>0.0f ?
				(int)(1000.0f/gDebugSmoothedFrameMs+0.5f) : 0;
		}
		gDebugPreviousFrameMs=now;
	}else{
		gDebugPreviousFrameMs=0.0;
	}
	gFrameBegun = true;
	if(gVrLoggedFrames < 10 || (gFrameState.shouldRender && gVrLoggedRenderableFrames < 10))
		VrLog("Begin frame %d shouldRender=%u predicted=%lld\n", gVrLoggedFrames,
			(unsigned)gFrameState.shouldRender, (long long)gFrameState.predictedDisplayTime);
	return true;
}

bool EndXrFrame(const XrCompositionLayerBaseHeader *const *layers, uint32_t count)
{
	if(!gFrameBegun)
		return false;
	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	end.displayTime = gFrameState.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = count;
	end.layers = layers;
	const double timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
	XrResult result = xrEndFrame(gSession, &end);
	if(gPerfFrameStarted)
		gPerfCurrent.xrEndFrameMs += (float)(PerfNowMs() - timingStart);
	const bool ok = XrOk(result, "xrEndFrame");
	if(!ok)
		ScheduleRuntimeRecovery("xrEndFrame", result);
	if(gVrLoggedFrames < 10 || (count > 0 && gVrLoggedRenderableFrames < 10)){
		VrLog("End frame %d layers=%u ok=%d\n", gVrLoggedFrames, count, ok ? 1 : 0);
		if(count > 0)
			gVrLoggedRenderableFrames++;
	}
	gVrLoggedFrames++;
	gFrameBegun = false;
	// The compositor is done with anything submitted several frames ago, so
	// this is where retired swapchains become safe to destroy.
	UpdateRetiredSwapchains();
	if(ok && count > 0) gWasSubmitting = true;
	return ok;
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

XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	const XrVector3f t = { 2.0f*(q.y*v.z-q.z*v.y), 2.0f*(q.z*v.x-q.x*v.z),
		2.0f*(q.x*v.y-q.y*v.x) };
	const XrVector3f cross = { q.y*t.z-q.z*t.y, q.z*t.x-q.x*t.z, q.x*t.y-q.y*t.x };
	return { v.x+q.w*t.x+cross.x, v.y+q.w*t.y+cross.y, v.z+q.w*t.z+cross.z };
}

void ApplyCinemaQuadPose(XrCompositionLayerQuad *layer)
{
	if(!layer)
		return;
	if(!gCinemaAnchorValid && gViewSpace && gLocalSpace){
		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
		if(XrOk(xrLocateSpace(gViewSpace, gLocalSpace,
		   gFrameState.predictedDisplayTime, &location), "xrLocateSpace(cinema)") &&
		   (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
		   (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0){
			const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
			const XrVector3f headForward = Rotate(
				location.pose.orientation, localForward);
			const float yaw = atan2f(-headForward.x, -headForward.z);
			gCinemaAnchorPose = {};
			gCinemaAnchorPose.orientation.y = sinf(yaw*0.5f);
			gCinemaAnchorPose.orientation.w = cosf(yaw*0.5f);
			const XrVector3f localScreenOffset = { 0.0f, 0.0f, -2.0f };
			const XrVector3f screenOffset = Rotate(
				gCinemaAnchorPose.orientation, localScreenOffset);
			gCinemaAnchorPose.position.x = location.pose.position.x+screenOffset.x;
			gCinemaAnchorPose.position.y = location.pose.position.y;
			gCinemaAnchorPose.position.z = location.pose.position.z+screenOffset.z;
			gCinemaAnchorValid = true;
		}
	}
	if(gCinemaAnchorValid){
		layer->space = gLocalSpace;
		layer->pose = gCinemaAnchorPose;
		return;
	}
	// Tracking can be unavailable for the very first submitted frame. Keep that
	// single frame visible and retry the world-locked anchor on the next one.
	// A PERSISTENT fallback means the theater screen rides the player's head;
	// count it so the pace telemetry can prove or clear this on player logs.
	gCinemaViewFallbackFrames++;
	layer->space = gViewSpace;
	layer->pose = {};
	layer->pose.orientation.w = 1.0f;
	layer->pose.position.z = -2.0f;
}

CVector ToGameVector(const XrVector3f &v)
{
	return gBaseCamera.GetRight()*(-v.x) + gBaseCamera.GetUp()*v.y + gBaseCamera.GetForward()*(-v.z);
}

CVector ToTrackingVector(const CVector &v)
{
	// Exact inverse of ToGameVector for the orthonormal gameplay-camera basis.
	// Steering references live in OpenXR local space so virtual vehicle motion,
	// suspension and camera following cannot feed back into controller input.
	return CVector(-DotProduct(v, gBaseCamera.GetRight()),
		DotProduct(v, gBaseCamera.GetUp()),
		-DotProduct(v, gBaseCamera.GetForward()));
}

CVector ToTrackingPosition(const CVector &worldPosition)
{
	return ToTrackingVector(worldPosition-gBaseCamera.GetPosition());
}

CVector GetRawTrackedHandPosition(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT || !gTrackedHandPoseValid[hand])
		return CVector(0.0f, 0.0f, 0.0f);
	const XrVector3f &position = gTrackedHandPose[hand].position;
	return CVector(position.x, position.y, position.z);
}

CVector RotateAroundAxis(const CVector &vector, const CVector &axis, float angle)
{
	const float cosine = cosf(angle);
	const float sine = sinf(angle);
	return vector*cosine + CrossProduct(axis, vector)*sine +
		axis*(DotProduct(axis, vector)*(1.0f-cosine));
}

float WrapCalibrationRadians(float angle)
{
	while(angle > PI) angle -= 2.0f*PI;
	while(angle < -PI) angle += 2.0f*PI;
	return angle;
}

int QuantizeCalibrationValue(float value, int minimum, int maximum)
{
	const int rounded = (int)(value >= 0.0f ? value+0.5f : value-0.5f);
	return Min(Max(rounded, minimum), maximum);
}

void BuildRawWeaponAimFrame(int hand, CMatrix *frame)
{
	frame->SetUnity();
	if(hand < 0 || hand >= EYE_COUNT || !gTrackedHandAimPoseValid[hand])
		return;
	const XrPosef &pose = gTrackedHandAimPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	frame->GetRight() = ToGameVector(Rotate(pose.orientation, localRight));
	frame->GetForward() = ToGameVector(Rotate(pose.orientation, localForward));
	frame->GetUp() = ToGameVector(Rotate(pose.orientation, localUp));
	frame->GetRight().Normalise();
	frame->GetForward().Normalise();
	frame->GetUp().Normalise();
	frame->GetPosition() = gBaseCamera.GetPosition()+ToGameVector(pose.position);
}

void BuildWeaponModelCalibrationMatrix(int hand,
	const WeaponCalibration &calibration, CMatrix *matrix)
{
	CVector position(0.0f, 0.0f, 0.0f);
	CVector right(1.0f, 0.0f, 0.0f);
	CVector forward(0.0f, 1.0f, 0.0f);
	CVector up(0.0f, 0.0f, 1.0f);
	if(hand >= 0 && hand < EYE_COUNT && gTrackedHandPoseValid[hand]){
		const XrPosef &gripPose = gTrackedHandPose[hand];
		const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
		const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
		const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
		const CVector gripRight = ToGameVector(Rotate(
			gripPose.orientation, localRight));
		const CVector gripUp = ToGameVector(Rotate(
			gripPose.orientation, localUp));
		const CVector gripForward = ToGameVector(Rotate(
			gripPose.orientation, localForward));
		CMatrix rawAim;
		BuildRawWeaponAimFrame(hand, &rawAim);
		forward = gTrackedHandAimPoseValid[hand] ?
			rawAim.GetForward() : gripUp;
		forward.Normalise();
		// This is the exact tracked-hand basis used by RenderVrTrackedWeapon.
		// Keeping it here makes the alignment delta account for the fixed OpenXR
		// grip/aim-space relationship instead of assuming those poses coincide.
		const int modelHand = 1-hand;
		up = gripRight*(modelHand == 0 ? 1.0f : -1.0f);
		up -= forward*DotProduct(up, forward);
		if(up.MagnitudeSqr() < 0.0001f)
			up = gripRight*(modelHand == 0 ? 1.0f : -1.0f);
		up.Normalise();
		right = CrossProduct(up, forward);
		right.Normalise();
		if(DotProduct(right, gripForward) < 0.0f)
			right = right*-1.0f;
		position = gBaseCamera.GetPosition()+ToGameVector(gripPose.position);
	}

	matrix->SetUnity();
	matrix->GetRight() = right*-1.0f;
	matrix->GetForward() = forward*-1.0f;
	matrix->GetUp() = up;
	matrix->GetPosition() = position+
		right*((float)calibration.offsetX/200.0f)+
		forward*((float)calibration.offsetY/200.0f)+
		up*((float)calibration.offsetZ/200.0f);
	CMatrix baseAdjustment, rotationX, rotationY, rotationZ;
	baseAdjustment.SetRotate(DEGTORAD(90.0f), 0.0f, DEGTORAD(-90.0f));
	rotationX.SetRotateX(DEGTORAD((float)calibration.rotationX/
		WEAPON_CALIBRATION_VALUE_SCALE));
	rotationY.SetRotateY(DEGTORAD((float)calibration.rotationY/
		WEAPON_CALIBRATION_VALUE_SCALE));
	rotationZ.SetRotateZ(DEGTORAD((float)calibration.rotationZ/
		WEAPON_CALIBRATION_VALUE_SCALE));
	*matrix = *matrix*baseAdjustment*rotationX*rotationY*rotationZ;
}

void BuildWeaponAimCalibrationMatrix(int hand,
	const WeaponCalibration &calibration, CMatrix *matrix)
{
	CMatrix base;
	BuildRawWeaponAimFrame(hand, &base);
	CVector aimRight = base.GetRight();
	CVector aimForward = base.GetForward();
	CVector aimUp = base.GetUp();
	aimForward.Normalise();
	aimUp -= aimForward*DotProduct(aimUp, aimForward);
	aimUp.Normalise();
	aimRight -= aimForward*DotProduct(aimRight, aimForward);
	aimRight -= aimUp*DotProduct(aimRight, aimUp);
	aimRight.Normalise();
	const float pitch = DEGTORAD((float)calibration.aimRotationX/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float yaw = DEGTORAD((float)calibration.aimRotationY/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float roll = DEGTORAD((float)calibration.aimRotationZ/
		WEAPON_CALIBRATION_VALUE_SCALE);
	if(pitch != 0.0f){
		aimForward = RotateAroundAxis(aimForward, aimRight, pitch);
		aimUp = RotateAroundAxis(aimUp, aimRight, pitch);
	}
	if(yaw != 0.0f){
		aimForward = RotateAroundAxis(aimForward, aimUp, yaw);
		aimRight = RotateAroundAxis(aimRight, aimUp, yaw);
	}
	if(roll != 0.0f){
		aimRight = RotateAroundAxis(aimRight, aimForward, roll);
		aimUp = RotateAroundAxis(aimUp, aimForward, roll);
	}
	matrix->SetUnity();
	matrix->GetRight() = aimRight;
	matrix->GetForward() = aimForward;
	matrix->GetUp() = aimUp;
	const CVector muzzleLocal(
		(float)calibration.aimOffsetX/200.0f,
		0.18f+(float)calibration.aimOffsetZ/200.0f,
		(float)calibration.aimOffsetY/200.0f);
	matrix->GetPosition() = base.GetPosition()+Multiply3x3(*matrix, muzzleLocal);
}

float CalibrationAngleCost(float pitch, float yaw, float roll,
	const WeaponCalibration &reference)
{
	const float referencePitch = DEGTORAD((float)reference.aimRotationX/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float referenceYaw = DEGTORAD((float)reference.aimRotationY/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float referenceRoll = DEGTORAD((float)reference.aimRotationZ/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float dp = WrapCalibrationRadians(pitch-referencePitch);
	const float dy = WrapCalibrationRadians(yaw-referenceYaw);
	const float dr = WrapCalibrationRadians(roll-referenceRoll);
	return dp*dp+dy*dy+dr*dr;
}

bool ExtractWeaponAimCalibration(int hand, const CMatrix &worldAim,
	const WeaponCalibration &reference, WeaponCalibration *calibration)
{
	if(!calibration)
		return false;
	CMatrix base;
	BuildRawWeaponAimFrame(hand, &base);
	const float baseDeterminant = DotProduct(base.GetRight(),
		CrossProduct(base.GetForward(), base.GetUp()));
	const float basisHandedness = baseDeterminant < 0.0f ? -1.0f : 1.0f;
	CMatrix localAim = Invert(base)*worldAim;
	localAim.Reorthogonalise();
	const CVector &localPosition = localAim.GetPosition();
	const float muzzleX = DotProduct(localPosition, localAim.GetRight());
	const float muzzleForward = DotProduct(localPosition,
		localAim.GetForward());
	const float muzzleUp = DotProduct(localPosition, localAim.GetUp());
	if(!_finite(muzzleX) || !_finite(muzzleForward) || !_finite(muzzleUp))
		return false;

	const float sineYaw = Min(Max(-localAim.GetForward().x, -1.0f), 1.0f);
	const float cosineYaw = sqrtf(Max(0.0f, 1.0f-sineYaw*sineYaw));
	float pitch, yaw, roll;
	if(cosineYaw > 0.0001f){
		const float localYawA = atan2f(sineYaw, cosineYaw);
		const float localPitchA = atan2f(localAim.GetForward().z,
			localAim.GetForward().y);
		const float localRollA = atan2f(localAim.GetUp().x,
			localAim.GetRight().x);
		const float localYawB = localYawA >= 0.0f ?
			PI-localYawA : -PI-localYawA;
		const float localPitchB = atan2f(-localAim.GetForward().z,
			-localAim.GetForward().y);
		const float localRollB = atan2f(-localAim.GetUp().x,
			-localAim.GetRight().x);
		const float pitchA = localPitchA*basisHandedness;
		const float yawA = localYawA*basisHandedness;
		const float rollA = localRollA*basisHandedness;
		const float pitchB = localPitchB*basisHandedness;
		const float yawB = localYawB*basisHandedness;
		const float rollB = localRollB*basisHandedness;
		if(CalibrationAngleCost(pitchA, yawA, rollA, reference) <=
		   CalibrationAngleCost(pitchB, yawB, rollB, reference)){
			pitch = pitchA;
			yaw = yawA;
			roll = rollA;
		}else{
			pitch = pitchB;
			yaw = yawB;
			roll = rollB;
		}
	}else{
		// At +/-90 degrees yaw the X-Z-Y representation has one free axis.
		// Preserve the previous split and distribute only the shortest constraint
		// correction, avoiding a sudden 180-degree menu jump at the singularity.
		float localPitch = DEGTORAD((float)reference.aimRotationX/
			WEAPON_CALIBRATION_VALUE_SCALE)*basisHandedness;
		float localRoll = DEGTORAD((float)reference.aimRotationZ/
			WEAPON_CALIBRATION_VALUE_SCALE)*basisHandedness;
		const float localYaw = sineYaw >= 0.0f ? PI*0.5f : -PI*0.5f;
		if(sineYaw >= 0.0f){
			const float required = atan2f(localAim.GetRight().z,
				localAim.GetRight().y);
			const float correction = WrapCalibrationRadians(
				required-(localPitch-localRoll))*0.5f;
			localPitch += correction;
			localRoll -= correction;
		}else{
			const float required = atan2f(-localAim.GetRight().z,
				localAim.GetUp().z);
			const float correction = WrapCalibrationRadians(
				required-(localPitch+localRoll))*0.5f;
			localPitch += correction;
			localRoll += correction;
		}
		pitch = localPitch*basisHandedness;
		yaw = localYaw*basisHandedness;
		roll = localRoll*basisHandedness;
	}

	calibration->aimOffsetX = QuantizeCalibrationValue(
		muzzleX*200.0f, -100, 100);
	calibration->aimOffsetY = QuantizeCalibrationValue(
		muzzleUp*200.0f, -100, 100);
	calibration->aimOffsetZ = QuantizeCalibrationValue(
		(muzzleForward-0.18f)*200.0f, -100, 100);
	calibration->aimRotationX = QuantizeCalibrationValue(
		RADTODEG(WrapCalibrationRadians(pitch))*
		WEAPON_CALIBRATION_VALUE_SCALE, -360, 360);
	calibration->aimRotationY = QuantizeCalibrationValue(
		RADTODEG(WrapCalibrationRadians(yaw))*
		WEAPON_CALIBRATION_VALUE_SCALE, -360, 360);
	calibration->aimRotationZ = QuantizeCalibrationValue(
		RADTODEG(WrapCalibrationRadians(roll))*
		WEAPON_CALIBRATION_VALUE_SCALE, -360, 360);
	return true;
}

void MirrorWeaponAimRelative(const CMatrix &canonical, CMatrix *mirrored)
{
	CMatrix reflection;
	reflection.SetUnity();
	reflection.GetRight().x = -1.0f;
	// The independently-authored LEFT model frame already supplies the mirrored
	// world output (W_left = P*W_right). Only reflect the relative aim frame's
	// local input axis: R_left = R_right*S. Left-multiplying would incorrectly
	// mirror the weapon-local muzzle translation, while S*R*S would preserve the
	// determinant and leave LEFT with the wrong aim handedness. This mapping is
	// involutive because (R*S)*S == R.
	*mirrored = canonical*reflection;
}

bool BuildAlignedWeaponAimMatrix(int hand, int weaponType,
	const WeaponCalibration &modelCalibration, CMatrix *aim)
{
	if(!aim || hand < 0 || hand >= EYE_COUNT)
		return false;
	CMatrix relative;
	if(!LoadActiveWeaponAimRelative(weaponType, &relative))
		return false;
	if(hand == 0){
		CMatrix mirrored;
		MirrorWeaponAimRelative(relative, &mirrored);
		relative = mirrored;
	}
	CMatrix weapon;
	BuildWeaponModelCalibrationMatrix(hand, modelCalibration, &weapon);
	*aim = weapon*relative;
	return _finite(aim->px) && _finite(aim->py) && _finite(aim->pz) &&
		aim->GetForward().MagnitudeSqr() > 0.5f;
}

int QuantizeAimRelative(float value)
{
	const float scaled = value*(float)WEAPON_AIM_ALIGNMENT_SCALE;
	return (int)(scaled >= 0.0f ? scaled+0.5f : scaled-0.5f);
}

void PersistWeaponAimRelative(int weaponType, const CMatrix &relative)
{
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	WriteWeaponCalibrationValue(section, 1, "AimAlignedTransformVersion",
		WEAPON_AIM_ALIGNMENT_VERSION);
	WriteWeaponCalibrationValue(section, 1, "AimAlignedTransformScale",
		WEAPON_AIM_ALIGNMENT_SCALE);
#define WRITE_AIM_RELATIVE(component, key) \
	WriteWeaponCalibrationValue(section, 1, key, \
		QuantizeAimRelative(relative.component))
	WRITE_AIM_RELATIVE(rx, "AimAlignedRightX");
	WRITE_AIM_RELATIVE(ry, "AimAlignedRightY");
	WRITE_AIM_RELATIVE(rz, "AimAlignedRightZ");
	WRITE_AIM_RELATIVE(fx, "AimAlignedForwardX");
	WRITE_AIM_RELATIVE(fy, "AimAlignedForwardY");
	WRITE_AIM_RELATIVE(fz, "AimAlignedForwardZ");
	WRITE_AIM_RELATIVE(ux, "AimAlignedUpX");
	WRITE_AIM_RELATIVE(uy, "AimAlignedUpY");
	WRITE_AIM_RELATIVE(uz, "AimAlignedUpZ");
	WRITE_AIM_RELATIVE(px, "AimAlignedPositionX");
	WRITE_AIM_RELATIVE(py, "AimAlignedPositionY");
	WRITE_AIM_RELATIVE(pz, "AimAlignedPositionZ");
#undef WRITE_AIM_RELATIVE
	// Commit marker is written last. A partially written transform is therefore
	// never treated as an enabled binding after interruption.
	WritePrivateProfileStringA(section, "AimAligned", "1",
		GetVrSettingsPath());
	const int set = (int)GetActiveWeaponModelSet();
	gWeaponAimRelative[set][weaponType] = relative;
	gWeaponAimRelativeLoaded[set][weaponType] = true;
	gWeaponAimRelativeEnabled[set][weaponType] = true;
}

void SaveCanonicalWeaponAimValues()
{
	SaveCurrentWeaponCalibrationValue("AimOffsetX", gWeaponAimOffsetXCm);
	SaveCurrentWeaponCalibrationValue("AimOffsetY", gWeaponAimOffsetYCm);
	SaveCurrentWeaponCalibrationValue("AimOffsetZ", gWeaponAimOffsetZCm);
	SaveCurrentWeaponCalibrationValue("AimRotationX", gWeaponAimRotationXDeg);
	SaveCurrentWeaponCalibrationValue("AimRotationY", gWeaponAimRotationYDeg);
	SaveCurrentWeaponCalibrationValue("AimRotationZ", gWeaponAimRotationZDeg);
}

void SaveCurrentWeaponAimAligned(bool aligned)
{
	SyncCurrentWeaponCalibration();
	const int weaponType = GetCalibrationWeaponType();
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return;
	char section[96];
	if(!GetCurrentWeaponCalibrationSection(section))
		return;
	if(aligned){
		if(!gTrackedHandPoseValid[1] || !gTrackedHandAimPoseValid[1])
			return;
		BackupVrSettings("aim_alignment_lock");
		// Materialise the effective canonical profile (including scale-1 legacy
		// migration) before capturing a relationship that will outlive the menu.
		SaveCurrentWeaponCalibrationValue("AimOffsetX", gWeaponAimOffsetXCm);
		const WeaponCalibration *effectiveRight =
			GetWeaponCalibration(1, weaponType);
		if(!effectiveRight)
			return;
		WeaponCalibration canonical = *effectiveRight;
		WeaponCalibration current;
		CaptureCurrentWeaponCalibration(current);
		CopyWeaponAim(current, canonical);
		CMatrix weapon, aim;
		BuildWeaponModelCalibrationMatrix(1, canonical, &weapon);
		BuildWeaponAimCalibrationMatrix(1, canonical, &aim);
		const CMatrix relative = Invert(weapon)*aim;
		PersistWeaponAimRelative(weaponType, relative);
		gWeaponAimAligned = true;
	}else{
		if(!gTrackedHandPoseValid[1] || !gTrackedHandAimPoseValid[1])
			return;
		const WeaponCalibration *right = GetWeaponCalibration(1, weaponType);
		if(!right)
			return;
		CMatrix effectiveAim;
		if(!BuildAlignedWeaponAimMatrix(1, weaponType, *right, &effectiveAim))
			return;
		WeaponCalibration current;
		CaptureCurrentWeaponCalibration(current);
		WeaponCalibration effective = current;
		if(!ExtractWeaponAimCalibration(1, effectiveAim, current, &effective))
			return;
		BackupVrSettings("aim_alignment_unlock");
		CopyWeaponAim(effective, current);
		ApplyCurrentWeaponCalibration(current);
		// Preserve the exact effective beam before releasing the relationship;
		// OFF therefore has no visible laser jump and resumes ordinary editing.
		SaveCanonicalWeaponAimValues();
		WritePrivateProfileStringA(section, "AimAligned", "0",
			GetVrSettingsPath());
		InvalidateWeaponAimRelative(weaponType, GetActiveWeaponModelSet());
		gWeaponAimAligned = false;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++)
		gTrackedAimCacheValid[hand] = false;
}

bool GetEffectiveAlignedRightAimCalibration(int weaponType,
	WeaponCalibration *effective)
{
	if(!effective || weaponType < 0 ||
	   weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return false;
	const WeaponCalibration *right = GetWeaponCalibration(1, weaponType);
	if(!right)
		return false;
	CMatrix aim;
	if(!BuildAlignedWeaponAimMatrix(1, weaponType, *right, &aim))
		return false;
	*effective = *right;
	return ExtractWeaponAimCalibration(1, aim, *right, effective);
}

void SaveWeaponModelCalibrationEdit(const char *name, int *value, int newValue)
{
	if(!name || !value)
		return;
	*value = newValue;
	SaveCurrentWeaponCalibrationValue(name, newValue);
}

bool IsBikeSidearmTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
		return true;
	default:
		return false;
	}
}

void RestrictImmersiveVehicleWeaponsToSidearms(CPlayerPed *player)
{
	if(!player)
		return;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const int slot = gHeldWeaponSlot[hand];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot >= 0 && !IsBikeSidearmTypeInternal(weaponType)){
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gWeaponHolsterSelection[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			ClearDroppedWeapon(hand);
		}
	}
	uint32 sidearmMask = 0;
	for(int slot = WEAPONSLOT_HANDGUN; slot <= WEAPONSLOT_SUBMACHINEGUN; slot++){
		if(!player->HasWeaponSlot(slot))
			continue;
		const int weaponType = GetVrWeaponTypeForSlot(slot);
		if(IsBikeSidearmTypeInternal(weaponType))
			sidearmMask |= 1u << slot;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++)
		if(gHeldWeaponSlot[hand] >= 0)
			sidearmMask &= ~(1u << gHeldWeaponSlot[hand]);
	gWeaponHolsterMask = sidearmMask;
}

void RotateBikeHandleCalibration(CMatrix *matrix,
	const BikeHandleCalibration &calibration)
{
	if(!matrix)
		return;
	const float pitch = DEGTORAD((float)calibration.rotationX/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float yaw = DEGTORAD((float)calibration.rotationY/
		WEAPON_CALIBRATION_VALUE_SCALE);
	const float roll = DEGTORAD((float)calibration.rotationZ/
		WEAPON_CALIBRATION_VALUE_SCALE);
	if(pitch != 0.0f){
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetRight(), pitch);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetRight(), pitch);
	}
	if(yaw != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetUp(), yaw);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetUp(), yaw);
	}
	if(roll != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetForward(), roll);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetForward(), roll);
	}
	matrix->GetRight().Normalise();
	matrix->GetForward().Normalise();
	matrix->GetUp().Normalise();
	// A cylindrical grip is axially identical after a 180-degree turn, so old
	// per-model calibrations legitimately contain both signs for the same socket.
	// Resolve that ambiguity only in the visual adapter: fingers must face the
	// motorcycle's forward hemisphere. Rotating Forward and Up together keeps a
	// proper wrist basis while preserving position, palm normal and interaction.
	CBike *bike = GetActivePlayerBike();
	if(bike && DotProduct(matrix->GetForward(), bike->GetForward()) < 0.0f){
		matrix->GetForward() *= -1.0f;
		matrix->GetUp() *= -1.0f;
	}
}

void GetVehicleControlAdjustment(CVehicle *vehicle, CVector *centerOffset,
	float *radiusOffset)
{
	if(centerOffset)
		*centerOffset = CVector(0.0f, 0.0f, 0.0f);
	if(radiusOffset)
		*radiusOffset = 0.0f;
	if(!vehicle)
		return;
	VehicleCategoryCalibration *category =
		GetVehicleCategoryCalibration(vehicle);
	VehicleViewCalibration *model =
		GetVehicleViewCalibration(vehicle->GetModelIndex());
	if(!category || !model)
		return;
	if(centerOffset)
		*centerOffset = CVector(
			(float)(category->wheelCenterXCm+model->wheelCenterXCm)/100.0f,
			(float)(category->wheelCenterYCm+model->wheelCenterYCm)/100.0f,
			(float)(category->wheelCenterZCm+model->wheelCenterZCm)/100.0f);
	if(radiusOffset)
		*radiusOffset = (float)(category->wheelRadiusCm+
			model->wheelRadiusCm)/100.0f;
}

struct BikeHandlePose
{
	CVector center;
	CVector right;
	CVector forward;
	CVector up;
};

bool BuildBikeHandlePose(CBike *bike, BikeHandlePose *pose)
{
	if(!bike || !pose)
		return false;
	bike->CalculateLeanMatrix();
	pose->right = bike->m_leanMatrix.GetRight();
	pose->forward = bike->m_leanMatrix.GetForward();
	pose->up = bike->m_leanMatrix.GetUp();
	if(pose->right.MagnitudeSqr() < 0.0001f ||
	   pose->forward.MagnitudeSqr() < 0.0001f ||
	   pose->up.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->forward.Normalise();
	pose->up.Normalise();
	pose->center = bike->GetPosition()+pose->forward*0.30f+pose->up*0.82f;
	if(bike->m_aBikeNodes[BIKE_HANDLEBARS]){
		RwMatrix *handleLtm =
			RwFrameGetLTM(bike->m_aBikeNodes[BIKE_HANDLEBARS]);
		if(handleLtm)
			pose->center = CVector(handleLtm->pos);
	}
	CVector controlCenterOffset;
	GetVehicleControlAdjustment(bike, &controlCenterOffset, nil);
	pose->center += pose->right*controlCenterOffset.x+
		pose->forward*controlCenterOffset.y+pose->up*controlCenterOffset.z;
	return true;
}

bool BuildDrawnBikeHandlePose(CBike *bike, BikeHandlePose *pose)
{
	if(!bike || !pose || !bike->m_aBikeNodes[BIKE_HANDLEBARS]) return false;
	RwMatrix *frame = RwFrameGetLTM(bike->m_aBikeNodes[BIKE_HANDLEBARS]);
	if(!frame) return false;
	pose->right = CVector(frame->right);
	pose->forward = CVector(frame->up);
	pose->up = CVector(frame->at);
	pose->center = CVector(frame->pos);
	if(pose->right.MagnitudeSqr() < 0.0001f || pose->forward.MagnitudeSqr() < 0.0001f || pose->up.MagnitudeSqr() < 0.0001f) return false;
	pose->right.Normalise(); pose->forward.Normalise(); pose->up.Normalise();
	CVector offset;
	GetVehicleControlAdjustment(bike, &offset, nil);
	pose->center += pose->right*offset.x+pose->forward*offset.y+pose->up*offset.z;
	return true;
}

bool BuildBikeHandleMatrixInternal(int hand, CMatrix *matrix,
	bool applySteering)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   !IsImmersiveBikeDrivingActiveInternal())
		return false;
	CBike *bike = GetActivePlayerBike();
	BikeHandleCalibration *calibration =
		GetBikeHandleCalibration(bike->GetModelIndex(), hand);
	if(!calibration)
		return false;
	BikeHandlePose pose;
	if(!BuildBikeHandlePose(bike, &pose))
		return false;
	const bool onDrawnBars = applySteering && BuildDrawnBikeHandlePose(bike, &pose);
	float controlRadiusOffset = 0.0f;
	GetVehicleControlAdjustment(bike, nil,
		&controlRadiusOffset);
	matrix->SetUnity();
	// RenderVrTrackedHand derives its palm normal from Matrix::Right and its
	// pointing direction from the aim ray. This mirrored controller basis keeps
	// both palms naturally resting on the bar before per-model calibration.
	matrix->GetRight() = pose.up*(hand == 0 ? -1.0f : 1.0f);
	matrix->GetForward() = pose.forward;
	matrix->GetUp() = CrossProduct(matrix->GetRight(),
		matrix->GetForward());
	matrix->GetUp().Normalise();
	float lateral = (float)calibration->offsetX/200.0f+
		(hand == 0 ? -controlRadiusOffset : controlRadiusOffset);
	// Category and model radius adjustments add together, but can never invert
	// the left/right grip ordering even if both are set to their minimum.
	lateral = hand == 0 ? Min(lateral, -0.08f) : Max(lateral, 0.08f);
	matrix->GetPosition() = pose.center +
		pose.right*lateral +
		pose.forward*((float)calibration->offsetY/200.0f) +
		pose.up*((float)calibration->offsetZ/200.0f);
	RotateBikeHandleCalibration(matrix, *calibration);
	const float steeringAngle = IsImmersiveBikeDrivingActiveInternal(bike) ?
		gImmersiveBikePhysicalAngle : bike->m_fWheelAngle;
	if(applySteering && !onDrawnBars && steeringAngle != 0.0f){
		// The hands and calibration handles represent the physical bar held by the
		// player, so they follow the authoritative VR joint angle directly. Vice
		// City's speed-dependent m_fWheelAngle remains the front-fork/physics angle;
		// using it here visually erased most real hand motion at speed.
		matrix->GetPosition() = pose.center+RotateAroundAxis(
			matrix->GetPosition()-pose.center, pose.up, steeringAngle);
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			pose.up, steeringAngle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			pose.up, steeringAngle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			pose.up, steeringAngle);
	}
	if(applySteering && hand == 1 && !gBikeManualThrottle && bike->pHandling){
		const float maximum = bike->pHandling->Transmission.fMaxVelocity;
		const float speed = bike->m_vecMoveSpeed.Magnitude2D();
		if(_finite(maximum) && maximum > 0.01f && _finite(speed)){
			const float twist = clamp(speed/maximum, 0.0f, 1.0f)*DEGTORAD(-43.0f);
			// Visual wrist travel only; the trigger still owns acceleration.
			matrix->GetRight() = RotateAroundAxis(matrix->GetRight(), matrix->GetForward(), twist);
			matrix->GetUp() = RotateAroundAxis(matrix->GetUp(), matrix->GetForward(), twist);
		}
	}
	return true;
}

void ApplyBikeVisualGripAlignment(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT)
		return;
	// Bike calibration points the socket Forward axis along the authored grip.
	// Feeding that axis straight to the hand renderer also points the fingers
	// along the handlebar, so the two fists face inward at one another. Fingers
	// wrap across a cylindrical grip: rotate only the visual hand by 90 degrees
	// around its preserved palm normal. Interaction sockets and calibration cubes
	// remain authoritative and are intentionally not rotated here.
	// Matrix::Right already has the hand-dependent sign. Applying opposite
	// numeric angles cancels that mirrored local axis and sends both wrists to
	// the same world side. Use one local conversion for both hands; the distinct
	// Keep this socket-to-wrist conversion identical for both sides; the hand
	// renderer owns the final anatomical LEFT mesh parity.
	const float angle = DEGTORAD(-90.0f);
	matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
		matrix->GetRight(), angle);
	matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
		matrix->GetRight(), angle);
	matrix->GetForward().Normalise();
	matrix->GetUp().Normalise();
	// A cylindrical grip has no authored "front" along its own axis, and several
	// stock bike profiles encode that axis with the opposite sign.  Resolve that
	// ambiguity only at the final visual adapter: preserve the palm normal and
	// rotate the wrist by 180 degrees when its fingers would otherwise point back
	// at the rider.  This is model-independent and leaves sockets, interaction,
	// steering and all saved calibration values untouched.
	CBike *bike = GetActivePlayerBike();
	if(bike && DotProduct(matrix->GetForward(), bike->GetForward()) < 0.0f){
		matrix->GetForward() *= -1.0f;
		matrix->GetUp() *= -1.0f;
	}
}

struct CarWheelPose
{
	CVector center;
	CVector right;
	CVector up;
	CVector normal;
	float radius;
};

bool BuildCarWheelPose(CVehicle *car, CarWheelPose *pose)
{
	if(!car || !pose)
		return false;
	CVehicleModelInfo *model =
		(CVehicleModelInfo*)CModelInfo::GetModelInfo(car->GetModelIndex());
	if(!model)
		return false;
	CVector local = model->GetFrontSeatPosn();
	// The authored front-seat point is the passenger side. Mirroring X yields
	// the driver's shoulder line; the small forward/up offsets put the neutral
	// calibration origin at the steering wheel instead of inside Tommy's chest.
	if(!car->IsBoat()) local.x = -local.x;
	local.y += 0.33f;
	local.z += 0.30f;
	CVector controlCenterOffset;
	GetVehicleControlAdjustment(car, &controlCenterOffset, nil);
	local += controlCenterOffset;
	pose->center = car->GetPosition()+Multiply3x3(car->GetMatrix(), local);
	pose->normal = car->GetForward();
	pose->right = car->GetRight();
	if(pose->normal.MagnitudeSqr() < 0.0001f ||
	   pose->right.MagnitudeSqr() < 0.0001f)
		return false;
	pose->normal.Normalise();
	pose->right -= pose->normal*DotProduct(pose->right, pose->normal);
	if(pose->right.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->up = CrossProduct(pose->right, pose->normal);
	if(pose->up.MagnitudeSqr() < 0.0001f)
		return false;
	pose->up.Normalise();
	VehicleCategoryCalibration *category =
		GetVehicleCategoryCalibration(car);
	VehicleViewCalibration *view =
		GetVehicleViewCalibration(car->GetModelIndex());
	// The wheel plane is authoritative for the rendered VR rim, automatic hand
	// anchors and physical steering input. Classic/Modern category values and the
	// active model's values are additive half-degrees. Apply the combined local
	// Euler correction once so both layers remain predictable: pitch around the
	// car's right axis, yaw around the resulting up axis, then roll/clocking around
	// the resulting wheel normal. The authored neutral DFF frame is intentionally
	// not tilted here; calibration aligns this VR pose to that existing geometry.
	const int pitchHalfDeg = Min(Max(
		(category ? category->carWheelPitchHalfDeg : 0)+
		(view ? view->carWheelPitchHalfDeg : 0),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
		VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	const int yawHalfDeg = Min(Max(
		(category ? category->carWheelYawHalfDeg : 0)+
		(view ? view->carWheelYawHalfDeg : 0),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
		VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	const int rollHalfDeg = Min(Max(
		(category ? category->carWheelRollHalfDeg : 0)+
		(view ? view->carWheelRollHalfDeg : 0),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
		VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	const float pitch = DEGTORAD((float)pitchHalfDeg/2.0f);
	const float yaw = DEGTORAD((float)yawHalfDeg/2.0f);
	const float roll = DEGTORAD((float)rollHalfDeg/2.0f);
	if(pitch != 0.0f){
		pose->normal = RotateAroundAxis(pose->normal, pose->right, pitch);
		pose->up = RotateAroundAxis(pose->up, pose->right, pitch);
	}
	if(yaw != 0.0f){
		pose->right = RotateAroundAxis(pose->right, pose->up, yaw);
		pose->normal = RotateAroundAxis(pose->normal, pose->up, yaw);
	}
	if(roll != 0.0f){
		pose->right = RotateAroundAxis(pose->right, pose->normal, roll);
		pose->up = RotateAroundAxis(pose->up, pose->normal, roll);
	}
	// Eliminate accumulated float drift while preserving the calibrated roll.
	pose->normal.Normalise();
	pose->right -= pose->normal*DotProduct(pose->right, pose->normal);
	if(pose->right.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->up = CrossProduct(pose->right, pose->normal);
	if(pose->up.MagnitudeSqr() < 0.0001f)
		return false;
	pose->up.Normalise();
	int radiusCm = category ? category->carWheelRadiusCm :
		VR_CAR_WHEEL_DEFAULT_RADIUS_CM;
	if(view && view->carWheelRadiusCm != 0)
		radiusCm = view->carWheelRadiusCm;
	radiusCm = Min(Max(radiusCm, VR_CAR_WHEEL_MIN_RADIUS_CM),
		VR_CAR_WHEEL_MAX_RADIUS_CM);
	pose->radius = (float)radiusCm/100.0f;
	return true;
}

bool BuildCarWheelMatrixInternal(int hand, CMatrix *matrix,
	bool applySteering)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   !IsVrCarDrivingActiveInternal())
		return false;
	CVehicle *car = GetActivePlayerCar();
	CarWheelPose pose;
	if(!BuildCarWheelPose(car, &pose))
		return false;
	matrix->SetUnity();
	// Car grips are fixed, derived points at 9 and 3 o'clock. There are no
	// independent hand offsets, so calibration cannot stretch or shift the
	// physical wheel away from its authoritative centre and radius.
	const float side = hand == 0 ? -1.0f : 1.0f;
	// RenderVrTrackedHand multiplies Matrix::Right by the hand side, producing
	// the outward radial direction. UltimateXR maps its +Y palm normal to the
	// negative of that direction (and the procedural fingers curl there too), so
	// each palm faces the wheel centre while its finger-root axis runs along the
	// vertical tangent. The previous wheel-up basis left both palms flat and
	// drove a closed fist through the dashboard instead of around the rim.
	matrix->GetRight() = pose.right;
	matrix->GetForward() = pose.normal;
	matrix->GetUp() = CrossProduct(matrix->GetRight(),
		matrix->GetForward());
	matrix->GetUp().Normalise();
	matrix->GetPosition() = pose.center+pose.right*(side*pose.radius);
	const float physicalAngle = IsImmersiveCarDrivingActiveInternal() ?
		gImmersiveCarPhysicalAngle : gMotionVehiclePhysicalAngle;
	if(applySteering && physicalAngle != 0.0f){
		// The logical wheel angle and Rodrigues rotation use opposite signs in
		// Vice City's car basis. This sign is visual only: vehicle steering is
		// passed through independently in GetImmersiveCarSteering.
		const float angle = -physicalAngle;
		matrix->GetPosition() = pose.center+RotateAroundAxis(
			matrix->GetPosition()-pose.center, pose.normal, angle);
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			pose.normal, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			pose.normal, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			pose.normal, angle);
	}
	return true;
}

void UpdateImmersiveCarModelSteeringWheelInternal(CVehicle *vehicle)
{
	if(!vehicle || !vehicle->IsCar())
		return;
	CAutomobile *car = (CAutomobile*)vehicle;
	RwFrame *wheelFrame = car->m_aCarNodes[CAR_STEERING_WHEEL];
	if(!wheelFrame || !car->m_bVrSteeringWheelNeutralValid)
		return;

	float physicalAngle = 0.0f;
	if(IsImmersiveCarDrivingActiveInternal(car))
		physicalAngle = gImmersiveCarPhysicalAngle;
	else if(IsMotionDrivingEnvironmentActive() &&
	   IsVrCarDrivingActiveInternal(car))
		physicalAngle = gMotionVehiclePhysicalAngle;
	const float visualAngle = -physicalAngle;
	if(Abs(visualAngle-car->m_fVrSteeringWheelAppliedAngle) < 0.0001f)
		return;

	CMatrix wheelMatrix(RwFrameGetMatrix(wheelFrame), false);
	wheelMatrix.CopyOnlyMatrix(car->m_vrSteeringWheelNeutralMatrix);
	if(visualAngle != 0.0f){
		RwFrame *parent = RwFrameGetParent(wheelFrame);
		CarWheelPose wheelPose;
		if(!parent || !BuildCarWheelPose(car, &wheelPose)){
			wheelMatrix.UpdateRW();
			car->m_fVrSteeringWheelAppliedAngle = 0.0f;
			return;
		}
		// Candidate atomics do not share a common authored local axis or pivot.
		// Convert the authoritative calibrated wheel centre and normal into the
		// frame parent's space, then rotate both basis and origin around that pivot.
		// This also handles DFFs whose isolated wheel geometry is authored in car
		// coordinates with a zero frame origin; rotating the frame at its own origin
		// would otherwise swing the wheel through the dashboard.
		CMatrix parentWorld(RwFrameGetLTM(parent), false);
		CMatrix inverseParent;
		Invert(parentWorld, inverseParent);
		CVector localAxis = Multiply3x3(inverseParent, wheelPose.normal);
		if(localAxis.MagnitudeSqr() < 0.0001f){
			wheelMatrix.UpdateRW();
			car->m_fVrSteeringWheelAppliedAngle = 0.0f;
			return;
		}
		localAxis.Normalise();
		const CVector localPivot = inverseParent*wheelPose.center;
		wheelMatrix.GetPosition() = localPivot+RotateAroundAxis(
			wheelMatrix.GetPosition()-localPivot, localAxis, visualAngle);
		wheelMatrix.GetRight() = RotateAroundAxis(
			wheelMatrix.GetRight(), localAxis, visualAngle);
		wheelMatrix.GetForward() = RotateAroundAxis(
			wheelMatrix.GetForward(), localAxis, visualAngle);
		wheelMatrix.GetUp() = RotateAroundAxis(
			wheelMatrix.GetUp(), localAxis, visualAngle);
	}
	wheelMatrix.UpdateRW();
	car->m_fVrSteeringWheelAppliedAngle = visualAngle;
}

float WrapCarWheelAngle(float angle)
{
	const float pi = 3.14159265358979323846f;
	while(angle > pi) angle -= 2.0f*pi;
	while(angle < -pi) angle += 2.0f*pi;
	return angle;
}

float UnwrapCarWheelAngle(float angle, float reference)
{
	const float pi = 3.14159265358979323846f;
	while(angle-reference > pi) angle -= 2.0f*pi;
	while(angle-reference < -pi) angle += 2.0f*pi;
	return angle;
}

float PlanarCarWheelAngle(const CVector &vector,
	const CVector &right, const CVector &up)
{
	return atan2f(DotProduct(vector, up), DotProduct(vector, right));
}

float WrapBikeSteeringAngle(float angle)
{
	return WrapCarWheelAngle(angle);
}

float PlanarBikeHandleAngle(const CVector &vector, const CVector &right,
	const CVector &forward)
{
	return atan2f(DotProduct(vector, forward), DotProduct(vector, right));
}

bool CaptureBikeOneHandSteering(CBike *bike, uint32 realHandMask,
	const CVector &neutralChord, const BikeHandlePose &pose,
	float physicalAngle)
{
	if(!bike || (realHandMask != 1u && realHandMask != 2u))
		return false;
	gBikeOneHandSteeringState = BikeOneHandSteeringState();
	const int hand = realHandMask == 1u ? 0 : 1;
	if(!gTrackedHandPoseValid[hand])
		return false;

	CVector rightTracking = ToTrackingVector(pose.right);
	CVector forwardTracking = ToTrackingVector(pose.forward);
	if(rightTracking.MagnitudeSqr() < 0.0001f ||
	   forwardTracking.MagnitudeSqr() < 0.0001f)
		return false;
	rightTracking.Normalise();
	forwardTracking -= rightTracking*
		DotProduct(forwardTracking, rightTracking);
	if(forwardTracking.MagnitudeSqr() < 0.0001f)
		return false;
	forwardTracking.Normalise();

	// Seed the missing-hand chord at the already applied bar angle.  Subsequent
	// observations live entirely in raw OpenXR gameplay space, so motorcycle
	// translation, body lean, handlebar animation and horizon-lock camera changes
	// cannot feed the steering output back into its own input.
	CVector seedChord = neutralChord;
	if(physicalAngle != 0.0f)
		seedChord = RotateAroundAxis(seedChord, pose.up, physicalAngle);
	seedChord = ToTrackingVector(seedChord);
	const float seedPlaneLengthSqr =
		sq(DotProduct(seedChord, rightTracking))+
		sq(DotProduct(seedChord, forwardTracking));
	const CVector handTracking = GetRawTrackedHandPosition(hand);
	if(seedPlaneLengthSqr <= 0.0001f ||
	   !_finite(handTracking.x) || !_finite(handTracking.y) ||
	   !_finite(handTracking.z))
		return false;

	gBikeOneHandSteeringState.vehicle = bike;
	gBikeOneHandSteeringState.realHandMask = realHandMask;
	gBikeOneHandSteeringState.valid = true;
	gBikeOneHandSteeringState.referenceHandTracking = handTracking;
	gBikeOneHandSteeringState.seedChordTracking = seedChord;
	gBikeOneHandSteeringState.rightTracking = rightTracking;
	gBikeOneHandSteeringState.forwardTracking = forwardTracking;
	gBikeOneHandSteeringState.referencePhysicalAngle = physicalAngle;
	return true;
}

bool RebaseBikeOneHandSteering(CBike *bike, uint32 realHandMask,
	float appliedAngle)
{
	if(!bike || !gBikeOneHandSteeringState.valid ||
	   gBikeOneHandSteeringState.vehicle != bike ||
	   gBikeOneHandSteeringState.realHandMask != realHandMask ||
	   (realHandMask != 1u && realHandMask != 2u))
		return false;
	const int hand = realHandMask == 1u ? 0 : 1;
	if(!gTrackedHandPoseValid[hand])
		return false;
	const CVector currentHandTracking = GetRawTrackedHandPosition(hand);
	if(!_finite(currentHandTracking.x) || !_finite(currentHandTracking.y) ||
	   !_finite(currentHandTracking.z))
		return false;

	// Do not sample the live motorcycle again at a stop. Keep the capture-time
	// tracking frame frozen and rotate only its canonical chord from the previous
	// reference angle to the angle that was actually applied. This removes hidden
	// overtravel while making the first inward millimetre respond immediately.
	CVector steeringAxis = CrossProduct(
		gBikeOneHandSteeringState.rightTracking,
		gBikeOneHandSteeringState.forwardTracking);
	if(steeringAxis.MagnitudeSqr() < 0.0001f)
		return false;
	steeringAxis.Normalise();
	const float appliedDelta = WrapBikeSteeringAngle(appliedAngle-
		gBikeOneHandSteeringState.referencePhysicalAngle);
	gBikeOneHandSteeringState.seedChordTracking = RotateAroundAxis(
		gBikeOneHandSteeringState.seedChordTracking, steeringAxis,
		appliedDelta);
	gBikeOneHandSteeringState.referenceHandTracking = currentHandTracking;
	gBikeOneHandSteeringState.referencePhysicalAngle = appliedAngle;
	return true;
}

bool SolveBikeOneHandSteering(CBike *bike, uint32 realHandMask,
	float *desiredAngle)
{
	if(!bike || !desiredAngle || !gBikeOneHandSteeringState.valid ||
	   gBikeOneHandSteeringState.vehicle != bike ||
	   gBikeOneHandSteeringState.realHandMask != realHandMask ||
	   (realHandMask != 1u && realHandMask != 2u))
		return false;
	const int hand = realHandMask == 1u ? 0 : 1;
	if(!gTrackedHandPoseValid[hand])
		return false;
	const CVector currentHandTracking = GetRawTrackedHandPosition(hand);
	if(!_finite(currentHandTracking.x) || !_finite(currentHandTracking.y) ||
	   !_finite(currentHandTracking.z))
		return false;
	const float handSign = realHandMask == 2u ? 2.0f : -2.0f;
	const CVector actualChord =
		gBikeOneHandSteeringState.seedChordTracking+
		(currentHandTracking-
		 gBikeOneHandSteeringState.referenceHandTracking)*handSign;
	const float actualPlaneLengthSqr =
		sq(DotProduct(actualChord,
			gBikeOneHandSteeringState.rightTracking))+
		sq(DotProduct(actualChord,
			gBikeOneHandSteeringState.forwardTracking));
	if(actualPlaneLengthSqr <= 0.0001f ||
	   !_finite(actualChord.x) || !_finite(actualChord.y) ||
	   !_finite(actualChord.z))
		return false;
	const float seedAngle = PlanarBikeHandleAngle(
		gBikeOneHandSteeringState.seedChordTracking,
		gBikeOneHandSteeringState.rightTracking,
		gBikeOneHandSteeringState.forwardTracking);
	const float currentAngle = PlanarBikeHandleAngle(actualChord,
		gBikeOneHandSteeringState.rightTracking,
		gBikeOneHandSteeringState.forwardTracking);
	*desiredAngle = gBikeOneHandSteeringState.referencePhysicalAngle+
		WrapBikeSteeringAngle(currentAngle-seedAngle);
	return _finite(*desiredAngle) != 0;
}

float OneHandTangentialSteeringAngle(float tangentialTravel,
	float radius, float maxAngle)
{
	if(radius <= 0.001f)
		return 0.0f;
	const float normalizedTravel = tangentialTravel/radius;
	const float magnitude = Abs(normalizedTravel);
	const float circularLimit = sinf(maxAngle);
	// Inside the useful range this is exactly the inverse of the tangential
	// travel of a point moving around a circle. Beyond the physical stop, keep
	// the scalar unbounded so anti-windup can rebase at the current hand position
	// and the first movement back responds immediately.
	const float angle = magnitude <= circularLimit ?
		asinf(Min(magnitude, 1.0f)) :
		maxAngle+(magnitude-circularLimit);
	return normalizedTravel < 0.0f ? -angle : angle;
}

bool GetTrackedHornHandPosition(int hand, CVector *position)
{
	if(!position || hand < 0 || hand >= EYE_COUNT ||
	   !gTrackedHandPoseValid[hand])
		return false;
	*position = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandPose[hand].position);
	return true;
}

void UpdateImmersiveCarHorn(CVehicle *car,
	const CVector &center, const CVector &axis, uint32 blockedHands)
{
	(void)axis;
	if(!car){
		gImmersiveCarHornPressed = false;
		for(int hand = 0; hand < EYE_COUNT; hand++){
			gCarHornContact[hand] = false;
			gCarHornArmed[hand] = false;
			gCarHornPreviousDistance[hand] = 1000.0f;
		}
		return;
	}
	bool pressed = false;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			gCarWheelGrabbed[hand] ||
			gHeldWeaponSlot[hand] >= 0 ||
			IsWeaponSupportHandInternal(hand) ||
			IsHandBusyWithReload(hand);
		CVector handPosition;
		if(unavailable || !GetTrackedHornHandPosition(hand, &handPosition)){
			gCarHornContact[hand] = false;
			gCarHornArmed[hand] = false;
			gCarHornPreviousDistance[hand] = 1000.0f;
			continue;
		}
		const float distance = (handPosition-center).Magnitude();
		const float previousDistance = gCarHornPreviousDistance[hand];
		const float enterDistance = 0.105f;
		const float leaveDistance = 0.15f;
		const float armNearDistance = 0.12f;
		const float armFarDistance = 0.26f;
		if(gCarHornContact[hand]){
			if(distance > leaveDistance)
				gCarHornContact[hand] = false;
		}else{
			// Pointing at the wheel must never sound the horn. Arm it only while
			// the tracked hand is moving toward the centre, then require an
			// actual inward crossing of the small contact sphere.
			if(distance >= armNearDistance &&
			   distance <= armFarDistance &&
			   previousDistance < 999.0f &&
			   distance < previousDistance-0.002f)
				gCarHornArmed[hand] = true;
			if(distance > armFarDistance)
				gCarHornArmed[hand] = false;
			if(gCarHornArmed[hand] &&
			   distance <= enterDistance &&
			   previousDistance < 999.0f &&
			   distance < previousDistance){
				gCarHornContact[hand] = true;
				gCarHornArmed[hand] = false;
			}
		}
		gCarHornPreviousDistance[hand] = distance;
		pressed = pressed || gCarHornContact[hand];
	}
	gImmersiveCarHornPressed = pressed;
}

uint32 UpdateImmersiveCarInput(const float *grips, uint32 blockedHands)
{
	if(!grips || !IsImmersiveCarDrivingActiveInternal()){
		ResetImmersiveCarInteraction();
		return 0;
	}
	CVehicle *car = GetActivePlayerCar();
	CPlayerPed *player = FindPlayerPed();
	if(!car || !player){
		ResetImmersiveCarInteraction();
		return 0;
	}
	RestrictImmersiveVehicleWeaponsToSidearms(player);
	CMatrix neutralAnchors[EYE_COUNT];
	CMatrix anchors[EYE_COUNT];
	CVector handPositions[EYE_COUNT];
	bool anchorValid[EYE_COUNT] = {};
	gCarWheelUnavailableMask = 0;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		anchorValid[hand] =
			BuildCarWheelMatrixInternal(hand, &neutralAnchors[hand], false) &&
			BuildCarWheelMatrixInternal(hand, &anchors[hand], true);
		if(gTrackedHandPoseValid[hand])
			handPositions[hand] = gBaseCamera.GetPosition()+
				ToGameVector(gTrackedHandPose[hand].position);
		else
			handPositions[hand] = CVector(0.0f, 0.0f, 0.0f);
		gCarWheelDistance[hand] =
			anchorValid[hand] && gTrackedHandPoseValid[hand] ?
			(handPositions[hand]-anchors[hand].GetPosition()).Magnitude() :
			1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !gTrackedHandPoseValid[hand] ||
			gHeldWeaponSlot[hand] >= 0 ||
			IsWeaponSupportHandInternal(hand) ||
			IsHandBusyWithReload(hand);
		if(unavailable)
			gCarWheelUnavailableMask |= 1u << hand;
		if(gCarWheelGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gCarWheelGrabbed[hand] = false;
		}
		if(!gCarWheelGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f &&
		   gCarWheelDistance[hand] <= 0.23f){
			gCarWheelGrabbed[hand] = true;
			debug("[OpenXR] %s car wheel grip grabbed on %s\n",
				hand == 0 ? "Left" : "Right",
				GetActiveVrVehicleName());
		}
		if(grips[hand] <= 0.30f)
			gCarWheelGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gCarWheelGripDown[hand] = true;
	}

	const bool left = gCarWheelGrabbed[0] && anchorValid[0];
	const bool right = gCarWheelGrabbed[1] && anchorValid[1];
	CarWheelPose wheelPose;
	if(!BuildCarWheelPose(car, &wheelPose)){
		ResetImmersiveCarInteraction();
		return 0;
	}
	const CVector center = wheelPose.center;
	const CVector axis = wheelPose.normal;
	const uint32 realHandMask =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	const float maxSteering = DEGTORAD(80.0f);
	gImmersiveCarDesiredAngle = 0.0f;
	gImmersiveCarSteeringOverflow = 0.0f;
	float steeringAngle = 0.0f;
	if(realHandMask != 0){
		if(gCarSteeringChordState.valid &&
		   gCarSteeringChordState.vehicle != car){
			gImmersiveCarPhysicalAngle = 0.0f;
			gCarSteeringChordState = ImmersiveSteeringChordState();
		}

		if(realHandMask != 3u){
			// A real wheel constrains one held hand to a single tangent. Measure only
			// that coordinate in the current vehicle frame; reaching inward/outward or
			// toward the dashboard must never change steering sensitivity or sign.
			const int hand = realHandMask == 1u ? 0 : 1;
			CVector radial = neutralAnchors[hand].GetPosition()-center;
			radial -= axis*DotProduct(radial, axis);
			const float radius = radial.Magnitude();
			CVector tangent = CrossProduct(radial, axis);
			const bool pointValid = radius >= 0.08f &&
				tangent.MagnitudeSqr() > 0.0001f &&
				_finite(handPositions[hand].x) &&
				_finite(handPositions[hand].y) &&
				_finite(handPositions[hand].z);
			if(pointValid){
				tangent.Normalise();
				const float coordinate = DotProduct(
					handPositions[hand]-neutralAnchors[hand].GetPosition(),
					tangent);
				const bool ownershipChanged =
					!gCarSteeringChordState.valid ||
					gCarSteeringChordState.vehicle != car ||
					gCarSteeringChordState.realHandMask != realHandMask;
				if(ownershipChanged){
					const uint32 nextCapture =
						gCarSteeringChordState.captureCount+1;
					gCarSteeringChordState = ImmersiveSteeringChordState();
					gCarSteeringChordState.vehicle = car;
					gCarSteeringChordState.realHandMask = realHandMask;
					gCarSteeringChordState.referenceActive = true;
					gCarSteeringChordState.referenceAngle =
						gImmersiveCarPhysicalAngle;
					gCarSteeringChordState.oneHandReferenceCoordinate =
						coordinate;
					gCarSteeringChordState.continuousAngle =
						gImmersiveCarPhysicalAngle;
					gCarSteeringChordState.captureCount = nextCapture;
					gCarSteeringChordState.valid = true;
				}
				const float desiredAngle =
					gCarSteeringChordState.referenceAngle+
					OneHandTangentialSteeringAngle(coordinate, radius,
						maxSteering)-
					OneHandTangentialSteeringAngle(
						gCarSteeringChordState.oneHandReferenceCoordinate,
						radius, maxSteering);
				gImmersiveCarDesiredAngle = desiredAngle;
				const bool discontinuity = !_finite(desiredAngle) ||
					Abs(desiredAngle-gImmersiveCarPhysicalAngle) >
						DEGTORAD(45.0f);
				steeringAngle = discontinuity ?
					gImmersiveCarPhysicalAngle :
					clamp(desiredAngle, -maxSteering, maxSteering);
				gImmersiveCarSteeringOverflow =
					desiredAngle-steeringAngle;
				if(discontinuity || steeringAngle != desiredAngle){
					// Rebase at the physical stop itself. Moving farther outside the
					// rim cannot accumulate hidden travel; the first millimetre back
					// changes the applied angle immediately.
					gCarSteeringChordState.referenceAngle = steeringAngle;
					gCarSteeringChordState.oneHandReferenceCoordinate =
						coordinate;
					if(discontinuity)
						gCarSteeringChordState.captureCount++;
				}
				gCarSteeringChordState.lastDelta = steeringAngle-
					gImmersiveCarPhysicalAngle;
				gCarSteeringChordState.continuousAngle = steeringAngle;
				gCarSteeringChordState.pointValidMask = realHandMask;
			}else{
				steeringAngle = gImmersiveCarPhysicalAngle;
				const uint32 captureCount =
					gCarSteeringChordState.captureCount;
				gCarSteeringChordState = ImmersiveSteeringChordState();
				gCarSteeringChordState.captureCount = captureCount;
				gCarSteeringChordState.rebaseOnNextValid = true;
			}
		}else{
			// Two real hands are deliberately the accepted v0.4.1 labelled chord
			// path. Do not route them through the one-hand tangent constraint.
			const CVector chord = handPositions[1]-handPositions[0];
			const float chordPlaneLengthSqr =
				sq(DotProduct(chord, wheelPose.right))+
				sq(DotProduct(chord, wheelPose.up));
			if(chordPlaneLengthSqr > 0.0001f){
				const float chordAngle = PlanarCarWheelAngle(chord,
					wheelPose.right, wheelPose.up);
			const bool ownershipChanged =
				!gCarSteeringChordState.valid ||
				gCarSteeringChordState.vehicle != car ||
				gCarSteeringChordState.realHandMask != realHandMask;
			if(ownershipChanged){
				const uint32 nextCapture =
					gCarSteeringChordState.captureCount+1;
				gCarSteeringChordState = ImmersiveSteeringChordState();
				gCarSteeringChordState.vehicle = car;
				gCarSteeringChordState.realHandMask = realHandMask;
				gCarSteeringChordState.referenceAngle = WrapCarWheelAngle(
					chordAngle-gImmersiveCarPhysicalAngle);
				gCarSteeringChordState.continuousAngle =
					gImmersiveCarPhysicalAngle;
				gCarSteeringChordState.captureCount = nextCapture;
				gCarSteeringChordState.valid = true;
			}
			steeringAngle = WrapCarWheelAngle(chordAngle-
				gCarSteeringChordState.referenceAngle);
			steeringAngle = UnwrapCarWheelAngle(steeringAngle,
				gCarSteeringChordState.continuousAngle);
			const float desiredAngle = steeringAngle;
			gImmersiveCarDesiredAngle = desiredAngle;
			// Keep the stored chord state on the physical stop itself. Without this
			// anti-windup, invisible angle accumulated outside +/-80 degrees and had
			// to be unwound before the car reacted in the opposite direction.
			const bool discontinuity = Abs(WrapCarWheelAngle(desiredAngle-
				gImmersiveCarPhysicalAngle)) > DEGTORAD(45.0f);
			steeringAngle = discontinuity ? gImmersiveCarPhysicalAngle :
				clamp(desiredAngle, -maxSteering, maxSteering);
			gImmersiveCarSteeringOverflow = desiredAngle-steeringAngle;
			if(discontinuity || steeringAngle != desiredAngle){
				gCarSteeringChordState.referenceAngle = WrapCarWheelAngle(
					chordAngle-steeringAngle);
				if(discontinuity)
					gCarSteeringChordState.captureCount++;
			}
			gCarSteeringChordState.lastDelta = steeringAngle-
				gImmersiveCarPhysicalAngle;
			gCarSteeringChordState.continuousAngle = steeringAngle;
			gCarSteeringChordState.pointValidMask = realHandMask;
			}else{
			// A nearly collapsed chord has no reliable direction. Hold the last angle
			// and invalidate only its reference, so the first valid point re-latches
			// instead of emerging on the opposite side at full lock. This 1 cm chord
			// guard is far smaller than the removed 5 cm per-hand dead region.
			steeringAngle = gImmersiveCarPhysicalAngle;
			const uint32 captureCount =
				gCarSteeringChordState.captureCount;
			gCarSteeringChordState = ImmersiveSteeringChordState();
			gCarSteeringChordState.captureCount = captureCount;
			gCarSteeringChordState.rebaseOnNextValid = true;
			}
		}
	}else{
		gCarSteeringChordState = ImmersiveSteeringChordState();
		gImmersiveCarPhysicalAngle = 0.0f;
	}
	// Reach full vehicle lock before fixed 9-and-3 grips can cross sides. This
	// keeps hand identity readable without reducing the car's steering range.
	gImmersiveCarPhysicalAngle =
		clamp(steeringAngle, -maxSteering, maxSteering);
	float steering =
		clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.03f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	// Hand/controller ownership and the rendered wheel orientation are already
	// correct here. Pass the physical wheel direction through unchanged; swapping
	// hands or sockets to compensate would make the grips impossible to reach.
	gImmersiveCarSteering = steering;
	const uint32 capturedHands =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	UpdateImmersiveCarHorn(car, center, axis,
		blockedHands | capturedHands);
	return capturedHands;
}

bool GetMotionSteeringHeading(float *heading)
{
	const int hand = Min(Max(gMotionSteeringHand, 0), EYE_COUNT-1);
	if(!heading || !gTrackedHandAimPoseValid[hand])
		return false;
	// OpenXR is Y-up. Project the selected controller's aim vector onto the
	// horizontal X/Z plane, which is the direct equivalent of the Unreal
	// Z-axis yaw used by common UEVR motion-steering scripts.
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f forward =
		Rotate(gTrackedHandAimPose[hand].orientation, localForward);
	const float horizontalLengthSqr =
		forward.x*forward.x+forward.z*forward.z;
	if(horizontalLengthSqr < 0.01f)
		return false;
	*heading = atan2f(forward.x, -forward.z);
	return true;
}

void UpdateMotionDrivingInput()
{
	if(!IsMotionDrivingEnvironmentActive() ||
	   gVrMenuVisible || gCheatMenuVisible){
		ResetMotionSteeringInteraction();
		return;
	}
	CVehicle *vehicle = FindPlayerVehicle();
	CPlayerPed *player = FindPlayerPed();
	if(!vehicle || !player ||
	   (!IsVrCarDrivingActiveInternal(vehicle) &&
	    !IsVrBikeDrivingActiveInternal(vehicle))){
		ResetMotionSteeringInteraction();
		return;
	}
	RestrictImmersiveVehicleWeaponsToSidearms(player);
	if(gMotionSteeringVehicle != vehicle){
		gMotionSteeringVehicle = vehicle;
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		gMotionSteeringReferenceValid = false;
		gMotionSteeringReferenceHeading = 0.0f;
	}
	float heading;
	if(!GetMotionSteeringHeading(&heading)){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		return;
	}

	// The driver's real controller pose at the first accelerator press is the
	// steering centre. This avoids assuming that an OpenXR controller has one
	// universal "forward" pose: the physical grip angle differs between users
	// and can also change while entering a vehicle. Keep this centre for the
	// whole ride so releasing and pressing the accelerator in a turn cannot
	// recenter the wheel unexpectedly.
	const bool acceleratorPressed = gTrackedHandTrigger[1] >= 0.15f;
	if(!gMotionSteeringReferenceValid){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		if(!acceleratorPressed)
			return;
		gMotionSteeringReferenceHeading = heading;
		gMotionSteeringReferenceValid = true;
		debug("[OpenXR] Motion steering centred on R2 at %.2f degrees\n",
			RADTODEG(gMotionSteeringReferenceHeading));
	}

	// Turning the controller clockwise/right produces negative GTA steering,
	// matching the already validated physical-wheel convention.
	const float maxAngle = DEGTORAD(90.0f);
	const float midAngle = DEGTORAD(30.0f);
	const float fineScale = 0.5f;
	float angle = clamp(-WrapCarWheelAngle(
		heading-gMotionSteeringReferenceHeading), -maxAngle, maxAngle);
	if(Abs(angle) < DEGTORAD(3.0f))
		angle = 0.0f;
	gMotionVehiclePhysicalAngle = angle;
	const float absAngle = Abs(angle);
	float steering;
	if(absAngle <= midAngle)
		steering = angle/midAngle*fineScale;
	else{
		const float sign = angle >= 0.0f ? 1.0f : -1.0f;
		steering = sign*(fineScale+
			(absAngle-midAngle)/(maxAngle-midAngle)*(1.0f-fineScale));
	}
	gMotionVehicleSteering =
		Abs(steering) < 0.01f ? 0.0f :
		clamp(steering, -1.0f, 1.0f);
}

bool NormalizeXrQuaternion(const XrQuaternionf &input,
	XrQuaternionf *output)
{
	if(!output || !_finite(input.x) || !_finite(input.y) ||
	   !_finite(input.z) || !_finite(input.w))
		return false;
	const float lengthSqr = input.x*input.x+input.y*input.y+
		input.z*input.z+input.w*input.w;
	if(!_finite(lengthSqr) || lengthSqr < 0.000001f)
		return false;
	const float inverseLength = 1.0f/sqrtf(lengthSqr);
	output->x = input.x*inverseLength;
	output->y = input.y*inverseLength;
	output->z = input.z*inverseLength;
	output->w = input.w*inverseLength;
	return true;
}

float DotXrQuaternion(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
}

XrQuaternionf MultiplyXrQuaternion(const XrQuaternionf &a,
	const XrQuaternionf &b)
{
	XrQuaternionf result;
	result.x = a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y;
	result.y = a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x;
	result.z = a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w;
	result.w = a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z;
	return result;
}

bool CaptureBikeThrottleReference()
{
	if(!gTrackedHandPoseValid[1])
		return false;
	if(!NormalizeXrQuaternion(gTrackedHandPose[1].orientation,
	   &gBikeThrottleReferenceOrientation))
		return false;
	gBikeThrottleRawTwistAngle = 0.0f;
	gBikeThrottleTwistValid = true;
	gBikeThrottleCaptureCount++;
	return true;
}

bool GetBikeThrottleTwistAngle(float *angle)
{
	if(!angle || !gTrackedHandPoseValid[1])
		return false;
	XrQuaternionf current;
	if(!NormalizeXrQuaternion(gTrackedHandPose[1].orientation, &current))
		return false;
	// q and -q encode the same orientation. Keep the current sample in the same
	// quaternion hemisphere as the captured grip so the signed angle cannot jump
	// by a full revolution when the runtime changes representation.
	if(DotXrQuaternion(gBikeThrottleReferenceOrientation, current) < 0.0f){
		current.x = -current.x;
		current.y = -current.y;
		current.z = -current.z;
		current.w = -current.w;
	}
	XrQuaternionf inverseReference = {
		-gBikeThrottleReferenceOrientation.x,
		-gBikeThrottleReferenceOrientation.y,
		-gBikeThrottleReferenceOrientation.z,
		gBikeThrottleReferenceOrientation.w
	};
	XrQuaternionf relative = MultiplyXrQuaternion(inverseReference, current);
	if(!NormalizeXrQuaternion(relative, &relative))
		return false;

	// OpenXR defines grip -Z along the tube formed by the non-thumb fingers,
	// from little finger to thumb. On the RIGHT motorcycle grip that direction
	// is inward, so local +Z is the anatomical outward throttle axis. Measure
	// only swing-twist around that one controller-local axis: virtual handle
	// calibration, steering and hand position cannot bias the throttle reading.
	const XrVector3f twistAxis = { 0.0f, 0.0f, 1.0f };
	const float projection = relative.x*twistAxis.x+
		relative.y*twistAxis.y+relative.z*twistAxis.z;
	XrQuaternionf twist = {
		twistAxis.x*projection,
		twistAxis.y*projection,
		twistAxis.z*projection,
		relative.w
	};
	if(!NormalizeXrQuaternion(twist, &twist))
		return false;
	const float signedSine = twist.x*twistAxis.x+
		twist.y*twistAxis.y+twist.z*twistAxis.z;
	*angle = WrapCarWheelAngle(2.0f*atan2f(signedSine, twist.w));
	return _finite(*angle) != 0;
}

void UpdateImmersiveBikeThrottle(CBike *bike, bool rightHandleGrabbed)
{
	const bool triggerHeld = gTrackedHandTrigger[1] >= 0.45f;
	if(!bike || !rightHandleGrabbed || !triggerHeld){
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleGestureActive = false;
		gBikeThrottleReferenceValid = false;
		gBikeThrottleRawTwistAngle = 0.0f;
		gBikeThrottleTwistValid = false;
		return;
	}
	if(!gBikeThrottleGestureActive){
		// This edge is valid whether the trigger followed the right grip or was
		// already held when the right hand joined a left-hand ride.
		gBikeThrottleReferenceValid = CaptureBikeThrottleReference();
		gBikeThrottleGestureActive = gBikeThrottleReferenceValid;
		gImmersiveBikeThrottle = 0.0f;
		return;
	}
	if(!gBikeThrottleReferenceValid){
		gImmersiveBikeThrottle = 0.0f;
		return;
	}
	float delta;
	if(!GetBikeThrottleTwistAngle(&delta)){
		// Do not silently recapture neutral mid-squeeze. Zero is the safe response
		// to a lost orientation sample; the original reference remains latched.
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleTwistValid = false;
		return;
	}
	gBikeThrottleRawTwistAngle = delta;
	gBikeThrottleTwistValid = true;
	const float openingAngle = delta;
	const float deadZone = DEGTORAD(1.5f);
	const float fullThrottleAngle = DEGTORAD(43.0f);
	gImmersiveBikeThrottle = openingAngle <= deadZone ? 0.0f :
		clamp((openingAngle-deadZone)/(fullThrottleAngle-deadZone),
			0.0f, 1.0f);
}

void UpdateImmersiveBikeLean(CBike *bike, const CMatrix *anchors,
	const CVector *handPositions, bool left, bool right,
	float physicalSteeringAngle)
{
	// A deliberate wheelie/stand gesture requires both real hands. With one
	// hand, ordinary steering/reaching movements otherwise look exactly like a
	// vertical lean gesture and can pitch the bike without the rider intending it.
	if(!bike || !anchors || !handPositions || !left || !right){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		for(int hand = 0; hand < EYE_COUNT; hand++)
			gBikeLeanReferenceValid[hand] = false;
		return;
	}

	float heightTotal = 0.0f;
	int heightHands = 0;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool grabbed = hand == 0 ? left : right;
		if(!grabbed || !gTrackedHandPoseValid[hand]){
			gBikeLeanReferenceValid[hand] = false;
			continue;
		}
		// Measure the gesture in raw OpenXR tracking space.  Measuring it
		// against the animated motorcycle anchor creates a feedback loop: once
		// the bike leans, its handle moves and can keep the gesture latched even
		// after the real hands return to neutral.
		const float currentTrackingY =
			gTrackedHandPose[hand].position.y;
		if(!gBikeLeanReferenceValid[hand]){
			gBikeLeanReferenceTrackingY[hand] = currentTrackingY;
			gBikeLeanReferenceValid[hand] = true;
			continue;
		}
		heightTotal += currentTrackingY-
			gBikeLeanReferenceTrackingY[hand];
		heightHands++;
	}
	if(heightHands == 0){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}

	BikeLeanCalibration *calibration =
		GetBikeLeanCalibration(bike->GetModelIndex());
	if(!calibration){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}
	const float handHeight = heightTotal/(float)heightHands;
	const float wheelieHeight =
		(float)calibration->wheelieHeightCm/100.0f;
	const float standDepth =
		(float)calibration->standHeightCm/100.0f;

	// Each bike has explicit vertical trigger distances. Raising the hands from
	// their captured neutral height requests a wheelie; lowering them requests
	// the forward standing/stoppie pose. Hysteresis keeps the state stable near
	// the configured boundary while CBike performs its normal smooth blending.
	if(gBikeLeanGestureState == 0){
		if(handHeight >= wheelieHeight)
			gBikeLeanGestureState = -1;
		else if(handHeight <= -standDepth)
			gBikeLeanGestureState = 1;
	}else if(gBikeLeanGestureState < 0){
		if(handHeight <= Max(wheelieHeight*0.50f, 0.025f))
			gBikeLeanGestureState = 0;
	}else{
		if(handHeight >= -Max(standDepth*0.50f, 0.025f))
			gBikeLeanGestureState = 0;
	}
	gImmersiveBikeLean = (float)gBikeLeanGestureState;
}

uint32 UpdateImmersiveBikeInput(const float *grips, uint32 blockedHands)
{
	if(!grips || !IsImmersiveBikeDrivingActiveInternal()){
		ResetImmersiveBikeInteraction();
		return 0;
	}
	CBike *bike = GetActivePlayerBike();
	CPlayerPed *player = FindPlayerPed();
	if(!bike || !player){
		ResetImmersiveBikeInteraction();
		return 0;
	}

	// Only compact one-handed firearms remain available while physically
	// driving. A long gun brought into the seat returns to its body slot.
	RestrictImmersiveVehicleWeaponsToSidearms(player);

	CMatrix anchors[EYE_COUNT];
	CMatrix grabAnchors[EYE_COUNT];
	CVector handPositions[EYE_COUNT];
	bool anchorValid[EYE_COUNT] = {};
	gBikeHandleUnavailableMask = 0;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		anchorValid[hand] =
			BuildBikeHandleMatrixInternal(hand, &anchors[hand], false) &&
			BuildBikeHandleMatrixInternal(hand, &grabAnchors[hand], true);
		if(gTrackedHandPoseValid[hand])
			handPositions[hand] = gBaseCamera.GetPosition()+
				ToGameVector(gTrackedHandPose[hand].position);
		else
			handPositions[hand] = CVector(0.0f, 0.0f, 0.0f);
		gBikeHandleDistance[hand] =
			anchorValid[hand] && gTrackedHandPoseValid[hand] ?
			(handPositions[hand]-grabAnchors[hand].GetPosition()).Magnitude() :
			1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !gTrackedHandPoseValid[hand] ||
			gHeldWeaponSlot[hand] >= 0 ||
			IsWeaponSupportHandInternal(hand) ||
			IsHandBusyWithReload(hand);
		if(unavailable)
			gBikeHandleUnavailableMask |= 1u << hand;
		if(gBikeHandleGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gBikeHandleGrabbed[hand] = false;
			gBikeLeanReferenceValid[hand] = false;
		}
		if(!gBikeHandleGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f &&
		   gBikeHandleDistance[hand] <= 0.17f){
			gBikeHandleGrabbed[hand] = true;
			gBikeLeanReferenceValid[hand] = false;
			debug("[OpenXR] %s motorcycle handle grabbed on %s\n",
				hand == 0 ? "Left" : "Right", GetActiveVrBikeName());
		}
		if(grips[hand] <= 0.30f)
			gBikeHandleGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gBikeHandleGripDown[hand] = true;
	}

	const bool left = gBikeHandleGrabbed[0] && anchorValid[0];
	const bool right = gBikeHandleGrabbed[1] && anchorValid[1];
	BikeHandlePose handlePose;
	if(!BuildBikeHandlePose(bike, &handlePose)){
		ResetImmersiveBikeInteraction();
		return 0;
	}
	const uint32 realHandMask =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	const float maxSteering = DEGTORAD(35.0f);
	gImmersiveBikeDesiredAngle = 0.0f;
	gImmersiveBikeSteeringOverflow = 0.0f;
	float steeringAngle = 0.0f;
	if(realHandMask != 0){
		const CVector neutral =
			anchors[1].GetPosition()-anchors[0].GetPosition();
		if(realHandMask == 3u){
			gBikeOneHandSteeringState = BikeOneHandSteeringState();
			// Literal v0.4.1 two-hand equation: actual labelled chord relative
			// to the neutral labelled chord. This accepted path stays untouched.
			const CVector actual = handPositions[1]-handPositions[0];
			const float neutralPlaneLengthSqr =
				sq(DotProduct(neutral, handlePose.right))+
				sq(DotProduct(neutral, handlePose.forward));
			const float actualPlaneLengthSqr =
				sq(DotProduct(actual, handlePose.right))+
				sq(DotProduct(actual, handlePose.forward));
			const bool pointValid = neutralPlaneLengthSqr > 0.0001f &&
				actualPlaneLengthSqr > 0.0001f &&
				_finite(actual.x) && _finite(actual.y) && _finite(actual.z);
			float rawSteeringAngle = 0.0f;
			if(pointValid)
				rawSteeringAngle = WrapBikeSteeringAngle(
					PlanarBikeHandleAngle(actual, handlePose.right,
						handlePose.forward)-
					PlanarBikeHandleAngle(neutral, handlePose.right,
						handlePose.forward));
			if(pointValid){
				const bool ownershipChanged =
					!gBikeSteeringChordState.valid ||
					gBikeSteeringChordState.vehicle != bike ||
					gBikeSteeringChordState.realHandMask != realHandMask;
				if(ownershipChanged){
					const bool forceReference =
						gBikeSteeringChordState.rebaseOnNextValid;
					const uint32 nextCapture =
						gBikeSteeringChordState.captureCount+1;
					gBikeSteeringChordState = ImmersiveSteeringChordState();
					gBikeSteeringChordState.vehicle = bike;
					gBikeSteeringChordState.realHandMask = realHandMask;
					gBikeSteeringChordState.captureCount = nextCapture;
					gBikeSteeringChordState.valid = true;
					gBikeSteeringChordState.referenceActive = forceReference;
					if(forceReference)
						gBikeSteeringChordState.referenceAngle =
							WrapBikeSteeringAngle(rawSteeringAngle-
								gImmersiveBikePhysicalAngle);
				}
				steeringAngle = !gBikeSteeringChordState.referenceActive ?
					rawSteeringAngle :
					WrapBikeSteeringAngle(rawSteeringAngle-
						gBikeSteeringChordState.referenceAngle);
				const float desiredAngle = steeringAngle;
				gImmersiveBikeDesiredAngle = desiredAngle;
				const float physicalDelta = WrapBikeSteeringAngle(
					desiredAngle-gImmersiveBikePhysicalAngle);
				const bool discontinuity = !_finite(desiredAngle) ||
					Abs(physicalDelta) > DEGTORAD(45.0f);
				steeringAngle = discontinuity ? gImmersiveBikePhysicalAngle :
					clamp(desiredAngle, -maxSteering, maxSteering);
				gImmersiveBikeSteeringOverflow = WrapBikeSteeringAngle(
					desiredAngle-steeringAngle);
				if(discontinuity || steeringAngle != desiredAngle){
					gBikeSteeringChordState.referenceAngle =
						WrapBikeSteeringAngle(rawSteeringAngle-steeringAngle);
					gBikeSteeringChordState.referenceActive = true;
					if(discontinuity)
						gBikeSteeringChordState.captureCount++;
				}
				gBikeSteeringChordState.lastDelta = WrapBikeSteeringAngle(
					steeringAngle-gImmersiveBikePhysicalAngle);
				gBikeSteeringChordState.continuousAngle = steeringAngle;
				gBikeSteeringChordState.pointValidMask = realHandMask;
			}else{
				steeringAngle = gImmersiveBikePhysicalAngle;
				const uint32 captureCount =
					gBikeSteeringChordState.captureCount;
				gBikeSteeringChordState = ImmersiveSteeringChordState();
				gBikeSteeringChordState.captureCount = captureCount;
				gBikeSteeringChordState.rebaseOnNextValid = true;
			}
		}else{
			const bool ownershipChanged =
				!gBikeOneHandSteeringState.valid ||
				gBikeOneHandSteeringState.vehicle != bike ||
				gBikeOneHandSteeringState.realHandMask != realHandMask ||
				!gBikeSteeringChordState.valid ||
				gBikeSteeringChordState.vehicle != bike ||
				gBikeSteeringChordState.realHandMask != realHandMask;
			if(ownershipChanged){
				const uint32 nextCapture =
					gBikeSteeringChordState.captureCount+1;
				gBikeSteeringChordState = ImmersiveSteeringChordState();
				gBikeSteeringChordState.vehicle = bike;
				gBikeSteeringChordState.realHandMask = realHandMask;
				gBikeSteeringChordState.captureCount = nextCapture;
				gBikeSteeringChordState.valid = CaptureBikeOneHandSteering(
					bike, realHandMask, neutral, handlePose,
					gImmersiveBikePhysicalAngle);
				gBikeSteeringChordState.referenceActive = true;
			}
			float desiredAngle = gImmersiveBikePhysicalAngle;
			const bool pointValid = gBikeSteeringChordState.valid &&
				SolveBikeOneHandSteering(bike, realHandMask, &desiredAngle);
			steeringAngle = desiredAngle;
			gImmersiveBikeDesiredAngle = desiredAngle;
			const float physicalDelta = WrapBikeSteeringAngle(
				desiredAngle-gImmersiveBikePhysicalAngle);
			const bool discontinuity = !pointValid || !_finite(desiredAngle) ||
				Abs(physicalDelta) > DEGTORAD(45.0f);
			steeringAngle = discontinuity ? gImmersiveBikePhysicalAngle :
				clamp(desiredAngle, -maxSteering, maxSteering);
			gImmersiveBikeSteeringOverflow = WrapBikeSteeringAngle(
				desiredAngle-steeringAngle);
			if(discontinuity || steeringAngle != desiredAngle){
				// Rebase at the exact applied stop without sampling the live leaned bike.
				// Continuing outward stays at the stop, while the first inward movement
				// responds immediately and can never unwind hidden virtual overtravel.
				gBikeSteeringChordState.valid = RebaseBikeOneHandSteering(
					bike, realHandMask, steeringAngle);
				gBikeSteeringChordState.captureCount++;
			}
			gBikeSteeringChordState.lastDelta = WrapBikeSteeringAngle(
				steeringAngle-gImmersiveBikePhysicalAngle);
			gBikeSteeringChordState.continuousAngle = steeringAngle;
			gBikeSteeringChordState.pointValidMask =
				pointValid ? realHandMask : 0u;
		}
		gImmersiveBikePhysicalAngle = clamp(
			steeringAngle, -maxSteering, maxSteering);
	}else{
		gBikeSteeringChordState = ImmersiveSteeringChordState();
		gBikeOneHandSteeringState = BikeOneHandSteeringState();
		gImmersiveBikePhysicalAngle = 0.0f;
	}
	float steering = clamp(steeringAngle/maxSteering,
		-1.0f, 1.0f);
	const float deadZone = 0.035f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	gImmersiveBikeSteering = steering;
	UpdateImmersiveBikeLean(bike, anchors, handPositions, left, right,
		gImmersiveBikePhysicalAngle);
	UpdateImmersiveBikeThrottle(bike, right);
	return (left ? 1u : 0u) | (right ? 2u : 0u);
}

bool IsPhysicalScopeWeaponTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_CAMERA:
		return true;
	default:
		return false;
	}
}

float GetTrackedScopeZoomInternal()
{
	switch(gTrackedScopeWeaponType){
	case WEAPONTYPE_SNIPERRIFLE: return 2.5f;
	case WEAPONTYPE_LASERSCOPE: return 3.0f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 1.8f;
	case WEAPONTYPE_CAMERA: return 1.6f;
	default: return 1.0f;
	}
}

void ApplyTrackedScopeZoomToUv(float *scaleX, float *scaleY,
	float *offsetX, float *offsetY)
{
	if(!scaleX || !scaleY || !offsetX || !offsetY)
		return;
	const float zoom = GetTrackedScopeZoomInternal();
	if(zoom <= 1.0f)
		return;
	// Crop around the stable centre of the symmetric source render.  This keeps
	// the OpenXR eye poses/FOV untouched, so activating an optic cannot reintroduce
	// the old stereo warp or make RenderWare cull a zoomed eye differently.
	*scaleX /= zoom;
	*scaleY /= zoom;
	*offsetX = 0.5f+(*offsetX-0.5f)/zoom;
	*offsetY = 0.5f+(*offsetY-0.5f)/zoom;
}

bool IsPhysicalGunTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
	case WEAPONTYPE_MP5:
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
	case WEAPONTYPE_HELICANNON:
	case WEAPONTYPE_CAMERA:
		return true;
	default:
		return false;
	}
}

bool IsPhysicalMeleeTypeInternal(int weaponType)
{
	// Vice City's authored melee block is contiguous. Unarmed and brass
	// knuckles live in slot zero and are driven directly by a closed fist; the
	// remaining types share the physical MELEE holster slot.
	return weaponType >= WEAPONTYPE_UNARMED &&
		weaponType <= WEAPONTYPE_CHAINSAW;
}

bool IsPhysicalThrowableTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_GRENADE:
	case WEAPONTYPE_DETONATOR_GRENADE:
	case WEAPONTYPE_TEARGAS:
	case WEAPONTYPE_MOLOTOV:
		return true;
	default:
		return false;
	}
}

CVector BuildTrackedThrowableVelocity(int weaponType, CVector direction)
{
	if(!IsPhysicalThrowableTypeInternal(weaponType) ||
	   direction.MagnitudeSqr() < 0.0001f)
		return CVector(0.0f, 0.0f, 0.0f);
	direction.Normalise();
	// Game physics stores velocity in world units per normalized 30 Hz tick.
	// A fixed launch speed makes the preview stable while R2 is held and lets the
	// player choose the landing point by moving the controller, rather than by
	// timing an invisible legacy animation charge.
	return direction*0.46f;
}

bool BuildTrackedThrowableLaunch(int weaponType, const CVector &requestedSource,
	CVector direction, CVector *source, CVector *velocity)
{
	if(!source || !velocity || !IsPhysicalThrowableTypeInternal(weaponType) ||
	   direction.MagnitudeSqr() < 0.0001f)
		return false;
	direction.Normalise();
	*source = requestedSource;
	*velocity = BuildTrackedThrowableVelocity(weaponType, direction);

	// A tracked controller can physically pass through a wall. Clamp the spawn
	// point to the visible side of any geometry between the player's head and the
	// requested hand/muzzle point; the preview and the real projectile both call
	// this helper, so they cannot disagree about that correction.
	CPlayerPed *player = FindPlayerPed();
	if(player){
		const CVector anchor = gBaseCamera.GetPosition();
		CVector reach = requestedSource-anchor;
		const float reachLength = reach.Magnitude();
		if(reachLength > 0.001f){
			reach /= reachLength;
			CColPoint point;
			CEntity *hitEntity = nil;
			CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
			CWorld::pIgnoreEntity = player;
			const bool hit = CWorld::ProcessLineOfSight(anchor,
				requestedSource, point, hitEntity, true, true, true, true,
				true, false, false, false);
			CWorld::pIgnoreEntity = savedIgnoreEntity;
			if(hit){
				const float safeDistance = Max(0.0f,
					(point.point-anchor).Magnitude()-0.08f);
				*source = anchor+reach*safeDistance;
			}
		}
	}
	return velocity->MagnitudeSqr() > 0.0001f;
}

bool IsPhysicalWeaponTypeInternal(int weaponType)
{
	return IsPhysicalGunTypeInternal(weaponType) ||
		IsPhysicalMeleeTypeInternal(weaponType) ||
		IsPhysicalThrowableTypeInternal(weaponType);
}

bool IsManualReloadWeaponTypeInternal(int weaponType)
{
	// The first physical-reload pass covers detachable magazines which can be
	// manipulated with one free hand.  Python is deliberately excluded: a
	// revolver needs a separate cylinder/speed-loader interaction rather than a
	// rectangular magazine pretending to fit into its grip.
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
		return true;
	default:
		return false;
	}
}

bool IsTwoHandedWeaponTypeInternal(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_MP5:
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
		return true;
	default:
		return false;
	}
}

float GetOneHandAimSwayDegrees(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_MP5: return 1.25f;
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN: return 1.50f;
	case WEAPONTYPE_STUBBY_SHOTGUN: return 2.00f;
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER: return 1.75f;
	case WEAPONTYPE_SNIPERRIFLE: return 2.25f;
	case WEAPONTYPE_LASERSCOPE: return 2.00f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 3.00f;
	case WEAPONTYPE_FLAMETHROWER: return 2.25f;
	case WEAPONTYPE_M60: return 2.50f;
	case WEAPONTYPE_MINIGUN: return 3.25f;
	default: return 0.0f;
	}
}

bool IsWeaponSupportHandInternal(int hand)
{
	for(int primary = 0; primary < EYE_COUNT; primary++)
		if(gWeaponSupportHand[primary] == hand)
			return true;
	return false;
}

void ClearWeaponSupportForHand(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gWeaponSupportHand[hand] = -1;
	for(int primary = 0; primary < EYE_COUNT; primary++)
		if(gWeaponSupportHand[primary] == hand)
			gWeaponSupportHand[primary] = -1;
}

bool BuildTrackedWeaponHandBasis(int hand, CVector *position,
	CVector *right, CVector *up, CVector *forward)
{
	if(hand < 0 || hand >= EYE_COUNT || !position || !right || !up ||
	   !forward || !gTrackedHandPoseValid[hand])
		return false;
	const XrPosef &gripPose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const int modelHand = 1-hand;
	*up = ToGameVector(Rotate(gripPose.orientation, localRight))*
		(modelHand == 0 ? 1.0f : -1.0f);
	if(gTrackedHandAimPoseValid[hand])
		*forward = ToGameVector(Rotate(
			gTrackedHandAimPose[hand].orientation, localForward));
	else
		*forward = ToGameVector(Rotate(gripPose.orientation, localUp));
	forward->Normalise();
	*up -= *forward*DotProduct(*up, *forward);
	if(up->MagnitudeSqr() < 0.0001f)
		*up = ToGameVector(Rotate(gripPose.orientation, localRight))*
			(modelHand == 0 ? 1.0f : -1.0f);
	up->Normalise();
	*right = CrossProduct(*up, *forward);
	right->Normalise();
	const CVector gripForward =
		ToGameVector(Rotate(gripPose.orientation, localForward));
	if(DotProduct(*right, gripForward) < 0.0f)
		*right *= -1.0f;
	*position = gBaseCamera.GetPosition()+ToGameVector(gripPose.position);
	return true;
}

bool BuildCanonicalSupportWeaponFrame(const CVector &forwardCandidate,
	const CVector &upCandidate, CVector *right, CVector *up, CVector *forward)
{
	if(!right || !up || !forward ||
	   !_finite(forwardCandidate.x) || !_finite(forwardCandidate.y) ||
	   !_finite(forwardCandidate.z) || !_finite(upCandidate.x) ||
	   !_finite(upCandidate.y) || !_finite(upCandidate.z))
		return false;
	*forward = forwardCandidate;
	if(forward->MagnitudeSqr() < 0.0001f)
		return false;
	forward->Normalise();
	*up = upCandidate-*forward*DotProduct(upCandidate, *forward);
	if(up->MagnitudeSqr() < 0.0001f)
		return false;
	up->Normalise();
	// A left-hand weapon render may carry a reflected model matrix. Barrel and
	// top are the two physical axes that must survive that reflection; derive the
	// lateral axis from them instead of inheriting whichever axis was mirrored.
	*right = CrossProduct(*forward, *up);
	if(right->MagnitudeSqr() < 0.0001f)
		return false;
	right->Normalise();
	*up = CrossProduct(*right, *forward);
	up->Normalise();
	return true;
}

bool BuildSupportGripVector(int primaryHand, int weaponType,
	CVector *pivot, CVector *expected, CVector *frameRight = nil,
	CVector *frameUp = nil, CVector *frameForward = nil)
{
	CVector rawRight, rawUp, rawForward;
	if(!pivot || !expected || !IsTwoHandedWeaponTypeInternal(weaponType) ||
	   ((frameRight || frameUp || frameForward) &&
	    (!frameRight || !frameUp || !frameForward)) ||
	   !BuildTrackedWeaponHandBasis(primaryHand, pivot, &rawRight, &rawUp,
		&rawForward))
		return false;
	const SupportGripCalibration *calibration =
		GetSupportGripCalibration(primaryHand, weaponType);
	if(!calibration)
		return false;

	CVector socketRight = rawRight;
	CVector socketUp = rawUp;
	CVector socketForward = rawForward;
	CVector visualRight = rawRight;
	CVector visualUp = rawUp;
	CVector visualForward = rawForward;
	CVector socketOrigin = *pivot;
	if(calibration->poseVersion >= SUPPORT_GRIP_POSE_VERSION){
		const WeaponCalibration *weaponCalibration =
			GetWeaponCalibration(primaryHand, weaponType);
		if(!weaponCalibration)
			return false;
		// V5 support XYZ is authored in the exact unbraced visible weapon frame.
		// Moving or rotating the model therefore carries its socket and height; the
		// raw primary grip remains only the physical two-hand brace pivot.
		CMatrix weaponModel;
		BuildWeaponModelCalibrationMatrix(primaryHand, *weaponCalibration,
			&weaponModel);
		CVector barrel = weaponModel.GetRight();
		CVector top = weaponModel.GetForward();
		CVector lateral;
		if(!BuildCanonicalSupportWeaponFrame(barrel, top,
		   &lateral, &top, &barrel))
			return false;
		// Position uses the intuitive model-local contract X=lateral, Y=barrel,
		// Z=top. Wrist calibration retains the deterministic basis in which the
		// user's seven release poses were authored, so their palm rotations remain
		// visually identical after the v4->v5 socket migration.
		socketRight = lateral;
		socketForward = barrel;
		socketUp = top;
		visualRight = barrel;
		visualUp = lateral*-1.0f;
		visualForward = top*-1.0f;
		socketOrigin = weaponModel.GetPosition();
	}
	const CVector socket = socketOrigin+
		socketRight*((float)calibration->offsetX/200.0f)+
		socketForward*((float)calibration->offsetY/200.0f)+
		socketUp*((float)calibration->offsetZ/200.0f);
	*expected = socket-*pivot;
	if(frameRight){
		// Return the stable wrist authoring frame. This is the visual fallback when
		// the current weapon atomic/contact matrix is unavailable.
		if(calibration->poseVersion < SUPPORT_GRIP_POSE_VERSION &&
		   !BuildCanonicalSupportWeaponFrame(visualForward, visualUp,
		   &visualRight, &visualUp, &visualForward))
			return false;
		*frameRight = visualRight;
		*frameUp = visualUp;
		*frameForward = visualForward;
	}
	return expected->MagnitudeSqr() >= 0.0001f;
}

bool BuildTwoHandRotation(int primaryHand, int weaponType, CVector *pivot,
	CVector *axis, float *angle)
{
	if(!pivot || !axis || !angle || primaryHand < 0 ||
	   primaryHand >= EYE_COUNT)
		return false;
	const int supportHand = gWeaponSupportHand[primaryHand];
	if(supportHand < 0 || supportHand >= EYE_COUNT ||
	   !gTrackedHandPoseValid[supportHand])
		return false;
	CVector expected;
	if(!BuildSupportGripVector(primaryHand, weaponType, pivot, &expected))
		return false;
	const CVector supportPosition = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandPose[supportHand].position);
	CVector actual = supportPosition-*pivot;
	if(actual.MagnitudeSqr() < 0.0001f)
		return false;
	expected.Normalise();
	actual.Normalise();
	const float cosine = Min(Max(DotProduct(expected, actual), -1.0f), 1.0f);
	*axis = CrossProduct(expected, actual);
	const float sine = axis->Magnitude();
	if(sine < 0.000001f){
		// Parallel vectors need no correction. An exactly opposite support
		// direction has no unique shortest-arc axis, so retaining the current
		// pose is safer than choosing an arbitrary axis and flipping the gun.
		*axis = CVector(0.0f, 0.0f, 1.0f);
		*angle = 0.0f;
		return true;
	}
	*axis *= 1.0f/sine;
	// Match the proven STALKER brace: atan2 remains stable close to 0/180
	// degrees, while the 90-degree safety cap prevents a misplaced support
	// controller from turning a long gun inside out.
	*angle = Min(atan2f(sine, cosine), DEGTORAD(90.0f));
	return true;
}

// Apply the two-hand brace only to a copy used for rendering the hands. Raw
// gTrackedHandPose/gTrackedHandAimPose remain authoritative for grip tests,
// holsters, reloads and every other physical interaction.
bool ApplyTwoHandVisualHandLock(int hand, CMatrix *handMatrix,
	CVector *aimOrigin, CVector *aimDirection, float *grip, float *trigger)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return false;
	int primary = -1;
	int support = -1;
	int primarySlot = -1;
	int weaponType = -1;
	bool calibrationPreview = false;
	for(int candidate = 0; candidate < EYE_COUNT; candidate++){
		const int slot = gHeldWeaponSlot[candidate];
		if(slot < 0)
			continue;
		const int candidateType = GetVrWeaponTypeForSlot(slot);
		if(!IsTwoHandedWeaponTypeInternal(candidateType))
			continue;
		const bool candidatePreview =
			IsTrackedWeaponCalibrationTarget(candidate, candidateType);
		int candidateSupport = gWeaponSupportHand[candidate];
		if(candidatePreview){
			// Calibration must not rotate the weapon or its laser. Show only the
			// otherwise-free support glove at the saved socket so pose selection is
			// visible while the physical two-hand solver remains disabled.
			candidateSupport = 1-candidate;
			if(hand != candidateSupport || gHeldWeaponSlot[candidateSupport] >= 0 ||
			   !gTrackedHandPoseValid[candidateSupport])
				continue;
		}else if(candidateSupport < 0 || candidateSupport >= EYE_COUNT ||
		   (hand != candidate && hand != candidateSupport))
			continue;
		primary = candidate;
		support = candidateSupport;
		primarySlot = slot;
		weaponType = candidateType;
		calibrationPreview = candidatePreview;
		break;
	}
	if(primary < 0)
		return false;

	CVector pivot, axis;
	float angle = 0.0f;
	if(!calibrationPreview &&
	   !BuildTwoHandRotation(primary, weaponType, &pivot, &axis, &angle))
		return false;
	if(hand == primary){
		// The primary glove follows the exact brace delta already shared by the
		// visible weapon, muzzle, laser and shot. Its physical grip remains the
		// pivot, so normally only orientation changes.
		if(handMatrix && angle != 0.0f){
			handMatrix->GetRight() = RotateAroundAxis(
				handMatrix->GetRight(), axis, angle);
			handMatrix->GetUp() = RotateAroundAxis(
				handMatrix->GetUp(), axis, angle);
			handMatrix->GetForward() = RotateAroundAxis(
				handMatrix->GetForward(), axis, angle);
			handMatrix->GetPosition() = pivot+RotateAroundAxis(
				handMatrix->GetPosition()-pivot, axis, angle);
		}
		if(aimOrigin && angle != 0.0f)
			*aimOrigin = pivot+RotateAroundAxis(*aimOrigin-pivot, axis, angle);
		if(aimDirection && angle != 0.0f){
			*aimDirection = RotateAroundAxis(*aimDirection, axis, angle);
			aimDirection->Normalise();
		}
		return true;
	}

	CVector rawPivot, weaponRight, weaponUp, weaponForward;
	CVector expected;
	if(hand != support ||
	   !BuildSupportGripVector(primary, weaponType, &rawPivot, &expected,
	   &weaponRight, &weaponUp, &weaponForward))
		return false;
	if(calibrationPreview)
		pivot = rawPivot;
	if(angle != 0.0f){
		expected = RotateAroundAxis(expected, axis, angle);
		weaponRight = RotateAroundAxis(weaponRight, axis, angle);
		weaponUp = RotateAroundAxis(weaponUp, axis, angle);
		weaponForward = RotateAroundAxis(weaponForward, axis, angle);
	}
	const CVector lockedSocket = pivot+expected;
	const SupportGripCalibration *calibration =
		GetSupportGripCalibration(primary, weaponType);
	// RenderVrTrackedHands submits both weapon models before either glove, so a
	// matching current-frame contact matrix is the exact final basis after the
	// authored axis conversion, per-weapon calibration and two-hand brace. Use
	// it for the support wrist when available; an absent atomic or stale cache
	// safely falls back to the braced physical weapon basis above.
	if(calibration && calibration->poseVersion >= SUPPORT_GRIP_POSE_VERSION &&
	   gTrackedWeaponContactMatrixSlot[primary] == primarySlot &&
	   gTrackedWeaponContactMatrixType[primary] == weaponType &&
	   gTrackedWeaponContactMatrixFrame[primary] == CTimer::GetFrameCounter()){
		CVector barrel = gTrackedWeaponContactMatrix[primary].GetRight();
		CVector top = gTrackedWeaponContactMatrix[primary].GetForward();
		CVector lateral;
		if(BuildCanonicalSupportWeaponFrame(barrel, top,
		   &lateral, &top, &barrel)){
			weaponRight = barrel;
			weaponUp = lateral*-1.0f;
			weaponForward = top*-1.0f;
		}
	}

	// The v5 socket is canonical model-local, while this wrist authoring basis is
	// intentionally v4-compatible so the seven in-headset palm calibrations keep
	// their exact appearance. Per-weapon rotations below turn that stable base
	// into the final grip. Nothing here feeds the physical socket or brace solve.
	if(!BuildCanonicalSupportWeaponFrame(weaponForward, weaponUp,
	   &weaponRight, &weaponUp, &weaponForward))
		return false;
	const int gripType = calibration ? calibration->gripType :
		VR_SUPPORT_GRIP_MAGAZINE;
	// RenderVrTrackedHand decodes its palm normal from Matrix::Right with this
	// exact per-hand sign. These equations cover all four combinations before the
	// final LEFT mesh reflection: magazine LEFT/RIGHT face inward from opposite sides;
	// support-from-below LEFT/RIGHT both face the weapon's underside.
	const float palmSign = support == 0 ? -1.0f : 1.0f;
	CVector encodedPalmAxis;
	if(gripType == VR_SUPPORT_GRIP_FROM_BELOW){
		// Both palms face the same underside of the weapon. Matrix::Right must
		// carry the inverse hand sign so RenderVrTrackedHand decodes that shared
		// anatomical palm normal for either support hand.
		encodedPalmAxis = weaponUp*(-palmSign);
	}else{
		// Magazine/foregrip mode closes the palms inward from opposite sides.
		encodedPalmAxis = weaponRight;
	}
	encodedPalmAxis.Normalise();
	CVector handUp = encodedPalmAxis*palmSign;
	CVector handForward = weaponForward;
	handForward.Normalise();
	CVector handRight = CrossProduct(handUp, handForward);
	if(handRight.MagnitudeSqr() < 0.0001f)
		return false;
	handRight.Normalise();

	// Optional per-weapon/per-modelset wrist correction. X/Y/Z are the final
	// rendered hand's right, palm-normal and finger-forward axes respectively;
	// the support socket and two-hand weapon steering remain physically unchanged.
	const float rotationX = DEGTORAD((float)(calibration ?
		calibration->rotationX : 0)/WEAPON_CALIBRATION_VALUE_SCALE);
	const float rotationY = DEGTORAD((float)(calibration ?
		calibration->rotationY : 0)/WEAPON_CALIBRATION_VALUE_SCALE);
	const float rotationZ = DEGTORAD((float)(calibration ?
		calibration->rotationZ : 0)/WEAPON_CALIBRATION_VALUE_SCALE);
	if(rotationX != 0.0f){
		handUp = RotateAroundAxis(handUp, handRight, rotationX);
		handForward = RotateAroundAxis(handForward, handRight, rotationX);
	}
	if(rotationY != 0.0f){
		handRight = RotateAroundAxis(handRight, handUp, rotationY);
		handForward = RotateAroundAxis(handForward, handUp, rotationY);
	}
	if(rotationZ != 0.0f){
		handRight = RotateAroundAxis(handRight, handForward, rotationZ);
		handUp = RotateAroundAxis(handUp, handForward, rotationZ);
	}
	handForward.Normalise();
	handUp -= handForward*DotProduct(handUp, handForward);
	if(handUp.MagnitudeSqr() < 0.0001f)
		return false;
	handUp.Normalise();
	handRight = CrossProduct(handUp, handForward);
	handRight.Normalise();
	// The canonicalised LEFT UXRH asset needs one extra anatomical roll in the
	// support path; the RIGHT asset already matches this authored wrist frame.
	// Rotate only the physical LEFT support hand around the unchanged finger
	// direction. Applying this to RIGHT turns an already-correct palm upside down.
	if(support == 0){
		handUp *= -1.0f;
		handRight *= -1.0f;
	}
	encodedPalmAxis = handUp*palmSign;
	if(handMatrix){
		// The baked LEFT/RIGHT UXRH meshes are canonicalised into the same local
		// axes; anatomical handedness is supplied by RenderVrTrackedHand's final
		// lateral-axis parity check.  Preserve a negative sentinel only for LEFT so
		// that check reflects its mesh once.  Encoding a proper frame for both hands
		// made the LEFT support glove look like a second RIGHT hand (wrong thumb).
		handMatrix->GetRight() = encodedPalmAxis;
		handMatrix->GetUp() = handForward;
		handMatrix->GetForward() = handRight*
			(support == 0 ? -1.0f : 1.0f);
		handMatrix->GetPosition() = lockedSocket;
	}
	if(aimOrigin)
		*aimOrigin = lockedSocket;
	if(aimDirection)
		*aimDirection = handForward;
	if(grip)
		*grip = gripType == VR_SUPPORT_GRIP_FROM_BELOW ? 0.55f : 1.0f;
	if(trigger)
		*trigger = gripType == VR_SUPPORT_GRIP_FROM_BELOW ? 0.65f : 1.0f;
	return true;
}

bool BuildWeaponHolsterMatrix(int slot, CMatrix *matrix)
{
	if(!matrix || slot <= WEAPONSLOT_UNARMED || slot >= TOTAL_WEAPON_SLOTS)
		return false;
	if(IsVrDrivingActiveInternal() &&
	   !IsBikeSidearmTypeInternal(GetVrWeaponTypeForSlot(slot)))
		return false;
	const int point = FindHolsterPointForSlot(slot);
	if(point < 0)
		return false;
	static const float lateral[HOLSTER_POINT_COUNT] = {
		-0.27f, 0.27f, -0.21f, 0.21f, 0.0f, 0.24f, -0.24f
	};
	static const float vertical[HOLSTER_POINT_COUNT] = {
		-0.58f, -0.58f, -0.30f, -0.30f, -0.36f, -0.14f, -0.14f
	};
	static const float depth[HOLSTER_POINT_COUNT] = {
		0.07f, 0.07f, 0.12f, 0.12f, 0.14f, -0.23f, -0.23f
	};
	const bool behind = point == HOLSTER_BACK_LEFT ||
		point == HOLSTER_BACK_RIGHT;
	CVector holsterAnchor = gBaseCamera.GetPosition();
	CVector holsterLeft = gBaseCamera.GetRight();
	CVector holsterUp(0.0f, 0.0f, 1.0f);
	CVector holsterForward = gBaseCamera.GetForward();
	if(gGameplaySpace && gHeadLocomotionPoseValid){
		// The gameplay camera anchor follows Tommy, while the OpenXR view pose
		// contains the player's real movement inside the guardian.  Belt sockets
		// must include both.  Otherwise physically stepping backwards moves the
		// eyes away from a stationary belt and makes every weapon jump in front.
		const XrVector3f eyeCentre = {
			(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
			(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
			(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f
		};
		const CVector trackedOffset = ToGameVector(eyeCentre);
		if(_finite(trackedOffset.x) && _finite(trackedOffset.y) &&
		   _finite(trackedOffset.z))
			holsterAnchor += trackedOffset;

		// A body holster follows head yaw continuously, independently of the
		// locomotion mode, but never inherits head pitch/roll.  This matches a
		// wearable belt: looking around rotates its layout, while world-up keeps
		// waist/chest/back heights stable.
		const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
		XrVector3f headForward = Rotate(
			gLocatedViews[0].pose.orientation, localForward);
		const float horizontalLengthSqr =
			headForward.x*headForward.x+headForward.z*headForward.z;
		if(_finite(headForward.x) && _finite(headForward.y) &&
		   _finite(headForward.z) && horizontalLengthSqr > 0.0025f){
			const float inverseLength = 1.0f/sqrtf(horizontalLengthSqr);
			gHolsterHeadForwardTracking = {
				headForward.x*inverseLength, 0.0f,
				headForward.z*inverseLength
			};
			gHolsterHeadForwardValid = true;
		}
		// Yaw is undefined while looking almost exactly vertically. Preserve the
		// last reliable HMD heading in that narrow cone instead of amplifying pose
		// noise or snapping all sockets back to Tommy's body direction.
		if(gHolsterHeadForwardValid)
			holsterForward = ToGameVector(gHolsterHeadForwardTracking);
	}
	holsterForward -= holsterUp*DotProduct(holsterForward, holsterUp);
	if(holsterForward.MagnitudeSqr() < 0.000001f){
		// Looking almost straight up/down has no useful forward projection. The
		// camera's lateral axis still carries stable yaw, so recover forward from
		// it instead of snapping the belt to an arbitrary world heading.
		holsterLeft = gBaseCamera.GetRight();
		holsterLeft -= holsterUp*DotProduct(holsterLeft, holsterUp);
		if(holsterLeft.MagnitudeSqr() < 0.000001f)
			holsterLeft = CVector(-1.0f, 0.0f, 0.0f);
		holsterLeft.Normalise();
		holsterForward = CrossProduct(holsterLeft, holsterUp);
	}
	holsterForward.Normalise();
	holsterLeft = CrossProduct(holsterUp, holsterForward);
	holsterLeft.Normalise();
	holsterUp = CrossProduct(holsterForward, holsterLeft);
	holsterUp.Normalise();
	matrix->SetUnity();
	// Holstered guns hang barrel-down with their broad side facing away from the
	// body on both the front and back planes. This basis deliberately includes
	// the inverse of the legacy weapon frame's fixed authored adjustment.
	matrix->GetRight() = holsterForward*(behind ? 1.0f : -1.0f);
	matrix->GetForward() = holsterUp;
	matrix->GetUp() = holsterLeft*(behind ? 1.0f : -1.0f);
	matrix->GetPosition() = holsterAnchor +
		holsterLeft*lateral[point] +
		holsterUp*vertical[point] +
		holsterForward*depth[point];
	return true;
}

#ifdef RW_D3D12
void DestroyTrackedForegroundResolvePipeline()
{
	gTrackedForegroundResolveAvailable = false;
	if(gTrackedForegroundResolvePipeline){
		gTrackedForegroundResolvePipeline->Release();
		gTrackedForegroundResolvePipeline = nil;
	}
	if(gTrackedForegroundResolveRootSignature){
		gTrackedForegroundResolveRootSignature->Release();
		gTrackedForegroundResolveRootSignature = nil;
	}
}

bool EnsureTrackedForegroundResolvePipeline()
{
	if(gTrackedForegroundResolvePipeline &&
	   gTrackedForegroundResolveRootSignature)
		return true;
	DestroyTrackedForegroundResolvePipeline();
	ID3D12Device *device = rw::d3d12::getDevice();
	if(!device)
		return false;

	D3D12_DESCRIPTOR_RANGE range = {};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 8;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC signature = {};
	signature.NumParameters = 2;
	signature.pParameters = params;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT result = D3D12SerializeRootSignature(&signature,
		D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(result)){
		if(errors)
			VrLog("Tracked foreground root signature: %s\n",
				(const char*)errors->GetBufferPointer());
		if(errors) errors->Release();
		if(serialized) serialized->Release();
		return false;
	}
	if(errors) errors->Release();
	result = device->CreateRootSignature(0, serialized->GetBufferPointer(),
		serialized->GetBufferSize(),
		IID_PPV_ARGS(&gTrackedForegroundResolveRootSignature));
	serialized->Release();
	if(FAILED(result)){
		DestroyTrackedForegroundResolvePipeline();
		return false;
	}

	static const char *shaderSource =
		"cbuffer Constants : register(b0) { float2 uvScale; float2 uvOffset;"
		" float2 uvMin; float2 uvMax; };"
		"Texture2D image : register(t0); SamplerState imageSampler : register(s0);"
		"struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };"
		"VSOut VSMain(uint id : SV_VertexID) { VSOut output;"
		" float2 position=id==0?float2(-1.0,-1.0):"
		"                 id==1?float2(-1.0,3.0):float2(3.0,-1.0);"
		" output.position=float4(position,0.0,1.0);"
		" output.uv=float2((position.x+1.0)*0.5,(1.0-position.y)*0.5);"
		" return output; }"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float2 uv=clamp(input.uv*uvScale+uvOffset,uvMin,uvMax);"
		" return image.Sample(imageSampler,uv); }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *pixelShader = nil;
	errors = nil;
	result = D3DCompile(shaderSource, strlen(shaderSource),
		"tracked_foreground", nil, nil, "VSMain", "vs_5_0", 0, 0,
		&vertexShader, &errors);
	if(FAILED(result)){
		if(errors)
			VrLog("Tracked foreground vertex shader: %s\n",
				(const char*)errors->GetBufferPointer());
		if(errors) errors->Release();
		if(vertexShader) vertexShader->Release();
		DestroyTrackedForegroundResolvePipeline();
		return false;
	}
	if(errors){ errors->Release(); errors = nil; }
	result = D3DCompile(shaderSource, strlen(shaderSource),
		"tracked_foreground", nil, nil, "PSMain", "ps_5_0", 0, 0,
		&pixelShader, &errors);
	if(FAILED(result)){
		if(errors)
			VrLog("Tracked foreground pixel shader: %s\n",
				(const char*)errors->GetBufferPointer());
		if(errors) errors->Release();
		vertexShader->Release();
		if(pixelShader) pixelShader->Release();
		DestroyTrackedForegroundResolvePipeline();
		return false;
	}
	if(errors) errors->Release();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = gTrackedForegroundResolveRootSignature;
	desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	desc.VS.BytecodeLength = vertexShader->GetBufferSize();
	desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	desc.PS.BytecodeLength = pixelShader->GetBufferSize();
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	// librw's world blend writes premultiplied RGB into the transparent scratch
	// target (SRC_ALPHA/INV_SRC_ALPHA for RGB). Composite that premultiplied value
	// exactly once; multiplying by source alpha again darkens lasers and edges.
	D3D12_RENDER_TARGET_BLEND_DESC &blend =
		desc.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_ONE;
	blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	result = device->CreateGraphicsPipelineState(&desc,
		IID_PPV_ARGS(&gTrackedForegroundResolvePipeline));
	vertexShader->Release();
	pixelShader->Release();
	if(FAILED(result)){
		DestroyTrackedForegroundResolvePipeline();
		return false;
	}
	return true;
}

bool AreCopyCompatibleFormats(DXGI_FORMAT source, DXGI_FORMAT target)
{
	if(source == target)
		return true;
	return (source == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	        source == DXGI_FORMAT_R8G8B8A8_UNORM ||
	        source == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
	       (target == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
	        target == DXGI_FORMAT_R8G8B8A8_UNORM ||
	        target == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
}

struct TrackedForegroundResolveEye
{
	rw::Raster *source;
	rw::Raster *parent;
	D3D12_GPU_DESCRIPTOR_HANDLE sourceView;
	ID3D12Resource *targetResource;
	D3D12_CPU_DESCRIPTOR_HANDLE targetView;
	uint32 targetViewIndex;
	Swapchain *target;
};

void ReleaseTrackedForegroundResolveEye(TrackedForegroundResolveEye *prepared)
{
	if(!prepared || prepared->targetViewIndex == UINT32_MAX)
		return;
	rw::d3d12::deferDescriptorRelease(UINT32_MAX,
		prepared->targetViewIndex, UINT32_MAX);
	prepared->targetViewIndex = UINT32_MAX;
}

bool PrepareTrackedForegroundResolveEye(int eyeIndex,
	TrackedForegroundResolveEye *prepared)
{
	if(!prepared)
		return false;
	*prepared = {};
	prepared->targetViewIndex = UINT32_MAX;
	if(eyeIndex < 0 || eyeIndex >= EYE_COUNT ||
	   !gEye[eyeIndex].color ||
	   !gTrackedForegroundResolveAvailable ||
	   !gTrackedForegroundResolvePipeline ||
	   !gTrackedForegroundResolveRootSignature)
		return false;
	prepared->source = (rw::Raster*)gEye[eyeIndex].color;
	prepared->parent = prepared->source->parent ?
		prepared->source->parent : prepared->source;
	if(!prepared->parent || prepared->parent->width <= 0 ||
	   prepared->parent->height <= 0 || prepared->source->width <= 0 ||
	   prepared->source->height <= 0)
		return false;
	if(!rw::d3d12::getTextureView(prepared->source,
	   &prepared->sourceView, nil) || prepared->sourceView.ptr == 0)
		return false;
	ID3D12Device *device = rw::d3d12::getDevice();
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	ID3D12DescriptorHeap *heap = rw::d3d12::getShaderResourceHeap();
	prepared->target = &gEye[eyeIndex].swapchain;
	// The mandatory world resolve already acquired this image. Never acquire a
	// second time here: composite directly while XR ownership is still active.
	if(!device || !list || !heap || !prepared->target->acquired ||
	   prepared->target->width <= 0 || prepared->target->height <= 0)
		return false;
	prepared->targetResource = AcquiredTexture(*prepared->target);
	if(!prepared->targetResource ||
	   !rw::d3d12::allocateRenderTargetDescriptor(&prepared->targetView,
	   &prepared->targetViewIndex)){
		return false;
	}
	D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
	viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(prepared->targetResource, &viewDesc,
		prepared->targetView);
	return true;
}

void ResolveTrackedForegroundEye(int eyeIndex,
	TrackedForegroundResolveEye *prepared)
{
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	ID3D12DescriptorHeap *heap = rw::d3d12::getShaderResourceHeap();
	rw::Raster *source = prepared->source;
	rw::Raster *parent = prepared->parent;
	Swapchain &target = *prepared->target;
	list->OMSetRenderTargets(1, &prepared->targetView, FALSE, nil);
	D3D12_VIEWPORT viewport = {
		0.0f, 0.0f, (float)target.width, (float)target.height, 0.0f, 1.0f
	};
	D3D12_RECT scissor = { 0, 0, (LONG)target.width, (LONG)target.height };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	rw::d3d12::resetWorldDrawState();
	list->SetDescriptorHeaps(1, &heap);
	list->SetGraphicsRootSignature(gTrackedForegroundResolveRootSignature);
	list->SetPipelineState(gTrackedForegroundResolvePipeline);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const XrFovf &fov = gRenderFov[eyeIndex];
	const float left = -tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float up = tanf(fov.angleUp);
	const float down = -tanf(fov.angleDown);
	float eyeScaleX = (left+right)/(2.0f*gSourceTanX);
	float eyeScaleY = (up+down)/(2.0f*gSourceTanY);
	float eyeOffsetX = (gSourceTanX-left)/(2.0f*gSourceTanX);
	float eyeOffsetY = (gSourceTanY-up)/(2.0f*gSourceTanY);
	ApplyTrackedScopeZoomToUv(&eyeScaleX, &eyeScaleY,
		&eyeOffsetX, &eyeOffsetY);
	const float regionScaleX = (float)source->width/(float)parent->width;
	const float regionScaleY = (float)source->height/(float)parent->height;
	const float regionOffsetX = (float)source->offsetX/(float)parent->width;
	const float regionOffsetY = (float)source->offsetY/(float)parent->height;
	struct Constants {
		float uvScale[2];
		float uvOffset[2];
		float uvMin[2];
		float uvMax[2];
	} constants = {};
	constants.uvScale[0] = eyeScaleX*regionScaleX;
	constants.uvScale[1] = eyeScaleY*regionScaleY;
	constants.uvOffset[0] = regionOffsetX+eyeOffsetX*regionScaleX;
	constants.uvOffset[1] = regionOffsetY+eyeOffsetY*regionScaleY;
	constants.uvMin[0] = regionOffsetX+0.5f/(float)parent->width;
	constants.uvMin[1] = regionOffsetY+0.5f/(float)parent->height;
	constants.uvMax[0] = regionOffsetX+regionScaleX-
		0.5f/(float)parent->width;
	constants.uvMax[1] = regionOffsetY+regionScaleY-
		0.5f/(float)parent->height;
	list->SetGraphicsRoot32BitConstants(0, 8, &constants, 0);
	list->SetGraphicsRootDescriptorTable(1, prepared->sourceView);
	list->DrawInstanced(3, 1, 0, 0);
	ReleaseTrackedForegroundResolveEye(prepared);
}

bool CopyRasterToSwapchain(RwRaster *source, int sourceWidth, int sourceHeight,
	Swapchain &target, uint32)
{
	if(!source || !AcquireSwapchain(target))
		return false;
	rw::Raster *raster = ((rw::Raster*)source)->parent;
	ID3D12Resource *sourceResource = nil;
	ID3D12Resource *targetResource = AcquiredTexture(target);
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	if(!raster || !targetResource || !list ||
	   !rw::d3d12::getRasterResource(raster, &sourceResource) || !sourceResource){
		VrLog("CopyRaster unavailable raster=%p target=%p list=%p source=%p\n",
			(void*)raster, (void*)targetResource, (void*)list, (void*)sourceResource);
		return false;
	}
	D3D12_RESOURCE_DESC sourceDesc = sourceResource->GetDesc();
	D3D12_RESOURCE_DESC targetDesc = targetResource->GetDesc();
	if((int)sourceDesc.Width != sourceWidth || (int)sourceDesc.Height != sourceHeight ||
	   sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height ||
	   !AreCopyCompatibleFormats(sourceDesc.Format, targetDesc.Format) ||
	   sourceDesc.SampleDesc.Count != targetDesc.SampleDesc.Count){
		VrLog("CopyRaster mismatch source=%llux%u fmt=%u samples=%u target=%llux%u fmt=%u samples=%u\n",
			(unsigned long long)sourceDesc.Width, sourceDesc.Height, (unsigned)sourceDesc.Format,
			sourceDesc.SampleDesc.Count, (unsigned long long)targetDesc.Width, targetDesc.Height,
			(unsigned)targetDesc.Format, targetDesc.SampleDesc.Count);
		return false;
	}
	if(!rw::d3d12::transitionRaster(raster, D3D12_RESOURCE_STATE_COPY_SOURCE))
		return false;
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = targetResource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	list->ResourceBarrier(1, &barrier);
	list->CopyResource(targetResource, sourceResource);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	list->ResourceBarrier(1, &barrier);
	rw::d3d12::transitionRaster(raster, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if(gVrLoggedRenderableFrames < 10)
		VrLog("CopyRaster ok %dx%d targetIndex=%u\n", sourceWidth, sourceHeight,
			target.acquiredIndex);
	return true;
}

bool DrawEyeFxaa(EyeBuffer &eye, int eyeIndex)
{
	if(eyeIndex < 0 || eyeIndex >= EYE_COUNT ||
	   !AcquireSwapchain(eye.swapchain))
		return false;
	const XrFovf &fov = gRenderFov[eyeIndex];
	const float left = -tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float up = tanf(fov.angleUp);
	const float down = -tanf(fov.angleDown);
	float uvScaleX = (left + right) / (2.0f * gSourceTanX);
	float uvScaleY = (up + down) / (2.0f * gSourceTanY);
	float uvOffsetX = (gSourceTanX - left) / (2.0f * gSourceTanX);
	// D3D textures use a top-left origin, unlike the GL source used below.
	float uvOffsetY = (gSourceTanY - up) / (2.0f * gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX, &uvScaleY, &uvOffsetX, &uvOffsetY);
	uint32 colorMode = 0;
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
	blurColor[0] = red*CPostFX::Intensity/255.0f;
	blurColor[1] = green*CPostFX::Intensity/255.0f;
	blurColor[2] = blue*CPostFX::Intensity/255.0f;
	contrastMult[0] = (red-64.0f)/256.0f+1.4f;
	contrastMult[1] = (green-64.0f)/256.0f+1.4f;
	contrastMult[2] = (blue-64.0f)/256.0f+1.4f;
	contrastAdd[0] = red/1536.0f-0.05f;
	contrastAdd[1] = green/1536.0f-0.05f;
	contrastAdd[2] = blue/1536.0f-0.05f;
#endif
	rw::Raster *source = (rw::Raster*)eye.color;
	rw::d3d12::beginGpuTimestamp(rw::d3d12::GPU_STAGE_RESOLVE);
	const bool resolved = rw::d3d12::resolveRasterToExternal(
		source, AcquiredTexture(eye.swapchain),
		eye.swapchain.width, eye.swapchain.height,
		uvScaleX, uvScaleY, uvOffsetX, uvOffsetY,
		gAntiAliasingEnabled, colorMode, blurColor,
		contrastMult, contrastAdd) != 0;
	rw::d3d12::endGpuTimestamp(rw::d3d12::GPU_STAGE_RESOLVE);
	if(!resolved){
		VrLog("D3D12 eye resolve failed eye=%d scale=(%.5f,%.5f) offset=(%.5f,%.5f)\n",
			eyeIndex, uvScaleX, uvScaleY, uvOffsetX, uvOffsetY);
		ReleaseSwapchain(eye.swapchain);
	}
	return resolved;
}

bool DrawEyeDlaa(EyeBuffer &eye, int eyeIndex, RwCamera *camera)
{
	if(gTemporalAaBackend != TEMPORAL_AA_DLAA ||
	   !Dlaa::IsSupported() || !camera ||
	   eyeIndex < 0 || eyeIndex >= EYE_COUNT || !eye.color || !eye.depth)
		return false;
	rw::Raster *colorRaster = ((rw::Raster*)eye.color)->parent;
	rw::Raster *depthRaster = ((rw::Raster*)eye.depth)->parent;
	ID3D12Resource *colorResource = nil;
	ID3D12Resource *depthResource = nil;
	D3D12_GPU_DESCRIPTOR_HANDLE depthView = {};
	ID3D12Resource *objectMotionResource = nil;
	D3D12_GPU_DESCRIPTOR_HANDLE objectMotionView = {};
	if(!colorRaster || !depthRaster ||
	   !rw::d3d12::getRasterResource(colorRaster, &colorResource) || !colorResource ||
	   !rw::d3d12::transitionRaster(colorRaster, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
	   !rw::d3d12::getDepthTextureView(depthRaster, &depthResource, &depthView) ||
	   !depthResource || depthView.ptr == 0 ||
	   !rw::d3d12::getMotionTextureView(colorRaster, &objectMotionResource,
	                                  &objectMotionView) ||
	   !objectMotionResource || objectMotionView.ptr == 0 ||
	   !rw::d3d12::transitionDepthRaster(depthRaster, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
		return false;

	Dlaa::EyeInput input = {};
	input.color = colorResource;
	input.depth = depthResource;
	input.depthShaderResourceView = depthView.ptr;
	input.objectMotionShaderResourceView = objectMotionView.ptr;
	input.width = eye.renderWidth;
	input.height = eye.renderHeight;
	// The reconstruction target: the full render-scale image. Equal to the
	// render size in DLAA mode; larger in the DLSS upscaling modes, where the
	// scene rendered smaller and the network rebuilds the rest.
	input.outputWidth = (uint32)(eye.swapchain.width*gRenderScale+0.5f);
	input.outputHeight = (uint32)(eye.swapchain.height*gRenderScale+0.5f);
	input.sourceLeft = ((rw::Raster*)eye.color)->offsetX;
	if(!rw::d3d12::getStereoWorldCamera(eyeIndex, input.view, input.projection))
		return false;
	input.nearPlane = gFirstPersonEnabled ? 0.05f : gOriginalNearPlane;
	input.farPlane = RwCameraGetFarClipPlane(camera);
	rw::d3d12::beginGpuTimestamp(rw::d3d12::GPU_STAGE_TEMPORAL_AA);
	Dlaa::EyeOutput output = {};
	const bool dlaaOk = Dlaa::EvaluateEye(eyeIndex, input, &output);
	rw::d3d12::endGpuTimestamp(rw::d3d12::GPU_STAGE_TEMPORAL_AA);
	if(!dlaaOk || !output.color ||
	   output.shaderResourceView == 0 || !AcquireSwapchain(eye.swapchain))
		return false;

	const XrFovf &fov = gRenderFov[eyeIndex];
	const float left = -tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float up = tanf(fov.angleUp);
	const float down = -tanf(fov.angleDown);
	float uvScaleX = (left + right) / (2.0f * gSourceTanX);
	float uvScaleY = (up + down) / (2.0f * gSourceTanY);
	float uvOffsetX = (gSourceTanX - left) / (2.0f * gSourceTanX);
	float uvOffsetY = (gSourceTanY - up) / (2.0f * gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX, &uvScaleY, &uvOffsetX, &uvOffsetY);
	uint32 colorMode = 0;
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
	blurColor[0] = red*CPostFX::Intensity/255.0f;
	blurColor[1] = green*CPostFX::Intensity/255.0f;
	blurColor[2] = blue*CPostFX::Intensity/255.0f;
	contrastMult[0] = (red-64.0f)/256.0f+1.4f;
	contrastMult[1] = (green-64.0f)/256.0f+1.4f;
	contrastMult[2] = (blue-64.0f)/256.0f+1.4f;
	contrastAdd[0] = red/1536.0f-0.05f;
	contrastAdd[1] = green/1536.0f-0.05f;
	contrastAdd[2] = blue/1536.0f-0.05f;
#endif
	D3D12_GPU_DESCRIPTOR_HANDLE outputView = { output.shaderResourceView };
	rw::d3d12::beginGpuTimestamp(rw::d3d12::GPU_STAGE_RESOLVE);
	const bool resolved = rw::d3d12::resolveTextureToExternal(
		(ID3D12Resource*)output.color, outputView, AcquiredTexture(eye.swapchain),
		output.width, output.height, eye.swapchain.width, eye.swapchain.height,
		uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, false, colorMode, blurColor,
		contrastMult, contrastAdd) != 0;
	rw::d3d12::endGpuTimestamp(rw::d3d12::GPU_STAGE_RESOLVE);
	if(!resolved){
		VrLog("D3D12 DLAA eye resolve failed eye=%d\n", eyeIndex);
		ReleaseSwapchain(eye.swapchain);
	}
	return resolved;
}

bool EvaluateD3D12Fsr2Frame(RwCamera *camera,
	Fsr2::EyeOutput outputs[EYE_COUNT])
{
	if(gTemporalAaBackend != TEMPORAL_AA_FSR2 ||
	   !Fsr2::IsSupported() || !camera || !outputs)
		return false;
	Fsr2::EyeInput inputs[EYE_COUNT] = {};
	for(int eyeIndex = 0; eyeIndex < EYE_COUNT; eyeIndex++){
		EyeBuffer &eye = gEye[eyeIndex];
		if(!eye.color || !eye.depth)
			return false;
		rw::Raster *colorRaster = ((rw::Raster*)eye.color)->parent;
		rw::Raster *depthRaster = ((rw::Raster*)eye.depth)->parent;
		ID3D12Resource *colorResource = nil;
		ID3D12Resource *depthResource = nil;
		D3D12_GPU_DESCRIPTOR_HANDLE depthView = {};
		if(!colorRaster || !depthRaster ||
		   !rw::d3d12::getRasterResource(colorRaster, &colorResource) ||
		   !colorResource ||
		   !rw::d3d12::transitionRaster(colorRaster,
				D3D12_RESOURCE_STATE_COPY_SOURCE) ||
		   !rw::d3d12::getDepthTextureView(depthRaster,
				&depthResource, &depthView) ||
		   !depthResource || depthView.ptr == 0 ||
		   !rw::d3d12::transitionDepthRaster(depthRaster,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
			return false;
		Fsr2::EyeInput &input = inputs[eyeIndex];
		input.color = colorResource;
		input.depth = depthResource;
		input.depthShaderResourceView = depthView.ptr;
		input.width = eye.renderWidth;
		input.height = eye.renderHeight;
		input.sourceLeft = ((rw::Raster*)eye.color)->offsetX;
		if(!rw::d3d12::getStereoWorldCamera(eyeIndex, input.view,
		   input.projection))
			return false;
		input.nearPlane = gFirstPersonEnabled ?
			0.05f : gOriginalNearPlane;
		input.farPlane = RwCameraGetFarClipPlane(camera);
	}
	rw::d3d12::beginGpuTimestamp(rw::d3d12::GPU_STAGE_TEMPORAL_AA);
	const bool evaluated = Fsr2::EvaluateFrame(inputs, outputs);
	rw::d3d12::endGpuTimestamp(rw::d3d12::GPU_STAGE_TEMPORAL_AA);
	return evaluated;
}

bool DrawEyeFsr2Output(EyeBuffer &eye, int eyeIndex,
	const Fsr2::EyeOutput &output)
{
	if(eyeIndex < 0 || eyeIndex >= EYE_COUNT || !output.color ||
	   output.shaderResourceView == 0 || !AcquireSwapchain(eye.swapchain))
		return false;
	const XrFovf &fov = gRenderFov[eyeIndex];
	const float left = -tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float up = tanf(fov.angleUp);
	const float down = -tanf(fov.angleDown);
	float uvScaleX = (left + right) / (2.0f * gSourceTanX);
	float uvScaleY = (up + down) / (2.0f * gSourceTanY);
	float uvOffsetX = (gSourceTanX - left) / (2.0f * gSourceTanX);
	float uvOffsetY = (gSourceTanY - up) / (2.0f * gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX, &uvScaleY,
		&uvOffsetX, &uvOffsetY);
	uint32 colorMode = 0;
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
	blurColor[0] = red*CPostFX::Intensity/255.0f;
	blurColor[1] = green*CPostFX::Intensity/255.0f;
	blurColor[2] = blue*CPostFX::Intensity/255.0f;
	contrastMult[0] = (red-64.0f)/256.0f+1.4f;
	contrastMult[1] = (green-64.0f)/256.0f+1.4f;
	contrastMult[2] = (blue-64.0f)/256.0f+1.4f;
	contrastAdd[0] = red/1536.0f-0.05f;
	contrastAdd[1] = green/1536.0f-0.05f;
	contrastAdd[2] = blue/1536.0f-0.05f;
#endif
	D3D12_GPU_DESCRIPTOR_HANDLE outputView = {
		output.shaderResourceView
	};
	rw::d3d12::beginGpuTimestamp(rw::d3d12::GPU_STAGE_RESOLVE);
	const bool resolved = rw::d3d12::resolveTextureToExternal(
		(ID3D12Resource*)output.color, outputView,
		AcquiredTexture(eye.swapchain), output.width, output.height,
		eye.swapchain.width, eye.swapchain.height,
		uvScaleX, uvScaleY, uvOffsetX, uvOffsetY, false, colorMode,
		blurColor, contrastMult, contrastAdd) != 0;
	rw::d3d12::endGpuTimestamp(rw::d3d12::GPU_STAGE_RESOLVE);
	if(!resolved){
		VrLog("D3D12 FSR2 eye resolve failed eye=%d\n", eyeIndex);
		ReleaseSwapchain(eye.swapchain);
	}
	return resolved;
}

bool DrawTrackedForegroundEye(RwCamera *camera, int eyeIndex)
{
	if(!camera || eyeIndex < 0 || eyeIndex >= EYE_COUNT ||
	   !gTrackedForegroundRenderCallback ||
	   !gEye[eyeIndex].color || !gEye[eyeIndex].depth)
		return false;
	CMatrix eyeCamera;
	if(!GetEyeCamera(eyeIndex, &eyeCamera))
		return false;
	rw::d3d12::setStereoWorldEye(eyeIndex);
	RwV2d window = { gSourceTanX, gSourceTanY };
	RwV2d offset = { 0.0f, 0.0f };
	// The opaque world has already been reconstructed and copied to its XR
	// swapchain. Reuse its now-dead scene color as a zero-allocation foreground
	// scratch target; the matching depth sub-raster remains valid.
	RwCameraSetRaster(camera, gEye[eyeIndex].color);
	RwCameraSetZRaster(camera, gEye[eyeIndex].depth);
	RwCameraSetViewWindow(camera, &window);
	RwCameraSetViewOffset(camera, &offset);
	const float nearPlane = gFirstPersonEnabled ? 0.05f : gOriginalNearPlane;
	RwCameraSetNearClipPlane(camera, nearPlane);
	CDraw::SetNearClipZ(nearPlane);
	RsGlobal.width = gEye[eyeIndex].renderWidth;
	RsGlobal.height = gEye[eyeIndex].renderHeight;
	RwMatrix *matrix = RwFrameGetMatrix(RwCameraGetFrame(camera));
	*RwMatrixGetRight(matrix) = eyeCamera.GetRight();
	*RwMatrixGetUp(matrix) = eyeCamera.GetUp();
	*RwMatrixGetAt(matrix) = eyeCamera.GetForward();
	*RwMatrixGetPos(matrix) = eyeCamera.GetPosition();
	RwMatrixUpdate(matrix);
	RwFrameUpdateObjects(RwCameraGetFrame(camera));
	RwFrameOrthoNormalize(RwCameraGetFrame(camera));
	TheCamera.GetMatrix() = eyeCamera;
	CDraw::SetFOV(RADTODEG(2.0f*atanf(gSourceTanX)));
	TheCamera.CalculateDerivedValues();
	RwRGBA transparent = { 0, 0, 0, 0 };
	// Preserve the completed world depth. Only transparent color is reset.
	RwCameraClear(camera, &transparent, rwCAMERACLEARIMAGE);
	if(!RwCameraBeginUpdate(camera))
		return false;
	// Keep the late layer unjittered by leaving the foreground camera projection
	// untouched. Do not recapture it as the temporal world camera: this pass runs
	// after DLAA and would otherwise replace the next frame's world history with
	// the tracked-foreground near plane.
	TheCamera.m_viewMatrix.Update();
	rw::d3d12::setWorldRenderStage(rw::d3d12::WORLD_STAGE_FX_HANDS);
	gTrackedForegroundRenderCallback();
	rw::d3d12::setWorldRenderStage(rw::d3d12::WORLD_STAGE_OTHER);
	RwCameraEndUpdate(camera);
	return true;
}

void RestoreTrackedForegroundCameraBinding(RwCamera *camera)
{
	if(!camera)
		return;
	RwCameraSetRaster(camera, gOriginalColor);
	RwCameraSetZRaster(camera, gOriginalDepth);
	RwCameraSetViewWindow(camera, &gOriginalViewWindow);
	RwCameraSetViewOffset(camera, &gOriginalViewOffset);
	RwCameraSetNearClipPlane(camera, gOriginalNearPlane);
	CDraw::SetNearClipZ(gOriginalDrawNear);
	RsGlobal.width = gOriginalScreenWidth;
	RsGlobal.height = gOriginalScreenHeight;
	RwMatrix *matrix = RwFrameGetMatrix(RwCameraGetFrame(camera));
	*matrix = gOriginalFrameMatrix;
	RwMatrixUpdate(matrix);
	RwFrameUpdateObjects(RwCameraGetFrame(camera));
	RwFrameOrthoNormalize(RwCameraGetFrame(camera));
}

bool RenderTrackedForeground(RwCamera *camera)
{
	if(!gTrackedForegroundResolveAvailable ||
	   !gTrackedForegroundRenderCallback ||
	   !gTrackedForegroundVisibilityCallback ||
	   !gTrackedForegroundVisibilityCallback())
		return false;
	// Foreground is composited after the temporal resolve and has no motion
	// consumer. Reuse its color/depth without clearing or writing the motion MRT.
	rw::d3d12::setRasterMotionEnabled((rw::Raster*)gStereoColor, false);
	// main.cpp restored the gameplay camera transform before entering submit;
	// preserve that high-level state while this auxiliary pass changes it per eye.
	const CMatrix savedCamera = TheCamera.GetMatrix();
	const float savedFov = CDraw::GetFOV();
	bool rendered = true;
	for(int eye = 0; eye < EYE_COUNT; eye++){
		if(!DrawTrackedForegroundEye(camera, eye)){
			rendered = false;
			break;
		}
	}
	// Do not composite either eye until both foreground views exist. A draw-side
	// failure therefore leaves a coherent world-only stereo pair for this frame.
	if(rendered){
		TrackedForegroundResolveEye prepared[EYE_COUNT] = {};
		for(int eye = 0; eye < EYE_COUNT; eye++)
			prepared[eye].targetViewIndex = UINT32_MAX;
		bool ready = true;
		for(int eye = 0; eye < EYE_COUNT; eye++){
			if(!PrepareTrackedForegroundResolveEye(eye, &prepared[eye])){
				ready = false;
				break;
			}
		}
		if(!ready){
			for(int eye = 0; eye < EYE_COUNT; eye++)
				ReleaseTrackedForegroundResolveEye(&prepared[eye]);
			rendered = false;
		}
		// Every operation that can fail has now completed for both eyes. From
		// this point the command-list writes are issued as one stereo transaction,
		// so the current frame cannot contain foreground in only one eye.
		if(ready){
			for(int eye = 0; eye < EYE_COUNT; eye++)
				ResolveTrackedForegroundEye(eye, &prepared[eye]);
		}
	}
	TheCamera.GetMatrix() = savedCamera;
	CDraw::SetFOV(savedFov);
	TheCamera.CalculateDerivedValues();
	RestoreTrackedForegroundCameraBinding(camera);
	rw::d3d12::setStereoWorldEye(-1);
	// A structural failure switches the following frame back to the established
	// world pass. The already-resolved world remains submit-safe in this frame.
	if(!rendered)
		gTrackedForegroundResolveAvailable = false;
	return rendered;
}

bool FinishD3D12SwapchainWrites()
{
	// The command list must be submitted before releasing OpenXR images, but a
	// CPU-side fence wait is unnecessary. The runtime synchronizes its use of
	// the D3D12 queue, while librw fences allocator reuse independently.
	const double timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
	const bool completed = rw::d3d12::submitForExternal() != 0;
	if(gPerfFrameStarted)
		gPerfCurrent.d3d12ExternalSubmitMs +=
			(float)(PerfNowMs() - timingStart);
	if(!completed)
		VrLog("submitAndWaitForExternal failed\n");
	for(int eye = 0; eye < EYE_COUNT; eye++)
		ReleaseSwapchain(gEye[eye].swapchain);
	ReleaseSwapchain(gHudSwapchain);
	ReleaseSwapchain(gWristHudSwapchain);
	ReleaseSwapchain(gDebugSwapchain);
	ReleaseSwapchain(gVrMenuSwapchain);
	ReleaseSwapchain(gCinemaSwapchain);
	return completed;
}
#else
bool CopyRasterToSwapchain(RwRaster *source, int sourceWidth, int sourceHeight,
	Swapchain &target, GLenum filter)
{
	if(!AcquireSwapchain(target))
		return false;
	rw::Raster *raster = ((rw::Raster*)source)->parent;
	rw::gl3::Gl3Raster *native = PLUGINOFFSET(rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
	GLint oldDraw = 0, oldRead = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, native->fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		AcquiredTexture(target), 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if(complete)
		glBlitFramebuffer(0, 0, sourceWidth, sourceHeight, 0, 0, target.width, target.height,
			GL_COLOR_BUFFER_BIT, filter);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDraw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldRead);
	ReleaseSwapchain(target);
	return complete;
}

bool DrawEyeFxaa(EyeBuffer &eye, int eyeIndex)
{
	if(!gFxaaProgram || !gFxaaVertexArray || !AcquireSwapchain(eye.swapchain))
		return false;
	int colorMode=0;
	float blurColor[4]={0.0f,0.0f,0.0f,30.0f/255.0f};
	float contrastMult[3]={1.0f,1.0f,1.0f}, contrastAdd[3]={0.0f,0.0f,0.0f};
#ifdef EXTENDED_COLOURFILTER
	if(gLightingEnabled && TheCamera.m_BlurType!=MOTION_BLUR_NONE){
		if(CPostFX::EffectSwitch==CPostFX::POSTFX_NORMAL) colorMode=1;
		else if(CPostFX::EffectSwitch==CPostFX::POSTFX_MOBILE) colorMode=2;
	}
	const float red=(float)TheCamera.m_BlurRed, green=(float)TheCamera.m_BlurGreen, blue=(float)TheCamera.m_BlurBlue;
	blurColor[0]=red*CPostFX::Intensity/255.0f; blurColor[1]=green*CPostFX::Intensity/255.0f;
	blurColor[2]=blue*CPostFX::Intensity/255.0f;
	contrastMult[0]=(red-64.0f)/256.0f+1.4f; contrastMult[1]=(green-64.0f)/256.0f+1.4f;
	contrastMult[2]=(blue-64.0f)/256.0f+1.4f;
	contrastAdd[0]=red/1536.0f-0.05f; contrastAdd[1]=green/1536.0f-0.05f; contrastAdd[2]=blue/1536.0f-0.05f;
#endif
	rw::Raster *raster=((rw::Raster*)eye.color)->parent;
	rw::gl3::Gl3Raster *native=PLUGINOFFSET(rw::gl3::Gl3Raster,raster,rw::gl3::nativeRasterOffset);
	GLint oldDraw=0,oldRead=0,oldProgram=0,oldVao=0,oldActive=0,oldTexture=0,oldViewport[4]={};
	GLboolean oldMask[4]={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
	glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao);
	glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive); glGetIntegerv(GL_VIEWPORT,oldViewport); glGetBooleanv(GL_COLOR_WRITEMASK,oldMask);
	const GLboolean blend=glIsEnabled(GL_BLEND),depth=glIsEnabled(GL_DEPTH_TEST),cull=glIsEnabled(GL_CULL_FACE);
	const GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),srgb=glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,AcquiredTexture(eye.swapchain),0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw); glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);
		ReleaseSwapchain(eye.swapchain); return false;
	}
	glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture); glBindTexture(GL_TEXTURE_2D,native->texid);
	GLint oldMin=GL_NEAREST,oldMag=GL_NEAREST; glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&oldMin);
	glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,&oldMag);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,gAntiAliasingEnabled?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,gAntiAliasingEnabled?GL_LINEAR:GL_NEAREST);
	glViewport(0,0,eye.swapchain.width,eye.swapchain.height); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
	glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB);
	glUseProgram(gFxaaProgram); glBindVertexArray(gFxaaVertexArray);
	const XrFovf &fov=gRenderFov[eyeIndex];
	const float left=-tanf(fov.angleLeft), right=tanf(fov.angleRight);
	const float up=tanf(fov.angleUp), down=-tanf(fov.angleDown);
	float uvScaleX=(left+right)/(2.0f*gSourceTanX);
	float uvScaleY=(up+down)/(2.0f*gSourceTanY);
	float uvOffsetX=(gSourceTanX-left)/(2.0f*gSourceTanX);
	float uvOffsetY=(gSourceTanY-down)/(2.0f*gSourceTanY);
	ApplyTrackedScopeZoomToUv(&uvScaleX,&uvScaleY,&uvOffsetX,&uvOffsetY);
	glUniform1i(gFxaaTextureUniform,0);
	glUniform2f(gFxaaInverseSizeUniform,1.0f/eye.swapchain.width,1.0f/eye.swapchain.height);
	glUniform2f(gFxaaUvScaleUniform,uvScaleX,uvScaleY);
	glUniform2f(gFxaaUvOffsetUniform,uvOffsetX,uvOffsetY);
	glUniform1i(gFxaaEnabledUniform,gAntiAliasingEnabled?1:0); glUniform1i(gColorModeUniform,colorMode);
	glUniform4fv(gBlurColorUniform,1,blurColor); glUniform3fv(gContrastMultUniform,1,contrastMult); glUniform3fv(gContrastAddUniform,1,contrastAdd);
	glDrawArrays(GL_TRIANGLES,0,3);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,oldMin); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,oldMag);
	glBindTexture(GL_TEXTURE_2D,oldTexture); glActiveTexture(oldActive); glBindVertexArray(oldVao); glUseProgram(oldProgram);
	glViewport(oldViewport[0],oldViewport[1],oldViewport[2],oldViewport[3]); glColorMask(oldMask[0],oldMask[1],oldMask[2],oldMask[3]);
	if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if(srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw); glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);
	ReleaseSwapchain(eye.swapchain);
	return true;
}
#endif

#include "OpenXRWristHud.h"

bool UpdateHudSwapchain(RwCamera *camera)
{
	gWristHud.routingMask = gWristHudLayerMask = 0;
	if((!gGameplayHudVisible && !IsTrackedScopeActive()) ||
	   !gHudColor || !gHudDepth)
		return false;
	RwRaster *oldColor = RwCameraGetRaster(camera);
	RwRaster *oldDepth = RwCameraGetZRaster(camera);
	const int oldWidth = RsGlobal.width, oldHeight = RsGlobal.height;
	RwCameraSetRaster(camera, gHudColor);
	RwCameraSetZRaster(camera, gHudDepth);
	RsGlobal.width = VR_HUD_WIDTH;
	RsGlobal.height = VR_HUD_HEIGHT;
	PrepareWristHud();
	RwRGBA transparent = { 0, 0, 0, 0 };
	RwCameraClear(camera, &transparent, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
	bool rendered = RwCameraBeginUpdate(camera) != nil;
	if(rendered){ RenderVrGameplayHud(); RwCameraEndUpdate(camera); }
	const bool copied = rendered && CopyRasterToSwapchain(gHudColor,
		VR_HUD_WIDTH, VR_HUD_HEIGHT, gHudSwapchain,
#ifdef RW_D3D12
		0
#else
		GL_NEAREST
#endif
	);
	if(copied) UpdateWristHudAtlas(camera);
	RwCameraSetRaster(camera, oldColor);
	RwCameraSetZRaster(camera, oldDepth);
	RsGlobal.width = oldWidth;
	RsGlobal.height = oldHeight;
	return copied;
}

int16 AxisValue(float value) { return (int16)(clamp(value, -1.0f, 1.0f)*128.0f); }
int16 TriggerValue(float value) { return (int16)(clamp(value, 0.0f, 1.0f)*255.0f); }
void MergeAxis(int16 &destination, int16 value) { if(Abs(value)>Abs(destination)) destination=value; }
void MergeButton(int16 &destination, bool pressed) { if(pressed) destination=255; }

bool ReadVector(XrAction action, int hand, XrVector2f &value)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action; get.subactionPath = gActions.hands[hand];
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if(XR_FAILED(xrGetActionStateVector2f(gSession, &get, &state)) || !state.isActive) return false;
	value = state.currentState; return true;
}

float ReadFloat(XrAction action, int hand)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action; get.subactionPath = gActions.hands[hand];
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	return XR_SUCCEEDED(xrGetActionStateFloat(gSession, &get, &state)) && state.isActive ? state.currentState : 0.0f;
}

bool ReadBool(XrAction action, int hand = -1)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action; get.subactionPath = hand >= 0 ? gActions.hands[hand] : XR_NULL_PATH;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(gSession, &get, &state)) && state.isActive && state.currentState;
}

bool IsHandPoseActive(XrAction action, int hand)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action;
	get.subactionPath = gActions.hands[hand];
	XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
	return XR_SUCCEEDED(xrGetActionStatePose(gSession, &get, &state)) && state.isActive;
}

bool LocateHandAction(XrAction action, XrSpace space, int hand, XrPosef &pose,
	XrVector3f *linearVelocity = nil, XrVector3f *angularVelocity = nil)
{
	if(linearVelocity)
		*linearVelocity = { 0.0f, 0.0f, 0.0f };
	if(angularVelocity)
		*angularVelocity = { 0.0f, 0.0f, 0.0f };
	if(!space || !gGameplaySpace || !IsHandPoseActive(action, hand))
		return false;
	XrSpaceVelocity velocity = { XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if(linearVelocity || angularVelocity)
		location.next = &velocity;
	if(XR_FAILED(xrLocateSpace(space, gGameplaySpace,
		gFrameState.predictedDisplayTime, &location)))
		return false;
	const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT |
		XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if((location.locationFlags & required) != required)
		return false;
	pose = location.pose;
	if(linearVelocity &&
	   (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
		*linearVelocity = velocity.linearVelocity;
	if(angularVelocity &&
	   (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0)
		*angularVelocity = velocity.angularVelocity;
	return true;
}

void LocateTrackedHands()
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gTrackedHandPoseValid[hand] = LocateHandAction(gActions.gripPose,
			gActions.gripSpace[hand], hand, gTrackedHandPose[hand],
			&gTrackedHandLinearVelocity[hand],
			&gTrackedHandAngularVelocity[hand]);
		gTrackedHandAimPoseValid[hand] = LocateHandAction(gActions.aimPose,
			gActions.aimSpace[hand], hand, gTrackedHandAimPose[hand]);
	}
}

float Halton(uint32 index, uint32 base)
{
	float result = 0.0f;
	float fraction = 1.0f;
	while(index){
		fraction /= (float)base;
		result += fraction*(float)(index % base);
		index /= base;
	}
	return result;
}
}

void PerfBeginFrame()
{
	// Collection used to run only while recording to CSV, which left the
	// in-headset panel with nothing to show. It now also runs whenever that
	// panel is open; only the storing of samples stays tied to recording.
	const bool collecting = PerfCollecting();
#ifdef RW_D3D12
	rw::d3d12::setDetailedProfilingEnabled(collecting);
#endif
	if(!collecting || gPerfFrameStarted) return;
	if(gPerfRecording && gPerfRecordedSamples >= VR_PERF_MAX_SAMPLES) return;
	gPerfCurrent = {};
	gPerfVisibleBuildingEntityCount = 0;
	gPerfCurrent.slowStreamItemId = -1;
	gPerfCurrent.slowStreamItemType = -1;
	gPerfStreamItemStartMs = 0.0;
#ifdef RW_D3D12
	rw::d3d12::resetFrameSyncProfile();
	rw::d3d12::resetWorldRenderProfile();
#endif
	gPerfFrameStartMs = PerfNowMs();
	gPerfFrameStarted = true;
}
void PerfAbortFrame() { gPerfFrameStarted = false; gPerfStreamItemStartMs = 0.0; }
void PerfEndFrame(float x, float y, float z)
{
	if(!gPerfFrameStarted) return;
#ifdef RW_D3D12
	rw::d3d12::FrameSyncProfile syncProfile = {};
	rw::d3d12::getFrameSyncProfile(&syncProfile);
	gPerfCurrent.d3d12FrameFenceWaitMs = syncProfile.frameFenceWaitMs;
	gPerfCurrent.d3d12FullGpuWaitMs = syncProfile.fullGpuWaitMs;
	rw::d3d12::GpuFrameProfile gpuProfile = {};
	rw::d3d12::getGpuFrameProfile(&gpuProfile);
	gPerfCurrent.gpuProfileValid = gpuProfile.valid ? 1u : 0u;
	gPerfCurrent.gpuSceneMs = gpuProfile.stageMs[rw::d3d12::GPU_STAGE_SCENE];
	gPerfCurrent.gpuTemporalAaMs =
		gpuProfile.stageMs[rw::d3d12::GPU_STAGE_TEMPORAL_AA];
	gPerfCurrent.gpuResolveMs = gpuProfile.stageMs[rw::d3d12::GPU_STAGE_RESOLVE];
	gPerfCurrent.gpuTotalMs = gpuProfile.totalMs;
	rw::d3d12::WorldRenderProfile worldProfile = {};
	rw::d3d12::getWorldRenderProfile(&worldProfile);
	gPerfCurrent.geometryInstanceMs = worldProfile.geometryInstanceMs;
	gPerfCurrent.geometryBufferUploadMs = worldProfile.bufferUploadMs;
	gPerfCurrent.geometryBufferBytes = worldProfile.bufferBytes;
	gPerfCurrent.worldSubmittedIndices = worldProfile.submittedIndices;
	gPerfCurrent.geometryInstances = worldProfile.geometryInstances;
	gPerfCurrent.worldDrawCalls = worldProfile.drawCalls;
	gPerfCurrent.stereoBundleBuildMs = worldProfile.stereoBundleBuildMs;
	gPerfCurrent.stereoBundleWaitMs = worldProfile.stereoBundleWaitMs;
	gPerfCurrent.stereoBundleDrawCalls = worldProfile.stereoBundleDrawCalls;
	gPerfCurrent.stereoBundleFallbacks = worldProfile.stereoBundleFallbacks;
	gPerfCurrent.stereoSinglePassBegins = worldProfile.stereoSinglePassBegins;
	gPerfCurrent.stereoSinglePassDrawCalls = worldProfile.stereoSinglePassDrawCalls;
	gPerfCurrent.stereoSinglePassIndices = worldProfile.stereoSinglePassIndices;
	gPerfCurrent.stereoSinglePassFallbacks = worldProfile.stereoSinglePassFallbacks;
	gPerfCurrent.monoDrawCalls = worldProfile.monoDrawCalls;
	gPerfCurrent.replayDrawCalls = worldProfile.replayDrawCalls;
	for(int i = 0; i < 24; i++)
		gPerfCurrent.stageMonoDrawCalls[i] = worldProfile.stageMonoDrawCalls[i];
	gPerfCurrent.fixedFoveatedBegins = worldProfile.fixedFoveatedBegins;
	gPerfCurrent.fixedFoveatedFailures = worldProfile.fixedFoveatedFailures;
	rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
	rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
	gPerfCurrent.fixedFoveatedProfile = foveatedInfo.profile;
	gPerfCurrent.fixedFoveatedTileSize = foveatedInfo.tileSize;
#endif
	gPerfCurrent.particleActive = CParticle::GetTotalActiveCount();
	gPerfCurrent.heliDustActive = CParticle::GetActiveCount(PARTICLE_HELI_DUST);
	gPerfCurrent.frameMs = (float)(PerfNowMs()-gPerfFrameStartMs);
	gPerfCurrent.elapsedSeconds = (PerfNowMs()-gPerfRecordingStartMs)/1000.0;
	gPerfCurrent.playerX=x; gPerfCurrent.playerY=y; gPerfCurrent.playerZ=z;
	// Whatever the phases and the OpenXR calls did not account for. A capture
	// showed this reaching 14ms while every named phase looked healthy, so it is
	// measured explicitly rather than left to be worked out from a spreadsheet.
	float accounted = 0.0f;
	for(int i = 0; i < PERF_PHASE_COUNT; i++)
		accounted += gPerfCurrent.phaseMs[i];
	accounted += gPerfCurrent.xrWaitFrameMs + gPerfCurrent.xrBeginFrameMs +
		gPerfCurrent.xrAcquireMs + gPerfCurrent.xrSwapchainWaitMs +
		gPerfCurrent.xrReleaseMs + gPerfCurrent.xrEndFrameMs +
		gPerfCurrent.xrLocateViewsMs;
	gPerfCurrent.otherMs = gPerfCurrent.frameMs-accounted;
	if(gPerfCurrent.otherMs < 0.0f)
		gPerfCurrent.otherMs = 0.0f;

	PerfPublishDisplaySample(gPerfCurrent);
	if(gPerfRecording && gPerfRecordedSamples < VR_PERF_MAX_SAMPLES){
		gPerfSamples[gPerfRecordedSamples] = gPerfCurrent;
		if(gPerfLiveCsv){
			WritePerfCsvSample(gPerfLiveCsv, gPerfRecordedSamples, gPerfCurrent);
			if((gPerfRecordedSamples % 30) == 29)
				fflush(gPerfLiveCsv);
		}
		gPerfRecordedSamples++;
		if(gPerfRecordedSamples >= VR_PERF_MAX_SAMPLES){
			gPerfRecording = false;
			if(gPerfLiveCsv){
				fclose(gPerfLiveCsv);
				gPerfLiveCsv = nil;
			}
			DumpPerfRecording();
		}
	}
	gPerfFrameStarted = false;
	gPerfStreamItemStartMs = 0.0;
}
void PerfBeginPhase(ePerfPhase phase)
{
	if(gPerfFrameStarted && phase >= 0 && phase < PERF_PHASE_COUNT)
		gPerfPhaseStartMs[phase] = PerfNowMs();
}
void PerfEndPhase(ePerfPhase phase)
{
	if(gPerfFrameStarted && phase >= 0 && phase < PERF_PHASE_COUNT)
		gPerfCurrent.phaseMs[phase] += (float)(PerfNowMs()-gPerfPhaseStartMs[phase]);
}
void PerfSetStreamingStats(int requested, uint64 memory)
{
	if(gPerfFrameStarted){ gPerfCurrent.requestedModels=requested; gPerfCurrent.streamingMemory=memory; }
}
void PerfBeginStreamItem(int streamId, int streamType)
{
	if(!gPerfFrameStarted){ gPerfStreamItemStartMs = 0.0; return; }
#ifdef RW_D3D12
	rw::d3d12::resetTextureUploadProfile();
#endif
	gPerfStreamItemId = streamId;
	gPerfStreamItemType = streamType;
	gPerfStreamItemStartMs = PerfNowMs();
}
void PerfEndStreamItem()
{
	if(gPerfStreamItemStartMs <= 0.0 || !gPerfFrameStarted){
		gPerfStreamItemStartMs = 0.0;
		return;
	}
	const float elapsed = (float)(PerfNowMs() - gPerfStreamItemStartMs);
	// A single streamed item costing half the 90Hz frame budget guarantees a
	// dropped frame. Captures showed HD replacement textures reaching 15ms, so
	// name the offender instead of leaving an opaque stream id in the CSV.
	if(elapsed > 5.0f){
		const char *itemName = "?";
		if(gPerfStreamItemId >= 0 && gPerfStreamItemId < MODELINFOSIZE){
			CBaseModelInfo *slowModel = CModelInfo::GetModelInfo(gPerfStreamItemId);
			if(slowModel) itemName = slowModel->GetModelName();
		}else if(gPerfStreamItemId >= STREAM_OFFSET_TXD &&
		         gPerfStreamItemId < STREAM_OFFSET_TXD+TXDSTORESIZE)
			itemName = CTxdStore::GetTxdName(gPerfStreamItemId-STREAM_OFFSET_TXD);
		float txAlloc = 0.0f, txDesc = 0.0f, txFoot = 0.0f, txUpRes = 0.0f;
		float txCopy = 0.0f, txQueue = 0.0f; double txMb = 0.0; int txCount = 0;
#ifdef RW_D3D12
		// The frame-cumulative texture profile; at 5ms+ the slow item dominates
		// it, so the split tells which stage of texture creation is eating the
		// time: placed-heap allocation (a fresh 128MB heap page is milliseconds),
		// descriptor/footprint bookkeeping, upload-buffer creation, the CPU copy,
		// or command recording.
		rw::d3d12::TextureUploadProfile slowProfile = {};
		rw::d3d12::getTextureUploadProfile(&slowProfile);
		txAlloc = slowProfile.defaultResourceMs; txDesc = slowProfile.descriptorMs;
		txFoot = slowProfile.footprintMs; txUpRes = slowProfile.uploadResourceMs;
		txCopy = slowProfile.cpuCopyMs; txQueue = slowProfile.queueMs;
		txMb = slowProfile.uploadBytes/(1024.0*1024.0);
		txCount = (int)slowProfile.uploads;
#endif
		float aInfo = 0.0f, aScan = 0.0f, aHeap = 0.0f, aPlace = 0.0f;
#ifdef RW_D3D12
		aInfo = slowProfile.allocInfoMs; aScan = slowProfile.allocScanMs;
		aHeap = slowProfile.allocHeapMs; aPlace = slowProfile.allocPlaceMs;
#endif
		VrLog("Slow stream item: %s (%d) took %.1fms "
			"[alloc %.1f (info %.1f scan %.1f heap %.1f place %.1f) "
			"upres %.1f copy %.1f queue %.1f | %.2fMB x%d]\n",
			itemName ? itemName : "?", gPerfStreamItemId, elapsed,
			txAlloc, aInfo, aScan, aHeap, aPlace,
			txUpRes, txCopy, txQueue, txMb, txCount);
	}
	if(elapsed > gPerfCurrent.slowStreamItemMs){
		gPerfCurrent.slowStreamItemMs = elapsed;
		gPerfCurrent.slowStreamItemId = gPerfStreamItemId;
		gPerfCurrent.slowStreamItemType = gPerfStreamItemType;
#ifdef RW_D3D12
		rw::d3d12::TextureUploadProfile profile = {};
		rw::d3d12::getTextureUploadProfile(&profile);
		gPerfCurrent.textureDefaultResourceMs = profile.defaultResourceMs;
		gPerfCurrent.textureDescriptorMs = profile.descriptorMs;
		gPerfCurrent.textureFootprintMs = profile.footprintMs;
		gPerfCurrent.textureUploadResourceMs = profile.uploadResourceMs;
		gPerfCurrent.textureCpuCopyMs = profile.cpuCopyMs;
		gPerfCurrent.textureQueueMs = profile.queueMs;
		gPerfCurrent.textureUploadBytes = profile.uploadBytes;
		gPerfCurrent.textureUploadCount = profile.uploads;
#endif
	}
	gPerfStreamItemStartMs = 0.0;
}
void PerfCountVisibleEntity(ePerfVisibleType type, const CEntity *entity)
{
	if(!gPerfFrameStarted) return;
	if(type==PERF_VISIBLE_BUILDING){
		// Stereo fallback and the legacy two-eye renderer can submit the same
		// building twice. Keep this diagnostic as a unique post-cull entity count
		// so an OCCLUSION ON/OFF comparison is not distorted by render mode.
		if(entity){
			for(int i = 0; i < gPerfVisibleBuildingEntityCount; i++)
				if(gPerfVisibleBuildingEntities[i] == entity)
					return;
			if(gPerfVisibleBuildingEntityCount < NUMVISIBLEENTITIES)
				gPerfVisibleBuildingEntities[gPerfVisibleBuildingEntityCount++] = entity;
		}
		gPerfCurrent.visibleBuildings++;
	}
	else if(type==PERF_VISIBLE_OBJECT) gPerfCurrent.visibleObjects++;
	else if(type==PERF_VISIBLE_PED) gPerfCurrent.visiblePeds++;
	else if(type==PERF_VISIBLE_VEHICLE) gPerfCurrent.visibleVehicles++;
}
void PerfCountEntityRender() { if(gPerfFrameStarted) gPerfCurrent.entityRenderCalls++; }

static int WrapWeaponRotation(int degrees)
{
	while(degrees > 360) degrees -= 720;
	while(degrees < -360) degrees += 720;
	return degrees;
}

void ChangeVrMenuValue(int direction)
{
	if(gVrLocomotionMenuVisible && gVrControlsMenuVisible){
		const int source = gVrControlsMenuSelection-VR_CONTROLS_FIRST_SOURCE;
		if(source >= 0 && source < VrPadBindings::SOURCE_COUNT){
			int target = gVrPadBindings.target[source];
			do {
				target = (target+VrPadBindings::TARGET_COUNT+(direction < 0 ? -1 : 1))%VrPadBindings::TARGET_COUNT;
			} while(target == VrPadBindings::FireTarget(CPad::GetPad(0)->GetMode()));
			gVrPadBindings.Set(source, target);
			SaveVrSetting(VrPadBindings::Key(source), gVrPadBindings.target[source]);
		}else if(gVrControlsMenuSelection == VR_CONTROLS_LAYOUT ||
		         gVrControlsMenuSelection == VR_CONTROLS_RESET){
			gVrPadBindings.Apply(gVrControlsMenuSelection == VR_CONTROLS_LAYOUT &&
				gVrPadBindings.GetLayout() == VrPadBindings::DEFAULT ? VrPadBindings::SWAPPED_HANDS : VrPadBindings::DEFAULT);
			for(int input = 0; input < VrPadBindings::SOURCE_COUNT; input++)
				SaveVrSetting(VrPadBindings::Key(input), gVrPadBindings.target[input]);
		}else if(gVrControlsMenuSelection == VR_CONTROLS_LOOK_BEHIND){
			gStickLookBehind = !gStickLookBehind;
			SaveVrSetting("StickLookBehind", gStickLookBehind);
		}else if(gVrControlsMenuSelection == VR_CONTROLS_CROUCH){
			gStickCrouch = !gStickCrouch;
			SaveVrSetting("StickCrouch", gStickCrouch);
		}else if(gVrControlsMenuSelection == VR_CONTROLS_BACK){
			gVrControlsMenuVisible = false;
			gVrLocomotionMenuSelection = VR_LOCOMOTION_CONTROLS;
		}
		return;
	}
	if(gVrDlssTuningMenuVisible){
		switch(gVrDlssTuningMenuSelection){
		case VR_DLSS_TUNING_INTENSITY:
			Dlaa::SetNeuralRenderingIntensityPercent(
				Dlaa::GetNeuralRenderingIntensityPercent()+direction*10);
			SaveDlssNrTuningSettings();
			break;
		case VR_DLSS_TUNING_STRUCTURE:
			Dlaa::SetNeuralRenderingStructurePercent(
				Dlaa::GetNeuralRenderingStructurePercent()+direction*10);
			SaveDlssNrTuningSettings();
			break;
		case VR_DLSS_TUNING_LOCAL_TONE:
			Dlaa::SetNeuralRenderingLocalTonePercent(
				Dlaa::GetNeuralRenderingLocalTonePercent()+direction*10);
			SaveDlssNrTuningSettings();
			break;
		case VR_DLSS_TUNING_GLOBAL_TONE:
			Dlaa::SetNeuralRenderingGlobalTonePercent(
				Dlaa::GetNeuralRenderingGlobalTonePercent()+direction*10);
			SaveDlssNrTuningSettings();
			break;
		case VR_DLSS_TUNING_STYLE:
			Dlaa::SetNeuralRenderingStyle((Dlaa::GetNeuralRenderingStyle()+3+
				(direction < 0 ? -1 : 1))%3);
			SaveDlssNrTuningSettings();
			break;
		case VR_DLSS_TUNING_AUTO_MASK:
			Dlaa::SetNeuralRenderingAutoMask(
				!Dlaa::IsNeuralRenderingAutoMaskEnabled());
			SaveDlssNrTuningSettings();
			break;
		case VR_DLSS_TUNING_RESET:
			Dlaa::ResetNeuralRenderingTuning();
			SaveDlssNrTuningSettings();
			ShowDlssProfileToast();
			break;
		case VR_DLSS_TUNING_BACK:
			gVrDlssTuningMenuVisible = false;
			gVrGraphicsMenuVisible = true;
			break;
		}
		return;
	}
	if(gVrRainMenuVisible){
		switch(gVrRainMenuSelection){
		case VR_RAIN_MODE:
			CWeather::RainGraphicsMode =
				(CWeather::RainGraphicsMode+4+(direction < 0 ? -1 : 1))%4;
			SaveVrSetting("RainGraphicsMode", CWeather::RainGraphicsMode);
			break;
		case VR_RAIN_INTENSITY:
			CWeather::RainGraphicsIntensity = Min(Max(
				CWeather::RainGraphicsIntensity+direction*10, 50), 200);
			SaveVrSetting("RainGraphicsIntensity",
				CWeather::RainGraphicsIntensity);
			break;
		case VR_RAIN_DENSITY:
			CWeather::RainGraphicsDensity = Min(Max(
				CWeather::RainGraphicsDensity+direction*10, 25), 300);
			SaveVrSetting("RainGraphicsDensity", CWeather::RainGraphicsDensity);
			break;
		case VR_RAIN_SURFACES:
			CWeather::RainSurfaceMode =
				(CWeather::RainSurfaceMode+4+(direction < 0 ? -1 : 1))%4;
			SaveVrSetting("RainSurfaceMode", CWeather::RainSurfaceMode);
			break;
		case VR_RAIN_PUDDLE_COVERAGE:
			gPuddleCoveragePercent = Min(Max(
				gPuddleCoveragePercent+direction*5, 25), 200);
			SaveVrSetting("PuddleCoverage", gPuddleCoveragePercent);
			break;
		case VR_RAIN_PUDDLE_EDGE:
			gPuddleEdgeSoftnessPercent = Min(Max(
				gPuddleEdgeSoftnessPercent+direction*5, 25), 200);
			SaveVrSetting("PuddleEdgeSoftness", gPuddleEdgeSoftnessPercent);
			break;
		case VR_RAIN_PUDDLE_RIPPLE:
			gPuddleRipplePercent = Min(Max(
				gPuddleRipplePercent+direction*5, 0), 200);
			SaveVrSetting("PuddleRipple", gPuddleRipplePercent);
			break;
		case VR_RAIN_RESET:
			CWeather::RainGraphicsMode = 3;
			CWeather::RainGraphicsIntensity = 200;
			CWeather::RainGraphicsDensity = 300;
			CWeather::RainSurfaceMode = 3;
			gPuddleCoveragePercent = 25;
			gPuddleEdgeSoftnessPercent = 25;
			gPuddleRipplePercent = 100;
			SaveVrSetting("RainGraphicsMode", CWeather::RainGraphicsMode);
			SaveVrSetting("RainGraphicsIntensity", CWeather::RainGraphicsIntensity);
			SaveVrSetting("RainGraphicsDensity", CWeather::RainGraphicsDensity);
			SaveVrSetting("RainSurfaceMode", CWeather::RainSurfaceMode);
			SaveVrSetting("PuddleCoverage", gPuddleCoveragePercent);
			SaveVrSetting("PuddleEdgeSoftness", gPuddleEdgeSoftnessPercent);
			SaveVrSetting("PuddleRipple", gPuddleRipplePercent);
			break;
		case VR_RAIN_BACK:
			gVrRainMenuVisible = false;
			gVrEffectsMenuVisible = true;
			break;
		}
		return;
	}
	if(gVrReflectionMenuVisible){
		if(gVrReflectionPage == 1){
			gVrReflectionMenuSelection = Min(Max(gVrReflectionMenuSelection, 0), VR_CAR_REFLECTION_BACK);
			if(gVrReflectionMenuSelection == VR_CAR_REFLECTION_BACK){
				gVrReflectionPage = 0; gVrReflectionMenuSelection = VR_REFLECTION_CARS;
			}else{
				int *values[] = { &gCarReflectionPercent, &gCarReflectionSsrPercent, &gCarReflectionSsrDistance };
				const char *keys[] = { "CarReflectionIntensity", "CarReflectionSsr", "CarReflectionSsrDistance" };
				const int item = gVrReflectionMenuSelection;
				*values[item] = Min(Max(*values[item]+direction*(item == VR_CAR_REFLECTION_DISTANCE ? 10 : 5), 0), item == VR_CAR_REFLECTION_DISTANCE ? 500 : 200);
				SaveVrSetting(keys[item], *values[item]);
				ApplyGraphicsEffectsSettings();
			}
			return;
		}
		if(gVrReflectionPage == 2){
			gVrReflectionMenuSelection = Min(Max(gVrReflectionMenuSelection, 0), VR_WATER_BACK);
			if(gVrReflectionMenuSelection == VR_WATER_BACK){
				gVrReflectionPage = 0; gVrReflectionMenuSelection = VR_REFLECTION_OCEAN;
			}else if(gVrReflectionMenuSelection == VR_WATER_ENABLE){
				gModernWaterEnabled = !gModernWaterEnabled;
				SaveVrSetting("ModernWater", gModernWaterEnabled ? 1 : 0);
				ApplyGraphicsEffectsSettings();
			}else{
				int *values[] = { &gWaterReflectionPercent, &gOceanWavePercent, &gWaterSpeedPercent, &gWaterDistortionPercent, &gWaterSheenPercent, &gWaterGlintPercent, &gWaterSparksPercent };
				const char *keys[] = { "WaterReflection", "OceanWaveIntensity", "WaterSpeed", "WaterDistortion", "WaterSheen", "WaterGlint", "WaterSparks" };
				const int item = gVrReflectionMenuSelection-VR_WATER_REFLECTION;
				*values[item] = Min(Max(*values[item]+direction*5, 0), 200);
				SaveVrSetting(keys[item], *values[item]);
				ApplyGraphicsEffectsSettings();
			}
			return;
		}
		switch(gVrReflectionMenuSelection){
		case VR_REFLECTION_CARS:
			gVrReflectionPage = 1; gVrReflectionMenuSelection = 0;
			break;
		case VR_REFLECTION_OCEAN:
			gVrReflectionPage = 2; gVrReflectionMenuSelection = 0;
			break;
		case VR_REFLECTION_PUDDLES:
			gPuddleReflectionPercent = Min(Max(
				gPuddleReflectionPercent+direction*10, 0), 200);
			SaveVrSetting("PuddleReflection", gPuddleReflectionPercent);
			break;
		case VR_REFLECTION_RESET:
			gModernWaterEnabled = true;
			SaveVrSetting("ModernWater", 1);
			gWaterSpeedPercent = gWaterDistortionPercent = gCarReflectionSsrPercent = 100;
			gWaterSheenPercent = gWaterGlintPercent = gWaterSparksPercent = gCarReflectionSsrDistance = 0;
			SaveVrSetting("WaterSpeed", gWaterSpeedPercent);
			SaveVrSetting("WaterDistortion", gWaterDistortionPercent);
			SaveVrSetting("WaterSheen", gWaterSheenPercent);
			SaveVrSetting("WaterGlint", gWaterGlintPercent);
			SaveVrSetting("WaterSparks", gWaterSparksPercent);
			SaveVrSetting("CarReflectionSsr", gCarReflectionSsrPercent);
			SaveVrSetting("CarReflectionSsrDistance", gCarReflectionSsrDistance);
			gCarReflectionPercent = 100;
			gWaterReflectionPercent = 80;
			gOceanWavePercent = 100;
			gPuddleReflectionPercent = 75;
			SaveVrSetting("CarReflectionIntensity", gCarReflectionPercent);
			SaveVrSetting("WaterReflection", gWaterReflectionPercent);
			SaveVrSetting("OceanWaveIntensity", gOceanWavePercent);
			SaveVrSetting("PuddleReflection", gPuddleReflectionPercent);
			ApplyGraphicsEffectsSettings();
			break;
		case VR_REFLECTION_BACK:
			gVrReflectionMenuVisible = false;
			gVrEffectsMenuVisible = true;
			break;
		}
		return;
	}
	if(gVrLightingMenuVisible){
		switch(gVrLightingMenuSelection){
		case VR_LIGHTING_MODE:
#ifdef RW_D3D12
			gDynamicLights = (gDynamicLights+3+direction)%3;
			SaveVrSetting("DynamicLights", gDynamicLights);
#endif
			break;
		case VR_LIGHTING_INTENSITY:
#ifdef RW_D3D12
			gDynamicLightIntensityPercent = Min(Max(
				gDynamicLightIntensityPercent+direction*5, 25), 200);
			SaveVrSetting("DynamicLightIntensity",
				gDynamicLightIntensityPercent);
#endif
			break;
		case VR_LIGHTING_GLOW:
#ifdef RW_D3D12
			gDynamicLightGlowPercent = Min(Max(
				gDynamicLightGlowPercent+direction*5, 25), 200);
			SaveVrSetting("DynamicLightGlowIntensity",
				gDynamicLightGlowPercent);
#endif
			break;
		case VR_LIGHTING_MAX_LIGHTS:
#ifdef RW_D3D12
			gDynamicLightMax = Min(Max(gDynamicLightMax+direction*2, 2), 8);
			SaveVrSetting("DynamicLightMax", gDynamicLightMax);
#endif
			break;
		case VR_LIGHTING_BACK:
			gVrLightingMenuVisible = false;
			gVrEffectsMenuVisible = true;
			break;
		case VR_LIGHTING_SHADOWS:
			CShadows::SetRenderEnabled(!CShadows::IsRenderEnabled());
			SaveVrSetting("ShadowsEnabled", CShadows::IsRenderEnabled() ? 1 : 0);
			break;
		case VR_LIGHTING_DISTANCE_FOG:
			gDistanceFogEnabled = !gDistanceFogEnabled;
			SaveVrSetting("DistanceFog", gDistanceFogEnabled ? 1 : 0);
#ifdef RW_D3D12
			rw::d3d12::setDistanceFogEnabled(gDistanceFogEnabled);
#endif
			break;
		}
		return;
	}
	if(gVrEffectsMenuVisible){
		switch(gVrEffectsMenuSelection){
		case VR_EFFECTS_MASTER:
			gEffectsEnabled = !gEffectsEnabled;
			SaveVrSetting("EffectsEnabled", gEffectsEnabled ? 1 : 0);
			ApplyGraphicsEffectsSettings();
			break;
		case VR_EFFECTS_LIGHTING_MENU:
#ifdef RW_D3D12
			gVrEffectsMenuVisible = false;
			gVrLightingMenuVisible = true;
			gVrLightingMenuSelection = 0;
#endif
			break;
		case VR_EFFECTS_RAIN_MENU:
#ifdef RW_D3D12
			gVrEffectsMenuVisible = false;
			gVrRainMenuVisible = true;
			gVrRainMenuSelection = 0;
#endif
			break;
		case VR_EFFECTS_REFLECTION_MENU:
#ifdef RW_D3D12
			gVrEffectsMenuVisible = false;
			gVrReflectionMenuVisible = true;
			gVrReflectionMenuSelection = 0;
			gVrReflectionPage = 0;
#endif
			break;
		case VR_EFFECTS_BACK:
			gVrEffectsMenuVisible = false;
			gVrGraphicsMenuVisible = true;
			break;
		case VR_EFFECTS_FOUNTAINS:
			CParticleObject::SetVrFountainQuality((CParticleObject::GetVrFountainQuality()+VR_FOUNTAIN_QUALITY_COUNT+direction)%VR_FOUNTAIN_QUALITY_COUNT);
			SaveVrSetting("FountainQuality", CParticleObject::GetVrFountainQuality());
			break;
		}
		return;
	}
	if(gVrBikeCalibrationMenuVisible){
		CVehicle *vehicle = FindPlayerVehicle();
		if(!vehicle || !IsImmersiveDrivingActiveInternal()){
			gVrBikeCalibrationMenuVisible = false;
			return;
		}
		const bool bike = vehicle->IsBike();
		const int model = vehicle->GetModelIndex();
		const int item = GetVehicleCalibrationMenuItemForRow(
			gVrBikeCalibrationMenuSelection);
		if(item == VR_BIKE_CAL_HAND){
			gBikeCalibrationEditHand = 1-gBikeCalibrationEditHand;
			return;
		}
		BikeHandleCalibration *calibration = bike ?
			GetBikeHandleCalibration(model, gBikeCalibrationEditHand) : nil;
		BikeLeanCalibration *leanCalibration = bike ?
			GetBikeLeanCalibration(model) : nil;
		const int categoryIndex = GetVrVehicleCategory(vehicle);
		VehicleCategoryCalibration *categoryCalibration =
			GetVehicleCategoryCalibration(vehicle);
		VehicleViewCalibration *viewCalibration =
			GetVehicleViewCalibration(model);
		if(bike && !calibration)
			return;
		switch(item){
		case VR_BIKE_CAL_OFFSET_X:
			if(!bike || !calibration)
				break;
			calibration->offsetX =
				Min(Max(calibration->offsetX+direction, -300), 300);
			SaveBikeHandleCalibrationValue(model,
				gBikeCalibrationEditHand, "OffsetX",
				calibration->offsetX);
			break;
		case VR_BIKE_CAL_OFFSET_Y:
			if(!bike || !calibration)
				break;
			calibration->offsetY =
				Min(Max(calibration->offsetY+direction, -300), 300);
			SaveBikeHandleCalibrationValue(model,
				gBikeCalibrationEditHand, "OffsetY",
				calibration->offsetY);
			break;
		case VR_BIKE_CAL_OFFSET_Z:
			if(!bike || !calibration)
				break;
			calibration->offsetZ =
				Min(Max(calibration->offsetZ+direction, -300), 300);
			SaveBikeHandleCalibrationValue(model,
				gBikeCalibrationEditHand, "OffsetZ",
				calibration->offsetZ);
			break;
		case VR_BIKE_CAL_ROT_X:
			if(!bike || !calibration)
				break;
			calibration->rotationX = Min(Max(
				calibration->rotationX+direction, -720), 720);
			SaveBikeHandleCalibrationValue(model,
				gBikeCalibrationEditHand, "RotationX",
				calibration->rotationX);
			break;
		case VR_BIKE_CAL_ROT_Y:
			if(!bike || !calibration)
				break;
			calibration->rotationY = Min(Max(
				calibration->rotationY+direction, -720), 720);
			SaveBikeHandleCalibrationValue(model,
				gBikeCalibrationEditHand, "RotationY",
				calibration->rotationY);
			break;
		case VR_BIKE_CAL_ROT_Z:
			if(!bike || !calibration)
				break;
			calibration->rotationZ = Min(Max(
				calibration->rotationZ+direction, -720), 720);
			SaveBikeHandleCalibrationValue(model,
				gBikeCalibrationEditHand, "RotationZ",
				calibration->rotationZ);
			break;
		case VR_BIKE_CAL_GLOBAL_CENTER_X:
			if(categoryCalibration){
				categoryCalibration->wheelCenterXCm = Min(Max(
					categoryCalibration->wheelCenterXCm+direction,
					-100), 100);
				SaveVehicleCategoryCalibrationValue(categoryIndex,
					"WheelCenterXCm", categoryCalibration->wheelCenterXCm);
			}
			break;
		case VR_BIKE_CAL_GLOBAL_CENTER_Y:
			if(categoryCalibration){
				categoryCalibration->wheelCenterYCm = Min(Max(
					categoryCalibration->wheelCenterYCm+direction,
					-100), 100);
				SaveVehicleCategoryCalibrationValue(categoryIndex,
					"WheelCenterYCm", categoryCalibration->wheelCenterYCm);
			}
			break;
		case VR_BIKE_CAL_GLOBAL_CENTER_Z:
			if(categoryCalibration){
				categoryCalibration->wheelCenterZCm = Min(Max(
					categoryCalibration->wheelCenterZCm+direction,
					-100), 100);
				SaveVehicleCategoryCalibrationValue(categoryIndex,
					"WheelCenterZCm", categoryCalibration->wheelCenterZCm);
			}
			break;
		case VR_BIKE_CAL_GLOBAL_RADIUS:
			if(categoryCalibration){
				if(bike){
					categoryCalibration->wheelRadiusCm = Min(Max(
						categoryCalibration->wheelRadiusCm+direction,
						-20), 40);
					SaveVehicleCategoryCalibrationValue(categoryIndex,
						"WheelRadiusCm",
						categoryCalibration->wheelRadiusCm);
				}else{
					categoryCalibration->carWheelRadiusCm = Min(Max(
						categoryCalibration->carWheelRadiusCm+direction,
						VR_CAR_WHEEL_MIN_RADIUS_CM),
						VR_CAR_WHEEL_MAX_RADIUS_CM);
					SaveVehicleCategoryCalibrationValue(categoryIndex,
						"WheelRadiusV2Cm",
						categoryCalibration->carWheelRadiusCm);
				}
			}
			break;
		case VR_CAR_CAL_GLOBAL_PITCH:
			if(!bike && categoryCalibration){
				categoryCalibration->carWheelPitchHalfDeg = Min(Max(
					categoryCalibration->carWheelPitchHalfDeg+direction,
					-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
					VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
				SaveVehicleCategoryCalibrationValue(categoryIndex,
					"WheelPitchHalfDeg",
					categoryCalibration->carWheelPitchHalfDeg);
			}
			break;
		case VR_CAR_CAL_GLOBAL_YAW:
			if(!bike && categoryCalibration){
				categoryCalibration->carWheelYawHalfDeg = Min(Max(
					categoryCalibration->carWheelYawHalfDeg+direction,
					-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
					VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
				SaveVehicleCategoryCalibrationValue(categoryIndex,
					"WheelYawHalfDeg",
					categoryCalibration->carWheelYawHalfDeg);
			}
			break;
		case VR_CAR_CAL_GLOBAL_ROLL:
			if(!bike && categoryCalibration){
				categoryCalibration->carWheelRollHalfDeg = Min(Max(
					categoryCalibration->carWheelRollHalfDeg+direction,
					-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
					VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
				SaveVehicleCategoryCalibrationValue(categoryIndex,
					"WheelRollHalfDeg",
					categoryCalibration->carWheelRollHalfDeg);
			}
			break;
		case VR_BIKE_CAL_MODEL_CENTER_X:
			if(viewCalibration){
				viewCalibration->wheelCenterXCm = Min(Max(
					viewCalibration->wheelCenterXCm+direction, -100), 100);
				SaveVehicleViewCalibrationValue(model, "WheelCenterXCm",
					viewCalibration->wheelCenterXCm);
			}
			break;
		case VR_BIKE_CAL_MODEL_CENTER_Y:
			if(viewCalibration){
				viewCalibration->wheelCenterYCm = Min(Max(
					viewCalibration->wheelCenterYCm+direction, -100), 100);
				SaveVehicleViewCalibrationValue(model, "WheelCenterYCm",
					viewCalibration->wheelCenterYCm);
			}
			break;
		case VR_BIKE_CAL_MODEL_CENTER_Z:
			if(viewCalibration){
				viewCalibration->wheelCenterZCm = Min(Max(
					viewCalibration->wheelCenterZCm+direction, -100), 100);
				SaveVehicleViewCalibrationValue(model, "WheelCenterZCm",
					viewCalibration->wheelCenterZCm);
			}
			break;
		case VR_BIKE_CAL_MODEL_RADIUS:
			if(viewCalibration){
				if(bike){
					viewCalibration->wheelRadiusCm = Min(Max(
						viewCalibration->wheelRadiusCm+direction, -20), 40);
					SaveVehicleViewCalibrationValue(model, "WheelRadiusCm",
						viewCalibration->wheelRadiusCm);
				}else{
					int radius = viewCalibration->carWheelRadiusCm;
					if(radius == 0){
						const int inherited = categoryCalibration ?
							categoryCalibration->carWheelRadiusCm :
							VR_CAR_WHEEL_DEFAULT_RADIUS_CM;
						radius = Min(Max(inherited+direction,
							VR_CAR_WHEEL_MIN_RADIUS_CM),
							VR_CAR_WHEEL_MAX_RADIUS_CM);
					}else if(direction < 0 &&
					   radius <= VR_CAR_WHEEL_MIN_RADIUS_CM)
						radius = 0;
					else if(direction > 0 &&
					   radius >= VR_CAR_WHEEL_MAX_RADIUS_CM)
						radius = 0;
					else
						radius = Min(Max(radius+direction,
							VR_CAR_WHEEL_MIN_RADIUS_CM),
							VR_CAR_WHEEL_MAX_RADIUS_CM);
					viewCalibration->carWheelRadiusCm = radius;
					SaveVehicleViewCalibrationValue(model,
						"WheelRadiusV2Cm", radius);
				}
			}
			break;
		case VR_CAR_CAL_MODEL_PITCH:
			if(!bike && viewCalibration){
				viewCalibration->carWheelPitchHalfDeg = Min(Max(
					viewCalibration->carWheelPitchHalfDeg+direction,
					-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
					VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
				SaveVehicleViewCalibrationValue(model,
					"WheelPitchHalfDeg",
					viewCalibration->carWheelPitchHalfDeg);
			}
			break;
		case VR_CAR_CAL_MODEL_YAW:
			if(!bike && viewCalibration){
				viewCalibration->carWheelYawHalfDeg = Min(Max(
					viewCalibration->carWheelYawHalfDeg+direction,
					-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
					VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
				SaveVehicleViewCalibrationValue(model,
					"WheelYawHalfDeg",
					viewCalibration->carWheelYawHalfDeg);
			}
			break;
		case VR_CAR_CAL_MODEL_ROLL:
			if(!bike && viewCalibration){
				viewCalibration->carWheelRollHalfDeg = Min(Max(
					viewCalibration->carWheelRollHalfDeg+direction,
					-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG),
					VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
				SaveVehicleViewCalibrationValue(model,
					"WheelRollHalfDeg",
					viewCalibration->carWheelRollHalfDeg);
			}
			break;
		case VR_BIKE_CAL_WHEELIE_HEIGHT:
			if(!leanCalibration)
				break;
			leanCalibration->wheelieHeightCm = Min(Max(
				leanCalibration->wheelieHeightCm+direction, 5), 100);
			SaveBikeLeanCalibrationValue(model, "WheelieHeightCm",
				leanCalibration->wheelieHeightCm);
			break;
		case VR_BIKE_CAL_STAND_HEIGHT:
			if(!leanCalibration)
				break;
			leanCalibration->standHeightCm = Min(Max(
				leanCalibration->standHeightCm+direction, 5), 100);
			SaveBikeLeanCalibrationValue(model, "StandHeightCm",
				leanCalibration->standHeightCm);
			break;
		case VR_BIKE_CAL_BACK:
			gVrBikeCalibrationMenuVisible = false;
			break;
		}
		if(!bike){
			// The isolated DFF wheel caches only its last steering angle. Force it
			// to rebuild from the untouched authored neutral frame so live centre or
			// orientation calibration immediately updates its pivot and spin axis.
			CVehicle *car = GetActivePlayerCar();
			if(car && car->IsCar())
				((CAutomobile*)car)->m_fVrSteeringWheelAppliedAngle = 1000.0f;
		}
		return;
	}
	if(gVrModelMenuVisible){
		if(gVrModelMenuSelection == VR_MODEL_BACK){
			gVrModelMenuVisible = false;
			return;
		}
		if(gVrModelMenuSelection == VR_MODEL_PRESET){
			ModelSets::CycleRequested(direction);
			const ModelSets::eModelSet preset = ModelSets::GetRequested();
			for(int category = 0;
			    category < ModelSets::MODEL_CATEGORY_COUNT; category++){
				if(ModelSets::IsCategoryAvailable(
				   (ModelSets::eModelCategory)category))
					ModelSets::SetRequestedForCategory(
						(ModelSets::eModelCategory)category,
						preset == ModelSets::MODEL_SET_MODERN &&
						category == ModelSets::MODEL_CATEGORY_VEGETATION ?
							ModelSets::MODEL_SET_CLASSIC : preset);
			}
			return;
		}
		const ModelSets::eModelCategory category =
			(ModelSets::eModelCategory)(gVrModelMenuSelection-
				VR_MODEL_WORLD);
		if(!ModelSets::IsCategoryAvailable(category))
			return;
		if(ModelSets::GetRequested() == ModelSets::MODEL_SET_CLASSIC){
			// Starting a custom mix from Classic is predictable: keep every
			// category Classic, then enable only the row the player selected.
			ModelSets::SetRequested(ModelSets::MODEL_SET_MODERN);
			for(int item = 0; item < ModelSets::MODEL_CATEGORY_COUNT; item++)
				ModelSets::SetRequestedForCategory(
					(ModelSets::eModelCategory)item,
					ModelSets::MODEL_SET_CLASSIC);
			ModelSets::CycleRequestedCategory(category, direction);
		}else{
			ModelSets::CycleRequestedCategory(category, direction);
		}
		return;
	}
	if(gVrGraphicsMenuVisible){
		switch(gVrGraphicsMenuSelection){
		case VR_GRAPHICS_RENDER_SCALE:
			gRenderScaleIndex = Min(Max(gRenderScaleIndex+direction, 0),
				MaxSupportedRenderScaleIndex());
			gRenderScaleChangePending = true;
			break;
		case VR_GRAPHICS_VRS:
#ifdef RW_D3D12
			gFixedFoveatedProfile = (gFixedFoveatedProfile+
				rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT+direction) %
				rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT;
			rw::d3d12::setFixedFoveatedRenderingProfile(
				gFixedFoveatedProfile);
			SaveVrSetting("VRS", gFixedFoveatedProfile);
#endif
			break;
		case VR_GRAPHICS_TEMPORAL_AA:
			{
			const int previousBackend = gTemporalAaBackend;
			gTemporalAaBackend = (gTemporalAaBackend+
				TEMPORAL_AA_BACKEND_COUNT+direction) %
				TEMPORAL_AA_BACKEND_COUNT;
			SaveVrSetting("TemporalAA", gTemporalAaBackend);
			// Keep the old key synchronized so older executables still start
			// with a safe temporal-AA choice if a user temporarily rolls back.
			SaveVrSetting("DLAA",
				gTemporalAaBackend == TEMPORAL_AA_DLAA ? 1 : 0);
			ResetTemporalAaHistory();
			QueueTemporalAaBackendRelease(previousBackend,
				gTemporalAaBackend);
			if(gTemporalAaBackend == TEMPORAL_AA_DLAA &&
			   !gDlaaStereoActivationReady){
				gDlaaStereoActivationFailed = false;
				gDlaaStereoWarmupFrames =
					DLAA_ACTIVATION_WARMUP_FRAMES;
			}
			if(gTemporalAaBackend == TEMPORAL_AA_FSR2)
				gFsr2StereoActivationFailed = false;
			// The effective render size depends on the backend when a DLSS
			// upscaling mode is selected; rebuild the targets to match.
			if(gDlssQualityMode != 0)
				gRenderScaleChangePending = true;
			}
			break;
		case VR_GRAPHICS_DLSS_MODE:
			gDlssQualityMode = (gDlssQualityMode+4+direction)%4;
			SaveVrSetting("DlssMode", gDlssQualityMode);
			SaveVrSetting("DLSSNeuralRegion", gDlssQualityMode);
			Dlaa::SetQualityMode(gDlssQualityMode);
			// The scene renders at the mode's reduced size; rebuild the
			// stereo targets through the same deferred path the render
			// scale option uses.
			gRenderScaleChangePending = true;
			ResetTemporalAaHistory();
			break;
		case VR_GRAPHICS_DLSS_NR_ENABLE:
			{
			const bool enable = !Dlaa::IsNeuralRenderingEnabled();
			Dlaa::SetNeuralRenderingEnabled(enable);
			if(enable){
				Dlaa::SetNeuralRenderingMode(gDlssComparisonProfile);
				Dlaa::SetNeuralRenderingOutputVisible(true);
			}
			SaveVrSetting("DLSSNeuralRendering", enable ? 1 : 0);
			SaveVrSetting("DLSSNeuralRenderingOutputVisible",
				enable ? 1 : 0);
			ResetTemporalAaHistory();
			ShowDlssProfileToast();
			}
			break;
		case VR_GRAPHICS_DLSS_NR:
			if(!Dlaa::IsNeuralRenderingEnabled())
				break;
			gDlssComparisonProfile = 1+
				(gDlssComparisonProfile-1+4+direction)%4;
			SaveVrSetting("DLSSNeuralRenderingCompareMode",
				gDlssComparisonProfile);
			Dlaa::SetNeuralRenderingMode(gDlssComparisonProfile);
			SaveDlssNrTuningSettings();
			SaveVrSetting("DLSSNeuralRenderingMode",
				gDlssComparisonProfile);
			ShowDlssProfileToast();
			break;
		case VR_GRAPHICS_DLSS_NR_PASSES:
			{
			const int passes = 1+
				(Dlaa::GetNeuralRenderingPassCount()-1+3+direction)%3;
			Dlaa::SetNeuralRenderingPassCount(passes);
			SaveVrSetting("DLSSNeuralPasses", passes);
			ResetTemporalAaHistory();
			ShowDlssProfileToast();
			}
			break;
		case VR_GRAPHICS_DLSS_NR_REGION:
			{
			const int region = (Dlaa::GetNeuralRenderingRegionMode()+4+direction)%4;
			gDlssQualityMode = region;
			Dlaa::SetNeuralRenderingRegionMode(region);
			SaveVrSetting("DlssMode", gDlssQualityMode);
			SaveVrSetting("DLSSNeuralRegion", region);
			gRenderScaleChangePending = true;
			ResetTemporalAaHistory();
			ShowDlssProfileToast();
			}
			break;
		case VR_GRAPHICS_DLSS_NR_TUNING:
			gVrDlssTuningMenuVisible = true;
			gVrGraphicsMenuVisible = false;
			gVrDlssTuningMenuSelection = 0;
			break;
		case VR_GRAPHICS_EFFECTS:
#ifdef RW_D3D12
			gVrEffectsMenuVisible = true;
			gVrLightingMenuVisible = false;
			gVrRainMenuVisible = false;
			gVrReflectionMenuVisible = false;
			gVrGraphicsMenuVisible = false;
			gVrEffectsMenuSelection = 0;
#endif
			break;
		case VR_GRAPHICS_JITTER:
			gTemporalJitterMode =
				(gTemporalJitterMode+6+direction)%6;
			SaveVrSetting("TemporalJitter", gTemporalJitterMode);
			ResetTemporalAaHistory();
			break;
		case VR_GRAPHICS_FXAA:
			gAntiAliasingEnabled = !gAntiAliasingEnabled;
			SaveVrSetting("AntiAliasing", gAntiAliasingEnabled);
			break;
		case VR_GRAPHICS_COLOR:
			gLightingEnabled = !gLightingEnabled;
			SaveVrSetting("ViceCityColor", gLightingEnabled);
			break;
		case VR_GRAPHICS_SUN_GLARE:
			gSunGlarePercent = Min(Max(gSunGlarePercent+direction*10,
				0), 100);
			SaveVrSetting("SunGlarePercent", gSunGlarePercent);
			break;
		case VR_GRAPHICS_OCCLUSION:
			CRenderer::SetVrOcclusionCulling(
				!CRenderer::IsVrOcclusionCullingEnabled());
			SaveVrSetting("OcclusionCulling",
				CRenderer::IsVrOcclusionCullingEnabled() ? 1 : 0);
			break;
		case VR_GRAPHICS_BACK:
			gVrGraphicsMenuVisible = false;
			break;
		case VR_GRAPHICS_MIPMAPS:
			gGenerateMipmapsRequested = !gGenerateMipmapsRequested;
			SaveVrSetting("GenerateMipmaps", gGenerateMipmapsRequested ? 1 : 0);
			break;
		case VR_GRAPHICS_FOLIAGE_SOFTNESS:
			gFoliageSoftness = (gFoliageSoftness+4+direction)%4;
			SaveVrSetting("FoliageSoftness", gFoliageSoftness);
#ifdef RW_D3D12
			rw::d3d12::setMaskedMipBias(gFoliageSoftness);
#endif
			break;
		}
		return;
	}
	if(gVrTrafficMenuVisible){
		switch(gVrTrafficMenuSelection){
		case VR_TRAFFIC_PEDESTRIANS:
			gTrafficPedPercent = Min(Max(
				gTrafficPedPercent+direction*5, 50), 300);
			SaveVrSetting("PedTrafficPercent", gTrafficPedPercent);
			ApplyTrafficSettings();
			break;
		case VR_TRAFFIC_VEHICLES:
			gTrafficCarPercent = Min(Max(
				gTrafficCarPercent+direction*5, 50), 300);
			SaveVrSetting("CarTrafficPercent", gTrafficCarPercent);
			ApplyTrafficSettings();
			break;
		case VR_TRAFFIC_DEFAULTS:
			VrRagdoll::SetEnabled(false);
			SaveVrSetting("Ragdolls", 0);
			gTrafficPedPercent = 135;
			gTrafficCarPercent = 135;
			SaveVrSetting("PedTrafficPercent", gTrafficPedPercent);
			SaveVrSetting("CarTrafficPercent", gTrafficCarPercent);
			ApplyTrafficSettings();
			break;
		case VR_TRAFFIC_RAGDOLLS:
			VrRagdoll::SetEnabled(!VrRagdoll::IsEnabled());
			SaveVrSetting("Ragdolls", VrRagdoll::IsEnabled());
			break;
		case VR_TRAFFIC_BACK:
			gVrTrafficMenuVisible = false;
			break;
		}
		return;
	}
	if(gVrHolsterMenuVisible){
		if(gVrHolsterMenuSelection < HOLSTER_POINT_COUNT)
			CycleHolsterPointSlot(gVrHolsterMenuSelection, direction);
		else
			gVrHolsterMenuVisible = false;
		return;
	}
	if(gVrCalibrationMenuVisible){
		SyncCurrentWeaponCalibration();
		SyncCurrentSupportGripCalibration();
		const bool canonicalOnlyRow =
			(gVrCalibrationMenuSelection >= VR_CAL_AIM_OFFSET_X &&
			 gVrCalibrationMenuSelection <= VR_CAL_AIM_ROT_Z) ||
			(gVrCalibrationMenuSelection >= VR_CAL_SUPPORT_GRIP_TYPE &&
			 gVrCalibrationMenuSelection <= VR_CAL_SUPPORT_ROT_Z);
		// LEFT aim and support are derived previews, never a second editable
		// calibration and never an indirect way to move the RIGHT profile.
		if(gCalibrationEditHand == 0 && canonicalOnlyRow)
			return;
		if(gWeaponAimAligned &&
		   gVrCalibrationMenuSelection >= VR_CAL_AIM_OFFSET_X &&
		   gVrCalibrationMenuSelection <= VR_CAL_AIM_ROT_Z)
			return;
		switch(gVrCalibrationMenuSelection){
		case VR_CAL_AIM_ALIGNED:
			SaveCurrentWeaponAimAligned(!gWeaponAimAligned);
			break;
		case VR_CAL_AIM_OFFSET_X:
			gWeaponAimOffsetXCm = Min(Max(gWeaponAimOffsetXCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("AimOffsetX", gWeaponAimOffsetXCm);
			break;
		case VR_CAL_AIM_OFFSET_Y:
			gWeaponAimOffsetYCm = Min(Max(gWeaponAimOffsetYCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("AimOffsetY", gWeaponAimOffsetYCm);
			break;
		case VR_CAL_AIM_OFFSET_Z:
			gWeaponAimOffsetZCm = Min(Max(gWeaponAimOffsetZCm+direction, -100), 100);
			SaveCurrentWeaponCalibrationValue("AimOffsetZ", gWeaponAimOffsetZCm);
			break;
		case VR_CAL_AIM_ROT_X:
			gWeaponAimRotationXDeg = WrapWeaponRotation(gWeaponAimRotationXDeg+direction);
			SaveCurrentWeaponCalibrationValue("AimRotationX", gWeaponAimRotationXDeg);
			break;
		case VR_CAL_AIM_ROT_Y:
			gWeaponAimRotationYDeg = WrapWeaponRotation(gWeaponAimRotationYDeg+direction);
			SaveCurrentWeaponCalibrationValue("AimRotationY", gWeaponAimRotationYDeg);
			break;
		case VR_CAL_AIM_ROT_Z:
			gWeaponAimRotationZDeg = WrapWeaponRotation(gWeaponAimRotationZDeg+direction);
			SaveCurrentWeaponCalibrationValue("AimRotationZ", gWeaponAimRotationZDeg);
			break;
		case VR_CAL_WEAPON_OFFSET_X:
			SaveWeaponModelCalibrationEdit("OffsetX", &gWeaponOffsetXCm,
				Min(Max(gWeaponOffsetXCm+direction, -100), 100));
			break;
		case VR_CAL_WEAPON_OFFSET_Y:
			SaveWeaponModelCalibrationEdit("OffsetY", &gWeaponOffsetYCm,
				Min(Max(gWeaponOffsetYCm+direction, -100), 100));
			break;
		case VR_CAL_WEAPON_OFFSET_Z:
			SaveWeaponModelCalibrationEdit("OffsetZ", &gWeaponOffsetZCm,
				Min(Max(gWeaponOffsetZCm+direction, -100), 100));
			break;
		case VR_CAL_WEAPON_ROT_X:
			SaveWeaponModelCalibrationEdit("RotationX", &gWeaponRotationXDeg,
				WrapWeaponRotation(gWeaponRotationXDeg+direction));
			break;
		case VR_CAL_WEAPON_ROT_Y:
			SaveWeaponModelCalibrationEdit("RotationY", &gWeaponRotationYDeg,
				WrapWeaponRotation(gWeaponRotationYDeg+direction));
			break;
		case VR_CAL_WEAPON_ROT_Z:
			SaveWeaponModelCalibrationEdit("RotationZ", &gWeaponRotationZDeg,
				WrapWeaponRotation(gWeaponRotationZDeg+direction));
			break;
		case VR_CAL_SUPPORT_GRIP_TYPE:
			gSupportGripType = (gSupportGripType+VR_SUPPORT_GRIP_TYPE_COUNT+
				(direction > 0 ? 1 : -1)) % VR_SUPPORT_GRIP_TYPE_COUNT;
			SaveCurrentSupportGripCalibrationValue("SupportGripStyle",
				gSupportGripType);
			break;
		case VR_CAL_SUPPORT_OFFSET_X:
			gSupportGripOffsetXCm = Min(Max(gSupportGripOffsetXCm+direction, -200), 200);
			SaveCurrentSupportGripCalibrationValue("SupportGripOffsetX",
				gSupportGripOffsetXCm);
			break;
		case VR_CAL_SUPPORT_OFFSET_Y:
			gSupportGripOffsetYCm = Min(Max(gSupportGripOffsetYCm+direction, -200), 200);
			SaveCurrentSupportGripCalibrationValue("SupportGripOffsetY",
				gSupportGripOffsetYCm);
			break;
		case VR_CAL_SUPPORT_OFFSET_Z:
			gSupportGripOffsetZCm = Min(Max(gSupportGripOffsetZCm+direction, -200), 200);
			SaveCurrentSupportGripCalibrationValue("SupportGripOffsetZ",
				gSupportGripOffsetZCm);
			break;
		case VR_CAL_SUPPORT_ROT_X:
			gSupportGripRotationXDeg = WrapWeaponRotation(
				gSupportGripRotationXDeg+direction);
			SaveCurrentSupportGripCalibrationValue("SupportGripRotationX",
				gSupportGripRotationXDeg);
			break;
		case VR_CAL_SUPPORT_ROT_Y:
			gSupportGripRotationYDeg = WrapWeaponRotation(
				gSupportGripRotationYDeg+direction);
			SaveCurrentSupportGripCalibrationValue("SupportGripRotationY",
				gSupportGripRotationYDeg);
			break;
		case VR_CAL_SUPPORT_ROT_Z:
			gSupportGripRotationZDeg = WrapWeaponRotation(
				gSupportGripRotationZDeg+direction);
			SaveCurrentSupportGripCalibrationValue("SupportGripRotationZ",
				gSupportGripRotationZDeg);
			break;
		case VR_CAL_BACK:
			gVrCalibrationMenuVisible = false;
			break;
		case VR_CAL_LASER:
			CycleWeaponLaserOverride(GetCalibrationWeaponType(), direction);
			break;
		}
		return;
	}
	if(gVrHudMenuVisible){
		const int panel = gWristHud.panel;
		char key[96];
		gWristHudFailed = false;
		if(gWristHud.editing){
			const int item = gVrHudMenuSelection;
			if(item >= VR_WRIST_ALONG && item <= VR_WRIST_SIZE)
				gWristHud.ChangePlacement(item-VR_WRIST_ALONG, direction);
			else if(item >= VR_WRIST_RED && item <= VR_WRIST_BLUE){
				const int channel = item-VR_WRIST_RED;
				static const char *const keys[] = {"AmmoColourRed", "AmmoColourGreen", "AmmoColourBlue"};
				gWristHud.ammoColour[channel] = Min(Max(gWristHud.ammoColour[channel]+direction, 0), 255);
				gWristHud.Save(keys[channel], gWristHud.ammoColour[channel]);
			}else if(item == VR_WRIST_CONTEXT)
				gWristHud.context = (gWristHud.context+3+direction)%3;
			else if(item == VR_WRIST_SIDE){
				gWristHud.underside[panel] = !gWristHud.underside[panel];
				sprintf(key, "%sUnderside", WristHudSettings::EnableKey(panel));
				gWristHud.Save(key, gWristHud.underside[panel]);
			}else if(item == VR_WRIST_HAND){
				gWristHud.hand[panel] ^= 1;
				sprintf(key, "%sHand", WristHudSettings::EnableKey(panel));
				gWristHud.Save(key, gWristHud.hand[panel]);
			}else if(item == VR_WRIST_COPY || item == VR_WRIST_RESET)
				gWristHud.ResetPlacement(item == VR_WRIST_COPY);
			else if(item == VR_WRIST_BACK){
				gWristHud.editing = false;
				gVrHudMenuSelection = VR_HUD_WRIST_PLACEMENT;
			}
			return;
		}
		switch(gVrHudMenuSelection){
		case VR_HUD_ENABLED:
			gGameplayHudVisible = !gGameplayHudVisible;
			SaveVrSetting("GameplayHud", gGameplayHudVisible);
			break;
		case VR_HUD_HORIZONTAL_SCALE:
			gHudWidthPercent = Min(Max(
				gHudWidthPercent+direction*5, 50), 200);
			SaveVrSetting("HudWidthPercent", gHudWidthPercent);
			break;
		case VR_HUD_SCALE:
			gHudScalePercent = Min(Max(
				gHudScalePercent+direction*5, 50), 200);
			SaveVrSetting("HudScalePercent", gHudScalePercent);
			break;
		case VR_HUD_OFFSET_X:
			gHudOffsetXCm = Min(Max(
				gHudOffsetXCm+direction, -100), 100);
			SaveVrSetting("HudOffsetXCm", gHudOffsetXCm);
			break;
		case VR_HUD_OFFSET_Y:
			gHudOffsetYCm = Min(Max(
				gHudOffsetYCm+direction, -100), 100);
			SaveVrSetting("HudOffsetYCm", gHudOffsetYCm);
			break;
		case VR_HUD_PRESET:
			gWristHud.SetPreset(strcmp(gWristHud.PresetName(), "IMMERSIVE") != 0);
			break;
		case VR_HUD_WRIST_PANEL:
			gWristHud.panel = (gWristHud.panel+WristHudSettings::PANEL_COUNT+direction)%WristHudSettings::PANEL_COUNT;
			break;
		case VR_HUD_WRIST_ENABLED:
			gWristHud.enabled[panel] = !gWristHud.enabled[panel];
			gWristHud.Save(WristHudSettings::EnableKey(panel), gWristHud.enabled[panel]);
			break;
		case VR_HUD_WRIST_HAND:
			gWristHud.hand[panel] ^= 1;
			sprintf(key, "%sHand", WristHudSettings::EnableKey(panel));
			gWristHud.Save(key, gWristHud.hand[panel]);
			break;
		case VR_HUD_WRIST_SIDE:
			gWristHud.underside[panel] = !gWristHud.underside[panel];
			sprintf(key, "%sUnderside", WristHudSettings::EnableKey(panel));
			gWristHud.Save(key, gWristHud.underside[panel]);
			break;
		case VR_HUD_WRIST_PLACEMENT:
			gWristHud.editing = true;
			gVrHudMenuSelection = 0;
			break;
		case VR_HUD_WRIST_GAZE:
			gWristHud.gaze = !gWristHud.gaze;
			gWristHud.Save("WristPanelGazeReveal", gWristHud.gaze);
			break;
		case VR_HUD_WRIST_RANGE:
			gWristHud.gazeRangeCm = Min(Max(gWristHud.gazeRangeCm+direction,20),200);
			gWristHud.Save("WristPanelGazeRangeCm", gWristHud.gazeRangeCm);
			break;
		case VR_HUD_WRIST_VEHICLE:
			gWristHud.inVehicle = !gWristHud.inVehicle;
			gWristHud.Save("WristPanelsInVehicle", gWristHud.inVehicle);
			break;
		case VR_HUD_CLASSIC_WEAPON:
			gWristHud.classicWeapon = !gWristHud.classicWeapon;
			gWristHud.Save("HudWeaponPanel", gWristHud.classicWeapon);
			break;
		case VR_HUD_CLASSIC_CLOCK:
			gWristHud.classicClock = !gWristHud.classicClock;
			gWristHud.Save("HudClock", gWristHud.classicClock);
			break;
		case VR_HUD_BACK:
			gVrHudMenuVisible = false;
			break;
		}
		return;
	}
	if(gVrVehicleMenuVisible){
		switch(gVrVehicleMenuSelection){
		case VR_VEHICLE_CAR_THIRD_PERSON:
		case VR_VEHICLE_BIKE_THIRD_PERSON:
		case VR_VEHICLE_BOAT_THIRD_PERSON:
		{
			const int category = gVrVehicleMenuSelection-VR_VEHICLE_CAR_THIRD_PERSON;
			gVehicleThirdPerson[category] = !gVehicleThirdPerson[category];
			char key[64];
			sprintf(key, "VehicleThirdPerson%s", gVehicleCategorySettingPrefixes[category]);
			SaveVrSetting(key, gVehicleThirdPerson[category]);
			ResetImmersiveDrivingInteraction();
			gRecenterRequested = true;
			break;
		}
		case VR_VEHICLE_DEFAULT_BODY:
			gDefaultDrivingBodyVisible = !gDefaultDrivingBodyVisible;
			SaveVrSetting("DefaultDrivingBodyVisible", gDefaultDrivingBodyVisible);
			break;
		case VR_VEHICLE_CAR_DRIVING_TYPE:
		case VR_VEHICLE_BOAT_DRIVING_TYPE:
		{
			const bool boat = gVrVehicleMenuSelection == VR_VEHICLE_BOAT_DRIVING_TYPE;
			int &drivingType = boat ? gBoatDrivingType : gCarDrivingType;
			drivingType =
				(drivingType+VR_DRIVING_TYPE_COUNT+direction) %
				VR_DRIVING_TYPE_COUNT;
			SaveVrSetting(boat ? "BoatDrivingType" : "CarDrivingType", drivingType);
			ResetImmersiveDrivingInteraction();
			if(GetDrivingTypeForVehicle(FindPlayerVehicle()) !=
			   VR_DRIVING_IMMERSIVE)
				gVrBikeCalibrationMenuVisible = false;
			break;
		}
		case VR_VEHICLE_BIKE_DRIVING_TYPE:
			gBikeDrivingType =
				(gBikeDrivingType+VR_DRIVING_TYPE_COUNT+direction) %
				VR_DRIVING_TYPE_COUNT;
			SaveVrSetting("BikeDrivingType", gBikeDrivingType);
			ResetImmersiveDrivingInteraction();
			if(GetDrivingTypeForVehicle(FindPlayerVehicle()) !=
			   VR_DRIVING_IMMERSIVE)
				gVrBikeCalibrationMenuVisible = false;
			break;
		case VR_VEHICLE_GLOBAL_SEAT_HEIGHT:
		{
			CVehicle *vehicle = FindPlayerVehicle();
			const int category = GetVrVehicleCategory(vehicle);
			VehicleCategoryCalibration *calibration =
				GetVehicleCategoryCalibration(vehicle);
			if(calibration){
				const bool defaultView = GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT && category != VR_VEHICLE_CATEGORY_HELI;
				int &value = defaultView ? gDefaultSeatHeightCm[category] : calibration->seatHeightCm;
				value = Min(Max(value+direction, -100), 150);
				if(defaultView){
					char key[64];
					sprintf(key, "Default%sSeatHeightCm", gVehicleCategorySettingPrefixes[category]);
					SaveVrSetting(key, value);
				}else
					SaveVehicleCategoryCalibrationValue(category, "SeatHeightCm", value);
			}
			break;
		}
		case VR_VEHICLE_GLOBAL_SEAT_FORWARD:
		{
			CVehicle *vehicle = FindPlayerVehicle();
			const int category = GetVrVehicleCategory(vehicle);
			VehicleCategoryCalibration *calibration =
				GetVehicleCategoryCalibration(vehicle);
			if(calibration){
				const bool defaultView = GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT && category != VR_VEHICLE_CATEGORY_HELI;
				int &value = defaultView ? gDefaultSeatDistanceCm[category] : calibration->seatDistanceCm;
				value = Min(Max(value+direction, -100), 100);
				if(defaultView){
					char key[64];
					sprintf(key, "Default%sSeatDistanceCm", gVehicleCategorySettingPrefixes[category]);
					SaveVrSetting(key, value);
				}else
					SaveVehicleCategoryCalibrationValue(category, "SeatDistanceCm", value);
			}
			break;
		}
		case VR_VEHICLE_MODEL_SEAT_HEIGHT:
		case VR_VEHICLE_MODEL_SEAT_FORWARD:
		{
			CVehicle *vehicle = FindPlayerVehicle();
			VehicleViewCalibration *calibration = vehicle ?
				GetVehicleViewCalibration(vehicle->GetModelIndex()) : nil;
			if(calibration){
				const bool defaultView = GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT &&
					GetVrVehicleCategory(vehicle) != VR_VEHICLE_CATEGORY_HELI;
				const bool height = gVrVehicleMenuSelection == VR_VEHICLE_MODEL_SEAT_HEIGHT;
				int &value = height ? (defaultView ? calibration->defaultSeatHeightCm : calibration->seatHeightCm) :
					(defaultView ? calibration->defaultSeatDistanceCm : calibration->seatDistanceCm);
				value = Min(Max(value+direction, -100), 100);
				SaveVehicleViewCalibrationValue(vehicle->GetModelIndex(), height ?
					(defaultView ? "DefaultSeatHeightCm" : "SeatHeightCm") :
					(defaultView ? "DefaultSeatDistanceCm" : "SeatDistanceCm"), value);
			}
			break;
		}
		case VR_VEHICLE_MOTION_HAND:
			gMotionSteeringHand = 1-gMotionSteeringHand;
			SaveVrSetting("MotionSteeringHand", gMotionSteeringHand);
			ResetMotionSteeringInteraction();
			break;
		case VR_VEHICLE_WHEEL_VISIBLE:
		case VR_VEHICLE_BOAT_WHEEL_VISIBLE:
		{
			const bool boat = gVrVehicleMenuSelection == VR_VEHICLE_BOAT_WHEEL_VISIBLE;
			bool &visible = boat ? gImmersiveBoatWheelVisible : gImmersiveCarWheelVisible;
			visible = !visible;
			SaveVrSetting(boat ? "ImmersiveBoatWheelVisible" : "ImmersiveCarWheelVisible", visible);
			break;
		}
		case VR_VEHICLE_MODEL_WHEEL_VISIBLE:
		{
			CVehicle *vehicle = FindPlayerVehicle();
			VehicleViewCalibration *calibration =
				vehicle && (vehicle->IsCar() || vehicle->IsBoat()) ?
				GetVehicleViewCalibration(vehicle->GetModelIndex()) : nil;
			if(calibration){
				calibration->carWheelVisibilityOverride =
					calibration->carWheelVisibilityOverride == 0 ? -1 : 0;
				SaveVehicleViewCalibrationValue(vehicle->GetModelIndex(),
					"VirtualWheelVisibility",
					calibration->carWheelVisibilityOverride);
			}
			break;
		}
		case VR_VEHICLE_HANDLE_CUBES:
		{
			const int category = GetVrVehicleCategory(FindPlayerVehicle());
			bool &visible = category == VR_VEHICLE_CATEGORY_BIKE ? gBikeHandleHighlightsEnabled :
				category == VR_VEHICLE_CATEGORY_BOAT ? gBoatHandleHighlightsEnabled : gCarHandleHighlightsEnabled;
			visible = !visible;
			SaveVrSetting(category == VR_VEHICLE_CATEGORY_BIKE ? "BikeHandleHighlights" :
				category == VR_VEHICLE_CATEGORY_BOAT ? "BoatHandleHighlights" : "CarHandleHighlights", visible);
			break;
		}
		case VR_VEHICLE_WHEEL_HAND_PULL_BACK:
			gWheelHandPullBackMm = Min(Max(gWheelHandPullBackMm+direction*5, -80), 80);
			SaveVrSetting("WheelHandPullBackMm", gWheelHandPullBackMm);
			break;
		case VR_VEHICLE_BIKE_LOCK_HORIZON:
			gBikeLockHorizonEnabled = !gBikeLockHorizonEnabled;
			SaveVrSetting("BikeLockHorizon",
				gBikeLockHorizonEnabled);
			break;
		case VR_VEHICLE_BIKE_THROTTLE:
			gBikeManualThrottle = !gBikeManualThrottle;
			SaveVrSetting("ImmersiveBikeManualThrottle", gBikeManualThrottle);
			break;
		case VR_VEHICLE_BIKE_VISUAL_LEAN:
			gBikeVisualLeanPercent = Min(Max(gBikeVisualLeanPercent+direction*5, 25), 100);
			SaveVrSetting("BikeVisualLeanPercent", gBikeVisualLeanPercent);
			break;
		case VR_VEHICLE_BIKE_VIEW_TILT:
			gBikeViewFollowingTilt = !gBikeViewFollowingTilt;
			SaveVrSetting("BikeViewFollowsTilt", gBikeViewFollowingTilt);
			break;
		case VR_VEHICLE_BIKE_THROW_RIDER:
			gBikeRiderCanBeThrown = !gBikeRiderCanBeThrown;
			SaveVrSetting("BikeRiderCanBeThrown", gBikeRiderCanBeThrown);
			break;
		case VR_VEHICLE_CALIBRATION:
			if(IsImmersiveDrivingActiveInternal()){
				gVrBikeCalibrationMenuVisible = true;
				gVrBikeCalibrationMenuSelection = 0;
				gBikeCalibrationEditHand = 0;
			}
			break;
		case VR_VEHICLE_BACK:
			gVrVehicleMenuVisible = false;
			break;
		}
		return;
	}
	if(gVrLocomotionMenuVisible){
		switch(gVrLocomotionMenuSelection){
		case VR_LOCOMOTION_MOVEMENT_MODE:
			// Keep the row visible so the menu layout and saved-setting format
			// remain stable, but do not expose the unfinished teleport mode in
			// release builds.
			gMovementMode = VR_MOVEMENT_SMOOTH;
			ResetTeleportInteraction();
			break;
		case VR_LOCOMOTION_MOVEMENT_ORIENTATION:
			gMovementOrientation =
				(gMovementOrientation+VR_MOVEMENT_ORIENTATION_COUNT+
				 direction) % VR_MOVEMENT_ORIENTATION_COUNT;
			SaveVrSetting("MovementOrientation", gMovementOrientation);
			break;
		case VR_LOCOMOTION_TURN_MODE:
			gTurnMode = (gTurnMode+VR_TURN_MODE_COUNT+direction) %
				VR_TURN_MODE_COUNT;
			SaveVrSetting("TurnMode", gTurnMode);
			gSnapTurnStickLatched = false;
			break;
		case VR_LOCOMOTION_TURN_SENSITIVITY:
			if(gMovementOrientation ==
			   VR_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL){
				gHeadSteeringSensitivityPercent = Min(Max(
					gHeadSteeringSensitivityPercent+direction*5,
					25), 300);
				SaveVrSetting("HeadSteeringSensitivityPercent",
					gHeadSteeringSensitivityPercent);
			}else{
				gTurnSensitivityPercent = Min(Max(
					gTurnSensitivityPercent+direction*5, 25), 300);
				SaveVrSetting("TurnSensitivityPercent",
					gTurnSensitivityPercent);
			}
			break;
		case VR_LOCOMOTION_SNAP_ANGLE:
			gSnapTurnAngleDegrees += direction*15;
			if(gSnapTurnAngleDegrees < 15)
				gSnapTurnAngleDegrees = 90;
			if(gSnapTurnAngleDegrees > 90)
				gSnapTurnAngleDegrees = 15;
			SaveVrSetting("SnapTurnAngleDegrees",
				gSnapTurnAngleDegrees);
			break;
		case VR_LOCOMOTION_HEAD_BOBBING:
			gHeadBobbingEnabled = !gHeadBobbingEnabled;
			SaveVrSetting("HeadBobbing", gHeadBobbingEnabled);
			break;
		case VR_LOCOMOTION_CUTSCENES:
			gCutsceneMode = 1-gCutsceneMode;
			SaveVrSetting("CutsceneMode", gCutsceneMode);
			gRecenterRequested = true;
			ResetTemporalAaHistory();
			break;
		case VR_LOCOMOTION_CONTROLS:
			gVrControlsMenuVisible = true;
			gVrControlsMenuSelection = 0;
			break;
		case VR_LOCOMOTION_BACK:
			gVrLocomotionMenuVisible = false;
			break;
		}
		return;
	}
	switch(gVrMenuSelection){
	case VR_MAIN_GRAPHICS:
		gVrGraphicsMenuVisible = true;
		gVrDlssTuningMenuVisible = false;
		gVrEffectsMenuVisible = false;
		gVrLightingMenuVisible = false;
		gVrRainMenuVisible = false;
		gVrReflectionMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrGraphicsMenuSelection = 0;
		break;
	case VR_MAIN_MODEL_SET:
		gVrModelMenuVisible = true;
		gVrGraphicsMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrModelMenuSelection = 0;
		break;
	case VR_MAIN_TRAFFIC_SETTINGS:
		gVrTrafficMenuVisible = true;
		gVrGraphicsMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrTrafficMenuSelection = 0;
		break;
	case VR_MAIN_HUD:
		gVrHudMenuVisible = true;
		gVrGraphicsMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrHudMenuSelection = 0;
		gWristHud.editing = false;
		break;
	case VR_MAIN_HANDS:
		gVrHandsEnabled = !gVrHandsEnabled;
		if(!gVrHandsEnabled)
			for(int hand = 0; hand < EYE_COUNT; hand++)
				ClearWeaponSupportForHand(hand);
		SaveVrSetting("VrHands", gVrHandsEnabled);
		break;
	case VR_MAIN_LASER:
		gWeaponLaserEnabled = !gWeaponLaserEnabled;
		SaveVrSetting("WeaponLaser", gWeaponLaserEnabled);
		break;
	case VR_MAIN_WEAPON_HAPTICS:
		gWeaponHapticsEnabled = !gWeaponHapticsEnabled;
		SaveVrSetting("WeaponHaptics", gWeaponHapticsEnabled);
		break;
	case VR_MAIN_WEAPON_HAPTICS_STRENGTH:
		gWeaponHapticsStrengthPercent = Min(Max(
			gWeaponHapticsStrengthPercent+direction*10, 10), 200);
		SaveVrSetting("WeaponHapticsStrengthPercent",
			gWeaponHapticsStrengthPercent);
		break;
	case VR_MAIN_HOLSTER_HIGHLIGHTS:
		gWeaponHolsterHighlightsEnabled = !gWeaponHolsterHighlightsEnabled;
		SaveVrSetting("HolsterHighlights", gWeaponHolsterHighlightsEnabled);
		break;
	case VR_MAIN_MANUAL_RELOAD:
		gManualReloadEnabled = !gManualReloadEnabled;
		if(!gManualReloadEnabled)
			for(int hand = 0; hand < EYE_COUNT; hand++)
				gManualReload[hand] = ManualReloadState();
		SaveVrSetting("ManualReloading", gManualReloadEnabled);
		break;
	case VR_MAIN_SCOPE_AIM:
		gPhysicalScopeAimEnabled = !gPhysicalScopeAimEnabled;
		SaveVrSetting("PhysicalScopeAim", gPhysicalScopeAimEnabled);
		ResetTrackedScopeState();
		break;
	case VR_MAIN_AIM_DIRECTION:
		gHeadAimEnabled = !gHeadAimEnabled;
		SaveVrSetting("HeadAim", gHeadAimEnabled);
		gTrackedScopeReticleTargetValid = false;
		for(int hand = 0; hand < EYE_COUNT; hand++)
			gTrackedAimCacheValid[hand] = false;
		break;
	case VR_MAIN_VEHICLE_SETTINGS:
		gVrVehicleMenuVisible = true;
		gVrGraphicsMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrVehicleMenuSelection = 0;
		break;
	case VR_MAIN_LOCOMOTION_SETTINGS:
		gVrLocomotionMenuVisible = true;
		gVrControlsMenuVisible = false;
		gVrGraphicsMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuSelection = 0;
		break;
	case VR_MAIN_CHEATS:
		gCheatMenuVisible = true;
		gCheatMenuOpenedFromVrMenu = true;
		gVrMissionMenuVisible = false;
		gVrMissionCategory = -1;
		gVrMenuVisible = false;
		gVrGraphicsMenuVisible = false;
		gVrDlssTuningMenuVisible = false;
		gVrEffectsMenuVisible = false;
		gVrLightingMenuVisible = false;
		gVrRainMenuVisible = false;
		gVrReflectionMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gCheatMenuSelection = 0;
		gVrMenuVerticalDown = gVrMenuHorizontalDown = false;
		gVrMenuNavigation.Reset();
		gCheatMenuHorizontalNavigation.Reset();
		gVrMenuDecreaseDown = gVrMenuIncreaseDown = false;
		gVrMenuDecreaseRepeatAt = gVrMenuIncreaseRepeatAt = 0;
		gVrMenuDecreaseHoldStartedAt = gVrMenuIncreaseHoldStartedAt = 0;
		break;
	case VR_MAIN_DEBUG:
		gDebugVisible = !gDebugVisible;
		break;
	case VR_MAIN_GRIP_LOCK:
		gWeaponGripLockEnabled = !gWeaponGripLockEnabled;
		SaveVrSetting("WeaponGripLock", gWeaponGripLockEnabled);
		break;
	case VR_MAIN_CALIBRATION:
		gVrCalibrationMenuVisible = true;
		gVrGraphicsMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrHolsterMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrCalibrationMenuSelection = 0;
		SyncCurrentWeaponCalibration();
		SyncCurrentSupportGripCalibration();
		break;
	case VR_MAIN_HOLSTERS:
		gVrHolsterMenuVisible = true;
		gVrGraphicsMenuVisible = false;
		gVrModelMenuVisible = false;
		gVrTrafficMenuVisible = false;
		gVrVehicleMenuVisible = false;
		gVrHudMenuVisible = false;
		gVrCalibrationMenuVisible = false;
		gVrBikeCalibrationMenuVisible = false;
		gVrLocomotionMenuVisible = false;
		gVrHolsterMenuSelection = 0;
		break;
	case VR_MAIN_ABOUT:
		gVrAboutVisible = true;
		// If this is still the first launch, opening ABOUT manually counts as
		// seeing the same welcome card. Preserve the flag so dismissal writes
		// WelcomeShown instead of showing it again after the next launch.
		gVrAboutFirstRunPending = false;
		gVrAboutDismissArmed = false;
		gVrAboutReleaseGate = false;
		gVrAboutWasRendered = false;
		break;
	}
}

bool IsVrMenuValueRepeatable()
{
	if(gVrLocomotionMenuVisible && gVrControlsMenuVisible)
		return false;
	if(gVrModelMenuVisible)
		return false;
	if(gVrDlssTuningMenuVisible)
		return gVrDlssTuningMenuSelection == VR_DLSS_TUNING_INTENSITY ||
			gVrDlssTuningMenuSelection == VR_DLSS_TUNING_STRUCTURE ||
			gVrDlssTuningMenuSelection == VR_DLSS_TUNING_LOCAL_TONE ||
			gVrDlssTuningMenuSelection == VR_DLSS_TUNING_GLOBAL_TONE;
	if(gVrRainMenuVisible)
		return gVrRainMenuSelection == VR_RAIN_INTENSITY ||
			gVrRainMenuSelection == VR_RAIN_DENSITY ||
			gVrRainMenuSelection == VR_RAIN_PUDDLE_COVERAGE ||
			gVrRainMenuSelection == VR_RAIN_PUDDLE_EDGE ||
			gVrRainMenuSelection == VR_RAIN_PUDDLE_RIPPLE;
	if(gVrReflectionMenuVisible)
		return gVrReflectionPage == 1 ? gVrReflectionMenuSelection < VR_CAR_REFLECTION_BACK :
			gVrReflectionPage == 2 ? (gVrReflectionMenuSelection > VR_WATER_ENABLE && gVrReflectionMenuSelection < VR_WATER_BACK) :
			gVrReflectionMenuSelection == VR_REFLECTION_PUDDLES;
	if(gVrEffectsMenuVisible)
		return false;
	if(gVrLightingMenuVisible){
#ifdef RW_D3D12
		return gVrLightingMenuSelection == VR_LIGHTING_INTENSITY ||
			gVrLightingMenuSelection == VR_LIGHTING_GLOW ||
			gVrLightingMenuSelection == VR_LIGHTING_MAX_LIGHTS;
#else
		return false;
#endif
	}
	if(gVrGraphicsMenuVisible){
		return
			gVrGraphicsMenuSelection == VR_GRAPHICS_RENDER_SCALE ||
			gVrGraphicsMenuSelection == VR_GRAPHICS_SUN_GLARE;
	}
	if(gVrTrafficMenuVisible)
		return gVrTrafficMenuSelection == VR_TRAFFIC_PEDESTRIANS ||
			gVrTrafficMenuSelection == VR_TRAFFIC_VEHICLES;
	if(gVrBikeCalibrationMenuVisible){
		const int item = GetVehicleCalibrationMenuItemForRow(
			gVrBikeCalibrationMenuSelection);
		if(IsImmersiveCarDrivingActiveInternal())
			return item != VR_BIKE_CAL_BACK;
		return item >= VR_BIKE_CAL_OFFSET_X &&
			item <= VR_BIKE_CAL_STAND_HEIGHT;
	}
	if(gVrCalibrationMenuVisible){
		if(gCalibrationEditHand == 0 &&
		   ((gVrCalibrationMenuSelection >= VR_CAL_AIM_OFFSET_X &&
		     gVrCalibrationMenuSelection <= VR_CAL_AIM_ROT_Z) ||
		    (gVrCalibrationMenuSelection >= VR_CAL_SUPPORT_GRIP_TYPE &&
		     gVrCalibrationMenuSelection <= VR_CAL_SUPPORT_ROT_Z)))
			return false;
		if(gWeaponAimAligned &&
		   gVrCalibrationMenuSelection >= VR_CAL_AIM_OFFSET_X &&
		   gVrCalibrationMenuSelection <= VR_CAL_AIM_ROT_Z)
			return false;
		return gVrCalibrationMenuSelection >= VR_CAL_AIM_OFFSET_X &&
			gVrCalibrationMenuSelection <= VR_CAL_SUPPORT_ROT_Z &&
			gVrCalibrationMenuSelection != VR_CAL_SUPPORT_GRIP_TYPE;
	}
	if(gVrHudMenuVisible){
		if(gWristHud.editing)
			return gVrHudMenuSelection >= VR_WRIST_ALONG && gVrHudMenuSelection <= VR_WRIST_BLUE;
		return (gVrHudMenuSelection >= VR_HUD_HORIZONTAL_SCALE &&
			gVrHudMenuSelection <= VR_HUD_OFFSET_Y) || gVrHudMenuSelection == VR_HUD_WRIST_RANGE;
	}
	if(gVrHolsterMenuVisible)
		return false;
	if(gVrVehicleMenuVisible)
		return (gVrVehicleMenuSelection >= VR_VEHICLE_GLOBAL_SEAT_HEIGHT &&
			 gVrVehicleMenuSelection <= VR_VEHICLE_MODEL_SEAT_FORWARD) ||
			gVrVehicleMenuSelection == VR_VEHICLE_WHEEL_HAND_PULL_BACK ||
			gVrVehicleMenuSelection == VR_VEHICLE_BIKE_VISUAL_LEAN;
	if(gVrLocomotionMenuVisible)
		return gVrLocomotionMenuSelection ==
			VR_LOCOMOTION_TURN_SENSITIVITY;
	return gVrMenuSelection == VR_MAIN_WEAPON_HAPTICS_STRENGTH;
}

bool ShouldApplyVrMenuValueInput(bool down, bool wasDown,
	ULONGLONG now, ULONGLONG *nextRepeatAt)
{
	if(!nextRepeatAt)
		return down && !wasDown;
	if(!down){
		*nextRepeatAt = 0;
		return false;
	}
	if(!wasDown){
		// A quick tap is always exactly one step. Holding begins repeating only
		// after a deliberate pause, which keeps fine calibration practical.
		*nextRepeatAt = now+420ULL;
		return true;
	}
	if(!IsVrMenuValueRepeatable() || *nextRepeatAt == 0 || now < *nextRepeatAt)
		return false;
	*nextRepeatAt = now+85ULL;
	return true;
}

int VrMenuValueRepeatMagnitude(bool down, bool wasDown, ULONGLONG now,
	ULONGLONG *holdStartedAt)
{
	if(!holdStartedAt)
		return 1;
	if(!down){
		*holdStartedAt = 0;
		return 1;
	}
	if(!wasDown || *holdStartedAt == 0)
		*holdStartedAt = now;
	const ULONGLONG heldMs = now-*holdStartedAt;
	// Keep a tap and the first second precise. Long calibration holds then cover
	// 180 degrees in a few seconds without increasing INI write frequency.
	if(heldMs >= 2600ULL)
		return 10;
	if(heldMs >= 1200ULL)
		return 3;
	return 1;
}

void HandleVrMenuInput(const XrVector2f &stick, bool decrease, bool increase,
	bool select, bool back)
{
	const bool vertical = fabsf(stick.y) >= 0.65f;
	int *selection;
	int itemCount;
	if(gVrDlssTuningMenuVisible){
		selection = &gVrDlssTuningMenuSelection;
		itemCount = VR_DLSS_TUNING_MENU_ITEM_COUNT;
	}else if(gVrRainMenuVisible){
		selection = &gVrRainMenuSelection;
		itemCount = VR_RAIN_MENU_ITEM_COUNT;
	}else if(gVrReflectionMenuVisible){
		selection = &gVrReflectionMenuSelection;
		itemCount = gVrReflectionPage == 1 ? VR_CAR_REFLECTION_ITEM_COUNT :
			gVrReflectionPage == 2 ? VR_WATER_ITEM_COUNT : VR_REFLECTION_MENU_ITEM_COUNT;
	}else if(gVrEffectsMenuVisible){
		selection = &gVrEffectsMenuSelection;
		itemCount = VR_EFFECTS_MENU_ITEM_COUNT;
	}else if(gVrLightingMenuVisible){
		selection = &gVrLightingMenuSelection;
		itemCount = VR_LIGHTING_MENU_ITEM_COUNT;
	}else if(gVrModelMenuVisible){
		selection = &gVrModelMenuSelection;
		itemCount = VR_MODEL_MENU_ITEM_COUNT;
	}else if(gVrGraphicsMenuVisible){
		selection = &gVrGraphicsMenuSelection;
		itemCount = VR_GRAPHICS_MENU_ITEM_COUNT;
	}else if(gVrTrafficMenuVisible){
		selection = &gVrTrafficMenuSelection;
		itemCount = VR_TRAFFIC_MENU_ITEM_COUNT;
	}else if(gVrBikeCalibrationMenuVisible){
		selection = &gVrBikeCalibrationMenuSelection;
		itemCount = GetVehicleCalibrationMenuItemCount();
	}else if(gVrHolsterMenuVisible){
		selection = &gVrHolsterMenuSelection;
		itemCount = HOLSTER_MENU_ITEM_COUNT;
	}else if(gVrCalibrationMenuVisible){
		selection = &gVrCalibrationMenuSelection;
		itemCount = VR_CALIBRATION_MENU_ITEM_COUNT;
	}else if(gVrHudMenuVisible){
		selection = &gVrHudMenuSelection;
		itemCount = gWristHud.editing ? VR_WRIST_ITEM_COUNT : VR_HUD_MENU_ITEM_COUNT;
	}else if(gVrVehicleMenuVisible){
		selection = &gVrVehicleMenuSelection;
		itemCount = VR_VEHICLE_MENU_ITEM_COUNT;
	}else if(gVrLocomotionMenuVisible){
		selection = gVrControlsMenuVisible ? &gVrControlsMenuSelection : &gVrLocomotionMenuSelection;
		itemCount = gVrControlsMenuVisible ? VR_CONTROLS_ITEM_COUNT : VR_LOCOMOTION_MENU_ITEM_COUNT;
	}else{
		selection = &gVrMenuSelection;
		itemCount = VR_MENU_ITEM_COUNT;
	}
	const ULONGLONG menuInputNow = GetTickCount64();
	const int navigation = gVrMenuNavigation.Pulse(stick.y, menuInputNow);
	if(navigation){
		*selection += navigation;
		if(*selection < 0) *selection = itemCount-1;
		if(*selection >= itemCount) *selection = 0;
		// Do not carry accelerated edits to a different row under held triggers.
		gVrMenuDecreaseRepeatAt = gVrMenuIncreaseRepeatAt = menuInputNow+420ULL;
		gVrMenuDecreaseHoldStartedAt = gVrMenuIncreaseHoldStartedAt = menuInputNow;
	}
	const int decreaseMagnitude = VrMenuValueRepeatMagnitude(decrease,
		gVrMenuDecreaseDown, menuInputNow, &gVrMenuDecreaseHoldStartedAt);
	const int increaseMagnitude = VrMenuValueRepeatMagnitude(increase,
		gVrMenuIncreaseDown, menuInputNow, &gVrMenuIncreaseHoldStartedAt);
	if(ShouldApplyVrMenuValueInput(decrease, gVrMenuDecreaseDown,
	   menuInputNow, &gVrMenuDecreaseRepeatAt))
		ChangeVrMenuValue(IsVrMenuValueRepeatable() ? -decreaseMagnitude : -1);
	if(ShouldApplyVrMenuValueInput(increase, gVrMenuIncreaseDown,
	   menuInputNow, &gVrMenuIncreaseRepeatAt))
		ChangeVrMenuValue(IsVrMenuValueRepeatable() ? increaseMagnitude : 1);
	const bool mainAction = !gVrModelMenuVisible &&
		!gVrGraphicsMenuVisible &&
		!gVrDlssTuningMenuVisible &&
		!gVrEffectsMenuVisible &&
		!gVrLightingMenuVisible &&
		!gVrRainMenuVisible &&
		!gVrReflectionMenuVisible &&
		!gVrTrafficMenuVisible &&
		!gVrBikeCalibrationMenuVisible &&
		!gVrHolsterMenuVisible &&
		!gVrCalibrationMenuVisible &&
		!gVrHudMenuVisible &&
		!gVrVehicleMenuVisible &&
		!gVrLocomotionMenuVisible &&
		(gVrMenuSelection == VR_MAIN_GRAPHICS ||
		 gVrMenuSelection == VR_MAIN_MODEL_SET ||
		 gVrMenuSelection == VR_MAIN_TRAFFIC_SETTINGS ||
		 gVrMenuSelection == VR_MAIN_HUD ||
		 gVrMenuSelection == VR_MAIN_CALIBRATION ||
		 gVrMenuSelection == VR_MAIN_HOLSTERS ||
		 gVrMenuSelection == VR_MAIN_CHEATS ||
		 gVrMenuSelection == VR_MAIN_ABOUT ||
		 gVrMenuSelection == VR_MAIN_VEHICLE_SETTINGS ||
		 gVrMenuSelection == VR_MAIN_LOCOMOTION_SETTINGS);
	if(select && !gVrMenuSelectDown &&
	   (gVrModelMenuVisible || gVrGraphicsMenuVisible ||
	    gVrDlssTuningMenuVisible ||
	    gVrEffectsMenuVisible ||
	    gVrLightingMenuVisible ||
	    gVrRainMenuVisible || gVrReflectionMenuVisible ||
	    gVrTrafficMenuVisible ||
	    gVrBikeCalibrationMenuVisible ||
	    gVrHolsterMenuVisible ||
	    gVrCalibrationMenuVisible || gVrHudMenuVisible ||
	    gVrVehicleMenuVisible ||
	    gVrLocomotionMenuVisible || mainAction))
		ChangeVrMenuValue(1);
	if(back && !gVrMenuBackDown){
		if(gVrDlssTuningMenuVisible){
			gVrDlssTuningMenuVisible = false;
			gVrGraphicsMenuVisible = true;
		}else if(gVrRainMenuVisible){
			gVrRainMenuVisible = false;
			gVrEffectsMenuVisible = true;
		}else if(gVrReflectionMenuVisible){
			if(gVrReflectionPage){
				gVrReflectionMenuSelection = gVrReflectionPage == 1 ? VR_REFLECTION_CARS : VR_REFLECTION_OCEAN;
				gVrReflectionPage = 0;
			}else{
				gVrReflectionMenuVisible = false;
				gVrEffectsMenuVisible = true;
			}
		}else if(gVrLightingMenuVisible){
			gVrLightingMenuVisible = false;
			gVrEffectsMenuVisible = true;
		}else if(gVrEffectsMenuVisible){
			gVrEffectsMenuVisible = false;
			gVrGraphicsMenuVisible = true;
		}else if(gVrModelMenuVisible)
			gVrModelMenuVisible = false;
		else if(gVrGraphicsMenuVisible)
			gVrGraphicsMenuVisible = false;
		else if(gVrTrafficMenuVisible)
			gVrTrafficMenuVisible = false;
		else if(gVrBikeCalibrationMenuVisible)
			gVrBikeCalibrationMenuVisible = false;
		else if(gVrHolsterMenuVisible)
			gVrHolsterMenuVisible = false;
		else if(gVrCalibrationMenuVisible)
			gVrCalibrationMenuVisible = false;
		else if(gVrHudMenuVisible){
			if(gWristHud.editing){
				gWristHud.editing = false;
				gVrHudMenuSelection = VR_HUD_WRIST_PLACEMENT;
			}else
				gVrHudMenuVisible = false;
		}
		else if(gVrVehicleMenuVisible)
			gVrVehicleMenuVisible = false;
		else if(gVrLocomotionMenuVisible){
			if(gVrControlsMenuVisible){
				gVrControlsMenuVisible = false;
				gVrLocomotionMenuSelection = VR_LOCOMOTION_CONTROLS;
			}else
				gVrLocomotionMenuVisible = false;
		}
		else
			gVrMenuVisible = false;
	}
	gVrMenuVerticalDown = vertical;
	gVrMenuHorizontalDown = false;
	gVrMenuSelectDown = select;
	gVrMenuBackDown = back;
	gVrMenuDecreaseDown = decrease;
	gVrMenuIncreaseDown = increase;
}

#if defined(DEBUG) && !defined(FINAL)
void HandleVrMissionMenuInput(const XrVector2f &stick, bool select, bool back)
{
	const bool vertical = fabsf(stick.y) >= 0.65f;
	int *selection = gVrMissionCategory < 0 ?
		&gVrMissionCategorySelection : &gVrMissionMenuSelection;
	const int count = gVrMissionCategory < 0 ?
		GetVrMissionCategoryCount() : GetVrMissionCount(gVrMissionCategory);
	const ULONGLONG menuInputNow = GetTickCount64();
	const int navigation = gVrMenuNavigation.Pulse(stick.y, menuInputNow);
	if(navigation && count > 0){
		*selection += navigation;
		if(*selection < 0) *selection = count-1;
		if(*selection >= count) *selection = 0;
	}
	if(select && !gVrMenuSelectDown){
		if(gVrMissionCategory < 0){
			gVrMissionCategory = gVrMissionCategorySelection;
			gVrMissionMenuSelection = 0;
		}else{
			const bool started = ActivateVrMission(gVrMissionCategory,
				gVrMissionMenuSelection);
			debug("[OpenXR] Mission %s: %s\n",
				GetVrMissionName(gVrMissionCategory,
					gVrMissionMenuSelection),
				started ? "started" : "not safe/available");
			if(started){
				gVrMissionMenuVisible = false;
				gCheatMenuVisible = false;
				gCheatMenuOpenedFromVrMenu = false;
			}
		}
	}
	if(back && !gVrMenuBackDown){
		if(gVrMissionCategory >= 0)
			gVrMissionCategory = -1;
		else
			gVrMissionMenuVisible = false;
	}
	gVrMenuVerticalDown = vertical;
	gVrMenuHorizontalDown = false;
	gVrMenuSelectDown = select;
	gVrMenuBackDown = back;
}
#endif

void HandleCheatMenuInput(const XrVector2f &stick, bool select, bool back)
{
#if defined(DEBUG) && !defined(FINAL)
	if(gVrMissionMenuVisible){
		HandleVrMissionMenuInput(stick, select, back);
		return;
	}
#endif
	const int count = GetOpenXrCheatMenuCount();
	const bool vertical = fabsf(stick.y) >= 0.65f;
	const bool horizontal = fabsf(stick.x) >= 0.65f;
	const ULONGLONG menuInputNow = GetTickCount64();
	const int navigation = gVrMenuNavigation.Pulse(stick.y, menuInputNow);
	if(navigation && count > 0){
		gCheatMenuSelection += navigation;
		if(gCheatMenuSelection < 0) gCheatMenuSelection = count-1;
		if(gCheatMenuSelection >= count) gCheatMenuSelection = 0;
	}
	const int direction = gCheatMenuHorizontalNavigation.Pulse(-stick.x, menuInputNow);
	if(direction){
#if defined(DEBUG) && !defined(FINAL)
		if(gCheatMenuSelection == VR_CHEAT_CAL_HOLSTER)
			CycleHolsterCalibrationPoint(direction);
		else if(gCheatMenuSelection >= VR_CHEAT_LOCAL_ITEM_COUNT &&
#else
		if(gCheatMenuSelection >= VR_CHEAT_LOCAL_ITEM_COUNT &&
#endif
		   gCheatMenuSelection < VR_CHEAT_LOCAL_ITEM_COUNT+GetVrCheatCount() &&
		   CycleVrCheatSelection(
			gCheatMenuSelection-VR_CHEAT_LOCAL_ITEM_COUNT,
			direction))
			debug("[OpenXR] Cheat model selected: %s\n",
				GetOpenXrCheatMenuName(gCheatMenuSelection));
	}
	if(select && !gVrMenuSelectDown){
		bool activated = false;
#if defined(DEBUG) && !defined(FINAL)
		if(gCheatMenuSelection == VR_CHEAT_CAL_NEXT_WEAPON)
			activated = PutNextHolsterCalibrationWeapon();
		else if(gCheatMenuSelection == VR_CHEAT_CAL_FINISH)
			activated = FinishHolsterCalibrationLoadout();
		else if(gCheatMenuSelection == GetOpenXrMissionMenuIndex()){
			gVrMissionMenuVisible = true;
			gVrMissionCategory = -1;
			gVrMissionCategorySelection = 0;
			gVrMissionMenuSelection = 0;
			activated = true;
		}else
#endif
		if(gCheatMenuSelection >= VR_CHEAT_LOCAL_ITEM_COUNT &&
		   gCheatMenuSelection < VR_CHEAT_LOCAL_ITEM_COUNT+GetVrCheatCount())
			activated = ActivateVrCheat(
				gCheatMenuSelection-VR_CHEAT_LOCAL_ITEM_COUNT);
		debug("[OpenXR] Cheat %s: %s\n",
			GetOpenXrCheatMenuName(gCheatMenuSelection),
			activated ? "activated" : "not available");
	}
	if(back && !gVrMenuBackDown){
		// Closing the menu never mutates an active developer calibration preview;
		// the Debug-only FINISH action owns loadout restoration.
		gCheatMenuVisible = false;
		if(gCheatMenuOpenedFromVrMenu){
			gCheatMenuOpenedFromVrMenu = false;
			gVrMenuVisible = true;
		}
	}
	gVrMenuVerticalDown = vertical;
	gVrMenuHorizontalDown = horizontal;
	gVrMenuSelectDown = select;
	gVrMenuBackDown = back;
}

void ToggleCheatMenu()
{
	gCheatMenuVisible = !gCheatMenuVisible;
	gCheatMenuOpenedFromVrMenu = false;
	gVrMissionMenuVisible = false;
	gVrMissionCategory = -1;
	gVrMenuVisible = false;
	gVrGraphicsMenuVisible = false;
	gVrDlssTuningMenuVisible = false;
	gVrEffectsMenuVisible = false;
	gVrLightingMenuVisible = false;
	gVrRainMenuVisible = false;
	gVrReflectionMenuVisible = false;
	gVrModelMenuVisible = false;
	gVrTrafficMenuVisible = false;
	gVrVehicleMenuVisible = false;
	gVrLocomotionMenuVisible = false;
	gVrHudMenuVisible = false;
	gVrHolsterMenuVisible = false;
	gVrCalibrationMenuVisible = false;
	gVrBikeCalibrationMenuVisible = false;
	gCheatMenuSelection = 0;
	gVrMenuVerticalDown = gVrMenuHorizontalDown = false;
	gVrMenuNavigation.Reset();
	gCheatMenuHorizontalNavigation.Reset();
	gVrMenuSelectDown = gVrMenuBackDown = false;
	gVrMenuDecreaseDown = gVrMenuIncreaseDown = false;
	gVrMenuDecreaseRepeatAt = gVrMenuIncreaseRepeatAt = 0;
	gVrMenuDecreaseHoldStartedAt = gVrMenuIncreaseHoldStartedAt = 0;
	debug("[OpenXR] Cheat menu: %s\n", gCheatMenuVisible ? "open" : "closed");
}

void ResetManualReloadState(ManualReloadState &state)
{
	state = ManualReloadState();
}

bool BuildManualReloadSpotMatrix(int slot, CMatrix *matrix)
{
	if(!matrix)
		return false;
	CMatrix holster;
	const bool assigned = BuildWeaponHolsterMatrix(slot, &holster);
	// The weapon leaves this body spot when it is grabbed.  Present the fresh
	// magazine at that now-empty spot, upright against the player's chest. If
	// the player edits this slot to EMPTY while still holding it, keep reload
	// usable at a fixed emergency chest point instead of losing the magazine.
	matrix->SetUnity();
	matrix->GetRight() = gBaseCamera.GetRight();
	matrix->GetUp() = gBaseCamera.GetUp();
	matrix->GetForward() = gBaseCamera.GetForward();
	matrix->GetPosition() = assigned ? holster.GetPosition() :
		gBaseCamera.GetPosition() + gBaseCamera.GetUp()*-0.34f +
		gBaseCamera.GetForward()*0.14f;
	return true;
}

bool BuildManualReloadHeldMatrix(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   !gTrackedHandPoseValid[hand])
		return false;
	const XrPosef &pose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	CVector right = ToGameVector(Rotate(pose.orientation, localRight));
	CVector up = ToGameVector(Rotate(pose.orientation, localUp));
	CVector forward = ToGameVector(Rotate(pose.orientation, localForward));
	right.Normalise();
	up.Normalise();
	forward.Normalise();
	matrix->SetUnity();
	// A magazine is pinched along the controller's pointing axis.  The exact
	// orientation is intentionally not part of the insertion test yet; position
	// is forgiving while the procedural hand/model alignment is still evolving.
	matrix->GetRight() = right;
	matrix->GetUp() = forward;
	matrix->GetForward() = up;
	matrix->GetPosition() = gBaseCamera.GetPosition() +
		ToGameVector(pose.position) + forward*0.035f - up*0.015f;
	return true;
}

bool BuildManualReloadSocketPosition(int weaponHand, CVector *position)
{
	if(!position || weaponHand < 0 || weaponHand >= EYE_COUNT)
		return false;
	const XrPosef *pose = nil;
	if(gTrackedHandAimPoseValid[weaponHand])
		pose = &gTrackedHandAimPose[weaponHand];
	else if(gTrackedHandPoseValid[weaponHand])
		pose = &gTrackedHandPose[weaponHand];
	if(!pose)
		return false;
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	CVector up = ToGameVector(Rotate(pose->orientation, localUp));
	CVector forward = ToGameVector(Rotate(pose->orientation, localForward));
	up.Normalise();
	forward.Normalise();
	// The calibrated weapon origin lies inside the gripping hand.  A generous
	// socket just below and slightly ahead of it matches the pistol/SMG magwell
	// without requiring weapon-specific DFF sockets in this first pass.
	*position = gBaseCamera.GetPosition() + ToGameVector(pose->position) -
		up*0.060f + forward*0.020f;
	return true;
}

bool IsMagazineHandInUse(int hand)
{
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++)
		if(gManualReload[weaponHand].active &&
		   gManualReload[weaponHand].magazineHand == hand)
			return true;
	return false;
}

uint32 UpdateManualReloadInput(float leftGrip, float rightGrip,
	uint32 blockedHands)
{
	const float grips[EYE_COUNT] = { leftGrip, rightGrip };
	uint32 capturedHands = 0;
	const ULONGLONG now = GetTickCount64();

	if(!ShouldUseManualReload()){
		for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++)
			ResetManualReloadState(gManualReload[weaponHand]);
		for(int hand = 0; hand < EYE_COUNT; hand++)
			gManualReloadGripDown[hand] = grips[hand] >= 0.45f;
		return 0;
	}

	// Reject stale targets before accepting any grip.  The final request is
	// validated again against the exact inventory slot by CPlayerPed.
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++){
		ManualReloadState &reload = gManualReload[weaponHand];
		if(reload.active &&
		   (gHeldWeaponSlot[weaponHand] != reload.slot ||
		    !gTrackedHandPoseValid[weaponHand]))
			ResetManualReloadState(reload);
		if(reload.active && reload.magazineHand >= 0)
			capturedHands |= 1u << reload.magazineHand;
	}

	// A held magazine follows either hand.  Moving it away from the body spot
	// once prevents an accidental instant reload when the gun is also near the
	// chest; bringing it into the magwell completes insertion immediately.
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++){
			ManualReloadState &reload = gManualReload[weaponHand];
			const int magazineHand = reload.magazineHand;
			if(!reload.active || reload.requested || magazineHand < 0 ||
			   (blockedHands & (1u << magazineHand)) != 0)
				continue;
			capturedHands |= 1u << magazineHand;
			if(grips[magazineHand] <= 0.30f){
				reload.magazineHand = -1;
				reload.movedAwayFromSpot = false;
				reload.grabbedAt = 0;
				debug("[OpenXR] Reload magazine returned to slot %d\n", reload.slot);
				continue;
			}
			CMatrix heldMatrix, spotMatrix;
			CVector socketPosition;
			if(!BuildManualReloadHeldMatrix(magazineHand, &heldMatrix) ||
			   !BuildManualReloadSpotMatrix(reload.slot, &spotMatrix) ||
			   !BuildManualReloadSocketPosition(weaponHand, &socketPosition))
				continue;
			if((heldMatrix.GetPosition()-spotMatrix.GetPosition()).Magnitude() >= 0.08f)
				reload.movedAwayFromSpot = true;
			if(reload.movedAwayFromSpot && now-reload.grabbedAt >= 150 &&
			   (heldMatrix.GetPosition()-socketPosition).Magnitude() <= 0.115f){
				reload.requested = true;
				debug("[OpenXR] Reload magazine inserted for %s hand slot %d\n",
					weaponHand == 0 ? "left" : "right", reload.slot);
			}
	}

	// A free hand can grab the closest available magazine.  Magazine grips
	// take priority over neighbouring weapon holsters.
	for(int hand = 0; hand < EYE_COUNT; hand++){
			if((capturedHands & (1u << hand)) != 0 ||
			   (blockedHands & (1u << hand)) != 0 ||
			   gHeldWeaponSlot[hand] >= 0 || !gTrackedHandPoseValid[hand] ||
			   grips[hand] < 0.65f || gManualReloadGripDown[hand] ||
			   IsMagazineHandInUse(hand))
				continue;
			const CVector handPosition = gBaseCamera.GetPosition() +
				ToGameVector(gTrackedHandPose[hand].position);
			float closestDistance = 0.20f;
			int closestWeaponHand = -1;
			for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++){
				ManualReloadState &reload = gManualReload[weaponHand];
				if(!reload.active || reload.requested || reload.magazineHand >= 0 ||
				   hand == weaponHand)
					continue;
				CMatrix spotMatrix;
				if(!BuildManualReloadSpotMatrix(reload.slot, &spotMatrix))
					continue;
				const float distance =
					(handPosition-spotMatrix.GetPosition()).Magnitude();
				if(distance < closestDistance){
					closestDistance = distance;
					closestWeaponHand = weaponHand;
				}
			}
			if(closestWeaponHand >= 0){
				ManualReloadState &reload = gManualReload[closestWeaponHand];
				reload.magazineHand = hand;
				reload.movedAwayFromSpot = false;
				reload.grabbedAt = now;
				capturedHands |= 1u << hand;
				debug("[OpenXR] Reload magazine grabbed by %s hand for slot %d\n",
					hand == 0 ? "left" : "right", reload.slot);
			}
	}

	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(grips[hand] <= 0.30f)
			gManualReloadGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gManualReloadGripDown[hand] = true;
	}
	return capturedHands;
}

bool IsWeaponSlotOwnedByOtherHand(int hand, int slot)
{
	for(int other = 0; other < EYE_COUNT; other++)
		if(other != hand && (gHeldWeaponSlot[other] == slot || gDroppedWeaponSlot[other] == slot))
			return true;
	return false;
}

void ResetPhysicalMeleeMotion(int hand);
void InvalidatePhysicalMeleeWeaponMatrix(int hand);
static const float WEAPON_HOLSTER_REACH = 0.24f;

bool IsHeldWeaponNearAssignedHolster(int hand, int slot)
{
	if(hand < 0 || hand >= EYE_COUNT || slot <= WEAPONSLOT_UNARMED ||
	   slot >= TOTAL_WEAPON_SLOTS || !gTrackedHandPoseValid[hand])
		return false;
	CMatrix holster;
	if(!BuildWeaponHolsterMatrix(slot, &holster))
		return false;
	const CVector handPosition = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandPose[hand].position);
	// Match the existing grab radius.  The same physical socket therefore owns
	// both directions of the interaction instead of requiring a weapon-specific
	// offset or an invisible second return zone.
	return (handPosition-holster.GetPosition()).Magnitude() <=
		WEAPON_HOLSTER_REACH;
}

void ReturnHeldWeaponToHolster(int hand, int slot)
{
	ClearWeaponSupportForHand(hand);
	gHeldWeaponSlot[hand] = -1;
	gWeaponHolsterSelection[hand] = -1;
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
	ClearDroppedWeapon(hand);
	ResetManualReloadState(gManualReload[hand]);
	gTrackedThrowablePreviewActive[hand] = false;
	gTrackedAimCacheValid[hand] = false;
	debug("[OpenXR] Weapon returned to holster by %s hand slot %d\n",
		hand == 0 ? "left" : "right", slot);
}

void InvalidatePhysicalMeleeWeaponMatrix(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedWeaponContactMatrixSlot[hand] = -1;
	gTrackedWeaponContactMatrixType[hand] = -1;
	gTrackedWeaponContactMatrixFrame[hand] = 0;
}

void StartDroppedWeapon(int hand, int slot)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0)
		return;
	if(!gTrackedHandPoseValid[hand]){
		InvalidatePhysicalMeleeWeaponMatrix(hand);
		ResetPhysicalMeleeMotion(hand);
		return;
	}
	ClearWeaponSupportForHand(hand);
	const XrPosef &pose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	gDroppedWeaponMatrix[hand].SetUnity();
	gDroppedWeaponMatrix[hand].GetRight() = ToGameVector(Rotate(pose.orientation, localRight));
	gDroppedWeaponMatrix[hand].GetForward() = ToGameVector(Rotate(pose.orientation, localForward));
	gDroppedWeaponMatrix[hand].GetUp() = ToGameVector(Rotate(pose.orientation, localUp));
	gDroppedWeaponMatrix[hand].GetRight().Normalise();
	gDroppedWeaponMatrix[hand].GetForward().Normalise();
	gDroppedWeaponMatrix[hand].GetUp().Normalise();
	if(gTrackedWeaponRenderMatrixSlot[hand] == slot)
		gDroppedWeaponMatrix[hand] = gTrackedWeaponRenderMatrix[hand];
	gDroppedWeaponStartPosition[hand] = gTrackedWeaponRenderMatrixSlot[hand] == slot ?
		gDroppedWeaponMatrix[hand].GetPosition() :
		gBaseCamera.GetPosition()+ToGameVector(pose.position);
	gDroppedWeaponMatrix[hand].GetPosition() = gDroppedWeaponStartPosition[hand];
	gDroppedWeaponGravityUp[hand] = gBaseCamera.GetUp();
	gDroppedWeaponGravityUp[hand].Normalise();
	gDroppedWeaponLinearVelocity[hand] = ToGameVector(gTrackedHandLinearVelocity[hand]);
	const float linearSpeed = gDroppedWeaponLinearVelocity[hand].Magnitude();
	if(linearSpeed > 6.0f)
		gDroppedWeaponLinearVelocity[hand] *= 6.0f/linearSpeed;
	gDroppedWeaponAngularVelocity[hand] = ToGameVector(gTrackedHandAngularVelocity[hand]);
	const float angularSpeed = gDroppedWeaponAngularVelocity[hand].Magnitude();
	if(angularSpeed > 12.0f)
		gDroppedWeaponAngularVelocity[hand] *= 12.0f/angularSpeed;
	gDroppedWeaponSlot[hand] = slot;
	gDroppedWeaponStartTime[hand] = GetTickCount64();
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
}

enum { DROPPED_WEAPON_LIFETIME_MS = 1800 };

void ClearDroppedWeapon(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gDroppedWeaponSlot[hand] = -1;
	gDroppedWeaponStartTime[hand] = 0;
	gDroppedWeaponLinearVelocity[hand] = CVector(0.0f, 0.0f, 0.0f);
	gDroppedWeaponAngularVelocity[hand] = CVector(0.0f, 0.0f, 0.0f);
}

bool BuildDroppedWeaponMatrix(int hand, ULONGLONG now, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= EYE_COUNT ||
	   gDroppedWeaponSlot[hand] < 0 || gDroppedWeaponStartTime[hand] == 0)
		return false;
	const double elapsedMs = (double)(now-gDroppedWeaponStartTime[hand]);
	if(elapsedMs < 0.0 || elapsedMs >= DROPPED_WEAPON_LIFETIME_MS)
		return false;
	const float elapsed = (float)(elapsedMs/1000.0);
	*matrix = gDroppedWeaponMatrix[hand];
	const float angularSpeed = gDroppedWeaponAngularVelocity[hand].Magnitude();
	if(angularSpeed > 0.001f){
		CVector spinAxis = gDroppedWeaponAngularVelocity[hand];
		spinAxis *= 1.0f/angularSpeed;
		const float angle = angularSpeed*elapsed;
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(), spinAxis, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(), spinAxis, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(), spinAxis, angle);
	}
	// Dropped weapons use 4.2 world units/s^2; the catch lifetime is unchanged.
	const float fallDistance = 0.5f*4.2f*elapsed*elapsed;
	matrix->GetPosition() = gDroppedWeaponStartPosition[hand] +
		gDroppedWeaponLinearVelocity[hand]*elapsed -
		gDroppedWeaponGravityUp[hand]*fallDistance;
	return true;
}

void GiveWeaponToHand(int hand, int slot, const char *reason)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0)
		return;
	ClearWeaponSupportForHand(hand);
	gHeldWeaponSlot[hand] = slot;
	gWeaponHolsterSelection[hand] = slot;
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
	// A deliberate holster/catch grab is already a safe neutral origin.  Arm the
	// first valid tracked sample immediately instead of requiring the player to
	// hold perfectly still after a respawn before the weapon starts working.
	gPhysicalMeleeFreshGrab[hand] = true;
	if(slot == WEAPONSLOT_PROJECTILE &&
	   GetVrWeaponTypeForSlot(slot) == WEAPONTYPE_DETONATOR_GRENADE){
		// C4 stays in the grabbing hand; its controller is calibrated and used
		// independently in the opposite hand.
		gTrackedDetonatorHand = 1-hand;
		gCalibrationEditHand = gTrackedDetonatorHand;
		gTrackedDetonatorWasActive[gTrackedDetonatorHand] = false;
	}else
		gCalibrationEditHand = hand;
	debug("[OpenXR] Weapon %s by %s hand slot %d\n",
		reason, hand == 0 ? "left" : "right", slot);
}

void ClearAttachedMissionWeaponState(const float *grips)
{
	if(!gAttachedMissionWeaponForced)
		return;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gHeldWeaponSlot[hand] == gAttachedMissionWeaponSlot)
			gHeldWeaponSlot[hand] = -1;
		ClearWeaponSupportForHand(hand);
		gWeaponHolsterSelection[hand] = -1;
		gTrackedWeaponRenderMatrixSlot[hand] = -1;
		gTrackedWeaponContactMatrixSlot[hand] = -1;
		InvalidatePhysicalMeleeWeaponMatrix(hand);
		ClearDroppedWeapon(hand);
		ResetManualReloadState(gManualReload[hand]);
		gManualReloadGripDown[hand] = grips && grips[hand] >= 0.45f;
		gWeaponHolsterGripDown[hand] = grips && grips[hand] >= 0.45f;
		gTrackedThrowablePreviewActive[hand] = false;
		gTrackedAimCacheValid[hand] = false;
		ResetPhysicalMeleeMotion(hand);
	}
	debug("[OpenXR] Released scripted mounted weapon slot %d\n",
		gAttachedMissionWeaponSlot);
	gAttachedMissionWeaponForced = false;
	gAttachedMissionWeaponSlot = -1;
}

bool UpdateAttachedMissionWeaponInput(const float *grips)
{
	CPlayerPed *player = FindPlayerPed();
	const bool active = player && player->m_attachedTo &&
		player->m_currentWeapon > WEAPONSLOT_UNARMED &&
		player->m_currentWeapon < TOTAL_WEAPON_SLOTS &&
		IsPhysicalGunTypeInternal(player->GetWeapon()->m_eWeaponType);
	if(!active){
		ClearAttachedMissionWeaponState(grips);
		return false;
	}

	const int slot = player->m_currentWeapon;
	const int weaponHand = 1;
	if(!gAttachedMissionWeaponForced ||
	   gAttachedMissionWeaponSlot != slot ||
	   gHeldWeaponSlot[weaponHand] != slot){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gWeaponHolsterSelection[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			gTrackedWeaponContactMatrixSlot[hand] = -1;
			InvalidatePhysicalMeleeWeaponMatrix(hand);
			ClearDroppedWeapon(hand);
			ResetManualReloadState(gManualReload[hand]);
			gTrackedThrowablePreviewActive[hand] = false;
			gTrackedAimCacheValid[hand] = false;
			ResetPhysicalMeleeMotion(hand);
		}
		GiveWeaponToHand(weaponHand, slot, "scripted mounted gun forced into");
		// PlayerControlM16 consumes the tracked trigger directly, so there is no
		// holster selection for ProcessPlayerWeapon to consume in this state.
		gWeaponHolsterSelection[weaponHand] = -1;
		gAttachedMissionWeaponForced = true;
		gAttachedMissionWeaponSlot = slot;
	}

	// The scripted gun remains owned by the right hand, but a normal calibrated
	// foregrip grab is still allowed.  This is kept inside the mounted path so a
	// left-grip press cannot leak into Vice City's L1/radio binding before the
	// generic holster code has recognised the support hand.
	const int supportHand = 0;
	const int weaponType = player->GetWeapon()->m_eWeaponType;
	if(gWeaponSupportHand[weaponHand] == supportHand){
		if(!gTrackedHandPoseValid[supportHand] || grips[supportHand] <= 0.30f){
			gWeaponSupportHand[weaponHand] = -1;
			debug("[OpenXR] Scripted mounted-gun support grip released\n");
		}
	}else{
		gWeaponSupportHand[weaponHand] = -1;
		if(gTrackedHandPoseValid[supportHand] && grips[supportHand] >= 0.65f){
			CVector pivot, expected;
			if(BuildSupportGripVector(weaponHand, weaponType,
			   &pivot, &expected)){
				const CVector supportPosition = gBaseCamera.GetPosition()+
					ToGameVector(gTrackedHandPose[supportHand].position);
				if((supportPosition-(pivot+expected)).Magnitude() <= 0.18f){
					gWeaponSupportHand[weaponHand] = supportHand;
					gCalibrationEditHand = weaponHand;
					debug("[OpenXR] Scripted mounted gun grabbed with both hands\n");
				}
			}
		}
	}
	for(int hand = 0; hand < EYE_COUNT; hand++)
		gWeaponHolsterGripDown[hand] = grips && grips[hand] >= 0.45f;
	return true;
}

void UpdateWeaponHolsterInput(float leftGrip, float rightGrip, uint32 blockedHands)
{
	const ULONGLONG now = GetTickCount64();
	const float grips[EYE_COUNT] = { leftGrip, rightGrip };
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gDroppedWeaponSlot[hand] >= 0 &&
		   (double)(now-gDroppedWeaponStartTime[hand]) >= DROPPED_WEAPON_LIFETIME_MS)
			ClearDroppedWeapon(hand);
	}
	if(UpdateAttachedMissionWeaponInput(grips))
		return;
	if(FindPlayerVehicle() != nil &&
	   !IsVrDrivingActiveInternal()){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			ClearDroppedWeapon(hand);
			gWeaponHolsterGripDown[hand] = grips[hand] >= 0.45f;
		}
		return;
	}

	uint32 handledHands = blockedHands;
	// Maintain an existing support grip before considering a fresh transfer.
	// Releasing the support hand only detaches it. Releasing the primary while
	// the support hand remains squeezed promotes that hand to primary, which
	// makes natural hand-to-hand passes possible without dropping the weapon.
	for(int primary = 0; primary < EYE_COUNT; primary++){
		const int support = gWeaponSupportHand[primary];
		if(support < 0)
			continue;
		const int slot = gHeldWeaponSlot[primary];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot < 0 || gHeldWeaponSlot[support] >= 0 ||
		   !IsTwoHandedWeaponTypeInternal(weaponType) ||
		   !gTrackedHandPoseValid[primary] || !gTrackedHandPoseValid[support]){
			gWeaponSupportHand[primary] = -1;
			continue;
		}
		if(grips[support] <= 0.30f){
			gWeaponSupportHand[primary] = -1;
			debug("[OpenXR] Support grip released from %s hand slot %d\n",
				support == 0 ? "left" : "right", slot);
			continue;
		}
		handledHands |= 1u << support;
		if(grips[primary] <= 0.30f && !gWeaponGripLockEnabled){
			gHeldWeaponSlot[primary] = -1;
			gWeaponHolsterSelection[primary] = -1;
			gTrackedWeaponRenderMatrixSlot[primary] = -1;
			gWeaponSupportHand[primary] = -1;
			GiveWeaponToHand(support, slot, "promoted from support to");
			handledHands |= 1u << primary;
		}
	}

	// A fresh squeeze at the saved foregrip socket creates a real two-handed
	// hold. The supporting hand never owns the inventory slot and therefore
	// cannot fire a duplicate shot or render a duplicate weapon.
	for(int primary = 0; primary < EYE_COUNT; primary++){
		const int support = 1-primary;
		const int slot = gHeldWeaponSlot[primary];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot < 0 || gWeaponSupportHand[primary] >= 0 ||
		   gHeldWeaponSlot[support] >= 0 ||
		   (handledHands & (1u << support)) != 0 ||
		   !IsTwoHandedWeaponTypeInternal(weaponType) ||
		   !gTrackedHandPoseValid[support] || grips[support] < 0.65f ||
		   gWeaponHolsterGripDown[support])
			continue;
		CVector pivot, expected;
		if(!BuildSupportGripVector(primary, weaponType, &pivot, &expected))
			continue;
		const CVector supportPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[support].position);
		if((supportPosition-(pivot+expected)).Magnitude() > 0.18f)
			continue;
		gWeaponSupportHand[primary] = support;
		gCalibrationEditHand = primary;
		handledHands |= 1u << support;
		debug("[OpenXR] Two-hand support grip: %s supports %s hand slot %d\n",
			support == 0 ? "left" : "right",
			primary == 0 ? "left" : "right", slot);
		if(grips[primary] <= 0.30f && !gWeaponGripLockEnabled){
			// Both edges can arrive in the same OpenXR sample (release primary,
			// squeeze support). Promote immediately so the generic drop pass below
			// cannot destroy the newly established hand-off.
			gHeldWeaponSlot[primary] = -1;
			gWeaponHolsterSelection[primary] = -1;
			gTrackedWeaponRenderMatrixSlot[primary] = -1;
			gWeaponSupportHand[primary] = -1;
			GiveWeaponToHand(support, slot, "promoted from fresh support to");
			handledHands |= 1u << primary;
		}
	}

	// A fresh squeeze close to the gun in the other hand performs a direct
	// hand-off.  It deliberately works while grip lock is enabled: the lock only
	// suppresses an accidental release, never an explicit second-hand grab.
	for(int receiver = 0; receiver < EYE_COUNT; receiver++){
		const int source = 1-receiver;
		if((handledHands & (1u << receiver)) != 0 ||
		   IsWeaponSupportHandInternal(receiver) ||
		   gHeldWeaponSlot[receiver] >= 0 || gHeldWeaponSlot[source] < 0 ||
		   !gTrackedHandPoseValid[receiver] || !gTrackedHandPoseValid[source] ||
		   grips[receiver] < 0.65f || gWeaponHolsterGripDown[receiver])
			continue;
		const CVector receiverPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[receiver].position);
		const int sourceSlot = gHeldWeaponSlot[source];
		const CVector sourcePosition = gTrackedWeaponRenderMatrixSlot[source] == sourceSlot ?
			gTrackedWeaponRenderMatrix[source].GetPosition() :
			gBaseCamera.GetPosition()+ToGameVector(gTrackedHandPose[source].position);
		if((receiverPosition-sourcePosition).Magnitude() > 0.20f)
			continue;
		const int slot = gHeldWeaponSlot[source];
		CMatrix transferredMatrix;
		const bool hasTransferredMatrix =
			gTrackedWeaponRenderMatrixSlot[source] == slot;
		if(hasTransferredMatrix)
			transferredMatrix = gTrackedWeaponRenderMatrix[source];
		ClearWeaponSupportForHand(source);
		gHeldWeaponSlot[source] = -1;
		gTrackedWeaponRenderMatrixSlot[source] = -1;
		gWeaponHolsterSelection[source] = -1;
		GiveWeaponToHand(receiver, slot, "transferred to");
		if(hasTransferredMatrix){
			gTrackedWeaponRenderMatrix[receiver] = transferredMatrix;
			gTrackedWeaponRenderMatrixSlot[receiver] = slot;
		}
		handledHands |= (1u << source) | (1u << receiver);
	}

	// A flying gun can be caught with the other free hand.  Unlike a holster or
	// direct hand-off, the grip may already be held while waiting for the gun to
	// enter the catch radius, which makes actual toss-and-catch gestures reliable.
	for(int receiver = 0; receiver < EYE_COUNT; receiver++){
		const int source = 1-receiver;
		if((handledHands & (1u << receiver)) != 0 ||
		   IsWeaponSupportHandInternal(receiver) ||
		   gHeldWeaponSlot[receiver] >= 0 || grips[receiver] < 0.55f ||
		   !gTrackedHandPoseValid[receiver] || gDroppedWeaponSlot[source] < 0)
			continue;
		CMatrix flyingWeapon;
		if(!BuildDroppedWeaponMatrix(source, now, &flyingWeapon))
			continue;
		const CVector receiverPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[receiver].position);
		if((receiverPosition-flyingWeapon.GetPosition()).Magnitude() > 0.22f)
			continue;
		const int slot = gDroppedWeaponSlot[source];
		ClearDroppedWeapon(source);
		GiveWeaponToHand(receiver, slot, "caught by");
		gTrackedWeaponRenderMatrix[receiver] = flyingWeapon;
		gTrackedWeaponRenderMatrixSlot[receiver] = slot;
		handledHands |= 1u << receiver;
	}

	for(int hand = 0; hand < EYE_COUNT; hand++){
		// A remote grenade grabbed by the previous hand in this same loop
		// immediately reserves its opposite controller hand.
		handledHands |= GetTrackedDetonatorHandMaskInternal();
		const float grip = grips[hand];
		if(IsWeaponSupportHandInternal(hand))
			handledHands |= 1u << hand;
		if((handledHands & (1u << hand)) != 0){
			// Keep the holster edge latch in sync while this grip belongs to a
			// magazine.  The still-held grip must not immediately grab a gun after
			// insertion; a fresh grab requires a real release first.
			if(grip <= 0.30f)
				gWeaponHolsterGripDown[hand] = false;
			else if(grip >= 0.65f)
				gWeaponHolsterGripDown[hand] = true;
			continue;
		}
		// Grip hysteresis prevents a slightly noisy analogue squeeze from dropping
		// a gun that the player is still physically holding. Grip Lock blocks an
		// ordinary release, but a fresh release edge inside this weapon's own socket
		// is an explicit holster gesture and must remain available in that mode.
		if(gHeldWeaponSlot[hand] >= 0 && grip <= 0.30f){
			const int releasedSlot = gHeldWeaponSlot[hand];
			const bool holsterRelease = gWeaponHolsterGripDown[hand] &&
				IsHeldWeaponNearAssignedHolster(hand, releasedSlot);
			if(holsterRelease)
				ReturnHeldWeaponToHolster(hand, releasedSlot);
			else if(!gWeaponGripLockEnabled){
				gHeldWeaponSlot[hand] = -1;
				StartDroppedWeapon(hand, releasedSlot);
				debug("[OpenXR] Weapon released from %s hand slot %d\n",
					hand == 0 ? "left" : "right", releasedSlot);
			}
		}
		if(gHeldWeaponSlot[hand] < 0 && !IsWeaponSupportHandInternal(hand) &&
		   grip >= 0.65f &&
		   !gWeaponHolsterGripDown[hand] && gTrackedHandPoseValid[hand]){
			const CVector handPosition = gBaseCamera.GetPosition()+
				ToGameVector(gTrackedHandPose[hand].position);
			float closestDistance = WEAPON_HOLSTER_REACH;
			int closestSlot = -1;
			for(int slot = WEAPONSLOT_MELEE; slot < TOTAL_WEAPON_SLOTS; slot++){
				if((gWeaponHolsterMask & (1u << slot)) == 0 ||
				   IsWeaponSlotOwnedByOtherHand(hand, slot))
					continue;
				CMatrix holster;
				if(!BuildWeaponHolsterMatrix(slot, &holster))
					continue;
				const float distance = (handPosition-holster.GetPosition()).Magnitude();
				if(distance < closestDistance){
					closestDistance = distance;
					closestSlot = slot;
				}
			}
			if(closestSlot >= 0){
				ClearDroppedWeapon(hand);
				GiveWeaponToHand(hand, closestSlot, "gripped from holster by");
			}
		}
		if(grip <= 0.30f)
			gWeaponHolsterGripDown[hand] = false;
		else if(grip >= 0.65f)
			gWeaponHolsterGripDown[hand] = true;
	}
}

float XrVectorLength(const XrVector3f &value)
{
	return sqrtf(value.x*value.x + value.y*value.y + value.z*value.z);
}

XrVector3f AddXrVector(const XrVector3f &left, const XrVector3f &right)
{
	return { left.x+right.x, left.y+right.y, left.z+right.z };
}

XrVector3f SubtractXrVector(const XrVector3f &left, const XrVector3f &right)
{
	return { left.x-right.x, left.y-right.y, left.z-right.z };
}

XrVector3f ScaleXrVector(const XrVector3f &value, float scale)
{
	return { value.x*scale, value.y*scale, value.z*scale };
}

XrVector3f CrossXrVector(const XrVector3f &left, const XrVector3f &right)
{
	return {
		left.y*right.z-left.z*right.y,
		left.z*right.x-left.x*right.z,
		left.x*right.y-left.y*right.x
	};
}

void ResetPhysicalMeleeMotion(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gPhysicalMeleeMotion[hand] = PhysicalMeleeMotion();
	gPhysicalMeleeStrike[hand].pending = false;
	gPhysicalMeleeFreshGrab[hand] = false;
}

bool IsPhysicalGameplayAvailable()
{
	CPlayerPed *player = FindPlayerPed();
	return !IsStereoCutsceneActive() && !IsVehicleThirdPersonActive() &&
		player && player->m_rwObject && !player->DyingOrDead() &&
		CWorld::Players[CWorld::PlayerInFocus].m_WBState == WBSTATE_PLAYING;
}

bool IsTrackedScopeGameplaySafe()
{
	return gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bWantToRestart &&
		!FrontEndMenuManager.m_bWantToLoad &&
		!FrontEndMenuManager.m_bMenuActive &&
		IsPhysicalGameplayAvailable() && !CTimer::GetIsPaused() &&
		!CGame::playingIntro && !CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!gVrMenuVisible && !gCheatMenuVisible;
}

void ResetFirstRunWelcomeStepBaseline()
{
	gVrAboutStepBaselineValid = false;
	gVrAboutStepControllableFrames = 0;
	gVrAboutStepLastGameFrame = 0;
	gVrAboutStepMovementInputSeen = false;
}

bool DidPlayerTakeFirstRunWelcomeStep()
{
	CPlayerPed *player = FindPlayerPed();
	CPad *pad = CPad::GetPad(CWorld::PlayerInFocus);
	const bool controllableOnFoot =
		IsTrackedScopeGameplaySafe() && !TheCamera.m_WideScreenOn &&
		player && pad && !player->bInVehicle && player->IsPedInControl() &&
		!pad->ArePlayerControlsDisabled() &&
		!CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	if(!controllableOnFoot){
		ResetFirstRunWelcomeStepBaseline();
		return false;
	}

	const CVector position = player->GetPosition();
	const uint32 gameFrame = CTimer::GetFrameCounter();
	const bool movementInput = pad->GetPedWalkLeftRight() != 0 ||
		pad->GetPedWalkUpDown() != 0;
	if(!gVrAboutStepBaselineValid){
		gVrAboutStepBaseline = position;
		gVrAboutStepPrevious = position;
		gVrAboutStepBaselineValid = true;
		gVrAboutStepControllableFrames = 1;
		gVrAboutStepLastGameFrame = gameFrame;
		gVrAboutStepMovementInputSeen = movementInput;
		return false;
	}
	if(gameFrame-gVrAboutStepLastGameFrame != 1U){
		// SubmitStereoFrame is not called by every cinema/loading path. A gap in
		// game frames therefore invalidates the pre-cutscene movement baseline.
		gVrAboutStepBaseline = position;
		gVrAboutStepPrevious = position;
		gVrAboutStepControllableFrames = 1;
		gVrAboutStepLastGameFrame = gameFrame;
		gVrAboutStepMovementInputSeen = movementInput;
		return false;
	}

	const float frameX = position.x-gVrAboutStepPrevious.x;
	const float frameY = position.y-gVrAboutStepPrevious.y;
	const float frameZ = position.z-gVrAboutStepPrevious.z;
	// A scripted relocation between gameplay states is not Tommy's first step.
	// Start over at the destination and wait for real on-foot movement there.
	if(frameX*frameX+frameY*frameY > 4.0f || Abs(frameZ) > 2.0f){
		gVrAboutStepBaseline = position;
		gVrAboutStepPrevious = position;
		gVrAboutStepControllableFrames = 1;
		gVrAboutStepLastGameFrame = gameFrame;
		gVrAboutStepMovementInputSeen = movementInput;
		return false;
	}

	gVrAboutStepPrevious = position;
	gVrAboutStepControllableFrames++;
	gVrAboutStepLastGameFrame = gameFrame;
	gVrAboutStepMovementInputSeen =
		gVrAboutStepMovementInputSeen || movementInput;
	const float movedX = position.x-gVrAboutStepBaseline.x;
	const float movedY = position.y-gVrAboutStepBaseline.y;
	return gVrAboutStepControllableFrames >= 2 &&
		gVrAboutStepMovementInputSeen &&
		movedX*movedX+movedY*movedY >= 0.0025f;
}

void ResetPhysicalWeaponStateForGameplayLoss(const float *grips)
{
	ClearAttachedMissionWeaponState(grips);
	for(int hand = 0; hand < EYE_COUNT; hand++){
		ClearWeaponSupportForHand(hand);
		gHeldWeaponSlot[hand] = -1;
		gWeaponHolsterSelection[hand] = -1;
		gTrackedWeaponRenderMatrixSlot[hand] = -1;
		InvalidatePhysicalMeleeWeaponMatrix(hand);
		ClearDroppedWeapon(hand);
		ResetManualReloadState(gManualReload[hand]);
		gManualReloadGripDown[hand] = grips && grips[hand] >= 0.45f;
		// Preserve the physical edge latch. A grip still held through the death
		// screen must be released once before it can grab a hospital holster.
		gWeaponHolsterGripDown[hand] = grips && grips[hand] >= 0.45f;
		gTrackedWeaponTriggerPressed[hand] = false;
		gTrackedWeaponTriggerJustPressed[hand] = false;
		gTrackedWeaponTriggerJustReleased[hand] = false;
		gTrackedThrowablePreviewActive[hand] = false;
		gTrackedAimCacheValid[hand] = false;
		ResetPhysicalMeleeMotion(hand);
	}
	gWeaponHolsterMask = 0;
	gActiveTrackedFireAimValid = false;
	gTrackedScopeReticleTargetValid = false;
	// A cutscene or world-state transition can temporarily suspend physical
	// gameplay without removing planted C4. Preserve the remembered off-hand;
	// the authoritative projectile query decides whether it should reappear.
	ResetTrackedDetonatorInteraction(false);
	ResetImmersiveDrivingInteraction();
}

bool IsHandBusyWithReload(int hand)
{
	for(int weaponHand = 0; weaponHand < EYE_COUNT; weaponHand++)
		if(gManualReload[weaponHand].active &&
		   gManualReload[weaponHand].magazineHand == hand)
			return true;
	return false;
}

int GetTrackedRemoteGrenadeHandInternal()
{
	for(int hand = 0; hand < EYE_COUNT; hand++)
		if(gHeldWeaponSlot[hand] == WEAPONSLOT_PROJECTILE &&
		   GetVrWeaponTypeForSlot(WEAPONSLOT_PROJECTILE) ==
			WEAPONTYPE_DETONATOR_GRENADE)
			return hand;
	return -1;
}

bool HasTrackedRemoteChargesInternal()
{
	CPlayerPed *player = FindPlayerPed();
	return player && CProjectileInfo::HasDetonatorProjectile(player);
}

bool IsTrackedDetonatorHandReservedInternal(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT || !gVrHandsEnabled ||
	   !gSessionRunning || FindPlayerVehicle() != nil ||
	   !IsPhysicalGameplayAvailable())
		return false;
	const int grenadeHand = GetTrackedRemoteGrenadeHandInternal();
	if(grenadeHand < 0 && !HasTrackedRemoteChargesInternal())
		return false;
	const int desiredHand = grenadeHand >= 0 ? 1-grenadeHand :
		gTrackedDetonatorHand;
	return desiredHand == hand && gHeldWeaponSlot[hand] < 0 &&
		!IsWeaponSupportHandInternal(hand) && !IsHandBusyWithReload(hand);
}

bool IsTrackedDetonatorActiveInternal(int hand)
{
	return IsTrackedDetonatorHandReservedInternal(hand) &&
		gTrackedHandPoseValid[hand] && gTrackedHandAimPoseValid[hand];
}

uint32 GetTrackedDetonatorHandMaskInternal()
{
	uint32 mask = 0;
	for(int hand = 0; hand < EYE_COUNT; hand++)
		if(IsTrackedDetonatorHandReservedInternal(hand))
			mask |= 1u << hand;
	return mask;
}

void ResetTrackedDetonatorInteraction(bool clearCharges)
{
	if(clearCharges){
		gTrackedDetonatorHand = -1;
	}
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gTrackedDetonatorWasActive[hand] = false;
		gTrackedDetonatorWaitForTriggerRelease[hand] = false;
		gTrackedDetonatorTriggerJustPressed[hand] = false;
	}
}

void UpdateTrackedDetonatorTriggerInput(const float *triggers, bool blocked)
{
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool active = IsTrackedDetonatorActiveInternal(hand);
		gTrackedDetonatorTriggerJustPressed[hand] = false;
		if(!active){
			gTrackedDetonatorWasActive[hand] = false;
			gTrackedDetonatorWaitForTriggerRelease[hand] = false;
			continue;
		}
		const float trigger = triggers ? triggers[hand] : 0.0f;
		if(!gTrackedDetonatorWasActive[hand])
			gTrackedDetonatorWaitForTriggerRelease[hand] = trigger > 0.30f;
		gTrackedDetonatorWasActive[hand] = true;
		if(blocked){
			if(trigger > 0.30f)
				gTrackedDetonatorWaitForTriggerRelease[hand] = true;
			continue;
		}
		if(gTrackedDetonatorWaitForTriggerRelease[hand]){
			if(trigger <= 0.30f)
				gTrackedDetonatorWaitForTriggerRelease[hand] = false;
			continue;
		}
		gTrackedDetonatorTriggerJustPressed[hand] =
			gTrackedWeaponTriggerJustPressed[hand];
	}
}

float GetPhysicalMeleeReach(int weaponType)
{
	if(weaponType == WEAPONTYPE_UNARMED ||
	   weaponType == WEAPONTYPE_BRASSKNUCKLE)
		return 0.11f;
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo((eWeaponType)weaponType);
	if(!info)
		return 0.45f;
	// weapon.dat's forward fire offset is the authored contact reach (for
	// example 0.8 m for the bat and 1.2 m for the katana).
	return Max(0.18f, Abs(info->m_vecFireOffset.y));
}

CVector GetPhysicalMeleeModelTip(int weaponType)
{
	// Measured from the shipped DFF bounds. Vice City's hand-held melee models
	// extend along local +Z (the final RenderWare matrix's Up axis), not along
	// weapon.dat's animated-ped fire-offset axis. This makes contact follow the
	// visible calibrated blade/club exactly.
	switch(weaponType){
	case WEAPONTYPE_SCREWDRIVER: return CVector(0.160f, -0.017f, 0.277f);
	case WEAPONTYPE_GOLFCLUB: return CVector(0.029f, 0.0f, 0.848f);
	case WEAPONTYPE_NIGHTSTICK: return CVector(0.050f, 0.030f, 0.451f);
	case WEAPONTYPE_KNIFE: return CVector(0.133f, 0.040f, 0.416f);
	case WEAPONTYPE_BASEBALLBAT: return CVector(0.068f, -0.019f, 0.680f);
	case WEAPONTYPE_HAMMER: return CVector(0.045f, 0.036f, 0.317f);
	case WEAPONTYPE_CLEAVER: return CVector(0.075f, 0.036f, 0.270f);
	case WEAPONTYPE_MACHETE: return CVector(0.079f, 0.023f, 0.572f);
	// Authored katana.dff tip.  The whole blade is offset from the model origin;
	// tracing local +Z made the physical edge sit visibly below the rendered one.
	case WEAPONTYPE_KATANA: return CVector(0.0468f, -0.0246f, 0.9847f);
	case WEAPONTYPE_CHAINSAW: return CVector(0.865f, 0.033f, 0.335f);
	default: return CVector(0.0f, 0.0f, GetPhysicalMeleeReach(weaponType));
	}
}

CVector GetPhysicalMeleeModelRoot(int weaponType)
{
	// Start immediately beyond each authored handle/guard. The DFF meshes are
	// offset several centimetres from their frame origins, so a generic zero root
	// makes the collision capsule visibly miss the bat, blade or chainsaw bar.
	switch(weaponType){
	case WEAPONTYPE_SCREWDRIVER: return CVector(0.078f, 0.027f, 0.0f);
	case WEAPONTYPE_GOLFCLUB: return CVector(0.064f, 0.035f, 0.0f);
	case WEAPONTYPE_NIGHTSTICK: return CVector(0.081f, 0.030f, 0.0f);
	case WEAPONTYPE_KNIFE: return CVector(0.089f, 0.038f, 0.0f);
	case WEAPONTYPE_BASEBALLBAT: return CVector(0.066f, 0.035f, 0.0f);
	case WEAPONTYPE_HAMMER: return CVector(0.078f, 0.035f, 0.0f);
	case WEAPONTYPE_CLEAVER: return CVector(0.073f, 0.036f, 0.0f);
	case WEAPONTYPE_MACHETE: return CVector(0.073f, 0.018f, 0.0f);
	case WEAPONTYPE_KATANA: return CVector(0.0725f, 0.0245f, 0.0467f);
	case WEAPONTYPE_CHAINSAW: return CVector(0.485f, 0.038f, 0.160f);
	default: return CVector(0.0f, 0.0f, 0.0f);
	}
}

CVector ToPhysicalMeleeTrackingSpace(const CVector &worldPoint)
{
	CVector right = gBaseCamera.GetRight();
	CVector forward = gBaseCamera.GetForward();
	CVector up = gBaseCamera.GetUp();
	right.Normalise();
	forward.Normalise();
	up.Normalise();
	const CVector relative = worldPoint-gBaseCamera.GetPosition();
	return CVector(DotProduct(relative, right), DotProduct(relative, forward),
		DotProduct(relative, up));
}

void UpdatePhysicalMeleeInput(uint32 blockedHands)
{
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	const uint32 frame = CTimer::GetFrameCounter();
	const float dt = Max(0.001f, CTimer::GetTimeStepNonClippedInSeconds());
	const bool playerUnavailable = !IsPhysicalGameplayAvailable();
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gPhysicalMeleeStrike[hand].pending &&
		   frame-gPhysicalMeleeStrike[hand].frame > 1U)
			gPhysicalMeleeStrike[hand].pending = false;
		if(playerUnavailable || (blockedHands & (1u << hand)) != 0 ||
		   gVrMenuVisible || gCheatMenuVisible ||
		   FindPlayerVehicle() != nil || !gVrHandsEnabled ||
		   !gTrackedHandPoseValid[hand] ||
		   CTimer::GetIsPaused() || CGame::playingIntro ||
		   CCutsceneMgr::IsRunning() || CCutsceneMgr::IsCutsceneProcessing() ||
		   IsWeaponSupportHandInternal(hand) ||
		   IsHandBusyWithReload(hand)){
			ResetPhysicalMeleeMotion(hand);
			continue;
		}

		int slot = gHeldWeaponSlot[hand];
		int weaponType = -1;
		bool strikeEnabled = false;
		if(slot >= 0){
			weaponType = GetVrWeaponTypeForSlot(slot);
			strikeEnabled = IsPhysicalMeleeTypeInternal(weaponType);
		}else{
			slot = WEAPONSLOT_UNARMED;
			weaponType = GetVrWeaponTypeForSlot(slot);
			// A real fist closes both the three grip fingers and the index finger.
			strikeEnabled = IsPhysicalMeleeTypeInternal(weaponType) &&
				gTrackedHandGrip[hand] >= 0.65f &&
				gTrackedHandTrigger[hand] >= 0.45f;
		}
		if(!IsPhysicalMeleeTypeInternal(weaponType)){
			ResetPhysicalMeleeMotion(hand);
			continue;
		}

		const XrPosef &pose = gTrackedHandAimPoseValid[hand] ?
			gTrackedHandAimPose[hand] : gTrackedHandPose[hand];
		const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
		XrVector3f forward = Rotate(pose.orientation, localForward);
		const float reach = GetPhysicalMeleeReach(weaponType);
		const XrVector3f reachVector = ScaleXrVector(forward, reach);
		const XrVector3f fallbackPoint = AddXrVector(pose.position, reachVector);
		const CVector basePosition = gBaseCamera.GetPosition();
		// Keep controller/handle motion separate from the authored damaging blade.
		// The former drives gesture velocity; the latter is stored in the world
		// sweep so calibration/model offsets cannot move collision off the mesh.
		CVector worldMotionRoot = basePosition+ToGameVector(pose.position);
		CVector worldBladeRoot = worldMotionRoot;
		CVector worldPoint = basePosition+ToGameVector(fallbackPoint);
		// The renderer exposes the exact final matrix after hand calibration,
		// authored model-axis conversion and the optional two-hand transform.
		// Using its authored fire offset keeps the physical blade/bat tip attached
		// to the visible model instead of an uncalibrated controller ray.
		bool usedContactMatrix = false;
		if(slot != WEAPONSLOT_UNARMED &&
		   gTrackedWeaponContactMatrixSlot[hand] == slot &&
		   gTrackedWeaponContactMatrixType[hand] == weaponType &&
		   frame-gTrackedWeaponContactMatrixFrame[hand] <= 2U){
			worldMotionRoot = gTrackedWeaponContactMatrix[hand].GetPosition();
			worldBladeRoot = gTrackedWeaponContactMatrix[hand]*
				GetPhysicalMeleeModelRoot(weaponType);
			worldPoint = gTrackedWeaponContactMatrix[hand]*
				GetPhysicalMeleeModelTip(weaponType);
			usedContactMatrix = true;
		}
		// Keep a second copy relative to the gameplay camera origin. The world
		// positions are required for collision, but include Tommy's locomotion and
		// walking bob; those must never count as a physical controller swing.
		const CVector trackingRoot = ToPhysicalMeleeTrackingSpace(worldMotionRoot);
		const CVector trackingPoint = ToPhysicalMeleeTrackingSpace(worldPoint);
		const int supportHand = slot == WEAPONSLOT_UNARMED ? -1 :
			gWeaponSupportHand[hand];

		PhysicalMeleeMotion &motion = gPhysicalMeleeMotion[hand];
		if(!motion.valid || motion.slot != slot ||
		   motion.weaponType != weaponType ||
		   motion.usedContactMatrix != usedContactMatrix ||
		   motion.supportHand != supportHand ||
		   frame-motion.previousFrame > 2U || dt > 0.05f){
			const bool armFreshGrab = gPhysicalMeleeFreshGrab[hand] &&
				usedContactMatrix;
			motion = PhysicalMeleeMotion();
			motion.valid = true;
			motion.slot = slot;
			motion.weaponType = weaponType;
			motion.usedContactMatrix = usedContactMatrix;
			motion.supportHand = supportHand;
			motion.previousFrame = frame;
			motion.calmSinceTime = now;
			motion.previousWorldPoint = worldPoint;
			motion.previousWorldRoot = worldBladeRoot;
			motion.previousTrackingPoint = trackingPoint;
			motion.previousTrackingRoot = trackingRoot;
			motion.armed = armFreshGrab;
			continue;
		}

		const CVector trackingTravelled =
			trackingPoint-motion.previousTrackingPoint;
		const float trackingTravelDistance = trackingTravelled.Magnitude();
		const float speed = trackingTravelDistance/dt;
		const CVector trackingRootTravelled =
			trackingRoot-motion.previousTrackingRoot;
		const float rootSpeed = trackingRootTravelled.Magnitude()/dt;
		const CVector previousBlade = motion.previousTrackingPoint-
			motion.previousTrackingRoot;
		const CVector currentBlade = trackingPoint-trackingRoot;
		const float angularTipSpeed = (currentBlade-previousBlade).Magnitude()/dt;
		const float strikeSpeed = slot == WEAPONSLOT_UNARMED ? 1.25f : 0.70f;
		const bool calmForRearm = slot == WEAPONSLOT_UNARMED ?
			speed <= 0.48f : speed <= 0.40f && rootSpeed <= 0.30f;
		if(calmForRearm){
			if(motion.calmSinceTime == 0)
				motion.calmSinceTime = now;
			if(slot == WEAPONSLOT_UNARMED ||
			   now-motion.calmSinceTime >= 70U)
				motion.armed = true;
		}else{
			motion.calmSinceTime = 0;
		}
		// A collision (especially against a moving vehicle) can leave the tracked
		// tip above the calm threshold indefinitely because of tiny controller and
		// contact-matrix jitter. Never allow one resolved hit to permanently lock
		// melee input. Player locomotion has already been removed from `speed`, so
		// this fallback cannot bring back passive hits while merely walking.
		if(!motion.armed && motion.lastStrikeTime != 0 &&
		   now-motion.lastStrikeTime >= 450U){
			motion.armed = true;
			motion.calmSinceTime = 0;
		}

		// Large discontinuities are tracking/recenter events, never superhuman
		// attacks. Keep the new sample but require a calm frame before rearming.
		if(trackingTravelDistance > 0.65f){
			motion.armed = false;
			motion.previousWorldPoint = worldPoint;
			motion.previousWorldRoot = worldBladeRoot;
			motion.previousTrackingPoint = trackingPoint;
			motion.previousTrackingRoot = trackingRoot;
			motion.previousFrame = frame;
			continue;
		}

		// Require motion produced by the tracked hand or by deliberate blade
		// rotation. Merely walking Tommy (or an NPC walking into a stationary
		// weapon) changes world coordinates but cannot satisfy this local gesture.
		const bool deliberateMotion = slot == WEAPONSLOT_UNARMED ?
			rootSpeed >= 0.75f : rootSpeed >= 0.25f || angularTipSpeed >= 0.50f;
		if(motion.strikeInProgress && now > motion.strikeContinueUntil){
			motion.strikeInProgress = false;
			motion.strikePeakSpeed = 0.0f;
		}
		const bool fastStrikeSample = strikeEnabled && motion.armed &&
			speed >= strikeSpeed && deliberateMotion &&
			now-motion.lastStrikeTime >= 220U;
		if(fastStrikeSample && weaponType == WEAPONTYPE_KATANA){
			motion.strikeInProgress = true;
			motion.strikePeakSpeed = Max(motion.strikePeakSpeed, speed);
			motion.strikeContinueUntil = now+180U;
		}
		// Once a genuine fast swing has started, keep publishing its slower tail.
		// The broad torso hull can be entered first while the visible edge reaches
		// the head a few frames later; head classification must see those frames.
		const bool continuationSample = weaponType == WEAPONTYPE_KATANA &&
			motion.strikeInProgress &&
			now <= motion.strikeContinueUntil && speed >= 0.08f;
		if(strikeEnabled && motion.armed &&
		   (fastStrikeSample || continuationSample) &&
		   !gPhysicalMeleeStrike[hand].pending){
			PhysicalMeleeStrike &strike = gPhysicalMeleeStrike[hand];
			strike.pending = true;
			strike.slot = slot;
			strike.weaponType = weaponType;
			strike.sweepStart = motion.previousWorldPoint;
			strike.sweepEnd = worldPoint;
			strike.rootStart = motion.previousWorldRoot;
			strike.rootEnd = worldBladeRoot;
			strike.speed = Max(speed, motion.strikePeakSpeed);
			strike.frame = frame;
		}
		motion.previousWorldPoint = worldPoint;
		motion.previousWorldRoot = worldBladeRoot;
		motion.previousTrackingPoint = trackingPoint;
		motion.previousTrackingRoot = trackingRoot;
		motion.previousFrame = frame;
		if(gPhysicalMeleeFreshGrab[hand] && usedContactMatrix)
			gPhysicalMeleeFreshGrab[hand] = false;
	}
}

void ResetTeleportInteraction()
{
	gTeleportInputDown = false;
	gTeleportPreviewActive = false;
	gTeleportTargetValid = false;
	gTeleportTrajectoryCount = 0;
}

bool BuildTeleportTrajectory()
{
	CPlayerPed *player = FindPlayerPed();
	if(!player)
		return false;
	// Teleport is deliberately a one-control comfort action.  Its destination
	// is a short step in Tommy's current forward direction; no controller ray
	// or hand pose is required.
	CVector direction = player->GetForward();
	direction.z = 0.0f;
	if(direction.MagnitudeSqr() < 0.0001f)
		return false;
	direction.Normalise();
	const CVector playerPosition = player->GetPosition();
	const CVector source =
		playerPosition+CVector(0.0f, 0.0f, 0.75f);
	CVector destination = playerPosition+direction*4.0f;
	gTeleportTargetValid = false;
	CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
	CWorld::pIgnoreEntity = player;

	// Stop safely before a wall, vehicle or pedestrian instead of placing the
	// player inside the first obstruction.
	CColPoint forwardHit;
	CEntity *forwardEntity = nil;
	const CVector forwardEnd =
		destination+CVector(0.0f, 0.0f, 0.75f);
	if(CWorld::ProcessLineOfSight(source, forwardEnd, forwardHit,
	   forwardEntity, true, true, true, true, true, false, false, false))
		destination = forwardHit.point-direction*0.55f;

	// Snap the horizontal destination to walkable ground.  This also rejects
	// ledges and water/empty space instead of teleporting Tommy into a fall.
	CColPoint groundHit;
	CEntity *groundEntity = nil;
	const CVector groundStart =
		destination+CVector(0.0f, 0.0f, 1.75f);
	if(CWorld::ProcessVerticalLine(groundStart, destination.z-4.0f,
	   groundHit, groundEntity, true, false, false, true, true, false,
	   nil)){
		const float distance =
			(groundHit.point-playerPosition).Magnitude2D();
		gTeleportTargetValid =
			groundHit.normal.z >= 0.55f && distance >= 0.45f;
		if(gTeleportTargetValid)
			gTeleportTarget =
				groundHit.point+CVector(0.0f, 0.0f, FEET_OFFSET);
	}
	CWorld::pIgnoreEntity = savedIgnoreEntity;

	// Draw a simple fixed arc to make the pending step obvious.  The endpoint
	// turns red when no safe ground was found.
	const CVector visualEnd = gTeleportTargetValid ?
		gTeleportTarget : destination;
	gTeleportTrajectoryCount = VR_TELEPORT_MAX_POINTS;
	for(int index = 0; index < gTeleportTrajectoryCount; index++){
		const float t = (float)index/
			(float)(gTeleportTrajectoryCount-1);
		CVector point = source+(visualEnd-source)*t;
		point.z += 0.85f*4.0f*t*(1.0f-t);
		gTeleportTrajectory[index] = point;
	}
	return true;
}

bool UpdateTeleportInput(const XrVector2f &leftStick)
{
	const bool environmentReady =
		gMovementMode == VR_MOVEMENT_TELEPORT &&
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!TheCamera.m_WideScreenOn &&
		FindPlayerPed() && !FindPlayerVehicle() &&
		!CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	if(!environmentReady){
		ResetTeleportInteraction();
		return false;
	}
	const bool pressed = leftStick.y >= 0.55f;
	if(pressed){
		gTeleportPreviewActive = BuildTeleportTrajectory();
		gTeleportInputDown = true;
		return true;
	}
	if(gTeleportInputDown){
		if(gTeleportPreviewActive && gTeleportTargetValid){
			CPlayerPed *player = FindPlayerPed();
			if(player){
				player->Teleport(gTeleportTarget);
				gTrackingCenterValid = false;
				ResetTemporalAaHistory();
			}
		}
		ResetTeleportInteraction();
		return true;
	}
	gTeleportPreviewActive = false;
	gTeleportTargetValid = false;
	gTeleportTrajectoryCount = 0;
	return false;
}

void ApplySnapTurn(float stickX)
{
	if(gTurnMode != VR_TURN_SNAP || FindPlayerVehicle() ||
	   CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() ||
	   gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn){
		gSnapTurnStickLatched = fabsf(stickX) >= 0.35f;
		return;
	}
	if(fabsf(stickX) <= 0.35f){
		gSnapTurnStickLatched = false;
		return;
	}
	if(gSnapTurnStickLatched || fabsf(stickX) < 0.70f)
		return;
	gSnapTurnStickLatched = true;
	const float delta = -copysignf(DEGTORAD(
		(float)gSnapTurnAngleDegrees), stickX);
	CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
	cam.Beta += delta;
	cam.m_fTrueBeta += delta;
	cam.m_fTargetBeta += delta;
	while(cam.Beta >= PI) cam.Beta -= TWOPI;
	while(cam.Beta < -PI) cam.Beta += TWOPI;
	while(cam.m_fTrueBeta >= PI) cam.m_fTrueBeta -= TWOPI;
	while(cam.m_fTrueBeta < -PI) cam.m_fTrueBeta += TWOPI;
	while(cam.m_fTargetBeta >= PI) cam.m_fTargetBeta -= TWOPI;
	while(cam.m_fTargetBeta < -PI) cam.m_fTargetBeta += TWOPI;
	CPlayerPed *player = FindPlayerPed();
	if(player){
		player->m_fRotationCur += delta;
		player->m_fRotationDest += delta;
		player->SetHeading(player->m_fRotationCur);
	}
	ResetTemporalAaHistory();
}

float GetHeadSteeringAxis(const XrVector2f &moveStick)
{
	// HEAD DIRECTED keeps the useful head-aligned body/movement behavior but
	// deliberately has no assisted camera rotation. Only the legacy experimental
	// mode turns head yaw into a continuous right-stick axis.
	if(gMovementOrientation !=
	   VR_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL ||
	   !IsExperimentalHeadTurningActive())
		return 0.0f;

	// Head steering is active only while the player is deliberately moving.
	// A small controller drift must never rotate the virtual world by itself.
	if(moveStick.x*moveStick.x+moveStick.y*moveStick.y < 0.20f*0.20f)
		return 0.0f;

	// Treat head yaw as a turn-rate control rather than a finite direction
	// offset. Holding the head slightly to one side keeps turning the gameplay
	// camera, so a seated player can rotate through 180 degrees and then return
	// the neck to centre. A dedicated saved sensitivity keeps this independent
	// from ordinary smooth-stick turning.
	const float deadZone = DEGTORAD(10.0f);
	const float fullSpeedAngle = DEGTORAD(55.0f);
	const float absoluteYaw = fabsf(gHeadLocomotionYaw);
	if(absoluteYaw <= deadZone)
		return 0.0f;

	float axis = clamp((absoluteYaw-deadZone)/
		(fullSpeedAngle-deadZone), 0.0f, 1.0f);
	// Smoothstep keeps the transition out of the dead zone gentle while still
	// reaching full controller turn speed at a comfortable head angle.
	axis = axis*axis*(3.0f-2.0f*axis);
	// OpenXR's rightward head yaw is negative in our flattened convention,
	// while Vice City's positive right-stick axis turns the camera right.
	return copysignf(axis, -gHeadLocomotionYaw);
}

bool ApplyTouchInput(CControllerState *state)
{
	// This is the once-per-gameplay-frame input entry point. Keep calibration
	// ownership valid even if the menu swapchain is temporarily unavailable.
	ValidateHolsterCalibrationLifecycle();
	// Flat mode: V toggles first person <-> classic third person. Lives here
	// because this runs once per frame with or without a VR session.
	if(gFlatModeEnabled){
		const bool vDown = (GetAsyncKeyState('V') & 0x8000) != 0;
		if(vDown && !gFlatViewKeyWasDown){
			gFlatFirstPersonEnabled = !gFlatFirstPersonEnabled;
			VrLog("Flat view toggled: %s\n",
				gFlatFirstPersonEnabled ? "first person" : "third person");
		}
		gFlatViewKeyWasDown = vDown;
	}
	if(!state || !gSession || !gSessionRunning || !PollEvents()) return false;
	XrActiveActionSet active = { gActions.set, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets=1; sync.activeActionSets=&active;
	if(XR_FAILED(xrSyncActions(gSession, &sync))) return false;
	XrVector2f sticks[EYE_COUNT] = {};
	const bool leftActive = ReadVector(gActions.stick, 0, sticks[0]);
	const bool rightActive = ReadVector(gActions.stick, 1, sticks[1]);
	const bool connected = leftActive || rightActive;
	const bool connectionChanged = connected != gTouchWasConnected;
	if(connectionChanged){
		debug("[OpenXR] Touch controllers %s\n", connected ? "connected" : "disconnected");
		if(connected) debug("[OpenXR] Shortcuts: grips+Menu or grips+both triggers+X VR settings, grips+Y profiler, grips+A debug, grips+B DLSS5 baseline compare (cheats when NR disabled), grips+right stick left/right weapons, grips+left stick click+left trigger SPS mode, grips+right stick click+right trigger VRS, grips+both sticks recenter\n");
		gTouchWasConnected = connected;
	}
	if(!connected){
		ClearAttachedMissionWeaponState(nil);
		for(int hand = 0; hand < EYE_COUNT; hand++){
			ClearWeaponSupportForHand(hand);
			gTrackedWeaponTriggerPressed[hand]=false;
			gTrackedWeaponTriggerJustPressed[hand]=false;
			gTrackedWeaponTriggerJustReleased[hand]=false;
			gTrackedThrowablePreviewActive[hand]=false;
			gHeldWeaponSlot[hand]=-1;
			gTrackedWeaponRenderMatrixSlot[hand]=-1;
			ClearDroppedWeapon(hand);
			ResetManualReloadState(gManualReload[hand]);
			gManualReloadGripDown[hand]=false;
			ResetPhysicalMeleeMotion(hand);
		}
		// Preserve already planted charges across a transient controller loss, but
		// require a fresh R2 press after tracking returns.
		ResetTrackedDetonatorInteraction(false);
		ResetImmersiveDrivingInteraction();
		return false;
	}
	const float leftGrip=ReadFloat(gActions.squeeze,0), rightGrip=ReadFloat(gActions.squeeze,1);
	const float leftTrigger=ReadFloat(gActions.trigger,0), rightTrigger=ReadFloat(gActions.trigger,1);
	gTrackedHandGrip[0]=leftGrip; gTrackedHandGrip[1]=rightGrip;
	gTrackedHandTrigger[0]=leftTrigger; gTrackedHandTrigger[1]=rightTrigger;
	const float physicalGrips[EYE_COUNT] = { leftGrip, rightGrip };
	const bool physicalGameplayAvailable = IsPhysicalGameplayAvailable();
	if(!physicalGameplayAvailable){
		if(gPhysicalGameplayWasAvailable)
			ResetPhysicalWeaponStateForGameplayLoss(physicalGrips);
		else{
			// Keep latches synchronized throughout the death screen so a released
			// grip is ready for a deliberate new squeeze after respawn.
			for(int hand = 0; hand < EYE_COUNT; hand++)
				gWeaponHolsterGripDown[hand] = physicalGrips[hand] >= 0.45f;
		}
	}
	gPhysicalGameplayWasAvailable = physicalGameplayAvailable;
	const bool a=ReadBool(gActions.a), b=ReadBool(gActions.b);
	const bool x=ReadBool(gActions.x), y=ReadBool(gActions.y);
	const bool leftStickClick=ReadBool(gActions.stickClick,0);
	const bool rightStickClick=ReadBool(gActions.stickClick,1);
	const bool menu=ReadBool(gActions.menu);
	const bool aboutInputDown = a || b || x || y || menu ||
		leftStickClick || rightStickClick ||
		leftGrip >= 0.55f || rightGrip >= 0.55f ||
		leftTrigger >= 0.55f || rightTrigger >= 0.55f;
	if((gVrAboutVisible && gVrAboutWasRendered) || gVrAboutReleaseGate){
		// Opening ABOUT uses one of these same buttons. Require one completely
		// neutral action-sync before arming dismissal, then keep consuming input
		// until the closing button is released. This prevents both an immediate
		// close from a startup-held control and a close press leaking into gameplay.
		if(gVrAboutVisible){
			if(!aboutInputDown)
				gVrAboutDismissArmed = true;
			else if(gVrAboutDismissArmed && gVrAboutWasRendered){
				gVrAboutVisible = false;
				gVrAboutDismissArmed = false;
				gVrAboutReleaseGate = true;
				if(gVrAboutFirstRun){
					SaveVrSetting("WelcomeShown", 1);
					gVrAboutFirstRun = false;
				}
			}
		}else if(!aboutInputDown)
			gVrAboutReleaseGate = false;

		for(int hand = 0; hand < EYE_COUNT; hand++){
			gTrackedWeaponTriggerPressed[hand] = false;
			gTrackedWeaponTriggerJustPressed[hand] = false;
			gTrackedWeaponTriggerJustReleased[hand] = false;
			gTrackedThrowablePreviewActive[hand] = false;
		}
		gVrMenuVerticalDown = fabsf(sticks[0].y) >= 0.65f;
		gVrMenuHorizontalDown = fabsf(sticks[0].x) >= 0.65f;
		gVrMenuSelectDown = a || rightStickClick;
		gVrMenuBackDown = b || leftStickClick;
		gVrMenuDecreaseDown = leftTrigger >= 0.55f;
		gVrMenuIncreaseDown = rightTrigger >= 0.55f;
		gVrMenuDecreaseRepeatAt = gVrMenuIncreaseRepeatAt = 0;
		gVrMenuDecreaseHoldStartedAt = gVrMenuIncreaseHoldStartedAt = 0;
		return true;
	}
	// Physical grabs win over a plain two-grip squeeze, so two holsters can be
	// grabbed simultaneously. An explicit service button still wins, preserving
	// access to the VR/debug menus even when both hands happen to be near slots.
	// Trigger-only two-grip chords conflict with making two real fists and with
	// punching while the other hand holds a weapon. Keep service shortcuts
	// explicit: trigger diagnostics now also require their matching stick click.
	const bool bothGrips = leftGrip >= 0.75f && rightGrip >= 0.75f;
	const bool alternateVrMenuShortcut = bothGrips &&
		leftTrigger >= 0.75f && rightTrigger >= 0.75f &&
		(x || leftStickClick);
	CVehicle *touchVehicle = FindPlayerVehicle();
	// In Immersive/Motion driving a physically held weapon owns B even while
	// the other hand keeps a grip on the wheel.  Without this exception the
	// ordinary both-grips+B service chord swallowed every shot until the wheel
	// hand was released.
	const bool physicalVehicleWeaponAvailableForButton =
		touchVehicle != nil && IsVrDrivingActiveInternal(touchVehicle) &&
		physicalGameplayAvailable && !gVrMenuVisible && !gCheatMenuVisible &&
		(gHeldWeaponSlot[0] >= 0 || gHeldWeaponSlot[1] >= 0);
	const bool serviceChord = bothGrips &&
		(menu || a || (b && !physicalVehicleWeaponAvailableForButton) || y ||
		 alternateVrMenuShortcut ||
		 (leftStickClick && rightStickClick) ||
		 (leftStickClick && leftTrigger >= 0.75f) ||
		 (rightStickClick && rightTrigger >= 0.75f));
	const bool remoteVehicleActive =
		CWorld::Players[CWorld::PlayerInFocus].m_pRemoteVehicle != nil;
	// Default driving keeps the original Vice City drive-by implementation
	// (animations, ammo, firing rate and mission restrictions), but exposes it
	// through a chord which does not steal throttle, brake, radio or exit.
	// Hold B and one grip to choose a side; B by itself fires forward on bikes.
	// The both-grips+B service chord retains priority when no physical weapon is
	// being held; a held weapon uses B for fire instead.
	const bool defaultDriveByAvailable =
		touchVehicle != nil &&
		GetDrivingTypeForVehicle(touchVehicle) == VR_DRIVING_DEFAULT &&
		(physicalGameplayAvailable || IsVehicleThirdPersonActive()) && !serviceChord &&
		!gVrMenuVisible && !gCheatMenuVisible;
	const bool defaultDriveByLeft = defaultDriveByAvailable && b &&
		leftGrip >= 0.55f && rightGrip < 0.55f;
	const bool defaultDriveByRight = defaultDriveByAvailable && b &&
		rightGrip >= 0.55f && leftGrip < 0.55f;
	const bool defaultDriveByForward = defaultDriveByAvailable && b &&
		leftGrip < 0.55f && rightGrip < 0.55f &&
		touchVehicle->IsBike();
	const bool defaultDriveByFire = defaultDriveByLeft ||
		defaultDriveByRight || defaultDriveByForward;
	const bool vrRadioPressed =
		touchVehicle != nil &&
		!gVrMenuVisible && !gCheatMenuVisible &&
		!serviceChord && x;
	gVrRadioChangeJustPressed =
		vrRadioPressed && !gVrRadioButtonDown;
	gVrRadioButtonDown = vrRadioPressed;
	const uint32 allTrackedHands = (1u << EYE_COUNT)-1u;
	uint32 trackedDetonatorHands = GetTrackedDetonatorHandMaskInternal();
	uint32 vehicleCapturedHands = UpdateImmersiveBikeInput(physicalGrips,
		(serviceChord || gVrMenuVisible || gCheatMenuVisible) ?
			allTrackedHands : 0);
	vehicleCapturedHands |= UpdateImmersiveCarInput(physicalGrips,
		(serviceChord || gVrMenuVisible || gCheatMenuVisible) ?
			allTrackedHands : 0);
	UpdateMotionDrivingInput();
	uint32 reloadCapturedHands = 0;
	if(physicalGameplayAvailable && !gVrMenuVisible && !gCheatMenuVisible){
		reloadCapturedHands = UpdateManualReloadInput(
			leftGrip, rightGrip,
			(serviceChord ? allTrackedHands : trackedDetonatorHands) |
				vehicleCapturedHands);
		if(!serviceChord)
			UpdateWeaponHolsterInput(leftGrip, rightGrip,
				reloadCapturedHands | trackedDetonatorHands |
				vehicleCapturedHands);
	}
	// Grips are reserved for the fixed gun and its foregrip while attached to
	// the mission helicopter.  Never expose either squeeze as a legacy shoulder
	// button (L1 changes the radio in Vice City's vehicle control path).
	if(gAttachedMissionWeaponForced)
		vehicleCapturedHands |= allTrackedHands;
	// A grab can make a remote grenade (and therefore its opposite-hand
	// controller) active during UpdateWeaponHolsterInput.
	trackedDetonatorHands = GetTrackedDetonatorHandMaskInternal();
	const bool modifier=gHeldWeaponSlot[0] < 0 && gHeldWeaponSlot[1] < 0 &&
		trackedDetonatorHands == 0 &&
		leftGrip>=0.75f && rightGrip>=0.75f;
	// In Immersive and Motion driving the analogue triggers belong exclusively
	// to throttle and brake. Keep the physically held weapon and its tracked aim,
	// but fire it with the same B button used by Default drive-by controls.
	const bool vehicleWeaponButtonAvailable =
		physicalVehicleWeaponAvailableForButton && !serviceChord;
	bool vehicleWeaponButtonConsumed = false;
	const float triggers[EYE_COUNT] = { leftTrigger, rightTrigger };
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const bool useVehicleWeaponButton =
			vehicleWeaponButtonAvailable && gHeldWeaponSlot[hand] >= 0;
		vehicleWeaponButtonConsumed |= useVehicleWeaponButton;
		const bool weaponTriggerPressed = !serviceChord &&
			(useVehicleWeaponButton ? b :
			 (gTrackedWeaponTriggerPressed[hand] ? triggers[hand] >= 0.45f :
			  triggers[hand] >= 0.55f));
		gTrackedWeaponTriggerJustReleased[hand]=!weaponTriggerPressed &&
			gTrackedWeaponTriggerPressed[hand];
		gTrackedWeaponTriggerJustPressed[hand]=weaponTriggerPressed &&
			!gTrackedWeaponTriggerPressed[hand];
		gTrackedWeaponTriggerPressed[hand]=weaponTriggerPressed;
	}
	UpdatePhysicalMeleeInput(serviceChord ? allTrackedHands :
		(trackedDetonatorHands | vehicleCapturedHands));
	const bool legacyVrMenuShortcut=bothGrips && menu;
	const bool vrMenuShortcut=legacyVrMenuShortcut ||
		alternateVrMenuShortcut;
	const bool vrMenuToggled=vrMenuShortcut && !gTouchVrMenuShortcutDown;
	if(vrMenuToggled){
		gVrMenuVisible=!gVrMenuVisible;
		gVrGraphicsMenuVisible=false;
		gVrDlssTuningMenuVisible=false;
		gVrEffectsMenuVisible=false;
		gVrLightingMenuVisible=false;
		gVrRainMenuVisible=false;
		gVrReflectionMenuVisible=false;
		gVrModelMenuVisible=false;
		gVrTrafficMenuVisible=false;
		gVrVehicleMenuVisible=false;
		gVrHudMenuVisible=false;
		gVrHolsterMenuVisible=false;
		gVrCalibrationMenuVisible=false;
		gVrBikeCalibrationMenuVisible=false;
		gVrLocomotionMenuVisible=false;
		gCheatMenuVisible=false;
		gCheatMenuOpenedFromVrMenu=false;
		gVrMissionMenuVisible=false;
		gVrMissionCategory=-1;
		if(IsTrackedDetonatorHandReservedInternal(0))
			gCalibrationEditHand = 0;
		else if(IsTrackedDetonatorHandReservedInternal(1))
			gCalibrationEditHand = 1;
		else if(gHeldWeaponSlot[0] >= 0 && gHeldWeaponSlot[1] < 0)
			gCalibrationEditHand = 0;
		else if(gHeldWeaponSlot[1] >= 0 && gHeldWeaponSlot[0] < 0)
		gCalibrationEditHand = 1;
		gVrMenuSelection=0;
		gVrMenuVerticalDown=gVrMenuHorizontalDown=false;
		gVrMenuNavigation.Reset();
		gCheatMenuHorizontalNavigation.Reset();
		gVrMenuSelectDown=false;
		// L3 is also the universal menu-back fallback. If it was used in the
		// opening chord, latch it now so the menu does not close again in this
		// same input frame.
		gVrMenuBackDown=b || leftStickClick;
		// The fallback chord itself holds both triggers. Seed the menu latches
		// from their current state so opening the menu cannot immediately change
		// Render Scale from 100% to 125%.
		gVrMenuDecreaseDown=leftTrigger>=0.55f;
		gVrMenuIncreaseDown=rightTrigger>=0.55f;
		gVrMenuDecreaseRepeatAt=gVrMenuIncreaseRepeatAt=0;
		gVrMenuDecreaseHoldStartedAt=gVrMenuIncreaseHoldStartedAt=0;
		debug("[OpenXR] VR settings: %s\n",gVrMenuVisible?"open":"closed");
	}
	gTouchVrMenuShortcutDown=vrMenuShortcut;
	const bool cheatMenuShortcut=modifier && b;
	const bool cheatMenuToggled=cheatMenuShortcut && !gTouchWeatherShortcutDown;
	const bool neuralComparisonToggled = cheatMenuToggled &&
		Dlaa::IsNeuralRenderingEnabled();
	if(neuralComparisonToggled){
		const bool showModel = !Dlaa::IsNeuralRenderingOutputVisible();
		Dlaa::SetNeuralRenderingOutputVisible(showModel);
		SaveVrSetting("DLSSNeuralRenderingOutputVisible",
			showModel ? 1 : 0);
		ShowDlssProfileToast();
		debug("[OpenXR] DLSS 5 A/B: %s\n", showModel ?
			Dlaa::GetNeuralRenderingModeName() : "DLAA BASELINE");
	}else if(cheatMenuToggled){
		gCheatMenuVisible=!gCheatMenuVisible;
		gCheatMenuOpenedFromVrMenu=false;
		gVrMissionMenuVisible=false;
		gVrMissionCategory=-1;
		gVrMenuVisible=false;
		gVrGraphicsMenuVisible=false;
		gVrDlssTuningMenuVisible=false;
		gVrEffectsMenuVisible=false;
		gVrLightingMenuVisible=false;
		gVrRainMenuVisible=false;
		gVrReflectionMenuVisible=false;
		gVrModelMenuVisible=false;
		gVrTrafficMenuVisible=false;
		gVrVehicleMenuVisible=false;
		gVrHudMenuVisible=false;
		gVrHolsterMenuVisible=false;
		gVrCalibrationMenuVisible=false;
		gVrBikeCalibrationMenuVisible=false;
		gVrLocomotionMenuVisible=false;
		gCheatMenuSelection=0;
		gVrMenuVerticalDown=gVrMenuHorizontalDown=false;
		gVrMenuNavigation.Reset();
		gCheatMenuHorizontalNavigation.Reset();
		gVrMenuSelectDown=false; gVrMenuBackDown=b;
		gVrMenuDecreaseDown=gVrMenuIncreaseDown=false;
		gVrMenuDecreaseRepeatAt=gVrMenuIncreaseRepeatAt=0;
		gVrMenuDecreaseHoldStartedAt=gVrMenuIncreaseHoldStartedAt=0;
		debug("[OpenXR] Cheat menu: %s\n",gCheatMenuVisible?"open":"closed");
	}
	gTouchWeatherShortcutDown=cheatMenuShortcut;
	UpdateTrackedDetonatorTriggerInput(triggers,
		serviceChord || gVrMenuVisible || gCheatMenuVisible ||
		!physicalGameplayAvailable);
	if(gVrMenuVisible){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			gTrackedWeaponTriggerPressed[hand]=false;
			gTrackedWeaponTriggerJustPressed[hand]=false;
			gTrackedWeaponTriggerJustReleased[hand]=false;
			gTrackedThrowablePreviewActive[hand]=false;
		}
		HandleVrMenuInput(sticks[0], leftTrigger>=0.55f,
			rightTrigger>=0.55f, a || rightStickClick,
			b || leftStickClick);
		return true;
	}
	if(gCheatMenuVisible){
		for(int hand = 0; hand < EYE_COUNT; hand++){
			gTrackedWeaponTriggerPressed[hand]=false;
			gTrackedWeaponTriggerJustPressed[hand]=false;
			gTrackedWeaponTriggerJustReleased[hand]=false;
			gTrackedThrowablePreviewActive[hand]=false;
		}
		HandleCheatMenuInput(sticks[0], a || rightStickClick,
			b || leftStickClick);
		return true;
	}
	if(vrMenuToggled || cheatMenuToggled || neuralComparisonToggled)
		return true;
	const bool perfShortcut=modifier && y;
	const bool debugShortcut=modifier && a;
	const bool spsShortcut=modifier && leftStickClick && leftTrigger>=0.75f;
	bool vrsShortcut=false;
#ifdef RW_D3D12
	vrsShortcut=modifier && rightStickClick && rightTrigger>=0.75f;
#endif
	const bool recenterShortcut=modifier && leftStickClick && rightStickClick;
	const bool weaponStickHorizontal=fabsf(sticks[1].x)>=0.65f &&
		fabsf(sticks[1].x)>fabsf(sticks[1].y);
	const bool weaponLeftShortcut=!gVrHandsEnabled && modifier &&
		FindPlayerVehicle()==nil && !remoteVehicleActive &&
		weaponStickHorizontal && sticks[1].x<0.0f;
	const bool weaponRightShortcut=!gVrHandsEnabled && modifier &&
		FindPlayerVehicle()==nil && !remoteVehicleActive &&
		weaponStickHorizontal && sticks[1].x>0.0f;
	if(perfShortcut && !gTouchPerfShortcutDown) TogglePerfRecording();
	if(debugShortcut && !gTouchDebugShortcutDown){
		gDebugVisible=!gDebugVisible;
		debug("[OpenXR] Debug overlay: %s\n",gDebugVisible?"visible":"hidden");
	}
	if(spsShortcut && !gTouchSpsShortcutDown){
		gFullStereoSinglePass=!gFullStereoSinglePass;
		debug("[OpenXR] Stereo tail mode: %s\n",
			gFullStereoSinglePass?"full SPS":"hybrid packet replay");
	}
#ifdef RW_D3D12
	if(vrsShortcut && !gTouchVrsShortcutDown){
		gFixedFoveatedProfile = (gFixedFoveatedProfile+1) %
			rw::d3d12::FIXED_FOVEATED_PROFILE_COUNT;
		rw::d3d12::setFixedFoveatedRenderingProfile(gFixedFoveatedProfile);
		rw::d3d12::FixedFoveatedRenderingInfo foveatedInfo = {};
		rw::d3d12::getFixedFoveatedRenderingInfo(&foveatedInfo);
		SaveVrSetting("VRS", gFixedFoveatedProfile);
		debug("[OpenXR] Fixed foveated rendering profile %u (%s, Tier %u, tile %u)\n",
			foveatedInfo.profile, foveatedInfo.supported ? "ready" : "unsupported",
			foveatedInfo.tier, foveatedInfo.tileSize);
	}
#endif
	if(recenterShortcut && !gTouchRecenterShortcutDown){
		gRecenterRequested=true;
		gCinemaAnchorValid=false;
		debug("[OpenXR] Gameplay view recenter requested\n");
	}
	gTouchPerfShortcutDown=perfShortcut;
	gTouchDebugShortcutDown=debugShortcut;
	gTouchSpsShortcutDown=spsShortcut;
	gTouchVrsShortcutDown=vrsShortcut;
	gTouchRecenterShortcutDown=recenterShortcut;
	const bool teleportConsumed = UpdateTeleportInput(sticks[0]);
	UpdateCutsceneCameraInput(leftStickClick, rightStickClick);
	if(IsStereoCutsceneActive() && !gVrMenuVisible && !gCheatMenuVisible){
		MergeButton(state->Cross, a);
		MergeButton(state->Start, menu);
		return true;
	}
	const float headSteeringAxis =
		teleportConsumed ? 0.0f : GetHeadSteeringAxis(sticks[0]);
	if(!teleportConsumed){
		float moveX = sticks[0].x;
		float moveY = sticks[0].y;
		// Radial deadzone with rescale. Touch sticks drift with age, and the
		// raw rest offset here used to walk the player sideways on its own —
		// the steering path above already guards against this, movement never
		// did. 12% matches the platform dashboards, rescale keeps full range.
		{
			const float magnitude = sqrtf(moveX*moveX+moveY*moveY);
			const float moveDeadZone = 0.12f;
			if(magnitude <= moveDeadZone){
				moveX = 0.0f;
				moveY = 0.0f;
			}else{
				const float rescale =
					(magnitude-moveDeadZone)/((1.0f-moveDeadZone)*magnitude);
				moveX *= rescale;
				moveY *= rescale;
			}
			// Cross-axis deadzone. A worn stick wanders laterally while held
			// at its extreme (±0.2 with forward fully pressed was measured in
			// the field): the radial ring above cannot catch that, because
			// the vector's magnitude is large. The headset OS hides the same
			// wear with its own axial deadzones — the raw PC path must too.
			// Deliberate diagonals produce components far above a quarter of
			// the dominant axis and are unaffected.
			const float crossDeadZone = 0.25f;
			if(fabsf(moveX) < crossDeadZone*fabsf(moveY))
				moveX = 0.0f;
			else if(fabsf(moveY) < crossDeadZone*fabsf(moveX))
				moveY = 0.0f;
		}
		if(IsHeadRelativeLocomotionActive()){
			// Vice City interprets the left stick in the body-camera frame. Rotate
			// only by the HMD's local yaw so forward follows the viewer without
			// leaking head pitch/roll into movement or affecting vehicles.
			const float cosine = cosf(gHeadLocomotionYaw);
			const float sine = sinf(gHeadLocomotionYaw);
			const float headX = moveX*cosine-moveY*sine;
			const float headY = moveY*cosine+moveX*sine;
			moveX = headX;
			moveY = headY;
		}
		// Input-source telemetry for the "dragged sideways" reports: whatever
		// already sits in the pad state here arrived from NON-VR sources
		// (keyboard, legacy devices) before the OpenXR sticks merge on top.
		// One line per second, only while something is non-zero.
		{
			static ULONGLONG lastInputLogMs;
			const ULONGLONG inputNow = GetTickCount64();
			if(inputNow-lastInputLogMs >= 1000 &&
			   (Abs(state->LeftStickX) > 6 || Abs(state->LeftStickY) > 6 ||
			    fabsf(sticks[0].x) > 0.02f || fabsf(sticks[0].y) > 0.02f)){
				lastInputLogMs = inputNow;
				VrLog("Move input: pad pre-VR %d,%d | raw stick %.3f,%.3f | "
					"after deadzone %.3f,%.3f\n",
					(int)state->LeftStickX, (int)state->LeftStickY,
					sticks[0].x, sticks[0].y, moveX, moveY);
			}
		}
		MergeAxis(state->LeftStickX, AxisValue(moveX));
		MergeAxis(state->LeftStickY, AxisValue(-moveY));
	}
	ApplySnapTurn(sticks[1].x);
	if(!weaponLeftShortcut && !weaponRightShortcut){
		const float physicalTurnScale =
			(float)gTurnSensitivityPercent/100.0f;
		const float headTurnScale =
			(float)gHeadSteeringSensitivityPercent/100.0f;
		if(remoteVehicleActive){
			// RC aircraft use the right stick for yaw.  Locomotion turn mode
			// and sensitivity must never consume or rescale that mission input.
			MergeAxis(state->RightStickX, AxisValue(sticks[1].x));
		}else if(gTurnMode != VR_TURN_SNAP){
			MergeAxis(state->RightStickX, AxisValue(clamp(
				sticks[1].x*physicalTurnScale, -1.0f, 1.0f)));
		}
		// HEAD locomotion always uses a smooth head-driven camera turn. The
		// selected SNAP/SMOOTH mode continues to control the physical right stick.
		MergeAxis(state->RightStickX, AxisValue(clamp(
			headSteeringAxis*headTurnScale, -1.0f, 1.0f)));
		MergeAxis(state->RightStickY, AxisValue(-sticks[1].y));
	}
	MergeButton(state->LeftShoulder2, weaponLeftShortcut);
	MergeButton(state->RightShoulder2, weaponRightShortcut);
	const int padMode=CPad::GetPad(0)->GetMode();
	const bool remapOnFoot = gVrPadBindings.GetLayout() != VrPadBindings::DEFAULT &&
		physicalGameplayAvailable && !touchVehicle && !remoteVehicleActive &&
		gGameState == GS_PLAYING_GAME && !FrontEndMenuManager.m_bMenuActive &&
		!CCutsceneMgr::IsRunning() && !CCutsceneMgr::IsCutsceneProcessing() &&
		!TheCamera.m_WideScreenOn;
	MergeButton(state->LeftShoulder2, defaultDriveByLeft);
	MergeButton(state->RightShoulder2, defaultDriveByRight);
	if(defaultDriveByFire){
		if(padMode == 3)
			MergeButton(state->RightShoulder1, true);
		else
			MergeButton(state->Circle, true);
	}
	if(gImmersiveCarHornPressed){
		switch(padMode){
		case 1:
			state->LeftShoulder1 =
				Max(state->LeftShoulder1, (int16)255);
			break;
		case 2:
			state->RightShoulder1 =
				Max(state->RightShoulder1, (int16)255);
			break;
		default:
			state->LeftShock = Max(state->LeftShock, (int16)255);
			break;
		}
	}
	if(!modifier && !remapOnFoot){
		if(gHeldWeaponSlot[0] < 0 && !IsWeaponSupportHandInternal(0) &&
		   (reloadCapturedHands & 1u) == 0 &&
		   (trackedDetonatorHands & 1u) == 0 &&
		   (vehicleCapturedHands & 1u) == 0 &&
		   !IsVrCarDrivingActiveInternal() && !defaultDriveByLeft)
			state->LeftShoulder1=Max(state->LeftShoulder1,TriggerValue(leftGrip));
		// A grip holding a physical weapon belongs exclusively to that hand.
		if(padMode != 3 && gHeldWeaponSlot[1] < 0 &&
		   !IsWeaponSupportHandInternal(1) &&
		   (reloadCapturedHands & 2u) == 0 &&
		   (trackedDetonatorHands & 2u) == 0 &&
		   (vehicleCapturedHands & 2u) == 0 &&
		   !IsVrCarDrivingActiveInternal() && !defaultDriveByRight)
			state->RightShoulder1=Max(state->RightShoulder1,TriggerValue(rightGrip));
	}
	// RC missions do not put Tommy in a vehicle, but their aircraft/buggy uses
	// the same accelerator, brake and vehicle button layout.
	const bool inVehicle = FindPlayerVehicle() != nil ||
		CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode();
	if(!spsShortcut && (inVehicle || !gVrHandsEnabled))
		state->Square=Max(state->Square,TriggerValue(leftTrigger));
	if(!vrsShortcut){
		const int16 rightTriggerValue=TriggerValue(rightTrigger);
		if(inVehicle){
			// Keep the original accelerator binding while driving.
			state->Cross=Max(state->Cross,rightTriggerValue);
		}else if(!gVrHandsEnabled){
			const int weaponType = GetVrCurrentWeaponType();
			const bool directVrFirearm =
				(weaponType >= WEAPONTYPE_COLT45 && weaponType <= WEAPONTYPE_MINIGUN) ||
				weaponType == WEAPONTYPE_HELICANNON;
			if(!directVrFirearm){
				// Melee, thrown weapons, detonator and camera still use their
				// legacy action state until their physical VR interactions exist.
				switch(padMode){
				case 2: state->Cross=Max(state->Cross,rightTriggerValue); break;
				case 3: state->RightShoulder1=Max(state->RightShoulder1,rightTriggerValue); break;
				default: state->Circle=Max(state->Circle,rightTriggerValue); break;
				}
			}
		}
		// Firearms are deliberately not translated to a legacy pad button.
		// CPlayerPed consumes their OpenXR trigger directly, so SetAttack cannot
		// produce a second delayed bullet from the same physical press.
	}
	if(!modifier && !remapOnFoot){
		// Suppress whichever face button the selected classic control mode
		// treats as fire. Other face-button actions remain available.
		if(padMode != 2 && !remoteVehicleActive)
			MergeButton(state->Cross,a);
		if(!defaultDriveByFire && !vehicleWeaponButtonConsumed &&
		   padMode != 0 && padMode != 1)
			MergeButton(state->Circle,b);
		// The RC helicopter script listens to the stock vehicle-fire action for
		// dropping a bomb. A normal driven vehicle keeps X exclusively for the
		// radio, while remote vehicles receive the expected action on A.
		if(remoteVehicleActive && a && !serviceChord){
			if(padMode == 3)
				MergeButton(state->RightShoulder1, true);
			else
				MergeButton(state->Circle, true);
		}else if(!inVehicle)
			MergeButton(state->Square,x);
		MergeButton(state->Triangle,y);
	}
	if(!modifier && !remapOnFoot){
		if(gStickCrouch || inVehicle) MergeButton(state->LeftShock,leftStickClick);
		if(inVehicle && gStickLookBehind)
			MergeButton(state->RightShock,rightStickClick);
		else if(!inVehicle && rightStickClick){
			// R3 is sprint on foot. Do not also pass it as RightShock there:
			// Vice City maps that state to look-behind.
			if(padMode == 2)
				state->Circle = Max(state->Circle, (int16)255);
			else
				state->Cross = Max(state->Cross, (int16)255);
		}
	}
	if(remapOnFoot && !modifier){
		VrPadBindings::Context context;
		context.gameplay = physicalGameplayAvailable;
		context.onFoot = !inVehicle;
		context.remote = remoteVehicleActive;
		context.consumed = serviceChord || gVrMenuVisible || gCheatMenuVisible;
		context.padMode = padMode;
		context.stickCrouch = gStickCrouch;
		const uint32 reservedHands = reloadCapturedHands | trackedDetonatorHands | vehicleCapturedHands;
		for(int hand = 0; hand < EYE_COUNT; hand++){
			if(gHeldWeaponSlot[hand] >= 0 || IsWeaponSupportHandInternal(hand) ||
			   (reservedHands & (1u << hand))){
				context.capturedSources |= 1u << (hand == 0 ? VrPadBindings::LEFT_GRIP : VrPadBindings::RIGHT_GRIP);
				context.capturedTargets |= 1u << (hand == 0 ? VrPadBindings::L1 : VrPadBindings::R1);
			}
		}
		const int values[VrPadBindings::SOURCE_COUNT] = { a ? 255 : 0, b ? 255 : 0,
			x ? 255 : 0, y ? 255 : 0, TriggerValue(leftGrip), TriggerValue(rightGrip),
			leftStickClick ? 255 : 0, rightStickClick ? 255 : 0 };
		int buttons[VrPadBindings::TARGET_COUNT];
		VrPadBindings::Resolve(gVrPadBindings, values, context, buttons);
		// Merge only this input's contributions; keyboard/pad state is not erased.
		state->Square = Max(state->Square, (int16)buttons[VrPadBindings::SQUARE]);
		state->Cross = Max(state->Cross, (int16)buttons[VrPadBindings::CROSS]);
		state->Circle = Max(state->Circle, (int16)buttons[VrPadBindings::CIRCLE]);
		state->Triangle = Max(state->Triangle, (int16)buttons[VrPadBindings::TRIANGLE]);
		state->LeftShoulder1 = Max(state->LeftShoulder1, (int16)buttons[VrPadBindings::L1]);
		state->RightShoulder1 = Max(state->RightShoulder1, (int16)buttons[VrPadBindings::R1]);
		state->LeftShoulder2 = Max(state->LeftShoulder2, (int16)buttons[VrPadBindings::L2]);
		state->RightShoulder2 = Max(state->RightShoulder2, (int16)buttons[VrPadBindings::R2]);
		state->LeftShock = Max(state->LeftShock, (int16)buttons[VrPadBindings::L3]);
		state->RightShock = Max(state->RightShock, (int16)buttons[VrPadBindings::R3]);
	}
	if(!vrMenuShortcut)
		MergeButton(state->Start,menu);
	return true;
}

bool BeginStereoFrame(RwCamera *camera, const CMatrix &baseCamera)
{
	ValidateHolsterCalibrationLifecycle();
	SetFramePhase("stereo");
	if(!camera || !BeginXrFrame()) return false;
	if(!gFrameState.shouldRender){ EndXrFrame(nil,0); return false; }
	// The next frontend or cutscene should anchor a fresh static theater
	// screen in front of the viewer instead of reusing a pre-gameplay
	// location — but only after a sustained stretch of real gameplay.
	// Scripted sequences interleave short gameplay windows between their
	// widescreen segments; resetting the anchor across every one of those
	// made the screen re-center on the player's head several times per
	// scene, which reads as the picture chasing and shaking with the head.
	gConsecutiveStereoFrames++;
	if(gConsecutiveStereoFrames >= 90 && gCinemaAnchorValid){
		gCinemaAnchorResets++;
		gCinemaAnchorValid = false;
	}
	// Stereo wrote real world content into the eye images; the keep-alive
	// cinema projection must re-clear the full rotation before reuse.
	{
		const size_t eyeImages = Max(gEye[0].swapchain.images.size(),
			gEye[1].swapchain.images.size());
		gCinemaEyeClearsPending = (uint32)eyeImages;
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF4)){
		gLightingEnabled=!gLightingEnabled;
		SaveVrSetting("ViceCityColor", gLightingEnabled);
		debug("[OpenXR] Vice City color filter: %s\n",gLightingEnabled?"enabled":"disabled");
	}
	if(ControlsManager.GetIsKeyboardKeyJustDown(rsF5)){
		gAntiAliasingEnabled=!gAntiAliasingEnabled;
		SaveVrSetting("AntiAliasing", gAntiAliasingEnabled);
		debug("[OpenXR] Anti-aliasing comparison: %s\n",gAntiAliasingEnabled?"enabled":"disabled");
	}
	gBaseCamera=baseCamera;
	CVehicle *viewVehicle = FindPlayerVehicle();
	if(IsStereoCutsceneActive() || IsVehicleThirdPersonActive()) viewVehicle = nil;
	ApplyVehicleViewTranslation(gBaseCamera, viewVehicle);
	CVehicle *horizonVehicle = viewVehicle;
	if(gBikeLockHorizonEnabled && horizonVehicle &&
	   horizonVehicle->IsBike() &&
	   gGameState == GS_PLAYING_GAME &&
	   !CCutsceneMgr::IsRunning() &&
	   !CCutsceneMgr::IsCutsceneProcessing() &&
	   !TheCamera.m_WideScreenOn){
		CVector forward = gBaseCamera.GetForward();
		if(forward.MagnitudeSqr() > 0.0001f){
			forward.Normalise();
			const CVector worldUp(0.0f, 0.0f, 1.0f);
			// Camera matrices in reVC store the screen-left vector in the
			// GetRight() slot (see CCamera::CalculateDerivedValues).  Supplying
			// a mathematical right vector here mirrors the OpenXR IPD and makes
			// the two eyes look away from each other.  Preserve the camera's
			// pitch/heading and rebuild only its roll against world-up.
			CVector left = CrossProduct(worldUp, forward);
			if(left.MagnitudeSqr() > 0.0001f){
				left.Normalise();
				CVector up = CrossProduct(forward, left);
				up.Normalise();
				gBaseCamera.GetRight() = left;
				gBaseCamera.GetUp() = up;
				gBaseCamera.GetForward() = forward;
			}
		}
	}
	const bool vehicleViewActive = viewVehicle != nil;
	if(!gVehicleViewStateValid){
		gVehicleViewStateValid = true;
		gVehicleViewWasActive = vehicleViewActive;
	}else if(gVehicleViewWasActive != vehicleViewActive){
		// Quest changes between its simple on-foot anchor and full vehicle basis
		// here and re-latches the current physical head yaw on that mode change.
		// Recreate the PC gameplay space at the same boundary: otherwise HEAD
		// DIRECTION carries its old room-space yaw into the authored vehicle basis,
		// so Tommy can finish the entry animation facing sideways or backwards.
		gVehicleViewWasActive = vehicleViewActive;
		gRecenterRequested = true;
	}
	gOriginalColor=RwCameraGetRaster(camera); gOriginalDepth=RwCameraGetZRaster(camera);
	gOriginalViewWindow=*RwCameraGetViewWindow(camera); gOriginalViewOffset=*RwCameraGetViewOffset(camera);
	gOriginalFrameMatrix=*RwFrameGetMatrix(RwCameraGetFrame(camera));
	gOriginalScreenWidth=RsGlobal.width; gOriginalScreenHeight=RsGlobal.height;
	gOriginalNearPlane=RwCameraGetNearClipPlane(camera); gOriginalDrawNear=CDraw::GetNearClipZ();
	gFramePrepared=true;
	if(gRecenterRequested){
		if(gGameplaySpace) xrDestroySpace(gGameplaySpace);
		gGameplaySpace=XR_NULL_HANDLE;
		gTrackingCenterValid=false;
		gHeadLocomotionPoseValid=false;
		gHeadLocomotionYaw=0.0f;
		gHolsterHeadForwardValid=false;
		gRecenterRequested=false;
		InvalidateImmersiveTrackingReferences();
		ResetMotionSteeringInteraction();
		ResetTemporalAaHistory();
	}
	XrViewLocateInfo locate={XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime=gFrameState.predictedDisplayTime;
	auto locateViews=[&](XrSpace space)->bool {
		locate.space=space;
		XrViewState viewState={XR_TYPE_VIEW_STATE}; uint32_t count=0;
		for(int i=0;i<EYE_COUNT;i++) gLocatedViews[i]={XR_TYPE_VIEW};
		const double timingStart = gPerfFrameStarted ? PerfNowMs() : 0.0;
		XrResult result = xrLocateViews(
			gSession,&locate,&viewState,EYE_COUNT,&count,gLocatedViews);
		if(gPerfFrameStarted)
			gPerfCurrent.xrLocateViewsMs +=
				(float)(PerfNowMs() - timingStart);
		return XrOk(result,"xrLocateViews") &&
			count==EYE_COUNT &&
			(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0 &&
			(viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)!=0;
	};
	if(!locateViews(gGameplaySpace ? gGameplaySpace : gLocalSpace)){
		RestoreCamera(camera); EndXrFrame(nil,0); return false;
	}
	if(!gGameplaySpace && gSessionState != XR_SESSION_STATE_FOCUSED){
		// Never anchor the gameplay space from an unworn headset. Sessions
		// launched with the HMD lying on the desk used to latch the desk's
		// yaw as "forward", silently rotating all head-relative movement for
		// the whole session ("pressed forward, walked sideways"). Until the
		// player actually puts the headset on (FOCUSED), render against the
		// plain LOCAL space; the anchor latches on the first worn frame.
	}else if(!gGameplaySpace){
		const XrVector3f centre={
			(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
			(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
			(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f };
		const XrVector3f localForward={0.0f,0.0f,-1.0f};
		const XrVector3f forward=Rotate(gLocatedViews[0].pose.orientation,localForward);
		const float yaw=atan2f(-forward.x,-forward.z);
		XrReferenceSpaceCreateInfo gameplayInfo={XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		gameplayInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
		gameplayInfo.poseInReferenceSpace.position=centre;
		gameplayInfo.poseInReferenceSpace.orientation.y=sinf(yaw*0.5f);
		gameplayInfo.poseInReferenceSpace.orientation.w=cosf(yaw*0.5f);
		if(!XrOk(xrCreateReferenceSpace(gSession,&gameplayInfo,&gGameplaySpace),"xrCreateReferenceSpace(gameplay)") ||
		   !locateViews(gGameplaySpace)){
			RestoreCamera(camera); EndXrFrame(nil,0); return false;
		}
		debug("[OpenXR] Gameplay view recentered\n");
	}
	const XrVector3f headLocalForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f headForward = Rotate(
		gLocatedViews[0].pose.orientation, headLocalForward);
	if(headForward.x*headForward.x+headForward.z*headForward.z > 0.0001f){
		gHeadLocomotionYaw = atan2f(-headForward.x, -headForward.z);
		gHeadLocomotionPoseValid = true;
	}
	LocateTrackedHands();
	for(int eye=0;eye<EYE_COUNT;eye++){
		gRenderPose[eye]=gLocatedViews[eye].pose;
		// Unlike the old LibOVR calibration path, OpenXR already supplies the exact
		// asymmetric optical projection for each physical eye.  Rendering a wider
		// synthetic symmetric frustum and then describing it to the compositor as a
		// real view creates a direction-dependent lens warp on Quest.
		gRenderFov[eye]=gLocatedViews[eye].fov;
	}
	// RenderWare gets one stable symmetric projection for both eyes. The final
	// backend resolve pass remaps that source into each eye's exact asymmetric
	// OpenXR frustum.
	gSourceTanX=0.0f; gSourceTanY=0.0f;
	for(int eye=0;eye<EYE_COUNT;eye++){
		gSourceTanX=Max(gSourceTanX,Max(-tanf(gRenderFov[eye].angleLeft),tanf(gRenderFov[eye].angleRight)));
		gSourceTanY=Max(gSourceTanY,Max(tanf(gRenderFov[eye].angleUp),-tanf(gRenderFov[eye].angleDown)));
	}
	XrVector3f center={ (gRenderPose[0].position.x+gRenderPose[1].position.x)*0.5f,
		(gRenderPose[0].position.y+gRenderPose[1].position.y)*0.5f,
		(gRenderPose[0].position.z+gRenderPose[1].position.z)*0.5f };
	if(gFirstPersonEnabled && !gTrackingCenterValid){ gTrackingCenterOrigin=center; gTrackingCenterValid=true; }
	XrVector3f tracking={ gFirstPersonEnabled?center.x-gTrackingCenterOrigin.x:0.0f,
		gFirstPersonEnabled?center.y-gTrackingCenterOrigin.y:0.0f,
		gFirstPersonEnabled?center.z-gTrackingCenterOrigin.z:0.0f };
	// OpenXR supplies the physically correct per-eye poses.  Keep the scale and
	// direction fixed so an accidental input can never invalidate stereo again.
	const float scale=1.0f;
	for(int eye=0;eye<EYE_COUNT;eye++){
		gRenderPose[eye].position.x=(gRenderPose[eye].position.x-center.x)*scale+tracking.x;
		gRenderPose[eye].position.y=(gRenderPose[eye].position.y-center.y)*scale+tracking.y;
		gRenderPose[eye].position.z=(gRenderPose[eye].position.z-center.z)*scale+tracking.z;
	}
	UpdateTrackedScopeState();
	// Gameplay input is processed before BeginStereoFrame, when gBaseCamera and
	// the OpenXR hand poses still belong to the preceding rendered frame. Keep a
	// render-prepared aim snapshot for the following simulation tick instead of
	// allowing trigger input to populate this frame's cache from that stale
	// anchor. Refresh every held weapon now that both the camera and poses agree.
	for(int hand = 0; hand < EYE_COUNT; hand++){
		gTrackedAimCacheValid[hand] = false;
		const int slot = gHeldWeaponSlot[hand];
		if(slot < 0 || slot >= TOTAL_WEAPON_SLOTS)
			continue;
		CVector source, direction;
		GetTrackedWeaponAim(hand, GetVrWeaponTypeForSlot(slot),
			&source, &direction);
	}
	gTemporalJitterX = gTemporalJitterY = 0.0f;
	gTemporalJitterClipX = gTemporalJitterClipY = 0.0f;
#ifdef RW_D3D12
	const bool wantsObjectMotion =
		gTemporalAaBackend == TEMPORAL_AA_DLAA &&
		gDlaaStereoActivationReady && !gDlaaStereoActivationFailed &&
		Dlaa::IsSupported();
	if(!rw::d3d12::setRasterMotionEnabled((rw::Raster*)gStereoColor,
	   wantsObjectMotion) && wantsObjectMotion){
		gDlaaStereoActivationFailed = true;
		VrLog("DLAA object-motion allocation failed; retaining FXAA fallback\n");
	}
	// Jitter may only reach the projection when a temporal resolve will
	// actually integrate it. DLAA spends frames in an activation warmup —
	// and restarts it after every cinema interruption — during which the
	// frames are presented raw; jittering those just shakes the image.
	const bool temporalResolveActive =
		gTemporalAaBackend == TEMPORAL_AA_DLAA ?
			(gDlaaStereoActivationReady && !gDlaaStereoActivationFailed) :
			gTemporalAaBackend != TEMPORAL_AA_OFF;
	if(temporalResolveActive &&
	   IsSelectedTemporalAaSupported() && gTemporalJitterMode != 0 &&
	   gEye[0].renderWidth > 0 && gEye[0].renderHeight > 0){
		const uint32 sequence = (gTemporalFrameIndex++ % 8U) + 1U;
		const float projectionJitterX = Halton(sequence, 2U)-0.5f;
		const float projectionJitterY = Halton(sequence, 3U)-0.5f;
		float reportedScaleX = 1.0f;
		float reportedScaleY = 1.0f;
		if(gTemporalJitterMode == 2)
			reportedScaleX = 2.0f;
		else if(gTemporalJitterMode == 3)
			reportedScaleX = reportedScaleY = 2.0f;
		else if(gTemporalJitterMode == 4)
			reportedScaleX = reportedScaleY = 0.5f;
		else if(gTemporalJitterMode == 5)
			reportedScaleX = reportedScaleY = -1.0f;
		gTemporalJitterX = projectionJitterX*reportedScaleX;
		gTemporalJitterY = projectionJitterY*reportedScaleY;
		gTemporalJitterClipX = 2.0f*projectionJitterX/(float)gEye[0].renderWidth;
		gTemporalJitterClipY = -2.0f*projectionJitterY/(float)gEye[0].renderHeight;
	}
#endif
	return true;
}

bool GetEyeCamera(int eye, CMatrix *eyeCamera)
{
	if(!gFramePrepared || !eyeCamera || eye<0 || eye>=EYE_COUNT) return false;
	const XrVector3f localUp={0,1,0}, localForward={0,0,-1}; const XrPosef &pose=gRenderPose[eye];
	*eyeCamera=gBaseCamera;
	CVector forward=ToGameVector(Rotate(pose.orientation,localForward));
	CVector upVector=ToGameVector(Rotate(pose.orientation,localUp));
	forward.Normalise(); CVector leftVector=CrossProduct(upVector,forward); leftVector.Normalise();
	upVector=CrossProduct(forward,leftVector); upVector.Normalise();
	eyeCamera->GetRight()=leftVector; eyeCamera->GetUp()=upVector; eyeCamera->GetForward()=forward;
	eyeCamera->GetPosition()=gBaseCamera.GetPosition()+ToGameVector(pose.position);
	return true;
}

bool BeginEye(RwCamera *camera, int eye, CMatrix *eyeCamera, float *horizontalFov)
{
	if(!gFramePrepared || !camera || !GetEyeCamera(eye, eyeCamera)) return false;
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(eye);
#endif
	RwV2d window={gSourceTanX,gSourceTanY};
	RwV2d offset={0.0f,0.0f};
	RwCameraSetRaster(camera,gEye[eye].color); RwCameraSetZRaster(camera,gEye[eye].depth);
	RwCameraSetViewWindow(camera,&window); RwCameraSetViewOffset(camera,&offset);
	if(gFirstPersonEnabled){ RwCameraSetNearClipPlane(camera,0.05f); CDraw::SetNearClipZ(0.05f); }
	RsGlobal.width=gEye[eye].renderWidth; RsGlobal.height=gEye[eye].renderHeight;
	RwMatrix *matrix=RwFrameGetMatrix(RwCameraGetFrame(camera));
	*RwMatrixGetRight(matrix)=eyeCamera->GetRight(); *RwMatrixGetUp(matrix)=eyeCamera->GetUp();
	*RwMatrixGetAt(matrix)=eyeCamera->GetForward(); *RwMatrixGetPos(matrix)=eyeCamera->GetPosition();
	RwMatrixUpdate(matrix); RwFrameUpdateObjects(RwCameraGetFrame(camera)); RwFrameOrthoNormalize(RwCameraGetFrame(camera));
	if(horizontalFov) *horizontalFov=RADTODEG(2.0f*atanf(gSourceTanX));
	return true;
}

void GetTemporalJitterClip(float *x, float *y)
{
	if(x) *x = gTemporalJitterClipX;
	if(y) *y = gTemporalJitterClipY;
}

void SetTrackedForegroundRenderCallback(
	TrackedForegroundRenderCallback callback)
{
	gTrackedForegroundRenderCallback = callback;
}

void SetTrackedForegroundVisibilityCallback(
	TrackedForegroundVisibilityCallback callback)
{
	gTrackedForegroundVisibilityCallback = callback;
}

bool IsTrackedForegroundRenderAvailable()
{
#ifdef RW_D3D12
	return gTrackedForegroundResolveAvailable &&
		gTrackedForegroundResolvePipeline != nil &&
		gTrackedForegroundResolveRootSignature != nil;
#else
	return false;
#endif
}

#ifdef RW_D3D12
// Flat-mode desktop temporal AA: after the world (and post effects) rendered
// into the window back buffer, copy the scene into a staging input, run the
// DLAA evaluate on the dedicated desktop slot, and copy the reconstructed
// image back over the back buffer. The HUD then draws on top, untouched by
// the network. VR sessions never reach this path: it is flat-mode only.
static ID3D12Resource *gDesktopAaStaging;
static uint32 gDesktopAaStagingWidth;
static uint32 gDesktopAaStagingHeight;

void ReleaseDesktopAaStaging()
{
	if(gDesktopAaStaging){
		// Previous frames may still reference the texture; the deferred queue
		// releases it once the GPU is past them.
		rw::d3d12::deferReleaseAfterNextSubmit(gDesktopAaStaging);
		gDesktopAaStaging = nil;
	}
	gDesktopAaStagingWidth = 0;
	gDesktopAaStagingHeight = 0;
}

void EvaluateDesktopTemporalAa(void *rwCamera)
{
	if(!gFlatModeEnabled || gTemporalAaBackend != TEMPORAL_AA_DLAA ||
	   !gStreamlineEnabled || !Dlaa::IsSupported())
		return;
	RwCamera *camera = (RwCamera*)rwCamera;
	if(!camera)
		return;
	RwRaster *frameRaster = RwCameraGetRaster(camera);
	RwRaster *zRaster = RwCameraGetZRaster(camera);
	if(!frameRaster || !zRaster)
		return;
	const uint32 width = (uint32)RwRasterGetWidth(frameRaster);
	const uint32 height = (uint32)RwRasterGetHeight(frameRaster);
	if(width == 0 || height == 0)
		return;
	if(gDesktopAaStaging &&
	   (gDesktopAaStagingWidth != width || gDesktopAaStagingHeight != height)){
		ReleaseDesktopAaStaging();
		Dlaa::ResetHistory();
	}
	if(!gDesktopAaStaging){
		if(!rw::d3d12::createStagingColorTexture((int32)width, (int32)height,
		   &gDesktopAaStaging) || !gDesktopAaStaging){
			gDesktopAaStaging = nil;
			return;
		}
		gDesktopAaStagingWidth = width;
		gDesktopAaStagingHeight = height;
	}
	if(!Dlaa::BeginFrame(0.0f, 0.0f))
		return;
	rw::d3d12::captureDesktopWorldCamera();
	if(!rw::d3d12::copyCurrentBackBufferToExternal(gDesktopAaStaging))
		return;
	ID3D12GraphicsCommandList *list = rw::d3d12::getCommandList();
	if(!list)
		return;
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = gDesktopAaStaging;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	list->ResourceBarrier(1, &barrier);

	rw::Raster *depthRaster = ((rw::Raster*)zRaster)->parent;
	ID3D12Resource *depthResource = nil;
	D3D12_GPU_DESCRIPTOR_HANDLE depthView = {};
	if(depthRaster &&
	   rw::d3d12::getDepthTextureView(depthRaster, &depthResource, &depthView) &&
	   depthResource && depthView.ptr != 0 &&
	   rw::d3d12::transitionDepthRaster(depthRaster,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)){
		Dlaa::EyeInput input = {};
		input.color = gDesktopAaStaging;
		input.depth = depthResource;
		input.depthShaderResourceView = depthView.ptr;
		input.width = width;
		input.height = height;
		input.outputWidth = width;
		input.outputHeight = height;
		input.sourceLeft = 0;
		if(rw::d3d12::getDesktopWorldCamera(input.view, input.projection)){
			input.nearPlane = RwCameraGetNearClipPlane(camera);
			input.farPlane = RwCameraGetFarClipPlane(camera);
			Dlaa::EyeOutput output = {};
			const bool evaluated =
				Dlaa::EvaluateEye(Dlaa::kDesktopSlot, input, &output);
			if(Dlaa::ConsumeNeuralRenderingPassCountChanged())
				SaveVrSetting("DLSSNeuralPasses",
					Dlaa::GetNeuralRenderingPassCount());
			if(evaluated && output.color &&
			   rw::d3d12::copyExternalToCurrentBackBuffer(
				(ID3D12Resource*)output.color,
				(uint32)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)){
				static bool logged;
				if(!logged){
					logged = true;
					VrLog("Flat desktop DLAA active: %ux%u\n", width, height);
				}
			}
		}
		// The window depth buffer stays bound for the rest of the frame; put it
		// back in write state so the HUD's depth-stencil binding stays legal.
		rw::d3d12::transitionDepthRaster(depthRaster,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	list->ResourceBarrier(1, &barrier);
	// Streamline leaves arbitrary pipeline state behind; force librw to rebind
	// everything on the next draw.
	rw::d3d12::resetWorldDrawState();
}
#else
void EvaluateDesktopTemporalAa(void*){}
#endif

int GetStereoScalePercent(){ return 100; }
bool IsStereoReversed(){ return false; }
bool IsFirstPersonEnabled(){ return gFirstPersonEnabled; }
bool IsStereoCutsceneActive()
{
	return gCutsceneMode != 0 && gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded && !FrontEndMenuManager.m_bMenuActive &&
		!FrontEndMenuManager.m_bWantToRestart && !FrontEndMenuManager.m_bWantToLoad &&
		!CGame::playingIntro && FindPlayerPed() != nil &&
		(CCutsceneMgr::IsRunning() || TheCamera.m_WideScreenOn);
}

static RpHAnimHierarchy *GetCutsceneHeadHierarchy(CEntity *entity)
{
	if(!entity || !entity->m_rwObject || RwObjectGetType(entity->m_rwObject) != rpCLUMP ||
	   !IsClumpSkinned((RpClump*)entity->m_rwObject))
		return nil;
	RpHAnimHierarchy *hierarchy = GetAnimHierarchyFromSkinClump((RpClump*)entity->m_rwObject);
	return hierarchy && RpHAnimIDGetIndex(hierarchy, BONE_head) >= 0 &&
		RpHAnimHierarchyGetMatrixArray(hierarchy) ? hierarchy : nil;
}

static int StageCutsceneCameras(CEntity **actors, int capacity)
{
	int count = 0;
	for(int i = 0; i < Min(CCutsceneMgr::GetNumCutsceneObjects(), (int)NUMCUTSCENEOBJECTS) && count < capacity; i++){
		CEntity *actor = CCutsceneMgr::GetCutsceneObject(i);
		if(GetCutsceneHeadHierarchy(actor)) actors[count++] = actor;
	}
	if(count || CCutsceneMgr::IsRunning() || CCutsceneMgr::IsCutsceneProcessing()) return count;
	CPlayerPed *player = FindPlayerPed();
	if(GetCutsceneHeadHierarchy(player) && count < capacity) actors[count++] = player;
	CVehicle *vehicle = player && player->bInVehicle ? player->m_pMyVehicle : nil;
	if(!vehicle) return count;
	if(vehicle->pDriver != player && GetCutsceneHeadHierarchy(vehicle->pDriver) && count < capacity)
		actors[count++] = vehicle->pDriver;
	for(int i = 0; i < (int)ARRAY_SIZE(vehicle->pPassengers) && count < capacity; i++)
		if(vehicle->pPassengers[i] != player && GetCutsceneHeadHierarchy(vehicle->pPassengers[i]))
			actors[count++] = vehicle->pPassengers[i];
	return count;
}

static void UpdateCutsceneCameraInput(bool leftClick, bool rightClick)
{
	if(!IsStereoCutsceneActive() || gVrMenuVisible || gCheatMenuVisible){
		gCutsceneCycleDown = gCutsceneStoreDown = false;
		if(!IsStereoCutsceneActive()) gCutsceneCameraScene[0] = '\0';
		return;
	}
	const char *scene = CCutsceneMgr::IsRunning() ? CCutsceneMgr::GetCutsceneName() : "scripted";
	if(scene && strncmp(scene, gCutsceneCameraScene, sizeof(gCutsceneCameraScene)-1) != 0){
		strncpy(gCutsceneCameraScene, scene, sizeof(gCutsceneCameraScene)-1);
		gCutsceneCameraScene[sizeof(gCutsceneCameraScene)-1] = '\0';
		gCutsceneCamera = Min(Max((int)GetPrivateProfileIntA("VRCutsceneCamera", gCutsceneCameraScene,
			0, GetVrSettingsPath()), 0), (int)NUMCUTSCENEOBJECTS);
		gRecenterRequested = true;
	}
	const bool cycle = rightClick && !leftClick;
	const bool store = leftClick && !rightClick;
	if(cycle && !gCutsceneCycleDown){
		CEntity *actors[NUMCUTSCENEOBJECTS];
		gCutsceneCamera = (gCutsceneCamera+1)%(StageCutsceneCameras(actors, ARRAY_SIZE(actors))+1);
		gRecenterRequested = true;
	}
	if(store && !gCutsceneStoreDown && gCutsceneCameraScene[0]){
		char value[16];
		sprintf(value, "%d", gCutsceneCamera);
		WritePrivateProfileStringA("VRCutsceneCamera", gCutsceneCameraScene, value, GetVrSettingsPath());
	}
	gCutsceneCycleDown = cycle;
	gCutsceneStoreDown = store;
}

void ApplyCutsceneCamera(CMatrix *camera)
{
	gCutsceneCameraActor = nil;
	if(!camera || !IsStereoCutsceneActive() || gCutsceneCamera <= 0) return;
	CEntity *actors[NUMCUTSCENEOBJECTS];
	const int count = StageCutsceneCameras(actors, ARRAY_SIZE(actors));
	if(gCutsceneCamera > count) return;
	CEntity *actor = actors[gCutsceneCamera-1];
	RpHAnimHierarchy *hierarchy = GetCutsceneHeadHierarchy(actor);
	if(!hierarchy) return;
	RwMatrix &head = RpHAnimHierarchyGetMatrixArray(hierarchy)[RpHAnimIDGetIndex(hierarchy, BONE_head)];
	CVector forward(RwMatrixGetUp(&head)->x, RwMatrixGetUp(&head)->y, 0.0f);
	if(forward.MagnitudeSqr() < 0.0001f){ forward = actor->GetForward(); forward.z = 0.0f; }
	if(forward.MagnitudeSqr() < 0.0001f) forward = CVector(0.0f, 1.0f, 0.0f);
	forward.Normalise();
	const CVector up(0.0f, 0.0f, 1.0f);
	camera->GetForward() = forward;
	camera->GetUp() = up;
	camera->GetRight() = CrossProduct(up, forward);
	camera->GetPosition() = CVector(RwMatrixGetPos(&head)->x, RwMatrixGetPos(&head)->y, RwMatrixGetPos(&head)->z);
	gCutsceneCameraActor = actor;
}

bool ShouldHideCameraActorHead(CEntity *entity)
{
	if(!gFramePrepared || !entity) return false;
	if(IsStereoCutsceneActive()) return entity == gCutsceneCameraActor;
	CPlayerPed *player = FindPlayerPed();
	return entity == player && player && player->bInVehicle && gFirstPersonEnabled &&
		!IsVehicleThirdPersonActive();
}
bool IsVehicleThirdPersonActive()
{
	if(!gSessionRunning || gFlatModeEnabled || gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded || FrontEndMenuManager.m_bMenuActive ||
	   FrontEndMenuManager.m_bWantToRestart || FrontEndMenuManager.m_bWantToLoad ||
	   CGame::playingIntro)
		return false;
	CPlayerPed *player = FindPlayerPed();
	CVehicle *vehicle = FindPlayerVehicle();
	const int category = GetVrVehicleCategory(vehicle);
	if(!player || !player->m_rwObject || player->DyingOrDead() || !vehicle ||
	   category < 0 || !gVehicleThirdPerson[category] ||
	   CCutsceneMgr::IsRunning() || CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return false;
	const PedState state = player->GetPedState();
	return player->bInVehicle && state != PED_EXIT_CAR && state != PED_DRAG_FROM_CAR &&
		state != PED_ENTER_CAR && state != PED_CARJACK && state != PED_OPEN_DOOR &&
		player->m_objective != OBJECTIVE_LEAVE_CAR &&
		player->m_objective != OBJECTIVE_LEAVE_CAR_AND_DIE;
}
bool ShouldHideDefaultDrivingBody()
{
	return !gDefaultDrivingBodyVisible && gFramePrepared && gFirstPersonEnabled &&
		!IsVehicleThirdPersonActive() && !IsStereoCutsceneActive() &&
		FindPlayerVehicle() != nil;
}
bool IsBikeViewFollowingTilt(){ return gBikeViewFollowingTilt; }
bool IsBikeManualThrottle(){ return gBikeManualThrottle; }
int GetBikeVisualLeanPercent(){ return gBikeVisualLeanPercent; }
bool CanBikeRiderBeThrown(){ return !gSessionRunning || gFlatModeEnabled || gBikeRiderCanBeThrown; }
bool IsFlatFirstPersonEnabled(){ return gFlatModeEnabled && gFlatFirstPersonEnabled; }
bool IsFlatNeuralRenderingActive()
{
	return gFlatModeEnabled && Dlaa::IsNeuralRenderingActive();
}
bool IsFlatNeuralRenderingOutputVisible()
{
	return IsFlatNeuralRenderingActive() &&
		Dlaa::IsNeuralRenderingOutputVisible();
}
bool IsFlatNeuralRenderingSplitView()
{
	return IsFlatNeuralRenderingActive() && Dlaa::IsNeuralRenderingSplitView();
}
bool IsFlatNeuralRenderingOverlayVisible()
{
	return IsFlatNeuralRenderingActive() &&
		Dlaa::IsNeuralRenderingOverlayVisible();
}
int GetFlatNeuralRenderingOverlaySelection()
{
	return Dlaa::GetNeuralRenderingOverlaySelection();
}
int GetFlatNeuralRenderingPassCount()
{
	return Dlaa::GetNeuralRenderingPassCount();
}
const char *GetFlatNeuralRenderingEffectName()
{
	return Dlaa::GetNeuralRenderingEffectName();
}
const char *GetFlatNeuralRenderingViewName()
{
	return Dlaa::GetNeuralRenderingViewName();
}
bool IsFlatNeuralRenderingFloatTuningVerified()
{
	return Dlaa::IsNeuralRenderingFloatTuningVerified();
}
float GetSunGlareScale(){ return gSunGlarePercent/100.0f; }
void ReapplyTrafficSettings(){ ApplyTrafficSettings(); }
void ReloadVehicleCategoryCalibration()
{
	const char *path = GetVrSettingsPath();
	// Split the old shared height into independent category values. Existing
	// profiles inherit DrivingYOffsetCm once as the default for each missing new
	// key; changing one category afterwards never changes another one.
	int legacyDrivingYOffsetCm;
	const bool hasLegacyDrivingYOffset = ReadVrProfileInt("VR",
		"DrivingYOffsetCm", path, &legacyDrivingYOffsetCm);
	legacyDrivingYOffsetCm = Min(Max(hasLegacyDrivingYOffset ?
		legacyDrivingYOffsetCm : 15, -100), 150);
	VehicleCategoryCalibration classicCarDefaults, modernCarDefaults;
	GetBuiltInVehicleCategoryCalibration(ModelSets::MODEL_SET_CLASSIC,
		VR_VEHICLE_CATEGORY_CAR, &classicCarDefaults);
	GetBuiltInVehicleCategoryCalibration(ModelSets::MODEL_SET_MODERN,
		VR_VEHICLE_CATEGORY_CAR, &modernCarDefaults);
	for(int category = 0; category < VR_VEHICLE_CATEGORY_COUNT; category++){
		VehicleCategoryCalibration &calibration =
			gVehicleCategoryCalibration[category];
		VehicleCategoryCalibration defaults;
		GetBuiltInVehicleCategoryCalibration(GetActiveVehicleModelSet(), category,
			&defaults);
		calibration.seatHeightCm = ReadVehicleCategorySetting(category,
			"SeatHeightCm", hasLegacyDrivingYOffset ? legacyDrivingYOffsetCm :
			defaults.seatHeightCm, -100, 150, path);
		calibration.seatDistanceCm = ReadVehicleCategorySetting(category,
			"SeatDistanceCm", defaults.seatDistanceCm, -100, 100, path);
		calibration.wheelCenterXCm = ReadVehicleCategorySetting(category,
			"WheelCenterXCm", defaults.wheelCenterXCm, -100, 100, path);
		calibration.wheelCenterYCm = ReadVehicleCategorySetting(category,
			"WheelCenterYCm", defaults.wheelCenterYCm, -100, 100, path);
		calibration.wheelCenterZCm = ReadVehicleCategorySetting(category,
			"WheelCenterZCm", defaults.wheelCenterZCm, -100, 100, path);
		calibration.carWheelRadiusCm = category == VR_VEHICLE_CATEGORY_CAR ?
			ReadCarWheelCategoryRadius(path, classicCarDefaults.carWheelRadiusCm,
				modernCarDefaults.carWheelRadiusCm) :
			ReadVehicleCategorySetting(category, "WheelRadiusV2Cm", defaults.carWheelRadiusCm,
				VR_CAR_WHEEL_MIN_RADIUS_CM, VR_CAR_WHEEL_MAX_RADIUS_CM, path);
		calibration.carWheelPitchHalfDeg =
			ReadVehicleCategorySetting(category, "WheelPitchHalfDeg",
				defaults.carWheelPitchHalfDeg,
				-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
				VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG, path);
		calibration.carWheelYawHalfDeg =
			ReadVehicleCategorySetting(category, "WheelYawHalfDeg",
				defaults.carWheelYawHalfDeg,
				-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
				VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG, path);
		calibration.carWheelRollHalfDeg =
			ReadVehicleCategorySetting(category, "WheelRollHalfDeg",
				defaults.carWheelRollHalfDeg,
				-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
				VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG, path);
		calibration.wheelRadiusCm = ReadVehicleCategorySetting(category,
			"WheelRadiusCm", defaults.wheelRadiusCm, -20, 40, path);
		calibration.valid = true;
	}
}
bool IsStereoFrameActive(){ return gFrameBegun && gFramePrepared; }
bool IsHeadRelativeLocomotionActive()
{
	return gFirstPersonEnabled &&
		gMovementOrientation != VR_MOVEMENT_ORIENTATION_BODY &&
		gHeadLocomotionPoseValid && FindPlayerVehicle() == nil &&
		!CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() &&
		IsTrackedScopeGameplaySafe() && !TheCamera.m_WideScreenOn;
}
bool IsHeadDirectedLocomotionActive()
{
	return IsHeadRelativeLocomotionActive() &&
		gMovementOrientation == VR_MOVEMENT_ORIENTATION_HEAD_DIRECTED;
}
bool IsExperimentalHeadTurningActive()
{
	return IsHeadRelativeLocomotionActive() &&
		(gMovementOrientation ==
			VR_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL ||
		 gMovementOrientation ==
			VR_MOVEMENT_ORIENTATION_HEAD_DIRECTED);
}
bool IsHeadBobbingEnabled(){ return gHeadBobbingEnabled; }
bool CanSkipDesktopGameplayRender(){ return gSession!=XR_NULL_HANDLE && gWasSubmitting; }
void ReportDesktopRenderSkip(bool skipping, bool vrReady, bool wideScreen,
	bool cutsceneProcessing, bool cutsceneRunning)
{
	static int lastState = -1;
	const int state = (skipping?1:0)|(vrReady?2:0)|(wideScreen?4:0)|
		(cutsceneProcessing?8:0)|(cutsceneRunning?16:0);
	if(state == lastState)
		return;
	lastState = state;
	VrLog("Desktop gameplay render %s: vrReady=%d wideScreen=%d "
		"cutsceneProcessing=%d cutsceneRunning=%d\n",
		skipping?"skipped":"RUNNING", vrReady?1:0, wideScreen?1:0,
		cutsceneProcessing?1:0, cutsceneRunning?1:0);
}
// Exe-relative vr_settings.ini path for game-side readers; the cwd-relative
// ".\\vr_settings.ini" reads a nonexistent file under launchers that start
// the game with a different working directory.
void PushDynamicLights()
{
#ifdef RW_D3D12
	LoadVrSettings();
	if(gEffectsEnabled){
		rw::d3d12::setEffectsTime((float)(CTimer::GetTimeInMilliseconds()%3600000u)*0.001f);
		if(gModernWaterEnabled && (gWaterSheenPercent > 0 || gWaterGlintPercent > 0)){
			const CVector &sunDirection = CTimeCycle::GetSunDirection();
			const float sun[] = { CTimeCycle::GetSunCoreRed()/255.0f, CTimeCycle::GetSunCoreGreen()/255.0f, CTimeCycle::GetSunCoreBlue()/255.0f };
			const float sky[] = { CTimeCycle::GetSkyTopRed()/255.0f, CTimeCycle::GetSkyTopGreen()/255.0f, CTimeCycle::GetSkyTopBlue()/255.0f };
			rw::d3d12::setWaterLighting(&sunDirection.x, sun, sky);
		}
	}
	const bool modernSurfaces = gEffectsEnabled &&
		CWeather::RainSurfaceMode > 0;
	rw::d3d12::setRainSurfaceSettings(
		modernSurfaces ? CWeather::RainSurfaceWetness : 0.0f,
		modernSurfaces ? CWeather::Rain*CWeather::RainGraphicsIntensity/100.0f : 0.0f,
		modernSurfaces ? CWeather::RainSurfaceMode : 0,
		(float)gPuddleCoveragePercent, (float)gPuddleEdgeSoftnessPercent,
		(float)gPuddleRipplePercent, (float)gPuddleReflectionPercent);
	if(!gEffectsEnabled || !gDynamicLights){
		rw::d3d12::setDynamicPointLights(nil, 0, 0.0f);
		return;
	}

	struct Candidate { float distance; int index; };
	Candidate candidates[NUMPOINTLIGHTS];
	int candidateCount = 0;
	const CVector camPos = TheCamera.GetPosition();
	for(int i = 0; i < CPointLights::NumLights; i++){
		const CRegisteredPointLight &light = CPointLights::aLights[i];
		// Fog-only and DARKEN entries are not additive light sources. Lamps that
		// are currently off still register black entries for legacy shadows.
		if(light.type != CPointLights::LIGHT_POINT &&
		   light.type != CPointLights::LIGHT_DIRECTIONAL)
			continue;
		if(light.radius <= 0.0f ||
		   (light.red <= 0.0f && light.green <= 0.0f && light.blue <= 0.0f))
			continue;
		candidates[candidateCount].distance =
			(light.coors-camPos).Magnitude()-light.radius;
		candidates[candidateCount].index = i;
		candidateCount++;
	}

	// Keep a selected light until a challenger wins by a visible margin. This
	// is the Quest hysteresis that prevents cabins and roads pulsing while two
	// similarly distant street lamps exchange places in the nearest-N set.
	static CVector heldPicks[rw::d3d12::MAX_DYNAMIC_POINT_LIGHTS];
	static int heldPickCount;
	for(int i = 0; i < candidateCount; i++){
		const CVector &coors =
			CPointLights::aLights[candidates[i].index].coors;
		for(int j = 0; j < heldPickCount; j++)
			if((coors-heldPicks[j]).MagnitudeSqr() < 9.0f){
				candidates[i].distance -= 5.0f;
				break;
			}
	}
	for(int i = 1; i < candidateCount; i++){
		const Candidate candidate = candidates[i];
		int j = i-1;
		for(; j >= 0 && candidates[j].distance > candidate.distance; j--)
			candidates[j+1] = candidates[j];
		candidates[j+1] = candidate;
	}

	rw::d3d12::DynamicPointLight lights[
		rw::d3d12::MAX_DYNAMIC_POINT_LIGHTS];
	const int count = Min(candidateCount,
		Min(gDynamicLightMax, (int)rw::d3d12::MAX_DYNAMIC_POINT_LIGHTS));
	const float intensity = gDynamicLightIntensityPercent/100.0f;
	const float boundary = candidateCount > count ?
		candidates[count].distance : 1e9f;
	for(int i = 0; i < count; i++){
		const CRegisteredPointLight &src =
			CPointLights::aLights[candidates[i].index];
		const float fade = Min(1.0f,
			(boundary-candidates[i].distance)/6.0f);
		rw::d3d12::DynamicPointLight &dst = lights[i];
		dst.position[0] = src.coors.x;
		dst.position[1] = src.coors.y;
		dst.position[2] = src.coors.z;
		dst.radius = src.radius;
		dst.colour[0] = src.red*intensity*fade;
		dst.colour[1] = src.green*intensity*fade;
		dst.colour[2] = src.blue*intensity*fade;
		dst.spot = src.type == CPointLights::LIGHT_DIRECTIONAL;
		dst.direction[0] = src.dir.x;
		dst.direction[1] = src.dir.y;
		dst.direction[2] = src.dir.z;
	}
	heldPickCount = count;
	for(int i = 0; i < count; i++)
		heldPicks[i] = CPointLights::aLights[candidates[i].index].coors;
	rw::d3d12::setDynamicPointLights(lights, count,
		gDynamicLights >= 2 ?
		0.14f*(gDynamicLightGlowPercent/100.0f) : 0.0f);
#endif
}

const char *GetVrSettingsFilePath()
{
	return GetVrSettingsPath();
}
// The public twin of VrLog: the internal one sits in the anonymous
// namespace, and varargs cannot be forwarded, so the body is repeated.
// Shares gVrLogStarted so whichever writes first creates the file.
void GameLog(const char *format, ...)
{
#ifdef RW_D3D12
	FILE *file = fopen("openxr_d3d12.log", gVrLogStarted ? "a" : "w");
	if(!file)
		return;
	gVrLogStarted = true;
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fclose(file);
#else
	(void)format;
#endif
}
bool ShouldRouteGameplayHudToVr()
{
	return gSession != XR_NULL_HANDLE && gSessionRunning && gWasSubmitting &&
		gHudColor && gHudDepth &&
		(gGameplayHudVisible || IsTrackedScopeActive()) &&
		gGameState == GS_PLAYING_GAME &&
		!FrontEndMenuManager.m_bGameNotLoaded &&
		!FrontEndMenuManager.m_bMenuActive &&
		!FrontEndMenuManager.m_bWantToRestart &&
		!FrontEndMenuManager.m_bWantToLoad &&
		!CGame::playingIntro && !CCutsceneMgr::IsRunning() &&
		!CCutsceneMgr::IsCutsceneProcessing() &&
		!TheCamera.m_WideScreenOn && FindPlayerPed() != nil;
}
bool UseFullStereoSinglePass(){ return gFullStereoSinglePass; }
bool AreTrackedHandsEnabled(){ return gVrHandsEnabled; }
bool AreWeaponHolsterHighlightsEnabled(){ return gWeaponHolsterHighlightsEnabled; }

void BeginNewGameCinemaHold()
{
	gNewGameCinemaHold = true;
	gNewGameCinemaHoldStartedAt = GetTickCount64();
	debug("[OpenXR] Holding loading cinema until first cutscene frame\n");
}

bool IsNewGameCinemaHoldActive()
{
	if(gNewGameCinemaHold &&
	   GetTickCount64()-gNewGameCinemaHoldStartedAt > 15000ULL){
		gNewGameCinemaHold = false;
		debug("[OpenXR] New-game cinema hold timed out; releasing gameplay\n");
	}
	return gNewGameCinemaHold;
}

void EndNewGameCinemaHold()
{
	if(gNewGameCinemaHold)
		debug("[OpenXR] First cutscene frame ready; releasing cinema hold\n");
	gNewGameCinemaHold = false;
}
bool ShouldUseTrackedHands()
{
	return IsTrackedHandReady(0) || IsTrackedHandReady(1);
}
bool IsTrackedHandReady(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled && gFramePrepared &&
		gTrackedHandPoseValid[hand] &&
		!IsStereoCutsceneActive() && !IsVehicleThirdPersonActive() &&
		!CWorld::Players[CWorld::PlayerInFocus].IsPlayerInRemoteMode() &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}
bool IsTrackedWeaponLaserEnabled(){ return gWeaponLaserEnabled; }
bool IsTrackedWeaponLaserEnabledForType(int weaponType)
{
	const int value = GetWeaponLaserOverride(weaponType);
	return value < 0 ? gWeaponLaserEnabled : value != 0;
}
bool IsHudPanelOnWrist(int panel)
{
	return panel >= 0 && panel < WristHudSettings::PANEL_COUNT &&
		(gWristHud.routingMask & (1u << panel)) != 0;
}
bool IsClassicHudWeaponPanelVisible() { return !ShouldRouteGameplayHudToVr() || gWristHud.classicWeapon; }
bool IsClassicHudClockVisible() { return !ShouldRouteGameplayHudToVr() || gWristHud.classicClock; }
void GetWristAmmoColour(uint8 *red, uint8 *green, uint8 *blue)
{
	if(red) *red = (uint8)gWristHud.ammoColour[0];
	if(green) *green = (uint8)gWristHud.ammoColour[1];
	if(blue) *blue = (uint8)gWristHud.ammoColour[2];
}
bool IsTrackedScopeActive()
{
	return gTrackedScopeHand >= 0 && gTrackedScopeWeaponType >= 0;
}
bool IsTrackedScopeActiveForHand(int hand)
{
	return IsTrackedScopeActive() && gTrackedScopeHand == hand;
}
int GetTrackedScopeWeaponType()
{
	return IsTrackedScopeActive() ? gTrackedScopeWeaponType : -1;
}
bool ShouldUseTrackedWeapon(int hand)
{
	return IsTrackedHandReady(hand) && gHeldWeaponSlot[hand] >= 0 &&
		gTrackedHandAimPoseValid[hand];
}

bool IsTrackedWeaponTriggerPressed(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled &&
		gHeldWeaponSlot[hand] >= 0 && gSessionRunning &&
		gTrackedWeaponTriggerPressed[hand] &&
		gTrackedHandAimPoseValid[hand] &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

bool IsTrackedWeaponTriggerJustPressed(int hand)
{
	return IsTrackedWeaponTriggerPressed(hand) && gTrackedWeaponTriggerJustPressed[hand];
}

bool IsTrackedWeaponTriggerJustReleased(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled &&
		gHeldWeaponSlot[hand] >= 0 && gSessionRunning &&
		gTrackedWeaponTriggerJustReleased[hand] &&
		gTrackedHandAimPoseValid[hand] &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

static int GetTrackedWeaponFireTriggerHand(int weaponHand, int weaponType)
{
	if(weaponHand < 0 || weaponHand >= EYE_COUNT)
		return -1;
	if(weaponType != WEAPONTYPE_ROCKETLAUNCHER || FindPlayerVehicle() != nil)
		return weaponHand;

	const int triggerHand = 1-weaponHand;
	// A free hand and the weapon's support hand both have no owned weapon slot.
	// If it owns another gun, suppress the RPG instead of firing two weapons from
	// one controller edge.
	if(gHeldWeaponSlot[triggerHand] >= 0 ||
	   IsTrackedDetonatorActiveInternal(triggerHand) ||
	   !gTrackedHandPoseValid[triggerHand])
		return -1;
	return triggerHand;
}

bool IsTrackedWeaponFireTriggerPressed(int weaponHand, int weaponType)
{
	const int triggerHand =
		GetTrackedWeaponFireTriggerHand(weaponHand, weaponType);
	return triggerHand >= 0 && gVrHandsEnabled &&
		gHeldWeaponSlot[weaponHand] >= 0 && gSessionRunning &&
		gTrackedWeaponTriggerPressed[triggerHand] &&
		gTrackedHandAimPoseValid[weaponHand] &&
		(FindPlayerVehicle() == nil || IsVrDrivingActiveInternal());
}

bool IsTrackedWeaponFireTriggerJustPressed(int weaponHand, int weaponType)
{
	const int triggerHand =
		GetTrackedWeaponFireTriggerHand(weaponHand, weaponType);
	return triggerHand >= 0 &&
		IsTrackedWeaponFireTriggerPressed(weaponHand, weaponType) &&
		gTrackedWeaponTriggerJustPressed[triggerHand];
}

bool IsTrackedDetonatorActive(int hand)
{
	return IsTrackedDetonatorActiveInternal(hand);
}

bool IsTrackedDetonatorTriggerJustPressed(int hand)
{
	return HasTrackedRemoteChargesInternal() &&
		IsTrackedDetonatorActiveInternal(hand) &&
		gTrackedDetonatorTriggerJustPressed[hand];
}

bool IsTrackedRemoteGrenadeFireActive()
{
	return gActiveTrackedFireAimValid && gActiveTrackedFireHand >= 0 &&
		gActiveTrackedFireWeaponType == WEAPONTYPE_DETONATOR_GRENADE;
}

bool ShouldKeepTrackedRemoteCharges(CEntity *source)
{
	return source != nil && source == FindPlayerPed() &&
		CProjectileInfo::HasDetonatorProjectile(source);
}

void NotifyTrackedRemoteGrenadeThrown(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedDetonatorHand = 1-hand;
	gCalibrationEditHand = gTrackedDetonatorHand;
	debug("[OpenXR] Remote charge armed; detonator assigned to %s hand\n",
		gTrackedDetonatorHand == 0 ? "left" : "right");
}

void NotifyTrackedDetonatorActivated(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedDetonatorTriggerJustPressed[hand] = false;
	gTrackedDetonatorWaitForTriggerRelease[hand] = true;
	debug("[OpenXR] Remote charges detonated from %s hand\n",
		hand == 0 ? "left" : "right");
}

bool IsManualReloadWeaponType(int weaponType)
{
	return IsManualReloadWeaponTypeInternal(weaponType);
}

void SetManualReloadWeaponState(int weaponHand, int slot, int weaponType,
	bool available)
{
	if(weaponHand < 0 || weaponHand >= EYE_COUNT)
		return;
	ManualReloadState &reload = gManualReload[weaponHand];
	available = available && ShouldUseManualReload() &&
		IsManualReloadWeaponTypeInternal(weaponType) &&
		slot > WEAPONSLOT_UNARMED && slot < TOTAL_WEAPON_SLOTS &&
		gHeldWeaponSlot[weaponHand] == slot;
	if(!available){
		ResetManualReloadState(reload);
		return;
	}
	// An empty detachable magazine needs the other hand.  Release a long-gun
	// support grip as soon as physical reload becomes available rather than
	// allowing the same controller to own both interactions.
	if(gWeaponSupportHand[weaponHand] >= 0)
		gWeaponSupportHand[weaponHand] = -1;
	if(!reload.active || reload.slot != slot ||
	   reload.weaponType != weaponType){
		ResetManualReloadState(reload);
		reload.active = true;
		reload.weaponHand = weaponHand;
		reload.slot = slot;
		reload.weaponType = weaponType;
		debug("[OpenXR] Physical reload available for %s hand slot %d type %d\n",
			weaponHand == 0 ? "left" : "right", slot, weaponType);
	}
}

bool ShouldUseManualReload()
{
	return gManualReloadEnabled && gVrHandsEnabled && gSessionRunning &&
		FindPlayerVehicle() == nil;
}

bool ConsumeManualReloadRequest(int weaponHand, int slot, int weaponType)
{
	if(weaponHand < 0 || weaponHand >= EYE_COUNT)
		return false;
	ManualReloadState &reload = gManualReload[weaponHand];
	const bool requested = reload.active && reload.requested &&
		reload.weaponHand == weaponHand && reload.slot == slot &&
		reload.weaponType == weaponType &&
		gHeldWeaponSlot[weaponHand] == slot;
	if(requested)
		ResetManualReloadState(reload);
	return requested;
}

bool GetManualReloadMagazineMatrix(int weaponHand, CMatrix *matrix,
	int *weaponType, bool *held)
{
	if(!matrix || weaponHand < 0 || weaponHand >= EYE_COUNT)
		return false;
	ManualReloadState &reload = gManualReload[weaponHand];
	if(!reload.active || reload.requested || !ShouldUseManualReload())
		return false;
	const bool magazineHeld = reload.magazineHand >= 0;
	const bool ready = magazineHeld ?
		BuildManualReloadHeldMatrix(reload.magazineHand, matrix) :
		BuildManualReloadSpotMatrix(reload.slot, matrix);
	if(!ready)
		return false;
	if(weaponType)
		*weaponType = reload.weaponType;
	if(held)
		*held = magazineHeld;
	return true;
}

void SetWeaponHolsterMask(uint32 mask)
{
	const uint32 ownedMask = mask;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(gHeldWeaponSlot[hand] >= 0 &&
		   (ownedMask & (1u << gHeldWeaponSlot[hand])) == 0){
			// Death/arrest can clear Vice City's inventory while the OpenXR hand
			// still remembers the old slot.  Drop that stale ownership before it
			// can hide a newly restored holster or reuse pre-death melee state.
			ClearWeaponSupportForHand(hand);
			gHeldWeaponSlot[hand] = -1;
			gWeaponHolsterSelection[hand] = -1;
			gTrackedWeaponRenderMatrixSlot[hand] = -1;
			InvalidatePhysicalMeleeWeaponMatrix(hand);
			ResetPhysicalMeleeMotion(hand);
		}
		if(gDroppedWeaponSlot[hand] >= 0 &&
		   (ownedMask & (1u << gDroppedWeaponSlot[hand])) == 0)
			ClearDroppedWeapon(hand);
		if(gHeldWeaponSlot[hand] >= 0)
			mask &= ~(1u << gHeldWeaponSlot[hand]);
		if(gDroppedWeaponSlot[hand] >= 0)
			mask &= ~(1u << gDroppedWeaponSlot[hand]);
	}
	gWeaponHolsterMask = mask;
}

bool ConsumeWeaponHolsterSelection(int hand, int *slot)
{
	if(hand < 0 || hand >= EYE_COUNT || !slot)
		return false;
	*slot = gWeaponHolsterSelection[hand];
	gWeaponHolsterSelection[hand] = -1;
	return *slot >= 0;
}

bool GetWeaponHolsterMatrix(int slot, CMatrix *matrix)
{
	return gVrHandsEnabled && gFramePrepared &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal()) &&
		(gWeaponHolsterMask & (1u << slot)) != 0 &&
		BuildWeaponHolsterMatrix(slot, matrix);
}

bool IsPhysicalGunType(int weaponType)
{
	return IsPhysicalGunTypeInternal(weaponType);
}

bool IsPhysicalMeleeType(int weaponType)
{
	return IsPhysicalMeleeTypeInternal(weaponType);
}

bool IsPhysicalThrowableType(int weaponType)
{
	return IsPhysicalThrowableTypeInternal(weaponType);
}

bool IsPhysicalWeaponType(int weaponType)
{
	return IsPhysicalWeaponTypeInternal(weaponType);
}

bool ConsumePhysicalMeleeStrike(int hand, int *slot, int *weaponType,
	CVector *sweepStart, CVector *sweepEnd, float *speed,
	CVector *rootStart, CVector *rootEnd)
{
	if(hand < 0 || hand >= EYE_COUNT || !slot || !weaponType ||
	   !sweepStart || !sweepEnd || !gPhysicalMeleeStrike[hand].pending)
		return false;
	PhysicalMeleeStrike &strike = gPhysicalMeleeStrike[hand];
	if(CTimer::GetFrameCounter()-strike.frame > 1U){
		strike.pending = false;
		return false;
	}
	*slot = strike.slot;
	*weaponType = strike.weaponType;
	*sweepStart = strike.sweepStart;
	*sweepEnd = strike.sweepEnd;
	if(rootStart)
		*rootStart = strike.rootStart;
	if(rootEnd)
		*rootEnd = strike.rootEnd;
	if(speed)
		*speed = strike.speed;
	strike.pending = false;
	return true;
}

void ResolvePhysicalMeleeStrike(int hand, bool contact)
{
	if(hand < 0 || hand >= EYE_COUNT || !contact)
		return;
	// A miss leaves the swing live, allowing the following 90 Hz sweep segment
	// to reach the target. A real contact latches until the hand slows down,
	// guaranteeing one damage event per deliberate swing.
	gPhysicalMeleeMotion[hand].armed = false;
	gPhysicalMeleeMotion[hand].strikeInProgress = false;
	gPhysicalMeleeMotion[hand].strikePeakSpeed = 0.0f;
	gPhysicalMeleeMotion[hand].strikeContinueUntil = 0;
	gPhysicalMeleeMotion[hand].calmSinceTime = 0;
	gPhysicalMeleeMotion[hand].lastStrikeTime =
		CTimer::GetTimeInMillisecondsNonClipped();
}

bool IsPhysicalWeaponInteractionActive()
{
	// Do not depend on a single frame's pose validity: a transient tracking loss
	// must not let the legacy ammo-empty switch select an invisible inventory gun.
	return gVrHandsEnabled && gSessionRunning &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

bool IsTrackedWeaponHeld(int hand)
{
	return hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled &&
		gHeldWeaponSlot[hand] >= 0 && gSessionRunning &&
		(FindPlayerVehicle() == nil ||
		 IsVrDrivingActiveInternal());
}

int GetHeldWeaponSlot(int hand)
{
	return hand >= 0 && hand < EYE_COUNT ? gHeldWeaponSlot[hand] : -1;
}

void ReleaseTrackedWeaponAfterUse(int hand, int slot)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0 ||
	   gHeldWeaponSlot[hand] != slot)
		return;
	ClearWeaponSupportForHand(hand);
	gHeldWeaponSlot[hand] = -1;
	gWeaponHolsterSelection[hand] = -1;
	gTrackedWeaponRenderMatrixSlot[hand] = -1;
	InvalidatePhysicalMeleeWeaponMatrix(hand);
	ResetPhysicalMeleeMotion(hand);
	ClearDroppedWeapon(hand);
	ResetManualReloadState(gManualReload[hand]);
	gTrackedThrowablePreviewActive[hand] = false;
	// The controller is normally still squeezing grip after releasing R2. Keep
	// the body-point edge latched until a real grip release, so the next grenade
	// cannot jump into the same hand immediately.
	gWeaponHolsterGripDown[hand] = true;
}

void SetTrackedWeaponRenderMatrix(int hand, int slot, int weaponType,
	const CMatrix *matrix, const CMatrix *contactMatrix)
{
	if(hand < 0 || hand >= EYE_COUNT || slot < 0 || !matrix)
		return;
	gTrackedWeaponRenderMatrix[hand] = *matrix;
	gTrackedWeaponRenderMatrixSlot[hand] = slot;
	if(contactMatrix){
		gTrackedWeaponContactMatrix[hand] = *contactMatrix;
		gTrackedWeaponContactMatrixSlot[hand] = slot;
		gTrackedWeaponContactMatrixType[hand] = weaponType;
		gTrackedWeaponContactMatrixFrame[hand] = CTimer::GetFrameCounter();
	}
}

int GetDroppedWeaponSlot(int hand)
{
	return hand >= 0 && hand < EYE_COUNT ? gDroppedWeaponSlot[hand] : -1;
}

bool GetDroppedWeaponMatrix(int slot, CMatrix *matrix)
{
	if(!matrix)
		return false;
	const ULONGLONG now = GetTickCount64();
	for(int hand = 0; hand < EYE_COUNT; hand++){
		if(slot != gDroppedWeaponSlot[hand])
			continue;
		return BuildDroppedWeaponMatrix(hand, now, matrix);
	}
	return false;
}

void GetTrackedWeaponOffset(float *offsetX, float *offsetY, float *offsetZ)
{
	GetTrackedWeaponOffsetForType(gCalibrationEditHand, GetCalibrationWeaponType(),
		offsetX, offsetY, offsetZ);
}

void GetTrackedWeaponRotation(float *rotationX, float *rotationY, float *rotationZ)
{
	GetTrackedWeaponRotationForType(gCalibrationEditHand, GetCalibrationWeaponType(),
		rotationX, rotationY, rotationZ);
}

void GetTrackedWeaponOffsetForType(int hand, int weaponType,
	float *offsetX, float *offsetY, float *offsetZ)
{
	const WeaponCalibration *calibration = GetWeaponCalibration(hand, weaponType);
	if(offsetX) *offsetX = calibration ? (float)calibration->offsetX/200.0f : 0.0f;
	if(offsetY) *offsetY = calibration ? (float)calibration->offsetY/200.0f : 0.0f;
	if(offsetZ) *offsetZ = calibration ? (float)calibration->offsetZ/200.0f : 0.0f;
}

void GetTrackedWeaponRotationForType(int hand, int weaponType,
	float *rotationX, float *rotationY, float *rotationZ)
{
	const WeaponCalibration *calibration = GetWeaponCalibration(hand, weaponType);
	if(rotationX) *rotationX = calibration ? (float)calibration->rotationX/WEAPON_CALIBRATION_VALUE_SCALE : 0.0f;
	if(rotationY) *rotationY = calibration ? (float)calibration->rotationY/WEAPON_CALIBRATION_VALUE_SCALE : 0.0f;
	if(rotationZ) *rotationZ = calibration ? (float)calibration->rotationZ/WEAPON_CALIBRATION_VALUE_SCALE : 0.0f;
}

bool ApplyTrackedWeaponTwoHandTransform(int primaryHand, int weaponType,
	CMatrix *matrix)
{
	if(!matrix || IsTrackedWeaponCalibrationTarget(primaryHand, weaponType))
		return false;
	CVector pivot, axis;
	float angle;
	if(!BuildTwoHandRotation(primaryHand, weaponType, &pivot, &axis, &angle))
		return false;
	if(angle != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(), axis, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(), axis, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(), axis, angle);
		matrix->GetPosition() = pivot+
			RotateAroundAxis(matrix->GetPosition()-pivot, axis, angle);
	}
	return true;
}

bool IsTrackedWeaponCalibrationTarget(int hand, int weaponType)
{
	return gVrMenuVisible && gVrCalibrationMenuVisible &&
		hand == (gCalibrationEditHand == 0 ? 0 : 1) &&
		weaponType == GetCalibrationWeaponType() &&
		IsTwoHandedWeaponTypeInternal(weaponType);
}

bool IsTrackedWeaponSupportFromBelow(int primaryHand, int weaponType)
{
	const SupportGripCalibration *calibration =
		GetSupportGripCalibration(primaryHand, weaponType);
	return calibration &&
		calibration->gripType == VR_SUPPORT_GRIP_FROM_BELOW;
}

bool GetTrackedWeaponSupportAnchor(int primaryHand, int weaponType,
	CVector *position, bool *engaged)
{
	if(!position || primaryHand < 0 || primaryHand >= EYE_COUNT ||
	   gHeldWeaponSlot[primaryHand] < 0)
		return false;
	CVector pivot, expected;
	if(!BuildSupportGripVector(primaryHand, weaponType, &pivot, &expected))
		return false;
	const bool calibrationPreview =
		IsTrackedWeaponCalibrationTarget(primaryHand, weaponType);
	const int supportHand = gWeaponSupportHand[primaryHand] >= 0 ?
		gWeaponSupportHand[primaryHand] : 1-primaryHand;
	const bool supportEngaged = gWeaponSupportHand[primaryHand] == supportHand;
	if(supportEngaged && !calibrationPreview){
		CVector rotationPivot, axis;
		float angle;
		if(!BuildTwoHandRotation(primaryHand, weaponType, &rotationPivot,
		   &axis, &angle))
			return false;
		if(angle != 0.0f)
			expected = RotateAroundAxis(expected, axis, angle);
	}else if(!calibrationPreview){
		if(supportHand < 0 || supportHand >= EYE_COUNT ||
		   gHeldWeaponSlot[supportHand] >= 0 ||
		   !gTrackedHandPoseValid[supportHand])
			return false;
		const CVector supportPosition = gBaseCamera.GetPosition()+
			ToGameVector(gTrackedHandPose[supportHand].position);
		if((supportPosition-(pivot+expected)).Magnitude() > 0.45f)
			return false;
	}
	*position = pivot+expected;
	if(engaged)
		*engaged = !calibrationPreview && supportEngaged;
	return true;
}

bool ApplyTrackedScopeReticleAim(int hand, int weaponType,
	const CVector &muzzle, CVector *direction)
{
	if(!direction || gTrackedScopeHand != hand ||
	   gTrackedScopeWeaponType != weaponType ||
	   (!gPhysicalScopeAimEnabled && weaponType != WEAPONTYPE_CAMERA) ||
	   !gTrackedScopeReticleTargetValid || !IsTrackedScopeGameplaySafe())
		return false;
	CVector convergedDirection = gTrackedScopeReticleTarget-muzzle;
	if(convergedDirection.MagnitudeSqr() < 0.0001f)
		return false;
	convergedDirection.Normalise();
	*direction = convergedDirection;
	return true;
}

bool IsHeadAimWeaponTypeInternal(int weaponType)
{
	// The mission camera retains its physical viewfinder behaviour. Throwables
	// and melee weapons have independent physical interaction paths.
	return weaponType != WEAPONTYPE_CAMERA &&
		IsPhysicalGunTypeInternal(weaponType);
}

bool HasTrackedHeadAimWeaponInternal()
{
	if(!gHeadAimEnabled)
		return false;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const int slot = gHeldWeaponSlot[hand];
		if(slot >= 0 && slot < TOTAL_WEAPON_SLOTS &&
		   IsHeadAimWeaponTypeInternal(GetVrWeaponTypeForSlot(slot)))
			return true;
	}
	return false;
}

bool ApplyTrackedHeadAim(int weaponType, const CVector &muzzle,
	CVector *direction)
{
	if(!direction || !gHeadAimEnabled ||
	   !IsHeadAimWeaponTypeInternal(weaponType) ||
	   !gTrackedScopeReticleTargetValid || !IsTrackedScopeGameplaySafe())
		return false;
	CVector convergedDirection = gTrackedScopeReticleTarget-muzzle;
	if(convergedDirection.MagnitudeSqr() < 0.0001f)
		return false;
	convergedDirection.Normalise();
	*direction = convergedDirection;
	return true;
}

void UpdateTrackedScopeReticleTarget()
{
	gTrackedScopeReticleTargetValid = false;
	const bool physicalScopeNeedsTarget =
		gTrackedScopeHand >= 0 && gTrackedScopeWeaponType >= 0 &&
		(gPhysicalScopeAimEnabled ||
		 gTrackedScopeWeaponType == WEAPONTYPE_CAMERA);
	if((!physicalScopeNeedsTarget && !HasTrackedHeadAimWeaponInternal()) ||
	   !gFramePrepared || !IsTrackedScopeGameplaySafe())
		return;
	const XrVector3f eyeCentre = {
		(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
		(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
		(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f
	};
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const CVector reticleOrigin = gBaseCamera.GetPosition()+
		ToGameVector(eyeCentre);
	CVector reticleForward = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localForward));
	// During a frontend/world transition OpenXR can briefly retain a located-view
	// flag while the pose or restored game camera is being rebuilt. Never pass a
	// non-finite ray into the legacy sector traversal: unlike modern collision
	// code it assumes valid bounded coordinates and can otherwise loop forever.
	if(!_finite(reticleOrigin.x) || !_finite(reticleOrigin.y) ||
	   !_finite(reticleOrigin.z) || !_finite(reticleForward.x) ||
	   !_finite(reticleForward.y) || !_finite(reticleForward.z) ||
	   reticleForward.MagnitudeSqr() < 0.0001f ||
	   reticleOrigin.x <= WORLD_MIN_X+1.0f ||
	   reticleOrigin.x >= WORLD_MAX_X-1.0f ||
	   reticleOrigin.y <= WORLD_MIN_Y+1.0f ||
	   reticleOrigin.y >= WORLD_MAX_Y-1.0f)
		return;
	reticleForward.Normalise();
	CVector reticleTarget = reticleOrigin+reticleForward*150.0f;
	reticleTarget.x = Max(WORLD_MIN_X+1.0f,
		Min(WORLD_MAX_X-1.0f, reticleTarget.x));
	reticleTarget.y = Max(WORLD_MIN_Y+1.0f,
		Min(WORLD_MAX_Y-1.0f, reticleTarget.y));
	CColPoint hitPoint;
	CEntity *hitEntity = nil;
	CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
	CVehicle *playerVehicle = FindPlayerVehicle();
	const bool savedVehicleCollision =
		playerVehicle != nil && playerVehicle->bUsesCollision;
	CWorld::pIgnoreEntity = FindPlayerPed();
	// The physical weapon path already lets shots leave the player's vehicle.
	// Do the same for the HMD convergence ray, otherwise the gaze target can
	// stop on the local windscreen/body before the actual shot is constructed.
	if(playerVehicle != nil)
		playerVehicle->bUsesCollision = false;
	if(CWorld::ProcessLineOfSight(reticleOrigin, reticleTarget, hitPoint,
	   hitEntity, true, true, true, true, true, false, false, false))
		reticleTarget = hitPoint.point;
	if(playerVehicle != nil)
		playerVehicle->bUsesCollision = savedVehicleCollision;
	CWorld::pIgnoreEntity = savedIgnoreEntity;
	gTrackedScopeReticleTarget = reticleTarget;
	gTrackedScopeReticleTargetValid = true;
}

bool BuildTrackedWeaponAimInternal(int hand, int weaponType, CVector *source,
	CVector *direction, bool applyOneHandSway)
{
	if(!source || !direction || hand < 0 || hand >= EYE_COUNT ||
	   !gVrHandsEnabled || gHeldWeaponSlot[hand] < 0 || !gSessionRunning ||
	   !gTrackedHandAimPoseValid[hand] ||
	   (FindPlayerVehicle() != nil &&
	    !IsVrDrivingActiveInternal()))
		return false;
	const WeaponCalibration *calibration = GetWeaponCalibration(hand, weaponType);
	if(!calibration)
		return false;
	const bool calibrationPreview =
		IsTrackedWeaponCalibrationTarget(hand, weaponType);
	const XrPosef &pose = gTrackedHandAimPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	CVector aimForward, aimUp, aimRight;
	CMatrix alignedAim;
	if(BuildAlignedWeaponAimMatrix(hand, weaponType, *calibration,
	   &alignedAim)){
		aimRight = alignedAim.GetRight();
		aimForward = alignedAim.GetForward();
		aimUp = alignedAim.GetUp();
		aimForward.Normalise();
		aimUp -= aimForward*DotProduct(aimUp, aimForward);
		aimUp.Normalise();
		// The OpenXR-to-game aim basis is reflected. Preserve its authored lateral
		// sign just like the unlocked path; reconstructing it with a cross product
		// would silently reverse pitch sway only while AIM ALIGNED is enabled.
		aimRight -= aimForward*DotProduct(aimRight, aimForward);
		aimRight -= aimUp*DotProduct(aimRight, aimUp);
		aimRight.Normalise();
		*source = alignedAim.GetPosition();
		*direction = aimForward;
	}else{
		aimForward = ToGameVector(Rotate(pose.orientation, localForward));
		aimUp = ToGameVector(Rotate(pose.orientation, localUp));
		aimRight = ToGameVector(Rotate(pose.orientation, localRight));
		aimForward.Normalise();
		// Preserve the reflected OpenXR basis while removing numerical skew. Matrix
		// Euler multiplication here previously made calibration axes overlap.
		aimUp -= aimForward*DotProduct(aimUp, aimForward);
		aimUp.Normalise();
		aimRight -= aimForward*DotProduct(aimRight, aimForward);
		aimRight -= aimUp*DotProduct(aimRight, aimUp);
		aimRight.Normalise();
		const float pitch = DEGTORAD((float)calibration->aimRotationX/WEAPON_CALIBRATION_VALUE_SCALE);
		const float yaw = DEGTORAD((float)calibration->aimRotationY/WEAPON_CALIBRATION_VALUE_SCALE);
		const float roll = DEGTORAD((float)calibration->aimRotationZ/WEAPON_CALIBRATION_VALUE_SCALE);
		if(pitch != 0.0f){
			aimForward = RotateAroundAxis(aimForward, aimRight, pitch);
			aimUp = RotateAroundAxis(aimUp, aimRight, pitch);
		}
		if(yaw != 0.0f){
			aimForward = RotateAroundAxis(aimForward, aimUp, yaw);
			aimRight = RotateAroundAxis(aimRight, aimUp, yaw);
		}
		if(roll != 0.0f){
			aimRight = RotateAroundAxis(aimRight, aimForward, roll);
			aimUp = RotateAroundAxis(aimUp, aimForward, roll);
		}
		aimForward.Normalise();
		aimUp -= aimForward*DotProduct(aimUp, aimForward);
		aimUp.Normalise();
		aimRight -= aimForward*DotProduct(aimRight, aimForward);
		aimRight -= aimUp*DotProduct(aimRight, aimUp);
		aimRight.Normalise();
		*direction = aimForward;
		// OpenXR aim pose starts inside the Touch controller. The per-weapon local
		// rotation and XYZ muzzle offsets below are shared by the visible laser and
		// the actual shot, so calibration can never make the two disagree.
		*source = gBaseCamera.GetPosition() + ToGameVector(pose.position) +
			aimRight*((float)calibration->aimOffsetX/200.0f) +
			aimUp*((float)calibration->aimOffsetY/200.0f) +
			*direction*(0.18f + (float)calibration->aimOffsetZ/200.0f);
	}
	CVector pivot, axis;
	float angle;
	const bool supported = !calibrationPreview &&
		BuildTwoHandRotation(hand, weaponType, &pivot, &axis, &angle);
	if(supported && angle != 0.0f){
		*source = pivot+RotateAroundAxis(*source-pivot, axis, angle);
		*direction = RotateAroundAxis(*direction, axis, angle);
		direction->Normalise();
	}
	if(applyOneHandSway && !calibrationPreview &&
	   IsTwoHandedWeaponTypeInternal(weaponType) && !supported){
		// Long guns deliberately become substantially less stable in one hand.
		// The 1.5 multiplier is shared by their real shot and visible hip-fire
		// laser, while scope proximity itself uses the unswayed optical axis.
		const float amplitude = GetOneHandAimSwayDegrees(weaponType)*1.5f;
		const float time = (float)(CTimer::GetTimeInMillisecondsNonClipped() %
			120000U)*0.001f;
		const float phase = weaponType*0.731f + hand*2.173f;
		const float yaw = DEGTORAD(amplitude*(
			0.68f*sinf(3.35f*time+phase) +
			0.32f*sinf(8.91f*time+1.73f*phase+0.4f)));
		const float pitch = DEGTORAD(0.78f*amplitude*(
			0.62f*sinf(2.87f*time+1.31f*phase+1.2f) +
			0.38f*sinf(7.43f*time+0.77f*phase)));
		CVector swayedRight = RotateAroundAxis(aimRight, aimUp, yaw);
		*direction = RotateAroundAxis(*direction, aimUp, yaw);
		*direction = RotateAroundAxis(*direction, swayedRight, pitch);
		direction->Normalise();
	}
	return true;
}

bool GetTrackedWeaponAim(int hand, int weaponType, CVector *source, CVector *direction)
{
	if(!source || !direction || hand < 0 || hand >= EYE_COUNT)
		return false;
	const uint32 frame = CTimer::GetFrameCounter();
	if(gTrackedAimCacheValid[hand] &&
	   gTrackedAimCacheWeaponType[hand] == weaponType &&
	   (!gFramePrepared || gTrackedAimCacheFrame[hand] == frame)){
		*source = gTrackedAimCacheSource[hand];
		*direction = gTrackedAimCacheDirection[hand];
		return true;
	}
	// The game simulation runs before the next OpenXR frame locates its camera
	// and controllers. Rebuilding here would combine a new game-frame number
	// with the previous render's spatial anchor, causing the laser and bullet
	// origin to jump only while firing. If no prepared snapshot exists yet,
	// safely defer the shot until the first rendered VR frame.
	if(!gFramePrepared)
		return false;
	if(!BuildTrackedWeaponAimInternal(hand, weaponType, source, direction, true))
		return false;
	// Scope activation continues to use the untouched physical barrel above, but
	// the final shot/laser converges on the world point under the HMD reticle.
	// Apply it only here, after raw pose/sway and before this frame's aim is cached.
	const CVector physicallySwayedDirection = *direction;
	if(ApplyTrackedHeadAim(weaponType, *source, direction)){
		// Head-relative aiming is an accessibility option, not an accuracy
		// bypass. Preserve the existing one-hand instability of long guns by
		// applying the same physical-barrel angular delta after convergence.
		if(IsTwoHandedWeaponTypeInternal(weaponType)){
			CVector unswayedSource, unswayedDirection;
			if(BuildTrackedWeaponAimInternal(hand, weaponType,
			   &unswayedSource, &unswayedDirection, false)){
				const float dot = Max(-1.0f, Min(1.0f,
					DotProduct(unswayedDirection, physicallySwayedDirection)));
				CVector swayAxis = CrossProduct(
					unswayedDirection, physicallySwayedDirection);
				if(swayAxis.MagnitudeSqr() > 0.000001f){
					swayAxis.Normalise();
					*direction = RotateAroundAxis(*direction, swayAxis,
						acosf(dot));
					direction->Normalise();
				}
			}
		}
	}else
		ApplyTrackedScopeReticleAim(hand, weaponType, *source, direction);
	gTrackedAimCacheValid[hand] = true;
	gTrackedAimCacheFrame[hand] = frame;
	gTrackedAimCacheWeaponType[hand] = weaponType;
	gTrackedAimCacheSource[hand] = *source;
	gTrackedAimCacheDirection[hand] = *direction;
	return true;
}

bool GetTrackedScopeMetrics(int hand, int weaponType, bool relaxed,
	float *score)
{
	if(!IsPhysicalScopeWeaponTypeInternal(weaponType) ||
	   (!gPhysicalScopeAimEnabled && weaponType != WEAPONTYPE_CAMERA) ||
	   !gFramePrepared)
		return false;
	CVector sightPosition, sightDirection;
	if(!BuildTrackedWeaponAimInternal(hand, weaponType, &sightPosition,
	   &sightDirection, false))
		return false;
	if(IsTwoHandedWeaponTypeInternal(weaponType)){
		// A long-gun optic only becomes usable after the saved foregrip socket is
		// actually held by the other controller. Merely moving the free hand near
		// the gun is not sufficient: UpdateWeaponHolsterInput owns that grip state.
		const int supportHand = gWeaponSupportHand[hand];
		if(supportHand < 0 || supportHand >= EYE_COUNT || supportHand == hand ||
		   !gTrackedHandPoseValid[supportHand])
			return false;
	}
	sightDirection.Normalise();
	// Controller poses are located in raw gGameplaySpace. gRenderPose has already
	// been recentered for the game camera, so mixing it with a raw hand pose makes
	// the apparent HMD-to-controller distance include the tracking-origin offset.
	// Use the unmodified located views here so both ends share one XR space.
	const XrVector3f eyeCentre = {
		(gLocatedViews[0].pose.position.x+gLocatedViews[1].pose.position.x)*0.5f,
		(gLocatedViews[0].pose.position.y+gLocatedViews[1].pose.position.y)*0.5f,
		(gLocatedViews[0].pose.position.z+gLocatedViews[1].pose.position.z)*0.5f
	};
	const CVector headPosition = gBaseCamera.GetPosition()+
		ToGameVector(eyeCentre);
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	CVector headForward = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localForward));
	CVector headUp = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localUp));
	CVector headRight = ToGameVector(Rotate(
		gLocatedViews[0].pose.orientation, localRight));
	headForward.Normalise();
	headUp.Normalise();
	headRight.Normalise();
	// Use the controller's physical proximity to the HMD as the primary gesture.
	// The muzzle/laser origin includes per-weapon calibration and can be tens of
	// centimetres away from the actual grip, which made a strict ray-to-eye test
	// impossible to satisfy for several otherwise correctly calibrated weapons.
	const CVector controllerPosition = gBaseCamera.GetPosition()+
		ToGameVector(gTrackedHandAimPose[hand].position);
	const CVector headToController = controllerPosition-headPosition;
	const float controllerDistance = headToController.Magnitude();
	const float controllerForward = DotProduct(headToController, headForward);
	const float controllerHeight = DotProduct(headToController, headUp);
	const float controllerLateral = DotProduct(headToController, headRight);
	const float alignment = DotProduct(headForward, sightDirection);
	const float minimumDistance = relaxed ? 0.04f : 0.06f;
	const float maximumDistance = relaxed ? 0.70f : 0.58f;
	const float minimumForward = relaxed ? -0.03f : 0.04f;
	const float maximumForward = relaxed ? 0.62f : 0.50f;
	// The trigger/grip sits below the optical tube on long guns, so the valid eye
	// band is intentionally asymmetric while still excluding a weapon at chest or
	// waist height. The wider relaxed band prevents flicker after activation.
	const float minimumHeight = relaxed ? -0.32f : -0.24f;
	const float maximumHeight = relaxed ? 0.16f : 0.10f;
	const float maximumLateral = relaxed ? 0.34f : 0.25f;
	const float minimumAlignment = relaxed ? 0.80f : 0.90f;
	if(controllerDistance < minimumDistance || controllerDistance > maximumDistance ||
	   controllerForward < minimumForward || controllerForward > maximumForward ||
	   controllerHeight < minimumHeight || controllerHeight > maximumHeight ||
	   fabsf(controllerLateral) > maximumLateral ||
	   alignment < minimumAlignment)
		return false;
	if(score)
		*score = fabsf(controllerHeight)*1.40f+fabsf(controllerLateral)+
			(1.0f-alignment)*0.35f+
			Max(0.0f, controllerForward-0.38f)*0.20f;
	return true;
}

void SetTrackedScopeStateInternal(int hand, int weaponType)
{
	if(gTrackedScopeHand == hand && gTrackedScopeWeaponType == weaponType)
		return;
	const bool wasActive = gTrackedScopeHand >= 0;
	gTrackedScopeHand = hand;
	gTrackedScopeWeaponType = weaponType;
	gTrackedScopeInvalidSince = 0;
	gTrackedScopeCandidateHand = -1;
	gTrackedScopeCandidateWeaponType = -1;
	gTrackedScopeCandidateSince = 0;
	gTrackedScopeReticleTargetValid = false;
	for(int aimHand = 0; aimHand < EYE_COUNT; aimHand++)
		gTrackedAimCacheValid[aimHand] = false;
	ResetTemporalAaHistory();
	if(hand >= 0)
		debug("[OpenXR] Physical optic active: hand=%d weapon=%d\n",
			hand, weaponType);
	else if(wasActive)
		debug("[OpenXR] Physical optic released\n");
}

void ResetTrackedScopeState()
{
	SetTrackedScopeStateInternal(-1, -1);
	gTrackedScopeCandidateHand = -1;
	gTrackedScopeCandidateWeaponType = -1;
	gTrackedScopeCandidateSince = 0;
	gTrackedScopeInvalidSince = 0;
}

void UpdateTrackedScopeState()
{
	if(!gVrHandsEnabled || !IsTrackedScopeGameplaySafe() ||
	   FindPlayerVehicle() != nil){
		ResetTrackedScopeState();
		return;
	}
	const ULONGLONG now = GetTickCount64();
	if(gTrackedScopeHand >= 0){
		const int slot = gTrackedScopeHand < EYE_COUNT ?
			gHeldWeaponSlot[gTrackedScopeHand] : -1;
		const int heldType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		const bool valid = heldType == gTrackedScopeWeaponType &&
			GetTrackedScopeMetrics(gTrackedScopeHand,
				gTrackedScopeWeaponType, true, nil);
		if(valid){
			gTrackedScopeInvalidSince = 0;
			return;
		}
		if(gTrackedScopeInvalidSince == 0)
			gTrackedScopeInvalidSince = now;
		else if(now-gTrackedScopeInvalidSince >= 150ULL)
			ResetTrackedScopeState();
		return;
	}

	int bestHand = -1;
	int bestWeaponType = -1;
	float bestScore = 1000000.0f;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		const int slot = gHeldWeaponSlot[hand];
		if(slot < 0 || slot >= TOTAL_WEAPON_SLOTS)
			continue;
		const int weaponType = GetVrWeaponTypeForSlot(slot);
		float score = 0.0f;
		if(!GetTrackedScopeMetrics(hand, weaponType, false, &score) ||
		   score >= bestScore)
			continue;
		bestScore = score;
		bestHand = hand;
		bestWeaponType = weaponType;
	}
	if(bestHand < 0){
		gTrackedScopeCandidateHand = -1;
		gTrackedScopeCandidateWeaponType = -1;
		gTrackedScopeCandidateSince = 0;
		return;
	}
	if(gTrackedScopeCandidateHand != bestHand ||
	   gTrackedScopeCandidateWeaponType != bestWeaponType){
		gTrackedScopeCandidateHand = bestHand;
		gTrackedScopeCandidateWeaponType = bestWeaponType;
		gTrackedScopeCandidateSince = now;
		return;
	}
	if(now-gTrackedScopeCandidateSince >= 35ULL)
		SetTrackedScopeStateInternal(bestHand, bestWeaponType);
}

bool GetTrackedThrowableLaunch(int hand, int weaponType, CVector *source,
	CVector *velocity)
{
	if(!source || !velocity || !IsPhysicalThrowableTypeInternal(weaponType))
		return false;
	CVector direction;
	CVector requestedSource;
	if(!GetTrackedWeaponAim(hand, weaponType, &requestedSource, &direction))
		return false;
	return BuildTrackedThrowableLaunch(weaponType, requestedSource, direction,
		source, velocity);
}

void SetTrackedThrowablePreviewActive(int hand, bool active)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return;
	gTrackedThrowablePreviewActive[hand] = active &&
		gHeldWeaponSlot[hand] == WEAPONSLOT_PROJECTILE;
}

bool IsTrackedThrowablePreviewActive(int hand)
{
	return hand >= 0 && hand < EYE_COUNT &&
		gTrackedThrowablePreviewActive[hand] &&
		IsTrackedWeaponTriggerPressed(hand);
}

bool GetTeleportTrajectory(CVector *points, int maximumPoints,
	int *pointCount, bool *targetValid)
{
	if(pointCount)
		*pointCount = 0;
	if(targetValid)
		*targetValid = false;
	if(!points || maximumPoints <= 0 || !gTeleportPreviewActive ||
	   gTeleportTrajectoryCount < 2)
		return false;
	const int count = Min(maximumPoints, gTeleportTrajectoryCount);
	for(int index = 0; index < count; index++)
		points[index] = gTeleportTrajectory[index];
	if(pointCount)
		*pointCount = count;
	if(targetValid)
		*targetValid = gTeleportTargetValid;
	return true;
}

void BeginTrackedWeaponFire(int hand, int weaponType, const CVector &source,
	const CVector &direction)
{
	gActiveTrackedFireHand = hand;
	gActiveTrackedFireWeaponType = weaponType;
	gActiveTrackedFireSource = source;
	gActiveTrackedFireDirection = direction;
	gActiveTrackedFireDirection.Normalise();
	gActiveTrackedFireAimValid = true;
}

void EndTrackedWeaponFire()
{
	gActiveTrackedFireHand = -1;
	gActiveTrackedFireWeaponType = -1;
	gActiveTrackedFireAimValid = false;
}

void NotifyTrackedWeaponFired(int hand, int weaponType)
{
	if(!gWeaponHapticsEnabled || hand < 0 || hand >= EYE_COUNT ||
	   !gSessionRunning || gSession == XR_NULL_HANDLE ||
	   gActions.haptic == XR_NULL_HANDLE ||
	   !IsPhysicalGunTypeInternal(weaponType) ||
	   weaponType == WEAPONTYPE_CAMERA)
		return;

	float amplitude = 0.42f;
	XrDuration duration = 25000000;
	switch(weaponType){
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_HELICANNON:
		amplitude = 0.72f;
		duration = 45000000;
		break;
	case WEAPONTYPE_MINIGUN:
	case WEAPONTYPE_FLAMETHROWER:
		amplitude = 0.28f;
		duration = 18000000;
		break;
	case WEAPONTYPE_COLT45:
		amplitude = 0.34f;
		duration = 22000000;
		break;
	default:
		break;
	}
	amplitude = Min(1.0f, amplitude *
		((float)gWeaponHapticsStrengthPercent/100.0f));

	XrHapticActionInfo actionInfo = { XR_TYPE_HAPTIC_ACTION_INFO };
	actionInfo.action = gActions.haptic;
	actionInfo.subactionPath = gActions.hands[hand];
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.duration = duration;
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude;
	xrApplyHapticFeedback(gSession, &actionInfo,
		(const XrHapticBaseHeader*)&vibration);
}

bool GetActiveTrackedWeaponAim(CVector *source, CVector *direction)
{
	if(!source || !direction || gActiveTrackedFireHand < 0 ||
	   !gActiveTrackedFireAimValid)
		return false;
	*source = gActiveTrackedFireSource;
	*direction = gActiveTrackedFireDirection;
	return true;
}

bool GetActiveTrackedThrowableLaunch(CVector *source, CVector *velocity)
{
	if(!source || !velocity || gActiveTrackedFireHand < 0 ||
	   !gActiveTrackedFireAimValid ||
	   !IsPhysicalThrowableTypeInternal(gActiveTrackedFireWeaponType))
		return false;
	return BuildTrackedThrowableLaunch(gActiveTrackedFireWeaponType,
		gActiveTrackedFireSource, gActiveTrackedFireDirection, source, velocity);
}

bool GetTrackedHandMatrix(int hand, CMatrix *handMatrix, float *grip, float *trigger)
{
	if(!handMatrix || hand < 0 || hand >= EYE_COUNT || !gVrHandsEnabled ||
	   !gFramePrepared || !gTrackedHandPoseValid[hand])
		return false;
	if(gBikeHandleGrabbed[hand] &&
	   BuildBikeHandleMatrixInternal(hand, handMatrix, true)){
		if(grip) *grip = gTrackedHandGrip[hand];
		if(trigger) *trigger = gTrackedHandTrigger[hand];
		return true;
	}
	if(gCarWheelGrabbed[hand] &&
	   BuildCarWheelMatrixInternal(hand, handMatrix, true)){
		// The interaction anchor remains exactly on the authoritative rim. Move
		// only the rendered palm a little outward, then close every finger around
		// the rim; the marker, radius and steering maths are deliberately unchanged.
		const float side = hand == 0 ? -1.0f : 1.0f;
		handMatrix->GetPosition() +=
			handMatrix->GetRight()*(side*0.025f);
		handMatrix->GetPosition() -= handMatrix->GetForward()*(gWheelHandPullBackMm*0.001f);
		const float wheelGrip = Max(gTrackedHandGrip[hand], 0.90f);
		if(grip) *grip = wheelGrip;
		if(trigger) *trigger = Max(gTrackedHandTrigger[hand], wheelGrip);
		return true;
	}
	const XrPosef &pose = gTrackedHandPose[hand];
	const XrVector3f localRight = { 1.0f, 0.0f, 0.0f };
	const XrVector3f localUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	*handMatrix = gBaseCamera;
	handMatrix->GetRight() = ToGameVector(Rotate(pose.orientation, localRight));
	handMatrix->GetUp() = ToGameVector(Rotate(pose.orientation, localUp));
	handMatrix->GetForward() = ToGameVector(Rotate(pose.orientation, localForward));
	handMatrix->GetRight().Normalise();
	handMatrix->GetUp().Normalise();
	handMatrix->GetForward().Normalise();
	handMatrix->GetPosition() = gBaseCamera.GetPosition() + ToGameVector(pose.position);
	// A real two-handed weapon cannot stretch when the controllers are held at
	// slightly different radii. The raw support controller still steers the gun,
	// while its rendered hand is pinned to the calibrated foregrip socket.
	for(int primary = 0; primary < EYE_COUNT; primary++){
		if(gWeaponSupportHand[primary] != hand)
			continue;
		const int slot = gHeldWeaponSlot[primary];
		const int weaponType = slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		CVector supportAnchor;
		bool engaged = false;
		if(GetTrackedWeaponSupportAnchor(primary, weaponType, &supportAnchor,
		   &engaged) && engaged)
			handMatrix->GetPosition() = supportAnchor;
		break;
	}
	if(grip) *grip = gTrackedHandGrip[hand];
	if(trigger) *trigger = gTrackedHandTrigger[hand];
	return true;
}

bool GetTrackedVisualHandMatrix(int hand, CMatrix *handMatrix, float *grip,
	float *trigger)
{
	if(!GetTrackedHandMatrix(hand, handMatrix, grip, trigger))
		return false;
	if(hand >= 0 && hand < EYE_COUNT && gBikeHandleGrabbed[hand])
		ApplyBikeVisualGripAlignment(hand, handMatrix);
	ApplyTwoHandVisualHandLock(hand, handMatrix, nil, nil, grip, trigger);
	return true;
}

bool GetTrackedHandAimRay(int hand, CVector *origin, CVector *direction)
{
	if(!origin || !direction || hand < 0 || hand >= EYE_COUNT || !gVrHandsEnabled ||
	   !gFramePrepared || !gTrackedHandAimPoseValid[hand])
		return false;
	CMatrix handle;
	if(gBikeHandleGrabbed[hand] &&
	   BuildBikeHandleMatrixInternal(hand, &handle, true)){
		*origin = handle.GetPosition();
		*direction = handle.GetForward();
		direction->Normalise();
		return true;
	}
	if(gCarWheelGrabbed[hand] &&
	   BuildCarWheelMatrixInternal(hand, &handle, true)){
		*origin = handle.GetPosition();
		*direction = handle.GetForward();
		direction->Normalise();
		return true;
	}
	const XrPosef &pose = gTrackedHandAimPose[hand];
	const XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	*origin = gBaseCamera.GetPosition() + ToGameVector(pose.position);
	*direction = ToGameVector(Rotate(pose.orientation, localForward));
	direction->Normalise();
	return true;
}

bool GetTrackedVisualHandAimRay(int hand, CVector *origin, CVector *direction)
{
	if(!origin || !direction)
		return false;
	if(hand >= 0 && hand < EYE_COUNT && gVrHandsEnabled && gFramePrepared &&
	   gBikeHandleGrabbed[hand]){
		CMatrix visualHandle;
		if(BuildBikeHandleMatrixInternal(hand, &visualHandle, true)){
			ApplyBikeVisualGripAlignment(hand, &visualHandle);
			*origin = visualHandle.GetPosition();
			*direction = visualHandle.GetForward();
			direction->Normalise();
			return true;
		}
	}
	if(!GetTrackedHandAimRay(hand, origin, direction))
		return false;
	ApplyTwoHandVisualHandLock(hand, nil, origin, direction, nil, nil);
	return true;
}

bool IsImmersiveBikeDrivingActive()
{
	return IsImmersiveBikeDrivingActiveInternal();
}

bool IsImmersiveDrivingActive()
{
	return IsVrDrivingActiveInternal();
}

bool IsImmersiveCarDrivingActive()
{
	return IsImmersiveCarDrivingActiveInternal();
}

bool IsVrBikeDrivingActive()
{
	return IsVrBikeDrivingActiveInternal();
}

bool IsVrCarDrivingActive()
{
	return IsVrCarDrivingActiveInternal();
}

bool IsVrRadioControlActive()
{
	return gVrHandsEnabled && gSessionRunning &&
		(FindPlayerVehicle() != nil || gAttachedMissionWeaponForced);
}

bool ConsumeVrRadioChange()
{
	if(!IsVrRadioControlActive()){
		gVrRadioChangeJustPressed = false;
		return false;
	}
	const bool pressed = gVrRadioChangeJustPressed;
	gVrRadioChangeJustPressed = false;
	return pressed;
}

bool GetImmersiveCarSteering(CVehicle *car, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveCarDrivingActiveInternal(car)){
		*steering = gImmersiveCarSteering;
		return true;
	}
	if(IsMotionDrivingEnvironmentActive() &&
	   IsVrCarDrivingActiveInternal(car)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool GetImmersiveBikeSteering(CVehicle *bike, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveBikeDrivingActiveInternal(bike)){
		*steering = gImmersiveBikeSteering;
		return true;
	}
	if(IsMotionDrivingEnvironmentActive() &&
	   IsVrBikeDrivingActiveInternal(bike)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool GetImmersiveBikeThrottle(CVehicle *bike, float *throttle)
{
	if(!throttle || !gBikeManualThrottle || !IsImmersiveBikeDrivingActiveInternal(bike))
		return false;
	*throttle = gImmersiveBikeThrottle;
	return true;
}

bool GetBikeVisualSteerAngle(CVehicle *bike, float *angle)
{
	if(!angle || !IsImmersiveBikeDrivingActiveInternal(bike) || !_finite(gImmersiveBikePhysicalAngle)) return false;
	*angle = gImmersiveBikePhysicalAngle;
	return true;
}

bool GetImmersiveBikeLean(CVehicle *bike, float *lean)
{
	if(!lean || !IsImmersiveBikeDrivingActiveInternal(bike))
		return false;
	*lean = gImmersiveBikeLean;
	return true;
}

bool GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix)
{
	return BuildBikeHandleMatrixInternal(hand, matrix, true);
}

bool IsImmersiveBikeHandleGrabbed(int hand)
{
	return hand >= 0 && hand < EYE_COUNT &&
		IsImmersiveBikeDrivingActiveInternal() &&
		gBikeHandleGrabbed[hand];
}

bool ShouldRenderImmersiveBikeHandleMarker(int hand)
{
	return hand >= 0 && hand < EYE_COUNT &&
		IsImmersiveBikeDrivingActiveInternal() &&
		(gVrBikeCalibrationMenuVisible ||
		 gBikeHandleHighlightsEnabled);
}

bool GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix)
{
	if(IsVrCarDrivingActiveInternal())
		return BuildCarWheelMatrixInternal(hand, matrix, true);
	return BuildBikeHandleMatrixInternal(hand, matrix, true);
}

bool IsImmersiveSteeringHandleGrabbed(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return false;
	if(IsImmersiveCarDrivingActiveInternal())
		return gCarWheelGrabbed[hand];
	return IsImmersiveBikeHandleGrabbed(hand);
}

bool ShouldRenderImmersiveSteeringHandleMarker(int hand)
{
	if(hand < 0 || hand >= EYE_COUNT)
		return false;
	if(IsImmersiveCarDrivingActiveInternal())
		return gVrBikeCalibrationMenuVisible ||
			(FindPlayerVehicle()->IsBoat() ? gBoatHandleHighlightsEnabled : gCarHandleHighlightsEnabled);
	return ShouldRenderImmersiveBikeHandleMarker(hand);
}

bool ShouldRenderImmersiveCarWheel()
{
	if(!IsVrCarDrivingActiveInternal())
		return false;
	CVehicle *car = GetActivePlayerCar();
	if(!car || !(car->IsBoat() ? gImmersiveBoatWheelVisible : gImmersiveCarWheelVisible)) return false;
	VehicleViewCalibration *calibration = car ?
		GetVehicleViewCalibration(car->GetModelIndex()) : nil;
	if(calibration && calibration->carWheelVisibilityOverride >= 0)
		return calibration->carWheelVisibilityOverride != 0;
	return true;
}

bool GetImmersiveCarWheelPose(CVector *center, CVector *right,
	CVector *up, CVector *normal, float *radius)
{
	if(!center || !right || !up || !normal || !radius ||
	   !IsVrCarDrivingActiveInternal())
		return false;
	CarWheelPose pose;
	if(!BuildCarWheelPose(GetActivePlayerCar(), &pose))
		return false;
	const float physicalAngle = IsImmersiveCarDrivingActiveInternal() ?
		gImmersiveCarPhysicalAngle : gMotionVehiclePhysicalAngle;
	const float visualAngle = -physicalAngle;
	*center = pose.center;
	*normal = pose.normal;
	*right = pose.right;
	*up = pose.up;
	if(visualAngle != 0.0f){
		*right = RotateAroundAxis(*right, *normal, visualAngle);
		*up = RotateAroundAxis(*up, *normal, visualAngle);
	}
	*radius = pose.radius;
	return true;
}

void UpdateImmersiveCarModelSteeringWheel(CVehicle *vehicle)
{
	UpdateImmersiveCarModelSteeringWheelInternal(vehicle);
}

bool IsVrBikeHorizonLocked()
{
	return gBikeLockHorizonEnabled;
}

bool IsImmersiveBikeSidearm(int weaponType)
{
	return IsBikeSidearmTypeInternal(weaponType);
}

bool IsImmersiveVehicleSidearm(int weaponType)
{
	return IsBikeSidearmTypeInternal(weaponType);
}

bool SubmitStereoFrame(RwCamera *camera)
{
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(-1);
#endif
	if(!gFramePrepared || !gFrameBegun) return false;
	if(!gDlssProfileToastPresented && Dlaa::IsNeuralRenderingEnabled()){
		gDlssProfileToastPresented = true;
		ShowDlssProfileToast();
	}
	if(gVrAboutFirstRunPending && DidPlayerTakeFirstRunWelcomeStep()){
		gVrAboutFirstRunPending = false;
		ResetFirstRunWelcomeStepBaseline();
		gVrAboutVisible = true;
		gVrAboutDismissArmed = false;
		gVrAboutReleaseGate = false;
		gVrAboutWasRendered = false;
	}
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitStereoFrame begin\n");
	// World rendering is complete and its collision lists are stable here. Cache
	// the point under the HMD reticle for next-frame weapon input; never perform
	// this query from loading/menu weapon updates while the world is rebuilding.
	UpdateTrackedScopeReticleTarget();
#ifdef RW_D3D12
	if(gEffectsEnabled &&
	   ((gCarReflectionPercent > 0 && gCarReflectionSsrPercent > 0) || (gModernWaterEnabled && gWaterReflectionPercent > 0) ||
	    (CWeather::RainSurfaceMode >= 2 &&
	     gPuddleReflectionPercent > 0 && CWeather::RainSurfaceWetness > 0.001f)) &&
	   gEye[0].color != nil){
		rw::Raster *reflectionSource =
			((rw::Raster*)gEye[0].color)->parent;
		if(reflectionSource != nil){
			const bool reflectionCaptured =
				rw::d3d12::captureScreenSpaceReflectionFrame(reflectionSource) != 0;
			if(reflectionCaptured && gScreenSpaceReflectionFailed)
				VrLog("Screen-space reflection capture recovered\n");
			else if(!reflectionCaptured && !gScreenSpaceReflectionFailed)
				VrLog("Screen-space reflection capture failed; check Graphics status\n");
			gScreenSpaceReflectionFailed = !reflectionCaptured;
		}
	}else{
		// Retire history when all reflection features are explicitly disabled.
		// The renderer retains it during merely dry frames to avoid churn.
		rw::d3d12::captureScreenSpaceReflectionFrame(nil);
		gScreenSpaceReflectionFailed = false;
	}
	const bool dlaaWarmupFrame =
		gTemporalAaBackend == TEMPORAL_AA_DLAA &&
		!gDlaaStereoActivationReady;
	if(dlaaWarmupFrame && gDlaaStereoWarmupFrames > 0)
		gDlaaStereoWarmupFrames--;
	const bool dlaaFrame =
		gTemporalAaBackend == TEMPORAL_AA_DLAA &&
		gDlaaStereoActivationReady && !gDlaaStereoActivationFailed &&
		Dlaa::BeginFrame(
		gTemporalJitterX, gTemporalJitterY);
	const bool fsr2Frame =
		gTemporalAaBackend == TEMPORAL_AA_FSR2 &&
		!gFsr2StereoActivationFailed &&
		Fsr2::BeginFrame(gTemporalJitterX, gTemporalJitterY);
	Fsr2::EyeOutput fsr2Outputs[EYE_COUNT] = {};
	const bool fsr2Evaluated = fsr2Frame &&
		EvaluateD3D12Fsr2Frame(camera, fsr2Outputs);
#endif
	for(int eye=0;eye<EYE_COUNT;eye++){
		bool copied = false;
#ifdef RW_D3D12
		if(fsr2Evaluated)
			copied = DrawEyeFsr2Output(gEye[eye], eye,
				fsr2Outputs[eye]);
		else if(dlaaFrame)
			copied = DrawEyeDlaa(gEye[eye], eye, camera);
#endif
		if(!copied)
			copied=DrawEyeFxaa(gEye[eye],eye);
#ifndef RW_D3D12
		if(!copied)
			copied=CopyRasterToSwapchain(gEye[eye].color,gEye[eye].renderWidth,gEye[eye].renderHeight,
				gEye[eye].swapchain,gAntiAliasingEnabled?GL_LINEAR:GL_NEAREST);
#endif
		if(!copied){
			RestoreCamera(camera);
#ifdef RW_D3D12
			FinishD3D12SwapchainWrites();
#endif
			EndXrFrame(nil,0); return false;
		}
	}
#ifdef RW_D3D12
	// Temporal reconstruction and the opaque world resolve are complete. Draw
	// controller-driven geometry now, against the preserved world depth, into a
	// transparent scratch image, then alpha-composite it over the already
	// acquired world swapchain. It never enters temporal history.
	RenderTrackedForeground(camera);
#endif
	RestoreCamera(camera);
#ifdef RW_D3D12
	if(dlaaFrame && !Dlaa::WasLastEvaluationSuccessful()){
		gDlaaStereoActivationReady = false;
		gDlaaStereoActivationFailed = true;
		VrLog("DLAA real evaluation failed: %s; retaining FXAA fallback\n",
			Dlaa::GetStatus());
	}
	if(fsr2Frame && !fsr2Evaluated){
		gFsr2StereoActivationFailed = true;
		gTemporalAaReleasePending |= TEMPORAL_AA_RELEASE_FSR2;
		VrLog("FSR2 evaluation failed: %s; retaining FXAA fallback\n",
			Fsr2::GetStatus());
	}
#endif
	const bool showHud=UpdateHudSwapchain(camera);
	const bool showDebug=UpdateDebugSwapchain();
	const bool showVrMenu=UpdateVrMenuSwapchain();
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitStereoFrame copies done hud=%d debug=%d\n", showHud ? 1 : 0,
			showDebug ? 1 : 0);
#ifdef RW_D3D12
	const bool prepareDlaaAfterSubmit =
		dlaaWarmupFrame && gDlaaStereoWarmupFrames == 0 &&
		!gDlaaStereoActivationFailed;
	if(!FinishD3D12SwapchainWrites()){
		EndXrFrame(nil,0);
		return false;
	}
	if(prepareDlaaAfterSubmit){
		// Streamline creates the DLAA feature lazily on the first real
		// evaluation. Let the cinema-to-world transition settle first, submit
		// several ordinary stereo frames, then drain the queue. The next frame
		// follows the original working EvaluateEye path with valid color/depth
		// tags while the GPU starts from a known idle state.
		gDlaaStereoActivationReady = rw::d3d12::waitForGpu() != 0;
		gDlaaStereoActivationFailed = !gDlaaStereoActivationReady;
		if(gDlaaStereoActivationReady)
			VrLog("DLAA activation barrier complete; real evaluation starts next frame\n");
		else
			VrLog("DLAA activation barrier failed; retaining FXAA fallback\n");
	}
#endif
	XrCompositionLayerProjectionView projectionViews[EYE_COUNT];
	for(int eye=0;eye<EYE_COUNT;eye++){
		projectionViews[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		projectionViews[eye].pose=gLocatedViews[eye].pose;
		projectionViews[eye].fov=gRenderFov[eye];
		projectionViews[eye].subImage.swapchain=gEye[eye].swapchain.handle;
		projectionViews[eye].subImage.imageRect.offset={0,0};
		projectionViews[eye].subImage.imageRect.extent={gEye[eye].swapchain.width,gEye[eye].swapchain.height};
		projectionViews[eye].subImage.imageArrayIndex=0;
	}
	XrCompositionLayerProjection projection={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	projection.space=gGameplaySpace; projection.viewCount=EYE_COUNT; projection.views=projectionViews;
	XrCompositionLayerQuad hud={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showHud){
		hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		hud.space=gViewSpace; hud.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		hud.subImage.swapchain=gHudSwapchain.handle; hud.subImage.imageRect.extent={VR_HUD_WIDTH,VR_HUD_HEIGHT};
		hud.pose.orientation.w=1.0f;
		const bool trackedScope = IsTrackedScopeActive();
		hud.pose.position.x=trackedScope?0.0f:gHudOffsetXCm*0.01f;
		hud.pose.position.y=trackedScope?0.0f:gHudOffsetYCm*0.01f;
		hud.pose.position.z=-1.6f;
		if(trackedScope){
			// Cover the complete Quest view while an opaque optic mask is active.
			hud.size.width=5.7f;
			hud.size.height=3.2f;
		}else{
			const float hudScale=gHudScalePercent/100.0f;
			const float hudWidthScale=gHudWidthPercent/100.0f;
			hud.size.width=1.8f*hudScale*hudWidthScale;
			hud.size.height=1.8f*hudScale*
				(float)VR_HUD_HEIGHT/VR_HUD_WIDTH;
		}
	}
	XrCompositionLayerQuad debugLayer={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showDebug){
		debugLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		debugLayer.space=gViewSpace; debugLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		debugLayer.subImage.swapchain=gDebugSwapchain.handle;
		debugLayer.subImage.imageRect.extent={VR_DEBUG_WIDTH,VR_DEBUG_HEIGHT};
		debugLayer.pose.orientation.w=1.0f;
		debugLayer.pose.position.y=-0.24f; debugLayer.pose.position.z=-1.5f;
		debugLayer.size.width=1.2f;
		debugLayer.size.height=1.2f*(float)VR_DEBUG_HEIGHT/VR_DEBUG_WIDTH;
	}
	XrCompositionLayerQuad vrMenuLayer={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showVrMenu){
		const bool compactDlssToast =
			!gVrMenuVisible && !gCheatMenuVisible && !gVrAboutVisible;
		vrMenuLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		vrMenuLayer.space=gViewSpace; vrMenuLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		vrMenuLayer.subImage.swapchain=gVrMenuSwapchain.handle;
		vrMenuLayer.subImage.imageRect.extent={VR_MENU_WIDTH,VR_MENU_HEIGHT};
		vrMenuLayer.pose.orientation.w=1.0f;
		vrMenuLayer.pose.position.y=compactDlssToast ? 0.20f : 0.0f;
		vrMenuLayer.pose.position.z=-1.65f;
		vrMenuLayer.size.width=compactDlssToast ? 1.65f : 2.15f;
		vrMenuLayer.size.height=vrMenuLayer.size.width*
			(float)VR_MENU_HEIGHT/VR_MENU_WIDTH;
	}
	const XrCompositionLayerBaseHeader *layers[4+WristHudSettings::PANEL_COUNT]; uint32_t count=0;
	layers[count++]=(const XrCompositionLayerBaseHeader*)&projection;
	if(showHud) layers[count++]=(const XrCompositionLayerBaseHeader*)&hud;
	if(showHud)
		for(int panel = 0; panel < WristHudSettings::PANEL_COUNT; panel++)
			if(gWristHudLayerMask & (1u << panel))
				layers[count++]=(const XrCompositionLayerBaseHeader*)&gWristHudLayers[panel];
	if(showDebug) layers[count++]=(const XrCompositionLayerBaseHeader*)&debugLayer;
	if(showVrMenu) layers[count++]=(const XrCompositionLayerBaseHeader*)&vrMenuLayer;
	return EndXrFrame(layers,count);
}

#ifdef RW_D3D12
// Stage cinema from the backbuffer and submit it to both eyes as a stereo
// projection layer, avoiding SteamVR's quad-only throttling.
// Returns 1 = submitted, 0 = not attempted (classic quad path should run),
// -1 = failed after the XR frame was committed (caller must just fail).
int SubmitTheaterCinemaFrame(int width, int height,
	int contentWidth, int contentHeight, bool holdLastFrame, bool trace)
{
	if(!gCinemaTheaterMode || !gLocalSpace ||
	   contentWidth <= 0 || contentHeight <= 0)
		return 0;
	// Anchor exactly where the quad layer would sit.
	XrCompositionLayerQuad anchorProbe = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	ApplyCinemaQuadPose(&anchorProbe);
	if(anchorProbe.space != gLocalSpace)
		return 0;	// tracking not up yet; the classic path covers this frame
	if(!holdLastFrame){
		if(gCinemaStagingTexture &&
		   (gCinemaStagingWidth != width || gCinemaStagingHeight != height)){
			rw::d3d12::deferRelease(gCinemaStagingTexture);
			gCinemaStagingTexture = nil;
		}
		if(!gCinemaStagingTexture){
			if(!rw::d3d12::createStagingColorTexture(width, height,
			   &gCinemaStagingTexture))
				return 0;
			gCinemaStagingWidth = width;
			gCinemaStagingHeight = height;
		}
		if(trace) VrLog("Cinema trace: theater stage copy\n");
		// With the frame closed (menus) this copies the last presented
		// image through an independent list with CPU waits -- the exact
		// years-old menu path, and no XR image is acquired yet. With the
		// frame open (cutscenes) it records into the frame list.
		if(!rw::d3d12::copyCurrentBackBufferToExternal(gCinemaStagingTexture))
			return 0;
	}else if(!gCinemaStagingTexture)
		return 0;
	// Everything below records into the standard frame command list.
	if(trace) VrLog("Cinema trace: theater ensure frame\n");
	if(!rw::d3d12::ensureFrameOpen())
		return 0;
	// The menu overlay upload records into the open frame list.
	const bool showVrMenu = UpdateVrMenuSwapchain();
	XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = gFrameState.predictedDisplayTime;
	locate.space = gLocalSpace;
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = 0;
	XrView located[EYE_COUNT] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	if(trace) VrLog("Cinema trace: theater locate views\n");
	if(!XrOk(xrLocateViews(gSession, &locate, &viewState, EYE_COUNT,
	   &viewCount, located), "xrLocateViews(theater)") ||
	   viewCount != EYE_COUNT ||
	   (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0 ||
	   (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0)
		return 0;
	// World-space corners of the theater screen (TL, TR, BL, BR), the same
	// size and pose the quad layer used.
	const float quadWidth = 3.2f;
	const float quadHeight = 3.2f*(float)contentHeight/(float)contentWidth;
	const XrVector3f localCorners[4] = {
		{ -quadWidth*0.5f,  quadHeight*0.5f, 0.0f },
		{  quadWidth*0.5f,  quadHeight*0.5f, 0.0f },
		{ -quadWidth*0.5f, -quadHeight*0.5f, 0.0f },
		{  quadWidth*0.5f, -quadHeight*0.5f, 0.0f },
	};
	XrVector3f worldCorners[4];
	for(int i = 0; i < 4; i++){
		const XrVector3f rotated = Rotate(anchorProbe.pose.orientation,
			localCorners[i]);
		worldCorners[i].x = anchorProbe.pose.position.x + rotated.x;
		worldCorners[i].y = anchorProbe.pose.position.y + rotated.y;
		worldCorners[i].z = anchorProbe.pose.position.z + rotated.z;
	}
	if(trace) VrLog("Cinema trace: theater eye draws\n");
	for(int eye = 0; eye < EYE_COUNT; eye++){
		const XrPosef &viewPose = located[eye].pose;
		const XrQuaternionf inverseOrientation = { -viewPose.orientation.x,
			-viewPose.orientation.y, -viewPose.orientation.z,
			viewPose.orientation.w };
		const XrFovf &fov = located[eye].fov;
		const float tanLeft = tanf(fov.angleLeft);
		const float tanRight = tanf(fov.angleRight);
		const float tanUp = tanf(fov.angleUp);
		const float tanDown = tanf(fov.angleDown);
		if(tanRight-tanLeft < 0.0001f || tanUp-tanDown < 0.0001f)
			return 0;
		const float projX = 2.0f/(tanRight-tanLeft);
		const float projShiftX = (tanRight+tanLeft)/(tanRight-tanLeft);
		const float projY = 2.0f/(tanUp-tanDown);
		const float projShiftY = (tanUp+tanDown)/(tanUp-tanDown);
		float corners[16];
		for(int i = 0; i < 4; i++){
			const XrVector3f offset = {
				worldCorners[i].x - viewPose.position.x,
				worldCorners[i].y - viewPose.position.y,
				worldCorners[i].z - viewPose.position.z };
			const XrVector3f viewCorner = Rotate(inverseOrientation, offset);
			// Standard asymmetric XR projection. w carries perspective, so
			// the quad's texture interpolates correctly at any angle.
			const float clipW = -viewCorner.z;
			corners[i*4+0] = projX*viewCorner.x + projShiftX*viewCorner.z;
			corners[i*4+1] = projY*viewCorner.y + projShiftY*viewCorner.z;
			corners[i*4+2] = 0.5f*clipW;
			corners[i*4+3] = clipW;
		}
		if(!AcquireSwapchain(gEye[eye].swapchain) ||
		   !rw::d3d12::drawCinemaQuadToExternal(gCinemaStagingTexture,
			AcquiredTexture(gEye[eye].swapchain),
			gEye[eye].swapchain.width, gEye[eye].swapchain.height,
			corners)){
			// Releases whatever was acquired; the XR frame is still open,
			// so the classic quad path can take over this frame.
			FinishD3D12SwapchainWrites();
			return 0;
		}
	}
	if(trace) VrLog("Cinema trace: theater finish writes\n");
	if(!FinishD3D12SwapchainWrites()){
		EndXrFrame(nil, 0);
		return -1;
	}
	XrCompositionLayerProjectionView projectionViews[EYE_COUNT];
	for(int eye = 0; eye < EYE_COUNT; eye++){
		projectionViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		projectionViews[eye].pose = located[eye].pose;
		projectionViews[eye].fov = located[eye].fov;
		projectionViews[eye].subImage.swapchain = gEye[eye].swapchain.handle;
		projectionViews[eye].subImage.imageRect.extent = {
			gEye[eye].swapchain.width, gEye[eye].swapchain.height };
	}
	XrCompositionLayerProjection projection = {
		XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	projection.space = gLocalSpace;
	projection.viewCount = EYE_COUNT;
	projection.views = projectionViews;
	XrCompositionLayerQuad vrMenuLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	if(showVrMenu){
		const bool compactDlssToast =
			!gVrMenuVisible && !gCheatMenuVisible && !gVrAboutVisible;
		vrMenuLayer.layerFlags =
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		vrMenuLayer.space = gViewSpace;
		vrMenuLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		vrMenuLayer.subImage.swapchain = gVrMenuSwapchain.handle;
		vrMenuLayer.subImage.imageRect.extent = { VR_MENU_WIDTH, VR_MENU_HEIGHT };
		vrMenuLayer.pose.orientation.w = 1.0f;
		vrMenuLayer.pose.position.y = compactDlssToast ? 0.20f : 0.0f;
		vrMenuLayer.pose.position.z = -1.65f;
		vrMenuLayer.size.width = compactDlssToast ? 1.65f : 2.15f;
		vrMenuLayer.size.height = vrMenuLayer.size.width*
			(float)VR_MENU_HEIGHT/VR_MENU_WIDTH;
	}
	const XrCompositionLayerBaseHeader *layers[2];
	uint32 count = 0;
	layers[count++] = (const XrCompositionLayerBaseHeader*)&projection;
	if(showVrMenu)
		layers[count++] = (const XrCompositionLayerBaseHeader*)&vrMenuLayer;
	if(trace) VrLog("Cinema trace: theater end frame layers=%u\n", count);
	const bool ended = EndXrFrame(layers, count);
	if(trace) VrLog("Cinema trace: theater end ok=%d\n", ended ? 1 : 0);
	if(!ended)
		return -1;
	gCinemaFrameValid = true;
	gCinemaContentWidth = contentWidth;
	gCinemaContentHeight = contentHeight;
	return 1;
}
#endif

bool SubmitCinemaFrame(RwCamera *camera, bool holdLastFrame)
{
	SetFramePhase(holdLastFrame ? "cinema-hold" : "cinema");
	// Step trace for the first cinema frames of a session. Each VrLog opens
	// and closes the file, so when a step blocks forever the last line in
	// the log names it. Field logs cannot rely on gVrLoggedRenderableFrames
	// here -- the startup movies exhaust that counter before cinema starts.
	static int gCinemaTraceFrames;
	const bool traceCinema = !holdLastFrame && gCinemaTraceFrames < 3;
	if(traceCinema)
		gCinemaTraceFrames++;
	if(traceCinema) VrLog("Cinema trace: reset TAA history\n");
	// A cinema/loading frame marks a hard boundary for physical optics. Never
	// carry an old weapon/reticle target into a frontend restart or world rebuild.
	ResetTrackedScopeState();
	ResetTemporalAaHistory();
	if(!gDlaaStereoActivationReady && !gDlaaStereoActivationFailed)
		gDlaaStereoWarmupFrames = DLAA_ACTIVATION_WARMUP_FRAMES;
	if(traceCinema) VrLog("Cinema trace: begin xr frame\n");
	if(!camera || !BeginXrFrame()) return false;
	if(traceCinema) VrLog("Cinema trace: xr frame open shouldRender=%d\n",
		gFrameState.shouldRender ? 1 : 0);
	if(!gFrameState.shouldRender){ EndXrFrame(nil,0); return false; }
	gConsecutiveStereoFrames = 0;
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitCinemaFrame begin\n");
	// Once-a-second pace line while flat content is on screen. Separates the
	// look-alike "cutscene jitter" reports: low fresh count = content
	// updating too rarely (judder), view-fallback frames = the screen riding
	// the head, anchor resets = the screen re-centering between segments.
	{
		static ULONGLONG cinemaPaceWindowStart;
		static uint32 cinemaPaceFresh, cinemaPaceHeld;
		static float cinemaPaceWaitMs;
		const ULONGLONG paceNow = GetTickCount64();
		if(cinemaPaceWindowStart == 0)
			cinemaPaceWindowStart = paceNow;
		if(holdLastFrame) cinemaPaceHeld++; else cinemaPaceFresh++;
		cinemaPaceWaitMs += gLastXrWaitFrameMs;
		if(paceNow-cinemaPaceWindowStart >= 1000){
			const uint32 paceFrames = cinemaPaceFresh+cinemaPaceHeld;
			VrLog("Cinema pace: %u fresh, %u held, %u view-fallback, "
				"%u anchor resets, wait avg %.1fms in %ums\n",
				cinemaPaceFresh, cinemaPaceHeld,
				gCinemaViewFallbackFrames, gCinemaAnchorResets,
				paceFrames > 0 ? cinemaPaceWaitMs/paceFrames : 0.0f,
				(uint32)(paceNow-cinemaPaceWindowStart));
			cinemaPaceFresh = cinemaPaceHeld = 0;
			cinemaPaceWaitMs = 0.0f;
			gCinemaViewFallbackFrames = 0;
			gCinemaAnchorResets = 0;
			cinemaPaceWindowStart = paceNow;
		}
	}
	if(holdLastFrame){
#ifdef RW_D3D12
		if(gCinemaTheaterMode && gCinemaFrameValid){
			const int theater = SubmitTheaterCinemaFrame(0, 0,
				gCinemaContentWidth, gCinemaContentHeight, true, false);
			if(theater != 0)
				return theater > 0;
		}
#endif
		if(!gCinemaFrameValid || !gCinemaQuadImageValid ||
		   !gCinemaSwapchain.handle ||
		   gCinemaContentWidth <= 0 || gCinemaContentHeight <= 0){
			// Never reveal the partially unloaded world. A missing previous cinema
			// image is rare (first frame/session loss), and black is the safe fallback.
			EndXrFrame(nil, 0);
			return false;
		}
		XrCompositionLayerQuad cinema = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		ApplyCinemaQuadPose(&cinema);
		cinema.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		cinema.subImage.swapchain = gCinemaSwapchain.handle;
		cinema.subImage.imageRect.extent = {
			gCinemaContentWidth, gCinemaContentHeight
		};
		cinema.size.width = 3.2f;
		cinema.size.height = 3.2f*(float)gCinemaContentHeight/
			(float)gCinemaContentWidth;
		const XrCompositionLayerBaseHeader *layers[] = {
			(const XrCompositionLayerBaseHeader*)&cinema
		};
		return EndXrFrame(layers, 1);
	}
#ifdef RW_D3D12
	int width = 0, height = 0;
	rw::d3d12::getPresentSize(&width, &height);
#else
	GLint viewport[4]={}; glGetIntegerv(GL_VIEWPORT,viewport);
	const int width=viewport[2], height=viewport[3];
#endif
	if(width<=0 || height<=0){ EndXrFrame(nil,0); return false; }
	if(!gCinemaSwapchain.handle){ EndXrFrame(nil,0); return false; }
	const int contentWidth = Min(width, gCinemaSwapchain.width);
	const int contentHeight = Min(height, gCinemaSwapchain.height);
#ifdef RW_D3D12
	if(gCinemaTheaterMode){
		const int theater = SubmitTheaterCinemaFrame(width, height,
			contentWidth, contentHeight, false, traceCinema);
		if(theater != 0)
			return theater > 0;
		if(traceCinema) VrLog("Cinema trace: theater fell back to quad\n");
	}
#endif
	if(traceCinema) VrLog("Cinema trace: acquire cinema swapchain\n");
	if(!AcquireSwapchain(gCinemaSwapchain)){ EndXrFrame(nil,0); return false; }
	gCinemaFrameValid = false;
	bool showVrMenu = false;
	bool cinemaEyesReady = false;
#ifdef RW_D3D12
	// SteamVR throttles xrWaitFrame towards ~15Hz once it believes the
	// application stopped rendering, and after the first stereo frame a
	// session submitting only the theater quad qualifies — every following
	// menu and cutscene then runs its whole game loop at ~15fps, which is
	// the exact "cutscene judder" players reported. Keeping a cleared
	// projection layer underneath the quad is visually identical (the same
	// black void around the screen) but keeps full-rate pacing.
	if(traceCinema) VrLog("Cinema trace: copy backbuffer\n");
	const bool cinemaCopied = rw::d3d12::copyCurrentBackBufferToExternal(
		AcquiredTexture(gCinemaSwapchain)) != 0;
	if(traceCinema) VrLog("Cinema trace: copy done=%d, vr menu update\n",
		cinemaCopied ? 1 : 0);
	showVrMenu = UpdateVrMenuSwapchain();
	if(traceCinema) VrLog("Cinema trace: vr menu done=%d\n", showVrMenu ? 1 : 0);
	// Acquire the eye images after all CPU/GPU waits. From this point through
	// FinishD3D12SwapchainWrites, submit clears without a CPU wait and release
	// every image acquired for this frame.
	if(traceCinema && gRuntimeNeedsCinemaKeepAlive)
		VrLog("Cinema trace: acquire eyes\n");
	if(gRuntimeNeedsCinemaKeepAlive &&
	   AcquireSwapchain(gEye[0].swapchain) &&
	   AcquireSwapchain(gEye[1].swapchain)){
		if(traceCinema) VrLog("Cinema trace: eyes acquired, clears pending=%u\n",
			gCinemaEyeClearsPending);
		if(gCinemaEyeClearsPending > 0){
			ID3D12Resource *eyes[] = {
				AcquiredTexture(gEye[0].swapchain),
				AcquiredTexture(gEye[1].swapchain)
			};
			if(rw::d3d12::clearExternalTexturesNoWait(eyes, 2) != 0){
				gCinemaEyeClearsPending--;
				gCinemaEyesEverCleared = true;
				cinemaEyesReady = true;
			}
		}else
			cinemaEyesReady = gCinemaEyesEverCleared;
	}
	if(traceCinema) VrLog("Cinema trace: finish writes, eyesReady=%d\n",
		cinemaEyesReady ? 1 : 0);
	const bool cinemaGpuSubmitted = FinishD3D12SwapchainWrites();
	if(traceCinema) VrLog("Cinema trace: writes done=%d\n",
		cinemaGpuSubmitted ? 1 : 0);
	const bool cinemaSubmitted = cinemaCopied && cinemaGpuSubmitted;
	if(gVrLoggedRenderableFrames < 10)
		VrLog("SubmitCinemaFrame copy=%d gpu=%d size=%dx%d\n",
			cinemaCopied ? 1 : 0, cinemaSubmitted ? 1 : 0, width, height);
	if(!cinemaSubmitted){
		ReleaseSwapchain(gCinemaSwapchain);
		EndXrFrame(nil,0);
		return false;
	}
	gCinemaFrameValid = true;
	gCinemaQuadImageValid = true;
	gCinemaContentWidth = contentWidth;
	gCinemaContentHeight = contentHeight;
#else
	GLint oldDraw=0,oldRead=0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER,0); glReadBuffer(GL_BACK);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gCopyFramebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,AcquiredTexture(gCinemaSwapchain),0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(viewport[0],viewport[1],viewport[0]+width,viewport[1]+height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_LINEAR);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw); glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);
	ReleaseSwapchain(gCinemaSwapchain);
	gCinemaFrameValid = true;
	gCinemaQuadImageValid = true;
	gCinemaContentWidth = contentWidth;
	gCinemaContentHeight = contentHeight;
	showVrMenu = UpdateVrMenuSwapchain();
#endif
	XrCompositionLayerQuad cinema={XR_TYPE_COMPOSITION_LAYER_QUAD};
	ApplyCinemaQuadPose(&cinema); cinema.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
	cinema.subImage.swapchain=gCinemaSwapchain.handle;
	cinema.subImage.imageRect.extent={contentWidth,contentHeight};
	cinema.size.width=3.2f;
	cinema.size.height=3.2f*(float)contentHeight/contentWidth;
	XrCompositionLayerQuad vrMenuLayer={XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(showVrMenu){
		const bool compactDlssToast =
			!gVrMenuVisible && !gCheatMenuVisible && !gVrAboutVisible;
		vrMenuLayer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		vrMenuLayer.space=gViewSpace; vrMenuLayer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
		vrMenuLayer.subImage.swapchain=gVrMenuSwapchain.handle;
		vrMenuLayer.subImage.imageRect.extent={VR_MENU_WIDTH,VR_MENU_HEIGHT};
		vrMenuLayer.pose.orientation.w=1.0f;
		vrMenuLayer.pose.position.y=compactDlssToast ? 0.20f : 0.0f;
		vrMenuLayer.pose.position.z=-1.65f;
		vrMenuLayer.size.width=compactDlssToast ? 1.65f : 2.15f;
		vrMenuLayer.size.height=vrMenuLayer.size.width*
			(float)VR_MENU_HEIGHT/VR_MENU_WIDTH;
	}
	XrCompositionLayerProjection cinemaProjection={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerProjectionView cinemaProjectionViews[EYE_COUNT];
	bool cinemaProjectionReady = false;
	if(cinemaEyesReady && gLocalSpace){
		if(traceCinema) VrLog("Cinema trace: locate views\n");
		XrViewLocateInfo locate={XR_TYPE_VIEW_LOCATE_INFO};
		locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime=gFrameState.predictedDisplayTime;
		locate.space=gLocalSpace;
		XrViewState viewState={XR_TYPE_VIEW_STATE}; uint32_t viewCount=0;
		XrView located[EYE_COUNT]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
		if(XrOk(xrLocateViews(gSession,&locate,&viewState,EYE_COUNT,
		   &viewCount,located),"xrLocateViews(cinema)") &&
		   viewCount==EYE_COUNT &&
		   (viewState.viewStateFlags &
		    XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0){
			for(int eye=0;eye<EYE_COUNT;eye++){
				cinemaProjectionViews[eye]=
					{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				cinemaProjectionViews[eye].pose=located[eye].pose;
				cinemaProjectionViews[eye].fov=located[eye].fov;
				cinemaProjectionViews[eye].subImage.swapchain=
					gEye[eye].swapchain.handle;
				cinemaProjectionViews[eye].subImage.imageRect.extent={
					gEye[eye].swapchain.width,
					gEye[eye].swapchain.height};
			}
			cinemaProjection.space=gLocalSpace;
			cinemaProjection.viewCount=EYE_COUNT;
			cinemaProjection.views=cinemaProjectionViews;
			cinemaProjectionReady=true;
		}
	}
	const XrCompositionLayerBaseHeader *layers[3]; uint32 count=0;
	if(cinemaProjectionReady)
		layers[count++]=(const XrCompositionLayerBaseHeader*)&cinemaProjection;
	layers[count++]=(const XrCompositionLayerBaseHeader*)&cinema;
	if(showVrMenu) layers[count++]=(const XrCompositionLayerBaseHeader*)&vrMenuLayer;
	if(traceCinema) VrLog("Cinema trace: end frame layers=%u projection=%d\n",
		count, cinemaProjectionReady ? 1 : 0);
	const bool cinemaFrameEnded = EndXrFrame(layers,count);
	if(traceCinema) VrLog("Cinema trace: end frame ok=%d\n",
		cinemaFrameEnded ? 1 : 0);
	return cinemaFrameEnded;
}

bool CaptureStartupWindow(HWND window)
{
	if(CaptureMovieFrameRGBA(gStartupPixels, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT))
		return true;
	if(!window || !IsWindow(window))
		return false;
	if(!gStartupCaptureDc){
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = VR_STARTUP_WIDTH;
		info.bmiHeader.biHeight = -VR_STARTUP_HEIGHT;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		gStartupCaptureDc = CreateCompatibleDC(nil);
		gStartupCaptureBitmap = CreateDIBSection(gStartupCaptureDc, &info,
			DIB_RGB_COLORS, &gStartupCaptureBits, nil, 0);
		if(!gStartupCaptureDc || !gStartupCaptureBitmap || !gStartupCaptureBits){
			DestroyStartupCapture();
			return false;
		}
		gStartupCaptureOldBitmap = SelectObject(gStartupCaptureDc, gStartupCaptureBitmap);
	}
	RECT client = {};
	if(!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top)
		return false;
	POINT origin = { client.left, client.top };
	if(!ClientToScreen(window, &origin))
		return false;
	HDC desktop = GetDC(nil);
	if(!desktop)
		return false;
	SetStretchBltMode(gStartupCaptureDc, HALFTONE);
	SetBrushOrgEx(gStartupCaptureDc, 0, 0, nil);
	const BOOL copied = StretchBlt(gStartupCaptureDc, 0, 0,
		VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT, desktop, origin.x, origin.y,
		client.right-client.left, client.bottom-client.top, SRCCOPY | CAPTUREBLT);
	ReleaseDC(nil, desktop);
	if(!copied)
		return false;
	const uint8 *source = (const uint8*)gStartupCaptureBits;
	for(int pixel = 0; pixel < VR_STARTUP_WIDTH*VR_STARTUP_HEIGHT; pixel++){
		gStartupPixels[pixel*4+0] = source[pixel*4+2];
		gStartupPixels[pixel*4+1] = source[pixel*4+1];
		gStartupPixels[pixel*4+2] = source[pixel*4+0];
		gStartupPixels[pixel*4+3] = 255;
	}
	return true;
}

// Only the selected temporal backend is brought up.
//
// Both used to initialise unconditionally. Streamline works by interposing the
// D3D12 device and command lists, and FSR2 attaches to the same device and
// builds its own pipeline on it; with both live, the first real DLAA
// evaluation deadlocked inside slEvaluateFeature -- the DLSS context was
// created, its extents logged, and the call never returned, hanging the
// process. Whichever backend the player is not using has no business touching
// the device at all.
void AttachTemporalAaDevices()
{
	if(gTemporalAaBackend == TEMPORAL_AA_DLAA)
		Dlaa::AttachDevice();
	else if(gTemporalAaBackend == TEMPORAL_AA_FSR2)
		Fsr2::AttachDevice();
}

void InitializeTemporalAAEarly()
{
	// The settings decide which backend may touch the device, and this runs
	// before anything else would read them. LoadVrSettings only reads the ini
	// and guards against running twice, so pulling it forward is free.
	LoadVrSettings();
	// Streamline's interposition must be installed before the D3D12 device
	// exists, and it stays passive until a feature is evaluated, so it is set
	// up unconditionally -- otherwise switching to DLAA in the settings menu
	// could never work for the rest of the session. StreamlineEnabled=0 is
	// the explicit opt-out for machines where the interposed device delivers
	// black frames; without slInit the interposer is a plain passthrough.
	if(gStreamlineEnabled)
		Dlaa::InitializeEarly();
	else
		debug("[OpenXR] Streamline disabled via vr_settings.ini; "
			"DLAA unavailable this session\n");
	// FSR2 is not passive: Initialize/AttachDevice build a pipeline on the
	// device. Only do that when FSR2 is the backend in use.
	if(gTemporalAaBackend == TEMPORAL_AA_FSR2)
		Fsr2::Initialize();
#ifdef RW_D3D12
	rw::d3d12::setDeviceCreatedCallback(AttachTemporalAaDevices);
#endif
}

void StartEarly()
{
	AttachTemporalAaDevices();
	debug("[OpenXR] %s\n", Dlaa::GetStatus());
	debug("[OpenXR] %s\n", Fsr2::GetStatus());
	EnsureSession();
}

bool SubmitStartupFrame(void *nativeWindow)
{
	SetFramePhase("startup");
	if(!BeginXrFrame())
		return false;
	if(!gFrameState.shouldRender){
		EndXrFrame(nil, 0);
		return false;
	}
	if(!gCinemaSwapchain.handle ||
	   gCinemaSwapchain.width < VR_STARTUP_WIDTH ||
	   gCinemaSwapchain.height < VR_STARTUP_HEIGHT){
		EndXrFrame(nil, 0);
		return false;
	}
	if(!CaptureStartupWindow((HWND)nativeWindow) || !AcquireSwapchain(gCinemaSwapchain)){
		EndXrFrame(nil, 0);
		return false;
	}
	gCinemaFrameValid = false;
#ifdef RW_D3D12
	const bool uploaded = rw::d3d12::uploadRgbaToExternal(
		AcquiredTexture(gCinemaSwapchain), gStartupPixels,
		VR_STARTUP_WIDTH*4, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT) != 0;
	const bool submitted = FinishD3D12SwapchainWrites();
	if(!uploaded || !submitted){
		EndXrFrame(nil, 0);
		return false;
	}
	gCinemaFrameValid = true;
	gCinemaContentWidth = VR_STARTUP_WIDTH;
	gCinemaContentHeight = VR_STARTUP_HEIGHT;
#else
	GLint oldTexture = 0, oldAlignment = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
	glBindTexture(GL_TEXTURE_2D, AcquiredTexture(gCinemaSwapchain));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT,
		GL_RGBA, GL_UNSIGNED_BYTE, gStartupPixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	ReleaseSwapchain(gCinemaSwapchain);
	gCinemaFrameValid = true;
	gCinemaContentWidth = VR_STARTUP_WIDTH;
	gCinemaContentHeight = VR_STARTUP_HEIGHT;
#endif
	XrCompositionLayerQuad cinema = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	ApplyCinemaQuadPose(&cinema);
	cinema.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	cinema.subImage.swapchain = gCinemaSwapchain.handle;
	cinema.subImage.imageRect.extent = { VR_STARTUP_WIDTH, VR_STARTUP_HEIGHT };
	cinema.size.width = 3.2f;
	cinema.size.height = 1.8f;
	const XrCompositionLayerBaseHeader *layers[] = {
		(const XrCompositionLayerBaseHeader*)&cinema
	};
	return EndXrFrame(layers, 1);
}

void CancelStereoFrame(RwCamera *camera){
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(-1);
	rw::d3d12::invalidateScreenSpaceReflectionHistory();
#endif
	ResetTemporalAaHistory();
	RestoreCamera(camera);
#ifdef RW_D3D12
	FinishD3D12SwapchainWrites();
#endif
	if(gFrameBegun) EndXrFrame(nil,0);
}
void SetInactive(){
#ifdef RW_D3D12
	rw::d3d12::setStereoWorldEye(-1);
	rw::d3d12::invalidateScreenSpaceReflectionHistory();
#endif
	ResetImmersiveDrivingInteraction();
	ResetTemporalAaHistory();
	ResetTrackedScopeState();
	gDlssProfileToastPresented = false;
	ClearAttachedMissionWeaponState(nil);
	gWasSubmitting=false;
	for(int hand = 0; hand < EYE_COUNT; hand++){
		ClearWeaponSupportForHand(hand);
		gHeldWeaponSlot[hand]=-1;
		gTrackedWeaponRenderMatrixSlot[hand]=-1;
		ClearDroppedWeapon(hand);
		gWeaponHolsterSelection[hand]=-1;
		gWeaponHolsterGripDown[hand]=false;
		gTrackedWeaponTriggerPressed[hand]=false;
		gTrackedWeaponTriggerJustPressed[hand]=false;
		gTrackedWeaponTriggerJustReleased[hand]=false;
		gTrackedThrowablePreviewActive[hand]=false;
		gTrackedAimCacheValid[hand]=false;
		ResetManualReloadState(gManualReload[hand]);
		gManualReloadGripDown[hand]=false;
		ResetPhysicalMeleeMotion(hand);
	}
	gTrackedHandPoseValid[0]=gTrackedHandPoseValid[1]=false;
	gTrackedHandAimPoseValid[0]=gTrackedHandAimPoseValid[1]=false;
	gActiveTrackedFireAimValid=false;
	// SetInactive is also used for recoverable OpenXR frame/session hiccups.
	// Do not orphan live C4 just because the headset skipped a frame.
	ResetTrackedDetonatorInteraction(false);
}
void PrepareForGameShutdown()
{
	if(gHolsterCalibrationCheat.active){
		CPlayerPed *player = FindPlayerPed();
		if(IsHolsterCalibrationOwnerValid(player))
			FinishHolsterCalibrationLoadout();
		else
			DiscardHolsterCalibrationLoadout("OpenXR shutdown");
	}
	// Reset the complete gameplay-side interaction state while its model and TXD
	// references are still valid. This also invalidates contact matrices and all
	// trigger/grip latches so an in-process restart cannot reuse a stale weapon.
	ResetPhysicalWeaponStateForGameplayLoss(nil);
	ResetTrackedScopeState();
	gWasSubmitting=false;
	gTrackedHandPoseValid[0]=gTrackedHandPoseValid[1]=false;
	gTrackedHandAimPoseValid[0]=gTrackedHandAimPoseValid[1]=false;
	ResetTrackedDetonatorInteraction(true);
	gTrackedForegroundRenderCallback=nil;
	gTrackedForegroundVisibilityCallback=nil;
}

void Shutdown()
{
	// Game-owned references are released earlier by PrepareForGameShutdown,
	// before CModelInfo and CTxdStore are destroyed. Keep this late phase limited
	// to OpenXR/RenderWare resources.
	if(gPerfRecording){
		gPerfRecording=false;
		if(gPerfLiveCsv){
			fclose(gPerfLiveCsv);
			gPerfLiveCsv=nil;
		}
		DumpPerfRecording();
	}
	DestroyRuntime();
#ifdef RW_D3D12
	DestroyTrackedForegroundResolvePipeline();
	ReleaseDesktopAaStaging();
#endif
	Fsr2::Shutdown();
	Dlaa::Shutdown();
	debug("[OpenXR] Runtime shut down\n");
}
}

#endif
