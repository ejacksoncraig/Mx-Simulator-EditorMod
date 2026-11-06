# In-game editor: how gradients look and behave (render + UI spec)

*A copy of MX Simulator Tools' `docs/editor-gradients-ui.md` (2026-11-05). The game facts are the same in both:
when one changes, change the other. Mentions of the app's files (`web/`, `core/`) are that repo's.*

Target: mx.exe SHA-256 54676784e8f2e87f9043d8e97b6c421402af89abeebb52cee00972703bada392, Ghidra 12.1.4 via REA, Nov 4 2026.
All facts **(verified)** from the exe unless tagged. Complements the math in `docs/game/track-editor-gradients.md`
(profile Hermite, linear/radial `s`, brush smoothstep), which is unchanged.

## 0. View and units
- The editor is a **top-down orthographic plan view**. World X = screen right, world Z = screen **down**
  (mouse Y grows down and maps to +Z: `z = (mouseY - H/2)/H * viewFt + centerZ`).
- `viewFt = mapSizeFt / 2^zoom` = world feet shown over the screen **height** (terrain+0x10c8 / 2^zoom).
- **u = max(viewFt * 0.01, 1.0)**: the editor's marker unit, 1% of screen height, never below 1 ft.
  Every handle, cross and pick radius is a multiple of u, so handles stay the same on-screen size at any zoom.
- Colors are RGBA, drawn with alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), no depth test.

## 1. Primitives the editor uses
| name | addr | what it draws |
|---|---|---|
| ring(x,z,r,w) | 0x14005b870 | filled annulus, inner radius r - w/2, outer r + w/2, 64 segments (QUAD_STRIP) |
| circle(x,z,r) | 0x14005bee0 | outline circle, 64 segments (LINE_LOOP) |
| arc(cx,cz,r,a0,a1) | 0x14005b9e0 | arc vertices every 2π/64 (used for the brush capsule) |

## 2. Each gradient on the map (modes 5-10: all gradient paint/add modes, Placement, Edit) — 0x14005c170
Let A=(ax,az), B=(bx,bz). "Hot" = this gradient is selected **and** mode is Gradient Placement (9).
1. Lines (GL_LINES), color **white α1 if hot, else white α0.25**:
   - A → B
   - cross at A: (ax-2u, az)→(ax+2u, az) and (ax, az-2u)→(ax, az+2u)
2. Endpoint handles at A and at B, radius **0.5u**:
   - outline ring width **0.375u**: black α1 if hot, else black α0.25
   - fill ring width **0.25u** on top: white α1 if hot, else white α0.25
3. Only for the selected gradient (any of these modes): **circle around A**, white α0.25, radius **|AB|**.
   In Gradient Edit with a profile point selected, the radius is that point's **s** instead (shows where
   that distance lands on the map, which for a radial gradient is exactly the ring at that height).
- Unselected gradients are drawn the same way, just faint. In Edit mode the endpoints are never "hot".
- The A cross marks the start (s = 0). There is no arrowhead; direction is A→B.

## 3. Gradient Edit (mode 10): the profile graph — inside 0x1400613b0
Drawn only for the selected gradient, after all gradients. It is drawn **on the map, 1:1 in feet, anchored at A,
axis-aligned (NOT rotated to the AB direction)**: graph x = ax + s, graph z = az - h (height goes up on screen).
Let pts = (s_i, h_i), s0 = first s, sN = last s.
- Vertical extent in 10 ft bands: lo = min(0, min floor(h_i/10)), hi = max(10, max ceil(h_i/10)).
  top z = -10·hi, bottom z = -10·lo (relative to az). So the graph is always at least 0..100 ft tall.
Draw order:
1. Background quad s0..sN × bottom..top, **white α0.25**.
2. Major grid **black α0.25**: horizontal lines at h = 10·i for i = lo..hi; vertical lines at s = s0 + 10·j
   while 10·j ≤ sN - s0. Lines span the whole background.
3. Minor grid **black α0.0625**: horizontal at h = 2.5·i for i = lo..4·hi; vertical at s = s0 + 2.5·j.
   (Quirk: the minor horizontal loop starts at i = lo, not 4·lo, so below 0 ft it only covers a quarter of
   the negative range. Harmless; copy or fix.)
4. Profile outline, **black α1, LINE_LOOP**: (s0, 0) → each point; Curve segments are sampled every
   **0.25 ft** of s with the Hermite profile, Line segments emit only their start point → (sN, 0) → closes
   back along h = 0. Result: a closed silhouette of the jump between the profile and the 0 ft baseline.
5. Point handles, radius 0.5u: outline ring width 0.375u then fill ring width 0.25u.
   - selected point: **black outline, white fill**
   - other points: **grey (0.5,0.5,0.5) outline, black fill**

## 4. Side panel (left column, 20 px rows; labels from 0x140224a00 strings)
Always: 16 mode buttons (Raise … Decals), SubMode toggle (none / Raise Only / Lower Only), Save, Zoom,
Strength, Focus, Radius, then readouts `Actual Radius: R (focus·R)` and `Actual Strength: 2^strength`.
Gradient Placement (9) adds: **New Gradient**, **Copy Gradient**, readout `Length: |AB|`.
Gradient Edit (10) adds: **Segment Type** (Line/Curve toggle), **Granularity** slider, **Set From Terrain**,
**Flatten**, readouts `Slope: m` (tangent at the selected point; if that point's segment is a Line it shows
the next point's tangent) and `Height: h`.
Bottom of panel: cursor `x y z` (y = ground height under the cursor) and the last status message.

## 5. Sliders (value, step, min, max) — struct {label*, double value, step, min, max}
| slider | default | step | range | meaning |
|---|---|---|---|---|
| Radius | 64 | 1 | 1..128 | brush radius in **screen pixels**; R ft = Radius · viewFt / screenH |
| Focus | 0.5 | 1/32 | 0..1 | inner (full-strength) radius = Focus · R |
| Strength | 3 | 0.25 | -5..5 | exponent; actual = 2^Strength (brush clamps to 0..1) |
| Granularity | 0 (off) | 1/16 ft | 0..1 ft | snap step when dragging Edit points |
| Height (billboards) | 0 | 0.25 | -50..50 | not gradient related |
Keys: `[` / `]` strength ∓ step, `=` / `-` zoom ±0.5 (clamped at min), arrows pan by viewFt/4.

## 6. Interaction
### Gradient Placement (9)
- **Click** within 0.5u of the selected gradient's A → drag A; within 0.5u of its B → drag B.
  Otherwise click within 0.5u of **any** gradient's A or B → select that gradient (point selection cleared).
  Clicking empty space does nothing (no gradient is created by clicking).
- "Untouched" = profile has ≤ 2 points and all their heights are 0.
  - drag A, untouched: only A moves (B stays) · drag A, edited: **whole gradient translates** (length kept)
  - drag B, untouched: B follows the mouse · drag B, edited: B rotates around A, **length locked to sN**
  - on mouse-up after dragging an untouched gradient: profile resets to (0,0),(|AB|,0) (so length follows).
- **New Gradient**: A = view center, B = A + (50, 0) ft (50 ft along +X), flat profile, becomes selected.
- **Copy Gradient**: duplicate of the selected one shifted **10 ft sideways**: offset = 10·(dz, -dx)/|AB|
  (for a gradient pointing +X that is 10 ft toward -Z, i.e. up on screen). Same points and segment types,
  becomes selected.
  With nothing selected: status "No gradient selected".
### Gradient Edit (10)
- Graph point under the mouse is at world (ax + s, az - h). **Click within 0.5u of a point** → select + drag.
  The Segment Type toggle loads that point's type.
- **Click inside the graph strictly between two points' s** → inserts a point there at the mouse
  (s = mx - ax, h = az - mz), inheriting the previous point's segment type, and starts dragging it.
- **Drag**: if Granularity > 0, s and h each round to the nearest multiple. First/last point: only h moves
  (s locked). Interior point: moves while prev.s < s < next.s; **dragging past a neighbour deletes the
  point** (that is the editor's delete). No other delete exists for profile points.
- **Segment Type** writes Line/Curve to the selected point (= the segment from it to the next point).
- **Flatten** (also key `f`): profile reset to (0,0),(|AB|,0), both Line.
- **Set From Terrain**: samples the ground under A→B every **cell size** (plus one sample at |AB|),
  all segments Curve, then simplifies:
  1. from the end, finds runs of points with equal slope (|Δslope| < 1/8192); a run spanning ≥ 2√2 cells
     collapses to a single **Line** segment (interior points removed)
  2. 6 passes with tolerance 1/128, 1/64 … 1/4 ft: removes any point between two Curve segments whose height
     is within tolerance of the straight line through its neighbours
  Point selection is cleared.
### Brush modes (0-8) cursor
- White α1 outline circle of radius R at the cursor, plus an inner circle at Focus·R when Focus > 0.
- `l` key: drops a **line anchor** at the cursor; the cursor then draws as a capsule (two arcs) from the anchor
  to the mouse, and the stroke paints along that segment. `l` again clears it; mouse-up also clears it.
  (Capsule shape from the arc calls; exact arc arguments not traced: **(observed)**.)

## 7. Gradient record in memory (stride DAT_1417e2628)
float ax, az, bx, bz · points* (8 bytes each: float s, h), capacity, count, stride=8 ·
segType* (1 byte each: 1 = Curve, else Line), capacity, count, stride=1.
Globals: count DAT_1417e2624, array DAT_1417e2618, selected gradient DAT_1417e2634, selected point
DAT_1417e2630, mode DAT_1417e2600, drag state DAT_1417e2608 (1 = A / point, 2 = B).
Byte offsets (verified 2026-11-05 by reading the running game, 93 of 93 records matched edinfo): record 64 bytes; points
vector at +0x10 and types vector at +0x28, each {ptr, i32 capacity, i32 count, i32 stride, 4 bytes padding}.

## 8. Camera: why the graph sits where it does (verified, 2026-11-04)
The editor's camera (start of 0x1400613b0) is: ortho over the window, then `rotate(90, 1, 0, 0)`, `translate(0, -1, 0)`,
`scale(k)`, `translate(-centerX, 0, -centerZ)`. Rotating +90 degrees about X sends world +Z to screen **down**, so the
graph's `z = az - h` puts taller heights **up** the screen. The matrix helpers are a command list: 0x140044e80 push,
0x140044ec0 pop, 0x140044b90 rotate, 0x140044c20 scale, 0x140044b00 translate. The graph block calls only
push, translate(ax, 0, az), draw, pop: **no rotate**, so the graph never turns with the gradient. It always runs along
+X from A, whatever direction A to B points, and it is drawn on top of the map at true scale (1 graph ft = 1 map ft).
No text is drawn on the graph: no axis labels, no numbers. The only numbers are the panel readouts.

## 9. The side panel's look (verified, 0x1400613b0 + widgets at 0x14001e190..0x14001fd80)
The panel is drawn in screen space over the map's left edge, not beside it.
- **Unit p = window height / 64.** Everything below is in p.
- **Backdrop:** black alpha 0.5 from x = 0 to 21, full window height (64), then a strip from 21 to 21.25 fading from
  alpha 0.5 to 0 (QUADS with per-vertex alpha). The map shows through.
- **Rows:** start 0.5 in from the left edge. Each widget is 20 wide and 1 tall, and rows are 1.5 apart (0.5 gap).
  The first row is at the top.
- **Box shape (0x14001e670), pad 0.125:** fill the rect grown by 0.125 on every side, then draw a 0.125-wide band around
  the original rect a second time. Translucent colours stack, so the edge reads about twice as strong as the inside.
- **Colours (RGBA):**

| use | normal | hovered or active |
|---|---|---|
| button / choice value / slider bar box | 0.5, 0.5, 0.5, 0.25 | 0.9, 0.9, 0.9, 0.8 |
| text on those | 0.9, 0.9, 0.9, 0.8 | 0, 0, 0, 1 (black) |
| label box (slider and choice labels, readouts) | 0, 0, 0, 0.25 | same |
| label text | 0.9, 0.9, 0.9, 1 | same |

  "Active" means the mouse is over the widget, or for the 16 mode buttons, it is the current mode. Plain buttons
  (New Gradient, Flatten…) light up only on hover; there is no pressed state.
- **Button (0x14001ea30):** one box, text left-aligned.
- **Choice (0x14001ed10), e.g. Segment Type:** label box from 0 to 10 - 0.125 with the label; value box from 10 + 0.125
  to 20 with the current option's text. Segment Type options: index 0 "Line", index 1 "Curve".
- **Slider (0x14001f130), e.g. Granularity:**
  - label box on the left, label text left-aligned, colour as label text;
  - value text `%.2f` in its own label box, right-aligned so it ends 0.125 left of the middle (10). Its width is
    reserved for `max(max, 1000)` printed with `%.2f`, so the label box shrinks to fit;
  - bar box from 10 + 0.125 to 20 (hover colours), and inside it a fill from the bar's left edge, width
    `(20 - 10.125) * t`, where `t = clamp((value - min) / (max - min), 0, 1)`. Fill colour = that state's text
    colour, so it is light grey normally and black when hovered.
  - Granularity therefore shows `0.00` with an empty bar by default; `0.06` (1/16 ft), `0.13`, `0.25`, … `1.00`.
- **Readout (0x14001e830):** a label box with text, e.g. `Length: 60.00`, `Slope: 0.250`, `Height: 7.00`.

## 10. Which widgets show in each gradient mode (verified, 0x1400613b0)
Always, top to bottom: the 16 mode buttons, SubMode (none / Raise Only / Lower Only), Save, Zoom, Strength, Focus,
Radius, `Actual Radius: R (Focus*R)`, `Actual Strength: 2^Strength`. Then, only in that mode (other modes' widgets
are skipped and their hit boxes cleared):
- **Gradient Placement (9):** New Gradient, Copy Gradient, `Length: %.2f` (only when a gradient is selected).
- **Gradient Edit (10):** Segment Type, Granularity, Set From Terrain, Flatten, then `Slope: %.3f` and `Height: %.2f`
  (only when a point is selected).
- **Statues (13):** Show Wireframe, Show Collidable, Snap to Gradient (see 13).
Last rows of the panel: the cursor `x y z` (`%.2f %.2f %.2f`, y = ground height under the cursor) and the status text.

## 11. How the widgets respond (verified)
- Each mouse move picks the widget under the cursor (0x14001fa70); its index is the hovered widget.
- **While the mouse is over a widget, every key goes to that widget and the map never sees it.** So `f`, `7`, `8` and
  the arrow keys only act on the map when the mouse is off the panel.
- Button (0x14001fad0): fires on a click inside it, or Enter or Space while hovered.
- Choice (0x14001fb40): click, Enter, Space or Right arrow = next option, Left = previous, wrapping round. Typing letters
  jumps to the first option containing them (the matched text is drawn red); Backspace clears the typing.
- Slider (0x14001fd80):
  - Left / Right arrow while hovered: one step down / up, clamped to min..max.
  - Click or drag anywhere within 15 px of the bar (horizontally): `value = min + round((max - min) * t / step) * step`,
    `t` = mouse position across the bar, clamped. Dragging keeps updating while the button is held (the drag
    handler re-sends to the widget it started on).
- Changing mode (buttons or number keys) first saves an undo snapshot of any terrain painted since the last one
  (0x14005b060), and clears the status text.

## 12. Gradient Edit, step by step as the game behaves
1. Select a gradient in **Placement** (click its A or B handle). Edit mode cannot select a gradient: its clicks only touch
   the selected gradient's points, and a click that hits nothing **leaves both the gradient and the selected point as
   they were**.
2. Press `8` (mouse off the panel) or click the Gradient Edit button. Nothing moves: the camera stays put and the graph
   appears at A, running along +X. If A is near the screen edge the graph can be off-screen; the user pans.
3. The panel swaps the Placement widgets for Segment Type, Granularity, Set From Terrain, Flatten.
4. Click a point: it is selected (black outline, white fill) and Segment Type shows its type. The circle around A
   changes radius to that point's s, showing where that distance falls on the map.
5. Click between two points' s (anywhere on the map in the game): a point is inserted at the exact mouse position
   (**no Granularity snap on insert**), selected, and dragging starts; snapping applies from the first mouse move.
6. Drag: Granularity snaps s and h to the nearest multiple (`floor(v / g + 0.5) * g`), both relative to A, so snapped
   values are distances along the gradient and heights, not map coordinates. Ends move only in h. Past a neighbour,
   the point is deleted and the drag ends.
7. Segment Type writes to the selected point (the segment from it to the next one). With no point selected it only
   changes the widget.
8. Flatten (`f`) and Set From Terrain clear the selected point.
9. Back to Placement with `7`: the graph disappears, the endpoints become draggable and bright again.

## 13. Other features that use the selected gradient
- **Gradient Linear/Radial Paint/Add (modes 5-8) paint the selected gradient**, wherever you brush. Every brush helper
  (profile 0x14005f6e5/0x14005f821, linear s 0x14005f93d, radial s 0x14005fa10) reads the selected gradient
  (DAT_1417e2634). Select in Placement, switch to a paint mode, then brush along the line. A and B only define s; the
  stroke can go anywhere, and the brush footprint decides what changes. All gradients stay drawn faintly in these modes.
- **Snap to Gradient (Statues mode, 0x14005de30 button 0x23):** snaps the selected statue to the selected gradient.
  Let d = the statue's distance from A, and s* = the profile point whose s is closest to d.
  - If the statue is closer to the line A to B than |s* - d|, it moves to its projection on the line, turned to face
    along the line.
  - Otherwise it slides along the direction from A to the statue until it is exactly s* from A (onto the circle that a
    radial gradient makes at that point), turned to face A.
  - Turning is skipped when rot >= 3.25. The yaw is wrapped to (-pi, pi] and keeps the upright flag (minus 4*pi).
    y is set to 0 (on the ground). Exact facing sign is not checked in game **(unverified)**.

## Evidence
| addr | role |
|---|---|
| 0x1400613b0 | per-frame draw + panel (graph block 0x1400631d6..0x1400644db) |
| 0x14005c170 | draw one gradient on the map |
| 0x140060eb0 | sample a Curve segment every 0.25 ft |
| 0x1400645d0 | mouse-down / key handler (pick radius 0.5u, insert point) |
| 0x140066bf0 | mouse-drag handler (A/B drag rules, Granularity snap, delete-by-drag-past) |
| 0x14005de30 | panel buttons (New/Copy Gradient, Segment Type, Set From Terrain, Flatten, Snap to Gradient) |
| 0x14005d160 / 0x14005cf30 | New Gradient / Flatten (reset profile) |
| 0x1401776e0..0x1401778e0 | slider structs (Granularity, Height, Spacing, Radius, Focus, Strength, Zoom) |
| 0x140225190..0x140225398 | colors and constants (white/black α1/α0.25, grey 0.5, α0.0625, 10, 2.5, 0.25) |
| 0x14001e190 / 0x14001e670 / 0x14001e520 | panel row cursor / widget box shape / flat quad |
| 0x14001ea30 / 0x14001ed10 / 0x14001f130 / 0x14001e830 | button / choice / slider / readout drawing |
| 0x14001fa70 / 0x14001fad0 / 0x14001fb40 / 0x14001fd80 | hover pick / button press / choice cycle + type-ahead / slider click, drag, arrows |
| 0x140177070..0x14017715f | panel pad 0.125 and widget colours |
| 0x1402253b0..0x1402253cc | panel backdrop colour, 64-unit height, 21 / 21.25 width, -1.5 row pitch |
| 0x14005b060 | undo snapshot taken on mode change and brush mouse-up |
