// Instalador de datos (T9): extrae del APK del usuario (y de sus OBB) lo que necesita el port.
//
// Entradas aceptadas (ANALYSIS §10, T9):
//   a) APK "repack" grande: lib/armeabi-v7a/*.so + res/raw/*.mp4 + assets/archiveObbCache.zip.001..00N
//      (ZIP partido por concatenacion que contiene main.1501...obb y patch.15325...obb).
//   b) APK original (pequeno) + los .obb sueltos (elegidos por el usuario o junto al APK).
//   c) APK que solo trae assets/game.apk (el original dentro del wrapper) + archiveObbCache.
//
// Salida en `dest` (por defecto <home>/data):
//   lib/libZombieWood.so, lib/libgenerator.so, obb/main.*.obb, obb/patch.*.obb, res/raw/* (videos mp4 y textos),
//   assets/colors.conf, assets/families.list, install.json (origen, hashes, version).
// Cada fichero se escribe como "<nombre>.part" y se renombra al terminar: una instalacion cortada no
// deja ficheros finales a medias. Todo se lee en streaming (zlib), sin ficheros temporales grandes.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace zw::installer {

// SHA-256 conocidos de la version soportada (Zombiewood 1.5.3, com.gameloft.android.ANMP.GloftZOHM).
inline constexpr const char* kSupportedVersion = "1.5.3";
inline constexpr const char* kShaZombieWood = "2855ed9f95367d5dc592a2179c048383c6a177b7ca6da19a2919a2527cfa967c";
inline constexpr const char* kShaGenerator = "3871b9c4ae5d1c491abbccc7af1744121bf964f52b4812d2f672c72ff7c062ed";
inline constexpr const char* kShaMainObb = "4260ed44b071530a99c7eb281a45424ec78ae6edb0948d442c11278fd1cddbfe";
inline constexpr const char* kShaPatchObb = "db9bfded92e0c736d0daff88ff7984012ec50c356cd5e955ca140f86bdad2296";
inline constexpr const char* kMainObbName = "main.1501.com.gameloft.android.ANMP.GloftZOHM.obb";
inline constexpr const char* kPatchObbName = "patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb";

// Progreso compartido con la UI (hilo del instalador -> hilo de la ventana).
struct Progress {
  std::atomic<std::uint64_t> done{0}, total{0};  // bytes escritos / esperados (total aproximado)
  std::atomic<bool> cancel{false};
  std::atomic<bool> finished{false};
  void SetStage(std::string s) {
    std::lock_guard lk(mu);
    stage = std::move(s);
  }
  std::string Stage() {
    std::lock_guard lk(mu);
    return stage;
  }

 private:
  std::mutex mu;
  std::string stage;
};

// Que trae un APK (sin extraer nada).
struct ApkInfo {
  bool ok = false;           // es un ZIP legible
  std::string error;
  bool has_lib = false;      // lib/armeabi-v7a/libZombieWood.so
  bool has_obb_cache = false;  // assets/archiveObbCache.zip.001...
  int obb_cache_parts = 0;
  bool has_inner_apk = false;  // assets/game.apk
  bool has_videos = false;     // res/raw/*.mp4
  std::uint64_t obb_cache_bytes = 0;  // suma de tamanos descomprimidos de las partes
  std::string package;         // "" si no se pudo saber
};
ApkInfo Inspect(const std::filesystem::path& apk);

// OBB sueltos que haya junto al APK (main.*.obb / patch.*.obb, en la misma carpeta o en obb/).
std::vector<std::filesystem::path> FindObbsNear(const std::filesystem::path& apk);

struct Source {
  std::filesystem::path apk;
  std::vector<std::filesystem::path> obbs;  // solo si el APK no trae archiveObbCache
};

struct Result {
  bool ok = false;
  std::string error;                  // si !ok
  std::vector<std::string> warnings;  // version distinta, hashes desconocidos...
  bool version_ok = false;            // libZombieWood.so con el SHA-256 de la 1.5.3
  std::string lib_sha256;
  std::vector<std::string> files;     // rutas relativas a dest escritas
};

// Instala en `dest`. Bloqueante: llamar desde un hilo propio si hay UI. Se puede cancelar con
// progress.cancel (devuelve !ok con error "cancelado").
Result Install(const Source& src, const std::filesystem::path& dest, Progress& progress);

// ¿Hay una instalacion utilizable en `dest`? (libZombieWood.so + OBB principal)
bool IsInstalled(const std::filesystem::path& dest);

// SHA-256 en hex (minusculas) de un fichero; "" si no se puede leer.
std::string Sha256File(const std::filesystem::path& p);

}  // namespace zw::installer
