# Zombiewood — análisis técnico (T1)

Especificación para T3–T8: qué hay que emular de Android debajo de `libZombieWood.so`.

- Binario: `lib/armeabi-v7a/libZombieWood.so`, 13 500 416 bytes, MD5 `73a6dc0f00e1690dc3122019054f89e1`, `DT_SONAME = libAndroid.so.full`.
- APK: `com.gameloft.android.ANMP.GloftZOHM` 1.5.3 (versionCode 15325, minSdk 14, targetSdk 19).
- Motor: **Glitch** (fork de Irrlicht de Gameloft) + **glf** (capa de plataforma) + **Vox** (audio) + **gameswf** (UI Flash) + Lua,
  FreeType, libpng, libjpeg, zlib, curl, OpenSSL, TinyXML, JsonCpp, todo enlazado estáticamente (incluido libstdc++/gnustl y el unwinder).
- Las direcciones de este documento son **direcciones virtuales del ELF** (base 0). En Ghidra la base por defecto es `0x10000` (sumar).
- Convención: **[C]** = confirmado en el binario/dex; **[I]** = inferido (coherente con la evidencia, sin verificar el camino completo);
  **[NC]** = no confirmado. Todo lo que no lleva marca es [C].
- Cómo se obtuvo: `tools/analysis/*.py` (pyelftools + capstone) sobre el `.so`, Ghidra 12.1.4 headless (`tools/analysis/ghidra/DecompileList.java`)
  y JADX 1.5 (API de `jadx-headless-mcp`) sobre el APK. Nada del juego se ha copiado al repo.

---

## 1. ABI

| Propiedad | Valor | Evidencia |
|---|---|---|
| Clase/endianness | ELF32 LE, `ET_DYN`, `EM_ARM`, `e_flags = 0x05000000` (EABI v5, **sin** `EF_ARM_ABI_FLOAT_HARD/SOFT`) | cabecera |
| CPU | ARMv7-A (`Tag_CPU_arch = 10`, `Tag_CPU_arch_profile = 'A'`, `Tag_CPU_name = "ARM v7"`) | `.ARM.attributes` |
| Conjuntos | `Tag_ARM_ISA_use = 1`, `Tag_THUMB_ISA_use = 2` → **Thumb-2** | `.ARM.attributes` |
| FP | `Tag_FP_arch = 4` → **VFPv3-D16**; no hay `Tag_Advanced_SIMD_arch` → **sin NEON** | `.ARM.attributes` |
| Paso de FP | **softfp**: `Tag_ABI_VFP_args` **ausente** (= AAPCS base). `Tag_ABI_HardFP_use = 3` (SP+DP) | `.ARM.attributes` |
| `wchar_t` | 4 bytes (`Tag_ABI_PCS_wchar_t = 4`); enums de 32 bits (`Tag_ABI_enum_size = 2`) | `.ARM.attributes` |
| Alineación | `Tag_ABI_align_needed/preserved = 1` → pila alineada a 8 en llamadas públicas | `.ARM.attributes` |
| División hardware | `Tag_DIV_use = 1` (no usada); las divisiones van por `__aeabi_*div*` internos | `.ARM.attributes` |

Confirmación de softfp en código: `VoxSetDataThreshold(double)` empieza con `vmov d6, r0, r1`; `glf::AndroidAddTouchEvent` convierte enteros
y pasa los `float` en `r3`/pila; `Java_…_GameRenderer_nativeInit` lee el 4.º argumento de `r3` y el 5.º de la pila.
**Regla para los thunks de imports (T3/T4/T5):** `float` en un registro núcleo/slot de pila de 4 bytes; `double`/`long long` en par
`r0:r1` o `r2:r3` (par alineado: si toca `r1` se salta a `r2:r3`) o en pila alineada a 8; retorno `float` en `r0`, `double`/`int64` en `r0:r1`.

Uso real de instrucciones (`insn_stats.py`, 26 999 funciones con símbolo):

- **26 857 funciones Thumb-2 (6.16 MB) y 142 ARM (21 KB)**. Las ARM son el runtime de C++/unwinder (`__cxa_*`, `__dynamic_cast`, `__gnu_Unwind_*`,
  type_info) y los helpers `__aeabi_*`. La **PLT es ARM** (stubs de 12 bytes) → todas las llamadas a imports desde Thumb son `BLX` (interworking obligatorio).
- **Veneers del linker** al final de `.text` (≈`0xb0be60`–`0xb0c078`): `bx pc; nop` (Thumb) + `ldr ip,[pc]; add pc,ip,pc; .word off` (ARM). Los usan los
  *tail calls* Thumb→PLT (p. ej. `glShaderSource`, `memcpy`, `free`…). El JIT debe soportar `bx pc` en Thumb y `add pc, ip, pc` en ARM.
- VFP: ~80 000 instrucciones (`vldr/vstr/vmov/vmul/vmla/vmls/vnmls/vnmul/vadd/vsub/vdiv/vsqrt/vabs/vneg/vcvt/vcmp/vcmpe/vmrs/vpush/vpop/vldm/vstm`,
  muchas condicionales dentro de bloques IT). Solo `d0–d15`.
- NEON: **no se usa** [I]. El barrido cuenta ~2 700 mnemónicos NEON pero caen en *literal pools* decodificados como código (el build no declara
  Advanced SIMD). Aun así dynarmic los soporta; no hace falta trabajo extra.
- **`ldrex/strex` + barreras (`dmb`)**: ~33 000 instrucciones reales — contadores de referencias atómicos (`boost::intrusive_ptr`, `IReferenceCounted`,
  COW de `std::string`). Con varios hilos guest **hace falta monitor exclusivo compartido** (ver §10).
- ARMv6T2+: `movw/movt`, `ubfx/sbfx/bfi/bfc`, `uxtb/uxth/sxtb/sxth`, `rev/rev16`, `clz`, `mls`, `umull/smull/umlal`, `tbb/tbh`, bloques `IT` (~24 000).
- `svc`, `bkpt`, `udf`, `mrc/mcr`, `sdiv/udiv`: solo aparecen en datos decodificados; **no hay syscalls directas**.

TLS / protección de pila / arranque:

- **No hay `PT_TLS` ni lecturas de `TPIDRURO`** (0 coincidencias de `mrc p15,0,rX,c13,c0,3` en ARM y Thumb). Todo el estado por hilo va por
  `pthread_key_create/getspecific/setspecific` (79 llamadas a `pthread_getspecific`). No hace falta emular el registro TLS.
- `__stack_chk_guard` se importa como dato (`R_ARM_GLOB_DAT`, 1 122 llamadas a `__stack_chk_fail`): el código hace `ldr rX,[GOT]; ldr rX,[rX]`.
  Basta un `uint32_t` en memoria guest con un valor constante.
- `PT_GNU_STACK` RW (pila no ejecutable), `PT_GNU_RELRO` (`0xc84370`, 334 992 bytes; se puede ignorar), `DT_SYMBOLIC`, `DT_FLAGS = 0x0a`
  (`SYMBOLIC|BIND_NOW`), `DT_FLAGS_1 = NOW` → resolver **todos** los `JUMP_SLOT` en la carga (sin lazy binding; `PLT0` no se usa).
- **Orden de arranque del loader** (lo que hace `dlopen` de bionic): mapear → relocar → ejecutar `.init_array` (**552 punteros**, `0xc91828`, todos a
  `.text`, la mayoría Thumb con bit 0) en orden → `JNI_OnLoad(vm, NULL)`. No hay `DT_INIT` ni `.preinit_array`. `.fini_array` tiene 1 entrada (`0xab7160`,
  `__do_global_dtors_aux` → `__cxa_finalize`); no ejecutarla. Los constructores estáticos llaman a imports (`__cxa_atexit`, `malloc`,
  `pthread_mutex_init`, `__cxa_guard_*`…): **toda la HLE de libc debe estar lista antes de `.init_array`**.
- Excepciones C++: el unwinder ARM EHABI está dentro del `.so` y pide la tabla con el import **`__gnu_Unwind_Find_exidx(pc, &count)`**: devolver
  `base + 0xb1c678` y `count = 0x2f6a0 / 8 = 24 276` (sección `.ARM.exidx`, `PT_ARM_EXIDX`). El estado EH por hilo usa `pthread_key` (`__cxa_get_globals`).

Segmentos: `PT_LOAD` R+X `0x0–0xc82d5c` (fichero = memoria), `PT_LOAD` RW vaddr `0xc84370` (offset fichero `0xc83370`, filesz `0x5c7a0`, memsz `0xab4c4`;
`.bss` = `0xce0b10`+`0x4ed24`). Reservar ≥ `0xd30000` contiguos. Exporta 35 581 símbolos (sin strip) — T3 puede resolver por nombre cualquier función interna.

---

## 2. Relocaciones

Solo `SHT_REL` (addend implícito en el destino). `elf_abi.py`:

| Sección | Tipo | Nº | Tratamiento |
|---|---|---|---|
| `.rel.dyn` | `R_ARM_RELATIVE` (23) | 56 376 (= `DT_RELCOUNT`, van primero) | `*p += base` |
| `.rel.dyn` | `R_ARM_ABS32` (2) | 6 467, **todas contra símbolos indefinidos** | `*p += S` (addend in situ, normalmente 0) |
| `.rel.dyn` | `R_ARM_GLOB_DAT` (21) | 18, todas contra indefinidos | `*p = S` |
| `.rel.plt` | `R_ARM_JUMP_SLOT` (22) | 356 | `*p = S` (resolver en carga) |

No hay `TLS_*`, `COPY`, `IRELATIVE`, `REL32` ni relocaciones de texto. Por `DT_SYMBOLIC`, ninguna relocación apunta a símbolos definidos en el propio `.so`.

Detalle de las relocaciones contra imports (importa para cómo se materializa cada import en memoria guest):

- `R_ARM_ABS32`: `__cxa_pure_virtual` ×6 457 (vtables), `malloc` ×3, `free` ×3, `realloc` ×2, `calloc` ×1, `strdup` ×1 (tablas de allocators de
  zlib/curl/OpenSSL). → **cada función importada necesita una dirección guest invocable** (un thunk), no basta con interceptar la PLT.
- `R_ARM_GLOB_DAT`: datos `__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `SL_IID_ENGINE`, `SL_IID_BUFFERQUEUE`, `SL_IID_PLAY`;
  y punteros a función `malloc`, `free`, `calloc`, `realloc`, `strdup`, `strcmp`, `fread`, `fwrite`, `fputc`, `__gnu_Unwind_Find_exidx`.
- 364 imports = 356 funciones con `JUMP_SLOT` + 8 objetos de datos.

---

## 3. Imports

Lista completa y legible por máquina en [`imports.json`](imports.json) (364 entradas: `name, lib, category, kind, signature, used, calls, data_relocs,
stub_ok, note`). `used` sale de `import_xrefs.py` (BL/BLX directos al stub PLT + veneers + relocaciones de datos): **los 364 tienen al menos una
referencia**; `stub_ok = true` marca los que pueden devolver un valor fijo/fallo sin afectar al juego.

Resumen por categoría (nº de símbolos · observaciones para T4):

| Categoría | Nº | Notas |
|---|---|---|
| `data` (libc) | 5 | `__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_` |
| `libc-runtime` | 22 | `__assert2` (5 588 sitios), `__cxa_atexit`, `__errno`, `__gnu_Unwind_Find_exidx`, `abort/exit`, `setjmp/longjmp`, `qsort`, `lrand48/srand48`, uid/pid, señales (solo OpenSSL) |
| `libc-memory` | 9 | `malloc/free/calloc/realloc` + `mem*` (`memcpy` 3 531 sitios) |
| `libc-string` | 39 | `str*`, `ato*`, `strto*`, `is*`, `tolower/toupper`, `setlocale` |
| `libc-wchar` | 22 | `wchar_t` de 32 bits; `wcslen` 190 sitios, `wmemcpy` 278 (glitch `stringw`, gameswf) |
| `libc-stdio` | 37 | `FILE*` reales (ver abajo); `sprintf` 1 051, `sscanf` 58, `snprintf` 158 |
| `libc-fs` | 20 | `stat/fstat`, `opendir/readdir`, `mkdir`, `unlink`, `pread`, `fsync`… |
| `libc-time` | 15 | `gettimeofday`, `clock`, `clock_gettime`, `time`, `localtime`, `usleep`, `sleep` |
| `libc-pthread` | 29 | ver §10 |
| `libc-net` | 23 | todos `stub_ok` (ver §11) |
| `libm` | 34 | variantes `float` y `double`, softfp |
| `libstdc++` | 4 | `__cxa_guard_acquire/release/abort`, `__cxa_pure_virtual`. **No** importa `operator new/delete` (gnustl estático → `malloc`) |
| `liblog` | 2 | `__android_log_print/vprint` |
| `opensles` | 4 | `slCreateEngine` + 3 IID (camino muerto por defecto, §8) |
| `gles2` | 99 | ver §7 |

No se importa nada de `libdl` (aunque es `DT_NEEDED`), ni `libandroid`, ni `libEGL`, ni `libz` (zlib interno), ni `dlopen/dlsym`, `mmap`, `sem_*`,
`pthread_exit/detach/kill`, `nanosleep`, `statfs`, `fileno`, `ferror`, `getc_unlocked`.

### Layouts bionic de 32 bits que el guest toca directamente

**`FILE` (84 bytes) y `__sF`** — el código usa macros inline de `<stdio.h>`, así que el `FILE*` devuelto por `fopen` debe ser una estructura real en memoria guest:

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

- `_file` (**offset 14**): `glf::FileStreamImpl::Impl::ReadWithOffset` hace `pread(*(short*)(fp+14), buf, n, off)` y `Close` hace `fsync(*(short*)(fp+14))`
  (macro `fileno`). Es el camino de **lectura asíncrona/posicionada del sistema de archivos del motor**: `_file` debe contener un fd que `pread` entienda.
- `_flags` (**offset 12**): `ferror`/`feof` inline leen `__SERR = 0x40` / `__SEOF = 0x20` (Lua `liolib`, libpng). Mantener esos bits (al menos a 0 / EOF correcto).
- `_p/_r/_w`: no hay evidencia de `getc/putc` inline (se importan como funciones) [I]; dejarlos a 0 es seguro (`_r = 0`, `_w = 0` fuerzan el camino lento).
- `stdout/stderr` se pasan como `&__sF[1]`, `&__sF[2]` a `fprintf/fwrite/fputs/fflush`.

**`struct stat` (104 bytes, ARM EABI)**: `st_dev` u64 @0, pad @8, `__st_ino` u32 @12, `st_mode` u32 @16, `st_nlink` u32 @20, `st_uid` @24, `st_gid` @28,
`st_rdev` u64 @32, pad @40, `st_size` **s64 @48**, `st_blksize` u32 @56, `st_blocks` u64 @64, `st_atime` @72 (+nsec @76), `st_mtime` @80, `st_ctime` @88,
`st_ino` u64 @96. `S_IFDIR = 0x4000`, `S_IFREG = 0x8000`.

**`struct dirent`**: `d_ino` u64 @0, `d_off` s64 @8, `d_reclen` u16 @16, `d_type` u8 @18 (`DT_DIR = 4`, `DT_REG = 8`), `d_name[256]` @19.

**`_ctype_`**: `const char *_ctype_` apunta a una tabla de 257 bytes; se indexa `(_ctype_ + 1)[c]`. Bits: `_U 0x01, _L 0x02, _N 0x04, _S 0x08, _P 0x10,
_C 0x20, _X 0x40, _B 0x80`. `_tolower_tab_` / `_toupper_tab_`: `const short *` a tablas de 257 `short`, `(tab + 1)[c]`. Ojo: los tres símbolos son
**punteros** (el GOT apunta a la variable puntero, que apunta a la tabla).

**`errno`**: `int *__errno(void)`; un `int` por hilo guest. Valores Linux (`ENOENT 2, EINTR 4, EAGAIN 11, EACCES 13, EEXIST 17, EINVAL 22, ETIMEDOUT 110`…).

**`jmp_buf`**: `long[64]` (256 bytes). `setjmp/longjmp` (13/27 sitios: libpng, libjpeg, Lua, FreeType, GLU tess) deben implementarse **sobre el estado del
CPU guest** (r4–r11, sp, lr, d8–d15, fpscr), no con `setjmp` del host.

**pthread (bionic 4.x)**: `pthread_t = long`; `pthread_mutex_t = { int volatile value; }` (**4 bytes**); `pthread_cond_t = { int volatile value; }` (4 bytes);
`pthread_attr_t` 24 bytes; `pthread_key_t = int`; `pthread_once_t = int`; `pthread_mutexattr_t = long`. Los objetos host no caben inline → tabla lateral
indexada por dirección guest (con creación perezosa para mutex estáticos a 0; inicializadores bionic: normal `0`, recursivo `0x4000`, errorcheck `0x8000`).

**tiempo**: `time_t`/`clock_t`/`off_t` de 32 bits; `timeval`/`timespec` 8 bytes; `struct tm` 44 bytes (9 `int` + `long tm_gmtoff` + `const char *tm_zone`);
`CLOCKS_PER_SEC = 1 000 000`.

**varargs**: `va_list` en ARM EABI es `struct { void *__ap; }` (una palabra, se pasa por valor). `printf`-family: `double`/`long long` alineados a 8 en la
zona de argumentos. `sscanf` necesita implementación completa (`%f %d %x %s %c %[...] %n`, anchuras).

Callbacks guest que la HLE debe poder invocar: comparador de `qsort` (13 sitios), `pthread_create` start routine, destructores de `pthread_key_create`,
`pthread_once`, y —si se guarda— `__cxa_atexit`.

`lrand48/srand48` (47/5 sitios): RNG de jugabilidad; implementar el LCG de 48 bits exacto (`X = X·0x5DEECE66D + 0xB`, `srand48` fija `X = seed<<16 | 0x330E`).

`sysconf`: 1 sitio, `GetNumberOfCores()` → `sysconf(0x61 /*_SC_NPROCESSORS_ONLN*/)`; alimenta el perfil gráfico (`GetProfileGPU`).

---

## 4. Secuencia de arranque (lado Java → nativo)

Extraída de `classes.dex` (`Game`, `GL2JNILib`, `GL2JNIView`, `GLSurfaceView` propio, clases `as`/`at`/`av`, `GameRenderer`). El APK es un *repack*:
el launcher real es `com.dataManager.MainActivity` (extrae `assets/game.apk` y `assets/archiveObbCache.zip.00N`), que acaba lanzando `…GloftZOHM.Game`.

Hilos Java: **UI** (main looper) y **GL** (el `GLThread` del `GLSurfaceView`, único hilo que hace GL y que llama a `step`).

| # | Hilo | Llamada | Argumentos / efecto nativo |
|---|---|---|---|
| 0 | UI | `System.loadLibrary("ZombieWood")` → `.init_array` → **`JNI_OnLoad(vm, 0)`** | `VoxSetJavaVM(vm)`; `mJavaVM = vm`; `vm->GetEnv(&env, 0x10004)` (si ≠ 0 devuelve −1); `AndroidOS_JNIOnload(vm)`: guarda `AndroidOS_JavaVM`, `AndroidOS_GetEnv()`, **`AndroidOS::Init()`** (FindClass de `…/GLUtils/SUtils`, `…/GLUtils/Device`, `…/installer/GameInstaller` y sus métodos — si falla llama `exit`), `InAppBilling_setJavaVM`, `SplashScreen_setJavaVM`, `AdServer_setJavaVM`, `GameUtils_setJavaVM`. Devuelve `0x10004` |
| 1 | UI | `Game.onCreate`: `PortingJNI.Init`, `Tracking.init` (solo Java) | — |
| 2 | UI | `Game.onStart`: **`Game.nativeSetGPUInfo(GraphicChooser.b)`** | cadena `GL_RENDERER` obtenida antes por la activity `GraphicChooser`; se guarda en `GetPhoneGPUNamePointer()` |
| 3 | UI | (activity `GameInstaller`: DRM + extracción de OBB; llama `GameInstaller.initNative`, `GDRMPolicy.*`, `nativeStart`…) | **Solo instalador; no hace falta para jugar** (§11). Termina con `setResult(1)` |
| 4 | UI | `Game.onActivityResult(100, 1)` → **`GL2JNILib.init()`** | `env->GetJavaVM`; `NewGlobalRef(clase GL2JNILib)`; cachea IDs estáticos `getResource`, `setupPaths`, `createView`, `setViewSettings`, `setCurrentContext`, `enableAccelerometer`; `glf::AndroidInit()` → llama a Java **`GL2JNILib.createView()`** |
| 4a | UI | (dentro de `createView`) `new GL2JNIView` → `new at()` → **`GameRenderer.nativeInit(Game.getManufacture(), displayW, displayH, Game.O)`** | `s_windowWidth = displayW`, `s_windowHeight = displayH` (tamaño de pantalla en píxeles). `manufacture`: 0 genérico, 1 Motorola, 2 Samsung, 3 HTC, 4 Sony, 5 Sharp… (usar **0**). `Game.O` = versionName `"1.5.3"` (no se usa en nativo) |
| 5 | UI | **`Game.nativeInit()`** | `NewGlobalRef(clase Game)`; cachea ~60 IDs estáticos de `Game` (tabla §5.1) y **llama inmediatamente** a `GetPhoneManufacturer/Model/Firmware/Device/CPUName/CPUFreq/GPUName`, `getSDFolder`, `getSaveFolder`, `GetIMEI`, `GetWSClose`, copiando las cadenas a globales (`GetSDFolderPathPointer()`, `GetSaveFolderPathPointer()`…) |
| 6 | UI | `InAppBilling.init(F)` → `InAppBilling.nativeInit…` | opcional (IAP) |
| 7 | UI | `Game.onResume` (1.ª vez): `PlatformAndroid.nativeInit()`, `FacebookAndroidGLSocialLib.nativeInit()`, `GameAPIAndroidGLSocialLib.nativeInit()` | cachean clase + IDs sociales (tabla §5.3). Opcionales |
| 8 | **GL** | `ConfigChooser`: **`GL2JNILib.initGL()`** | función vacía |
| 9 | GL | **`GL2JNILib.getViewSettings()`** | `m_gAppPath = sdFolder + "/"`; `glf::AndroidGetViewSettings()`: si no hay app → crea `glf::Globals`, **`NewApp(0, argv)`** (`new Application`, 0x11f88 bytes), `App::MyInit()` → `ThreadMgr::Init`, `Application::Init(CreationSettings&)`; después llama a Java **`GL2JNILib.setViewSettings(pixelSize, zBuffer, stencil, csaa, flag)`** |
| 10 | GL | `eglCreateContext` (cliente **GLES 2**) → **`GL2JNILib.getNumExtraContext()`** | devuelve `CreationSettings+0x3c` = **0** (valor por defecto; nadie lo cambia) → Java no crea contextos pbuffer extra |
| 11 | GL | `onSurfaceCreated` → **`GL2JNILib.stateChanged(true)`** | envía `CoreEvent{type 0x65, 5}` (5 = contexto creado; `false` → 4) |
| 12 | GL | `Game.GetPhoneInfo()` → `nativeSetPhoneModel(Build.MODEL)`, `nativeSetIMEI(id)`, **`nativeSetGPUInfo(glGetString(GL_RENDERER))`**, `nativeSetCPUInfo(maxFreqKHz float)` | además Java calcula `GetTextureFormat()` a partir de `GL_EXTENSIONS` (1 PVRTC, 2 ATC, 3 DXT, 4 ETC1) |
| 13 | GL | `onSurfaceChanged(w,h)` → `glViewport(0,0,w,h)` (Java) + **`GL2JNILib.resize(w, h)`** | `glf::AndroidResizeScreen`: guarda tamaño en `AppImpl`, marca el hilo como principal |
| 14 | GL | cada frame: **`GL2JNILib.step()`** (si no está en pausa) y después `eglSwapBuffers` | `glf::AndroidUpdate()` → `gApp->vtbl[0x24]()` (= `App::Update` → `Application::Update`). En la **primera** `Application::Update` se ejecuta `Application::SetupDevice()` (crea el dispositivo Glitch/driver GLES2, monta los OBB, `Application::Init(device)`, `ExtractInitFiles`, `GLOTManager::InitManager`, `PostInit`). El *swap* lo hace Java, no el nativo |
| 15 | UI | foco/pausa: `GL2JNILib.pause()` / `resume()`, `Game.nativeVoxSuspend()` / `nativeVoxResume()` | `appPause/appResume`; `resume` vuelve a consultar `isWifiEnabled` |

Notas:

- `GL2JNILib.setPaths(String,String,String)` existe pero **Java no lo llama** y el nativo está vacío. Las rutas llegan por `getSDFolder/getSaveFolder/GetObbPath`.
- `Application::Init(CreationSettings&)` (paso 9) fija: ancho/alto = `s_windowWidth/Height` del paso 4a, `[0x2c] = 16` y `[0x2d] = 16`, `[0x28] = 2`,
  `[0x44] = 1`, y `[0x13] = 1` solo si el nombre de GPU en minúsculas contiene una de: `adreno`, `powervr sgx`, `nvidia ap`, `nvidia tegra`,
  `mali-400 mp`… (lista en `0xb4c088`). Con eso `setViewSettings` recibe `(16, 16, 8 o 0, 4, x)`: RGB565, depth ≥16 (Java intenta 24 y cae a 16), stencil 8 si
  la GPU está en la lista. **Antes** llama a `CheckDiskSpace()`; si devuelve 0, `MyInit` falla ("MyInit failed") [NC: no se ha analizado qué consulta; no hay `statfs` importado].
- `GameRenderer.nativeRender/nativeResize/nativeDestroy` y `GameGLSurfaceView.*` son del camino antiguo (no usado por `Game`); `nativeRender` = `appUpdate`.
- Orden mínimo recomendado para el port (T7/T8): 0 → 2 → 4 (+4a) → 5 → [7] → 9 → 11 → 12 → 13 → bucle 14. `nativeSetGPUInfo` **debe** preceder a 9.

---

## 5. Llamadas nativo → Java

Patrón de uso de JNI en el `.so`:

- Obtención de `JNIEnv`: `AndroidOS_GetEnv()` (pthread key propia; si no hay env → `vm->AttachCurrentThread(&env, 0)`), `NVThreadGetCurrentJNIEnv()`
  (`AttachCurrentThread` + `GetEnv(…, 0x10004)`), y bloques `GetEnv(&env, 0x10002/0x10004/0x10006)`; si devuelve `-2 (JNI_EDETACHED)` hacen
  `AttachCurrentThread` … `DetachCurrentThread`. **Stub recomendado: `GetEnv` y `AttachCurrentThread` devuelven siempre 0 y un `JNIEnv*` válido para el
  hilo que llama; `DetachCurrentThread` = 0.** Slots de `JavaVM` usados: `AttachCurrentThread` (+0x10), `DetachCurrentThread` (+0x14), `GetEnv` (+0x18).
- Las clases propias se resuelven con `FindClass("com/gameloft/android/ANMP/GloftZOHM" + "/Sufijo")` (se construye con `sprintf`/`memcpy`, a veces desde
  `"com.gameloft.android.ANMP.GloftZOHM"` reemplazando `.`→`/`) o usando el `jclass` que llega como 2.º argumento del `Java_*_nativeInit` → `NewGlobalRef`.
- **Casi todo es estático**: `GetStaticMethodID` + `CallStatic<T>Method`. El `.so` usa tanto los slots variádicos (`CallStaticIntMethod` +0x204,
  `CallStaticVoidMethod` +0x234, `CallStaticObjectMethod` +0x1c8…) como los **`…V` con `va_list`** (los wrappers inline `_JNIEnv::CallStaticVoidMethod(...)`
  exportados hacen `push {r2,r3}` y llaman al slot `V`, p. ej. +0x238). T7 debe implementar ambas familias leyendo argumentos de `r2/r3`+pila o del
  `va_list` guest (`float` promocionado a `double`, `jlong`/`jdouble` alineados a 8).
- Slots de `JNIEnv` vistos: `FindClass`, `GetObjectClass`, `NewGlobalRef`, `DeleteGlobalRef`, `DeleteLocalRef`, `PushLocalFrame`, `PopLocalFrame`,
  `GetMethodID`, `GetStaticMethodID`, `GetFieldID`*, `GetStaticFieldID`, `GetStaticObjectField`, `GetStaticIntField`*, `NewObject`, `Call<T>Method(V)`,
  `CallNonvirtual<Void|Int|Object|Boolean>Method(V)`, `CallStatic<Void|Int|Long|Float|Boolean|Object>Method(V)`, `NewStringUTF`, `GetStringUTFChars`,
  `ReleaseStringUTFChars`, `GetStringUTFLength`*, `GetArrayLength`, `NewByteArray`, `GetByteArrayElements/Release…`, `GetByteArrayRegion`*,
  `SetByteArrayRegion`*, `GetIntArrayElements/Release…`, `GetObjectArrayElement`, `GetPrimitiveArrayCritical/Release…`, `GetJavaVM`,
  `ExceptionCheck/Clear`*, `RegisterNatives` **no** (todo son exports `Java_*`). (* = [I], visto por offset sin seguir el flujo completo; implementar la tabla completa de JNI 1.6 es barato y evita sorpresas.)
- **Un stub debe tolerar `jclass`/`jmethodID` nulos** (si T7 decide no llamar a algún `nativeInit`, el nativo llamará con IDs a 0): devolver 0/NULL.

Leyenda de "stub": valor que debe devolver la clase Java falsa.

### 5.1 `com/gameloft/android/ANMP/GloftZOHM/Game` (IDs cacheados en `Game.nativeInit`; todos `static`)

| Método | Firma JNI | Qué hace en Android | Stub |
|---|---|---|---|
| `getSDFolder` | `()Ljava/lang/String;` | carpeta de datos (pref. `SDFolder`), por defecto `/sdcard/android/data/<pkg>/files` | ruta guest de datos, **sin** `/` final |
| `getSaveFolder` | `()Ljava/lang/String;` | por defecto `/data/data/<pkg>` | ruta guest de saves, sin `/` final |
| `GetObbPath` | `()Ljava/lang/String;` | `<ext>/Android/obb/<pkg>/` | ruta guest **con `/` final** (el nativo hace `strcat(path, nombre)`) |
| `GetMainObbName` | `()Ljava/lang/String;` | constante | `"main.1501.com.gameloft.android.ANMP.GloftZOHM.obb"` |
| `GetPatchObbName` | `()Ljava/lang/String;` | constante | `"patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb"` (o `""` si no existe: el nativo lo omite) |
| `GetExtraObbName` | `()Ljava/lang/String;` | pref. `extraFile` | `""` |
| `GetPhoneManufacturer` / `GetPhoneModel` / `GetPhoneDevice` / `GetPhoneFirmware` | `()Ljava/lang/String;` | `Build.*` | `"samsung"`, `"GT-I9500"`, `"ja3g"`, `"4.4.2"` (cualquiera; evitar modelos de la lista Xperia/`droid2`) |
| `GetPhoneCPUName` | `()Ljava/lang/String;` | línea `Processor` de `/proc/cpuinfo` | `"ARMv7 Processor rev 3 (v7l)"` |
| `GetPhoneCPUFreq` | `()F` | kHz máx. | `1900000.0f` |
| `GetPhoneGPUName` | `()Ljava/lang/String;` | `GL_RENDERER` | igual que lo pasado a `nativeSetGPUInfo` (p. ej. `"Adreno (TM) 330"`, ver §7) |
| `GetIMEI` | `()Ljava/lang/String;` | deviceId/serial | cadena fija, p. ej. `"000000000000000"` |
| `GetWSClose` | `()Z` | flag de welcome screen | `false` |
| `getNumberOfCPUCores` | `()I` | nº de núcleos | 4 |
| `getManufacture` | `()I` | código de fabricante | 0 |
| `GetTextureFormat` | `()I` | 1 PVRTC / 2 ATC / 3 DXT / 4 ETC1 | **4** |
| `GetPhoneLanguage` | `()I` | 0 en,1 fr,2 de,3 es,4 it,5 pt/br,6 ja,7 ko,8 zh,9 ru | según config (0 por defecto) |
| `GetPhoneLanguageS` / `GetCurrentLocale` | `()Ljava/lang/String;` | `Locale` | `"English (United States)"` / `"en_US"` |
| `GetCurrentTime` | `()J` | `System.currentTimeMillis()` | reloj real en ms |
| `isWifiEnabled` | `()I` | 1 si hay wifi | **0** (→ `ZombiesGame::SetInternet(false)`) |
| `IsJailBroken` | `()I` | existe `Superuser.apk` | 0 |
| `GetXperiaSlideout` | `()I` | 2 = no es Xperia Play | 2 |
| `keepScreenOn` | `(Z)Z` | wake lock | devuelve el argumento |
| `hasClosedActivity` | `()I` | flag GLLive | 0 |
| `checkFreeCashReady` | `()Z` | anuncios | `false` |
| `getRewardsAvailable` / `getRewardsUser` / `getInstalledGames` / `getGLLivePassword` / `getGLLiveUsername` | `()Ljava/lang/String;` | servicios Gameloft | `""` |
| `Exit` | `()V` | mata el proceso | cerrar la app host |
| `sendAppToBackground` / `Pause` | `()V` | ir a HOME | minimizar o no-op |
| `openBrowser` | `(Ljava/lang/String;)V` | `ACTION_VIEW` | no-op (o abrir URL en el host) |
| `playCinematicAndroid` | `(Ljava/lang/String;)V` | lanza `MyVideoView` con `res/raw/intro_hd.mp4` o `moviecomplete_hd.mp4` | no-op (ver §8) |
| `getHttpResponse` | `(Ljava/lang/String;)V` | GET en un hilo Java | no-op |
| `HideScreen` / `ShowScreen` | `(I)V` | oculta/muestra los layouts Android de logo/carga (0/1/2) | no-op (`HideScreen` se llama desde `SetupDevice` y menús) |
| `PrintDebug`, `ShowPlusOne` | `(Ljava/lang/String;)V` | vacío | no-op |
| `PopupTrophyGL`, `notifyTrophy`, `launchIGP`, `downloadWelcomeScr`, `showAds`, `loadFreeCash` | `(I)V` | GLLive/ads | no-op |
| `launchGLLive` | `(IZ)V` | GLLive | no-op |
| `enableLaunchPN` | `(Z)V` | push | no-op |
| `logoutGLLive`, `displayWelcomeBackUser`, `launchWelcomeScr`, `setIsPayingUser`, `hideAds`, `showInterstitial`, `showFreeCash` | `()V` | varios | no-op |

### 5.2 Clases de plataforma del juego

| Clase (prefijo `…/GloftZOHM`) | Dónde se resuelve | Métodos estáticos (firma) → stub |
|---|---|---|
| `GL2JNILib` | `GL2JNILib.init` (jclass arg) | `createView()V` → **dispara 4a** (crear vista/ventana y llamar `GameRenderer.nativeInit`); `setViewSettings(IIIII)V` → guardar (pixelSize, depth, stencil, csaa, flag) para elegir config EGL; `setCurrentContext(I)Z` → `eglMakeCurrent` del contexto *i* (−1 = soltar); devolver `true` para 0/−1 (con 0 contextos extra no se pide otro); `setupPaths()V` vacío; `enableAccelerometer(ZF)V` vacío; `getResource(Ljava/lang/String;)[B` → `null` (sin llamadores) |
| `/GLUtils/SUtils` | `AndroidOS::Init` (FindClass) y `SUtils.nativeInit` (jclass) | `getPreferenceString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` → `""` (clave `SDFolder` de `GamePrefs`: `""` ⇒ el nativo usa `/sdcard/android/data/<pkg>/files`); `getPackage()Ljava/lang/String;` → `"com.gameloft.android.ANMP.GloftZOHM"`; `getSaveFolder()`/`getSDFolder()` → como en `Game`; `getContext()Landroid/content/Context;` → objeto opaco no nulo; `playVideo(Ljava/lang/String;Z)Z` → `false`; `stopVideo()V`, `inGameVideoSetSkipEnabled(Z)V`, `shareInfo(…3×String)V`, `showCantGoBackPopup(I)V`, `deleteFile(Ljava/lang/String;)V` → no-op (deleteFile: borrar el fichero); `getAssetAsString(Ljava/lang/String;)[B` → contenido del asset o `null`; `getGameName()`, `getInjectedIGP()`, `getInjectedSerialKey()`, `getMetaDataValue(String)` → `""`; `retrieveBarrels()[I` → `int[0]`; `getGLUID(Ljava/lang/String;)[I` → 4 enteros fijos; `initCheckConnectionType()I` → 0; `nativeSetPreference(Landroid/os/Bundle;)V` / `nativeGetPreference(Landroid/os/Bundle;)Landroid/os/Bundle;` → almacén clave/valor persistente (**preferencias del juego**; campos de bundle `npDataType, npKey, npPrefName, npData, npDefaultValue, npResult`); `genericUnzipArchive(String,String)Z` → `false`; `removeDirectoryRecursively(String)Z` → `true` |
| `/GLUtils/Device` | `AndroidOS::Init`, `Device.nativeInit` | `getUserAgent()` → UA fijo; `d1`, `getAndroidId`, `getSerial`, `getSerialNo`, `getDeviceFirmware`, `getMacAddress`, `getDeviceIMEI`, `getHDIDFV`, `getHDIDFVVersion`, `getGLDID`, `getDeviceName`, `getPhoneManufacturer`, `getPhoneModel`, `retrieveDeviceCarrier`, `retrieveDeviceCountry`, `retrieveDeviceRegion`, `retrieveDeviceLanguage`, `retrieveCPUSerial`, `getPhoneDevice`, `getPhoneProduct` — todos `()Ljava/lang/String;` → cadenas fijas no nulas (`"en"`, `"US"`, `"00:00:00:00:00:00"`…) |
| `/GLUtils/ResLoader` | `AResLoader_Init` (FindClass) | `getLength(Ljava/lang/String;)I` → tamaño del asset del APK o −1; `getBytes(Ljava/lang/String;)[B`; `getAssetList(Ljava/lang/String;)[Ljava/lang/String;` → lector de `assets/` (solo hay `colors.conf` y `families.list`, que también están en el OBB). Devolver −1/`null` [I] |
| `/installer/GameInstaller` | `AndroidOS::Init` (FindClass), `initNative` | campos estáticos `mPreferencesName` (`Ljava/lang/String;` = `"GamePrefs"`), `m_sInstance`, `mDeviceInfo` (`Landroid/telephony/TelephonyManager;`); métodos `startGame()V`, `getSDFolder()Ljava/lang/String;`. Solo `mPreferencesName` se lee al jugar (`AndroidOS::GetSDFolder`) |
| `/SendInfo` | `SendInfo.initMethods` | `getSDFolder()`, `getGLDID()`, `getPhoneCarrier()`, `getLocaleCountry()`, `getLocaleLanguage()`, `getManufacturerModel()` → String; `getGLUID()[I`. Tracking; opcional |
| `/DataSharing` | `DataSharing.nativeInit` | `setSharedValue(String,String)V`, `getSharedValue(String)String` → `""`, `deleteSharedValue(String)V` |
| `/PushNotification/C2DMAndroidUtils` | `C2DMAndroidUtils.nativeInit` | `HasPushNotification()I` → **0 (se consulta en cada `Application::Update`)**; `ResetNotificationStatus()V`; `SetEnable(Z)V`; `IsEnable()Z` → false; `CancelAll()V`; `GetUserID()String`; `GetStatus(Z)I` → 0; `SendPush(Landroid/os/Bundle;String;String;Z)String`; `SendPushToMyself`, `SetOfflineDeviceCredential()I`, `SetUserCredential(String,String)I`, `SetCustomUserCredential(String,String,String)I`, `DeletePush(String,Z)I`, `GetJanusToken(String,String,String)String`, `GetBundleData()Landroid/os/Bundle;` → null, `SetDontDisturbEnable(Z)V`, `IsDontDisturbEnable()Z`, `GetRegistrationID()String` |
| `/SplashScreenActivity` | FindClass en cada uso | `downloadWS(String)V`, `isWSReady(String)I` → 0, `isActive()I` → 0, `showLocalWS(String)V`, `deleteWS(String)V`, `SetWSLanguage(I)V` |
| `/InGameBrowser` | FindClass | `showForum()V` |
| `/AdServer` | FindClass | `staticShowInterstitialWithTags(String)V` |
| `/iab/InAppBilling` | `InAppBilling.nativeInit` | nombres ofuscados (se descifran con `InAppBilling::readChar`); **[NC]**. Usa además una clase tipo Bundle. No llamar a `InAppBilling.nativeInit*` y tolerar IDs nulos |
| `com/gameloft/glot/PortingJNI` | `glot::IsEnvAndClassSet` (FindClass) | `getSystemUpTimeMillis()J` → ms monotónicos; `hasConnectivity()Z` → **false**; `getIdentifier()`, `getMac()`, `getDeviceName()`, `getDeviceFirmware()`, `getDeviceLanguage()`, `getDeviceCountry()`, `getDeviceCarrier()` → String fijas |

### 5.3 Social (todas opcionales; IDs cacheados en los `nativeInit` respectivos)

- `com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib`: `Init(ZLjava/lang/String;)V`, `Login`, `Logout`, `GetMyInfo`, `GetFriends…`, `PostToWall…`,
  `postOpenGraphAction`, `GetPicture`, `getUserAchievements`… (`V`), `IsLoggedIn()Z` → **false**, `hasPermission(String)Z` → false, `getAccessToken()String` → `""`.
- `com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib`: `InitGameAPI`, `ConnectToService`, `ShowAchievements`, `UnlockAchievement`, `SubmitScore`…
  (`V`), `IsLoggedIn()Z` → false, `GetAccessToken()String` → `""`.
- `com/gameloft/GLSocialLib/PlatformAndroid`: `GetDataFromURL(Ljava/lang/String;)[B` → `null`.
- `…/renren/RenrenAndroidGLSocialLib`: clase ausente en este APK (los `Java_*` existen); ignorar.

### 5.4 Clases del framework Android que el nativo usa directamente

| Clase | Uso | Stub |
|---|---|---|
| **`android/media/AudioTrack`** | **salida de audio por defecto** (§8): `<init>(IIIIII)V`, static `getMinBufferSize(III)I`, static `getNativeOutputSampleRate(I)I`, `play()V`, `pause()V`, `stop()V`, `release()V`, `write([BII)I`, `getPlayState()I` | implementación real sobre SDL (T6/T7) |
| `android/os/Process` | `glf::Thread::Impl::SetPriority` → static `setThreadPriority(I)V` | no-op |
| `android/os/Bundle` | `ABundle::init`: `<init>()V`, `putString`, `getString`, `getInt`, `putInt`, `getLong`, `putLong`, `getBoolean`, `putBoolean`, `containsKey(String)Z`, `clear()V`, `getByteArray`, `putByteArray` | mapa clave→valor real (lo usan preferencias `nativeSet/GetPreference`, push e IAP) |
| `android/view/ViewRoot` | `Game.processTouchpadAsPointer` → `setProcessPositionEvents(Z)V` | no llamar a ese nativo |
| `android/telephony/TelephonyManager`, `android/os/Build`, `android/os/Build$VERSION`, `android/os/SystemProperties`, `android/provider/Settings$Secure`, `android/content/Context`, `java/util/UUID` | solo en `GameInstaller.nativeGetDevID` (DRM) | no llamar |

---

## 6. Archivos

### 6.1 Rutas

Todas las rutas absolutas salen de tres valores que da Java (copiados en `Game.nativeInit`), más literales:

| Origen | Valor Android | Uso |
|---|---|---|
| `getSDFolder()` → `GetSDFolderPathPointer()` | `/sdcard/android/data/com.gameloft.android.ANMP.GloftZOHM/files` | `m_gAppPath = sd + "/"` (directorio de datos de trabajo: descargas, `d_o_w_n_l_o_a_d_e_d.txt`, ficheros extraídos, trofeos); `CopyPath()` |
| `getSaveFolder()` → `GetSaveFolderPathPointer()` | `/data/data/com.gameloft.android.ANMP.GloftZOHM` | saves; `CopySPath()` |
| `GetObbPath()` + `Get{Main,Patch,Extra}ObbName()` | `/sdcard/Android/obb/<pkg>/` + nombre | OBB montados como archivo |
| literales en `.rodata` | `/data/data/<pkg>/player.dat`, `/data/data/<pkg>/playerbckp.dat`, `/data/data/<pkg>/heatup.dat`, `/data/data/<pkg>/Playtest.txt|csv` | **save principal y su copia** (rutas *hardcoded*, no pasan por `getSaveFolder`) |
| literales | `/proc/meminfo` (lee `MemTotal:` para el perfil de memoria), `/sdcard/gameloft/games/ANDROID.txt`, `/system/app/Superuser.apk`, `/system/bin/su`, `/system/xbin/su` | sondas; pueden no existir |

Otros nombres relativos al directorio de save/datos vistos en `.rodata`: `/level.dat`, `/zomb.dat`, `/dlc.dat`, `/cloudlevel.dat`, `/cloudplayer.dat`,
`/cloudzomb.dat`, `/BackupCloudData.dat`, `/Nope.dat`, `/Nogplus.dat`, `serverConfig.sav`, `c2dm.sav`, `tracker%d.dat`, `tracking_markers.dat`,
`error_events.dat`, `ses_info.dat`, `user_data.dat`, `r_ev.dat`, `w_ev.dat`, `aux_file_data.dat`, sufijos `.sav`, `.data`, `.tmp`, `.bin`, `promo.xml`.

**Recomendación (T4)**: una capa de traducción de rutas en `fopen/stat/opendir/mkdir/remove/rename/unlink/open`:
`/data/data/com.gameloft.android.ANMP.GloftZOHM/**` → `<userdir>/save/**`; `/sdcard/android/data/<pkg>/files/**` (comparar sin distinguir mayúsculas:
aparece `android` y `Android`) → `<userdir>/files/**`; `/sdcard/Android/obb/<pkg>/**` → carpeta de OBB del usuario; `/proc/meminfo` → fichero sintético
(`MemTotal:  2097152 kB`); resto → `ENOENT`. Crear los directorios de antemano (el nativo solo hace `mkdir` de subcarpetas).

Acceso a ficheros del motor: `glf::FileStreamImpl` → `fopen(path, "rb" | "wb" | "r+b" | "a+b")`, `fseek/ftell` para el tamaño, `fread/fwrite`, **`pread` con
`FILE->_file`** (§3) para lecturas asíncronas (`FileStreamImpl::ReadASync` crea un `glf::Thread`), `fflush + fsync + fclose`. Directorios con
`opendir/readdir/closedir` (`glf::fs::DirHandle::FindFirst`), `mkdir` (`glf::Fs::MkDir`), `unlink/remove/rename`, `stat` (17 sitios), `chdir/getcwd` (cwd virtual).

### 6.2 OBB y `.bar`

- **Los `.obb` son ZIP estándar** y **el nativo los abre directamente** (no espera los datos extraídos): `Application::SetupDevice` hace, sobre el
  `IFileSystem` de Glitch (`device+0x2c`, método virtual `+0x1c`, args `(path, 1, 1, 1)`), en este orden: **patch** (si el nombre no es vacío) →
  **extra** (si no vacío) → **main**. Con la semántica de Irrlicht (se busca en orden de alta) el patch tiene prioridad sobre el main [I].
  Antes: `glf::fs::SetAlternateDataDir(0, "./")` y `SetAlternateDataDir(1, <ruta global>)`.
- El instalador Java (`GameInstaller`) además **extrae** el OBB entrada por entrada a `sd_folder` (y valida tamaños/CRC). El port no necesita esa copia para
  el motor [I]; solo hay que dar al nativo los dos `.obb` tal cual. Si aparece algún acceso a un fichero suelto en `sd_folder` (p. ej. Vox con
  `nativeGetObbPath` en `VoxSoundManager::VoxSoundManager`, que abre los sonidos desde el OBB), T9 puede extraer el OBB como hace Java (main y luego patch
  sobrescribiendo) — **[NC]** qué ficheros concretos se piden sueltos.
- `main.1501….obb` (289 401 986 bytes): 562 entradas, **todas STORED** (método 0, sin cifrar, sin *data descriptor*): 436 `.wav`, 64 `.mpc` (Musepack SV8,
  magia `MPCK`), 8 `.vxn` (Vox nativo), 22 `.bdae`, **15 `.bar`**, `zombies.swf` (1.76 MB), `zombies_load.swf`, y configuración (`gamesettings.xml`,
  `debugsettings.xml`, `techniques.xml`, `tracker.xml`, `eventParameters.xml`, `eventsParameters.json`, `snsconfig.json`, `ZOMBIES_SDD_VOX.xml`,
  `Zombiewood_SDD_Vox_1.1.xml`, `soundsInfo.dat`, `PackedWaves*_sounds.txt`, `colors.conf`, `families.list`, `GameVersion.txt` = `1.5.0`).
- `patch.15325….obb` (1 479 926 bytes): 2 entradas **DEFLATED**: `effects.bar` y `zombies.swf` (sustituyen a las del main).
- **`.bar` = ZIP anidado** (misma firma `PK\x03\x04`; sin cabecera propia). Dentro del main van STORED, así que se pueden leer por offset sin extraer.
  Excepción: `oconf.bar` (168 bytes) es **texto plano** (`XPlayerURL: …`, `GGI: 27991`, `GameVer: 1.5.0`…).

  | `.bar` | Entradas | Contenido |
  |---|---|---|
  | `actors.bar` | 520 | `.tga` (341), `.bdae` (154), `.png` (25) |
  | `weapons.bar` | 531 | `.bdae`, `.png`, `.tga` |
  | `swfs.bar` | 546 | `.png` (texturas del UI Flash; 489 STORED) |
  | `location_{BuildingTop,HauntedMansion,Hollywood,SantaMonica,Studio,mp}.bar` | 222/95/343/293/8/13 | `.tga`, `.bdae`, `.irr`, `.png` |
  | `levels.bar` | 143 | `.lvc` (142), `.bin` |
  | `effects.bar` | 127 | **92 `.glsl`**, 31 `.bdae` (materiales/efectos COLLADA binario), `glsl.config`, `GLSL/file.map`, `shaderMap.json` |
  | `fonts.bar` | 8 | `.ttf` |
  | `particlesystems.bar` | 22 | `.png`, `.tga`, `.bin` |
  | `zombies_strings.bar` | 23 | tablas de texto por idioma (`zombies_text_{en,fr,de,es,it,br,ja,ko,cn,ru,tr,vo}`, `android_*`) |

- **Tercer nivel**: muchos "ficheros" dentro de un `.bar` son a su vez ZIP: los `.bdae` contienen `little_endian_not_quantized.bdae` (magia `BRES`) +
  `version.nfo`; las texturas con alfa (`*.tga`, `*.png` lógicos) contienen `SPLIT` (4 bytes) + `rgb.ktx` + `alpha.ktx`. El motor resuelve esto solo
  (lector ZIP de Glitch + zlib interno); **la HLE solo tiene que servir `fopen/fread/fseek/pread` correctos sobre el `.obb`**.
- Cómo abre el motor un `.bar` dentro del OBB (por offset sobre el mismo `FILE*`, o cargándolo a memoria): **[NC]**. Nombres de `.bar` referenciados en
  `.rodata`: los 15 anteriores + `winter_assets.bar` (no existe en estos OBB; contenido opcional).
- Formatos de imagen reales tras descomprimir (todos los `.bar` del main): **1 918 KTX** (todos `glInternalFormat = 0x8D64 GL_ETC1_RGB8_OES`, con mipmaps),
  530 PNG reales, 23 TGA sin comprimir (tipo 2, 24/32 bpp) y **19 PVR v2** en `swfs.bar` con extensión `.png` (cabecera de 52 bytes, magia `PVR!`,
  formato `0x19` = **PVRTC 4bpp**, p. ej. `posters_03.png` 1024×1024). Ver §7.

---

## 7. Gráficos

- **GLES 2.0** exclusivamente: Java crea el contexto con `EGL_CONTEXT_CLIENT_VERSION = 2` y `CGlfDevice::createDriver` → `createOpenGLES2Driver`
  (`CCommonGLDriver<CProgrammableGLDriver<…CGLSLShaderHandler…>>`). No se importa ninguna función de pipeline fijo. EGL lo gestiona Java: el nativo **no**
  llama a EGL ni hace *swap*; pide cambio de contexto vía `GL2JNILib.setCurrentContext(I)Z`.
- **Contextos**: `getNumExtraContext()` devuelve 0 → un solo contexto, en el hilo GL. `glf::App::ReserveContext()` (llamado por
  `glitch::thread::this_thread::reserveContext` desde hilos de tareas) falla limpiamente cuando no hay contextos extra y `SetAsMainThread()` llama
  `setCurrentContext(0 | -1)`. → T5 puede asumir **un contexto y un hilo GL** [I] (si se quisiera carga de texturas multihilo habría que cambiar
  `CreationSettings+0x3c` y dar contextos compartidos; no recomendado).
- **Framebuffer por defecto pedido** (`setViewSettings`): color 16 bpp (RGB565), depth 16 (Java prueba 24), stencil 8 solo si la GPU está en la lista de
  §4, sin MSAA/CSAA. En PC: pedir RGBA8 + D24S8 no rompe nada.
- **`glGetString`**: 6 sitios. `genericDriverInit` consulta `GL_VERSION` (espera formato `"OpenGL ES <maj>.<min> …"`: busca `"OpenGL "` y parsea `%u.%u`; avisa
  si < 1.1), `GL_RENDERER` (compara con `"Mali-400 MP"` para un workaround de *vertex attrib shadow state*), `GL_VENDOR`, `GL_EXTENSIONS`
  (`CCommonGLDriverBase::initExtensions`); `CGLSLShaderHandler::doVersionCheck` consulta `GL_SHADING_LANGUAGE_VERSION` (`"OpenGL ES GLSL ES 1.00"`).
  **Recomendación**: T5 sintetiza las cadenas en memoria guest: `GL_VERSION = "OpenGL ES 2.0"`, `GL_SHADING_LANGUAGE_VERSION = "OpenGL ES GLSL ES 1.00"`,
  `GL_VENDOR`/`GL_RENDERER` = los de un Adreno (coherente con `nativeSetGPUInfo`), y una lista de extensiones **filtrada** (lista blanca).
- **Extensiones**: `initExtensions` compara contra una tabla genérica de ~300 nombres de Glitch (`0xb9b700`–`0xb9e400`, incluye extensiones de escritorio).
  Las que tienen efecto práctico con los imports presentes: `GL_OES_compressed_ETC1_RGB8_texture`, `GL_IMG_texture_compression_pvrtc`,
  `GL_AMD_compressed_ATC_texture`, `GL_EXT_texture_compression_dxt1`/`s3tc`, `GL_OES_mapbuffer` (`glMapBufferOES/glUnmapBufferOES`), `GL_OES_texture_3D`
  (`gl*3DOES`), `GL_OES_depth24`, `GL_OES_packed_depth_stencil`, `GL_OES_depth_texture`, `GL_OES_rgb8_rgba8`, `GL_OES_element_index_uint`,
  `GL_OES_texture_npot`, `GL_OES_standard_derivatives`, `GL_OES_vertex_half_float`, `GL_OES_texture_float/half_float`, `GL_EXT_texture_format_BGRA8888`,
  `GL_EXT_discard_framebuffer`, `GL_OES_vertex_array_object`, `GL_OES_get_program_binary`. Qué rama toma el driver para cada una: **[NC]** (no decompilado
  entero). Lista blanca sugerida: `GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8
  GL_OES_element_index_uint GL_OES_texture_npot GL_OES_standard_derivatives GL_OES_depth_texture` y, solo si T5 implementa el *shadow buffer*,
  `GL_OES_mapbuffer`. **No** anunciar `GL_OES_texture_3D`, `GL_OES_vertex_array_object`, `GL_OES_get_program_binary` ni PVRTC/ATC/DXT.
- **Texturas comprimidas**: un único sitio llama a `glCompressedTexImage2D` (`CTexture::updateData`), otro a `…SubImage2D` y a las variantes `3DOES`.
  Cargadores presentes: `CImageLoaderKTX`, `PVR`, `DDS`, `PNG`, `JPG`, `TGA`, `BMP`. Datos reales: **ETC1 en KTX (1 918)** + 19 PVRTC4.
  - **ETC1**: no se ha encontrado decodificador software de ETC1 en el `.so` → **hay que anunciar `GL_OES_compressed_ETC1_RGB8_texture`** y aceptar
    `internalformat 0x8D64`. ANGLE/D3D11 lo expone normalmente (lo transcodifica a RGBA8); si un backend no lo expone, T5 debe decodificar ETC1 a RGB8 en el
    thunk de `glCompressedTexImage2D` (≈150 líneas) y llamar a `glTexImage2D`. El alfa va en una segunda textura ETC1 (`alpha.ktx`) que mezcla el shader.
  - **PVRTC**: existe `PVRTCDecompress(const void*, int, int, int, uchar*)` llamado desde `glitch::video::pixel_format::convert` → el motor descomprime
    por software cuando el formato no está soportado [I]. No anunciar `GL_IMG_texture_compression_pvrtc`. Si aun así llegara `0x8C00–0x8C03` a
    `glCompressedTexImage2D`, devolver `GL_INVALID_ENUM` (riesgo: 19 pósters del UI en blanco).
  - `GetTextureFormat()` (Java) = 4 (ETC1) por coherencia.
- **Texturas 3D** (`glTexImage3DOES`, `glCompressedTexImage3DOES`, sub-variantes): solo alcanzables con `GL_OES_texture_3D`; no hay texturas 3D en los
  datos. Stub que pone `GL_INVALID_OPERATION`.
- **Shaders**: GLSL ES 1.00 en texto, en `effects.bar` (`*VS.glsl`/`*FS.glsl`, con `precision` por variable: `uniform lowp sampler2D …`, `varying mediump vec2 …`),
  referenciados por los efectos `.bdae` y `shaderMap.json`/`glsl.config`; también `%s/%s.shd` y `shader.xml` en `.rodata` (ruta alternativa [NC]). El motor
  antepone `#define`s y llama `glShaderSource` (vía veneer) con varias cadenas, `glCompileShader`, `glGetShaderiv/InfoLog`, `glLinkProgram`,
  `glGetActiveAttrib/Uniform`, `glGet{Attrib,Uniform}Location`. No usa binarios de programa (no importa `glProgramBinaryOES`). ANGLE acepta ESSL 1.00 tal cual.
- **Otros puntos GL** (99 imports, todos usados): FBO completo (`glGen/Bind/Delete{Framebuffers,Renderbuffers}`, `glFramebufferTexture2D`,
  `glFramebufferRenderbuffer`, `glRenderbufferStorage`, `glCheckFramebufferStatus`) → post-efectos (`Bloom*`, `UsePostEffects=1` en `gamesettings.xml`);
  `glCopyTexSubImage2D`, `glGenerateMipmap`, `glReadPixels` (capturas/depurador), `glPolygonOffset`, `glSampleCoverage`, `glBlendColor/Equation`, `glLineWidth`,
  uniformes `1f/1fv/1i/1iv/2fv/2iv/3fv/3iv/4fv/4iv/Matrix4fv` (no hay `Matrix2fv/3fv`), `glVertexAttrib4f`, `glVertexAttribPointer`, `glDrawArrays/Elements`.
- **Riesgos frente a ANGLE / traducción de punteros (T5)**:
  1. `glVertexAttribPointer`/`glDrawElements` con **punteros guest a arrays en cliente** (sin VBO): hay 26 sitios de `glBindBuffer` y 3 de `glBufferData`,
     así que se usan ambos modos [I]. Traducir guest→host y garantizar que el dato sigue vivo hasta el *draw* (con memoria guest mapeada 1:1 en el host
     basta con sumar la base).
  2. `glMapBufferOES` devuelve puntero: ANGLE devolvería un puntero host fuera del espacio guest → *shadow buffer* en memoria guest y `glBufferSubData`
     en `glUnmapBufferOES`, o no anunciar la extensión.
  3. `glGetString`, `glGet*InfoLog`, `glGetActive*`, `glGetShaderSource`, `glGenTextures/Buffers/…`, `glGetIntegerv/Floatv`, `glReadPixels`: punteros de salida guest.
  4. `glShaderSource`: `const char **` guest (array de punteros de 32 bits) + `const GLint *length` opcional.
  5. Precisión: shaders escritos para móviles (`lowp/mediump`); en D3D11 todo es `highp`, sin problema. `GL_OES_standard_derivatives` debe estar si algún shader usa `dFdx` [NC].
  6. RGB565/RGBA4444/RGBA5551 en `glTexImage2D` (formatos habituales de Glitch): ANGLE los soporta.
  7. El nativo asume que el *viewport*/tamaño lo fija `resize(w,h)`; un cambio de tamaño de ventana debe repetir `glViewport` + `GL2JNILib.resize`.

---

## 8. Audio y vídeo

Motor **Vox** (`vox::DriverAndroid`, `vox_driver_android.cpp`). Tiene dos backends y elige en `DriverAndroid::Init`:

```
if (DriverAndroid::s_androidAPILevel > 8)  _InitOSL();   // OpenSL ES
else                                        _InitAT();    // android.media.AudioTrack por JNI
```

`s_androidAPILevel` (`.data`, `0xcd6034`) vale **3** de fábrica y solo lo cambia el export `VoxSetAndroidAPILevel(int)`, que **nadie llama** (ni nativo
ni Java). → **En el APK real el audio sale por `AudioTrack` (JNI), no por OpenSL.** El port puede elegir:

**Opción A (comportamiento original, recomendada): AudioTrack falso en T7 + SDL en T6.**

- `_InitAT` (hilo que inicializa Vox): `vm->GetEnv(&env, 0x10002)`; `FindClass("android/media/AudioTrack")` + `NewGlobalRef`; IDs: `<init>(IIIIII)V`,
  static `getMinBufferSize(III)I`, `play/pause/stop/release ()V`, `write([BII)I`, `getPlayState()I`, static `getNativeOutputSampleRate(I)I`.
  - `rate = AudioTrack.getNativeOutputSampleRate(3 /*STREAM_MUSIC*/)` → **devolver 44100** (el mezclador interno es 44 100 Hz; con otra tasa activa un
    remuestreador lineal de punto fijo 2.14: paso = `round(44100·16384 / rate)`).
  - `minBuf = AudioTrack.getMinBufferSize(rate, 0xC /*CHANNEL_OUT_STEREO*/, 2 /*ENCODING_PCM_16BIT*/)` (bytes). `frames = minBuf / 4`;
    frames por callback = `min(frames, (int)(rate · 0.02322))` (≈1024 a 44.1 kHz). **Devolver ≥ 4096** (p. ej. 8192).
  - `pthread_create(UpdateThreadedAT)` + `pthread_mutex_init` + `pthread_cond_init`.
- `UpdateThreadedAT` (**pthread guest propio**): `AttachCurrentThread`; `PushLocalFrame(2)`; `track = NewObject(AudioTrack, <init>, 3, rate, 0xC, 2,
  frames*4, 1 /*MODE_STREAM*/)` → `NewGlobalRef`; `CallNonvirtualVoidMethod(play)`; `arr = NewByteArray(frames*4)`; bucle mientras el driver está vivo:
  si no está suspendido → `DoCallbackAT`; si lo está → `pthread_cond_wait` y `usleep(1)`. Al salir: `stop`, `release`, `DeleteGlobalRef`, `PopLocalFrame`, `DetachCurrentThread`.
- `DoCallbackAT`: `p = GetPrimitiveArrayCritical(arr, 0)`; bajo el mutex de Vox, `DriverCallbackInterface::_FillBuffer((short*)p, n)` (mezcla **PCM 16-bit
  con signo, estéreo entrelazado, little-endian**); `ReleasePrimitiveArrayCritical(arr, p, 0)`; `CallNonvirtualIntMethod(track, write, arr, 0, n*4)`;
  después se auto-regula con `_GetTime()` + `usleep` (duerme si va por delante del reloj). → `write` puede ser **no bloqueante** (encolar en un ring buffer
  de SDL y devolver `n`), pero es más robusto bloquear cuando la cola supere ~100 ms.
- `Suspend/Resume` (`Game.nativeVoxSuspend/Resume`) → `pause()`/`play()` y la condición.

**Opción B: forzar OpenSL** llamando al export `VoxSetAndroidAPILevel(14)` tras `JNI_OnLoad` y antes de la primera `step()`. Entonces `_InitOSL`:

- `SetDriverSampleRate(44100)`, periodo de callback `0.02322 s`, buffer propio de **4096 bytes** (1024 frames).
- `slCreateEngine(&engineObj, 0, NULL, 0, NULL, NULL)` → `Realize(SL_BOOLEAN_FALSE)` → `GetInterface(SL_IID_ENGINE)` → `CreateOutputMix(&mix, 0, NULL, NULL)`
  → `Realize` → `CreateAudioPlayer(&player, &src, &sink, 1, {SL_IID_BUFFERQUEUE}, {SL_BOOLEAN_TRUE})` con
  `SLDataLocator_BufferQueue{ 0x6 /*SL_DATALOCATOR_BUFFERQUEUE*/, numBuffers = 2 }` y
  `SLDataFormat_PCM{ formatType 2, numChannels 2, samplesPerSec 44 100 000 mHz, bitsPerSample 16, containerSize 16, channelMask 0x3 (FL|FR), endianness 2 (little) }`
  → `Realize` → `GetInterface(SL_IID_PLAY)`, `GetInterface(SL_IID_BUFFERQUEUE)` → `RegisterCallback(CallbackOSL, this)` → `DoCallbackOSL()` (ceba la cola)
  → `SetPlayState(SL_PLAYSTATE_PLAYING = 3)`. `Suspend` → `SetPlayState(2)`, `Resume` → `SetPlayState(3)`.
- `CallbackOSL(bq, ctx)` corre **en el hilo de audio de OpenSL** (en el port: un hilo guest dedicado, nunca el callback de SDL directamente): toma el mutex de
  Vox y llama `DoCallbackOSL`: `GetState(&st)`; mientras `st.count < 2`: `_FillBuffer(buf, 1024)` + `Enqueue(bq, buf, 4096)`.
  **Reutiliza el mismo puntero de buffer para los dos `Enqueue` consecutivos → `Enqueue` debe copiar los datos en el momento.**
- Métodos de vtable usados: `SLObjectItf`: `Realize`(+0), `GetInterface`(+0xc), `Destroy`; `SLEngineItf`: `CreateAudioPlayer`(+8), `CreateOutputMix`(+0x1c);
  `SLPlayItf`: `SetPlayState`(+0); `SLBufferQueueItf`: `Enqueue`(+0), `Clear`(+4), `GetState`(+8), `RegisterCallback`(+0xc). También existe
  `RecordCallbackOSL` (micrófono; no usado).

Formatos de audio de los datos: `.wav` (PCM/IMA-ADPCM/MS-ADPCM), `.mpc` (Musepack SV8, decodificador interno), `.vxn` (formato nativo Vox), Ogg (stb_vorbis
presente). Todo se decodifica en el `.so`; la HLE solo ve PCM. `VoxThread` crea hilos `glf::Thread` propios para streaming.

**Vídeo**: los `.mp4` (`res/raw/logo_hd.mp4`, `intro_hd.mp4`, `moviecomplete_hd.mp4`, `moviecomplete_ld.mp4`) los reproduce **Java**:
`ZombiesGame::PlayCinematic(name)` → `nativePlayCinematicAndroid` → `Game.playCinematicAndroid(String)` → activity `MyVideoView` (elige `intro` si el
nombre contiene "intro", si no `moviecomplete`). Mientras dura, la activity `Game` pierde el foco (→ `GL2JNILib.pause()` / `nativeVoxSuspend`) y al volver
`resume`. **`SUtils.onVideoFinished` nativo es una función vacía**: el nativo no espera ninguna notificación. Stub mínimo: no-op (se salta el vídeo);
versión completa (T9): pausar, reproducir el mp4 con el host, reanudar. `SUtils.playVideo(String,Z)Z` (vídeo in-game de anuncios) → `false`.

---

## 9. Input

Firmas JNI (todas `static`, `(JNIEnv*, jclass, …)`):

| Nativo | Firma | Comportamiento |
|---|---|---|
| `Game.nativeOnTouch(int action, int x, int y, int pointerId)` | `(IIII)V` | → `appOnTouch`. `action`: **1 = down, 2 = move, 0 = up**. `x,y` en píxeles del *surface* (origen arriba-izquierda), ya divididos por el factor de escala Java (`Game.a`, 1.0). Los *move* se **limitan por puntero** con `clock()` (umbral según perfil CPU/GPU); down/up pasan siempre. Acaba en `glf::AndroidAddTouchEvent` |
| `GL2JNILib.touchEvent(int action, int x, int y, int pointerId)` | `(IIII)V` | directo a `glf::AndroidAddTouchEvent` (sin limitador). Java no lo usa |
| `glf::AndroidAddTouchEvent(action, x, y, id)` | — | `Gamepad::RaiseTouchEvent(pad0, 0, type, id, (float)x, (float)y, true)` con `type` = `0xd6` (down), `0xd7` (move), `0xd8` (up) |
| `GameGLSurfaceView.nativeOnTouch` | `(IIII)V` | idéntico a `Game.nativeOnTouch` (vista antigua) |
| `Game.nativeOnKeyDown(int keyCode)` | `(I)V` | → `appOnKeyDown` = **función vacía** |
| `Game.nativeOnKeyUp(int keyCode)` | `(I)V` | → `Application::OnAndroidKeyEvent(false, keyCode)` |
| `Game.nativeAccelerator(float x, float y, float z)` | `(FFF)V` | `appOnAccelerator` |
| `Game.nativeOrientation(float, float, float)` | `(FFF)V` | `appOnOrientation` |
| `GL2JNILib.accelerometerEvent(float, float, float)` | `(FFF)V` | `Gamepad::RaiseAccelerometerEvent` |
| `GL2JNILib.orientationChanged(int)` | `(I)V` | `glf::AndroidOrientationChanged` → evento `type 100` (2/3 → `SetOrientation`) |
| `Game.nativeXperiaToggleJoysticks(int)` | `(I)V` | `Application::XperiaToggleJoysticks`: solo cambia frames del HUD Flash (muestra/oculta sticks virtuales) |
| `Game.processTouchpadAsPointer(ViewParent, boolean)` | `(Landroid/view/ViewParent;Z)Z` | llama por JNI a `android.view.ViewRoot.setProcessPositionEvents(Z)V` (API oculta de Xperia Play). **No llamar** |
| `Game.nativeCanInterrupt()` | `()I` | lee un flag global | 
| `Game.nativeCanAutoOrientate()` | `()Z` | flag |

- **Multitouch**: `pointerId` 0..n del `MotionEvent`. Java, en dispositivos normales, envía: `ACTION_DOWN` → `(1,x,y,0)`; `POINTER_DOWN` → `(1,x,y,idx)`;
  `MOVE` → `(2,x,y,id)` por cada puntero; `POINTER_UP` → `(0,x,y,idx)`; `ACTION_UP` → `(0,x,y,0)` **y** `(0,x,y,1)`. El juego es un *twin-stick shooter*:
  stick izquierdo (mover) y derecho (apuntar/disparar) son zonas táctiles del HUD Flash.
- **Teclas** (`Application::OnAndroidKeyEvent`, solo *key up*): si el juego está en estado "bloqueado" (flag `+4`) solo atiende **`KEYCODE_BACK (4)`** →
  `nativeExit()` o `nativeSendAppBackground()`; en estado normal → `CKeyManager::OnEvent(keyCode)`, que reconoce **`4` (BACK: navegación atrás/pausa, 38
  comparaciones)**, **`82` (MENU)**, y de forma puntual `0x1c`, `0x67`, `0x6c`, `0x130`, `0x136`, `0x137` (botones físicos de Xperia Play / códigos evdev)
  [I: significado exacto de estos últimos no confirmado]. Java filtra `VOLUME_UP/DOWN (24/25)` y `CAMERA (27)`.
- **Gamepad**: **no hay soporte nativo de mando**. `glf::Gamepad::RaiseButtonEvent` y `RaiseAnalogicStickEvent` existen pero **no tienen llamadores**; el
  único "mando" es el Xperia Play, cuyo *touchpad* Java convierte en toques con `pointerId` **60** (stick izq.) y **61** (stick der.) en coordenadas
  re-mapeadas (`x·82/360 + 70 [+480]`, `427 − y·82/360 [+5]`, es decir, sobre un HUD de 854×480).
  → **T8 debe implementar teclado/ratón/mando como toques virtuales**: ratón = puntero 0; WASD/stick izq. = puntero sintético que hace down en el centro
  del stick virtual izquierdo y move proporcional; stick der./ratón = segundo puntero sobre el stick derecho; `Esc` = `nativeOnKeyUp(4)`. Las posiciones
  exactas de los sticks en el HUD hay que medirlas en ejecución [NC].

---

## 10. Hilos

`pthread_create` tiene 4 sitios de llamada:

| Sitio | Quién lo usa | Propósito |
|---|---|---|
| `glf::Thread::Start(Runnable&, int prio)` | `glf::FileStreamImpl::ReadASync` (lecturas asíncronas con `pread`), `glitch::task::CCpuTaskHandler::init(n, …)` (pool de tareas del motor: carga de recursos/animación), `vox::VoxThread` (mezcla/streaming de Vox, nombre `"VoxThread"`) | atributos: `detachstate = JOINABLE (0)`, **`stacksize = 0x10000` (64 KB)**; rutina `glf::Thread::Impl::RunThread` → `Thread::MyRun`. Al arrancar cada hilo: `ThreadMgr::OnStartThread` (copia nodos TLS con `pthread_setspecific`) y los *start handlers* registrados, entre ellos `glf::AndroidOnStartThread` (`GetEnv` + `AttachCurrentThread` + `android.os.Process.setThreadPriority`). Al salir: `AndroidOnExitThread` (`DetachCurrentThread`) |
| `vox::DriverAndroid::_InitAT` | driver de audio AudioTrack | hilo `UpdateThreadedAT` (§8). Solo en opción A |
| `glwebtools::Thread::Start(Priority)` | `GlWebToolsCore::Initialize`, `JobRunner::Start`, `gaia::Gaia::Initialize`, `gaia::ThreadManager::Update`, `AnubisLib::AnubisManager::Initialize`, `CHeatUpManager` | HTTP/servicios online (curl). Con red cortada quedan ociosos o no se crean |
| `CTransportManagerImpl::StartReceiverThread` | multijugador | no se alcanza sin red [I] |

Hilos "externos" que entran en código guest: el hilo **UI** (init, input, pausa) y el hilo **GL** (todo lo demás). En el port pueden ser el mismo hilo host
siempre que se respete el orden de §4; `glf` identifica el hilo principal con `pthread_self()` en `App::SetAsMainThread` (llamado desde `resize`), así que
**`step`, `resize`, `stateChanged` y `getViewSettings` deben ejecutarse en el mismo hilo guest**. El input (`nativeOnTouch`) llega en Android desde el hilo UI
mientras `step` corre en el GL: las colas de eventos de `glf` están protegidas [I], pero lo más seguro es inyectar el input entre frames desde el hilo GL.

Sincronización usada:

- `pthread_mutex_*` (11 `init`, 47 `lock`, 49 `unlock`, 2 `trylock`) con `pthread_mutexattr_settype` → **recursivos** (`glf::Mutex`, `vox::Mutex`,
  `glwebtools::Mutex`); `pthread_cond_*` (`wait`, `timedwait`, `signal`, `broadcast`) en `glf::Condition` y en el driver de audio; `pthread_once` (1);
  `pthread_join` (4 sitios); `sched_yield`, `usleep`.
- **`glf::SpinLock`** y los contadores de referencias usan **`ldrex/strex`** → el JIT necesita monitor exclusivo global entre hilos guest (dynarmic
  `ExclusiveMonitor`); si T3 serializa los hilos guest con un *big lock*, debe soltarlo en las llamadas bloqueantes (`pthread_cond_wait`, `usleep`,
  `AudioTrack.write`, E/S) o habrá interbloqueos con `SpinLock`.
- `__cxa_guard_acquire/release` (530 sitios): inicialización perezosa de estáticos locales desde varios hilos → implementación con exclusión real.
- TLS: `pthread_key_create` en `AndroidOS_GetEnv` (con destructor que hace `DetachCurrentThread`), `glf::ThreadMgr::Init` y `__cxa_get_globals`.
  `glf::Thread::GetSequentialThreadId()` asigna índices pequeños por hilo (arrays de tamaño fijo, 16 handlers/hilos) → no crear hilos guest extra innecesarios.
- Las pilas de 64 KB que pide `glf` son escasas para un JIT con *fastmem* pero es lo que usa el original; el host puede reservar más sin efectos.

---

## 11. Red, DRM, IAP y social

Los servidores de Gameloft ya no existen; todo debe responder "sin conexión". Puntos de corte, por capa:

1. **Java (T7)**: `Game.isWifiEnabled()` → 0 (→ `ZombiesGame::SetInternet(false)` en `appInit`, `appResume` y periódicamente en `appUpdate`);
   `PortingJNI.hasConnectivity()` → false (GLOT/tracking no intenta enviar); `SUtils.initCheckConnectionType()` → 0; **no llamar** a
   `NetworkStateReceiver.setConnectionType` ni a `Game.nativeConnectionLoss`; `C2DMAndroidUtils.HasPushNotification()` → 0; `IsLoggedIn()` → false en
   Facebook/GameAPI; `GetDataFromURL` → null; `getHttpResponse`/`openBrowser` → no-op.
2. **libc (T4)**: `socket()` → −1 (`errno = EACCES`), `getaddrinfo` → `EAI_NONAME`, `gethostbyname` → NULL, `connect/send/recv` → −1,
   `select/poll` sin descriptores → dormir el *timeout* y devolver 0 (hay 18 sitios de `select`; **no** devolver inmediatamente o algún hilo girará en vacío).
   curl/glwebtools/CSocket/AnubisLib/`glf::debugger` fallan limpiamente al no poder crear el socket.
3. **DRM / instalador**: `GameInstaller.initNative/nativeStart/nativeGetDevID/getPublicKey` y `GDRMPolicy.*` solo los invoca la activity Java
   `GameInstaller` (licencia Google + descarga de datos). El juego (`Game` → `GL2JNILib`) no depende de ellos: **no llamarlos**. `nativeStart` solo hace
   `CallStaticVoidMethod(GameInstaller.startGame)`. [NC: no se ha buscado exhaustivamente una comprobación tardía de licencia dentro del juego; no hay
   cadenas ni llamadas que lo sugieran fuera de `installer_*`.]
4. **IAP**: `InAppBilling.*` (5 nativos + clase ofuscada `iab/s` con `bq/gk/gn/in/rn`) solo responde a callbacks Java de compra. Sin llamadas Java no
   hace nada. La tienda del juego mostrará ítems de dinero real sin precio; `Game.nativeAddCash(int, String)` es el nativo que acredita moneda (útil para
   un menú de trucos en T9).
5. **Social/Push/Ads/GLLive/Welcome screen/IGP**: todo va por métodos Java `void` (no-op) o getters (`""`/0/false), ver §5.
6. **Tracking (GLOT)** escribe ficheros locales (`tracker*.dat`, `error_events.dat`…) en la carpeta de save: dejarlo funcionar.

Riesgo: pantallas que **esperan** respuesta de red (login GLLive, `GameServerConfig.json`, multijugador "Arena", *cloud save*) podrían quedarse esperando un
*timeout* de curl en vez de fallar al instante; con `socket()` fallando el error es inmediato [I]. El modo historia es offline [I].

---

## 12. Riesgos y recomendaciones

### Generales

| # | Riesgo | Recomendación |
|---|---|---|
| R1 | Imports referenciados **por dirección** (`R_ARM_ABS32`/`GLOB_DAT` de `malloc`, `free`, `fread`, `strcmp`, `__cxa_pure_virtual`…) | T3: cada import = thunk en memoria guest (p. ej. `svc #n; bx lr` en ARM, o dirección trampa reconocida por el JIT); PLT/GOT y relocaciones de datos apuntan al mismo thunk |
| R2 | Interworking: PLT ARM, código Thumb-2, veneers `bx pc`, callbacks con bit 0 | Respetar el bit 0 en todas las entradas host→guest (`.init_array`, `Java_*`, start routines, comparadores) |
| R3 | `ldrex/strex` masivo con hilos reales | Monitor exclusivo compartido; validar pronto con el pool de tareas de Glitch y Vox |
| R4 | `FILE` con campos leídos inline (`_file` @14, `_flags` @12) y `pread` sobre ese fd | T4: `FILE` bionic real en memoria guest + tabla de fds propia; `pread` thread-safe |
| R5 | Excepciones C++ dependen de `__gnu_Unwind_Find_exidx` y de que el JIT permita al unwinder leer/restaurar registros (corre como código guest normal) | Implementar el import (§1); no requiere soporte especial del JIT |
| R6 | `setjmp/longjmp` guest | Implementarlos sobre el contexto del CPU emulado (o como rutinas en ensamblador guest inyectadas) |
| R7 | 552 constructores estáticos antes de `JNI_OnLoad` | HLE de libc/pthread completa antes de `.init_array`; log por constructor para depurar |
| R8 | El nativo llama a Java **dentro** de `GL2JNILib.init` (`createView`) y de `Game.nativeInit` (getters) | El JNI falso debe ser reentrante (host→guest→host→guest) |
| R9 | `AndroidOS::Init` hace `exit()` si no encuentra `SUtils`/`Device`/`GameInstaller` | `FindClass` debe devolver siempre un `jclass` no nulo (clase genérica "desconocida") y `GetStaticMethodID` un ID no nulo |
| R10 | Perfil gráfico/CPU decidido por cadenas (`GL_RENDERER`, modelo, nº de núcleos, `MemTotal`) | Presentarse como gama alta: `nativeSetGPUInfo("Adreno (TM) 330")`, 4 núcleos, 2 GB. (`GetGPUProfile`: `adreno` < 220 → 0; `powervr sgx` < 541 → 0; `nvidia ap` → 0; `nvidia tegra` → 1; resto → 2) |

### Por tarea

- **T3 (loader/CPU)**: reservar `0xd30000`; aplicar los 4 tipos de relocación; resolver todos los `JUMP_SLOT` en carga; `e_entry = 0`; ejecutar
  `.init_array` (552) y luego `JNI_OnLoad`. dynarmic A32 con Thumb-2, VFPv3 (D16 basta), IT blocks, `tbb/tbh`, exclusivos. CP15 no se usa (devolver 0 a
  cualquier `mrc`). Callbacks host→guest reentrantes y por hilo. Tamaños de pila: principal ≥ 1 MB; hilos `glf` piden 64 KB (dar ≥ 256 KB).
  Exportar a los demás módulos la resolución de símbolos por nombre (hay 35 581; útil para `VoxSetAndroidAPILevel`, `Game_nativeAddCash`, depuración).
- **T4 (libc)**: prioridades por frecuencia: `memcpy/memset/memmove/memcmp/strlen/strcmp` (nativos sobre punteros host), `sprintf/snprintf/sscanf`
  (formateo propio con varargs guest), `malloc/free` (heap guest propio, alineado a 8; ojo `strdup`), stdio con `FILE` bionic (§3), `stat`/dirent bionic,
  pthread con tablas laterales (mutex/cond de 4 bytes), `gettimeofday/clock/clock_gettime` monotónicos, `lrand48` exacto, tablas `_ctype_`/`_tolower_tab_`/
  `_toupper_tab_`, `setlocale` → `"C"`, `wchar_t` de 32 bits (¡en Windows `wchar_t` es de 16!: no delegar en la CRT). Red: todo a fallo (§11).
  Traducción de rutas (§6.1) y `/proc/meminfo` sintético.
- **T5 (GLES→ANGLE)**: contexto ES 2.0 único; cadenas `glGetString` sintetizadas y lista blanca de extensiones (§7); ETC1 obligatorio (nativo de ANGLE o
  decodificado en el thunk); traducción de punteros en ~25 funciones; `glMapBufferOES` con *shadow* o sin anunciar; stubs con error para las `3DOES`.
  El *swap* lo hace el frontend tras cada `step()`.
- **T6 (audio)**: implementar **AudioTrack falso** (opción A) con cola SDL a 44 100 Hz/S16/estéreo; `getMinBufferSize` = 8192; `write` copia y, si la cola
  supera ~100 ms, bloquea. OpenSL (opción B) solo como alternativa: 4 símbolos + vtables guest, `Enqueue` con copia inmediata, callback en hilo guest.
  Los 4 imports de OpenSL deben existir en cualquier caso (se resuelven en carga).
- **T7 (JNI)**: `JNIEnv`/`JavaVM` como vtables en memoria guest con thunks; familias `Call*Method` variádica **y** `V`; referencias como *handles* de 32
  bits; `String` UTF-8 (`NewStringUTF`/`GetStringUTFChars`), `byte[]`/`int[]`/`String[]`, `Bundle` (mapa), `AudioTrack`. Tolerar clases/IDs desconocidos
  (devolver 0/`""`/no-op y **registrar** `clase.método+firma` la primera vez: es la mejor herramienta para descubrir llamadas no listadas aquí).
  Tabla de stubs: §5. Secuencia: §4.
- **T8 (integración/input)**: bucle = `step()` + swap a 30–60 fps (el juego mide tiempo real con `gettimeofday`; [NC] si asume *vsync* a 60 o limita a 30);
  input como toques virtuales (§9); `Esc` → `nativeOnKeyUp(4)`; pérdida de foco → `GL2JNILib.pause()` + `nativeVoxSuspend()`, recuperación → `resume()` +
  `nativeVoxResume()`; resize → `glViewport` + `GL2JNILib.resize(w,h)` (y `GameRenderer.nativeInit` debe haberse llamado con el tamaño inicial).
  Resolución lógica: el HUD Flash está diseñado para 16:9 (854×480 de referencia en el mapeo Xperia).
- **T9 (instalador)**: entrada del usuario = este APK *repack* (`assets/game.apk` con el APK original, `assets/archiveObbCache.zip.001–005` = ZIP partido
  con los OBB, `lib/armeabi-v7a/libZombieWood.so`) **o** APK original + OBB sueltos. Salida: `libZombieWood.so`, los dos `.obb` sin tocar, y
  `res/raw/*.mp4` si se quiere vídeo. `libgenerator.so` es del *wrapper* (`com.dataManager`), no del juego: ignorarlo.

### No confirmado (resumen)

1. Qué comprueba `CheckDiskSpace()` (si falla, `MyInit` aborta el arranque).
2. Si el motor necesita ficheros sueltos en `sd_folder` además de los OBB montados, y cómo abre los `.bar` anidados (offset vs memoria).
3. Ramas exactas de `initExtensions`/driver por extensión; ausencia total de decodificador ETC1 software; uso real de `glMapBufferOES` y de arrays en cliente.
4. Nombres/firmas de los métodos Java de `InAppBilling` (ofuscados en el nativo).
5. Significado de los keycodes `0x1c, 0x67, 0x6c, 0x130, 0x136, 0x137` en `CKeyManager::OnEvent`; posiciones de los sticks virtuales en el HUD.
6. Semántica exacta de `ResLoader.getLength/getBytes` cuando el asset no existe y del 2.º directorio de `SetAlternateDataDir`.
7. Si alguna pantalla del flujo offline bloquea esperando red; *frame pacing* esperado (30 vs 60 fps).
8. Uso de NEON real (se asume ninguno) y de `getc/putc` inline sobre `FILE` (se asume ninguno).

---

## Apéndice A — Los 88 nativos `Java_*` exportados

Prefijo `P = Java_com_gameloft_android_ANMP_GloftZOHM_`, `S = Java_com_gameloft_GLSocialLib_`.

- `P GL2JNILib_`: `init`, `initGL`, `getViewSettings`, `getNumExtraContext`, `setNumExtraContext(I)`, `resize(II)`, `step`, `stateChanged(Z)`,
  `pause`, `resume`, `destroy`, `touchEvent(IIII)`, `accelerometerEvent(FFF)`, `orientationChanged(I)`, `setPaths(String,String,String)` (vacío).
- `P Game_`: `nativeInit`, `nativeOnTouch(IIII)`, `nativeOnKeyDown(I)`, `nativeOnKeyUp(I)`, `nativeAccelerator(FFF)`, `nativeOrientation(FFF)`,
  `nativeSetGPUInfo(String)`, `nativeSetCPUInfo(F)`, `nativeSetPhoneModel(String)`, `nativeSetIMEI(String)`, `nativeGetInfo(String×4)`,
  `nativeVoxMute`, `nativeVoxSuspend`, `nativeVoxResume`, `nativeVoxTutorialIntro`, `nativeAddCash(I,String)`, `nativeCanInterrupt()I`,
  `nativeCanAutoOrientate()Z`, `nativeConnectionLoss`, `nativeShortcutToScreens(String)`, `nativeXperiaToggleJoysticks(I)`,
  `processTouchpadAsPointer(ViewParent,Z)Z`.
- `P GameRenderer_`: `nativeInit(I,I,I,String)`, `nativeRender`, `nativeResize(II)`, `nativeDestroy`. `P GameGLSurfaceView_`: `nativeOnTouch(IIII)`,
  `nativePause`, `nativeResume`.
- `P GLUtils_`: `SUtils_nativeInit`, `SUtils_onVideoFinished` (vacío), `Device_nativeInit`, `NetworkStateReceiver_setConnectionType`.
- `P`: `DataSharing_nativeInit`, `SendInfo_initMethods`, `PushNotification_C2DMAndroidUtils_nativeInit`, `…_nativeCallBack`.
- `P iab_`: `InAppBilling_nativeInit`, `…_nativeSendData`, `…_nativeSetContext`, `…_nativeSetIABObject`, `s_bq`, `s_gk`, `s_gn`, `s_in`, `s_rn`.
- `P installer_`: `GameInstaller_initNative`, `…_nativeStart`, `…_nativeGetDevID`, `…_getPublicKey`, `GDRMPolicy_initNativeAP`, `…_nativeAllow`,
  `…_processServer`, `…_getConstString`, `…_setConst`, `…_setLongConst`, `…_setTime`.
- `S`: `PlatformAndroid_nativeInit`; `facebook_FacebookAndroidGLSocialLib_{nativeInit, nativeOnFBDataLoad, nativeOnFBDialogDidComplete,
  nativeOnFBDialogDidNotComplete, nativeOnFBFailWithError}`; `GameAPI_GameAPIAndroidGLSocialLib_{nativeInit, nativeGameAPIComplete,
  nativeGameAPICompleteWithData, nativeGameAPIDidNotComplete, nativeGameAPIDidPlusOneButton}`; `renren_RenrenAndroidGLSocialLib_{nativeInit,
  nativeOnRRDataLoad, nativeOnRRDialogDidComplete, nativeOnRRDialogDidNotComplete, nativeOnRRFailWithError}`.

Otros exports C útiles para el host: `VoxSetAndroidAPILevel(int)`, `VoxSetJavaVM`, `appPause/appResume/appUpdate/appOnTouch`, `nativeExit`, `mJavaVM`.

## Apéndice B — Herramientas (`tools/analysis/`)

| Script | Qué hace |
|---|---|
| `elf_abi.py <so> [json]` | cabeceras, segmentos, `.ARM.attributes`, `DT_*`, conteo de relocaciones por tipo, `.init_array` |
| `import_xrefs.py <so> <json>` | por import: stub PLT, slot GOT, nº de llamadas (BL/BLX ARM y Thumb-2 + veneers), relocaciones de datos, funciones llamadoras |
| `gen_imports.py <xrefs.json> <imports.json>` | genera `docs/imports.json` (firmas y notas mantenidas a mano en el script) |
| `insn_stats.py <so> [json]` | ARM vs Thumb, clases de instrucción (VFP/NEON/exclusivos/CP15…), mnemónicos FP |
| `rodata_strings.py <so> [--grep RE]` | cadenas de una sección con su dirección |
| `disasm_fn.py <so> <símbolo|0xaddr>[,…] [--jni] [--calls-only]` | desensamblado anotado: llamadas por nombre, cadenas/GOT de los idiomas PIC, slots de `JNIEnv` |
| `scan_all.py <so> <out.txt>` | base de referencias de todo `.text` (llamadas + cadenas + slots JNI por función), para `grep` |
| `jni_table.py <scan_all.txt>` | funciones que resuelven clases/métodos Java y la secuencia de nombres/firmas |
| `obb_inspect.py <obb> [--list] [--nested-list N]` | estructura de un OBB y de sus `.bar` anidados |
| `ghidra/DecompileList.java` | script headless: decompila una lista de funciones sin autoanálisis completo |

Las salidas voluminosas (proyectos Ghidra, `scan_all.txt`, JADX) viven en `C:\Users\chdut\ZombiewoodDev\analysis\`, fuera del repo.
