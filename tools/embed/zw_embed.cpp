// zw_embed: herramienta de build (Linux). Convierte el arte opcional y no versionado de resources/ en datos
// incrustados en el ejecutable, lo que en Windows hacen los .rc:
//   resources/zombiewood_cursor.bin -> zw_embedded_cursor[]      (formato "ZWCU" de tools/make_cursor.ps1, tal cual)
//   resources/zombiewood.ico        -> zw_embedded_icon_rgba[]   (una entrada PNG del .ico decodificada a RGBA8)
// y escribe un PNG de 256 px para el icono del AppImage / .desktop. Si falta un fichero, el dato queda vacio
// (cursor del sistema, sin icono de ventana) y el PNG es un icono generico generado aqui.
//
// Uso: zw_embed <cursor.bin|-> <icono.ico|-> <salida.cpp> <salida.png>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;

bool ReadAll(const std::string& path, Bytes& out) {
  if (path == "-" || path.empty()) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::uint32_t Le16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
std::uint32_t Le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint32_t Be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

const std::uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

struct Image {
  int w = 0, h = 0;
  Bytes rgba;
};

// PNG de 8 bits por canal, RGB o RGBA, sin entrelazado (lo que escribe tools/make_icon.ps1).
bool DecodePng(const std::uint8_t* p, std::size_t n, Image& img) {
  if (n < 8 || std::memcmp(p, kPngSig, 8) != 0) return false;
  Bytes idat;
  int depth = 0, color = 0, interlace = 0;
  for (std::size_t off = 8; off + 12 <= n;) {
    const std::uint32_t len = Be32(p + off);
    if (off + 12 + len > n) return false;
    const char* type = reinterpret_cast<const char*>(p + off + 4);
    const std::uint8_t* data = p + off + 8;
    if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
      img.w = static_cast<int>(Be32(data));
      img.h = static_cast<int>(Be32(data + 4));
      depth = data[8];
      color = data[9];
      interlace = data[12];
    } else if (!std::memcmp(type, "IDAT", 4)) {
      idat.insert(idat.end(), data, data + len);
    } else if (!std::memcmp(type, "IEND", 4)) {
      break;
    }
    off += 12 + len;
  }
  if (img.w <= 0 || img.h <= 0 || img.w > 1024 || img.h > 1024 || depth != 8 || interlace != 0 || (color != 6 && color != 2))
    return false;
  const int bpp = color == 6 ? 4 : 3;
  const std::size_t stride = static_cast<std::size_t>(img.w) * bpp;
  Bytes raw((stride + 1) * img.h);
  uLongf raw_len = static_cast<uLongf>(raw.size());
  if (uncompress(raw.data(), &raw_len, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || raw_len != raw.size()) return false;
  img.rgba.assign(static_cast<std::size_t>(img.w) * img.h * 4, 255);
  Bytes prev(stride, 0), cur(stride);
  for (int y = 0; y < img.h; ++y) {
    const std::uint8_t* row = raw.data() + (stride + 1) * y;
    const int filter = row[0];
    for (std::size_t x = 0; x < stride; ++x) {
      const int a = x >= static_cast<std::size_t>(bpp) ? cur[x - bpp] : 0;
      const int b = prev[x];
      const int c = x >= static_cast<std::size_t>(bpp) ? prev[x - bpp] : 0;
      int v = row[1 + x];
      switch (filter) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: {
          const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: break;
      }
      cur[x] = static_cast<std::uint8_t>(v);
    }
    for (int x = 0; x < img.w; ++x) {
      std::uint8_t* d = &img.rgba[(static_cast<std::size_t>(y) * img.w + x) * 4];
      d[0] = cur[x * bpp];
      d[1] = cur[x * bpp + 1];
      d[2] = cur[x * bpp + 2];
      if (bpp == 4) d[3] = cur[x * bpp + 3];
    }
    prev.swap(cur);
  }
  return true;
}

void PutBe32(Bytes& v, std::uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}
void Chunk(Bytes& out, const char* type, const Bytes& data) {
  PutBe32(out, static_cast<std::uint32_t>(data.size()));
  Bytes body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  PutBe32(out, static_cast<std::uint32_t>(crc32(0, body.data(), static_cast<uInt>(body.size()))));
}
Bytes EncodePng(const Image& img) {
  Bytes raw;
  for (int y = 0; y < img.h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), img.rgba.begin() + static_cast<std::size_t>(y) * img.w * 4,
               img.rgba.begin() + static_cast<std::size_t>(y + 1) * img.w * 4);
  }
  Bytes z(compressBound(static_cast<uLong>(raw.size())));
  uLongf zl = static_cast<uLongf>(z.size());
  compress2(z.data(), &zl, raw.data(), static_cast<uLong>(raw.size()), 9);
  z.resize(zl);
  Bytes out(kPngSig, kPngSig + 8), ihdr;
  PutBe32(ihdr, static_cast<std::uint32_t>(img.w));
  PutBe32(ihdr, static_cast<std::uint32_t>(img.h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  return out;
}

// Icono generico (sin arte del juego): cuadrado verde oscuro redondeado con una "Z" clara.
Image PlaceholderIcon(int n) {
  Image img;
  img.w = img.h = n;
  img.rgba.assign(static_cast<std::size_t>(n) * n * 4, 0);
  const double r = n * 0.16;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const double cx = x < r ? r : (x > n - 1 - r ? n - 1 - r : x), cy = y < r ? r : (y > n - 1 - r ? n - 1 - r : y);
      const double dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy > r * r) continue;
      const double u = (x + 0.5) / n, v = (y + 0.5) / n;
      const double t = 0.11;  // grosor del trazo
      const bool top = v > 0.22 && v < 0.22 + t && u > 0.24 && u < 0.76;
      const bool bottom = v > 0.78 - t && v < 0.78 && u > 0.24 && u < 0.76;
      const double diag = 0.76 - (v - 0.22) / 0.56 * 0.52;  // de arriba-derecha a abajo-izquierda
      const bool mid = v >= 0.22 && v <= 0.78 && u > diag - t * 0.75 && u < diag + t * 0.75 && u > 0.24 && u < 0.76;
      std::uint8_t* d = &img.rgba[(static_cast<std::size_t>(y) * n + x) * 4];
      if (top || bottom || mid) {
        d[0] = 0xC8; d[1] = 0xF0; d[2] = 0x8C;
      } else {
        d[0] = 0x1E; d[1] = 0x3A; d[2] = 0x1C;
      }
      d[3] = 255;
    }
  return img;
}

void EmitArray(std::FILE* f, const char* name, const Bytes& data) {
  std::fprintf(f, "extern const unsigned char %s[] = {", name);
  if (data.empty()) std::fprintf(f, "0");
  for (std::size_t i = 0; i < data.size(); ++i) std::fprintf(f, "%s%u,", i % 24 == 0 ? "\n" : "", data[i]);
  std::fprintf(f, "};\nextern const unsigned %s_size = %zu;\n", name, data.size());
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "uso: zw_embed <cursor.bin|-> <icono.ico|-> <salida.cpp> <salida.png>\n");
    return 2;
  }
  Bytes cursor, ico;
  const bool have_cursor = ReadAll(argv[1], cursor) && cursor.size() >= 12 && !std::memcmp(cursor.data(), "ZWCU", 4);
  if (!have_cursor) cursor.clear();

  // .ico: ICONDIR (6 bytes) + n x ICONDIRENTRY (16 bytes: w, h, ..., u32 size @8, u32 offset @12).
  Image window_icon, big_icon;
  Bytes big_png;
  if (ReadAll(argv[2], ico) && ico.size() >= 6 && Le16(ico.data() + 2) == 1) {
    const std::uint32_t n = Le16(ico.data() + 4);
    int best_small = 0, best_big = 0;
    for (std::uint32_t i = 0; i < n && 6 + 16 * (i + 1) <= ico.size(); ++i) {
      const std::uint8_t* e = ico.data() + 6 + 16 * i;
      const std::uint32_t size = Le32(e + 8), off = Le32(e + 12);
      if (off + size > ico.size()) continue;
      Image img;
      if (!DecodePng(ico.data() + off, size, img) || img.w != img.h) continue;  // las entradas BMP se ignoran
      // Icono de ventana: la entrada mas cercana a 128 px. Icono de escritorio: la mayor.
      if (!best_small || std::abs(img.w - 128) < std::abs(best_small - 128)) {
        best_small = img.w;
        window_icon = img;
      }
      if (img.w > best_big) {
        best_big = img.w;
        big_icon = img;
        big_png.assign(ico.data() + off, ico.data() + off + size);
      }
    }
  }
  const bool have_icon = window_icon.w > 0;
  if (!have_icon) big_png = EncodePng(PlaceholderIcon(256));

  std::FILE* f = std::fopen(argv[3], "wb");
  if (!f) {
    std::fprintf(stderr, "zw_embed: no se puede escribir %s\n", argv[3]);
    return 1;
  }
  std::fprintf(f, "// Generado por tools/embed/zw_embed.cpp. No editar.\n");
  EmitArray(f, "zw_embedded_cursor", cursor);
  EmitArray(f, "zw_embedded_icon_rgba", window_icon.rgba);
  std::fprintf(f, "extern const int zw_embedded_icon_size = %d;\n", window_icon.w);
  std::fclose(f);

  std::ofstream png(argv[4], std::ios::binary);
  png.write(reinterpret_cast<const char*>(big_png.data()), static_cast<std::streamsize>(big_png.size()));
  if (!png) {
    std::fprintf(stderr, "zw_embed: no se puede escribir %s\n", argv[4]);
    return 1;
  }
  std::printf("zw_embed: cursor %s (%zu bytes), icono %s (%d px ventana, PNG %zu bytes)\n", have_cursor ? "si" : "no",
              cursor.size(), have_icon ? "si" : "generico", window_icon.w, big_png.size());
  return 0;
}
