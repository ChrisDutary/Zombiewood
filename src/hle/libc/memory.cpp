// malloc & co. sobre el heap guest (GuestMemory) y familia mem*.
#include <cstring>

#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

gaddr hle_malloc(gsize n) {
  const gaddr p = GuestMemory::Alloc(n);
  if (!p) SetErrno(err::NOMEM);
  return p;
}
gaddr hle_calloc(gsize n, gsize s) {
  const gaddr p = GuestMemory::Calloc(n, s);
  if (!p) SetErrno(err::NOMEM);
  return p;
}
gaddr hle_realloc(gaddr p, gsize n) {
  if (p && n == 0) {  // bionic (dlmalloc, REALLOC_ZERO_BYTES_FREES): libera y devuelve NULL
    GuestMemory::Free(p);
    return 0;
  }
  const gaddr r = GuestMemory::Realloc(p, n);
  if (!r) SetErrno(err::NOMEM);
  return r;
}
void hle_free(gaddr p) { GuestMemory::Free(p); }
gaddr hle_memalign(gsize align, gsize n) {
  if (align < 8) align = 8;
  if (align & (align - 1)) {  // dlmalloc redondea a potencia de 2
    gsize a = 8;
    while (a < align) a <<= 1;
    align = a;
  }
  return GuestMemory::AllocAligned(n, align);
}
int hle_posix_memalign(GPtr<std::uint32_t> out, gsize align, gsize n) {
  if (!align || (align & (align - 1)) || (align % 4)) return err::INVAL;
  const gaddr p = hle_memalign(align, n);
  if (!p) return err::NOMEM;
  *out = p;
  return 0;
}
gaddr hle_valloc(gsize n) { return GuestMemory::AllocAligned(n, 4096); }
gaddr hle_pvalloc(gsize n) { return GuestMemory::AllocAligned((n + 4095) & ~4095u, 4096); }
gsize hle_malloc_usable_size(gaddr p) { return GuestMemory::UsableSize(p); }

void* hle_memcpy(void* d, const void* s, gsize n) { return n ? std::memmove(d, s, n) : d; }  // tolera solape
void* hle_memmove(void* d, const void* s, gsize n) { return n ? std::memmove(d, s, n) : d; }
void* hle_memset(void* d, int c, gsize n) { return n ? std::memset(d, c, n) : d; }
int hle_memcmp(const void* a, const void* b, gsize n) {
  if (!n) return 0;
  const int r = std::memcmp(a, b, n);
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}
void* hle_memchr(const void* s, int c, gsize n) { return n ? const_cast<void*>(std::memchr(s, c, n)) : nullptr; }
void* hle_memrchr(const void* s, int c, gsize n) {
  const auto* p = static_cast<const std::uint8_t*>(s);
  for (gsize i = n; i-- > 0;)
    if (p[i] == static_cast<std::uint8_t>(c)) return const_cast<std::uint8_t*>(p + i);
  return nullptr;
}
void* hle_memccpy(void* d, const void* s, int c, gsize n) {
  auto* dp = static_cast<std::uint8_t*>(d);
  const auto* sp = static_cast<const std::uint8_t*>(s);
  for (gsize i = 0; i < n; ++i) {
    dp[i] = sp[i];
    if (sp[i] == static_cast<std::uint8_t>(c)) return dp + i + 1;
  }
  return nullptr;
}
void* hle_memmem(const void* h, gsize hn, const void* nd, gsize nn) {
  if (!nn) return const_cast<void*>(h);
  if (nn > hn) return nullptr;
  const auto* hp = static_cast<const std::uint8_t*>(h);
  for (gsize i = 0; i + nn <= hn; ++i)
    if (hp[i] == *static_cast<const std::uint8_t*>(nd) && !std::memcmp(hp + i, nd, nn)) return const_cast<std::uint8_t*>(hp + i);
  return nullptr;
}
void hle_bcopy(const void* s, void* d, gsize n) {
  if (n) std::memmove(d, s, n);
}
void hle_bzero(void* d, gsize n) {
  if (n) std::memset(d, 0, n);
}
// ARM EABI helpers (por si alguna lib los importa en vez de enlazarlos estaticamente).
void hle_aeabi_memcpy(void* d, const void* s, gsize n) { hle_memcpy(d, s, n); }
void hle_aeabi_memset(void* d, gsize n, int c) { hle_memset(d, c, n); }
void hle_aeabi_memclr(void* d, gsize n) { hle_bzero(d, n); }

}  // namespace

void RegisterMemory() {
  ZW_HLE_FAST("malloc", hle_malloc);
  ZW_HLE_FAST("calloc", hle_calloc);
  ZW_HLE_FAST("realloc", hle_realloc);
  ZW_HLE_FAST("free", hle_free);
  ZW_HLE_FAST("memalign", hle_memalign);
  ZW_HLE_FAST("posix_memalign", hle_posix_memalign);
  ZW_HLE_FAST("valloc", hle_valloc);
  ZW_HLE_FAST("pvalloc", hle_pvalloc);
  ZW_HLE_FAST("malloc_usable_size", hle_malloc_usable_size);
  ZW_HLE_FAST("dlmalloc", hle_malloc);
  ZW_HLE_FAST("dlfree", hle_free);

  ZW_HLE_FAST("memcpy", hle_memcpy);
  ZW_HLE_FAST("memmove", hle_memmove);
  ZW_HLE_FAST("memset", hle_memset);
  ZW_HLE_FAST("memcmp", hle_memcmp);
  ZW_HLE_FAST("bcmp", hle_memcmp);
  ZW_HLE_FAST("memchr", hle_memchr);
  ZW_HLE_FAST("memrchr", hle_memrchr);
  ZW_HLE_FAST("memccpy", hle_memccpy);
  ZW_HLE_FAST("memmem", hle_memmem);
  ZW_HLE_FAST("bcopy", hle_bcopy);
  ZW_HLE_FAST("bzero", hle_bzero);
  for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8", "__aeabi_memmove", "__aeabi_memmove4",
                        "__aeabi_memmove8"})
    ZW_HLE_FAST(n, hle_aeabi_memcpy);
  for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"}) ZW_HLE_FAST(n, hle_aeabi_memset);
  for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) ZW_HLE_FAST(n, hle_aeabi_memclr);
}

}  // namespace zw::hle::libc
