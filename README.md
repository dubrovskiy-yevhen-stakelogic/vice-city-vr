# Vice City VR

![Vice City VR logo](logo.png)

**Current version:** v0.5.2-alpha

**Platform:** Windows PCVR

> [!TIP]
> **Join the Flat2VR Discord!** Development updates, player feedback, testing,
> and discussion of the mod take place in the
> [Vice City VR discussion channel](https://discord.com/channels/747967102895390741/1529621098751197365).
> Join the Flat2VR server first if the channel link does not open for you.

Vice City VR is an unofficial VR conversion of the original 2003 PC release of
*Grand Theft Auto: Vice City*. It adds stereoscopic 6DoF OpenXR rendering,
tracked hands, physical weapons, immersive vehicles, VR-native menus, and
comfort controls while preserving the original game and campaign.

> [!IMPORTANT]
> Vice City VR does not include Vice City or replacement game assets. A legally
> obtained copy of the original 2003 PC release is required.
>
> The main game/runtime source remains private during active development. This
> public repository contains release downloads, player documentation, and
> standalone utilities that let players build an optional Modern asset overlay
> from their own game and separately downloaded mods.

## Features

- Full stereoscopic PCVR through OpenXR with tracked head movement.
- Left- and right-hand VR models, motion-controlled firearms, dual wielding,
  two-handed support grips, physical scopes, weapon hand-offs, and configurable
  per-weapon calibration.
- Physical punching, melee combat, grenades, Molotovs, remote explosives, and
  optional physical magazine reloading for supported firearms.
- Body holsters that follow the player's view, including holstering while
  Weapon Grip Lock is enabled.
- Independently selectable Default, Immersive, and Motion control modes for
  cars and motorcycles, including one-handed physical steering, bike throttle,
  drive-by shooting, and per-vehicle calibration.
- A sector-aware traffic system with independent pedestrian and vehicle density
  controls from 50% to 300%.
- Smooth and snap turning, body-relative or head-directed movement, walking
  head-bob control, motorcycle horizon lock, and headset recentering.
- Configurable VR HUD, world-locked presentation for menus and cutscenes, and
  an in-headset About screen.
- Direct3D 12 single-pass stereo, Render Scale, VRS, FXAA, NVIDIA DLAA/DLSS,
  AMD FSR 2 Native AA, and optional occlusion culling.
- Optional Classic/Modern asset mixing by category: world/buildings,
  vegetation/palms, vehicles, pedestrians, and weapons.

The full story campaign has been completed in VR by players. This is still an
alpha release, so hardware-, runtime-, and mission-specific issues may remain.

## Download

Download the Windows PCVR package from
[GitHub Releases](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/releases).

The release archive is named **Vice-City-VR-v0.5.2-alpha.zip**. It does not
replace **gta-vc.exe** and does not contain the original game.

## Requirements

- Windows 10 or Windows 11, 64-bit.
- A Direct3D 12-capable GPU.
- A PC VR headset and an active OpenXR runtime.
- A legal PC installation of *Grand Theft Auto: Vice City* (2003).
- Microsoft Visual C++ 2015-2022 Redistributable (x64).

NVIDIA DLAA/DLSS requires a compatible RTX GPU and a current driver. FSR 2
Native AA and the standard FXAA path are available on other supported GPUs.

## Installation

1. Install a legal PC copy of *Grand Theft Auto: Vice City* (2003).
2. Back up important saves.
3. Extract the contents of the **Vice-City-VR-v0.5.2-alpha** folder into the
   game directory, next to **gta-vc.exe**.
4. Select your headset software as the active OpenXR runtime.
5. Connect the headset, then run **reVC.exe**.

Keep **gta-vc.exe** and all original game-data folders. Updates preserve
**reVC.ini** and **vr_settings.ini** when those files already exist.

## First start and VR menu

The welcome card appears after Tommy takes his first controllable step in the
game world, not during startup menus or cutscenes. It explains the main VR
features and can later be reopened from **VR Menu > About**.

> [!IMPORTANT]
> **The normal, easiest menu shortcut is: hold both Grip buttons, then press
> the Menu button once.** You do not need to press both triggers or X unless
> your controller/runtime does not expose a working Menu button.

- Open the VR menu with **both grips + Menu**. This is the recommended method.
- If the controller has no usable Menu button, hold **both grips + both
  triggers**, then press **X / left primary** or the **left stick click**.
- Recenter with **both grips + L3 + R3**.
- Inside menus, use the left stick to select, L2/R2 to change values, A or the
  right stick click to activate, and B or the left stick click to go back.

Oculus Touch, Valve Index, HTC Vive, Windows Mixed Reality, and the Khronos
Generic Controller profile have built-in OpenXR bindings. Some SteamVR
controller combinations may still need custom bindings.

## Immersive motorcycle controls

Immersive motorcycle steering starts when a hand grips a nearby handlebar.
Motorcycle acceleration uses the **right trigger by default**, so a new player
can ride immediately after grabbing the bars.

**VR Menu > Vehicle Settings > Bike Throttle** is deliberately highlighted:

- **Right Trigger (Recommended)** - normal, discoverable acceleration.
- **Manual Wrist Twist (Advanced)** - hold the right handle, hold its trigger,
  then physically twist the controller like a motorcycle throttle.

The Immersive HUD has a shared global motorcycle layout. Open a HUD panel's
calibration while not seated on a bike to edit that global layout. Open the
same calibration while seated on a bike to create an override for that exact
motorcycle model. Until an override is changed, that model inherits the global
layout; **Reset This Model to Global** removes the override.

## Copy PC saves to Quest

Connect an authorized Quest by USB, close the game on both devices, then
double-click **TRANSFER_PC_SAVES_TO_QUEST.bat**.

The wizard:

- finds the PC save folder or asks you to select the Vice City VR folder;
- finds an existing ADB installation, or offers to download Google's official
  Android platform-tools;
- detects every available PC save slot;
- converts all compatible Win64 Vice City VR saves to the Quest format;
- backs up occupied Quest slots before replacement; and
- reads every transferred slot back to verify it.

Original retail, legacy x86, unknown, and corrupt saves are rejected instead of
being written. The diagnostic log is saved as
**%TEMP%\ViceCityVR-PC-Saves-To-Quest.log**.

## Optional Modern assets

The release works with the original Classic assets. Replacement assets are
optional and are never bundled.

Double-click **PREPARE_MODERN_MODELS.bat** and select the legal Vice City game
folder. The public wizard downloads the two tested source packs, verifies and
caches them, builds a staged overlay, and installs it only after final
validation. A retry reuses completed downloads and extractions. HD vegetation
is deliberately excluded, so the expensive replacement palms remain Classic.
See [Building the optional Modern asset set](MODERN_MODELS.md) for hashes,
command-line use, safety guarantees, and troubleshooting.

After building the overlay, open **VR Menu > Model Assets**. You can independently
choose Classic or Modern for:

- World / Buildings
- Vegetation / Palms
- Vehicles
- Pedestrians
- Weapons

Every asset-category change requires a full game restart. On a fresh v0.5.2
configuration, **Vegetation / Palms defaults to Classic** even when other Modern
categories are selected. This is intentional: the tested HD palms contain far
more geometry than the original vegetation and can dominate GPU frame time.
Modern vegetation remains available as an opt-in choice.

Existing **vr_settings.ini** files are preserved during an update. If you used
an earlier test build, confirm that **Vegetation / Palms** is set to **Classic**.

## Changes in v0.5.2-alpha

- Added optional Modern assets and per-category Classic/Modern mixing.
- Added the high-density traffic system and independent 50-300% pedestrian and
  vehicle controls.
- Added occlusion culling to avoid rendering large parts of the city hidden
  behind nearby geometry.
- Reworked two-handed weapon support, mirrored hand orientation, calibration,
  holsters, and Grip Lock interaction.
- Reworked one-handed immersive motorcycle and car controls, including stable
  steering, right-trigger acceleration by default, and optional physical wrist
  throttle.
- Added global and per-motorcycle-model Immersive HUD layouts with explicit
  inheritance and reset-to-global behavior.
- Added the one-click PC-to-Quest save-transfer wizard.
- Expanded the Classic calibration baselines and added separate Modern
  weapon/vehicle profiles.
- Added the first-run welcome card, About page, coloured VR-menu categories,
  and the **NOT FOR SALE** notice.
- Removed one-at-a-time weapon granting and arbitrary mission selection from
  Release builds; developer tools remain available in Debug builds.
- Fixed an assertion that could occur while leaving the game and made
  performance diagnostics opt-in.

For the full list, see [Vice City VR v0.5.2 GitHub release text](GITHUB_RELEASE_v0.5.2.md).

## Known issues

- Ammu-Nation wall displays do not visibly highlight the selected weapon in VR.
  Use the weapon name and price shown on the HUD as the selection indicator.
- Occlusion culling is enabled by default. If geometry or one eye briefly
  jumps, flickers, or appears incorrectly culled, open **VR Menu > Graphics >
  Occlusion Culling** and switch it off. The toggle applies immediately.
- Modern vegetation is extremely demanding compared with Classic vegetation.
  DLSS, Render Scale, and VRS reduce pixel cost but do not remove its extra
  geometry. Keep **Vegetation / Palms: Classic** for the recommended profile.
- Original ASI, CLEO, Modloader, and binary-patching plugins are not compatible
  unless they are separately ported to reVC.

## Reporting problems

Please include:

- Headset and controllers.
- GPU and active OpenXR runtime.
- PCVR connection method.
- Selected asset categories and traffic percentages.
- Exact reproduction steps.
- Relevant logs beside **reVC.exe**.

Report reproducible issues through
[GitHub Issues](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/issues),
or open the
[Flat2VR Discord discussion](https://discord.com/channels/747967102895390741/1529621098751197365)
(existing Flat2VR server membership is required for this channel deep-link).

## Legal notice

Vice City VR is an unofficial fan-made project. It is not affiliated with,
endorsed by, or sponsored by Rockstar Games, Take-Two Interactive, Meta, or the
authors and hosts of optional replacement-asset packs.

*Grand Theft Auto*, *Grand Theft Auto: Vice City*, and all related trademarks
and game assets belong to their respective owners. Players must legally obtain
the game and all optional mods themselves. Neither the release nor this
repository grants permission to redistribute original game data, third-party
mods, or a generated Modern overlay.
