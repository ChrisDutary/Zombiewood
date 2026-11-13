// Primer arranque (T9): si faltan los datos del juego, pide el APK del usuario (dialogo de Windows),
// lo instala en <home>/data con una pantalla de progreso ImGui en la propia ventana del juego y
// actualiza zombiewood.json ("game_dir").
#pragma once
#include <filesystem>
#include <vector>

#include <SDL.h>

namespace zw {

struct Settings;

struct SetupOptions {
  std::filesystem::path install_apk;             // --install=<apk>: sin dialogo de APK
  std::vector<std::filesystem::path> obbs;       // --obb=<fichero> (repetible), para el APK pequeno
  bool force = false;                            // --reinstall: instalar aunque ya haya datos
};

// Devuelve true si al terminar hay datos utilizables (ya estaban o se instalaron). `s` se recarga si se
// instalo algo. Necesita la ventana con su contexto GL actual en este hilo.
bool EnsureGameData(SDL_Window* win, Settings& s, const SetupOptions& o);

// Dialogos nativos (Windows). Vacio = cancelado.
std::filesystem::path PickApkDialog(SDL_Window* owner);
std::vector<std::filesystem::path> PickObbDialog(SDL_Window* owner);

}  // namespace zw
