#pragma once

#ifdef GTA_VR_OCULUS

class CMatrix;
class CVector;
class CControllerState;
class CEntity;
class CVehicle;

namespace OculusVR
{
enum ePerfPhase
{
	PERF_PHASE_GAME,
	PERF_PHASE_AUDIO,
	PERF_PHASE_STREAMING,
	PERF_PHASE_WORLD_LIST,
	PERF_PHASE_PRE_RENDER,
	PERF_PHASE_SCENE_SETUP,
	PERF_PHASE_DESKTOP_RENDER,
	PERF_PHASE_UI,
	PERF_PHASE_CINEMA_SUBMIT,
	PERF_PHASE_LEFT_EYE,
	PERF_PHASE_RIGHT_EYE,
	PERF_PHASE_SUBMIT,
	PERF_PHASE_DESKTOP_PRESENT,
	// Regions that used to sit between the phases above. Together they closed a
	// gap that reached 14ms in a captured frame, which was invisible to both the
	// CSV and the in-headset panel.
	PERF_PHASE_FRAME_INIT,
	PERF_PHASE_VR_SETUP,
	PERF_PHASE_VR_OVERLAYS,
	PERF_PHASE_CAMERA_END,
	// Session upkeep run before xrWaitFrame: event polling, and the deferred
	// render-scale and temporal-AA rebuilds. The latter performs a full GPU
	// flush, so it belongs on the panel rather than in the unmeasured residue.
	PERF_PHASE_SESSION_UPKEEP,
	PERF_PHASE_COUNT
};

enum ePerfVisibleType
{
	PERF_VISIBLE_BUILDING,
	PERF_VISIBLE_OBJECT,
	PERF_VISIBLE_PED,
	PERF_VISIBLE_VEHICLE
};

// Appends to openxr_d3d12.log, the file players already collect, so
// game-side subsystems (saves, streaming) can leave diagnostics there.
void GameLog(const char *format, ...);

// Absolute exe-relative path to vr_settings.ini. Game-side ini reads must
// use this instead of ".\\vr_settings.ini": launchers can start the game
// with a different working directory.
const char *GetVrSettingsFilePath();

bool BeginStereoFrame(RwCamera *camera, const CMatrix &baseCamera);
bool GetEyeCamera(int eye, CMatrix *eyeCamera);
bool BeginEye(RwCamera *camera, int eye, CMatrix *eyeCamera, float *horizontalFov);
void GetTemporalJitterClip(float *x, float *y);
#ifdef GTA_VR_OPENXR
typedef void (*TrackedForegroundRenderCallback)(void);
typedef bool (*TrackedForegroundVisibilityCallback)(void);
void SetTrackedForegroundRenderCallback(TrackedForegroundRenderCallback callback);
void SetTrackedForegroundVisibilityCallback(
	TrackedForegroundVisibilityCallback callback);
bool IsTrackedForegroundRenderAvailable();
#endif
// Flat-mode only: DLAA/DLSS evaluate on the desktop back buffer after the
// world pass; no-op in VR sessions or when DLAA is off. Takes the RwCamera
// the world just rendered with.
void EvaluateDesktopTemporalAa(void *rwCamera);
bool SubmitStereoFrame(RwCamera *camera);
bool SubmitCinemaFrame(RwCamera *camera, bool holdLastFrame = false);
void BeginNewGameCinemaHold();
bool IsNewGameCinemaHoldActive();
void EndNewGameCinemaHold();
void InitializeTemporalAAEarly();
void StartEarly();
void ReapplyTrafficSettings();
void ReloadVehicleCategoryCalibration();
bool SubmitStartupFrame(void *nativeWindow);
void CancelStereoFrame(RwCamera *camera);
void SetInactive();
int GetStereoScalePercent();
bool IsStereoReversed();
bool IsFirstPersonEnabled();
#ifdef GTA_VR_OPENXR
bool IsStereoCutsceneActive();
void ApplyCutsceneCamera(CMatrix *camera);
bool ShouldHideCameraActorHead(CEntity *entity);
bool IsVehicleThirdPersonActive();
bool ShouldHideDefaultDrivingBody();
bool IsBikeViewFollowingTilt();
bool IsBikeManualThrottle();
bool GetBikeVisualSteerAngle(CVehicle *bike, float *angle);
int GetBikeVisualLeanPercent();
bool CanBikeRiderBeThrown();
#else
inline bool IsStereoCutsceneActive(){ return false; }
inline void ApplyCutsceneCamera(CMatrix *){}
inline bool ShouldHideCameraActorHead(CEntity *){ return false; }
inline bool IsVehicleThirdPersonActive(){ return false; }
inline bool ShouldHideDefaultDrivingBody(){ return false; }
inline bool IsBikeViewFollowingTilt(){ return true; }
inline bool IsBikeManualThrottle(){ return true; }
inline bool GetBikeVisualSteerAngle(CVehicle *, float *){ return false; }
inline int GetBikeVisualLeanPercent(){ return 100; }
inline bool CanBikeRiderBeThrown(){ return true; }
#endif
// True only in flat (non-VR) mode with FlatFirstPerson=1: the game camera
// holds the native first-person look-around mode permanently.
bool IsFlatFirstPersonEnabled();
// True only after a flat-mode frame has completed a successful direct
// DLSS Neural Rendering evaluation. Used by the desktop diagnostic badge.
bool IsFlatNeuralRenderingActive();
bool IsFlatNeuralRenderingOutputVisible();
bool IsFlatNeuralRenderingSplitView();
bool IsFlatNeuralRenderingOverlayVisible();
int GetFlatNeuralRenderingOverlaySelection();
int GetFlatNeuralRenderingPassCount();
const char *GetFlatNeuralRenderingEffectName();
const char *GetFlatNeuralRenderingViewName();
bool IsFlatNeuralRenderingFloatTuningVerified();
#ifdef GTA_VR_OPENXR
float GetSunGlareScale();
#endif
bool IsHeadRelativeLocomotionActive();
bool IsExperimentalHeadTurningActive();
bool IsHeadDirectedLocomotionActive();
bool IsStereoFrameActive();
bool IsHeadBobbingEnabled();
bool CanSkipDesktopGameplayRender();
void ReportDesktopRenderSkip(bool skipping, bool vrReady, bool wideScreen,
	bool cutsceneProcessing, bool cutsceneRunning);
bool ShouldRouteGameplayHudToVr();
#ifdef GTA_VR_OPENXR
bool IsHudPanelOnWrist(int panel);
bool IsClassicHudWeaponPanelVisible();
bool IsClassicHudClockVisible();
void GetWristAmmoColour(uint8 *red, uint8 *green, uint8 *blue);
#endif
bool UseFullStereoSinglePass();
bool AreTrackedHandsEnabled();
bool AreWeaponHolsterHighlightsEnabled();
bool ShouldUseTrackedHands();
bool IsTrackedHandReady(int hand);
bool IsTrackedWeaponLaserEnabled();
#ifdef GTA_VR_OPENXR
bool IsTrackedWeaponLaserEnabledForType(int weaponType);
#endif
bool IsTrackedScopeActive();
bool IsTrackedScopeActiveForHand(int hand);
int GetTrackedScopeWeaponType();
bool ShouldUseTrackedWeapon(int hand);
bool IsTrackedWeaponTriggerPressed(int hand);
bool IsTrackedWeaponTriggerJustPressed(int hand);
bool IsTrackedWeaponTriggerJustReleased(int hand);
#ifdef GTA_VR_OPENXR
// Resolves the physical fire trigger for a held weapon. The on-foot RPG uses
// the opposite (support/free) controller; all other weapons retain ownership.
bool IsTrackedWeaponFireTriggerPressed(int weaponHand, int weaponType);
bool IsTrackedWeaponFireTriggerJustPressed(int weaponHand, int weaponType);
#endif
bool IsTrackedDetonatorActive(int hand);
bool IsTrackedDetonatorTriggerJustPressed(int hand);
bool IsTrackedRemoteGrenadeFireActive();
bool ShouldKeepTrackedRemoteCharges(CEntity *source);
void NotifyTrackedRemoteGrenadeThrown(int hand);
void NotifyTrackedDetonatorActivated(int hand);
bool IsManualReloadWeaponType(int weaponType);
void SetManualReloadWeaponState(int weaponHand, int slot, int weaponType, bool available);
bool ShouldUseManualReload();
bool ConsumeManualReloadRequest(int weaponHand, int slot, int weaponType);
bool GetManualReloadMagazineMatrix(int weaponHand, CMatrix *matrix,
	int *weaponType = nil, bool *held = nil);
void SetWeaponHolsterMask(uint32 mask);
bool ConsumeWeaponHolsterSelection(int hand, int *slot);
bool GetWeaponHolsterMatrix(int slot, CMatrix *matrix);
bool IsPhysicalGunType(int weaponType);
bool IsPhysicalMeleeType(int weaponType);
bool IsPhysicalThrowableType(int weaponType);
bool IsPhysicalWeaponType(int weaponType);
bool IsPhysicalWeaponInteractionActive();
bool IsTrackedWeaponHeld(int hand);
int GetHeldWeaponSlot(int hand);
void SetTrackedWeaponRenderMatrix(int hand, int slot, int weaponType,
	const CMatrix *matrix,
	const CMatrix *contactMatrix = nil);
int GetDroppedWeaponSlot(int hand);
bool GetDroppedWeaponMatrix(int slot, CMatrix *matrix);
void GetTrackedWeaponOffset(float *offsetX, float *offsetY, float *offsetZ);
void GetTrackedWeaponRotation(float *rotationX, float *rotationY, float *rotationZ);
void GetTrackedWeaponOffsetForType(int hand, int weaponType, float *offsetX, float *offsetY, float *offsetZ);
void GetTrackedWeaponRotationForType(int hand, int weaponType, float *rotationX, float *rotationY, float *rotationZ);
bool ApplyTrackedWeaponTwoHandTransform(int primaryHand, int weaponType,
	CMatrix *matrix);
#ifdef GTA_VR_OPENXR
bool IsTrackedWeaponCalibrationTarget(int hand, int weaponType);
bool IsTrackedWeaponSupportFromBelow(int primaryHand, int weaponType);
#endif
bool GetTrackedWeaponSupportAnchor(int primaryHand, int weaponType,
	CVector *position, bool *engaged = nil);
bool GetTrackedWeaponAim(int hand, int weaponType, CVector *source, CVector *direction);
bool GetTrackedThrowableLaunch(int hand, int weaponType, CVector *source,
	CVector *velocity);
void SetTrackedThrowablePreviewActive(int hand, bool active);
bool IsTrackedThrowablePreviewActive(int hand);
bool GetTeleportTrajectory(CVector *points, int maximumPoints,
	int *pointCount, bool *targetValid = nil);
void BeginTrackedWeaponFire(int hand, int weaponType, const CVector &source,
	const CVector &direction);
void EndTrackedWeaponFire();
void NotifyTrackedWeaponFired(int hand, int weaponType);
bool GetActiveTrackedWeaponAim(CVector *source, CVector *direction);
bool GetActiveTrackedThrowableLaunch(CVector *source, CVector *velocity);
void ReleaseTrackedWeaponAfterUse(int hand, int slot);
bool GetTrackedHandMatrix(int hand, CMatrix *handMatrix, float *grip = nil, float *trigger = nil);
bool GetTrackedHandAimRay(int hand, CVector *origin, CVector *direction);
#ifdef GTA_VR_OPENXR
bool GetTrackedVisualHandMatrix(int hand, CMatrix *handMatrix,
	float *grip = nil, float *trigger = nil);
bool GetTrackedVisualHandAimRay(int hand, CVector *origin,
	CVector *direction);
#endif
bool IsImmersiveDrivingActive();
bool IsImmersiveCarDrivingActive();
bool IsImmersiveBikeDrivingActive();
bool IsVrCarDrivingActive();
bool IsVrBikeDrivingActive();
bool IsVrRadioControlActive();
bool ConsumeVrRadioChange();
bool GetImmersiveCarSteering(CVehicle *car, float *steering);
bool GetImmersiveBikeSteering(CVehicle *bike, float *steering);
bool GetImmersiveBikeThrottle(CVehicle *bike, float *throttle);
bool GetImmersiveBikeLean(CVehicle *bike, float *lean);
bool GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix);
bool IsImmersiveBikeHandleGrabbed(int hand);
bool ShouldRenderImmersiveBikeHandleMarker(int hand);
bool GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix);
bool IsImmersiveSteeringHandleGrabbed(int hand);
bool ShouldRenderImmersiveSteeringHandleMarker(int hand);
bool ShouldRenderImmersiveCarWheel();
#ifdef GTA_VR_OPENXR
bool GetImmersiveCarWheelPose(CVector *center, CVector *right,
	CVector *up, CVector *normal, float *radius);
void UpdateImmersiveCarModelSteeringWheel(CVehicle *vehicle);
#endif
bool IsVrBikeHorizonLocked();
bool IsImmersiveBikeSidearm(int weaponType);
bool IsImmersiveVehicleSidearm(int weaponType);
bool ConsumePhysicalMeleeStrike(int hand, int *slot, int *weaponType,
	CVector *sweepStart, CVector *sweepEnd, float *speed = nil,
	CVector *rootStart = nil, CVector *rootEnd = nil);
void ResolvePhysicalMeleeStrike(int hand, bool contact);

void PerfBeginFrame();
// Transfers the completed GTA point-light list from the previous game frame to
// the D3D12 world shader before CPointLights::InitPerFrame clears it.
void PushDynamicLights();
void PerfAbortFrame();
void PerfEndFrame(float playerX, float playerY, float playerZ);
void PerfBeginPhase(ePerfPhase phase);
void PerfEndPhase(ePerfPhase phase);
void PerfSetStreamingStats(int requestedModels, uint64 memoryUsed);
void PerfBeginStreamItem(int streamId, int streamType);
void PerfEndStreamItem();
void PerfCountVisibleEntity(ePerfVisibleType type, const CEntity *entity = nil);
void PerfCountEntityRender();

bool ApplyTouchInput(CControllerState *state);
void ToggleCheatMenu();
// Must run before CModelInfo/TXD teardown; releases calibration-owned model refs
// and clears gameplay objects that the later runtime shutdown must not touch.
void PrepareForGameShutdown();
void Shutdown();
}

#endif
