#include "loader/load_only.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "loader/loader.h"

namespace zw::loader {
namespace bootstrap {
namespace {
constexpr int kJniSlots = 232;
gaddr g_vm = 0, g_env = 0;
std::mutex g_mu;
std::map<int, int> g_slot_calls;

const char* SlotName(int s) {
  switch (s) {
    case 4: return "GetVersion";
    case 6: return "FindClass";
    case 15: return "ExceptionOccurred";
    case 16: return "ExceptionDescribe";
    case 17: return "ExceptionClear";
    case 19: return "PushLocalFrame";
    case 20: return "PopLocalFrame";
    case 21: return "NewGlobalRef";
    case 22: return "DeleteGlobalRef";
    case 23: return "DeleteLocalRef";
    case 31: return "GetObjectClass";
    case 33: return "GetMethodID";
    case 94: return "GetFieldID";
    case 113: return "GetStaticMethodID";
    case 144: return "GetStaticFieldID";
    case 145: return "GetStaticObjectField";
    case 167: return "NewStringUTF";
    case 168: return "GetStringUTFLength";
    case 169: return "GetStringUTFChars";
    case 170: return "ReleaseStringUTFChars";
    case 219: return "GetJavaVM";
    case 228: return "ExceptionCheck";
    default: return "";
  }
}

std::string SafeStr(gaddr a) { return a && GuestMemory::IsMapped(a) ? mem::ReadCStr(a, 256) : std::string("<null>"); }

// Handler generico de todos los slots de JNIEnv: el n.º de slot sale del nombre del stub.
void jni_generic(Cpu& c) {
  const int slot = std::atoi(std::strrchr(c.current_import(), '_') + 1);
  bool first;
  {
    std::lock_guard lk(g_mu);
    first = g_slot_calls[slot]++ == 0;
  }
  const std::uint32_t a1 = c.reg(1), a2 = c.reg(2), a3 = c.reg(3);
  std::uint32_t ret = 0;
  std::string detail;
  switch (slot) {
    case 4: ret = 0x10006; break;
    case 6: ret = 0x00C10000u + slot; detail = SafeStr(a1); break;
    case 21: case 31: ret = a1 ? a1 : 0x00C20000u; break;
    case 33: case 113: case 94: case 144:
      ret = 0x00D00000u + slot;
      detail = SafeStr(a2) + " " + SafeStr(a3);
      break;
    case 167: ret = 0x00E00000u; detail = SafeStr(a1); break;
    case 169: {
      static gaddr empty = mem::StrDup("");
      ret = empty;
      if (a2) mem::W8(a2, 0);
      break;
    }
    case 219: if (a1) mem::W32(a1, g_vm); ret = 0; break;
    default: break;
  }
  if (first || !detail.empty())
    log::Info("[jni falso] JNIEnv slot {} {}({}) -> 0x{:x}  (lr = {})", slot, SlotName(slot), detail, ret, debug::addr2sym(c.lr()));
  c.reg(0) = ret;
  c.reg(1) = 0;
}

int vm_get_env(gaddr, GPtr<std::uint32_t> penv, int version) {
  log::Info("[jni falso] JavaVM::GetEnv(version 0x{:x})", static_cast<unsigned>(version));
  if (penv) *penv = g_env;
  return 0;
}
int vm_attach(gaddr, GPtr<std::uint32_t> penv, gaddr) {
  if (penv) *penv = g_env;
  return 0;
}
int vm_zero() { return 0; }
}  // namespace

gaddr FakeJavaVM() {
  if (g_vm) return g_vm;
  const gaddr env_tab = GuestMemory::Calloc(kJniSlots, 4);
  for (int i = 4; i < kJniSlots; ++i) {
    const std::string name = "JNIEnv::slot_" + std::to_string(i);
    hle::Register(name, jni_generic);
    mem::W32(env_tab + 4 * i, hle::StubFor(name));
  }
  g_env = GuestMemory::Alloc(4);
  mem::W32(g_env, env_tab);

  const gaddr vm_tab = GuestMemory::Calloc(8, 4);
  mem::W32(vm_tab + 4 * 3, ZW_HLE_THUNK("JavaVM::DestroyJavaVM", vm_zero));
  mem::W32(vm_tab + 4 * 4, ZW_HLE_THUNK("JavaVM::AttachCurrentThread", vm_attach));
  mem::W32(vm_tab + 4 * 5, ZW_HLE_THUNK("JavaVM::DetachCurrentThread", vm_zero));
  mem::W32(vm_tab + 4 * 6, ZW_HLE_THUNK("JavaVM::GetEnv", vm_get_env));
  mem::W32(vm_tab + 4 * 7, ZW_HLE_THUNK("JavaVM::AttachCurrentThreadAsDaemon", vm_attach));
  g_vm = GuestMemory::Alloc(4);
  mem::W32(g_vm, vm_tab);
  return g_vm;
}
}  // namespace bootstrap

namespace {
template <class... A>
void Out(std::format_string<A...> f, A&&... a) {
  const std::string s = std::format(f, std::forward<A>(a)...);
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}
}  // namespace

int RunLoadOnly(const std::vector<std::filesystem::path>& libs, gaddr java_vm) {
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - a).count(); };
  if (!GuestMemory::Init()) {
    Out("load-only: no se pudo reservar la memoria guest");
    return 2;
  }
  bootstrap::Register();
  const gaddr vm = java_vm ? java_vm : bootstrap::FakeJavaVM();
  int rc = 0;

  std::vector<Module*> mods;
  for (const auto& path : libs) {
    const auto t0 = Clock::now();
    Module* m = Load(path);
    if (!m) {
      Out("load-only: FALLO al cargar {}", path.string());
      return 2;
    }
    mods.push_back(m);
    Out("== {} ==", path.string());
    Out("  base 0x{:08x}, tamano 0x{:x}, soname '{}', carga+relocs en {} ms", m->base(), m->size(), m->soname(), ms(t0));
    for (const auto& s : m->segments())
      Out("  segmento 0x{:08x}-0x{:08x} {}{}{}", s.addr, s.addr + s.size, (s.prot & kProtRead) ? "r" : "-",
          (s.prot & kProtWrite) ? "w" : "-", (s.prot & kProtExec) ? "x" : "-");
    std::string needed;
    for (const auto& n : m->needed()) needed += n + " ";
    Out("  DT_NEEDED: {}", needed);
    Out("  simbolos definidos: {}, constructores: {}, .ARM.exidx: 0x{:08x} ({} entradas), JNI_OnLoad: {}", m->symbols().size(),
        m->init_functions().size(), m->exidx(), m->exidx_count(),
        m->FindExport("JNI_OnLoad") ? std::format("0x{:08x}", m->FindExport("JNI_OnLoad")) : std::string("no exportado"));

    std::size_t n_mod = 0, n_data = 0, n_ph = 0, n_stub = 0, n_ok = 0;
    std::string missing, placeholders;
    for (const auto& i : m->imports()) {
      switch (i.via) {
        case ImportInfo::Via::Module: ++n_mod; break;
        case ImportInfo::Via::HleData: ++n_data; break;
        case ImportInfo::Via::HleDataPlaceholder: ++n_ph; placeholders += i.name + " "; break;
        case ImportInfo::Via::HleStub:
          ++n_stub;
          if (i.has_handler) ++n_ok;
          else missing += i.name + " ";
          break;
        default: break;
      }
    }
    Out("  imports: {} en total -> {} a otras libs, {} datos HLE, {} datos sin definir, {} funciones HLE ({} con handler, {} sin handler)",
        m->imports().size(), n_mod, n_data, n_ph, n_stub, n_ok, n_stub - n_ok);
    if (!placeholders.empty()) Out("  datos sin definir (bloque a cero): {}", placeholders);
    Out("  imports SIN handler ({}): {}", n_stub - n_ok, missing.empty() ? "(ninguno)" : missing);
  }

  for (Module* m : mods) {
    Out("== ejecutando {} ==", m->name());
    std::size_t cur = 0, total = m->init_functions().size();
    gaddr cur_fn = 0;
    const auto t0 = Clock::now();
    try {
      RunInit(*m, [&](std::size_t i, std::size_t, gaddr fn) {
        cur = i;
        cur_fn = fn;
      });
      Out("  .init_array: {} constructores ejecutados sin fallos en {} ms", total, ms(t0));
    } catch (const GuestFault& f) {
      Out("  .init_array: DETENIDO en el constructor {}/{} (0x{:08x} {}) por fallo guest:\n{}", cur + 1, total, cur_fn,
          addr2sym(cur_fn), f.what());
      rc = 1;
      continue;
    } catch (const GuestExit& e) {
      Out("  .init_array: DETENIDO en el constructor {}/{} (0x{:08x} {}): el guest llamo a exit/abort (codigo {})", cur + 1, total,
          cur_fn, addr2sym(cur_fn), e.code);
      rc = 1;
      continue;
    }
    const auto t1 = Clock::now();
    try {
      if (auto v = CallJniOnLoad(*m, vm)) Out("  JNI_OnLoad(vm, NULL) -> 0x{:x} en {} ms", static_cast<std::uint32_t>(*v), ms(t1));
      else Out("  JNI_OnLoad: no exportado");
    } catch (const GuestFault& f) {
      Out("  JNI_OnLoad: DETENIDO por fallo guest:\n{}", f.what());
      rc = 1;
    } catch (const GuestExit& e) {
      Out("  JNI_OnLoad: DETENIDO: el guest llamo a exit/abort (codigo {})", e.code);
      rc = 1;
    }
  }

  Out("== resumen ==");
  std::string called_missing;
  std::uint64_t total_calls = 0;
  std::size_t distinct = 0;
  for (const auto& [name, n] : hle::CallCounts()) {
    total_calls += n;
    if (n) ++distinct;
    if (n && !hle::IsRegistered(name)) called_missing += std::format("{}x{} ", name, n);
  }
  Out("  llamadas a imports: {} ({} imports distintos)", total_calls, distinct);
  Out("  imports sin handler que SI se llegaron a invocar: {}", called_missing.empty() ? "(ninguno)" : called_missing);
  const HeapStats hs = GuestMemory::GetHeapStats();
  Out("  heap guest: {} bloques vivos, {} KiB en uso, {} KiB mapeados", hs.alloc_count, hs.bytes_in_use / 1024, hs.bytes_mapped / 1024);
  Out("  resultado: {}", rc == 0 ? "OK" : "DETENIDO (ver arriba)");
  return rc;
}

}  // namespace zw::loader
