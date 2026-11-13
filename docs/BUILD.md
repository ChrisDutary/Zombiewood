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
| SDL2, ANGLE (`angle`, GLES2 sobre D3D11), zlib, nlohmann-json, boost-icl/variant | vcpkg (modo manifest) | `builtin-baseline` en `vcpkg.json` |
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

Una carpeta con `zombiewood.exe` + `SDL2.dll`, `libEGL.dll`, `libGLESv2.dll` y `z.dll` es todo lo que necesita el usuario
final; el juego original lo aporta él (nunca va en el repo). `<home>` es `ZOMBIEWOOD_HOME` si existe, si no la carpeta del exe.

```bat
C:\Users\chdut\ZombiewoodDev\build\win-release\src\frontend\zombiewood.exe
```

### Primer arranque (instalador, T9)

Si no hay datos (`<home>/data/lib/libZombieWood.so` + OBB main) aparece un asistente en la propia ventana:
**Elegir APK… (Enter)** abre el diálogo de Windows. Vale:

- el APK grande (`assets/archiveObbCache.zip.001…00N` con los OBB dentro), o
- el APK original + `main.1501…obb` / `patch.15325…obb` (se buscan junto al APK y en `obb/`; si no están, se piden con otro diálogo).

Se extrae en streaming a `<home>/data/`: `lib/libZombieWood.so`, `lib/libgenerator.so`, `obb/main…obb`, `obb/patch…obb`,
`res/raw/*.mp4` (+ textos), `assets/colors.conf`, `assets/families.list` e `install.json` (origen, SHA-256, avisos). Se
verifica el SHA-256 de `libZombieWood.so` (1.5.3 = `2855ed9f…fa967c`) y de los OBB; si no coinciden se avisa (otra versión
puede no funcionar). Con el APK de 306 MB tarda ~1–3 s en un SSD. Después se escribe `zombiewood.json` completo y **Jugar (Enter)**.

Sin interfaz: `zombiewood --install=<apk> [--obb=<main.obb> --obb=<patch.obb>] [--install-only]`; `--reinstall` vuelve a
abrir el asistente aunque haya datos.

### Carpetas en `<home>`

| Carpeta / fichero | Contenido |
|---|---|
| `zombiewood.json` (+ `.bak`) | configuración (la edita el menú F1) |
| `data/` | datos del juego instalados + `/sdcard/android/data/<pkg>/files` del guest (lo que escribe el juego) |
| `saves/` | `/data/data/<pkg>/` del guest: `player.dat`, `zomb.dat`, `level.dat`, `zw_prefs.json`… |
| `saves_backup/` | copias automáticas: `<fecha>_inicio/` al arrancar y `<fecha>/` con la versión anterior de cada save que el juego sobrescribe (se conservan `backups_keep`); `shadow/` es interno |
| `logs/` | `zombiewood.log` + `zombiewood.1.log`…`.4.log` (rotación al arrancar y al pasar de 32 MiB). Las líneas Info idénticas repetidas (p. ej. el spam "Sending TapJoy request… / Response not valid" del juego sin red) se resumen como `(xN en 30 s, suprimidas) …`; `ZW_LOG_NODEDUP=1` lo desactiva |
| `screens/` | capturas (F12) |
| `imgui.ini` | posiciones de las ventanas del menú |
| `gamecontrollerdb.txt` | opcional, mapeos extra de mandos SDL |

### `zombiewood.json`

Todas las claves son opcionales (los comentarios son solo de esta página):

```json
{
  "game_dir": "data",                     // lib/, obb/, res/raw/, assets/ (relativa a <home> o absoluta)
  "lib": "...", "assets_dir": "...", "videos_dir": "...", "screens_dir": "...",  // sobreescriben lo derivado
  "vfs": { "data_dir": "data", "saves_dir": "saves", "obb_dir": "..." },
  "backend": "d3d11",                     // ANGLE: "d3d11" | "gl" (OpenGL de escritorio) | "vulkan" | "d3d9"; al reiniciar
  "width": 1280, "height": 720, "fullscreen": false,
  "internal_width": 1280, "internal_height": 720,   // resolución a la que dibuja el juego (se escala con letterbox)
  "internal_follows_window": false,                 // true: resolución interna = ventana (GL2JNILib.resize al cambiar)
  "linear_filter": true, "vsync": true, "fps_limit": 60,   // 0 = sin límite; el juego usa tiempo real
  "show_fps": false, "menu_bar": false,             // contador de FPS; barra de menú visible al arrancar
  "cursor": "zombie", "cursor_size": 48,            // (0.2.0) cursor zombi incrustado | "system"; 32/48/64/96 px
  "pause_on_focus_loss": false,                     // minimizar siempre pausa (pause + nativeVoxSuspend)
  "volume": 1.0, "video_volume": 1.0, "mute": false,
  "skip_videos": false, "play_logo": true,          // vídeos de res/raw (logo al arrancar, intro, moviecomplete)
  "language": 0,                                    // 0 en,1 fr,2 de,3 es,4 it,5 pt,6 ja,7 ko,8 zh,9 ru (al reiniciar)
  "backups_keep": 20,                               // conjuntos de copias de saves
  "input": { }                                      // ver abajo; el menú Controles lo escribe completo
}
```

### Menú (F1)

Barra superior estilo Ship of Harkinian: **Zombiewood** (pausar/reanudar, captura, salir), **Gráficos** (backend,
resolución interna, filtrado, VSync, límite de FPS, pantalla completa, mostrar FPS, saltar vídeos, idioma), **Audio**
(volumen del juego y de los vídeos, silenciar), **Controles** (remapeo: "Cambiar" y pulsar una tecla o botón del mando,
"+" añade, "x" borra; zona muerta, ratón), **Carpetas** (abrir saves, copias de seguridad, logs, capturas, datos; crear
copia ahora) y **Ayuda**. Los cambios se guardan al momento en `zombiewood.json`; backend e idioma piden reiniciar.
Mientras el menú tiene el ratón o el teclado, esos eventos no llegan al juego.

### Vídeos

`Game.playCinematicAndroid` (intro y "movie complete") y el logo de Gameloft al arrancar se reproducen con Windows Media
Foundation (H.264/AAC del sistema; en Windows N hace falta el Media Feature Pack, si no, se saltan). Cualquier tecla, clic o
botón del mando los salta. `"skip_videos": true` los omite siempre.

### Controles

Controles por defecto (el juego solo entiende toques y BACK/MENU: teclado y mando se traducen a toques virtuales
sobre el HUD, `src/frontend/input.cpp`):

| Acción | Ratón / teclado | Mando |
|---|---|---|
| Tocar (menús, botones) | clic izquierdo (puntero 0); pantallas táctiles: multitouch | — |
| Mover | `W A S D` (stick virtual izquierdo) | stick izquierdo |
| Apuntar y disparar | flechas, o mantener clic derecho (dispara hacia el cursor) | stick derecho |
| Botiquín / arma / pausa / aceptar (`>>`) | `E` · `Q` · `P` · `Espacio` | `Y` · `RB`/`X` · `Start` · `A` |
| Atrás (BACK) / MENU | `Esc` · `Tab` | `Back`/`B` · `Guide` |
| Menú del port / captura PNG / pantalla completa | `F1` · `F12` · `F11` o `Alt+Enter` | — |

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

### Línea de comandos y pruebas

`--lib=<so>`, `--no-audio`, `--uncapped` (sin vsync ni límite, para medir; no se guarda en el json), `--frames=N`,
`--shot-every=N`, `--hidden` (ventana oculta; tampoco reproduce el logo), `--install=<apk>`, `--obb=<fichero>`,
`--install-only`, `--reinstall`, `--script=<fichero>` / `--control=<fichero>` (comandos de prueba: `wait`, `tap`,
`down/move/up`, `key`, `sdlkey`, `pad`, `axis`, `mouse`, `shot`, `pause`, `resume`, `resize`, `quit` y, de T9,
`menu [graphics|audio|controls|about|none]`, `wshot <nombre>` (captura de la ventana, con el menú), `remap <destino> [add]`
+ `ui key <tecla>` / `ui pad <botón>` (evento SDL real hacia el menú); con `--control` se ejecutan las líneas que se van
añadiendo al fichero), `--load-only <so>` (diagnóstico de carga con la HLE real) y `--smoke-test[=N]` (prueba de ventana +
ImGui de T2, sin juego). Entorno: `ZW_TRACE_INPUT=1`, `ZW_AUDIO_DUMP=<pcm>`, `ZW_LOG_NODEDUP=1`, `ANGLE_DEFAULT_PLATFORM`
(gana a `backend`) y los de CONTRACTS §8.

- El post-build copia `libEGL.dll`, `libGLESv2.dll` (ANGLE) y `z.dll` junto al exe. El contexto se pide ES 3.0 (el juego
  usa ES 2.0; el frontend necesita `glBlitFramebuffer` para escalar el framebuffer interno).

## Empaquetar el release (T10)

```powershell
pwsh tools\package.ps1 -Clean        # configura (preset win-release), compila, ctest, monta y comprime
pwsh tools\package.ps1 -SkipBuild    # solo reempaqueta lo que ya hay en el directorio de build
```

Salida (por defecto en `C:\Users\chdut\ZombiewoodDev\dist\`): carpeta `Zombiewood-Win64\` con `zombiewood.exe`, `SDL2.dll`,
`libEGL.dll`, `libGLESv2.dll`, `z.dll`, `gamecontrollerdb.txt` (solo entradas de Windows, `tools/package/`), `readme.txt`
(`tools/package/readme.txt`), `README.md` y `licenses/`; y `Zombiewood-Win64-<versión>.zip` (+ `.sha256`). El script falla si
el paquete contiene `*.apk/.obb/.so/.mp4/.bar/.dex`. Parámetros: `-DistDir`, `-BuildDir`, `-Clean`, `-SkipBuild`, `-SkipTests`.
`-Clean` hace un build realmente desde cero (vcpkg + clones de FetchContent: ~15-20 min, necesita red).

El QA con entrada real de Windows está en `tools/qa/` (`qa_phase1…5`) y sus resultados en [`QA.md`](QA.md).
Al abrirse con doble clic el exe suelta su consola (no sale ventana negra); desde un terminal la conserva.

## Estructura de targets

`zw_core` (log/config/paths), `zw_installer` (T9: APK → datos, zlib + BCrypt), `zw_loader`, `zw_cpu` (+ `zw_cpu_deps` =
dynarmic), `zw_hle_libc`, `zw_hle_gles`, `zw_hle_audio`, `zw_jni`, `zw_frontend` (ventana SDL2 + contexto GLES, arranque,
hilo GL, input; T9: asistente, menú ImGui, vídeos con Media Foundation, copias de saves) y el ejecutable `zombiewood`.
`ctest`: 8 pruebas, incluida `installer` (APK sintéticos, no necesita los datos del juego). zlib viene de vcpkg
(declarado en `vcpkg.json`, `find_package(ZLIB)`).

## Notas

- **Debug fuera de vcvars**: el exe `win-debug` usa el CRT de depuración (`msvcp140d.dll`, ...). Ejecútalo desde un shell con
  vcvars cargado o añade al `PATH` `...\BuildTools\VC\Redist\MSVC\<ver>\debug_nonredist\x64\Microsoft.VC143.DebugCRT`.
  `win-release` no tiene este requisito.
- El post-build también copia `z.dll` (el ANGLE de vcpkg lo importa incluso en debug).
- **ANGLE y ETC1** (medido en RTX 3060 / D3D11): ANGLE 2.1.7258 expone contexto `OpenGL ES 3.0`, `GL_OES_compressed_ETC2_*`,
  `GL_EXT_texture_compression_dxt1`, `GL_ANGLE_texture_compression_dxt5`, pero **NO** `GL_OES_compressed_ETC1_RGB8_texture`
  ni `GL_OES_depth_texture`. ETC2 RGB8 decodifica bloques ETC1 idénticamente, así que T5 puede mapear
  `GL_ETC1_RGB8_OES` a `GL_COMPRESSED_RGB8_ETC2` (o descomprimir en CPU).
