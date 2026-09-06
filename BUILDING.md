# Building the assembled PC source

The build helper performs assembly in a new work directory, generates Visual
Studio projects, and optionally runs MSBuild. It never installs or launches the
game.

## Requirements

- Windows PowerShell 5.1 or newer;
- Git for Windows;
- Visual Studio 2022 with the Desktop development with C++ workload;
- a `premake5.exe` supplied outside this repository;
- OpenXR, NVIDIA Streamline, FidelityFX FSR2, OpenAL Soft, and mpg123 SDK trees.

The proprietary SDKs and their runtime binaries are deliberately external to
this source kit. Pass their local paths explicitly.

## Build command

Both the upstream checkout and work directory follow the assembly safety rules:
the checkout must be at the fixed clean commit, and the work directory must not
already exist.

```powershell
.\BUILD_PC.bat `
  -Revc D:\source\revc-base `
  -WorkDir D:\work\vice-city-vr-build `
  -Premake D:\tools\premake5.exe `
  -OpenXrSdk D:\sdk\openxr `
  -StreamlineSdk D:\sdk\streamline `
  -Fsr2Sdk D:\sdk\fsr2 `
  -OpenAlSdk D:\sdk\openal-soft `
  -Mpg123Sdk D:\sdk\mpg123
```

Use `-MSBuild <path>` when Visual Studio is installed outside its default
location. Use `-GenerateOnly` to stop after generating the solution.

The expected project-built Release artifacts are:

```text
<WorkDir>\source\bin\win-amd64-librw_d3d12-oal\Release\reVC.exe
<WorkDir>\source\tools\dlss\forwarder\bin\Release\nvngx.dll_dlssnr.dll
```

The forwarder is built from the GPL-covered source carried in the assembled
tree. NVIDIA's Streamline, NGX, and DLSS Neural Rendering runtime/model DLLs
remain external and must be staged separately under their applicable terms.
The [DLSS 5 setup guide](docs/DLSS5_SETUP.md) lists the exact filenames and how
to distinguish a loaded model from successful displayed output.

On success the helper prints each artifact size and SHA-256. That proves only
that compilation completed; runtime acceptance still requires a separate test
using a legally obtained game installation.
