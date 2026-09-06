# Optional DLSS 5 / Neural Rendering setup

This is an experimental PC Direct3D 12 feature, separate from ordinary DLAA
and DLSS Super Resolution. It is optional: leave `NEURAL DLSS 5` off to use
the normal renderer, DLAA, or standard DLSS without neural model evaluation.
The source kit does not contain NVIDIA DLLs, model weights, SDK archives, or
modified NVIDIA files. Source integration alone does not make an unsupported
GPU or incompatible model compatible.

## Easy setup for players

You need the playable Windows x64 Vice City VR build, not just this source kit.
The game folder must contain `reVC.exe` and the project-built
`nvngx.dll_dlssnr.dll`. This installer does not compile or download the game.

1. Close Vice City VR. Extract the setup ZIP or the source-kit ZIP completely.
2. Double-click `INSTALL_DLSS5.bat`, choose **Install**, then select the game's
   `reVC.exe` in the file picker.
3. Choose the RTX 50 signed-model profile or the experimental RTX 40 profile.
4. Review the sources and type `INSTALL`. The modified RTX 40 model also
   requires typing `RTX40` to accept its separate warning.
5. Wait for download and verification. `INSTALLED` means the files are in place,
   not that neural rendering has been tested on your GPU.
6. Start the game yourself and use the Graphics settings described below.

No Git, compiler or headset is needed to run setup. It never starts the game,
changes VR/settings files, stops processes or modifies Windows driver folders.
If your game folder is not writable, move/extract the player build to a folder
you can write to instead of disabling Windows protections.

Run the same launcher again and choose **Restore** to undo an installation.
Original files and a restore manifest are kept in `dlss5-backups` inside the game
folder. Restore refuses to overwrite files changed since installation; keep the
backup if you manually update DLLs later. Reinstalling an identical set changes
nothing. Verified downloads are cached in the local application data directory
under `ViceCityVR/DLSS5Cache`; they can be reused on another run.

### Download sources and limitations

The catalog in `tools/dlss/packages.json` pins URLs, archive and DLL SHA-256
values, sizes and signature policies. The installer does not follow a moving
`latest` release or download and execute another installer.

- DLSS SR 310.7.0 comes directly from the
  [pinned NVIDIA DLSS SDK revision](https://github.com/NVIDIA/DLSS/tree/a291cc7d2cc642a51566f3dfd5376f635cd1b284).
- Streamline 2.13 comes from the
  [RankFTW community mirror](https://github.com/RankFTW/rhi-repo/releases/tag/streamline-2.13.0.0).
  The five installed DLLs have valid NVIDIA signatures and match the development
  build's Streamline files. Streamline 2.14 is not installed automatically.
- The RTX 50 profile uses the
  [NVIDIA-signed NR 310.8.0 model from the same mirror](https://github.com/RankFTW/rhi-repo/releases/tag/dlssnr-310.8.0).
- The RTX 40 profile uses the
  [modified NR 310.8.0-RTX40 model](https://github.com/RankFTW/rhi-repo/releases/tag/dlssnr-310.8.0-RTX40).
  Its embedded NVIDIA signature is invalid because its contents were changed.
  Only its exact pinned hash is accepted, with separate user consent. It is not
  the same file as the locally modified development model; its in-game
  compatibility has not been established. Other GPU families are not validated.

Original NVIDIA files must pass Windows signature validation. The exception is
limited to that one pinned modified RTX 40 model, not arbitrary unsigned DLLs.
Checksums establish which bytes were downloaded; they do not establish safety,
redistribution rights or runtime compatibility. Review the publishers' terms.
No NVIDIA files or model weights are bundled in the setup ZIP or source kit.

## Manual dependency setup

Use NVIDIA's [Streamline project](https://github.com/NVIDIA-RTX/Streamline)
and [official releases](https://github.com/NVIDIA-RTX/Streamline/releases)
for its SDK/runtime distribution. NVIDIA documents that binary artifacts are
distributed in release archives, not in the source-only Git checkout. Obtain
DLSS components under their own terms from their authorized source; the
[DLSS SDK](https://github.com/NVIDIA/DLSS) is a separate project. A generic
DLSS SR download is not a substitute for the Neural Rendering model.

Use a coherent, compatible x64 runtime set. Do not mix `sl.*` DLLs from
different Streamline versions, overwrite system driver DLLs, disable signature
checks, or download replacement DLLs from untrusted file-sharing sites.
If you do not want to use the community downloads above and an authorized
compatible NR model is not available to you, use ordinary DLAA/DLSS. The kit
contains installer source and connection instructions, not a bundled model.

The local integration was developed with Streamline runtime 2.13.0, DLSS SR
310.7.0 and NR model 310.8.0. These are compatibility observations, not a claim
that any file bearing those version strings is trusted or will work on every
GPU. The model/driver ABI and vendor support checks can change between builds.

## File layout

Close the game and back up your existing installation's runtime files before
making local changes. Keep the original game data in your own installation;
do not copy it into this source kit. Place the following beside `reVC.exe`:

| Component | Files |
| --- | --- |
| Base PCVR runtime | `openxr_loader.dll`, `OpenAL32.dll`, `libmpg123-0.dll`, `sl.interposer.dll` |
| Ordinary DLAA / DLSS components | `sl.common.dll`, `sl.dlss.dll`, `sl.pcl.dll`, `nvngx_dlss.dll` |
| Optional Neural Rendering | `sl.dlss_nr.dll`, `nvngx_dlssnr.dll` |
| Project-built NR forwarder | `nvngx.dll_dlssnr.dll` |

Keep `nvngx.dll_dlssnr.dll` under that exact name. It is built from source by
`BUILD_PC.bat`; it is not NVIDIA's `nvngx.dll`. The integration reuses the NGX
core initialized by Streamline/the graphics driver. Do not copy arbitrary
`nvngx.dll` or `_nvngx.dll` files into Windows system directories.

The current executable imports `sl.interposer.dll` at startup, so that file is
required even when temporal AA and neural rendering are disabled in settings.
An OFF setting cannot bypass a missing Windows loader dependency. This build
uses OpenAL audio, not the original game's Miles `mss32.dll` backend; use the
executable built by this kit with its documented runtime set.

The runtime components may have additional prerequisites supplied by their
upstream packages. Filenames being present does not prove that Windows can
load their dependencies, that feature creation succeeds, or that output is
actually displayed. To record local provenance without uploading any files:

```powershell
Get-ChildItem -LiteralPath D:\Games\ViceCityVR -File |
  Where-Object Name -Match '^(sl\.|nvngx)' |
  Get-FileHash -Algorithm SHA256
```

Keep these hashes and the driver/runtime versions with a bug report. Never
attach the proprietary model or modified DLLs to the source repository.

## Enable and compare in the headset

1. Open VR Settings, then Graphics. Keep the existing VR presentation mode;
   switching to flat mode is not required for installation or testing.
2. Select the DLAA temporal backend, enable `NEURAL DLSS 5`, choose one model
   profile, and start with `DLSS 5 PASSES` at `1X`.
3. Choose `DLSS 5 WORK SCALE`: `FULL`, `QUALITY`, `BALANCED`, or `PERFORMANCE`.
   This is the same render-resolution choice as `DLSS MODE`, not a second
   independent percentage. Full uses native-resolution DLAA reconstruction;
   lower modes use standard DLSS Super Resolution after NR.
4. Wait for resource creation and history warmup after changing resolution or
   pass count. Confirm the menu says `ACTIVE`, not `PREPARING` or `ERROR`.
5. With empty hands, hold both grips and press B to compare the selected
   profile against its normal DLAA/DLSS baseline. This changes the whole view,
   not one eye or half of the screen. The baseline bypasses NR evaluation.

The render chain is scene color at the selected work resolution, then 1-3 NR
passes, then one DLSS SR/DLAA reconstruction to the headset output resolution.
It covers the full image. It is not the older central crop/feather experiment.
Approximate linear work scales are 100%, 67%, 58%, and 50%, with dimensions
aligned to the renderer's constraints. Neither those percentages nor pass
count imply a fixed FPS multiplier.

`2X` and `3X` are expensive opt-in visual experiments. They retain separate
temporal contexts and intermediate images for each pass and eye. Start with
one pass and increase only while monitoring GPU frame time and memory. There
is no integrated frame generation that turns these into low-cost VR modes.

## Confirm actual output, not just loading

The game writes `streamline_dlaa.log` in its working directory (normally the
game folder). For VR, successful presentation produces entries for both eyes:

```text
[DLSS-NR] slot 0 1x sequential evaluation active: work ... -> output ...
[DLSS-NR] slot 1 1x sequential evaluation active: work ... -> output ...
[DLSS-NR] eye 0 reconstructed local NR color ... -> ...
[DLSS-NR] eye 1 reconstructed local NR color ... -> ...
```

The pass count and dimensions vary with the selection. A line saying the
plugin is loaded or the feature was created is not sufficient. The `ACTIVE`
state additionally requires NR output to be selected for reconstruction in
both eyes. Inspect subsequent errors as well: a previous success line does not
prove that a later mode change succeeded. On failure the game can use the
normal baseline; an unexpectedly high FPS alone is not proof of NR working.

Report the log, exact mode/pass count, per-eye work/output dimensions, hardware,
driver and DLL versions, plus an A/B observation. Close other GPU-heavy games
before a performance comparison. Do not post account details or proprietary
binaries. See [performance capture instructions](VR_PERFORMANCE.md).

If extra Streamline logging is requested for a specific initialization issue,
an empty file named `sl_verbose` beside the executable enables it on the next
launch. Remove that marker after the diagnostic run; verbose logging can
distort timing and should not be used for the release performance baseline.
