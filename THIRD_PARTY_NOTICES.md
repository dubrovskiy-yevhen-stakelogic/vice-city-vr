# Third-party notices

Vice City VR builds on third-party projects. Each component remains subject to
its own license and terms.

- reVC reverse-engineered game code: upstream project terms and notices. This
  kit contains only textual patch hunks against the pinned upstream commit.
- librw RenderWare-compatible renderer: MIT License, retained at
  `librw/LICENSE`.
- `GetGitRevisionDescription` CMake modules: Boost Software License 1.0,
  retained at `overlay/cmake/LICENSE_1_0.txt`.
- `FindSndFile.cmake`: BSD 3-Clause License, retained at
  `overlay/cmake/COPYING-CMAKE-SCRIPTS`.
- Dear ImGui and ImGuizmo: MIT licenses retained under
  `librw/skeleton/imgui/`.
- Embedded ProggyClean font by Tristan Grimmer: MIT License, retained at
  `librw/skeleton/imgui/LICENSE_proggyclean.txt`.
- LodePNG, the stb headers, and `ini_parser.hpp`: their permissive terms are
  retained in the corresponding source headers.
- GLAD-generated loader code: generated-code provenance retained at
  `librw/src/gl/glad/NOTICE_glad_generated.txt`.
- VRMADA UltimateXR hand source assets: MIT License, retained at
  `third_party/ULTIMATEXR_LICENSE.txt`.
- Khronos OpenXR loader: Apache License 2.0.
- OpenAL Soft: LGPL-2.0-or-later with separately licensed portions.
- mpg123: LGPL-2.1-only with separately licensed portions.
- NVIDIA Streamline and DLSS/DLAA: NVIDIA component licenses.
- AMD FidelityFX Super Resolution 2: MIT License.
- Microsoft D3D12 helper header used by FSR2: MIT License.
- DLSS-NR caller-gate forwarder: derivative of Dagherbou/OptiScaler_DLSSNR
  commit `393e0706b950a0ff1498e9dcf66989a80de72f31`, GPL-3.0-or-later.
  Corresponding source and license are retained under `overlay/tools/dlss/`.

The external SDK/runtime components are build or player-release prerequisites;
they are not redistributed by this source kit. Historical binary provenance
for earlier player packages is recorded in `docs/DEPENDENCY_SOURCES.md`.

The original Grand Theft Auto: Vice City game and its data are not included.
Grand Theft Auto and related names and assets are the property of their
respective owners. This project is not affiliated with or endorsed by Rockstar
Games or Take-Two Interactive.
