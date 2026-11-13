# T9 — Data installer, configuration, saves and ImGui menu

> Historical task spec (November 2026). Paths refer to the original development machine.

Read `docs/PLAN.md` and the current state of the code. Repo on `main`.

## Goal
A Ship of Harkinian-like experience for the end user.

## Requirements
- First run without data: a dialog (SDL/Windows file dialog) to choose **the user's APK** (the large one with `assets/archiveObbCache.zip.00N` or the small one + a folder/`.obb` files).
  Extract into `<home>/data/`: `lib/libZombieWood.so`, `lib/libgenerator.so`, the main+patch OBBs (or their contents, depending on how the VFS uses them), and the resources T1 lists. Verify known SHA-256 hashes (`libZombieWood.so` = `2855ed9f95367d5dc592a2179c048383c6a177b7ca6da19a2919a2527cfa967c`) and warn if the version is not the supported 1.5.3.
- `zombiewood.json`: backend (ANGLE D3D11 / OpenGL via ANGLE), internal resolution, vsync, fullscreen, volume, control map, data path.
- ImGui menu (F1 key, top bar like SoH): graphics, audio, controls (remapping), show FPS, open the saves/logs folders.
- Saves in `<home>/saves/` (mapped from `/data/data/...`), automatic backup before overwriting.
- Rotating logs in `<home>/logs/`. Limit the game's spam ("Sending TapJoy request… Response not valid", ~10 lines/s
  in menus): group identical repetitions, or short-circuit the TapJoy request in the JNI/network layer so it is never generated.
- **Videos** (left over from T8, currently skipped): the `.mp4` files in the APK's `res/raw` (`logo_hd`, `intro_hd`, `moviecomplete_hd/ld`…)
  requested by `SUtils` (see ANALYSIS §8 and the emulated class in `src/jni`). Extract them during installation and play them full
  screen with **Windows Media Foundation** (no new dependencies; `IMFSourceReader` → GLES texture via ANGLE, audio through
  SDL), skippable by click/key, and call `SUtils.onVideoFinished` at the end as Java does. If a video fails, skip it as today.
  A `skip_videos` option in `zombiewood.json`.

## Proof
A test from a clean folder: exe + DLLs → choose `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk` → it installs → the game starts. Screenshots of the menu.

## Constraints
Edit `src/frontend/`, `src/installer/`, `src/core/` (config), tests and docs. Commits `zombiewood(T9): ...`, no push.
