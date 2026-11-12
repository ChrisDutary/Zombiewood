# T4 — HLE de bionic: libc, libm, libdl, pthread, libstdc++, liblog, sockets

Lee `docs/PLAN.md`, `docs/ANALYSIS.md`, `docs/CONTRACTS.md`, `docs/imports.json`. Repo en `main`.

## Objetivo
Implementar con la API de `docs/CONTRACTS.md` todos los imports no-GL/no-OpenSL de `libZombieWood.so` y `libgenerator.so`
con la **ABI de bionic de 32 bits** (layouts de struct de ARM32, no los de Windows x64).

## Alcance
- Memoria: malloc/calloc/realloc/free/memalign sobre el heap guest; mem*/str*/wcs*/wmem*, ctype (`_ctype_`, `_tolower_tab_`, `_toupper_tab_` como datos guest con layout bionic).
- Stdio: `FILE` guest opaco + `__sF` (stdin/out/err) con layout bionic; fopen/fread/fwrite/fseek/ftell/fgets/fprintf/… sobre un **VFS**
  que mapea rutas Android a carpetas del host: `/sdcard/gameloft/games/<...>/`, `/sdcard/android/data/com.gameloft.android.ANMP.GloftZOHM/files`,
  `/data/data/com.gameloft.android.ANMP.GloftZOHM/` → `<home>/data/` y `<home>/saves/` (configurable). Si T1 indica que el juego lee el OBB
  como zip, el VFS debe poder montar el `.obb` (zip stored) en solo lectura con el patch OBB superpuesto.
- printf-family completa (incluye `%lld`, `%f` con doubles softfp, `%s` guest, `%ls`), sscanf/fscanf, strtod/strtol…, strftime/time/gmtime/localtime(_r)/mktime/clock_gettime/gettimeofday (structs de 32 bits).
- POSIX: open/read/write/close/lseek/stat/fstat/mkdir/unlink/rename/opendir/readdir/closedir (`struct dirent`/`struct stat` de bionic arm), access, getcwd/chdir, getenv, sysconf, getpid/uid…
- pthread: create/join/detach, mutex (normal/recursive), cond (incl. timedwait), once, keys/specific, attr*, sched_yield, usleep/sleep — usando la API de hilos de T3. Tipos con tamaños bionic de 32 bits (`pthread_mutex_t` = 4 bytes, etc.).
- setjmp/longjmp (con ayuda de T3 si requiere guardar contexto de Jit — `ask` si falta en el contrato), `__cxa_atexit`, `__cxa_finalize`, `__cxa_guard_*`, `__cxa_pure_virtual`, `__stack_chk_fail`, `__assert2`, `abort`, `__errno`, `bsd_signal`/`sigaction` (no-op), `raise`.
- libm completo (float y double, softfp), `__gnu_Unwind_Find_exidx`.
- libdl: dlopen/dlsym/dlclose/dlerror sobre las libs cargadas y tabla HLE.
- liblog: `__android_log_print/vprint` → log del runtime.
- sockets/getaddrinfo/gethostbyname/select/poll: fallar limpio (ENETUNREACH / NULL) para que el juego vaya a modo offline.
- `system`, `tmpfile`, `tmpnam`, `getpwuid`: stubs razonables.

## Entregables
`src/hle/libc/` (un archivo por área), tests en `tests/hle_libc/` ejecutando funciones guest de prueba o llamando handlers directo.

## Evidencia
Tests en verde; `zombiewood --load-only` sin imports libc pendientes (pega la lista de imports aún sin handler: solo deberían quedar GLES/OpenSL/JNI); prueba de VFS abriendo un `.bar` real desde `C:\Users\chdut\ZombiewoodDev\gamedata\`.

## Restricciones
Edita solo `src/hle/libc/`, `tests/hle_libc/` y su CMake. Contratos: `ask`. Commits `zombiewood(T4): ...`, sin push.
