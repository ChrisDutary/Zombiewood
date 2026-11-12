// CPU guest ARM32 (dynarmic), puente host->guest e hilos (T3).
// Contrato publico: ver docs/CONTRACTS.md §3-§5.
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "cpu/guest_memory.h"

namespace zw {

// `long long` / `unsigned long long` del guest en firmas HLE y en call_guest.
// (Los enteros de 64 bits "desnudos" se rechazan en compilacion: en MSVC x64 size_t es
// uint64_t y se confundiria con un argumento guest de 64 bits.)
struct gi64 {
  std::int64_t v = 0;
  constexpr gi64() = default;
  constexpr gi64(std::int64_t x) : v(x) {}
  constexpr operator std::int64_t() const { return v; }
};
struct gu64 {
  std::uint64_t v = 0;
  constexpr gu64() = default;
  constexpr gu64(std::uint64_t x) : v(x) {}
  constexpr operator std::uint64_t() const { return v; }
};

// El guest hizo algo irrecuperable (acceso a memoria no mapeada, instruccion indefinida,
// import sin handler con politica Abort...). what() incluye el volcado de registros.
struct GuestFault : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// Lanzar desde un handler HLE (no FAST) para terminar el proceso guest (exit/abort).
struct GuestExit {
  int code = 0;
};

namespace detail {
struct CpuImpl;

template <class T>
inline constexpr bool is_word_int =
    (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 4;
template <class T>
inline constexpr bool is_raw_int64 = (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) == 8;
template <class T>
struct is_gptr : std::false_type {};
template <class T>
struct is_gptr<GPtr<T>> : std::true_type {};

// Empaquetado de argumentos host -> palabras AAPCS (softfp).
struct WordPack {
  std::uint32_t* w;
  std::size_t n = 0;
  void P32(std::uint32_t v) { w[n++] = v; }
  void P64(std::uint64_t v) {
    if (n & 1) w[n++] = 0;  // los valores de 64 bits van en indice par (r0:r1, r2:r3, pila alineada a 8)
    w[n++] = static_cast<std::uint32_t>(v);
    w[n++] = static_cast<std::uint32_t>(v >> 32);
  }
  template <class T>
  void Push(const T& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, bool>) {
      P32(v ? 1u : 0u);
    } else if constexpr (is_word_int<D>) {
      if constexpr (std::is_enum_v<D>) P32(static_cast<std::uint32_t>(v));
      else if constexpr (std::is_signed_v<D>) P32(static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
      else P32(static_cast<std::uint32_t>(v));
    } else if constexpr (std::is_same_v<D, gi64> || std::is_same_v<D, gu64>) {
      P64(static_cast<std::uint64_t>(v.v));
    } else if constexpr (std::is_same_v<D, float>) {
      P32(std::bit_cast<std::uint32_t>(v));
    } else if constexpr (std::is_same_v<D, double>) {
      P64(std::bit_cast<std::uint64_t>(v));
    } else if constexpr (std::is_pointer_v<D>) {
      P32(h2g(v));
    } else if constexpr (std::is_null_pointer_v<D>) {
      P32(0);
    } else if constexpr (is_gptr<D>::value) {
      P32(v.addr);
    } else {
      static_assert(!is_raw_int64<D>,
                    "call_guest: entero de 64 bits ambiguo; usa zw::gi64/zw::gu64 (long long guest) "
                    "o convierte a uint32_t/gaddr/gsize");
      static_assert(is_raw_int64<D>, "call_guest: tipo de argumento no soportado");
    }
  }
};

template <class R>
R UnpackResult(std::uint64_t r0r1) {
  const auto r0 = static_cast<std::uint32_t>(r0r1);
  if constexpr (std::is_void_v<R>) {
    (void)r0;
  } else if constexpr (std::is_same_v<R, bool>) {
    return (r0 & 0xFF) != 0;
  } else if constexpr (is_word_int<R>) {
    return static_cast<R>(r0);
  } else if constexpr (is_raw_int64<R> || std::is_same_v<R, gi64> || std::is_same_v<R, gu64>) {
    return static_cast<R>(r0r1);
  } else if constexpr (std::is_same_v<R, float>) {
    return std::bit_cast<float>(r0);
  } else if constexpr (std::is_same_v<R, double>) {
    return std::bit_cast<double>(r0r1);
  } else if constexpr (std::is_pointer_v<R>) {
    return g2h<std::remove_pointer_t<R>>(r0);
  } else if constexpr (is_gptr<R>::value) {
    return R(r0);
  } else {
    static_assert(std::is_void_v<R>, "call_guest: tipo de retorno no soportado");
  }
}
}  // namespace detail

// Ranuras de datos por hilo para los modulos HLE (punteros host opacos, inicialmente nullptr).
enum class UserSlot { Libc, Pthread, Gles, Audio, Jni, Frontend, Spare0, Spare1, Count };

// Estado de un hilo guest. Hay exactamente uno por hilo host que ejecuta codigo guest.
// Solo se puede usar desde su propio hilo host.
class Cpu {
 public:
  // Cpu del hilo host actual. Si el hilo aun no tiene, lo crea ("attach") con una pila guest
  // de threads::kDefaultStackSize. Nunca falla (aborta el proceso si no hay memoria).
  static Cpu& Current();
  static Cpu* CurrentOrNull();

  // --- Registros --------------------------------------------------------------
  std::uint32_t& reg(int i) { return regs_[i]; }  // r0..r15
  std::uint32_t reg(int i) const { return regs_[i]; }
  std::uint32_t& sp() { return regs_[13]; }
  std::uint32_t& lr() { return regs_[14]; }
  std::uint32_t& pc() { return regs_[15]; }
  std::uint32_t cpsr() const;
  void set_cpsr(std::uint32_t v);
  std::uint32_t fpscr() const;
  void set_fpscr(std::uint32_t v);
  std::uint64_t d(int i) const {  // d0..d31 (bits crudos)
    return ext_[2 * i] | (static_cast<std::uint64_t>(ext_[2 * i + 1]) << 32);
  }
  void set_d(int i, std::uint64_t v) {
    ext_[2 * i] = static_cast<std::uint32_t>(v);
    ext_[2 * i + 1] = static_cast<std::uint32_t>(v >> 32);
  }
  std::uint32_t s(int i) const { return ext_[i]; }  // s0..s31 (bits crudos)
  void set_s(int i, std::uint32_t v) { ext_[i] = v; }

  // Palabra de argumento AAPCS n.º idx de la llamada a import en curso: 0..3 = r0..r3,
  // 4.. = [sp + 4*(idx-4)]. Solo tiene sentido dentro de un handler HLE.
  std::uint32_t arg_word(unsigned idx) const {
    return idx < 4 ? regs_[idx] : mem::R32(regs_[13] + 4 * (idx - 4));
  }

  // --- Identidad / datos por hilo --------------------------------------------------
  std::uint32_t thread_id() const { return thread_id_; }  // != 0, unico entre hilos vivos
  const std::string& name() const { return name_; }
  gaddr tls() const { return tls_; }                // bloque TLS bionic (valor de TPIDRURO), 64 palabras
  gaddr errno_addr() const { return errno_addr_; }  // int errno de este hilo (para __errno())
  gaddr stack_base() const { return stack_base_; }  // direccion mas baja de la pila guest
  gsize stack_size() const { return stack_size_; }
  void*& user(UserSlot s) { return user_[static_cast<int>(s)]; }
  // Nombre del import cuyo handler se esta ejecutando ("" si ninguno) y su LR guest.
  const char* current_import() const { return cur_import_; }
  // Profundidad de anidamiento host->guest (0 = no hay codigo guest en ejecucion en este hilo).
  int call_depth() const { return depth_; }

  // --- host -> guest --------------------------------------------------------------
  // Llama a la funcion guest `fn` (bit 0 = Thumb) con `nwords` palabras AAPCS ya empaquetadas.
  // Reentrante: se puede invocar desde un handler HLE (no FAST). Devuelve r0 | r1 << 32.
  // Conserva todos los registros nucleo del llamador guest. Lanza GuestFault/GuestExit.
  std::uint64_t CallRaw(gaddr fn, const std::uint32_t* words, std::size_t nwords);

  template <class R = std::uint32_t, class... A>
  R Call(gaddr fn, const A&... args) {
    std::uint32_t words[2 * sizeof...(A) + 1];
    detail::WordPack pack{words};
    (pack.Push(args), ...);
    return detail::UnpackResult<R>(CallRaw(fn, words, pack.n));
  }

  Cpu(const Cpu&) = delete;
  Cpu& operator=(const Cpu&) = delete;
  ~Cpu();

 private:
  friend struct detail::CpuImpl;
  Cpu();
  std::uint32_t* regs_ = nullptr;
  std::uint32_t* ext_ = nullptr;
  std::uint32_t thread_id_ = 0;
  std::string name_;
  gaddr tls_ = 0, errno_addr_ = 0, stack_base_ = 0;
  gsize stack_size_ = 0;
  int depth_ = 0;
  const char* cur_import_ = "";
  void* user_[static_cast<int>(UserSlot::Count)] = {};
  std::unique_ptr<detail::CpuImpl> impl_;
};

// Llama a una funcion guest desde el host en el hilo actual (ver Cpu::Call).
//   int r = zw::call_guest<int>(cmp, a, b);            // gaddr / enteros <= 32 bits: 1 palabra
//   zw::call_guest<void>(fn, 1.5f, 2.0, zw::gi64{7});  // float: 1 palabra; double/gi64: par alineado
// Los punteros host T* se convierten con h2g (deben apuntar a memoria guest).
template <class R = std::uint32_t, class... A>
inline R call_guest(gaddr fn, const A&... args) {
  return Cpu::Current().template Call<R>(fn, args...);
}

// ---------------------------------------------------------------------------
namespace threads {
inline constexpr gsize kDefaultStackSize = 1u << 20;  // 1 MiB
inline constexpr gsize kMinStackSize = 256u << 10;    // las peticiones menores se suben a 256 KiB
inline constexpr std::size_t kMaxThreads = 128;       // hilos guest vivos a la vez

using ThreadId = std::uint32_t;  // == Cpu::thread_id(); 0 = invalido

struct CreateParams {
  gaddr entry = 0;       // void* (*)(void*) guest (bit 0 = Thumb)
  gaddr arg = 0;
  gsize stack_size = 0;  // 0 = kDefaultStackSize
  bool detached = false;
  std::string name;      // para logs
};

// Crea un hilo host con su propio Cpu/Jit y ejecuta entry(arg). Devuelve 0 si falla.
ThreadId Create(const CreateParams& p);
// Espera a que termine y recoge el valor devuelto por entry. false si el id no existe,
// es detached o ya fue recogido. Bloquea sin retener ningun lock del runtime.
bool Join(ThreadId id, std::uint32_t* retval = nullptr);
bool Detach(ThreadId id);
bool IsAlive(ThreadId id);
ThreadId CurrentId();    // == Cpu::Current().thread_id()
std::size_t Count();     // hilos guest vivos (incluye los "attach" implicitos)

// Hooks que se ejecutan EN el hilo guest: al arrancar (antes de entry) y al terminar
// (despues de entry, antes de destruir el Cpu; ahi van los destructores de pthread_key,
// DetachCurrentThread...). Tambien se ejecutan para hilos host adjuntados con Cpu::Current()
// (start: en el attach; exit: en DetachCurrent()/fin del hilo host). Registrar antes de crear hilos.
void AddStartHook(std::function<void(Cpu&)> fn);
void AddExitHook(std::function<void(Cpu&)> fn);
// Destruye el Cpu del hilo host actual (hilos adjuntados implicitamente). No-op si no hay.
void DetachCurrent();
}  // namespace threads

// ---------------------------------------------------------------------------
namespace cpu {
// Invalida el codigo traducido de [addr, addr+size) en TODOS los hilos (tras parchear codigo guest).
void InvalidateCode(gaddr addr, gsize size);

// Que hacer cuando un hilo creado con threads::Create muere por GuestFault o hace GuestExit.
// Por defecto: log + std::_Exit(code) (fault -> 70).
void SetFatalHandler(std::function<void(const std::string& what)> fn);
void SetExitHandler(std::function<void(int code)> fn);

// setjmp/longjmp sobre el estado del CPU guest. Ya estan registrados como imports
// "setjmp", "_setjmp", "sigsetjmp", "longjmp", "_longjmp", "siglongjmp"; T4 no debe implementarlos.
}  // namespace cpu

// ---------------------------------------------------------------------------
namespace debug {
// Resuelve una direccion guest a "lib!simbolo+0xNN". El loader instala el suyo al cargar.
using Symbolizer = std::function<bool(gaddr addr, std::string& out)>;
void AddSymbolizer(Symbolizer fn);
// "libZombieWood.so!JNI_OnLoad+0x1c", "hle!malloc" o "0x12345678" si no se conoce.
std::string addr2sym(gaddr addr);
// Direcciones de retorno plausibles halladas en la pila (heuristico: palabras que apuntan a
// codigo justo despues de un BL/BLX). [0] = pc, [1] = lr.
std::vector<gaddr> Backtrace(const Cpu& cpu, std::size_t max_frames = 32);
// Registros + backtrace simbolizado, listo para log.
std::string DumpState(const Cpu& cpu);
// Traza de llamadas a imports (tambien con la variable de entorno ZW_TRACE_IMPORTS=1;
// ZW_TRACE_FILTER=subcadena limita por nombre).
void SetTraceImports(bool on);
bool TraceImports();
}  // namespace debug

}  // namespace zw
