# Editor mod: game builds

The editor tools use about 50 fixed addresses inside mx.exe (functions, globals, the brush loop). Every game build
moves them, so the mod carries one address table per build (`src/builds.c`) and picks it by the running exe's
SHA-256. A build without a table gets no tools (a box says the version isn't supported) but still gets the online
tripwire, which needs no addresses.

| Build | SHA-256 | Status |
|---|---|---|
| 1.0 (2025-01-26) | `54676784...bada392` | every address found here first, by disassembly |
| snapshot 2026-05-14 (1684) | `b1cc5dc3...d710f6cf` | ported 2026-11-05; the build new features are checked on first |
| snapshot 2025-03-26 (1681) | `0ce56445...0c961376` | ported 2026-11-05 |

## How a build is ported

Each address is matched against 1.0 by disassembly, read-only (mx.exe is never changed):

- Code: the instructions at each known address, with branch targets and RIP-relative displacements masked, must match
  exactly once in the new build. A match also confirms the stack sizes and struct offsets in that code.
- Data: every instruction that uses the global in 1.0 is matched the same way, and where the new instruction points
  is a vote. Calls count as sites too, for functions that don't match on their own.
- Anything ambiguous is checked by hand. No address is guessed.
- Also checked: the terrain struct offsets the mod uses (`+0x10c8 .. +0x49574`), counted across the whole `.text` of
  both builds, and the brush loop's stack slots `[rsp+0x1210]`/`[rsp+0x1218]` used by `brush_stub.asm`.
- Then in the editor on a scratch track: both hooks install (their prologue checks pass) and heights and gradients
  read back the same as the track's files.

## 1684 (2026-05-14)

- 23 of 25 code addresses and all 23 data addresses matched; every vote was unanimous. The editor globals all moved by
  +0x391ac60 and the window/GL pointers by +0x80, which agrees with each match.
- `RVA_VERTEX` (a tiny wrapper with near-identical copies) was ambiguous: its 6 call sites all voted 0xe1610.
- `RVA_KEY_HANDLER` didn't match: the function is instruction for instruction the same but its frame shrank from
  `sub rsp, 0x118` to `0xb8`, so the xmm saves moved. Found by comparing the globals each candidate function uses
  (0xea690: all 31 of the old handler's, translated), then confirmed by reading it. `FRAME_KEY_HANDLER` = 0xb8.
- `RVA_GAME_MALLOC`, `RVA_GAME_FREE`, `RVA_RADIAL_HEIGHT`, `RVA_PROFILE_HEIGHT` each matched once (0xdfff0, 0xe0310,
  0xe54c0, 0xe52d0). The gaps between them are the same in all three builds (malloc = realloc - 0x200, free = realloc
  + 0x120, radial = profile + 0x1f0), and the radial helper's first read is the selected gradient (0x50fd294), which
  the mod also checks before patching.
- In the editor: both hooks installed, heights matched `terrain.png` (max 8e-7 ft) and all 93 gradients matched
  `edinfo`.

## 1681 (2025-03-26)

- The same picture as 1684: 23 code and all 23 data addresses matched with unanimous votes (editor globals moved by
  -0x1307460, window/GL pointers by +0xa0), `RVA_VERTEX` came from its 6 call sites (0x60790), and the key handler
  has the smaller 0xb8 frame: 0x69810, a 203-byte exact match of 1684's handler.
- malloc, free, radial and profile helpers: 0x6dd60, 0x6e080, 0x64640, 0x64450.
- Terrain offsets and the brush stack slots are the same as 1.0's. In the editor: both hooks installed.

## The brush sliders (mod 0.28.0, 2026-11-06)

`rva_radius`, `rva_focus` and `rva_granularity` are the values of the game's own Radius, Focus and Granularity
sliders. They weren't matched by code: the editor's slider structs (64 bytes each, `{label, value, step, min, max,
...}`) sit next to each other in `.data` ending with Zoom, and each one's label pointer names it. In all three builds the
labels, defaults and ranges read Granularity (0..1, step 1/16), Height, Spacing, Radius (64, 1..128), Focus (0.5, 0..1,
step 1/32), Strength, Zoom. `.claude/skills/mx-editor-mod/find_sliders.py "<mx.exe>" <rva_zoom>` prints them for a new
build. The mod also checks each struct's label at run time before reading or writing the value.

| Build | Radius | Focus | Granularity |
|---|---|---|---|
| 1.0 | 0x177828 | 0x177868 | 0x1776e8 |
| 1684 | 0x21bc08 | 0x21bc48 | 0x21bac8 |
| 1681 | 0x176568 | 0x1765a8 | 0x176428 |

## Decals (mod 0.35.0, 2026-11-09): 1684 only

The decal tools use six more addresses (`rva_decal_replace`, `_shift`, `_insert`, `_remove`, `rva_selected_decal`,
`rva_decal_group`) and check each function's first 16 bytes (`decal_sig`) before the first call. They were found by
disassembly of 1684 (`.claude/skills/mx-editor-mod/game/decals.md`) and are 0 in the 1.0 and 1681 tables, so the
decal tools stay off there (the panel says so) until they're ported with `port_build.py` or by hand: match 0xcf70,
0xc020, 0xcd50 and 0xcca0 by code, and the two globals by the instructions in 0xe2610 / 0xe2d10 that use them.

| Build | replace | shift | insert | remove | selected | group |
|---|---|---|---|---|---|---|
| 1684 | 0xcf70 | 0xc020 | 0xcd50 | 0xcca0 | 0x50fd2a4 | 0x50fd2a8 |
| 1.0, 1681 | not ported | | | | | |

Confirmed in 1684 (0x1400ed97b, 0x1400e7a83): the brush stroke and the `Actual Radius` readout read Radius and Focus
fresh, outer ft = Radius * mapFt / (screenH * 2^zoom), inner = Focus * outer. The mod also reads the Zoom slider's
struct (label "Zoom", at `rva_zoom`) for its range. Still to confirm: that Gradient Edit snaps a dragged point with
Granularity.
