# The app's gradient tools inside the game's editor (spec and task list)

Goal (2026-11-05): the app's new gradient tools (Curve Path, Curve Profile, and later others) show up and work
inside `mx.exe --editor` as if they shipped with the game: buttons in the game's own panel style, the curve and its
handles drawn on the game's map, worked with the game's mouse and keys. You never touch the app window.

Read first: `docs/design.md` (rules, protocol, M0-M4), `game/editor-gradients-ui.md` (the game's panel look
and widget behaviour), MX Simulator Tools' `docs/track-editor-app.md` ("Curved gradients"). Every address below is from mx.exe build
54676784...a392, written as the full VA (`0x14xxxxxxx`); subtract `0x140000000` for the RVA the mod uses.

Rules that matter here (CLAUDE.md): code patches and hooks in memory are allowed **offline only**. The mod must stay a
plain pass-through unless `--editor` and the known build, must never be active online (the tripwire, `docs/design.md` rule 2), and
the app never writes game memory itself.

## 1. Architecture: the app is the brain, the mod is the eyes and hands

The curve tool already exists and is tested in the app: `web/curve-tool.js` and `web/pen-path.js` (the pen, picking,
dragging, the panel rows) and `core/curved.py` (path, baking). Porting that to C would mean two copies of every rule.
Instead:

- **The mod** draws what the app tells it, using the game's own drawing and widget functions, so it looks native. It
  reports the game's mouse and keys to the app in world feet, and handles its own widgets' hover, click and slider drags
  the way the game does.
- **The app** runs the tool. The Track Editor follows the game's view and treats game events exactly like clicks on its
  own map (the JS handlers already work in world feet through `toScreen` / screen-to-world). After each change it sends
  the mod a new draw list and panel description. Heights still go through M3's `CurveInGame` path (preview and Apply).
- **Fail-safe:** with no app connected, or no panel sent, the mod draws nothing extra and eats no input. The game is
  exactly as before.

The app must be running for the tools to work. Over a local pipe a round trip is about a millisecond; at 60 fps the
app polls events every frame or two, which is fast enough for dragging.

## 2. What the game does (verified in REA, 2026-11-05)

### View and mouse
| what | where | type |
|---|---|---|
| window client width, height (px) | `0x1433e40d8`, `0x1433e40dc` | int |
| mouse x, y (client px, y down) | `0x1433e40e0`, `0x1433e40e4` | int, updated by the game's message loop |
| ortho half extents | `0x1433e40e8`, `0x1433e40ec` | used as `ortho(-e8, e8, -ec, ec, -1000, 1000)` |
| view centre x, z (ft) | `0x1417e25dc`, `0x1417e25e0` | float |
| zoom | `0x1401778e8` | double (the Zoom slider's value; slider struct at `0x1401778e0`) |
| map size (ft) | `terrain + 0x10c8` | float |
| editor mode | `0x1417e2600` | int 0..15 |
| hovered game widget | `0x1417e25a4` | int, -1 = none (0..15 mode buttons, 0x10 SubMode, 0x11 Save, 0x12 Zoom, 0x13 Strength, 0x14 Focus, 0x15 Radius, ... up to 0x27) |
| mouse button state | `0x1417e25e4` | 0 up, 1 painting on the map, `id | 0x10000` dragging game widget `id` |

World from screen, as the game computes it everywhere (0x1400613b0, 0x1400645d0):
```
viewFt = sizeFt / 2^zoom
x = (mouseX - W/2) / H * viewFt + centreX        (W/2 and H/2 are integer divisions)
z = (mouseY - H/2) / H * viewFt + centreZ
```
The inverse gives screen px for a world point. The marker unit is `u = max(viewFt * 0.01, 1)` ft and pick radius
`u / 2` (game/editor-gradients-ui.md section 1).

### Input: one handler for keys and mouse buttons
`0x1400645d0(uint key, int down)`, returns 1 when handled. The main loop (0x14016e460) calls it for every key and mouse
button press and release, with the game's own key codes:
- left mouse button = the code in `0x1433e4060`; pan-drag button = `0x1433e4064`; arrow keys = `0x1433e4040..4c`.
  Printable keys are their ASCII code (`0x73` 's' save, `8` Backspace undo, `0x31..0x39` mode keys, `=` / `-` zoom).
- Its first lines: when the mouse moved, it re-picks the hovered widget with `0x14001fa70(list)`. Then, on a left press
  with a widget hovered, it starts a widget drag (`25e4 = id | 0x10000`, calls `0x14005de30(key)`); with no widget
  hovered it starts a map action for the current mode.
- `0x140066bf0()` is the per-frame drag and paint step. It only paints while `25e4 == 1`, so a press the mod swallows
  never paints.

### Drawing: the editor's whole 2D pass is one function
`0x1400613b0()` (called once per frame from the main loop at 0x140172ff8) sets the camera, draws the terrain and the
current mode's overlays (gradients, gates, statues), then the panel, last line `status text`. It records into the
game's command list (ops at `0x1413dcb00`, count `0x1413dcaf8`) and its vertex batch, which are flushed after it
returns. **So anything drawn right after it returns, with the same helpers, renders on top in the same frame.**

Helpers (all take floats in xmm registers, Windows x64 ABI):
| call | what |
|---|---|
| `0x140044910()` | load identity |
| `0x140044b00(x, y, z)` | translate (op 0x14) |
| `0x140044c20(x, y, z)` | scale (op 0x13) |
| `0x140044b90(angle, x, y, z)` | rotate |
| `0x140044e80()` / `0x140044ec0()` | push / pop |
| `0x140044e30(mode)` | matrix mode (0x1700 modelview, 0x1701 projection) |
| `0x14000c9b0(prim)` | begin a batch: 1 lines, 2 line loop, 7 quads (GL primitive numbers) |
| `0x14005b730(x, z)` | vertex at (x, 0, z) with the current colour and uv |
| `0x14005bb60(x, y, z)` | vertex at (x, y, z) with the current colour and uv (verified). The panel's screen-space quads use this with z = 0; `b730(x, z)` is the same with y = 0 |
| `0x14000cd00()` | end the batch. The game sets the flag at `0x1412c3ac8` to 1 before the vertices and `= 0` before this call |
| colour | RGBA floats at `0x140177030..3c`; uv at `0x140177040..4c`. The code reaches them through pointer globals: colour `*(ptr*)0x14023d9a0`, batch flag `*(ptr*)0x14023d9e0`, white texture id `*(ptr*)0x14023d890` |
| `0x140044710(cap)` / `0x1400446c0(cap)` | enable / disable (`0xbe2` blend) |
| `0x140044540(target, id)` | bind texture (`0xde1`, `**0x14023d890` = the white texture the panel uses) |
| `0x1400445b0(src, dst)` | blend func (`0x302, 0x303`) |
| `0x1400f2610(str)` | text width; `0x1400f27c0(r, g, b, a, str)` draws text |

Widgets (use these so ours look and behave exactly like the game's):
| call | struct |
|---|---|
| button `0x14001ea30(btn, on, pos, scale, width, align)` | `{char *label; int rect[4]; int pad[2];}` (stride 0x18). Verified: `on` picks the lit colours; the game passes `on = 1` when the button is hovered or is the current mode (0x140061de7), so hover is just `on`. `width` = 20, `align` = 0.0 (text offset factor). It writes the rect in window px, y down: `x0 = pos.x*sx, y0 = (pos.y+1)*sy, x1 = (pos.x+width)*sx, y1 = pos.y*sy`, with scale = (H/64, -H/64) |
| choice `0x14001ed10(ch, hot, pos, scale, width, ..., split)` | `{char *label; char **options; ... int selected @+0x14; int rect[4] @+0x18}` |
| slider `0x14001f130(sl, hot, pos, scale, width, ..., split)` | `{char *label; double value, step, min, max; int rect[4] @+0x28; int knob[2] @+0x38}` |
| readout `0x14001e830(str, colourA, colourB, ...)` | text in a label box |
| next row `0x14001e190(float *pos, float *scale, float dx, float dy)` | translate the modelview by (dx, dy, 0) and add (dx, dy) to `pos` (verified; `scale` isn't read) |
| hide `0x14001e510(rect)` | sets a rect to -1 so it can't be hovered |
| hover pick `0x14001fa70(list)` | `list` is a null-terminated array of `int*` rects; returns the index under the mouse or -1 |
| press `0x14001fad0`, choice cycle `0x14001fb40`, slider click/drag/arrows `0x14001fd80` | the game's own widget handlers (check their args before calling them on our structs) |

The panel's screen space (verified, 0x140061b33..0x140061bbf): identity, `translate(-e8, ec, -1)`, `scale(ec/32, ec/32,
ec/32)`, with `e8, ec` the ortho half extents (window `+0x68, +0x6c`). So 1 unit = 1/64 of the height, origin top-left,
y up (the bottom is -64), and the window is `64 * e8 / ec` units wide. The backdrop: blend on, white texture, blend func,
`c9b0(7)`, flag = 1, vertices (0,0), (0,-64), (21,-64), (21,0), then the fade (21,0), (21,-64), clear (21.25,-64),
(21.25,0), flag = 0, `cd00`, blend off, then `e190(pos, scale, 0.5, 0)` and the rows.
**Corner order matters:** the game's quads go counter-clockwise (left top, left bottom, right bottom, right top). Clockwise
quads face away and aren't drawn; M5's first build drew nothing but the button until this was fixed.

The panel's layout (0x140061bc4..0x140061d8f): unit `p = H / 64` px (`local scale = (H/64, -H/64)`); backdrop quad from
x 0 to 21 p (plus a 21..21.25 fade strip), full height; first row at x = 0.5 p; each row moves down 1.5 p
(`e190(pos, scale, 0, -1.5)`); widgets are 20 p wide; labels/values split at the middle (10 p). Constants at
`0x1402253a0..cc`: 20, 64, ..., 0.5, -64, 21, 21.25, -1.5. Colours: `0x140177070..15f` (table in game/editor-gradients-ui.md
section 9).

### Hook points
All three start with the same 19-byte prologue, with no RIP-relative instructions, so a 14-byte absolute jump plus a
trampoline of the moved bytes works:
```
41 57 41 56 41 55 41 54 55 57 56 53 48 81 ec <imm32>     push r15..rbx (12 bytes), sub rsp, imm32 (7 bytes)
```
- `0x1400613b0` editor draw: `sub rsp, 0x558`
- `0x1400645d0` key/button handler: `sub rsp, 0x118`
- `0x140066bf0` per-frame drag/paint: `sub rsp, 0x1e8` (probably not needed, see above)

Check the 19 bytes match before patching, patch with VirtualProtect + FlushInstructionCache, only after `start()`
enabled the mod, and restore them if the tripwire fires or the DLL detaches.

## 3. How the mod does it

**Hook A, after the editor draw (`0x1400613b0`):** call the original, then, if the app has sent a scene:
1. Map overlay: set up screen space the same way the panel does (copy the call sequence just before 0x140061c69), but
   with scale 1 px, convert each world point to screen px with the formula in section 2, and draw lines and markers
   with `c9b0 / b730 / cd00`. This avoids rebuilding the map camera. (Alternative: replay the camera ops from the top
   of 0x1400613b0 and draw in world feet; only if the screen-space version shows any offset.)
2. Our panel: a second panel down the **right** edge, mirroring the game's (backdrop 0..21 p from the right edge, same
   rows, same widget calls). The left panel's height changes with the mode, so putting ours under it would collide. Draw
   each widget the app described with the game's widget functions into the mod's own structs, and keep a
   null-terminated rect list for hover picks.

**Hook B, the key/button handler (`0x1400645d0`):** before calling the original:
1. If the mouse is over one of our widgets: handle it like the game handles its own (press = button fires, choice cycles,
   slider click/drag through our own copy of 0x14001fd80's rule: `value = min + round((max - min) * t / step) * step`),
   queue a `widget` event for the app, and return 1. Keys while hovering our widget go to it, like the game's (section 11).
2. Else, if one of our tools is active and the game's hovered widget is -1 (the mouse is on the map): queue the event
   (press/release, key code, world x, z, screen px, Ctrl/Shift/Alt from GetAsyncKeyState) and return 1, **except**:
   the pan-drag button, `=`/`-` zoom, arrows, `s` save, Backspace undo and Tab always go to the game too.
3. Else call the original.
Mouse moves: the mod samples `0x1433e40e0/e4` every frame in the SwapBuffers hook and queues one `move` when it changed
and a tool is active (or a button is held on our widget).

A tool is active when the app says so (its panel has a mode button marked on). Clicking any game mode button, or a game
number key, turns our tool off and queues a `toolOff` event, so the game's own tools keep working normally.

## 4. Protocol additions (protocol.h and game_link.py, the test keeps them in step)
| type | direction | payload |
|---|---|---|
| `get_events` (10) / `events` (11) | app to mod / reply | reply JSON: `{"view": {"cx", "cz", "viewFt", "w", "h", "mode", "u"}, "events": [{"t": "down"|"up"|"move"|"key"|"widget"|"toolOff", "x", "z", "px", "py", "key", "ctrl", "shift", "alt", "id", "value"}]}`. The mod keeps a ring of 256 events; on overflow it drops the oldest `move`s first. |
| `set_scene` (12) | app to mod | JSON: `{"tool": "curve"|"curveEdit"|null, "panel": [rows], "draw": [items]}`. Rows: `{"id", "kind": "button"|"choice"|"slider"|"label", "label", "on", "options", "selected", "value", "min", "max", "step"}`. Draw items: `{"k": "line", "pts": [[x, z], ...], "rgba": [...], "closed": bool}`, `{"k": "dot", "x", "z", "size": u-multiples, "rgba"}`, `{"k": "cross" ...}`. Reply `applied`. Applied at the next frame, replacing the last scene. Empty object = clear (nothing drawn, no input taken). |

JSON keeps it debuggable; a curve with a few hundred sample points is a few KB. Raise the 64-byte small-request limit in
`pipe.c` for type 12 (cap it, e.g. 256 KB). The mod copies the scene into its own memory on the game thread; the draw
hook only reads that copy.

## 5. How the app does it
- `core/game_link.py`: `get_events()`, `set_scene(scene)`.
- The bridge: a "Play in game" switch in the Track Editor (on while the editor mod is connected and the game has the
  same track open, the M3 checks). While on, a timer (every 16 ms) polls events and hands them to the page.
- `editor.js`: a "game viewport" mode. The page sets its view to the game's (`cx`, `cz`, `viewFt`), so `toScreen` and
  the pick radii match the game's, and feeds `down` / `move` / `up` / `key` into the same handlers the canvas uses.
- `curve-tool.js`: one function that turns the current curve state into draw items (the same things it paints on the
  canvas: path, anchors, handles, the selected point, the yellow profile dots, the Curve Profile graph 1:1 at A like the
  game's Edit graph), and one that turns its panel rows (`widget.button / choice / slider`) into scene rows. Panel
  `widget` events call the same row callbacks. Send a new scene after each change, at most once a frame.
- Heights: unchanged, M3's `CurveInGame` (Live = preview with no undo, Apply = one undo step).

## 6. Steps, each with a check (do them in order; ModTest only)
- **M5, hooks and a test draw (mod 0.5.0).** Install hooks A and B, pass everything through. Hook A draws one fixed red
  cross at the world point under the view centre and a fixed button "MX Tools" in a right-hand panel. Check: the cross
  stays on the same ground spot while panning and zooming; the button looks like the game's (compare a screenshot of
  the left panel); hovering it lights it up like the game's; the game's own tools and keys all still work; `MXMOD_READONLY=1`
  (or a new `MXMOD_NOHOOKS=1`) starts with no hooks at all.
  **Passed 2026-11-05 on ModTest (mod 0.5.0, `src/ingame.c`).**
  - Both prologues matched before patching (`sub rsp` 0x558 and 0x118) and both hooks went in at start-up.
  - In the game: the right-hand strip and the MX Tools button in the game's style, lit on hover; clicks on it (counted
    in `hello` as `toolPresses`) painted nothing; the red cross sat on the map centre and stayed on that ground while
    panning and zooming; mode keys, painting and Backspace worked as before.
  - Diagnostics (`hello.ingame`): the overlay ran on all 1220 editor frames; 2560 x 1440 window = 113.8 units wide;
    the button's rect x 2098..2548, y 11..33 px.
  - `MXMOD_NOHOOKS=1`: log "hooks off", `hello.hooks` = "off (MXMOD_NOHOOKS)", and reading the game's memory showed both
    targets still hold the original 19 bytes. `MXMOD_READONLY=1` turns the hooks off too (patching code is a write).
  - Hook B swallows left clicks that start on our panel, and their release; a press that starts on the game's side
    (a widget drag, a stroke) is released to the game wherever the mouse is.
  - The bytes are put back when the DLL is unloaded while the game runs (not at exit).
- **M6, scene from the app.** `set_scene` with draw items. Check: the app redraws gradient N's A to B line in a different
  colour (draw a `line` from `get_gradients`), and it sits exactly on the game's own line at every zoom.
  **Passed 2026-11-05 on ModTest (mod 0.6.0, `src/scene.c`).**
  - `game_link scene-line 0 --seconds 180` drew all 93 gradients' A to B lines and a cross at each end (279 items).
    In the game, every magenta line sat on the game's own line and the crosses on its A and B handles, zoomed in and out
    and panned. They went when the CLI disconnected (log 14:28:19; the scene read back empty).
  - Points are placed the way the game's map camera places them, not with the mouse formula: in panel units
    `ux = (x - cx) * 64 / viewFt + 32 * e8 / ec`, `uy = -(z - cz) * 64 / viewFt - 32` (no integer W/2).
  - With no scene, the editor is exactly as it was (no panel, no input taken). M5's test cross is gone; the panel and
    its button show only while the scene names a `tool`.
  - The scene is cleared when the app disconnects, so a crashed app leaves nothing on screen. Lines are 1 px GL lines;
    they're drawn after the game's panel, so they can cross over it (clipping left for M8).
  - The parser was checked on its own first (`scene_test.c` against `scene.c`, 13 cases plus a 20,000-point line from
    Python); that caught `[1, 2, 3]` being taken as a point.
- **M7, events.** `get_events`, the tool-active flag, swallowing map clicks. Check (CLI is enough): with a dummy tool on,
  clicks on the map print world x, z that match the game's own cursor readout (the `x y z` row at the bottom of the
  left panel); the map doesn't paint; the pan button, zoom keys, `s` and Backspace still work; clicking a game mode
  button turns the tool off.
  **Passed 2026-11-05 on ModTest (mod 0.7.0).** `game_link events --seconds 300` turned on a dummy tool. Map
  clicks put a cross under the mouse and didn't paint; pan, arrows, zoom keys and Backspace still worked; `f` and `u` did
  nothing in the game; `7` turned the tool off and the game's tools worked again. What arrived:
  - The left mouse button is key **8192** (0x2000), down and up at the same world point (e.g. 1538.13, 499.91).
  - **Every key comes twice:** a raw code in the 0x10xx range, then the character: `f` = 4129 (0x1021) then 102,
    `u` = 4118 (0x1016) then 117, `7` = 4167 (0x1047) then 55. Only the character matches `'0'..'9'`, `s`, `=`, `-`.
    So the raw copies of the keys that must reach the game (s, =, -, Backspace, number keys) are swallowed while the tool
    is on and only the characters get through. That worked here, but if the game ever acts on the raw code instead,
    those keys would need their raw codes passed too.
  - Codes **4108..4110** (0x100c..0x100e) arrived as down/up pairs and the tool took them; the view changed while one
    was held. Probably the mouse wheel or the middle button (not confirmed).
  - The game stops answering while it isn't drawing (window in the background): the CLI now waits instead of exiting.
- **M8a, Curve Path in the game, driven by the app. Works (2026-11-05, mod 0.7.1).** Play In Game in the Track Editor's
  curve panel: `GamePlay` in `core/game_link.py` holds the one pipe, polls events on a thread and sends the newest scene;
  the page follows the game's view (scale `H / viewFt`) and feeds game events into the canvas handlers (now named
  `mapMouseDown / Move / Up / KeyDown`). A curve was drawn and edited with the game's mouse. Gotchas: the scene has to
  be sent from the poll loop, since `requestAnimationFrame` doesn't run while the app's window is behind the game's; and
  Chromium's background timer throttling is turned off in `__main__.py`. The mouse wheel didn't zoom with the tool on:
  codes 0x100c..0x100e now always go to the game (0.7.1, not yet checked in game).
  **Decision (2026-11-05): the tools should work inside the game without the app.** The app keeps its own curve
  tool; the in-game work moves to a standalone tool in the mod, section 8. M8a stays as it is but isn't built on.
- ~~**M8, Curve Path in the game.**~~ ~~**M9, the panel.**~~ Superseded by section 8 (the tool runs inside the mod, no
  app). M8a above is the app-driven version that was built first.

## 8. Standalone: the curve tool inside the mod, no app (decided 2026-11-05)

The tools are meant to live in the game's editor and not need the app running. The app keeps its own curve tool;
the mod gets its own copy, ported to C. Everything M5-M7 built stays: the hooks, the drawing helpers, the input
handling, and M2/M3's height writes (called directly instead of over the pipe). The pipe stays for tests and the app.

**What gets ported** (the rules are already written and tested, so the port follows them line for line):
| from | what | to |
|---|---|---|
| `web/pen-path.js` (150 lines) | anchors and handles, Bezier, split, pull and move handles, corners, offset lines, spots along, nearest on path | `src/pen.c` |
| `web/curve-tool.js` (mouse, keys, drawing) | click for a corner, drag for smooth, drag points and handles, Alt corner, Ctrl delete, click the line to add, first point closes, last point resumes, Enter/Esc finish, pick another curve | `src/curvetool.c` |
| `web/gradient-overlay.js` (profile graph) | pick, insert, drag (granularity, delete past a neighbour), slopes, the graph at A | `curvetool.c` |
| `core/gradient.py` | `profile_heights`, `brush_weight`, `simplified_profile`, `sample_bilinear` | `src/curvemath.c` |
| `core/curved.py` | `sample_path`, `path_length`, `paint_curve` (with bank and window), extend/stretch, `points_from_ground` | `curvemath.c` |
| `core/curved.py` file I/O | `mx-tools-curves.json` beside the track's files, the same format, so the app and the game share curves | `src/curvefile.c` (the scene parser's reader, plus a writer) |

**How it's tested:** the ported files don't touch the game, so they build natively too. `tests/` gets a small
driver (`curvetest.exe`) that reads a curve and a ground block as JSON and prints the baked heights, path samples or
pen results. A pytest case builds it when MSVC is there (skipped otherwise) and compares it with the Python and JS
originals: baking within 1e-4 ft of `paint_curve`, the pen's edits identical to `pen-path.js` (through node). Then each
step below is checked in the game on ModTest.

**In the game:** our panel down the right edge is always there in the editor, only as tall as its rows, with **Curve
Path** and **Curve Profile** buttons in the game's style. Picking one turns our tool on (map clicks are ours); a game
mode button or number key turns it off, as in M7. The track folder comes from `--track` (under
`%LOCALAPPDATA%\MX Simulator`).

Steps, each with a check (ModTest only):
- **S1, panel and curves on the map (mod 0.8.0).** The panel with the two mode buttons; the track's
  `mx-tools-curves.json` read at start and drawn like the app draws them. Check: curves drawn in the app show in the
  game in the same place; the panel's buttons light and switch the tool on and off; with the tool off the editor is as
  before.
- **S2, the pen.** `pen.c` and the Curve Path mouse and keys. Check: `curvetest` matches `pen-path.js` on a script of
  edits; in the game, drawing and editing a curve behave like the app's (corner, smooth, handles, Alt, Ctrl, split,
  close, resume, Enter/Esc), and the result is saved to `mx-tools-curves.json` (atomically, keeping the file's other
  curves) when the game saves with `s` or from a Save row. The app opens it and shows the same curve.
- **S3, Curve Profile.** The profile graph at the curve's start, like the game's Gradient Edit graph, with granularity,
  Flatten and Set From Terrain (read from the game's own heights). Check: the same profile edits give the same points as
  the app's; the yellow dots show where each point falls on the curve.
- **S4, baking.** `curvemath.c`'s `paint_curve` on the game's heights: Live (preview, no undo step, put back when it
  stops) and Apply (one undo step), M3's rules. Check: `curvetest` within 1e-4 ft of Python; in the game, Apply then
  `game_link --track` shows the heights the app's Bake gives for the same curve; one Backspace removes it.
- **S5, the rest of the panel.** New Curve / Finish, Path Change (Extend/Stretch), Inner and Outer (sliders; typed values
  aren't possible in the game's widgets), Bank, Bake As, Strength, Live, Apply, Delete Curve, the Length and Tightest turn
  readouts. Check: everything in the app's curve panel can be done from the game alone.

Results:
- **S1 passed 2026-11-05 (mod 0.8.0):** the panel (top right, as tall as its rows) with Curve Path and Curve Profile in
  the game's style, lit on hover and when on; the track's curves (two test curves written with the app's
  `serialize_curves`) drawn where the app draws them; the tool on and off from the buttons, 7 or a mode button.
- **S2 passed 2026-11-05 (mod 0.9.0, `pen.c`, `curvetool.c`, `curvefile.c`, `json.c`):** drawing (corner, drag for
  smooth, the stretch to the mouse, Enter or Finish), picking, dragging points and handles, adding on the line, Alt
  corner, Ctrl delete, New Curve, Delete Curve, Save Curves (also on the game's `s`; written to a .tmp and swapped in).
  Checked natively first: `pen_test.c` against `curved.py` / `gradient.py` on the file (lengths within 4e-10 ft, profile
  heights the same, extend / cut / stretch identical, the written file reads back the same in Python).
  Unsaved edits here win over a file the app rewrites meanwhile (the status says so).
- **The mouse wheel:** the editor only zooms with `=` and `-` (0x1400652d0: only 0x3d and 0x2d change the zoom); turning
  the wheel codes into those keys didn't zoom either, so the wheel just goes to the game. Dropped.
  **Done another way in 0.35.1:** `=` / `-` only add or take 0.5 off the Zoom slider's value (1.0 0x655c8, 1684
  0xeb668; `-` clamps at its min, -3), so the mod subclasses the game's window (found with EnumThreadWindows on the game
  thread) for WM_MOUSEWHEEL alone and writes the slider itself (label-checked, `game_slider_set`), 0.25 a notch, moving
  the view centre so the ground under the cursor stays put. Only while the editor drew within the last 2 frames and the
  mouse is on the map (not either panel); the message is then swallowed, otherwise passed on. Not checked in the game
  yet. Also seen there: `]` / `[` (0x5d / 0x5b) are the game's Strength +/- keys in every mode (slider at 0x1778a0 on
  1.0); the decal tool takes them in Decals mode only.
- S3 (profile graph) skipped for now.
- **S4 passed 2026-11-05 (mod 0.10.0, `curvemath.c`):** Live Preview (no undo step, ground put back when it stops or the
  tool goes off) and Apply To Ground (one undo step), with M3's rules ported from `CurveInGame` (the ground under the
  curve saved once, every write checked against what was left, a changed ground stops the preview without writing).
  Heights go through `game_write_heights` in `game.c`, the same sequence SET_HEIGHTS uses. Applying the S-bend:
  the game's ground matched Python's `paint_curve` of the saved curve on the files to 9.5e-7 ft over the whole map
  (24,203 cells), and one Backspace put it back exactly (0 cells differ). Settings fixed until S5: inner 10, outer 20,
  strength 1, Paint. Checked natively first (`bake_test.c`): Paint matches to 5e-15 ft; Add with bank to 3e-8, except
  9 cells just outside a closed curve's sharp corner, equally far from two chords whose side signs differ, where the
  tie goes the other way (numpy's `u**3` against C's `u*u*u` in the chord ends). Both answers are right there.
- **S5 passed 2026-11-05 (mod 0.11.0):** the rest of the panel with the game's own widgets: Length and Tightest turn
  (readouts, "sides will fold" when Outer is wider than the tightest turn), Path Change (Extend / Stretch), Inner, Outer
  (inner can't pass outer), Bank, Strength (sliders, stepped like the game's: `min + round((max - min) * t / step) *
  step` along the bar the slider draws), Bake As (Paint / Add), SubMode (none / raise / lower), and Live re-previews when
  any of them changes. Checked in the game: everything works and looks like the game's.
  Later: Bank is -0.5..0.5 (0.39.2). A slider through zero snaps to 0 within 4 px of it (0.39.1), because 200 steps
  on a narrower bar left 0 between two pixels. Bake Side (0.40.0): Both / Left / Right. `curve_paint` skips cells with
  `side > inner` for Left and `side < -inner` for Right (0.40.1: the core between both inner lines stays, only the other side's fade goes), where side is the signed distance from `dx*oz - dz*ox` (positive =
  right of the chord, looking start to end on the map, z down the screen). Not tried in the game.
  Widgets, verified by disassembly: choice `0x14001ed10(ch, on, pos, scale, 20, 0, 0.5)`, struct {label, options,
  count @+0x10, selected @+0x14, rect @+0x18} (40 bytes); slider `0x14001f130(sl, on, pos, scale, 20, 0, 0.5)`, struct
  {label, double value, step, min, max, rect @+0x28, bar x0, x1 @+0x38} (64 bytes); readout `0x14001e830(text,
  *(ptr*)0x14023d560, *(ptr*)0x14023d550, 20, 0)`. Open question 2 answered: the mod draws with the game's widget
  functions and handles clicks and drags itself, so the game's handlers are never called on our structs.
- **S3 passed 2026-11-05 (mod 0.12.0, `profile.c`):** Curve Profile in the game: the graph at the curve's first point
  like the game's Gradient Edit (backdrop, 10 and 2.5 ft grid, outline, rings; `graphGeometry` / `paintProfileGraph`),
  pick / insert / drag with Granularity and delete past a neighbour (`pickPoint`, `insertPoint`, `dragPoint`), Segment
  Type, Flatten (and `f`), Set From Terrain from the game's own heights (any preview taken off first), Slope and Height
  readouts, the yellow dots and the mouse ring along the curve. Set From Terrain checked natively against
  `points_from_ground`: the same points and types, heights within 5e-12 ft. New scene kinds for it: segments, quad, ring.
- **Gradient Length for the game's own gradients (mod 0.13.0, built 2026-11-05):** in Gradient
  Placement our panel shows **Gradient Length: Locked / Extend / Stretch** (Locked = the game as it is). With Extend or
  Stretch, dragging B of a shaped gradient puts B under the mouse and refits its profile to the new length every frame
  (before the game draws and again at SwapBuffers), from the profile as it was when the drag began, with `profile_fit`
  (`extended_points` / `stretched_points`). Untouched gradients are left to the game. The drag is recognised from
  mode 9, drag state `0x1417e2608 == 2` (B) and `0x1417e25e4 == 1` (button held on the map). Extend can need one more
  profile point than the gradient has room for; then the status line says so and Stretch still works.
- **Gradient Length checked in the game 2026-11-05: works.**
- **Profile sections (mod 0.14.0, `library.c`, built 2026-11-05):** a library of profile pieces (a 3 ft
  jump, a table top...) kept across tracks in `%LOCALAPPDATA%\mxtools-mod\profile-sections.json`. In the game's Gradient
  Edit (mode 10, the selected gradient) and in our Curve Profile: Mark Start and Mark End on picked points, Save Section
  (kept relative to its first point, named from its shape: "3.0 ft high, 24 ft long"), Section (choose), Paste At Point
  (its first point on the picked point at that point's height; the points it covers replaced; the profile's length
  unchanged), Delete Section. A game gradient only takes what fits in its room (16 or 32 points); curves hold 128.
- **Sections, mod 0.15.0 (changes asked for after trying 0.14.0):** the Section row is a dropdown (the chosen name across the
  whole row; a click lists every saved section under it, a click on one picks it), and **Rename Section** types a name
  in the game: while typing, every key is the mod's (characters only, not the raw 0x10xx copies; Enter keeps it, Esc
  cancels, Backspace deletes), and typing ends whenever its row isn't on screen, so the keyboard always comes back.
  New automatic names are shorter ("2.7 ft high, 34 ft") so they fit a row. The panel holds 32 rows, so a very long
  list gets cut off.
- **Brush shapes (mod 0.16.0, built 2026-11-05):** in the game's brush modes (0..8) our panel shows
  Brush Shape (Circle / Square / Oval), Brush Angle and Oval Width. The game's brush is `0x14005fb10(x0, z0, x1, z1,
  outer, inner, strength)` (from the drag step at 0x140066fa0), one call per stroke step: it walks the cells around the
  segment and, for every mode but Smooth (mode 2 has its own loop at 0x140060cd4), all three cases (beyond the end, the
  middle, before the start) meet at 0x140060035 with the offset to the stroke in xmm2 (x) and xmm0 (z), then square, add
  and `sqrtsd` to the distance at 0x14006005a, which the inner/outer smoothstep and the mode's jump table use. Nothing else
  jumps into those 37 bytes. While a shape is chosen they jump to `brush_stub.asm` (does the covered integer work, saves
  the volatile registers, calls `mx_brush_distance(dz, dx, outer, inner)`, returns with xmm2 squared as the game left it);
  Circle puts the original bytes back. Square: the larger rotated offset (a turned square is drawn inside the brush's
  round reach, since the game only visits cells within outer of the stroke); Oval: one axis divided by Oval Width. The
  stroke's capsule still uses the nearest point by the game's measure, so a fast drag sweeps the shape a little unevenly.
  The mod draws the shape's outline at the mouse once a stroke has told it the radii; the game still draws its circle.
- **Triangle brush (mod 0.32.0, proportions 0.33.0):** Brush Shape also has Triangle, with Triangle Width (base over
  height) and Triangle Lean (the tip along the base; -1 or 1 is a right angle, and the two are mirror images, which no
  rotation can swap). The distance is the largest of three edge measures (`tri_shape` in `ingame.c`), each normal / its
  distance from the centre. The centre is the centroid and the farthest corner is on the outer radius, so the shape stays
  inside the brush's round reach at any angle; an equilateral covers about 41% of the circle's area. Brush Angle runs to
  360 because a lopsided triangle differs after a half turn. It is the one shape that isn't the same at the negated
  offset, so it relies on the loop's offset being cell minus stroke in all three cases. Checked by disassembly on all three builds (in 1.0, 0x14006001d and 0x1400606e0);
  check it again when porting a new build.
- **No more point limits (mod 0.18.0):** the game's realloc is `0x14000bbc0(old, size)` (the CRT's realloc plus the
  game's own allocation tracking for `MX_CHECK_ALLOC`), and the game grows a full gradient vector by doubling its capacity
  and reallocating capacity * stride (0x14005d0c0). `grow_vector` in `game.c` does the same, so paste, Extend and
  `set_gradient` grow a gradient's points and types instead of refusing past 16 or 32. Curves hold 1024 path points and
  2048 profile points.
- **Sharing (0.17.0):** the DLL needs only Windows' own `kernel32`, `user32` and `bcrypt` (the C runtime is built in).
  Its files moved to `%LOCALAPPDATA%\MX Simulator\mxtools` (log, `profile-sections.json`, copied over once from the old
  `%LOCALAPPDATA%\mxtools-mod`): the game's data folder, always writable, and the game only lists folders with a
  `terrain.hf` as tracks (0x1400e59f0), so it doesn't show up. `package.ps1` builds `dist\mxtools-editor-mod-<v>.zip`
  with the DLL, `install.bat` / `uninstall.bat` (ask for admin, keep MXS Shaders as `opengl32_mxs.dll`, put it back;
  `goto` instead of `( )` blocks since the path has `(x86)`, CRLF) and a README for whoever gets it.
- **Any track, no `--track` needed (mod 0.19.0):** the open track's folder name is the `char *` at +0x28 of the window
  struct (`*(ptr*)0x14023d7e0`); the game's own saves build `"@" + it + "/edinfo"` (0x14005f243) and `"/terrain.hf"`
  (0x140018dce), "@" being `%LOCALAPPDATA%\MX Simulator`. The curve tool reads it about once a second and follows the
  track the editor has open (saving any unsaved curves to the old track first); `--track` is only the fallback. `hello`
  reports it as `track.name`, which `game_track_name` prefers.
- **An unknown game version says so (mod 0.20.0):** in an `--editor` session on a build the mod doesn't know, it still
  does nothing to the game, and now shows a message box once (from its own thread, so the game keeps drawing):
  "This version of MX Simulator isn't supported by the MX Simulator Tools editor mod yet...". `MXMOD_PRETEND_UNKNOWN=1`
  makes it act that way on the known build, to see it. A game update means a new build: the addresses have to be checked
  again and `KNOWN_SHA256` updated.
- **Build 1684's brush (0xe55c0, read 2026-11-10):** one loop for every mode. Smooth (mode 2) builds its blur kernel
  at 0xe6784 and rejoins the main loop at 0xe58f1; its jump-table case (0xe5ff8, table 0x2318b0) averages the kernel and
  lands on 0xe5b9e like Raise, so the patched distance (0xe5ae5) shapes Raise, Lower, Smooth and the rest. A second copy
  of the loop (0xe61f0, own sqrt at 0xe62c4, table 0x2318d4 acting only for modes 2 and 3) is reached only when the
  first column is negative, which the clamp at 0xe5713 rules out. `smooth_shaped` in builds.c marks the builds where
  Smooth is shaped (1684), for the panel note and the outline.
- **Not done:** undo of curve edits in the game (the app has it); typed widths (sliders instead); a shaped Smooth on
  1.0 and 1681.

## 9. Gradient tools and the panel's look (mod 0.25.0, built 2026-11-05)

Requested by track builders. `src/gradtool.c` owns these rows; they show while the game's own tools are in use.

**The panel (decided 2026-11-05):** it doesn't have to copy the old editor screen; readable and easy to
use comes first. It still draws with the game's widgets, but rows now come in groups under a gold header (CURVES, BRUSH
SHAPE, GRADIENT 12 OF 93, SAVED GRADIENTS, RADIAL SWEEP, PROFILE SECTIONS), with grey hint lines that say what to do
next ("Click a gradient's line to pick it", "Click the map where A goes"). Both are `readout(text, text_rgba, box_rgba,
min_width, align)` (0x14001e830, verified: the box colour is the third argument, `align` a float, 0 left / 1 right)
with a clear box. The panel holds 40 rows (1 + 40 x 1.5 = 61 of the window's 64 units).

**Delete Gradient (button, or Del in Placement and Edit) and Undo Delete:**
- New Gradient (0x14005d160) mallocs a fresh points vector (16 x 8 bytes) and types vector (16) with the game's malloc
  (0x14000b9c0), builds the record and copies it onto `array[count++]`, doubling the array through the realloc when
  full (0x14005d220). Nothing reuses or shares vectors, so records can slide. Delete: free both vectors with the game's
  free (0x14000bce0: drops the pointer from its allocation tracking, then the CRT free; 286 callers), slide the rest
  down, zero the slot past the end, count - 1, selection cleared. Refused while a button is held or the track is
  locked. The ground the gradient shaped stays (heights don't know gradients).
- **A gradient must stay selected while any are left** (found in 0.25.0, fixed in 0.25.1): Placement's
  click starts from the selected gradient and returns at once when it is -1 (0x1400eb079 on 1684: `js` to the
  handler's end, before the loop that picks the gradient nearest the mouse at 0x1400ebd2f), and Gradient Edit's does
  the same (0x1400eae2b). The game never sets -1 itself while gradients exist. So a delete selects the gradient that
  took its place, or the new last one; -1 only when none are left.
- The game's Backspace only holds heightmaps, so Undo Delete is ours: the last 16 deletions on the open track, put
  back newest first at their old numbers (Del twice deletes two, since the next one is selected).
- Del (0x7f) and `x` are only acted on in Statues (13) and Decals (15) (key handler 0x1400eb459 / 0x1400ec1e3 on 1684),
  so Del is free in modes 9 and 10. Each key also arrives as a raw 0x10xx code, which still goes to the game.

**Export Gradient, Saved Gradients:** one click writes the picked gradient to
`%LOCALAPPDATA%\MX Simulator\mxtools\gradients\<track> #<n>, <length> ft.json` (never over an older file: " (2)" ...):
`{"format": "mxtools-gradient", "version": 1, "track", "gradient", "a": [x, z], "b": [x, z], "points": [{"type",
"s", "h"}]}`. The list shows the folder newest first (read again every 2 s in modes 9 and 10, so files copied in show
up). **Place On Map**: the next click on the map puts A there with the same direction and length (a gold line follows
the mouse; Esc or Cancel stops; pan, zoom and mode keys still work). **Put Back Where It Was** adds it at its saved
spot, unless a gradient with the same ends and profile is already there (0.25.2: pressed twice in testing, it
stacked an invisible copy), which it names instead. A placed gradient is added at the end and selected. **Open Gradients Folder** starts Explorer on it.

**Radial Sweep (modes 7 and 8):** the game's radial helper (0x14005fa10, `float(float x, float z)`): `v = cell - A`; if
`dot(v, B - A) < 0`, v loses its part along A to B; then it tail-calls 0x14005f820 (`float(float s)`, the selected
gradient's profile height at s). That is a 180 degree sweep centred on B that runs straight past its edges. Our rule,
`sweep_s` in `src/sweep.c`: inside the wedge `s = |v|`; outside, `s` = v's length along the nearer edge, 0 if that is
negative. At 180 centred it is the game's rule exactly (`tests/test_sweep.py`, 4,000 random cells). Sweep 30 to 360
in 15s; From B: Both Sides (centred), Clockwise or Counter-CW (on screen; z runs down it, so a growing angle turns
clockwise). The wedge is drawn from A out to the profile's length, with short faint lines where it runs straight.
While the sweep isn't the game's own, the helper's first 14 bytes (`mov eax, [rip+selected]`, `test`, `js`, three
whole instructions) jump to `radial_height` in `ingame.c`, which reads the gradient as the original does and calls
0x14005f820. The bytes are checked first (opcodes, and the first read must be the selected gradient) and put back at
180 centred and when the DLL unloads.

**Berm Preview (0.34.0, `src/bermpreview.c`):** a button in Placement and Edit that shows what Radial Add would do with
the picked gradient, painted on the real ground with no undo step, using the Sweep rows (shown while it's on). Each cell:
`ground + weight * h(s)`, with `s` from `sweep_s` and `weight` from `sweep_weight`: 1 across the wedge out to the
profile's end, then the brush's smoothstep over Outer minus Inner. It ignores Strength (see the brush section of
`game/track-editor-gradients.md`) and the game's SubMode, whose address isn't in the build tables. Leaving Placement and
Edit ends it, so a stroke can't land on it. It updates when the mouse is let go, because `game_write_heights` refuses
while the game's drag holds it. It puts the track's min and max height back when it comes off; curve Live and the stamp
don't yet.

**Previews and saving (0.34.0):** the game's save (0x140018d90) has one caller, 0x14005bcb0, which is called only from
the `s` key (0x140065c31) and the panel button handler (0x14005e14d), and both run inside the key handler we hook. So
`key_detour` takes every preview off before the game sees `s`, `p`, a number key, or any key or click with a game widget
hovered; that also covers Set From Terrain. If one can't come off (a held mouse button, the edit lock), the key is
swallowed, because the track file must never get a preview. Addresses are build 1.0's.

**Profile sections, reworded (0.25.1, feedback: "really hard to understand for someone who doesn't know what it does"):**
two groups. SAVE A PROFILE SECTION says what a section is, then one numbered step at a time ("1. Click where it
starts on the graph", "2. Click where it ends", "3. Save it"), with buttons that say what they do ("Use Point As
Section Start", "Start: point 3 (change)", Clear Start And End). The marks are drawn on the graph: a green ring at the
start, an orange one at the end, the piece between in green. YOUR SECTIONS shows the chosen one as a dropdown
("3.0 ft up, 24 ft  v", "^" when open, "> " before the chosen one in the list) with "Click the name to pick another",
then Paste At Picked Point, Rename, Delete.

Checks to run in the game (ModTest):
1. Placement: pick a gradient, Delete (and Del): it goes, the ground stays; Undo Delete brings it back with its
   profile; `s`, reload: `edinfo` has the list as shown.
2. Export, then Place On Map on another spot and Put Back; both look like the original in Gradient Edit.
3. Radial Paint at 90 Clockwise on a berm: only that quarter follows the profile, straight past its edges; back at 180
   Both Sides it paints like the game (log: "radial sweep off").

Size: roughly 1,500 lines of C across the steps; S1 and S2 are the largest. Open question 2 (calling the game's widget
handlers on our structs, or copying their rules) gets answered in S1 for buttons and S5 for sliders.

## 7. Open questions (answer while building)
1. ~~The exact args of `0x14005bb60` and `0x14001e190` and the panel's translate/scale (disassemble 0x140061b40..0x140061d8f).~~
   Answered by disassembly (2026-11-05): `bb60(x, y, z)`, `e190(pos, scale, dx, dy)`, panel space as in section 2.
2. Whether calling the game's widget handlers (`0x14001fad0 / fb40 / fd80`) on our structs is safe, or simpler to copy
   their rules (they may read `0x1417e25e4` or the game's widget table).
3. Whether the main loop calls `0x1400645d0` for mouse wheel or other buttons we should pass through. Partly answered in
   M7: codes 0x100c..0x100e arrive as down/up pairs (wheel or middle button?), and every key arrives as a raw 0x10xx code
   and then its character.
4. Mouse move frequency: `0x1433e40e0` is written in the message loop (0x140170a46..0x140170ac1); one sample per frame
   should be enough, but check fast drags don't skip.
5. Right-hand panel versus the screen width at 4:3 (21 p each side leaves at least 24 p of map at 16:9; check small
   windows).
6. **The tripwire.** Done in 0.21.0: the `sendto` import entry is swapped before any hook goes in, and every packet
   is dropped (this PC included, since 0.23.1) with a one-time warning (details in docs/design.md rule 2).

## 10. Duplicate Curve and moving a range of profile points (mod 0.27.0, built 2026-11-06)

- **Duplicate Curve** (Curve Path, a curve selected): a copy 10 ft to the side, the way the game's Copy Gradient offsets
  its copy (`10 * (dz, -dx) / |AB|`, with AB the curve's first segment), selected so it can be dragged into place.
- **A range of points, moved as one**, on the game's Gradient Edit graph and on our Curve Profile graph: Shift+click a
  point to start the range, Shift+click another to end it, and every point from one to the other is in it (gold rings
  and the profile between them in gold). A plain click on a point in the range drags them all
  (`profile_group_drag`, `profile.c`); a plain click anywhere else, Esc or Clear Selection lets go of it, and so does any
  added or deleted point. The held point goes under the mouse; the rest move by the same amount. The range stops just
  short (1/64 ft) of its unselected neighbours instead of passing them, and with an end of the profile in it, it only
  moves up and down.
- On the game's graph only those clicks are taken (`curvetool_graph_down` from the key handler, with our tool off and
  the game in mode 10); every other click goes to the game. The drag rewrites the gradient's profile each frame with
  `game_set_profile`, the same point count, so nothing is reallocated.
- Not done: the game's own Granularity isn't read (its address isn't known), so on a game gradient the range moves
  without snapping; on a curve our Granularity applies. Not checked in the game yet: whether Shift+click does anything
  of the game's own in Gradient Edit.

## 11. Brush size in feet and presets (mod 0.28.0, built 2026-11-06)

- The curve tool's Inner slider now tops out at 50 and Outer at 100 (finer steps for the small sizes used most).
- **The game brush's size in feet.** In the game's brush modes our BRUSH group has Outer (ft) and Inner (ft). They set
  the game's own Radius slider (screen px: outer ft = Radius * viewFt / screenH) and Focus (inner / outer) every frame,
  so the size on the ground stays put while zooming (`brush_size_frame`). Moving the game's own Radius or Focus (by
  more than its step's rounding) lets go of the hold. Addresses and how they were found: `docs/builds.md`.
  **0.29.1:** the slider's 1..128 px range broke the hold a couple of zoom steps in (a 50 ft brush needs ~150 px at
  zoom 2.5). The hold now writes Radius past 128 (`game_slider_set_unclamped`), kept to the feet the game's own
  1..128 px reach at some zoom (Zoom slider -3..8, step 0.125). Safe because (1684 disassembly): the brush stroke
  (0x1400ed97b), cursor ring and `Actual Radius` readout (0x1400e7a83) all read Radius fresh as
  `Radius * mapFt / (screenH * 2^zoom)`, the brush gets outer/inner as feet arguments, the slider draw (0x1400f9170)
  clamps its fill fraction to 0..1 and never writes the value, and only the click handler for widget 0x15
  (0x1400e3981) touches the struct, which also lets go of the hold.
- **Presets** (`presets.c`, `%LOCALAPPDATA%\MX Simulator\mxtools\presets.json`), two lists:
  - CURVE PRESETS (with a curve selected): Inner, Outer, Bank, Strength, Bake As, SubMode.
  - BRUSH PRESETS (the game's brush modes): outer and inner ft, Brush Shape, Angle, Oval Width, Sweep and From B.
  Save As New Preset names it from its values; Rename Preset types a name (the same typing as Rename Section); a
  click on a preset in the dropdown uses it. A field missing from the file leaves that setting as it is.
- The range drag on a game gradient now snaps with the game's own Granularity slider.
- **Moving a whole curve (0.29.0):** in Curve Path, Shift+drag on any part of a curve (its line, a point or a handle)
  moves every point and handle by the mouse's offset from the press (`DRAG_MOVE`, from a copy of the points taken at the
  press, so it doesn't drift). A curve that isn't selected is picked by the same press. Not while drawing a new curve
  (clicks add points then). The profile keeps its length, since a move doesn't change the path; Live Preview follows.

## 12. Rut stamps (mod 0.30.0, built 2026-11-06; shelved in 0.34.1)

**Shelved (0.34.1):** not finished, so the Rut Stamp button is hidden and `curvetool_set_mode` refuses the stamp mode
unless the DLL is built with `MXMOD_STAMP=1` (`curvetool.h`). `stamptool.c` is still compiled so it keeps building.

`src/stamptool.c`, the **Rut Stamp** button under Curve Profile (`CURVE_MODE_STAMP`; the curve tool routes the panel,
mouse, keys, frame and scene to it, row kinds from `STAMPTOOL_ROW` = 200, checked before `GRADTOOL_ROW`).

- **The stamp** is a height image added to the ground under it, so it fits any terrain. Grey is a share of Depth:
  Flat Is = Mid Grey: `2g - 1`; White: `g - 1`; Black: `g`. Times alpha (transparent = no change). Image x runs along
  Length, image y across Width; at Angle 0 it's as it looks in the image editor (x right, z down the screen).
- **Images** come from `%LOCALAPPDATA%\MX Simulator\mxtools\stamps` (PNG, TIFF, BMP; up to 4096x4096), read with WIC.
  ole32 (CoInitializeEx / CoCreateInstance) and shell32 (ShellExecuteW for Open Stamps Folder) are loaded with
  LoadLibrary when first used, so the DLL's imports didn't change. A 16-bit grey PNG is read as is; 48/64-bit RGB(A) is
  converted to 64bppRGBA (same, linear, family); anything else to 32bppRGBA. Never 8-bit to a 16-bit WIC format: WIC
  treats those as linear and would gamma-bend the greys. Grey = RGB average. Picking a file stamp keeps Length and
  sets Width from the image's proportions. "Built-in Rut" (a trough with a low berm each side) needs no file.
- **Painting** (`paint`): each cell's centre (x = col * cell, z = row * cell) in the stamp's own axes, bilinear, k x k
  samples a cell (k = image px per cell, 1..6) so a big image isn't read as noise. End Fade and Side Fade (ft) are a
  smoothstep from each edge in.
- **Preview** is the curve tool's Live, on floats (`pv_*`): the ground under it is kept, each change is painted from
  that copy, the last preview's cells are put back. Retried only when a setting changes (also after a failed write).
  If the ground under it changed (the game's Backspace undo), it starts again from the ground as it is now.
  Apply (or Enter) = one undo snapshot, then the stamp is no longer placed (the next click places a new copy).
- **Save:** `s` takes the preview off first (`stamptool_before_save`, from the key detour before the game sees the key)
  and waits 30 frames before showing it again, so the track file never gets a preview in it. Since 0.34.0 the same
  check covers every preview (stamp, curve Live, Berm Preview) and the game's Save button, not just `s`; see Berm Preview.
- **Mouse and keys:** click places it (and starts a move drag); a press on it (10 px slack) moves it; Ctrl+drag turns
  it (whole degrees); `[` `]` turn 5 degrees; Esc / Del lift it (ground put back); Enter applies.
- Checked outside the game (a test harness including stamptool.c): 16-bit PNG greys exact, 8-bit greys not
  gamma-shifted (200 -> 51400), alpha read, missing file refused, built-in paints -0.85 / +0.12 ft at Depth 1 on 1 ft
  cells. Not yet checked in the game.

## 14. Decal editing in Decals mode (mod 0.35.0, built 2026-11-09, 1684 only)

`src/decaltool.c` (the tool, the maths), the decal section of `src/game.c` (the game's calls, index fix-ups, checks),
rows from `DECALTOOL_ROW` = 300 (checked before the stamp's 200). Everything about the game's side is in
`game/decals.md`; the axes are its section 1 (width = size * f5 along (cos, -sin), height = size along (sin, cos)).

- **Picking** is the game's own (0.47.0): its centre dot or rotate dot. Until 0.46 the mod picked the smallest decal
  whose box held the mouse, but that took every click inside a big decal, so it was dropped at Jackson's request. Every
  press off the selection's handles goes to the game (its pick, its move with the group, its rotate, stamping). The
  decal a click would pick (the game's dot rule, `dot_at`) is outlined faintly, and a new selection shows its name.
- **Handles on the selection** (gold outline, screen-px sized): corners scale about the centre (f5 kept; Shift: whole
  feet of height); side handles stretch with the opposite side fixed (Alt: about the centre; Shift: whole feet), hidden
  when that side is under 40 px. Since 0.48.0 a press anywhere along an edge (within 7 px of it) stretches that side too,
  drawn thick while hovered, unless a decal's dot is under the mouse (the dot stays the game's); the ring past the top edge turns it (relative to where it was grabbed; Shift: 15
  degree steps). Each mouse move calls 0xcf70 with a record built from the decal as it was at the press, with sin and
  cos computed from the new rot. One undo step per drag.
- **Keys** (only while a decal is picked; mode 15; our tool off): `d` is the game's own copy (0.35.0 had a flush copy
  on the right in its place; removed in 0.35.2 at Jackson's request, the game's is the one wanted); `[` `]` 10% smaller
  or bigger; `{` `}`
  (Shift) 1%, but only while the game's group is empty, since with a group they are the game's spread and pull;
  Ctrl+Z (char 26, or `z` with Ctrl held) undoes. Buttons: Bigger, Smaller, Delete Decal, Undo. (The game's own copies
  aren't on our undo; its copy keeps working with ours, since our undo finds a decal by its bytes, not its index.)
- **Undo:** our own, 64 steps per track: the 72-byte record before and after, the index and the op. An entry applies
  only while the decal it left is still there, found by its bytes (at its index first, so the game's own inserts and
  deletes moving it are fine); one the game changed since is dropped with a message.
- **The game's rotate drag** writes the previous move's sin and cos; each frame in mode 15 the selection's sin and cos
  are put right with 0xcf70 when they're off by more than 1e-5 (the one write allowed while the game holds the button).
- **Refused** while the edit lock (terrain+0x49574) is set or the game holds the mouse button. Saving is the game's own
  Save; the panel says it rewrites the decals file whole and drops lines it can't read.
- On 1.0 and 1681 the decal addresses are 0, so the panel says "Not ported to this game build yet" and nothing is taken.
- **Checked outside the game** (a test including decaltool.c with the game's calls stood in by an array, 40 checks):
  corners against the game's quad table (0x3060) at f5 = 2, width = size * f5, both stretches keep the opposite side's
  corners, Alt keeps the centre, a corner at 1.5x gives size 1.5x with f5 kept, the turn handle reads rot by the game's
  own rotate rule, Shift snaps to 15 degrees, D's copy's left corners sit on the original's right corners, undo of
  scale / copy / delete, smallest-area picking, `}` left to the game with a group, a stale undo dropped untouched.
- **Not checked in the game yet** (the brief's steps, on ModTest): 1. which image edge is v = 0 (a lopsided picture at
  f5 = 2); 2. the outline sits on the decal at f5 = 1 and 2; 3. turning and corner scaling re-bake the ground smoothly
  (per-move cost) and match the app for the same numbers; 4. side stretch; 5. D and undo; also that Ctrl+Z arrives as
  char 26 or `z`, and that the game draws nothing odd with its selection set by us. Step 6 (pipe messages) not done.

## 13. Set From Terrain that keeps sharp edges (mod 0.31.0, built 2026-11-08)

The game's method rounds off lips: one sample per cell, then six passes that drop points by comparing each to a straight
line through its neighbours (tolerance up to 1/4 ft), never to the ground, so the error adds up. Profiles on test
ground with 5-6 ft jumps came out up to 6.2 ft wrong. `profile_trace_ground` (`profile.c`) replaces the port of that method on the curve
tool's button, and **Set From Terrain (Sharp)** (TRACE THE GROUND, in the game's Gradient Edit) runs it along the selected
gradient's line. The game's own button is untouched. **Terrain Fit (ft)**, default 0.05, is shared by both.

- **Why Lines:** a Curve is always tangent to its neighbour at a shared point (the slope rules in
  `game/track-editor-gradients.md`), so only a point with a Line on both sides can hold a kink.
- **Steps:** read the ground every cell/4; find edges (slope one cell each side changes by 0.25 or more, peak within a
  cell); give each a Line half a cell down each face, held in place; collapse straight runs (the game's first step);
  then remove a point only if the rebuilt curve stays within Terrain Fit of every ground sample it can change.
- **Too many points for a profile:** loosen up to about 0.5 ft, then let go of the edges and loosen further. The status
  line always gives the real worst distance.
- Worst can pass Terrain Fit near an edge: held points aren't removed or moved, and a grid peak can fall between samples.
- Checked outside the game (old vs new driver, synthetic ground, 2 ft cells, Fit 0.05): worst error 0.6-6.2 ft (0.6-3.5 ft within 4 ft of a lip or edge)
  became 0.05-0.25 ft, points 1.2-2x (34-71 vs 19-51). Flat ground, short paths, off-map paths and pure noise all
  give valid profiles; a zero-length path is refused. Not yet checked in the game.

## 15. Folding groups and scrolling lists (mod 0.37.0, built 2026-11-12)

- `curvetool_panel` builds every row into `g_all` (BUILD_ROWS = 160, kinds and args sized the same), then `fold_panel`
  drops the rows under folded headers and packs rows, kinds and args down to the 40 shown. So folding frees rows.
- A header (ROWK_HEADER with text) is clickable: `[-]` open, `[+]` folded, keyed by its text with digits taken out
  ("GRADIENT 12 OF 93" = "GRADIENT 3 OF 93"). Kept in `mxtools\panel-folds.txt`, one key a line.
- Headers have no widget rect, so `row_at_mouse` (ingame.c) finds a row by layout: row i's widget spans 0.5 + 1.5 i ..
  1.5 + 1.5 i units down (M5's button measured 11..33 px at 22.5 px a unit). Not yet checked against a real rect.
- Lists (sections, both preset lists, saved gradients) show PANEL_LIST_SHOW = 8 from `g_list_first[list]`, with
  PANEL_LIST_UP / PANEL_LIST_DOWN rows (kinds 9000 / 9001, handled in `curvetool_press` before any tool's range) that
  page by 4. Rows carry `panel_row.list`; the wheel over our panel scrolls the list under the mouse instead of zooming.

## 16. Undo and remembered settings (mod 0.38.0, built 2026-11-12)

- **Curve undo** (`curvetool.c`, the undo section before `curvetool_press`): `act_begin` / `act_end` wrap a map press
  to its release (`curvetool_mouse`), each panel press and each key. The outermost begin takes a compact copy (count,
  selection, each curve's header and its anchors and points in use); the end keeps it on the undo list only if the
  curves differ beyond the selection. Ctrl+Z (char 26, or `z` with Ctrl held) / Ctrl+Y (25 or `y`) swap copies; `load()`
  clears both lists. Checked natively (`undo_test.c` in the session scratchpad, 13 checks); not in the game yet.
- **Undo Delete Section / Preset:** `library_insert` / `preset_insert` put the last 16 back at their old index.
- **Settings:** `curve-settings.txt` ("name value" lines), read in `curvetool_start`, written 30 frames after the
  values stop changing.
- **Live Preview and the height range:** the first `live_cover` keeps `game_height_range`; each write records the range
  after it; taking the preview off (cancel, before a save) sets it back, keeping any widening past what our last write
  left (the game's own brush). Apply sets it back before its final write. The stamp (shelved) still doesn't.

## 17. Ground grid and moving a whole gradient (mod 0.39.0, built 2026-11-12)

- **Grid** (`grid_scene`, `curvetool.c`): drawn first in `curvetool_scene` as two SCENE_SEGMENTS items (minor alpha 0.13,
  major 0.3), clipped to 0..(width - 1) * cell_ft and to the screen from the game's panel edge (21.25 units). Spacing
  multiplied by 2, 5, 10... until lines are 8 px apart. `g_grid` is kept in `curve-settings.txt` ("grid", 0..5).
- **Whole gradient move** (`gradtool.c`): `gradtool_down` from the key detour, Placement only (in Gradient Edit,
  Shift+click is the range select on the graph). Nearest line within 8 px, the selected one winning a tie. Each move
  writes start ends + mouse offset through `game_gradient_set_ends` (game.c: `list_editable` checks, then ax, az, bx, bz
  in place); the game never saw the press, so its mouse flag stays clear. Undo finds the gradient by the ends the move
  left, not its index. Not yet checked: whether Shift+click does anything of the game's own in Placement.
