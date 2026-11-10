@echo off
rem MX Simulator Tools editor mod: copies opengl32.dll into the game folder. Offline editor use only.
setlocal

rem The mod's DLL: beside this script (a players' zip), else the repo's build folder (run from a clone).
rem Only ever install this mod: the DLL must be ours (it has the mxmod_version export).
set "SRC=%~dp0opengl32.dll"
if not exist "%SRC%" set "SRC=%~dp0..\build\opengl32.dll"
if not exist "%SRC%" goto nodll
findstr /m "mxmod_version" "%SRC%" >nul
if errorlevel 1 goto nodll

rem The game folder needs admin to write: ask for it once.
net session >nul 2>&1
if errorlevel 1 goto elevate

rem Where the game is: where its installer says (its uninstall entry), the usual install folders, or ask.
set "GAME="
for %%k in ("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\MX Simulator" "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MX Simulator" "HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MX Simulator") do call :fromreg %%k
if defined GAME if exist "%GAME%\mx.exe" goto found
set "GAME=%ProgramFiles(x86)%\MX Simulator"
if exist "%GAME%\mx.exe" goto found
set "GAME=%ProgramFiles%\MX Simulator"
if exist "%GAME%\mx.exe" goto found
set "GAME="
echo Couldn't find MX Simulator. To find its folder: right-click your MX Simulator shortcut, Open file location,
echo then click the address bar at the top and copy it.
set /p "GAME=Paste the folder MX Simulator is installed in (the one with mx.exe): "
if not defined GAME goto nogame
set "GAME=%GAME:"=%"
rem A path to mx.exe, or a whole shortcut target ("E:\MX Simulator\mx.exe" --editor), works too: keep what's before mx.exe.
set "GAME=%GAME:mx.exe=|%"
for /f "delims=|" %%a in ("%GAME%") do set "GAME=%%a"
if "%GAME:~-1%"==" " set "GAME=%GAME:~0,-1%"
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
if not exist "%GAME%\mx.exe" goto nogame
:found
echo Game folder: "%GAME%"
tasklist /fi "imagename eq mx.exe" | find /i "mx.exe" >nul
if not errorlevel 1 goto running

set "RENAMED="
if not exist "%GAME%\opengl32.dll" goto copy
findstr /m "mxmod_version" "%GAME%\opengl32.dll" >nul
if not errorlevel 1 goto copy
rem Another opengl32.dll is there (MXS Shaders?): keep it as opengl32_mxs.dll, which the mod loads.
if exist "%GAME%\opengl32_mxs.dll" goto both
ren "%GAME%\opengl32.dll" opengl32_mxs.dll
if errorlevel 1 goto failed
set "RENAMED=1"
echo Kept your existing opengl32.dll as opengl32_mxs.dll; the mod loads it, so it keeps working.

:copy
copy /y "%SRC%" "%GAME%\opengl32.dll" >nul
if errorlevel 1 goto failed
echo Installed. Start the game with:  mx.exe --editor   and open any track.
echo Online play is blocked while the mod is installed: run uninstall.bat to play online.
pause
exit /b 0

:elevate
rem The script's path goes through an environment variable, never into the command text, so no folder name can
rem break or change the command.
set "MXMOD_SELF=%~f0"
powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath $env:MXMOD_SELF"
exit /b 0
:fromreg
rem The folder of the uninstaller the game's installer registered under key %1, if there is one.
if defined GAME exit /b 0
for /f "tokens=2,*" %%a in ('reg query %1 /v UninstallString 2^>nul ^| find "UninstallString"') do set "UNINST=%%b"
if not defined UNINST exit /b 0
for %%f in ("%UNINST:"=%") do set "GAME=%%~dpf"
set "UNINST="
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
if not exist "%GAME%\mx.exe" set "GAME="
exit /b 0
:nogame
echo mx.exe wasn't found there. Check the folder and run this again.
pause
exit /b 1
:running
echo Close MX Simulator first, then run this again.
pause
exit /b 1
:both
echo Both opengl32.dll and opengl32_mxs.dll are in the game folder and neither is this mod. Sort them out by hand.
pause
exit /b 1
:failed
echo Couldn't copy opengl32.dll into "%GAME%".
rem Put the other mod's DLL back under its own name, so a failed install leaves the folder as it was.
if defined RENAMED ren "%GAME%\opengl32_mxs.dll" opengl32.dll
if defined RENAMED echo Your original opengl32.dll was put back.
pause
exit /b 1
:nodll
echo No MX Simulator Tools editor mod opengl32.dll found next to this script or in ..\build. Nothing was changed.
echo From a download: extract the whole zip first (right-click it, Extract All), then run install.bat from that folder.
echo From the repo: run build.bat first.
pause
exit /b 1
