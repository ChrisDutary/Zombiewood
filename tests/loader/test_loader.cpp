// Tests de zw_loader (T3): carga de dos .so ELF32 ARM reales (tests/loader/testlib.c compilado
// con clang/lld, incrustado en testlib_blob.inc), relocaciones, imports de funcion y de dato,
// constructores, simbolos, resolucion cruzada entre libs, exidx y JNI_OnLoad.
#include <cstdio>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "loader/load_only.h"
#include "loader/loader.h"
#include "testlib_blob.inc"

using namespace zw;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    ++g_checks;                                                      \
    if (!(cond)) {                                                   \
      ++g_failed;                                                    \
      std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

static int host_add(int a, int b) { return a + b; }
static double host_scale(double a, float b) { return a * b; }
static std::string g_seen;
static int host_var(const char* fmt, Varargs va) {
  const int x = va.Next<int>();
  const double d = va.Next<double>();
  const gi64 q = va.Next<gi64>();
  g_seen = std::string(fmt) + "|" + std::to_string(x) + "|" + std::to_string(d) + "|" + std::to_string(q.v);
  return x * 2;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  log::Init();
  GuestMemory::Init();

  // HLE registrada ANTES de cargar; host_var se registra DESPUES a proposito (debe funcionar igual).
  ZW_HLE_FAST("host_add", host_add);
  ZW_HLE("host_scale", host_scale);
  mem::W32(hle::DefineData("host_data", 4), 4000);
  loader::bootstrap::Register();  // malloc

  try {
    loader::Module* a = loader::LoadFromMemory("libzwtest.so", kTestLibA, sizeof kTestLibA);
    CHECK(a != nullptr);
    if (!a) return 1;
    CHECK(a->soname() == "libzwtest.so" && a->base() >= GuestMemory::kLibsBase && (a->base() & 0xFFFF) == 0);
    CHECK(loader::FindModule("libzwtest.so") == a && loader::ModuleAt(a->base() + 0x100) == a);
    // (clang fusiona los dos constructores del .c en una unica entrada de .init_array)
    CHECK(!a->init_done() && !a->init_functions().empty());

    // imports
    bool saw_add = false, saw_data = false, saw_var = false;
    for (const auto& i : a->imports()) {
      if (i.name == "host_add") saw_add = i.via == loader::ImportInfo::Via::HleStub && i.has_handler && i.addr == hle::StubFor("host_add");
      if (i.name == "host_data") saw_data = i.via == loader::ImportInfo::Via::HleData && i.addr == hle::FindData("host_data");
      if (i.name == "host_var") saw_var = i.via == loader::ImportInfo::Via::HleStub && !i.has_handler;
    }
    CHECK(saw_add && saw_data && saw_var);
    ZW_HLE("host_var", host_var);

    // exports y simbolos
    const gaddr call_add = a->FindExport("call_add"), arm_mul = a->FindExport("arm_mul");
    CHECK(call_add && (call_add & 1) == 1);  // Thumb
    CHECK(arm_mul && (arm_mul & 1) == 0);    // ARM
    CHECK(a->FindExport("no_existe") == 0 && loader::FindSymbol("call_add") == call_add);
    CHECK(loader::addr2sym(call_add) == "libzwtest.so!call_add");
    CHECK(loader::addr2sym(call_add + 4) == "libzwtest.so!call_add+0x4");
    CHECK(debug::addr2sym(arm_mul + 4) == "libzwtest.so!arm_mul+0x4");
    const gaddr value = a->FindExport("exported_value");
    CHECK(value && mem::R32(value) == 1234);  // constructores aun no ejecutados

    // constructores
    std::size_t seen = 0;
    loader::RunInit(*a, [&](std::size_t, std::size_t, gaddr) { ++seen; });
    CHECK(seen == a->init_functions().size() && a->init_done());
    CHECK(call_guest<int>(a->FindExport("get_ctor_count")) == 11);
    CHECK(mem::R32(value) == 1234 + 4000);  // el constructor leyo el dato importado
    loader::RunInit(*a);                    // idempotente
    CHECK(call_guest<int>(a->FindExport("get_ctor_count")) == 11);

    // llamadas: JUMP_SLOT + tabla de punteros a import, softfp, variadica, RELATIVE
    CHECK(call_guest<int>(call_add, 3, 4) == 14);
    CHECK(call_guest<double>(a->FindExport("call_scale"), 2.5, 4.0f) == 10.0);
    CHECK(call_guest<int>(a->FindExport("call_var"), 21) == 42);
    std::printf("  variadica vio: %s\n", g_seen.c_str());
    CHECK(g_seen == "hola desde libzwtest|21|1.500000|" + std::to_string(21LL << 33));
    CHECK(mem::ReadCStr(call_guest<gaddr>(a->FindExport("get_msg"))) == "hola desde libzwtest");
    CHECK(call_guest<int>(a->FindExport("read_host_data")) == 4000);
    CHECK(*call_guest<int*>(a->FindExport("alloc_int"), 77) == 77);
    CHECK(call_guest<int>(arm_mul, 6, 7) == 42);

    // JNI_OnLoad y exidx
    auto ver = loader::CallJniOnLoad(*a, loader::bootstrap::FakeJavaVM());
    CHECK(ver && *ver == 0x10004);
    CHECK(a->exidx() != 0 && a->exidx_count() > 0);
    {
      gaddr cnt = GuestMemory::Calloc(1, 4);
      CHECK(call_guest<gaddr>(hle::StubFor("__gnu_Unwind_Find_exidx"), call_add, cnt) == a->exidx());
      CHECK(mem::R32(cnt) == a->exidx_count());
    }

    // segunda lib: resolucion cruzada contra la primera
    loader::Module* b = loader::LoadFromMemory("libzwtest2.so", kTestLibB, sizeof kTestLibB, {.defer_init = false});
    CHECK(b != nullptr);
    if (b) {
      CHECK(b->base() != a->base() && loader::Modules().size() == 2);
      int cross = 0;
      for (const auto& i : b->imports()) cross += i.via == loader::ImportInfo::Via::Module;
      CHECK(cross == 3);
      CHECK(call_guest<int>(b->FindExport("cross_call"), 3, 4) == 14 + 12 + 5234);
    }

    // el texto es de solo lectura para el guest; un ELF invalido se rechaza limpiamente
    CHECK((GuestMemory::ProtOf(call_add) & kProtWrite) == 0 && (GuestMemory::ProtOf(call_add) & kProtExec));
    CHECK(loader::LoadFromMemory("basura", "no soy un elf, de verdad que no; relleno relleno relleno relleno relleno", 64) == nullptr);
  } catch (const GuestFault& f) {
    std::printf("GuestFault inesperado:\n%s\n", f.what());
    return 2;
  }

  std::printf("%d comprobaciones, %d fallos\n", g_checks, g_failed);
  std::puts(g_failed ? "FALLO" : "ok");
  return g_failed ? 1 : 0;
}
