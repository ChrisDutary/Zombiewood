// Motor printf/scanf de la HLE (semantica BSD/bionic) sobre argumentos guest (VaList).
// - printf: flags "-+ #0", anchura/precision (incl. '*' y posicionales "%n$"/"*m$"), longitudes
//   hh h l ll q L j z t, conversiones d i u o x X c s p n f F e E g G a A C S %, %ls/%lc.
//   long = 32 bits, long long/intmax_t = 64 bits (alineado a 8), double softfp, wchar_t 32 bits.
// - scanf: %d %i %u %o %x %X %p %f %e %g %a %s %c %[...] %n %%, '*', anchura, longitudes.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

enum Len : std::uint8_t { kNone, kHH, kH, kL, kLL, kBigL, kJ, kZ, kT };
enum class AK : std::uint8_t { None, I32, I64, F64 };

struct Spec {
  bool literal = false;
  std::size_t lb = 0, le = 0;  // literal [lb, le)
  bool left = false, plus = false, space = false, alt = false, zero = false;
  int width = -1, width_arg = -1;
  int prec = -1, prec_arg = -1;
  Len len = kNone;
  std::uint32_t conv = 0;
  int arg = -1;
};

template <class C>
inline std::uint32_t U(C c) {
  if constexpr (sizeof(C) == 1) return static_cast<unsigned char>(c);
  else return static_cast<std::uint32_t>(c);
}

template <class C>
int ParseNum(const C*& p) {
  int v = 0;
  while (U(*p) >= '0' && U(*p) <= '9') {
    if (v < 100000000) v = v * 10 + static_cast<int>(U(*p) - '0');
    ++p;
  }
  return v;
}

AK KindOf(const Spec& s) {
  switch (s.conv) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
      return (s.len == kLL || s.len == kJ || s.len == kBigL) ? AK::I64 : AK::I32;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
      return AK::F64;
    case 'c': case 's': case 'p': case 'n': case 'C': case 'S': case 'D': case 'O': case 'U':
      return AK::I32;
    default:
      return AK::None;
  }
}

template <class C>
std::vector<Spec> Parse(const C* fmt) {
  std::vector<Spec> out;
  const C* p = fmt;
  int seq = 0;  // indice secuencial (base 1)
  while (U(*p)) {
    if (U(*p) != '%') {
      Spec s;
      s.literal = true;
      s.lb = static_cast<std::size_t>(p - fmt);
      while (U(*p) && U(*p) != '%') ++p;
      s.le = static_cast<std::size_t>(p - fmt);
      out.push_back(s);
      continue;
    }
    const C* start = p++;
    Spec s;
    // posicional "%n$"
    {
      const C* q = p;
      if (U(*q) >= '1' && U(*q) <= '9') {
        const int n = ParseNum(q);
        if (U(*q) == '$') {
          s.arg = n;
          p = q + 1;
        }
      }
    }
    for (;; ++p) {
      const auto c = U(*p);
      if (c == '-') s.left = true;
      else if (c == '+') s.plus = true;
      else if (c == ' ') s.space = true;
      else if (c == '#') s.alt = true;
      else if (c == '0') s.zero = true;
      else if (c == '\'') {}
      else break;
    }
    auto star = [&](int& val, int& arg) {
      if (U(*p) == '*') {
        ++p;
        const C* q = p;
        if (U(*q) >= '1' && U(*q) <= '9') {
          const int n = ParseNum(q);
          if (U(*q) == '$') {
            arg = n;
            p = q + 1;
            return;
          }
        }
        arg = 0;  // secuencial: se numera abajo
      } else {
        val = ParseNum(p);
      }
    };
    if (U(*p) == '*' || (U(*p) >= '1' && U(*p) <= '9')) star(s.width, s.width_arg);
    if (U(*p) == '.') {
      ++p;
      s.prec = 0;
      star(s.prec, s.prec_arg);
    }
    for (bool more = true; more;) {
      switch (U(*p)) {
        case 'h': s.len = s.len == kH ? kHH : kH; ++p; break;
        case 'l': s.len = s.len == kL ? kLL : kL; ++p; break;
        case 'q': s.len = kLL; ++p; break;
        case 'L': s.len = kBigL; ++p; break;
        case 'j': s.len = kJ; ++p; break;
        case 'z': s.len = kZ; ++p; break;
        case 't': s.len = kT; ++p; break;
        default: more = false;
      }
    }
    s.conv = U(*p);
    if (!s.conv) {  // '%' suelto al final: se copia literal
      Spec l;
      l.literal = true;
      l.lb = static_cast<std::size_t>(start - fmt);
      l.le = static_cast<std::size_t>(p - fmt);
      out.push_back(l);
      break;
    }
    ++p;
    if (s.conv == '%') {
      Spec l;
      l.literal = true;
      l.lb = static_cast<std::size_t>(p - 1 - fmt);
      l.le = static_cast<std::size_t>(p - fmt);
      out.push_back(l);
      continue;
    }
    if (s.width_arg == 0) s.width_arg = ++seq;
    if (s.prec_arg == 0) s.prec_arg = ++seq;
    if (s.arg < 0 && KindOf(s) != AK::None) s.arg = ++seq;
    out.push_back(s);
  }
  return out;
}

// Lee los argumentos (en orden de indice) segun los tipos pedidos por el formato.
std::vector<std::uint64_t> FetchArgs(const std::vector<Spec>& specs, VaList& ap) {
  std::vector<AK> kinds;
  auto need = [&](int idx, AK k) {
    if (idx <= 0) return;
    if (kinds.size() <= static_cast<std::size_t>(idx)) kinds.resize(idx + 1, AK::None);
    if (kinds[idx] == AK::None) kinds[idx] = k;
  };
  for (const auto& s : specs) {
    if (s.literal) continue;
    need(s.width_arg, AK::I32);
    need(s.prec_arg, AK::I32);
    need(s.arg, KindOf(s));
  }
  std::vector<std::uint64_t> v(kinds.size(), 0);
  for (std::size_t i = 1; i < kinds.size(); ++i) {
    if (kinds[i] == AK::I64 || kinds[i] == AK::F64) v[i] = ap.NextDword();
    else v[i] = ap.NextWord();
  }
  return v;
}

std::string ToBase(std::uint64_t u, unsigned base, bool upper) {
  const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char buf[80];
  int n = 0;
  do {
    buf[n++] = dig[u % base];
    u /= base;
  } while (u);
  std::string s;
  while (n) s += buf[--n];
  return s;
}

// Lectura de cadenas guest con limite.
std::string GuestStr(gaddr a, int prec) {
  if (!a) return prec >= 0 && prec < 6 ? std::string("(null)").substr(0, prec) : "(null)";
  if (!GuestMemory::IsMapped(a)) return "<?>";
  if (prec < 0) return mem::ReadCStr(a);
  std::string s;
  for (int i = 0; i < prec; ++i) {
    const char c = static_cast<char>(mem::R8(a + i));
    if (!c) break;
    s += c;
  }
  return s;
}
std::u32string GuestWStr(gaddr a, int prec) {
  if (!a) return U"(null)";
  std::u32string s;
  for (int i = 0; prec < 0 || i < prec; ++i) {
    const char32_t c = mem::R32(a + 4 * i);
    if (!c) break;
    s += c;
  }
  return s;
}

template <class O>
void AppendNarrow(std::basic_string<O>& out, const std::string& s) {
  for (unsigned char c : s) out += static_cast<O>(c);
}
template <class O>
void Pad(std::basic_string<O>& out, int n, char c) {
  for (int i = 0; i < n; ++i) out += static_cast<O>(c);
}

template <class O>
void Field(std::basic_string<O>& out, const Spec& s, int width, const std::basic_string<O>& body) {
  const int padn = width > static_cast<int>(body.size()) ? width - static_cast<int>(body.size()) : 0;
  if (!s.left) Pad(out, padn, ' ');
  out += body;
  if (s.left) Pad(out, padn, ' ');
}

template <class O, class C>
std::basic_string<O> Render(const C* fmt, VaList& ap) {
  const auto specs = Parse(fmt);
  const auto args = FetchArgs(specs, ap);
  auto argv = [&](int i) -> std::uint64_t { return i > 0 && static_cast<std::size_t>(i) < args.size() ? args[i] : 0; };
  std::basic_string<O> out;
  for (const auto& s0 : specs) {
    if (s0.literal) {
      for (std::size_t i = s0.lb; i < s0.le; ++i) out += static_cast<O>(U(fmt[i]));
      continue;
    }
    Spec s = s0;
    int width = s.width, prec = s.prec;
    if (s.width_arg > 0) {
      width = static_cast<std::int32_t>(argv(s.width_arg));
      if (width < 0) {
        s.left = true;
        width = -width;
      }
    }
    if (s.prec_arg > 0) {
      prec = static_cast<std::int32_t>(argv(s.prec_arg));
      if (prec < 0) prec = -1;
    }
    const std::uint64_t raw = argv(s.arg);
    std::uint32_t conv = s.conv;
    if (conv == 'D' || conv == 'O' || conv == 'U') {  // BSD: %D == %ld
      conv = conv == 'D' ? 'd' : conv == 'O' ? 'o' : 'u';
    }
    if (conv == 'C') {
      conv = 'c';
      s.len = kL;
    }
    if (conv == 'S') {
      conv = 's';
      s.len = kL;
    }
    switch (conv) {
      case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
        const bool is64 = KindOf(s) == AK::I64 && conv != 'p';
        bool neg = false;
        std::uint64_t u = 0;
        unsigned base = 10;
        if (conv == 'd' || conv == 'i') {
          std::int64_t v = is64 ? static_cast<std::int64_t>(raw) : static_cast<std::int32_t>(raw);
          if (s.len == kHH) v = static_cast<std::int8_t>(v);
          else if (s.len == kH) v = static_cast<std::int16_t>(v);
          neg = v < 0;
          u = neg ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
        } else {
          u = is64 ? raw : static_cast<std::uint32_t>(raw);
          if (s.len == kHH && conv != 'p') u = static_cast<std::uint8_t>(u);
          else if (s.len == kH && conv != 'p') u = static_cast<std::uint16_t>(u);
          base = conv == 'o' ? 8 : conv == 'u' ? 10 : 16;
        }
        std::string digits = ToBase(u, base, conv == 'X');
        if (prec >= 0) {
          if (prec == 0 && u == 0) digits.clear();
          while (static_cast<int>(digits.size()) < prec) digits.insert(digits.begin(), '0');
        }
        if (conv == 'o' && s.alt && (digits.empty() || digits[0] != '0')) digits.insert(digits.begin(), '0');
        std::string prefix;
        if (conv == 'd' || conv == 'i') prefix = neg ? "-" : s.plus ? "+" : s.space ? " " : "";
        else if (conv == 'p') prefix = "0x";
        else if ((conv == 'x' || conv == 'X') && s.alt && u) prefix = conv == 'x' ? "0x" : "0X";
        const int total = static_cast<int>(prefix.size() + digits.size());
        const int padn = width > total ? width - total : 0;
        if (s.left) {
          AppendNarrow(out, prefix + digits);
          Pad(out, padn, ' ');
        } else if (s.zero && prec < 0) {
          AppendNarrow(out, prefix);
          Pad(out, padn, '0');
          AppendNarrow(out, digits);
        } else {
          Pad(out, padn, ' ');
          AppendNarrow(out, prefix + digits);
        }
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double d;
        std::memcpy(&d, &raw, 8);
        const bool upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
        if (std::isinf(d) || std::isnan(d)) {
          std::string body = std::isnan(d) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
          if (std::isinf(d)) body = (d < 0 ? "-" : s.plus ? "+" : s.space ? " " : "") + body;
          std::basic_string<O> b;
          AppendNarrow(b, body);
          Field(out, s, width, b);
          break;
        }
        std::string spec = "%";
        if (s.left) spec += '-';
        if (s.plus) spec += '+';
        if (s.space) spec += ' ';
        if (s.alt) spec += '#';
        if (s.zero) spec += '0';
        if (width > 0) spec += std::to_string(width);
        if (prec >= 0) spec += "." + std::to_string(prec);
        spec += static_cast<char>(conv);
        std::vector<char> buf(400 + static_cast<std::size_t>(prec > 0 ? prec : 0) + static_cast<std::size_t>(width > 0 ? width : 0));
        const int n = std::snprintf(buf.data(), buf.size(), spec.c_str(), d);
        if (n > 0) AppendNarrow(out, std::string(buf.data(), static_cast<std::size_t>(n) < buf.size() ? n : buf.size() - 1));
        break;
      }
      case 'c': {
        std::basic_string<O> b;
        if constexpr (sizeof(O) == 1) {
          const std::uint32_t c = static_cast<std::uint32_t>(raw);
          b += static_cast<O>(s.len == kL ? (c < 256 ? c : '?') : (c & 0xFF));
        } else {
          b += static_cast<O>(s.len == kL ? static_cast<std::uint32_t>(raw) : (raw & 0xFF));
        }
        Field(out, s, width, b);
        break;
      }
      case 's': {
        std::basic_string<O> b;
        const gaddr a = static_cast<gaddr>(raw);
        if (s.len == kL) {
          const auto w = GuestWStr(a, prec);
          for (char32_t c : w) {
            if constexpr (sizeof(O) == 1) b += static_cast<O>(c < 256 ? c : '?');
            else b += static_cast<O>(c);
          }
        } else {
          AppendNarrow(b, GuestStr(a, prec));
        }
        Field(out, s, width, b);
        break;
      }
      case 'n': {
        const gaddr a = static_cast<gaddr>(raw);
        if (a) {
          const std::uint64_t n = out.size();
          if (s.len == kHH) mem::W8(a, static_cast<std::uint8_t>(n));
          else if (s.len == kH) mem::W16(a, static_cast<std::uint16_t>(n));
          else if (s.len == kLL || s.len == kJ) mem::W64(a, n);
          else mem::W32(a, static_cast<std::uint32_t>(n));
        }
        break;
      }
      default:  // conversion desconocida: BSD la imprime tal cual
        out += static_cast<O>(conv);
        break;
    }
  }
  return out;
}

// ------------------------------------------------------------------------------------------
// scanf
bool IsSpace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int DigitVal(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

struct Counted {
  ScanInput& in;
  int count = 0;
  int Get() {
    const int c = in.Get();
    if (c >= 0) ++count;
    return c;
  }
  void Unget(int c) {
    if (c >= 0) {
      in.Unget(c);
      --count;
    }
  }
};

void StoreInt(gaddr a, Len len, std::uint64_t v) {
  if (!a) return;
  switch (len) {
    case kHH: mem::W8(a, static_cast<std::uint8_t>(v)); break;
    case kH: mem::W16(a, static_cast<std::uint16_t>(v)); break;
    case kLL: case kJ: case kBigL: mem::W64(a, v); break;
    default: mem::W32(a, static_cast<std::uint32_t>(v)); break;
  }
}

}  // namespace

std::string FormatPrintf(const char* fmt, VaList& ap) {
  if (!fmt) return {};
  return Render<char>(fmt, ap);
}
std::u32string FormatPrintfW(const std::uint32_t* fmt, VaList& ap) {
  if (!fmt) return {};
  return Render<char32_t>(fmt, ap);
}

int ScanfCore(ScanInput& raw_in, const char* fmt, VaList& ap) {
  Counted in{raw_in};
  int assigned = 0;
  bool any_conv = false;
  const auto* f = reinterpret_cast<const unsigned char*>(fmt);
  auto input_fail = [&]() { return (assigned == 0 && !any_conv) ? -1 : assigned; };
  while (*f) {
    if (IsSpace(*f)) {
      while (IsSpace(*f)) ++f;
      int c;
      while ((c = in.Get()) >= 0 && IsSpace(c)) {}
      in.Unget(c);
      continue;
    }
    if (*f != '%') {
      const int c = in.Get();
      if (c < 0) return input_fail();
      if (c != *f) {
        in.Unget(c);
        return assigned;
      }
      ++f;
      continue;
    }
    ++f;
    if (*f == '%') {
      int c;
      while ((c = in.Get()) >= 0 && IsSpace(c)) {}
      if (c < 0) return input_fail();
      if (c != '%') {
        in.Unget(c);
        return assigned;
      }
      ++f;
      continue;
    }
    bool suppress = false;
    if (*f == '*') {
      suppress = true;
      ++f;
    }
    int width = 0;
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    Len len = kNone;
    for (bool more = true; more;) {
      switch (*f) {
        case 'h': len = len == kH ? kHH : kH; ++f; break;
        case 'l': len = len == kL ? kLL : kL; ++f; break;
        case 'q': len = kLL; ++f; break;
        case 'L': len = kBigL; ++f; break;
        case 'j': len = kJ; ++f; break;
        case 'z': case 't': len = kZ; ++f; break;
        default: more = false;
      }
    }
    const int conv = *f;
    if (!conv) break;
    ++f;
    if (conv == 'n') {
      if (!suppress) StoreInt(ap.Next<gaddr>(), len, static_cast<std::uint64_t>(in.count));
      continue;
    }
    if (conv != '[' && conv != 'c') {
      int c;
      while ((c = in.Get()) >= 0 && IsSpace(c)) {}
      if (c < 0) return input_fail();
      in.Unget(c);
    }
    const int maxw = width > 0 ? width : 0x7FFFFFFF;
    switch (conv) {
      case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
        int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
        std::string tok;
        int c = in.Get();
        if ((c == '+' || c == '-') && static_cast<int>(tok.size()) < maxw) {
          tok += static_cast<char>(c);
          c = in.Get();
        }
        if ((base == 0 || base == 16) && c == '0' && static_cast<int>(tok.size()) < maxw) {
          tok += '0';
          c = in.Get();
          if ((c == 'x' || c == 'X') && static_cast<int>(tok.size()) < maxw) {
            const int nx = in.Get();
            if (DigitVal(nx) < 16) {
              tok += static_cast<char>(c);
              base = 16;
              c = nx;
            } else {
              in.Unget(nx);
              // "0x" sin digitos: el valor es 0 y la 'x' queda sin consumir
              in.Unget(c);
              c = -2;
            }
          } else if (base == 0) {
            base = 8;
          }
        }
        if (base == 0) base = 10;
        if (c != -2) {
          while (c >= 0 && DigitVal(c) < base && static_cast<int>(tok.size()) < maxw) {
            tok += static_cast<char>(c);
            c = in.Get();
          }
          in.Unget(c);
        }
        const bool has_digit = tok.find_first_of("0123456789abcdefABCDEF") != std::string::npos;
        if (!has_digit) return c == -1 ? input_fail() : assigned;
        any_conv = true;
        const bool neg = !tok.empty() && tok[0] == '-';
        const char* digits = tok.c_str() + ((tok[0] == '+' || tok[0] == '-') ? 1 : 0);
        std::uint64_t v = std::strtoull(digits, nullptr, base);
        if (neg) v = 0 - v;
        if (!suppress) {
          StoreInt(ap.Next<gaddr>(), conv == 'p' ? kNone : len, v);
          ++assigned;
        }
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        std::string tok;
        auto room = [&] { return static_cast<int>(tok.size()) < maxw; };
        int c = in.Get();
        if ((c == '+' || c == '-') && room()) {
          tok += static_cast<char>(c);
          c = in.Get();
        }
        if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
          const char* word = (c == 'i' || c == 'I') ? "infinity" : "nan";
          std::size_t k = 0;
          while (c >= 0 && word[k] && (c | 0x20) == word[k] && room()) {
            tok += static_cast<char>(c);
            ++k;
            c = in.Get();
          }
          in.Unget(c);
        } else {
          bool hex = false;
          if (c == '0' && room()) {
            tok += '0';
            c = in.Get();
            if ((c == 'x' || c == 'X') && room()) {
              hex = true;
              tok += static_cast<char>(c);
              c = in.Get();
            }
          }
          auto isd = [&](int ch) { return hex ? DigitVal(ch) < 16 : (ch >= '0' && ch <= '9'); };
          while (c >= 0 && isd(c) && room()) {
            tok += static_cast<char>(c);
            c = in.Get();
          }
          if (c == '.' && room()) {
            tok += '.';
            c = in.Get();
            while (c >= 0 && isd(c) && room()) {
              tok += static_cast<char>(c);
              c = in.Get();
            }
          }
          const int ech = hex ? 'p' : 'e';
          if ((c | 0x20) == ech && room() && tok.find_first_of("0123456789abcdefABCDEF") != std::string::npos) {
            std::string save = tok;
            std::vector<int> pend{c};
            tok += static_cast<char>(c);
            int c2 = in.Get();
            if ((c2 == '+' || c2 == '-') && room()) {
              pend.push_back(c2);
              tok += static_cast<char>(c2);
              c2 = in.Get();
            }
            if (c2 >= '0' && c2 <= '9') {
              while (c2 >= '0' && c2 <= '9' && room()) {
                tok += static_cast<char>(c2);
                c2 = in.Get();
              }
              c = c2;
            } else {
              // exponente incompleto ("1e", "1e+"): se devuelve lo consumido y vale la mantisa
              in.Unget(c2);
              for (auto it = pend.rbegin(); it != pend.rend(); ++it) in.Unget(*it);
              tok = save;
              c = -3;
            }
          }
          if (c != -3) in.Unget(c);
        }
        char* end = nullptr;
        const double d = std::strtod(tok.c_str(), &end);
        if (tok.empty() || end == tok.c_str()) return assigned;
        any_conv = true;
        if (!suppress) {
          const gaddr a = ap.Next<gaddr>();
          if (len == kL || len == kBigL || len == kLL) mem::Write<double>(a, d);
          else mem::Write<float>(a, static_cast<float>(d));
          ++assigned;
        }
        break;
      }
      case 's': {
        const gaddr a = suppress ? 0 : ap.Next<gaddr>();
        int n = 0;
        while (n < maxw) {
          const int c = in.Get();
          if (c < 0) break;
          if (IsSpace(c)) {
            in.Unget(c);
            break;
          }
          if (a) {
            if (len == kL) mem::W32(a + 4 * n, static_cast<std::uint32_t>(c));
            else mem::W8(a + n, static_cast<std::uint8_t>(c));
          }
          ++n;
        }
        if (!n) return input_fail();
        if (a) {
          if (len == kL) mem::W32(a + 4 * n, 0);
          else mem::W8(a + n, 0);
          ++assigned;
        }
        any_conv = true;
        break;
      }
      case 'c': {
        const int w = width > 0 ? width : 1;
        const gaddr a = suppress ? 0 : ap.Next<gaddr>();
        int n = 0;
        for (; n < w; ++n) {
          const int c = in.Get();
          if (c < 0) break;
          if (a) {
            if (len == kL) mem::W32(a + 4 * n, static_cast<std::uint32_t>(c));
            else mem::W8(a + n, static_cast<std::uint8_t>(c));
          }
        }
        if (n < w) return n == 0 ? input_fail() : assigned;
        if (a) ++assigned;
        any_conv = true;
        break;
      }
      case '[': {
        bool set[256] = {};
        bool negate = false;
        if (*f == '^') {
          negate = true;
          ++f;
        }
        if (*f == ']') {
          set[']'] = true;
          ++f;
        }
        while (*f && *f != ']') {
          if (f[1] == '-' && f[2] && f[2] != ']') {
            for (int ch = f[0]; ch <= f[2]; ++ch) set[ch] = true;
            f += 3;
          } else {
            set[*f++] = true;
          }
        }
        if (*f == ']') ++f;
        const gaddr a = suppress ? 0 : ap.Next<gaddr>();
        int n = 0, c = 0;
        while (n < maxw) {
          c = in.Get();
          if (c < 0) break;
          if (set[c & 0xFF] == negate) {
            in.Unget(c);
            break;
          }
          if (a) {
            if (len == kL) mem::W32(a + 4 * n, static_cast<std::uint32_t>(c));
            else mem::W8(a + n, static_cast<std::uint8_t>(c));
          }
          ++n;
        }
        if (!n) return c < 0 ? input_fail() : assigned;
        if (a) {
          if (len == kL) mem::W32(a + 4 * n, 0);
          else mem::W8(a + n, 0);
          ++assigned;
        }
        any_conv = true;
        break;
      }
      default:
        return assigned;
    }
  }
  return assigned;
}

}  // namespace zw::hle::libc
