// HLE minima de arranque (T3) para `--load-only` y tests: lo justo de bionic para ejecutar
// constructores estaticos y JNI_OnLoad. NO es la libc del port (eso es T4, src/hle/libc):
// solo registra los nombres que nadie haya registrado antes, y T8 no la usa.
#include "loader/load_only.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"

namespace zw::loader::bootstrap {
namespace {

// --- memoria / cadenas ------------------------------------------------------------
gaddr b_malloc(gsize n) { return GuestMemory::Alloc(n); }
void b_free(gaddr p) { GuestMemory::Free(p); }
gaddr b_calloc(gsize n, gsize s) { return GuestMemory::Calloc(n, s); }
gaddr b_realloc(gaddr p, gsize n) { return GuestMemory::Realloc(p, n); }
void* b_memcpy(void* d, const void* s, gsize n) { return std::memcpy(d, s, n); }
void* b_memmove(void* d, const void* s, gsize n) { return std::memmove(d, s, n); }
void* b_memset(void* d, int c, gsize n) { return std::memset(d, c, n); }
int b_memcmp(const void* a, const void* b, gsize n) { return std::memcmp(a, b, n); }
void* b_memchr(void* s, int c, gsize n) { return std::memchr(s, c, n); }
gsize b_strlen(const char* s) { return static_cast<gsize>(std::strlen(s)); }
char* b_strcpy(char* d, const char* s) { return std::strcpy(d, s); }
char* b_strncpy(char* d, const char* s, gsize n) { return std::strncpy(d, s, n); }
char* b_strcat(char* d, const char* s) { return std::strcat(d, s); }
int b_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
int b_strncmp(const char* a, const char* b, gsize n) { return std::strncmp(a, b, n); }
int b_strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }
char* b_strchr(char* s, int c) { return std::strchr(s, c); }
char* b_strrchr(char* s, int c) { return std::strrchr(s, c); }
char* b_strstr(char* s, const char* n) { return std::strstr(s, n); }
gaddr b_strdup(const char* s) { return s ? mem::StrDup(s) : 0; }
int b_atoi(const char* s) { return std::atoi(s); }
double b_atof(const char* s) { return std::atof(s); }

// --- printf minimo sobre VaList ---------------------------------------------------
std::string VFormat(const char* fmt, VaList& ap) {
  std::string out;
  if (!fmt) return out;
  char buf[512];
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') {
      out += *p;
      continue;
    }
    std::string spec = "%";
    ++p;
    while (*p && std::strchr("-+ #0", *p)) spec += *p++;
    auto star_or_digits = [&] {
      if (*p == '*') {
        spec += std::to_string(ap.Next<int>());
        ++p;
      } else {
        while (*p >= '0' && *p <= '9') spec += *p++;
      }
    };
    star_or_digits();
    if (*p == '.') {
      spec += *p++;
      star_or_digits();
    }
    int longs = 0;
    while (*p && std::strchr("hlLqjzt", *p)) {
      if (*p == 'l') ++longs;
      if (*p == 'q' || *p == 'L' || *p == 'j') longs = 2;
      ++p;
    }
    const char c = *p;
    if (!c) break;
    buf[0] = 0;
    switch (c) {
      case '%': out += '%'; continue;
      case 'd': case 'i':
        if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)ap.Next<gi64>().v);
        else std::snprintf(buf, sizeof buf, (spec + "d").c_str(), ap.Next<int>());
        break;
      case 'u': case 'x': case 'X': case 'o':
        if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), (unsigned long long)ap.Next<gu64>().v);
        else std::snprintf(buf, sizeof buf, (spec + c).c_str(), ap.Next<unsigned>());
        break;
      case 'c': std::snprintf(buf, sizeof buf, (spec + "c").c_str(), ap.Next<int>()); break;
      case 'p': std::snprintf(buf, sizeof buf, "0x%08x", ap.Next<unsigned>()); break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        std::snprintf(buf, sizeof buf, (spec + c).c_str(), ap.Next<double>());
        break;
      case 's': {
        const gaddr a = ap.Next<gaddr>();
        const std::string s = a ? (GuestMemory::IsMapped(a) ? mem::ReadCStr(a) : "<puntero invalido>") : "(null)";
        std::string tmp(s.size() + 64, 0);
        const int n = std::snprintf(tmp.data(), tmp.size(), (spec + "s").c_str(), s.c_str());
        out.append(tmp.data(), n > 0 ? static_cast<std::size_t>(n) : 0);
        continue;
      }
      case 'n': ap.Next<gaddr>(); continue;
      default: out += spec; out += c; continue;
    }
    out += buf;
  }
  return out;
}
int StoreN(char* dst, gsize cap, const std::string& s) {
  if (dst && cap) {
    const gsize n = static_cast<gsize>(std::min<std::size_t>(s.size(), cap - 1));
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
  }
  return static_cast<int>(s.size());
}
int b_sprintf(char* d, const char* f, Varargs va) { return StoreN(d, 0x7FFFFFFF, VFormat(f, va)); }
int b_snprintf(char* d, gsize n, const char* f, Varargs va) { return StoreN(d, n, VFormat(f, va)); }
int b_vsprintf(char* d, const char* f, VaList ap) { return StoreN(d, 0x7FFFFFFF, VFormat(f, ap)); }
int b_vsnprintf(char* d, gsize n, const char* f, VaList ap) { return StoreN(d, n, VFormat(f, ap)); }
int b_printf(const char* f, Varargs va) {
  const std::string s = VFormat(f, va);
  log::Info("[guest stdout] {}", s);
  return static_cast<int>(s.size());
}
int b_fprintf(gaddr, const char* f, Varargs va) { return b_printf(f, va); }
int b_puts(const char* s) {
  log::Info("[guest stdout] {}", s ? s : "(null)");
  return 0;
}
int b_log_print(int prio, const char* tag, const char* f, Varargs va) {
  log::Info("[logcat {}/{}] {}", prio, tag ? tag : "?", VFormat(f, va));
  return 0;
}
int b_log_vprint(int prio, const char* tag, const char* f, VaList ap) {
  log::Info("[logcat {}/{}] {}", prio, tag ? tag : "?", VFormat(f, ap));
  return 0;
}

// --- runtime C/C++ ------------------------------------------------------------------
int b_zero() { return 0; }
int b_cxa_guard_acquire(GPtr<std::uint8_t> g) { return *g ? 0 : 1; }
void b_cxa_guard_release(GPtr<std::uint8_t> g) { *g = 1; }
void b_cxa_pure_virtual(Cpu& c) { throw GuestFault("llamada a metodo virtual puro (__cxa_pure_virtual)\n" + debug::DumpState(c)); }
void b_stack_chk_fail(Cpu& c) { throw GuestFault("__stack_chk_fail: pila corrupta\n" + debug::DumpState(c)); }
void b_assert2(Cpu& c, const char* file, int line, const char* func, const char* expr) {
  throw GuestFault(std::format("__assert2: {}:{} {}: {}\n{}", file ? file : "?", line, func ? func : "?", expr ? expr : "?",
                               debug::DumpState(c)));
}
void b_abort(Cpu& c) {
  log::Error("el guest llamo a abort()\n{}", debug::DumpState(c));
  throw GuestExit{134};
}
void b_exit(Cpu& c, int code) {
  log::Warn("el guest llamo a exit({})\n{}", code, debug::DumpState(c));
  throw GuestExit{code};
}
gaddr b_errno(Cpu& c) { return c.errno_addr(); }
int b_sysconf(int name) { return name == 0x61 ? 4 : -1; }
gaddr b_getenv(const char*) { return 0; }

// --- pthread minimo -----------------------------------------------------------------
std::mutex g_mu;
std::uint32_t g_next_key = 1;
std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> g_tls;  // (hilo, clave) -> valor
int b_key_create(GPtr<std::uint32_t> key, gaddr) {
  std::lock_guard lk(g_mu);
  *key = g_next_key++;
  return 0;
}
gaddr b_getspecific(Cpu& c, std::uint32_t key) {
  std::lock_guard lk(g_mu);
  auto it = g_tls.find({c.thread_id(), key});
  return it == g_tls.end() ? 0 : it->second;
}
int b_setspecific(Cpu& c, std::uint32_t key, gaddr v) {
  std::lock_guard lk(g_mu);
  g_tls[{c.thread_id(), key}] = v;
  return 0;
}
int b_once(GPtr<std::uint32_t> once, gaddr fn) {
  if (*once == 0) {
    *once = 1;
    call_guest<void>(fn);
  }
  return 0;
}
std::uint32_t b_self(Cpu& c) { return c.thread_id(); }
int b_equal(std::uint32_t a, std::uint32_t b) { return a == b; }

// --- tiempo --------------------------------------------------------------------------
int b_gettimeofday(GPtr<std::uint32_t> tv, gaddr) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (tv) {
    tv[0] = static_cast<std::uint32_t>(us / 1000000);
    tv[1] = static_cast<std::uint32_t>(us % 1000000);
  }
  return 0;
}
std::uint32_t b_time(GPtr<std::uint32_t> out) {
  const auto t = static_cast<std::uint32_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  if (out) *out = t;
  return t;
}
std::uint32_t b_clock() {
  static const auto t0 = std::chrono::steady_clock::now();
  return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}
int b_clock_gettime(int, GPtr<std::uint32_t> ts) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (ts) {
    ts[0] = static_cast<std::uint32_t>(ns / 1000000000);
    ts[1] = static_cast<std::uint32_t>(ns % 1000000000);
  }
  return 0;
}

// --- libm ---------------------------------------------------------------------------
#define ZW_M1(n) double m_##n(double x) { return std::n(x); } float m_##n##f(float x) { return std::n(x); }
#define ZW_M2(n) double m_##n(double x, double y) { return std::n(x, y); } float m_##n##f(float x, float y) { return std::n(x, y); }
ZW_M1(sin) ZW_M1(cos) ZW_M1(tan) ZW_M1(asin) ZW_M1(acos) ZW_M1(atan) ZW_M1(sqrt) ZW_M1(floor) ZW_M1(ceil)
ZW_M1(exp) ZW_M1(log) ZW_M1(log10) ZW_M1(fabs)
ZW_M2(pow) ZW_M2(fmod) ZW_M2(atan2)
#undef ZW_M1
#undef ZW_M2

template <class F>
void IfAbsent(const char* name, F&& reg) {
  if (!hle::IsRegistered(name)) reg();
}
}  // namespace

#define BOOT(name, fn) IfAbsent(name, [] { ZW_HLE(name, fn); })
#define BOOT_FAST(name, fn) IfAbsent(name, [] { ZW_HLE_FAST(name, fn); })

void Register() {
  BOOT("malloc", b_malloc); BOOT("free", b_free); BOOT("calloc", b_calloc); BOOT("realloc", b_realloc);
  BOOT_FAST("memcpy", b_memcpy); BOOT_FAST("memmove", b_memmove); BOOT_FAST("memset", b_memset);
  BOOT_FAST("memcmp", b_memcmp); BOOT_FAST("memchr", b_memchr);
  BOOT_FAST("strlen", b_strlen); BOOT_FAST("strcpy", b_strcpy); BOOT_FAST("strncpy", b_strncpy);
  BOOT_FAST("strcat", b_strcat); BOOT_FAST("strcmp", b_strcmp); BOOT_FAST("strncmp", b_strncmp);
  BOOT_FAST("strcasecmp", b_strcasecmp); BOOT_FAST("strchr", b_strchr); BOOT_FAST("strrchr", b_strrchr);
  BOOT_FAST("strstr", b_strstr); BOOT("strdup", b_strdup); BOOT_FAST("atoi", b_atoi); BOOT_FAST("atof", b_atof);
  BOOT("sprintf", b_sprintf); BOOT("snprintf", b_snprintf); BOOT("vsprintf", b_vsprintf); BOOT("vsnprintf", b_vsnprintf);
  BOOT("printf", b_printf); BOOT("fprintf", b_fprintf); BOOT("puts", b_puts);
  BOOT("__android_log_print", b_log_print); BOOT("__android_log_vprint", b_log_vprint);
  BOOT_FAST("__cxa_atexit", b_zero); BOOT_FAST("__cxa_finalize", b_zero);
  BOOT_FAST("__cxa_guard_acquire", b_cxa_guard_acquire); BOOT_FAST("__cxa_guard_release", b_cxa_guard_release);
  BOOT_FAST("__cxa_guard_abort", b_zero);
  BOOT("__cxa_pure_virtual", b_cxa_pure_virtual); BOOT("__stack_chk_fail", b_stack_chk_fail);
  BOOT("__assert2", b_assert2); BOOT("abort", b_abort); BOOT("exit", b_exit);
  BOOT_FAST("__errno", b_errno); BOOT_FAST("sysconf", b_sysconf); BOOT_FAST("getenv", b_getenv);
  for (const char* n : {"pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock", "pthread_mutex_unlock",
                        "pthread_mutex_trylock", "pthread_mutexattr_init", "pthread_mutexattr_destroy",
                        "pthread_mutexattr_settype", "pthread_cond_init", "pthread_cond_destroy", "pthread_cond_signal",
                        "pthread_cond_broadcast", "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setstacksize",
                        "pthread_attr_setdetachstate", "pthread_key_delete", "sched_yield"})
    if (!hle::IsRegistered(n)) ZW_HLE_FAST(n, b_zero);
  BOOT("pthread_key_create", b_key_create); BOOT("pthread_getspecific", b_getspecific);
  BOOT("pthread_setspecific", b_setspecific); BOOT("pthread_once", b_once);
  BOOT_FAST("pthread_self", b_self); BOOT_FAST("pthread_equal", b_equal);
  BOOT_FAST("gettimeofday", b_gettimeofday); BOOT_FAST("time", b_time); BOOT_FAST("clock", b_clock);
  BOOT_FAST("clock_gettime", b_clock_gettime);
#define ZW_REGM(n) BOOT_FAST(#n, m_##n); BOOT_FAST(#n "f", m_##n##f);
  ZW_REGM(sin) ZW_REGM(cos) ZW_REGM(tan) ZW_REGM(asin) ZW_REGM(acos) ZW_REGM(atan) ZW_REGM(sqrt) ZW_REGM(floor)
  ZW_REGM(ceil) ZW_REGM(exp) ZW_REGM(log) ZW_REGM(log10) ZW_REGM(fabs) ZW_REGM(pow) ZW_REGM(fmod) ZW_REGM(atan2)
#undef ZW_REGM

  // Datos importados (solo si nadie los ha definido).
  if (!hle::FindData("__stack_chk_guard")) mem::W32(hle::DefineData("__stack_chk_guard", 4), 0x2F7A1C55u);
  if (!hle::FindData("__sF")) hle::DefineData("__sF", 3 * 84);
  if (!hle::FindData("_ctype_")) {
    const gaddr ct = GuestMemory::Calloc(1, 257), lo = GuestMemory::Calloc(257, 2), up = GuestMemory::Calloc(257, 2);
    for (int c = 0; c < 256; ++c) {
      std::uint8_t f = 0;
      if (c < 128) {
        if (std::isupper(c)) f |= 0x01;
        if (std::islower(c)) f |= 0x02;
        if (std::isdigit(c)) f |= 0x04;
        if (std::isspace(c)) f |= 0x08;
        if (std::ispunct(c)) f |= 0x10;
        if (std::iscntrl(c)) f |= 0x20;
        if (std::isxdigit(c)) f |= 0x40;
        if (c == ' ') f |= 0x80;
      }
      mem::W8(ct + 1 + c, f);
      mem::W16(lo + 2 * (1 + c), static_cast<std::uint16_t>(c < 128 ? std::tolower(c) : c));
      mem::W16(up + 2 * (1 + c), static_cast<std::uint16_t>(c < 128 ? std::toupper(c) : c));
    }
    mem::W16(lo, 0xFFFF);
    mem::W16(up, 0xFFFF);
    mem::W32(hle::DefineData("_ctype_", 4), ct);
    if (!hle::FindData("_tolower_tab_")) mem::W32(hle::DefineData("_tolower_tab_", 4), lo);
    if (!hle::FindData("_toupper_tab_")) mem::W32(hle::DefineData("_toupper_tab_", 4), up);
  }
  for (const char* n : {"SL_IID_ENGINE", "SL_IID_BUFFERQUEUE", "SL_IID_PLAY"})
    if (!hle::FindData(n)) mem::W32(hle::DefineData(n, 4), GuestMemory::Calloc(1, 16));
}

}  // namespace zw::loader::bootstrap
