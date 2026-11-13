#pragma once
#include <filesystem>

namespace zw::paths {
// Carpeta del ejecutable.
std::filesystem::path ExeDir();
// Carpeta de datos/logs/config: variable de entorno ZOMBIEWOOD_HOME si existe, si no ExeDir().
std::filesystem::path Home();
std::filesystem::path LogsDir();
std::filesystem::path ConfigFile();  // <Home>/zombiewood.json
std::filesystem::path DefaultGameDir();  // <Home>/data (destino del instalador)
std::filesystem::path SavesDir();        // <Home>/saves (= /data/data/<pkg> del guest, ver VfsConfig)
// Abre una carpeta en el Explorador (la crea si no existe).
void OpenFolder(const std::filesystem::path& p);
}  // namespace zw::paths
