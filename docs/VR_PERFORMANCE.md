# OpenXR VR performance captures

The built-in profiler records per-frame timing and renderer statistics without
requiring the headset to be removed.

## Recording

1. Hold both grip buttons and press Y to start a capture.
2. `REC:<frames>` appears in the headset debug panel while recording.
3. Repeat the test route or action.
4. Hold both grips and press Y again to stop and save.

The live recording is written beside the executable as:

```text
vr_perf_openxr_live.csv
```

Stopping the capture creates timestamped files:

```text
vr_perf_openxr_YYYYMMDD_HHMMSS.csv
vr_perf_openxr_YYYYMMDD_HHMMSS.txt
```

## Repeatable driving test

For comparisons, keep the save, weather, vehicle, route, headset refresh rate,
OpenXR resolution and graphics toggles unchanged.

1. Load a save at the Ocean View Hotel.
2. Wait ten seconds for initial streaming to settle.
3. Enter the same vehicle and start at the same position.
4. Start the profiler.
5. Drive the same Ocean Drive and Malibu route without changing camera mode.
6. Stop the profiler immediately after returning to the starting point.

Use captures of at least 90 seconds and compare multiple runs. Background
compilation, shader processing, downloads and other CPU/GPU-heavy applications
can invalidate a comparison.

## Main timing fields

- `frame_ms` - total application frame time.
- `game_ms` - game simulation time.
- `stream_ms` - `CStreaming::Update` CPU time.
- `audio_ms` - complete game audio service time.
- `scene_setup_ms` - shared stereo scene setup.
- `world_list_ms` - visibility and render-list construction.
- `pre_render_ms` - entity pre-render and animation preparation.
- `left_eye_ms`, `right_eye_ms` - CPU time spent in eye-specific work.
- `submit_ms` - VR resolve and OpenXR submission work.
- `ui_ms` - HUD and 2D rendering work.
- `desktop_present_ms` - desktop presentation cost.

## OpenXR and synchronization fields

- `xr_wait_frame_ms`, `xr_begin_frame_ms` - OpenXR frame pacing.
- `xr_acquire_ms`, `xr_swapchain_wait_ms`, `xr_release_ms` - image ownership.
- `xr_locate_views_ms` - headset view location.
- `xr_end_frame_ms` - compositor submission.
- `d3d12_external_submit_ms` - D3D12 submission for OpenXR-owned resources.
- `d3d12_frame_fence_wait_ms` - normal frame-fence wait.
- `d3d12_full_gpu_wait_ms` - explicit full GPU waits; normally zero.

## Streaming and resource fields

- `slow_stream_item_ms`, `slow_stream_item_id`, `slow_stream_item_type` - the
  slowest individual streaming item in the frame.
- `tex_*_ms` - texture resource, descriptor, footprint, CPU copy and queue
  costs for streamed textures.
- `tex_upload_mb`, `tex_upload_count` - uploaded texture volume and count.
- `geometry_instance_ms` - first-use RenderWare geometry conversion.
- `geometry_buffer_upload_ms`, `geometry_buffer_mb` - geometry upload cost.
- `requested_models`, `stream_memory_mb` - streaming pressure.

## World and stereo fields

- `visible_buildings`, `visible_objects`, `visible_peds`, `visible_vehicles` -
  entities in the shared VR visibility list.
- `entity_render_calls` - high-level RenderWare entity submissions.
- `world_draw_calls`, `world_indices` - low-level D3D12 world work.
- `stereo_single_pass_begins` - single-pass world stages entered that frame.
  FULL mode normally reports five.
- `stereo_single_pass_draw_calls`, `stereo_single_pass_indices` - geometry
  submitted as instanced stereo.
- `stereo_single_pass_fallbacks` - stages that returned to the compatibility
  path.
- `stereo_bundle_*` - retained diagnostic counters for the older packet/bundle
  comparison path.

## Interpreting captures

At 90 Hz the application has approximately 11.11 ms per frame. Look at the
95th and 99th percentile frame times before focusing on a single worst frame.
Correlate a spike with its phase instead of assuming it is a GPU problem.

Examples:

- High `audio_ms` points to streamed audio or device work.
- High `stream_ms` with texture counters identifies asset upload pressure.
- High `scene_setup_ms` or world draw counts points to scene complexity.
- High `xr_wait_frame_ms` with otherwise low work is usually normal runtime
  pacing rather than a rendering stall.
- A non-zero `d3d12_full_gpu_wait_ms` deserves investigation because the normal
  OpenXR path is designed to avoid full-queue synchronization.

CPU submission timings are not GPU execution times. In particular, a short
`submit_ms` does not mean DLAA, reflections or NR were cheap on the GPU. Check
the OpenXR runtime's application GPU timing and reprojection/dropped-frame
statistics separately. At 72 Hz the budget is about 13.89 ms; a displayed
72 FPS can still include compositor-generated frames.

## Release regression gate

Test the previous accepted player build and the candidate with the same
driver, headset, render resolution, refresh rate, save, assets and settings.
Use Release builds, close other GPU-heavy applications, remove `sl_verbose`,
and separate shader/model warmup from steady-state captures. Record cold-start
cost separately; do not erase startup failures by discarding warmup samples.

| Case | What must be compared |
| --- | --- |
| Fresh settings, optional effects off | CPU/GPU frame time, allocations and VRAM versus the accepted baseline |
| Ordinary DLAA, NR off | Static scene and moving peds/vehicles; no NR evaluation or model-resource creation |
| Effects master off after enabling all effects | Rain/SSR/light work stops; scene appearance returns to baseline |
| Reflections only | Water, vehicles and puddles tested separately, including each OFF state |
| Dynamic lights only | Day/night and traffic-heavy scenes; OFF, low and maximum counts |
| Rain only | Clear weather, heavy rain and drying; particle density and surface settings separately |
| NR 1X | Full, Quality, Balanced and Performance; confirm both eyes reconstruct NR output |
| NR 2X / 3X | Opt-in stress tests; log memory, frame pacing and mode-switch recovery, not a default target |
| Asset streaming | Classic assets and each available optional pack; driving and rapid camera turns |
| VR transitions | Menus, cutscenes, recentering, vehicle views and return to gameplay retain stereo and pacing |

Run at least three comparable 90-second captures per baseline case. Report
median, p95 and p99 frame times, missed frames, peak VRAM and scene draw counts.
Investigate a repeatable baseline regression above the normal run-to-run
variation; do not average it away with the faster results of an optional mode.
Keep one lower-end supported PC in the release matrix: a flagship GPU cannot
establish performance safety for all players.

Mode switches may recreate resources, but steady-state rendering should not
perform full GPU drains, unbounded file logging, shader compilation, or model
creation every frame. Verify this in the relevant counters/logs and source
paths. Confirm that turning an effect off prevents its expensive render work,
not merely hiding the final image.

Store test evidence outside the public source kit. Mark missing hardware,
headset, or timing checks as pending. Passing the source audit, compilation,
or headless NR probes is not equivalent to completing this runtime matrix.
