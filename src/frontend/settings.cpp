#include "frontend/settings.h"

#include <algorithm>
#include <cstdlib>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "frontend/game.h"

namespace zw {
namespace fs = std::filesystem;

Settings Settings::Load(const GameOptions* opt) {
  Settings s;
  const auto cfg = LoadConfig();
  const auto home = paths::Home();
  auto str = [&](const char* k) { return cfg.contains(k) && cfg[k].is_string() ? cfg[k].get<std::string>() : std::string(); };
  std::error_code ec;
  // game_dir: carpeta con lib/libZombieWood.so, obb/*.obb, res/raw/*.mp4 y assets/. Por defecto la del instalador
  // (<home>/data); compatibilidad: si no hay nada ahi pero <home>/lib existe, <home>.
  if (!str("game_dir").empty()) s.game_dir = PathFromUtf8(str("game_dir"), home);
  else if (!fs::exists(paths::DefaultGameDir() / "lib", ec) && fs::exists(home / "lib" / "libZombieWood.so", ec)) s.game_dir = home;
  else s.game_dir = paths::DefaultGameDir();
  s.lib = (opt && !opt->lib.empty()) ? opt->lib : !str("lib").empty() ? PathFromUtf8(str("lib"), home) : s.game_dir / "lib" / "libZombieWood.so";
  s.screens_dir = str("screens_dir").empty() ? home / "screens" : PathFromUtf8(str("screens_dir"), home);
  if (!str("assets_dir").empty()) s.assets_dir = PathFromUtf8(str("assets_dir"), home);
  else s.assets_dir = fs::exists(s.game_dir / "assets", ec) ? s.game_dir / "assets" : s.game_dir;
  s.videos_dir = str("videos_dir").empty() ? s.game_dir / "res" / "raw" : PathFromUtf8(str("videos_dir"), home);
  s.vfs = hle::libc::DefaultVfsConfig();
  const bool obb_in_cfg = cfg.contains("vfs") && cfg["vfs"].is_object() && cfg["vfs"].contains("obb_dir");
  if (!obb_in_cfg) s.vfs.obb_dir = s.game_dir / "obb";

  s.backend = ConfigGet<std::string>(cfg, "backend", "d3d11");
  s.win_w = ConfigGet(cfg, "width", 1280);
  s.win_h = ConfigGet(cfg, "height", 720);
  s.internal_w = ConfigGet(cfg, "internal_width", 1280);
  s.internal_h = ConfigGet(cfg, "internal_height", 720);
  s.follow_window = ConfigGet(cfg, "internal_follows_window", false);
  s.vsync = ConfigGet(cfg, "vsync", true);
  s.fullscreen = ConfigGet(cfg, "fullscreen", false);
  s.fps_limit = ConfigGet(cfg, "fps_limit", 60);
  s.show_fps = ConfigGet(cfg, "show_fps", false);
  s.cursor = ConfigGet<std::string>(cfg, "cursor", "zombie");
  if (s.cursor != "zombie" && s.cursor != "system") s.cursor = "zombie";
  s.cursor_size = std::clamp(ConfigGet(cfg, "cursor_size", 48), 16, 128);
  s.menu_bar = ConfigGet(cfg, "menu_bar", false);
  s.pause_on_focus_loss = ConfigGet(cfg, "pause_on_focus_loss", false);
  s.linear_filter = ConfigGet(cfg, "linear_filter", true);
  s.volume = std::clamp(ConfigGet(cfg, "volume", 1.0f), 0.0f, 1.0f);
  s.video_volume = std::clamp(ConfigGet(cfg, "video_volume", 1.0f), 0.0f, 1.0f);
  s.mute = ConfigGet(cfg, "mute", false);
  s.pad_driver = ConfigGet<std::string>(cfg, "pad_driver", "auto");
  if (s.pad_driver != "auto" && s.pad_driver != "compat") s.pad_driver = "auto";
  s.audio_latency = ConfigGet<std::string>(cfg, "audio_latency", "normal");
  if (s.audio_latency != "low" && s.audio_latency != "normal" && s.audio_latency != "safe") s.audio_latency = "normal";
  s.skip_videos = ConfigGet(cfg, "skip_videos", false);
  s.play_logo = ConfigGet(cfg, "play_logo", true);
  s.language = ConfigGet(cfg, "language", 0);
  s.backups_keep = std::max(1, ConfigGet(cfg, "backups_keep", 20));
  if (cfg.contains("input")) s.input = cfg["input"];
  if (opt && opt->uncapped) {
    s.cli_uncapped = true;
    s.vsync = false;
    s.fps_limit = 0;
  }
  s.internal_w = std::clamp(s.internal_w, 320, 4096);
  s.internal_h = std::clamp(s.internal_h, 240, 4096);
  s.win_w = std::clamp(s.win_w, 320, 16384);
  s.win_h = std::clamp(s.win_h, 240, 16384);
  return s;
}

void Settings::MergeInto(nlohmann::json& cfg) const {
  if (!cfg.is_object()) cfg = nlohmann::json::object();
  std::error_code ec;
  const auto home = paths::Home();
  // game_dir relativo a <home> si cuelga de ella (carpeta portable).
  const auto rel = fs::relative(game_dir, home, ec);
  if (!ec && !rel.empty() && rel.native()[0] != L'.') cfg["game_dir"] = PathToUtf8(rel);
  else cfg["game_dir"] = PathToUtf8(game_dir);
  cfg["backend"] = backend;
  cfg["width"] = win_w;
  cfg["height"] = win_h;
  cfg["fullscreen"] = fullscreen;
  cfg["internal_width"] = internal_w;
  cfg["internal_height"] = internal_h;
  cfg["internal_follows_window"] = follow_window;
  cfg["linear_filter"] = linear_filter;
  if (!cli_uncapped) {
    cfg["vsync"] = vsync;
    cfg["fps_limit"] = fps_limit;
  }
  cfg["show_fps"] = show_fps;
  cfg["cursor"] = cursor;
  cfg["cursor_size"] = cursor_size;
  cfg["menu_bar"] = menu_bar;
  cfg["pause_on_focus_loss"] = pause_on_focus_loss;
  cfg["volume"] = volume;
  cfg["video_volume"] = video_volume;
  cfg["mute"] = mute;
  cfg["audio_latency"] = audio_latency;
  cfg["pad_driver"] = pad_driver;
  cfg["skip_videos"] = skip_videos;
  cfg["play_logo"] = play_logo;
  cfg["language"] = language;
  cfg["backups_keep"] = backups_keep;
  if (!input.is_null()) cfg["input"] = input;
}

bool Settings::Save() const {
  auto cfg = LoadConfig();
  MergeInto(cfg);
  return SaveConfig(cfg);
}

void Settings::ApplyBackendEnv() const {
  // ANGLE lee ANGLE_DEFAULT_PLATFORM al crear el EGLDisplay por defecto (Display.cpp): d3d11, d3d9, gl, vulkan...
  // No sobreescribir si el usuario ya la puso en el entorno.
  if (const char* v = std::getenv("ANGLE_DEFAULT_PLATFORM"); v && *v) {
    log::Info("backend: ANGLE_DEFAULT_PLATFORM={} (del entorno)", v);
    return;
  }
  const std::string b = backend.empty() ? "d3d11" : backend;
  _putenv_s("ANGLE_DEFAULT_PLATFORM", b.c_str());
  log::Info("backend: ANGLE {}", b);
}

}  // namespace zw
