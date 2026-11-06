# MX Simulator Tools editor mod

An `opengl32.dll` that MX Simulator loads from its own folder. Inside `mx.exe --editor` it adds tools to the game's own
track editor (curved gradients, profile sections, brush shapes, delete / export / place gradients, radial sweep), and it
answers MX Simulator Tools (Jackson Craig's Python track app, a separate repo) over a local pipe. C, built with MSVC.
Offline only. Owner: Jackson Craig. Personal folders, if any, are in `CLAUDE.local.md` (not in git).

## Commands

- Build: `build.bat` (finds MSVC itself) makes `build\opengl32.dll`. `build.sh` is the mingw-w64 compile check.
- Install into the game folder (game closed): `powershell -ExecutionPolicy Bypass -File install.ps1`; undo with
  `uninstall.ps1`. Then `"C:\Program Files (x86)\MX Simulator\mx.exe" --editor`, open a track, TAB into the editor.
  The mod writes its log at the game's first frame, which can be minutes after launch.
- Log: `%LOCALAPPDATA%\MX Simulator\mxtools\log.txt` (replaced each start).
- Tests: `python -m venv .venv`, `.venv\Scripts\python.exe -m pip install -r requirements-dev.txt`, then
  `.venv\Scripts\python.exe -m pytest tests -q` (builds the pure C parts with MSVC; skipped without it).
- Zip for players: `package.ps1` makes `dist\mxtools-editor-mod-<version>.zip`.
- Version: `MXMOD_VERSION` in `src/protocol.h`; bump it with every change that gets installed. It shows in the panel.

## Layout

- `src/proxy.c` forwards every GL export to the real (or chained) opengl32; `src/tripwire.c` blocks the network.
- `src/game.c`: build detection, the editor's state, height and gradient writes, the pipe requests (game thread).
- `src/ingame.c`: the inline hooks, our panel's drawing and input, the brush and radial patches.
- `src/curvetool.c` (panel rows, curve tool, profile sections), `src/gradtool.c` (gradient tools, radial sweep),
  `src/pen.c`, `src/curvemath.c`, `src/profile.c`, `src/sweep.c`, `src/library.c`, `src/curvefile.c`, `src/json.c`.
- `src/builds.c`: one address table per game build. `tools/port_build.py` finds the addresses in a new build.
- `package/`: what goes in the players' zip (README.txt, install.bat, uninstall.bat).
- `tests/`: pytest; native drivers next to them.

## Knowledge (read the doc before working in its area)

- `docs/design.md`: rules, what the mod touches in memory, the app protocol, milestones M0-M4, the tripwire.
- `docs/ingame-tools.md`: the in-game tools: hooks, the game's view, input, drawing and widget functions, and every
  tool's findings (section 8 standalone tools, section 9 gradient tools and the panel's look).
- `docs/builds.md`: the builds the mod knows and how to port a new one.
- `docs/game/`: how the game's own editor works (using it, its panel and gradients, its gradient and brush math).
  These are copies of MX Simulator Tools' docs: when one side learns something about the game, update the other too.
- Code comments that name `core/*.py` or `web/*.js` mean MX Simulator Tools' files: the curve tool was ported from
  the app line for line, and the two must keep giving the same results.

## Ground rules

- **Offline only.** The tools only run with
  `--editor` on a known build. The online tripwire is armed in every session the DLL is loaded in and must stay that
  way: every send and connect refused, failing closed if it can't arm (`docs/design.md` rule 2).
- Never change `mx.exe` on disk; the same file is used online. Read-only analysis is fine. In memory the mod may patch
  code and hook functions (offline only), checking the bytes first and putting them back when it unloads.
- Only the DLL writes game memory; the app never does, it sends the mod commands.
- Respect the editor's edit lock and a held mouse button; grow or free game data only with the game's own
  malloc / realloc / free.
- Never guess an address: disassemble, or match it with `tools/port_build.py`. Build and check new features on the most
  recent snapshot first; the other builds are confirmed later.
- Match the game's own math (`docs/game/track-editor-gradients.md`) so tools behave the way MX does.
- The UI is ours: readable and intuitive first, grouped under headers with hints. It doesn't have to copy the old
  editor screen, but it draws with the game's own widgets.
- Another `opengl32.dll` (a shader mod, a GL mod loader) is chained, not replaced: `install.ps1` keeps it as
  `opengl32_mxs.dll` and `src/proxy.c` loads it. Keep that working.
- Keep `src/protocol.h` and MX Simulator Tools' `mxtools/core/game_link.py` in step (its tests read this file when this
  repo is cloned beside it, or `MXMOD_DIR` points here), and `mx-tools-curves.json` in the format the app writes.
- Test only on a scratch copy of a track (for example `ModTest`, a copy of `BetterBlank`).

## Working style

- Explain the why briefly, skip filler, no em dashes in prose.
- Branch per feature, `git pull` before starting, push when done.
- Prove changes: a pytest case in `tests/` for pure code, and a check in the game on the scratch track (the log shows
  what happened).
- When you learn something durable about the game or the mod, write it into the matching `docs/` file in the same
  commit.
