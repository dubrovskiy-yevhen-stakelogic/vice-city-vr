# VR runtime notes

The primary project overview, requirements and build instructions are
documented in [README.md](../README.md).

## Supported path

The actively developed VR configuration is:

- Windows x64
- librw Direct3D 12 backend
- OpenXR PC VR output
- OpenAL audio

Meta Quest through Quest Link or Air Link is the main tested runtime. OpenXR is
used through the Khronos loader, so the integration is not tied to LibOVR or to
a single headset vendor.

## Presentation modes

- Gameplay uses native headset-tracked stereoscopic projection.
- Major world stages use FULL single-pass stereo by default.
- Frontend menus use an OpenXR cinema quad.
- Cutscenes default to the cinema screen; Locomotion settings also offer
  stereoscopic cutscenes with camera selection and saved camera preferences.
- The gameplay HUD uses a separate OpenXR quad and can be hidden.

For a black mission cutscene in the headset with a visible monitor image, use
the external [cutscene mode switch](CUTSCENE_MODE.md) to try stereo without
navigating the headset menu.

## Experimental DLSS 5 passes


The Graphics menu exposes `DLSS 5 PASSES` with 1X, 2X and 3X choices. 1X is
the default. In 2X/3X modes each additional neural-rendering pass consumes the
previous pass output and owns an independent temporal context; the
implementation uses separate ping-pong textures so a pass never reads and
writes the same D3D12 resource.

2X and 3X are deliberately opt-in experiments intended mainly for very fast
desktop GPUs and offline comparisons. Their model cost is approximately
multiplied by the pass count and they are not recommended for VR frame rates.
The desktop F11 panel can change `PASSES`, while F12 restores 1X; the choice is
saved as `DLSSNeuralPasses=1..3` in `vr_settings.ini`.

The old `SCENE SCALE` / `DLSS 5 WORK SCALE` control is removed. NR now keeps the
scene at the selected headset render scale, with full-resolution DLAA for both
the enhanced image and the A/B baseline. `DLSS MODE` is locked to DLAA while NR
is enabled; disabling NR restores the saved ordinary DLAA/DLSS SR choice.
Both grips + B compares one selected model profile against the normal baseline,
with NR bypassed in the baseline view.

`NR MODEL SCALE` changes only the resolution of the copy sent to NR: FULL
100% (default), QUALITY 75%, BALANCED 67%, or PERFORMANCE 50% per dimension.
It does not change scene rendering or headset resolution, and
does not add another DLSS reconstruction pass. With foveation enabled the
percentage applies to its central crop; otherwise it applies to the full eye.
The reduced modes compare the model output with the matching downsampled
original, then add that neural difference back to the original scene color.
The full-resolution scene remains the base instead of being replaced by a
blurred enlargement. FULL bypasses this additional scaling/composition path.

This separate VR-only experiment requires the direct NR backend. It is saved
as `DLSS5ModelScaleMode=0..3`, reports failures explicitly, and can be combined
with SHARED and foveation. Start at 1X; compare FULL against QUALITY with the
same headset render scale, profile and head motion. Preserving the original base does
not guarantee identical neural detail, temporal stability or an FPS increase.

For older NR-enabled settings without `DLSS5ModelScaleMode`, the saved
`DlssMode` level supplies the initial model-scale choice. Explicit model-scale
settings, including FULL, take priority. The old `DLSSNeuralRegion` key is ignored.

When DLSS 5 is enabled, `DLSS 5 STEREO` defaults to `SHARED (EXPERIMENTAL)`.
The shared mode runs the selected NR pass chain on the left eye, then uses both eyes'
depth and camera transforms to transfer its color changes to the right eye.
Both eyes retain their own scene render and DLAA/DLSS reconstruction; this is
not a duplicated mono image or frame generation. Newly exposed or unreliable
pixels keep the original scene color. Check foliage, vehicles and depth edges
while moving your head: this mode can introduce differences between eyes and
has no guaranteed FPS gain. It does not affect flat mode.

The choice is saved as `DLSS5StereoMode=0..1` in `vr_settings.ini`. If sharing
fails, the menu and temporary status strip show an error and NR is bypassed
for both eyes. Ordinary reconstruction keeps its existing error fallback.
Switch back to `PER EYE` to restore independent neural rendering.

`DLSS 5 FOVEATION` defaults to QUALITY when DLSS 5 is enabled. It restricts NR
to a fixed central part of the work-resolution image: approximately 85% of
the width and height in QUALITY, 75% in BALANCED, or 65% in PERFORMANCE.
Crop dimensions are aligned to 16 pixels. The neural result is feathered
into the original peripheral scene color before ordinary DLAA/DLSS reconstructs
the full image. This is not VRS, eye tracking, another scene-scale setting, or
a ReShade injector; it can be combined with SHARED and 1X/2X/3X NR passes.
This experiment requires the direct NR backend; other backend paths report
an explicit error. The central-region idea has a related implementation in
[Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS),
but this renderer does not bundle or require that injector.

DLSS 5 itself remains OFF until selected. Missing stereo/foveation settings
use SHARED/QUALITY; existing saved choices, including PER EYE and OFF, are
preserved. The saved key is `DLSS5FoveationMode=0..3`. The status strip identifies
`FOV` and the selected mode, and reports foveation failures rather than `ACTIVE`.
Compare OFF against the other modes while moving your head and looking away
from the center: feathering can still leave visible changes in detail, tone
or motion near the processing boundary. The smaller NR area does not imply a
fixed FPS gain. Flat mode ignores this setting.

For the external runtime filenames, installation boundaries, and actual
activation checks, see [DLSS5_SETUP.md](DLSS5_SETUP.md).

## Required runtime files

Place the following files beside `reVC.exe` in a complete Vice City game
directory:

- `openxr_loader.dll`
- `OpenAL32.dll`
- `libmpg123-0.dll`
- `sl.interposer.dll` (a linked startup dependency even with temporal AA off)

The original game assets are required and are not provided by this repository.

## Useful controls

The player-facing [control map](CONTROL_MAP.md) covers movement, weapons,
driving, menu navigation and the main-menu Controls remapper.

| Chord | Action |
| --- | --- |
| Both grips + X | Toggle gameplay HUD |
| Both grips + Y | Start or stop performance capture |
| Both grips + A | Toggle debug overlay |
| Both grips + B | DLSS 5 baseline comparison when enabled; otherwise cheats |
| Both grips + left stick click + left trigger | Toggle stereo tail mode |
| Both grips + both thumbstick clicks | Recenter gameplay view |

VR Settings / Controls provides DEFAULT, SWAPPED HANDS and CUSTOM mappings for
on-foot A/B/X/Y, grips and stick clicks. Driving/RC controls, service chords
and physical triggers are unchanged. DEFAULT preserves the PC right-click
sprint action. Fire is reserved for the physical trigger; a legacy fire-button
mapping does not synthesize a second shot. Invalid saved mapping values fall
back to their source button's default.

The in-game Graphics / Effects submenu keeps the new weather path opt-in. Rain
renderer choices are CLASSIC, MODERN and CINEMATIC; the separate RAIN SURFACES
control selects OFF, WET, PUDDLES or CINEMATIC. Wetness accumulates while it rains
and dries gradually. Puddle reflections use a per-eye history and are the most
expensive setting. Keep optional effects off for a baseline performance test;
verify measured frame cost rather than assuming that an OFF label alone
guarantees no background work.

See [VR_PERFORMANCE.md](VR_PERFORMANCE.md) for capture instructions and metric
definitions.

## HUD layouts

HUD Settings retains the classic head-locked layout by default. The optional
IMMERSIVE preset enables four independent panels: minimap, status, clock and
ammo. Each panel can be enabled separately, assigned to either hand and worn
on the outer or inner side. `PANEL PLACEMENT` opens per-context calibration for
on-foot, car and bike placement: along/across/lift, pitch/yaw/roll and size.
The placement menu also provides ammo RGB, copy-other-side and reset controls.

Gaze-only display, look range, driving panels and the classic weapon/clock
elements are separate settings. Immersive-driving panels use the neutral
dashboard/handlebar anchor, not the rotating steering wheel. The PC path uses
OpenXR compositor quads; unlike standalone world geometry, they are not
occluded by the rendered scene. Gaze visibility currently switches at a fixed
view cone and range rather than fading smoothly. The ammo panel shows the
player's active weapon on its selected wrist; separate ammo panels for each
held weapon are not implemented. Headset placement and comfort must be checked
in-game. The optional atlas is created lazily and is not rendered or copied
when all panels are off or outside the gaze visibility range.
After first use, that bounded swapchain allocation is retained until the VR
session ends; switching back to CLASSIC stops its rendering, copying and layer
submission without forcing a GPU-idle wait to free it immediately.

## Rain starting values

The optional rain controls use the maintained test preset as their starting
values: intensity 200%, density 300%, puddle coverage 25%, edge softness 25%
and ripple strength 100%. These values are not a recommendation to enable all
effects on every PC. Modern rain and rain surfaces remain off until explicitly
enabled; the settings are stored independently of their enable switches.

Dynamic lighting, reflections and rain have separate pages under Effects, so
each cost can be tested and disabled independently. Keep the effects master
off for the conservative baseline and enable one feature at a time. A saved
user configuration is not silently replaced with the release preset.

Reflections has separate CAR and OCEAN pages. Car SSR mix defaults to 100%
and its maximum distance defaults to 0 (unlimited). Ocean wave speed and
reflection distortion default to 100%; sky sheen, sun glint and local-light
sparks default to 0 and are opt-in. Puddles retain their own ripple control
under Rain rather than inheriting ocean distortion. The OCEAN EFFECTS switch
controls the ocean additions independently and defaults to ON beneath the
overall effects master, which defaults to OFF. Setting ocean reflection to
0 disables only that reflection; sheen, glint and sparks have their own
independent controls.

## Texture and legacy effects controls

Graphics / Generate Mipmaps defaults to ON. Changing it affects texture loading
on the next start, so a pending change is marked RESTART. Foliage Softness is a
live adjustment in half-mip-level steps from 0 to 3; its default is 0.

The Lighting page also contains the original shadow setting and distance fog.
These original rendering controls remain independent of the optional DX12
effects master. Distance fog defaults to ON. Effects / Fountains offers OFF,
LOW, OPTIMIZED and ORIGINAL, with OPTIMIZED as the default. They are particle
quality choices, not an automatic enable switch for the modern rain path.
