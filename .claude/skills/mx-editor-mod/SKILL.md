---
name: mx-editor-mod
description: How the MX Simulator editor mod (an opengl32.dll proxy) works inside mx.exe's track editor - its hooks, the game's view, input, drawing and widget functions, the editor's gradients and brush math, and how to port the mod's ~50 addresses to a new game build with port_build.py. Use when porting the mod to a new mx.exe build, adding or changing an in-game tool, debugging a hook or a write to the editor, or answering how the game's own editor behaves.
---

# MX Simulator editor mod

The repo's `README.md` and `DEVELOPERS.md` cover what the mod is and how the code is laid out. This skill holds the
deeper knowledge: what was learned about mx.exe by disassembly, and the tool to port the addresses.

## Open the file for the job

| Job | Read |
|---|---|
| A new mx.exe build came out (the tools say "version isn't supported") | "Porting a build" below, then `docs/builds.md` |
| Adding or changing an in-game tool, a hook, drawing or input | [ingame-tools.md](ingame-tools.md): section 2 (the game's view, input, draw pass, hook points), 3 (how the mod does it), 8 and 9 (each tool's findings) |
| How the game's own editor is used (modes, keys, saving) | [game/editor.md](game/editor.md) |
| The game's gradient panel, widgets, colours, gradient records | [game/editor-gradients-ui.md](game/editor-gradients-ui.md) |
| The game's gradient and brush height math (tools must match it) | [game/track-editor-gradients.md](game/track-editor-gradients.md) |
| Decals: data, paint order, edit calls, Decals mode (15) and its panel, the f5 axis | [game/decals.md](game/decals.md) |
| Statues: the list, the record, the update call, Snap to Gradient's math, the mod's statue group | [game/statues.md](game/statues.md) |
| The game's sliders (Radius, Focus, Granularity...) in a new build | `find_sliders.py`, and "The brush sliders" in `docs/builds.md` |

## Ground rules

- **Offline only.** The tools only turn on with `--editor` on a build in `src/builds.c`. The tripwire
  (`src/tripwire.c`) is armed whenever the DLL is loaded and must stay that way: it refuses every send and connect,
  and if it can't arm, the game closes.
- **Never change mx.exe on disk**: the same file is used online. Reading it (disassembly) is fine. In memory the mod
  may patch code and hook functions, checking the original bytes first and putting them back when it unloads.
- **Never guess an address.** Disassemble, or match it with `port_build.py`. A wrong address corrupts the game.
- Write game data only the way the game does: its own malloc / realloc / free, then the redraw flag and one undo
  snapshot (`game_write_heights` in `src/game.c`). Respect the editor's edit lock and a held mouse button.
- Match the game's own math (`game/track-editor-gradients.md`) so tools behave the way MX does.
- Another `opengl32.dll` (a shader mod) is chained as `opengl32_mxs.dll`, not replaced. Keep that working.
- Test only on a scratch copy of a track (for example `ModTest`, a copy of `BetterBlank`).

## Dev loop

1. `build.bat` (finds MSVC itself) makes `build\opengl32.dll`.
2. With the game closed: `powershell -ExecutionPolicy Bypass -File install.ps1` (undo: `uninstall.ps1`).
3. `"C:\Program Files (x86)\MX Simulator\mx.exe" --editor --track ModTest`, TAB into the editor.
4. Read `%LOCALAPPDATA%\MX Simulator\mxtools\log.txt` (replaced each start; first lines at the game's first frame,
   which can take minutes). It shows the build found, the tripwire state and whether each hook installed.
5. Bump `MXMOD_VERSION` in `src/protocol.h` for every build that gets installed; it shows in the panel.

`MXMOD_NOHOOKS=1` leaves the game's code untouched; `MXMOD_READONLY=1` turns every write into a log line.

## Porting a build

1. `pip install capstone pefile`, then
   `python .claude/skills/mx-editor-mod/port_build.py "<mx.exe 1.0>" "<new mx.exe>"`. Read-only. The first exe must
   be build 1.0 (SHA-256 `54676784...bada392`): the script's starting addresses are 1.0's.
2. Anything that comes out `None` or `ambiguous` gets checked by hand in a disassembler. `docs/builds.md` shows how
   the last two ports handled theirs (a tiny wrapper with near-identical copies, a function whose stack frame shrank).
3. Check what the script can't: the terrain struct offsets (`+0x10c8 .. +0x49574`) and the brush loop's stack slots
   (`[rsp+0x1210]`, `[rsp+0x1218]`, used by `src/brush_stub.asm`), as `docs/builds.md` describes.
4. Add the table to `src/builds.c` with the new exe's SHA-256, build, and check in the editor: the log says both hooks
   installed, and heights and gradients match the track's files.
5. Add the build to `docs/builds.md`.

## About the references in these files

These notes were written while the mod lived in a bigger repo. Some files they mention aren't here:

- `docs/design.md`, `tests/`, `CLAUDE.md`: the old repo's design notes and tests. The rules that mattered are above.
- `core/*.py`, `web/*.js`, `mxtools/...`, `game_link`: MX Simulator Tools, the separate track app the mod's APP LINK
  code talks to. The curve tool was ported from it line for line.
- Addresses like `0x140066bf0` are virtual addresses in build 1.0 (base `0x140000000`); subtract the base for the RVA.

When you learn something durable about the game or the mod, add it to the matching file here.
