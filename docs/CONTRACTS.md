# Zombiewood — contratos entre módulos (T3)

API pública del núcleo (`zw_cpu` + `zw_loader`) que usan T4–T8. Las cabeceras son la fuente de verdad:

| Cabecera | Contenido |
|---|---|
| `cpu/guest_memory.h` | `gaddr`, `g2h/h2g`, `GuestMemory` (páginas + heap), `mem::` (lectura/escritura), `GPtr<T>` |
| `cpu/cpu.h` | `Cpu` (registros, datos por hilo), `call_guest`, `threads::`, `cpu::`, `debug::`, `GuestFault`, `GuestExit`, `gi64/gu64` |
| `cpu/hle.h` | `ZW_HLE*`, `hle::Register/StubFor/DefineData…`, `VaList`, `Varargs` |
| `loader/loader.h` | `loader::Load/RunInit/CallJniOnLoad/FindSymbol/addr2sym`, `Module` |

CMake de un módulo consumidor: `target_link_libraries(zw_hle_xxx PUBLIC zw_cpu)` (+ `zw_loader` si necesita símbolos) e
`#include "cpu/hle.h"`. No hace falta incluir nada de dynarmic.

**Estabilidad:** a partir de este commit solo habrá cambios compatibles (añadidos). Cambios incompatibles → vía coordinador.

Todo vive en el espacio de nombres `zw`. ABI del guest (T1): ARMv7-A, Thumb-2, VFPv3, **softfp**, little-endian, `wchar_t` de 32 bits.

---

## 0. Reglas de oro

1. **Nunca uses tipos del host cuyo tamaño difiere del guest en una firma HLE**: `size_t`, `long long`, `intptr_t`, `time_t`,
   `off_t`, `wchar_t`, `long double`. Usa `zw::gsize` (= `uint32_t`), `int32_t`, `zw::gaddr`, `zw::gi64/gu64` (long long del
   guest), `uint32_t` para `wchar_t`. Un entero de 64 bits "desnudo" como parámetro **no compila** (a propósito).
2. **Los structs que ve el guest se declaran con campos de ancho fijo** (`uint32_t`, `GPtr<T>`), nunca con punteros host.
3. **Los handlers corren en paralelo** en varios hilos host (un hilo host por hilo guest, sin *big lock*). Todo estado global
   de un módulo HLE debe ser thread-safe. Se puede bloquear libremente en un handler normal (no `FAST`).
4. **Registro explícito, no estático**: cada módulo expone una función `Register()` y dentro usa `ZW_HLE(...)`. (Un
   inicializador estático en una `.lib` sin otras referencias lo descarta el enlazador de MSVC.) Convención de nombres:
   `zw::hle::libc::Register()`, `zw::hle::gles::Register()`, `zw::hle::audio::Register()`, `zw::jni::Register()`; T8 las
   llama **antes** de `loader::Load()`.
5. Toda dirección de función guest lleva el **bit 0 = Thumb**. No lo quites ni lo pongas a mano: usa las direcciones tal como
   las dan `FindExport/FindSymbol`, `.init_array` o el propio guest.

---

## 1. Memoria guest (`cpu/guest_memory.h`)

Se reservan 4 GiB en el host: `host = zw::g_membase + guest`. Traducir es sumar; no hay tabla de páginas software.

```cpp
using gaddr = uint32_t;                 // dirección guest, 0 = NULL
template <class T = void> T* g2h(gaddr a);   // 0 -> nullptr
gaddr h2g(const void* p);                    // nullptr -> 0; error en log (y 0) si p no es memoria guest
```

### 1.1 Mapa de direcciones

| Zona | Rango | Uso |
|---|---|---|
| (nulo) | `0x00000000–0x0EFFFFFF` | nunca mapeado (atrapa NULL y offsets pequeños) |
| `Zone::Thunks` | `0x0F000000–0x0FFFFFFF` | stubs de imports (`svc #n; bx lr`), stub de retorno de `call_guest` |
| `Zone::Libs` | `0x10000000–0x1FFFFFFF` | `.so` cargados. El primero queda en `0x10000000` ⇒ `guest = 0x10000000 + vaddr ELF` de ANALYSIS.md |
| `Zone::Heap` | `0x20000000–0x7FFFFFFF` | heap (`Alloc`) y `MapAnywhere(Zone::Heap)` |
| `Zone::Stacks` | `0x80000000–0x8FFFFFFF` | pilas de hilos (con página de guarda) y bloques TLS |
| kuser | `0xFFFF0000–0xFFFF0FFF` | *kuser helpers* de Linux ARM (`__kuser_cmpxchg` `0xffff0fc0`, `__kuser_memory_barrier` `0xffff0fa0`, `__kuser_cmpxchg64` `0xffff0f60`, `__kuser_get_tls` `0xffff0fe0`), implementados en código guest con `LDREX/STREX`. El `.so` los llama por dirección fija (atómicos de protobuf); no son imports |

Tocar desde el host una dirección guest no mapeada = violación de acceso; el runtime la vuelca con el estado guest y aborta.
Un acceso **del guest** a memoria no mapeada acaba en `GuestFault` con volcado, no en un crash.
Si el puntero viene del guest y puede ser basura, valida con `GuestMemory::IsMapped(addr, size)`.

### 1.2 Páginas

```cpp
bool  GuestMemory::Init();                                   // T8 lo llama al arrancar (idempotente)
bool  GuestMemory::Map(gaddr addr, gsize size, Prot prot, std::string_view tag = {});
gaddr GuestMemory::MapAnywhere(Zone z, gsize size, Prot prot, std::string_view tag = {}, gsize align = 4096);
bool  GuestMemory::Unmap(gaddr addr, gsize size);
bool  GuestMemory::Protect(gaddr addr, gsize size, Prot prot);
bool  GuestMemory::IsMapped(gaddr addr, gsize size = 1);
```

`Prot`: `kProtRead|kProtWrite|kProtExec` (`kProtRW`, `kProtRX`…). Las páginas nuevas están a cero. El texto de los `.so` se
mapea **solo lectura** (escribir en él desde el guest es un fallo; para parchear usa `hle::HookGuestFunction` o
`Protect` + `cpu::InvalidateCode`).

### 1.3 Heap (para `malloc` de T4 y para objetos que la HLE entrega al guest)

```cpp
gaddr GuestMemory::Alloc(gsize size);             // alineado a 8; Alloc(0) devuelve un bloque válido; 0 = sin memoria
gaddr GuestMemory::Calloc(gsize n, gsize size);   // a cero
gaddr GuestMemory::Realloc(gaddr p, gsize size);  // Realloc(0, n) == Alloc(n); conserva el contenido
gaddr GuestMemory::AllocAligned(gsize size, gsize align);  // memalign (se libera con Free)
void  GuestMemory::Free(gaddr p);                 // Free(0) no-op; doble free -> error en log, no revienta
gsize GuestMemory::UsableSize(gaddr p);
```

Thread-safe. El contenido de `Alloc` **no** está a cero. T4: `malloc/free/calloc/realloc/strdup` son envoltorios directos.

### 1.4 Lectura/escritura

```cpp
uint32_t v = zw::mem::R32(addr);   zw::mem::W32(addr, v);      // R8/R16/R32/R64, W8/W16/W32/W64
auto st = zw::mem::Read<MyGuestStruct>(addr);   zw::mem::Write<MyGuestStruct>(addr, st);
std::string s = zw::mem::ReadCStr(addr);                       // "" si addr == 0
zw::mem::WriteCStr(dst, "texto", cap);                         // trunca, siempre termina en '\0'
gaddr p = zw::mem::StrDup("hola");                             // en el heap guest (Free)
gaddr q = zw::mem::Dup(host_ptr, n);
```

`GPtr<T>`: puntero guest tipado de 4 bytes para campos de structs y parámetros:

```cpp
struct GuestIovec { zw::GPtr<uint8_t> base; uint32_t len; };   // sizeof == 8, igual que en el guest
auto* io = zw::g2h<GuestIovec>(addr);
memcpy(dst, io->base.get(), io->len);
```

---

## 2. Imports HLE (`cpu/hle.h`)

Cada import de función del `.so` se resuelve a un **stub guest** en `Zone::Thunks` (ARM: `svc #n; bx lr`). El `SVC` despacha al
handler registrado con ese nombre. El mismo stub sirve a la PLT, a `R_ARM_ABS32/GLOB_DAT` (punteros a `malloc`, etc.) y a
cualquier puntero a función que la HLE entregue al guest.

### 2.1 Registrar

```cpp
#include "cpu/hle.h"
namespace {
int      hle_abs(int x)                               { return x < 0 ? -x : x; }
zw::gsize hle_strlen(const char* s)                    { return (zw::gsize)std::strlen(s); }
void*    hle_memcpy(void* d, const void* s, zw::gsize n) { return std::memcpy(d, s, n); }
double   hle_pow(double a, double b)                  { return std::pow(a, b); }
float    hle_sqrtf(float x)                           { return std::sqrt(x); }
zw::gaddr hle_malloc(zw::gsize n)                      { return zw::GuestMemory::Alloc(n); }
int      hle_errno_ptr_demo(zw::Cpu& cpu)             { return (int)cpu.errno_addr(); }   // Cpu& no consume argumentos
}
void zw::hle::libc::Register() {
  ZW_HLE("abs", hle_abs);
  ZW_HLE_FAST("strlen", hle_strlen);     // FAST: dentro del JIT, ver 2.4
  ZW_HLE_FAST("memcpy", hle_memcpy);
  ZW_HLE_FAST("pow", hle_pow);
  ZW_HLE_FAST("sqrtf", hle_sqrtf);
  ZW_HLE("malloc", hle_malloc);
}
```

- `ZW_HLE(nombre, fn)` / `ZW_HLE_FAST(nombre, fn)`: `fn` es una función libre/estática **no sobrecargada** (para `std::sin`
  etc. escribe un envoltorio). El marshalling se deduce de la firma.
- `ZW_HLE_RAW(nombre, void(*)(zw::Cpu&))`: handler crudo; lee `cpu.reg(0..3)`, `cpu.arg_word(i)` y escribe `cpu.reg(0)`.
- `ZW_HLE_THUNK(nombre, fn)` → `gaddr`: registra y devuelve la dirección del stub (para vtables, ver §7).
- `hle::Register(nombre, raw, flags)`: forma no-macro. Registrar el mismo nombre otra vez **sustituye** al anterior (el
  último gana); se puede registrar antes o después de cargar el `.so`.
- `hle::StubFor(nombre)` → dirección guest del stub (la crea si no existía; estable).

### 2.2 Marshalling automático (AAPCS softfp, confirmado por T1)

Los argumentos son "palabras" de 4 bytes: palabras 0–3 = `r0–r3`, palabra 4+ = `[sp + 4·(i−4)]`.

| Tipo C++ del parámetro | Consume | Notas |
|---|---|---|
| `bool`, enteros y enums ≤ 32 bits, `zw::gaddr`, `zw::gsize` | 1 palabra | se trunca al tipo (`char`, `short`…) |
| `float` | 1 palabra | bits crudos en registro núcleo/pila (softfp) |
| `double`, `zw::gi64`, `zw::gu64` | 2 palabras, **índice par** | `r0:r1` o `r2:r3` (si tocaba `r1`/`r3` se salta) o pila alineada a 8 |
| `T*` (puntero host) | 1 palabra | traducido con `g2h` (0 → `nullptr`). **No** se valida que esté mapeado |
| `zw::GPtr<T>` | 1 palabra | sin traducir; `.addr`, `.get()` |
| `zw::VaList` | 1 palabra | un `va_list` del guest (`vprintf(fmt, ap)`) |
| `zw::Varargs` | — (debe ser el último) | el `...` de la función: lee lo que sigue a los parámetros con nombre |
| `zw::Cpu&` | nada | acceso al hilo guest actual; puede ir en cualquier posición |
| `int64_t`, `uint64_t`, `size_t`, `long long` | **error de compilación** | ambiguo: usa `gi64/gu64` o `gsize` |

| Tipo de retorno | Dónde |
|---|---|
| `void` | — |
| `bool`, enteros/enums ≤ 32 bits | `r0` (con extensión de signo/cero) |
| `float` | `r0` (bits) |
| `double`, `gi64`, `gu64`, enteros de 64 bits | `r0:r1` (devolver `size_t` del host es válido: `r1` es *caller-saved*) |
| `T*` | `r0 = h2g(p)` (debe ser memoria guest o `nullptr`) |
| `GPtr<T>` | `r0` |

No soportado automáticamente: structs por valor (p. ej. `div_t div(int,int)`: el guest pasa un puntero oculto de resultado en
`r0`; declara `void hle_div(GPtr<DivT> out, int a, int b)`), y `long double` (= `double` en ARM).

### 2.3 Variádicas y `va_list`

```cpp
// Formateador común del módulo: recibe un lector de argumentos.
int do_vsnprintf(char* dst, zw::gsize cap, const char* fmt, zw::VaList& ap);

int hle_snprintf(char* dst, zw::gsize cap, const char* fmt, zw::Varargs va) { return do_vsnprintf(dst, cap, fmt, va); }
int hle_vsnprintf(char* dst, zw::gsize cap, const char* fmt, zw::VaList ap)  { return do_vsnprintf(dst, cap, fmt, ap); }

// Dentro del formateador:
int         i = ap.Next<int>();             // %d %c %x  (char/short ya vienen promocionados a int)
double      d = ap.Next<double>();          // %f %g     (float promocionado; Next<float>() no compila)
zw::gi64    q = ap.Next<zw::gi64>();        // %lld      (alineado a 8 automáticamente)
const char* s = ap.Next<const char*>();     // %s        (traducido a host; nullptr si 0)
zw::gaddr   p = ap.Next<zw::gaddr>();       // %p / %n
```

`VaList::FromGuest(gaddr ap)` y `VaList::FromCall(cpu, primera_palabra)` construyen lectores a mano (p. ej. en un handler
crudo de `CallStaticVoidMethod(env, cls, mid, ...)`: `VaList::FromCall(cpu, 3)`; en `CallStaticVoidMethodV(env, cls, mid, va_list)`:
parámetro `zw::VaList`). Para `Call…MethodA(…, const jvalue* args)` lee `mem::R64(args + 8*i)`.

### 2.4 Handlers `FAST` frente a normales

- **Normal** (`ZW_HLE`): el JIT se detiene en el `svc`, el handler corre fuera de `Jit::Run`. Puede llamar a `call_guest`,
  bloquear, lanzar `GuestExit`/`GuestFault`, crear hilos.
- **FAST** (`ZW_HLE_FAST`): corre dentro del callback del JIT (evita salir y volver a entrar en el JIT en cada llamada). **Prohibido**:
  `call_guest`, lanzar excepciones C++, bloquear indefinidamente, tocar `pc/sp/lr`. Úsalo para `mem*`, `str*`, `libm`, y casi
  todo `gl*`. En caso de duda, normal.

### 2.5 Datos importados

Los 8 imports de datos (`__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `SL_IID_ENGINE`,
`SL_IID_BUFFERQUEUE`, `SL_IID_PLAY`) tienen `st_size = 0` en el `.so`: el tamaño lo decide el módulo HLE dueño.

```cpp
void zw::hle::libc::Register() {
  zw::gaddr guard = zw::hle::DefineData("__stack_chk_guard", 4);     // bloque guest a cero, estable
  zw::mem::W32(guard, 0x5A5A1234);
  zw::gaddr sF = zw::hle::DefineData("__sF", 3 * 84);                // FILE __sF[3]
  zw::gaddr tab = zw::GuestMemory::Alloc(257);                       // tabla
  zw::gaddr ctype = zw::hle::DefineData("_ctype_", 4);               // `const char* _ctype_` (variable puntero)
  zw::mem::W32(ctype, tab);
}
```

**Deben definirse antes de `loader::Load()`** (el GOT se rellena en la carga). Un dato sin definir se resuelve a un bloque
de 1024 bytes a cero con un aviso en el log (`Via::HleDataPlaceholder`); `hle::FindData(nombre)` lo devuelve igualmente.
Dueños: `__sF`, `__stack_chk_guard`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_` → T4; `SL_IID_*` → T6.

### 2.6 Imports sin handler, traza y estadísticas

- Import sin handler: la **primera** llamada escribe `import sin handler: <nombre> (lr = <símbolo>)` y devuelve 0 (`r0 = 0`).
  Con `ZW_ABORT_ON_UNIMPLEMENTED=1` o `hle::SetUnimplementedPolicy(OnUnimplemented::Abort)` lanza `GuestFault`.
- `hle::UnimplementedImports()` lista los stubs pedidos sin handler (lo usa `--load-only`).
- `ZW_TRACE_IMPORTS=1` (o `debug::SetTraceImports(true)`): una línea por llamada (`hilo nombre(r0, r1, r2, r3) lr=…` y el
  retorno). `ZW_TRACE_FILTER=subcadena` filtra por nombre.
- `hle::CallCounts()`: contador de llamadas por import.

### 2.7 Ya implementados por T3 (no los registres)

`setjmp`, `_setjmp`, `sigsetjmp`, `longjmp`, `_longjmp`, `siglongjmp` (sobre el estado del CPU guest: r4–r11, sp, lr, d8–d15,
fpscr; `jmp_buf` de 256 bytes de bionic sobra; `longjmp` a través de fronteras `call_guest` funciona) y
`__gnu_Unwind_Find_exidx` (lo registra el loader con las tablas `.ARM.exidx` de cada módulo). Si un módulo registra uno de
estos nombres, sustituye al de T3.

### 2.8 Sustituir funciones internas del `.so`

```cpp
static int my_CheckDiskSpace() { return 1; }
ZW_HLE("zw_CheckDiskSpace", my_CheckDiskSpace);
zw::hle::HookGuestFunction(zw::loader::FindSymbol("_Z14CheckDiskSpacev"), "zw_CheckDiskSpace");
```

Parchea la entrada de la función con un salto al stub (hacerlo después de `Load`, antes de ejecutar ese código).

---

## 3. Host → guest: `call_guest` (`cpu/cpu.h`)

```cpp
template <class R = uint32_t, class... A> R zw::call_guest(gaddr fn, const A&... args);
```

- Ejecuta `fn` en el `Cpu` del hilo host actual y vuelve cuando la función retorna (LR mágico → stub de retorno).
- **Reentrante**: se puede llamar desde un handler normal (qsort → comparador, `pthread_once`, JNI `Call…Method` → Java falso
  → `Java_…` nativo → …). Conserva todos los registros núcleo del llamador guest.
- Argumentos: mismas reglas que §2.2 en sentido inverso — enteros ≤ 32 bits/`gaddr`/enum/`bool` (1 palabra), `float`
  (1 palabra), `double`/`gi64`/`gu64` (2 palabras, índice par), `T*` (se convierte con `h2g`: **debe** apuntar a memoria
  guest), `GPtr<T>`, `nullptr`. Un `int64_t`/`size_t` desnudo no compila.
- `R`: `void`, enteros, `bool`, `float`, `double`, `gi64/gu64/int64_t/uint64_t`, `T*` (traducido con `g2h`), `GPtr<T>`.
- Desde un hilo host que nunca ha ejecutado guest, la primera llamada lo "adjunta" (crea su `Cpu` con pila de 1 MiB).
- Errores: `zw::GuestFault` (el guest falló; `what()` trae registros + backtrace) y `zw::GuestExit{code}` (alguien hizo
  `exit`) se propagan como excepciones C++ hasta quien llamó a `call_guest` en el nivel más externo.

```cpp
// qsort (T4)
void hle_qsort(zw::gaddr base, zw::gsize n, zw::gsize sz, zw::gaddr cmp) {
  std::vector<uint8_t> tmp(sz);
  // ... algoritmo host; para comparar:
  int r = zw::call_guest<int>(cmp, base + i * sz, base + j * sz);
}
// Llamar a un nativo JNI (T7/T8)
zw::gaddr fn = zw::loader::FindSymbol("Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_resize");
zw::call_guest<void>(fn, env, clazz, 1280, 720);
zw::call_guest<void>(nativeAccel, env, clazz, 0.0f, 9.8f, 0.0f);     // floats: softfp automático
```

Cadenas/arrays para el guest: primero cópialos a memoria guest (`mem::StrDup`, `GuestMemory::Alloc`) y pasa el `gaddr`.

---

## 4. `Cpu` y datos por hilo (`cpu/cpu.h`)

```cpp
zw::Cpu& cpu = zw::Cpu::Current();      // el del hilo host actual (lo crea si hace falta)
cpu.reg(0..15), cpu.sp(), cpu.lr(), cpu.pc(), cpu.cpsr(), cpu.fpscr(), cpu.d(i)/set_d(i, bits), cpu.s(i)/set_s
cpu.arg_word(i)        // palabra AAPCS i de la llamada a import en curso
cpu.thread_id()        // uint32 != 0, único entre hilos vivos (sirve de pthread_t / gettid)
cpu.errno_addr()       // gaddr de un int por hilo -> `int* __errno()` devuelve esto
cpu.tls()              // bloque TLS bionic del hilo (64 palabras; TPIDRURO lo devuelve). [0] = self, [1] = thread_id
cpu.user(zw::UserSlot::Libc)   // void*& por hilo para cada módulo (Libc, Pthread, Gles, Audio, Jni, Frontend, Spare0/1)
cpu.stack_base(), cpu.stack_size(), cpu.call_depth(), cpu.current_import()
```

```cpp
zw::gaddr hle_errno(zw::Cpu& cpu) { return cpu.errno_addr(); }          // ZW_HLE_FAST("__errno", hle_errno)
```

`user(slot)` empieza en `nullptr`; libéralo en un *exit hook* (§5). El registro CP15 `TPIDRURO` devuelve `cpu.tls()` (T1: el
juego no lo usa; el resto de CP15 lee 0).

---

## 5. Hilos (`zw::threads`, `cpu/cpu.h`)

Modelo: **un hilo host + un `Dynarmic::A32::Jit` por hilo guest**, concurrencia real, monitor exclusivo global para
`LDREX/STREX`. No hay *big lock*: no hay nada que soltar en llamadas bloqueantes.

```cpp
struct CreateParams { gaddr entry; gaddr arg; gsize stack_size = 0; bool detached = false; std::string name; };
ThreadId threads::Create(const CreateParams&);          // 0 si falla. stack < 256 KiB se sube a 256 KiB
bool     threads::Join(ThreadId, uint32_t* retval = nullptr);
bool     threads::Detach(ThreadId);
bool     threads::IsAlive(ThreadId);
ThreadId threads::CurrentId();
void     threads::AddStartHook(std::function<void(Cpu&)>);   // en el hilo nuevo, antes de entry
void     threads::AddExitHook(std::function<void(Cpu&)>);    // en el hilo, al terminar (destructores de pthread_key, JNI detach)
void     threads::DetachCurrent();                           // para hilos host adjuntados implícitamente
```

Hilos host adjuntados implícitamente (los que llaman a `call_guest`/`Cpu::Current()` sin venir de `threads::Create`): los
*start hooks* corren en el attach; los *exit hooks* **solo** si el hilo llama a `threads::DetachCurrent()` (si el hilo host
termina sin llamarlo, su `Cpu` se destruye sin ejecutar hooks).

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

- El hilo principal del juego es simplemente el hilo host que llama a `call_guest` (T8); `glf` exige que
  `step/resize/stateChanged/getViewSettings` vayan en el mismo hilo host.
- Mutex/cond/keys de pthread: tablas laterales en el host (T4), con primitivas host (`std::mutex`, `SRWLOCK`…). Las claves
  TLS pueden guardarse en `cpu.user(UserSlot::Pthread)`.
- Si un hilo creado con `Create` muere por `GuestFault`/`GuestExit`, se llama a `cpu::SetFatalHandler` /
  `cpu::SetExitHandler` (por defecto: log y `_Exit`).
- Máximo `threads::kMaxThreads` (128) hilos guest vivos.

---

## 6. Loader (`loader/loader.h`)

```cpp
zw::GuestMemory::Init();
zw::hle::libc::Register(); /* gles, audio, jni... */          // handlers + DefineData ANTES de cargar
zw::loader::Module* m = zw::loader::Load("…/libZombieWood.so"); // mapea + reloca + resuelve; no ejecuta nada
zw::loader::RunInit(*m);                                       // .init_array (552 constructores), en este hilo
auto ver = zw::loader::CallJniOnLoad(*m, java_vm_gaddr);       // JNI_OnLoad(vm, NULL) -> 0x10004
```

- Resolución de un símbolo indefinido: el propio módulo → otras libs cargadas (orden de carga) → dato HLE
  (`STT_OBJECT`) → stub HLE (`STT_FUNC`, siempre se resuelve, tenga o no handler).
- `Module::FindExport(nombre)` / `loader::FindSymbol(nombre)` → dirección lista para `call_guest` (bit Thumb incluido), 0
  si no existe. Los 35 581 símbolos del `.so` están disponibles (p. ej. `VoxSetAndroidAPILevel`, `nativeExit`, `mJavaVM`).
- `Module::imports()` → cómo se resolvió cada import (`Via`, `has_handler`). `Module::exidx()/exidx_count()`.
- `loader::addr2sym(addr)` / `zw::debug::addr2sym(addr)` → `"libZombieWood.so!_ZN3glf3App6UpdateEv+0x1c"`, `"hle!malloc"`.
- `Module::base()`: `0x10000000` para la primera lib ⇒ las direcciones de ANALYSIS.md se obtienen restando la base.
- Relocaciones: `R_ARM_RELATIVE`, `ABS32`, `GLOB_DAT`, `JUMP_SLOT`, `REL32`, `NONE` (REL). Todo se resuelve en la carga.

---

## 7. JNI y otras vtables guest (T6/T7)

Un `JNIEnv*` es un puntero guest a un puntero guest a una tabla de punteros a función guest. Cada entrada es un stub HLE:

```cpp
// Handlers con la firma real del slot (softfp y varargs los resuelve el marshalling):
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

- Nombres de stub: cualquier cadena única (`"JNIEnv::X"`, `"JavaVM::GetEnv"`, `"SLEngineItf::CreateAudioPlayer"`); aparecen
  tal cual en trazas y backtraces (`hle!JNIEnv::FindClass`).
- Slots no implementados: rellénalos con `hle::StubFor("JNIEnv::slot_123")` sin registrar handler → aviso con nombre y LR la
  primera vez, devuelve 0.
- `JNIEnv` por hilo: `cpu.user(UserSlot::Jni)`; limpieza en `threads::AddExitHook`.
- Java falso → nativo: `zw::call_guest<R>(zw::loader::FindSymbol("Java_…"), env, clazz, args…)` (reentrante, §3).
- Callbacks de OpenSL/AudioTrack: **no** llames a `call_guest` desde el callback de audio de SDL con el hilo de SDL si
  puedes evitarlo; crea un hilo guest (`threads::Create` con un stub propio como entry, o un `std::thread` host que use
  `call_guest`: se adjunta solo y debe llamar a `threads::DetachCurrent()` al acabar).

---

## 8. Depuración (`zw::debug`, `cpu/cpu.h`)

```cpp
std::string         debug::addr2sym(gaddr);
std::vector<gaddr>  debug::Backtrace(const Cpu&, size_t max = 32);   // heurístico (escaneo de pila validando BL/BLX)
std::string         debug::DumpState(const Cpu&);                    // registros + backtrace simbolizado
void                debug::SetTraceImports(bool);
```

Ante un fallo guest (memoria no mapeada, instrucción indefinida, `bkpt`) el runtime escribe `DumpState` en el log y lanza
`GuestFault`. Ante una violación de acceso del host dentro de un handler (puntero guest inválido) escribe el import en curso,
la dirección guest implicada y `DumpState`, y termina el proceso.

Variables de entorno: `ZW_TRACE_IMPORTS`, `ZW_TRACE_FILTER`, `ZW_ABORT_ON_UNIMPLEMENTED`, `ZW_TRACE_INIT=1` (una línea por
constructor de `.init_array`), `ZW_JIT_CACHE_MB` (caché de código por hilo, 64 por defecto), `ZW_DEBUG_VEH=1` (vuelca cada
excepción del host de primera oportunidad), `ZW_FASTMEM=0` / `ZW_FASTMEM_EXCLUSIVE=0` (ver abajo).

**Acceso a memoria del JIT.** Por defecto dynarmic usa *fastmem* (`host = base + guest`, los fallos de página del host se
convierten en `GuestFault`) y `LDREX/STREX` por `cmpxchg`. `ZW_FASTMEM=0` cambia a tabla de páginas + monitor exclusivo global
(más lento, sin excepciones del host; útil para depurar); `ZW_FASTMEM_EXCLUSIVE=0` mantiene fastmem pero pasa los exclusivos
por el monitor. Fastmem necesita un parche local a dynarmic que aplica `cmake/ZwDependencies.cmake` en cada configure: su
manejador de excepciones de Windows reservaba 8 bytes de pila en vez del *shadow space* de la ABI x64 (`sub rsp, 8` →
`sub rsp, 0x28`), lo que con MSVC corrompía la dirección de retorno en el primer fallo de fastmem.
Medido en win-debug: bucle atómico de 2 hilos (4 M de `LDREX/STREX` contendidos) 8.2–8.5 s con tabla de páginas frente a
1.6–3.5 s con fastmem; los 552 constructores de `--load-only` no cambian (6–8 s en ambos modos: domina la compilación del JIT
en un dynarmic sin optimizar).

---

## 9. Arranque de referencia (T8)

```cpp
zw::GuestMemory::Init();
zw::hle::libc::Register();  zw::hle::gles::Register();  zw::hle::audio::Register();  zw::jni::Register();
auto* m = zw::loader::Load(so_path);
try {
  zw::loader::RunInit(*m);
  zw::loader::CallJniOnLoad(*m, zw::jni::JavaVM());
  // ... secuencia de ANALYSIS.md §4 con call_guest ...
} catch (const zw::GuestFault& f) { zw::log::Error("{}", f.what()); }
  catch (const zw::GuestExit& e)  { /* salir con e.code */ }
```

---

## 10. `--load-only` y HLE mínima de arranque (`loader/load_only.h`)

```
zombiewood --load-only <ruta .so> [más .so...]        (o el ejecutable suelto zw_loadonly, mismos argumentos)
```

Carga las libs en orden, imprime segmentos/relocaciones/imports (con la lista de los que no tienen handler), ejecuta
`.init_array` y `JNI_OnLoad` con un `JavaVM` falso y resume qué imports sin handler se llegaron a invocar. Código de salida 0
si llegó al final. Útil para T4–T7: al enlazar vuestro `Register()` antes de `RunLoadOnly`, la lista de "sin handler" baja.

- `loader::bootstrap::Register()`: ~100 imports mínimos de bionic (malloc, `mem*`/`str*`, `printf` básico, `pthread_key_*`,
  `__cxa_*`, tiempo, libm, tablas `_ctype_`…) **solo para nombres que nadie haya registrado**. Es andamiaje de T3 para
  `--load-only` y tests; no sustituye a T4 (mutex/cond son no-ops, sin stdio). T8 no debe llamarla.
- `loader::bootstrap::FakeJavaVM()`: `JavaVM*` guest con `GetEnv/AttachCurrentThread` y un `JNIEnv` de 232 slots genéricos
  que registran cada llamada (`[jni falso] … FindClass(…)`) y devuelven handles ficticios. T7 lo reemplaza.

Estado medido con solo la HLE mínima (win-debug): `libZombieWood.so` carga en `0x10000000` (56 376 RELATIVE, 6 467 ABS32,
18 GLOB_DAT, 356 JUMP_SLOT), los 552 constructores se ejecutan sin fallos y `JNI_OnLoad` devuelve `0x10004`
(`AndroidOS::Init` encuentra `SUtils`, `Device` y `GameInstaller`). Durante ese arranque el `.so` solo invoca tres imports sin
handler: `wctob`, `btowc` y `wctype` (inicialización de `std::locale`/`ctype<wchar_t>` de gnustl).

---

## Audio (T6) — `hle/audio/audio_track.h`, `hle/audio/audio_hle.h`

Backend C++ puro (sin JNI) de `android.media.AudioTrack` (modo STREAM) sobre SDL2; T7 lo envuelve como clase Java emulada.
CMake: `target_link_libraries(x PRIVATE zw_hle_audio)`; `#include "audio_track.h"` (namespace `zw::audio`).

```cpp
int  audio::native_output_sample_rate();                               // 44100
int  audio::min_buffer_size(int rate, int channelCfg, int encoding);   // bytes, >= 8192

audio::Track t(streamType, rate, channelCfg, encoding, bufferBytes, mode = kModeStream);
// channelCfg: kChannelOutMono=4 / kChannelOutStereo=12 (0xC); encoding: kEncodingPcm16=2 / kEncodingPcm8=3
t.play(); t.pause(); t.stop() /*descarta la cola*/; t.release() /*despierta writers; idempotente*/;
int n = t.write(const void* pcm, int bytes);  // bytes aceptados; -1 si liberada
int s = t.play_state();                        // kStateStopped=1, kStatePaused=2, kStatePlaying=3
```

- `write` copia a una cola y **bloquea** mientras haya > ~100 ms en cola y la pista esté en `PLAYING` (y no en pausa global).
  Pausada/parada/liberada **nunca** bloquea (acepta hasta ~200 ms y devuelve lo aceptado). `release()` desbloquea a un writer.
- SDL abre un dispositivo por pista (formato nativo de la pista; SDL convierte rate/canales/bits). Si no hay dispositivo, un hilo
  consume a tiempo real (el juego no se bloquea). Un *underrun* emite silencio.
- Control global (menú T9, `nativeVoxMute`, minimizar): `audio::set_master_volume(0..1)`, `set_muted(bool)`, `set_global_paused(bool)`
  (pausa/reanuda todas las pistas sin cambiar su `play_state`). Se aplican en el callback, afectan también a datos ya encolados.
- Tests/CI: `audio::set_driver("dummy")` antes de crear la primera pista; `audio::set_tap(fn)` observa lo que va al dispositivo.
- **OpenSL ES** (`zw::hle::audio::Register()`, `audio_hle.h`; T8 la llama antes de `loader::Load`): define `SL_IID_ENGINE`,
  `SL_IID_PLAY`, `SL_IID_BUFFERQUEUE` (variables puntero a `SLInterfaceID_` de 16 bytes con los GUID reales) y registra
  `slCreateEngine`, que devuelve `SL_RESULT_FEATURE_UNSUPPORTED` (12) y pone `*pEngine = NULL`. El juego usa `AudioTrack`
  (`s_androidAPILevel = 3`), así que OpenSL es camino muerto.
