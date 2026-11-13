// Configuracion del port (T9): modelo de zombiewood.json que lee el arranque y edita el menu ImGui.
// Las claves desconocidas del json del usuario se conservan al guardar.
#pragma once
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "hle/libc/libc.h"

namespace zw {

struct GameOptions;

struct Settings {
  // --- datos ---
  std::filesystem::path game_dir;  // lib/, obb/, res/raw/, assets/ (instalador: <home>/data)
  std::filesystem::path lib, screens_dir, assets_dir, videos_dir;
  hle::libc::VfsConfig vfs;
  // --- graficos ---
  std::string backend = "d3d11";  // ANGLE: "d3d11" | "gl" (OpenGL de escritorio) | "vulkan" | "d3d9" (necesita reiniciar)
  int win_w = 1280, win_h = 720;
  int internal_w = 1280, internal_h = 720;
  bool follow_window = false;  // resolucion interna = tamano de la ventana (resize al juego)
  bool vsync = true, fullscreen = false, pause_on_focus_loss = false, linear_filter = true;
  int fps_limit = 60;
  bool show_fps = false;
  std::string cursor = "zombie";  // "zombie" (incrustado en el exe, si existe) | "system" (cursor de Windows)
  int cursor_size = 48;           // px; se usa el tamano incrustado mas cercano (32/48/64/96)
  bool menu_bar = false;  // barra de menu visible al arrancar (F1 la alterna)
  // --- audio ---
  float volume = 1.0f;        // 0..1, audio del juego (Vox -> AudioTrack)
  float video_volume = 1.0f;  // 0..1, audio de los videos
  bool mute = false;
  // --- videos ---
  bool skip_videos = false;
  bool play_logo = true;  // logo_hd.mp4 al arrancar (splash Java original)
  // --- juego ---
  int language = 0;
  int backups_keep = 20;  // conjuntos de copias de seguridad de saves a conservar
  nlohmann::json input;   // objeto "input" (ver input.h)
  bool cli_uncapped = false;  // --uncapped: vsync/fps_limit vienen de la linea de comandos, no se guardan

  // Lee <home>/zombiewood.json (+ opciones de linea de comandos).
  static Settings Load(const GameOptions* opt = nullptr);
  // Vuelca los campos editables sobre `cfg` (json original, conserva claves desconocidas).
  void MergeInto(nlohmann::json& cfg) const;
  // LoadConfig + MergeInto + SaveConfig.
  bool Save() const;
  // Variable de entorno de ANGLE para elegir backend; llamar antes de crear la ventana.
  void ApplyBackendEnv() const;
};

}  // namespace zw
