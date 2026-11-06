#!/bin/sh
# Build mod/build/opengl32.dll with mingw-w64 (Linux cross-compiler, or MSYS2 on Windows).
# Same result as build.bat; used for compile checks in the cloud sandbox.
set -e
cd "$(dirname "$0")"
python3 tools/gen_exports.py
mkdir -p build
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -shared -o build/opengl32.dll \
  src/proxy.c src/game.c src/pipe.c src/log.c src/ingame.c src/scene.c src/json.c src/curvefile.c src/curvetool.c src/gradtool.c src/sweep.c src/pen.c src/curvemath.c src/profile.c src/library.c src/tripwire.c src/builds.c src/brush_stub.S src/stubs_gas.S src/opengl32.def \
  -lbcrypt -luser32 -static-libgcc -Wl,--enable-stdcall-fixup
echo "Built $(pwd)/build/opengl32.dll"
