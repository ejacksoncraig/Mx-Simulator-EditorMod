# Developer brief

## What it does

An `opengl32.dll` that MX Simulator loads from its own folder. Inside `mx.exe --editor` it adds tools to the game's
track editor (curved gradients, profile sections, brush shapes, gradient delete / export / place, radial sweep) and
draws them with the game's own widgets. It also serves the editor's terrain and gradients over a local pipe
(`\\.\pipe\mxmod`) for MX Simulator Tools, a separate track app (the code marked `APP LINK`). Outside `--editor`, or
on an unknown game build, it only passes GL calls through. Whenever it's loaded it blocks mx.exe's network calls (the
tripwire).

## How it does it

1. **Proxy** ([src/proxy.c](src/proxy.c)). Exports every opengl32 function and forwards it to the real one: to
   `opengl32_mxs.dll` if it sits beside us (another GL mod, renamed), else System32's, loaded at the first GL call
   rather than inside DllMain. Only `wglSwapBuffers` and
   `glClear` are intercepted. They give us a per-frame callback on the game thread (`mod_on_frame`), and all game work
   happens there.
2. **Build detection** ([src/builds.c](src/builds.c)). The running exe's SHA-256 picks an address table: about 50
   RVAs for editor globals, game functions (malloc / realloc / free, redraw, undo snapshot, widgets, GL wrappers) and
   hook points. No match means no tools. [docs/builds.md](docs/builds.md) says how each address was found.
3. **Hooks** ([src/ingame.c](src/ingame.c)). Inline hooks on the editor's draw function and key handler. Each one
   checks the exact original prologue bytes before patching, jumps to our detour, calls the original through a
   trampoline, and puts the bytes back on unload. The brush loop gets a small patch ([src/brush_stub.asm](src/brush_stub.asm))
   for brush shapes.
4. **Writes** ([src/game.c](src/game.c)). Heights and gradients are changed the way the game's brush does it: heap
   data only, grown with the game's own allocator, then the redraw flag and one undo snapshot. Writes wait while the
   editor is locked or a mouse button is held.
5. **Tools** (`src/curvetool.c`, `gradtool.c`, `pen.c`, `curvemath.c`, `profile.c`, `sweep.c`). Pure math plus panel
   rows. They match the game's own gradient math.

## Using it from another mod's DLL

**Option A: built-in chain with detection (recommended, and the only option for an online-safe mod).** Keep the two
as separate DLLs and never merge ours into an online mod. Your mod stays `opengl32.dll` when it's installed alone.
When ours is also installed, yours becomes `opengl32_mxs.dll` (`install.ps1` renames it), and ours loads it and
forwards every GL call to it, so your rendering works unchanged.

Your mod can tell which setup it's in. Ours exports `mxmod_version`. If the module named `opengl32.dll` in the process
has that export, the editor mod is loaded and its tripwire is blocking mx.exe's network calls. Check this before your
online code runs (at the first frame, not in DllMain):

```c
static const char *editor_mod_version(void) /* NULL: not loaded, normal online session */
{
    HMODULE gl = GetModuleHandleW(L"opengl32.dll"); /* ours when chained, yours when alone */
    FARPROC f = gl ? GetProcAddress(gl, "mxmod_version") : NULL;
    return f ? ((const char *(*)(void))f)() : NULL;
}
```

When it returns a version, treat the session as offline: turn off your online features and tell the player
"MX Simulator Tools editor mod installed: online is blocked, run its uninstall to race online." That way the player
gets a clear message instead of connections that fail with no explanation. When it returns NULL, your mod works as it
always does.

**Option B: talk to it over the pipe (the APP LINK, built for MX Simulator Tools).** If your DLL needs the editor's terrain, connect to `\\.\pipe\mxmod` instead of
finding the memory yourself. Frames are `u32 length, u32 type, payload`, little-endian. `HELLO`, `GET_HEIGHTS` /
`SET_HEIGHTS`, `GET_GRADIENTS` / `SET_GRADIENT`, `SET_SCENE` (draw lines and markers over the map) and `GET_EVENTS`
(mouse and keys while your tool is on). It's all in [src/protocol.h](src/protocol.h). Writes are applied on the game
thread with undo, so you get the safety rules for free.

**Option C: lift the parts into your own DLL (offline-only mods only).** The pieces are independent:
- `src/builds.h` + `src/builds.c`: the address table and SHA-256 lookup. Call `mx_build_find`, then add RVAs to the
  exe's base.
- `hook_install` / `hook_remove` in `src/ingame.c`: a self-contained 64-bit inline hook (19-byte prologue check,
  14-byte absolute jump, trampoline).
- `game_write_heights` in `src/game.c`: the write path to copy if you change terrain.
- Run your work from a per-frame callback on the game thread, like `mod_on_frame`. Never from DllMain or another
  thread.
- If your DLL goes in place of ours, bring `src/tripwire.c` too and keep it armed, so the game can't go online with
  the editor patched.

Build with `build.bat` (MSVC). [docs/builds.md](docs/builds.md) covers the known game builds and porting a new one.
