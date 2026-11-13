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
  auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
  if (j.is_discarded() || !j.is_object()) {
    log::Warn("config invalida: {}", p.string());
    return nlohmann::json::object();
  }
  log::Info("config cargada: {}", p.string());
  return j;
}

bool SaveConfig(const nlohmann::json& cfg) {
  const auto p = paths::ConfigFile();
  auto tmp = p;
  tmp += ".tmp";
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      log::Error("no se puede escribir {}", tmp.string());
      return false;
    }
    f << cfg.dump(2) << "\n";
    if (!f) return false;
  }
  if (std::filesystem::exists(p, ec)) {
    auto bak = p;
    bak += ".bak";
    std::filesystem::copy_file(p, bak, std::filesystem::copy_options::overwrite_existing, ec);
  }
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    log::Error("no se puede reemplazar {}: {}", p.string(), ec.message());
    return false;
  }
  log::Info("config guardada: {}", p.string());
  return true;
}

std::filesystem::path PathFromUtf8(const std::string& s, const std::filesystem::path& base) {
  std::filesystem::path p(std::u8string(s.begin(), s.end()));
  return p.is_absolute() ? p : base / p;
}

std::string PathToUtf8(const std::filesystem::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}
}  // namespace zw
