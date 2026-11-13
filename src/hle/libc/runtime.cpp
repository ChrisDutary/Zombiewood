// Runtime C/C++: __cxa_*, abort/exit/__assert2/__stack_chk_fail, __errno, senales (no-op), qsort/bsearch,
// rand48, syscall, __stack_chk_guard.
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "hle/libc/internal.h"
#include "hle/libc/libc.h"

namespace zw::hle::libc {
namespace {

// --- errno / terminacion -------------------------------------------------------------------------
gaddr hle_errno(Cpu& c) { return c.errno_addr(); }

[[noreturn]] void Fault(Cpu& c, const std::string& what) {
  log::Error("{}", what);
  throw GuestFault(what + "\n" + debug::DumpState(c));
}
void hle_abort(Cpu& c) {
  log::Error("el guest llamo a abort()\n{}", debug::DumpState(c));
  throw GuestExit{134};
}
void hle_exit(Cpu& c, int code) {
  log::Warn("el guest llamo a exit({}) desde {}", code, debug::addr2sym(c.lr()));
  StdioFlushAll();
  throw GuestExit{code};
}
void hle__exit(int code) { throw GuestExit{code}; }
void hle_assert2(Cpu& c, const char* file, int line, const char* func, const char* expr) {
  Fault(c, std::format("__assert2: {}:{}: {}: assertion \"{}\" failed", file ? file : "?", line, func ? func : "?",
                       expr ? expr : "?"));
}
void hle_assert(Cpu& c, const char* file, int line, const char* expr) {
  Fault(c, std::format("__assert: {}:{}: assertion \"{}\" failed", file ? file : "?", line, expr ? expr : "?"));
}
void hle_stack_chk_fail(Cpu& c) { Fault(c, "__stack_chk_fail: desbordamiento de pila detectado"); }
void hle_pure_virtual(Cpu& c) { Fault(c, "__cxa_pure_virtual: llamada a un metodo virtual puro"); }

// --- __cxa_atexit ---------------------------------------------------------------------------------
struct AtExit {
  gaddr fn, arg, dso;
};
std::mutex g_atexit_mu;
std::vector<AtExit> g_atexit;
int hle_cxa_atexit(gaddr fn, gaddr arg, gaddr dso) {
  std::lock_guard lk(g_atexit_mu);
  g_atexit.push_back({fn, arg, dso});
  return 0;
}
int hle_atexit(gaddr fn) { return hle_cxa_atexit(fn, 0, 0); }
void hle_cxa_finalize(gaddr) {}  // el .so nunca se descarga

// --- __cxa_guard_* (ARM EABI: guarda de 32 bits, bit 0 del primer byte = inicializado) ----------
std::mutex g_guard_mu;
std::condition_variable g_guard_cv;
std::unordered_map<gaddr, std::uint32_t> g_guard_owner;  // guarda en curso -> hilo
int hle_guard_acquire(Cpu& c, gaddr g) {
  if (mem::R8(g) & 1) return 0;
  std::unique_lock lk(g_guard_mu);
  for (;;) {
    if (mem::R8(g) & 1) return 0;
    auto it = g_guard_owner.find(g);
    if (it == g_guard_owner.end()) break;
    if (it->second == c.thread_id())
      Fault(c, std::format("__cxa_guard_acquire: inicializacion estatica recursiva (guarda {:#x})", g));
    g_guard_cv.wait(lk);
  }
  g_guard_owner[g] = c.thread_id();
  mem::W8(g + 1, 1);  // "en curso" (como gnustl)
  return 1;
}
void hle_guard_release(gaddr g) {
  {
    std::lock_guard lk(g_guard_mu);
    mem::W8(g + 1, 0);
    mem::W8(g, 1);
    g_guard_owner.erase(g);
  }
  g_guard_cv.notify_all();
}
void hle_guard_abort(gaddr g) {
  {
    std::lock_guard lk(g_guard_mu);
    mem::W8(g + 1, 0);
    g_guard_owner.erase(g);
  }
  g_guard_cv.notify_all();
}

// --- senales: no-op -----------------------------------------------------------------------------
gaddr hle_signal(int sig, gaddr) {
  (void)sig;
  return 0;  // SIG_DFL
}
int hle_sigaction(int, gaddr, gaddr old) {
  if (old) std::memset(g2h(old), 0, 16);  // struct sigaction de ARM: 16 bytes
  return 0;
}
int hle_raise(Cpu& c, int sig) {
  log::Warn("raise({}) desde {}", sig, debug::addr2sym(c.lr()));
  if (sig == 6 /*SIGABRT*/) throw GuestExit{134};
  return 0;
}
int hle_kill(int, int) { return 0; }
int hle_sigset_zero(GPtr<std::uint32_t> set) {
  if (set) *set = 0;
  return 0;
}
int hle_sigset_op(GPtr<std::uint32_t> set, int sig) {
  if (!set || sig < 1 || sig > 32) return -1;
  *set |= 1u << (sig - 1);
  return 0;
}
int hle_sigdelset(GPtr<std::uint32_t> set, int sig) {
  if (!set || sig < 1 || sig > 32) return -1;
  *set &= ~(1u << (sig - 1));
  return 0;
}
int hle_sigismember(GPtr<std::uint32_t> set, int sig) {
  if (!set || sig < 1 || sig > 32) return -1;
  return (*set >> (sig - 1)) & 1;
}
std::uint32_t hle_alarm(std::uint32_t) { return 0; }

// --- qsort / bsearch ------------------------------------------------------------------------------
void hle_qsort(gaddr base, gsize n, gsize sz, gaddr cmp) {
  if (n < 2 || !sz) return;
  // Copia de trabajo en memoria guest (el comparador recibe punteros guest) y orden estable de
  // indices: un comparador inconsistente no puede sacar el algoritmo de rango.
  const gaddr tmp = GuestMemory::Alloc(n * sz);
  if (!tmp) return;
  std::memcpy(g2h(tmp), g2h(base), static_cast<std::size_t>(n) * sz);
  std::vector<std::uint32_t> idx(n);
  std::iota(idx.begin(), idx.end(), 0u);
  try {
    std::stable_sort(idx.begin(), idx.end(), [&](std::uint32_t a, std::uint32_t b) {
      return call_guest<int>(cmp, tmp + a * sz, tmp + b * sz) < 0;
    });
  } catch (...) {
    GuestMemory::Free(tmp);
    throw;
  }
  for (gsize i = 0; i < n; ++i) std::memcpy(g2h(base + i * sz), g2h(tmp + idx[i] * sz), sz);
  GuestMemory::Free(tmp);
}
gaddr hle_bsearch(gaddr key, gaddr base, gsize n, gsize sz, gaddr cmp) {
  gsize lo = 0, hi = n;
  while (lo < hi) {
    const gsize mid = lo + (hi - lo) / 2;
    const int r = call_guest<int>(cmp, key, base + mid * sz);
    if (r == 0) return base + mid * sz;
    if (r < 0) hi = mid;
    else lo = mid + 1;
  }
  return 0;
}

// --- rand48 (LCG exacto de 48 bits) ----------------------------------------------------------------
std::mutex g_rand_mu;
std::uint16_t g_x[3] = {0x330E, 0xABCD, 0x1234};
std::uint16_t g_a[3] = {0xE66D, 0xDEEC, 0x0005};
std::uint16_t g_c = 0x000B;
std::uint64_t Next48(std::uint16_t x[3]) {
  const std::uint64_t X = static_cast<std::uint64_t>(x[0]) | (static_cast<std::uint64_t>(x[1]) << 16) |
                          (static_cast<std::uint64_t>(x[2]) << 32);
  const std::uint64_t A = static_cast<std::uint64_t>(g_a[0]) | (static_cast<std::uint64_t>(g_a[1]) << 16) |
                          (static_cast<std::uint64_t>(g_a[2]) << 32);
  const std::uint64_t R = (A * X + g_c) & 0xFFFFFFFFFFFFull;
  x[0] = static_cast<std::uint16_t>(R);
  x[1] = static_cast<std::uint16_t>(R >> 16);
  x[2] = static_cast<std::uint16_t>(R >> 32);
  return R;
}
int hle_lrand48() {
  std::lock_guard lk(g_rand_mu);
  return static_cast<int>(Next48(g_x) >> 17);
}
int hle_mrand48() {
  std::lock_guard lk(g_rand_mu);
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(Next48(g_x) >> 16));
}
double hle_drand48() {
  std::lock_guard lk(g_rand_mu);
  return static_cast<double>(Next48(g_x)) / 281474976710656.0;
}
int hle_nrand48(GPtr<std::uint16_t> xs) {
  std::lock_guard lk(g_rand_mu);
  std::uint16_t x[3] = {xs[0], xs[1], xs[2]};
  const auto r = Next48(x);
  for (int i = 0; i < 3; ++i) xs[i] = x[i];
  return static_cast<int>(r >> 17);
}
int hle_jrand48(GPtr<std::uint16_t> xs) {
  std::lock_guard lk(g_rand_mu);
  std::uint16_t x[3] = {xs[0], xs[1], xs[2]};
  const auto r = Next48(x);
  for (int i = 0; i < 3; ++i) xs[i] = x[i];
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(r >> 16));
}
double hle_erand48(GPtr<std::uint16_t> xs) {
  std::lock_guard lk(g_rand_mu);
  std::uint16_t x[3] = {xs[0], xs[1], xs[2]};
  const auto r = Next48(x);
  for (int i = 0; i < 3; ++i) xs[i] = x[i];
  return static_cast<double>(r) / 281474976710656.0;
}
void hle_srand48(int seed) {
  std::lock_guard lk(g_rand_mu);
  g_x[0] = 0x330E;
  g_x[1] = static_cast<std::uint16_t>(seed);
  g_x[2] = static_cast<std::uint16_t>(static_cast<std::uint32_t>(seed) >> 16);
  g_a[0] = 0xE66D;
  g_a[1] = 0xDEEC;
  g_a[2] = 0x0005;
  g_c = 0x000B;
}
gaddr hle_seed48(GPtr<std::uint16_t> s) {
  static gaddr prev = 0;
  if (!prev) prev = GuestMemory::Calloc(3, 2);
  std::lock_guard lk(g_rand_mu);
  for (int i = 0; i < 3; ++i) mem::W16(prev + 2 * i, g_x[i]);
  for (int i = 0; i < 3; ++i) g_x[i] = s[i];
  g_a[0] = 0xE66D;
  g_a[1] = 0xDEEC;
  g_a[2] = 0x0005;
  g_c = 0x000B;
  return prev;
}
// bionic 4.x: rand() = lrand48(), srand() = srand48(), random() idem.
int hle_rand() { return hle_lrand48(); }
void hle_srand(std::uint32_t s) { hle_srand48(static_cast<int>(s)); }
int hle_rand_r(GPtr<std::uint32_t> seed) {
  std::uint32_t s = *seed;
  s = s * 1103515245u + 12345u;
  *seed = s;
  return static_cast<int>((s / 65536u) % 32768u);
}

// --- syscall --------------------------------------------------------------------------------------
int hle_syscall(Cpu& c, int nr, Varargs va) {
  switch (nr) {
    case 20: return 4242;                                       // getpid
    case 224: return static_cast<int>(4242 + c.thread_id());   // gettid
    case 158: std::this_thread::yield(); return 0;             // sched_yield
    case 240: {                                                 // futex: espera/aviso cortos
      const gaddr uaddr = va.NextWord();
      const int op = static_cast<int>(va.NextWord()) & 0x7F;
      const std::uint32_t val = va.NextWord();
      if (op == 0 /*WAIT*/) {
        if (mem::R32(uaddr) != val) {
          SetErrno(c, err::AGAIN);
          return -1;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return 0;
      }
      return 0;  // WAKE: los que esperan sondean
    }
    default:
      log::Warn("syscall({}) no soportado (lr = {})", nr, debug::addr2sym(c.lr()));
      SetErrno(c, err::NOSYS);
      return -1;
  }
}

}  // namespace

void RunAtexit() {
  std::vector<AtExit> list;
  {
    std::lock_guard lk(g_atexit_mu);
    list.swap(g_atexit);
  }
  for (auto it = list.rbegin(); it != list.rend(); ++it) call_guest<void>(it->fn, it->arg);
}

void RegisterRuntime() {
  mem::W32(DefineData("__stack_chk_guard", 4), 0x2F7A1C55u);

  ZW_HLE_FAST("__errno", hle_errno);
  ZW_HLE("abort", hle_abort);
  ZW_HLE("exit", hle_exit);
  ZW_HLE("_exit", hle__exit);
  ZW_HLE("_Exit", hle__exit);
  ZW_HLE("__assert2", hle_assert2);
  ZW_HLE("__assert", hle_assert);
  ZW_HLE("__stack_chk_fail", hle_stack_chk_fail);
  ZW_HLE("__cxa_pure_virtual", hle_pure_virtual);
  ZW_HLE("__cxa_atexit", hle_cxa_atexit);
  ZW_HLE("atexit", hle_atexit);
  ZW_HLE("__cxa_finalize", hle_cxa_finalize);
  ZW_HLE("__cxa_guard_acquire", hle_guard_acquire);
  ZW_HLE("__cxa_guard_release", hle_guard_release);
  ZW_HLE("__cxa_guard_abort", hle_guard_abort);

  ZW_HLE("signal", hle_signal);
  ZW_HLE("bsd_signal", hle_signal);
  ZW_HLE("sysv_signal", hle_signal);
  ZW_HLE("sigaction", hle_sigaction);
  ZW_HLE("raise", hle_raise);
  ZW_HLE("kill", hle_kill);
  ZW_HLE_FAST("sigemptyset", hle_sigset_zero);
  ZW_HLE_FAST("sigfillset", hle_sigset_zero);
  ZW_HLE_FAST("sigaddset", hle_sigset_op);
  ZW_HLE_FAST("sigdelset", hle_sigdelset);
  ZW_HLE_FAST("sigismember", hle_sigismember);
  ZW_HLE_FAST("alarm", hle_alarm);

  ZW_HLE("qsort", hle_qsort);
  ZW_HLE("bsearch", hle_bsearch);
  ZW_HLE_FAST("lrand48", hle_lrand48);
  ZW_HLE_FAST("mrand48", hle_mrand48);
  ZW_HLE_FAST("drand48", hle_drand48);
  ZW_HLE_FAST("nrand48", hle_nrand48);
  ZW_HLE_FAST("jrand48", hle_jrand48);
  ZW_HLE_FAST("erand48", hle_erand48);
  ZW_HLE_FAST("srand48", hle_srand48);
  ZW_HLE_FAST("seed48", hle_seed48);
  ZW_HLE_FAST("rand", hle_rand);
  ZW_HLE_FAST("random", hle_rand);
  ZW_HLE_FAST("srand", hle_srand);
  ZW_HLE_FAST("srandom", hle_srand);
  ZW_HLE_FAST("rand_r", hle_rand_r);
  ZW_HLE("syscall", hle_syscall);
}

}  // namespace zw::hle::libc
