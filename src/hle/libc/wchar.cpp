// wchar_t de 32 bits (bionic 4.x: conversiones multibyte byte a byte), wcs*/wmem*, wctype.
#include <cwctype>
#include <cstring>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

using W = std::uint32_t;
constexpr W kWEOF = 0xFFFFFFFFu;

gsize hle_wcslen(const W* s) {
  gsize n = 0;
  while (s[n]) ++n;
  return n;
}
gsize hle_wcsnlen(const W* s, gsize m) {
  gsize n = 0;
  while (n < m && s[n]) ++n;
  return n;
}
W* hle_wcscpy(W* d, const W* s) {
  std::memmove(d, s, 4 * (hle_wcslen(s) + 1));
  return d;
}
W* hle_wcsncpy(W* d, const W* s, gsize n) {
  gsize i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
W* hle_wcscat(W* d, const W* s) {
  hle_wcscpy(d + hle_wcslen(d), s);
  return d;
}
W* hle_wcsncat(W* d, const W* s, gsize n) {
  W* p = d + hle_wcslen(d);
  gsize i = 0;
  for (; i < n && s[i]; ++i) p[i] = s[i];
  p[i] = 0;
  return d;
}
int hle_wcscmp(const W* a, const W* b) {
  for (; *a && *a == *b; ++a, ++b) {}
  return *a < *b ? -1 : *a > *b ? 1 : 0;
}
int hle_wcsncmp(const W* a, const W* b, gsize n) {
  for (; n; --n, ++a, ++b) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (!*a) return 0;
  }
  return 0;
}
W Lw(W c) { return (c >= 'A' && c <= 'Z') ? c + 32 : (c < 0x10000 ? static_cast<W>(std::towlower(static_cast<wint_t>(c))) : c); }
W Uw(W c) { return (c >= 'a' && c <= 'z') ? c - 32 : (c < 0x10000 ? static_cast<W>(std::towupper(static_cast<wint_t>(c))) : c); }
int hle_wcscasecmp(const W* a, const W* b) {
  for (;; ++a, ++b) {
    const W x = Lw(*a), y = Lw(*b);
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}
W* hle_wcschr(const W* s, W c) {
  for (;; ++s) {
    if (*s == c) return const_cast<W*>(s);
    if (!*s) return nullptr;
  }
}
W* hle_wcsrchr(const W* s, W c) {
  const W* r = nullptr;
  for (;; ++s) {
    if (*s == c) r = s;
    if (!*s) return const_cast<W*>(r);
  }
}
W* hle_wcsstr(const W* h, const W* n) {
  const gsize nl = hle_wcslen(n);
  if (!nl) return const_cast<W*>(h);
  for (; *h; ++h)
    if (!std::memcmp(h, n, 4 * nl) && hle_wcsnlen(h, nl) == nl) return const_cast<W*>(h);
  return nullptr;
}
gsize hle_wcsspn(const W* s, const W* a) {
  gsize i = 0;
  for (; s[i] && hle_wcschr(a, s[i]); ++i) {}
  return i;
}
gsize hle_wcscspn(const W* s, const W* r) {
  gsize i = 0;
  for (; s[i] && !hle_wcschr(r, s[i]); ++i) {}
  return i;
}
W* hle_wcspbrk(const W* s, const W* a) {
  const gsize i = hle_wcscspn(s, a);
  return s[i] ? const_cast<W*>(s + i) : nullptr;
}
gaddr hle_wcsdup(const W* s) {
  const gsize n = hle_wcslen(s) + 1;
  const gaddr p = GuestMemory::Alloc(4 * n);
  if (p) std::memcpy(g2h(p), s, 4 * n);
  return p;
}
gsize hle_wcsxfrm(W* d, const W* s, gsize n) {
  const gsize l = hle_wcslen(s);
  if (n) {
    const gsize c = l < n - 1 ? l : n - 1;
    std::memmove(d, s, 4 * c);
    d[c] = 0;
  }
  return l;
}
W* hle_wmemchr(const W* s, W c, gsize n) {
  for (gsize i = 0; i < n; ++i)
    if (s[i] == c) return const_cast<W*>(s + i);
  return nullptr;
}
int hle_wmemcmp(const W* a, const W* b, gsize n) {
  for (gsize i = 0; i < n; ++i)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
W* hle_wmemcpy(W* d, const W* s, gsize n) { return n ? static_cast<W*>(std::memmove(d, s, 4ull * n)) : d; }
W* hle_wmemset(W* d, W c, gsize n) {
  for (gsize i = 0; i < n; ++i) d[i] = c;
  return d;
}

// --- conversiones (byte a byte, como bionic 4.x) ----------------------------------------------
W hle_btowc(int c) { return c == -1 ? kWEOF : static_cast<std::uint8_t>(c); }
int hle_wctob(W c) { return c < 256 ? static_cast<int>(c) : -1; }
gsize hle_mbrtowc(GPtr<W> pwc, GPtr<std::uint8_t> s, gsize n, gaddr) {
  if (!s) return 0;
  if (!n) return static_cast<gsize>(-2);
  const W c = *s;
  if (pwc) *pwc = c;
  return c ? 1 : 0;
}
int hle_mbtowc(GPtr<W> pwc, GPtr<std::uint8_t> s, gsize n) {
  if (!s) return 0;
  if (!n) return -1;
  if (pwc) *pwc = *s;
  return *s ? 1 : 0;
}
gsize hle_wcrtomb(GPtr<std::uint8_t> s, W wc, gaddr) {
  if (s) *s = static_cast<std::uint8_t>(wc);
  return 1;
}
int hle_wctomb(GPtr<std::uint8_t> s, W wc) {
  if (!s) return 0;
  *s = static_cast<std::uint8_t>(wc);
  return 1;
}
gsize hle_mbrlen(GPtr<std::uint8_t> s, gsize n, gaddr) { return hle_mbrtowc(GPtr<W>(), s, n, 0); }
int hle_mblen(GPtr<std::uint8_t> s, gsize n) { return hle_mbtowc(GPtr<W>(), s, n); }
int hle_mbsinit(gaddr) { return 1; }
gsize hle_mbstowcs(GPtr<W> d, const char* s, gsize n) {
  const gsize l = static_cast<gsize>(std::strlen(s));
  if (!d) return l;
  gsize i = 0;
  for (; i < n && i < l; ++i) d[i] = static_cast<std::uint8_t>(s[i]);
  if (i < n) d[i] = 0;
  return i;
}
gsize hle_wcstombs(GPtr<char> d, const W* s, gsize n) {
  const gsize l = hle_wcslen(s);
  if (!d) return l;
  gsize i = 0;
  for (; i < n && i < l; ++i) d[i] = static_cast<char>(s[i]);
  if (i < n) d[i] = 0;
  return i;
}
gsize hle_mbsrtowcs(GPtr<W> d, GPtr<std::uint32_t> src, gsize n, gaddr) {
  const char* s = g2h<char>(*src);
  const gsize r = hle_mbstowcs(d, s, n);
  if (d) *src = r < n ? 0 : *src + r;
  return r;
}
gsize hle_wcsrtombs(GPtr<char> d, GPtr<std::uint32_t> src, gsize n, gaddr) {
  const W* s = g2h<W>(*src);
  const gsize r = hle_wcstombs(d, s, n);
  if (d) *src = r < n ? 0 : *src + 4 * r;
  return r;
}
int hle_wcstol(const W* s, GPtr<std::uint32_t> end, int base) {
  std::string n;
  gsize i = 0;
  for (; s[i] && s[i] < 128; ++i) n += static_cast<char>(s[i]);
  char* e = nullptr;
  const long r = std::strtol(n.c_str(), &e, base);
  if (end) *end = h2g(s + (e - n.c_str()));
  return static_cast<int>(r);
}
std::uint32_t hle_wcstoul(const W* s, GPtr<std::uint32_t> end, int base) {
  std::string n;
  for (gsize i = 0; s[i] && s[i] < 128; ++i) n += static_cast<char>(s[i]);
  char* e = nullptr;
  const unsigned long r = std::strtoul(n.c_str(), &e, base);
  if (end) *end = h2g(s + (e - n.c_str()));
  return static_cast<std::uint32_t>(r);
}
double hle_wcstod(const W* s, GPtr<std::uint32_t> end) {
  std::string n;
  for (gsize i = 0; s[i] && s[i] < 128; ++i) n += static_cast<char>(s[i]);
  char* e = nullptr;
  const double r = std::strtod(n.c_str(), &e);
  if (end) *end = h2g(s + (e - n.c_str()));
  return r;
}
float hle_wcstof(const W* s, GPtr<std::uint32_t> end) { return static_cast<float>(hle_wcstod(s, end)); }

// --- clasificacion --------------------------------------------------------------------------
// wctype_t de bionic: alnum=1, alpha, blank, cntrl, digit, graph, lower, print, punct, space, upper, xdigit.
int Is(W c, int t) {
  if (c >= 0x10000 || c == kWEOF) return 0;
  const auto w = static_cast<wint_t>(c);
  switch (t) {
    case 1: return std::iswalnum(w) != 0;
    case 2: return std::iswalpha(w) != 0;
    case 3: return c == ' ' || c == '\t';
    case 4: return std::iswcntrl(w) != 0;
    case 5: return c >= '0' && c <= '9';
    case 6: return std::iswgraph(w) != 0;
    case 7: return std::iswlower(w) != 0;
    case 8: return std::iswprint(w) != 0;
    case 9: return std::iswpunct(w) != 0;
    case 10: return std::iswspace(w) != 0;
    case 11: return std::iswupper(w) != 0;
    case 12: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    default: return 0;
  }
}
std::uint32_t hle_wctype(const char* name) {
  static const char* kN[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                             "lower", "print", "punct", "space", "upper", "xdigit"};
  for (int i = 0; i < 12; ++i)
    if (name && !std::strcmp(name, kN[i])) return static_cast<std::uint32_t>(i + 1);
  return 0;
}
int hle_iswctype(W c, std::uint32_t t) { return Is(c, static_cast<int>(t)); }
#define ISW(n, t) \
  int hle_isw##n(W c) { return Is(c, t); }
ISW(alnum, 1) ISW(alpha, 2) ISW(blank, 3) ISW(cntrl, 4) ISW(digit, 5) ISW(graph, 6) ISW(lower, 7) ISW(print, 8)
ISW(punct, 9) ISW(space, 10) ISW(upper, 11) ISW(xdigit, 12)
#undef ISW
W hle_towlower(W c) { return c == kWEOF ? c : Lw(c); }
W hle_towupper(W c) { return c == kWEOF ? c : Uw(c); }
int hle_wcwidth(W c) { return c ? (c < 32 ? -1 : 1) : 0; }

// --- E/S y formato ancho ------------------------------------------------------------------------
W hle_getwc(gaddr fp) {
  const int c = StdioGetc(fp);
  return c < 0 ? kWEOF : static_cast<W>(c);
}
W hle_putwc(W c, gaddr fp) { return StdioPutc(static_cast<int>(c & 0xFF), fp) < 0 ? kWEOF : c; }
W hle_ungetwc(W c, gaddr fp) { return c == kWEOF || StdioUngetc(static_cast<int>(c & 0xFF), fp) < 0 ? kWEOF : c; }
W hle_getwchar() { return hle_getwc(FindData("__sF")); }
W hle_putwchar(W c) { return hle_putwc(c, FindData("__sF") + 84); }
int hle_fputws(const W* s, gaddr fp) {
  std::string n;
  for (; *s; ++s) n += static_cast<char>(*s);
  return StdioWrite(fp, n.data(), static_cast<std::uint32_t>(n.size())) < 0 ? -1 : 0;
}
int hle_fwide(gaddr, int mode) { return mode; }

int StoreW(W* d, gsize n, const std::u32string& s) {
  if (!d || !n) return -1;
  if (s.size() >= n) {
    std::memcpy(d, s.data(), 4ull * (n - 1));
    d[n - 1] = 0;
    return -1;  // swprintf: truncado -> -1
  }
  std::memcpy(d, s.data(), 4 * s.size());
  d[s.size()] = 0;
  return static_cast<int>(s.size());
}
int hle_swprintf(W* d, gsize n, const W* fmt, Varargs va) { return StoreW(d, n, FormatPrintfW(fmt, va)); }
int hle_vswprintf(W* d, gsize n, const W* fmt, VaList ap) { return StoreW(d, n, FormatPrintfW(fmt, ap)); }
int ToFileW(gaddr fp, const std::u32string& s) {
  std::string n;
  for (char32_t c : s) n += static_cast<char>(c < 256 ? c : '?');
  return StdioWrite(fp, n.data(), static_cast<std::uint32_t>(n.size())) < 0 ? -1 : static_cast<int>(s.size());
}
int hle_fwprintf(gaddr fp, const W* fmt, Varargs va) { return ToFileW(fp, FormatPrintfW(fmt, va)); }
int hle_vfwprintf(gaddr fp, const W* fmt, VaList ap) { return ToFileW(fp, FormatPrintfW(fmt, ap)); }
int hle_wprintf(const W* fmt, Varargs va) { return ToFileW(FindData("__sF") + 84, FormatPrintfW(fmt, va)); }

gsize hle_wcsftime(W* d, gsize n, const W* fmt, gaddr tm) {
  std::string f;
  for (; *fmt; ++fmt) f += static_cast<char>(*fmt < 128 ? *fmt : '?');
  const std::string s = Strftime(f.c_str(), tm);
  if (s.size() + 1 > n) return 0;
  for (std::size_t i = 0; i < s.size(); ++i) d[i] = static_cast<std::uint8_t>(s[i]);
  d[s.size()] = 0;
  return static_cast<gsize>(s.size());
}

}  // namespace

void RegisterWchar() {
  ZW_HLE_FAST("wcslen", hle_wcslen);
  ZW_HLE_FAST("wcsnlen", hle_wcsnlen);
  ZW_HLE_FAST("wcscpy", hle_wcscpy);
  ZW_HLE_FAST("wcsncpy", hle_wcsncpy);
  ZW_HLE_FAST("wcscat", hle_wcscat);
  ZW_HLE_FAST("wcsncat", hle_wcsncat);
  ZW_HLE_FAST("wcscmp", hle_wcscmp);
  ZW_HLE_FAST("wcscoll", hle_wcscmp);
  ZW_HLE_FAST("wcsncmp", hle_wcsncmp);
  ZW_HLE_FAST("wcscasecmp", hle_wcscasecmp);
  ZW_HLE_FAST("wcschr", hle_wcschr);
  ZW_HLE_FAST("wcsrchr", hle_wcsrchr);
  ZW_HLE_FAST("wcsstr", hle_wcsstr);
  ZW_HLE_FAST("wcsspn", hle_wcsspn);
  ZW_HLE_FAST("wcscspn", hle_wcscspn);
  ZW_HLE_FAST("wcspbrk", hle_wcspbrk);
  ZW_HLE_FAST("wcsdup", hle_wcsdup);
  ZW_HLE_FAST("wcsxfrm", hle_wcsxfrm);
  ZW_HLE_FAST("wmemchr", hle_wmemchr);
  ZW_HLE_FAST("wmemcmp", hle_wmemcmp);
  ZW_HLE_FAST("wmemcpy", hle_wmemcpy);
  ZW_HLE_FAST("wmemmove", hle_wmemcpy);
  ZW_HLE_FAST("wmemset", hle_wmemset);
  ZW_HLE_FAST("btowc", hle_btowc);
  ZW_HLE_FAST("wctob", hle_wctob);
  ZW_HLE_FAST("mbrtowc", hle_mbrtowc);
  ZW_HLE_FAST("mbtowc", hle_mbtowc);
  ZW_HLE_FAST("wcrtomb", hle_wcrtomb);
  ZW_HLE_FAST("wctomb", hle_wctomb);
  ZW_HLE_FAST("mbrlen", hle_mbrlen);
  ZW_HLE_FAST("mblen", hle_mblen);
  ZW_HLE_FAST("mbsinit", hle_mbsinit);
  ZW_HLE_FAST("mbstowcs", hle_mbstowcs);
  ZW_HLE_FAST("wcstombs", hle_wcstombs);
  ZW_HLE_FAST("mbsrtowcs", hle_mbsrtowcs);
  ZW_HLE_FAST("wcsrtombs", hle_wcsrtombs);
  ZW_HLE_FAST("wcstol", hle_wcstol);
  ZW_HLE_FAST("wcstoul", hle_wcstoul);
  ZW_HLE_FAST("wcstod", hle_wcstod);
  ZW_HLE_FAST("wcstof", hle_wcstof);
  ZW_HLE_FAST("wctype", hle_wctype);
  ZW_HLE_FAST("iswctype", hle_iswctype);
  ZW_HLE_FAST("iswalnum", hle_iswalnum);
  ZW_HLE_FAST("iswalpha", hle_iswalpha);
  ZW_HLE_FAST("iswblank", hle_iswblank);
  ZW_HLE_FAST("iswcntrl", hle_iswcntrl);
  ZW_HLE_FAST("iswdigit", hle_iswdigit);
  ZW_HLE_FAST("iswgraph", hle_iswgraph);
  ZW_HLE_FAST("iswlower", hle_iswlower);
  ZW_HLE_FAST("iswprint", hle_iswprint);
  ZW_HLE_FAST("iswpunct", hle_iswpunct);
  ZW_HLE_FAST("iswspace", hle_iswspace);
  ZW_HLE_FAST("iswupper", hle_iswupper);
  ZW_HLE_FAST("iswxdigit", hle_iswxdigit);
  ZW_HLE_FAST("towlower", hle_towlower);
  ZW_HLE_FAST("towupper", hle_towupper);
  ZW_HLE_FAST("wcwidth", hle_wcwidth);
  ZW_HLE("getwc", hle_getwc);
  ZW_HLE("fgetwc", hle_getwc);
  ZW_HLE("putwc", hle_putwc);
  ZW_HLE("fputwc", hle_putwc);
  ZW_HLE("ungetwc", hle_ungetwc);
  ZW_HLE("getwchar", hle_getwchar);
  ZW_HLE("putwchar", hle_putwchar);
  ZW_HLE("fputws", hle_fputws);
  ZW_HLE("fwide", hle_fwide);
  ZW_HLE("swprintf", hle_swprintf);
  ZW_HLE("vswprintf", hle_vswprintf);
  ZW_HLE("fwprintf", hle_fwprintf);
  ZW_HLE("vfwprintf", hle_vfwprintf);
  ZW_HLE("wprintf", hle_wprintf);
  ZW_HLE("wcsftime", hle_wcsftime);
}

}  // namespace zw::hle::libc
