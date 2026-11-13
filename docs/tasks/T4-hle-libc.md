# T4 — bionic HLE: libc, libm, libdl, pthread, libstdc++, liblog, sockets

> Historical task spec (November 2026). Paths refer to the original development machine.

Read `docs/PLAN.md`, `docs/ANALYSIS.md`, `docs/CONTRACTS.md`, `docs/imports.json`. Repo on `main`.

## Goal
Implement, with the API in `docs/CONTRACTS.md`, every non-GL/non-OpenSL import of `libZombieWood.so` and `libgenerator.so`
using the **32-bit bionic ABI** (ARM32 struct layouts, not the Windows x64 ones).

## Scope
- Memory: malloc/calloc/realloc/free/memalign on the guest heap; mem*/str*/wcs*/wmem*, ctype (`_ctype_`, `_tolower_tab_`, `_toupper_tab_` as guest data with the bionic layout).
- Stdio: an opaque guest `FILE` + `__sF` (stdin/out/err) with the bionic layout; fopen/fread/fwrite/fseek/ftell/fgets/fprintf/… on top of a **VFS**
  that maps Android paths to host folders: `/sdcard/gameloft/games/<...>/`, `/sdcard/android/data/com.gameloft.android.ANMP.GloftZOHM/files`,
  `/data/data/com.gameloft.android.ANMP.GloftZOHM/` → `<home>/data/` and `<home>/saves/` (configurable). If T1 says the game reads the OBB
  as a zip, the VFS must be able to mount the `.obb` (stored zip) read-only with the patch OBB layered on top.
- The complete printf family (including `%lld`, `%f` with softfp doubles, guest `%s`, `%ls`), sscanf/fscanf, strtod/strtol…, strftime/time/gmtime/localtime(_r)/mktime/clock_gettime/gettimeofday (32-bit structs).
- POSIX: open/read/write/close/lseek/stat/fstat/mkdir/unlink/rename/opendir/readdir/closedir (bionic arm `struct dirent`/`struct stat`), access, getcwd/chdir, getenv, sysconf, getpid/uid…
- pthread: create/join/detach, mutex (normal/recursive), cond (incl. timedwait), once, keys/specific, attr*, sched_yield, usleep/sleep — using T3's thread API. Types with 32-bit bionic sizes (`pthread_mutex_t` = 4 bytes, etc.).
- setjmp/longjmp (with T3's help if it needs to save Jit context — `ask` if the contract lacks it), `__cxa_atexit`, `__cxa_finalize`, `__cxa_guard_*`, `__cxa_pure_virtual`, `__stack_chk_fail`, `__assert2`, `abort`, `__errno`, `bsd_signal`/`sigaction` (no-op), `raise`.
- Complete libm (float and double, softfp), `__gnu_Unwind_Find_exidx`.
- libdl: dlopen/dlsym/dlclose/dlerror over the loaded libs and the HLE table.
- liblog: `__android_log_print/vprint` → the runtime log.
- sockets/getaddrinfo/gethostbyname/select/poll: fail cleanly (ENETUNREACH / NULL) so the game goes into offline mode.
- `system`, `tmpfile`, `tmpnam`, `getpwuid`: reasonable stubs.

## Deliverables
`src/hle/libc/` (one file per area), tests in `tests/hle_libc/` running test guest functions or calling the handlers directly.

## Proof
Green tests; `zombiewood --load-only` with no pending libc imports (paste the list of imports still without a handler: only GLES/OpenSL/JNI should remain); a VFS test opening a real `.bar` from `C:\Users\chdut\ZombiewoodDev\gamedata\`.

## Constraints
Edit only `src/hle/libc/`, `tests/hle_libc/` and their CMake. Contracts: `ask`. Commits `zombiewood(T4): ...`, no push.
