# Black cutscenes in the headset

For Vice City VR PCVR 0.5.5 or newer. If a mission cutscene is visible on the
monitor but black in the headset, try stereoscopic cutscenes. One player using
VirtualDesktopXR confirmed that this bypassed the problem in 0.5.5; the cause
of the cinema-screen failure is still under investigation.

1. Close the game.
2. Extract the downloaded ZIP before running anything.
3. Double-click `CUTSCENE_MODE.bat`. If it is beside `reVC.exe`, it finds the
   game automatically. Otherwise, select your game's `reVC.exe` in the dialog.
4. Choose **1 - STEREO**, then start the game normally.

To restore the flat cinema screen, close the game, run the same file and
choose **2 - CINEMA SCREEN**. Stereo cutscenes put the scene around you instead
of showing it on a virtual screen; use the mode you find more comfortable.

The switch creates `vr_settings.ini` if needed and changes only
`[VR] CutsceneMode`. Before changing an existing file, it saves an exact copy
beside it as `vr_settings.ini.cutscene-<timestamp>-<id>.bak`. Other graphics,
controls and save files are left alone. An already selected mode needs no
write or new backup. No downloads, game launch or administrator request are
performed. Windows PowerShell 5.1 is used; no separate script installation is
needed. If Windows blocks the script, report its message rather than disabling
system-wide security settings.

The same setting is available in the game's Locomotion menu when that menu is
visible. This helper makes it accessible from outside the headset.

If stereo does not help, send the new `openxr_d3d12.log`, the exact game build,
and whether the monitor still shows the affected mission scene. Loading screens
and frontend menus continue to use cinema presentation.
