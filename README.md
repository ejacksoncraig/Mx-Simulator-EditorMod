# MX Simulator Tools editor mod (offline only)

An `opengl32.dll` that MX Simulator loads from its own folder. Inside `mx.exe --editor` it adds tools to the game's
track editor: curved gradients, gradient length, profile sections, brush shapes, delete / export / place gradients and
radial sweep. It also lets MX Simulator Tools (Jackson Craig's track-building app, a separate repo) read and write the
editor's terrain live over a local pipe. Its version shows faintly at the top of its panel.

**Offline only.** The mod is a plain
pass-through unless the command line has `--editor` and mx.exe is a build it has an address table for. In every
session it's loaded in, with or without `--editor`, the online tripwire (`src/tripwire.c`) refuses every network call
and offers to close the game. So don't join online races with it installed: run `uninstall.ps1` first.

## Working alongside another opengl32.dll (shaders, GL mod loaders)

Both mods want to be `opengl32.dll`, so they chain:

1. `install.ps1` finds a foreign `opengl32.dll` in the game folder (ours is recognised by its `mxmod_version` export)
   and renames it to `opengl32_mxs.dll`.
2. The game loads ours. `src/proxy.c` loads `opengl32_mxs.dll` if it's beside us, else the system opengl32, and
   forwards every GL export to it. We only hook `wglSwapBuffers` and `glClear` on the way through.
3. `uninstall.ps1` removes ours and renames `opengl32_mxs.dll` back.

Online, the other mod should run on its own: the tripwire blocks the network whenever our DLL is loaded. Keep the two
as separate DLLs rather than merging them.

## Setup (once)

1. Visual Studio Build Tools 2022 with the C++ workload:
   `winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`
2. Optional, so builds install without an admin prompt. In an **admin** PowerShell:
   `icacls "C:\Program Files (x86)\MX Simulator" /grant "${env:USERNAME}:(OI)(CI)M"`
3. A scratch track: copy `%LOCALAPPDATA%\MX Simulator\BetterBlank` (or any track) to `ModTest`. Never test the mod on
   a track you care about.
4. For the tests and tools: `python -m venv .venv` then `.venv\Scripts\python.exe -m pip install -r requirements-dev.txt`.

## Build, install, run

```
build.bat
powershell -ExecutionPolicy Bypass -File install.ps1
"C:\Program Files (x86)\MX Simulator\mx.exe" --editor --track ModTest
```

Press TAB until the editor screen shows. The tools are in the panel at the top right. The log
(`%LOCALAPPDATA%\MX Simulator\mxtools\log.txt`) gets its first lines at the game's first frame, which can take a while.
Set `MXMOD_NOHOOKS=1` before starting to leave the game's code untouched, or `MXMOD_READONLY=1` to turn every write
into a log line.

Tests: `.venv\Scripts\python.exe -m pytest tests -q`. Players' zip: `powershell -ExecutionPolicy Bypass -File
package.ps1` makes `dist\mxtools-editor-mod-<version>.zip` (the DLL, install.bat, uninstall.bat,
[package/README.txt](package/README.txt)).

## What it touches

- Never mx.exe's file. Inside `--editor` it patches the editor's draw and key handler in memory (`src/ingame.c`),
  checking the original bytes first and putting them back on unload.
- It reads the editor's heightmap and gradients and writes them the way the game's brush does: heap data only, then
  the game's redraw flag and undo snapshot.
- It listens on `\\.\pipe\mxmod` for local connections only.
- Its files: `%LOCALAPPDATA%\MX Simulator\mxtools\` (`log.txt`, `profile-sections.json`, `gradients\`), and
  `mx-tools-curves.json` beside each track.

## Where to read next

- [docs/design.md](docs/design.md): the rules, what the mod touches in memory, the pipe protocol, the tripwire.
- [docs/ingame-tools.md](docs/ingame-tools.md): the hooks, the game's view, input, drawing and widget functions, and
  how each tool works.
- [docs/builds.md](docs/builds.md): the game builds the mod knows and how to port a new one (`tools/port_build.py`).
- [docs/game/](docs/game/): how the game's own editor works and its gradient and brush math.
- [CLAUDE.md](CLAUDE.md): the same map, for working on the mod with Claude Code.

## Files

- `src/proxy.c`: loads the real (or chained) opengl32 and forwards every export; hooks `wglSwapBuffers` and `glClear`.
- `src/tripwire.c`: the online tripwire, armed from DllMain in every session.
- `src/game.c`: the known build, reading the editor state and writing heights on the game thread.
- `src/ingame.c`: the inline hooks (prologue check, trampolines) and what they draw and catch.
- `src/builds.c`: one address table per game build.
- `src/curvetool.c`, `src/gradtool.c`, `src/pen.c`, `src/curvemath.c`, `src/profile.c`, `src/library.c`,
  `src/sweep.c`, `src/curvefile.c`: the in-game tools. `src/scene.c`: shapes the app asks the mod to draw.
- `src/pipe.c`: the pipe server thread. `src/log.c`: the log. `src/protocol.h`: the pipe protocol, shared with the
  app's client.
- `tools/gen_exports.py` + `tools/exports.txt`: generate the export table and jump stubs (`src/*_gen*`,
  `opengl32.def`, `stubs_*`). `python tools/gen_exports.py --from-system` re-reads System32's opengl32.dll.
- `build.bat` (MSVC) and `build.sh` (mingw-w64 compile check). `tests/`: pytest, with native drivers.
