#pragma once
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace zw {
// Lee <Home>/zombiewood.json. Si no existe o es invalido devuelve un objeto vacio.
nlohmann::json LoadConfig();
// Escribe <Home>/zombiewood.json (indentado, escritura atomica via .tmp; la version anterior queda en
// zombiewood.json.bak). Devuelve false si no se pudo escribir.
bool SaveConfig(const nlohmann::json& cfg);
// Helper con valor por defecto.
template <class T>
T ConfigGet(const nlohmann::json& cfg, const char* key, T def) {
  if (!cfg.is_object() || !cfg.contains(key)) return def;
  try {
    return cfg.value(key, def);
  } catch (const nlohmann::json::exception&) {
    return def;  // tipo incorrecto en el json del usuario
  }
}
// Ruta UTF-8 del json <-> std::filesystem::path. Relativas a `base`.
std::filesystem::path PathFromUtf8(const std::string& s, const std::filesystem::path& base);
std::string PathToUtf8(const std::filesystem::path& p);
}  // namespace zw
