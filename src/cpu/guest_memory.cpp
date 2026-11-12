#include "cpu/guest_memory.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "cpu/internal.h"

namespace zw {

std::uint8_t* g_membase = nullptr;
namespace internal {
std::uint8_t* g_page_prot = nullptr;
std::uint8_t** g_page_table = nullptr;
}

namespace {
using internal::g_page_prot;
using internal::g_page_table;

void SetPages(gaddr start, std::size_t len, Prot prot) {
  std::memset(g_page_prot + (start >> 12), static_cast<int>(prot), len >> 12);
  std::uint8_t* v = prot != kProtNone ? g_membase : nullptr;
  for (std::size_t p = start >> 12, e = p + (len >> 12); p < e; ++p) g_page_table[p] = v;
}

constexpr gaddr kPage = GuestMemory::kPageSize;
constexpr std::uint64_t kSpace = 1ull << 32;

std::once_flag g_init_once;
bool g_init_ok = false;
std::mutex g_page_mu;

struct ZoneAlloc {
  gaddr begin, end;
  std::map<gaddr, gsize> free_;  // inicio -> tamano
  std::map<gaddr, gsize> used_;
};
ZoneAlloc g_zones[4] = {
    {GuestMemory::kLibsBase, GuestMemory::kLibsEnd, {}, {}},
    {GuestMemory::kHeapBase, GuestMemory::kHeapEnd, {}, {}},
    {GuestMemory::kStacksBase, GuestMemory::kStacksEnd, {}, {}},
    {GuestMemory::kThunksBase, GuestMemory::kThunksEnd, {}, {}},
};

gsize PageUp(gsize v) { return (v + kPage - 1) & ~(kPage - 1); }

DWORD HostProt(Prot p) {
  if (p & kProtWrite) return PAGE_READWRITE;
  if (p & (kProtRead | kProtExec)) return PAGE_READONLY;
  return PAGE_NOACCESS;
}

bool MapLocked(gaddr addr, gsize size, Prot prot) {
  const gaddr start = addr & ~(kPage - 1);
  const std::uint64_t end64 = (std::uint64_t(addr) + size + kPage - 1) & ~std::uint64_t(kPage - 1);
  if (size == 0 || end64 > kSpace) return false;
  const std::size_t len = static_cast<std::size_t>(end64 - start);
  bool was_mapped = false;
  for (std::uint64_t a = start; a < end64; a += kPage) was_mapped |= g_page_prot[a >> 12] != 0;
  void* p = VirtualAlloc(g_membase + start, len, MEM_COMMIT, PAGE_READWRITE);
  if (!p) {
    log::Error("GuestMemory::Map: VirtualAlloc(0x{:08x}, 0x{:x}) fallo ({})", start, len, GetLastError());
    return false;
  }
  if (was_mapped) std::memset(g_membase + start, 0, len);
  if (HostProt(prot) != PAGE_READWRITE) {
    DWORD old;
    VirtualProtect(g_membase + start, len, HostProt(prot), &old);
  }
  SetPages(start, len, prot);
  return true;
}

bool UnmapLocked(gaddr addr, gsize size) {
  const gaddr start = addr & ~(kPage - 1);
  const std::uint64_t end64 = (std::uint64_t(addr) + size + kPage - 1) & ~std::uint64_t(kPage - 1);
  if (size == 0 || end64 > kSpace) return false;
  const std::size_t len = static_cast<std::size_t>(end64 - start);
  VirtualFree(g_membase + start, len, MEM_DECOMMIT);
  SetPages(start, len, kProtNone);
  return true;
}

gaddr ZoneTake(ZoneAlloc& z, gsize size, gsize align) {
  for (auto it = z.free_.begin(); it != z.free_.end(); ++it) {
    const gaddr s = it->first;
    const gsize n = it->second;
    const std::uint64_t a = (std::uint64_t(s) + align - 1) & ~std::uint64_t(align - 1);
    if (a + size > std::uint64_t(s) + n) continue;
    const gaddr res = static_cast<gaddr>(a);
    const gsize head = res - s;
    const gsize tail = static_cast<gsize>(std::uint64_t(s) + n - (a + size));
    z.free_.erase(it);
    if (head) z.free_[s] = head;
    if (tail) z.free_[res + size] = tail;
    z.used_[res] = size;
    return res;
  }
  return 0;
}

void ZoneGive(ZoneAlloc& z, gaddr addr, gsize size) {
  auto [it, ok] = z.free_.emplace(addr, size);
  if (!ok) return;
  auto next = std::next(it);
  if (next != z.free_.end() && it->first + it->second == next->first) {
    it->second += next->second;
    z.free_.erase(next);
  }
  if (it != z.free_.begin()) {
    auto prev = std::prev(it);
    if (prev->first + prev->second == it->first) {
      prev->second += it->second;
      z.free_.erase(it);
    }
  }
}

// ---------------------------------------------------------------------------
// Heap: clases de tamano con listas libres + bloques grandes por paginas.
constexpr std::uint32_t kHdr = 8;
constexpr std::uint32_t kMagicLive = 0xA110C000u, kMagicFree = 0xF4EE0000u, kMagicMask = 0xFFFFF000u;
constexpr gsize kSmallMax = 256u << 10;  // por encima -> paginas

struct Hdr {
  std::uint32_t tag;   // magic | clase
  std::uint32_t size;  // tamano pedido
};

constexpr int kLinear = 64;  // clases 16..1024 de 16 en 16
int ClassOf(gsize n) {
  if (n <= 1024) return n <= 16 ? 0 : static_cast<int>((n + 15) / 16 - 1);
  // geometricas: 4 pasos por duplicacion a partir de 1024
  int msb = static_cast<int>(std::bit_width(n - 1)) - 1;  // n-1 en [2^msb, 2^(msb+1))
  const gsize base = gsize(1) << msb;
  const gsize step = base / 4;
  const int sub = static_cast<int>((n - 1 - base) / step);  // 0..3
  return kLinear + (msb - 10) * 4 + sub;
}
gsize ClassSize(int c) {
  if (c < kLinear) return gsize(c + 1) * 16;
  const int g = c - kLinear;
  const int msb = 10 + g / 4;
  const gsize base = gsize(1) << msb;
  return base + (base / 4) * (g % 4 + 1);
}
constexpr int kNumClasses = kLinear + (18 - 10) * 4;  // hasta 256 KiB

std::mutex g_heap_mu;
gaddr g_free_list[kNumClasses] = {};
std::unordered_map<gaddr, gsize> g_large;  // user ptr -> tamano pedido
HeapStats g_stats;

bool Refill(int c) {
  const gsize stride = ClassSize(c) + kHdr;
  gsize chunk = PageUp(std::max<gsize>(64u << 10, stride * 8));
  gaddr base;
  {
    std::lock_guard lk(g_page_mu);
    base = ZoneTake(g_zones[static_cast<int>(Zone::Heap)], chunk, kPage);
    if (!base || !MapLocked(base, chunk, kProtRW)) return false;
  }
  g_stats.bytes_mapped += chunk;
  for (gsize off = 0; off + stride <= chunk; off += stride) {
    const gaddr user = base + off + kHdr;
    mem::Write<Hdr>(user - kHdr, Hdr{kMagicFree | std::uint32_t(c), 0});
    mem::W32(user, g_free_list[c]);
    g_free_list[c] = user;
  }
  return true;
}

gaddr LargeAlloc(gsize size, gsize align) {
  const gsize len = PageUp(std::max<gsize>(size, 1));
  if (len < size) return 0;
  std::lock_guard lk(g_page_mu);
  gaddr a = ZoneTake(g_zones[static_cast<int>(Zone::Heap)], len, std::max(align, kPage));
  if (!a) return 0;
  if (!MapLocked(a, len, kProtRW)) {
    g_zones[static_cast<int>(Zone::Heap)].used_.erase(a);
    ZoneGive(g_zones[static_cast<int>(Zone::Heap)], a, len);
    return 0;
  }
  return a;
}
}  // namespace

// ---------------------------------------------------------------------------
bool GuestMemory::Init() {
  std::call_once(g_init_once, [] {
    // +64 KiB de margen para que un acceso que cruce el final de los 4 GiB caiga en zona reservada.
    void* p = VirtualAlloc(nullptr, kSpace + 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
      log::Error("GuestMemory: no se pudieron reservar 4 GiB de espacio de direcciones ({})", GetLastError());
      return;
    }
    g_membase = static_cast<std::uint8_t*>(p);
    internal::g_page_prot = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 1u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    for (auto& z : g_zones) z.free_[z.begin] = z.end - z.begin;
    internal::g_page_table = static_cast<std::uint8_t**>(
        VirtualAlloc(nullptr, (1u << 20) * sizeof(std::uint8_t*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_init_ok = internal::g_page_prot != nullptr && internal::g_page_table != nullptr;
    log::Debug("GuestMemory: base host = {}", static_cast<void*>(g_membase));
  });
  return g_init_ok;
}

bool GuestMemory::IsInitialized() { return g_init_ok; }

bool GuestMemory::Map(gaddr addr, gsize size, Prot prot, std::string_view) {
  if (!Init()) return false;
  std::lock_guard lk(g_page_mu);
  return MapLocked(addr, size, prot);
}

gaddr GuestMemory::MapAnywhere(Zone zone, gsize size, Prot prot, std::string_view tag, gsize align) {
  if (!Init() || size == 0) return 0;
  const gsize len = PageUp(size);
  if (len < size) return 0;
  if (align < kPage) align = kPage;
  std::lock_guard lk(g_page_mu);
  auto& z = g_zones[static_cast<int>(zone)];
  gaddr a = ZoneTake(z, len, align);
  if (!a) {
    log::Error("GuestMemory::MapAnywhere: zona {} sin espacio para 0x{:x} bytes ({})", static_cast<int>(zone), len, tag);
    return 0;
  }
  if (!MapLocked(a, len, prot)) {
    z.used_.erase(a);
    ZoneGive(z, a, len);
    return 0;
  }
  return a;
}

bool GuestMemory::Unmap(gaddr addr, gsize size) {
  if (!g_init_ok) return false;
  std::lock_guard lk(g_page_mu);
  for (auto& z : g_zones) {
    auto it = z.used_.find(addr);
    if (it != z.used_.end()) {
      const gsize len = it->second;
      z.used_.erase(it);
      UnmapLocked(addr, len);
      ZoneGive(z, addr, len);
      return true;
    }
  }
  return UnmapLocked(addr, size);
}

bool GuestMemory::Protect(gaddr addr, gsize size, Prot prot) {
  if (!g_init_ok || size == 0) return false;
  std::lock_guard lk(g_page_mu);
  const gaddr start = addr & ~(kPage - 1);
  const std::uint64_t end64 = (std::uint64_t(addr) + size + kPage - 1) & ~std::uint64_t(kPage - 1);
  if (end64 > kSpace) return false;
  for (std::uint64_t a = start; a < end64; a += kPage)
    if (!g_page_prot[a >> 12] && prot != kProtNone) return false;  // no mapeado
  DWORD old;
  if (!VirtualProtect(g_membase + start, static_cast<std::size_t>(end64 - start), HostProt(prot), &old)) return false;
  // kProtNone sobre una pagina mapeada la deja como guarda: sigue "reservada" pero inaccesible.
  SetPages(start, static_cast<std::size_t>(end64 - start), prot);
  return true;
}

bool GuestMemory::IsMapped(gaddr addr, gsize size) {
  if (!g_init_ok) return false;
  if (size == 0) size = 1;
  const std::uint64_t end64 = std::uint64_t(addr) + size;
  if (end64 > kSpace) return false;
  for (std::uint64_t p = addr >> 12; p <= ((end64 - 1) >> 12); ++p)
    if (!g_page_prot[p]) return false;
  return true;
}

Prot GuestMemory::ProtOf(gaddr addr) { return g_init_ok ? static_cast<Prot>(g_page_prot[addr >> 12]) : kProtNone; }

bool GuestMemory::Contains(const void* p) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  return g_membase && b >= g_membase && b < g_membase + kSpace;
}

gaddr h2g(const void* p) {
  if (!p) return 0;
  if (!GuestMemory::Contains(p)) {
    log::Error("h2g: el puntero host {} no pertenece a la memoria guest", p);
    return 0;
  }
  return static_cast<gaddr>(static_cast<const std::uint8_t*>(p) - g_membase);
}

// --- heap --------------------------------------------------------------------
gaddr GuestMemory::Alloc(gsize size) {
  if (!Init()) return 0;
  if (size > kSmallMax) {
    gaddr a = LargeAlloc(size, kPage);
    if (!a) return 0;
    std::lock_guard lk(g_heap_mu);
    g_large[a] = size;
    g_stats.bytes_in_use += size;
    g_stats.bytes_mapped += PageUp(size);
    g_stats.alloc_count++;
    return a;
  }
  const int c = ClassOf(size);
  std::lock_guard lk(g_heap_mu);
  if (!g_free_list[c] && !Refill(c)) return 0;
  const gaddr user = g_free_list[c];
  g_free_list[c] = mem::R32(user);
  mem::Write<Hdr>(user - kHdr, Hdr{kMagicLive | std::uint32_t(c), size});
  g_stats.bytes_in_use += size;
  g_stats.alloc_count++;
  return user;
}

gaddr GuestMemory::Calloc(gsize count, gsize size) {
  const std::uint64_t total = std::uint64_t(count) * size;
  if (total > 0xFFFFFFFFull) return 0;
  gaddr p = Alloc(static_cast<gsize>(total));
  if (p && total) std::memset(g_membase + p, 0, static_cast<std::size_t>(total));
  return p;
}

gsize GuestMemory::UsableSize(gaddr p) {
  if (!p || !g_init_ok) return 0;
  if ((p & (kPage - 1)) == 0) {
    std::lock_guard lk(g_heap_mu);
    auto it = g_large.find(p);
    if (it != g_large.end()) return PageUp(it->second);
  }
  if (!IsMapped(p - kHdr, kHdr)) return 0;
  const Hdr h = mem::Read<Hdr>(p - kHdr);
  if ((h.tag & kMagicMask) != kMagicLive) return 0;
  return ClassSize(static_cast<int>(h.tag & ~kMagicMask));
}

void GuestMemory::Free(gaddr p) {
  if (!p || !g_init_ok) return;
  if ((p & (kPage - 1)) == 0) {
    std::unique_lock lk(g_heap_mu);
    auto it = g_large.find(p);
    if (it != g_large.end()) {
      const gsize size = it->second;
      g_large.erase(it);
      g_stats.bytes_in_use -= size;
      g_stats.bytes_mapped -= PageUp(size);
      g_stats.alloc_count--;
      lk.unlock();
      Unmap(p, PageUp(size));
      return;
    }
  }
  if ((p & 7) || !IsMapped(p - kHdr, kHdr)) {
    log::Error("GuestMemory::Free(0x{:08x}): puntero invalido", p);
    return;
  }
  std::lock_guard lk(g_heap_mu);
  const Hdr h = mem::Read<Hdr>(p - kHdr);
  if ((h.tag & kMagicMask) != kMagicLive || (h.tag & ~kMagicMask) >= std::uint32_t(kNumClasses)) {
    log::Error("GuestMemory::Free(0x{:08x}): {}", p,
               (h.tag & kMagicMask) == kMagicFree ? "doble free" : "puntero que no es del heap");
    return;
  }
  const int c = static_cast<int>(h.tag & ~kMagicMask);
  mem::Write<Hdr>(p - kHdr, Hdr{kMagicFree | std::uint32_t(c), 0});
  mem::W32(p, g_free_list[c]);
  g_free_list[c] = p;
  g_stats.bytes_in_use -= h.size;
  g_stats.alloc_count--;
}

gaddr GuestMemory::Realloc(gaddr p, gsize size) {
  if (!p) return Alloc(size);
  const gsize usable = UsableSize(p);
  if (!usable) {
    log::Error("GuestMemory::Realloc(0x{:08x}): puntero invalido", p);
    return 0;
  }
  bool large = false;
  if ((p & (kPage - 1)) == 0) {
    std::lock_guard lk(g_heap_mu);
    large = g_large.count(p) != 0;
  }
  if (!large && size <= usable && size <= kSmallMax) {
    std::lock_guard lk(g_heap_mu);
    Hdr h = mem::Read<Hdr>(p - kHdr);
    g_stats.bytes_in_use += size;
    g_stats.bytes_in_use -= h.size;
    h.size = size;
    mem::Write<Hdr>(p - kHdr, h);
    return p;
  }
  gsize old_size;
  if (large) {
    std::lock_guard lk(g_heap_mu);
    old_size = g_large[p];
    if (size > kSmallMax && PageUp(size) == PageUp(old_size)) {
      g_stats.bytes_in_use += size;
      g_stats.bytes_in_use -= old_size;
      g_large[p] = size;
      return p;
    }
  } else {
    old_size = mem::Read<Hdr>(p - kHdr).size;
  }
  gaddr n = Alloc(size);
  if (!n) return 0;
  std::memcpy(g_membase + n, g_membase + p, std::min(old_size, size));
  Free(p);
  return n;
}

gaddr GuestMemory::AllocAligned(gsize size, gsize align) {
  if (align <= 8) return Alloc(size);
  if (!Init() || (align & (align - 1))) return 0;
  gaddr a = LargeAlloc(size, align);
  if (!a) return 0;
  std::lock_guard lk(g_heap_mu);
  g_large[a] = std::max<gsize>(size, 1);
  g_stats.bytes_in_use += std::max<gsize>(size, 1);
  g_stats.bytes_mapped += PageUp(std::max<gsize>(size, 1));
  g_stats.alloc_count++;
  return a;
}

HeapStats GuestMemory::GetHeapStats() {
  std::lock_guard lk(g_heap_mu);
  return g_stats;
}

// --- mem ---------------------------------------------------------------------
namespace mem {
std::string ReadCStr(gaddr a, gsize max_len) {
  if (!a) return {};
  const char* s = reinterpret_cast<const char*>(g_membase + a);
  return std::string(s, strnlen(s, max_len));
}
gsize WriteCStr(gaddr dst, std::string_view s, gsize cap) {
  if (!dst || cap == 0) return 0;
  const gsize n = static_cast<gsize>(std::min<std::size_t>(s.size(), cap - 1));
  std::memcpy(g_membase + dst, s.data(), n);
  g_membase[dst + n] = 0;
  return n;
}
gaddr StrDup(std::string_view s) {
  gaddr p = GuestMemory::Alloc(static_cast<gsize>(s.size() + 1));
  if (p) WriteCStr(p, s, static_cast<gsize>(s.size() + 1));
  return p;
}
gaddr Dup(const void* data, gsize size) {
  gaddr p = GuestMemory::Alloc(size);
  if (p && size) std::memcpy(g_membase + p, data, size);
  return p;
}
}  // namespace mem

}  // namespace zw
