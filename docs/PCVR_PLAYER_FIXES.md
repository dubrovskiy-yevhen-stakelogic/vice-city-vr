# PCVR 0.5.5 player fixes - test build

This candidate adds three changes to the existing 0.5.5 PCVR package:

- **Black mission cutscenes:** `CUTSCENE_MODE.bat` provides an external choice
  between STEREO and CINEMA SCREEN, with an automatic settings backup. Stereo
  bypassed the reported black cinema screen for one VirtualDesktopXR player.
  The cinema-screen rendering fault itself is still under investigation.
- **Ped reactions to aimed weapons:** the Quest aimed-gun threat check is now
  connected to PCVR. A nearby ped facing a held firearm with line of sight can
  enter the game's existing threat response before a shot is fired. Its
  personality and native AI decide whether to flee, report or fight. Cameras,
  melee weapons and holstered guns do not trigger this check. As on Quest, it
  uses the first eligible held gun when both hands carry firearms.
- **Right waist holster:** the default category is HANDGUN again. Existing
  custom holster assignments are preserved. If you previously saved SCOPED on
  WAIST RIGHT, change that point to HANDGUN in the Holsters menu. This does not
  grant a weapon; the holster uses the handgun in your inventory.

Close the game before installing this candidate. Keep a copy of your working
`reVC.exe` so you can return to it. Extract the contents of the archive's
`Vice-City-VR-v0.5.5-PCVR` folder into your existing PCVR game directory. No
`vr_settings.ini`, saves or game assets are supplied. Existing runtime DLLs
are retained in the same form as the previous player package.

The Release build, 27 host checks of the new aim functions, Quest targeting
function comparison, holster defaults and 9 switch checks passed. These checks
do not replace testing the new executable in a headset. Test a civilian and an
armed gang member, an occluded ped, both hands separately, returning the gun to
its holster, a new game with default settings, and your existing saved loadout.
