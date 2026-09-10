# Vice City VR 0.5.5.1 player-package notices

The root LICENSE applies only to original Vice City VR contributions, not to
the complete executable or third-party components. The game requires a legally
obtained original PC Vice City installation. No retail executable, GTA game
data, replacement model packs, NVIDIA runtime DLLs or model weights are included.
This project is not affiliated with Rockstar Games, Take-Two or NVIDIA.

## Game code and project source

The modified reVC executable is built from the public patch-only source kit:
https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr

The pinned upstream reVC baseline is commit
`026cd10f3fdbd92c089830e5067c4457c53c1b51` of
https://github.com/mrxenginner/reVC . Credit belongs to the re3/reVC contributors,
including aap, Fire_Head, shfil, erorcun, Nick007J and Serge.
The upstream README contains the following notice; this package does not
replace it with the project's MIT license:

> We don't feel like we're in a position to give this code a license.
> The code should only be used for educational, documentation and modding purposes.
> We do not encourage piracy or commercial use.
> Please keep derivate work open source and give proper credit.

The public source kit contains the project delta and renderer source, not a
complete upstream tree. Its assembly instructions identify the required base.

## Included components

- librw: MIT, `licenses/librw-LICENSE.txt`.
- Khronos OpenXR loader: Apache-2.0, `licenses/OpenXR-LICENSE.txt`.
- OpenAL Soft 1.21.0: LGPL-2.0-or-later and the separately licensed BSD/MIT
  portions documented in `licenses/OpenAL-Soft-*.txt`.
- libmpg123 1.26.3: LGPL-2.1-only and the separately licensed ICY text helper,
  `licenses/mpg123-COPYING.txt` and `licenses/mpg123-ICY-BSD.txt`.
- AMD FidelityFX FSR2 and the Microsoft D3D12 helper: MIT, with separate
  notices in `licenses`.
- Dear ImGui, ImGuizmo and ProggyClean: MIT, `licenses/LICENSE_imgui.txt`,
  `licenses/LICENSE_imguizmo.txt`, `licenses/LICENSE_proggyclean.txt`.
- LodePNG, stb headers and ini_parser: notices retained in
  `licenses/Embedded-Dependency-Notices.txt`.
- VRMADA UltimateXR hands: MIT, `models/vrhands/ULTIMATEXR_LICENSE.txt`.
  The source assets, revision and conversion are identified in
  `models/vrhands/SOURCE.md`; converted geometry/textures are included.

Complete corresponding upstream source archives for the separately shipped
OpenAL and mpg123 DLLs are included in `licenses/source`, with exact provenance
and hashes in `DEPENDENCY_SOURCES.md`. These are the same binaries recorded
there for the earlier release. They remain separate DLLs which recipients can
replace with compatible modified versions. The complete mpg123 source also
contains separately licensed programs/modules not present in this Windows DLL.

## DLSS-NR forwarder

`nvngx.dll_dlssnr.dll` is the project-built compatibility adapter, not a
proprietary NVIDIA model. It is derived from Dagherbou/OptiScaler_DLSSNR commit
`393e0706b950a0ff1498e9dcf66989a80de72f31`, under GPL-3.0-or-later.
Corresponding modified source, Visual Studio project, instructions and the
complete GPL text are included at
`licenses/source/dlssnr-forwarder/tools/dlss/`.

## External NVIDIA downloads

SETUP_RUNTIME.bat and INSTALL_DLSS5.bat download files only after the recipient
accepts their sources. NVIDIA components retain NVIDIA's terms. The RTX40 model
is modified community material and is not NVIDIA-signed or officially supported;
the installer explains this separately. Hash pinning is an integrity check,
not a redistribution grant, security certification or compatibility promise.
See `docs/DLSS5_SETUP.md` and `tools/dlss/packages.json`.
