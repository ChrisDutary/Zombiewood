// Decodificadores software ETC1 / PVRTC (v1) / ATC -> RGBA8 (T5).
//
// - ETC1: especificacion OES_compressed_ETC1_RGB8_texture (exacto).
// - PVRTC v1 4bpp: escalado bilineal de las imagenes A/B a partir de los centros de bloque (con
//   envoltura) y modulacion {0,3,5,8}/8 (o punch-through {0,4,4+alfa 0,8}/8). Precision: se interpola
//   en 8 bits en vez del punto fijo del SDK de PowerVR, diferencias de +-1 LSB.
// - PVRTC v1 2bpp: modo directo (1 bit/pixel) exacto; modo interpolado aproximado (se rellenan los
//   pixeles no almacenados con la media de sus vecinos). Los datos del juego no traen 2bpp (T1).
// - ATC (AMD): paleta {c0, 5/8, 3/8, c1} o {0, c0-c1/4, c0, c1}; alfa explicito (4 bits) o interpolado
//   (como DXT5). Los datos del juego no traen ATC (T1); no verificado contra hardware.
#include "hle/gles/texdecode.h"

#include <algorithm>
#include <cstring>

namespace zw::hle::gles::texdecode {
namespace {

inline std::uint8_t Clamp8(int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline int Expand5(int v) { return (v << 3) | (v >> 2); }
inline int Expand6(int v) { return (v << 2) | (v >> 4); }
inline int Expand4(int v) { return (v << 4) | v; }

// ---------------------------------------------------------------- ETC1
constexpr int kEtcModifiers[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},   {9, 29, -9, -29},     {13, 42, -13, -42},
    {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183},
};

// ---------------------------------------------------------------- PVRTC
struct Rgba { int r, g, b, a; };

// Color A: bits 0..15 de la palabra de color (bit 0 = modo de modulacion).
Rgba PvrColorA(std::uint32_t c) {
  if (c & 0x8000) {  // opaco: RGB 554
    int r = (c >> 10) & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 1) & 0x0F;
    b = (b << 1) | (b >> 3);
    return {Expand5(r), Expand5(g), Expand5(b), 255};
  }
  // translucido: ARGB 3443
  int a = (c >> 12) & 0x7, r = (c >> 8) & 0xF, g = (c >> 4) & 0xF, b = (c >> 1) & 0x7;
  b = (b << 1) | (b >> 2);
  return {Expand4(r), Expand4(g), Expand4(b), Expand4(a << 1)};
}
// Color B: bits 16..31.
Rgba PvrColorB(std::uint32_t c) {
  if (c & 0x80000000u) {  // opaco: RGB 555
    int r = (c >> 26) & 0x1F, g = (c >> 21) & 0x1F, b = (c >> 16) & 0x1F;
    return {Expand5(r), Expand5(g), Expand5(b), 255};
  }
  int a = (c >> 28) & 0x7, r = (c >> 24) & 0xF, g = (c >> 20) & 0xF, b = (c >> 16) & 0xF;
  return {Expand4(r), Expand4(g), Expand4(b), Expand4(a << 1)};
}

// Indice de bloque "twiddled" (Morton) del SDK de PowerVR. xs/ys en bloques (potencias de 2).
std::uint32_t PvrTwiddle(std::uint32_t xs, std::uint32_t ys, std::uint32_t x, std::uint32_t y) {
  const std::uint32_t min_dim = std::min(xs, ys);
  std::uint32_t max_val = (ys < xs) ? x : y;
  std::uint32_t tw = 0, src = 1, dst = 1, shift = 0;
  while (src < min_dim) {
    if (y & src) tw |= dst;
    if (x & src) tw |= dst << 1;
    src <<= 1;
    dst <<= 2;
    ++shift;
  }
  max_val >>= shift;
  tw |= max_val << (2 * shift);
  return tw;
}

std::uint32_t NextPow2(std::uint32_t v) {
  std::uint32_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

bool DecodePVRTC(bool two_bpp, const std::uint8_t* src, std::uint32_t size, int w, int h,
                 std::vector<std::uint8_t>& out) {
  const int bw = two_bpp ? 8 : 4, bh = 4;
  // Dimensiones en bloques (minimo 2x2; PVRTC v1 exige potencias de 2).
  const std::uint32_t bx_n = std::max<std::uint32_t>(2, NextPow2((w + bw - 1) / bw));
  const std::uint32_t by_n = std::max<std::uint32_t>(2, NextPow2((h + bh - 1) / bh));
  if (static_cast<std::uint64_t>(bx_n) * by_n * 8 > size) return false;
  auto word = [&](std::uint32_t bx, std::uint32_t by, int which) {
    const std::uint32_t idx = PvrTwiddle(bx_n, by_n, bx % bx_n, by % by_n);
    std::uint32_t v;
    std::memcpy(&v, src + idx * 8 + which * 4, 4);
    return v;
  };
  const int pw = static_cast<int>(bx_n) * bw, ph = static_cast<int>(by_n) * bh;

  // Modulacion por pixel del bloque que lo contiene: devuelve peso 0..8 y si es punch-through (alfa 0).
  auto modulation = [&](int x, int y, int& weight, bool& punch) {
    const std::uint32_t bx = x / bw, by = y / bh;
    const std::uint32_t mod = word(bx, by, 0), col = word(bx, by, 1);
    const int lx = x % bw, ly = y % bh;
    punch = false;
    if (!two_bpp) {
      const int v = (mod >> ((ly * 4 + lx) * 2)) & 3;
      if (col & 1) {  // punch-through
        static constexpr int kW[4] = {0, 4, 4, 8};
        weight = kW[v];
        punch = (v == 2);
      } else {
        static constexpr int kW[4] = {0, 3, 5, 8};
        weight = kW[v];
      }
      return;
    }
    if (!(col & 1)) {  // 2bpp directo: 1 bit por pixel
      weight = ((mod >> (ly * 8 + lx)) & 1) ? 8 : 0;
      return;
    }
    // 2bpp interpolado (aproximado): valores de 2 bits en tablero de ajedrez.
    auto stored = [&](int sx, int sy) {
      const std::uint32_t sbx = ((sx % pw + pw) % pw) / bw, sby = ((sy % ph + ph) % ph) / bh;
      const std::uint32_t m = word(sbx, sby, 0);
      const int llx = ((sx % pw + pw) % pw) % bw, lly = ((sy % ph + ph) % ph) % bh;
      const int i = (lly * bw + llx) >> 1;
      static constexpr int kW[4] = {0, 3, 5, 8};
      return kW[(m >> (i * 2)) & 3];
    };
    if (((x + y) & 1) == 0) {
      weight = stored(x, y);
    } else {
      weight = (stored(x - 1, y) + stored(x + 1, y) + stored(x, y - 1) + stored(x, y + 1) + 2) / 4;
    }
  };

  out.assign(static_cast<std::size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Posicion respecto a los centros de bloque (bw/2, bh/2), con envoltura.
      const int fx = x - bw / 2, fy = y - bh / 2;
      const int bx0 = (fx >= 0 ? fx / bw : -((-fx + bw - 1) / bw));
      const int by0 = (fy >= 0 ? fy / bh : -((-fy + bh - 1) / bh));
      const int wx = fx - bx0 * bw, wy = fy - by0 * bh;  // 0..bw-1 / 0..bh-1
      const std::uint32_t x0 = static_cast<std::uint32_t>((bx0 % static_cast<int>(bx_n) + bx_n) % bx_n);
      const std::uint32_t y0 = static_cast<std::uint32_t>((by0 % static_cast<int>(by_n) + by_n) % by_n);
      const std::uint32_t x1 = (x0 + 1) % bx_n, y1 = (y0 + 1) % by_n;
      const std::uint32_t c00 = word(x0, y0, 1), c10 = word(x1, y0, 1), c01 = word(x0, y1, 1), c11 = word(x1, y1, 1);
      const int w00 = (bw - wx) * (bh - wy), w10 = wx * (bh - wy), w01 = (bw - wx) * wy, w11 = wx * wy;
      const int den = bw * bh;
      auto bil = [&](Rgba (*f)(std::uint32_t)) {
        const Rgba a = f(c00), b = f(c10), c = f(c01), d = f(c11);
        return Rgba{(a.r * w00 + b.r * w10 + c.r * w01 + d.r * w11 + den / 2) / den,
                    (a.g * w00 + b.g * w10 + c.g * w01 + d.g * w11 + den / 2) / den,
                    (a.b * w00 + b.b * w10 + c.b * w01 + d.b * w11 + den / 2) / den,
                    (a.a * w00 + b.a * w10 + c.a * w01 + d.a * w11 + den / 2) / den};
      };
      const Rgba A = bil(PvrColorA), B = bil(PvrColorB);
      int m;
      bool punch;
      modulation(x, y, m, punch);
      std::uint8_t* o = &out[(static_cast<std::size_t>(y) * w + x) * 4];
      o[0] = Clamp8((A.r * (8 - m) + B.r * m) / 8);
      o[1] = Clamp8((A.g * (8 - m) + B.g * m) / 8);
      o[2] = Clamp8((A.b * (8 - m) + B.b * m) / 8);
      o[3] = punch ? 0 : Clamp8((A.a * (8 - m) + B.a * m) / 8);
    }
  }
  return true;
}

// ---------------------------------------------------------------- ATC
void DecodeAtcColor(const std::uint8_t* blk, std::uint8_t px[16][4]) {
  const std::uint32_t c0 = blk[0] | (blk[1] << 8), c1 = blk[2] | (blk[3] << 8);
  std::uint32_t idx;
  std::memcpy(&idx, blk + 4, 4);
  const int r0 = Expand5((c0 >> 10) & 31), g0 = Expand5((c0 >> 5) & 31), b0 = Expand5(c0 & 31);
  const int r1 = Expand5((c1 >> 11) & 31), g1 = Expand6((c1 >> 5) & 63), b1 = Expand5(c1 & 31);
  int pal[4][3];
  if (c0 & 0x8000) {
    pal[0][0] = pal[0][1] = pal[0][2] = 0;
    pal[1][0] = std::max(0, r0 - r1 / 4);
    pal[1][1] = std::max(0, g0 - g1 / 4);
    pal[1][2] = std::max(0, b0 - b1 / 4);
    pal[2][0] = r0; pal[2][1] = g0; pal[2][2] = b0;
  } else {
    pal[0][0] = r0; pal[0][1] = g0; pal[0][2] = b0;
    pal[1][0] = (5 * r0 + 3 * r1) / 8; pal[1][1] = (5 * g0 + 3 * g1) / 8; pal[1][2] = (5 * b0 + 3 * b1) / 8;
    pal[2][0] = (3 * r0 + 5 * r1) / 8; pal[2][1] = (3 * g0 + 5 * g1) / 8; pal[2][2] = (3 * b0 + 5 * b1) / 8;
  }
  pal[3][0] = r1; pal[3][1] = g1; pal[3][2] = b1;
  for (int i = 0; i < 16; ++i) {
    const int s = (idx >> (2 * i)) & 3;
    px[i][0] = Clamp8(pal[s][0]);
    px[i][1] = Clamp8(pal[s][1]);
    px[i][2] = Clamp8(pal[s][2]);
    px[i][3] = 255;
  }
}

void DecodeInterpAlpha(const std::uint8_t* blk, std::uint8_t px[16][4]) {
  const int a0 = blk[0], a1 = blk[1];
  int pal[8];
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
  } else {
    for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
  std::uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(blk[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) px[i][3] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7]);
}

bool DecodeATC(std::uint32_t fmt, const std::uint8_t* src, std::uint32_t size, int w, int h,
               std::vector<std::uint8_t>& out) {
  const int bs = fmt == kATC_RGB ? 8 : 16;
  const int bxn = (w + 3) / 4, byn = (h + 3) / 4;
  if (static_cast<std::uint64_t>(bxn) * byn * bs > size) return false;
  out.assign(static_cast<std::size_t>(w) * h * 4, 0);
  for (int by = 0; by < byn; ++by) {
    for (int bx = 0; bx < bxn; ++bx) {
      const std::uint8_t* blk = src + (static_cast<std::size_t>(by) * bxn + bx) * bs;
      std::uint8_t px[16][4];
      DecodeAtcColor(blk + (bs - 8), px);
      if (fmt == kATC_RGBA_EXPLICIT) {
        for (int i = 0; i < 16; ++i) px[i][3] = static_cast<std::uint8_t>(Expand4((blk[i / 2] >> ((i & 1) * 4)) & 0xF));
      } else if (fmt == kATC_RGBA_INTERPOLATED) {
        DecodeInterpAlpha(blk, px);
      }
      for (int i = 0; i < 16; ++i) {
        const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < w && y < h) std::memcpy(&out[(static_cast<std::size_t>(y) * w + x) * 4], px[i], 4);
      }
    }
  }
  return true;
}

}  // namespace

void DecodeETC1Block(const std::uint8_t b[8], std::uint8_t out[64]) {
  const std::uint32_t hi = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
  const std::uint32_t lo = (b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
  const bool diff = (hi >> 1) & 1, flip = hi & 1;
  const int t1 = (hi >> 5) & 7, t2 = (hi >> 2) & 7;
  int c1[3], c2[3];
  if (diff) {
    for (int i = 0; i < 3; ++i) {
      const int base = (hi >> (27 - 8 * i)) & 0x1F;
      int d = (hi >> (24 - 8 * i)) & 0x7;
      if (d >= 4) d -= 8;
      c1[i] = Expand5(base);
      c2[i] = Expand5((base + d) & 0x1F);
    }
  } else {
    for (int i = 0; i < 3; ++i) {
      c1[i] = Expand4((hi >> (28 - 8 * i)) & 0xF);
      c2[i] = Expand4((hi >> (24 - 8 * i)) & 0xF);
    }
  }
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      const int i = x * 4 + y;  // indices en orden de columnas
      const int msb = (lo >> (16 + i)) & 1, lsb = (lo >> i) & 1;
      const bool second = flip ? (y >= 2) : (x >= 2);
      const int* c = second ? c2 : c1;
      const int m = kEtcModifiers[second ? t2 : t1][(msb << 1) | lsb];
      std::uint8_t* o = &out[(y * 4 + x) * 4];
      o[0] = Clamp8(c[0] + m);
      o[1] = Clamp8(c[1] + m);
      o[2] = Clamp8(c[2] + m);
      o[3] = 255;
    }
  }
}

bool IsPVRTC(std::uint32_t f) { return f >= kPVRTC_RGB_4BPP && f <= kPVRTC_RGBA_2BPP; }
bool IsATC(std::uint32_t f) { return f == kATC_RGB || f == kATC_RGBA_EXPLICIT || f == kATC_RGBA_INTERPOLATED; }
bool CanDecode(std::uint32_t f) { return f == kETC1_RGB8 || IsPVRTC(f) || IsATC(f); }

std::uint32_t CompressedSize(std::uint32_t f, int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  const std::uint32_t uw = static_cast<std::uint32_t>(w), uh = static_cast<std::uint32_t>(h);
  if (f == kETC1_RGB8 || f == kATC_RGB) return ((uw + 3) / 4) * ((uh + 3) / 4) * 8;
  if (f == kATC_RGBA_EXPLICIT || f == kATC_RGBA_INTERPOLATED) return ((uw + 3) / 4) * ((uh + 3) / 4) * 16;
  if (f == kPVRTC_RGB_4BPP || f == kPVRTC_RGBA_4BPP) return (std::max(uw, 8u) * std::max(uh, 8u) * 4 + 7) / 8;
  if (f == kPVRTC_RGB_2BPP || f == kPVRTC_RGBA_2BPP) return (std::max(uw, 16u) * std::max(uh, 8u) * 2 + 7) / 8;
  return 0;
}

bool Decode(std::uint32_t f, const std::uint8_t* src, std::uint32_t size, int w, int h, std::vector<std::uint8_t>& out) {
  if (!src || w <= 0 || h <= 0) return false;
  if (f == kETC1_RGB8) {
    const int bxn = (w + 3) / 4, byn = (h + 3) / 4;
    if (static_cast<std::uint64_t>(bxn) * byn * 8 > size) return false;
    out.assign(static_cast<std::size_t>(w) * h * 4, 0);
    std::uint8_t px[64];
    for (int by = 0; by < byn; ++by) {
      for (int bx = 0; bx < bxn; ++bx) {
        DecodeETC1Block(src + (static_cast<std::size_t>(by) * bxn + bx) * 8, px);
        for (int y = 0; y < 4; ++y) {
          for (int x = 0; x < 4; ++x) {
            const int X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h) std::memcpy(&out[(static_cast<std::size_t>(Y) * w + X) * 4], &px[(y * 4 + x) * 4], 4);
          }
        }
      }
    }
    return true;
  }
  if (IsPVRTC(f)) return DecodePVRTC(f == kPVRTC_RGB_2BPP || f == kPVRTC_RGBA_2BPP, src, size, w, h, out);
  if (IsATC(f)) return DecodeATC(f, src, size, w, h, out);
  return false;
}

}  // namespace zw::hle::gles::texdecode
