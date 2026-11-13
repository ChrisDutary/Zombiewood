// Input del port (T8): raton, teclado y mando -> eventos tactiles del juego (ANALYSIS §9).
// El juego no tiene soporte nativo de mando/teclado: todo se traduce a toques virtuales sobre los
// controles del HUD (stick izquierdo = mover, stick derecho = apuntar/disparar, botones) y a los dos
// keycodes Android que entiende (BACK = 4, MENU = 82, en key-up).
//
// Mapa configurable en zombiewood.json, objeto "input" (se fusiona sobre los valores por defecto;
// ver DefaultConfig() en input.cpp y docs/BUILD.md). Coordenadas normalizadas: x en [0,1] del ancho,
// y en [0,1] del alto de la resolucion interna; "radius" en fracciones del alto.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zw::input {

// action: 1 = down, 2 = move, 0 = up (convencion de Game.nativeOnTouch). x, y en pixeles internos.
using TouchFn = std::function<void(int action, int x, int y, int id)>;
using KeyFn = std::function<void(int android_keycode)>;  // se entrega en key-up

inline constexpr int kMousePointer = 0, kMovePointer = 1, kAimPointer = 2, kFirstButtonPointer = 3;

class Mapper {
 public:
  static nlohmann::json DefaultConfig();
  // `user` = objeto "input" de zombiewood.json (o null). Los nombres de tecla son los de SDL
  // (SDL_GetScancodeFromName: "W", "Space", "Left Shift"...); los de mando, los de SDL_GameController
  // ("a", "leftshoulder", "righttrigger"...).
  void Configure(const nlohmann::json& user);
  void SetScreen(int w, int h);

  // Eventos crudos (se llaman desde el hilo del juego, entre frames).
  void Key(int sdl_scancode, bool down);
  void PadButton(int sdl_button, bool down);
  void PadAxis(int sdl_axis, float value);                 // -1..1 (gatillos 0..1)
  void MouseButton(int sdl_button, bool down, int x, int y);  // x, y en pixeles internos
  void MouseMove(int x, int y);

  // Emite los toques/teclas pendientes. Llamar una vez por frame.
  void Update(const TouchFn& touch, const KeyFn& key);
  // Suelta todo lo pulsado (pausa / perdida de foco).
  void ReleaseAll(const TouchFn& touch);

 private:
  struct Binding {
    std::vector<int> keys, pad_buttons, pad_triggers;
  };
  struct Stick {
    float x = 0, y = 0, radius = 0.1f;
    int key_up = -1, key_down = -1, key_left = -1, key_right = -1;
    int axis_x = -1, axis_y = -1;
    int pointer = 0;
    bool active = false;
    int last_x = 0, last_y = 0;
  };
  struct Button {
    std::string name;
    float x = 0, y = 0;
    Binding bind;
    int pointer = 0;
    bool active = false;
  };
  struct AndroidKey {
    int code = 0;
    Binding bind;
    bool was_down = false;
  };

  static Binding ParseBinding(const nlohmann::json& j);
  static void ParseStick(const nlohmann::json& j, Stick& s);
  bool Pressed(const Binding& b) const;
  bool KeyDown(int sc) const { return sc >= 0 && sc < 512 && keys_[sc]; }
  void UpdateStick(Stick& s, float vx, float vy, const TouchFn& touch);

  int w_ = 1280, h_ = 720;
  bool keys_[512] = {};
  bool pad_buttons_[32] = {};
  float pad_axes_[8] = {};
  float deadzone_ = 0.25f, trigger_threshold_ = 0.4f;
  bool mouse_touch_ = true;
  int mouse_aim_button_ = 3;  // SDL_BUTTON_RIGHT; 0 = desactivado
  float player_x_ = 0.5f, player_y_ = 0.5f;
  Stick move_, aim_;
  std::vector<Button> buttons_;
  std::vector<AndroidKey> android_keys_;
  // raton
  bool mouse_left_ = false, mouse_aim_ = false;
  int mouse_x_ = 0, mouse_y_ = 0;
  struct MouseEv {
    int action, x, y;
  };
  std::vector<MouseEv> mouse_queue_;
};

}  // namespace zw::input
