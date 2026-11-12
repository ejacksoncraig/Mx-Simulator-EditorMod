# Changelog

What each version of the MX Simulator Tools editor mod added or changed, newest first. When `MXMOD_VERSION` in
`src/protocol.h` goes up, add a line here.

Versions before 0.36 were rebuilt from the notes in `.claude/skills/mx-editor-mod/ingame-tools.md` and from the text in
each packaged DLL in `dist\` (0.26.0 onward), since git has only one commit. Nothing was found for 0.22, 0.24 or 0.26.

## 0.36.2 (2026-11-12)
- Statue group: drag any statue with a gold ring to move the whole group by the same offset. Each keeps its turn and
  its height above the ground. One drag is one undo step (Undo Statue Move, or Ctrl+Z), sharing the 16 steps with
  Snap. A plain click on a group statue no longer selects it in the game; on other statues it still does.
  Not tried in the game yet.

## 0.36.1 (2026-11-10)
- On build 1684 the panel no longer says "Smooth stays round", and the shape's outline shows in Smooth too. On 1684,
  Smooth goes through the same shaped distance as Raise and Lower. 1.0 and 1681 still smooth round.

## 0.36.0 (2026-11-10)
- A statue group in Statues mode (build 1684 only; 1.0 and 1681 show "Not ported to this game build yet"):
  - Ctrl+click a statue to add it to the group or drop it. With an empty group, the game's selected statue joins too.
  - Ctrl+drag on empty ground draws a box; the statues whose centres are inside join the group.
  - Snap Group to Gradient runs the game's own Snap to Gradient on every statue in the group, against the selected
    gradient. Undo Statue Snap (or Ctrl+Z) puts them back, 16 steps.
  - Group statues show a gold ring. The group clears when a statue is added or deleted.
  - Not tried in the game yet.

## 0.35.2 (2026-11-09)
- Removed the mod's decal copy (D and "Copy To Its Right"). `d` is the game's own copy again.

## 0.35.1 (2026-11-09)
- The mouse wheel zooms the editor (0.25 of the Zoom slider a notch), keeping the ground under the cursor in place.
  Not tried in the game yet.

## 0.35.0 (2026-11-09)
- Decal editing in Decals mode (build 1684 only; 1.0 and 1681 show "Not ported to this game build yet"):
  - Click picks the smallest decal under the mouse.
  - Corners scale the decal; sides stretch it (Alt: both sides, Shift: whole feet).
  - A rotate handle turns it (Shift: 15° steps).
  - `[` `]` scale 10%; `{` `}` scale 1% (only while the game's group is empty).
  - Delete Decal, and our own undo (Ctrl+Z, 64 steps per track).
  - A copy key (D), removed in 0.35.2.
  - Not tried in the game yet.

## 0.34.1 (2026-11-09)
- Rut Stamp shelved: the button is hidden. The code is kept, and building with `MXMOD_STAMP=1` brings it back.

## 0.34.0 (2026-11-09)
- Berm Preview in Gradient Placement and Edit: shows what Radial Add would do, with no undo step.
- Every preview (Berm Preview, curve Live, stamp) comes off before the game saves (`s` or the Save button), so the
  track file never gets a preview in it.

## 0.33.0 (2026-11-09)
- Triangle Width and Triangle Lean for the triangle brush.

## 0.32.0 (2026-11-09)
- Triangle brush shape.

## 0.31.0 (2026-11-08)
- Set From Terrain (Sharp) and Terrain Fit (ft): traced profiles keep jump lips and edges sharp. The curve tool's own
  Set From Terrain uses this too. Not tried in the game yet.

## 0.30.0 (2026-11-06)
- Rut Stamp: stamps a height image (PNG, TIFF, BMP, or the built-in rut) onto the ground, with Depth, Length, Width,
  End Fade and Side Fade.

## 0.29.1 (2026-11-06)
- The brush size in feet holds at any zoom (it used to let go a couple of zoom steps in).

## 0.29.0 (2026-11-06)
- Shift+drag moves a whole curve.
- The "Undo Paste Or Move" button from 0.28.1 is gone.

## 0.28.1 (2026-11-06)
- Undo Paste Or Move (Ctrl+Z) button. Removed again in 0.29.0.

## 0.28.0 (2026-11-06)
- The game brush's size in feet (Outer and Inner), held while zooming.
- Brush presets and curve presets: save, rename, delete, pick from a list.
- The curve's Inner slider goes to 50 ft and Outer to 100 ft.
- Moving a range of points on a game gradient snaps to the game's Granularity.

## 0.27.0 (2026-11-06)
- Duplicate Curve: a copy 10 ft to the side.
- Move several profile points: Shift+click the start and end, then drag. Works on the game's graph and ours.

## 0.25.2 (2026-11-05)
- Put Back Where It Was no longer stacks an invisible copy when pressed twice.

## 0.25.1 (2026-11-05)
- A gradient always stays selected after a delete (the game's clicks need one).
- Profile sections reworded as numbered steps, with the start and end marked on the graph.

## 0.25.0 (2026-11-05)
- New panel look: gold group headers and grey hint lines.
- Delete Gradient (also Del) and Undo Delete (the last 16).
- Export Gradient and Saved Gradients: Place On Map, Put Back Where It Was, Open Gradients Folder.
- Radial Sweep: 30° to 360°, both sides, clockwise or counter-clockwise.

## 0.23.1
- The tripwire also drops network sends to this PC.

## 0.21.0
- Tripwire: every network send is dropped while the mod is loaded, with a one-time warning.

## 0.20.0 (2026-11-05)
- An unsupported game version shows a message instead of quietly doing nothing.

## 0.19.0 (2026-11-05)
- Works on whatever track the editor has open; `--track` is no longer needed.

## 0.18.0 (2026-11-05)
- No more point limits: gradients grow as needed. Curves hold 1024 path points and 2048 profile points.

## 0.17.0 (2026-11-05)
- Easy to share: no runtime to install, a zip with install.bat and uninstall.bat, and the files kept in
  `%LOCALAPPDATA%\MX Simulator\mxtools`.

## 0.16.0 (2026-11-05)
- Brush shapes for the game's brush: Circle, Square, Oval, with Brush Angle and Oval Width.

## 0.15.0 (2026-11-05)
- Profile sections: a dropdown to pick one, and Rename Section.

## 0.14.0 (2026-11-05)
- Profile sections: save a piece of a profile (a jump, a table top) and paste it onto another gradient or curve.

## 0.13.0 (2026-11-05)
- Gradient Length (Locked / Extend / Stretch) for the game's own gradients.

## 0.12.0 (2026-11-05)
- Curve Profile graph: edit points, Segment Type, Flatten, Set From Terrain, Slope and Height readouts.

## 0.11.0 (2026-11-05)
- Curve settings: Inner, Outer, Bank, Strength, Bake As, SubMode, Path Change, plus Length and Tightest Turn readouts.

## 0.10.0 (2026-11-05)
- Live Preview and Apply To Ground for curves (Apply is one undo step).

## 0.9.0 (2026-11-05)
- Draw and edit curves in the game: points, handles, Alt corner, Ctrl delete, New / Delete / Save Curves.

## 0.8.0 (2026-11-05)
- The mod's panel with Curve Path and Curve Profile, and the track's curves drawn on the map.

## 0.7.1 (2026-11-05)
- The app (MX Simulator Tools) can draw and edit curves in the game. Later replaced by tools that run inside the mod.

## 0.7.0 (2026-11-05)
- The app gets the editor's mouse and keys.

## 0.6.0 (2026-11-05)
- The app can draw lines and markers over the editor map.

## 0.5.0 (2026-11-05)
- Hooks into the editor's drawing and input, and the right-hand "MX Tools" panel.

## Before 0.5
- The app link: MX Simulator Tools reads and writes the editor's ground and gradients over a local connection.
