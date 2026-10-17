# Linux (Steam Play / Proton)

The PCVR build is a Windows x64 program, but it runs on Linux through Proton.
Proton Experimental translates Direct3D 12 with vkd3d-proton and forwards the
game's OpenXR session to the Linux SteamVR runtime, including the
`XR_KHR_D3D12_enable` path this build uses. No native Linux build is needed.

Community-tested configuration: CachyOS, niri (Wayland), NVIDIA RTX 3090,
PlayStation VR2 on SteamVR, Proton Experimental, player build 0.5.5.1. Other
distributions, GPUs and headsets are expected to behave similarly but have not
been confirmed. Treat this as a community guide, not an official support matrix.

## Install

1. Install the original Grand Theft Auto: Vice City (Steam app 12110) and make
   a separate copy of its folder, for example `~/Games/ViceCityVR`. Steam's
   compatibility rating for the original game does not apply here: the original
   `gta-vc.exe` is not used, only its game data.
2. Extract the player ZIP and copy the **contents** of its
   `Vice-City-VR-v0.5.5.1-PCVR` folder into that copy, beside `gta-vc.exe`.
3. Run the Linux runtime setup from the game folder (only `python3` is needed):

   ```sh
   sh SETUP_RUNTIME_LINUX.sh
   ```

   Choose Install, review the download sources, type `INSTALL` and wait for
   `INSTALLED`. It installs the same five BASE files as `SETUP_RUNTIME.bat`,
   with the same pinned sizes and SHA-256 values, and backs up replaced files
   under `dlss5-backups`. Run it again and choose Restore to undo. Windows
   Authenticode signatures cannot be checked on Linux; the pinned hashes
   identify the exact NVIDIA-signed files instead. Neural DLSS 5 is not offered
   because it has not been validated under Proton.
4. In Steam, choose **Games > Add a Non-Steam Game**, browse to `reVC.exe` and
   add it. In its **Properties > Compatibility**, force **Proton Experimental**.
   Older Proton versions may lack D3D12 support in their OpenXR bridge.
5. Recommended **Launch Options**:

   ```text
   PROTON_ENABLE_NVAPI=1 %command%
   ```

   Add `PROTON_LOG=1` when reporting a problem; Proton then writes
   `~/steam-<appid>.log`.
6. Start SteamVR first and wait for the headset's home environment, then start
   the game from Steam.

The game creates its own Proton prefix on the first start; this takes a moment.
Proton normally provides the Visual C++ runtime. If a `MSVCP140.dll` or
`VCRUNTIME140.dll` error appears, install it into the game's prefix with
`protontricks <appid> vcrun2022`.

## Troubleshooting

- **Black headset image or a crash during start-up:** close the game and add
  `StreamlineEnabled=0` under `[VR]` in `vr_settings.ini`. This skips Streamline
  initialization; FXAA and FSR2 remain available, DLAA does not.
- **SteamVR does not start (`VRInitError_Compositor_CannotDRMLeaseDisplay` in
  `vrcompositor.txt`):** this is a SteamVR/headset issue, not the game. The
  compositor could not lease the headset display. Check that the headset's
  DisplayPort connector reports `connected` under `/sys/class/drm/`; with a
  PlayStation VR2 PC adapter, unplugging and reconnecting the adapter restores
  it. Wayland compositors must support `wp_drm_lease_device_v1`.
- **Mission cutscene black in the headset:** `CUTSCENE_MODE.bat` cannot run on
  Linux. Set `CutsceneMode=1` (stereo) or `CutsceneMode=0` (cinema screen)
  under `[VR]` in `vr_settings.ini` instead.
- **Logs:** the game writes `openxr_d3d12.log` beside `reVC.exe`; SteamVR logs
  are in Steam's `logs` folder.

## PlayStation VR2 Sense controllers

The game suggests bindings for Touch, Index, Vive, Windows Mixed Reality and
generic controllers. SteamVR automatically converts the Touch bindings for
PlayStation VR2 Sense controllers, but the conversion currently loses two
inputs:

- the menu action stays on a nonexistent `menu` input, so neither the pause menu
  nor **both grips + Menu** works;
- the right-stick click is dropped, which breaks R3 actions such as sprint and
  the both-sticks recenter chord.

Fix both in **SteamVR Settings > Controllers > Manage Controller Bindings**:
select reVC, edit the active binding, bind **Create** (left) to `menu` and the
**right stick click** to `stick_click`, then save. Until then, the alternative
shortcut **both grips (L1 + R1) + both triggers + left stick click** opens VR
Settings without the menu button. On Sense controllers the grip action is on
L1/R1.
