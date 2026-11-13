# T1 — Deep analysis of the game (REA / Ghidra / JADX)

> Historical task spec (November 2026). Paths refer to the original development machine; the project then lived in the
> `Zombiewood/` folder of another repository.

Read `Zombiewood/docs/PLAN.md` first (context, paths, rules). Repo: `C:\Users\chdut\OneDrive\Escritorio\2026-1GS242-Dutary-Christian`, branch `main`.

## Goal
Produce the technical specification T3–T8 need to replace Android underneath `libZombieWood.so` without guessing.
This is a **read-only task with respect to the game**: you do not modify binaries or data.

## Inputs
- `C:\Users\chdut\ZombiewoodDev\gamedata\lib\libZombieWood.so` (ELF32 ARM, not stripped), `classes.dex`, `AndroidManifest.xml`, `obb\*.obb`
- Full APK: `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk`
- Imports/exports already extracted: `C:\Users\chdut\ZombiewoodDev\analysis\libZombieWood.dynsyms.json` (script `elfdyn.py` next to it)
- Tools: REA (MCP `rea` if loaded, otherwise the `rea` CLI; skill `reverse-engineer-anything`). For the CLI, export first:
  `JAVA_HOME=C:\Users\chdut\Tools\jdk-21.0.13+9`, `GHIDRA_INSTALL_DIR=C:\Users\chdut\Tools\ghidra_12.1.4_PUBLIC`,
  `REA_JADX_MCP_JAR=C:\Users\chdut\Tools\android\jadx-headless-mcp-0.7.1-all.jar`, `REA_APKTOOL_COMMAND=C:\Users\chdut\Tools\android\apktool.bat`.
  Running Ghidra headless directly (`support\analyzeHeadless.bat`) also works. You may install extra analysis tools
  in `C:\Users\chdut\Tools` (e.g. `pip install pyelftools capstone` with `python -m pip --user`).
- Ghidra projects / large outputs: `C:\Users\chdut\ZombiewoodDev\analysis\` (never inside the repo).

## Deliverables (in the repo)
`Zombiewood/docs/ANALYSIS.md` covering at least:
1. **ABI**: confirm softfp vs hardfp (`.ARM.attributes` Tag_ABI_VFP_args), ARMv7 + VFPv3/NEON in use, Thumb-2 yes/no, TLS (`mrc p15` TPIDRURO), `__stack_chk_guard`, `.init_array`/`JNI_OnLoad` order.
2. **Relocations**: types present in `.rel.dyn`/`.rel.plt` (R_ARM_RELATIVE, GLOB_DAT, JUMP_SLOT, ABS32…) with counts.
3. **Imports by category** with the expected C signature of each (32-bit bionic libc: layout of `FILE`/`__sF`, `struct stat`, `_ctype_`, `errno` via `__errno`, 32-bit `pthread_*` types, `jmp_buf` size). Mark which ones are really used (xrefs) and which can be stubbed.
4. **Startup sequence** performed by the Java side (from JADX): exact order of native calls (`JNI_OnLoad`, `Game.nativeInit`, `GL2JNILib.setPaths/init/initGL/resize/step`, `GameRenderer.*`…), with arguments and values (paths, sizes, flags), and on which thread (UI vs GL thread).
5. **Native→Java calls**: every `FindClass`/`GetMethodID`/`GetStaticMethodID`/`Call*Method`/`GetFieldID` the `.so` makes (class/method/signature strings), what each Java method does and what a reasonable stub should return. A table ready for T7.
6. **Files**: paths it opens (OBB: does it read the `.obb` as a zip directly or expect it extracted in `/sdcard/gameloft/games/GloftZOHM/`?), saves, configs; how it resolves the patch OBB. The `.bar` format (header, index) if needed by the data loader.
7. **Graphics**: GLES version and queried extensions (`glGetString`), `glCompressedTexImage2D` formats used (ETC1/PVRTC/ATC/DXT) and whether there is a fallback, use of `glCompressedTexImage3DOES`, shaders (`.shd`, GLSL ES). Risks with ANGLE.
8. **Audio**: how it uses OpenSL (PCM format, sample rate, number of buffers, bufferqueue callback thread), and video (`SUtils.onVideoFinished` → the `.mp4` files in `res/raw` are played by Java).
9. **Input**: signatures of `touchEvent`/`nativeOnTouch`/`nativeOnKeyDown`/`accelerometerEvent`, Android key codes it understands, whether it supports gamepads (`nativeXperiaToggleJoysticks`, `processTouchpadAsPointer`).
10. **Threads**: `pthread_create` call sites and purpose; relevant synchronization.
11. **Network/DRM/IAP/Social**: which paths can be cut off by returning "offline" without blocking the game.
12. **Risks** and concrete recommendations for T3–T8.

Also `Zombiewood/tools/analysis/` with any reusable scripts you wrote (no game data) and
`Zombiewood/docs/imports.json`: a list of imports `{name, lib, category, signature, used, stub_ok}`.

## Constraints
- Edit only `Zombiewood/docs/ANALYSIS.md`, `Zombiewood/docs/imports.json`, `Zombiewood/tools/analysis/`.
- Do not copy large amounts of decompiled code into the repo: describe behavior, signatures and constants.
- Commit: `zombiewood(T1): ...` with `git add` of those paths only. No push.

## Proof of completion
`ANALYSIS.md` covers the 12 points (explicitly marking what you could not confirm), `imports.json` has all 364 entries, and the report includes summaries of the startup sequence and the native→Java table.
