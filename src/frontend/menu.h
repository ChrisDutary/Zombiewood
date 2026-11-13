// Menu ImGui del port (T9), estilo Ship of Harkinian: barra superior que se muestra/oculta con F1.
// Vive en el hilo GL (el del contexto): se dibuja sobre la imagen ya escalada, justo antes del swap.
#pragma once
#include <functional>
#include <string>

#include <SDL.h>

struct ImGuiContext;

namespace zw {

struct Settings;
namespace input { class Mapper; }

struct MenuStats {
  float fps = 0, step_ms = 0;
  long long frame = 0;
  int internal_w = 0, internal_h = 0, window_w = 0, window_h = 0;
  bool paused = false;
  std::string renderer;  // GL_RENDERER
};

// Acciones que el menu pide al hilo GL / UI.
struct MenuActions {
  std::function<void()> apply_vsync;           // SDL_GL_SetSwapInterval segun settings
  std::function<void(bool)> set_fullscreen;    // se ejecuta en el hilo UI
  std::function<void()> apply_resolution;      // internal_w/h / follow_window cambiados
  std::function<void()> apply_audio;           // volume / mute cambiados
  std::function<void()> apply_input;           // settings.input cambiado
  std::function<void(bool)> set_paused;
  std::function<void()> screenshot;
  std::function<void()> backup_now;
  std::function<void()> quit;
};

class Menu {
 public:
  void Init(SDL_Window* win, Settings& s, input::Mapper& mapper, MenuActions actions);
  void Shutdown();
  bool initialized() const { return ctx_ != nullptr; }

  void Toggle();
  // Abre una ventana del menu por nombre ("graphics", "audio", "controls", "about"); para pruebas/capturas.
  void Open(const std::string& which);
  // Empieza a esperar la tecla/boton para un control ("move_stick.up", "buttons.0"...); para pruebas.
  void StartCapture(const std::string& target, bool add = false);
  bool bar_visible() const { return bar_; }
  // ¿Hay que dibujar algo este frame? (barra, ventanas o contador de FPS)
  bool Active() const;
  // Entrega un evento SDL a ImGui (solo si la barra esta visible). Devuelve true si el menu se lo queda
  // (no debe llegar al juego): captura de tecla para remapear, o ImGui quiere raton/teclado.
  bool ProcessEvent(const SDL_Event& e);
  // Dibuja el menu en el framebuffer 0 (drawable dw x dh). Llamar con el contexto GL actual.
  void Draw(int dw, int dh, const MenuStats& st);

 private:
  void DrawMenuBar(const MenuStats& st);
  void DrawGraphics();
  void DrawAudio();
  void DrawControls();
  void DrawAbout(const MenuStats& st);
  void DrawFps(const MenuStats& st);
  void Changed();  // guarda zombiewood.json

  SDL_Window* win_ = nullptr;
  Settings* s_ = nullptr;
  input::Mapper* mapper_ = nullptr;
  MenuActions act_;
  ImGuiContext* ctx_ = nullptr;
  bool bar_ = false;
  bool show_graphics_ = false, show_audio_ = false, show_controls_ = false, show_about_ = false;
  // Remapeo: ruta json del destino ("move_stick/keys/up", "buttons/2", "android_keys/0") y modo.
  std::string capture_;
  bool capture_add_ = false;
  std::string status_;
  double status_until_ = 0;
};

}  // namespace zw
