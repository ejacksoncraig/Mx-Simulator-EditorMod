MX Simulator Tools - editor mod
================================

Extra tools inside MX Simulator's track editor:
- Curve Path and Curve Profile: curved gradients, drawn and shaped in the editor, previewed live on the ground and
  applied with one Backspace to undo.
- Gradient Length (in Gradient Placement): drag a shaped gradient's end to Extend or Stretch its profile.
- Profile sections (in Gradient Edit and Curve Profile): save a piece of a profile (a 3 ft jump, a table top...) and
  paste it onto any gradient. Your sections are kept for every track.
- Brush shapes (in the brush modes): Square or Oval, at any angle, instead of only a circle.
- Delete Gradient (in Gradient Placement and Edit, or the Del key), with Undo Delete. The ground it already shaped
  stays as it is.
- Export Gradient and Saved Gradients: save one gradient to a file in one click, then place it on any track (click
  the map where its start goes) or put it back where it was.
- Radial Sweep (in Gradient Radial Paint and Add): sweep from 30 to 360 degrees instead of the game's 180, centred on
  the gradient's end or starting from it either way. The sweep is drawn on the map.
The tools are in the panel at the top right of the editor, under the mod's version number.

OFFLINE ONLY. While the mod is installed you can't play online, with or without --editor. The mod changes the game's
code while the editor runs, and the game reports that kind of change to the server, so as long as the mod's
opengl32.dll is in the game folder the game can't send anything to the internet: trying to join a server shows a
message saying online play is blocked, and nothing is sent. It asks whether to close the game: Yes closes it, No goes
back to the game so you can save your track first.
To play online, run uninstall.bat. Run install.bat again when you want the editor tools back.

Install
1. Close MX Simulator. Unzip this anywhere except the game folder (unzipping into it would overwrite an
   opengl32.dll that's already there, like MXS Shaders').
2. Run install.bat (it asks for admin once, to write into the game folder). It finds the game in Program Files, or
   asks you to paste the folder if you installed it somewhere else. If another mod already put an opengl32.dll there
   (MXS Shaders, for example), it keeps working: that file is renamed to opengl32_mxs.dll and the mod loads it.
3. Start the game in editor mode:  "C:\Program Files (x86)\MX Simulator\mx.exe" --editor
   (a shortcut with that target works too), open any track, and press TAB until the editor shows. The tools follow
   whichever track you have open. (--track YourTrackFolder still works to start on one.)

The tools work on the most recent MX Simulator snapshot (2026-05-14, build 1684). With a version the mod doesn't
know, the editor works normally without them (a box says so), and online play is still blocked. If the mod ever
can't block online play on some version, it says so and closes the game instead of letting it run.
It never changes mx.exe on disk. It keeps its own files (a log, your saved profile sections, and exported gradients
in the gradients folder) in %LOCALAPPDATA%\MX Simulator\mxtools, and saves curves beside the track's files as
mx-tools-curves.json.

Uninstall
Close MX Simulator and run uninstall.bat. It removes the mod and puts back the opengl32.dll it renamed, if any.
