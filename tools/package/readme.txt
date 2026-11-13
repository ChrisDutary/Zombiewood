ZOMBIEWOOD for Windows (native port) - version @VERSION@
========================================================

This package does NOT include the game. You need your own legitimate copy of
Zombiewood 1.5.3 (Android APK by Gameloft/TechJ5, package
techj5gameszombiew153). Zombiewood runs by loading the files from that APK;
nothing from it is redistributed here.

REQUIREMENTS
- Windows 10/11 64-bit, a GPU with Direct3D 11 support.
- Windows N/KN: install the "Media Feature Pack" (otherwise videos are skipped).
- Your APK (the "big" one with archiveObbCache, or the APK + the main/patch OBBs).

INSTALLATION
1. Extract this folder anywhere you can write to (do not use
   "Program Files").
2. Run zombiewood.exe. Since there is no data yet, a wizard appears:
   "Elegir APK... (Enter)" (Choose APK) -> select your APK. It takes a few seconds.
3. "Jugar (Enter)" (Play).
Everything that gets created (data/, saves/, saves_backup/, logs/, zombiewood.json)
stays next to the exe. To remove the game, delete the folder.

UPDATING
Close the game and copy the new version's files over this folder
(replace files). Your saves, data and settings are not touched.

DEFAULT CONTROLS
  Move: W A S D             Aim/shoot: arrow keys, or right click
  Medkit: E   Weapon: Q   Pause: P   Accept (>>): Space
  Back: Esc   Game menu: Tab   Port menu: F1
  Screenshot: F12   Fullscreen: F11 or Alt+Enter
  Gamepad: sticks, A/B/X/Y, RB, Start, Back (remappable in F1 > Controles)

TROUBLESHOOTING
- Graphics problems: F1 > Graficos > Backend = "gl" (OpenGL), then restart.
- If something fails, check the logs folder (zombiewood.log).
- Your saves are in saves\; automatic copies are kept in saves_backup\.

Full documentation, configuration and licenses: README.md, LICENSE.txt and licenses\.
(The port menu is currently in Spanish.)
