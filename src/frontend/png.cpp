// PNG minimo (RGB8, zlib con bloques "stored"): suficiente para capturas sin dependencias.
#include "frontend/png.h"

#include <cstdio>
#include <vector>

namespace zw {
namespace {
std::uint32_t g_crc_table[256];
bool g_crc_init = false;

std::uint32_t Crc(std::uint32_t c, const std::uint8_t* p, std::size_t n) {
  if (!g_crc_init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      g_crc_table[i] = v;
    }
    g_crc_init = true;
  }
  for (std::size_t i = 0; i < n; ++i) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
void Be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 24));
  v.push_back(static_cast<std::uint8_t>(x >> 16));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}
void Chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
  Be32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  Be32(out, Crc(0xFFFFFFFFu, out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}
}  // namespace

bool WritePngRgba(const std::filesystem::path& path, const std::uint8_t* rgba, int w, int h, bool flip_y) {
  // Filas con filtro 0, RGB (se descarta el alfa: el framebuffer del juego no lo usa de forma fiable).
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(h) * (1 + 3 * w));
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* row = rgba + static_cast<std::size_t>(flip_y ? h - 1 - y : y) * w * 4;
    raw.push_back(0);
    for (int x = 0; x < w; ++x) raw.insert(raw.end(), row + 4 * x, row + 4 * x + 3);
  }
  std::vector<std::uint8_t> z;
  z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
  z.push_back(0x78);
  z.push_back(0x01);
  std::uint32_t a = 1, b = 0;
  for (std::size_t off = 0; off < raw.size();) {
    const std::size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
    z.push_back(off + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n));
    z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n));
    z.push_back(static_cast<std::uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    for (std::size_t i = 0; i < n;) {  // adler32 por tramos (evita el modulo por byte)
      const std::size_t m = n - i < 5552 ? n - i : 5552;
      for (std::size_t k = 0; k < m; ++k) {
        a += raw[off + i + k];
        b += a;
      }
      a %= 65521;
      b %= 65521;
      i += m;
    }
    off += n;
  }
  Be32(z, (b << 16) | a);

  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  Be32(ihdr, static_cast<std::uint32_t>(w));
  Be32(ihdr, static_cast<std::uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});

  std::FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}
}  // namespace zw
