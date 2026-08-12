# Vice City VR v0.5.0-alpha

Vice City VR v0.5.0 is a major Windows PCVR update focused on physical
interaction, vehicle handling, optional HD assets, city performance, and
release-ready defaults.

Players have confirmed that the complete story campaign can be finished in VR.
The project remains an alpha because headset-, runtime-, controller-, and
mission-specific issues may still exist.

Vice City VR does not include *Grand Theft Auto: Vice City* or any optional
replacement game assets. A legally obtained copy of the original 2003 PC
release is required.

## Download and install

1. Download **Vice-City-VR-v0.5.0-alpha.zip** from the Assets section below.
2. Back up important saves.
3. Extract the contents of the **Vice-City-VR-v0.5.0-alpha** folder into the
   Vice City game directory, next to **gta-vc.exe**.
4. Select the headset software you use as the active OpenXR runtime.
5. Connect the headset and run **reVC.exe**.

Keep **gta-vc.exe** and the original game-data folders. The mod does not replace
the original executable.

The Microsoft Visual C++ 2015-2022 Redistributable (x64) is required.

## Highlights

- Players have completed the full story campaign in VR.
- Optional Classic/Modern asset mixing by individual category.
- Classic vegetation is now the recommended default, avoiding the largest
  GPU cost found in the tested HD model pack.
- New city occlusion culling substantially reduces geometry submitted behind
  nearby walls.
- High-density traffic controls from 50% to 300%.
- Reworked one-handed physical motorcycle and car control.
- Reworked weapon support grips, mirrored hand orientation, calibration, and
  holster behavior.
- First-run welcome card, persistent About page, coloured menu categories, and
  a visible **NOT FOR SALE** notice.

## Classic and Modern assets

v0.5.0 can load optional replacement assets from an isolated
**modelsets\modern** overlay without overwriting the legal Classic game files.
Replacement assets are not included in this release.

The new **VR Menu > Model Assets** page provides:

- Recommended preset (Modern categories with Classic vegetation)
- World / Buildings
- Vegetation / Palms
- Vehicles
- Pedestrians
- Weapons

Each category can independently use Classic or Modern assets. Every category
change requires a full game restart because RenderWare archives and model data
are selected during startup.

### Important vegetation default

**Vegetation / Palms defaults to Classic on a fresh v0.5.0 configuration.**

Profiling showed that the tested HD palms, not image resolution, were the main
cause of the severe open-world GPU spikes. Several replacement palm models
contain dramatically more geometry than their Classic equivalents and are
placed hundreds of times around the city. Keeping Classic vegetation allows
Modern vehicles, weapons, pedestrians, and buildings to be used without paying
that cost.

Modern vegetation remains available as an opt-in option. Existing
**vr_settings.ini** files are preserved during updates, so users of earlier
test builds should confirm **Vegetation / Palms: Classic** manually.

### Build the optional Modern overlay locally

The release includes standalone utilities and instructions for constructing
the tested Modern overlay from a legal game installation and three external
downloads:

1. [GTA VC HD + Weapons](https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view)
2. [Mods](https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view)
3. [Vegetation in HD](https://libertycity.net/files/gta-vice-city/126557-vegetation-in-hd.html)

The Vegetation in HD archive password is **libertycity**.

Exact tested inputs:

| Archive | Size in bytes | SHA-256 |
| --- | ---: | --- |
| GTA VC HD + Weapons.zip | 1,878,280,127 | 81A7962479752F3A07004A2E12964815435CAF4B0A0F637EE982460171A5D94C |
| Mods.zip | 2,377,186,981 | F33031091DBE50A3BEDCEEFAF3FDE6F6DDD96F9841AABE1ECD3713818DED9725 |
| 1557511944_1557494130_hd-vegetation.zip | 21,815,644 | D5F8CC260913046C9C11DE0570FD2BA20C5B97EDED5DA2C43AB834992E356AFB |

See the
[Modern asset build guide](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/blob/main/MODERN_MODELS.md)
for the tested SHA-256 hashes, exact build command, expected output,
performance guidance, and redistribution warning. Do not upload or share the
generated Modern archive: it contains data derived from the player's original
game and third-party mods.

## High-density traffic

- Added the sector-aware traffic director developed for high-density play.
- Added independent pedestrian and vehicle density controls from 50% to 300%
  in 5% steps.
- Fresh installations default both traffic categories to 135%.
- Traffic follows local road demand and can fill undersupplied road links.
- Recently seen areas are preserved to avoid obvious pop-in.
- Distant civilian traffic can remain as lightweight proxies until it
  approaches the player.
- Ambient, pursuit, parked, stalled, debris, and mission vehicles are tracked
  separately.
- Generation-safe histories and reserved physical pool capacity prevent
  optional ambient density from corrupting mission or combat entities.
- Wanted levels, mission vehicles, collisions, damage, sounds, and pedestrian
  reactions retain their normal synchronous gameplay behavior.

## Weapons and VR hands

- Corrected left/right anatomical orientation and thumb direction in the
  existing per-hand VR models.
- Reworked two-handed support-grip orientation for left- and right-handed use.
- Fixed support hands appearing inverted on one side while correcting the
  other.
- Fixed support-grip transforms flickering between two positions after moving
  the weapon or editing calibration.
- Fixed held weapons occasionally appearing to update at a very low rate while
  the game itself continued running.
- Moving the main weapon no longer invalidates the calibrated support grip.
- Reworked and retuned the built-in support-grip poses into stable model-bound
  profiles.
- Classic and Modern weapons have separate absolute calibration profiles.
- A Modern weapon inherits the complete effective Classic calibration until
  its Modern profile is edited.
- Calibration values are never accidentally added on top of one another.
- Increased held-button acceleration in numeric calibration rows, making large
  rotations such as 180 degrees practical inside the headset.
- Routed RPG fire to L2 when held in the right hand and R2 when held in the
  left, using the opposite free/support controller while suppressing conflicts
  with another held gun or remote detonator.
- Improved small support-hand offsets on affected long guns and shotguns.

## Holsters and Grip Lock

- Body holsters now continuously follow the headset's facing direction.
- Fixed holstered weapons moving in front of the player when walking backward.
- Holsters remain attached to the player instead of lagging behind movement.
- Weapon Grip Lock still prevents accidental free dropping, but a held weapon
  can now be released into a nearby matching holster.
- Existing holster layouts and player calibration remain preserved.

## Vehicles

- Reworked one-handed physical steering with stable per-grab references and
  hard-stop anti-windup, while preserving the established two-hand path.
- Corrected the delayed-then-sudden left turn seen on the Faggio and the
  left/right steering imbalance seen on other motorcycles.
- Prevented the Faggio from spinning in place from an excessive one-handed left
  steering value.
- Corrected handle and hand orientation across the supported motorcycles,
  including scooters and sport/off-road bikes.
- Physical bike throttle is now based on a captured wrist reference angle
  rather than accumulated movement.
- Returning the wrist to the neutral position releases throttle without
  requiring the trigger to be released.
- Engine/throttle audio now follows the released physical throttle state.
- Retuned built-in motorcycle/Sanchez profiles and added Modern-model vehicle
  and handle defaults.
- Car and motorcycle driving modes remain independently selectable as Default,
  Immersive, or Motion.
- Motion-controller drive-by shooting and physical vehicle sidearms retain
  their tracked aim.

## Rendering and performance

- Added city occlusion culling to avoid submitting large numbers of buildings
  hidden behind nearby geometry.
- Added **VR Menu > Graphics > Occlusion Culling** as a live on/off control.
- Added independent Classic/Modern asset routing so expensive asset categories
  can be disabled without losing every HD replacement.
- Kept Classic fallback behavior for missing Modern loose files.
- Kept **gta3.img** and **gta3.dir** as a required matching pair.
- Preserved support for large IMG archives and replacement models with many
  materials.
- Added the generated vegetation manifest used for deterministic vegetation
  separation instead of unreliable model-name guessing.
- Improved DLAA/DLSS activation and safe fallback handling.
- Kept AMD FSR 2 Native AA and FXAA as alternative anti-aliasing paths.
- Performance capture counters remain available, but expensive diagnostics and
  verbose logging run only when explicitly enabled.
- Made helicopter dust frame-rate-independent and kept it capped for VR.

Lower Render Scale, DLSS upscaling, and VRS can reduce pixel-shading work;
DLAA and FSR 2 Native AA are image-space anti-aliasing paths. None removes
high-poly geometry or extra material passes. If GPU time stays high when
resolution is reduced, first confirm that **Vegetation / Palms** is set to
**Classic**.

## Interface and first-run experience

- Added a welcome card that appears only after Tommy takes his first
  controllable step in the game world.
- The welcome card no longer appears in the frontend or between cutscenes.
- It closes with a normal button press and is shown only once.
- Added **VR Menu > About** so the same information can always be reopened.
- The card explains the VR menu, cheats, driving modes, calibration, alpha
  status, community link, and occlusion fallback.
- Added colours to the main submenu categories so frequently used sections are
  easier to find.
- Added **NOT FOR SALE** below the version on the main menu.
- Increased acceleration when holding L2/R2 on large numeric menu values.
- Removed one-at-a-time weapon granting and arbitrary mission selection from
  Release builds. These development tools remain available only in Debug
  builds.

## Calibration and update compatibility

- Expanded the Classic release baselines and added separate Modern weapon and
  vehicle profiles, including mirrored aim, support grips, laser, seating,
  steering-wheel, and handle transforms where applicable.
- Existing **vr_settings.ini** values remain absolute player overrides.
- Built-in defaults are used only when a setting is absent and are never added
  to an existing player value.
- Updating does not replace **reVC.ini** or **vr_settings.ini**.

## Stability fixes

- Fixed a texture-store assertion that could occur while exiting the game.
- Stabilized weapon/support-grip updates after live calibration changes.
- Removed release-time diagnostic logging that could affect frame pacing.
- Preserved safe in-place restart and new-game behavior.

## Existing controls worth knowing

- Open VR settings: **both grips + Menu**.
- Menu fallback: **both grips + both triggers + X / left primary / L3**.
- Recenter: **both grips + L3 + R3**.
- Toggle debug overlay: **both grips + A**.
- Start or stop a performance capture: **both grips + Y**.
- Fire while driving in any driving mode: **B**.
- RC helicopter bomb: **A**.

The in-headset menu also contains graphics, HUD, locomotion, vehicles, traffic,
weapon/vehicle calibration, holsters, accessibility, diagnostics, cheats, and
About.

## Updating from v0.4.1

Extract the new archive over the existing Vice City VR files. The release does
not include **reVC.ini** or **vr_settings.ini**, so the player's preferences,
weapon calibration, holster setup, and vehicle calibration remain intact.

After updating:

1. Start **reVC.exe**.
2. Open **VR Menu > Model Assets**.
3. If a Modern overlay is installed, select the desired Modern categories.
4. Confirm **Vegetation / Palms: Classic**.
5. Fully restart after changing an asset category.

For a clean cross-vendor graphics baseline, use 100% Render Scale, Temporal AA
Off, and FXAA On before enabling DLAA, DLSS, FSR 2, higher supersampling, or
VRS.

## Known issues

- Ammu-Nation wall displays do not visibly highlight the currently selected
  weapon in VR. The HUD weapon name and price are the selection indicator.
- Occlusion culling has been stable in repeated testing, but a rare stereo
  artifact was observed once during development. If geometry or one eye jumps,
  flickers, or is incorrectly hidden, switch **VR Menu > Graphics > Occlusion
  Culling** to Off. The toggle applies immediately.
- Modern vegetation is intentionally not the recommended default because the
  tested HD palms are exceptionally geometry-heavy.
- The complete campaign has been finished by players, but the release has not
  been validated on every headset, OpenXR runtime, controller profile, and
  optional asset combination.
- Manual physical magazine reload remains limited to supported one-handed
  firearms.
- Original ASI, CLEO, Modloader, and binary limit-adjuster plugins are not
  compatible unless separately ported to reVC.

## Reporting problems

When reporting an issue, include:

- Headset and controller models.
- GPU and driver.
- Active OpenXR runtime and PCVR connection method.
- Traffic percentages.
- Classic/Modern choice for every affected asset category.
- Exact reproduction steps.
- Relevant logs beside **reVC.exe**.

Use [GitHub Issues](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/issues)
or open the
[Flat2VR Discord discussion](https://discord.com/channels/747967102895390741/1529621098751197365)
(existing Flat2VR server membership is required for this channel deep-link).

## Legal

Vice City VR is an unofficial fan-made project and is not affiliated with,
endorsed by, or sponsored by Rockstar Games, Take-Two Interactive, Meta, or the
authors and hosts of optional replacement packs.

The release does not contain the original game or optional third-party
replacement assets. Players must obtain the game and external mods legally.
Do not redistribute a locally generated **modelsets\modern** directory because
it contains data derived from original game files and third-party mods.
