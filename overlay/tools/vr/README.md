# VR and save-boundary smoke tests

Build `menu-navigation-test.vcxproj` with Visual Studio 2022, Release/x64,
then run `build-local/menu-tests/menu-navigation-test.exe` from the source root.

The executable exercises the same navigation helper as settings, cheats and
mission lists: engage/release hysteresis, held repeat, immediate reversal,
reopening, and one-pulse behavior after a stalled frame. It also checks save
block size/alignment boundaries, including unsigned wraparound inputs.

These are host-side unit checks. They do not launch the game, measure FPS,
validate headset input mappings, or validate all nested save-file payloads.

`wrist-hud-test.vcxproj` builds a separate host test into
`build-local/wrist-hud-tests/wrist-hud-test.exe`. It exercises the production
wrist rotation/quaternion helpers over 125 orientation combinations, rejects
degenerate/non-finite bases, and checks classic defaults and real INI
roundtrips for presets, bounded calibration, copy/reset and ammo colours.
It creates and removes only a unique temporary INI, never the player's
settings. These checks do not validate compositor placement in a headset.

Two additional tests include the actual controller-mapping and camera-head
scope headers. Build and run them from the assembled source root in a Visual
Studio 2022 developer PowerShell:

```powershell
New-Item -ItemType Directory -Path build-local\vr-tests -Force | Out-Null
cl /nologo /std:c++17 /EHsc /W4 /WX /MT tools\vr\tests\camera-head-scope-test.cpp /Febuild-local\vr-tests\camera-head-scope-test.exe /Fobuild-local\vr-tests\camera-head-scope-test.obj
cl /nologo /std:c++17 /EHsc /W4 /WX /MT tools\vr\tests\pad-bindings-test.cpp /Febuild-local\vr-tests\pad-bindings-test.exe /Fobuild-local\vr-tests\pad-bindings-test.obj
& .\build-local\vr-tests\camera-head-scope-test.exe
& .\build-local\vr-tests\pad-bindings-test.exe
```

The first tests head-subtree bounds and restoration. The second tests mapping
defaults, swaps, captured buttons and invalid saved targets. Both are host-only
checks and do not change game settings or start OpenXR.

## Steering and wrist-HUD lifecycle regressions

`car-steering-test.vcxproj` tests the production steering state machine:
two-handed stop retention, full turns, one-handed polar tracking, grip changes,
invalid samples and recovery. Its checks remain active in Release builds.
It also tests Quest-compatible wheel acquisition: neutral socket distance,
fresh squeeze edges, release hysteresis and tracking loss. A numerical regression
keeps a second, already-squeezed hand below the wheel while a rotating socket
passes it, checking that one-handed control does not silently become two-handed.

The lifecycle runner compiles the actual wrist-HUD implementation with host
adapters and extracts the frame submission ordering from `OpenXRVR.cpp`. It
checks capture before camera restoration, wrist routing after restoration,
gaze-hidden panels, vehicle contexts and the classic/OFF path.

From the assembled source root in a Visual Studio 2022 developer shell:

```powershell
msbuild tools\vr\car-steering-test.vcxproj /p:Configuration=Release /p:Platform=x64
& .\build-local\car-steering-tests\car-steering-test.exe
python tools\vr\tests\run-wrist-hud-lifecycle.py --compiler cl --out-dir build-local\wrist-hud-lifecycle-tests
```

Choose a fresh lifecycle output directory. When testing directly from the
patch-only kit's `overlay` directory, put all outputs outside the kit: override
MSBuild `OutDir` and `IntDir`, and pass an external `--out-dir` to the runner.
These tests do not start the game or validate controller tracking in a headset.

## Vehicle camera, map proportions and modal input

Build `vehicle-view-settings-test.vcxproj` and `menu-input-routing-test.vcxproj`
as Release/x64, using the same external-output precautions described above.
With their default output locations in an assembled source tree, run:

```powershell
& .\build-local\vehicle-view-settings-tests\vehicle-view-settings-test.exe .\build-local\vehicle-view-settings-tests
& .\build-local\menu-input-tests\menu-input-routing-test.exe .\src\vr\OpenXRVR.cpp
```

The vehicle test calls the production signed settings loaders with the real
Win32 INI API. Its required directory argument is where it creates and removes
one temporary INI; it never opens the player's configuration. It checks absent
keys, negative offsets, bounds, explicit zeroes and legacy seat-height fallback.

The menu test covers inherited joystick input, shortcut threshold jitter,
close/release ordering, deliberate pause presses, missing controller samples,
small stick drift and the production input-routing call sites. Its checks remain
active in Release builds. It also checks that held menu scrolling still repeats.

The wrist-HUD lifecycle runner additionally covers different cached aspect
ratios, legacy radar/scaling settings and panel sizes. It checks square map and
marker scaling, proportional text panels, and restoration of render dimensions,
aspect ratio and classic HUD settings on success and failure.
