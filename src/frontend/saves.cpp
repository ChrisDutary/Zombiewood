#include "frontend/saves.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <vector>

#include "core/log.h"

namespace zw::saves {
namespace fs = std::filesystem;
namespace {

std::string Now() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
  return buf;
}

std::string Rel(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

}  // namespace

void Backup::Start(const fs::path& saves_dir, const fs::path& backup_dir, int keep) {
  Stop();
  saves_dir_ = saves_dir;
  backup_dir_ = backup_dir;
  keep_ = std::max(1, keep);
  std::error_code ec;
  fs::create_directories(backup_dir_ / "shadow", ec);
  {
    std::lock_guard lk(mu_);
    files_.clear();
    // Estado inicial: el shadow refleja lo que hay ahora; copia "inicio" de todo.
    const auto set = Snapshot("inicio");
    if (!set.empty()) log::Info("saves: copia de seguridad al iniciar en {}", set.string());
    for (auto it = fs::recursive_directory_iterator(saves_dir_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const fs::path rel = fs::relative(it->path(), saves_dir_, ec);
      State st;
      st.seen = {it->file_size(ec), it->last_write_time(ec).time_since_epoch().count()};
      fs::create_directories((backup_dir_ / "shadow" / rel).parent_path(), ec);
      if (fs::copy_file(it->path(), backup_dir_ / "shadow" / rel, fs::copy_options::overwrite_existing, ec)) {
        st.shadow = st.seen;
        st.has_shadow = true;
      }
      files_[Rel(rel)] = st;
    }
    Prune();
  }
  stop_ = false;
  th_ = std::thread([this] {
    while (!stop_) {
      for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (stop_) break;
      Poll();
    }
  });
  log::Info("saves: vigilando {} -> {} (se conservan {} conjuntos)", saves_dir_.string(), backup_dir_.string(), keep_);
}

void Backup::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
}

void Backup::Poll() {
  std::lock_guard lk(mu_);
  PollLocked();
}

fs::path Backup::NewSetDir(const std::string& tag) {
  fs::path dir = backup_dir_ / (tag.empty() ? Now() : Now() + "_" + tag);
  std::error_code ec;
  for (int i = 2; fs::exists(dir, ec); ++i) dir = backup_dir_ / std::format("{}_{}{}", Now(), tag.empty() ? "" : tag + "_", i);
  fs::create_directories(dir, ec);
  ++sets_;
  return dir;
}

void Backup::PollLocked() {
  std::error_code ec;
  if (!fs::is_directory(saves_dir_, ec)) return;
  fs::path set;  // conjunto de este sondeo (se crea solo si hace falta)
  for (auto it = fs::recursive_directory_iterator(saves_dir_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const fs::path rel = fs::relative(it->path(), saves_dir_, ec);
    const Stamp now{it->file_size(ec), it->last_write_time(ec).time_since_epoch().count()};
    State& st = files_[Rel(rel)];
    if (!(now == st.seen)) {
      st.seen = now;  // cambiando: esperar a que se estabilice
      continue;
    }
    if (st.has_shadow && st.shadow == now) continue;
    // Estable y distinto del shadow: el shadow es la version previa -> al conjunto de copias.
    const fs::path shadow = backup_dir_ / "shadow" / rel;
    if (st.has_shadow && fs::exists(shadow, ec)) {
      if (set.empty()) set = NewSetDir("");
      fs::create_directories((set / rel).parent_path(), ec);
      fs::rename(shadow, set / rel, ec);
      if (ec) fs::copy_file(shadow, set / rel, fs::copy_options::overwrite_existing, ec);
      log::Info("saves: {} sobrescrito por el juego; version anterior guardada en {}", Rel(rel), set.filename().string());
    }
    fs::create_directories(shadow.parent_path(), ec);
    if (fs::copy_file(it->path(), shadow, fs::copy_options::overwrite_existing, ec)) {
      st.shadow = now;
      st.has_shadow = true;
    }
  }
  if (!set.empty()) Prune();
}

fs::path Backup::Snapshot(const std::string& tag) {
  std::error_code ec;
  if (!fs::is_directory(saves_dir_, ec) || fs::is_empty(saves_dir_, ec)) return {};
  const fs::path set = NewSetDir(tag);
  fs::copy(saves_dir_, set, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) log::Warn("saves: copia {} incompleta: {}", set.string(), ec.message());
  return set;
}

void Backup::Prune() {
  std::error_code ec;
  std::vector<fs::path> sets;
  for (const auto& de : fs::directory_iterator(backup_dir_, ec))
    if (de.is_directory(ec) && de.path().filename() != "shadow") sets.push_back(de.path());
  std::sort(sets.begin(), sets.end());  // nombres = fecha_hora: orden cronologico
  while (static_cast<int>(sets.size()) > keep_) {
    fs::remove_all(sets.front(), ec);
    sets.erase(sets.begin());
  }
}

}  // namespace zw::saves
