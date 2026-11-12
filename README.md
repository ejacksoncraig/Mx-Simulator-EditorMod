# MX Simulator Tools editor mod (offline only)

An `opengl32.dll` that MX Simulator loads from its own folder. Inside `mx.exe --editor` it adds tools to the game's
track editor: curved gradients, gradient length, profile sections, brush shapes, delete / export / place gradients,
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

## What it touches

- Never mx.exe's file. Inside `--editor` it patches the editor's draw and key handler in memory (`src/ingame.c`),
  checking the original bytes first and putting them back on unload.
- It reads the editor's heightmap and gradients and writes them the way the game's brush does: heap data only, then
  the game's redraw flag and undo snapshot.
- APP LINK: it listens on `\\.\pipe\mxmod` for local connections only, for MX Simulator Tools (see below).
- Its files: `%LOCALAPPDATA%\MX Simulator\mxtools\` (`log.txt`, `profile-sections.json`, `gradients\`), and
  `mx-tools-curves.json` beside each track.

## Where to read next

- [DEVELOPERS.md](DEVELOPERS.md): a one-page brief on how it works and how another mod can run alongside it.
- [docs/builds.md](docs/builds.md): the game builds the mod knows and how their addresses were found.
- [.claude/skills/mx-editor-mod/](.claude/skills/mx-editor-mod/SKILL.md): for whoever maintains the mod. A Claude Code
  skill (loaded automatically in this repo) with the game's internals, the in-game tools' findings, and
  `port_build.py` for porting to a new game build. It isn't part of the DLL, so a code review can skip it.

## APP LINK: the code that's only for MX Simulator Tools

MX Simulator Tools is a separate track-building app (its own program and repo, not in here). This mod lets it read
and write the editor's terrain live over a local pipe. That code isn't needed by the in-game tools, and every piece of
it is marked `APP LINK` in the source, so a search finds all of it:

- `src/pipe.c`: the pipe server thread (local connections only).
- `src/protocol.h`: the message format, kept in step with the app's client.
- `src/scene.c`: shapes the app asks the mod to draw over the map.
- `src/game.c`: the request handlers (`handle_*`, `service_request`, `mod_request`) and the `pipe_start()` call.
- `src/ingame.c`: the mouse and key event queue the app reads (`queue`, `ingame_events_json`).

Code comments that name `core/*.py` or `web/*.js` also mean the app's files: the curve tool was ported from the app
line for line, and the two give the same results.

## Files

- `src/proxy.c`: loads the real (or chained) opengl32 and forwards every export; hooks `wglSwapBuffers` and `glClear`.
- `src/tripwire.c`: the online tripwire, armed from DllMain in every session.
- `src/game.c`: the known build, reading the editor state and writing heights on the game thread.
- `src/ingame.c`: the inline hooks (prologue check, trampolines) and what they draw and catch.
- `src/builds.c`: one address table per game build. `src/brush_stub.asm`: the brush-shape patch.
- `src/curvetool.c`, `src/gradtool.c`, `src/pen.c`, `src/curvemath.c`, `src/profile.c`, `src/library.c`,
  `src/stamptool.c` (rut stamps, shelved), `src/decaltool.c` (decal editing in Decals mode), `src/statuetool.c` (the statue group and its Snap to Gradient), `src/bermpreview.c`, `src/sweep.c`, `src/curvefile.c`, `src/json.c`: the in-game tools. `src/log.c`: the log.
- Generated, don't review line by line: `src/exports_gen.h`, `src/opengl32.def`, `src/stubs_masm.asm` are one
  "jump to the real opengl32 function" stub per GL export, written by `tools/gen_exports.py` from `tools/exports.txt`
  (`--from-system` re-reads System32's opengl32.dll).
- `build.bat` (MSVC), `install.ps1`, `uninstall.ps1` (for development).
- `package.ps1`: builds and makes the players' zip, `dist\mxtools-editor-mod-<version>.zip` (the DLL and the two
  .bat files), and prints its SHA-256 to publish with the release. Players extract it and double-click `install.bat`.
- `package/install.bat`, `package/uninstall.bat`: for players. They use the DLL beside them, or `..\build\` from a clone. Install only
  copies a DLL that is this mod, keeps another opengl32.dll as `opengl32_mxs.dll` (and puts it back if the copy
  fails); uninstall only deletes the DLL if it is this mod. Both refuse while the game is running.
