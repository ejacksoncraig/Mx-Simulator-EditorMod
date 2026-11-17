# Jcraig's Editor Mod: features (0.47.0)

Tools added to MX Simulator's track editor (`mx.exe --editor`). Offline only. The tools are in the panel at the top
right. Items marked * are new and haven't been tried in the game yet.

## Curves (curved gradients)
- Draw and edit curves on the map: points, handles, Alt for a corner, Ctrl to delete a point. New, Delete, Save
  Curves, Duplicate Curve (a copy 10 ft to the side), and Shift+drag to move a whole curve.
- Settings: Inner (up to 50 ft), Outer (up to 100 ft), Bank, Strength, Bake As, SubMode, Path Change, and Granularity.
  It also shows the Length and Tightest Turn.
- Curve Profile graph: edit points, Segment Type, Flatten, Set From Terrain (Sharp keeps jump lips and edges sharp,
  with Terrain Fit), and Slope and Height readouts.
- Live Preview, and Apply To Ground (one undo step).
- Bake Side: Both, Left or Right, for one-sided banked turns. *
- Ease Plain Points: smooths the notch where a straight meets a bend. On by default. *
- Curve presets: save, rename, delete, pick from a list.
- Undo and redo for curve edits: Ctrl+Z and Ctrl+Y, 64 steps. *
- Curves are saved beside the track as `mx-tools-curves.json`, the same file MX Simulator Tools uses.

## The game's gradients
- Gradient Length: Locked, Extend or Stretch.
- Delete Gradient (or Del), and Undo Delete for the last 16.
- Export Gradient, then Saved Gradients: Place On Map, Put Back Where It Was, Open Gradients Folder.
- Shift+drag a gradient's line to move the whole gradient. *
- Gradient group: Ctrl+click or Ctrl+box-select gradients and curves, then Shift+drag to move them together. *
- Radial Sweep: 30° to 360°, either side, clockwise or counter-clockwise.
- Berm Preview: shows what Radial Add would do before you do it.
- Move several profile points at once: Shift+click the start and end, then drag. Works on the game's graph and the
  curve graph. Moving points on a game gradient snaps to its Granularity.

## Profile sections
- Save a piece of a profile (a jump, a table top) and paste it onto another gradient or curve.
- Pick sections from a list, rename and delete them, and Undo Delete.

## Brush
- Brush shapes: Circle, Square, Oval and Triangle, with Brush Angle, Oval Width, Triangle Width and Triangle Lean.
- The brush size is set in feet (Outer and Inner) and stays the same at any zoom.
- Brush presets.
- On build 1684, Smooth uses the brush shape too.

## Statues (build 1684)
- Statue group: Ctrl+click or Ctrl+box-select statues. The group's statues get gold rings.
- Drag one statue in the group to move them all.
- Snap Group to Gradient (button, or `g`). *
- Duplicate Group: `d` puts the copies at the mouse, and the button puts them 10 ft to the side. *
- Linked pieces from MX Simulator Tools: Ctrl+click one part to take the whole piece. Duplicate keeps each copy linked,
  Snap moves each piece as one, and saving keeps the links in the app. *
- Statue Size: scale the group or one statue by a percentage, or set it back to Real Size. It writes the same files
  as MX Simulator Tools. *
- Clicking a statue shows its texture's name. *
- Ctrl+Z undoes moves, snaps, copies and resizes.

## Decals (build 1684)
- Pick a decal the game's way, by its centre dot or rotate dot. Drag the corners to scale it, the sides to stretch it, and the handle to rotate it.
  Shift and Alt change the steps.
- `[` `]` scale by 10%, and `{` `}` by 1%.
- Delete Decal, and undo (Ctrl+Z, 64 steps).
- Clicking a decal shows its texture's name and size. *

## View and panel
- The mouse wheel zooms, keeping the ground under the cursor in place.
- Ground Grid: lines every 5, 10, 25, 50 or 100 ft. *
- Click a panel group's header to fold it. Long lists scroll. *
- The tool settings and folded groups are remembered between sessions. *

## Safety
- It never changes mx.exe's file. It checks the game's code before hooking it, and puts it back when it closes.
- Previews are taken off before the game saves, so the track never saves a preview.
- Online tripwire: while the mod is installed, the game can't connect to anything. Uninstall the mod before racing
  online.
- It works alongside another `opengl32.dll` (shaders, overlays).
- Game builds: all the tools work on the 2026-05-14 build (1684). On 1.0 and 1681, everything except the statue and
  decal tools works. Any other build shows a message, and the mod does nothing.

## MX Simulator Tools link
- The app can read and write the editor's ground and gradients live, draw over the map, and see the mouse and keys.
