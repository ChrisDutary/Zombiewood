#pragma once
#include <filesystem>

namespace zw::paths {
// Carpeta del ejecutable.
std::filesystem::path ExeDir();
// Carpeta de datos/logs/config: variable de entorno ZOMBIEWOOD_HOME si existe, si no ExeDir().
std::filesystem::path Home();
std::filesystem::path LogsDir();
std::filesystem::path ConfigFile();  // <Home>/zombiewood.json
}  // namespace zw::paths
