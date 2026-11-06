# Editor mod: game builds

The editor tools use about 50 fixed addresses inside mx.exe (functions, globals, the brush loop). Every game build
moves them, so the mod carries one address table per build (`src/builds.c`) and picks it by the running exe's
SHA-256. A build without a table gets no tools (a box says the version isn't supported) but still gets the online
tripwire, which needs no addresses (`docs/design.md` rule 2).

| Build | File in Jackson's game folder | SHA-256 | Status |
|---|---|---|---|
| 1.0 (2025-01-26) | `mx-2024-07-24-idkwhtathiswas.exe` | `54676784...bada392` | every address found here first, by disassembly |
| snapshot 2026-05-14 (1684) | `mx.exe` | `b1cc5dc3...d710f6cf` | ported 2026-11-05 (below); the one Jackson plays and builds on |
| snapshot 2025-03-26 (1681) | `mx-2025-03-26-1681.exe` | `0ce56445...0c961376` | ported 2026-11-05 (below) |

Jackson renamed the files on 2026-11-05: `mx.exe` is now 1684. Go by the SHA-256, not the name. New features are
checked on 1684 first; the other builds are confirmed later.

## Porting a build

1. `python tools/port_build.py "<1.0 mx.exe>" "<new exe>"` (needs `pip install capstone pefile`; read-only). The
   first exe must be 1.0: the script's starting addresses are 1.0's.
   - Code: the instructions at each known address, with branch targets and RIP-relative displacements masked, must
     match exactly once in the new build. Everything else must be identical, so a match also confirms stack sizes and
     struct offsets in that code.
   - Data: every instruction that uses the global in the known build is matched the same way, and where the new
     instruction points is a vote. Calls count as sites too, for functions that don't match on their own.
2. Anything that comes out `None` or `ambiguous` gets checked by hand. Never guess an address.
3. Check what the script can't: the struct offsets the mod uses (terrain `+0x10c8 .. +0x49574`), counted across the
   whole `.text` of both builds, and the brush loop's stack slots `[rsp+0x1210]`/`[rsp+0x1218]` (used by
   `brush_stub.asm`) next to the patch.
4. Add the table to `builds.c`, build, and run the `game_link` checks (hello, heights, `gradients`) in the editor.

## Mod 0.25.0's addresses (2026-11-05)

`RVA_GAME_MALLOC`, `RVA_GAME_FREE`, `RVA_RADIAL_HEIGHT`, `RVA_PROFILE_HEIGHT` were added to `port_build.py`, and each
matched exactly once in both snapshots (1684: 0xdfff0, 0xe0310, 0xe54c0, 0xe52d0; 1681: 0x6dd60, 0x6e080, 0x64640,
0x64450). The gaps between them are the same in all three builds (malloc = realloc - 0x200, free = realloc + 0x120,
radial = profile + 0x1f0), and on 1684 the radial helper's first read is the selected gradient (0x50fd294), which
the mod also checks before patching.

## 1684 (2026-05-14), ported 2026-11-05

- 23 of 25 code addresses and all 23 data addresses matched; every vote was unanimous. The editor globals all moved by
  +0x391ac60 and the window/GL pointers by +0x80, which agrees with each match.
- `RVA_VERTEX` (a tiny wrapper with near-identical copies) was ambiguous: its 6 call sites all voted 0xe1610.
- `RVA_KEY_HANDLER` didn't match: the function is instruction for instruction the same but its frame shrank from
  `sub rsp, 0x118` to `0xb8`, so the xmm saves moved. Found by comparing the globals each candidate function uses
  (0xea690: all 31 of the old handler's, translated), then confirmed by reading it. `FRAME_KEY_HANDLER` = 0xb8.
- The eight terrain offsets are used the same number of times in both builds (e.g. `+0x49508` 93 times), and the brush
  loop reads its radii from the same stack slots.
- In the editor on ModTest: both hooks installed (their prologue checks passed), hello read the track (4097 wide,
  0.5 ft cells), heights matched `terrain.png` (max 8e-7 ft) and all 93 gradients matched `edinfo`.

## 1681 (2025-03-26), ported 2026-11-05

- The same picture as 1684: 23 code and all 23 data addresses matched with unanimous votes (editor globals moved by
  -0x1307460, window/GL pointers by +0xa0), `RVA_VERTEX` came from its 6 call sites (0x60790), and the key handler
  has the smaller 0xb8 frame: 0x69810, which shares all 31 globals and is a 203-byte exact match of 1684's handler.
  So the frame change came in between 1.0 and 1681.
- Terrain offsets and the brush stack slots are the same as 1.0's.
- In the editor on ModTest: both hooks installed.
