# Compilar Zombiewood desde cero (Windows x64)

## Requisitos

- Windows 10/11 x64.
- **Visual Studio 2022 Build Tools** con la carga "Desarrollo para el escritorio con C++" (MSVC x64, CMake, Ninja).
- **git**.
- **vcpkg** clonado en `C:\Users\chdut\Tools\vcpkg` y con `bootstrap-vcpkg.bat` ejecutado
  (si lo tienes en otra ruta, cambia `VCPKG_ROOT` y `toolchainFile` en `CMakePresets.json`).
- Acceso a internet en el primer configure (vcpkg y FetchContent descargan fuentes).

Dependencias (todas fijadas):

| Dependencia | Origen | Fijado por |
|---|---|---|
| SDL2, ANGLE (`angle`, GLES2 sobre D3D11), nlohmann-json, boost-icl/variant | vcpkg (modo manifest) | `builtin-baseline` en `vcpkg.json` |
| Dear ImGui v1.91.9 (+ backends SDL2 / OpenGL3 en modo ES2) | FetchContent | tag |
| dynarmic (azahar-emu, frontend A32 + backend x64, externals incluidos) | FetchContent | commit `a466015` |

El log y el config usan solo la biblioteca estándar (`std::format`), no hay spdlog.

## Compilar

Desde un shell con el entorno MSVC cargado (por ejemplo "x64 Native Tools Command Prompt for VS 2022",
o `call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`
y añadir al `PATH` el `cmake`/`ninja` de `Common7\IDE\CommonExtensions\Microsoft\CMake`):

```bat
cd Zombiewood
cmake --preset win-debug
cmake --build --preset win-debug
ctest --preset win-debug
```

`win-release` es equivalente. El directorio de build queda **fuera del repo**:
`C:\Users\chdut\ZombiewoodDev\build\<preset>` (el repo vive en OneDrive).

La primera configuración tarda bastante (vcpkg compila ANGLE y SDL2 desde fuente; ~10–20 min). Las siguientes son rápidas.

## Ejecutar

```bat
C:\Users\chdut\ZombiewoodDev\build\win-debug\src\frontend\zombiewood.exe
C:\Users\chdut\ZombiewoodDev\build\win-debug\src\frontend\zombiewood.exe --smoke-test=120
```

- `--smoke-test[=N]`: renderiza N frames (120 por defecto) y sale solo con código 0.
- El post-build copia `libEGL.dll` y `libGLESv2.dll` (ANGLE) junto al exe; SDL las carga como driver GLES
  (`SDL_HINT_OPENGL_ES_DRIVER=1` + perfil `SDL_GL_CONTEXT_PROFILE_ES`).
- Log: `<home>/logs/zombiewood.log` (además de consola). Config opcional: `<home>/zombiewood.json`
  (`width`, `height`). `<home>` es la variable de entorno `ZOMBIEWOOD_HOME` si existe, si no la carpeta del exe.

## Estructura de targets

`zw_core` (log/config/paths), `zw_loader`, `zw_cpu` (+ `zw_cpu_deps` = dynarmic), `zw_hle_libc`, `zw_hle_gles`,
`zw_hle_audio`, `zw_jni`, `zw_frontend` (ventana SDL2 + contexto GLES2) y el ejecutable `zombiewood`.
Los módulos aparte de `zw_core`, `zw_cpu_deps` y `zw_frontend` son placeholders hasta sus tareas.

## Notas

- **Debug fuera de vcvars**: el exe `win-debug` usa el CRT de depuración (`msvcp140d.dll`, ...). Ejecútalo desde un shell con
  vcvars cargado o añade al `PATH` `...\BuildTools\VC\Redist\MSVC\<ver>\debug_nonredist\x64\Microsoft.VC143.DebugCRT`.
  `win-release` no tiene este requisito.
- El post-build también copia `z.dll` (el ANGLE de vcpkg lo importa incluso en debug).
- **ANGLE y ETC1** (medido en RTX 3060 / D3D11): ANGLE 2.1.7258 expone contexto `OpenGL ES 3.0`, `GL_OES_compressed_ETC2_*`,
  `GL_EXT_texture_compression_dxt1`, `GL_ANGLE_texture_compression_dxt5`, pero **NO** `GL_OES_compressed_ETC1_RGB8_texture`
  ni `GL_OES_depth_texture`. ETC2 RGB8 decodifica bloques ETC1 idénticamente, así que T5 puede mapear
  `GL_ETC1_RGB8_OES` a `GL_COMPRESSED_RGB8_ETC2` (o descomprimir en CPU).
