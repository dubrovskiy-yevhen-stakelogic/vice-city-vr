# Experimental DLSS Neural Rendering integration

The Direct3D 12 integration supports OpenXR stereo and the existing desktop
renderer. Neural Rendering is separate from ordinary DLAA/DLSS and remains
opt-in. NVIDIA runtime DLLs, CUDA models and any locally modified NVIDIA files
are not part of the public patch-only source kit.

## Runtime inputs

The game loads the user-supplied x64 runtime set beside `reVC.exe`:

- Streamline: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.pcl.dll`;
- DLAA / Super Resolution model: `nvngx_dlss.dll`;
- optional NR plugin and model: `sl.dlss_nr.dll`, `nvngx_dlssnr.dll`;
- project-built forwarder: `nvngx.dll_dlssnr.dll`.

The executable imports `sl.interposer.dll` at startup, even when temporal AA
is OFF. It is therefore a base runtime dependency; the optional NR files are
not required to start the game. The OpenAL build does not use `mss32.dll`.

The tested integration uses Streamline 2.13.0, DLSS SR 310.7.0 and NR 310.8.0.
These version strings alone do not guarantee GPU compatibility or authenticity.
Obtain SDK/runtime components yourself under their applicable terms from
authorized sources. The public kit provides connection instructions, not a
download or redistribution of an unlocked model. Do not install replacement
NGX DLLs into Windows system directories.

Build the forwarder from the assembled source with Visual Studio 2022:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  .\tools\dlss\forwarder\dlssnr_forwarder.vcxproj `
  /p:Configuration=Release /p:Platform=x64
```

Its output is `tools\dlss\forwarder\bin\Release\nvngx.dll_dlssnr.dll`. The
public `BUILD_PC.bat` builds this project as well as the game. The forwarder is
a derivative of Dagherbou/OptiScaler_DLSSNR commit
`393e0706b950a0ff1498e9dcf66989a80de72f31`, under GPL-3.0-or-later; retain the
license at `tools/dlss/COPYING-GPL-3.0.txt` when distributing its source.

## Processing and controls

In VR Settings / Graphics, select the DLAA temporal backend, enable
`NEURAL DLSS 5`, choose a profile and leave `DLSS 5 PASSES` at `1X` initially.
`NR MODEL SCALE` offers Full, Quality, Balanced, or Performance without shrinking
the original scene. With NR enabled, `DLSS MODE` stays at full-resolution DLAA;
ordinary DLSS SR remains available when NR is disabled. Keep the user's existing
presentation mode; flat mode is not required for setup. Model scale, foveation
and SHARED processing are VR-only options.

The VR render chain is:

```text
scene -> optional central crop and model resize -> NR (1X / 2X / 3X)
      -> neural changes composed onto original scene -> 1:1 DLAA -> output
```

Lower model scales resize only the private NR input. Foveation independently
restricts its area; SHARED runs the pass chain on the left eye and reprojects
the neural changes to the right. Each pass has its own temporal context and
ping-pong outputs; PER EYE keeps independent NR contexts for both eyes.
Both original stereo views and both final DLAA reconstructions remain separate.

With empty hands, both grips + B toggles the chosen profile against its normal
DLAA/DLSS baseline. The baseline bypasses NR evaluation. It does not cycle all
profiles, use split-eye display, or extrapolate the model answer by 20X.
`2X` and `3X` are sequential model evaluations, not frame generation. They
increase GPU work and memory use substantially and remain optional experiments.

The existing desktop diagnostic panel uses F11 to show/hide options and F12
to reset them, including pass count. It is not required for the VR controls.

## Resource and runtime validation

Each eye's shared double-wide scene color is copied into a local texture
before NR evaluation, with color X/Y origins zero. The tested model rejects a
nonzero right-eye color offset with `0xBAD00005`. Depth retains its valid
per-eye offset in the double-wide source. After NR, Streamline constants are
submitted once per frame/viewport with the final input-history reset state.

The Graphics menu reports `ACTIVE` only when neural output was successfully
selected for reconstruction in both eyes. PER EYE evaluates both separately;
SHARED evaluates the left and validates the composed stereo pair. A loaded
plugin or feature creation alone is not acceptance. In PER EYE mode, look for
both eye reconstruction lines in the current `streamline_dlaa.log`:

```text
[DLSS-NR] slot 0 1x sequential evaluation active: work ... -> output ...
[DLSS-NR] slot 1 1x sequential evaluation active: work ... -> output ...
[DLSS-NR] eye 0 reconstructed local NR color ... -> ...
[DLSS-NR] eye 1 reconstructed local NR color ... -> ...
```

Read subsequent failures too. Resource creation takes a warmup frame after a
mode change; a fallback baseline may look fast while NR is not active. Model
output quality and stable headset frame pacing still require visual and timed
testing, independently of a successful GPU evaluation.

The implementation includes narrowly matched experimental compatibility work
for the locally tested plugin ABI, including a bounded in-memory adjustment
of a known plugin build. It does not alter the plugin file on disk or provide
compatible CUDA code for an unsupported model. This is not an NVIDIA-supported
hardware compatibility guarantee; the proprietary model remains user supplied.

Verbose Streamline logging is disabled for normal timing. An empty
`sl_verbose` marker beside the executable enables diagnostic logging on the
next launch; remove it after diagnosis and before performance captures.
