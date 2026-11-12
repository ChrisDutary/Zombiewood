// Detalles internos de zw_cpu (no forma parte del contrato).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"

namespace zw::internal {

// Permisos (Prot) por pagina guest; 1 byte por pagina, 2^20 entradas.
extern std::uint8_t* g_page_prot;
// Tabla de paginas para dynarmic (modo absolute_offset): g_membase si la pagina es accesible, nullptr si no.
extern std::uint8_t** g_page_table;
inline std::uint8_t PageProt(gaddr a) { return g_page_prot[a >> 12]; }
inline bool RangeHas(gaddr a, std::uint32_t size, std::uint8_t bits) {
  if ((PageProt(a) & bits) != bits) return false;
  const gaddr last = a + (size - 1);
  if (last < a) return false;
  return (last >> 12) == (a >> 12) || (PageProt(last) & bits) == bits;
}

// --- stubs HLE -------------------------------------------------------------
inline constexpr std::uint32_t kMaxStubs = 16384;
inline constexpr std::uint32_t kStubSize = 8;  // svc #n ; bx lr
inline constexpr gaddr kReturnStub = GuestMemory::kThunksBase;  // indice 0: retorno de call_guest

struct HleEntry {
  std::string name;
  std::atomic<hle::RawFn> fn{nullptr};
  std::atomic<std::uint32_t> flags{0};
  std::atomic<bool> warned{false};
  std::atomic<std::uint64_t> calls{0};
  gaddr stub = 0;
};
// nullptr si el indice no existe. Lectura sin lock (las entradas nunca se mueven).
HleEntry* HleEntryByIndex(std::uint32_t idx);
void HleEnsureInit();
// Como hle::Register pero sin inicializar (para RegisterBuiltins, que corre durante la inicializacion).
void HleRegisterNoInit(std::string_view name, hle::RawFn fn, std::uint32_t flags);
bool UnimplementedAborts();

// Registra los imports propios de zw_cpu (setjmp/longjmp...).
void RegisterBuiltins();

}  // namespace zw::internal
