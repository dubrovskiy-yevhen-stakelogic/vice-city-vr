# Vice City VR

![Vice City VR logo](logo.png)

**Current version:** `v0.4.1-alpha`
**Platform:** Windows PCVR

Vice City VR is an unofficial VR conversion of the original 2003 PC release
of *Grand Theft Auto: Vice City*. It adds full stereoscopic 6DoF VR, tracked
hands, physical weapon interaction, immersive vehicle controls, VR menus, and
comfort options while preserving the original game and campaign.

> [!IMPORTANT]
> **The source code is not currently public.**
>
> Vice City VR is still under active development. Its working source repository
> remains private while major systems are being completed, tested, and cleaned
> up. This public repository is intentionally used for release downloads and
> player documentation only.
>
> The source code is planned to be published when the project is ready for
> public development. There is no source download or public build guide at this
> time, and the absence of source files from this repository is intentional.

## Features

- Full stereoscopic 6DoF VR with tracked head movement.
- Tracked VR hands with configurable calibration.
- Motion-controlled firearms with independent hand aiming and dual wielding.
- One-handed and two-handed weapon handling, physical scopes, holsters, and
  weapon hand-offs.
- Physical punching, melee combat, grenades, Molotovs, and remote explosives.
- Optional physical magazine reloading for supported firearms.
- Independently selectable car and motorcycle driving styles: classic
  controls, immersive physical steering, and controller-based motion steering.
- Interactive car steering wheels and motorcycle handlebars, including
  one-handed driving and drive-by shooting.
- Per-vehicle seating and control calibration.
- Smooth and snap turning, adjustable sensitivity, body-relative or
  head-directed movement, optional walking head bob, motorcycle horizon lock,
  and other comfort options. Fresh installations use Head Directed movement
  with walking head bob disabled.
- In-headset settings, weapon calibration, holster configuration, cheat, and
  accessibility menus.
- Configurable VR HUD with radio and mission messages, plus world-locked
  theatre presentation for menus and cutscenes.
- Optional controller haptics for weapon fire with adjustable recoil strength.
- Improved draw distance, lighting, weather, rain, reflections, and visual
  effects, including world-stable VR clouds.
- Multiple anti-aliasing and image-quality options.

## Download

Download the newest Windows PCVR package from
[GitHub Releases](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/releases).

Release archives use the name `Vice-City-VR-v...zip`.

Vice City VR does **not** include the original game or any original copyrighted
game data. A legally obtained copy of the original 2003 PC release is required.

## PCVR installation

### Requirements

- Windows 10 or Windows 11, 64-bit.
- A Direct3D 12-capable GPU.
- A compatible PC VR headset and an active OpenXR runtime.
- A legally obtained PC copy of *Grand Theft Auto: Vice City* (2003).
- Microsoft Visual C++ 2015-2022 Redistributable (x64).

### Setup

1. Download the latest `Vice-City-VR-v...zip` release.
2. Extract its contents into the original Vice City game directory, next to
   `gta-vc.exe`.
3. Select the headset software you use as the active OpenXR runtime.
4. Connect or start the headset runtime, then run `reVC.exe`.

The mod does not replace `gta-vc.exe`. Keep the original executable and make a
backup of important saves while the project remains in alpha.

The mod uses the active Windows OpenXR runtime. Bindings are included for
Oculus Touch, Valve Index, HTC Vive, Windows Mixed Reality, and the Khronos
Generic Controller profile. Some SteamVR controller combinations may require
custom bindings.

## Opening the VR menu

- Hold **both grips + Menu**.
- On controllers without a usable Menu button, hold **both grips + both
  triggers**, then press **X** or the **left stick click**.

The in-headset menu contains graphics, HUD, movement, comfort, vehicle,
calibration, accessibility, and cheat settings.

## Changes in v0.4.1-alpha

- Restored temporary radio-station names, timed mission instructions, objective
  updates, and other short messages in the VR HUD.
- Added optional weapon-shot controller haptics with adjustable recoil
  strength.
- Added independent Default, Immersive, and Motion driving-mode selection for
  cars and motorcycles.
- Added Head Directed locomotion that keeps Tommy facing the actual movement
  direction without assisted camera turning or sideways running. It is the
  default movement direction on a fresh installation; the experimental
  assisted head-turn mode remains available separately.
- Added a walking head-bob toggle. Head bob is disabled by default on a fresh
  installation.
- Fixed the missing centre aiming dot in the tracked PSG-1 scope.
- Replaced the RPG scope's opaque centre rectangle with a transparent VR-safe
  reticle.
- Fixed sky clouds rotating with headset movement by keeping their VR geometry
  stable in the world.
- Moved Render Scale, VRS, Temporal AA, Temporal Jitter, FXAA, and the Vice City
  colour filter into a dedicated Graphics Settings submenu.
- Existing settings and weapon, hand, HUD, holster, and vehicle calibration
  values remain compatible and take precedence over built-in defaults.

## Alpha status

Vice City VR is still a work in progress. The complete campaign has not yet
been validated on every supported configuration, and mission-specific or
headset-specific issues may remain.

Before reporting a problem:

- Confirm that you are using the latest release.
- Include the headset, GPU, active OpenXR runtime, and PCVR connection method.
- Describe exact reproduction steps and attach relevant logs.
- Keep backups of important saves.

Problems can be reported through
[GitHub Issues](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/issues).

## Frequently asked questions

### Where is the source code?

It is currently kept in a private development repository. This public
repository intentionally contains release packages and documentation only.
The code is planned to be published after active development and cleanup have
reached a suitable state.

### Does the download contain Vice City?

No. Players must provide their own legally obtained copy of the original 2003
PC game. Original game assets must not be redistributed with the mod.

### Is this a finished release?

No. `v0.4.1` is an alpha release. It is playable, but bugs and incomplete edge
cases should still be expected.

## Credits

Thank you to everyone testing the project, reporting reproducible issues, and
helping improve the mod.

## Legal notice

Vice City VR is an unofficial fan-made project. It is not affiliated with,
endorsed by, or sponsored by Rockstar Games, Take-Two Interactive, or Meta.

*Grand Theft Auto*, *Grand Theft Auto: Vice City*, and all related trademarks
and game assets belong to their respective owners. This project does not grant
permission to distribute original game data and is not a replacement for
legally obtaining the game.
