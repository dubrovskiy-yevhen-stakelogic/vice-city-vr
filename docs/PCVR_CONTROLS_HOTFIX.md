# PCVR 0.5.5 controls hotfix

This update fixes driving, wrist-HUD and menu-input problems in the PC port.
It does not change saved settings, vehicle calibration, graphics options or
NVIDIA files.

- **Two-handed steering:** retain the measured controller angle beyond the
  wheel's physical stop, as on Quest. Returning from beyond the stop no longer
  shifts the original grip reference and leaves the hands out of sync.
- **One-handed steering:** measure the full angle around the calibrated wheel
  centre instead of a single tangent coordinate. Passing 90 degrees no longer
  reverses the measured direction. Tracking recovery and grip changes re-seat
  the reference at the current wheel position.
- **On-foot immersive HUD:** capture the wrist anchors while the VR frame is
  prepared, then use that frame-local snapshot when submitting the HUD. Restoring
  the gameplay camera no longer makes the hand-pose lookup fail.

The hotfix keeps the PC wheel-plane calibration and tracking safeguards;
motorcycle controls are unchanged. Classic HUD still avoids the wrist atlas.
Gaze-hidden wrist panels stay routed away from the classic HUD.

## Follow-up camera and menu fixes

- **Default driving camera:** decode signed INI values before clamping the
  seat offsets. Missing settings now use 15 cm height and zero distance,
  instead of incorrectly selecting the upper limits of 150 cm and 100 cm.
  The same correction preserves signed wheel-hand pullback values.
- **Wrist map proportions:** use the HUD render target's aspect ratio and
  square radar scaling for the wrist atlas. The map and its markers retain
  their proportions without changing the player's classic HUD preferences.
- **VR menu input:** consume controller input while the VR interface owns
  it, and require release of consumed controls before handing input back
  to gameplay. Releasing a grip while Menu remains held must not open the
  ordinary game menu; grip-threshold jitter must not retrigger the shortcut.

Existing seat, wrist-placement and button-binding preferences are retained.
No automatic calibration reset or migration is performed.

## Wheel-grip acquisition parity

Wheel acquisition now matches Quest: a new squeeze must start near the neutral
9/3-o'clock socket. A controller whose grip is already held cannot acquire the
wheel just because a rotating socket passes nearby. This prevents an unintended
switch from one-handed to two-handed steering and the resulting change in response.
An acquired grip still follows the turning wheel until it is released or becomes
unavailable. After tracking loss or a blocked hand, release and squeeze again.

The angle solver, steering sensitivity, stop behavior and rendered wheel remain
unchanged in this follow-up. It does not add tracking calls or graphics work.

## Headset checks

Use the same game installation and settings as 0.5.5. Select the immersive
driving mode and `HUD PRESET = IMMERSIVE` for the relevant checks.

1. With both grips held, turn past the wheel stop and slowly return through it.
   Repeat in both directions, including a controller angle beyond 180 degrees.
2. Repeat with one hand. Release and re-grab; switch between one and two hands;
   briefly lose tracking, then release and re-grab. The wheel must not jump on
   reacquisition. Keep the other hand below the wheel with its grip already
   squeezed: turning past it must not capture that hand or change the response.
3. On foot, move and rotate each wrist independently of the headset. Check the
   map, status, clock and ammo panels, gaze reveal, and their placement controls.
4. Enter and leave a car or bike, change HUD presets, and pass through a loading
   screen or cutscene. No panel should retain an old hand or vehicle anchor.
5. Select default driving with first-person vehicle view, enter a car and
   verify that the camera stays at the configured seat position. Check a
   negative seat adjustment and restart to verify that it is loaded correctly.
6. Check the wrist map and its markers with different classic HUD scaling
   settings. The wrist map should remain square; other panel shapes should
   retain their intended proportions.
7. Scroll the VR menus, enter and leave submenus, then close the interface.
   Test different release orders for Menu, grips and the fallback shortcut.
   The game menu should open only on a deliberate new pause-button press.

The corrected vehicle camera, wrist-map proportions and wheel acquisition were
checked in a local headset. The checklist above remains useful for other
controllers, vehicle models and saved settings; host regression tests alone
do not validate those combinations.
