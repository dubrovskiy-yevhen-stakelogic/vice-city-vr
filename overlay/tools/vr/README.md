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
