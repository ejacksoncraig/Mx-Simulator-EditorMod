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
