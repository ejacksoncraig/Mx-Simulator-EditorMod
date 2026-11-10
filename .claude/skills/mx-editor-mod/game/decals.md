# The game's decals: data, drawing order, edit calls, and Decals mode (15)

Found by disassembly of build **1684** (snapshot 2026-05-14, `b1cc5dc3...d710f6cf`), read-only, 2026-11-09.
Everything here is **(verified)** in the exe unless tagged (unverified) or (observed). Addresses are **RVAs on 1684**
(VA - 0x140000000), the same form as `src/builds.c`. Builds 1.0 and 1681 have not been looked at: port each address
the usual way (`port_build.py` / by hand) before using it there, and add it to `mx_build`.
`terrain` below is `*(void**)(base + rva_editor_terrain)`.

## 1. f5: the axis is the other way round from the app's assumption
Record +0x20 = sin(rot), +0x24 = cos(rot) (0x157650 stores +0x20 and names itself "sin" in its error string at
0x23cba0; 0x1567b0 is cos). The quad corners (0x3060), with W = size and L = size * f5:

| corner | world offset from (x, z) | uv |
|---|---|---|
| 0 | -L/2 (cos, -sin) - W/2 (sin, cos) | (0, 0) |
| 1 | +L/2 (cos, -sin) - W/2 (sin, cos) | (1, 0) |
| 2 | +L/2 (cos, -sin) + W/2 (sin, cos) | (1, 1) |
| 3 | -L/2 (cos, -sin) + W/2 (sin, cos) | (0, 1) |

- Texture **u** (image left to right) runs along world **(cos rot, -sin rot)** and covers **size * f5** ft.
- Texture **v** (image rows) runs along world **(sin rot, cos rot)** and covers **size** ft.
- So **width = size * f5, height = size, f5 = width / height**. The brief and the app (`decal-paint.js`) have
  size = width and f5 = height / width. That is wrong whenever f5 != 1; every decal seen so far has f5 = 1, which
  is why it never showed. The app's u direction is right.
- Stretch across (u): f5 = newWidth / size. Stretch along (v): size = newHeight, f5 = oldWidth / newHeight.
- (unverified) whether v = 0 is the image's top row (it decides whether rot = 0 looks upright or flipped in the
  editor's view). Check first on ModTest with a lopsided picture at f5 = 2. v = 0 is the corner side at
  -size/2 (sin, cos) from the centre, which is the side the game's rotate dot is on (centre - 4u (sin, cos)); the mod
  puts its turn handle there too and calls it the top.
- Bounds as the game computes them (bin 0xc0b0, un-bin 0xc5c0): half extents |cos| L/2 + |sin| W/2 in x and
  |sin| L/2 + |cos| W/2 in z. Un-binning pads them by 1.0039.

## 2. Data
- Decal array: `terrain+0x40` ptr, `+0x48` capacity, `+0x4c` count, `+0x50` stride (72).
- Record, 72 bytes: +0x00 color texture id, +0x04 `_tnorm` id, +0x08 `_tspec` id (0 = none), +0x0c x, +0x10 z,
  +0x14 rot, +0x18 size, +0x1c f5, +0x20 sin rot, +0x24 cos rot, +0x28..+0x44 uv corners (0,0) (1,0) (1,1) (0,1).
  **Whoever changes rot must also write +0x20 and +0x24.** The bins and the drawing only use those.
- Bins: the map is an 8 x 8 grid of index lists at `terrain + 0x58 + row*0xc0 + col*0x18`, each `{ptr, cap(int),
  count(int), stride(int)}` and kept sorted by decal index. Dirty bitmap `terrain+0xe88`, one bit per cell. Run lists
  for drawing at `+0x658`.
- `terrain+0xe90`: how many decals were appended since the last rebuild (drawn on top without rebuilding); -1 forces a
  rebuild. `terrain+0x4957c != 0` makes insert and replace skip binning (meaning not traced).
- After any decal change the game sets `terrain+0x49570 = 7`, the same "modified" flag as height edits, so Save
  writes the files. The edit lock is `terrain+0x49574`, as for heights.

## 3. Drawing order (what is on top)
The list order is the paint order: index 0 is painted first (bottom), the last decal last (top), alpha-blended
(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA). Load (0x0ff80) appends one record per line of `decals` in file order. Save
(0x0ef50) writes `[%f %f] %f %f %f %s` back in list order. Inside each bin the list stays sorted by index (0xc0b0).
Batching (0x99a0) only merges consecutive decals that share textures, so order is never regrouped by texture.
Drawing (0x3d20, from the incremental ground-texture rebuild 0xa5e0) runs three passes per texture level: color,
`_tnorm`, `_tspec`. A decal without `_tnorm` covers the color under it but leaves the normals alone.
A line whose color texture fails to load is skipped. The first line that doesn't parse ends the file.

## 4. The calls an edit needs (1684)
| RVA | signature | does |
|---|---|---|
| 0xcf70 | `(terrain, rec*, i)` | **replace decal i**: un-bin the old footprint and mark its tiles, copy 72 bytes, re-bin. The game's own drags call it on every mouse move. |
| 0xc020 | `(terrain, i, delta)` | add delta to every bin entry >= i (+1 before an insert, -1 after a delete) |
| 0xcd50 | `(terrain, rec*, i)` | insert at i (i = count is the cheap append path) and bin it |
| 0xcca0 | `(terrain, i)` | un-bin and remove decal i |
| 0xc5c0 / 0xc0b0 | `(terrain, i)` | un-bin / bin one decal (used by the three above) |
| 0x92d0 | `(terrain, i)` | mark every resident texture-level tile under decal i for redraw |
| 0x9b70 | `(terrain)` | rebuild dirty cells' VBOs and run lists |
- Insert as the game does it: `0xc020(t, i, +1)` then `0xcd50(t, &rec, i)`.
- Delete as the game does it: `0xcca0(t, i)` then `0xc020(t, i, -1)`.
- The ground re-bakes incrementally (0xa5e0), so replacing a decal on each mouse move is what the game itself does.
  The brief's fallback (an overlay quad while dragging, rebake on release) shouldn't be needed. Measure before
  building it.

## 5. Decals mode as the game has it (key/button handler 0xea690 = `rva_key_handler`, drag 0xec740)
Editor globals (1684): selected decal `int 0x50fd2a4` (-1 none). Drag kind `int 0x50fd268` (= `rva_drag_state`):
1 move, 2 rotate. Group selection: `0x50fd2a8` ptr, `0x50fd2b0` cap, `0x50fd2b4` count, `0x50fd2b8` stride, int indices.
Pick radius r = u/2 with u = max(viewFeet * 0.01, 1).
- **Click:** the first decal (lowest index, so the bottom-most) whose **centre** is within r is selected with a move
  drag. Otherwise the first whose **rotate dot**, at centre - 4u (sin rot, cos rot), is within r gets a rotate drag.
  Otherwise a new decal is stamped. There is no area pick: a small decal under a big overlay is reachable only by
  its centre dot.
- **Move drag:** x, z = mouse, `0xcf70` on each move over 0.001 ft. The group follows (0xe0db0).
- **Rotate drag:** rot = atan2(x - mouse x, z - mouse z). (observed) the sin/cos it writes are the previous rot's, so
  the ground lags one move behind. The mod should compute them from the new rot.
- **Drag that starts on empty ground:** stamps copies of the selected decal every size * Spacing ft (0xe2d10(0)).
- **`d` (100):** one copy, size * Spacing ft toward the mouse (0xe2d10(1)). **The brief's D collides with this.**
- **`x` / Del:** delete decals whose centre is within r (0xe2610(0)); **`X`:** same with a few-pixel radius.
- **`m`:** toggle decals under the cursor in the group; **`M`:** add all within a radius (0xe3530).
- **`{` / `}`:** spread or pull the group about the selection (0xe1090 -> 0xe0db0 with a scale; factor not read).
- New and stamped decals copy size, f5 and textures from the selected one. **There is no resize or stretch.**
- Decals mode does **not** check the edit lock. Backspace undo restores heightmaps only.

Panel choices in Decals mode (choice structs: int value at +0x14; slider: double at +8; labels checked by text):
| label | value (1684) | options (* default) | effect |
|---|---|---|---|
| Level | `int 0x21b874` | Top, Under Current, Over Current*, Bottom | where new and stamped decals go in the list, i.e. their paint order (0xe0500) |
| Hide different | `int 0x21b8b4` | No*, Yes | Yes: only decals with the selection's texture can be picked, deleted or grouped |
| Orientation | `int 0x21b8f4` | Copy, Random, Flow*, Flow2, Uphill | rot of new decals: kept, random, along the stroke, chained from the back edge, downhill |
| Spacing | `double 0x21b928` | 0.75* (step 1/16, min 0.25) | stamp distance = size * Spacing |
Level, where a "run" is consecutive decals sharing the selection's color texture: Top = end of the list,
Under Current = start of the selection's run, Over Current = just after its end, Bottom = index 0. The default puts
new decals just above their own texture group, not on top of everything, which is why placed decals seem to sink.

## 5b. Index fix-ups and signatures, read 2026-11-09 (verified, 1684)
- **Call signatures** (first instructions): 0xcf70, 0xcd50 and 0xcca0 take terrain in rcx and check the index against
  `[rcx+0x4c]` before anything else; 0xcf70 copies the 72 bytes as four movdqu and one qword. 0xcd50 grows the array
  itself when count == capacity. The mod checks each function's first 16 bytes (`decal_sig` in `src/builds.c`).
- **The game's copy (0xe2d10), inserting at idx:** every group entry >= idx goes up one; if idx <= the selection, the
  selection goes up one; `0xc020(t, idx, +1)`; `0xcd50(t, &rec, idx)`; then the selection = idx (0xe2f86).
- **The game's delete (0xe2610), per decal i:** i comes out of the group (later entries slide down, count - 1);
  `0xcca0(t, i)`; group entries >= i go down one; `0xc020(t, i, -1)`; selection -1 if it was i, one down if it was
  later. Once the selection is -1, the group's count is set to 0.
- **Del, `x` and `m` act at the mouse, not on the selection.** 0xe2610 loops over every decal and deletes each one whose
  centre is within r of the mouse; the selection is only read for Hide different (its texture). So writing a pick to
  `0x50fd2a4` does not point the game's Del at it (section 6 originally assumed it did). The mod has its own Delete
  Decal instead.

## 6. Notes for the decal tools (the brief)
- Pick the smallest-area decal whose turned rectangle holds the mouse, then write its index to `0x50fd2a4`, so the
  game draws it as selected and its copies, stamps and Level work from it. (Not its Del, `x` or `m`: see 5b.)
- Corner and side drags: build the new record (size and f5 with the axes of section 1, sin and cos from rot), then
  `0xcf70` on each move.
- D, a flush copy on its right: right = +u = (cos rot, -sin rot), centre moves by size * f5. Insert at i + 1 with the
  insert pair and select the copy. Swallow the key in the key-handler hook so the game's `d` doesn't run too.
- Indices shift on insert and delete: fix the game's selection and group list the way 0xe2d10 / 0xe2610 do.
- Undo: snapshot the 72-byte record plus the index and the kind of change before each edit. Restore with `0xcf70`,
  or the insert or delete pair.
- Refuse edits while the edit lock is set, even though the game doesn't check it here.
