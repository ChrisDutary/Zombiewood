// Memoria guest de 32 bits (T3). Contrato publico: ver docs/CONTRACTS.md §1.
//
// Se reservan 4 GiB contiguos en el host; `host = g_membase + guest`. Las paginas se
// confirman (commit) al mapear. Acceder a una direccion guest no mapeada desde el host
// provoca una violacion de acceso (que el runtime vuelca con contexto guest).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace zw {

using gaddr = std::uint32_t;  // direccion guest (0 = NULL)
using gsize = std::uint32_t;  // size_t del guest (32 bits). NO usar size_t del host en firmas HLE.
using gssize = std::int32_t;

// Base del espacio guest en el host. Valida tras GuestMemory::Init().
extern std::uint8_t* g_membase;

// guest -> host. 0 -> nullptr.
template <class T = void>
inline T* g2h(gaddr a) {
  return a ? reinterpret_cast<T*>(g_membase + a) : nullptr;
}
// host -> guest. nullptr -> 0. El puntero DEBE estar dentro del espacio guest
// (si no, registra un error y devuelve 0; ver GuestMemory::Contains).
gaddr h2g(const void* p);

enum Prot : std::uint32_t {
  kProtNone = 0,
  kProtRead = 1,
  kProtWrite = 2,
  kProtExec = 4,  // "ejecutable" para el JIT (el host nunca ejecuta paginas guest)
  kProtRW = kProtRead | kProtWrite,
  kProtRX = kProtRead | kProtExec,
  kProtRWX = kProtRead | kProtWrite | kProtExec,
};

// Zonas del espacio de direcciones guest (ver CONTRACTS.md §1.1).
enum class Zone { Libs, Heap, Stacks, Thunks };

struct HeapStats {
  std::uint64_t bytes_in_use = 0;   // suma de tamanos pedidos vivos
  std::uint64_t bytes_mapped = 0;   // memoria confirmada por el heap
  std::uint64_t alloc_count = 0;    // bloques vivos
};

class GuestMemory {
 public:
  static constexpr gaddr kPageSize = 0x1000;
  static constexpr gaddr kLibsBase = 0x10000000, kLibsEnd = 0x20000000;
  static constexpr gaddr kHeapBase = 0x20000000, kHeapEnd = 0x80000000;
  static constexpr gaddr kStacksBase = 0x80000000, kStacksEnd = 0x90000000;
  static constexpr gaddr kThunksBase = 0x0F000000, kThunksEnd = 0x10000000;

  // Reserva los 4 GiB. Idempotente y thread-safe. false si el host no puede reservar.
  static bool Init();
  static bool IsInitialized();

  // --- Paginas -------------------------------------------------------------
  // Mapea [addr, addr+size) (redondeado a paginas) en una direccion fija. Contenido a cero.
  static bool Map(gaddr addr, gsize size, Prot prot, std::string_view tag = {});
  // Reserva y mapea `size` bytes en una zona; devuelve la direccion (alineada a `align`,
  // potencia de 2 >= 4 KiB) o 0 si no hay espacio.
  static gaddr MapAnywhere(Zone zone, gsize size, Prot prot, std::string_view tag = {},
                           gsize align = kPageSize);
  static bool Unmap(gaddr addr, gsize size);
  static bool Protect(gaddr addr, gsize size, Prot prot);
  // true si todas las paginas de [addr, addr+size) estan mapeadas (con cualquier permiso != None).
  static bool IsMapped(gaddr addr, gsize size = 1);
  static Prot ProtOf(gaddr addr);
  // true si el puntero host cae dentro de los 4 GiB reservados.
  static bool Contains(const void* host_ptr);

  // --- Heap guest (thread-safe). Bloques alineados a 8, como dlmalloc de bionic. ----
  // Alloc(0) devuelve un bloque valido de tamano minimo. 0 si no hay memoria.
  static gaddr Alloc(gsize size);
  static gaddr Calloc(gsize count, gsize size);          // a cero; 0 si desborda
  static gaddr Realloc(gaddr p, gsize size);             // Realloc(0, n) == Alloc(n)
  static gaddr AllocAligned(gsize size, gsize align);    // memalign; align potencia de 2
  static void Free(gaddr p);                             // Free(0) es no-op
  static gsize UsableSize(gaddr p);                      // 0 si p no es un bloque vivo
  static HeapStats GetHeapStats();
};

// ---------------------------------------------------------------------------
// Lectura/escritura tipada (seguras frente a desalineamiento).
namespace mem {
template <class T>
inline T Read(gaddr a) {
  static_assert(std::is_trivially_copyable_v<T>);
  T v;
  std::memcpy(&v, g_membase + a, sizeof(T));
  return v;
}
template <class T>
inline void Write(gaddr a, const std::type_identity_t<T>& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(g_membase + a, &v, sizeof(T));
}
inline std::uint8_t R8(gaddr a) { return Read<std::uint8_t>(a); }
inline std::uint16_t R16(gaddr a) { return Read<std::uint16_t>(a); }
inline std::uint32_t R32(gaddr a) { return Read<std::uint32_t>(a); }
inline std::uint64_t R64(gaddr a) { return Read<std::uint64_t>(a); }
inline void W8(gaddr a, std::uint8_t v) { Write<std::uint8_t>(a, v); }
inline void W16(gaddr a, std::uint16_t v) { Write<std::uint16_t>(a, v); }
inline void W32(gaddr a, std::uint32_t v) { Write<std::uint32_t>(a, v); }
inline void W64(gaddr a, std::uint64_t v) { Write<std::uint64_t>(a, v); }

// Cadena C guest -> std::string ("" si a == 0). max_len limita la lectura.
std::string ReadCStr(gaddr a, gsize max_len = 0x100000);
// Copia `s` + '\0' en `dst` sin pasar de `cap` bytes (trunca, siempre termina en 0 si cap > 0).
// Devuelve los bytes escritos sin contar el terminador.
gsize WriteCStr(gaddr dst, std::string_view s, gsize cap);
// Duplica en el heap guest (liberar con GuestMemory::Free). Incluye '\0'.
gaddr StrDup(std::string_view s);
// Copia un bloque host al heap guest.
gaddr Dup(const void* data, gsize size);
}  // namespace mem

// Puntero guest tipado de 32 bits: para campos de structs guest y parametros HLE.
// sizeof(GPtr<T>) == 4. Se marshala como una palabra.
template <class T>
struct GPtr {
  gaddr addr = 0;
  GPtr() = default;
  explicit GPtr(gaddr a) : addr(a) {}
  T* get() const { return g2h<T>(addr); }
  explicit operator bool() const { return addr != 0; }
  template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
  U& operator*() const { return *get(); }
  T* operator->() const { return get(); }
  template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
  U& operator[](std::size_t i) const { return get()[i]; }
};
static_assert(sizeof(GPtr<int>) == 4);

}  // namespace zw
