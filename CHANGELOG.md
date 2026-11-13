# Changelog — Zombiewood for Windows

Every improvement and change to the port is documented here. Format based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).
Each version is published as a release on [GitHub](https://github.com/ChrisDutary/Zombiewood/releases) with the tag
`vX.Y.Z` and its zip `Zombiewood-Win64-X.Y.Z.zip`.

**Updating without losing your progress:** close the game and copy the contents of the new zip over your folder
(replace files). The zip only contains the program; `saves/`, `data/` and `zombiewood.json` are never touched.

## [Unreleased]

Changes already on `main` that will ship in the next version.

### Added
- **MIT license** (`LICENSE`, © 2026 ChrisDutary) for the project's own code and documentation. The zip will include
  it as `LICENSE.txt`.
- **Legal** section in the README: the game and its content belong to Gameloft and are not covered by the license; the
  project is unaffiliated; third-party art (icon/cursor) is outside the license; contact route for takedown requests.
- **English documentation:** `README.md` is now in English with a Spanish copy in `README.es.md` (linked from both);
  this changelog, everything under `docs/` and the zip's `readme.txt` were translated to English.
- README sections on updating without losing your progress and moving your save to another PC.

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
