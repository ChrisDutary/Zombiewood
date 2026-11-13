// Tests de zw_hle_libc (T4). Cada funcion se invoca por su stub guest (svc -> handler) con
// call_guest, asi que se prueba tambien el marshalling AAPCS softfp de extremo a extremo.
// Si existe C:\Users\chdut\ZombiewoodDev\gamedata (o ZW_GAMEDATA) se prueban ademas los OBB reales.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "hle/libc/libc.h"

using namespace zw;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    ++g_checks;                                                      \
    if (!(cond)) {                                                   \
      ++g_failed;                                                    \
      std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)
#define CHECK_EQ_STR(a, b)                                                                          \
  do {                                                                                              \
    ++g_checks;                                                                                     \
    const std::string _x = (a), _y = (b);                                                           \
    if (_x != _y) {                                                                                 \
      ++g_failed;                                                                                   \
      std::printf("  FALLO %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _x.c_str(), _y.c_str()); \
    }                                                                                               \
  } while (0)

template <class R = std::uint32_t, class... A>
R C(const char* name, const A&... a) {
  return call_guest<R>(hle::StubFor(name), a...);
}
static gaddr S(const std::string& s) { return mem::StrDup(s); }
static int Errno() { return static_cast<int>(mem::R32(Cpu::Current().errno_addr())); }
static gaddr Buf(gsize n) { return GuestMemory::Calloc(1, n); }

// Comparador "guest" para qsort: es un stub HLE, que el guest ve como codigo normal.
static int test_cmp(GPtr<std::int32_t> a, GPtr<std::int32_t> b) { return *a < *b ? -1 : *a > *b ? 1 : 0; }
// Entrada de hilo: incrementa un contador guest 1000 veces bajo un mutex guest.
static gaddr g_mutex = 0, g_counter = 0;
static std::uint32_t test_thread(gaddr arg) {
  for (int i = 0; i < 1000; ++i) {
    C<int>("pthread_mutex_lock", g_mutex);
    mem::W32(g_counter, mem::R32(g_counter) + 1);
    C<int>("pthread_mutex_unlock", g_mutex);
  }
  return arg + 1;
}
static std::atomic<int> g_once_runs{0};
static void test_once_init() { ++g_once_runs; }

static void TestStrings() {
  std::printf("[strings]\n");
  CHECK(C<std::uint32_t>("strlen", S("hola")) == 4);
  CHECK(C<int>("strcmp", S("a"), S("b")) < 0);
  CHECK(C<int>("strcasecmp", S("HeLLo"), S("hello")) == 0);
  const gaddr s = S("  -123xyz");
  const gaddr end = Buf(4);
  CHECK(C<int>("strtol", s, end, 10) == -123 && mem::R32(end) == s + 6);
  CHECK(C<std::uint32_t>("strtoul", S("0xffffffff"), 0, 16) == 0xFFFFFFFFu);
  CHECK(C<gi64>("strtoll", S("-9000000000"), 0, 10).v == -9000000000ll);
  CHECK(C<double>("strtod", S("2.5e3"), 0) == 2500.0);
  CHECK(C<double>("atof", S("1.25")) == 1.25);
  const gaddr str = S("a,b,,c");
  CHECK_EQ_STR(mem::ReadCStr(C<gaddr>("strtok", str, S(","))), "a");
  CHECK_EQ_STR(mem::ReadCStr(C<gaddr>("strtok", 0, S(","))), "b");
  CHECK_EQ_STR(mem::ReadCStr(C<gaddr>("strtok", 0, S(","))), "c");
  CHECK(C<gaddr>("strtok", 0, S(",")) == 0);
  const gaddr d = C<gaddr>("strdup", S("copia"));
  CHECK(d && mem::ReadCStr(d) == "copia" && GuestMemory::UsableSize(d) >= 6);
  C<void>("free", d);
  CHECK_EQ_STR(mem::ReadCStr(C<gaddr>("strerror", 2)), "No such file or directory");
  const gaddr b = Buf(16);
  CHECK(C<std::uint32_t>("strlcpy", b, S("123456789"), 4) == 9 && mem::ReadCStr(b) == "123");
  CHECK(C<int>("memcmp", S("abc"), S("abd"), 3) < 0);
}

static void TestPrintf() {
  std::printf("[printf]\n");
  const gaddr b = Buf(512);
  int n = C<int>("sprintf", b, S("%d|%5.2f|%lld|%s|%x|%-4s|%04d|%c|%%"), -7, 3.14159, gi64{-12345678901ll}, S("txt"),
                 255u, S("ab"), 42, 'Z');
  CHECK_EQ_STR(mem::ReadCStr(b), "-7| 3.14|-12345678901|txt|ff|ab  |0042|Z|%");
  CHECK(n == static_cast<int>(mem::ReadCStr(b).size()));
  C<int>("sprintf", b, S("%p %#x %+d %.3s %e %g"), 0x1234u, 0x1fu, 5, S("abcdef"), 1500.0, 0.0001);
  CHECK_EQ_STR(mem::ReadCStr(b), "0x1234 0x1f +5 abc 1.500000e+03 0.0001");
  // posicionales
  C<int>("sprintf", b, S("%2$s-%1$d-%2$s"), 9, S("x"));
  CHECK_EQ_STR(mem::ReadCStr(b), "x-9-x");
  // '*' y long long tras un int impar (alineacion a 8 de los varargs)
  C<int>("sprintf", b, S("%*d|%d|%llu"), 5, 1, 2, gu64{18446744073709551615ull});
  CHECK_EQ_STR(mem::ReadCStr(b), "    1|2|18446744073709551615");
  // snprintf trunca y devuelve la longitud completa
  CHECK(C<int>("snprintf", b, 5, S("%s"), S("0123456789")) == 10 && mem::ReadCStr(b) == "0123");
  // %ls con wchar_t de 32 bits
  const gaddr w = Buf(16);
  mem::W32(w, 'h');
  mem::W32(w + 4, 'i');
  C<int>("sprintf", b, S("[%ls]"), w);
  CHECK_EQ_STR(mem::ReadCStr(b), "[hi]");
  // vsnprintf con va_list guest (struct { void* __ap; } por valor)
  const gaddr ap = Buf(32);
  mem::W32(ap, 77);
  mem::W32(ap + 4, 0);  // relleno: el double va alineado a 8
  mem::Write<double>(ap + 8, 0.5);
  C<int>("vsnprintf", b, 64, S("%d %.1f"), ap);
  CHECK_EQ_STR(mem::ReadCStr(b), "77 0.5");
  // %n
  const gaddr cnt = Buf(4);
  C<int>("sprintf", b, S("abc%n"), cnt);
  CHECK(mem::R32(cnt) == 3);
  // inf/nan
  C<int>("sprintf", b, S("%f %F"), static_cast<double>(INFINITY), std::nan(""));
  CHECK_EQ_STR(mem::ReadCStr(b), "inf NAN");
}

static void TestScanf() {
  std::printf("[scanf]\n");
  const gaddr i1 = Buf(4), f1 = Buf(4), s1 = Buf(32), x1 = Buf(4), set = Buf(32), nn = Buf(4), d1 = Buf(8);
  int r = C<int>("sscanf", S(" 12 3.5 abc 0x1f [hola mundo]"), S("%d %f %s %x [%[^]]]%n"), i1, f1, s1, x1, set, nn);
  CHECK(r == 5);
  CHECK(static_cast<int>(mem::R32(i1)) == 12);
  CHECK(mem::Read<float>(f1) == 3.5f);
  CHECK_EQ_STR(mem::ReadCStr(s1), "abc");
  CHECK(mem::R32(x1) == 0x1f);
  CHECK_EQ_STR(mem::ReadCStr(set), "hola mundo");
  CHECK(mem::R32(nn) == 29);
  CHECK(C<int>("sscanf", S("2.25e1"), S("%lf"), d1) == 1 && mem::Read<double>(d1) == 22.5);
  CHECK(C<int>("sscanf", S(""), S("%d"), i1) == -1);
  CHECK(C<int>("sscanf", S("abc"), S("%d"), i1) == 0);
  CHECK(C<int>("sscanf", S("v=7;"), S("v=%d;"), i1) == 1 && mem::R32(i1) == 7);
  CHECK(C<int>("sscanf", S("12345"), S("%3d"), i1) == 1 && mem::R32(i1) == 123);
  CHECK(C<int>("sscanf", S("-5 6"), S("%*d %d"), i1) == 1 && mem::R32(i1) == 6);
}

static void TestCtypeWchar() {
  std::printf("[ctype/wchar]\n");
  const gaddr ct = mem::R32(hle::FindData("_ctype_"));
  CHECK(ct && (mem::R8(ct + 1 + 'A') & 0x01) && (mem::R8(ct + 1 + 'a') & 0x02) && (mem::R8(ct + 1 + '7') & 0x04) &&
        (mem::R8(ct + 1 + ' ') & 0x08) && (mem::R8(ct + 1 + 'f') & 0x40));
  const gaddr lo = mem::R32(hle::FindData("_tolower_tab_"));
  CHECK(mem::R16(lo + 2 * (1 + 'Q')) == 'q');
  CHECK(C<int>("isspace", '\t') && !C<int>("isalpha", '1') && C<int>("toupper", 'x') == 'X');
  CHECK_EQ_STR(mem::ReadCStr(C<gaddr>("setlocale", 6, 0)), "C");
  const gaddr w = Buf(64), wf = Buf(64);
  const char* f = "%d-%ls";
  for (std::size_t i = 0; i <= std::strlen(f); ++i) mem::W32(wf + 4 * i, static_cast<std::uint8_t>(f[i]));
  const gaddr arg = Buf(16);
  mem::W32(arg, 'o');
  mem::W32(arg + 4, 'k');
  CHECK(C<int>("swprintf", w, 16, wf, 42, arg) == 5);
  CHECK(C<std::uint32_t>("wcslen", w) == 5 && mem::R32(w) == '4' && mem::R32(w + 16) == 'k');
  CHECK(C<int>("wcscmp", w, w) == 0);
  CHECK(C<std::uint32_t>("btowc", -1) == 0xFFFFFFFFu && C<int>("wctob", 'a') == 'a');
  CHECK(C<std::uint32_t>("wctype", S("alpha")) == 2 && C<int>("iswctype", 'z', 2u) == 1);
  CHECK(C<std::uint32_t>("towupper", 'm') == 'M');
}

static void TestStdioPosix(const fs::path& home) {
  std::printf("[stdio/posix/vfs]\n");
  const std::string dir = "/data/data/com.gameloft.android.ANMP.GloftZOHM";
  const gaddr p = S(dir + "/test.txt");
  gaddr fp = C<gaddr>("fopen", p, S("w"));
  CHECK(fp != 0);
  CHECK(C<int>("fprintf", fp, S("linea %d\nfin"), 1) == 11);
  CHECK(C<int>("fclose", fp) == 0);
  CHECK(fs::exists(home / "saves" / "test.txt") && fs::file_size(home / "saves" / "test.txt") == 11);

  fp = C<gaddr>("fopen", p, S("rb"));
  CHECK(fp != 0);
  const std::int16_t fd = static_cast<std::int16_t>(mem::R16(fp + 14));
  CHECK(fd >= 3);
  const gaddr b = Buf(64);
  CHECK(C<gaddr>("fgets", b, 64, fp) == b && mem::ReadCStr(b) == "linea 1\n");
  CHECK(C<int>("ftell", fp) == 8);
  CHECK(C<std::uint32_t>("fread", b, 1, 64, fp) == 3 && std::memcmp(g2h(b), "fin", 3) == 0);
  CHECK(C<int>("feof", fp) == 1 && (mem::R16(fp + 12) & 0x20));  // __SEOF visible para la macro inline
  CHECK(C<int>("fseek", fp, 0, 2) == 0 && C<int>("ftell", fp) == 11 && !(mem::R16(fp + 12) & 0x20));
  CHECK(C<int>("pread", static_cast<int>(fd), b, 5, 2) == 5 && std::memcmp(g2h(b), "nea 1", 5) == 0);
  C<void>("rewind", fp);
  CHECK(C<int>("fgetc", fp) == 'l' && C<int>("ungetc", 'X', fp) == 'X' && C<int>("fgetc", fp) == 'X');
  const gaddr st = Buf(104);
  CHECK(C<int>("fstat", static_cast<int>(fd), st) == 0 && mem::R64(st + 48) == 11 && (mem::R32(st + 16) & 0xF000) == 0x8000);
  CHECK(C<int>("fclose", fp) == 0);
  CHECK(C<int>("pread", static_cast<int>(fd), b, 1, 0) == -1 && Errno() == 9);

  CHECK(C<int>("stat", p, st) == 0 && mem::R64(st + 48) == 11);
  CHECK(C<int>("stat", S(dir), st) == 0 && (mem::R32(st + 16) & 0xF000) == 0x4000);
  CHECK(C<int>("stat", S("/system/bin/su"), st) == -1 && Errno() == 2);
  CHECK(C<gaddr>("fopen", S("/nope/x.txt"), S("r")) == 0 && Errno() == 2);
  CHECK(C<int>("mkdir", S(dir + "/sub"), 0777) == 0 && fs::is_directory(home / "saves" / "sub"));
  CHECK(C<int>("mkdir", S(dir + "/sub"), 0777) == -1 && Errno() == 17);
  CHECK(C<int>("rename", p, S(dir + "/sub/renamed.txt")) == 0);

  // opendir/readdir con struct dirent de bionic (d_type @18, d_name @19)
  const gaddr dp = C<gaddr>("opendir", S(dir + "/sub"));
  CHECK(dp != 0);
  bool found = false;
  for (gaddr de; (de = C<gaddr>("readdir", dp));)
    if (mem::ReadCStr(de + 19) == "renamed.txt") found = mem::R8(de + 18) == 8;
  CHECK(found);
  CHECK(C<int>("closedir", dp) == 0);
  CHECK(C<int>("remove", S(dir + "/sub/renamed.txt")) == 0 && C<int>("remove", S(dir + "/sub")) == 0);
  CHECK(!fs::exists(home / "saves" / "sub"));

  // open/write/lseek/read con flags de Linux
  const int ofd = C<int>("open", S(dir + "/raw.bin"), 0x42 /*O_RDWR|O_CREAT*/, 0644);
  CHECK(ofd >= 3);
  CHECK(C<int>("write", ofd, S("abcdef"), 6) == 6);
  CHECK(C<int>("lseek", ofd, 2, 0) == 2 && C<int>("read", ofd, b, 10) == 4 && std::memcmp(g2h(b), "cdef", 4) == 0);
  CHECK(C<int>("close", ofd) == 0 && C<int>("unlink", S(dir + "/raw.bin")) == 0);

  // cwd virtual y rutas relativas
  CHECK(C<int>("chdir", S(dir)) == 0);
  CHECK(C<gaddr>("getcwd", b, 64) == b && mem::ReadCStr(b) == dir);
  fp = C<gaddr>("fopen", S("rel.txt"), S("w"));
  CHECK(fp && C<int>("fputs", S("x"), fp) >= 0 && C<int>("fclose", fp) == 0 && fs::exists(home / "saves" / "rel.txt"));
  C<int>("unlink", S("rel.txt"));
  C<int>("chdir", S("/"));

  // /proc/meminfo sintetico
  fp = C<gaddr>("fopen", S("/proc/meminfo"), S("r"));
  CHECK(fp && C<gaddr>("fgets", b, 64, fp) == b && mem::ReadCStr(b).rfind("MemTotal:", 0) == 0);
  C<int>("fclose", fp);
  // stdout por __sF[1]
  CHECK(C<int>("fprintf", hle::FindData("__sF") + 84, S("hola desde el guest %d\n"), 1) > 0);
  CHECK(static_cast<std::int16_t>(mem::R16(hle::FindData("__sF") + 84 + 14)) == 1);
  // tmpfile en memoria
  fp = C<gaddr>("tmpfile");
  CHECK(fp && C<std::uint32_t>("fwrite", S("12345"), 1, 5, fp) == 5);
  C<void>("rewind", fp);
  CHECK(C<int>("fscanf", fp, S("%d"), b) == 1 && mem::R32(b) == 12345);
  C<int>("fclose", fp);
}

static void TestTime() {
  std::printf("[time]\n");
  const gaddr t = Buf(4), tm = Buf(44), b = Buf(64);
  mem::W32(t, 86400 + 3661);  // 1970-01-02 01:01:01
  CHECK(C<gaddr>("gmtime_r", t, tm) == tm);
  CHECK(mem::R32(tm + 20) == 70 && mem::R32(tm + 12) == 2 && mem::R32(tm + 8) == 1 && mem::R32(tm + 24) == 5);
  CHECK(C<std::uint32_t>("strftime", b, 64, S("%Y-%m-%d %H:%M:%S %a %j"), tm) == 27);
  CHECK_EQ_STR(mem::ReadCStr(b), "1970-01-02 01:01:01 Fri 002");
  CHECK(C<int>("timegm", tm) == 86400 + 3661);
  const gaddr lt = C<gaddr>("localtime", t);
  CHECK(lt != 0 && C<int>("mktime", lt) == 86400 + 3661);
  const gaddr tv = Buf(8);
  CHECK(C<int>("gettimeofday", tv, 0) == 0 && mem::R32(tv) > 1600000000u);
  CHECK(C<int>("clock_gettime", 1, tv) == 0);
  CHECK(std::abs(C<int>("time", 0) - static_cast<int>(std::time(nullptr))) < 5);
  CHECK(C<double>("difftime", 10, 4) == 6.0);
  CHECK(C<gaddr>("strptime", S("Thu, 01 Jan 2015 10:20:30 GMT"), S("%a, %d %b %Y %H:%M:%S GMT"), tm) != 0);
  CHECK(mem::R32(tm + 20) == 115 && mem::R32(tm + 16) == 0 && mem::R32(tm + 4) == 20);
}

static void TestPthread() {
  std::printf("[pthread]\n");
  ZW_HLE("test_thread", test_thread);
  ZW_HLE("test_once_init", test_once_init);
  g_mutex = Buf(4);
  g_counter = Buf(4);
  const gaddr attr = Buf(4), rm = Buf(4);
  CHECK(C<int>("pthread_mutexattr_init", attr) == 0 && C<int>("pthread_mutexattr_settype", attr, 1) == 0);
  CHECK(C<int>("pthread_mutex_init", rm, attr) == 0);
  CHECK(C<int>("pthread_mutex_lock", rm) == 0 && C<int>("pthread_mutex_lock", rm) == 0);  // recursivo
  CHECK(C<int>("pthread_mutex_unlock", rm) == 0 && C<int>("pthread_mutex_unlock", rm) == 0);
  CHECK(C<int>("pthread_mutex_unlock", rm) == 1);  // EPERM
  mem::W32(g_mutex, 0);  // PTHREAD_MUTEX_INITIALIZER (creacion perezosa)
  const gaddr tids = Buf(16), ta = Buf(24), ret = Buf(4);
  CHECK(C<int>("pthread_attr_init", ta) == 0 && C<int>("pthread_attr_setstacksize", ta, 512u * 1024) == 0);
  for (int i = 0; i < 4; ++i) CHECK(C<int>("pthread_create", tids + 4 * i, ta, hle::StubFor("test_thread"), 100 + i) == 0);
  for (int i = 0; i < 4; ++i) {
    CHECK(C<int>("pthread_join", mem::R32(tids + 4 * i), ret) == 0 && mem::R32(ret) == static_cast<std::uint32_t>(101 + i));
  }
  CHECK(mem::R32(g_counter) == 4000);
  CHECK(C<std::uint32_t>("pthread_self") == Cpu::Current().thread_id());
  // trylock desde otro hilo host con el mutex tomado aqui -> EBUSY
  C<int>("pthread_mutex_lock", g_mutex);
  int other = -1;
  std::thread th([&] {
    other = C<int>("pthread_mutex_trylock", g_mutex);
    threads::DetachCurrent();
  });
  th.join();
  CHECK(other == 16);
  // cond_timedwait vence con ETIMEDOUT y devuelve el mutex tomado
  const gaddr cv = Buf(4), ts = Buf(8);
  mem::W32(ts, static_cast<std::uint32_t>(std::time(nullptr)) - 1);  // ya vencido
  CHECK(C<int>("pthread_cond_timedwait", cv, g_mutex, ts) == 110);
  CHECK(C<int>("pthread_mutex_unlock", g_mutex) == 0);
  // claves
  const gaddr key = Buf(4);
  CHECK(C<int>("pthread_key_create", key, 0) == 0);
  CHECK(C<int>("pthread_setspecific", mem::R32(key), 0x1234u) == 0 && C<gaddr>("pthread_getspecific", mem::R32(key)) == 0x1234u);
  // once
  const gaddr once = Buf(4);
  C<int>("pthread_once", once, hle::StubFor("test_once_init"));
  C<int>("pthread_once", once, hle::StubFor("test_once_init"));
  CHECK(g_once_runs == 1);
  // __cxa_guard
  const gaddr g = Buf(4);
  CHECK(C<int>("__cxa_guard_acquire", g) == 1);
  C<void>("__cxa_guard_release", g);
  CHECK((mem::R8(g) & 1) && C<int>("__cxa_guard_acquire", g) == 0);
}

static void TestMisc() {
  std::printf("[runtime/math/dl/net]\n");
  ZW_HLE("test_cmp", test_cmp);
  const gaddr arr = Buf(4 * 6);
  const int vals[6] = {5, -2, 9, 0, 3, -7};
  std::memcpy(g2h(arr), vals, sizeof vals);
  C<void>("qsort", arr, 6u, 4u, hle::StubFor("test_cmp"));
  const int want[6] = {-7, -2, 0, 3, 5, 9};
  CHECK(std::memcmp(g2h(arr), want, sizeof want) == 0);
  const gaddr key = Buf(4);
  mem::W32(key, 3);
  CHECK(C<gaddr>("bsearch", key, arr, 6u, 4u, hle::StubFor("test_cmp")) == arr + 12);
  // lrand48: LCG de 48 bits exacto
  C<void>("srand48", 1);
  std::uint64_t X = (1ull << 16) | 0x330E;
  bool ok = true;
  for (int i = 0; i < 5; ++i) {
    X = (0x5DEECE66Dull * X + 0xB) & 0xFFFFFFFFFFFFull;
    ok = ok && C<int>("lrand48") == static_cast<int>(X >> 17);
  }
  CHECK(ok);
  // libm softfp
  CHECK(C<double>("pow", 2.0, 10.0) == 1024.0);
  CHECK(std::fabs(C<float>("sinf", 1.5707963f) - 1.0f) < 1e-6f);
  CHECK(C<double>("atan2", 1.0, 1.0) == std::atan2(1.0, 1.0));
  const gaddr e = Buf(4);
  CHECK(C<float>("frexpf", 8.0f, e) == 0.5f && mem::R32(e) == 4);
  CHECK(C<double>("ldexp", 1.5, 3) == 12.0);
  const gaddr ip = Buf(4);
  CHECK(C<float>("modff", 2.75f, ip) == 0.75f && mem::Read<float>(ip) == 2.0f);
  // errno por hilo y sysconf
  CHECK(C<gaddr>("__errno") == Cpu::Current().errno_addr());
  CHECK(C<int>("sysconf", 0x61) >= 1 && C<int>("sysconf", 0x27) == 4096);
  CHECK(C<gaddr>("getenv", S("NO_EXISTE")) == 0);
  CHECK(C<int>("setenv", S("ZW"), S("1"), 1) == 0 && mem::ReadCStr(C<gaddr>("getenv", S("ZW"))) == "1");
  // libdl
  CHECK(C<gaddr>("dlsym", 0xFFFFFFFFu, S("strlen")) == hle::StubFor("strlen"));
  const gaddr h = C<gaddr>("dlopen", S("libm.so"), 0);
  CHECK(h != 0 && C<gaddr>("dlsym", h, S("sinf")) == hle::StubFor("sinf"));
  CHECK(C<gaddr>("dlopen", S("libnada.so"), 0) == 0 && C<gaddr>("dlerror") != 0 && C<gaddr>("dlerror") == 0);
  // red: todo falla limpio
  CHECK(C<int>("socket", 2, 1, 0) == -1 && Errno() == 13);
  const gaddr res = Buf(4);
  CHECK(C<int>("getaddrinfo", S("example.com"), 0, 0, res) == 8 && mem::R32(res) == 0);
  CHECK(C<gaddr>("gethostbyname", S("example.com")) == 0);
  CHECK(C<std::uint32_t>("inet_addr", S("1.2.3.4")) == 0x04030201u);
  CHECK_EQ_STR(mem::ReadCStr(C<gaddr>("inet_ntoa", 0x04030201u)), "1.2.3.4");
  CHECK(C<int>("select", 0, 0, 0, 0, Buf(8)) == 0);
  // log
  CHECK(C<int>("__android_log_print", 4, S("ZW"), S("valor=%d"), 5) == 1);
  // __stack_chk_guard
  CHECK(mem::R32(hle::FindData("__stack_chk_guard")) != 0);
  // abort -> GuestExit
  bool exited = false;
  try {
    C<void>("abort");
  } catch (const GuestExit& ex) {
    exited = ex.code == 134;
  }
  CHECK(exited);
}

// Lee el directorio central de un ZIP (en memoria guest) y devuelve el n.º de entradas.
static int ZipCount(gaddr fp) {
  C<int>("fseek", fp, -22, 2);
  const gaddr b = Buf(22);
  if (C<std::uint32_t>("fread", b, 1, 22, fp) != 22 || mem::R32(b) != 0x06054b50) return -1;
  return mem::R16(b + 10);
}

static void TestGameData(const fs::path& gd, const fs::path& home) {
  std::printf("[gamedata real: %s]\n", gd.string().c_str());
  hle::libc::VfsConfig cfg;
  cfg.data_dir = home / "data";
  cfg.saves_dir = home / "saves";
  cfg.obb_dir = gd / "obb";
  hle::libc::ConfigureVfs(cfg);
  const std::string files = "/sdcard/Android/data/com.gameloft.android.ANMP.GloftZOHM/files";
  // .bar STORED dentro del main OBB, leido por el VFS (ZIP anidado)
  gaddr fp = C<gaddr>("fopen", S(files + "/actors.bar"), S("rb"));
  CHECK(fp != 0);
  if (fp) {
    const gaddr b = Buf(4);
    CHECK(C<std::uint32_t>("fread", b, 1, 4, fp) == 4 && mem::R32(b) == 0x04034b50);
    C<int>("fseek", fp, 0, 2);
    const int size = C<int>("ftell", fp);
    const int n = ZipCount(fp);
    std::printf("  actors.bar: %d bytes, %d entradas\n", size, n);
    CHECK(n == 520);
    // pread por el fd del FILE (_file @14), como glf::FileStreamImpl::ReadWithOffset
    const int fd = static_cast<std::int16_t>(mem::R16(fp + 14));
    CHECK(C<int>("pread", fd, b, 4, 0) == 4 && mem::R32(b) == 0x04034b50);
    C<int>("fclose", fp);
  }
  // effects.bar: viene del patch (DEFLATED) y tapa al del main
  fp = C<gaddr>("fopen", S(files + "/effects.bar"), S("rb"));
  CHECK(fp != 0);
  if (fp) {
    const int n = ZipCount(fp);
    std::printf("  effects.bar (patch, inflate): %d entradas\n", n);
    CHECK(n == 127);
    C<int>("fclose", fp);
  }
  const gaddr st = Buf(104);
  CHECK(C<int>("stat", S(files + "/zombies.swf"), st) == 0 && mem::R64(st + 48) > 1000000);
  CHECK(C<int>("stat", S("/sdcard/Android/obb/com.gameloft.android.ANMP.GloftZOHM/main.1501.com.gameloft.android.ANMP.GloftZOHM.obb"), st) == 0 &&
        mem::R64(st + 48) == 289401986ull);
  // listar el directorio de datos: debe mostrar las entradas del OBB
  const gaddr dp = C<gaddr>("opendir", S(files));
  int bars = 0;
  for (gaddr de; dp && (de = C<gaddr>("readdir", dp));) {
    const std::string nm = mem::ReadCStr(de + 19);
    if (nm.size() > 4 && nm.substr(nm.size() - 4) == ".bar") ++bars;
  }
  if (dp) C<int>("closedir", dp);
  std::printf("  .bar listados en files/: %d\n", bars);
  CHECK(bars == 15);
  // escribir en el zip no se permite; en la carpeta host superpuesta si
  CHECK(C<gaddr>("fopen", S(files + "/nuevo.txt"), S("w")) != 0);
  C<int>("unlink", S(files + "/nuevo.txt"));
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  log::Init();
  GuestMemory::Init();
  const fs::path home = fs::temp_directory_path() / "zw_test_libc";
  std::error_code ec;
  fs::remove_all(home, ec);
  hle::libc::VfsConfig cfg;
  cfg.data_dir = home / "data";
  cfg.saves_dir = home / "saves";
  cfg.obb_dir.clear();
  hle::libc::ConfigureVfs(cfg);
  hle::libc::Register();
  try {
    TestStrings();
    TestPrintf();
    TestScanf();
    TestCtypeWchar();
    TestStdioPosix(home);
    TestTime();
    TestPthread();
    TestMisc();
    fs::path gd = "C:/Users/chdut/ZombiewoodDev/gamedata";
    if (const char* e = std::getenv("ZW_GAMEDATA")) gd = e;
    if (fs::exists(gd / "obb")) TestGameData(gd, home);
    else std::printf("[gamedata] no encontrado, se omite\n");
  } catch (const GuestFault& f) {
    std::printf("GuestFault: %s\n", f.what());
    ++g_failed;
  }
  std::printf("%d/%d comprobaciones OK\n", g_checks - g_failed, g_checks);
  return g_failed ? 1 : 0;
}
