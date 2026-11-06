# Editor mod: live terrain edits inside MX Simulator's editor (design)

Status: M0 to M4 passed on Jackson's PC (2026-11-05, results under "Milestones"); gradients grow through the game's
realloc since 0.18.0. The tools inside the editor (M5 on) are in `docs/ingame-tools.md`. Owner: Jackson. Offline
only: The mod must never be active in anonline session. "The app" below is MX Simulator Tools (its own repo); 
`core/*.py` and `web/*.js` are its files.

## Goal
The app stays the place where things are designed (gradients, curved gradients, profiles, objects). A small DLL
inside `mx.exe --editor` applies the app's results to the running editor, so the ground changes on screen right away
and the game's own Save (`s`) writes the files. No restart, no `/forcereload` (that only sets a flag: "Track will be
reloaded on next restart", 0x1400d5e65).

```
 MX Simulator Tools (Python)                     mx.exe --editor
 ┌──────────────────────────┐   named pipe      ┌───────────────────────────────┐
 │ core/gradient.py, curved │  \\.\pipe\mxmod   │ opengl32.dll (this mod)       │
 │ computes height patches  │ ───────────────▶  │  - passes GL through          │
 │ core/game_link.py        │ ◀─────────────── │  - wglSwapBuffers: apply queue │
 │ (client, never touches   │   status, reads   │  - writes heights, marks dirty │
 │  game memory itself)     │                   │  - (later) draws in-game UI    │
 └──────────────────────────┘                   └───────────────────────────────┘
```

## Rules for the mod (also in CLAUDE.md)
1. **Offline only.** It activates only when the command line has `--editor`. It never loads its features in a race or
   online session, and the README says so.
2. **Code changes in memory only, offline only** (changed 2026-11-05; until then: no code changes at all). mx.exe on
   disk is never touched, since the same file is used online. In memory the mod may patch bytes, hook functions inline
   and edit the import table, but only once rule 1 holds. mx.exe hashes about 1 MB of its own code (0x1400ba740 hashes
   0x140001540, length 0xfbc00, and compares with a stored value) and checks a table of memory regions round robin
   (0x1400bb970 / 0x1400bb0e0, region tables at 0x140218e20 and 0x140177bc0). The results feed the tamper flags
   (0x1400bbb10), which the main loop stores at 0x14356c2c4..cc, likely for the server. So a patched session will set
   them: before the first patch, confirm nothing keeps them past the session (a file, the registry, a later online
   connection) and that an `--editor` session can't go online. M0 to M4 use no code changes, so they don't depend on
   this.
   **Checked by disassembly before M5 (2026-11-05):** the slots 0x14356c2c0..cc are only written by 0x1400d27d0,
   0x1400d3a50 and the main loop, which stores the check results in `+0xc8`, `+0xcc` and `_time64()` in `+0xc4` (the
   time of the last check, which the main loop also uses to pace the checks). The only reader of the results is
   0x1400d3520 (called once, from 0x140172e0d): it formats them into a server message and shows "Not connected to
   server" when offline. Nothing that touches the slots writes a file or the registry. So the results live in that
   process's memory and only reach a server in a session that's connected.
   **The online tripwire (`src/tripwire.c`):** the game talks to servers over UDP only, through ws2_32 `sendto` (its
   import table entry is RVA 0x517a134 on the known build). Since 0.22.0 it is armed in every session the DLL is
   loaded in, editor or not and on any build, from DllMain before the game has sent anything (decided 2026-11-05:
   having the DLL in the game folder at all must keep a player off servers). It changes no code: it walks mx.exe's
   import table and swaps every entry equal to ws2_32's `sendto`, `WSASendTo`, `connect` or `WSAConnect` (found by
   value, so forwarders and other builds work too; the known build imports only `sendto`). If no `sendto` or
   `WSASendTo` entry is found, it fails closed (0.24.1): at the first frame a box says the mod can't keep this game
   version offline and the game exits, so a session never runs on with the network open (`MXMOD_PRETEND_UNARMED=1`
   shows that path on a build where it does arm; tested 2026-11-05). Every call is refused, this PC included (127.x and ::1
   passed until 0.23.1; dropped then because a server hosted on the same PC would have got through). Each one is
   dropped before it leaves: the game gets -1 with `WSAENETUNREACH` (it shows its own socket error), the log notes the
   first few, and a message box (on its own thread, at most once per 15 s after it's closed) says online play is
   blocked and asks "Close the game now?". Yes posts WM_CLOSE to the game window and calls ExitProcess if the game is
   still running 5 s later; No (the default, so a stray Enter loses nothing) goes back to the game. A box over the
   fullscreen game stayed hidden behind it even with MB_TOPMOST (0.21.0), so the game window is minimized while the
   box is up (works, 0.21.1). The entries are restored only if the DLL unloads while the game runs on. Tested
   2026-11-05 from `--editor`: joining a server sent nothing (three packets to 104.152.109.242:19876 blocked). Hello
   reports `ingame.tripwire` and `ingame.blocked`. To play online, run uninstall.
   **Why those four functions cover it (verified 2026-11-05, build 54676784...a392):** mx.exe's WS2_32 imports are
   only WSAStartup, WSACleanup, WSAGetLastError, socket, closesocket, gethostbyname, htons, ioctlsocket, recvfrom and
   sendto; no send, WSASend, WSASendTo or connect, no wininet/winhttp/urlmon, and no delay imports. The only DLL names
   it loads at run time are Shcore.dll and user32.dll (DPI). Its WMI use (`root\cimv2`, `Win32_PNPEntity`) is the
   standard XInput controller check, local only. The only hostname in the exe is `elsinore.mxsimulator.com`, the
   default text of the Server box (0x14021ae80); Connect sends to whatever that box holds (0x1400ee1b6), so the client
   talks to one server and nothing else. What still leaves the PC: the DNS lookup of that name (through Windows, not
   sendto), and anything another DLL in the process sends on its own.
   **What the server would get (0x1400d3520):** a text line `version <n> <SHA-1 hex> <time> <checkA+time+0x555>
   <checkB+time+0xaaa> <32 bytes at 0x1417e26e0 as hex> 2024-07-24-1671 snapshot <str 0x1413dcac8> <str 0x1413dcac0>`,
   signed with SHA-1 over an int, the line and 16 bytes at 0x144aeb430, sent as message 0x18 (or 0x1e with every byte
   shifted by 0x80). The two masked check results are what a patched session changes. What the server does with
   them isn't in the client.
3. **Respect the edit lock.** The brush, undo and statue code all refuse to edit while `terrain+0x49574 != 0`. The track
   loader sets it (0x1400e938a, right after the track-hash step, when a comparison matches; meaning not traced). The mod
   refuses too.
4. **Refuse unknown builds.** v1 pins mx.exe's SHA-256 (54676784...a392) and its addresses. Any other build: log it,
   disable, pass GL through untouched.
5. **The app never writes game memory.** Only the DLL does, and only through the commands below.

## Why a standalone opengl32.dll
- The game already loads `opengl32.dll` from its own folder first (that's how MXS Shaders works), so no injector is needed.
- `wglSwapBuffers` runs once per frame on the game's main thread, right after the editor drew (the editor's draw and
  input run in the main loop, 0x1400613b0 / 0x1400645d0). Applying edits there means the game never sees half-written
  data, with no locking inside the game.
- It also gives us the GL hooks for an in-game overlay later (section "Later").
- MXS Shaders: only one `opengl32.dll` fits in the folder. The mod checks for `opengl32_mxs.dll` next to it and loads that
  instead of System32's, so both work (rename MXS Shaders' DLL to `opengl32_mxs.dll`).
- **mx.exe calls `SwapBuffers` from GDI32, not `wglSwapBuffers`** (verified, import table). GDI forwards to the loaded
  opengl32's `wglSwapBuffers`, which is how MXS Shaders hooks MX. As a fallback the mod also hooks `glClear`: if no
  `wglSwapBuffers` call arrives after 240 clears, `glClear` drives the frame work and the log says so. First run: the
  hook fired every frame with MXS Shaders chained (`swaps` equals `frames`), so the fallback wasn't needed.
- Addresses are kept as offsets from the exe's load address (RVAs), not absolute numbers, so ASLR doesn't matter.
- Alternative considered: proxy `XInput9_1_0.dll` (mx.exe imports it, tiny export list). Rejected: no render hook for an
  overlay, and per-frame calls to it aren't confirmed.

## What the mod touches (verified in mx.exe, build 54676784...a392)
| thing | where | notes |
|---|---|---|
| editor terrain object | `*(ptr*)0x1417e25c8` (also `0x1433e40a0`) | null when no track is loaded |
| heights | `float* = *(ptr*)(terrain + 0x0)`, index `row * width + col` | **float32 feet in memory** (the 16-bit PNG is only the file format) |
| grid width | `int terrain + 0x49508` | heightmap is width x width |
| height range | `float terrain + 0x10f4` (max), `+0x10f8` (min) | brush widens these when it writes outside them |
| changed flags | `int terrain + 0x49570 = 7` | what every editor edit sets |
| change counter | `int terrain + 0x4953c += 1` | undo and brush increment it |
| redraw | call `0x140017830(terrain, col0, row0, col1, row1)` | only ORs dirty bits for 128 x 128-cell blocks into the bitmap at `terrain + 0x38`; the per-frame terrain update (0x140017500, called 8x per editor frame) rebuilds them. Cheap, no GL calls. |
| undo snapshot | set `int 0x1417e25d8 = 1`, then call `0x14005b060()` | copies current heights into the 8-deep undo ring and the stroke baseline `*(ptr*)0x1417e25d0`. Called after an edit, so Backspace returns to the state before it. |
| status text | `char* 0x1417e26a8` | point it at the mod's own static string to show "Applied curved gradient" in the panel |
| editor mode | `int 0x1417e2600` (0-15) | read only |
| gradients | array `*(ptr*)0x1417e2618`, count `0x1417e2624`, capacity `0x1417e2620`, stride `0x1417e2628` (64), selected `0x1417e2634`, selected point `0x1417e2630` | 64-byte record, verified against ModTest's edinfo (93 of 93): `+0x00` float ax, az, bx, bz; `+0x10` points {ptr, i32 capacity, count, stride 8} of float s, h; `+0x28` types {ptr, i32 capacity, count, stride 1} of u8 (1 = Curve); 4 bytes of padding after each stride. Capacities seen: 16 or 32 points, 128 gradients. Growing any of it must use the game's own realloc (0x14000bbc0), never the mod's allocator. |
| lock | `int terrain + 0x49574` | nonzero = editing locked, see rule 3 |
| game malloc / realloc / free | `0x14000b9c0(size)`, `0x14000bbc0(old, size)`, `0x14000bce0(p)` | the CRT's, plus the game's allocation tracking. Gradient vectors and the gradient array are only ever made, grown and freed through these |
| radial helper | `float 0x14005fa10(float x, float z)`, tail call to `float 0x14005f820(float s)` | Radial Paint/Add's profile height at a cell; replaced while the radial sweep isn't 180 centred (docs/ingame-tools.md section 9) |

Applying a height patch, step by step (copies what the brush does, 0x140066bf0 case 4):
1. Check build, `--editor`, terrain pointer, lock flag, patch inside the grid.
1b. If `0x1417e25d8 != 0` (a brush edit hasn't been snapshotted yet), call the undo snapshot first, so Backspace undoes
   the mod's edit on its own instead of merging it with the user's last stroke.
2. Write the floats.
3. Widen `+0x10f4` / `+0x10f8` if any new height is outside them.
4. `+0x49570 = 7`, `+0x4953c += 1`, call the redraw function on the patch rectangle.
5. Undo snapshot, status text.

## App to mod protocol (`\\.\pipe\mxmod`, one client at a time)
Little-endian frames: `u32 length, u32 type, payload`. JSON for small messages, raw floats for heights.
| type | direction | payload |
|---|---|---|
| `hello` | both | JSON: protocol version, exe SHA-256, `editor: bool`, `locked: bool`, `width`, `cellFt`, `minFt`, `maxFt`, `commandLine` (from 0.3.0; the app reads `--track` from it) |
| `get_heights` | app to mod | JSON rect `{col0,row0,cols,rows}`; reply is a float32 block. The app's view syncs from the game, not from disk. |
| `set_heights` (5) | app to mod | binary: i32 col0, row0, cols, rows, u32 flags (1 FIRST, 2 FINAL), float32 block (`rows*cols`, at most 262144). Applied at the next SwapBuffers. Reply `applied` (6), JSON `{applied, readonly, changes, minFt, maxFt}`. |
| `status` | mod to app | mode, change counter, lock, last error |
| `get_gradients` (7) / `gradients` (8) | app to mod / reply | binary: i32 count, selected, selected point; per gradient f32 ax, az, bx, bz, u32 points, u32 room, points x (f32 s, h), points x u8 type |
| `set_gradient` (9) | app to mod | binary: i32 index, f32 ax, az, bx, bz, u32 n, n x (f32 s, h), n x u8 type. In place, within the room the gradient has. Reply `applied` |

Limits: one patch per frame (a 2049 x 2049 grid is 16 MB; split big patches into strips of at most 1 MB per frame so the
editor stays smooth). The mod copies the queue on its own pipe thread, but **only the SwapBuffers hook touches game
memory.**

## Layout
- This repo (C, MSVC, built by `build.bat`, like MXS Shaders): `src/proxy.c` (load the real or chained opengl32,
  forward every export), `opengl32.def` + `stubs_*` generated by `tools/gen_exports.py`, `src/game.c` (addresses,
  checks, apply), `src/pipe.c`, `src/log.c` (`%LOCALAPPDATA%\MX Simulator\mxtools\log.txt`), the in-game tools
  (README.md, "Files").
- MX Simulator Tools' `mxtools/core/game_link.py`: the pipe client, pure Python (`open()` on the pipe path), no Qt,
  behind the Track Editor's Apply In Game, Live In Game, Get From Game, Send To Game and Play In Game.
- MX Simulator Tools' `tests/test_game_link.py`: a fake mod server in Python speaks the protocol, so the app side is
  tested without the game. The protocol constants live in `src/protocol.h`, mirrored by `game_link.py`; that test
  parses this repo's header (cloned beside the app, or `MXMOD_DIR`) so they can't drift.

## How to develop it (dev loop)
- Tools: Visual Studio Build Tools (free, "Desktop development with C++"), x64dbg for attaching to mx.exe, the REA /
  Ghidra setup for checking addresses. Never test on a real track: copy BetterBlank to `ModTest` first.
- Every milestone starts in **read-only mode** (a `MXMOD_READONLY=1` environment variable makes every write a no-op
  and logs what it would have done).
- After each milestone: start `mx.exe --editor --track ModTest`, check the log, then save in game and compare the saved
  `terrain.png` with what the app computed (`core/gradient_check.py` already does this kind of comparison).

## Milestones
- **M0, pass-through proxy.** The DLL loads, logs the exe hash and `--editor`, forwards GL. The game must look and run
  exactly as without it, with and without MXS Shaders chained. Done when a race and the editor both work unchanged.
  **Passed 2026-11-05, signed off by Jackson.** The log showed the known build and MXS Shaders chained, and the editor
  ran on ModTest. Claude's checks only covered `--editor`; the runs without it and with MXS Shaders removed have no
  logged results.
- **M1, read-only link.** Pipe up, `hello` and `get_heights` work. The app shows "Connected" and its map matches the
  game's terrain (compare a few hundred samples). Proves the addresses with zero writes. **Link passed 2026-11-05** on
  ModTest (4097 px), comparing every cell: 0 differed from `terrain.png` (largest 8.4e-7 ft, one 16-bit step is 1.7e-4 ft).
  After painting a hill, 50,738 cells differed by up to 8 ft, all inside box (3176, 1711, 3372, 2026), and the change
  counter went from 1 to 60. The change counter was already 1 at the first check; whether a load sets it isn't known.
  The app's "Connected" indicator is still to do.
- **M2, write heights.** `set_heights` with a straight gradient patch: the editor redraws, Backspace undoes it, `s` saves
  it, and the saved PNG matches the app's math within one 16-bit step. **Built 2026-11-05 (mod 0.2.0).** Results on
  ModTest, 2026-11-05, with keys sent by script:
  - Undo passed: `bump 2048 2048` took the cell from 0.000957 to 5.000957 ft, one Backspace put it back to 0.000957.
  - Save passed: before `s`, `check` on `gradient 1` found 29,871 of 35,192 cells off, so the gradient really changed
    the ground. After `s`, 0 cells were over one step (largest 8.5e-5 ft, one step is 1.7e-4 ft), and the whole map
    in memory matched the saved files.
  - On screen passed (Jackson watching): `bump 2048 2048 --radius 60 --height 20` showed the hill at once, with
    "Applied from MX Simulator Tools" in the panel, and his Backspace removed it (height back to 0.000957 ft). The
    height range (`maxFt`) widened to 20.0 and stayed there after the undo: Backspace doesn't narrow it back.
  - Pre-flush passed (README step 2, open question 7): Jackson painted a Gradient Linear Paint stroke (mode 5, 8,413
    cells, box 2185, 2002, 2284, 2093), then `bump` raised a hill on the rock (box 1928..2168, no overlap). First
    Backspace: only the hill went, the stroke was unchanged cell for cell. Second Backspace: the stroke went, and the
    whole map matched the saved files again. Each Backspace adds 1 to the change counter.
  - Riding passed (README step 4, open question 3): with the 20 ft hill raised by the mod, Jackson TABbed out and
    rode it; the bike felt it. Physics reads the same float heights, no extra step needed.
  - The terrain pointer stays null until TAB opens the editor (`hello` says "no track loaded" before that). The change
    counter read 1 with no edit made, and Backspace also adds 1 to it.
  - What `s` did on ModTest: rewrote `terrain.png`, `terrain.hf`, `edinfo`, `statues`, `billboards`, `decals`,
    `timing_gates`, `tilemap` and both `shading` files. It dropped the malformed last line of `statues` (`s`) and changed
    `billboards` from CRLF to LF. No other lines in those two changed, and `terrain.hf`, `edinfo`, `decals`,
    `timing_gates` and `tilemap` came out byte-identical.

  How it's built:
  - A big edit goes as strips of at most 262144 floats (1 MB), one strip per frame. The first strip carries FIRST
    (step 1b, flush the user's pending edit), the last FINAL (step 5, one undo snapshot and the status line), so one
    Backspace undoes the whole edit however many strips it took.
  - The mod refuses a strip while a mouse button is down, the track is locked, the rectangle leaves the grid, or a
    height isn't finite. A refusal mid-edit leaves the earlier strips applied without a snapshot; the app resends.
  - The redraw rectangle gets one cell of margin (clamped to the grid), since the cells around a patch get new normals.
  - `MXMOD_READONLY=1` makes every write a log line; `hello` reports `readonly`.
  - Pipe requests are read into one buffer the pipe thread owns; the game thread reads it in place (no copy) and the
    pipe thread doesn't reuse it until the game thread has answered or the request was taken back.
- **M3, curved gradients live.** "Apply in game" for curved gradients, with optional live preview while dragging
  (throttled to about 10 patches a second, each one undoing the previous preview before applying). **Passed
  2026-11-05 on ModTest (mod 0.3.0).** How it's built (`CurveInGame` in `core/game_link.py`, `edCurveGame` in the bridge,
  the Live In Game and Apply In Game rows in `web/curve-tool.js`, in Curve Path and Curve Profile):
  - The curve is painted with the same `paint_curve` as Bake Into Terrain, on the game's own ground: only the cells in
    its box are read from the game (`paint_curve` takes a window with an `origin`). The app's map and files don't change.
  - The ground under a preview is read once and kept, so each preview paints on the ground from before (Add never
    stacks), and moving the curve puts the old spot back. A preview writes with no FINAL, so no undo snapshot; the
    first one carries FIRST to flush the user's pending stroke. Apply writes FINAL: one undo step. Cancel (Live off,
    another track opened, leaving the Gradients tool) writes the saved ground back, also with no snapshot.
  - Before every write the whole saved area is read back and compared with what was left there. Any difference (a
    Backspace or a brush in the game) stops the preview without writing, so the app never overwrites the game's edit.
  - The app refuses unless the game was started with `--track` naming the open track's folder, and the map size matches.
  - The pipe stays open only while a preview is up; the status line ("Game: ModTest open in the editor") checks every
    5 s otherwise, since the mod takes one client at a time.

  Results:
  - Apply and Live worked in the game, and one Backspace removed each Apply. The curve stays in the app.
  - Undo (open question 7): after a run of Live drags and two Applies, the game took 38 writes (change counter 14 to
    52), so about 30 previews. The ground still came back exactly to the saved files with Backspace, which the 8-step
    ring couldn't do if previews took snapshots. Previews leave no undo step.
  - Safety: Backspace in the game during Live removed the preview (back to the ground from before it, since previews
    never snapshot), and the app stopped Live.
  - Not done: the "changed in the game" message on screen wasn't read back to Claude; only the ground was checked.
- **M4, gradient list sync.** Read the game's gradient array so the app shows what the editor has, and push edits back
  through the game's realloc. Only after M2 is solid. **Passed 2026-11-05 on ModTest (mod 0.4.0), in place only.**
  - The layout was checked first by reading mx.exe's memory from outside (read access only): all 93 records matched
    `edinfo`, points and segment types included.
  - `get_gradients` returns the list, the selected gradient and point, and each gradient's room. `set_gradient` rewrites
    one gradient's ends, points and types in place, refused when the points don't fit its room, while a mouse button
    is down, or when the points aren't finite, starting at 0 and going forward. If the editor's picked point would no
    longer exist, it's cleared. The status line says "Gradient changed by MX Simulator Tools". No undo snapshot: the
    game's undo only holds heightmaps.
  - The app: **Get From Game** and **Send To Game** in the Track Editor's Gradients panel (`edGradientsFromGame`,
    `edGradientToGame`). Get replaces the app's list as one undo, keeping ids by position. Send matches by position and
    refuses unless both lists have the same count. CLI: `game_link gradients --track <folder>` compares with `edinfo`.
  - Results: the game's 93 gradients matched `edinfo`. Jackson moved two gradients in the game without saving; the CLI
    named both (gradient 87's profile length also followed, the game's rule for an untouched gradient) and Get From
    Game moved them on the app's map. He reshaped gradient 5 in the app (21 points) and Send To Game showed it in the
    game's Gradient Edit; the log said `set gradient 4: 21 points`.
  - Not done yet: adding gradients or points past a gradient's room (needs the realloc's arguments, from the Mx Sim
    Project), and checking that `s` saves a sent gradient to `edinfo` (expected: the game writes its in-memory list).
- **M5-M9, the app's tools inside the game's editor.** Hooks on the editor's draw and key handler, a draw list and
  panel from the app, game events to the app. Spec, addresses and checks: `docs/ingame-tools.md`.

## Open questions (check before or during M0-M2)
1. List the regions in the tables at 0x140218e20 / 0x140177bc0 and confirm none cover the editor's heap data or the
   gradient globals. (They're checked even offline; offline nothing is known to react, but confirm.)
2. Meaning of the lock flag set at 0x1400e938a.
3. ~~Confirm physics and the bike read the same float heights, so riding (TAB out of the editor) sees the edit at
   once.~~ Confirmed in M2 (2026-11-05).
4. Confirm the "Update needed!" message (bit 4 of `+0x49570`) clears after a save and doesn't need extra work from us.
5. Confirm `wglSwapBuffers` and the editor's input/draw run on the same thread (expected: single-threaded main loop).
6. What the game does if the pipe sends a patch while the editor is mid-stroke (mouse held): v1 refuses `set_heights`
   while a mouse button is down in the editor (`0x1417e25e4 != 0`).
7. ~~Undo with the two snapshot calls (steps 1b and 5): test that one Backspace removes exactly the mod's edit, and that
   the ring of 8 isn't filled up by live preview (preview should snapshot only once, when committed).~~ Confirmed in M2
   (one Backspace per edit, stroke kept) and M3 (about 30 previews left no undo steps).

## Evidence
| addr | what |
|---|---|
| 0x1400ba740 | code hash over 0x140001540 + 0xfbc00, compared with 0x1417e26e0 |
| 0x1400bb970 / 0x1400bb0e0 | round-robin region check against tables 0x140218e20, 0x140177bc0 |
| 0x1400bbb10 | tamper flag accumulator (also called from bike physics 0x140034850, 0x14003b320) |
| 0x140172e6d | main loop: runs the checks, stores flags at 0x14356c2c4..cc |
| 0x140017830 | mark 128-cell terrain blocks dirty (bitmap at terrain + 0x38) |
| 0x140017500 | per-frame terrain update (8 calls per editor frame in 0x1400613b0) |
| 0x14005b060 | undo snapshot (ring of 8, baseline copy) |
| 0x140066bf0 | mouse-drag handler; case 4 is the height write sequence copied above |
| 0x1400e938a | track loader sets the edit lock |
| 0x1400d5e65 | `/forcereload`: sets 0x144a1147c = 1, "Track will be reloaded on next restart" |
