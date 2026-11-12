// Tests de zw_cpu (T3): memoria guest, marshalling de imports, call_guest reentrante,
// interworking, hilos con LDREX/STREX, setjmp/longjmp, fallos y hooks.
// El codigo guest es tests/cpu/guest_code.c compilado a Thumb-2 (guest_blob.inc).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "guest_blob.inc"

using namespace zw;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_failed;                                                          \
      std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                      \
  } while (0)
#define SECTION(name) std::printf("[%s]\n", name)

static gaddr g_blob = 0;
static gaddr Fn(unsigned off) { return g_blob + off; }

// ---- handlers ----------------------------------------------------------------
static bool g_mixed_ok = false;
static int h_mixed(int a, float b, double c, gi64 d, int e, float f, double g) {
  g_mixed_ok = a == 7 && b == 2.5f && c == 3.25 && d == 0x1122334455667788LL && e == 9 && f == -1.5f && g == 1e10;
  return g_mixed_ok ? 1234 : -1;
}
static std::string g_var_seen;
static int h_variadic(const char* fmt, Varargs va) {
  const int i = va.Next<int>();
  const double d = va.Next<double>();
  const gi64 q = va.Next<gi64>();
  const char* s = va.Next<const char*>();
  const int ch = va.Next<int>();
  char buf[128];
  std::snprintf(buf, sizeof buf, "%s|%d|%.2f|%llx|%s|%c", fmt, i, d, (unsigned long long)q.v, s, ch);
  g_var_seen = buf;
  return static_cast<int>(g_var_seen.size());
}
static int h_vlist(const char* fmt, VaList ap) { return ap.Next<int>() + static_cast<int>(ap.Next<double>()) + (fmt ? 1 : 0); }
static double h_pow(double a, double b) { return std::pow(a, b); }
static float h_sqrtf(float a) { return std::sqrt(a); }
static gi64 h_ll(gi64 a, int sh) { return a.v << sh; }
static int h_cb(gaddr fn, int x) { return call_guest<int>(fn, x, 5) + 100; }
static int h_inc(int x) { return x + 1; }
static int h_call2(gaddr fn, gaddr buf, gaddr lj) {
  call_guest<void>(fn, buf, lj);
  return 0;
}
static int h_hooked(int, int) { return 1000; }
static gaddr h_tid(Cpu& cpu) { return cpu.thread_id(); }

struct ThreadArgs {
  std::uint32_t counter, lock, plain, n;
};
static std::uint32_t h_thread_entry(GPtr<ThreadArgs> a) {
  call_guest<int>(Fn(kOff_t_atomic_loop), a->counter, a->n);
  call_guest<int>(Fn(kOff_t_spin_loop), a->lock, a->plain, a->n);
  return 0xC0FFEE;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  log::Init();
  if (!GuestMemory::Init()) {
    std::puts("GuestMemory::Init fallo");
    return 1;
  }

  SECTION("memoria");
  {
    gaddr a = GuestMemory::Alloc(100), b = GuestMemory::Alloc(100);
    CHECK(a && b && a != b && (a & 7) == 0);
    CHECK(GuestMemory::UsableSize(a) >= 100);
    std::memset(g2h(a), 0xAB, 100);
    gaddr a2 = GuestMemory::Realloc(a, 5000);
    CHECK(a2 && mem::R8(a2 + 99) == 0xAB);
    gaddr big = GuestMemory::Alloc(3u << 20);
    CHECK(big && GuestMemory::IsMapped(big, 3u << 20));
    mem::W32(big + (3u << 20) - 4, 0xDEADBEEF);
    gaddr big2 = GuestMemory::Realloc(big, 5u << 20);
    CHECK(big2 && mem::R32(big2 + (3u << 20) - 4) == 0xDEADBEEF);
    GuestMemory::Free(big2);
    CHECK(!GuestMemory::IsMapped(big2));
    gaddr z = GuestMemory::Calloc(10, 10);
    CHECK(z && mem::R32(z) == 0 && mem::R32(z + 96) == 0);
    gaddr al = GuestMemory::AllocAligned(100, 64);
    CHECK(al && (al & 63) == 0);
    GuestMemory::Free(al);
    gaddr s = mem::StrDup("hola mundo");
    CHECK(mem::ReadCStr(s) == "hola mundo");
    CHECK(mem::WriteCStr(s, "abcdefghijklmnop", 4) == 3 && mem::ReadCStr(s) == "abc");
    CHECK(h2g(g2h(s)) == s && g2h(0) == nullptr && h2g(nullptr) == 0);
    const auto before = GuestMemory::GetHeapStats().alloc_count;
    for (int r = 0; r < 3; ++r) {
      gaddr v[200];
      for (int i = 0; i < 200; ++i) v[i] = GuestMemory::Alloc(1 + i * 37);
      for (int i = 0; i < 200; ++i) GuestMemory::Free(v[i]);
    }
    CHECK(GuestMemory::GetHeapStats().alloc_count == before);
    GuestMemory::Free(a2); GuestMemory::Free(b); GuestMemory::Free(z); GuestMemory::Free(s);
    gaddr m = GuestMemory::MapAnywhere(Zone::Heap, 0x3000, kProtRW, "t");
    CHECK(m && GuestMemory::IsMapped(m, 0x3000) && !GuestMemory::IsMapped(m, 0x3001));
    CHECK(GuestMemory::Unmap(m, 0x3000) && !GuestMemory::IsMapped(m));
  }

  // Cargar el blob como codigo guest.
  g_blob = GuestMemory::MapAnywhere(Zone::Libs, sizeof kGuestBlob, kProtRW, "blob");
  std::memcpy(g2h(g_blob), kGuestBlob, sizeof kGuestBlob);
  GuestMemory::Protect(g_blob, sizeof kGuestBlob, kProtRX);

  ZW_HLE("h_mixed", h_mixed);
  ZW_HLE_FAST("h_mixed_fast", h_mixed);
  ZW_HLE("h_variadic", h_variadic);
  ZW_HLE("h_vlist", h_vlist);
  ZW_HLE_FAST("h_pow", h_pow);
  ZW_HLE_FAST("h_sqrtf", h_sqrtf);
  ZW_HLE("h_ll", h_ll);
  ZW_HLE("h_cb", h_cb);
  ZW_HLE_FAST("h_inc", h_inc);
  ZW_HLE("h_call2", h_call2);
  ZW_HLE_FAST("h_tid", h_tid);
  const gaddr entry_stub = ZW_HLE_THUNK("h_thread_entry", h_thread_entry);

  try {
    SECTION("import con int/float/double/long long (softfp, pila)");
    CHECK(call_guest<int>(Fn(kOff_t_mixed), hle::StubFor("h_mixed")) == 1234 && g_mixed_ok);
    g_mixed_ok = false;
    CHECK(call_guest<int>(Fn(kOff_t_mixed), hle::StubFor("h_mixed_fast")) == 1234 && g_mixed_ok);

    SECTION("import variadico y va_list");
    {
      gaddr fmt = mem::StrDup("fmt"), s = mem::StrDup("cadena");
      const int n = call_guest<int>(Fn(kOff_t_variadic), hle::StubFor("h_variadic"), fmt, s);
      std::printf("  visto: %s\n", g_var_seen.c_str());
      CHECK(g_var_seen == "fmt|42|2.75|102030405060708|cadena|z" && n == static_cast<int>(g_var_seen.size()));
      // va_list guest construido a mano: {int 5, (pad), double 8.0}
      gaddr ap = GuestMemory::Calloc(4, 8);
      mem::W32(ap, 5);
      mem::Write<double>(ap + 8, 8.0);
      CHECK(call_guest<int>(hle::StubFor("h_vlist"), fmt, ap) == 14);
    }

    SECTION("retornos float/double/long long");
    CHECK(call_guest<double>(Fn(kOff_t_ret_double), hle::StubFor("h_pow"), 2.0, 10.0) == 2048.0);
    CHECK(call_guest<float>(Fn(kOff_t_ret_float), hle::StubFor("h_sqrtf"), 16.0f) == 5.0f);
    CHECK(call_guest<gi64>(Fn(kOff_t_ret_ll), hle::StubFor("h_ll"), gi64{0x100000001LL}).v == 0x800000009LL);

    SECTION("callback host -> guest -> host -> guest");
    CHECK(call_guest<int>(Fn(kOff_t_callback), hle::StubFor("h_cb"), 20) == 20 * 2 + 5 + 100 + 1);

    SECTION("interworking ARM/Thumb");
    CHECK(call_guest<int>(Fn(kOff_t_arm_add), 4, 5, hle::StubFor("h_inc")) == 10);
    CHECK(call_guest<int>(Fn(kOff_t_interwork), 4, 5, hle::StubFor("h_inc"), Fn(kOff_t_arm_add)) == 30);

    SECTION("argumentos host -> guest por registros y pila");
    CHECK(call_guest<double>(Fn(kOff_t_sum8), 1, 2.5, 0.25f, gi64{(3LL << 32) | 4}, 10, 100.0) == 1 + 2.5 + 0.25 + 3 + 4 + 10 + 100.0);

    SECTION("VFP");
    {
      gaddr a = GuestMemory::Alloc(16), b = GuestMemory::Alloc(16);
      for (int i = 0; i < 4; ++i) {
        mem::Write<float>(a + 4 * i, float(i + 1));
        mem::Write<float>(b + 4 * i, 0.5f);
      }
      CHECK(call_guest<float>(Fn(kOff_t_dot), a, b, 4) == 1.25f);
    }

    SECTION("TLS (TPIDRURO), datos por hilo");
    CHECK(call_guest<gaddr>(Fn(kOff_t_read_tls)) == Cpu::Current().tls());
    CHECK(call_guest<std::uint32_t>(hle::StubFor("h_tid")) == Cpu::Current().thread_id());
    CHECK(Cpu::Current().errno_addr() != 0 && Cpu::Current().call_depth() == 0);

    SECTION("setjmp/longjmp cruzando call_guest");
    {
      gaddr buf = GuestMemory::Calloc(64, 4);
      CHECK(call_guest<int>(Fn(kOff_t_setjmp), hle::StubFor("setjmp"), hle::StubFor("longjmp"), hle::StubFor("h_call2"), buf) == 2077);
      CHECK(Cpu::Current().call_depth() == 0);
    }

    SECTION("import sin handler devuelve 0");
    CHECK(call_guest<int>(Fn(kOff_t_arm_add), 4, 5, hle::StubFor("zw_test_no_existe")) == 5);
    {
      bool listed = false;
      for (auto& n : hle::UnimplementedImports()) listed |= n == "zw_test_no_existe";
      CHECK(listed);
    }

    SECTION("dos hilos guest con LDREX/STREX (threads::Create)");
    {
      const std::uint32_t N = 500000;
      gaddr args = GuestMemory::Calloc(1, sizeof(ThreadArgs));
      gaddr cells = GuestMemory::Calloc(3, 64);
      *g2h<ThreadArgs>(args) = ThreadArgs{cells, cells + 64, cells + 128, N};
      threads::CreateParams p{entry_stub, args, 0, false, "worker"};
      const auto clock0 = std::chrono::steady_clock::now();
      const auto t1 = threads::Create(p), t2 = threads::Create(p);
      CHECK(t1 && t2 && t1 != t2);
      std::uint32_t r1 = 0, r2 = 0;
      CHECK(threads::Join(t1, &r1) && threads::Join(t2, &r2));
      CHECK(r1 == 0xC0FFEE && r2 == 0xC0FFEE);
      std::printf("  tiempo de los dos hilos: %lld ms\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - clock0).count());
      std::printf("  contador atomico = %u, contador bajo spinlock = %u (esperado %u)\n", mem::R32(cells), mem::R32(cells + 128), 2 * N);
      CHECK(mem::R32(cells) == 2 * N);
      CHECK(mem::R32(cells + 128) == 2 * N);
      CHECK(!threads::Join(t1) && !threads::IsAlive(t1));
    }

    SECTION("hilos host adjuntados (std::thread + call_guest)");
    {
      const int N = 100000;
      gaddr cell = GuestMemory::Calloc(1, 64);
      std::atomic<int> ok{0};
      auto body = [&] {
        if (call_guest<int>(Fn(kOff_t_atomic_loop), cell, N) == N) ++ok;
        threads::DetachCurrent();
      };
      std::thread a(body), b(body), c(body);
      a.join(); b.join(); c.join();
      CHECK(ok == 3 && mem::R32(cell) == 3u * N);
    }

    SECTION("fallo guest -> GuestFault con volcado");
    {
      bool caught = false;
      try {
        call_guest<int>(Fn(kOff_t_null_write), 1);
      } catch (const GuestFault& f) {
        caught = true;
        CHECK(std::string(f.what()).find("0x00000008") != std::string::npos);
      }
      CHECK(caught && Cpu::Current().call_depth() == 0);
      // El Cpu sigue siendo utilizable.
      CHECK(call_guest<int>(Fn(kOff_t_arm_add), 1, 2, hle::StubFor("h_inc")) == 4);
      caught = false;
      try {
        call_guest<int>(0x00001000);
      } catch (const GuestFault&) {
        caught = true;
      }
      CHECK(caught);
    }

    SECTION("HookGuestFunction (ARM y Thumb)");
    {
      ZW_HLE("h_hooked", h_hooked);
      CHECK(hle::HookGuestFunction(Fn(kOff_t_arm_add), "h_hooked"));
      CHECK(call_guest<int>(Fn(kOff_t_arm_add), 4, 5, hle::StubFor("h_inc")) == 1000);
      CHECK(call_guest<int>(Fn(kOff_t_interwork), 4, 5, hle::StubFor("h_inc"), Fn(kOff_t_arm_add)) == 3000);
      CHECK(hle::HookGuestFunction(Fn(kOff_t_callback), "h_hooked"));
      CHECK(call_guest<int>(Fn(kOff_t_callback), hle::StubFor("h_cb"), 20) == 1000);
    }

    SECTION("addr2sym");
    CHECK(debug::addr2sym(hle::StubFor("h_pow")) == "hle!h_pow");
  } catch (const GuestFault& f) {
    std::printf("GuestFault inesperado:\n%s\n", f.what());
    return 2;
  }

  std::printf("%d comprobaciones, %d fallos\n", g_checks, g_failed);
  std::puts(g_failed ? "FALLO" : "ok");
  return g_failed ? 1 : 0;
}
