# Renderer regression probe

This synthetic test uses the actual D3D12 librw backend. It creates a hidden
window and small render targets; it does not start the game, OpenXR, or
Streamline and does not require game assets or NVIDIA model files.

From a Visual Studio 2022 developer PowerShell, after generating the game
projects, build the same renderer library used by the game and the probe:

```powershell
MSBuild.exe build\librw.vcxproj /m /p:Configuration="Release win-amd64-librw_d3d12-oal" /p:Platform=x64
MSBuild.exe tools\performance\renderer-probe.vcxproj /m /p:Configuration=Release /p:Platform=x64
& .\tools\performance\bin\Release\renderer-probe.exe
```

Checks:

- Alpha-coverage preservation produces exactly the original bytes across
  720 inputs, including small/non-square textures, padded rows, opaque,
  empty, cutout, and random alpha. This covers both the small-mip and
  histogram paths.
- Rigid and skinned world geometry renders with mono and single-pass stereo
  shader variants. Enabling/disabling object-motion output does not change
  the RGB result in each synthetic scene.
- Ordinary camera/SSR/HUD targets do not allocate a motion attachment.
  Motion is enabled explicitly for the temporal consumer; short disables
  reuse the allocation, while full release permits clean reallocation.
- Child rasters share their parent's motion attachment.
- All 384 supported filter/address/bias sampler combinations have distinct,
  reusable descriptors and fit the bounded heap; out-of-range bias clamps.
- Startup mip generation OFF leaves newly created ordinary textures with
  one level, preserves authored chains, and does not mutate resident
  textures.
- Distance fog OFF changes a fogged world sample and ON restores it.
  Foliage bias changes a blended alpha-tested sample in half-level steps;
  zero restores the default RGB. Vehicle alpha and unmasked draws exclude
  the bias.
- Water sheen, sun glint and dynamic-light sparks change only tagged water;
  OFF restores the original RGB. Independent reflection distortion and wave
  speed work against a captured eye-local history; explicit time advances
  waves and speed zero freezes them. Car SSR mix/distance controls remove
  and restore its contribution, with a no-source OFF capture fast path.
- D3D12 debug-layer error/corruption messages cause failure when the debug
  layer is installed. Its availability is printed; warnings are reported.

Success ends with `MIP_COMPLETE cases=720 failures=0` and
`RENDERER_COMPLETE failures=0`. A failed assertion exits nonzero.

This is a correctness regression, not a performance benchmark. Its
synchronous readbacks intentionally stall the GPU. It does not establish
headset quality, dynamic motion-vector accuracy, frame time, long-session
memory stability, or behavior under device removal. Run GPU probes one at
a time, without a game or another GPU-intensive task running.

The complementary `tools/dlss/probe/nr-chain-probe.vcxproj` checks the real
game reconstruction code using a separate headless host. That host stubs
renderer integration, so passing it is not a substitute for this renderer
test or an in-headset regression run. See its own README for prerequisites.
