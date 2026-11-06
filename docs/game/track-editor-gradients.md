# In-game track editor: gradients and brushes

*A copy of MX Simulator Tools' `docs/track-editor-gradients.md` (2026-11-05). The game facts are the same in both:
when one changes, change the other. Paths like `core/` and `tests/` here are that repo's.*

> How the editor draws gradients on screen (colors, handle sizes, the Edit profile graph) and its panel features
> (New/Copy Gradient, Set From Terrain, Flatten, Granularity, drag rules): see [docs/game/editor-gradients-ui.md](editor-gradients-ui.md).

Reverse engineered from mx.exe (SHA-256 54676784...dada392) with Ghidra. The function addresses are for that build; the
full RE notes live in the "Mx Sim" Claude Project (`claude/mx-track-editor.md`).

## Modes
0 Raise, 1 Lower, 2 Smooth, 3 Smudge, 4 Snap, 5 Gradient Linear Paint, 6 Gradient Linear Add, 7 Gradient Radial Paint,
8 Gradient Radial Add, 9 Gradient Placement, 10 Gradient Edit, 11 Timing Gate, 12 Billboards, 13 Statues,
14 Tilemap Edit, 15 Decals. Sub-modes Raise Only / Lower Only clamp the result. Undo keeps 8 full heightmap copies.
Keys: number keys pick modes, `s` save, `p` export track.pov, Backspace undo, `f` reset gradient profile.
Full key list, Focus (inner radius = Focus x radius) and the jump/berm recipes: [editor.md](editor.md).
Save (0x140018d90) re-quantizes terrain.png to the current min/max every time, and writes shading.ppm/shadingx2.ppm,
which the game itself never reads.

## edinfo (track folder, plain text)
```
add_gradient <ax> <az> <bx> <bz>
add_point <type> <s> <h>
add_point <type> <s> <h>
```
- A, B: map coords in ft. **The gradient is always a straight line in plan.**
- Points belong to the last `add_gradient`. `s` = distance along it (ft), `h` = height (ft).
- `type 1` = the segment from this point to the next is a Curve; anything else = Line.
- New/reset gradient = points (0, 0) and (|AB|, 0), both Line.

## Turning a map point into s
- Linear: `s = dot(P - A, unit(B - A))`. Unbounded sideways; the brush footprint limits where it applies.
- Radial: `v = P - A`. If `dot(v, B - A) < 0` (behind A), remove v's A->B component. `s = |v|`.
  Result: circular in front of A, straight extrusion behind it (a half-round berm shape).

## Height profile h(s)
- Before the first point: first height. After the last: last height.
- Line segment: linear interpolation.
- Curve segment: cubic Hermite. With `L = s1 - s0`, `t = (s - s0) / L`, `m0 = slope0 * L`, `m1 = slope1 * L`:
  `h = h0 + t*(m0 + t*(3(h1-h0) - 2m0 - m1 + t*(m0 + m1 + 2(h0-h1))))`
- Slope at each point (secant = rise/run of a segment):
  - first point: first segment's secant; last point: last segment's secant
  - interior, previous segment Curve and next Curve: average of the two secants
  - interior, previous Curve and next Line: next secant
  - interior, previous Line: previous secant
  So curves meet neighboring straight segments tangentially (no kink).

## Brush
- Footprint = capsule around the mouse drag segment (previous position -> current). `d` = distance to that segment.
- Weight: `d <= inner` -> 1. Between inner and outer: `u = (d - inner)/(outer - inner)`, `w = 1 - 3u^2 + 2u^3` (smoothstep). Beyond outer -> 0.
- Paint: `delta = h(s) - current`. Add: `delta = h(s)`. `new = current + clamp(strength, 0, 1) * w * delta`, then Raise Only / Lower Only.

## Curved gradients (this app, built)
The game only curves the height profile; a gradient that follows a turn in plan view is the app's (and the editor
mod's) addition. How the tool works: "Curved gradients" in MX Simulator Tools' `docs/track-editor-app.md` (and this repo's
`docs/ingame-tools.md` section 8 for the copy in the game). The path is a Bezier pen path
(`core/curved.py`, `web/pen-path.js`). Per cell: `s` = arc length to the nearest point on the path, `d` = sideways
distance, the same profile and smoothstep falloff as above, plus optional banking `bank * signed d`.

Design notes that still hold:
- Tight turns: if the turn radius is smaller than the width, inner cells are nearest to two parts of the curve and the
  surface creases. The panel warns ("sides will fold").
- Ends need an explicit cutoff; the game's straight gradient has none because the brush limits it.
- Close the in-game editor before saving: it holds its own copy and overwrites on its save (or use the editor mod).

## Where the code is (in MX Simulator Tools; the mod's ports are listed in `docs/ingame-tools.md` section 8)
- Height math (profile, s/d per cell, falloff, applying to the grid): `mxtools/core/gradient.py` and
  `mxtools/core/curved.py`, numpy, no UI, tested in `tests/test_gradient.py`, `test_gradient_brush.py`, `test_curved.py`.
- The page only edits points and the profile and shows previews: the full-res grid stays in Python and the page gets
  a shaded image of it.
- Saving goes through `core.terrain.save_terrain` (backups, range widening, mtime guard).
- `mxtools/core/gradient_check.py`: compares the app's math with terrain the game painted
  (`python -m mxtools.core.gradient_check BEFORE AFTER --edge-ft N`).

## How to prove it matches the game
1. Copy a track (e.g. BetterBlank) to a scratch folder and back up its `edinfo`.
2. In the game (`--editor`), make one linear gradient with a few Line and Curve points, then paint it with
   Gradient Linear Paint, strength 1, a brush much wider than the area, so the result equals h(s) everywhere it covers.
   Press `s` to save. That writes `terrain.png` and `edinfo`, and rewrites the track's other files too (list in
   the editor mod's `docs/design.md`, M2).
3. In the tool, load the original terrain and that `edinfo`, apply the same gradient at full strength, and compare to
   the game's saved terrain over the painted area. They should match to within one 16-bit step (plus a little at
   the brush edge, which follows the drag path).
4. Keep the before/after files and `edinfo` as a test fixture in `tests/fixtures/gradient_reference/` (`before/`,
   `after/`, `edge_ft.txt`): `test_matches_the_gradient_painted_in_the_game` then runs the comparison. **Still to
   capture**; until then that test skips. Step 3 is automated by `gradient_check` (above).
The tests that need no game (profile endpoints, Line and Curve segments, the smoothstep weight) are in
`tests/test_gradient.py` and `tests/test_gradient_brush.py`.
