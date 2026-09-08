# PCVR control map

For Quest / Touch controllers, using the default VR bindings and the game's
standard control mode. Custom bindings and other classic control modes can
change the actions below. **Grip** is the side button; **trigger** is the
index-finger button. **L3 / R3** mean clicking the left / right stick.

## Start here

| Input | Action |
| --- | --- |
| Both grips + left Menu button | Open / close VR Settings |
| Both grips + both triggers + X | Alternative VR Settings shortcut if the runtime captures Menu |
| Both grips + both triggers + L3 | Alternative without a face button |
| Menu without a shortcut | Original game pause menu, when available through the runtime |
| Both grips + L3 + R3 | Recenter the gameplay view; put held weapons away first |

Release the opening shortcut before navigating. In VR Settings, use the left
stick up/down to scroll and left/right to adjust; left/right triggers also
decrease/increase values. **A or R3** selects, **B or L3** goes back.

## On foot

| Input | Action |
| --- | --- |
| Left stick | Move |
| Right stick left/right | Turn, using your selected smooth/snap turn setting |
| X | Jump |
| Y | Enter a vehicle / interact with the enter-exit action |
| A or R3 | Sprint |
| L3 | Crouch when `DEFAULT L3 CROUCH` is enabled |
| Grip near a holster | Draw / return a physical weapon |
| Free-hand grip near a supported weapon | Two-hand support |
| Trigger on the hand holding a weapon | Fire / use that weapon |

Physical weapon, reload and holster interactions take priority over ordinary
grip bindings. With tracked hands enabled, use physical weapons rather than
expecting the legacy attack face button to fire. Holster positions and manual
reload options are configurable in VR Settings.

## Driving

| Input | Action |
| --- | --- |
| Right trigger | Accelerate |
| Left trigger | Brake / reverse |
| Y | Exit |
| X | Change radio station |
| L3 | Horn in the standard control mode |
| R3 | Look behind, if enabled in Controls |
| Default driving: left stick | Steer |
| Immersive driving: grip near the wheel / handlebars | Grab and steer physically |
| Motion driving | Steer by controller motion using the configured vehicle settings |
| Default driving: B + one grip | Drive-by on that side; B alone fires forward on a bike |
| Immersive / Motion driving: B with a held weapon | Fire the held weapon; triggers remain throttle/brake |

For wheel acquisition, release and squeeze the grip near the neutral
9/3-o'clock positions. Steering and seating calibration are in the vehicle
settings. RC vehicles keep their mission-specific controls; A supplies the
RC vehicle-fire action (including dropping the RC helicopter bomb).

## Remap buttons

Open **VR Settings > Controls** directly from the main menu. This is the PCVR equivalent of
the standalone Controls page:

- **DEFAULT**, **SWAPPED HANDS** and **CUSTOM** layouts.
- Individual A/B/X/Y, grip and stick-click bindings for on-foot game actions.
- Separate L3 crouch and vehicle R3 look-behind switches.
- **RESET BINDINGS TO DEFAULTS** to recover the original layout.

The remapper does not change physical triggers, service shortcuts, vehicle/RC
bindings or tracked interactions. Fire stays on the physical trigger. Choosing
a legacy attack action does not add a second fire input. PCVR's default R3
on-foot action is sprint.

## Optional shortcuts

Put held weapons away before using these two-grip shortcuts.

| Input | Action |
| --- | --- |
| Both grips + A | Toggle debug overlay |
| Both grips + Y | Start / stop performance capture |
| Both grips + B | Compare DLSS 5 against baseline when Neural Rendering is enabled; otherwise open cheats |

Cheats are also available through VR Settings, so enabling neural comparison
does not remove access to them. Graphics and HUD preferences can be changed
through their menu pages without memorizing diagnostic shortcuts.
