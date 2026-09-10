# DLSS 5 optimization regression checks

From the source-kit root, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\dlss\test-nr-optimizations.ps1
```

Requires Visual Studio 2022 C++ build tools and a Windows SDK. Use `-MSBuild`
to select another installation and `-OutputDirectory` to select a new build
directory outside the source kit. By default, outputs stay in a unique temporary
directory printed by the script; no game installation or settings are touched.

The suite tests scene-scale migration, ordinary DLSS quality preservation,
model dimensions, foveation bounds and invalid inputs. Source checks verify
menu removal, saved-setting precedence and history resets. It also compiles the
production shared-stereo, foveation and model-scale compute shaders, and checks
three- and four-component source-kit versions. No NVIDIA SDK, model, headset,
download or game data is required.

These are deterministic host checks and shader compilation, not GPU execution,
model inference, headset acceptance or performance measurements.
