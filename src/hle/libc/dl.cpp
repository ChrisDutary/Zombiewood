// libdl sobre las libs cargadas por el loader y la tabla HLE.
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hle/libc/internal.h"
#include "loader/loader.h"

namespace zw::hle::libc {
namespace {

// Handles: 0x1000 + indice de modulo; 0x2000 = "lib del sistema" (tabla HLE); RTLD_DEFAULT = 0xFFFFFFFF.
constexpr gaddr kModBase = 0x1000, kHleHandle = 0x2000, kDefault = 0xFFFFFFFFu, kNext = 0xFFFFFFFEu;

void SetDlError(const std::string& s) {
  ThreadState& t = TS();
  const gaddr b = ThreadBuffer(&ThreadState::dlerror_buf, 256);
  mem::WriteCStr(b, s, 256);
  t.dlerror_set = true;
}
std::string Base(const char* p) {
  std::string s = p;
  const auto sl = s.rfind('/');
  return sl == std::string::npos ? s : s.substr(sl + 1);
}
gaddr hle_dlopen(const char* name, int) {
  if (!name) return kDefault;
  const std::string b = Base(name);
  const auto& mods = loader::Modules();
  for (std::size_t i = 0; i < mods.size(); ++i)
    if (mods[i]->name() == b || mods[i]->soname() == b) return kModBase + static_cast<gaddr>(i);
  static const char* kSys[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so", "libGLESv2.so",
                               "libGLESv1_CM.so", "libEGL.so", "libOpenSLES.so", "libandroid.so", "libz.so"};
  for (const char* s : kSys)
    if (b == s) return kHleHandle;
  SetDlError("dlopen failed: library \"" + b + "\" not found");
  return 0;
}
gaddr Hle(const char* sym) {
  if (const gaddr d = FindData(sym)) return d;
  if (IsRegistered(sym)) return StubFor(sym);
  return 0;
}
gaddr hle_dlsym(gaddr h, const char* sym) {
  if (!sym) return 0;
  gaddr r = 0;
  if (h >= kModBase && h < kModBase + loader::Modules().size()) {
    r = loader::Modules()[h - kModBase]->FindExport(sym);
  } else if (h == kHleHandle) {
    r = Hle(sym);
  } else if (h == kDefault || h == kNext) {
    r = loader::FindSymbol(sym);
    if (!r) r = Hle(sym);
  }
  if (!r) SetDlError(std::string("undefined symbol: ") + sym);
  return r;
}
int hle_dlclose(gaddr) { return 0; }
gaddr hle_dlerror() {
  ThreadState& t = TS();
  if (!t.dlerror_set) return 0;
  t.dlerror_set = false;
  return t.dlerror_buf;
}
// Dl_info {dli_fname, dli_fbase, dli_sname, dli_saddr}
int hle_dladdr(gaddr addr, gaddr info) {
  loader::Module* m = loader::ModuleAt(addr);
  if (!m) return 0;
  mem::W32(info, InternString(m->name()));
  mem::W32(info + 4, m->base());
  mem::W32(info + 8, 0);
  mem::W32(info + 12, 0);
  if (auto s = m->SymbolAt(addr & ~1u)) {
    mem::W32(info + 8, InternString(s->name));
    mem::W32(info + 12, s->addr | (s->thumb ? 1u : 0u));
  }
  return 1;
}

}  // namespace

void RegisterDl() {
  ZW_HLE("dlopen", hle_dlopen);
  ZW_HLE("dlsym", hle_dlsym);
  ZW_HLE("dlclose", hle_dlclose);
  ZW_HLE("dlerror", hle_dlerror);
  ZW_HLE("dladdr", hle_dladdr);
}

}  // namespace zw::hle::libc
