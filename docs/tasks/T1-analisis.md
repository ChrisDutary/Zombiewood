# T1 — Análisis profundo del juego (REA / Ghidra / JADX)

Lee primero `Zombiewood/docs/PLAN.md` (contexto, rutas, reglas). Repo: `C:\Users\chdut\OneDrive\Escritorio\2026-1GS242-Dutary-Christian`, rama `main`.

## Objetivo
Producir la especificación técnica que necesitan T3–T8 para reemplazar Android bajo `libZombieWood.so` sin adivinar.
Es una tarea de **solo lectura sobre el juego**: no modificas binarios ni datos.

## Insumos
- `C:\Users\chdut\ZombiewoodDev\gamedata\lib\libZombieWood.so` (ELF32 ARM, sin strip), `classes.dex`, `AndroidManifest.xml`, `obb\*.obb`
- APK completo: `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk`
- Imports/exports ya extraídos: `C:\Users\chdut\ZombiewoodDev\analysis\libZombieWood.dynsyms.json` (script `elfdyn.py` al lado)
- Herramientas: REA (MCP `rea` si está cargado, si no CLI `rea`; skill `reverse-engineer-anything`). Para el CLI exporta antes:
  `JAVA_HOME=C:\Users\chdut\Tools\jdk-21.0.13+9`, `GHIDRA_INSTALL_DIR=C:\Users\chdut\Tools\ghidra_12.1.4_PUBLIC`,
  `REA_JADX_MCP_JAR=C:\Users\chdut\Tools\android\jadx-headless-mcp-0.7.1-all.jar`, `REA_APKTOOL_COMMAND=C:\Users\chdut\Tools\android\apktool.bat`.
  Ghidra headless directo (`support\analyzeHeadless.bat`) también vale. Puedes instalar herramientas extra de análisis
  en `C:\Users\chdut\Tools` (p. ej. `pip install pyelftools capstone` con `python -m pip --user`).
- Proyectos Ghidra / salidas grandes: `C:\Users\chdut\ZombiewoodDev\analysis\` (nunca dentro del repo).

## Entregables (en el repo)
`Zombiewood/docs/ANALYSIS.md` con, como mínimo:
1. **ABI**: confirmar softfp vs hardfp (`.ARM.attributes` Tag_ABI_VFP_args), ARMv7 + VFPv3/NEON usados, Thumb-2 sí/no, TLS (`mrc p15` TPIDRURO), `__stack_chk_guard`, `.init_array`/`JNI_OnLoad` orden.
2. **Relocaciones**: tipos presentes en `.rel.dyn`/`.rel.plt` (R_ARM_RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32…) con conteos.
3. **Imports por categoría** con firma C esperada de cada uno (libc bionic 32-bit: layout de `FILE`/`__sF`, `struct stat`, `_ctype_`, `errno` vía `__errno`, `pthread_*` tipos de 32 bits, `jmp_buf` tamaño). Marca cuáles se usan de verdad (xrefs) y cuáles se pueden stubear.
4. **Secuencia de arranque** que hace el lado Java (desde JADX): orden exacto de llamadas nativas (`JNI_OnLoad`, `Game.nativeInit`, `GL2JNILib.setPaths/init/initGL/resize/step`, `GameRenderer.*`…), con argumentos y valores (rutas, tamaños, flags), y en qué hilo (UI vs GL thread).
5. **Llamadas nativo→Java**: todo `FindClass`/`GetMethodID`/`GetStaticMethodID`/`Call*Method`/`GetFieldID` que hace el `.so` (cadenas de clase/método/firma), qué hace cada método Java y qué debe devolver un stub razonable. Tabla lista para T7.
6. **Archivos**: rutas que abre (OBB: ¿lee el `.obb` como zip directamente o espera extraído en `/sdcard/gameloft/games/GloftZOHM/`?), saves, configs; cómo resuelve el patch OBB. Formato `.bar` (cabecera, índice) si es necesario para el loader de datos.
7. **Gráficos**: versión GLES y extensiones consultadas (`glGetString`), formatos de `glCompressedTexImage2D` usados (ETC1/PVRTC/ATC/DXT) y si hay fallback, uso de `glCompressedTexImage3DOES`, shaders (`.shd`, GLSL ES). Riesgos frente a ANGLE.
8. **Audio**: cómo usa OpenSL (formato PCM, sample rate, nº buffers, callback de bufferqueue en qué hilo), y vídeo (`SUtils.onVideoFinished` → los `.mp4` de `res/raw` los reproduce Java).
9. **Input**: firmas de `touchEvent`/`nativeOnTouch`/`nativeOnKeyDown`/`accelerometerEvent`, códigos de tecla Android que entiende, si soporta gamepad (`nativeXperiaToggleJoysticks`, `processTouchpadAsPointer`).
10. **Hilos**: `pthread_create` sitios y propósito; sincronización relevante.
11. **Red/DRM/IAP/Social**: qué rutas se pueden cortar devolviendo "sin conexión" sin bloquear el juego.
12. **Riesgos** y recomendaciones concretas para T3–T8.

Además `Zombiewood/tools/analysis/` con scripts reutilizables que hayas escrito (sin datos del juego) y
`Zombiewood/docs/imports.json`: lista de imports `{name, lib, category, signature, used, stub_ok}`.

## Restricciones
- Edita solo `Zombiewood/docs/ANALYSIS.md`, `Zombiewood/docs/imports.json`, `Zombiewood/tools/analysis/`.
- No copies código decompilado extenso al repo: describe comportamiento, firmas y constantes.
- Commit: `zombiewood(T1): ...` con `git add` solo de esas rutas. Sin push.

## Evidencia de terminado
`ANALYSIS.md` cubre los 12 puntos (marca explícitamente lo que no pudiste confirmar), `imports.json` tiene las 364 entradas, y el reporte incluye la secuencia de arranque y la tabla nativo→Java resumidas.
