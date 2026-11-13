// Tests del instalador (T9) con APK sinteticos (no necesitan los datos del juego):
//  - APK "grande" con archiveObbCache partido en 2 partes (deflate dentro de deflate), libs, videos y assets.
//  - APK pequeno + OBB sueltos junto al APK (FindObbsNear).
//  - errores: sin OBB principal, CRC corrupto, cancelacion, fichero que no es ZIP.
//  - SHA-256 conocido y config (SaveConfig/LoadConfig) en un <home> temporal.
#include <windows.h>
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "installer/installer.h"

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

static int g_fail = 0, g_ok = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (c) ++g_ok;                                                       \
    else {                                                               \
      ++g_fail;                                                          \
      std::printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c);         \
    }                                                                    \
  } while (0)

// ---------------------------------------------------------------- escritor ZIP minimo
class ZipW {
 public:
  void Add(const std::string& name, const Bytes& data, bool deflate, bool bad_crc = false) {
    Bytes comp = data;
    if (deflate) {
      z_stream z{};
      deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
      comp.resize(deflateBound(&z, static_cast<uLong>(data.size())) + 16);
      z.next_in = const_cast<Bytef*>(data.data());
      z.avail_in = static_cast<uInt>(data.size());
      z.next_out = comp.data();
      z.avail_out = static_cast<uInt>(comp.size());
      deflate_(z);
      comp.resize(z.total_out);
      deflateEnd(&z);
    }
    uLong crc = crc32(0, data.data(), static_cast<uInt>(data.size()));
    if (bad_crc) crc ^= 0x5a5a;
    Entry e{name, static_cast<std::uint32_t>(out_.size()), static_cast<std::uint32_t>(crc), static_cast<std::uint32_t>(comp.size()),
            static_cast<std::uint32_t>(data.size()), static_cast<std::uint16_t>(deflate ? 8 : 0)};
    U32(0x04034b50); U16(20); U16(0); U16(e.method); U16(0); U16(0);
    U32(e.crc); U32(e.csize); U32(e.usize); U16(static_cast<std::uint16_t>(name.size())); U16(0);
    out_.insert(out_.end(), name.begin(), name.end());
    out_.insert(out_.end(), comp.begin(), comp.end());
    entries_.push_back(e);
  }
  Bytes Finish() {
    const auto cd = static_cast<std::uint32_t>(out_.size());
    for (const auto& e : entries_) {
      U32(0x02014b50); U16(20); U16(20); U16(0); U16(e.method); U16(0); U16(0);
      U32(e.crc); U32(e.csize); U32(e.usize); U16(static_cast<std::uint16_t>(e.name.size())); U16(0); U16(0); U16(0); U16(0);
      U32(0); U32(e.lho);
      out_.insert(out_.end(), e.name.begin(), e.name.end());
    }
    const auto cd_size = static_cast<std::uint32_t>(out_.size()) - cd;
    U32(0x06054b50); U16(0); U16(0); U16(static_cast<std::uint16_t>(entries_.size())); U16(static_cast<std::uint16_t>(entries_.size()));
    U32(cd_size); U32(cd); U16(0);
    return out_;
  }

 private:
  struct Entry {
    std::string name;
    std::uint32_t lho, crc, csize, usize;
    std::uint16_t method;
  };
  static void deflate_(z_stream& z) { ::deflate(&z, Z_FINISH); }
  void U16(std::uint16_t v) { out_.push_back(v & 0xff); out_.push_back(v >> 8); }
  void U32(std::uint32_t v) { U16(v & 0xffff); U16(v >> 16); }
  Bytes out_;
  std::vector<Entry> entries_;
};

Bytes Pattern(std::size_t n, unsigned seed) {
  Bytes b(n);
  unsigned x = seed;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1103515245u + 12345u;
    b[i] = static_cast<unsigned char>((i % 97 < 60) ? (i & 0x3f) : (x >> 16));  // mitad compresible
  }
  return b;
}
Bytes Str(const std::string& s) { return Bytes(s.begin(), s.end()); }
void WriteFile(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
Bytes ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}

const Bytes kSo = Pattern(300000, 1), kGen = Pattern(5000, 2), kMp4 = Pattern(200000, 3), kMain = Pattern(3'000'000, 4),
            kPatch = Pattern(70000, 5);

Bytes ObbZip() {
  ZipW z;
  z.Add(std::string("main.1501.com.gameloft.android.ANMP.GloftZOHM.obb"), kMain, true);
  z.Add(std::string("patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb"), kPatch, false);
  return z.Finish();
}

Bytes Apk(bool with_cache, bool bad_crc = false) {
  ZipW z;
  z.Add("AndroidManifest.xml", Str("manifest"), true);
  z.Add("lib/armeabi-v7a/libZombieWood.so", kSo, true, bad_crc);
  z.Add("lib/armeabi-v7a/libgenerator.so", kGen, true);
  z.Add("res/raw/logo_hd.mp4", kMp4, false);
  z.Add("res/raw/infoversion.txt", Str("1.5.3"), false);
  z.Add("res/raw/serialkey.txt", Bytes{}, false);  // vacio: se omite
  z.Add("assets/colors.conf", Str("colors"), true);
  z.Add("assets/families.list", Str("families"), true);
  if (with_cache) {
    const Bytes obb = ObbZip();
    const std::size_t half = obb.size() / 2 + 123;  // corte arbitrario (a mitad de una entrada)
    z.Add("assets/archiveObbCache.zip.001", Bytes(obb.begin(), obb.begin() + half), true);
    z.Add("assets/archiveObbCache.zip.002", Bytes(obb.begin() + half, obb.end()), true);
  }
  return z.Finish();
}

int main() {
  wchar_t tmpw[MAX_PATH];
  GetTempPathW(MAX_PATH, tmpw);
  const fs::path root = fs::path(tmpw) / std::format("zw_test_installer_{}", GetCurrentProcessId());
  fs::remove_all(root);
  fs::create_directories(root);
  _wputenv_s(L"ZOMBIEWOOD_HOME", (root / "home").c_str());
  zw::log::Init();
  using namespace zw::installer;

  // SHA-256 conocido
  WriteFile(root / "abc.txt", Str("abc"));
  CHECK(Sha256File(root / "abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha256File(root / "no_existe") == "");

  // 1) APK grande con archiveObbCache
  {
    const fs::path apk = root / "big" / "game.apk";
    WriteFile(apk, Apk(true));
    const auto info = Inspect(apk);
    CHECK(info.ok && info.error.empty());
    CHECK(info.has_lib && info.has_obb_cache && info.obb_cache_parts == 2 && info.has_videos && !info.has_inner_apk);
    const fs::path dest = root / "big_out";
    CHECK(!IsInstalled(dest));
    Progress pr;
    const auto r = Install({apk, {}}, dest, pr);
    if (!r.ok) std::printf("error: %s\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(pr.finished);
    CHECK(pr.done > 0 && pr.done <= pr.total + 1);
    CHECK(!r.version_ok);  // la .so sintetica no es la 1.5.3 real
    CHECK(r.warnings.size() >= 3);  // .so, generator, main y patch desconocidos
    CHECK(ReadFile(dest / "lib" / "libZombieWood.so") == kSo);
    CHECK(ReadFile(dest / "lib" / "libgenerator.so") == kGen);
    CHECK(ReadFile(dest / "res" / "raw" / "logo_hd.mp4") == kMp4);
    CHECK(ReadFile(dest / "assets" / "colors.conf") == Str("colors"));
    CHECK(ReadFile(dest / "obb" / kMainObbName) == kMain);
    CHECK(ReadFile(dest / "obb" / kPatchObbName) == kPatch);
    CHECK(!fs::exists(dest / "res" / "raw" / "serialkey.txt"));
    CHECK(fs::exists(dest / "install.json"));
    CHECK(IsInstalled(dest));
    CHECK(r.lib_sha256 == Sha256File(dest / "lib" / "libZombieWood.so"));
    // sin .part sueltos
    bool part = false;
    for (const auto& de : fs::recursive_directory_iterator(dest))
      if (de.path().extension() == ".part") part = true;
    CHECK(!part);
  }

  // 2) APK pequeno + OBB sueltos junto al APK
  {
    const fs::path dir = root / "small";
    WriteFile(dir / "base.apk", Apk(false));
    WriteFile(dir / kMainObbName, kMain);
    WriteFile(dir / "obb" / kPatchObbName, kPatch);
    const auto info = Inspect(dir / "base.apk");
    CHECK(info.ok && !info.has_obb_cache);
    const auto obbs = FindObbsNear(dir / "base.apk");
    CHECK(obbs.size() == 2);
    Progress pr;
    const auto r = Install({dir / "base.apk", obbs}, root / "small_out", pr);
    CHECK(r.ok);
    CHECK(ReadFile(root / "small_out" / "obb" / kMainObbName) == kMain);
    CHECK(ReadFile(root / "small_out" / "obb" / kPatchObbName) == kPatch);
    CHECK(IsInstalled(root / "small_out"));
  }

  // 3) Sin OBB principal
  {
    WriteFile(root / "noobb" / "x.apk", Apk(false));
    Progress pr;
    const auto r = Install({root / "noobb" / "x.apk", {}}, root / "noobb_out", pr);
    CHECK(!r.ok && r.error.find("OBB principal") != std::string::npos);
    CHECK(!IsInstalled(root / "noobb_out"));
  }

  // 4) CRC corrupto
  {
    WriteFile(root / "crc" / "x.apk", Apk(true, /*bad_crc=*/true));
    Progress pr;
    const auto r = Install({root / "crc" / "x.apk", {}}, root / "crc_out", pr);
    CHECK(!r.ok && r.error.find("CRC") != std::string::npos);
    CHECK(!fs::exists(root / "crc_out" / "lib" / "libZombieWood.so"));
    CHECK(!fs::exists(root / "crc_out" / "lib" / "libZombieWood.so.part"));
  }

  // 5) Cancelacion
  {
    Progress pr;
    pr.cancel = true;
    const auto r = Install({root / "big" / "game.apk", {}}, root / "cancel_out", pr);
    CHECK(!r.ok && r.error.find("cancel") != std::string::npos);
    CHECK(!IsInstalled(root / "cancel_out"));
  }

  // 6) No es un ZIP
  {
    WriteFile(root / "bad.apk", Str("esto no es un zip"));
    const auto info = Inspect(root / "bad.apk");
    CHECK(!info.ok && !info.error.empty());
  }

  // 7) Config: guardar y releer (conserva claves desconocidas, deja .bak)
  {
    nlohmann::json c = {{"width", 1600}, {"clave_desconocida", "x"}};
    CHECK(zw::SaveConfig(c));
    c["width"] = 1920;
    CHECK(zw::SaveConfig(c));
    const auto back = zw::LoadConfig();
    CHECK(back.value("width", 0) == 1920 && back.value("clave_desconocida", std::string()) == "x");
    CHECK(fs::exists(root / "home" / "zombiewood.json.bak"));
    CHECK(zw::ConfigGet(back, "width", std::string("def")) == "def");  // tipo incorrecto -> defecto
  }

  // 8) Logs rotativos: Init otra vez rota zombiewood.log -> zombiewood.1.log
  {
    zw::log::Info("primera sesion");
    zw::log::Init();
    CHECK(fs::exists(root / "home" / "logs" / "zombiewood.1.log"));
    for (int i = 0; i < 50; ++i) zw::log::Info("[guest stdout] TapJoyManager.cpp - Response not valid");
    zw::log::Shutdown();
    std::ifstream f(root / "home" / "logs" / "zombiewood.log");
    std::string all((std::istreambuf_iterator<char>(f)), {}), line;
    std::size_t n = 0;
    for (std::size_t p = 0; (p = all.find("Response not valid", p)) != std::string::npos; ++p) ++n;
    CHECK(n == 4);  // 3 escritas + 1 resumen "(x47 ...)"
    CHECK(all.find("(x47") != std::string::npos);
  }

  std::printf("%d comprobaciones OK, %d fallos\n", g_ok, g_fail);
  std::error_code ec;
  fs::remove_all(root, ec);
  return g_fail ? 1 : 0;
}
