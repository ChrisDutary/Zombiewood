#include "installer/installer.h"

#include <windows.h>
#include <bcrypt.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "core/log.h"

namespace zw::installer {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kBuf = 1 << 20;

struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("cancelado por el usuario") {}
};

std::string U8(const fs::path& p) {
  const auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

fs::path U8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ------------------------------------------------------------------------------------- SHA-256
class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0) < 0)
      throw std::runtime_error("BCrypt SHA-256 no disponible");
  }
  ~Sha256() {
    if (h_) BCryptDestroyHash(h_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void Update(const void* p, std::size_t n) {
    BCryptHashData(h_, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0);
  }
  std::string HexDigest() {
    unsigned char d[32];
    BCryptFinishHash(h_, d, sizeof d, 0);
    std::string s;
    for (unsigned char c : d) s += std::format("{:02x}", c);
    return s;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE h_ = nullptr;
};

// ------------------------------------------------------------------------------------- ficheros
class InFile {
 public:
  explicit InFile(const fs::path& p) : f_(_wfopen(p.c_str(), L"rb")) {
    if (!f_) throw std::runtime_error("no se puede abrir " + U8(p));
    _fseeki64(f_, 0, SEEK_END);
    size_ = static_cast<std::uint64_t>(_ftelli64(f_));
  }
  ~InFile() {
    if (f_) std::fclose(f_);
  }
  InFile(const InFile&) = delete;
  InFile& operator=(const InFile&) = delete;
  std::uint64_t size() const { return size_; }
  void ReadAt(std::uint64_t off, void* dst, std::size_t n) {
    if (_fseeki64(f_, static_cast<long long>(off), SEEK_SET) != 0 || std::fread(dst, 1, n, f_) != n)
      throw std::runtime_error("lectura fuera del fichero (ZIP danado?)");
  }

 private:
  std::FILE* f_;
  std::uint64_t size_ = 0;
};

std::uint16_t Le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t Le32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
         static_cast<std::uint32_t>(p[3]) << 24;
}

// ------------------------------------------------------------------------------------- ZIP (APK)
struct ZipEntry {
  std::string name;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint64_t csize = 0, usize = 0, lho = 0;
};

class Zip {
 public:
  explicit Zip(const fs::path& p) : f_(p) {
    // EOCD: ultimos 22 + comentario (max 64 KiB).
    const std::uint64_t tail = std::min<std::uint64_t>(f_.size(), 22 + 65535);
    std::vector<unsigned char> b(static_cast<std::size_t>(tail));
    f_.ReadAt(f_.size() - tail, b.data(), b.size());
    std::size_t eocd = std::string::npos;
    for (std::size_t i = b.size() >= 22 ? b.size() - 22 : 0;; --i) {
      if (Le32(&b[i]) == 0x06054b50) {
        eocd = i;
        break;
      }
      if (i == 0) break;
    }
    if (eocd == std::string::npos) throw std::runtime_error("no es un ZIP/APK valido (sin directorio central)");
    const std::uint32_t cd_size = Le32(&b[eocd + 12]);
    const std::uint32_t cd_off = Le32(&b[eocd + 16]);
    std::vector<unsigned char> cd(cd_size);
    f_.ReadAt(cd_off, cd.data(), cd.size());
    for (std::size_t p = 0; p + 46 <= cd.size() && Le32(&cd[p]) == 0x02014b50;) {
      ZipEntry e;
      e.method = Le16(&cd[p + 10]);
      e.crc = Le32(&cd[p + 16]);
      e.csize = Le32(&cd[p + 20]);
      e.usize = Le32(&cd[p + 24]);
      const std::uint16_t nlen = Le16(&cd[p + 28]), xlen = Le16(&cd[p + 30]), clen = Le16(&cd[p + 32]);
      e.lho = Le32(&cd[p + 42]);
      if (p + 46 + nlen > cd.size()) break;
      e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
      entries_.push_back(std::move(e));
      p += 46u + nlen + xlen + clen;
    }
  }
  const std::vector<ZipEntry>& entries() const { return entries_; }
  const ZipEntry* Find(const std::string& name) const {
    for (const auto& e : entries_)
      if (e.name == name) return &e;
    return nullptr;
  }
  InFile& file() { return f_; }
  std::uint64_t DataOffset(const ZipEntry& e) {
    unsigned char h[30];
    f_.ReadAt(e.lho, h, sizeof h);
    if (Le32(h) != 0x04034b50) throw std::runtime_error("cabecera local ZIP invalida: " + e.name);
    return e.lho + 30 + Le16(h + 26) + Le16(h + 28);
  }

 private:
  InFile f_;
  std::vector<ZipEntry> entries_;
};

// Lector secuencial de una entrada del APK (STORED o DEFLATED) con verificacion de CRC al final.
class EntryStream {
 public:
  EntryStream(Zip& z, const ZipEntry& e) : z_(z), e_(e), off_(z.DataOffset(e)), in_(kBuf) {
    if (e.method != 0 && e.method != 8) throw std::runtime_error(std::format("{}: metodo ZIP {} no soportado", e.name, e.method));
    if (e.method == 8) {
      std::memset(&zs_, 0, sizeof zs_);
      if (inflateInit2(&zs_, -MAX_WBITS) != Z_OK) throw std::runtime_error("inflateInit2 fallo");
      zinit_ = true;
    }
  }
  ~EntryStream() {
    if (zinit_) inflateEnd(&zs_);
  }
  EntryStream(const EntryStream&) = delete;
  EntryStream& operator=(const EntryStream&) = delete;

  // Lee hasta n bytes descomprimidos; 0 = fin.
  std::size_t Read(void* dst, std::size_t n) {
    if (out_ >= e_.usize) {
      Finish();
      return 0;
    }
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, e_.usize - out_));
    std::size_t got = 0;
    if (e_.method == 0) {
      z_.file().ReadAt(off_ + out_, dst, n);
      got = n;
    } else {
      zs_.next_out = static_cast<Bytef*>(dst);
      zs_.avail_out = static_cast<uInt>(n);
      while (zs_.avail_out > 0) {
        if (zs_.avail_in == 0) {
          const std::uint64_t left = e_.csize - in_pos_;
          if (left == 0) break;
          const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(left, in_.size()));
          z_.file().ReadAt(off_ + in_pos_, in_.data(), k);
          in_pos_ += k;
          zs_.next_in = in_.data();
          zs_.avail_in = static_cast<uInt>(k);
        }
        const int r = inflate(&zs_, Z_NO_FLUSH);
        if (r == Z_STREAM_END) break;
        if (r != Z_OK && r != Z_BUF_ERROR) throw std::runtime_error(std::format("{}: datos deflate corruptos ({})", e_.name, r));
        if (r == Z_BUF_ERROR && zs_.avail_in == 0 && in_pos_ >= e_.csize) break;
      }
      got = n - zs_.avail_out;
      if (got == 0) throw std::runtime_error(e_.name + ": entrada truncada");
    }
    crc_ = crc32_z(crc_, static_cast<const Bytef*>(dst), got);
    out_ += got;
    if (out_ >= e_.usize) Finish();
    return got;
  }
  const ZipEntry& entry() const { return e_; }

 private:
  void Finish() {
    if (checked_) return;
    checked_ = true;
    if (crc_ != e_.crc) throw std::runtime_error(std::format("{}: CRC incorrecto (APK danado o incompleto)", e_.name));
  }
  Zip& z_;
  const ZipEntry e_;
  std::uint64_t off_;
  std::vector<unsigned char> in_;
  z_stream zs_{};
  bool zinit_ = false, checked_ = false;
  std::uint64_t in_pos_ = 0, out_ = 0;
  uLong crc_ = 0;
};

// Concatenacion secuencial de varias entradas (archiveObbCache.zip.001 + .002 + ...).
class ConcatStream {
 public:
  // `progress`: se incrementa con los bytes consumidos (= tamano de las partes: total exacto para la barra).
  ConcatStream(Zip& z, std::vector<const ZipEntry*> parts, std::atomic<std::uint64_t>* progress)
      : z_(z), parts_(std::move(parts)), progress_(progress) {}
  std::size_t Read(void* dst, std::size_t n) {
    auto* d = static_cast<unsigned char*>(dst);
    std::size_t got = 0;
    while (got < n) {
      if (!cur_) {
        if (idx_ >= parts_.size()) break;
        cur_ = std::make_unique<EntryStream>(z_, *parts_[idx_++]);
      }
      const std::size_t k = cur_->Read(d + got, n - got);
      if (k == 0) cur_.reset();
      got += k;
    }
    if (progress_) *progress_ += got;
    return got;
  }
  bool ReadExact(void* dst, std::size_t n) { return Read(dst, n) == n; }
  void Skip(std::uint64_t n) {
    std::vector<unsigned char> b(64 * 1024);
    while (n) {
      const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, b.size()));
      if (!ReadExact(b.data(), k)) throw std::runtime_error("archiveObbCache truncado");
      n -= k;
    }
  }

 private:
  Zip& z_;
  std::vector<const ZipEntry*> parts_;
  std::size_t idx_ = 0;
  std::unique_ptr<EntryStream> cur_;
  std::atomic<std::uint64_t>* progress_ = nullptr;
};

// ------------------------------------------------------------------------------------- salida
class OutFile {
 public:
  OutFile(const fs::path& final_path) : final_(final_path), part_(final_path) {
    part_ += ".part";
    fs::create_directories(final_.parent_path());
    f_ = _wfopen(part_.c_str(), L"wb");
    if (!f_) throw std::runtime_error("no se puede escribir " + U8(part_));
  }
  ~OutFile() {
    if (f_) {
      std::fclose(f_);
      std::error_code ec;
      fs::remove(part_, ec);
    }
  }
  OutFile(const OutFile&) = delete;
  OutFile& operator=(const OutFile&) = delete;
  void Write(const void* p, std::size_t n) {
    if (std::fwrite(p, 1, n, f_) != n) throw std::runtime_error("disco lleno o error de escritura: " + U8(part_));
    sha_.Update(p, n);
  }
  // Cierra y renombra; devuelve el SHA-256.
  std::string Commit() {
    std::fclose(f_);
    f_ = nullptr;
    std::error_code ec;
    fs::remove(final_, ec);
    fs::rename(part_, final_);
    return sha_.HexDigest();
  }

 private:
  fs::path final_, part_;
  std::FILE* f_ = nullptr;
  Sha256 sha_;
};

class Installer {
 public:
  Installer(const fs::path& dest, Progress& pr, Result& res) : dest_(dest), pr_(pr), res_(res), buf_(kBuf) {}

  void CheckCancel() {
    if (pr_.cancel) throw Cancelled();
  }

  // Copia una entrada del APK a dest/rel. Devuelve el SHA-256.
  std::string ExtractEntry(Zip& z, const ZipEntry& e, const fs::path& rel) {
    pr_.SetStage("Extrayendo " + U8(rel.filename()));
    EntryStream s(z, e);
    OutFile out(dest_ / rel);
    for (std::size_t k; (k = s.Read(buf_.data(), buf_.size())) > 0;) {
      CheckCancel();
      out.Write(buf_.data(), k);
      pr_.done += k;
    }
    auto sha = out.Commit();
    Wrote(rel);
    return sha;
  }

  std::string CopyFile(const fs::path& from, const fs::path& rel) {
    pr_.SetStage("Copiando " + U8(from.filename()));
    InFile in(from);
    OutFile out(dest_ / rel);
    for (std::uint64_t off = 0; off < in.size();) {
      CheckCancel();
      const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(buf_.size(), in.size() - off));
      in.ReadAt(off, buf_.data(), k);
      out.Write(buf_.data(), k);
      off += k;
      pr_.done += k;
    }
    auto sha = out.Commit();
    Wrote(rel);
    return sha;
  }

  // ZIP partido por concatenacion: recorre las cabeceras locales en orden (no hace falta acceso aleatorio).
  void ExtractObbCache(Zip& z, const std::vector<const ZipEntry*>& parts) {
    ConcatStream cs(z, parts, &pr_.done);
    std::vector<unsigned char> in(kBuf);
    for (;;) {
      CheckCancel();
      unsigned char h[30];
      if (!cs.ReadExact(h, 4)) break;
      const std::uint32_t sig = Le32(h);
      if (sig == 0x02014b50 || sig == 0x06054b50) break;  // directorio central: fin de las entradas
      if (sig != 0x04034b50) throw std::runtime_error("archiveObbCache: cabecera local inesperada");
      if (!cs.ReadExact(h + 4, 26)) throw std::runtime_error("archiveObbCache truncado");
      const std::uint16_t flags = Le16(h + 6), method = Le16(h + 8);
      const std::uint32_t crc = Le32(h + 14);
      std::uint64_t csize = Le32(h + 18), usize = Le32(h + 22);
      const std::uint16_t nlen = Le16(h + 26), xlen = Le16(h + 28);
      std::string name(nlen, '\0');
      if (!cs.ReadExact(name.data(), nlen)) throw std::runtime_error("archiveObbCache truncado");
      std::vector<unsigned char> extra(xlen);
      if (xlen && !cs.ReadExact(extra.data(), xlen)) throw std::runtime_error("archiveObbCache truncado");
      // ZIP64 en el extra (id 1) si los tamanos son 0xFFFFFFFF.
      for (std::size_t p = 0; p + 4 <= extra.size();) {
        const std::uint16_t id = Le16(&extra[p]), sz = Le16(&extra[p + 2]);
        if (id == 1 && sz >= 16 && p + 4 + 16 <= extra.size()) {
          auto le64 = [&](std::size_t o) { return Le32(&extra[o]) | static_cast<std::uint64_t>(Le32(&extra[o + 4])) << 32; };
          if (usize == 0xFFFFFFFF) usize = le64(p + 4);
          if (csize == 0xFFFFFFFF) csize = le64(p + 12);
        }
        p += 4u + sz;
      }
      if (flags & 8) throw std::runtime_error("archiveObbCache: entradas con data descriptor no soportadas");
      const std::string base = name.substr(name.find_last_of('/') + 1);
      const bool is_obb = !base.empty() && Lower(base).ends_with(".obb");
      if (!is_obb) {
        log::Info("instalador: archiveObbCache: se omite {}", name);
        cs.Skip(csize);
        continue;
      }
      const fs::path rel = fs::path("obb") / U8Path(base);
      pr_.SetStage("Extrayendo " + base);
      OutFile out(dest_ / rel);
      uLong c = 0;
      if (method == 0) {
        for (std::uint64_t left = csize; left;) {
          CheckCancel();
          const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf_.size()));
          if (!cs.ReadExact(buf_.data(), k)) throw std::runtime_error("archiveObbCache truncado");
          c = crc32_z(c, buf_.data(), k);
          out.Write(buf_.data(), k);
          left -= k;
        }
      } else if (method == 8) {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw std::runtime_error("inflateInit2 fallo");
        struct End {
          z_stream* z;
          ~End() { inflateEnd(z); }
        } end{&zs};
        std::uint64_t left = csize, outn = 0;
        int r = Z_OK;
        while (r != Z_STREAM_END) {
          CheckCancel();
          if (zs.avail_in == 0) {
            if (!left) throw std::runtime_error(base + ": deflate truncado");
            const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(left, in.size()));
            if (!cs.ReadExact(in.data(), k)) throw std::runtime_error("archiveObbCache truncado");
            left -= k;
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(k);
          }
          zs.next_out = buf_.data();
          zs.avail_out = static_cast<uInt>(buf_.size());
          r = inflate(&zs, Z_NO_FLUSH);
          if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) throw std::runtime_error(base + ": deflate corrupto");
          const std::size_t k = buf_.size() - zs.avail_out;
          c = crc32_z(c, buf_.data(), k);
          out.Write(buf_.data(), k);
          outn += k;
        }
        if (left) cs.Skip(left);
        if (outn != usize) throw std::runtime_error(std::format("{}: tamano {} != {}", base, outn, usize));
      } else {
        throw std::runtime_error(std::format("{}: metodo ZIP {} no soportado", base, method));
      }
      if (c != crc) throw std::runtime_error(base + ": CRC incorrecto (APK danado)");
      obb_sha_[Lower(base)] = out.Commit();
      Wrote(rel);
    }
  }

  void Wrote(const fs::path& rel) {
    res_.files.push_back(U8(rel));
    log::Info("instalador: escrito {}", U8(rel));
  }

  std::map<std::string, std::string> obb_sha_;

 private:
  fs::path dest_;
  Progress& pr_;
  Result& res_;
  std::vector<unsigned char> buf_;
};

bool IsMainObb(const std::string& n) { return Lower(n).starts_with("main.") && Lower(n).ends_with(".obb"); }
bool IsPatchObb(const std::string& n) { return Lower(n).starts_with("patch.") && Lower(n).ends_with(".obb"); }

std::vector<const ZipEntry*> CacheParts(const Zip& z) {
  std::vector<const ZipEntry*> parts;
  for (int i = 1; i < 100; ++i) {
    const auto* e = z.Find(std::format("assets/archiveObbCache.zip.{:03}", i));
    if (!e) break;
    parts.push_back(e);
  }
  return parts;
}

}  // namespace

// ------------------------------------------------------------------------------------- API
std::string Sha256File(const fs::path& p) {
  try {
    InFile f(p);
    Sha256 h;
    std::vector<unsigned char> b(kBuf);
    for (std::uint64_t off = 0; off < f.size();) {
      const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), f.size() - off));
      f.ReadAt(off, b.data(), k);
      h.Update(b.data(), k);
      off += k;
    }
    return h.HexDigest();
  } catch (...) {
    return "";
  }
}

ApkInfo Inspect(const fs::path& apk) {
  ApkInfo info;
  try {
    Zip z(apk);
    info.ok = true;
    info.has_lib = z.Find("lib/armeabi-v7a/libZombieWood.so") != nullptr;
    info.has_inner_apk = z.Find("assets/game.apk") != nullptr;
    for (const auto* e : CacheParts(z)) {
      ++info.obb_cache_parts;
      info.obb_cache_bytes += e->usize;
    }
    info.has_obb_cache = info.obb_cache_parts > 0;
    for (const auto& e : z.entries())
      if (e.name.starts_with("res/raw/") && Lower(e.name).ends_with(".mp4")) info.has_videos = true;
    if (!info.has_lib && !info.has_inner_apk) info.error = "el APK no contiene lib/armeabi-v7a/libZombieWood.so (no es Zombiewood?)";
  } catch (const std::exception& e) {
    info.error = e.what();
  }
  return info;
}

std::vector<fs::path> FindObbsNear(const fs::path& apk) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const fs::path dir : {apk.parent_path(), apk.parent_path() / "obb", apk.parent_path() / "com.gameloft.android.ANMP.GloftZOHM"}) {
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
      const std::string n = U8(de.path().filename());
      if (de.is_regular_file(ec) && (IsMainObb(n) || IsPatchObb(n)) && Lower(n).find("gloftzohm") != std::string::npos)
        out.push_back(de.path());
    }
  }
  return out;
}

bool IsInstalled(const fs::path& dest) {
  std::error_code ec;
  return fs::is_regular_file(dest / "lib" / "libZombieWood.so", ec) && fs::is_regular_file(dest / "obb" / kMainObbName, ec);
}

Result Install(const Source& src, const fs::path& dest, Progress& pr) {
  Result res;
  const auto t0 = std::chrono::steady_clock::now();
  log::Info("instalador: origen {} ({} OBB sueltos) -> {}", U8(src.apk), src.obbs.size(), U8(dest));
  fs::path tmp_inner;
  try {
    std::error_code ec;
    fs::create_directories(dest, ec);
    Installer ins(dest, pr, res);
    pr.SetStage("Leyendo el APK");
    auto zip = std::make_unique<Zip>(src.apk);
    auto cache = CacheParts(*zip);

    // Tamano total esperado (para la barra).
    std::uint64_t total = 0;
    for (const auto* e : cache) total += e->usize;  // la barra cuenta lo consumido de las partes
    for (const auto& o : src.obbs) total += fs::file_size(o, ec);

    // APK sin lib en la raiz pero con assets/game.apk (wrapper): extraer el original a un temporal.
    std::unique_ptr<Zip> inner;
    Zip* game = zip.get();
    if (!zip->Find("lib/armeabi-v7a/libZombieWood.so")) {
      const auto* ga = zip->Find("assets/game.apk");
      if (!ga) throw std::runtime_error("el APK no contiene lib/armeabi-v7a/libZombieWood.so ni assets/game.apk");
      tmp_inner = dest / "game.apk.tmp";
      total += ga->usize;
      pr.total = total;
      ins.ExtractEntry(*zip, *ga, tmp_inner.filename());
      res.files.pop_back();
      inner = std::make_unique<Zip>(tmp_inner);
      game = inner.get();
    }
    const ZipEntry* so = game->Find("lib/armeabi-v7a/libZombieWood.so");
    if (!so) throw std::runtime_error("assets/game.apk no contiene libZombieWood.so");
    std::vector<std::pair<const ZipEntry*, fs::path>> files;
    files.push_back({so, fs::path("lib") / "libZombieWood.so"});
    if (const auto* g = game->Find("lib/armeabi-v7a/libgenerator.so")) files.push_back({g, fs::path("lib") / "libgenerator.so"});
    for (const auto& e : game->entries()) {
      if (e.name.starts_with("res/raw/") && e.name.size() > 8 && e.usize > 0)
        files.push_back({&e, fs::path("res") / "raw" / U8Path(e.name.substr(8))});
      if (e.name == "assets/colors.conf" || e.name == "assets/families.list")
        files.push_back({&e, fs::path("assets") / U8Path(e.name.substr(7))});
    }
    for (const auto& [e, rel] : files) total += e->usize;
    pr.total = total;

    // 1) libs + recursos
    for (const auto& [e, rel] : files) {
      const std::string sha = ins.ExtractEntry(*game, *e, rel);
      if (rel.filename() == "libZombieWood.so") {
        res.lib_sha256 = sha;
        res.version_ok = sha == kShaZombieWood;
        if (!res.version_ok)
          res.warnings.push_back(std::format("libZombieWood.so tiene un SHA-256 desconocido ({}): no es la version {} soportada; "
                                             "puede no funcionar.",
                                             sha, kSupportedVersion));
      } else if (rel.filename() == "libgenerator.so" && sha != kShaGenerator) {
        res.warnings.push_back("libgenerator.so distinto del esperado (no lo usa el port).");
      }
    }
    if (const auto iv = dest / "res" / "raw" / "infoversion.txt"; fs::exists(iv, ec)) {
      std::ifstream f(iv);
      std::string v;
      std::getline(f, v);
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
      if (v != kSupportedVersion) res.warnings.push_back(std::format("infoversion.txt dice \"{}\" (soportada: {}).", v, kSupportedVersion));
    }

    // 2) OBB
    if (!cache.empty()) {
      log::Info("instalador: archiveObbCache en {} partes", cache.size());
      ins.ExtractObbCache(*zip, cache);
    } else {
      for (const auto& o : src.obbs) {
        const std::string n = U8(o.filename());
        ins.obb_sha_[Lower(n)] = ins.CopyFile(o, fs::path("obb") / o.filename());
      }
    }
    bool have_main = false, have_patch = false;
    for (const auto& [n, sha] : ins.obb_sha_) {
      if (n == Lower(kMainObbName)) {
        have_main = true;
        if (sha != kShaMainObb) res.warnings.push_back("OBB main con SHA-256 desconocido: version distinta o archivo danado.");
      } else if (n == Lower(kPatchObbName)) {
        have_patch = true;
        if (sha != kShaPatchObb) res.warnings.push_back("OBB patch con SHA-256 desconocido: version distinta o archivo danado.");
      } else {
        res.warnings.push_back("OBB no esperado (otra version?): " + n);
      }
    }
    if (!have_main)
      throw std::runtime_error(std::string("falta el OBB principal (") + kMainObbName +
                               "). Elige el APK grande (con assets/archiveObbCache.zip.00N) o indica los .obb.");
    if (!have_patch) res.warnings.push_back(std::string("falta el OBB de parche (") + kPatchObbName + "): el juego arrancara sin el.");

    // 3) install.json
    nlohmann::json j;
    j["source_apk"] = U8(src.apk);
    j["version_expected"] = kSupportedVersion;
    j["version_ok"] = res.version_ok;
    j["libZombieWood_sha256"] = res.lib_sha256;
    for (const auto& [n, sha] : ins.obb_sha_) j["obb_sha256"][n] = sha;
    j["files"] = res.files;
    j["warnings"] = res.warnings;
    std::ofstream(dest / "install.json") << j.dump(2);
    res.ok = true;
  } catch (const std::exception& e) {
    res.ok = false;
    res.error = e.what();
  }
  if (!tmp_inner.empty()) {
    std::error_code ec;
    fs::remove(tmp_inner, ec);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (res.ok) log::Info("instalador: OK en {:.1f} s ({} ficheros, {} avisos)", secs, res.files.size(), res.warnings.size());
  else log::Error("instalador: fallo tras {:.1f} s: {}", secs, res.error);
  for (const auto& w : res.warnings) log::Warn("instalador: {}", w);
  pr.finished = true;
  return res;
}

}  // namespace zw::installer
