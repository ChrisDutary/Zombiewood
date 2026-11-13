// ctype de bionic (locale "C"): tablas _ctype_/_tolower_tab_/_toupper_tab_ como datos guest
// y funciones is*/to*, setlocale.
#include <cstdint>
#include <cstring>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

// Bits de bionic (<ctype.h> de OpenBSD).
constexpr std::uint8_t U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80;

std::uint8_t Class(int c) {
  if (c < 0 || c > 127) return 0;
  std::uint8_t f = 0;
  if (c >= 'A' && c <= 'Z') f |= U;
  if (c >= 'a' && c <= 'z') f |= L;
  if (c >= '0' && c <= '9') f |= N;
  if (c == ' ' || (c >= '\t' && c <= '\r')) f |= S;
  if (c < 32 || c == 127) f |= C;
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= X;
  if (c == ' ') f |= B;
  if (c > 32 && c < 127 && !(f & (U | L | N))) f |= P;
  return f;
}

int hle_isalnum(int c) { return (Class(c) & (U | L | N)) != 0; }
int hle_isalpha(int c) { return (Class(c) & (U | L)) != 0; }
int hle_iscntrl(int c) { return (Class(c) & C) != 0; }
int hle_isdigit(int c) { return (Class(c) & N) != 0; }
int hle_isgraph(int c) { return (Class(c) & (P | U | L | N)) != 0; }
int hle_islower(int c) { return (Class(c) & L) != 0; }
int hle_isprint(int c) { return (Class(c) & (P | U | L | N | B)) != 0; }
int hle_ispunct(int c) { return (Class(c) & P) != 0; }
int hle_isspace(int c) { return (Class(c) & S) != 0; }
int hle_isupper(int c) { return (Class(c) & U) != 0; }
int hle_isxdigit(int c) { return (Class(c) & X) != 0; }
int hle_isblank(int c) { return c == ' ' || c == '\t'; }
int hle_isascii(int c) { return (c & ~0x7F) == 0; }
int hle_toascii(int c) { return c & 0x7F; }
int hle_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int hle_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

gaddr hle_setlocale(int, const char* loc) {
  // Solo existe "C" (bionic 4.x). Consultar o pedir "C"/"POSIX"/"" -> "C"; otro -> NULL.
  if (!loc || !*loc || !std::strcmp(loc, "C") || !std::strcmp(loc, "POSIX")) return InternString("C");
  return 0;
}

}  // namespace

void RegisterCtype() {
  // const char* _ctype_ -> tabla[1 + 256]; (_ctype_ + 1)[c]. Los tres simbolos son PUNTEROS.
  const gaddr ct = GuestMemory::Calloc(1, 1 + 256 + 3);
  const gaddr lo = GuestMemory::Calloc(1 + 256, 2);
  const gaddr up = GuestMemory::Calloc(1 + 256, 2);
  for (int c = 0; c < 256; ++c) {
    mem::W8(ct + 1 + c, Class(c));
    mem::W16(lo + 2 * (1 + c), static_cast<std::uint16_t>(hle_tolower(c)));
    mem::W16(up + 2 * (1 + c), static_cast<std::uint16_t>(hle_toupper(c)));
  }
  mem::W16(lo, 0xFFFF);  // indice -1 (EOF)
  mem::W16(up, 0xFFFF);
  mem::W32(DefineData("_ctype_", 4), ct);
  mem::W32(DefineData("_tolower_tab_", 4), lo);
  mem::W32(DefineData("_toupper_tab_", 4), up);

  ZW_HLE_FAST("isalnum", hle_isalnum);
  ZW_HLE_FAST("isalpha", hle_isalpha);
  ZW_HLE_FAST("iscntrl", hle_iscntrl);
  ZW_HLE_FAST("isdigit", hle_isdigit);
  ZW_HLE_FAST("isgraph", hle_isgraph);
  ZW_HLE_FAST("islower", hle_islower);
  ZW_HLE_FAST("isprint", hle_isprint);
  ZW_HLE_FAST("ispunct", hle_ispunct);
  ZW_HLE_FAST("isspace", hle_isspace);
  ZW_HLE_FAST("isupper", hle_isupper);
  ZW_HLE_FAST("isxdigit", hle_isxdigit);
  ZW_HLE_FAST("isblank", hle_isblank);
  ZW_HLE_FAST("isascii", hle_isascii);
  ZW_HLE_FAST("toascii", hle_toascii);
  ZW_HLE_FAST("tolower", hle_tolower);
  ZW_HLE_FAST("toupper", hle_toupper);
  ZW_HLE_FAST("_tolower", hle_tolower);
  ZW_HLE_FAST("_toupper", hle_toupper);
  ZW_HLE("setlocale", hle_setlocale);
}

}  // namespace zw::hle::libc
