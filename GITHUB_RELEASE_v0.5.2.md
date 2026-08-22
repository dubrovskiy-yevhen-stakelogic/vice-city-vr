# Vice City VR v0.5.2-alpha (in progress, not yet released)

v0.5.2 is a stability and compatibility update built from player-reported
issues. Every item below was diagnosed from real player logs and, where
possible, verified by the reporting player before landing here.

## Standalone Quest parity on PC

- Ported the portable gameplay and rendering fixes from the current public
  standalone Quest source kit while retaining the PC renderer, OpenXR runtime,
  DLAA/FSR paths, and the newer PC cutscene presentation safeguards.
- Added the experimental immersive HUD preset: the minimap, player status and
  clock can live on real stereo wrist panels with independent hand, side,
  on-foot/car/bike placement and scale calibration. The Classic head-locked HUD
  remains the default, and every panel fails open to its Classic copy if its
  render target or tracked-hand anchor is unavailable.
- Shipped the maintainer-tested Immersive HUD placements as the fresh preset
  defaults. Motorcycle panels now have a shared global layout plus optional
  per-model overrides created only when calibration is changed while seated;
  resetting a model returns it to global inheritance.
- Immersive motorcycles now accelerate with the right trigger by default. The
  advanced physical wrist-twist throttle remains available through a strongly
  highlighted Vehicle Settings row, avoiding the appearance of broken controls
  for players who do not know the gesture.
- Updated Modern vehicle and motorcycle view/handle baselines, including the
  PCJ600 and Sanchez, without replacing the PC one-hand steering solver.
- Head-relative locomotion now applies the requested lateral direction without
  the original 50-degree movement gate, and Tommy's body aligns before the
  animation group is selected.
- Physical bat and other melee swings keep publishing their short follow-through
  after the fast trigger sample, so a standing swing can connect instead of
  requiring the player to walk into the target.
- Stereo occlusion now tests all eight transformed entity bounds against exact
  authored world volumes from both physical eyes. It no longer depends on a
  rolled or asymmetric 2D eye projection, reducing mid-view geometry cuts.
- A full transparent-object list no longer makes a fading object disappear for
  the frame; it falls back to the normal opaque path.
- Modern wheels bind while their verified wheel TXD is current, avoiding white
  dummy textures caused by load-order collisions.
- Bullet-trace visibility and fly-by audio use a last-valid physical headset
  view basis even when the simulation emits a trace before the next stereo
  frame, instead of silently falling back to Tommy's body-facing camera.
- Rain emission is normalized to the original 30 Hz authored rate. Fountain
  particles gain Original, Optimized, Low and Off modes; Optimized is the fresh
  default and Original remains the exact-behaviour fallback.
- Save loading now rejects malformed or oversized blocks before alignment or
  buffer reads and reports read failures instead of continuing with partial
  data.
- SWAT rope spawning now fails safely when the population pool cannot allocate
  the descending officer instead of dereferencing a null ped.
- Added a one-click PC Modern-model preparation script. It asks for the legal
  Vice City installation, caches and verifies the two external source packs,
  excludes the known-heavy replacement palms, builds into staging, validates
  the result and only then replaces `modelsets\\modern`.
- Fresh model-category defaults match the tested standalone profile: Modern
  World / Buildings and Weapons, with Classic Vehicles, Pedestrians and
  Vegetation / Palms. A missing overlay falls back to Classic for the session
  without erasing the requested Modern preference.
- Occlusion Culling now exposes Off, Stereo Safe, Authored and Aggressive modes.
  The default Authored path uses the union of both tracked-eye frustums before
  applying exact dual-eye authored-volume occlusion; Off remains the exact
  full-360 fallback.
- Locomotion Settings now exposes a direct `RECENTER VIEW` action; the obsolete
  no-op teleport/movement row is no longer shown.

## SteamVR: cutscenes and menus at full frame rate

- Fixed cutscenes, loading screens, and menus dropping to ~15 fps on SteamVR
  after the first moment of gameplay. SteamVR deliberately throttles sessions
  that stop submitting stereo frames; flat content on the theater screen
  qualified. The game now renders the theater screen into a real stereo
  projection pass during flat content, so the session is never throttled.
  Confirmed fixed by the reporting player: full-rate cutscenes, menus, and
  saves on SteamVR.
- The screen keeps its world-anchored position, size, and the VR settings
  overlay; other runtimes (Oculus, VDXR) keep their existing proven path.
- Opt out with `[VR] CinemaProjection=0` in `vr_settings.ini` (returns to the
  0.5.0 behavior including the SteamVR throttle). The experimental
  `CinemaKeepAlive` key from interim test builds is retired and ignored.

## Crash and freeze fixes

- An IMG archive that exists but cannot be opened (an antivirus still
  scanning a freshly built Modern overlay, or an incomplete build) no longer
  crashes the game with an assertion. The session falls back to Classic
  assets and logs the file and error code.
- Fixed a crash when raising Render Scale to very high values (the
  post-effects buffer exceeded the D3D12 texture size limit); the effect now
  skips and retries instead.
- Fixed a startup crash window during the intro movie state when the VR
  runtime shuffled window focus.
- The game now refuses to start a second instance (previously invisible
  duplicate processes fought over the headset).
- Fixed a GPU memory spiral on 8 GB cards with Modern assets: the streaming
  cache is now sized from actual adapter VRAM, and GPU allocation failures
  shrink it further instead of escalating into paging and a DEVICE_HUNG
  device removal.
- Added `[VR] StreamlineEnabled=0` to bypass DLSS/Streamline initialization
  on machines where the interposer delivers black frames (some VDXR setups);
  DLAA is unavailable in that mode.

## Memory leaks

A full audit of the D3D12 backend found and fixed nine leaks and lifetime
bugs, including a use-after-free in deferred resource release (the root of
several DEVICE_HUNG and wrong-texture reports), post-effect buffers leaking
on pause-menu exit, texture heap pages that were never returned, an upload
pool that only grew, and stale queues surviving a device reset. Long
sessions no longer grow GPU or system memory to their peak forever.

## Saves

- A save that cannot be written now reports failure honestly. Previously a
  blocked save folder showed "saved successfully" with an empty slot list
  (stock bug, typically triggered by installs in write-protected folders).
- If the game folder refuses writes, saves automatically go to
  `Documents\GTA Vice City User Files` instead of silently going nowhere.
- The save folder is resolved absolutely from the executable location, so
  launchers with a different working directory can no longer scatter saves.
- The log now records the active save folder and every save with its path.

## Movement and input

- Fixed movement drifting or walking sideways for the whole session when the
  game was launched with the headset on the desk: the gameplay-space anchor
  is now latched only while the headset is actually worn, and re-latches
  after the headset has been set down for 30+ seconds.
- Added a proper stick deadzone (12% radial with rescale, plus cross-axis
  filtering) to VR locomotion; worn sticks no longer pull the player.
- Legacy DirectInput gamepads are ignored by default: phantom HID devices
  (RGB controllers, wheels) fed constant input under the VR sticks. Real
  pads and wheels can opt back in with `[VR] LegacyGamepad=1`.

## Cutscene stability on all runtimes

- Fixed the headset view slamming between the theater screen and the world
  when scripted sequences flickered their cutscene flags between stages.
- Fixed the theater screen re-centering onto the player's head several times
  within one scripted scene.
- Fixed visible shaking during DLAA warm-up frames after cutscene
  transitions.
- Render Scale and Temporal AA menu rows no longer trigger a full render
  target rebuild when the effective resolution did not change, and the
  graphics menu warns about the per-eye GPU cost above 100%.

## Diagnostics

- A failed presentation now writes `d3d12_device_removed.log` with the exact
  device-removal reason.
- The OpenXR log gained: cinema pacing telemetry (separates content judder,
  head-locked fallback, and anchor jumps), movement-input telemetry, a step
  trace for the first cinema frames, the resolved user-files folder, save
  results, and the cinema projection mode state.
- All `vr_settings.ini` reads now resolve the file relative to the
  executable; launching via a launcher or "Run as administrator" previously
  made some settings silently read from a nonexistent file.

# Vice City VR v0.5.0-alpha

v0.5.0 is a major Windows PCVR update focused on physical interaction,
vehicle handling, optional HD assets, city performance, calibration, and safer
release defaults. Players have confirmed that the complete story campaign can
be finished in VR. This remains an alpha because headset-, runtime-,
controller-, mission-, and optional-asset-specific issues may remain.

The release does not contain Grand Theft Auto: Vice City or replacement game
assets. A legal copy of the original 2003 PC game is required.

## Classic / Modern asset mixing

- Added an isolated `modelsets\modern` overlay that never overwrites Classic
  game files.
- Added independent Classic/Modern selection for World / Buildings,
  Vegetation / Palms, Vehicles, Pedestrians, and Weapons.
- Missing Modern entries safely fall back to the legal Classic installation.
- Every category change is saved and applied after a full restart.
- Added an exact generated vegetation manifest and separate Classic/Modern
  weapon and vehicle calibration defaults.
- Retained support for large IMG archives, appended entries, large texture
  sets, high material counts, and replacement handling data.
- Added a local builder and documented recipe. It consumes a player's legal
  game and separately downloaded packs; the generated overlay must not be
  redistributed.

**Vegetation / Palms now defaults to Classic**, including in the recommended
Modern preset. Profiling identified the tested HD palms as the dominant
open-world GPU geometry cost. Modern vegetation remains available as an opt-in.
Existing `vr_settings.ini` choices are preserved, so earlier testers should
verify this row manually.

## Traffic

- Added a sector-aware high-density traffic director.
- Added independent pedestrian and ambient-vehicle density from 50% to 300%,
  with fresh defaults of 135%.
- Added road-demand balancing, distant proxies, and visibility grace periods.
- Separated ambient, pursuit, parked, stalled, wrecked, and mission accounting.
- Added pool reserves and generation guards for mission actors, police,
  passengers, combat entities, and recycled slots.

## Weapons, hands, support grips, and holsters

- RIGHT is the canonical aim/support profile; LEFT is derived as a
  deterministic mirror while model placement remains adjustable per hand.
- Reworked support grips into a stable model-bound frame, so moving a weapon no
  longer makes the secondary hand flicker or detach.
- Added magazine/foregrip and support-from-below styles plus X/Y/Z wrist
  rotation and release calibration baselines.
- Corrected anatomical orientation, thumb direction, reflected-model culling,
  and long-gun/support-hand mirroring.
- Weapon, muzzle, laser, and both hands now share one stable two-hand brace.
- Added Aim Aligned calibration and accelerated long-hold editing.
- RPG fire is routed to L2 when held in the right hand and R2 when held in the
  left, using the opposite free/support controller while suppressing conflicts
  with another held gun or detonator.
- Tracked hands and weapons render after temporal reconstruction, reducing
  DLAA/FSR 2 history lag on controller-driven foreground objects.
- Holsters continuously follow HMD yaw and physical room-scale movement.
- Fixed holstered weapons jumping forward while the player steps backward.
- Grip Lock still blocks accidental drops but permits release into a nearby
  assigned holster.

## Immersive vehicles

- Reworked symmetric one-handed car and motorcycle steering.
- Added limit anti-windup so movement away from full lock responds immediately.
- Motorcycle steering uses a frozen tracking reference, preventing bike lean
  and animated handlebars from feeding back into the solver.
- Physical bike throttle uses an absolute wrist reference; returning to neutral
  releases both throttle and engine-rev audio.
- Prevented accidental one-handed wheelie/stand gestures.
- Added expanded category/per-model seat, steering, wheel, and per-hand handle
  calibration for Classic and Modern assets.
- Added optional calibration markers, virtual car wheel animation, and smoother
  camera yaw while entering or leaving vehicles.

## Rendering and performance

- Added experimental binocular building occlusion: a static building is
  rejected only when both tracked eyes report it hidden.
- Added a live persistent **Graphics > Occlusion Culling** switch.
- Added GPU-stage timings and submitted-entity/streaming counters to explicit
  captures while removing detailed profiler overhead from normal gameplay.
- Removed noisy per-frame Streamline/audio performance logging.
- Reduced reflection refresh work, removed an OpenAL stream busy wait, and
  avoided redundant steady-state volume/pan updates.
- Corrected glass depth behavior, capped frame-rate-independent helicopter dust,
  and stabilized the moon in world space.
- DLAA, NVIDIA DLSS modes, AMD FSR 2 Native AA, FXAA, Render Scale, and VRS
  remain available. They reduce pixel cost, not HD-model triangle/material cost.

## Interface, release UX, and tools

- Added an in-world first-run welcome card after Tommy's first controllable
  step and a persistent **VR Menu > About** page.
- Added shortcuts, driving/calibration guidance, alpha status, the Flat2VR
  discussion link, and an occlusion fallback notice.
- Added coloured menu categories and a `V0.5.0 ALPHA` / `NOT FOR SALE` badge.
- Added Model Assets and Traffic submenus.
- Removed one-at-a-time weapon setup and arbitrary mission selection from
  Release builds; they remain Debug-only developer tools.
- Added PC/Quest save-transfer tools plus IMG, DIR, TXD, geometry, and
  Modern-overlay build utilities.

## Stability and compatibility

- VR-owned weapon, holster, hand, and texture references are released before
  TXD/model teardown, fixing the exit assertion.
- In-process restart clears stale tracked-weapon and interaction state.
- Added safe world-list iteration, sector-index clamps, ped-pool spawn guards,
  HD vehicle material bounds, and safe invalid-handling rejection.
- Exempted cutscene objects from inappropriate tracked-eye culling and expanded
  VR pools for heavy scenes.

## Updating from v0.4.1

Extract the new package over the existing files. Release packages omit
`reVC.ini` and `vr_settings.ini`, preserving preferences and calibration. If a
Modern overlay is installed, open **VR Menu > Model Assets**, confirm
**Vegetation / Palms: Classic**, and restart after changing a category.

## Known issues

- Ammu-Nation wall displays do not visibly highlight the selected weapon in
  VR. Use the HUD weapon name and price as the selection cue.
- Occlusion culling is experimental. If geometry or one eye jumps, flickers,
  or is incorrectly hidden, disable **Graphics > Occlusion Culling**.
- Modern vegetation is intentionally not the recommended default because the
  tested HD palms are exceptionally geometry-heavy.
- Full campaign completion is player/community confirmation, not validation on
  every headset, runtime, controller, and optional asset combination.
- Original ASI, CLEO, Modloader, and binary-patching plugins require a separate
  reVC port and are not directly compatible.
