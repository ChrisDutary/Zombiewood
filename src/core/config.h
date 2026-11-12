#pragma once
#include <nlohmann/json.hpp>

namespace zw {
// Lee <Home>/zombiewood.json. Si no existe o es invalido devuelve un objeto vacio.
nlohmann::json LoadConfig();
// Helper con valor por defecto.
template <class T>
T ConfigGet(const nlohmann::json& cfg, const char* key, T def) {
  return cfg.is_object() && cfg.contains(key) ? cfg.value(key, def) : def;
}
}  // namespace zw
