# Changelog — Zombiewood for Windows

Every improvement and change to the port is documented here. Format based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).
Each version is published as a release on [GitHub](https://github.com/ChrisDutary/Zombiewood/releases) with the tag
`vX.Y.Z` and its zip `Zombiewood-Win64-X.Y.Z.zip`.

**Updating without losing your progress:** close the game and copy the contents of the new zip over your folder
(replace files). The zip only contains the program; `saves/`, `data/` and `zombiewood.json` are never touched.

## [Unreleased]

Work in progress for **0.3.0** on `feature/usability` (and `feature/linux`).

### Added
- **Menu in English and Spanish**: the F1 menu and the first-run wizard follow the system language by default;
  *Graphics → Menu language* switches it instantly (`"ui_language": "auto" | "es" | "en"`). The game's own language
  setting is now labeled *Game language*.
- **Optional on-screen touch sticks** (*Graphics → On-screen touch sticks*, `"hud_sticks"`): *Automatic* hides them
  while you play with keyboard or gamepad and shows them when you use the mouse as touch; or *Always show* / *Always
  hide*. It uses the game's own HUD switch (originally for the Xperia Play); controls keep working with the sticks
  hidden (verified with keyboard and touch input).
- **Button hints**: small badges next to the HUD buttons (medkit, weapon, pause) showing the key or gamepad button to
  press — keycaps for keyboard, PlayStation shapes (✕ ○ □ △) or Xbox letters for gamepads, following the last device
  used. Only shown inside scenes and hidden while paused (`"button_hints"`).
- **New version notice**: at startup the port checks GitHub's public releases (at most once a day, nothing about you
  is sent) and shows a *Download* banner/menu item when a newer version exists (`"update_check"`, toggle in *Help →
  About*). Test builds are ignored.
- *About* window shows the port version.

### Changed
- The game state (in a scene / paused) is read from the game's memory without modifying it, to drive the sticks and
  hints (`src/frontend/hud.cpp`).
- Test harness: `sdlkey <key>` (press without `up`) works again.

## [0.2.2] — 2026-11-15

Input, audio and quality-of-life fixes. Tested by the owner with a DualSense over USB (test builds 1 and 2).
Saves and settings from 0.2.x keep working.

### Fixed
- **Quick key/button taps were lost (bug B2)**: pressing and releasing within the same frame (~16 ms) never reached the
  game, so the first `Esc` often did nothing and fast gamepad taps were ignored. Presses are now latched until the next
  frame. Automated A/B test with instant taps: 0.2.1 delivered 0 of 3, 0.2.2 delivers 3 of 3.
- **Choppy audio at startup**: the audio queue was tight (~100 ms) and refilled immediately after every dropout, which
  sounded "laggy" while the PC was busy translating the game's code. Now the queue is larger (150 ms by default), the
  output buffer is 2048 frames, and after a dropout playback waits until the queue is half full again (a brief silence
  instead of crackling). Measured at startup: 2 dropouts (low), 1 (normal), 0 (safe).
- **Glitchy audio in the startup logo video** (the main source of the "laggy" sound at launch): the video player read
  audio just in time from the same reader as the frames and started the device almost empty, so the sound starved
  while the game was booting. Audio now has its own reader, stays ~0.5 s ahead and playback starts with ~250 ms
  buffered. Measured on `logo_hd.mp4`: **246 audio dropouts → 0**.
- **Black console window flashing at launch**: the exe is now a windowed app (no console is ever created); when started
  from a terminal it attaches to that console so CLI output still shows.

### Added
- **Gamepad section in F1 → Controles**: connected controllers, a live tester (sticks, triggers and pressed buttons as
  the game receives them), PlayStation button names (Cross, Circle, L1, R2…), a "swap sticks" option, and a **guided
  mapping wizard** that asks you to press the gamepad button for each action in turn.
- **Gamepad driver option** (`"pad_driver": "auto" | "compat"`): "compat" disables SDL's HIDAPI driver, for when
  another app (Steam, DS4Windows) holds the controller and no buttons arrive.
- Fallback for joysticks SDL does not recognize as a standard gamepad (opened in raw mode so they can be remapped).
- **Audio latency option** in F1 → Audio (`"audio_latency": "low" | "normal" | "safe"`) and a live dropout counter.
- More diagnostics in the log: controller type/axes/buttons/driver, the first input received from the gamepad, and
  audio dropouts every ~5 s (game) and per video.
- **Crash report**: if the game closes unexpectedly, it writes `logs/crash-<date>.dmp` and a log line with the
  exception code and the faulting module (helps diagnose crashes on other PCs).
- README: section for antivirus false positives (Panda, Defender…) deleting or blocking the exe.

## [0.2.1] — 2026-11-13

Documentation and licensing release; the game runtime is unchanged (0.2.0 saves and settings keep working).

### Added
- **MIT license** (`LICENSE`, © 2026 ChrisDutary) for the project's own code and documentation. The zip will include
  it as `LICENSE.txt`.
- **Legal** section in the README: the game and its content belong to Gameloft and are not covered by the license; the
  project is unaffiliated; third-party art (icon/cursor) is outside the license; contact route for takedown requests.
- **English documentation:** `README.md` is now in English with a Spanish copy in `README.es.md` (linked from both);
  this changelog, everything under `docs/` and the zip's `readme.txt` were translated to English.
- README sections on updating without losing your progress and moving your save to another PC.

### Changed
- The zip now ships `LICENSE.txt`, and its `readme.txt` is in English.
- Task specs in `docs/tasks/` renamed to English file names (`T1-analysis.md`, `T2-skeleton.md`, `T8-integration.md`).
- First build from the dedicated repository (clean build, same pinned dependencies).

## Repository

- 2026-11-13: the project moved to its own public repository, [ChrisDutary/Zombiewood](https://github.com/ChrisDutary/Zombiewood),
  with its full history (it previously lived in the `Zombiewood/` folder of a university repository). Releases 0.1.0
  and 0.2.0 were republished here with the same zips; the tags are now named `v0.1.0` and `v0.2.0`.

## [0.2.0] — 2026-11-13

### Added
- **Zombie cursor** (an arrow with an eye) instead of the Windows arrow, in the game, the wizard and the F1 menu.
  It is the system pointer (the game does not draw it), so it does not affect FPS or game logic.
- Options in **F1 → Gráficos (Graphics)**: *Cursor zombi* (on/off) and *Tamaño del cursor* (cursor size: 32 / 48 / 64 / 96 px).
  New keys in `zombiewood.json`: `"cursor": "zombie" | "system"` and `"cursor_size": 48`.
- `tools/make_cursor.ps1`: generates the cursor (`resources/zombiewood_cursor.bin`, embedded in the exe as a resource)
  from a PNG image with transparency; it crops the figure and uses the tip as the click point.
- This `CHANGELOG.md`.

### Changed
- The package includes the Visual C++ runtime (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) so the game
  opens on PCs without the *VC++ Redistributable* (already in the published 0.1.0 zip; now documented).
- `tools/package.ps1` refuses to regenerate the staging folder if it contains `data/`, `saves/` or `zombiewood.json`
  (prevents wiping a save if someone played from `dist\Zombiewood-Win64`).
- README installation guide: download from Releases, SmartScreen notice and a section for reporting tests on other PCs.

### Notes
- The cursor and icon are third-party art and are not versioned; they are embedded in the release exe.
- No changes to the core (CPU/loader/HLE): 0.1.0 saves keep working the same.

## [0.1.0] — 2026-11-13

First playable version.

### Added
- Complete runtime: ARM ELF loader + dynarmic JIT + Android HLE (bionic libc/pthread, OpenGL ES → ANGLE/D3D11,
  AudioTrack audio → SDL2, JNI and emulated Java classes).
- First-run installer from the user's APK (verifies the SHA-256 hashes of version 1.5.3).
- F1 menu in the style of Ship of Harkinian (graphics, audio, controls with remapping, folders, help).
- Game videos via Media Foundation, save backups, rotating logs.
- Controls: mouse as touches; WASD/arrow keys and gamepad as virtual touches on the HUD.
- Application icon.

### Known issues
- In-game, Esc sometimes needs two presses.
- Untested: physical gamepad, scenes 2+, other GPUs. Online services (store, TapJoy, Facebook) are unavailable.
