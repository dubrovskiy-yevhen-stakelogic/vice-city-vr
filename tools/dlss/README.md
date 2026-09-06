# DLSS 5 player setup

Extract the entire setup ZIP, then double-click `INSTALL_DLSS5.bat` at its root.
Choose Install, select the `reVC.exe` in your playable Vice City VR installation,
choose your GPU profile and read the download consent prompts. No Git or build
tools are required. The installed game must already contain the project's
`nvngx.dll_dlssnr.dll` forwarder; this tool does not build or download game files.

Choose **0 - BASE** for the required Streamline loader plus ordinary DLSS/DLAA,
without installing Neural DLSS 5. It installs exactly five files: the Streamline
interposer, common, DLSS and PCL DLLs, plus `nvngx_dlss.dll`. Streamline still
comes from the disclosed third-party mirror; DLSS SR comes directly from NVIDIA.
The NR model is not downloaded and existing NR plugin/model files are left
untouched. After BASE setup, simply start the game's `reVC.exe` in VR. On
non-NVIDIA GPUs this supplies the loader, not support for NVIDIA DLSS/DLAA.

Streamline also requires the [Microsoft Visual C++ v14 x64 runtime](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
Install that prerequisite if Windows reports a missing MSVCP140/VCRUNTIME140 DLL.

The installer downloads official NVIDIA DLSS SR and mirrored Streamline/NR
files using fixed URLs and SHA-256 checks. NVIDIA signatures are checked on
unmodified files. The RTX 40 model is a separately accepted, hash-pinned,
community-modified experiment with an invalid signature and unverified in-game
compatibility. The signed RTX 50 profile is also not a runtime test.

Setup leaves VR settings unchanged and never starts the game. For an RTX50 or
RTX40 neural-rendering installation, enable DLAA and
Neural DLSS 5 in the game's Graphics menu yourself, start with one pass and
confirm ACTIVE. Installed files alone do not prove that the effect is working.

Choose Restore in the same launcher to restore a saved runtime set. Backups live
under the game's `dlss5-backups` folder. Newer manual DLL changes are preserved
by refusing a conflicting restore. Cached downloads can be reused, but their
hashes are rechecked. No NVIDIA binaries are included in this setup package.

## Maintainer checks

Run `test-installer.ps1` with Windows PowerShell 5.1 for offline fixture tests.
It does not launch a game or load the synthetic test DLLs. For download and
signature validation without modifying a game, use `install-dlss5.ps1` with
`-VerifyOnly -NoPrompt -Profile BASE -AcceptCommunityRuntime`, or select RTX50
instead for the signed neural-rendering package. The RTX40 profile
additionally requires `-AcceptModifiedModel`. A custom `-CacheDir` must be outside
the source kit. These switches do not establish headset/runtime acceptance.
