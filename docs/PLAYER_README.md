# Vice City VR 0.5.5 - PCVR player quick start

Use **Vice-City-VR-v0.5.5-PCVR.zip** to play. The source-kit archive is for
developers; you do not need to compile anything to use the player archive.
This is an experimental Windows x64 Direct3D 12 / OpenXR mod for the original
PC Grand Theft Auto: Vice City (2003), not the Definitive Edition.

## Install

1. Make a separate copy of your legally owned original Vice City installation.
   Keep your original game and saves backed up.
2. Extract the ZIP completely. Copy the **contents** of its
   `Vice-City-VR-v0.5.5-PCVR` folder into that game copy, beside `gta-vc.exe`.
   Do not run it inside the ZIP or from an empty folder without the game data.
3. Install the [Microsoft Visual C++ v14 x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
   if it is not already installed. This is a runtime, not Visual Studio.
4. Run **SETUP_RUNTIME.bat** once. Choose Install, review the download sources,
   type `INSTALL`, and wait for `INSTALLED`. This installs required dependencies,
   not the optional DLSS 5 model. No Git, compiler or administrator rights needed.
5. Connect your headset and make sure its PC OpenXR runtime is active (for
   example Meta Quest Link or SteamVR). Then run **reVC.exe**.

The ZIP contains no NVIDIA DLLs or models. Setup downloads pinned files from
NVIDIA and the disclosed RankFTW community mirror, verifies them, and keeps
backups of replaced DLLs. It does not launch the game or modify VR preferences.
Review the sources and terms before accepting. If you do not accept them, use
the manual dependency instructions in `docs/DLSS5_SETUP.md`.

The game starts in VR by default. No `vr_settings.ini`, saves or personal
calibrations are supplied, so an upgrade retains your existing preferences.
Expensive new effects and Neural Rendering are opt-in on a fresh installation.

## Optional DLSS 5

Run **INSTALL_DLSS5.bat**, choose Install, select this game's `reVC.exe`, and
choose your profile. RTX 50 uses a NVIDIA-signed NR model from a community
mirror; RTX 40 uses a modified experimental model with an additional warning.
That exact RTX 40 download differs from the private development model and has
not been confirmed in a headset. Other GPUs are not validated for NR.

After setup, open VR Settings > Graphics, select DLAA as Temporal AA, turn
Neural DLSS 5 on, and start with 1X. Confirm `ACTIVE` and compare the image;
`INSTALLED` alone only means the files passed installation checks. 2X/3X are
extra rendering passes, not generated frames, and are much more expensive.
Run the installer again and choose Restore to undo its DLL changes.

## Included features

- Native DX12 rendering, stereoscopic 6DoF / room-scale OpenXR, tracked hands
  and weapons, VR driving, calibration, comfort controls and in-headset menus.
- VRS/foveated shading on supported hardware, FXAA, FSR2 native AA, DLAA/DLSS,
  motion vectors, mipmaps and foliage filtering.
- Optional lighting, car/ocean reflections, rain and puddles with separate
  controls; cutscene choices, customizable HUD and asset-profile support.

This public ZIP includes the MIT-licensed UltimateXR hands, but **not** GTA
game data, Modern/Xbox vehicle packs or optimized vegetation packs. Supply
separately obtained compatible asset packs yourself; Classic uses your game's
existing assets. Graphics features do not require those replacement packs.

## If it does not start

- `sl.interposer.dll` missing: run SETUP_RUNTIME first, in the installed game.
- `MSVCP140.dll` or `VCRUNTIME140*.dll` missing: install the Microsoft x64 runtime
  linked above, not individual replacement DLLs.
- `mss32.dll` missing: you are running a different/old EXE. This package uses
  OpenAL, not Miles. Check that you extracted the complete new archive.
- A blank/flat headset view: check the active PC OpenXR runtime and connection.
  If upgrading, check that your own `vr_settings.ini` does not enable FlatMode.
- Do not download individual missing DLLs from random sites or disable Windows
  security checks. Keep the full error and game log when reporting a problem.

See `docs/VR_README.md`, `docs/VR_PERFORMANCE.md`, and `docs/DLSS5_SETUP.md` for
controls, troubleshooting and limitations. This package has been checked for
file integrity and dependencies; the newly downloaded NR profiles are not a
promise of compatibility or stable frame rate on every PC/headset.

Source: https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr
Keep the bundled licenses and dependency sources with redistributions.
