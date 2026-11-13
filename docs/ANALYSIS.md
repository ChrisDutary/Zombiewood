# Zombiewood — technical analysis (T1)

Specification for T3–T8: what has to be emulated from Android underneath `libZombieWood.so`.

- Binary: `lib/armeabi-v7a/libZombieWood.so`, 13,500,416 bytes, MD5 `73a6dc0f00e1690dc3122019054f89e1`, `DT_SONAME = libAndroid.so.full`.
- APK: `com.gameloft.android.ANMP.GloftZOHM` 1.5.3 (versionCode 15325, minSdk 14, targetSdk 19).
- Engine: **Glitch** (Gameloft's Irrlicht fork) + **glf** (platform layer) + **Vox** (audio) + **gameswf** (Flash UI) + Lua,
  FreeType, libpng, libjpeg, zlib, curl, OpenSSL, TinyXML, JsonCpp, all statically linked (including libstdc++/gnustl and the unwinder).
- Addresses in this document are **ELF virtual addresses** (base 0). In Ghidra the default base is `0x10000` (add it).
- Convention: **[C]** = confirmed in the binary/dex; **[I]** = inferred (consistent with the evidence, full path not verified);
  **[NC]** = not confirmed. Anything without a mark is [C].
- How it was obtained: `tools/analysis/*.py` (pyelftools + capstone) on the `.so`, Ghidra 12.1.4 headless (`tools/analysis/ghidra/DecompileList.java`)
  and JADX 1.5 (the `jadx-headless-mcp` API) on the APK. Nothing from the game has been copied into the repo.

---

## 1. ABI

| Property | Value | Evidence |
|---|---|---|
| Class/endianness | ELF32 LE, `ET_DYN`, `EM_ARM`, `e_flags = 0x05000000` (EABI v5, **without** `EF_ARM_ABI_FLOAT_HARD/SOFT`) | header |
| CPU | ARMv7-A (`Tag_CPU_arch = 10`, `Tag_CPU_arch_profile = 'A'`, `Tag_CPU_name = "ARM v7"`) | `.ARM.attributes` |
| Instruction sets | `Tag_ARM_ISA_use = 1`, `Tag_THUMB_ISA_use = 2` → **Thumb-2** | `.ARM.attributes` |
| FP | `Tag_FP_arch = 4` → **VFPv3-D16**; no `Tag_Advanced_SIMD_arch` → **no NEON** | `.ARM.attributes` |
| FP argument passing | **softfp**: `Tag_ABI_VFP_args` **absent** (= base AAPCS). `Tag_ABI_HardFP_use = 3` (SP+DP) | `.ARM.attributes` |
| `wchar_t` | 4 bytes (`Tag_ABI_PCS_wchar_t = 4`); 32-bit enums (`Tag_ABI_enum_size = 2`) | `.ARM.attributes` |
| Alignment | `Tag_ABI_align_needed/preserved = 1` → stack 8-aligned at public calls | `.ARM.attributes` |
| Hardware division | `Tag_DIV_use = 1` (unused); divisions go through internal `__aeabi_*div*` | `.ARM.attributes` |

softfp confirmed in code: `VoxSetDataThreshold(double)` starts with `vmov d6, r0, r1`; `glf::AndroidAddTouchEvent` converts integers
and passes the `float`s in `r3`/the stack; `Java_…_GameRenderer_nativeInit` reads the 4th argument from `r3` and the 5th from the stack.
**Rule for import thunks (T3/T4/T5):** `float` in a core register/4-byte stack slot; `double`/`long long` in the pair
`r0:r1` or `r2:r3` (aligned pair: if `r1` is next it skips to `r2:r3`) or on the 8-aligned stack; `float` returns in `r0`, `double`/`int64` in `r0:r1`.

Actual instruction usage (`insn_stats.py`, 26,999 functions with symbols):

- **26,857 Thumb-2 functions (6.16 MB) and 142 ARM (21 KB)**. The ARM ones are the C++ runtime/unwinder (`__cxa_*`, `__dynamic_cast`, `__gnu_Unwind_*`,
  type_info) and the `__aeabi_*` helpers. **The PLT is ARM** (12-byte stubs) → every import call from Thumb is a `BLX` (interworking required).
- **Linker veneers** at the end of `.text` (≈`0xb0be60`–`0xb0c078`): `bx pc; nop` (Thumb) + `ldr ip,[pc]; add pc,ip,pc; .word off` (ARM). They are used by
  Thumb→PLT *tail calls* (e.g. `glShaderSource`, `memcpy`, `free`…). The JIT must support `bx pc` in Thumb and `add pc, ip, pc` in ARM.
- VFP: ~80,000 instructions (`vldr/vstr/vmov/vmul/vmla/vmls/vnmls/vnmul/vadd/vsub/vdiv/vsqrt/vabs/vneg/vcvt/vcmp/vcmpe/vmrs/vpush/vpop/vldm/vstm`,
  many conditional inside IT blocks). Only `d0–d15`.
- NEON: **not used** [I]. The sweep counts ~2,700 NEON mnemonics but they fall in *literal pools* decoded as code (the build does not declare
  Advanced SIMD). dynarmic supports them anyway; no extra work needed.
- **`ldrex/strex` + barriers (`dmb`)**: ~33,000 real instructions — atomic reference counters (`boost::intrusive_ptr`, `IReferenceCounted`,
  `std::string` COW). With several guest threads **a shared exclusive monitor is required** (see §10).
- ARMv6T2+: `movw/movt`, `ubfx/sbfx/bfi/bfc`, `uxtb/uxth/sxtb/sxth`, `rev/rev16`, `clz`, `mls`, `umull/smull/umlal`, `tbb/tbh`, `IT` blocks (~24,000).
- `svc`, `bkpt`, `udf`, `mrc/mcr`, `sdiv/udiv`: only appear in data decoded as code; **there are no direct syscalls**.

TLS / stack protection / startup:

- **There is no `PT_TLS` and no `TPIDRURO` reads** (0 matches for `mrc p15,0,rX,c13,c0,3` in ARM and Thumb). All per-thread state goes through
  `pthread_key_create/getspecific/setspecific` (79 calls to `pthread_getspecific`). There is no need to emulate the TLS register.
- `__stack_chk_guard` is imported as data (`R_ARM_GLOB_DAT`, 1,122 calls to `__stack_chk_fail`): the code does `ldr rX,[GOT]; ldr rX,[rX]`.
  A `uint32_t` in guest memory with a constant value is enough.
- `PT_GNU_STACK` RW (non-executable stack), `PT_GNU_RELRO` (`0xc84370`, 334,992 bytes; can be ignored), `DT_SYMBOLIC`, `DT_FLAGS = 0x0a`
  (`SYMBOLIC|BIND_NOW`), `DT_FLAGS_1 = NOW` → resolve **every** `JUMP_SLOT` at load time (no lazy binding; `PLT0` is unused).
- **Loader startup order** (what bionic's `dlopen` does): map → relocate → run `.init_array` (**552 pointers**, `0xc91828`, all into
  `.text`, mostly Thumb with bit 0) in order → `JNI_OnLoad(vm, NULL)`. There is no `DT_INIT` or `.preinit_array`. `.fini_array` has 1 entry (`0xab7160`,
  `__do_global_dtors_aux` → `__cxa_finalize`); do not run it. Static constructors call imports (`__cxa_atexit`, `malloc`,
  `pthread_mutex_init`, `__cxa_guard_*`…): **the whole libc HLE must be ready before `.init_array`**.
- C++ exceptions: the ARM EHABI unwinder lives inside the `.so` and asks for the table through the import **`__gnu_Unwind_Find_exidx(pc, &count)`**: return
  `base + 0xb1c678` and `count = 0x2f6a0 / 8 = 24,276` (`.ARM.exidx` section, `PT_ARM_EXIDX`). Per-thread EH state uses `pthread_key` (`__cxa_get_globals`).

Segments: `PT_LOAD` R+X `0x0–0xc82d5c` (file = memory), `PT_LOAD` RW vaddr `0xc84370` (file offset `0xc83370`, filesz `0x5c7a0`, memsz `0xab4c4`;
`.bss` = `0xce0b10`+`0x4ed24`). Reserve ≥ `0xd30000` contiguous bytes. Exports 35,581 symbols (not stripped) — T3 can resolve any internal function by name.

---

## 2. Relocations

Only `SHT_REL` (implicit addend at the target). `elf_abi.py`:

| Section | Type | Count | Handling |
|---|---|---|---|
| `.rel.dyn` | `R_ARM_RELATIVE` (23) | 56,376 (= `DT_RELCOUNT`, they come first) | `*p += base` |
| `.rel.dyn` | `R_ARM_ABS32` (2) | 6,467, **all against undefined symbols** | `*p += S` (in-place addend, normally 0) |
| `.rel.dyn` | `R_ARM_GLOB_DAT` (21) | 18, all against undefined symbols | `*p = S` |
| `.rel.plt` | `R_ARM_JUMP_SLOT` (22) | 356 | `*p = S` (resolve at load time) |

There are no `TLS_*`, `COPY`, `IRELATIVE`, `REL32` or text relocations. Because of `DT_SYMBOLIC`, no relocation targets symbols defined in the `.so` itself.

Details of the relocations against imports (this matters for how each import materializes in guest memory):

- `R_ARM_ABS32`: `__cxa_pure_virtual` ×6,457 (vtables), `malloc` ×3, `free` ×3, `realloc` ×2, `calloc` ×1, `strdup` ×1 (allocator tables of
  zlib/curl/OpenSSL). → **every imported function needs a callable guest address** (a thunk); intercepting the PLT is not enough.
- `R_ARM_GLOB_DAT`: the data `__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `SL_IID_ENGINE`, `SL_IID_BUFFERQUEUE`, `SL_IID_PLAY`;
  and function pointers `malloc`, `free`, `calloc`, `realloc`, `strdup`, `strcmp`, `fread`, `fwrite`, `fputc`, `__gnu_Unwind_Find_exidx`.
- 364 imports = 356 functions with `JUMP_SLOT` + 8 data objects.

---

## 3. Imports

The complete machine-readable list is in [`imports.json`](imports.json) (364 entries: `name, lib, category, kind, signature, used, calls, data_relocs,
stub_ok, note`). `used` comes from `import_xrefs.py` (direct BL/BLX to the PLT stub + veneers + data relocations): **all 364 have at least one
reference**; `stub_ok = true` marks those that can return a fixed value/failure without affecting the game.

Summary by category (number of symbols · notes for T4):

| Category | Count | Notes |
|---|---|---|
| `data` (libc) | 5 | `__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_` |
| `libc-runtime` | 22 | `__assert2` (5,588 sites), `__cxa_atexit`, `__errno`, `__gnu_Unwind_Find_exidx`, `abort/exit`, `setjmp/longjmp`, `qsort`, `lrand48/srand48`, uid/pid, signals (OpenSSL only) |
| `libc-memory` | 9 | `malloc/free/calloc/realloc` + `mem*` (`memcpy` 3,531 sites) |
| `libc-string` | 39 | `str*`, `ato*`, `strto*`, `is*`, `tolower/toupper`, `setlocale` |
| `libc-wchar` | 22 | 32-bit `wchar_t`; `wcslen` 190 sites, `wmemcpy` 278 (glitch `stringw`, gameswf) |
| `libc-stdio` | 37 | real `FILE*`s (see below); `sprintf` 1,051, `sscanf` 58, `snprintf` 158 |
| `libc-fs` | 20 | `stat/fstat`, `opendir/readdir`, `mkdir`, `unlink`, `pread`, `fsync`… |
| `libc-time` | 15 | `gettimeofday`, `clock`, `clock_gettime`, `time`, `localtime`, `usleep`, `sleep` |
| `libc-pthread` | 29 | see §10 |
| `libc-net` | 23 | all `stub_ok` (see §11) |
| `libm` | 34 | `float` and `double` variants, softfp |
| `libstdc++` | 4 | `__cxa_guard_acquire/release/abort`, `__cxa_pure_virtual`. Does **not** import `operator new/delete` (static gnustl → `malloc`) |
| `liblog` | 2 | `__android_log_print/vprint` |
| `opensles` | 4 | `slCreateEngine` + 3 IIDs (a dead path by default, §8) |
| `gles2` | 99 | see §7 |

Nothing is imported from `libdl` (even though it is `DT_NEEDED`), nor `libandroid`, `libEGL`, `libz` (internal zlib), `dlopen/dlsym`, `mmap`, `sem_*`,
`pthread_exit/detach/kill`, `nanosleep`, `statfs`, `fileno`, `ferror`, `getc_unlocked`.

### 32-bit bionic layouts the guest touches directly

**`FILE` (84 bytes) and `__sF`** — the code uses the inline macros from `<stdio.h>`, so the `FILE*` returned by `fopen` must be a real structure in guest memory:

```c
struct __sbuf { unsigned char *_base; int _size; };
struct FILE {                       /* sizeof = 84 */
    unsigned char *_p;      /*  0 */    int _r;            /*  4 */    int _w;          /*  8 */
    short _flags;           /* 12 */    short _file;       /* 14 */
    struct __sbuf _bf;      /* 16 */    int _lbfsize;      /* 24 */
    void *_cookie;          /* 28 */    int (*_close)();   /* 32 */    int (*_read)();  /* 36 */
    fpos_t (*_seek)();      /* 40 */    int (*_write)();   /* 44 */
    struct __sbuf _ext;     /* 48 */    unsigned char *_up;/* 56 */    int _ur;         /* 60 */
    unsigned char _ubuf[3]; /* 64 */    unsigned char _nbuf[1]; /* 67 */
    struct __sbuf _lb;      /* 68 */    int _blksize;      /* 76 */    fpos_t _offset;  /* 80 */
};
extern FILE __sF[3];                /* stdin = &__sF[0], stdout = +84, stderr = +168 */
```

- `_file` (**offset 14**): `glf::FileStreamImpl::Impl::ReadWithOffset` does `pread(*(short*)(fp+14), buf, n, off)` and `Close` does `fsync(*(short*)(fp+14))`
  (the `fileno` macro). This is the engine file system's **asynchronous/positioned read path**: `_file` must hold an fd that `pread` understands.
- `_flags` (**offset 12**): inline `ferror`/`feof` read `__SERR = 0x40` / `__SEOF = 0x20` (Lua `liolib`, libpng). Keep those bits (at least 0 / correct EOF).
- `_p/_r/_w`: no evidence of inline `getc/putc` (they are imported as functions) [I]; leaving them at 0 is safe (`_r = 0`, `_w = 0` force the slow path).
- `stdout/stderr` are passed as `&__sF[1]`, `&__sF[2]` to `fprintf/fwrite/fputs/fflush`.

**`struct stat` (104 bytes, ARM EABI)**: `st_dev` u64 @0, pad @8, `__st_ino` u32 @12, `st_mode` u32 @16, `st_nlink` u32 @20, `st_uid` @24, `st_gid` @28,
`st_rdev` u64 @32, pad @40, `st_size` **s64 @48**, `st_blksize` u32 @56, `st_blocks` u64 @64, `st_atime` @72 (+nsec @76), `st_mtime` @80, `st_ctime` @88,
`st_ino` u64 @96. `S_IFDIR = 0x4000`, `S_IFREG = 0x8000`.

**`struct dirent`**: `d_ino` u64 @0, `d_off` s64 @8, `d_reclen` u16 @16, `d_type` u8 @18 (`DT_DIR = 4`, `DT_REG = 8`), `d_name[256]` @19.

**`_ctype_`**: `const char *_ctype_` points to a 257-byte table; indexed as `(_ctype_ + 1)[c]`. Bits: `_U 0x01, _L 0x02, _N 0x04, _S 0x08, _P 0x10,
_C 0x20, _X 0x40, _B 0x80`. `_tolower_tab_` / `_toupper_tab_`: `const short *` to tables of 257 `short`s, `(tab + 1)[c]`. Careful: all three symbols are
**pointers** (the GOT points to the pointer variable, which points to the table).

**`errno`**: `int *__errno(void)`; one `int` per guest thread. Linux values (`ENOENT 2, EINTR 4, EAGAIN 11, EACCES 13, EEXIST 17, EINVAL 22, ETIMEDOUT 110`…).

**`jmp_buf`**: `long[64]` (256 bytes). `setjmp/longjmp` (13/27 sites: libpng, libjpeg, Lua, FreeType, GLU tess) must be implemented **on top of the guest
CPU state** (r4–r11, sp, lr, d8–d15, fpscr), not with the host's `setjmp`.

**pthread (bionic 4.x)**: `pthread_t = long`; `pthread_mutex_t = { int volatile value; }` (**4 bytes**); `pthread_cond_t = { int volatile value; }` (4 bytes);
`pthread_attr_t` 24 bytes; `pthread_key_t = int`; `pthread_once_t = int`; `pthread_mutexattr_t = long`. Host objects do not fit inline → a side table
indexed by guest address (with lazy creation for zero-initialized static mutexes; bionic initializers: normal `0`, recursive `0x4000`, errorcheck `0x8000`).

**time**: 32-bit `time_t`/`clock_t`/`off_t`; 8-byte `timeval`/`timespec`; 44-byte `struct tm` (9 `int`s + `long tm_gmtoff` + `const char *tm_zone`);
`CLOCKS_PER_SEC = 1,000,000`.

**varargs**: on ARM EABI `va_list` is `struct { void *__ap; }` (one word, passed by value). `printf` family: `double`/`long long` 8-aligned in the
argument area. `sscanf` needs a complete implementation (`%f %d %x %s %c %[...] %n`, widths).

Guest callbacks the HLE must be able to call: the `qsort` comparator (13 sites), the `pthread_create` start routine, `pthread_key_create` destructors,
`pthread_once`, and —if kept— `__cxa_atexit`.

`lrand48/srand48` (47/5 sites): the gameplay RNG; implement the exact 48-bit LCG (`X = X·0x5DEECE66D + 0xB`, `srand48` sets `X = seed<<16 | 0x330E`).

`sysconf`: 1 site, `GetNumberOfCores()` → `sysconf(0x61 /*_SC_NPROCESSORS_ONLN*/)`; feeds the graphics profile (`GetProfileGPU`).

---

## 4. Startup sequence (Java side → native)

Extracted from `classes.dex` (`Game`, `GL2JNILib`, `GL2JNIView`, the custom `GLSurfaceView`, classes `as`/`at`/`av`, `GameRenderer`). The APK is a *repack*:
the real launcher is `com.dataManager.MainActivity` (it extracts `assets/game.apk` and `assets/archiveObbCache.zip.00N`), which ends up launching `…GloftZOHM.Game`.

Java threads: **UI** (main looper) and **GL** (the `GLSurfaceView`'s `GLThread`, the only thread that does GL and calls `step`).

| # | Thread | Call | Arguments / native effect |
|---|---|---|---|
| 0 | UI | `System.loadLibrary("ZombieWood")` → `.init_array` → **`JNI_OnLoad(vm, 0)`** | `VoxSetJavaVM(vm)`; `mJavaVM = vm`; `vm->GetEnv(&env, 0x10004)` (if ≠ 0 returns −1); `AndroidOS_JNIOnload(vm)`: stores `AndroidOS_JavaVM`, `AndroidOS_GetEnv()`, **`AndroidOS::Init()`** (FindClass of `…/GLUtils/SUtils`, `…/GLUtils/Device`, `…/installer/GameInstaller` and their methods — on failure it calls `exit`), `InAppBilling_setJavaVM`, `SplashScreen_setJavaVM`, `AdServer_setJavaVM`, `GameUtils_setJavaVM`. Returns `0x10004` |
| 1 | UI | `Game.onCreate`: `PortingJNI.Init`, `Tracking.init` (Java only) | — |
| 2 | UI | `Game.onStart`: **`Game.nativeSetGPUInfo(GraphicChooser.b)`** | the `GL_RENDERER` string obtained earlier by the `GraphicChooser` activity; stored in `GetPhoneGPUNamePointer()` |
| 3 | UI | (the `GameInstaller` activity: DRM + OBB extraction; calls `GameInstaller.initNative`, `GDRMPolicy.*`, `nativeStart`…) | **Installer only; not needed to play** (§11). Ends with `setResult(1)` |
| 4 | UI | `Game.onActivityResult(100, 1)` → **`GL2JNILib.init()`** | `env->GetJavaVM`; `NewGlobalRef(GL2JNILib class)`; caches the static IDs `getResource`, `setupPaths`, `createView`, `setViewSettings`, `setCurrentContext`, `enableAccelerometer`; `glf::AndroidInit()` → calls Java **`GL2JNILib.createView()`** |
| 4a | UI | (inside `createView`) `new GL2JNIView` → `new at()` → **`GameRenderer.nativeInit(Game.getManufacture(), displayW, displayH, Game.O)`** | `s_windowWidth = displayW`, `s_windowHeight = displayH` (screen size in pixels). `manufacture`: 0 generic, 1 Motorola, 2 Samsung, 3 HTC, 4 Sony, 5 Sharp… (use **0**). `Game.O` = versionName `"1.5.3"` (unused natively) |
| 5 | UI | **`Game.nativeInit()`** | `NewGlobalRef(Game class)`; caches ~60 static `Game` IDs (table §5.1) and **immediately calls** `GetPhoneManufacturer/Model/Firmware/Device/CPUName/CPUFreq/GPUName`, `getSDFolder`, `getSaveFolder`, `GetIMEI`, `GetWSClose`, copying the strings into globals (`GetSDFolderPathPointer()`, `GetSaveFolderPathPointer()`…) |
| 6 | UI | `InAppBilling.init(F)` → `InAppBilling.nativeInit…` | optional (IAP) |
| 7 | UI | `Game.onResume` (1st time): `PlatformAndroid.nativeInit()`, `FacebookAndroidGLSocialLib.nativeInit()`, `GameAPIAndroidGLSocialLib.nativeInit()` | cache the class + social IDs (table §5.3). Optional |
| 8 | **GL** | `ConfigChooser`: **`GL2JNILib.initGL()`** | empty function |
| 9 | GL | **`GL2JNILib.getViewSettings()`** | `m_gAppPath = sdFolder + "/"`; `glf::AndroidGetViewSettings()`: if there is no app → creates `glf::Globals`, **`NewApp(0, argv)`** (`new Application`, 0x11f88 bytes), `App::MyInit()` → `ThreadMgr::Init`, `Application::Init(CreationSettings&)`; then calls Java **`GL2JNILib.setViewSettings(pixelSize, zBuffer, stencil, csaa, flag)`** |
| 10 | GL | `eglCreateContext` (**GLES 2** client) → **`GL2JNILib.getNumExtraContext()`** | returns `CreationSettings+0x3c` = **0** (the default; nobody changes it) → Java creates no extra pbuffer contexts |
| 11 | GL | `onSurfaceCreated` → **`GL2JNILib.stateChanged(true)`** | sends `CoreEvent{type 0x65, 5}` (5 = context created; `false` → 4) |
| 12 | GL | `Game.GetPhoneInfo()` → `nativeSetPhoneModel(Build.MODEL)`, `nativeSetIMEI(id)`, **`nativeSetGPUInfo(glGetString(GL_RENDERER))`**, `nativeSetCPUInfo(maxFreqKHz float)` | Java also computes `GetTextureFormat()` from `GL_EXTENSIONS` (1 PVRTC, 2 ATC, 3 DXT, 4 ETC1) |
| 13 | GL | `onSurfaceChanged(w,h)` → `glViewport(0,0,w,h)` (Java) + **`GL2JNILib.resize(w, h)`** | `glf::AndroidResizeScreen`: stores the size in `AppImpl`, marks the thread as the main one |
| 14 | GL | every frame: **`GL2JNILib.step()`** (unless paused) and then `eglSwapBuffers` | `glf::AndroidUpdate()` → `gApp->vtbl[0x24]()` (= `App::Update` → `Application::Update`). The **first** `Application::Update` runs `Application::SetupDevice()` (creates the Glitch device/GLES2 driver, mounts the OBBs, `Application::Init(device)`, `ExtractInitFiles`, `GLOTManager::InitManager`, `PostInit`). Java does the *swap*, not the native code |
| 15 | UI | focus/pause: `GL2JNILib.pause()` / `resume()`, `Game.nativeVoxSuspend()` / `nativeVoxResume()` | `appPause/appResume`; `resume` queries `isWifiEnabled` again |

Notes:

- `GL2JNILib.setPaths(String,String,String)` exists but **Java never calls it** and the native one is empty. Paths arrive through `getSDFolder/getSaveFolder/GetObbPath`.
- `Application::Init(CreationSettings&)` (step 9) sets: width/height = `s_windowWidth/Height` from step 4a, `[0x2c] = 16` and `[0x2d] = 16`, `[0x28] = 2`,
  `[0x44] = 1`, and `[0x13] = 1` only if the lowercase GPU name contains one of: `adreno`, `powervr sgx`, `nvidia ap`, `nvidia tegra`,
  `mali-400 mp`… (list at `0xb4c088`). With that, `setViewSettings` receives `(16, 16, 8 or 0, 4, x)`: RGB565, depth ≥16 (Java tries 24 and falls back to 16), stencil 8 if
  the GPU is in the list. **Before that** it calls `CheckDiskSpace()`; if it returns 0, `MyInit` fails ("MyInit failed") [NC: what it queries has not been analyzed; no `statfs` is imported].
- `GameRenderer.nativeRender/nativeResize/nativeDestroy` and `GameGLSurfaceView.*` belong to the old path (not used by `Game`); `nativeRender` = `appUpdate`.
- Recommended minimal order for the port (T7/T8): 0 → 2 → 4 (+4a) → 5 → [7] → 9 → 11 → 12 → 13 → loop 14. `nativeSetGPUInfo` **must** come before 9.

---

## 5. Native → Java calls

How the `.so` uses JNI:

- Getting a `JNIEnv`: `AndroidOS_GetEnv()` (its own pthread key; if there is no env → `vm->AttachCurrentThread(&env, 0)`), `NVThreadGetCurrentJNIEnv()`
  (`AttachCurrentThread` + `GetEnv(…, 0x10004)`), and `GetEnv(&env, 0x10002/0x10004/0x10006)` blocks; if it returns `-2 (JNI_EDETACHED)` they do
  `AttachCurrentThread` … `DetachCurrentThread`. **Recommended stub: `GetEnv` and `AttachCurrentThread` always return 0 and a valid `JNIEnv*` for the
  calling thread; `DetachCurrentThread` = 0.** `JavaVM` slots used: `AttachCurrentThread` (+0x10), `DetachCurrentThread` (+0x14), `GetEnv` (+0x18).
- The game's own classes are resolved with `FindClass("com/gameloft/android/ANMP/GloftZOHM" + "/Suffix")` (built with `sprintf`/`memcpy`, sometimes from
  `"com.gameloft.android.ANMP.GloftZOHM"` replacing `.`→`/`) or by using the `jclass` that arrives as the 2nd argument of `Java_*_nativeInit` → `NewGlobalRef`.
- **Almost everything is static**: `GetStaticMethodID` + `CallStatic<T>Method`. The `.so` uses both the variadic slots (`CallStaticIntMethod` +0x204,
  `CallStaticVoidMethod` +0x234, `CallStaticObjectMethod` +0x1c8…) and the **`…V` ones with a `va_list`** (the exported inline wrappers `_JNIEnv::CallStaticVoidMethod(...)`
  do `push {r2,r3}` and call the `V` slot, e.g. +0x238). T7 must implement both families, reading arguments from `r2/r3`+stack or from the
  guest `va_list` (`float` promoted to `double`, `jlong`/`jdouble` 8-aligned).
- `JNIEnv` slots seen: `FindClass`, `GetObjectClass`, `NewGlobalRef`, `DeleteGlobalRef`, `DeleteLocalRef`, `PushLocalFrame`, `PopLocalFrame`,
  `GetMethodID`, `GetStaticMethodID`, `GetFieldID`*, `GetStaticFieldID`, `GetStaticObjectField`, `GetStaticIntField`*, `NewObject`, `Call<T>Method(V)`,
  `CallNonvirtual<Void|Int|Object|Boolean>Method(V)`, `CallStatic<Void|Int|Long|Float|Boolean|Object>Method(V)`, `NewStringUTF`, `GetStringUTFChars`,
  `ReleaseStringUTFChars`, `GetStringUTFLength`*, `GetArrayLength`, `NewByteArray`, `GetByteArrayElements/Release…`, `GetByteArrayRegion`*,
  `SetByteArrayRegion`*, `GetIntArrayElements/Release…`, `GetObjectArrayElement`, `GetPrimitiveArrayCritical/Release…`, `GetJavaVM`,
  `ExceptionCheck/Clear`*; **not** `RegisterNatives` (everything is a `Java_*` export). (* = [I], seen by offset without following the full flow; implementing the complete JNI 1.6 table is cheap and avoids surprises.)
- **A stub must tolerate null `jclass`/`jmethodID`** (if T7 decides not to call some `nativeInit`, the native code will call with 0 IDs): return 0/NULL.

"Stub" legend: the value the fake Java class should return.

### 5.1 `com/gameloft/android/ANMP/GloftZOHM/Game` (IDs cached in `Game.nativeInit`; all `static`)

| Method | JNI signature | What it does on Android | Stub |
|---|---|---|---|
| `getSDFolder` | `()Ljava/lang/String;` | data folder (pref. `SDFolder`), by default `/sdcard/android/data/<pkg>/files` | guest data path, **without** a trailing `/` |
| `getSaveFolder` | `()Ljava/lang/String;` | by default `/data/data/<pkg>` | guest saves path, without a trailing `/` |
| `GetObbPath` | `()Ljava/lang/String;` | `<ext>/Android/obb/<pkg>/` | guest path **with a trailing `/`** (the native code does `strcat(path, name)`) |
| `GetMainObbName` | `()Ljava/lang/String;` | constant | `"main.1501.com.gameloft.android.ANMP.GloftZOHM.obb"` |
| `GetPatchObbName` | `()Ljava/lang/String;` | constant | `"patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb"` (or `""` if it does not exist: the native code skips it) |
| `GetExtraObbName` | `()Ljava/lang/String;` | pref. `extraFile` | `""` |
| `GetPhoneManufacturer` / `GetPhoneModel` / `GetPhoneDevice` / `GetPhoneFirmware` | `()Ljava/lang/String;` | `Build.*` | `"samsung"`, `"GT-I9500"`, `"ja3g"`, `"4.4.2"` (anything; avoid models in the Xperia/`droid2` list) |
| `GetPhoneCPUName` | `()Ljava/lang/String;` | the `Processor` line of `/proc/cpuinfo` | `"ARMv7 Processor rev 3 (v7l)"` |
| `GetPhoneCPUFreq` | `()F` | max kHz | `1900000.0f` |
| `GetPhoneGPUName` | `()Ljava/lang/String;` | `GL_RENDERER` | the same as passed to `nativeSetGPUInfo` (e.g. `"Adreno (TM) 330"`, see §7) |
| `GetIMEI` | `()Ljava/lang/String;` | deviceId/serial | a fixed string, e.g. `"000000000000000"` |
| `GetWSClose` | `()Z` | welcome screen flag | `false` |
| `getNumberOfCPUCores` | `()I` | number of cores | 4 |
| `getManufacture` | `()I` | manufacturer code | 0 |
| `GetTextureFormat` | `()I` | 1 PVRTC / 2 ATC / 3 DXT / 4 ETC1 | **4** |
| `GetPhoneLanguage` | `()I` | 0 en,1 fr,2 de,3 es,4 it,5 pt/br,6 ja,7 ko,8 zh,9 ru | per config (0 by default) |
| `GetPhoneLanguageS` / `GetCurrentLocale` | `()Ljava/lang/String;` | `Locale` | `"English (United States)"` / `"en_US"` |
| `GetCurrentTime` | `()J` | `System.currentTimeMillis()` | real clock in ms |
| `isWifiEnabled` | `()I` | 1 if there is wifi | **0** (→ `ZombiesGame::SetInternet(false)`) |
| `IsJailBroken` | `()I` | `Superuser.apk` exists | 0 |
| `GetXperiaSlideout` | `()I` | 2 = not an Xperia Play | 2 |
| `keepScreenOn` | `(Z)Z` | wake lock | returns the argument |
| `hasClosedActivity` | `()I` | GLLive flag | 0 |
| `checkFreeCashReady` | `()Z` | ads | `false` |
| `getRewardsAvailable` / `getRewardsUser` / `getInstalledGames` / `getGLLivePassword` / `getGLLiveUsername` | `()Ljava/lang/String;` | Gameloft services | `""` |
| `Exit` | `()V` | kills the process | close the host app |
| `sendAppToBackground` / `Pause` | `()V` | go HOME | minimize or no-op |
| `openBrowser` | `(Ljava/lang/String;)V` | `ACTION_VIEW` | no-op (or open the URL on the host) |
| `playCinematicAndroid` | `(Ljava/lang/String;)V` | launches `MyVideoView` with `res/raw/intro_hd.mp4` or `moviecomplete_hd.mp4` | no-op (see §8) |
| `getHttpResponse` | `(Ljava/lang/String;)V` | GET on a Java thread | no-op |
| `HideScreen` / `ShowScreen` | `(I)V` | hides/shows the Android logo/loading layouts (0/1/2) | no-op (`HideScreen` is called from `SetupDevice` and menus) |
| `PrintDebug`, `ShowPlusOne` | `(Ljava/lang/String;)V` | empty | no-op |
| `PopupTrophyGL`, `notifyTrophy`, `launchIGP`, `downloadWelcomeScr`, `showAds`, `loadFreeCash` | `(I)V` | GLLive/ads | no-op |
| `launchGLLive` | `(IZ)V` | GLLive | no-op |
| `enableLaunchPN` | `(Z)V` | push | no-op |
| `logoutGLLive`, `displayWelcomeBackUser`, `launchWelcomeScr`, `setIsPayingUser`, `hideAds`, `showInterstitial`, `showFreeCash` | `()V` | various | no-op |

### 5.2 The game's platform classes

| Class (prefix `…/GloftZOHM`) | Where it is resolved | Static methods (signature) → stub |
|---|---|---|
| `GL2JNILib` | `GL2JNILib.init` (jclass arg) | `createView()V` → **triggers 4a** (create the view/window and call `GameRenderer.nativeInit`); `setViewSettings(IIIII)V` → store (pixelSize, depth, stencil, csaa, flag) to choose the EGL config; `setCurrentContext(I)Z` → `eglMakeCurrent` of context *i* (−1 = release); return `true` for 0/−1 (with 0 extra contexts no other one is requested); `setupPaths()V` empty; `enableAccelerometer(ZF)V` empty; `getResource(Ljava/lang/String;)[B` → `null` (no callers) |
| `/GLUtils/SUtils` | `AndroidOS::Init` (FindClass) and `SUtils.nativeInit` (jclass) | `getPreferenceString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` → `""` (key `SDFolder` of `GamePrefs`: `""` ⇒ the native code uses `/sdcard/android/data/<pkg>/files`); `getPackage()Ljava/lang/String;` → `"com.gameloft.android.ANMP.GloftZOHM"`; `getSaveFolder()`/`getSDFolder()` → as in `Game`; `getContext()Landroid/content/Context;` → an opaque non-null object; `playVideo(Ljava/lang/String;Z)Z` → `false`; `stopVideo()V`, `inGameVideoSetSkipEnabled(Z)V`, `shareInfo(…3×String)V`, `showCantGoBackPopup(I)V`, `deleteFile(Ljava/lang/String;)V` → no-op (deleteFile: delete the file); `getAssetAsString(Ljava/lang/String;)[B` → the asset's contents or `null`; `getGameName()`, `getInjectedIGP()`, `getInjectedSerialKey()`, `getMetaDataValue(String)` → `""`; `retrieveBarrels()[I` → `int[0]`; `getGLUID(Ljava/lang/String;)[I` → 4 fixed integers; `initCheckConnectionType()I` → 0; `nativeSetPreference(Landroid/os/Bundle;)V` / `nativeGetPreference(Landroid/os/Bundle;)Landroid/os/Bundle;` → a persistent key/value store (**the game's preferences**; bundle fields `npDataType, npKey, npPrefName, npData, npDefaultValue, npResult`); `genericUnzipArchive(String,String)Z` → `false`; `removeDirectoryRecursively(String)Z` → `true` |
| `/GLUtils/Device` | `AndroidOS::Init`, `Device.nativeInit` | `getUserAgent()` → a fixed UA; `d1`, `getAndroidId`, `getSerial`, `getSerialNo`, `getDeviceFirmware`, `getMacAddress`, `getDeviceIMEI`, `getHDIDFV`, `getHDIDFVVersion`, `getGLDID`, `getDeviceName`, `getPhoneManufacturer`, `getPhoneModel`, `retrieveDeviceCarrier`, `retrieveDeviceCountry`, `retrieveDeviceRegion`, `retrieveDeviceLanguage`, `retrieveCPUSerial`, `getPhoneDevice`, `getPhoneProduct` — all `()Ljava/lang/String;` → fixed non-null strings (`"en"`, `"US"`, `"00:00:00:00:00:00"`…) |
| `/GLUtils/ResLoader` | `AResLoader_Init` (FindClass) | `getLength(Ljava/lang/String;)I` → the size of the APK asset or −1; `getBytes(Ljava/lang/String;)[B`; `getAssetList(Ljava/lang/String;)[Ljava/lang/String;` → an `assets/` reader (there are only `colors.conf` and `families.list`, which are also in the OBB). Return −1/`null` [I] |
| `/installer/GameInstaller` | `AndroidOS::Init` (FindClass), `initNative` | static fields `mPreferencesName` (`Ljava/lang/String;` = `"GamePrefs"`), `m_sInstance`, `mDeviceInfo` (`Landroid/telephony/TelephonyManager;`); methods `startGame()V`, `getSDFolder()Ljava/lang/String;`. Only `mPreferencesName` is read while playing (`AndroidOS::GetSDFolder`) |
| `/SendInfo` | `SendInfo.initMethods` | `getSDFolder()`, `getGLDID()`, `getPhoneCarrier()`, `getLocaleCountry()`, `getLocaleLanguage()`, `getManufacturerModel()` → String; `getGLUID()[I`. Tracking; optional |
| `/DataSharing` | `DataSharing.nativeInit` | `setSharedValue(String,String)V`, `getSharedValue(String)String` → `""`, `deleteSharedValue(String)V` |
| `/PushNotification/C2DMAndroidUtils` | `C2DMAndroidUtils.nativeInit` | `HasPushNotification()I` → **0 (queried on every `Application::Update`)**; `ResetNotificationStatus()V`; `SetEnable(Z)V`; `IsEnable()Z` → false; `CancelAll()V`; `GetUserID()String`; `GetStatus(Z)I` → 0; `SendPush(Landroid/os/Bundle;String;String;Z)String`; `SendPushToMyself`, `SetOfflineDeviceCredential()I`, `SetUserCredential(String,String)I`, `SetCustomUserCredential(String,String,String)I`, `DeletePush(String,Z)I`, `GetJanusToken(String,String,String)String`, `GetBundleData()Landroid/os/Bundle;` → null, `SetDontDisturbEnable(Z)V`, `IsDontDisturbEnable()Z`, `GetRegistrationID()String` |
| `/SplashScreenActivity` | FindClass on each use | `downloadWS(String)V`, `isWSReady(String)I` → 0, `isActive()I` → 0, `showLocalWS(String)V`, `deleteWS(String)V`, `SetWSLanguage(I)V` |
| `/InGameBrowser` | FindClass | `showForum()V` |
| `/AdServer` | FindClass | `staticShowInterstitialWithTags(String)V` |
| `/iab/InAppBilling` | `InAppBilling.nativeInit` | obfuscated names (decoded with `InAppBilling::readChar`); **[NC]**. Also uses a Bundle-like class. Do not call `InAppBilling.nativeInit*` and tolerate null IDs |
| `com/gameloft/glot/PortingJNI` | `glot::IsEnvAndClassSet` (FindClass) | `getSystemUpTimeMillis()J` → monotonic ms; `hasConnectivity()Z` → **false**; `getIdentifier()`, `getMac()`, `getDeviceName()`, `getDeviceFirmware()`, `getDeviceLanguage()`, `getDeviceCountry()`, `getDeviceCarrier()` → fixed Strings |

### 5.3 Social (all optional; IDs cached in the respective `nativeInit`s)

- `com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib`: `Init(ZLjava/lang/String;)V`, `Login`, `Logout`, `GetMyInfo`, `GetFriends…`, `PostToWall…`,
  `postOpenGraphAction`, `GetPicture`, `getUserAchievements`… (`V`), `IsLoggedIn()Z` → **false**, `hasPermission(String)Z` → false, `getAccessToken()String` → `""`.
- `com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib`: `InitGameAPI`, `ConnectToService`, `ShowAchievements`, `UnlockAchievement`, `SubmitScore`…
  (`V`), `IsLoggedIn()Z` → false, `GetAccessToken()String` → `""`.
- `com/gameloft/GLSocialLib/PlatformAndroid`: `GetDataFromURL(Ljava/lang/String;)[B` → `null`.
- `…/renren/RenrenAndroidGLSocialLib`: the class is missing from this APK (the `Java_*` exist); ignore it.

### 5.4 Android framework classes the native code uses directly

| Class | Use | Stub |
|---|---|---|
| **`android/media/AudioTrack`** | **the default audio output** (§8): `<init>(IIIIII)V`, static `getMinBufferSize(III)I`, static `getNativeOutputSampleRate(I)I`, `play()V`, `pause()V`, `stop()V`, `release()V`, `write([BII)I`, `getPlayState()I` | a real implementation on SDL (T6/T7) |
| `android/os/Process` | `glf::Thread::Impl::SetPriority` → static `setThreadPriority(I)V` | no-op |
| `android/os/Bundle` | `ABundle::init`: `<init>()V`, `putString`, `getString`, `getInt`, `putInt`, `getLong`, `putLong`, `getBoolean`, `putBoolean`, `containsKey(String)Z`, `clear()V`, `getByteArray`, `putByteArray` | a real key→value map (used by the `nativeSet/GetPreference` preferences, push and IAP) |
| `android/view/ViewRoot` | `Game.processTouchpadAsPointer` → `setProcessPositionEvents(Z)V` | do not call that native |
| `android/telephony/TelephonyManager`, `android/os/Build`, `android/os/Build$VERSION`, `android/os/SystemProperties`, `android/provider/Settings$Secure`, `android/content/Context`, `java/util/UUID` | only in `GameInstaller.nativeGetDevID` (DRM) | do not call |

---

## 6. Files

### 6.1 Paths

All absolute paths come from three values provided by Java (copied in `Game.nativeInit`), plus literals:

| Source | Android value | Use |
|---|---|---|
| `getSDFolder()` → `GetSDFolderPathPointer()` | `/sdcard/android/data/com.gameloft.android.ANMP.GloftZOHM/files` | `m_gAppPath = sd + "/"` (working data directory: downloads, `d_o_w_n_l_o_a_d_e_d.txt`, extracted files, trophies); `CopyPath()` |
| `getSaveFolder()` → `GetSaveFolderPathPointer()` | `/data/data/com.gameloft.android.ANMP.GloftZOHM` | saves; `CopySPath()` |
| `GetObbPath()` + `Get{Main,Patch,Extra}ObbName()` | `/sdcard/Android/obb/<pkg>/` + name | OBBs mounted as archives |
| literals in `.rodata` | `/data/data/<pkg>/player.dat`, `/data/data/<pkg>/playerbckp.dat`, `/data/data/<pkg>/heatup.dat`, `/data/data/<pkg>/Playtest.txt|csv` | **the main save and its copy** (*hardcoded* paths that do not go through `getSaveFolder`) |
| literals | `/proc/meminfo` (reads `MemTotal:` for the memory profile), `/sdcard/gameloft/games/ANDROID.txt`, `/system/app/Superuser.apk`, `/system/bin/su`, `/system/xbin/su` | probes; may not exist |

Other names relative to the save/data directory seen in `.rodata`: `/level.dat`, `/zomb.dat`, `/dlc.dat`, `/cloudlevel.dat`, `/cloudplayer.dat`,
`/cloudzomb.dat`, `/BackupCloudData.dat`, `/Nope.dat`, `/Nogplus.dat`, `serverConfig.sav`, `c2dm.sav`, `tracker%d.dat`, `tracking_markers.dat`,
`error_events.dat`, `ses_info.dat`, `user_data.dat`, `r_ev.dat`, `w_ev.dat`, `aux_file_data.dat`, suffixes `.sav`, `.data`, `.tmp`, `.bin`, `promo.xml`.

**Recommendation (T4)**: a path translation layer in `fopen/stat/opendir/mkdir/remove/rename/unlink/open`:
`/data/data/com.gameloft.android.ANMP.GloftZOHM/**` → `<userdir>/save/**`; `/sdcard/android/data/<pkg>/files/**` (compare case-insensitively:
both `android` and `Android` appear) → `<userdir>/files/**`; `/sdcard/Android/obb/<pkg>/**` → the user's OBB folder; `/proc/meminfo` → a synthetic file
(`MemTotal:  2097152 kB`); everything else → `ENOENT`. Create the directories beforehand (the native code only `mkdir`s subfolders).

Engine file access: `glf::FileStreamImpl` → `fopen(path, "rb" | "wb" | "r+b" | "a+b")`, `fseek/ftell` for the size, `fread/fwrite`, **`pread` with
`FILE->_file`** (§3) for asynchronous reads (`FileStreamImpl::ReadASync` creates a `glf::Thread`), `fflush + fsync + fclose`. Directories via
`opendir/readdir/closedir` (`glf::fs::DirHandle::FindFirst`), `mkdir` (`glf::Fs::MkDir`), `unlink/remove/rename`, `stat` (17 sites), `chdir/getcwd` (virtual cwd).

### 6.2 OBB and `.bar`

- **The `.obb` files are standard ZIPs** and **the native code opens them directly** (it does not expect extracted data): `Application::SetupDevice` calls, on
  Glitch's `IFileSystem` (`device+0x2c`, virtual method `+0x1c`, args `(path, 1, 1, 1)`), in this order: **patch** (if the name is not empty) →
  **extra** (if not empty) → **main**. With Irrlicht semantics (searched in the order added) the patch takes priority over the main [I].
  Before that: `glf::fs::SetAlternateDataDir(0, "./")` and `SetAlternateDataDir(1, <global path>)`.
- The Java installer (`GameInstaller`) also **extracts** the OBB entry by entry into `sd_folder` (and validates sizes/CRCs). The port does not need that copy for
  the engine [I]; it only has to give the native code the two `.obb` files as they are. If some access to a loose file in `sd_folder` shows up (e.g. Vox with
  `nativeGetObbPath` in `VoxSoundManager::VoxSoundManager`, which opens the sounds from the OBB), T9 can extract the OBB as Java does (main, then the patch
  overwriting) — **[NC]** which specific files are requested loose.
- `main.1501….obb` (289,401,986 bytes): 562 entries, **all STORED** (method 0, unencrypted, no *data descriptor*): 436 `.wav`, 64 `.mpc` (Musepack SV8,
  magic `MPCK`), 8 `.vxn` (native Vox), 22 `.bdae`, **15 `.bar`**, `zombies.swf` (1.76 MB), `zombies_load.swf`, and configuration (`gamesettings.xml`,
  `debugsettings.xml`, `techniques.xml`, `tracker.xml`, `eventParameters.xml`, `eventsParameters.json`, `snsconfig.json`, `ZOMBIES_SDD_VOX.xml`,
  `Zombiewood_SDD_Vox_1.1.xml`, `soundsInfo.dat`, `PackedWaves*_sounds.txt`, `colors.conf`, `families.list`, `GameVersion.txt` = `1.5.0`).
- `patch.15325….obb` (1,479,926 bytes): 2 **DEFLATED** entries: `effects.bar` and `zombies.swf` (they replace the main's).
- **`.bar` = a nested ZIP** (same `PK\x03\x04` signature; no header of its own). Inside the main they are STORED, so they can be read by offset without extracting.
  Exception: `oconf.bar` (168 bytes) is **plain text** (`XPlayerURL: …`, `GGI: 27991`, `GameVer: 1.5.0`…).

  | `.bar` | Entries | Contents |
  |---|---|---|
  | `actors.bar` | 520 | `.tga` (341), `.bdae` (154), `.png` (25) |
  | `weapons.bar` | 531 | `.bdae`, `.png`, `.tga` |
  | `swfs.bar` | 546 | `.png` (Flash UI textures; 489 STORED) |
  | `location_{BuildingTop,HauntedMansion,Hollywood,SantaMonica,Studio,mp}.bar` | 222/95/343/293/8/13 | `.tga`, `.bdae`, `.irr`, `.png` |
  | `levels.bar` | 143 | `.lvc` (142), `.bin` |
  | `effects.bar` | 127 | **92 `.glsl`**, 31 `.bdae` (materials/effects, binary COLLADA), `glsl.config`, `GLSL/file.map`, `shaderMap.json` |
  | `fonts.bar` | 8 | `.ttf` |
  | `particlesystems.bar` | 22 | `.png`, `.tga`, `.bin` |
  | `zombies_strings.bar` | 23 | per-language text tables (`zombies_text_{en,fr,de,es,it,br,ja,ko,cn,ru,tr,vo}`, `android_*`) |

- **A third level**: many "files" inside a `.bar` are ZIPs themselves: the `.bdae` files contain `little_endian_not_quantized.bdae` (magic `BRES`) +
  `version.nfo`; textures with alpha (logical `*.tga`, `*.png`) contain `SPLIT` (4 bytes) + `rgb.ktx` + `alpha.ktx`. The engine resolves all of this on its own
  (Glitch's ZIP reader + internal zlib); **the HLE only has to serve correct `fopen/fread/fseek/pread` on the `.obb`**.
- How the engine opens a `.bar` inside the OBB (by offset on the same `FILE*`, or loading it into memory): **[NC]**. `.bar` names referenced in
  `.rodata`: the 15 above + `winter_assets.bar` (it does not exist in these OBBs; optional content).
- Actual image formats after decompressing (every `.bar` in the main): **1,918 KTX** (all `glInternalFormat = 0x8D64 GL_ETC1_RGB8_OES`, with mipmaps),
  530 real PNGs, 23 uncompressed TGAs (type 2, 24/32 bpp) and **19 PVR v2** in `swfs.bar` with a `.png` extension (52-byte header, magic `PVR!`,
  format `0x19` = **PVRTC 4bpp**, e.g. `posters_03.png` 1024×1024). See §7.

---

## 7. Graphics

- **GLES 2.0** exclusively: Java creates the context with `EGL_CONTEXT_CLIENT_VERSION = 2` and `CGlfDevice::createDriver` → `createOpenGLES2Driver`
  (`CCommonGLDriver<CProgrammableGLDriver<…CGLSLShaderHandler…>>`). No fixed-pipeline function is imported. Java manages EGL: the native code does **not**
  call EGL or *swap*; it requests context switches via `GL2JNILib.setCurrentContext(I)Z`.
- **Contexts**: `getNumExtraContext()` returns 0 → a single context, on the GL thread. `glf::App::ReserveContext()` (called by
  `glitch::thread::this_thread::reserveContext` from task threads) fails cleanly when there are no extra contexts, and `SetAsMainThread()` calls
  `setCurrentContext(0 | -1)`. → T5 can assume **one context and one GL thread** [I] (multithreaded texture loading would require changing
  `CreationSettings+0x3c` and providing shared contexts; not recommended).
- **Requested default framebuffer** (`setViewSettings`): 16 bpp color (RGB565), depth 16 (Java tries 24), stencil 8 only if the GPU is in the list in
  §4, no MSAA/CSAA. On PC, requesting RGBA8 + D24S8 breaks nothing.
- **`glGetString`**: 6 sites. `genericDriverInit` queries `GL_VERSION` (expects the format `"OpenGL ES <maj>.<min> …"`: looks for `"OpenGL "` and parses `%u.%u`; warns
  if < 1.1), `GL_RENDERER` (compared with `"Mali-400 MP"` for a *vertex attrib shadow state* workaround), `GL_VENDOR`, `GL_EXTENSIONS`
  (`CCommonGLDriverBase::initExtensions`); `CGLSLShaderHandler::doVersionCheck` queries `GL_SHADING_LANGUAGE_VERSION` (`"OpenGL ES GLSL ES 1.00"`).
  **Recommendation**: T5 synthesizes the strings in guest memory: `GL_VERSION = "OpenGL ES 2.0"`, `GL_SHADING_LANGUAGE_VERSION = "OpenGL ES GLSL ES 1.00"`,
  `GL_VENDOR`/`GL_RENDERER` = an Adreno's (consistent with `nativeSetGPUInfo`), and a **filtered** extension list (an allowlist).
- **Extensions**: `initExtensions` compares against a generic Glitch table of ~300 names (`0xb9b700`–`0xb9e400`, including desktop extensions).
  The ones with practical effect given the imports present: `GL_OES_compressed_ETC1_RGB8_texture`, `GL_IMG_texture_compression_pvrtc`,
  `GL_AMD_compressed_ATC_texture`, `GL_EXT_texture_compression_dxt1`/`s3tc`, `GL_OES_mapbuffer` (`glMapBufferOES/glUnmapBufferOES`), `GL_OES_texture_3D`
  (`gl*3DOES`), `GL_OES_depth24`, `GL_OES_packed_depth_stencil`, `GL_OES_depth_texture`, `GL_OES_rgb8_rgba8`, `GL_OES_element_index_uint`,
  `GL_OES_texture_npot`, `GL_OES_standard_derivatives`, `GL_OES_vertex_half_float`, `GL_OES_texture_float/half_float`, `GL_EXT_texture_format_BGRA8888`,
  `GL_EXT_discard_framebuffer`, `GL_OES_vertex_array_object`, `GL_OES_get_program_binary`. Which branch the driver takes for each one: **[NC]** (not fully
  decompiled). Suggested allowlist: `GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8
  GL_OES_element_index_uint GL_OES_texture_npot GL_OES_standard_derivatives GL_OES_depth_texture` and, only if T5 implements the *shadow buffer*,
  `GL_OES_mapbuffer`. Do **not** advertise `GL_OES_texture_3D`, `GL_OES_vertex_array_object`, `GL_OES_get_program_binary` or PVRTC/ATC/DXT.
- **Compressed textures**: a single site calls `glCompressedTexImage2D` (`CTexture::updateData`), another calls `…SubImage2D` and the `3DOES` variants.
  Loaders present: `CImageLoaderKTX`, `PVR`, `DDS`, `PNG`, `JPG`, `TGA`, `BMP`. Actual data: **ETC1 in KTX (1,918)** + 19 PVRTC4.
  - **ETC1**: no software ETC1 decoder was found in the `.so` → **`GL_OES_compressed_ETC1_RGB8_texture` must be advertised** and
    `internalformat 0x8D64` accepted. ANGLE/D3D11 normally exposes it (transcoding it to RGBA8); if a backend does not expose it, T5 must decode ETC1 to RGB8 in the
    `glCompressedTexImage2D` thunk (≈150 lines) and call `glTexImage2D`. Alpha lives in a second ETC1 texture (`alpha.ktx`) that the shader blends.
  - **PVRTC**: there is a `PVRTCDecompress(const void*, int, int, int, uchar*)` called from `glitch::video::pixel_format::convert` → the engine decompresses
    in software when the format is unsupported [I]. Do not advertise `GL_IMG_texture_compression_pvrtc`. If `0x8C00–0x8C03` still reaches
    `glCompressedTexImage2D`, return `GL_INVALID_ENUM` (risk: 19 blank UI posters).
  - `GetTextureFormat()` (Java) = 4 (ETC1) for consistency.
  - (Measured later in T2/T5: ANGLE 2.1.7258 does **not** expose the ETC1 extension, but ETC1 data uploads bit-exactly as `GL_COMPRESSED_RGB8_ETC2`, with a software decoder as a fallback.)
- **3D textures** (`glTexImage3DOES`, `glCompressedTexImage3DOES`, sub-variants): only reachable with `GL_OES_texture_3D`; there are no 3D textures in the
  data. A stub that sets `GL_INVALID_OPERATION`.
- **Shaders**: GLSL ES 1.00 as text, in `effects.bar` (`*VS.glsl`/`*FS.glsl`, with per-variable `precision`: `uniform lowp sampler2D …`, `varying mediump vec2 …`),
  referenced by the `.bdae` effects and `shaderMap.json`/`glsl.config`; also `%s/%s.shd` and `shader.xml` in `.rodata` (an alternative path [NC]). The engine
  prepends `#define`s and calls `glShaderSource` (via a veneer) with several strings, `glCompileShader`, `glGetShaderiv/InfoLog`, `glLinkProgram`,
  `glGetActiveAttrib/Uniform`, `glGet{Attrib,Uniform}Location`. It does not use program binaries (no `glProgramBinaryOES` import). ANGLE accepts ESSL 1.00 as-is.
- **Other GL points** (99 imports, all used): full FBO support (`glGen/Bind/Delete{Framebuffers,Renderbuffers}`, `glFramebufferTexture2D`,
  `glFramebufferRenderbuffer`, `glRenderbufferStorage`, `glCheckFramebufferStatus`) → post-effects (`Bloom*`, `UsePostEffects=1` in `gamesettings.xml`);
  `glCopyTexSubImage2D`, `glGenerateMipmap`, `glReadPixels` (screenshots/debugger), `glPolygonOffset`, `glSampleCoverage`, `glBlendColor/Equation`, `glLineWidth`,
  uniforms `1f/1fv/1i/1iv/2fv/2iv/3fv/3iv/4fv/4iv/Matrix4fv` (no `Matrix2fv/3fv`), `glVertexAttrib4f`, `glVertexAttribPointer`, `glDrawArrays/Elements`.
- **Risks with ANGLE / pointer translation (T5)**:
  1. `glVertexAttribPointer`/`glDrawElements` with **guest pointers to client-side arrays** (no VBO): there are 26 `glBindBuffer` sites and 3 `glBufferData` sites,
     so both modes are used [I]. Translate guest→host and make sure the data is still alive at *draw* time (with guest memory mapped 1:1 on the host,
     adding the base is enough).
  2. `glMapBufferOES` returns a pointer: ANGLE would return a host pointer outside the guest space → a *shadow buffer* in guest memory and `glBufferSubData`
     in `glUnmapBufferOES`, or do not advertise the extension.
  3. `glGetString`, `glGet*InfoLog`, `glGetActive*`, `glGetShaderSource`, `glGenTextures/Buffers/…`, `glGetIntegerv/Floatv`, `glReadPixels`: guest output pointers.
  4. `glShaderSource`: a guest `const char **` (an array of 32-bit pointers) + an optional `const GLint *length`.
  5. Precision: shaders written for mobile (`lowp/mediump`); on D3D11 everything is `highp`, no problem. `GL_OES_standard_derivatives` must be present if any shader uses `dFdx` [NC].
  6. RGB565/RGBA4444/RGBA5551 in `glTexImage2D` (Glitch's usual formats): ANGLE supports them.
  7. The native code assumes the *viewport*/size is set by `resize(w,h)`; a window resize must repeat `glViewport` + `GL2JNILib.resize`.

---

## 8. Audio and video

The **Vox** engine (`vox::DriverAndroid`, `vox_driver_android.cpp`). It has two backends and picks one in `DriverAndroid::Init`:

```
if (DriverAndroid::s_androidAPILevel > 8)  _InitOSL();   // OpenSL ES
else                                        _InitAT();    // android.media.AudioTrack via JNI
```

`s_androidAPILevel` (`.data`, `0xcd6034`) is **3** out of the box and is only changed by the export `VoxSetAndroidAPILevel(int)`, which **nobody calls** (neither native
nor Java). → **In the real APK audio goes out through `AudioTrack` (JNI), not OpenSL.** The port can choose:

**Option A (the original behavior, recommended): a fake AudioTrack in T7 + SDL in T6.**

- `_InitAT` (the thread that initializes Vox): `vm->GetEnv(&env, 0x10002)`; `FindClass("android/media/AudioTrack")` + `NewGlobalRef`; IDs: `<init>(IIIIII)V`,
  static `getMinBufferSize(III)I`, `play/pause/stop/release ()V`, `write([BII)I`, `getPlayState()I`, static `getNativeOutputSampleRate(I)I`.
  - `rate = AudioTrack.getNativeOutputSampleRate(3 /*STREAM_MUSIC*/)` → **return 44100** (the internal mixer runs at 44,100 Hz; any other rate enables a
    linear 2.14 fixed-point resampler: step = `round(44100·16384 / rate)`).
  - `minBuf = AudioTrack.getMinBufferSize(rate, 0xC /*CHANNEL_OUT_STEREO*/, 2 /*ENCODING_PCM_16BIT*/)` (bytes). `frames = minBuf / 4`;
    frames per callback = `min(frames, (int)(rate · 0.02322))` (≈1024 at 44.1 kHz). **Return ≥ 4096** (e.g. 8192).
  - `pthread_create(UpdateThreadedAT)` + `pthread_mutex_init` + `pthread_cond_init`.
- `UpdateThreadedAT` (**its own guest pthread**): `AttachCurrentThread`; `PushLocalFrame(2)`; `track = NewObject(AudioTrack, <init>, 3, rate, 0xC, 2,
  frames*4, 1 /*MODE_STREAM*/)` → `NewGlobalRef`; `CallNonvirtualVoidMethod(play)`; `arr = NewByteArray(frames*4)`; loop while the driver is alive:
  if not suspended → `DoCallbackAT`; if suspended → `pthread_cond_wait` and `usleep(1)`. On exit: `stop`, `release`, `DeleteGlobalRef`, `PopLocalFrame`, `DetachCurrentThread`.
- `DoCallbackAT`: `p = GetPrimitiveArrayCritical(arr, 0)`; under Vox's mutex, `DriverCallbackInterface::_FillBuffer((short*)p, n)` (mixes **signed 16-bit PCM,
  interleaved stereo, little-endian**); `ReleasePrimitiveArrayCritical(arr, p, 0)`; `CallNonvirtualIntMethod(track, write, arr, 0, n*4)`;
  then it self-regulates with `_GetTime()` + `usleep` (sleeps if it is ahead of the clock). → `write` may be **non-blocking** (queue into an SDL ring buffer
  and return `n`), but blocking once the queue exceeds ~100 ms is more robust.
- `Suspend/Resume` (`Game.nativeVoxSuspend/Resume`) → `pause()`/`play()` and the condition variable.

**Option B: force OpenSL** by calling the export `VoxSetAndroidAPILevel(14)` after `JNI_OnLoad` and before the first `step()`. Then `_InitOSL`:

- `SetDriverSampleRate(44100)`, a callback period of `0.02322 s`, its own **4096-byte** buffer (1024 frames).
- `slCreateEngine(&engineObj, 0, NULL, 0, NULL, NULL)` → `Realize(SL_BOOLEAN_FALSE)` → `GetInterface(SL_IID_ENGINE)` → `CreateOutputMix(&mix, 0, NULL, NULL)`
  → `Realize` → `CreateAudioPlayer(&player, &src, &sink, 1, {SL_IID_BUFFERQUEUE}, {SL_BOOLEAN_TRUE})` with
  `SLDataLocator_BufferQueue{ 0x6 /*SL_DATALOCATOR_BUFFERQUEUE*/, numBuffers = 2 }` and
  `SLDataFormat_PCM{ formatType 2, numChannels 2, samplesPerSec 44 100 000 mHz, bitsPerSample 16, containerSize 16, channelMask 0x3 (FL|FR), endianness 2 (little) }`
  → `Realize` → `GetInterface(SL_IID_PLAY)`, `GetInterface(SL_IID_BUFFERQUEUE)` → `RegisterCallback(CallbackOSL, this)` → `DoCallbackOSL()` (primes the queue)
  → `SetPlayState(SL_PLAYSTATE_PLAYING = 3)`. `Suspend` → `SetPlayState(2)`, `Resume` → `SetPlayState(3)`.
- `CallbackOSL(bq, ctx)` runs **on OpenSL's audio thread** (in the port: a dedicated guest thread, never SDL's callback directly): it takes Vox's
  mutex and calls `DoCallbackOSL`: `GetState(&st)`; while `st.count < 2`: `_FillBuffer(buf, 1024)` + `Enqueue(bq, buf, 4096)`.
  **It reuses the same buffer pointer for two consecutive `Enqueue`s → `Enqueue` must copy the data at that moment.**
- vtable methods used: `SLObjectItf`: `Realize`(+0), `GetInterface`(+0xc), `Destroy`; `SLEngineItf`: `CreateAudioPlayer`(+8), `CreateOutputMix`(+0x1c);
  `SLPlayItf`: `SetPlayState`(+0); `SLBufferQueueItf`: `Enqueue`(+0), `Clear`(+4), `GetState`(+8), `RegisterCallback`(+0xc). There is also
  `RecordCallbackOSL` (microphone; unused).

Audio formats in the data: `.wav` (PCM/IMA-ADPCM/MS-ADPCM), `.mpc` (Musepack SV8, internal decoder), `.vxn` (Vox's native format), Ogg (stb_vorbis
present). Everything is decoded inside the `.so`; the HLE only sees PCM. `VoxThread` creates its own `glf::Thread` threads for streaming.

**Video**: the `.mp4` files (`res/raw/logo_hd.mp4`, `intro_hd.mp4`, `moviecomplete_hd.mp4`, `moviecomplete_ld.mp4`) are played by **Java**:
`ZombiesGame::PlayCinematic(name)` → `nativePlayCinematicAndroid` → `Game.playCinematicAndroid(String)` → the `MyVideoView` activity (it picks `intro` if the
name contains "intro", otherwise `moviecomplete`). While it plays, the `Game` activity loses focus (→ `GL2JNILib.pause()` / `nativeVoxSuspend`) and on return
`resume`. **The native `SUtils.onVideoFinished` is an empty function**: the native code does not wait for any notification. Minimal stub: no-op (skips the video);
full version (T9): pause, play the mp4 on the host, resume. `SUtils.playVideo(String,Z)Z` (in-game ad video) → `false`.

---

## 9. Input

JNI signatures (all `static`, `(JNIEnv*, jclass, …)`):

| Native | Signature | Behavior |
|---|---|---|
| `Game.nativeOnTouch(int action, int x, int y, int pointerId)` | `(IIII)V` | → `appOnTouch`. `action`: **1 = down, 2 = move, 0 = up**. `x,y` in *surface* pixels (top-left origin), already divided by the Java scale factor (`Game.a`, 1.0). *Move* events are **throttled per pointer** with `clock()` (threshold by CPU/GPU profile); down/up always pass. Ends in `glf::AndroidAddTouchEvent` |
| `GL2JNILib.touchEvent(int action, int x, int y, int pointerId)` | `(IIII)V` | straight to `glf::AndroidAddTouchEvent` (no throttling). Java does not use it |
| `glf::AndroidAddTouchEvent(action, x, y, id)` | — | `Gamepad::RaiseTouchEvent(pad0, 0, type, id, (float)x, (float)y, true)` with `type` = `0xd6` (down), `0xd7` (move), `0xd8` (up) |
| `GameGLSurfaceView.nativeOnTouch` | `(IIII)V` | identical to `Game.nativeOnTouch` (old view) |
| `Game.nativeOnKeyDown(int keyCode)` | `(I)V` | → `appOnKeyDown` = **an empty function** |
| `Game.nativeOnKeyUp(int keyCode)` | `(I)V` | → `Application::OnAndroidKeyEvent(false, keyCode)` |
| `Game.nativeAccelerator(float x, float y, float z)` | `(FFF)V` | `appOnAccelerator` |
| `Game.nativeOrientation(float, float, float)` | `(FFF)V` | `appOnOrientation` |
| `GL2JNILib.accelerometerEvent(float, float, float)` | `(FFF)V` | `Gamepad::RaiseAccelerometerEvent` |
| `GL2JNILib.orientationChanged(int)` | `(I)V` | `glf::AndroidOrientationChanged` → event `type 100` (2/3 → `SetOrientation`) |
| `Game.nativeXperiaToggleJoysticks(int)` | `(I)V` | `Application::XperiaToggleJoysticks`: only changes Flash HUD frames (shows/hides the virtual sticks) |
| `Game.processTouchpadAsPointer(ViewParent, boolean)` | `(Landroid/view/ViewParent;Z)Z` | calls `android.view.ViewRoot.setProcessPositionEvents(Z)V` via JNI (a hidden Xperia Play API). **Do not call** |
| `Game.nativeCanInterrupt()` | `()I` | reads a global flag |
| `Game.nativeCanAutoOrientate()` | `()Z` | flag |

- **Multitouch**: `pointerId` 0..n from the `MotionEvent`. On normal devices Java sends: `ACTION_DOWN` → `(1,x,y,0)`; `POINTER_DOWN` → `(1,x,y,idx)`;
  `MOVE` → `(2,x,y,id)` for each pointer; `POINTER_UP` → `(0,x,y,idx)`; `ACTION_UP` → `(0,x,y,0)` **and** `(0,x,y,1)`. The game is a *twin-stick shooter*:
  the left stick (move) and right stick (aim/shoot) are touch zones of the Flash HUD.
- **Keys** (`Application::OnAndroidKeyEvent`, *key up* only): if the game is in a "locked" state (flag `+4`) it only handles **`KEYCODE_BACK (4)`** →
  `nativeExit()` or `nativeSendAppBackground()`; in the normal state → `CKeyManager::OnEvent(keyCode)`, which recognizes **`4` (BACK: back navigation/pause, 38
  comparisons)**, **`82` (MENU)**, and occasionally `0x1c`, `0x67`, `0x6c`, `0x130`, `0x136`, `0x137` (Xperia Play physical buttons / evdev codes)
  [I: the exact meaning of the latter is not confirmed]. Java filters out `VOLUME_UP/DOWN (24/25)` and `CAMERA (27)`.
- **Gamepad**: **there is no native gamepad support**. `glf::Gamepad::RaiseButtonEvent` and `RaiseAnalogicStickEvent` exist but **have no callers**; the
  only "gamepad" is the Xperia Play, whose *touchpad* Java converts into touches with `pointerId` **60** (left stick) and **61** (right stick) at
  remapped coordinates (`x·82/360 + 70 [+480]`, `427 − y·82/360 [+5]`, i.e. on an 854×480 HUD).
  → **T8 must implement keyboard/mouse/gamepad as virtual touches**: mouse = pointer 0; WASD/left stick = a synthetic pointer that goes down at the center
  of the left virtual stick and moves proportionally; right stick/mouse = a second pointer on the right stick; `Esc` = `nativeOnKeyUp(4)`. The exact positions
  of the sticks on the HUD have to be measured at runtime [NC].

---

## 10. Threads

`pthread_create` has 4 call sites:

| Site | Who uses it | Purpose |
|---|---|---|
| `glf::Thread::Start(Runnable&, int prio)` | `glf::FileStreamImpl::ReadASync` (asynchronous reads with `pread`), `glitch::task::CCpuTaskHandler::init(n, …)` (the engine's task pool: resource loading/animation), `vox::VoxThread` (Vox mixing/streaming, named `"VoxThread"`) | attributes: `detachstate = JOINABLE (0)`, **`stacksize = 0x10000` (64 KB)**; routine `glf::Thread::Impl::RunThread` → `Thread::MyRun`. When each thread starts: `ThreadMgr::OnStartThread` (copies TLS nodes with `pthread_setspecific`) and the registered *start handlers*, including `glf::AndroidOnStartThread` (`GetEnv` + `AttachCurrentThread` + `android.os.Process.setThreadPriority`). On exit: `AndroidOnExitThread` (`DetachCurrentThread`) |
| `vox::DriverAndroid::_InitAT` | the AudioTrack audio driver | the `UpdateThreadedAT` thread (§8). Option A only |
| `glwebtools::Thread::Start(Priority)` | `GlWebToolsCore::Initialize`, `JobRunner::Start`, `gaia::Gaia::Initialize`, `gaia::ThreadManager::Update`, `AnubisLib::AnubisManager::Initialize`, `CHeatUpManager` | HTTP/online services (curl). With the network cut they stay idle or are never created |
| `CTransportManagerImpl::StartReceiverThread` | multiplayer | not reached without a network [I] |

"External" threads entering guest code: the **UI** thread (init, input, pause) and the **GL** thread (everything else). In the port they can be the same host thread
as long as the order in §4 is respected; `glf` identifies the main thread with `pthread_self()` in `App::SetAsMainThread` (called from `resize`), so
**`step`, `resize`, `stateChanged` and `getViewSettings` must run on the same guest thread**. On Android, input (`nativeOnTouch`) arrives from the UI thread
while `step` runs on the GL thread: `glf`'s event queues are protected [I], but the safest choice is to inject input between frames from the GL thread.

Synchronization used:

- `pthread_mutex_*` (11 `init`, 47 `lock`, 49 `unlock`, 2 `trylock`) with `pthread_mutexattr_settype` → **recursive** (`glf::Mutex`, `vox::Mutex`,
  `glwebtools::Mutex`); `pthread_cond_*` (`wait`, `timedwait`, `signal`, `broadcast`) in `glf::Condition` and in the audio driver; `pthread_once` (1);
  `pthread_join` (4 sites); `sched_yield`, `usleep`.
- **`glf::SpinLock`** and the reference counters use **`ldrex/strex`** → the JIT needs a global exclusive monitor across guest threads (dynarmic's
  `ExclusiveMonitor`); if T3 serializes guest threads with a *big lock*, it must release it on blocking calls (`pthread_cond_wait`, `usleep`,
  `AudioTrack.write`, I/O) or there will be deadlocks with `SpinLock`.
- `__cxa_guard_acquire/release` (530 sites): lazy initialization of local statics from several threads → an implementation with real mutual exclusion.
- TLS: `pthread_key_create` in `AndroidOS_GetEnv` (with a destructor that calls `DetachCurrentThread`), `glf::ThreadMgr::Init` and `__cxa_get_globals`.
  `glf::Thread::GetSequentialThreadId()` assigns small per-thread indices (fixed-size arrays, 16 handlers/threads) → do not create unnecessary extra guest threads.
- The 64 KB stacks `glf` asks for are tight for a JIT with *fastmem*, but that is what the original uses; the host can reserve more with no side effects.

---

## 11. Network, DRM, IAP and social

Gameloft's servers no longer exist; everything must answer "offline". Cut-off points, by layer:

1. **Java (T7)**: `Game.isWifiEnabled()` → 0 (→ `ZombiesGame::SetInternet(false)` in `appInit`, `appResume` and periodically in `appUpdate`);
   `PortingJNI.hasConnectivity()` → false (GLOT/tracking does not try to send); `SUtils.initCheckConnectionType()` → 0; **do not call**
   `NetworkStateReceiver.setConnectionType` or `Game.nativeConnectionLoss`; `C2DMAndroidUtils.HasPushNotification()` → 0; `IsLoggedIn()` → false in
   Facebook/GameAPI; `GetDataFromURL` → null; `getHttpResponse`/`openBrowser` → no-op.
2. **libc (T4)**: `socket()` → −1 (`errno = EACCES`), `getaddrinfo` → `EAI_NONAME`, `gethostbyname` → NULL, `connect/send/recv` → −1,
   `select/poll` with no descriptors → sleep for the *timeout* and return 0 (there are 18 `select` sites; do **not** return immediately or some thread will spin).
   curl/glwebtools/CSocket/AnubisLib/`glf::debugger` fail cleanly when they cannot create the socket.
3. **DRM / installer**: `GameInstaller.initNative/nativeStart/nativeGetDevID/getPublicKey` and `GDRMPolicy.*` are only invoked by the Java
   `GameInstaller` activity (Google license + data download). The game (`Game` → `GL2JNILib`) does not depend on them: **do not call them**. `nativeStart` only does
   `CallStaticVoidMethod(GameInstaller.startGame)`. [NC: a late license check inside the game has not been searched for exhaustively; there are no
   strings or calls suggesting one outside `installer_*`.]
4. **IAP**: `InAppBilling.*` (5 natives + the obfuscated class `iab/s` with `bq/gk/gn/in/rn`) only responds to Java purchase callbacks. Without Java calls it
   does nothing. The in-game shop will show real-money items without a price; `Game.nativeAddCash(int, String)` is the native that credits currency (useful for
   a cheat menu in T9).
5. **Social/Push/Ads/GLLive/Welcome screen/IGP**: everything goes through `void` Java methods (no-op) or getters (`""`/0/false), see §5.
6. **Tracking (GLOT)** writes local files (`tracker*.dat`, `error_events.dat`…) into the save folder: let it work.

Risk: screens that **wait** for a network response (GLLive login, `GameServerConfig.json`, "Arena" multiplayer, *cloud save*) might wait for a curl
*timeout* instead of failing instantly; with `socket()` failing the error is immediate [I]. Story mode is offline [I].

---

## 12. Risks and recommendations

### General

| # | Risk | Recommendation |
|---|---|---|
| R1 | Imports referenced **by address** (`R_ARM_ABS32`/`GLOB_DAT` of `malloc`, `free`, `fread`, `strcmp`, `__cxa_pure_virtual`…) | T3: each import = a thunk in guest memory (e.g. `svc #n; bx lr` in ARM, or a trap address the JIT recognizes); the PLT/GOT and data relocations point to the same thunk |
| R2 | Interworking: ARM PLT, Thumb-2 code, `bx pc` veneers, callbacks with bit 0 | Respect bit 0 on every host→guest entry (`.init_array`, `Java_*`, start routines, comparators) |
| R3 | Massive `ldrex/strex` use with real threads | A shared exclusive monitor; validate early with Glitch's task pool and Vox |
| R4 | `FILE` with fields read inline (`_file` @14, `_flags` @12) and `pread` on that fd | T4: a real bionic `FILE` in guest memory + its own fd table; thread-safe `pread` |
| R5 | C++ exceptions depend on `__gnu_Unwind_Find_exidx` and on the JIT letting the unwinder read/restore registers (it runs as normal guest code) | Implement the import (§1); no special JIT support needed |
| R6 | Guest `setjmp/longjmp` | Implement them on the emulated CPU context (or as injected guest assembly routines) |
| R7 | 552 static constructors before `JNI_OnLoad` | A complete libc/pthread HLE before `.init_array`; a per-constructor log for debugging |
| R8 | The native code calls Java **inside** `GL2JNILib.init` (`createView`) and `Game.nativeInit` (getters) | The fake JNI must be reentrant (host→guest→host→guest) |
| R9 | `AndroidOS::Init` calls `exit()` if it does not find `SUtils`/`Device`/`GameInstaller` | `FindClass` must always return a non-null `jclass` (a generic "unknown" class) and `GetStaticMethodID` a non-null ID |
| R10 | The graphics/CPU profile is decided by strings (`GL_RENDERER`, model, number of cores, `MemTotal`) | Present as high-end: `nativeSetGPUInfo("Adreno (TM) 330")`, 4 cores, 2 GB. (`GetGPUProfile`: `adreno` < 220 → 0; `powervr sgx` < 541 → 0; `nvidia ap` → 0; `nvidia tegra` → 1; everything else → 2) |

### Per task

- **T3 (loader/CPU)**: reserve `0xd30000`; apply the 4 relocation types; resolve every `JUMP_SLOT` at load time; `e_entry = 0`; run
  `.init_array` (552) and then `JNI_OnLoad`. dynarmic A32 with Thumb-2, VFPv3 (D16 is enough), IT blocks, `tbb/tbh`, exclusives. CP15 is unused (return 0 for
  any `mrc`). Reentrant, per-thread host→guest callbacks. Stack sizes: main ≥ 1 MB; `glf` threads ask for 64 KB (give ≥ 256 KB).
  Export symbol-by-name resolution to the other modules (there are 35,581; useful for `VoxSetAndroidAPILevel`, `Game_nativeAddCash`, debugging).
- **T4 (libc)**: priorities by frequency: `memcpy/memset/memmove/memcmp/strlen/strcmp` (native on host pointers), `sprintf/snprintf/sscanf`
  (our own formatting with guest varargs), `malloc/free` (our own guest heap, 8-aligned; watch `strdup`), stdio with a bionic `FILE` (§3), bionic `stat`/dirent,
  pthread with side tables (4-byte mutex/cond), monotonic `gettimeofday/clock/clock_gettime`, an exact `lrand48`, the `_ctype_`/`_tolower_tab_`/
  `_toupper_tab_` tables, `setlocale` → `"C"`, 32-bit `wchar_t` (on Windows `wchar_t` is 16-bit!: do not delegate to the CRT). Network: everything fails (§11).
  Path translation (§6.1) and a synthetic `/proc/meminfo`.
- **T5 (GLES→ANGLE)**: a single ES 2.0 context; synthesized `glGetString` strings and an extension allowlist (§7); ETC1 is mandatory (native in ANGLE or
  decoded in the thunk); pointer translation in ~25 functions; `glMapBufferOES` with a *shadow* or not advertised; error stubs for the `3DOES` ones.
  The frontend does the *swap* after each `step()`.
- **T6 (audio)**: implement a **fake AudioTrack** (option A) with an SDL queue at 44,100 Hz/S16/stereo; `getMinBufferSize` = 8192; `write` copies and, if the queue
  exceeds ~100 ms, blocks. OpenSL (option B) only as an alternative: 4 symbols + guest vtables, `Enqueue` with an immediate copy, callback on a guest thread.
  The 4 OpenSL imports must exist in any case (they are resolved at load time).
- **T7 (JNI)**: `JNIEnv`/`JavaVM` as vtables in guest memory with thunks; the variadic `Call*Method` family **and** the `V` one; references as 32-bit
  *handles*; UTF-8 `String`s (`NewStringUTF`/`GetStringUTFChars`), `byte[]`/`int[]`/`String[]`, `Bundle` (a map), `AudioTrack`. Tolerate unknown classes/IDs
  (return 0/`""`/no-op and **log** `class.method+signature` the first time: it is the best tool for discovering calls not listed here).
  Stub table: §5. Sequence: §4.
- **T8 (integration/input)**: loop = `step()` + swap at 30–60 fps (the game measures real time with `gettimeofday`; [NC] whether it assumes *vsync* at 60 or caps at 30);
  input as virtual touches (§9); `Esc` → `nativeOnKeyUp(4)`; focus loss → `GL2JNILib.pause()` + `nativeVoxSuspend()`, recovery → `resume()` +
  `nativeVoxResume()`; resize → `glViewport` + `GL2JNILib.resize(w,h)` (and `GameRenderer.nativeInit` must have been called with the initial size).
  Logical resolution: the Flash HUD is designed for 16:9 (854×480 as the reference in the Xperia mapping).
- **T9 (installer)**: the user's input = this *repack* APK (`assets/game.apk` with the original APK, `assets/archiveObbCache.zip.001–005` = a split ZIP
  with the OBBs, `lib/armeabi-v7a/libZombieWood.so`) **or** the original APK + loose OBBs. Output: `libZombieWood.so`, the two untouched `.obb` files, and
  `res/raw/*.mp4` if video is wanted. `libgenerator.so` belongs to the *wrapper* (`com.dataManager`), not the game: ignore it.

### Not confirmed (summary)

1. What `CheckDiskSpace()` checks (if it fails, `MyInit` aborts startup).
2. Whether the engine needs loose files in `sd_folder` in addition to the mounted OBBs, and how it opens the nested `.bar` files (offset vs memory).
3. The exact branches of `initExtensions`/the driver per extension; the complete absence of a software ETC1 decoder; real use of `glMapBufferOES` and of client-side arrays.
4. Names/signatures of the `InAppBilling` Java methods (obfuscated in the native code).
5. The meaning of keycodes `0x1c, 0x67, 0x6c, 0x130, 0x136, 0x137` in `CKeyManager::OnEvent`; the positions of the virtual sticks on the HUD.
6. The exact semantics of `ResLoader.getLength/getBytes` when the asset does not exist, and of the 2nd directory of `SetAlternateDataDir`.
7. Whether any screen in the offline flow blocks waiting for the network; the expected *frame pacing* (30 vs 60 fps).
8. Real NEON use (assumed none) and inline `getc/putc` on `FILE` (assumed none).

---

## Appendix A — The 88 exported `Java_*` natives

Prefix `P = Java_com_gameloft_android_ANMP_GloftZOHM_`, `S = Java_com_gameloft_GLSocialLib_`.

- `P GL2JNILib_`: `init`, `initGL`, `getViewSettings`, `getNumExtraContext`, `setNumExtraContext(I)`, `resize(II)`, `step`, `stateChanged(Z)`,
  `pause`, `resume`, `destroy`, `touchEvent(IIII)`, `accelerometerEvent(FFF)`, `orientationChanged(I)`, `setPaths(String,String,String)` (empty).
- `P Game_`: `nativeInit`, `nativeOnTouch(IIII)`, `nativeOnKeyDown(I)`, `nativeOnKeyUp(I)`, `nativeAccelerator(FFF)`, `nativeOrientation(FFF)`,
  `nativeSetGPUInfo(String)`, `nativeSetCPUInfo(F)`, `nativeSetPhoneModel(String)`, `nativeSetIMEI(String)`, `nativeGetInfo(String×4)`,
  `nativeVoxMute`, `nativeVoxSuspend`, `nativeVoxResume`, `nativeVoxTutorialIntro`, `nativeAddCash(I,String)`, `nativeCanInterrupt()I`,
  `nativeCanAutoOrientate()Z`, `nativeConnectionLoss`, `nativeShortcutToScreens(String)`, `nativeXperiaToggleJoysticks(I)`,
  `processTouchpadAsPointer(ViewParent,Z)Z`.
- `P GameRenderer_`: `nativeInit(I,I,I,String)`, `nativeRender`, `nativeResize(II)`, `nativeDestroy`. `P GameGLSurfaceView_`: `nativeOnTouch(IIII)`,
  `nativePause`, `nativeResume`.
- `P GLUtils_`: `SUtils_nativeInit`, `SUtils_onVideoFinished` (empty), `Device_nativeInit`, `NetworkStateReceiver_setConnectionType`.
- `P`: `DataSharing_nativeInit`, `SendInfo_initMethods`, `PushNotification_C2DMAndroidUtils_nativeInit`, `…_nativeCallBack`.
- `P iab_`: `InAppBilling_nativeInit`, `…_nativeSendData`, `…_nativeSetContext`, `…_nativeSetIABObject`, `s_bq`, `s_gk`, `s_gn`, `s_in`, `s_rn`.
- `P installer_`: `GameInstaller_initNative`, `…_nativeStart`, `…_nativeGetDevID`, `…_getPublicKey`, `GDRMPolicy_initNativeAP`, `…_nativeAllow`,
  `…_processServer`, `…_getConstString`, `…_setConst`, `…_setLongConst`, `…_setTime`.
- `S`: `PlatformAndroid_nativeInit`; `facebook_FacebookAndroidGLSocialLib_{nativeInit, nativeOnFBDataLoad, nativeOnFBDialogDidComplete,
  nativeOnFBDialogDidNotComplete, nativeOnFBFailWithError}`; `GameAPI_GameAPIAndroidGLSocialLib_{nativeInit, nativeGameAPIComplete,
  nativeGameAPICompleteWithData, nativeGameAPIDidNotComplete, nativeGameAPIDidPlusOneButton}`; `renren_RenrenAndroidGLSocialLib_{nativeInit,
  nativeOnRRDataLoad, nativeOnRRDialogDidComplete, nativeOnRRDialogDidNotComplete, nativeOnRRFailWithError}`.

Other C exports useful to the host: `VoxSetAndroidAPILevel(int)`, `VoxSetJavaVM`, `appPause/appResume/appUpdate/appOnTouch`, `nativeExit`, `mJavaVM`.

## Appendix B — Tools (`tools/analysis/`)

| Script | What it does |
|---|---|
| `elf_abi.py <so> [json]` | headers, segments, `.ARM.attributes`, `DT_*`, relocation counts by type, `.init_array` |
| `import_xrefs.py <so> <json>` | per import: PLT stub, GOT slot, call count (ARM and Thumb-2 BL/BLX + veneers), data relocations, calling functions |
| `gen_imports.py <xrefs.json> <imports.json>` | generates `docs/imports.json` (signatures and notes maintained by hand in the script) |
| `insn_stats.py <so> [json]` | ARM vs Thumb, instruction classes (VFP/NEON/exclusives/CP15…), FP mnemonics |
| `rodata_strings.py <so> [--grep RE]` | a section's strings with their addresses |
| `disasm_fn.py <so> <symbol|0xaddr>[,…] [--jni] [--calls-only]` | annotated disassembly: calls by name, strings/GOT for PIC idioms, `JNIEnv` slots |
| `scan_all.py <so> <out.txt>` | a reference database for all of `.text` (calls + strings + JNI slots per function), for `grep` |
| `jni_table.py <scan_all.txt>` | functions that resolve Java classes/methods and the sequence of names/signatures |
| `obb_inspect.py <obb> [--list] [--nested-list N]` | the structure of an OBB and its nested `.bar` files |
| `ghidra/DecompileList.java` | a headless script: decompiles a list of functions without full auto-analysis |

Large outputs (Ghidra projects, `scan_all.txt`, JADX) live in `C:\Users\chdut\ZombiewoodDev\analysis\`, outside the repo.
