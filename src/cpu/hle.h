// Registro de imports HLE y marshalling AAPCS softfp guest -> host (T3).
// Contrato publico: ver docs/CONTRACTS.md §2.
#pragma once
#include <bit>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpu/cpu.h"
#include "cpu/guest_memory.h"

namespace zw {

// Lector de argumentos variadicos del guest (reglas AAPCS base: palabras de 4 bytes,
// double/long long alineados a 8).
class VaList {
 public:
  VaList() = default;
  // A partir de un `va_list` guest (struct { void* __ap; } pasado por valor = 1 palabra).
  static VaList FromGuest(gaddr ap) {
    VaList v;
    v.ap_ = ap;
    return v;
  }
  // A partir de los argumentos de la llamada en curso, desde la palabra `first_word`
  // (0..3 = r0..r3, 4.. = pila).
  static VaList FromCall(Cpu& cpu, unsigned first_word) {
    VaList v;
    v.cpu_ = &cpu;
    v.idx_ = first_word;
    return v;
  }

  std::uint32_t NextWord() {
    if (cpu_) return cpu_->arg_word(idx_++);
    std::uint32_t v = mem::R32(ap_);
    ap_ += 4;
    return v;
  }
  std::uint64_t NextDword() {
    if (cpu_) {
      idx_ = (idx_ + 1) & ~1u;
      std::uint64_t lo = cpu_->arg_word(idx_), hi = cpu_->arg_word(idx_ + 1);
      idx_ += 2;
      return lo | (hi << 32);
    }
    ap_ = (ap_ + 7) & ~7u;
    std::uint64_t v = mem::R64(ap_);
    ap_ += 8;
    return v;
  }
  // Siguiente argumento con tipo. Recuerda las promociones de C: char/short -> int,
  // float -> double (Next<float>() no compila). Punteros T* se traducen con g2h.
  template <class T>
  T Next() {
    if constexpr (std::is_same_v<T, bool>) {
      return (NextWord() & 0xFF) != 0;
    } else if constexpr (detail::is_word_int<T>) {
      return static_cast<T>(NextWord());
    } else if constexpr (detail::is_raw_int64<T> || std::is_same_v<T, gi64> || std::is_same_v<T, gu64>) {
      return static_cast<T>(NextDword());
    } else if constexpr (std::is_same_v<T, double>) {
      return std::bit_cast<double>(NextDword());
    } else if constexpr (std::is_pointer_v<T>) {
      return g2h<std::remove_pointer_t<T>>(NextWord());
    } else if constexpr (detail::is_gptr<T>::value) {
      return T(NextWord());
    } else {
      static_assert(!std::is_same_v<T, float>, "VaList: float se promociona a double; usa Next<double>()");
      static_assert(std::is_same_v<T, float>, "VaList: tipo no soportado");
    }
  }
  // Solo para listas creadas con FromGuest: posicion actual (para va_copy o reenviar).
  gaddr guest_ap() const { return ap_; }
  bool from_call() const { return cpu_ != nullptr; }

 private:
  Cpu* cpu_ = nullptr;
  unsigned idx_ = 0;
  gaddr ap_ = 0;
};

// Parametro "..." de un handler: debe ser el ULTIMO parametro. Captura los argumentos
// variadicos que siguen a los parametros con nombre.
struct Varargs : VaList {
  Varargs() = default;
  explicit Varargs(const VaList& v) : VaList(v) {}
};

namespace hle {

// Handler crudo: lee/escribe registros a mano. El valor de retorno va en cpu.reg(0) (y reg(1)).
using RawFn = void (*)(Cpu&);

enum Flags : std::uint32_t {
  kDefault = 0,
  // El handler se ejecuta DENTRO del callback del JIT (mas rapido: no sale de Jit::Run).
  // Requisitos: no llamar a call_guest, no lanzar excepciones C++, no bloquear mucho tiempo,
  // no cambiar pc/sp. Ideal para memcpy/strlen/math/gl*.
  kFast = 1,
};

// Asocia `fn` al import `name`. Registrar dos veces el mismo nombre sustituye al anterior
// (el ultimo gana). Se puede registrar antes o despues de cargar el .so. Thread-safe.
void Register(std::string_view name, RawFn fn, std::uint32_t flags = kDefault);
bool IsRegistered(std::string_view name);

// Direccion guest (codigo ARM `svc #n; bx lr`) del stub del import `name`. La crea si no existe;
// siempre la misma para un nombre dado. Sirve como puntero a funcion para el guest
// (vtables de JNIEnv/OpenSL, callbacks, relocaciones). Llamar al stub sin handler registrado
// registra un aviso la primera vez y devuelve 0.
gaddr StubFor(std::string_view name);
// Nombre del import al que pertenece una direccion de stub ("" si no es un stub).
std::string StubName(gaddr addr);

// Datos importados (`__sF`, `_ctype_`, `__stack_chk_guard`, `SL_IID_*`...).
// DefineData: crea (o devuelve, si ya existe) un bloque guest a cero de `size` bytes para el
// simbolo. El modulo HLE lo rellena. DEBE llamarse antes de cargar el .so que lo importa.
gaddr DefineData(std::string_view name, gsize size, gsize align = 8);
// Asocia el simbolo a una direccion guest ya existente.
void DefineDataAt(std::string_view name, gaddr addr);
gaddr FindData(std::string_view name);  // 0 si no esta definido

// Imports cuyo stub se ha pedido (StubFor) y no tienen handler.
std::vector<std::string> UnimplementedImports();
enum class OnUnimplemented { ReturnZero, Abort };
// Por defecto ReturnZero (o Abort si ZW_ABORT_ON_UNIMPLEMENTED=1). Abort lanza GuestFault.
void SetUnimplementedPolicy(OnUnimplemented p);
// (nombre, n.º de llamadas) de todos los stubs, para perfilar.
std::vector<std::pair<std::string, std::uint64_t>> CallCounts();

// Sustituye una funcion guest ya cargada (p. ej. un export del .so) por un import HLE:
// parchea su entrada con un salto al stub de `name`. `fn` lleva el bit 0 si es Thumb.
// Necesita >= 12 bytes de funcion (16 si es Thumb y no esta alineada a 4).
bool HookGuestFunction(gaddr fn, std::string_view name);

namespace detail {
using ::zw::detail::is_gptr;
using ::zw::detail::is_raw_int64;
using ::zw::detail::is_word_int;

struct ArgCursor {
  Cpu& cpu;
  unsigned idx = 0;
  std::uint32_t N32() { return cpu.arg_word(idx++); }
  std::uint64_t N64() {
    idx = (idx + 1) & ~1u;
    std::uint64_t lo = cpu.arg_word(idx), hi = cpu.arg_word(idx + 1);
    idx += 2;
    return lo | (hi << 32);
  }
};

template <class P>
decltype(auto) GetArg(ArgCursor& c) {
  using D = std::remove_cv_t<std::remove_reference_t<P>>;
  if constexpr (std::is_same_v<D, Cpu>) {
    static_assert(std::is_lvalue_reference_v<P>, "HLE: el parametro Cpu debe ser Cpu&");
    return static_cast<Cpu&>(c.cpu);
  } else if constexpr (std::is_same_v<D, Varargs>) {
    return Varargs(VaList::FromCall(c.cpu, c.idx));
  } else if constexpr (std::is_same_v<D, VaList>) {
    return VaList::FromGuest(c.N32());
  } else if constexpr (std::is_same_v<D, bool>) {
    return (c.N32() & 0xFF) != 0;
  } else if constexpr (is_word_int<D>) {
    return static_cast<D>(c.N32());
  } else if constexpr (std::is_same_v<D, gi64>) {
    return gi64(static_cast<std::int64_t>(c.N64()));
  } else if constexpr (std::is_same_v<D, gu64>) {
    return gu64(c.N64());
  } else if constexpr (std::is_same_v<D, float>) {
    return std::bit_cast<float>(c.N32());
  } else if constexpr (std::is_same_v<D, double>) {
    return std::bit_cast<double>(c.N64());
  } else if constexpr (std::is_pointer_v<D>) {
    return g2h<std::remove_pointer_t<D>>(c.N32());
  } else if constexpr (is_gptr<D>::value) {
    return D(c.N32());
  } else {
    static_assert(!is_raw_int64<D>,
                  "HLE: parametro entero de 64 bits ambiguo (size_t del host?). Usa zw::gsize/"
                  "uint32_t para size_t del guest, o zw::gi64/zw::gu64 para long long del guest");
    static_assert(is_raw_int64<D>, "HLE: tipo de parametro no soportado");
  }
}

template <class R>
void SetResult(Cpu& cpu, const R& v) {
  if constexpr (std::is_same_v<R, bool>) {
    cpu.reg(0) = v ? 1u : 0u;
  } else if constexpr (is_word_int<R>) {
    if constexpr (std::is_enum_v<R>) cpu.reg(0) = static_cast<std::uint32_t>(v);
    else if constexpr (std::is_signed_v<R>) cpu.reg(0) = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
    else cpu.reg(0) = static_cast<std::uint32_t>(v);
  } else if constexpr (is_raw_int64<R> || std::is_same_v<R, gi64> || std::is_same_v<R, gu64>) {
    // Nota: un retorno size_t del host escribe tambien r1; es inocuo (r1 es caller-saved).
    const auto u = static_cast<std::uint64_t>(v);
    cpu.reg(0) = static_cast<std::uint32_t>(u);
    cpu.reg(1) = static_cast<std::uint32_t>(u >> 32);
  } else if constexpr (std::is_same_v<R, float>) {
    cpu.reg(0) = std::bit_cast<std::uint32_t>(v);
  } else if constexpr (std::is_same_v<R, double>) {
    const auto u = std::bit_cast<std::uint64_t>(v);
    cpu.reg(0) = static_cast<std::uint32_t>(u);
    cpu.reg(1) = static_cast<std::uint32_t>(u >> 32);
  } else if constexpr (std::is_pointer_v<R>) {
    cpu.reg(0) = h2g(v);
  } else if constexpr (is_gptr<R>::value) {
    cpu.reg(0) = v.addr;
  } else {
    static_assert(std::is_void_v<R>, "HLE: tipo de retorno no soportado");
  }
}

template <class R, class... A>
void Invoke(Cpu& cpu, R (*fn)(A...)) {
  ArgCursor cur{cpu};
  // La inicializacion con llaves garantiza la evaluacion de izquierda a derecha.
  std::tuple<decltype(GetArg<A>(cur))...> args{GetArg<A>(cur)...};
  if constexpr (std::is_void_v<R>) {
    std::apply(fn, std::move(args));
  } else {
    SetResult<std::remove_cv_t<R>>(cpu, std::apply(fn, std::move(args)));
  }
}

template <auto Fn>
struct Thunk {
  static void Call(Cpu& cpu) { Invoke(cpu, Fn); }
};
}  // namespace detail

}  // namespace hle
}  // namespace zw

// Registro con marshalling automatico. Usar DENTRO de una funcion de registro del modulo
// (no como inicializador estatico: el enlazador descarta objetos sin referencias de una .lib).
//
//   static int hle_abs(int x) { return x < 0 ? -x : x; }
//   void zw::hle::libc::Register() { ZW_HLE("abs", hle_abs); }
//
// `fn` debe ser una funcion libre o estatica no sobrecargada. Tipos admitidos: ver CONTRACTS.md §2.2.
#define ZW_HLE(name, fn) ::zw::hle::Register((name), &::zw::hle::detail::Thunk<fn>::Call, ::zw::hle::kDefault)
// Igual, pero el handler corre dentro del JIT (ver hle::kFast).
#define ZW_HLE_FAST(name, fn) ::zw::hle::Register((name), &::zw::hle::detail::Thunk<fn>::Call, ::zw::hle::kFast)
// Handler crudo void(Cpu&).
#define ZW_HLE_RAW(name, rawfn) ::zw::hle::Register((name), (rawfn), ::zw::hle::kDefault)
// Registra y devuelve la direccion guest del stub (para vtables / punteros a funcion).
#define ZW_HLE_THUNK(name, fn) (ZW_HLE((name), fn), ::zw::hle::StubFor((name)))
