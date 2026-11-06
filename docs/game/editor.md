# Using the in-game track editor

*A copy of the editor page of MX Simulator Tools' `mx-track-making` skill (2026-11-05). Square brackets name the
source: [verified] means checked in mx.exe or the game, others name the guide it came from.* The math behind gradients
and brushes (profile, falloff, edinfo) is in [track-editor-gradients.md](track-editor-gradients.md). This file is how
authors use the editor.
Main source: mxsimulator.com/editing.html (jlv, 2007, "To be continued...").

## Starting it
- `mx.exe --editor`, optionally `--track <folder>`; then press TAB until the editor screen shows [editing.html].
- Erosion is a separate command-line option: `--erode <x>` (multiplies each roost's `dig`).
- New tracks: start from a blank track folder in `%LOCALAPPDATA%\MX Simulator\` (BetterBlank is Jackson's test track).

## Screen
Left column = mode list (click to pick, or number keys) [verified mode numbers]:
0 Raise, 1 Lower, 2 Smooth, 3 Smudge, 4 Snap, 5 Gradient Linear Paint, 6 Gradient Linear Add, 7 Gradient Radial
Paint, 8 Gradient Radial Add, 9 Gradient Placement, 10 Gradient Edit, 11 Timing Gate, 12 Billboards, 13 Statues,
14 Tilemap Edit, 15 Decals. Then SubMode, Save, and the Zoom / Strength / Focus / Radius values (left-click a value
to lower it, right-click to raise it). The HUD also shows the mouse's world `x y z` (y = terrain height), "OK" or
"Update needed!", and `Actual Radius: R (inner)`.

**Focus = the solid part of the brush (observed):** the HUD reads `Focus 0.50 ... Actual Radius: 25.26 (12.63)`, so
inner radius = Focus x radius. Inside the inner radius the brush is at full weight, then smoothstep to zero at the
outer radius (docs/game/track-editor-gradients.md "Brush"). Focus 0 = soft all the way; Focus 1 = hard edge.

## Keys [editing.html; the keys exist in the input handler 0x1400645d0, actions as documented]
| key | action |
|---|---|
| `=` / `-` | zoom in / out |
| `[` / `]` | brush strength down / up |
| Backspace | undo (8 full heightmap copies, verified) |
| Del | delete (Billboards and Statues modes) |
| TAB | back to game mode |
| `d` | duplicate (Statues mode) |
| `f` | flatten the gradient profile (reset to two points at height 0) |
| `p` | export a `.pov` file (track.pov) |
| `s` | save |
| `u` | update the back buffer (rebuilds the render after painting; HUD shows "Update needed!" until you do) |
| arrows / right-drag | scroll the map |

Modes [editing.html]: Raise / Lower add or remove "strength" feet; Smooth blurs; Smudge blurs in the drag direction;
Snap drops detail by "strength" (polygon count optimization); Linear Paint/Add apply the profile along the gradient
line, Radial Paint/Add apply it around the gradient origin; Paint = set heights to the profile, Add = add the profile.

## Recipes [editing.html]
**Jump:** Gradient Placement, drag the endpoints along the jump's run. Gradient Edit: a side view appears (black grid
10 ft, grey 2.5 ft); click to add points until it's the jump shape. Gradient Linear Paint with Focus at least 0.5,
paint over the spot, press `u`. Distant terrain may look blocky until the tessellation map updates (about a minute).

**Berm:** Gradient Placement, center on the turn's center, endpoint outside the berm. Gradient Edit: `f` to flatten,
add a point at the berm's peak, one in the middle, one at the bottom; switch the bottom segment to a Curve (the HUD
shows "Segment type: Line/Curve"). Gradient Radial Add, Focus 0, zoom out until the brush covers the whole berm, one
click.

Why radial gives a berm: radial `s` = distance from the gradient origin, so the profile sweeps around the turn
(and behind the origin it extrudes straight, see docs). A berm that follows a non-circular turn is what this app's
curved gradient tool adds.

## What Save (`s`) writes (verified, 0x140018d90 and the object writers)
- `terrain.hf` and `terrain.png` (min/max recomputed, PNG re-quantized), `shading.ppm` + `shadingx2.ppm` (outputs
  only), `tilemap`, `decals` (if any), plus `edinfo`, `statues`, `billboards` from their modes.
- It writes its in-memory copy. Anything changed on disk while the editor has the track open is overwritten on save.
- Object positions in the editor's left panel are absolute heights; the files store y relative to the ground.

## Placing things the editor can't place
Flaggers (and gate rows) have no editor mode. Hover where you want one and read `x z` from the HUD, then type the line
into the file. For a gate row use mxsimulator.com/gate.html [Track Files Explained].

## Gradient Placement / Edit in detail
Rendering, panel buttons and mouse rules for the gradient modes are in
[editor-gradients-ui.md](editor-gradients-ui.md) (verified, 2026-11-04).
