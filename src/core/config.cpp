#include "core/config.h"

#include <fstream>

#include "core/log.h"
#include "core/paths.h"

namespace zw {
nlohmann::json LoadConfig() {
  auto p = paths::ConfigFile();
  std::ifstream f(p);
  if (!f) {
    log::Info("sin config ({}), usando valores por defecto", p.string());
    return nlohmann::json::object();
  }
  auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    log::Warn("config invalida: {}", p.string());
    return nlohmann::json::object();
  }
  log::Info("config cargada: {}", p.string());
  return j;
}
}  // namespace zw
