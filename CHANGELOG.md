# Changelog

What each version of the MX Simulator Tools editor mod added or changed, newest first. When `MXMOD_VERSION` in
`src/protocol.h` goes up, add a line here.

Versions before 0.36 were rebuilt from the notes in `.claude/skills/mx-editor-mod/ingame-tools.md` and from the text in
each packaged DLL in `dist\` (0.26.0 onward), since git has only one commit. Nothing was found for 0.22, 0.24 or 0.26.

## 0.46.0 (2026-11-16)
- Linked pieces from MX Simulator Tools work in Statues mode (13). The app's links (`mx-tools-groups.json` beside the
  track) are read when the track loads.
  - Ctrl+click a part of a linked piece: the whole piece joins the statue group, or leaves it. A Ctrl+drag box that
    takes one part takes the rest.
  - Duplicate Group (`d` or the button): each copied piece is linked as its own piece.
  - Snap Group to Gradient (`g` or the button): each linked piece moves as one. Its middle goes where the game's snap
    puts a statue there, and every part turns and moves the same, so the parts stay together. Statues that aren't
    linked snap one by one, as before.
  - When the game saves (`s`), the links file is written again with the pieces' new lines, so pieces moved, snapped or
    copied in the game are still linked in the app. Their billboards, flares and flaggers stay as the app wrote them.
  - The panel says how many linked pieces the track has.
- The JSON reader now reads `é`-style characters, which the app writes for any non-ASCII name.
- Tried with a fake statue list, not in the game yet.

## 0.45.1 (2026-11-16)
- `g` in Statues mode (13) with a statue group: Snap Group to Gradient, the same as the button. (`s` stays the game's
  save.) Ctrl+Z undoes it as before.
- Not tried in the game yet.

## 0.45.0 (2026-11-15)
- STATUE SIZE in Statues mode (13): resize statues in the editor the way MX Simulator Tools does.
  - Works on the statue group, or on the statue the game has selected when there's no group.
  - Scale by (%) (10 to 300) and Scale Group / Scale Statue multiply each statue's present size about its own centre.
    Real Size puts it back to the original files. The panel shows the (first) statue's size against its original.
  - The files are the app's: `<track>/statue/scaled/<mesh>-<pct>pct/` with the scaled `.jm`, its `_lod` meshes, its
    `_norm` / `_spec` maps, the scaled `.shp` and `mx-tools-scale.json`. Each tool picks up the other's copies, and a
    resize always counts from the original, so sizes don't drift. Checked byte for byte against the app's scaler.
  - The files are written straight away (the game has to load them), and the statue keeps its place in the list, its
    position, turn, height and texture. Press s to save the statues file.
  - Only loose files can be resized here. A mesh inside a .saf pack (or gzipped) says to use the app instead.
  - Ctrl+Z (or Undo Statue Size) puts the old files back.
  - 1684 only.
- Not tried in the game yet.

## 0.44.0 (2026-11-15)
- Clicking a decal (Decals mode, 15) shows its texture's name in the status line, with its number and size, like
  billboards do.
- Clicking a statue (Statues mode, 13) shows its texture's (png's) name in the status line, with its number.
- Both read the name with the game's own texture-name call (the one its statues and decals writers use). 1684 only.
- Not tried in the game yet.

## 0.43.0 (2026-11-15)
- Duplicate Group in Statues mode (13): copies every statue in the statue group with the game's own add (the one its
  `d` uses), keeping each one's object, turn and height above the ground.
  - `d` with the mouse on the map puts the copies so the group's middle lands on the mouse. With no group, `d` is the
    game's own one-statue copy as before.
  - The Duplicate Group button in STATUE GROUP puts them 10 ft along +x from the originals.
  - The group becomes the copies (gold), so a drag on one places them all.
  - Ctrl+Z (or Undo Statue Copy) takes the copies back off. It works only while the statue list is as the copy left it.
  - 1684 only, like the rest of the statue group.
- Not tried in the game yet.

## 0.42.0 (2026-11-14)
- Gradient group in Gradient Placement (9), like the statue group, holding the game's gradients and the mod's curves
  together:
  - Ctrl+click a gradient's or curve's line to add it to the group or drop it. With an empty group, the game's
    selected gradient joins too, so "click one, Ctrl+click the rest" works.
  - Ctrl+drag on empty ground draws a box; every gradient and curve whose middle is inside joins.
  - Shift+drag any gold line moves the whole group by the same offset. Profiles stay as they are and the ground doesn't
    change, as with the single move. Shift+drag on a line outside the group still moves just that one, and that now
    works on curves too.
  - Group lines are gold; with Ctrl or Shift held, the line a press would grab is outlined faintly. GRADIENT GROUP in
    the panel shows the count, Clear Group and the keys.
  - One group move is one undo step (Ctrl+Z or Undo Gradient Move, 16 kept). Anything changed since is left where it
    is. A move that includes curves is also one step in Curve Path's own undo.
  - Gradients leave the group when one is added or deleted (their numbers shift); curves leave it when one is deleted
    or the curves file is read again. Another track empties it.
  - Ctrl+click on empty ground in Placement now starts the box instead of reaching the game.
- Not tried in the game yet.

## 0.41.0 (2026-11-14)
- Ease Plain Points (curve tool, under Bake Side): On by default. When a curve's line runs through a plain point
  (one with no handles) and carries on within 30 degrees of straight, the bake gives that point short handles along the
  line, so it turns over a few feet instead of on the spot. This takes out most of the notch across the inside of a
  jump where a straight runs into a bend: on 1Halo's test curve the steepest step between neighbouring cells at the
  bend start went from 0.73 ft to 0.28 ft. The drawn line doesn't change; the baked one moves up to about 0.6 ft near
  such a point, and the profile slides by about 0.1 ft. Sharper corners stay sharp. Off bakes as before. Remembered
  between sessions; not kept in curve presets. The app doesn't do this yet, so with it on the two bake differently.
- Not tried in the game yet.

## 0.40.1 (2026-11-12)
- Bake Side Left/Right now keeps the full-height part between both inner lines and leaves out only the other side's
  fade, so that side ends in a hard edge at its inner line (0.40.0 cut it at the curve line). Bank still pivots on the
  curve line.
- Not tried in the game yet.

## 0.40.0 (2026-11-12)
- Bake Side (curve tool, under Bank): Both, Left or Right. Left or Right bakes only that side of the curve's line,
  looking from the curve's start to its end on the map; the other side's ground isn't touched. With Bank this makes a
  one-sided banked turn that rises from the line outward. Kept in curve presets and remembered between sessions.
- Not tried in the game yet.

## 0.39.2 (2026-11-12)
- Bank goes from -0.5 to 0.5 (was -1 to 1), so the slider is finer. 0.5 is about 27 degrees. A saved preset or setting
  past 0.5 is held to 0.5 when it's read.
- Not tried in the game yet.

## 0.39.1 (2026-11-12)
- Fix: Bank could never be set back to 0 once moved. The slider has 200 steps but fewer pixels, so 0 fell between two
  pixels. Sliders that run through zero now snap to 0 within 4 px of it, and every slider value is a whole number of
  steps.
- Not tried in the game yet.

## 0.39.0 (2026-11-12)
- Ground Grid (new VIEW group at the top of the panel): Off, 5, 10, 25, 50 or 100 ft lines over the track, in every
  mode. Brighter every 100 ft (every 5th line for the 50 and 100 ft grids). Zoomed far out it thins the lines and says
  so ("Zoomed out: lines every 50 ft"). Kept off the game's left panel, drawn under the mod's other lines, and
  remembered between sessions.
- Move a whole gradient: in Gradient Placement (9), hold Shift and drag a gradient's line. Both ends move together and
  the profile stays as it is. With Shift held, the line a press would grab is outlined in gold. Ctrl+Z (or Undo Gradient
  Move) puts the last 16 moves back. The ground doesn't change; only the gradient does.
- Not tried in the game yet.

## 0.38.0 (2026-11-12)
- Undo for curve edits: Ctrl+Z undoes, Ctrl+Y redoes (also the Undo Curve Edit and Redo buttons in Curve Path and
  Curve Profile). A press and drag, a panel button or a key is one step; 64 kept. Picking a curve alone isn't a step.
  Cleared when the curves file is read again (another track, or the app rewrote it). The game's Backspace still
  undoes ground changes.
- Undo Delete Section and Undo Delete Preset: the last 16 deleted, put back at their old place, newest first.
- The curve tool remembers its settings between sessions (Inner, Outer, Bank, Strength, Bake As, SubMode, Path
  Change, Granularity, Terrain Fit) in `mxtools\curve-settings.txt`.
- Fix: curve Live Preview now puts the track's min and max height back when it comes off (and before a save), as Berm
  Preview does, so a preview that reached higher or lower doesn't change how Save scales terrain.png. A height change
  the game's own brush made meanwhile is kept. Apply widens it only as far as the baked curve needs.
- Not tried in the game yet.

## 0.37.0 (2026-11-12)
- Fold groups: click a gold header (now marked `[-]`) to hide the rows under it, and again (`[+]`) to show them.
  Folded groups are remembered between sessions (`mxtools\panel-folds.txt`). A header lights up under the mouse.
- Long lists scroll: the Section, Curve Presets, Brush Presets and Saved Gradients dropdowns show 8 at a time, with
  "^ n more above" / "v n more below" rows to page through, and the mouse wheel over the list scrolls it. The buttons
  under a list (Paste, Rename, Delete) stay on screen. Opening a list scrolls to the one chosen.
- The panel is built in full before folding, so folding a group frees its rows for the groups below it.
- Not tried in the game yet.

## 0.36.3 (2026-11-12)
- The panel's top-right label reads "Jcraig's Editor Mod 0.36.3" (was "MX Tools ..."), a little larger and in light
  gold so it's easy to read.

## 0.36.2 (2026-11-12)
- Statue group: drag any statue with a gold ring to move the whole group by the same offset. Each keeps its turn and
  its height above the ground. One drag is one undo step (Undo Statue Move, or Ctrl+Z), sharing the 16 steps with
  Snap. A plain click on a group statue no longer selects it in the game; on other statues it still does.
  Tested in the game (2026-11-12): works.

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
  - Tested in the game (2026-11-12): works.

## 0.35.2 (2026-11-09)
- Removed the mod's decal copy (D and "Copy To Its Right"). `d` is the game's own copy again.

## 0.35.1 (2026-11-09)
- The mouse wheel zooms the editor (0.25 of the Zoom slider a notch), keeping the ground under the cursor in place.
  Tested in the game (2026-11-12): works.

## 0.35.0 (2026-11-09)
- Decal editing in Decals mode (build 1684 only; 1.0 and 1681 show "Not ported to this game build yet"):
  - Click picks the smallest decal under the mouse.
  - Corners scale the decal; sides stretch it (Alt: both sides, Shift: whole feet).
  - A rotate handle turns it (Shift: 15° steps).
  - `[` `]` scale 10%; `{` `}` scale 1% (only while the game's group is empty).
  - Delete Decal, and our own undo (Ctrl+Z, 64 steps per track).
  - A copy key (D), removed in 0.35.2.
  - Tested in the game (2026-11-12): works.

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
