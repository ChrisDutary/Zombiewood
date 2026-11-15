# Zombiewood — contracts between modules (T3)

The core's public API (`zw_cpu` + `zw_loader`) used by T4–T8. The headers are the source of truth:

| Header | Contents |
|---|---|
| `cpu/guest_memory.h` | `gaddr`, `g2h/h2g`, `GuestMemory` (pages + heap), `mem::` (read/write), `GPtr<T>` |
| `cpu/cpu.h` | `Cpu` (registers, per-thread data), `call_guest`, `threads::`, `cpu::`, `debug::`, `GuestFault`, `GuestExit`, `gi64/gu64` |
| `cpu/hle.h` | `ZW_HLE*`, `hle::Register/StubFor/DefineData…`, `VaList`, `Varargs` |
| `loader/loader.h` | `loader::Load/RunInit/CallJniOnLoad/FindSymbol/addr2sym`, `Module` |

CMake for a consuming module: `target_link_libraries(zw_hle_xxx PUBLIC zw_cpu)` (+ `zw_loader` if it needs symbols) and
`#include "cpu/hle.h"`. There is no need to include anything from dynarmic.

**Stability:** from this commit on there will only be compatible changes (additions). Incompatible changes → through the coordinator.

Everything lives in the `zw` namespace. Guest ABI (T1): ARMv7-A, Thumb-2, VFPv3, **softfp**, little-endian, 32-bit `wchar_t`.

> Runtime log messages quoted below (e.g. `import sin handler`) are the literal Spanish strings the code prints.

---

## 0. Golden rules

1. **Never use host types whose size differs from the guest's in an HLE signature**: `size_t`, `long long`, `intptr_t`, `time_t`,
   `off_t`, `wchar_t`, `long double`. Use `zw::gsize` (= `uint32_t`), `int32_t`, `zw::gaddr`, `zw::gi64/gu64` (the guest's
   long long), `uint32_t` for `wchar_t`. A "bare" 64-bit integer as a parameter **does not compile** (on purpose).
2. **Structs seen by the guest are declared with fixed-width fields** (`uint32_t`, `GPtr<T>`), never with host pointers.
3. **Handlers run in parallel** on several host threads (one host thread per guest thread, no *big lock*). All global state
   of an HLE module must be thread-safe. A normal (non-`FAST`) handler may block freely.
4. **Explicit registration, not static**: every module exposes a `Register()` function and uses `ZW_HLE(...)` inside it. (MSVC's
   linker discards a static initializer in a `.lib` with no other references.) Naming convention:
   `zw::hle::libc::Register()`, `zw::hle::gles::Register()`, `zw::hle::audio::Register()`, `zw::jni::Register()`; T8 calls
   them **before** `loader::Load()`.
5. Every guest function address carries **bit 0 = Thumb**. Do not clear or set it by hand: use addresses exactly as
   `FindExport/FindSymbol`, `.init_array` or the guest itself provide them.

---

## 1. Guest memory (`cpu/guest_memory.h`)

4 GiB are reserved on the host: `host = zw::g_membase + guest`. Translating is an addition; there is no software page table.

```cpp
using gaddr = uint32_t;                 // guest address, 0 = NULL
template <class T = void> T* g2h(gaddr a);   // 0 -> nullptr
gaddr h2g(const void* p);                    // nullptr -> 0; logs an error (and returns 0) if p is not guest memory
```

### 1.1 Address map

| Zone | Range | Use |
|---|---|---|
| (null) | `0x00000000–0x0EFFFFFF` | never mapped (catches NULL and small offsets) |
| `Zone::Thunks` | `0x0F000000–0x0FFFFFFF` | import stubs (`svc #n; bx lr`), `call_guest` return stub |
| `Zone::Libs` | `0x10000000–0x1FFFFFFF` | loaded `.so` files. The first one sits at `0x10000000` ⇒ `guest = 0x10000000 + ELF vaddr` from ANALYSIS.md |
| `Zone::Heap` | `0x20000000–0x7FFFFFFF` | heap (`Alloc`) and `MapAnywhere(Zone::Heap)` |
| `Zone::Stacks` | `0x80000000–0x8FFFFFFF` | thread stacks (with a guard page) and TLS blocks |
| kuser | `0xFFFF0000–0xFFFF0FFF` | Linux ARM *kuser helpers* (`__kuser_cmpxchg` `0xffff0fc0`, `__kuser_memory_barrier` `0xffff0fa0`, `__kuser_cmpxchg64` `0xffff0f60`, `__kuser_get_tls` `0xffff0fe0`), implemented in guest code with `LDREX/STREX`. The `.so` calls them by fixed address (protobuf atomics); they are not imports |

Touching an unmapped guest address from the host = access violation; the runtime dumps it with the guest state and aborts.
A **guest** access to unmapped memory ends in a `GuestFault` with a dump, not a crash.
If a pointer comes from the guest and may be garbage, validate it with `GuestMemory::IsMapped(addr, size)`.

### 1.2 Pages

```cpp
bool  GuestMemory::Init();                                   // T8 calls it at startup (idempotent)
bool  GuestMemory::Map(gaddr addr, gsize size, Prot prot, std::string_view tag = {});
gaddr GuestMemory::MapAnywhere(Zone z, gsize size, Prot prot, std::string_view tag = {}, gsize align = 4096);
bool  GuestMemory::Unmap(gaddr addr, gsize size);
bool  GuestMemory::Protect(gaddr addr, gsize size, Prot prot);
bool  GuestMemory::IsMapped(gaddr addr, gsize size = 1);
```

`Prot`: `kProtRead|kProtWrite|kProtExec` (`kProtRW`, `kProtRX`…). New pages are zeroed. The `.so` text is mapped
**read-only** (writing to it from the guest is a fault; to patch it use `hle::HookGuestFunction` or
`Protect` + `cpu::InvalidateCode`).

### 1.3 Heap (for T4's `malloc` and for objects the HLE hands to the guest)

```cpp
gaddr GuestMemory::Alloc(gsize size);             // 8-byte aligned; Alloc(0) returns a valid block; 0 = out of memory
gaddr GuestMemory::Calloc(gsize n, gsize size);   // zeroed
gaddr GuestMemory::Realloc(gaddr p, gsize size);  // Realloc(0, n) == Alloc(n); keeps the contents
gaddr GuestMemory::AllocAligned(gsize size, gsize align);  // memalign (freed with Free)
void  GuestMemory::Free(gaddr p);                 // Free(0) is a no-op; double free -> error in the log, does not crash
gsize GuestMemory::UsableSize(gaddr p);
```

Thread-safe. `Alloc`'s contents are **not** zeroed. T4: `malloc/free/calloc/realloc/strdup` are direct wrappers.

### 1.4 Reading/writing

```cpp
uint32_t v = zw::mem::R32(addr);   zw::mem::W32(addr, v);      // R8/R16/R32/R64, W8/W16/W32/W64
auto st = zw::mem::Read<MyGuestStruct>(addr);   zw::mem::Write<MyGuestStruct>(addr, st);
std::string s = zw::mem::ReadCStr(addr);                       // "" if addr == 0
zw::mem::WriteCStr(dst, "text", cap);                          // truncates, always ends in '\0'
gaddr p = zw::mem::StrDup("hello");                            // on the guest heap (Free)
gaddr q = zw::mem::Dup(host_ptr, n);
```

`GPtr<T>`: a typed 4-byte guest pointer for struct fields and parameters:

```cpp
struct GuestIovec { zw::GPtr<uint8_t> base; uint32_t len; };   // sizeof == 8, same as in the guest
auto* io = zw::g2h<GuestIovec>(addr);
memcpy(dst, io->base.get(), io->len);
```

---

## 2. HLE imports (`cpu/hle.h`)

Each function import of the `.so` resolves to a **guest stub** in `Zone::Thunks` (ARM: `svc #n; bx lr`). The `SVC` dispatches
to the handler registered under that name. The same stub serves the PLT, `R_ARM_ABS32/GLOB_DAT` (pointers to `malloc`, etc.)
and any function pointer the HLE hands to the guest.

### 2.1 Registering

```cpp
#include "cpu/hle.h"
namespace {
int      hle_abs(int x)                               { return x < 0 ? -x : x; }
zw::gsize hle_strlen(const char* s)                    { return (zw::gsize)std::strlen(s); }
void*    hle_memcpy(void* d, const void* s, zw::gsize n) { return std::memcpy(d, s, n); }
double   hle_pow(double a, double b)                  { return std::pow(a, b); }
float    hle_sqrtf(float x)                           { return std::sqrt(x); }
zw::gaddr hle_malloc(zw::gsize n)                      { return zw::GuestMemory::Alloc(n); }
int      hle_errno_ptr_demo(zw::Cpu& cpu)             { return (int)cpu.errno_addr(); }   // Cpu& consumes no arguments
}
void zw::hle::libc::Register() {
  ZW_HLE("abs", hle_abs);
  ZW_HLE_FAST("strlen", hle_strlen);     // FAST: inside the JIT, see 2.4
  ZW_HLE_FAST("memcpy", hle_memcpy);
  ZW_HLE_FAST("pow", hle_pow);
  ZW_HLE_FAST("sqrtf", hle_sqrtf);
  ZW_HLE("malloc", hle_malloc);
}
```

- `ZW_HLE(name, fn)` / `ZW_HLE_FAST(name, fn)`: `fn` is a free/static, **non-overloaded** function (for `std::sin` and the
  like, write a wrapper). Marshalling is deduced from the signature.
- `ZW_HLE_RAW(name, void(*)(zw::Cpu&))`: a raw handler; reads `cpu.reg(0..3)`, `cpu.arg_word(i)` and writes `cpu.reg(0)`.
- `ZW_HLE_THUNK(name, fn)` → `gaddr`: registers and returns the stub's address (for vtables, see §7).
- `hle::Register(name, raw, flags)`: the non-macro form. Registering the same name again **replaces** the previous one (the
  last one wins); you can register before or after loading the `.so`.
- `hle::StubFor(name)` → the guest address of the stub (created if it did not exist; stable).

### 2.2 Automatic marshalling (AAPCS softfp, confirmed by T1)

Arguments are 4-byte "words": words 0–3 = `r0–r3`, word 4+ = `[sp + 4·(i−4)]`.

| C++ parameter type | Consumes | Notes |
|---|---|---|
| `bool`, integers and enums ≤ 32 bits, `zw::gaddr`, `zw::gsize` | 1 word | truncated to the type (`char`, `short`…) |
| `float` | 1 word | raw bits in a core register/the stack (softfp) |
| `double`, `zw::gi64`, `zw::gu64` | 2 words, **even index** | `r0:r1` or `r2:r3` (skips `r1`/`r3` if that was next) or 8-aligned stack |
| `T*` (host pointer) | 1 word | translated with `g2h` (0 → `nullptr`). Mapping is **not** validated |
| `zw::GPtr<T>` | 1 word | untranslated; `.addr`, `.get()` |
| `zw::VaList` | 1 word | a guest `va_list` (`vprintf(fmt, ap)`) |
| `zw::Varargs` | — (must be last) | the function's `...`: reads whatever follows the named parameters |
| `zw::Cpu&` | nothing | access to the current guest thread; may appear in any position |
| `int64_t`, `uint64_t`, `size_t`, `long long` | **compile error** | ambiguous: use `gi64/gu64` or `gsize` |

| Return type | Where |
|---|---|
| `void` | — |
| `bool`, integers/enums ≤ 32 bits | `r0` (sign/zero-extended) |
| `float` | `r0` (bits) |
| `double`, `gi64`, `gu64`, 64-bit integers | `r0:r1` (returning a host `size_t` is valid: `r1` is *caller-saved*) |
| `T*` | `r0 = h2g(p)` (must be guest memory or `nullptr`) |
| `GPtr<T>` | `r0` |

Not supported automatically: structs by value (e.g. `div_t div(int,int)`: the guest passes a hidden result pointer in
`r0`; declare `void hle_div(GPtr<DivT> out, int a, int b)`), and `long double` (= `double` on ARM).

### 2.3 Variadics and `va_list`

```cpp
// A module's shared formatter: takes an argument reader.
int do_vsnprintf(char* dst, zw::gsize cap, const char* fmt, zw::VaList& ap);

int hle_snprintf(char* dst, zw::gsize cap, const char* fmt, zw::Varargs va) { return do_vsnprintf(dst, cap, fmt, va); }
int hle_vsnprintf(char* dst, zw::gsize cap, const char* fmt, zw::VaList ap)  { return do_vsnprintf(dst, cap, fmt, ap); }

// Inside the formatter:
int         i = ap.Next<int>();             // %d %c %x  (char/short arrive already promoted to int)
double      d = ap.Next<double>();          // %f %g     (float promoted; Next<float>() does not compile)
zw::gi64    q = ap.Next<zw::gi64>();        // %lld      (8-byte aligned automatically)
const char* s = ap.Next<const char*>();     // %s        (translated to host; nullptr if 0)
zw::gaddr   p = ap.Next<zw::gaddr>();       // %p / %n
```

`VaList::FromGuest(gaddr ap)` and `VaList::FromCall(cpu, first_word)` build readers by hand (e.g. in a raw handler of
`CallStaticVoidMethod(env, cls, mid, ...)`: `VaList::FromCall(cpu, 3)`; in `CallStaticVoidMethodV(env, cls, mid, va_list)`:
a `zw::VaList` parameter). For `Call…MethodA(…, const jvalue* args)` read `mem::R64(args + 8*i)`.

### 2.4 `FAST` vs normal handlers

- **Normal** (`ZW_HLE`): the JIT stops at the `svc`, and the handler runs outside `Jit::Run`. It may call `call_guest`,
  block, throw `GuestExit`/`GuestFault`, create threads.
- **FAST** (`ZW_HLE_FAST`): runs inside the JIT callback (avoids leaving and re-entering the JIT on every call). **Forbidden**:
  `call_guest`, throwing C++ exceptions, blocking indefinitely, touching `pc/sp/lr`. Use it for `mem*`, `str*`, `libm`, and
  almost all of `gl*`. When in doubt, use normal.

### 2.5 Imported data

The 8 data imports (`__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `SL_IID_ENGINE`,
`SL_IID_BUFFERQUEUE`, `SL_IID_PLAY`) have `st_size = 0` in the `.so`: the owning HLE module decides the size.

```cpp
void zw::hle::libc::Register() {
  zw::gaddr guard = zw::hle::DefineData("__stack_chk_guard", 4);     // zeroed guest block, stable
  zw::mem::W32(guard, 0x5A5A1234);
  zw::gaddr sF = zw::hle::DefineData("__sF", 3 * 84);                // FILE __sF[3]
  zw::gaddr tab = zw::GuestMemory::Alloc(257);                       // table
  zw::gaddr ctype = zw::hle::DefineData("_ctype_", 4);               // `const char* _ctype_` (pointer variable)
  zw::mem::W32(ctype, tab);
}
```

**They must be defined before `loader::Load()`** (the GOT is filled at load time). Undefined data resolves to a zeroed
1024-byte block with a warning in the log (`Via::HleDataPlaceholder`); `hle::FindData(name)` returns it just the same.
Owners: `__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_` → T4; `SL_IID_*` → T6.

### 2.6 Imports without a handler, tracing and statistics

- Import without a handler: the **first** call writes `import sin handler: <name> (lr = <symbol>)` (import without handler)
  and returns 0 (`r0 = 0`). With `ZW_ABORT_ON_UNIMPLEMENTED=1` or `hle::SetUnimplementedPolicy(OnUnimplemented::Abort)` it
  throws `GuestFault`.
- `hle::UnimplementedImports()` lists the stubs requested without a handler (used by `--load-only`).
- `ZW_TRACE_IMPORTS=1` (or `debug::SetTraceImports(true)`): one line per call (`thread name(r0, r1, r2, r3) lr=…` and the
  return value). `ZW_TRACE_FILTER=substring` filters by name.
- `hle::CallCounts()`: per-import call counter.

### 2.7 Already implemented by T3 (do not register them)

`setjmp`, `_setjmp`, `sigsetjmp`, `longjmp`, `_longjmp`, `siglongjmp` (on top of the guest CPU state: r4–r11, sp, lr, d8–d15,
fpscr; bionic's 256-byte `jmp_buf` is more than enough; `longjmp` across `call_guest` boundaries works) and
`__gnu_Unwind_Find_exidx` (registered by the loader with each module's `.ARM.exidx` tables). If a module registers one of
these names, it replaces T3's.

### 2.8 Replacing internal functions of the `.so`

```cpp
static int my_CheckDiskSpace() { return 1; }
ZW_HLE("zw_CheckDiskSpace", my_CheckDiskSpace);
zw::hle::HookGuestFunction(zw::loader::FindSymbol("_Z14CheckDiskSpacev"), "zw_CheckDiskSpace");
```

Patches the function's entry with a jump to the stub (do it after `Load`, before that code runs).

---

## 3. Host → guest: `call_guest` (`cpu/cpu.h`)

```cpp
template <class R = uint32_t, class... A> R zw::call_guest(gaddr fn, const A&... args);
```

- Runs `fn` on the current host thread's `Cpu` and returns when the function returns (magic LR → return stub).
- **Reentrant**: can be called from a normal handler (qsort → comparator, `pthread_once`, JNI `Call…Method` → fake Java
  → native `Java_…` → …). Preserves all of the guest caller's core registers.
- Arguments: the same rules as §2.2 in reverse — integers ≤ 32 bits/`gaddr`/enum/`bool` (1 word), `float`
  (1 word), `double`/`gi64`/`gu64` (2 words, even index), `T*` (converted with `h2g`: it **must** point to guest
  memory), `GPtr<T>`, `nullptr`. A bare `int64_t`/`size_t` does not compile.
- `R`: `void`, integers, `bool`, `float`, `double`, `gi64/gu64/int64_t/uint64_t`, `T*` (translated with `g2h`), `GPtr<T>`.
- From a host thread that has never run guest code, the first call "attaches" it (creates its `Cpu` with a 1 MiB stack).
- Errors: `zw::GuestFault` (the guest failed; `what()` carries registers + backtrace) and `zw::GuestExit{code}` (someone
  called `exit`) propagate as C++ exceptions up to whoever called `call_guest` at the outermost level.

```cpp
// qsort (T4)
void hle_qsort(zw::gaddr base, zw::gsize n, zw::gsize sz, zw::gaddr cmp) {
  std::vector<uint8_t> tmp(sz);
  // ... host algorithm; to compare:
  int r = zw::call_guest<int>(cmp, base + i * sz, base + j * sz);
}
// Calling a JNI native (T7/T8)
zw::gaddr fn = zw::loader::FindSymbol("Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_resize");
zw::call_guest<void>(fn, env, clazz, 1280, 720);
zw::call_guest<void>(nativeAccel, env, clazz, 0.0f, 9.8f, 0.0f);     // floats: automatic softfp
```

Strings/arrays for the guest: copy them into guest memory first (`mem::StrDup`, `GuestMemory::Alloc`) and pass the `gaddr`.

---

## 4. `Cpu` and per-thread data (`cpu/cpu.h`)

```cpp
zw::Cpu& cpu = zw::Cpu::Current();      // the current host thread's (created if needed)
cpu.reg(0..15), cpu.sp(), cpu.lr(), cpu.pc(), cpu.cpsr(), cpu.fpscr(), cpu.d(i)/set_d(i, bits), cpu.s(i)/set_s
cpu.arg_word(i)        // AAPCS word i of the import call in progress
cpu.thread_id()        // uint32 != 0, unique among live threads (serves as pthread_t / gettid)
cpu.errno_addr()       // gaddr of a per-thread int -> `int* __errno()` returns this
cpu.tls()              // the thread's bionic TLS block (64 words; returned by TPIDRURO). [0] = self, [1] = thread_id
cpu.user(zw::UserSlot::Libc)   // per-thread void*& for each module (Libc, Pthread, Gles, Audio, Jni, Frontend, Spare0/1)
cpu.stack_base(), cpu.stack_size(), cpu.call_depth(), cpu.current_import()
```

```cpp
zw::gaddr hle_errno(zw::Cpu& cpu) { return cpu.errno_addr(); }          // ZW_HLE_FAST("__errno", hle_errno)
```

`user(slot)` starts as `nullptr`; free it in an *exit hook* (§5). The CP15 register `TPIDRURO` returns `cpu.tls()` (T1: the
game does not use it; the rest of CP15 reads 0).

---

## 5. Threads (`zw::threads`, `cpu/cpu.h`)

Model: **one host thread + one `Dynarmic::A32::Jit` per guest thread**, real concurrency, a global exclusive monitor for
`LDREX/STREX`. There is no *big lock*: nothing to release around blocking calls.

```cpp
struct CreateParams { gaddr entry; gaddr arg; gsize stack_size = 0; bool detached = false; std::string name; };
ThreadId threads::Create(const CreateParams&);          // 0 on failure. Stacks < 256 KiB are raised to 256 KiB
bool     threads::Join(ThreadId, uint32_t* retval = nullptr);
bool     threads::Detach(ThreadId);
bool     threads::IsAlive(ThreadId);
ThreadId threads::CurrentId();
void     threads::AddStartHook(std::function<void(Cpu&)>);   // on the new thread, before entry
void     threads::AddExitHook(std::function<void(Cpu&)>);    // on the thread, when it ends (pthread_key destructors, JNI detach)
void     threads::DetachCurrent();                           // for implicitly attached host threads
```

Implicitly attached host threads (those that call `call_guest`/`Cpu::Current()` without coming from `threads::Create`): the
*start hooks* run on attach; the *exit hooks* run **only** if the thread calls `threads::DetachCurrent()` (if the host thread
ends without calling it, its `Cpu` is destroyed without running hooks).

`pthread_create` (T4):

```cpp
int hle_pthread_create(zw::GPtr<uint32_t> out, zw::gaddr attr, zw::gaddr start, zw::gaddr arg) {
  zw::threads::CreateParams p{start, arg, attr ? stack_size_of(attr) : 0, attr && is_detached(attr), "pthread"};
  zw::threads::ThreadId id = zw::threads::Create(p);
  if (!id) return 11 /*EAGAIN*/;
  *out = id;                       // pthread_t = ThreadId; pthread_self() = threads::CurrentId()
  return 0;
}
int hle_pthread_join(uint32_t t, zw::GPtr<uint32_t> ret) {
  uint32_t v = 0;
  if (!zw::threads::Join(t, &v)) return 3 /*ESRCH*/;
  if (ret) *ret = v;
  return 0;
}
```

- The game's main thread is simply the host thread that calls `call_guest` (T8); `glf` requires
  `step/resize/stateChanged/getViewSettings` to run on the same host thread.
- pthread mutexes/conds/keys: side tables on the host (T4), with host primitives (`std::mutex`, `SRWLOCK`…). TLS keys can
  be stored in `cpu.user(UserSlot::Pthread)`.
- If a thread created with `Create` dies from `GuestFault`/`GuestExit`, `cpu::SetFatalHandler` /
  `cpu::SetExitHandler` is called (default: log and `_Exit`).
- At most `threads::kMaxThreads` (128) live guest threads.

---

## 6. Loader (`loader/loader.h`)

```cpp
zw::GuestMemory::Init();
zw::hle::libc::Register(); /* gles, audio, jni... */          // handlers + DefineData BEFORE loading
zw::loader::Module* m = zw::loader::Load("…/libZombieWood.so"); // maps + relocates + resolves; runs nothing
zw::loader::RunInit(*m);                                       // .init_array (552 constructors), on this thread
auto ver = zw::loader::CallJniOnLoad(*m, java_vm_gaddr);       // JNI_OnLoad(vm, NULL) -> 0x10004
```

- Resolving an undefined symbol: the module itself → other loaded libs (in load order) → HLE data
  (`STT_OBJECT`) → HLE stub (`STT_FUNC`, always resolves, with or without a handler).
- `Module::FindExport(name)` / `loader::FindSymbol(name)` → an address ready for `call_guest` (Thumb bit included), 0
  if it does not exist. All 35,581 symbols of the `.so` are available (e.g. `VoxSetAndroidAPILevel`, `nativeExit`, `mJavaVM`).
- `Module::imports()` → how each import was resolved (`Via`, `has_handler`). `Module::exidx()/exidx_count()`.
- `loader::addr2sym(addr)` / `zw::debug::addr2sym(addr)` → `"libZombieWood.so!_ZN3glf3App6UpdateEv+0x1c"`, `"hle!malloc"`.
- `Module::base()`: `0x10000000` for the first lib ⇒ ANALYSIS.md addresses are obtained by subtracting the base.
- Relocations: `R_ARM_RELATIVE`, `ABS32`, `GLOB_DAT`, `JUMP_SLOT`, `REL32`, `NONE` (REL). Everything is resolved at load time.

---

## 7. JNI and other guest vtables (T6/T7)

A `JNIEnv*` is a guest pointer to a guest pointer to a table of guest function pointers. Each entry is an HLE stub:

```cpp
// Handlers with the slot's real signature (marshalling handles softfp and varargs):
zw::gaddr jni_FindClass(zw::gaddr env, const char* name);
uint32_t  jni_CallStaticIntMethod(zw::gaddr env, zw::gaddr cls, zw::gaddr mid, zw::Varargs va);
uint32_t  jni_CallStaticIntMethodV(zw::gaddr env, zw::gaddr cls, zw::gaddr mid, zw::VaList ap);
double    jni_CallStaticDoubleMethodA(zw::gaddr env, zw::gaddr cls, zw::gaddr mid, zw::gaddr jvalues);

void zw::jni::Register() {
  zw::gaddr table = zw::GuestMemory::Calloc(232, 4);                       // JNINativeInterface (JNI 1.6: 232 slots)
  zw::mem::W32(table + 4 * 6,   ZW_HLE_THUNK("JNIEnv::FindClass", jni_FindClass));
  zw::mem::W32(table + 4 * 129, ZW_HLE_THUNK("JNIEnv::CallStaticIntMethod", jni_CallStaticIntMethod));
  // ...
  zw::gaddr env = zw::GuestMemory::Alloc(4);   zw::mem::W32(env, table);   // JNIEnv = &table
}
```

- Stub names: any unique string (`"JNIEnv::X"`, `"JavaVM::GetEnv"`, `"SLEngineItf::CreateAudioPlayer"`); they appear
  as-is in traces and backtraces (`hle!JNIEnv::FindClass`).
- Unimplemented slots: fill them with `hle::StubFor("JNIEnv::slot_123")` without registering a handler → a warning with the
  name and LR the first time, returns 0.
- Per-thread `JNIEnv`: `cpu.user(UserSlot::Jni)`; cleanup in `threads::AddExitHook`.
- Fake Java → native: `zw::call_guest<R>(zw::loader::FindSymbol("Java_…"), env, clazz, args…)` (reentrant, §3).
- OpenSL/AudioTrack callbacks: do **not** call `call_guest` from SDL's audio callback on SDL's thread if you can avoid
  it; create a guest thread (`threads::Create` with your own stub as entry, or a host `std::thread` that uses
  `call_guest`: it attaches automatically and must call `threads::DetachCurrent()` when done).

---

## 8. Debugging (`zw::debug`, `cpu/cpu.h`)

```cpp
std::string         debug::addr2sym(gaddr);
std::vector<gaddr>  debug::Backtrace(const Cpu&, size_t max = 32);   // heuristic (stack scan validating BL/BLX)
std::string         debug::DumpState(const Cpu&);                    // registers + symbolized backtrace
void                debug::SetTraceImports(bool);
```

On a guest fault (unmapped memory, undefined instruction, `bkpt`) the runtime writes `DumpState` to the log and throws
`GuestFault`. On a host access violation inside a handler (invalid guest pointer) it writes the import in progress, the
guest address involved and `DumpState`, and terminates the process.

Environment variables: `ZW_TRACE_IMPORTS`, `ZW_TRACE_FILTER`, `ZW_ABORT_ON_UNIMPLEMENTED`, `ZW_TRACE_INIT=1` (one line per
`.init_array` constructor), `ZW_JIT_CACHE_MB` (per-thread code cache, 64 by default), `ZW_DEBUG_VEH=1` (dumps every
first-chance host exception), `ZW_FASTMEM=0` / `ZW_FASTMEM_EXCLUSIVE=0` (see below).

**JIT memory access.** By default dynarmic uses *fastmem* (`host = base + guest`, host page faults become `GuestFault`) and
`LDREX/STREX` via `cmpxchg`. `ZW_FASTMEM=0` switches to a page table + the global exclusive monitor (slower, no host
exceptions; useful for debugging); `ZW_FASTMEM_EXCLUSIVE=0` keeps fastmem but routes exclusives through the monitor.
Fastmem needs a local patch to dynarmic that `cmake/ZwDependencies.cmake` applies on every configure: its Windows exception
handler reserved 8 bytes of stack instead of the x64 ABI's *shadow space* (`sub rsp, 8` → `sub rsp, 0x28`), which with MSVC
corrupted the return address on the first fastmem fault.
Measured in win-debug: a 2-thread atomic loop (4 M contended `LDREX/STREX`) 8.2–8.5 s with the page table versus
1.6–3.5 s with fastmem; the 552 constructors of `--load-only` do not change (6–8 s in both modes: compiling the JIT in an
unoptimized dynarmic dominates). (Later, dynarmic was built optimized in Debug too: 2.0 s in debug, 0.22 s in release.)

---

## 9. Reference startup (T8)

```cpp
zw::GuestMemory::Init();
zw::hle::libc::Register();  zw::hle::gles::Register();  zw::hle::audio::Register();  zw::jni::Register();
auto* m = zw::loader::Load(so_path);
try {
  zw::loader::RunInit(*m);
  zw::loader::CallJniOnLoad(*m, zw::jni::JavaVM());
  // ... ANALYSIS.md §4 sequence with call_guest ...
} catch (const zw::GuestFault& f) { zw::log::Error("{}", f.what()); }
  catch (const zw::GuestExit& e)  { /* exit with e.code */ }
```

---

## 10. `--load-only` and minimal startup HLE (`loader/load_only.h`)

```
zombiewood --load-only <path to .so> [more .so...]        (or the standalone zw_loadonly executable, same arguments)
```

Loads the libs in order, prints segments/relocations/imports (with the list of those without a handler), runs
`.init_array` and `JNI_OnLoad` with a fake `JavaVM`, and summarizes which handler-less imports actually got called. Exit
code 0 if it reached the end. Useful for T4–T7: by linking your `Register()` before `RunLoadOnly`, the "without handler"
list shrinks.

- `loader::bootstrap::Register()`: ~100 minimal bionic imports (malloc, `mem*`/`str*`, basic `printf`, `pthread_key_*`,
  `__cxa_*`, time, libm, `_ctype_` tables…) **only for names nobody else has registered**. It is T3's scaffolding for
  `--load-only` and tests; it does not replace T4 (mutex/cond are no-ops, no stdio). T8 must not call it.
- `loader::bootstrap::FakeJavaVM()`: a guest `JavaVM*` with `GetEnv/AttachCurrentThread` and a `JNIEnv` with 232 generic slots
  that log every call (`[jni falso] … FindClass(…)`) and return dummy handles. T7 replaces it.

State measured with only the minimal HLE (win-debug): `libZombieWood.so` loads at `0x10000000` (56,376 RELATIVE, 6,467 ABS32,
18 GLOB_DAT, 356 JUMP_SLOT), the 552 constructors run without faults and `JNI_OnLoad` returns `0x10004`
(`AndroidOS::Init` finds `SUtils`, `Device` and `GameInstaller`). During that startup the `.so` only calls three imports
without a handler: `wctob`, `btowc` and `wctype` (gnustl's `std::locale`/`ctype<wchar_t>` initialization).

---

## Audio (T6) — `hle/audio/audio_track.h`, `hle/audio/audio_hle.h`

A pure C++ (no JNI) backend for `android.media.AudioTrack` (STREAM mode) on SDL2; T7 wraps it as an emulated Java class.
CMake: `target_link_libraries(x PRIVATE zw_hle_audio)`; `#include "audio_track.h"` (namespace `zw::audio`).

```cpp
int  audio::native_output_sample_rate();                               // 44100
int  audio::min_buffer_size(int rate, int channelCfg, int encoding);   // bytes, >= 8192

audio::Track t(streamType, rate, channelCfg, encoding, bufferBytes, mode = kModeStream);
// channelCfg: kChannelOutMono=4 / kChannelOutStereo=12 (0xC); encoding: kEncodingPcm16=2 / kEncodingPcm8=3
t.play(); t.pause(); t.stop() /*discards the queue*/; t.release() /*wakes writers; idempotent*/;
int n = t.write(const void* pcm, int bytes);  // bytes accepted; -1 if released
int s = t.play_state();                        // kStateStopped=1, kStatePaused=2, kStatePlaying=3
```

- `write` copies into a queue and **blocks** while more than ~100 ms is queued and the track is `PLAYING` (and not globally
  paused). Paused/stopped/released **never** blocks (accepts up to ~200 ms and returns what it accepted). `release()`
  unblocks a writer.
- SDL opens one device per track (in the track's native format; SDL converts rate/channels/bits). If there is no device,
  a thread consumes in real time (the game never blocks). An *underrun* outputs silence.
- Global control (T9's menu, `nativeVoxMute`, minimizing): `audio::set_master_volume(0..1)`, `set_muted(bool)`,
  `set_global_paused(bool)` (pauses/resumes every track without changing its `play_state`). Applied in the callback, so it
  also affects data already queued.
- (0.2.2) `audio::set_latency_ms(ms)` (before tracks are created; the frontend maps `audio_latency` to 80/150/250 ms) sets the
  queue size; the device block is 1024 frames below 120 ms, 2048 otherwise. After a dropout the track re-primes (outputs
  silence until half the queue is filled, or until the writer has been idle for 60 ms). `audio::underrun_count()` counts dropouts.
- Tests/CI: `audio::set_driver("dummy")` before creating the first track; `audio::set_tap(fn)` observes what goes to the device.
- **OpenSL ES** (`zw::hle::audio::Register()`, `audio_hle.h`; T8 calls it before `loader::Load`): defines `SL_IID_ENGINE`,
  `SL_IID_PLAY`, `SL_IID_BUFFERQUEUE` (pointer variables to 16-byte `SLInterfaceID_` with the real GUIDs) and registers
  `slCreateEngine`, which returns `SL_RESULT_FEATURE_UNSUPPORTED` (12) and sets `*pEngine = NULL`. The game uses
  `AudioTrack` (`s_androidAPILevel = 3`), so OpenSL is a dead path.

## JNI (T7) — `jni/jni.h`, `jni/java.h`

`target_link_libraries(... zw_jni)` (pulls in `zw_cpu` + `zw_loader`). `zw::jni::Register()` is idempotent; call it after
`GuestMemory::Init()` and before `loader::Load()`.

| API | Use |
|---|---|
| `jni::JavaVM()` / `jni::Env()` | the guest `JavaVM*` (for `CallJniOnLoad`) / the current thread's `JNIEnv*`. `GetEnv` and `AttachCurrentThread` always return 0 + a per-thread `JNIEnv`; `DetachCurrentThread` frees the thread's local refs |
| `jni::call_native<R>(name, args…)` | calls `Java_…` with `(JNIEnv*, jclass, args…)` on the current thread (reentrant). `name`: the full symbol, relative to the package (`"Game_nativeInit"`, `"GLUtils_SUtils_nativeInit"`) or `"class/x.method"` if it came from `RegisterNatives`. `std::string`/`const char*` → `jstring`. Its own local frame (refs created are freed on return). Throws `std::runtime_error` if it does not exist; `GuestFault/GuestExit` propagate |
| `jni::call_native_obj<R>(name, self, …)` | the same for instance natives |
| `jni::config()` | the "device" values (guest paths, OBB, model, GPU `"Adreno (TM) 330"`, language, 1280×720 screen, `prefs_file` to persist `SUtils.nativeSet/GetPreference`, `assets_dir`). Change them **before** `Game.nativeInit` (it copies the strings) |
| `jni::hooks()` | callbacks into the frontend: `create_view` (by default calls `GameRenderer.nativeInit(0, screen_w, screen_h, "1.5.3")`), `set_view_settings`, `set_current_context` (default `true`), `exit` (default `throw GuestExit{0}`), `play_cinematic` (default no-op = video skipped), `open_browser`, `keep_screen_on`, `send_to_background`, `show_screen` |
| `jni::SetAudioTrackBackend(factory)` | `android/media/AudioTrack` delegates to a `jni::AudioTrackBackend` (same shape as T6's `zw::audio::Track`). Default: a null backend that consumes in real time |
| `jni::Coverage()` / `CoverageReport()` | every `FindClass`/`Get*ID` the game requested and whether it is emulated; methods called without an implementation (logged once as `[jni] metodo NO implementado: Class.method(signature)`, returning 0/null) |
| `jni::DefineClass(name).Static/Virtual/Ctor/StaticField/InstanceField(...)` (`java.h`) | add or replace emulated Java methods from another module |

T6 → T7 adapter (T8, ~10 lines):

```cpp
struct SdlTrack final : zw::jni::AudioTrackBackend {
  zw::audio::Track t;
  SdlTrack(int s, int r, int c, int e, int b, int m) : t(s, r, c, e, b, m) {}
  void Play() override { t.play(); }   void Pause() override { t.pause(); }
  void Stop() override { t.stop(); }   void Release() override { t.release(); }
  int Write(const void* p, int n) override { return t.write(p, n); }
  int PlayState() const override { return t.play_state(); }
};
zw::jni::SetAudioTrackBackend([](int s, int r, int c, int e, int b, int m) { return std::make_unique<SdlTrack>(s, r, c, e, b, m); });
```

Model: `jobject` = handle `(index << 2) | kind` (1 local, 2 global, 3 weak = `GetObjectRefType`); `jmethodID`/`jfieldID` =
opaque values in `0x0A…`/`0x0B…` (the null zone: if the guest dereferences them, it fails visibly). Primitive arrays live in
guest memory (`Get*ArrayElements`/`GetPrimitiveArrayCritical` return the direct pointer, `isCopy = 0`). No Java exceptions:
`ExceptionCheck/Occurred` = 0, `Throw*` is logged and ignored. Unknown class or method: `FindClass`/`Get*ID` never return
0 with a valid `jclass` (ANALYSIS R9).

Diagnostics: `zw_jni_loadonly <libZombieWood.so> [--no-extra] [--trace-calls]` runs `.init_array`, `JNI_OnLoad`,
`nativeSetGPUInfo`, `GL2JNILib.init` (→ `createView` → `GameRenderer.nativeInit`), `Game.nativeInit` and the optional
`nativeInit`s with the real JNI, and lists the coverage.

**Pending for T8 (the frontend's `--load-only`):** `loader::RunLoadOnly` uses T3's `bootstrap::FakeJavaVM()`. For
`zombiewood --load-only` to use T7's JNI it is enough to (a) add a `gaddr java_vm = 0` parameter to `RunLoadOnly` and use
`java_vm ? java_vm : bootstrap::FakeJavaVM()`, and (b) in `main.cpp`: `zw::jni::Register(); return zw::loader::RunLoadOnly(libs,
zw::jni::JavaVM());`. (Done in T8, see below.)

## Frontend / integration (T8) — `frontend/game.h`, `frontend/input.h`

- `zw::RunGame(opts)`: UI (main) thread = SDL + steps 0–7 of ANALYSIS §4; GL thread = GLES context, steps 8–13 and the
  `GL2JNILib.step` loop + presentation. All input is injected between frames from the GL thread (`Game.nativeOnTouch`,
  `nativeOnKeyDown/Up`). Minimize → `GL2JNILib.pause` + `nativeVoxSuspend` + `audio::set_global_paused(true)`; restore → the
  reverse. Closing the window → pause (the game saves) and `_Exit` (guest threads remain alive: there is no orderly teardown).
- **Added to T5**: `hle::gles::SetDefaultFramebuffer(fbo)`: the guest's framebuffer 0 is a frontend FBO (RGBA8 +
  D24S8 at the internal resolution) copied to the window with `glBlitFramebuffer` (letterbox). `glBindFramebuffer(_, 0)` binds
  that FBO, and `GL_FRAMEBUFFER_BINDING` returns 0 while it is bound.
- **Added to T3**: `loader::RunLoadOnly(libs, java_vm = 0)`; `zombiewood --load-only` registers the real HLE and uses `jni::JavaVM()`.
- `jni::config()`: `screen_w/h` = internal resolution, `prefs_file` = `<saves>/zw_prefs.json`, `assets_dir`, `language`;
  `patch_obb` is cleared if the file does not exist. Hooks: `send_to_background` minimizes the window; `exit` keeps the
  default (`GuestExit`, caught by the GL thread); `play_cinematic` left unhooked (videos skipped → T9).
- `input::Mapper`: pointer 0 = mouse, 1 = move stick, 2 = aim stick, 3+ = HUD buttons, 10+ = real fingers.
  HUD measurements at 1280×720: sticks at (170,590) and (1108,590), radius ≈ 82 px; they are fixed (not floating).
- Measured (win-release, RTX 3060 Laptop, 1280×720): 60 fps with vsync (step 2–9 ms); unlimited ≈ 490–520 fps in menus and
  130–210 fps in matches. The game uses real time: it does not depend on a fixed 30/60.

## Frontend (T9) — installer, config, menu, videos, saves, logs

- `installer::Install(src, dest, progress)` (`src/installer/installer.h`): reads the APK as a stream (zlib, no large
  temporary files; `.part` files are renamed at the end), walks `assets/archiveObbCache.zip.00N` as a ZIP concatenated by
  local headers, or copies loose OBBs. Writes `dest/{lib,obb,res/raw,assets}` + `install.json`. The 1.5.3 SHA-256 hashes are
  in `installer.h` (`kShaZombieWood`, main/patch OBB): a mismatch is a **warning**, not an error. A missing main OBB = error.
- `<home>/data` = the default `game_dir` and also `vfs.data_dir` (= `/sdcard/.../files`): the names `lib/ obb/ res/ assets/`
  do not clash with the files the game writes.
- `Settings` (`frontend/settings.h`) is the model of `zombiewood.json`; `MergeInto` keeps unknown keys.
  `backend` → `ANGLE_DEFAULT_PLATFORM` before creating the window (ANGLE reads it with `GetEnvironmentVariable`).
- `jni::hooks().play_cinematic` = `video::Play` (Media Foundation) on the GL thread, **inside** `GL2JNILib.step`: pauses Vox's
  audio (`audio::set_global_paused`), presents to framebuffer 0 and restores the relevant GL state; when done it calls the
  native `GLUtils_SUtils_onVideoFinished` (empty). `logo_hd.mp4` plays before step 8.
- ImGui menu: its own context on the GL thread; SDL events travel raw in `Ev::raw` and the menu can keep them
  (`Menu::ProcessEvent`); "up" events always reach the game. It is drawn after the internal FBO blit, before the swap
  (ImGui's OpenGL3 backend saves and restores GL state, including `GL_ELEMENT_ARRAY_BUFFER` and the attributes).
- Saves: `saves::Backup` watches `<home>/saves` (polling every 2 s + before `GL2JNILib.pause` and on exit) and stores the
  previous version of each overwritten file in `<home>/saves_backup/<date>/` (it does not touch the libc HLE).
- `log`: rotation (`zombiewood.log` + `.1`…`.4`, and live past 32 MiB) and suppression of repeated Info/Debug lines
  (>3 in 30 s → summary `(xN ...)`); `log::Shutdown()` before `_Exit`. `ZW_LOG_NODEDUP=1` disables it.

## Frontend (0.2.0) — cursor

- `frontend/cursor.h`: `cursor::Apply(mode, size)` on the UI thread (`"zombie"` | `"system"`; the closest embedded size
  32/48/64/96). Data: `RCDATA 102` in the exe (`resources/cursor.rc`, generated by `tools/make_cursor.ps1`; format
  `"ZWCU"`, version, count, `{size, hot_x, hot_y, offset}` + BGRA pixels). The F1 menu posts a `kUserCursor` SDL user event
  carrying the size (0 = system) so the GL thread never touches SDL's cursor directly.
