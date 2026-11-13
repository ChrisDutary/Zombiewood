# T2 — Build skeleton + SDL2 window with a GLES2 context (ANGLE)

> Historical task spec (November 2026). Paths refer to the original development machine; the project then lived in the
> `Zombiewood/` folder of another repository.

Read `Zombiewood/docs/PLAN.md` first. Repo: `C:\Users\chdut\OneDrive\Escritorio\2026-1GS242-Dutary-Christian`, branch `main`.

## Goal
A C++20 project that builds on Windows x64 and opens an SDL2 window with an **ANGLE OpenGL ES 2.0 context** (D3D11 backend), clearing to a color and showing a test ImGui window. The base for every other task.

## Toolchain
- Visual Studio 2022 Build Tools (C++ x64, CMake, Ninja, clang-cl) is being/has been installed on this PC. Use `vswhere` to locate it and a "Developer PowerShell"/`vcvars64.bat`. If it is not there yet when you start, wait using a one-off check (no long polling loops) or `ask` the coordinator.
- vcpkg: `C:\Users\chdut\Tools\vcpkg` (manifest mode). Set `VCPKG_ROOT` in the preset, not globally.
- Build dir **outside the repo**: `C:\Users\chdut\ZombiewoodDev\build\<preset>` (the repo is in OneDrive).

## Deliverables
- `Zombiewood/CMakeLists.txt`, `CMakePresets.json` (presets `win-debug`, `win-release`, Ninja + MSVC or clang-cl; binaryDir pointing to ZombiewoodDev\build), `vcpkg.json` with a pinned baseline.
- Dependencies: SDL2, ANGLE (vcpkg port `angle` or pinned binaries; the app must load `libEGL.dll`/`libGLESv2.dll` copied next to the exe), Dear ImGui (SDL2 + OpenGL3 backend with GLSL ES `#version 100`), nlohmann-json, spdlog or equivalent, and **dynarmic** (FetchContent with a pinned tag/commit, A32 frontend, x64 backend) **only built and linked** into a `zw_cpu_deps` library for T3 to use; verify it builds.
- Targets: one empty or minimal library per module (`zw_core`, `zw_loader`, `zw_cpu`, `zw_hle_libc`, `zw_hle_gles`, `zw_hle_audio`, `zw_jni`, `zw_frontend`) following the layout in PLAN.md, and the `zombiewood` executable.
- `src/core/`: logging (file `logs/zombiewood.log` + console), reading `zombiewood.json`, path utilities (exe folder, a `SHIP_HOME`-like override `ZOMBIEWOOD_HOME`).
- `src/frontend/main.cpp`: a resizable 1280x720 window, creates an ANGLE EGL/GLES2 context (via the SDL hints `SDL_HINT_OPENGL_ES_DRIVER` + `SDL_GL_CONTEXT_PROFILE_ES`, or EGL directly), a loop with the ImGui demo; log `GL_VERSION`, `GL_RENDERER` and the extension list (check whether ANGLE exposes `GL_OES_compressed_ETC1_RGB8_texture`).
- `Zombiewood/docs/BUILD.md`: how to build from scratch.
- A trivial `ctest` that passes.

## Constraints
- Edit only: `Zombiewood/CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/`, `src/core/`, `src/frontend/`, empty/minimal `CMakeLists.txt` in the other `src/` folders, `tests/`, `docs/BUILD.md`.
- Commit `zombiewood(T2): ...` with `git add` of your paths only. No push. No binaries in the repo.

## Proof of completion
Output of `cmake --preset win-debug`, `cmake --build`, `ctest`, and the log of the exe run for ~5 s (closed programmatically with a `--smoke-test` flag that exits by itself after N frames) showing the ANGLE version/renderer and extensions.
