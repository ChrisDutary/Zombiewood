# Zombiewood — plan de trabajo

Port nativo para Windows x64 de *Zombiewood* (Gameloft, Android 1.5.3, `com.gameloft.android.ANMP.GloftZOHM`)
al estilo Ship of Harkinian: este repo contiene **solo el runtime**; cada usuario aporta su propia copia
(APK + OBB). Nunca se commitean archivos del juego (ver `.gitignore`).

## Técnica: so-loader + CPU ARM32 por JIT

`libZombieWood.so` es ELF32 ARM (armeabi-v7a), 35,581 símbolos exportados (sin strip), 364 imports:

| Lib importada | Cantidad aprox. | Reemplazo |
|---|---|---|
| libGLESv2 | 99 | ANGLE (GLES2 sobre D3D11), traducción de punteros guest→host |
| libOpenSLES | 4 (engine + bufferqueue + play) | SDL2 audio |
| libc / libm / libdl / pthread (bionic) | ~240 | `hle/libc` con ABI bionic de 32 bits |
| libstdc++ (mínimo de Android) | — | `hle/libc` (operator new/delete, __cxa_*) |
| liblog | 2 | log a archivo/consola |
| sockets | 15 | stubs (sin red: servidores de Gameloft ya no existen) |

Entrada: `JNI_OnLoad` + 88 funciones `Java_*` (ver `docs/ANALYSIS.md`). No usa EGL ni libandroid.
Datos: OBB `main.1501…obb` (276 MB, `.bar`) + `patch.15325…obb`, rutas `/sdcard/gameloft/games/…` y `/data/data/com.gameloft.android.ANMP.GloftZOHM/`.

## Estructura

```
Zombiewood/
  CMakeLists.txt, CMakePresets.json, vcpkg.json
  src/
    core/      utilidades comunes (log, config, paths)
    loader/    carga ELF32 ARM, relocs, resolución de imports (T3)
    cpu/       dynarmic A32, memoria guest, puente host<->guest, hilos (T3)
    hle/libc/  bionic libc/libm/pthread/libstdc++ (T4)
    hle/gles/  GLES2 -> ANGLE (T5)
    hle/audio/ OpenSL ES -> SDL2 (T6)
    jni/       JNIEnv/JavaVM falsos + clases Java emuladas (T7)
    frontend/  main, ventana SDL2, input, ImGui (T2/T8/T9)
    installer/ extracción de APK/OBB del usuario (T9)
  docs/        PLAN.md, ANALYSIS.md, CONTRACTS.md, BUILD.md
  tools/       scripts de análisis/inspección
```

## Entorno de desarrollo (fuera del repo, el repo vive en OneDrive)

- Datos de prueba: `C:\Users\chdut\ZombiewoodDev\gamedata\` (`lib/libZombieWood.so`, `obb/*.obb`, `classes.dex`, `AndroidManifest.xml`)
- APK completo: `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk` (solo lectura)
- Build: `C:\Users\chdut\ZombiewoodDev\build\` (`cmake --preset` debe apuntar ahí)
- Análisis: `C:\Users\chdut\ZombiewoodDev\analysis\`
- Herramientas: `C:\Users\chdut\Tools\` (vcpkg, jdk-21, ghidra_12.1.4_PUBLIC, android\apktool.bat, jadx-headless-mcp); REA CLI `rea`

## Tareas (DAG)

| # | Tarea | Depende | Modelo |
|---|---|---|---|
| T1 | Análisis profundo (REA/Ghidra/JADX) → `docs/ANALYSIS.md` | — | Fable |
| T2 | Esqueleto CMake/vcpkg, ventana SDL2 + contexto GLES2 vía ANGLE | — | Sonnet |
| T3 | Loader ELF + CPU dynarmic + thunks de imports + bridge + hilos | T1, T2 | Fable |
| T4 | HLE libc/libm/pthread/libstdc++ | T3 | Opus |
| T5 | HLE GLES2 → ANGLE | T3 | Opus |
| T6 | HLE OpenSL ES → SDL2 | T3 | Sonnet |
| T7 | JNI/Java falso | T1, T3 | Opus |
| T8 | Integración y bucle de juego, input; jugar menú → partida | T4–T7 | Fable |
| T9 | Instalador de datos, config, saves, menú ImGui | T8 | Sonnet |
| T10 | Empaquetado Win64, README, QA | T9 | Sonnet |

## Reglas para todos los workers

1. Rama `main`, worktree compartido: edita **solo** las carpetas de tu tarea (+ tu sección en docs). Si necesitas tocar
   otra, pídelo al coordinador con `ask`.
2. No commitees archivos del juego ni builds. No hagas `git push`. Commits pequeños con prefijo `zombiewood(Tn):` y
   `git add` solo de tus rutas (otros workers trabajan en paralelo).
3. La interfaz entre módulos está en `docs/CONTRACTS.md` (la escribe T3). Cambios a contratos → `ask` al coordinador.
4. C++20, MSVC/clang-cl x64, sin dependencias fuera de vcpkg/FetchContent con versión fijada.
5. Evidencia de terminado: build limpio + la prueba que pide tu spec, con salida pegada en tu reporte.
