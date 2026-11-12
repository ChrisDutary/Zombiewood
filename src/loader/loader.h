// Loader ELF32 ARM (T3). Contrato publico: ver docs/CONTRACTS.md §6.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpu/guest_memory.h"

namespace zw::loader {

struct SymbolInfo {
  std::string name;
  gaddr addr = 0;      // direccion guest SIN el bit Thumb
  gsize size = 0;
  bool is_func = false;
  bool thumb = false;
};

struct ImportInfo {
  std::string name;
  bool is_data = false;   // STT_OBJECT
  bool weak = false;
  gaddr addr = 0;         // a donde se resolvio (stub HLE, dato HLE o export de otra lib)
  enum class Via { Unresolved, Module, HleStub, HleData, HleDataPlaceholder } via = Via::Unresolved;
  bool has_handler = false;  // (solo HleStub) habia handler registrado al terminar Load()
};

struct Segment {
  gaddr addr = 0;
  gsize size = 0;
  Prot prot = kProtNone;
};

class Module {
 public:
  const std::string& name() const { return name_; }      // nombre del fichero
  const std::string& soname() const { return soname_; }
  gaddr base() const { return base_; }                   // sesgo de carga: guest = base + vaddr ELF
  gsize size() const { return size_; }
  bool Contains(gaddr a) const { return a >= base_ && a - base_ < size_; }
  const std::vector<Segment>& segments() const { return segments_; }
  const std::vector<std::string>& needed() const { return needed_; }

  // Export por nombre (tabla .hash). Devuelve la direccion guest lista para llamar
  // (con bit 0 si es Thumb) o 0 si no existe.
  gaddr FindExport(std::string_view name) const;
  // Simbolo que contiene `addr` (el mas cercano por debajo dentro de su tamano).
  std::optional<SymbolInfo> SymbolAt(gaddr addr) const;
  const std::vector<SymbolInfo>& symbols() const { return symbols_; }  // ordenados por addr
  const std::vector<ImportInfo>& imports() const { return imports_; }

  // Constructores: DT_INIT (si hay) seguido de .init_array, en orden, con bit Thumb.
  const std::vector<gaddr>& init_functions() const { return init_; }
  bool init_done() const { return init_done_; }
  // .ARM.exidx (para __gnu_Unwind_Find_exidx): direccion guest y n.º de entradas de 8 bytes.
  gaddr exidx() const { return exidx_; }
  std::uint32_t exidx_count() const { return exidx_count_; }

 private:
  friend struct ModuleBuilder;
  std::string name_, soname_;
  gaddr base_ = 0;
  gsize size_ = 0;
  std::vector<Segment> segments_;
  std::vector<std::string> needed_;
  std::vector<SymbolInfo> symbols_;
  std::vector<ImportInfo> imports_;
  std::vector<gaddr> init_;
  bool init_done_ = false;
  gaddr exidx_ = 0;
  std::uint32_t exidx_count_ = 0;
  // tablas dinamicas (direcciones guest)
  gaddr symtab_ = 0, strtab_ = 0, hash_ = 0;
  std::uint32_t nsyms_ = 0;
};

struct LoadOptions {
  // Si true (por defecto) Load() NO ejecuta constructores; llamar a RunInit() despues,
  // cuando toda la HLE este registrada.
  bool defer_init = true;
};

// Mapea el .so, aplica relocaciones y resuelve imports (otras libs ya cargadas -> datos HLE ->
// stubs HLE). Devuelve nullptr si falla (detalle en el log). El Module vive hasta el final del
// proceso. No thread-safe: cargar desde un solo hilo antes de arrancar hilos guest.
Module* Load(const std::filesystem::path& path, const LoadOptions& opts = {});
// Igual, desde una imagen en memoria (tests).
Module* LoadFromMemory(std::string name, const void* data, std::size_t size, const LoadOptions& opts = {});

// Ejecuta los constructores pendientes de `m` en el hilo actual (Cpu::Current()).
// `progress(i, n, addr)` se invoca antes de cada uno. Lanza GuestFault/GuestExit.
void RunInit(Module& m, const std::function<void(std::size_t, std::size_t, gaddr)>& progress = {});

// Llama a JNI_OnLoad(vm, NULL) si el modulo lo exporta. Devuelve su resultado (version JNI) o
// nullopt si no existe.
std::optional<std::int32_t> CallJniOnLoad(Module& m, gaddr java_vm);

const std::vector<Module*>& Modules();
Module* FindModule(std::string_view name_or_soname);
Module* ModuleAt(gaddr addr);
// Busca un export en todas las libs cargadas (en orden de carga). 0 si no existe.
gaddr FindSymbol(std::string_view name);
// "libZombieWood.so!nombre+0x1c" (equivale a zw::debug::addr2sym, que delega aqui).
std::string addr2sym(gaddr addr);

}  // namespace zw::loader
