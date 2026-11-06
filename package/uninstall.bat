@echo off
rem Takes the MX Simulator Tools editor mod out of the game folder, and puts back an opengl32.dll it kept aside.
setlocal

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
echo Game folder: %GAME%
tasklist /fi "imagename eq mx.exe" | find /i "mx.exe" >nul
if not errorlevel 1 goto running

if not exist "%GAME%\opengl32.dll" goto restore
findstr /m "mxmod_version" "%GAME%\opengl32.dll" >nul
if errorlevel 1 goto notours
del "%GAME%\opengl32.dll"
echo Removed the editor mod.

:restore
if not exist "%GAME%\opengl32_mxs.dll" goto done
ren "%GAME%\opengl32_mxs.dll" opengl32.dll
echo Put your original opengl32.dll (MXS Shaders?) back.
:done
echo Done. The game is as it was.
pause
exit /b 0

:elevate
powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
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
:running
echo Close MX Simulator first, then run this again.
pause
exit /b 1
:nogame
echo mx.exe wasn't found there. Check the folder and run this again.
pause
exit /b 1
:notours
echo The opengl32.dll in the game folder isn't this mod, so it was left alone.
pause
exit /b 1
