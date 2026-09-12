# Optional DLSS 5 / Neural Rendering setup

This is an experimental PC Direct3D 12 feature, separate from ordinary DLAA
and DLSS Super Resolution. It is optional: leave `NEURAL DLSS 5` off to use
the normal renderer, DLAA, or standard DLSS without neural model evaluation.
The source kit does not contain NVIDIA DLLs, model weights, SDK archives, or
modified NVIDIA files. Source integration alone does not make an unsupported
GPU or incompatible model compatible.

## Easy setup for players

For the required runtime **without DLSS 5**, use `SETUP_RUNTIME.bat` or the
installer's **BASE** profile. This installs four Streamline DLLs and the ordinary
DLAA/DLSS SR DLL; it does not download or replace the NR plugin/model. The
executable needs `sl.interposer.dll` even with temporal AA off. Installing these
base dependencies is not a guarantee that NVIDIA AA will work on every GPU.
You can add the optional NR files later with `INSTALL_DLSS5.bat`.

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

### Setup says the game folder is a source kit

Older setup scripts reject any folder containing `patch-manifest.json`, even
when a working game is installed there. This can affect updates that leave
source-kit metadata beside `reVC.exe`.

Use the corrected setup scripts; there is no need to delete or rename metadata.
The installer accepts this mixed layout when `data/gta-vc.dat` and
`models/gta3.img` are present and nonempty, and still validates the x64 game
executable and project forwarder. A source-only folder remains rejected.
Runtime checksums, signature policies, backup and restore are unchanged.

### Download sources and limitations

The catalog in `tools/dlss/packages.json` pins URLs, archive and DLL SHA-256
values, sizes and signature policies. The installer does not follow a moving
`latest` release or download and execute another installer.

- DLSS SR 310.7.0 comes directly from the
  [pinned NVIDIA DLSS SDK revision](https://github.com/NVIDIA/DLSS/tree/a291cc7d2cc642a51566f3dfd5376f635cd1b284).
- Streamline 2.13 comes from the
  [RankFTW community mirror](https://github.com/RankFTW/rhi-repo/releases/tag/streamline-2.13.0.0).
  The five DLLs used by the NR profiles (four in BASE) have valid NVIDIA
  signatures and match the development
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
3. Choose `NR MODEL SCALE`: `FULL`, `QUALITY`, `BALANCED`, or `PERFORMANCE`.
   Start with FULL, then reduce the model scale while comparing quality and
   GPU frame time. With NR enabled, the scene stays at the selected headset
   render scale and uses full-resolution DLAA, regardless of a saved ordinary
   `DLSS MODE` setting. The old `SCENE SCALE` / `DLSS 5 WORK SCALE` row is removed.
4. Wait for resource creation and history warmup after changing resolution or
   pass count. Confirm the menu says `ACTIVE`, not `PREPARING` or `ERROR`.
5. With empty hands, hold both grips and press B to compare the selected
   profile against its normal DLAA/DLSS baseline. This changes the whole view,
   not one eye or half of the screen. The baseline bypasses NR evaluation.

The scene retains its resolution. Foveation optionally selects a central crop;
model scale resizes only a copy of that area for the 1-3 NR passes. The neural
changes are composed onto the original scene, then one full-resolution DLAA
pass resolves the result. Both A/B views use the same scene resolution.
The separate headset `RENDER SCALE` still works normally. Neither model-scale
percentages nor pass count imply a fixed FPS multiplier.

With NR disabled, `DLSS MODE` restores its saved ordinary DLAA / Quality /
Balanced / Performance choice. For older settings without `DLSS5ModelScaleMode`,
an enabled NR setup migrates the old `DlssMode` level to model scale; an explicit
new model-scale setting, including FULL, always takes priority. The obsolete
`DLSSNeuralRegion` key is ignored.

`2X` and `3X` are expensive opt-in visual experiments. They retain separate
temporal contexts and intermediate images for each pass and eye in
`PER EYE` stereo mode. Start with
one pass and increase only while monitoring GPU frame time and memory. There
is no integrated frame generation that turns these into low-cost VR modes.

### Experimental shared-eye processing

For new settings, `DLSS 5 STEREO` selects `SHARED (EXPERIMENTAL)` in Graphics.
It runs the NR chain once on the left eye and reprojects the
neural color changes to the right eye using the scene depth. Both scene views
and both DLAA/DLSS reconstructions remain separate. Missing or unreliable
correspondences use the unmodified scene color rather than copying the left
eye's image. Flat mode is unchanged.

Compare against `PER EYE` at the same render/model scales, pass count and model profile.
Look at foliage, nearby objects and newly exposed surfaces during head motion;
the shared mode is not guaranteed to match independent processing or provide
a particular speedup. The large temporary status strip identifies `SHARED` or
`PER EYE`. A sharing error is reported explicitly and bypasses NR for both
eyes; select `PER EYE` to return to independent processing. Missing settings
default to `SHARED`; the saved key is `DLSS5StereoMode=1` (`0` selects `PER EYE`).

### Experimental central NR area

`DLSS 5 FOVEATION` in Graphics offers `OFF`, `QUALITY` (default), `BALANCED`,
and `PERFORMANCE`. These select a fixed central NR crop of approximately
85% x 85%, 75% x 75%, or 65% x 65% of the work-resolution image, respectively;
dimensions are aligned to 16 pixels. The rest of the image keeps its original
scene color. A feathered blend joins the central neural result to that color
before normal full-frame DLAA/DLSS reconstruction. It is a native renderer
option, not a ReShade/OptiScaler installation or eye-tracked foveation.
The current experiment requires the direct NR backend. Other backend paths
report an explicit foveation error instead of silently processing the full
image. For the related central-region approach, see
[Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS).
This implementation runs inside the game's renderer; its add-on, injector
and OpenXR layer are not bundled or required.

Foveation is independent of `NR MODEL SCALE`, headset render scale, VRS, pass count and `STEREO`.
It can be combined with SHARED and multiple NR passes. For a 2X comparison,
keep both scales, model profile and stereo mode unchanged, then compare
foveation OFF with QUALITY before trying smaller areas. Examine the boundary
and peripheral detail during head motion and while looking off-center;
the blend can remain noticeable and there is no guaranteed FPS improvement.

The temporary status strip shows `FOV` and the selected mode. A foveation
failure is reported as an error rather than successful activation. Switch to
OFF to restore full-area NR. The saved key is `DLSS5FoveationMode=0..3`;
flat mode does not use this setting. DLSS 5 itself remains OFF until selected;
the SHARED/QUALITY defaults only choose how it runs after enabling it. Existing
saved stereo/foveation choices are preserved, including explicit zero values.

### Experimental NR-only model resolution

`NR MODEL SCALE` replaces the old neural work-scale control. It offers
`FULL` 100% (default), `QUALITY` 75%,
`BALANCED` 67%, and `PERFORMANCE` 50% per dimension. It resizes only the copy
sent to the NR model, without reducing the game's scene render or headset
output resolution. With foveation enabled, the percentage applies to the
selected crop. Model dimensions are aligned to the renderer's constraints.

For reduced modes, the model output is compared with its matching downsampled
original. That neural difference is enlarged and composed onto the original
full-resolution scene color, including the foveation feather when enabled.
This keeps the original detail as the base instead of enlarging the entire
low-resolution image. There is still only one final DLAA/DLSS reconstruction.
`FULL` bypasses the added model-scaling path and preserves the previous
full-density NR processing.

This is a VR-only, direct-NR-backend experiment and is opt-in below FULL.
It does not change SHARED or foveation choices. Keep `1X` for the first test,
then compare model FULL with QUALITY while keeping headset render scale, profile,
stereo mode and foveation unchanged. Smaller model inputs can alter fine
detail and motion stability; matched residual composition is not a promise
of identical quality or a fixed FPS gain.

The saved key is `DLSS5ModelScaleMode=0..3`. The large status strip shows
`NR MODEL` separately from `FOV`; errors are reported instead of `ACTIVE`.
Selecting FULL restores the previous model-resolution path. Flat mode ignores
this setting; new settings use FULL, with the legacy migration described above.

## Confirm actual output, not just loading

The game writes `streamline_dlaa.log` in its working directory (normally the
game folder). In `PER EYE` mode, successful presentation produces
entries for both eyes:

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
