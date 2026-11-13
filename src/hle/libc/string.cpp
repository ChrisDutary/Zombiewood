// str*, strto*, ato*, strerror, abs/div.
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

inline int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

gsize hle_strlen(const char* s) { return static_cast<gsize>(std::strlen(s)); }
gsize hle_strnlen(const char* s, gsize n) {
  gsize i = 0;
  while (i < n && s[i]) ++i;
  return i;
}
char* hle_strcpy(char* d, const char* s) { return static_cast<char*>(std::memmove(d, s, std::strlen(s) + 1)); }
char* hle_stpcpy(char* d, const char* s) {
  const std::size_t n = std::strlen(s);
  std::memmove(d, s, n + 1);
  return d + n;
}
char* hle_strncpy(char* d, const char* s, gsize n) {
  gsize i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
gsize hle_strlcpy(char* d, const char* s, gsize n) {
  const gsize len = static_cast<gsize>(std::strlen(s));
  if (n) {
    const gsize c = len < n - 1 ? len : n - 1;
    std::memmove(d, s, c);
    d[c] = 0;
  }
  return len;
}
gsize hle_strlcat(char* d, const char* s, gsize n) {
  gsize dl = 0;
  while (dl < n && d[dl]) ++dl;
  const gsize sl = static_cast<gsize>(std::strlen(s));
  if (dl == n) return n + sl;
  const gsize room = n - dl - 1;
  const gsize c = sl < room ? sl : room;
  std::memmove(d + dl, s, c);
  d[dl + c] = 0;
  return dl + sl;
}
char* hle_strcat(char* d, const char* s) {
  std::memmove(d + std::strlen(d), s, std::strlen(s) + 1);
  return d;
}
char* hle_strncat(char* d, const char* s, gsize n) {
  char* p = d + std::strlen(d);
  gsize i = 0;
  for (; i < n && s[i]; ++i) p[i] = s[i];
  p[i] = 0;
  return d;
}
int hle_strcmp(const char* a, const char* b) {
  const int r = std::strcmp(a, b);
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}
int hle_strncmp(const char* a, const char* b, gsize n) {
  if (!n) return 0;
  const int r = std::strncmp(a, b, n);
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}
int hle_strcasecmp(const char* a, const char* b) {
  const auto* x = reinterpret_cast<const unsigned char*>(a);
  const auto* y = reinterpret_cast<const unsigned char*>(b);
  for (;; ++x, ++y) {
    const int d = lc(*x) - lc(*y);
    if (d || !*x) return d;
  }
}
int hle_strncasecmp(const char* a, const char* b, gsize n) {
  const auto* x = reinterpret_cast<const unsigned char*>(a);
  const auto* y = reinterpret_cast<const unsigned char*>(b);
  for (; n; --n, ++x, ++y) {
    const int d = lc(*x) - lc(*y);
    if (d || !*x) return d;
  }
  return 0;
}
char* hle_strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
char* hle_strrchr(const char* s, int c) { return const_cast<char*>(std::strrchr(s, c)); }
char* hle_strstr(const char* h, const char* n) { return const_cast<char*>(std::strstr(h, n)); }
char* hle_strcasestr(const char* h, const char* n) {
  const std::size_t nl = std::strlen(n);
  for (; *h; ++h)
    if (!hle_strncasecmp(h, n, static_cast<gsize>(nl))) return const_cast<char*>(h);
  return nl ? nullptr : const_cast<char*>(h);
}
char* hle_strpbrk(const char* s, const char* a) { return const_cast<char*>(std::strpbrk(s, a)); }
gsize hle_strspn(const char* s, const char* a) { return static_cast<gsize>(std::strspn(s, a)); }
gsize hle_strcspn(const char* s, const char* r) { return static_cast<gsize>(std::strcspn(s, r)); }
gaddr hle_strdup(const char* s) {
  if (!s) return 0;
  const gsize n = static_cast<gsize>(std::strlen(s));
  const gaddr p = GuestMemory::Alloc(n + 1);
  if (!p) {
    SetErrno(err::NOMEM);
    return 0;
  }
  std::memcpy(g2h(p), s, n + 1);
  return p;
}
gaddr hle_strndup(const char* s, gsize n) {
  const gsize l = hle_strnlen(s, n);
  const gaddr p = GuestMemory::Alloc(l + 1);
  if (!p) return 0;
  std::memcpy(g2h(p), s, l);
  mem::W8(p + l, 0);
  return p;
}
gsize hle_strxfrm(char* d, const char* s, gsize n) { return hle_strlcpy(d, s, n); }

char* tok_impl(char* s, const char* delim, gaddr& save) {
  if (!s) {
    if (!save) return nullptr;
    s = g2h<char>(save);
  }
  s += std::strspn(s, delim);
  if (!*s) {
    save = 0;
    return nullptr;
  }
  char* e = s + std::strcspn(s, delim);
  if (*e) {
    *e = 0;
    save = h2g(e + 1);
  } else {
    save = 0;
  }
  return s;
}
char* hle_strtok(char* s, const char* delim) { return tok_impl(s, delim, TS().strtok_save); }
char* hle_strtok_r(char* s, const char* delim, GPtr<std::uint32_t> save) {
  gaddr sv = *save;
  char* r = tok_impl(s, delim, sv);
  *save = sv;
  return r;
}
char* hle_strsep(GPtr<std::uint32_t> sp, const char* delim) {
  if (!*sp) return nullptr;
  char* s = g2h<char>(*sp);
  char* e = s + std::strcspn(s, delim);
  if (*e) {
    *e = 0;
    *sp = h2g(e + 1);
  } else {
    *sp = 0;
  }
  return s;
}

// --- conversiones -------------------------------------------------------------------------
template <class F>
auto WithErrno(F&& f) {
  errno = 0;
  auto r = f();
  if (errno == ERANGE) SetErrno(err::RANGE);
  else if (errno == EINVAL) SetErrno(err::INVAL);
  return r;
}
void SetEnd(GPtr<std::uint32_t> end, const char* p) {
  if (end) *end = h2g(p);
}
int hle_strtol(const char* s, GPtr<std::uint32_t> end, int base) {
  char* e = nullptr;
  const long r = WithErrno([&] { return std::strtol(s, &e, base); });  // long del host (MSVC) = 32 bits
  SetEnd(end, e);
  return static_cast<int>(r);
}
std::uint32_t hle_strtoul(const char* s, GPtr<std::uint32_t> end, int base) {
  char* e = nullptr;
  const unsigned long r = WithErrno([&] { return std::strtoul(s, &e, base); });
  SetEnd(end, e);
  return static_cast<std::uint32_t>(r);
}
gi64 hle_strtoll(const char* s, GPtr<std::uint32_t> end, int base) {
  char* e = nullptr;
  const long long r = WithErrno([&] { return std::strtoll(s, &e, base); });
  SetEnd(end, e);
  return gi64(r);
}
gu64 hle_strtoull(const char* s, GPtr<std::uint32_t> end, int base) {
  char* e = nullptr;
  const unsigned long long r = WithErrno([&] { return std::strtoull(s, &e, base); });
  SetEnd(end, e);
  return gu64(r);
}
double hle_strtod(const char* s, GPtr<std::uint32_t> end) {
  char* e = nullptr;
  const double r = WithErrno([&] { return std::strtod(s, &e); });
  SetEnd(end, e);
  return r;
}
float hle_strtof(const char* s, GPtr<std::uint32_t> end) {
  char* e = nullptr;
  const float r = WithErrno([&] { return std::strtof(s, &e); });
  SetEnd(end, e);
  return r;
}
int hle_atoi(const char* s) { return static_cast<int>(std::strtol(s, nullptr, 10)); }
int hle_atol(const char* s) { return static_cast<int>(std::strtol(s, nullptr, 10)); }
gi64 hle_atoll(const char* s) { return gi64(std::strtoll(s, nullptr, 10)); }
double hle_atof(const char* s) { return std::strtod(s, nullptr); }

int hle_abs(int x) { return x < 0 ? -x : x; }
gi64 hle_llabs(gi64 x) { return gi64(x.v < 0 ? -x.v : x.v); }
struct DivT {
  std::int32_t quot, rem;
};
void hle_div(GPtr<DivT> out, int a, int b) {
  if (!b) return;
  out->quot = a / b;
  out->rem = a % b;
}
struct LLDivT {
  std::int64_t quot, rem;
};
void hle_lldiv(GPtr<LLDivT> out, gi64 a, gi64 b) {
  if (!b.v) return;
  LLDivT r{a.v / b.v, a.v % b.v};
  mem::Write<LLDivT>(out.addr, r);
}

// --- strerror ------------------------------------------------------------------------------
const char* ErrText(int e) {
  switch (e) {
    case 0: return "Success";
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 3: return "No such process";
    case 4: return "Interrupted system call";
    case 5: return "I/O error";
    case 6: return "No such device or address";
    case 7: return "Argument list too long";
    case 8: return "Exec format error";
    case 9: return "Bad file number";
    case 10: return "No child processes";
    case 11: return "Try again";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 14: return "Bad address";
    case 16: return "Device or resource busy";
    case 17: return "File exists";
    case 18: return "Cross-device link";
    case 19: return "No such device";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 23: return "File table overflow";
    case 24: return "Too many open files";
    case 25: return "Not a typewriter";
    case 27: return "File too large";
    case 28: return "No space left on device";
    case 29: return "Illegal seek";
    case 30: return "Read-only file system";
    case 32: return "Broken pipe";
    case 33: return "Math argument out of domain of func";
    case 34: return "Math result not representable";
    case 35: return "Resource deadlock would occur";
    case 36: return "File name too long";
    case 38: return "Function not implemented";
    case 39: return "Directory not empty";
    case 88: return "Socket operation on non-socket";
    case 95: return "Operation not supported on transport endpoint";
    case 97: return "Address family not supported by protocol";
    case 100: return "Network is down";
    case 101: return "Network is unreachable";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    case 113: return "No route to host";
    case 115: return "Operation now in progress";
    default: return nullptr;
  }
}
gaddr hle_strerror(int e) {
  if (const char* t = ErrText(e)) return InternString(t);
  return InternString("Unknown error " + std::to_string(e));
}
int hle_strerror_r(int e, char* buf, gsize n) {
  const char* t = ErrText(e);
  const std::string s = t ? t : "Unknown error " + std::to_string(e);
  if (!n) return err::RANGE;
  hle_strlcpy(buf, s.c_str(), n);
  return s.size() + 1 > n ? err::RANGE : 0;
}

}  // namespace

void RegisterString() {
  ZW_HLE_FAST("strlen", hle_strlen);
  ZW_HLE_FAST("strnlen", hle_strnlen);
  ZW_HLE_FAST("strcpy", hle_strcpy);
  ZW_HLE_FAST("stpcpy", hle_stpcpy);
  ZW_HLE_FAST("strncpy", hle_strncpy);
  ZW_HLE_FAST("strlcpy", hle_strlcpy);
  ZW_HLE_FAST("strlcat", hle_strlcat);
  ZW_HLE_FAST("strcat", hle_strcat);
  ZW_HLE_FAST("strncat", hle_strncat);
  ZW_HLE_FAST("strcmp", hle_strcmp);
  ZW_HLE_FAST("strncmp", hle_strncmp);
  ZW_HLE_FAST("strcoll", hle_strcmp);
  ZW_HLE_FAST("strcasecmp", hle_strcasecmp);
  ZW_HLE_FAST("strncasecmp", hle_strncasecmp);
  ZW_HLE_FAST("strchr", hle_strchr);
  ZW_HLE_FAST("index", hle_strchr);
  ZW_HLE_FAST("strrchr", hle_strrchr);
  ZW_HLE_FAST("rindex", hle_strrchr);
  ZW_HLE_FAST("strstr", hle_strstr);
  ZW_HLE_FAST("strcasestr", hle_strcasestr);
  ZW_HLE_FAST("strpbrk", hle_strpbrk);
  ZW_HLE_FAST("strspn", hle_strspn);
  ZW_HLE_FAST("strcspn", hle_strcspn);
  ZW_HLE_FAST("strdup", hle_strdup);
  ZW_HLE_FAST("strndup", hle_strndup);
  ZW_HLE_FAST("strxfrm", hle_strxfrm);
  ZW_HLE_FAST("strtok", hle_strtok);
  ZW_HLE_FAST("strtok_r", hle_strtok_r);
  ZW_HLE_FAST("strsep", hle_strsep);

  ZW_HLE_FAST("strtol", hle_strtol);
  ZW_HLE_FAST("strtoul", hle_strtoul);
  ZW_HLE_FAST("strtoll", hle_strtoll);
  ZW_HLE_FAST("strtoull", hle_strtoull);
  ZW_HLE_FAST("strtoimax", hle_strtoll);
  ZW_HLE_FAST("strtoumax", hle_strtoull);
  ZW_HLE_FAST("strtoq", hle_strtoll);
  ZW_HLE_FAST("strtouq", hle_strtoull);
  ZW_HLE_FAST("strtod", hle_strtod);
  ZW_HLE_FAST("strtold", hle_strtod);  // long double == double en ARM
  ZW_HLE_FAST("strtof", hle_strtof);
  ZW_HLE_FAST("atoi", hle_atoi);
  ZW_HLE_FAST("atol", hle_atol);
  ZW_HLE_FAST("atoll", hle_atoll);
  ZW_HLE_FAST("atof", hle_atof);
  ZW_HLE_FAST("abs", hle_abs);
  ZW_HLE_FAST("labs", hle_abs);
  ZW_HLE_FAST("llabs", hle_llabs);
  ZW_HLE_FAST("imaxabs", hle_llabs);
  ZW_HLE_FAST("div", hle_div);
  ZW_HLE_FAST("ldiv", hle_div);
  ZW_HLE_FAST("lldiv", hle_lldiv);

  ZW_HLE("strerror", hle_strerror);
  ZW_HLE_FAST("strerror_r", hle_strerror_r);
}

}  // namespace zw::hle::libc
