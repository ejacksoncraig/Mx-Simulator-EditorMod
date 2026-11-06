@echo off
rem Build build\opengl32.dll with MSVC (Visual Studio Build Tools, "Desktop development with C++").
rem Run from anywhere: it finds the compiler itself. Output: build\opengl32.dll (+ .pdb for x64dbg).
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto have_compiler
rem Not in an if block: %VSDIR% would expand before it's set. "call": for /f's cmd /c strips the quotes around a path with (x86) in it.
for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio Build Tools with the C++ workload not found. See README.md, "Setup".
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_compiler

python tools\gen_exports.py || exit /b 1
if not exist build mkdir build

ml64 /nologo /c /Fobuild\stubs.obj src\stubs_masm.asm || exit /b 1
ml64 /nologo /c /Fobuild\brush_stub.obj src\brush_stub.asm || exit /b 1
cl /nologo /O2 /W4 /WX- /Zi /MT /LD /Fobuild\ /Fdbuild\ /Febuild\opengl32.dll ^
   src\proxy.c src\game.c src\pipe.c src\log.c src\ingame.c src\scene.c src\json.c src\curvefile.c src\curvetool.c src\gradtool.c src\sweep.c src\pen.c src\curvemath.c src\profile.c src\library.c src\tripwire.c src\builds.c build\stubs.obj build\brush_stub.obj ^
   /link /DEF:src\opengl32.def /PDB:build\opengl32.pdb bcrypt.lib kernel32.lib user32.lib || exit /b 1

echo Built %~dp0build\opengl32.dll
