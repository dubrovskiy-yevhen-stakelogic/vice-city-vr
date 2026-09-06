# Vice City VR patch-only source kit

Vice City VR brings the 2003 game to Windows x64 PCVR with a native Direct3D 12
renderer and a full OpenXR VR gameplay and interface layer.

Source-kit version: `0.5.5-alpha-pc`.

Already have the playable PCVR build? For optional DLSS 5 setup, double-click
[`INSTALL_DLSS5.bat`](INSTALL_DLSS5.bat), select your game's `reVC.exe` and follow
the prompts. It downloads pinned runtime files, checks them and backs up replaced
files. It uses a community mirror as well as NVIDIA; the RTX 40 option is a
modified, experimental model. No programming is needed. See
[the setup guide](docs/DLSS5_SETUP.md) before accepting the downloads.

## What changes from the original game

### Modern graphics and rendering

- **Native DX12 conversion:** a dedicated librw Direct3D 12 backend, with
  single-pass stereo for the main world rendering stages.
- **VRS / fixed-foveated shading:** selectable profiles on supported hardware,
  plus adjustable headset render scale.
- **DLAA and DLSS Super Resolution:** native-resolution NVIDIA temporal AA or
  Quality, Balanced and Performance reconstruction; FXAA and FSR2 native AA
  are alternative choices.
- **Experimental DLSS 5 Neural Rendering:** selectable model profiles, tuning,
  1X/2X/3X passes, Full/Quality/Balanced/Performance work scales, and an instant
  selected-profile-versus-baseline comparison with an in-headset status notice.
- **Temporal rendering inputs:** per-eye history and motion vectors for camera
  movement, moving vehicles and animated characters.
- **Improved texture filtering:** generated mipmaps, alpha-coverage handling
  for foliage and a separate foliage-softness adjustment.
- **Lighting and reflections:** optional dynamic per-pixel lights and
  screen-space reflections, with separate car, ocean and puddle controls;
  adjustable ocean waves, distortion, sheen and glints.
- **Expanded weather:** optional modern/cinematic rain, adjustable drop density
  and intensity, wet surfaces that dry over time, reflective puddles and
  independently controlled puddle ripples. Shadow, fog and fountain-quality
  controls are also available.

### Built for VR gameplay

- **6DoF and room-scale viewing:** native stereoscopic output, tracked head
  rotation and position, physical leaning and movement within your play space.
- **VR hands and weapons:** controller-directed aiming, two-handed grips,
  independently held weapons, body holsters, optional manual magazine reloading,
  scope aiming, physical melee swings and throws, and recoil haptics.
- **VR driving:** default, immersive and motion-control driving modes for cars,
  bikes and boats; virtual steering/handlebar controls and bike throttle gestures.
- **Personal calibration:** weapon/hand alignment, vehicle-specific seating and
  steering calibration, adjustable driving cameras and player-body visibility.
- **Movement and comfort:** smooth locomotion, snap or smooth turning,
  movement-orientation choices, head-bob controls, recentering and customizable
  controller bindings.
- **A full VR interface:** in-headset settings, colored submenus, scrolling,
  cheats, saved preferences and separate lighting, reflection and rain pages.
- **Customizable HUD:** classic or immersive wrist panels for map, status,
  clock and ammo, with independent placement, sizing and visibility controls.
- **Cutscene choices:** cinema-screen or stereoscopic presentation, camera
  selection and saved preferences.

### Customization and diagnostics

- **Mix-and-match asset profiles:** Classic/Modern choices for world,
  vegetation, vehicles, pedestrians and weapons, plus an Xbox vehicle profile
  and optimized-vegetation support. Asset packs are supplied separately;
  changing a profile requires a restart.
- **Built-in tools:** headset performance overlay and capture, desktop neural
  rendering controls, and a PC-to-Quest save-transfer tool.

The new lighting, reflection and weather effects are optional, with a master
switch and individual controls. Neural rendering is also opt-in: ordinary
DLAA/DLSS works independently of it. DLSS 5 requires compatible external NVIDIA
runtime/model files and can be very expensive, especially at 2X/3X in VR;
availability depends on hardware and runtime compatibility. See
[DLSS 5 setup](docs/DLSS5_SETUP.md), [VR controls and settings](docs/VR_README.md)
and [performance notes](docs/VR_PERFORMANCE.md).

## Source-kit contents

This repository carries only the project-authored delta needed to reproduce the
PC build. It does not redistribute the complete reVC source tree.

The fixed upstream baseline is:

- repository: `https://github.com/mrxenginner/reVC.git`
- commit: `026cd10f3fdbd92c089830e5067c4457c53c1b51`

The kit contains:

- `patches/`: text-only modifications to files that exist in the fixed baseline;
- `overlay/`: new project-authored source files;
- `librw/`: the audited source-only renderer fork copied to `vendor/librw`;
- `tools/source-kit/`: audit, assembly, and build-only scripts;
- `patch-manifest.json`: exact paths and SHA-256 values for every patch,
  preimage, result, overlay file, and librw file.

No game files, compiled binaries, signing material, proprietary SDKs, or media
assets belong in this repository.

## Assemble a source tree

Prepare an exact clean checkout first:

```powershell
git clone https://github.com/mrxenginner/reVC.git D:\source\revc-base
git -C D:\source\revc-base checkout --detach 026cd10f3fdbd92c089830e5067c4457c53c1b51
git -C D:\source\revc-base status --porcelain --untracked-files=all
```

The status command must print nothing. Then choose an output path that does not
exist:

```powershell
.\ASSEMBLE_SOURCE.bat -Revc D:\source\revc-base -Out D:\work\vice-city-vr-source
```

Assembly audits the kit, verifies the exact upstream commit and clean state,
exports the exact Git tree without workstation line-ending conversion, validates
the baseline's exact gitlink set, verifies every manifest hash, checks and
applies each patch, then copies the overlay and librw source. The input checkout
is never modified. Existing output paths are refused.

Run the publication audit independently with:

```powershell
.\AUDIT_SOURCE_KIT.bat
```

See [BUILDING.md](BUILDING.md) for compilation and [RELEASING.md](RELEASING.md)
for the release process.

For player-side optional neural rendering, see
[DLSS 5 setup](docs/DLSS5_SETUP.md). NVIDIA runtime and model files, including
any locally modified files used for hardware experiments, are not included.

## Manifest contract

`patch-manifest.json` uses schema version 1:

- `base`: exact repository, branch, commit, and Git tree IDs;
- `patches[]`: path relative to `patches/`, file SHA-256, and `targets[]`;
- every target: upstream-relative path, `preimageSha256`, and
  `postimageSha256`;
- `overlay[]`: output-relative path and SHA-256 relative to `overlay/`;
- `librw[]`: renderer-relative path and SHA-256 relative to `librw/`.

All manifest paths use forward slashes. Additions belong in the overlay, not in
the patch. Empty manifest arrays or hashes that do not match the release files
are rejected by the audit and assembler.
