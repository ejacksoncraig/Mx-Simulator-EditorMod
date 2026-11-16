# The game's statues in the editor: list, record, update call, Snap to Gradient

Found by disassembly of build **1684** (`b1cc5dc3...d710f6cf`), read-only, 2026-11-10. Addresses are **RVAs on 1684**.
Everything is **(verified)** in the exe unless tagged. 1.0 and 1681 are not ported (`statue_list` etc. are 0 there).

## 1. Data
- The list is a `{ptr, cap(int), count(int), stride(int)}` vector at **+0x23888** of the window struct
  (`*(void**)(base + 0x23d860)`, `rva_ptr_window`): ptr +0x23888, cap +0x23890, count +0x23894, stride +0x23898.
- Record: **+0x1c x, +0x20 world y, +0x24 z, +0x28 y offset above the ground** (the file's y), **+0x2c rot**, +0x30..
  the turn matrix (the update writes +0x30, +0x34, +0x38, +0x40, +0x44, +0x4c...). +0x00..+0x1b not traced (mesh,
  texture and shape, probably). Stride not read from a running game yet; the mod refuses one under 0x50.
- Statues mode's selected statue: `int 0x50fd2a0` (-1 none). Drag kind is the shared `0x50fd268` (1 = move).
- **Update `0x41110(statue*)`**: +0x20 = ground height at (x, z) (0x173b0 on the terrain) + the +0x28 offset, then the
  turn matrix from rot (upright when rot <= -2 pi, special when rot >= 3.25). Whoever writes x, z or rot calls it, then
  sets `terrain+0x49570 = 7` so Save writes `statues`. First bytes `41 54 53 48 81 ec a8 00 00 00 0f 29 74 24 20 0f`.

## 2. Clicks (key handler 0xea690, Statues branch 0xeab75)
Pick radius r = u/2 (u = max(viewFt * 0.01, 1), as for decals). The **first** statue (lowest index) whose centre is
within r is selected with a move drag. No modifier key is checked on the way there (read up to the pick; Ctrl+click
is assumed free, which the mod relies on: **unverified in game**).

## 3. Snap to Gradient (panel button 0x23, inline in the button handler 0xe38e0 at 0xe4291..0xe4b79)
Needs the edit lock clear (`terrain+0x49574 == 0`), a selected statue (0x50fd2a0) and a selected gradient (0x50fd294).
S = statue (x, z), A, B = the gradient's ends, all float:
1. d = |S - A| (via double sqrt). Best s* starts at **0** with diff = -d; for each profile point, nd = s - d replaces
   it when |nd| < |diff| (ties keep the earlier).
2. t = ((S - A) . (B - A)) / |B - A|^2; P = A + t (B - A) (not clamped); line = |P - S|.
3. **|diff| > line** -> onto the line: S = P; if rot < 3.25, rot = wrap(atan2(-t (B - A).x, -t (B - A).z)). The
   direction is scaled by t, so a statue behind A (t < 0) faces the other way.
4. Otherwise -> onto the ring: S = A + (s* / d)(S - A); if rot < 3.25, rot = wrap(atan2(A.z - S.z, S.x - A.x))
   (the old S), tangent to the ring.
5. wrap: w = (float)((a + pi) / 2pi), w -= floor(w), (float)w * 2pi(float) - pi(float); minus 4 pi (double) when the
   old rot <= -2 pi (keeps the upright flag).
6. +0x20 = 0, then 0x41110 (which puts ground + offset back in +0x20), then +0x49570 = 7. The y offset is untouched.

The mod's copy is `game_statue_snap` in `src/game.c`, with the same float/double steps. It also refuses a result that
isn't finite (a statue exactly on A, or A == B), where the game would write NaN.

## 4. The mod's statue group (mod 0.36.0, `src/statuetool.c`)
Ctrl+click toggles a statue in a group (with an empty group the game's selection joins as well); Ctrl+drag on empty
ground boxes statues by centre. Snap Group to Gradient runs `game_statue_snap` on each, one undo step for the lot
(Ctrl+Z / Undo Statue Snap; a statue moved since is skipped). The group is indices, so it clears when the list's
length changes. Not tried in the game yet.

0.36.2: a plain press on a group statue (nearest centre within the pick radius) is the mod's, so the game never starts
its own one-statue drag; the drag writes start pose + mouse offset to every member through `game_statue_set_pose`
(x, z, rot, then 0x41110, which keeps the +0x28 ground offset). The release makes one "Move" undo step. Writes go
ahead mid-drag because the game never saw the press, so its mouse-down flag stays clear.

## 5. Where a statue's file names are (1684, read 2026-11-12 from the statues writer; not used by the mod yet)
The writer (`lea` of `"[%f %f %f] %f %s %s %s"` at **0x4a4db**, loop 0x4a4f0..0x4a59f, `fprintf`-like 0x8d350) builds
each line from the record:
- **+0x00 mesh**: a pointer; the name is `*(char **)mesh` (getter 0xf0760 is `mov rax, [rcx]; ret`). Null -> "null".
- **+0x08 shape**: a pointer; the name is `*(char **)shape` (getter 0x77a80, the same one instruction). Null -> "null".
- **+0x10 texture**: an int id; 0x87040(id) walks the texture table (count `[0x40fcb3c]`, stride and pointer beside
  it) for the entry whose first int is the id and returns entry + 0x18, an inline name. 0 (not found) -> "null".
- The line's order is mesh (+0x00), texture (+0x10), shape (+0x08): x +0x1c, y offset +0x28, z +0x24, rot +0x2c.
- The writer reads the list from globals at **0x1ec5a48** (ptr), +0x50 cap, **0x1ec5a54** count, **0x1ec5a58** stride,
  and starts at the index in **0x1ec5a60**, not 0 (probably the timing gate's statues come first and aren't saved;
  unverified). 0x1ec5a48 - 0x23888 = 0x1ea21c0, so the window struct is likely that static (unverified).

## 6. Copying and deleting (1684, read 2026-11-15; used by mod 0.43.0's Duplicate Group)
- **`d`** in Statues mode (key handler 0xec1f7): needs the edit lock clear and a selected statue. It reads that record's
  names (mesh `*(char **)[+0x00]`, shape `*(char **)[+0x08]`, texture `0x87040([+0x10])`), then calls
  **`0x41430(float x, float y_offset, float z, float rot, mesh, texture, shape)`** with the mouse's world x/z, the
  record's +0x28 and +0x2c. That's the loader's own add: it loads the files by name, puts the record's +0x20 at
  ground (0x173b0) + offset, builds the turn matrix, **appends** to the list (the static vector at 0x1ec5a48, the same
  one the window struct holds at +0x23888) and returns count - 1. Then **`0x34e30()`** (no arguments; it zeroes a run
  of globals, probably the statue draw bins) and `terrain+0x49570 = 7`. Null names pass as NULL.
- **Del / x** (0xec35e): the first statue within the pick radius of the mouse, from index `[window+0x238a0]` on.
  **`0x4a390(record)`** frees what the record owns (+0x08 shape, +0x60, +0x68, +0x70); if it was the last one the count
  just drops by 1, otherwise 0xec6c7 shifts the rest down (not read). Then 0x34e30 and modified = 7. The selection isn't
  touched.
- The mod's copy (`game_statue_copy`, `game_statue_drop_last`, `game_statues_changed` in `src/game.c`) does the same
  calls; its undo only ever removes from the end, so no other statue's index moves. Not tried in the game yet.
- **Names on click (mod 0.44.0):** `0x87040(id)` also names decals: the decals writer (loop at 0xf910) calls it on the
  record's +0x00 colour id (it skips a decal whose id equals `terrain+0x4952c`). The mod shows the name for a decal
  pick and for a statue the game selected by a plain click (`game_texture_name`, `game_statue_texture`).
- **Files made while the game runs (1684):** an `@` path is opened by 0x8d450, which tries each search folder
  (0x8a1a0(i)) with a plain open (0x8a6f0) first, and only then `.gz` / `.scram` and the `.saf` file index (built once,
  0x8db70: "Listing directories", "Listing archives", "File index complete"). So a loose file written after startup is
  found; one inside a .saf isn't readable as a loose file. Mod 0.45.0's STATUE SIZE relies on this (`src/statuescale.c`),
  and points a statue at new files with `game_statue_set_files`: add with the new names, swap the two records, take
  the old one off the end (0x4a390, count - 1), then 0x34e30. Not tried in the game yet.
