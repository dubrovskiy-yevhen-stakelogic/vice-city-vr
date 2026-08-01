# Vice City VR

![Vice City VR logo](logo.png)

**Current version:** `v0.4.0-alpha`  
**Platforms:** PCVR and native Meta Quest  
**Developer:** **#yevhen4817 (Discord)**

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
- Three driving styles: classic controls, immersive physical steering, and
  controller-based motion steering.
- Interactive car steering wheels and motorcycle handlebars, including
  one-handed driving and drive-by shooting.
- Per-vehicle seating and control calibration.
- Smooth and snap turning, adjustable sensitivity, head-relative or
  body-relative movement, motorcycle horizon lock, and other comfort options.
- In-headset settings, weapon calibration, holster configuration, cheat, and
  accessibility menus.
- Configurable VR HUD and world-locked theatre presentation for menus and
  cutscenes.
- Improved draw distance, lighting, weather, rain, reflections, and visual
  effects.
- Multiple anti-aliasing and image-quality options in the PCVR release.
- Save transfer between the current PCVR and native Quest releases.

Some visual settings differ between PCVR and native Quest because the Quest
version runs directly on mobile headset hardware.

## Download

Download the newest PCVR or Quest package from
[GitHub Releases](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr/releases).

Release names identify the platform:

- `Vice-City-VR-v...zip` — Windows PCVR.
- `Vice-City-VR-Quest-v...zip` — native Meta Quest package, installers, and
  documentation.
- `Vice-City-VR-Quest-v...apk` — Quest application only. New players should
  normally download the complete Quest ZIP because the original game data must
  also be installed separately.

Vice City VR does **not** include the original game or any original copyrighted
game data. A legally obtained copy of the original 2003 PC release is required.

## PCVR installation

### Requirements

- Windows 10 or Windows 11, 64-bit.
- A Direct3D 12-capable GPU.
- A compatible PC VR headset and an active OpenXR runtime.
- A legally obtained PC copy of *Grand Theft Auto: Vice City* (2003).
- Microsoft Visual C++ 2015–2022 Redistributable (x64).

### Setup

1. Download the latest `Vice-City-VR-v...zip` release.
2. Extract its contents into the original Vice City game directory, next to
   `gta-vc.exe`.
3. Select the headset software you use as the active OpenXR runtime.
4. Connect or start the headset runtime, then run `reVC.exe`.

The mod does not replace `gta-vc.exe`. Keep the original executable and make a
backup of important saves while the project remains in alpha.

Meta Quest through Quest Link or Air Link is the primary tested PCVR setup.
OpenXR bindings are also included for common SteamVR controller profiles, but
non-Quest combinations may require custom SteamVR bindings.

## Native Meta Quest installation

The native Quest build runs directly on the headset. A gaming PC, Quest Link,
Air Link, and SteamVR are not required after installation.

### Requirements

- Meta Quest 3, which is the primary tested standalone headset.
- Developer Mode enabled on the headset.
- A USB data connection for initial installation.
- SideQuest or Android platform-tools (`adb`).
- A legally obtained copy of the original 2003 PC game.
- Windows, Linux, or Steam Deck in Desktop Mode for copying the owned game
  data to the headset.

### Setup

1. Download and extract the latest `Vice-City-VR-Quest-v...zip` release.
2. Connect the Quest over USB and accept its USB debugging prompt.
3. Install the included APK with SideQuest, or allow the included Linux script
   to install it automatically.
4. Copy the legally owned game data with the included installer:

   **Windows**

   ```bat
   install-game-data.bat "C:\Path\To\Grand Theft Auto Vice City"
   ```

   **Linux or Steam Deck**

   ```bash
   bash install-game-data.sh "/path/to/Grand Theft Auto Vice City"
   ```

5. Launch **Vice City VR** from the headset's Unknown Sources or Developer
   applications section.

Detailed installation, update, troubleshooting, and save-transfer instructions
are included inside the Quest ZIP.

Do not uninstall the Quest application during a normal update. Android may
remove its game data and saves when the application is uninstalled.

## Opening the VR menu

- Hold **both grips + Menu**.
- On controllers without a usable Menu button, hold **both grips + both
  triggers**, then press **X** or the **left stick click**.

The in-headset menu contains graphics, HUD, movement, comfort, vehicle,
calibration, accessibility, and cheat settings.

## Alpha status

Vice City VR is still a work in progress. The complete campaign has not yet
been validated on every supported configuration, and mission-specific or
headset-specific issues may remain.

Before reporting a problem:

- Confirm that you are using the latest release.
- State whether you are using PCVR or native Quest.
- Include the headset, GPU and active VR runtime where applicable.
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

No. `v0.4.0` is an alpha release. It is playable, but bugs and incomplete edge
cases should still be expected.

### Can the same save be used on PCVR and Quest?

Current releases include save-transfer tools for supported Vice City VR save
files. Follow the documentation supplied in the Quest package and always keep
a backup before conversion.

## Credits

Vice City VR is created and maintained by **#yevhen4817 (Discord)**.

Thank you to everyone testing the project, reporting reproducible issues, and
helping improve the mod.

## Legal notice

Vice City VR is an unofficial fan-made project. It is not affiliated with,
endorsed by, or sponsored by Rockstar Games, Take-Two Interactive, Meta, or
SideQuest.

*Grand Theft Auto*, *Grand Theft Auto: Vice City*, and all related trademarks
and game assets belong to their respective owners. This project does not grant
permission to distribute original game data and is not a replacement for
legally obtaining the game.
