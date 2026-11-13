# Zombiewood — work plan

Native Windows x64 port of *Zombiewood* (Gameloft, Android 1.5.3, `com.gameloft.android.ANMP.GloftZOHM`) in the style
of Ship of Harkinian: this repo contains **only the runtime**; every user provides their own copy (APK + OBB). Game
files are never committed (see `.gitignore`).

> Historical document: this is the plan the port was built with (November 2026). Paths under `C:\Users\…` refer to the
> original development machine.

## Technique: so-loader + JIT ARM32 CPU

`libZombieWood.so` is an ELF32 ARM (armeabi-v7a) library with 35,581 exported symbols (not stripped) and 364 imports:

| Imported lib | Approx. count | Replacement |
|---|---|---|
| libGLESv2 | 99 | ANGLE (GLES2 on D3D11), guest→host pointer translation |
| libOpenSLES | 4 (engine + bufferqueue + play) | SDL2 audio |
| libc / libm / libdl / pthread (bionic) | ~240 | `hle/libc` with the 32-bit bionic ABI |
| libstdc++ (Android's minimal one) | — | `hle/libc` (operator new/delete, __cxa_*) |
| liblog | 2 | log to file/console |
| sockets | 15 | stubs (no network: Gameloft's servers no longer exist) |

Entry points: `JNI_OnLoad` + 88 `Java_*` functions (see `docs/ANALYSIS.md`). It does not use EGL or libandroid.
Data: OBB `main.1501…obb` (276 MB, `.bar`) + `patch.15325…obb`, paths `/sdcard/gameloft/games/…` and
`/data/data/com.gameloft.android.ANMP.GloftZOHM/`.

## Layout

```
(repo root)
  CMakeLists.txt, CMakePresets.json, vcpkg.json
  src/
    core/      shared utilities (log, config, paths)
    loader/    ELF32 ARM loading, relocations, import resolution (T3)
    cpu/       dynarmic A32, guest memory, host<->guest bridge, threads (T3)
    hle/libc/  bionic libc/libm/pthread/libstdc++ (T4)
    hle/gles/  GLES2 -> ANGLE (T5)
    hle/audio/ OpenSL ES -> SDL2 (T6)
    jni/       fake JNIEnv/JavaVM + emulated Java classes (T7)
    frontend/  main, SDL2 window, input, ImGui (T2/T8/T9)
    installer/ extraction of the user's APK/OBB (T9)
  docs/        PLAN.md, ANALYSIS.md, CONTRACTS.md, BUILD.md
  tools/       analysis/inspection scripts
```

## Development environment (outside the repo, which lives in OneDrive)

- Test data: `C:\Users\chdut\ZombiewoodDev\gamedata\` (`lib/libZombieWood.so`, `obb/*.obb`, `classes.dex`, `AndroidManifest.xml`)
- Full APK: `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk` (read-only)
- Build: `C:\Users\chdut\ZombiewoodDev\build\` (`cmake --preset` must point there)
- Analysis: `C:\Users\chdut\ZombiewoodDev\analysis\`
- Tools: `C:\Users\chdut\Tools\` (vcpkg, jdk-21, ghidra_12.1.4_PUBLIC, android\apktool.bat, jadx-headless-mcp); REA CLI `rea`

## Tasks (DAG)

| # | Task | Depends on | Model |
|---|---|---|---|
| T1 | Deep analysis (REA/Ghidra/JADX) → `docs/ANALYSIS.md` | — | Fable |
| T2 | CMake/vcpkg skeleton, SDL2 window + GLES2 context via ANGLE | — | Sonnet |
| T3 | ELF loader + dynarmic CPU + import thunks + bridge + threads | T1, T2 | Fable |
| T4 | libc/libm/pthread/libstdc++ HLE | T3 | Opus |
| T5 | GLES2 → ANGLE HLE | T3 | Opus |
| T6 | OpenSL ES → SDL2 HLE | T3 | Sonnet |
| T7 | Fake JNI/Java | T1, T3 | Opus |
| T8 | Integration and game loop, input; play from menu → match | T4–T7 | Fable |
| T9 | Data installer, config, saves, ImGui menu | T8 | Sonnet |
| T10 | Win64 packaging, README, QA | T9 | Sonnet |

## Rules for all workers

1. `main` branch, shared worktree: edit **only** your task's folders (+ your section in the docs). If you need to touch
   another one, ask the coordinator with `ask`.
2. Do not commit game files or builds. Do not `git push`. Small commits prefixed with `zombiewood(Tn):` and `git add`
   only your own paths (other workers run in parallel).
3. The interface between modules is in `docs/CONTRACTS.md` (written by T3). Contract changes → `ask` the coordinator.
4. C++20, MSVC/clang-cl x64, no dependencies outside vcpkg/FetchContent with a pinned version.
5. Proof of completion: clean build + the test your spec requires, with its output pasted in your report.
