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
set ZOMBIEWOOD_HOME=C:\Users\chdut\ZombiewoodDev\run
C:\Users\chdut\ZombiewoodDev\build\win-release\src\frontend\zombiewood.exe
```

`<home>` es `ZOMBIEWOOD_HOME` si existe, si no la carpeta del exe. Ahí van `zombiewood.json`, `logs/zombiewood.log`,
`data/`, `saves/` (partida: `player.dat`, `zomb.dat`, `level.dat`, `zw_prefs.json`…), `screens/` y, opcional,
`gamecontrollerdb.txt`. El juego original lo aporta el usuario (`game_dir`), nunca va en el repo.

`zombiewood.json` (todas las claves son opcionales; los comentarios son solo de esta página):

```json
{
  "game_dir": "C:/Users/chdut/ZombiewoodDev/gamedata",   // lib/libZombieWood.so + obb/*.obb (+ assets del APK)
  "lib": "...", "assets_dir": "...", "screens_dir": "...",  // sobreescriben lo derivado de game_dir / home
  "vfs": { "data_dir": "data", "saves_dir": "saves", "obb_dir": "..." },
  "width": 1280, "height": 720, "fullscreen": false,      // ventana
  "internal_width": 1280, "internal_height": 720,         // resolución a la que dibuja el juego (se escala con letterbox)
  "internal_follows_window": false,                       // true: resolución interna = ventana (GL2JNILib.resize al cambiar)
  "linear_filter": true, "vsync": true, "fps_limit": 60,  // fps_limit 0 = sin límite; el juego usa tiempo real
  "pause_on_focus_loss": false,                           // minimizar siempre pausa (pause + nativeVoxSuspend)
  "language": 0,                                          // 0 en,1 fr,2 de,3 es,4 it,5 pt,6 ja,7 ko,8 zh,9 ru
  "input": { }                                            // ver abajo
}
```

Controles por defecto (el juego solo entiende toques y BACK/MENU: teclado y mando se traducen a toques virtuales
sobre el HUD, `src/frontend/input.cpp`):

| Acción | Ratón / teclado | Mando |
|---|---|---|
| Tocar (menús, botones) | clic izquierdo (puntero 0); pantallas táctiles: multitouch | — |
| Mover | `W A S D` (stick virtual izquierdo) | stick izquierdo |
| Apuntar y disparar | flechas, o mantener clic derecho (dispara hacia el cursor) | stick derecho |
| Botiquín / arma / pausa / aceptar (`>>`) | `E` · `Q` · `P` · `Espacio` | `Y` · `RB`/`X` · `Start` · `A` |
| Atrás (BACK) / MENU | `Esc` · `Tab` | `Back`/`B` · `Guide` |
| Captura PNG / pantalla completa | `F12` · `F11` o `Alt+Enter` | — |

Objeto `"input"` (se fusiona sobre los valores por defecto; `x`,`y` normalizados 0–1, `radius` en fracción del alto;
teclas = nombres de `SDL_GetScancodeFromName`, mando = nombres de `SDL_GameController`):

```json
"input": {
  "deadzone": 0.25, "mouse_touch": true, "mouse_aim_button": "right", "player_pos": [0.5, 0.45],
  "move_stick": { "x": 0.133, "y": 0.82, "radius": 0.105, "keys": {"up":"W","down":"S","left":"A","right":"D"}, "pad_stick": "left" },
  "aim_stick":  { "x": 0.866, "y": 0.82, "radius": 0.105, "keys": {"up":"Up","down":"Down","left":"Left","right":"Right"}, "pad_stick": "right" },
  "buttons": [ { "name": "botiquin", "x": 0.934, "y": 0.542, "keys": ["E"], "pad": ["y"] } ],
  "android_keys": [ { "code": 4, "keys": ["Escape"], "pad": ["back"] } ]
}
```

Opciones de línea de comandos: `--lib=<so>`, `--no-audio`, `--uncapped` (sin vsync ni límite, para medir), `--frames=N`,
`--shot-every=N`, `--hidden`, `--script=<fichero>` / `--control=<fichero>` (comandos de prueba: `wait`, `tap`,
`down/move/up`, `key`, `sdlkey`, `pad`, `axis`, `mouse`, `shot`, `pause`, `resume`, `resize`, `quit`; con `--control`
se ejecutan las líneas que se van añadiendo al fichero), `--load-only <so>` (diagnóstico de carga con la HLE real) y
`--smoke-test[=N]` (prueba de ventana + ImGui de T2, sin juego). Entorno: `ZW_TRACE_INPUT=1`, `ZW_AUDIO_DUMP=<pcm>`
y los de CONTRACTS §8.

- El post-build copia `libEGL.dll`, `libGLESv2.dll` (ANGLE) y `z.dll` junto al exe. El contexto se pide ES 3.0 (el juego
  usa ES 2.0; el frontend necesita `glBlitFramebuffer` para escalar el framebuffer interno).

## Estructura de targets

`zw_core` (log/config/paths), `zw_loader`, `zw_cpu` (+ `zw_cpu_deps` = dynarmic), `zw_hle_libc`, `zw_hle_gles`,
`zw_hle_audio`, `zw_jni`, `zw_frontend` (ventana SDL2 + contexto GLES, arranque, hilo GL, input) y el ejecutable `zombiewood`.

## Notas

- **Debug fuera de vcvars**: el exe `win-debug` usa el CRT de depuración (`msvcp140d.dll`, ...). Ejecútalo desde un shell con
  vcvars cargado o añade al `PATH` `...\BuildTools\VC\Redist\MSVC\<ver>\debug_nonredist\x64\Microsoft.VC143.DebugCRT`.
  `win-release` no tiene este requisito.
- El post-build también copia `z.dll` (el ANGLE de vcpkg lo importa incluso en debug).
- **ANGLE y ETC1** (medido en RTX 3060 / D3D11): ANGLE 2.1.7258 expone contexto `OpenGL ES 3.0`, `GL_OES_compressed_ETC2_*`,
  `GL_EXT_texture_compression_dxt1`, `GL_ANGLE_texture_compression_dxt5`, pero **NO** `GL_OES_compressed_ETC1_RGB8_texture`
  ni `GL_OES_depth_texture`. ETC2 RGB8 decodifica bloques ETC1 idénticamente, así que T5 puede mapear
  `GL_ETC1_RGB8_OES` a `GL_COMPRESSED_RGB8_ETC2` (o descomprimir en CPU).
