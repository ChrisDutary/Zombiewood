#include "loader/loader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"

namespace zw::loader {
namespace {

#pragma pack(push, 1)
struct Ehdr {
  std::uint8_t ident[16];
  std::uint16_t type, machine;
  std::uint32_t version, entry, phoff, shoff, flags;
  std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
  std::uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct Dyn {
  std::int32_t tag;
  std::uint32_t val;
};
struct Sym {
  std::uint32_t name, value, size;
  std::uint8_t info, other;
  std::uint16_t shndx;
};
struct Rel {
  std::uint32_t offset, info;
};
#pragma pack(pop)

enum : std::uint32_t { PT_LOAD = 1, PT_DYNAMIC = 2, PT_ARM_EXIDX = 0x70000001 };
enum : std::int32_t {
  DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
  DT_INIT = 12, DT_SONAME = 14, DT_REL = 17, DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23,
  DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27,
};
enum : std::uint32_t {
  R_ARM_NONE = 0, R_ARM_ABS32 = 2, R_ARM_REL32 = 3, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23,
};
enum : std::uint8_t { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STB_GLOBAL = 1, STB_WEAK = 2 };
constexpr gsize kDataPlaceholderSize = 1024;

std::vector<Module*> g_modules;
std::once_flag g_once;

std::uint32_t ElfHash(std::string_view name) {
  std::uint32_t h = 0;
  for (unsigned char c : name) {
    h = (h << 4) + c;
    const std::uint32_t g = h & 0xF0000000u;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

gaddr hle_find_exidx(gaddr pc, GPtr<std::int32_t> pcount) {
  if (Module* m = ModuleAt(pc & ~1u)) {
    if (pcount) *pcount = static_cast<std::int32_t>(m->exidx_count());
    return m->exidx();
  }
  if (pcount) *pcount = 0;
  return 0;
}

void InitOnce() {
  std::call_once(g_once, [] {
    GuestMemory::Init();
    debug::AddSymbolizer([](gaddr addr, std::string& out) {
      if (!ModuleAt(addr & ~1u)) return false;
      out = addr2sym(addr);
      return true;
    });
    if (!hle::IsRegistered("__gnu_Unwind_Find_exidx")) ZW_HLE_FAST("__gnu_Unwind_Find_exidx", hle_find_exidx);
  });
}
}  // namespace

struct ModuleBuilder {
  static Module* Build(std::string name, const std::uint8_t* file, std::size_t fsize);
  static void MarkInitDone(Module& m) { m.init_done_ = true; }
};

Module* ModuleBuilder::Build(std::string name, const std::uint8_t* file, std::size_t fsize) {
  InitOnce();
  auto fail = [&](const std::string& why) -> Module* {
    log::Error("loader: {}: {}", name, why);
    return nullptr;
  };
  if (fsize < sizeof(Ehdr)) return fail("fichero demasiado pequeno");
  Ehdr eh;
  std::memcpy(&eh, file, sizeof eh);
  if (std::memcmp(eh.ident, "\x7f" "ELF", 4) != 0) return fail("no es un ELF");
  if (eh.ident[4] != 1 || eh.ident[5] != 1) return fail("no es ELF32 little-endian");
  if (eh.machine != 40) return fail(std::format("e_machine = {} (se esperaba EM_ARM = 40)", eh.machine));
  if (eh.type != 3) return fail("no es ET_DYN");
  if (eh.phentsize != sizeof(Phdr) || std::uint64_t(eh.phoff) + std::uint64_t(eh.phnum) * sizeof(Phdr) > fsize)
    return fail("cabeceras de programa invalidas");

  std::vector<Phdr> ph(eh.phnum);
  std::memcpy(ph.data(), file + eh.phoff, eh.phnum * sizeof(Phdr));
  std::uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (const Phdr& p : ph) {
    if (p.type != PT_LOAD) continue;
    if (std::uint64_t(p.offset) + p.filesz > fsize || p.filesz > p.memsz) return fail("PT_LOAD fuera del fichero");
    lo = std::min(lo, p.vaddr & ~0xFFFu);
    hi = std::max(hi, (p.vaddr + p.memsz + 0xFFFu) & ~0xFFFu);
  }
  if (lo >= hi) return fail("sin segmentos PT_LOAD");

  const gsize span = hi - lo;
  const gaddr map = GuestMemory::MapAnywhere(Zone::Libs, span, kProtRW, name, 0x10000);
  if (!map) return fail("sin espacio en la zona de libs");
  auto m = std::make_unique<Module>();
  m->name_ = name;
  m->base_ = map - lo;
  m->size_ = span + lo;
  const gaddr base = m->base_;

  gaddr dyn = 0;
  for (const Phdr& p : ph) {
    if (p.type == PT_LOAD) {
      std::memcpy(g2h(base + p.vaddr), file + p.offset, p.filesz);
    } else if (p.type == PT_DYNAMIC) {
      dyn = base + p.vaddr;
    } else if (p.type == PT_ARM_EXIDX) {
      m->exidx_ = base + p.vaddr;
      m->exidx_count_ = p.memsz / 8;
    }
  }
  if (!dyn) return fail("sin PT_DYNAMIC");

  gaddr rel = 0, jmprel = 0, init = 0, init_array = 0;
  std::uint32_t relsz = 0, pltrelsz = 0, init_arraysz = 0, soname_off = 0xFFFFFFFFu, pltrel = 17;
  std::vector<std::uint32_t> needed_off;
  bool has_rela = false;
  for (gaddr d = dyn;; d += sizeof(Dyn)) {
    if (!m->Contains(d)) return fail("seccion .dynamic sin DT_NULL");
    const Dyn e = mem::Read<Dyn>(d);
    if (e.tag == DT_NULL) break;
    switch (e.tag) {
      case DT_NEEDED: needed_off.push_back(e.val); break;
      case DT_HASH: m->hash_ = base + e.val; break;
      case DT_STRTAB: m->strtab_ = base + e.val; break;
      case DT_SYMTAB: m->symtab_ = base + e.val; break;
      case DT_REL: rel = base + e.val; break;
      case DT_RELSZ: relsz = e.val; break;
      case DT_JMPREL: jmprel = base + e.val; break;
      case DT_PLTRELSZ: pltrelsz = e.val; break;
      case DT_PLTREL: pltrel = e.val; break;
      case DT_RELA: has_rela = true; break;
      case DT_INIT: init = base + e.val; break;
      case DT_INIT_ARRAY: init_array = base + e.val; break;
      case DT_INIT_ARRAYSZ: init_arraysz = e.val; break;
      case DT_SONAME: soname_off = e.val; break;
      default: break;
    }
  }
  if (has_rela || pltrel != 17) return fail("relocaciones RELA no soportadas (ARM usa REL)");
  if (!m->symtab_ || !m->strtab_) return fail("sin DT_SYMTAB/DT_STRTAB");
  if (!m->hash_) return fail("sin DT_HASH (DT_GNU_HASH no soportado)");
  m->nsyms_ = mem::R32(m->hash_ + 4);  // nchain == n.º de simbolos
  auto str = [&](std::uint32_t off) { return mem::ReadCStr(m->strtab_ + off, 4096); };
  if (soname_off != 0xFFFFFFFFu) m->soname_ = str(soname_off);
  for (auto off : needed_off) m->needed_.push_back(str(off));

  // --- tabla de simbolos propia (para addr2sym) ---
  m->symbols_.reserve(m->nsyms_);
  for (std::uint32_t i = 1; i < m->nsyms_; ++i) {
    const Sym s = mem::Read<Sym>(m->symtab_ + i * sizeof(Sym));
    const std::uint8_t type = s.info & 0xF;
    if (s.shndx == 0 || s.name == 0 || (type != STT_FUNC && type != STT_OBJECT)) continue;
    SymbolInfo si;
    si.name = str(s.name);
    si.is_func = type == STT_FUNC;
    si.thumb = si.is_func && (s.value & 1);
    si.addr = base + (si.is_func ? (s.value & ~1u) : s.value);
    si.size = s.size;
    m->symbols_.push_back(std::move(si));
  }
  std::sort(m->symbols_.begin(), m->symbols_.end(), [](const SymbolInfo& a, const SymbolInfo& b) { return a.addr < b.addr; });

  // --- relocaciones ---
  std::vector<gaddr> cache(m->nsyms_, 0);
  std::vector<std::uint8_t> cached(m->nsyms_, 0);
  auto resolve = [&](std::uint32_t idx) -> gaddr {
    if (idx >= m->nsyms_) return 0;
    if (cached[idx]) return cache[idx];
    const Sym s = mem::Read<Sym>(m->symtab_ + idx * sizeof(Sym));
    gaddr val = 0;
    if (s.shndx != 0) {
      val = base + s.value;
    } else {
      ImportInfo imp;
      imp.name = str(s.name);
      const std::uint8_t type = s.info & 0xF;
      imp.is_data = type == STT_OBJECT;
      imp.weak = (s.info >> 4) == STB_WEAK;
      for (Module* other : g_modules) {
        if (gaddr a = other->FindExport(imp.name)) {
          val = a;
          imp.via = ImportInfo::Via::Module;
          break;
        }
      }
      if (!val) {
        const gaddr data = hle::FindData(imp.name);
        if (data) {
          val = data;
          imp.via = ImportInfo::Via::HleData;
          imp.is_data = true;
        } else if (imp.is_data) {
          val = hle::DefineData(imp.name, kDataPlaceholderSize);
          imp.via = ImportInfo::Via::HleDataPlaceholder;
          log::Warn("loader: {}: dato importado '{}' sin definir; se usa un bloque de {} bytes a cero", name, imp.name,
                    kDataPlaceholderSize);
        } else {
          val = hle::StubFor(imp.name);
          imp.via = ImportInfo::Via::HleStub;
        }
      }
      imp.addr = val;
      m->imports_.push_back(std::move(imp));
    }
    cache[idx] = val;
    cached[idx] = 1;
    return val;
  };

  std::size_t counts[5] = {};
  bool bad = false;
  auto apply = [&](gaddr table, std::uint32_t bytes) {
    for (std::uint32_t off = 0; off + sizeof(Rel) <= bytes; off += sizeof(Rel)) {
      const Rel r = mem::Read<Rel>(table + off);
      const std::uint32_t type = r.info & 0xFF, sym = r.info >> 8;
      const gaddr P = base + r.offset;
      if (type == R_ARM_NONE) continue;
      if (!m->Contains(P) || !m->Contains(P + 3)) {
        log::Error("loader: {}: relocacion fuera del modulo (offset 0x{:x})", name, r.offset);
        bad = true;
        continue;
      }
      switch (type) {
        case R_ARM_RELATIVE: mem::W32(P, mem::R32(P) + base); counts[0]++; break;
        case R_ARM_ABS32: mem::W32(P, mem::R32(P) + resolve(sym)); counts[1]++; break;
        case R_ARM_GLOB_DAT: mem::W32(P, resolve(sym)); counts[2]++; break;
        case R_ARM_JUMP_SLOT: mem::W32(P, resolve(sym)); counts[3]++; break;
        case R_ARM_REL32: mem::W32(P, mem::R32(P) + resolve(sym) - P); counts[4]++; break;
        default:
          log::Error("loader: {}: tipo de relocacion no soportado {} (offset 0x{:x})", name, type, r.offset);
          bad = true;
      }
    }
  };
  if (rel) apply(rel, relsz);
  if (jmprel) apply(jmprel, pltrelsz);
  if (bad) return fail("relocaciones invalidas");

  // --- constructores ---
  if (init) m->init_.push_back(init);
  for (std::uint32_t off = 0; off + 4 <= init_arraysz; off += 4) {
    const gaddr f = mem::R32(init_array + off);
    if (f != 0 && f != 0xFFFFFFFFu) m->init_.push_back(f);
  }

  // --- permisos por pagina (union de los segmentos que tocan cada pagina) ---
  std::map<gaddr, std::uint32_t> page_prot;
  for (gaddr a = map; a < map + span; a += 0x1000) page_prot[a] = kProtNone;
  for (const Phdr& p : ph) {
    if (p.type != PT_LOAD || p.memsz == 0) continue;
    std::uint32_t prot = 0;
    if (p.flags & 4) prot |= kProtRead;
    if (p.flags & 2) prot |= kProtWrite;
    if (p.flags & 1) prot |= kProtExec | kProtRead;
    m->segments_.push_back(Segment{base + p.vaddr, p.memsz, static_cast<Prot>(prot)});
    for (gaddr a = (base + p.vaddr) & ~0xFFFu; a < base + p.vaddr + p.memsz; a += 0x1000) page_prot[a] |= prot;
  }
  for (auto it = page_prot.begin(); it != page_prot.end();) {
    auto run = it;
    gsize len = 0;
    while (run != page_prot.end() && run->second == it->second) {
      len += 0x1000;
      ++run;
    }
    GuestMemory::Protect(it->first, len, static_cast<Prot>(it->second));
    it = run;
  }
  cpu::InvalidateCode(map, span);

  for (auto& imp : m->imports_)
    if (imp.via == ImportInfo::Via::HleStub) imp.has_handler = hle::IsRegistered(imp.name);
  std::sort(m->imports_.begin(), m->imports_.end(), [](const ImportInfo& a, const ImportInfo& b) { return a.name < b.name; });

  log::Info("loader: {} (soname {}) cargado en 0x{:08x}-0x{:08x}; relocs: {} RELATIVE, {} ABS32, {} GLOB_DAT, {} JUMP_SLOT, "
            "{} REL32; {} simbolos, {} imports, {} constructores, exidx {} entradas",
            m->name_, m->soname_.empty() ? "-" : m->soname_, map, map + span, counts[0], counts[1], counts[2], counts[3],
            counts[4], m->symbols_.size(), m->imports_.size(), m->init_.size(), m->exidx_count_);
  Module* raw = m.release();
  g_modules.push_back(raw);
  return raw;
}

gaddr Module::FindExport(std::string_view name) const {
  if (!hash_) return 0;
  const std::uint32_t nbucket = mem::R32(hash_);
  if (!nbucket) return 0;
  const gaddr buckets = hash_ + 8, chains = buckets + 4 * nbucket;
  for (std::uint32_t i = mem::R32(buckets + 4 * (ElfHash(name) % nbucket)); i != 0 && i < nsyms_; i = mem::R32(chains + 4 * i)) {
    const Sym s = mem::Read<Sym>(symtab_ + i * sizeof(Sym));
    if (s.shndx == 0) continue;
    const std::uint8_t bind = s.info >> 4;
    if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
    const char* n = g2h<const char>(strtab_ + s.name);
    if (name.size() == std::strlen(n) && std::memcmp(n, name.data(), name.size()) == 0) return base_ + s.value;
  }
  return 0;
}

std::optional<SymbolInfo> Module::SymbolAt(gaddr addr) const {
  addr &= ~1u;
  auto it = std::upper_bound(symbols_.begin(), symbols_.end(), addr, [](gaddr a, const SymbolInfo& s) { return a < s.addr; });
  // Varios simbolos pueden compartir direccion; preferir uno que cubra `addr`.
  while (it != symbols_.begin()) {
    --it;
    if (addr - it->addr < std::max<gsize>(it->size, 1)) return *it;
    if (it->addr + 0x10000 < addr) break;  // demasiado lejos: no hay candidato
    if (it->is_func && it->size != 0) break;  // la funcion anterior termina antes de addr
  }
  return std::nullopt;
}

Module* Load(const std::filesystem::path& path, const LoadOptions& opts) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    log::Error("loader: no se puede abrir {}", path.string());
    return nullptr;
  }
  const auto size = static_cast<std::size_t>(f.tellg());
  std::vector<std::uint8_t> data(size);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  if (!f) {
    log::Error("loader: error leyendo {}", path.string());
    return nullptr;
  }
  return LoadFromMemory(path.filename().string(), data.data(), data.size(), opts);
}

Module* LoadFromMemory(std::string name, const void* data, std::size_t size, const LoadOptions& opts) {
  Module* m = ModuleBuilder::Build(std::move(name), static_cast<const std::uint8_t*>(data), size);
  if (m && !opts.defer_init) RunInit(*m);
  return m;
}

void RunInit(Module& m, const std::function<void(std::size_t, std::size_t, gaddr)>& progress) {
  if (m.init_done()) return;
  const char* e = std::getenv("ZW_TRACE_INIT");
  const bool trace = e && *e && *e != '0';
  const auto& fns = m.init_functions();
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (progress) progress(i, fns.size(), fns[i]);
    if (trace) log::Info("init {}/{}: {}", i + 1, fns.size(), addr2sym(fns[i]));
    call_guest<void>(fns[i]);
  }
  ModuleBuilder::MarkInitDone(m);
}

std::optional<std::int32_t> CallJniOnLoad(Module& m, gaddr java_vm) {
  const gaddr fn = m.FindExport("JNI_OnLoad");
  if (!fn) return std::nullopt;
  return call_guest<std::int32_t>(fn, java_vm, 0);
}

const std::vector<Module*>& Modules() { return g_modules; }

Module* FindModule(std::string_view n) {
  for (Module* m : g_modules)
    if (m->name() == n || m->soname() == n) return m;
  return nullptr;
}

Module* ModuleAt(gaddr addr) {
  for (Module* m : g_modules)
    if (m->Contains(addr)) return m;
  return nullptr;
}

gaddr FindSymbol(std::string_view name) {
  for (Module* m : g_modules)
    if (gaddr a = m->FindExport(name)) return a;
  return 0;
}

std::string addr2sym(gaddr addr) {
  Module* m = ModuleAt(addr & ~1u);
  if (!m) return debug::addr2sym(addr);
  if (auto s = m->SymbolAt(addr)) {
    const gaddr off = (addr & ~1u) - s->addr;
    return off ? std::format("{}!{}+0x{:x}", m->name(), s->name, off) : std::format("{}!{}", m->name(), s->name);
  }
  return std::format("{}+0x{:x}", m->name(), (addr & ~1u) - m->base());
}

}  // namespace zw::loader
