ZOMBIEWOOD for Linux x86_64 (native port) - version @VERSION@
============================================================

This package does NOT include the game. You need your own legitimate copy of
Zombiewood 1.5.3 (Android APK by Gameloft/TechJ5, package
techj5gameszombiew153). Zombiewood runs by loading the files from that APK;
nothing from it is redistributed here.

REQUIREMENTS
- A 64-bit (x86_64) Linux with glibc 2.38 or newer (Ubuntu 24.04, Fedora 39,
  Debian 13, current Arch...), X11 or Wayland.
- OpenGL ES 3.0 through the system EGL (Mesa or the vendor driver).
- Your APK (the "big" one with archiveObbCache, or the APK + the main/patch OBBs).
- Optional: zenity or kdialog, for the "choose APK" file dialog.

INSTALLATION (AppImage)
1. chmod +x Zombiewood-Linux-x86_64-@VERSION@.AppImage
2. ./Zombiewood-Linux-x86_64-@VERSION@.AppImage
   Since there is no data yet, a wizard appears: "Elegir APK... (Enter)"
   (Choose APK) -> select your APK. Without zenity/kdialog, type or paste the
   path of the APK into the text field instead. It takes a few seconds.
3. "Jugar (Enter)" (Play).
Everything that gets created (data/, saves/, saves_backup/, logs/,
zombiewood.json) goes to ~/.local/share/zombiewood (or $XDG_DATA_HOME/zombiewood;
set ZOMBIEWOOD_HOME to use another folder). To remove the game, delete the
AppImage and that folder.

INSTALLATION (tar.gz, portable)
1. tar xzf Zombiewood-Linux-x86_64-@VERSION@.tar.gz   (into a folder you can write to)
2. ./Zombiewood-Linux-x86_64/zombiewood
Everything that gets created stays next to the "zombiewood" binary, like the
Windows version. To remove the game, delete the folder.

Headless install: zombiewood --install=/path/to/your.apk --install-only

UPDATING
AppImage: replace the file. tar.gz: copy the new files over the folder.
Your saves, data and settings are not touched.

DEFAULT CONTROLS
  Move: W A S D             Aim/shoot: arrow keys, or right click
  Medkit: E   Weapon: Q   Pause: P   Accept (>>): Space
  Back: Esc   Game menu: Tab   Port menu: F1
  Screenshot: F12   Fullscreen: F11 or Alt+Enter
  Gamepad: sticks, A/B/X/Y, RB, Start, Back (remappable in F1 > Controles)

KNOWN LIMITS ON LINUX
- Videos (logo, intro, "movie complete") are not played yet: they are skipped.
- The "Backend" option of the F1 menu only applies to Windows.
- Steam Deck / SteamOS: untested.
- If something fails, check logs/zombiewood.log (and logs/crash-*.txt) in the
  data folder.

Full documentation, configuration and licenses: README.md, LINUX.md,
LICENSE.txt and licenses/. (The port menu is currently in Spanish.)
