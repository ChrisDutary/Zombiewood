#include "frontend/input.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace zw::input {

// Posiciones medidas sobre capturas del HUD a 1280x720 (sticks: centros (170,590) y (1108,590), radio ~82 px;
// botiquin "USE" (1195,390); arma (1150,70); pausa (62,42); boton verde ">>" de dialogos (1095,670)).
nlohmann::json Mapper::DefaultConfig() {
  return nlohmann::json::parse(R"json({
    "deadzone": 0.25,
    "trigger_threshold": 0.4,
    "mouse_touch": true,
    "mouse_aim_button": "right",
    "player_pos": [0.5, 0.45],
    "move_stick": { "x": 0.133, "y": 0.82, "radius": 0.105,
                    "keys": { "up": "W", "down": "S", "left": "A", "right": "D" }, "pad_stick": "left" },
    "aim_stick":  { "x": 0.866, "y": 0.82, "radius": 0.105,
                    "keys": { "up": "Up", "down": "Down", "left": "Left", "right": "Right" }, "pad_stick": "right" },
    "buttons": [
      { "name": "botiquin", "x": 0.934, "y": 0.542, "keys": ["E", "F"],     "pad": ["y"] },
      { "name": "arma",     "x": 0.900, "y": 0.100, "keys": ["Q", "R"],     "pad": ["rightshoulder", "x"] },
      { "name": "pausa",    "x": 0.048, "y": 0.058, "keys": ["P", "Return"], "pad": ["start"] },
      { "name": "aceptar",  "x": 0.855, "y": 0.930, "keys": ["Space"],      "pad": ["a"] }
    ],
    "android_keys": [
      { "code": 4,  "keys": ["Escape", "Backspace"], "pad": ["back", "b"] },
      { "code": 82, "keys": ["Tab"], "pad": ["guide"] }
    ]
  })json");
}

namespace {
int Scancode(const std::string& name) {
  const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
  if (sc == SDL_SCANCODE_UNKNOWN) log::Warn("[input] tecla desconocida en la config: \"{}\"", name);
  return sc == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(sc);
}
float Num(const nlohmann::json& j, const char* k, float def) {
  return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<float>() : def;
}
std::string Str(const nlohmann::json& j, const char* k, const std::string& def = {}) {
  return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : def;
}
}  // namespace

Mapper::Binding Mapper::ParseBinding(const nlohmann::json& j) {
  Binding b;
  if (j.contains("keys") && j["keys"].is_array())
    for (const auto& k : j["keys"])
      if (k.is_string())
        if (int sc = Scancode(k.get<std::string>()); sc >= 0) b.keys.push_back(sc);
  if (j.contains("pad") && j["pad"].is_array())
    for (const auto& p : j["pad"]) {
      if (!p.is_string()) continue;
      const std::string n = p.get<std::string>();
      if (auto btn = SDL_GameControllerGetButtonFromString(n.c_str()); btn != SDL_CONTROLLER_BUTTON_INVALID) {
        b.pad_buttons.push_back(btn);
      } else if (auto ax = SDL_GameControllerGetAxisFromString(n.c_str()); ax != SDL_CONTROLLER_AXIS_INVALID) {
        b.pad_triggers.push_back(ax);
      } else {
        log::Warn("[input] boton de mando desconocido en la config: \"{}\"", n);
      }
    }
  return b;
}

void Mapper::ParseStick(const nlohmann::json& j, Stick& s) {
  s.x = Num(j, "x", s.x);
  s.y = Num(j, "y", s.y);
  s.radius = Num(j, "radius", s.radius);
  if (j.contains("keys") && j["keys"].is_object()) {
    const auto& k = j["keys"];
    auto get = [&](const char* n) { return k.contains(n) && k[n].is_string() ? Scancode(k[n].get<std::string>()) : -1; };
    s.key_up = get("up");
    s.key_down = get("down");
    s.key_left = get("left");
    s.key_right = get("right");
  }
  const std::string ps = Str(j, "pad_stick");
  if (ps == "left") {
    s.axis_x = SDL_CONTROLLER_AXIS_LEFTX;
    s.axis_y = SDL_CONTROLLER_AXIS_LEFTY;
  } else if (ps == "right") {
    s.axis_x = SDL_CONTROLLER_AXIS_RIGHTX;
    s.axis_y = SDL_CONTROLLER_AXIS_RIGHTY;
  } else {
    s.axis_x = s.axis_y = -1;
  }
}

void Mapper::Configure(const nlohmann::json& user) {
  nlohmann::json cfg = DefaultConfig();
  if (user.is_object()) cfg.merge_patch(user);
  deadzone_ = Num(cfg, "deadzone", 0.25f);
  trigger_threshold_ = Num(cfg, "trigger_threshold", 0.4f);
  mouse_touch_ = cfg.value("mouse_touch", true);
  const std::string mab = Str(cfg, "mouse_aim_button", "right");
  mouse_aim_button_ = mab == "right" ? SDL_BUTTON_RIGHT : mab == "middle" ? SDL_BUTTON_MIDDLE : 0;
  if (cfg.contains("player_pos") && cfg["player_pos"].is_array() && cfg["player_pos"].size() == 2) {
    player_x_ = cfg["player_pos"][0].get<float>();
    player_y_ = cfg["player_pos"][1].get<float>();
  }
  move_ = {};
  aim_ = {};
  move_.pointer = kMovePointer;
  aim_.pointer = kAimPointer;
  if (cfg.contains("move_stick")) ParseStick(cfg["move_stick"], move_);
  if (cfg.contains("aim_stick")) ParseStick(cfg["aim_stick"], aim_);
  buttons_.clear();
  if (cfg.contains("buttons") && cfg["buttons"].is_array()) {
    int ptr = kFirstButtonPointer;
    for (const auto& j : cfg["buttons"]) {
      if (!j.is_object()) continue;
      Button b;
      b.name = Str(j, "name", "boton");
      b.x = Num(j, "x", 0.5f);
      b.y = Num(j, "y", 0.5f);
      b.bind = ParseBinding(j);
      b.pointer = ptr++;
      buttons_.push_back(std::move(b));
    }
  }
  android_keys_.clear();
  if (cfg.contains("android_keys") && cfg["android_keys"].is_array())
    for (const auto& j : cfg["android_keys"]) {
      if (!j.is_object() || !j.contains("code")) continue;
      AndroidKey k;
      k.code = j["code"].get<int>();
      k.bind = ParseBinding(j);
      android_keys_.push_back(std::move(k));
    }
  log::Info("[input] mapa: mover ({:.3f},{:.3f}) r {:.3f}; apuntar ({:.3f},{:.3f}) r {:.3f}; {} botones, {} teclas Android", move_.x,
            move_.y, move_.radius, aim_.x, aim_.y, aim_.radius, buttons_.size(), android_keys_.size());
}

void Mapper::SetScreen(int w, int h) {
  w_ = w;
  h_ = h;
}

void Mapper::Key(int sc, bool down) {
  if (sc < 0 || sc >= 512) return;
  keys_[sc] = down;
  if (down) key_hit_[sc] = true;
}
void Mapper::PadButton(int b, bool down) {
  if (b < 0 || b >= 32) return;
  pad_buttons_[b] = down;
  if (down) pad_hit_[b] = true;
  any_pad_input_ = true;
}
void Mapper::PadAxis(int a, float v) {
  if (a < 0 || a >= 8) return;
  pad_axes_[a] = v;
  if (std::fabs(v) > 0.2f) any_pad_input_ = true;
}

void Mapper::MouseButton(int button, bool down, int x, int y) {
  mouse_x_ = x;
  mouse_y_ = y;
  if (button == SDL_BUTTON_LEFT && mouse_touch_) {
    if (down != mouse_left_) {
      mouse_left_ = down;
      mouse_queue_.push_back({down ? 1 : 0, x, y});
    }
  } else if (mouse_aim_button_ && button == mouse_aim_button_) {
    mouse_aim_ = down;
  }
}
void Mapper::MouseMove(int x, int y) {
  mouse_x_ = x;
  mouse_y_ = y;
  if (mouse_left_) {
    // Varios movimientos entre dos frames se funden en uno.
    if (!mouse_queue_.empty() && mouse_queue_.back().action == 2) mouse_queue_.back() = {2, x, y};
    else mouse_queue_.push_back({2, x, y});
  }
}

bool Mapper::Pressed(const Binding& b) const {
  for (int k : b.keys)
    if (KeyDown(k)) return true;
  for (int p : b.pad_buttons)
    if (p >= 0 && p < 32 && pad_buttons_[p]) return true;
  for (int a : b.pad_triggers)
    if (a >= 0 && a < 8 && pad_axes_[a] > trigger_threshold_) return true;
  return false;
}

bool Mapper::Hit(const Binding& b) const {
  for (int k : b.keys)
    if (k >= 0 && k < 512 && key_hit_[k]) return true;
  for (int p : b.pad_buttons)
    if (p >= 0 && p < 32 && pad_hit_[p]) return true;
  return false;
}

void Mapper::UpdateStick(Stick& s, float vx, float vy, const TouchFn& touch) {
  const float mag = std::sqrt(vx * vx + vy * vy);
  const int cx = static_cast<int>(s.x * w_), cy = static_cast<int>(s.y * h_);
  if (mag < 1e-3f) {
    if (s.active) {
      touch(0, s.last_x, s.last_y, s.pointer);
      s.active = false;
    }
    return;
  }
  if (mag > 1.f) {
    vx /= mag;
    vy /= mag;
  }
  const float r = s.radius * h_;
  const int tx = std::clamp(static_cast<int>(cx + vx * r), 0, w_ - 1);
  const int ty = std::clamp(static_cast<int>(cy + vy * r), 0, h_ - 1);
  if (!s.active) {
    // El dedo "baja" en el centro del stick y se desplaza en el frame siguiente.
    touch(1, cx, cy, s.pointer);
    s.active = true;
    s.last_x = cx;
    s.last_y = cy;
    return;
  }
  // Se reenvia cada frame: Game.nativeOnTouch limita los "move" por tiempo y puede descartar alguno.
  touch(2, tx, ty, s.pointer);
  s.last_x = tx;
  s.last_y = ty;
}

void Mapper::Update(const TouchFn& touch, const KeyFn& key) {
  // Raton -> puntero 0.
  for (const auto& e : mouse_queue_) touch(e.action, e.x, e.y, kMousePointer);
  mouse_queue_.clear();

  auto stick_vec = [&](const Stick& s, float& vx, float& vy) {
    vx = (KeyDown(s.key_right) ? 1.f : 0.f) - (KeyDown(s.key_left) ? 1.f : 0.f);
    vy = (KeyDown(s.key_down) ? 1.f : 0.f) - (KeyDown(s.key_up) ? 1.f : 0.f);
    if (vx != 0 || vy != 0) {
      const float m = std::sqrt(vx * vx + vy * vy);
      vx /= m;
      vy /= m;
      return;
    }
    if (s.axis_x >= 0) {
      const float ax = pad_axes_[s.axis_x], ay = pad_axes_[s.axis_y];
      const float m = std::sqrt(ax * ax + ay * ay);
      if (m > deadzone_) {
        // Reescalar para que el borde de la zona muerta sea 0.
        const float k = std::min(1.f, (m - deadzone_) / (1.f - deadzone_)) / m;
        vx = ax * k;
        vy = ay * k;
        // Un stick virtual tactil necesita un desplazamiento minimo para registrar direccion.
        const float mm = std::sqrt(vx * vx + vy * vy);
        if (mm < 0.35f && mm > 0) {
          vx *= 0.35f / mm;
          vy *= 0.35f / mm;
        }
      }
    }
  };
  float vx, vy;
  stick_vec(move_, vx, vy);
  UpdateStick(move_, vx, vy, touch);

  stick_vec(aim_, vx, vy);
  if (vx == 0 && vy == 0 && mouse_aim_) {
    // Apuntar con el raton: direccion desde el jugador (centro de pantalla) hacia el cursor.
    const float dx = mouse_x_ - player_x_ * w_, dy = mouse_y_ - player_y_ * h_;
    const float m = std::sqrt(dx * dx + dy * dy);
    if (m > 4.f) {
      vx = dx / m;
      vy = dy / m;
    }
  }
  UpdateStick(aim_, vx, vy, touch);

  for (auto& b : buttons_) {
    // Un toque rapido (pulsar y soltar entre dos frames) cuenta como pulsado durante un frame.
    const bool down = Pressed(b.bind) || (!b.active && Hit(b.bind));
    if (down == b.active) continue;
    b.active = down;
    touch(down ? 1 : 0, static_cast<int>(b.x * w_), static_cast<int>(b.y * h_), b.pointer);
  }
  for (auto& k : android_keys_) {
    const bool held = Pressed(k.bind);
    // El juego actua en key-up: enviar si se solto desde el ultimo frame, o si hubo un toque completo dentro de el.
    if ((k.was_down && !held) || (!k.was_down && !held && Hit(k.bind))) key(k.code);
    k.was_down = held;
  }
  std::fill(std::begin(key_hit_), std::end(key_hit_), false);
  std::fill(std::begin(pad_hit_), std::end(pad_hit_), false);
}

void Mapper::ReleaseAll(const TouchFn& touch) {
  std::fill(std::begin(keys_), std::end(keys_), false);
  std::fill(std::begin(pad_buttons_), std::end(pad_buttons_), false);
  std::fill(std::begin(key_hit_), std::end(key_hit_), false);
  std::fill(std::begin(pad_hit_), std::end(pad_hit_), false);
  std::fill(std::begin(pad_axes_), std::end(pad_axes_), 0.f);
  mouse_queue_.clear();
  if (mouse_left_) touch(0, mouse_x_, mouse_y_, kMousePointer);
  mouse_left_ = mouse_aim_ = false;
  for (Stick* s : {&move_, &aim_})
    if (s->active) {
      touch(0, s->last_x, s->last_y, s->pointer);
      s->active = false;
    }
  for (auto& b : buttons_)
    if (b.active) {
      touch(0, static_cast<int>(b.x * w_), static_cast<int>(b.y * h_), b.pointer);
      b.active = false;
    }
  for (auto& k : android_keys_) k.was_down = false;
}

}  // namespace zw::input
