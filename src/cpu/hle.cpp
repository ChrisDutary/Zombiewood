#include "cpu/hle.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "cpu/internal.h"

namespace zw {
namespace internal {
namespace {
std::mutex g_mu;
std::once_flag g_once;
std::unique_ptr<HleEntry[]> g_entries;
std::atomic<std::uint32_t> g_count{0};
std::unordered_map<std::string, std::uint32_t> g_by_name;
std::unordered_map<std::string, gaddr> g_data;
std::atomic<int> g_policy{-1};

// Debe llamarse con g_mu tomado.
HleEntry& GetOrCreateLocked(std::string_view name) {
  auto it = g_by_name.find(std::string(name));
  if (it != g_by_name.end()) return g_entries[it->second];
  const std::uint32_t idx = g_count.load();
  if (idx >= kMaxStubs) {
    log::Error("hle: demasiados stubs (max {})", kMaxStubs);
    std::abort();
  }
  HleEntry& e = g_entries[idx];
  e.name = std::string(name);
  e.stub = GuestMemory::kThunksBase + idx * kStubSize;
  mem::W32(e.stub, 0xEF000000u | idx);  // svc #idx
  mem::W32(e.stub + 4, 0xE12FFF1Eu);    // bx lr
  g_by_name.emplace(e.name, idx);
  g_count.store(idx + 1);
  return e;
}
}  // namespace

void HleRegisterNoInit(std::string_view name, hle::RawFn fn, std::uint32_t flags) {
  std::lock_guard lk(g_mu);
  HleEntry& e = GetOrCreateLocked(name);
  e.flags.store(flags);
  e.fn.store(fn);
}

void HleEnsureInit() {
  std::call_once(g_once, [] {
    if (!GuestMemory::Init() ||
        !GuestMemory::Map(GuestMemory::kThunksBase, kMaxStubs * kStubSize, kProtRWX, "thunks")) {
      log::Error("hle: no se pudo mapear la zona de thunks");
      std::abort();
    }
    g_entries = std::make_unique<HleEntry[]>(kMaxStubs);
    {
      std::lock_guard lk(g_mu);
      GetOrCreateLocked("<call_guest return>");  // indice 0 == kReturnStub
    }
    RegisterBuiltins();
  });
}

HleEntry* HleEntryByIndex(std::uint32_t idx) { return idx < g_count.load(std::memory_order_acquire) ? &g_entries[idx] : nullptr; }

bool UnimplementedAborts() {
  int p = g_policy.load();
  if (p < 0) {
    const char* e = std::getenv("ZW_ABORT_ON_UNIMPLEMENTED");
    p = (e && *e && *e != '0') ? 1 : 0;
    g_policy.store(p);
  }
  return p == 1;
}
}  // namespace internal

namespace hle {
using namespace internal;

void Register(std::string_view name, RawFn fn, std::uint32_t flags) {
  HleEnsureInit();
  HleRegisterNoInit(name, fn, flags);
}

bool IsRegistered(std::string_view name) {
  HleEnsureInit();
  std::lock_guard lk(g_mu);
  auto it = g_by_name.find(std::string(name));
  return it != g_by_name.end() && g_entries[it->second].fn.load() != nullptr;
}

gaddr StubFor(std::string_view name) {
  HleEnsureInit();
  std::lock_guard lk(g_mu);
  return GetOrCreateLocked(name).stub;
}

std::string StubName(gaddr addr) {
  if (addr < GuestMemory::kThunksBase) return {};
  HleEntry* e = HleEntryByIndex((addr - GuestMemory::kThunksBase) / kStubSize);
  return e ? e->name : std::string();
}

gaddr DefineData(std::string_view name, gsize size, gsize align) {
  HleEnsureInit();
  std::lock_guard lk(g_mu);
  auto it = g_data.find(std::string(name));
  if (it != g_data.end()) return it->second;
  gaddr p = align > 8 ? GuestMemory::AllocAligned(size, align) : GuestMemory::Alloc(size);
  if (p) std::memset(g2h(p), 0, size);
  g_data.emplace(std::string(name), p);
  return p;
}

void DefineDataAt(std::string_view name, gaddr addr) {
  HleEnsureInit();
  std::lock_guard lk(g_mu);
  g_data[std::string(name)] = addr;
}

gaddr FindData(std::string_view name) {
  HleEnsureInit();
  std::lock_guard lk(g_mu);
  auto it = g_data.find(std::string(name));
  return it == g_data.end() ? 0 : it->second;
}

std::vector<std::string> UnimplementedImports() {
  HleEnsureInit();
  std::vector<std::string> out;
  const std::uint32_t n = g_count.load();
  for (std::uint32_t i = 1; i < n; ++i)
    if (!g_entries[i].fn.load()) out.push_back(g_entries[i].name);
  return out;
}

void SetUnimplementedPolicy(OnUnimplemented p) { g_policy.store(p == OnUnimplemented::Abort ? 1 : 0); }

std::vector<std::pair<std::string, std::uint64_t>> CallCounts() {
  HleEnsureInit();
  std::vector<std::pair<std::string, std::uint64_t>> out;
  const std::uint32_t n = g_count.load();
  for (std::uint32_t i = 1; i < n; ++i) out.emplace_back(g_entries[i].name, g_entries[i].calls.load());
  return out;
}

bool HookGuestFunction(gaddr fn, std::string_view name) {
  if (!fn) return false;
  const gaddr stub = StubFor(name);
  const bool thumb = fn & 1;
  gaddr a = fn & ~1u;
  std::uint8_t code[16];
  gsize n = 0;
  auto p16 = [&](std::uint16_t v) { std::memcpy(code + n, &v, 2); n += 2; };
  auto p32 = [&](std::uint32_t v) { std::memcpy(code + n, &v, 4); n += 4; };
  if (thumb) {
    if (a & 2) p16(0x46C0);  // nop: alinea a 4 el `bx pc`
    p16(0x4778);             // bx pc  (pasa a ARM en pc+4)
    p16(0x46C0);             // nop
  }
  p32(0xE51FF004u);  // ldr pc, [pc, #-4]
  p32(stub);
  if (!GuestMemory::IsMapped(a, n)) return false;
  const Prot old = GuestMemory::ProtOf(a);
  const Prot old2 = GuestMemory::ProtOf(a + n - 1);
  GuestMemory::Protect(a, n, kProtRWX);
  std::memcpy(g2h(a), code, n);
  if ((a >> 12) == ((a + n - 1) >> 12)) {
    GuestMemory::Protect(a, n, old);
  } else {
    GuestMemory::Protect(a, 1, old);
    GuestMemory::Protect(a + n - 1, 1, old2);
  }
  cpu::InvalidateCode(a, n);
  return true;
}

}  // namespace hle
}  // namespace zw
