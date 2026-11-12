# T2 — Esqueleto de build + ventana SDL2 con contexto GLES2 (ANGLE)

Lee primero `Zombiewood/docs/PLAN.md`. Repo: `C:\Users\chdut\OneDrive\Escritorio\2026-1GS242-Dutary-Christian`, rama `main`.

## Objetivo
Proyecto C++20 que compila en Windows x64 y abre una ventana SDL2 con un contexto **OpenGL ES 2.0 de ANGLE** (backend D3D11), limpiando a un color y mostrando una ventana ImGui de prueba. Base para el resto de tareas.

## Toolchain
- Visual Studio 2022 Build Tools (C++ x64, CMake, Ninja, clang-cl) se está instalando/instalado en esta PC. Usa `vswhere` para localizarlo y un "Developer PowerShell"/`vcvars64.bat`. Si al empezar aún no aparece, espera usando un chequeo puntual (no bucles de polling largos) o `ask` al coordinador.
- vcpkg: `C:\Users\chdut\Tools\vcpkg` (modo manifest). Configura `VCPKG_ROOT` en el preset, no global.
- Build dir **fuera del repo**: `C:\Users\chdut\ZombiewoodDev\build\<preset>` (el repo está en OneDrive).

## Entregables
- `Zombiewood/CMakeLists.txt`, `CMakePresets.json` (presets `win-debug`, `win-release`, Ninja + MSVC o clang-cl; binaryDir apuntando a ZombiewoodDev\build), `vcpkg.json` con baseline fijada.
- Dependencias: SDL2, ANGLE (port vcpkg `angle` o binarios fijados; la app debe cargar `libEGL.dll`/`libGLESv2.dll` copiados junto al exe), Dear ImGui (backend SDL2 + OpenGL3 con GLSL ES `#version 100`), nlohmann-json, spdlog o equivalente, y **dynarmic** (FetchContent con tag/commit fijado, frontend A32, backend x64) **solo compilado y enlazado** en una lib `zw_cpu_deps` para que T3 lo use; verifica que compila.
- Targets: biblioteca por módulo vacía o mínima (`zw_core`, `zw_loader`, `zw_cpu`, `zw_hle_libc`, `zw_hle_gles`, `zw_hle_audio`, `zw_jni`, `zw_frontend`) siguiendo la estructura de PLAN.md, y ejecutable `zombiewood`.
- `src/core/`: log (archivo `logs/zombiewood.log` + consola), lectura de `zombiewood.json`, utilidades de rutas (carpeta del exe, `SHIP_HOME`-like override `ZOMBIEWOOD_HOME`).
- `src/frontend/main.cpp`: ventana 1280x720 redimensionable, crea contexto EGL/GLES2 de ANGLE (vía SDL hints `SDL_HINT_OPENGL_ES_DRIVER` + `SDL_GL_CONTEXT_PROFILE_ES`, o EGL directo), loop con ImGui demo; log de `GL_VERSION`, `GL_RENDERER` y lista de extensiones (comprobar si ANGLE expone `GL_OES_compressed_ETC1_RGB8_texture`).
- `Zombiewood/docs/BUILD.md`: cómo compilar desde cero.
- Un `ctest` trivial que pase.

## Restricciones
- Edita solo: `Zombiewood/CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/`, `src/core/`, `src/frontend/`, `CMakeLists.txt` vacíos/mínimos de las demás carpetas de `src/`, `tests/`, `docs/BUILD.md`.
- Commit `zombiewood(T2): ...` con `git add` solo de tus rutas. Sin push. Nada de binarios en el repo.

## Evidencia de terminado
Salida de `cmake --preset win-debug`, `cmake --build`, `ctest`, y el log del exe ejecutado ~5 s (cerrado programáticamente con un flag `--smoke-test` que sale solo tras N frames) mostrando versión/renderer de ANGLE y extensiones.
