#include "frontend/menu.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "core/log.h"
#include "core/paths.h"
#include "frontend/input.h"
#include "frontend/settings.h"
#include "hle/audio/audio_track.h"
#include "imgui.h"

namespace zw {
namespace {

std::string g_start_backend;
int g_start_language = 0;

struct ResPreset {
  const char* name;
  int w, h;
};
constexpr ResPreset kRes[] = {{"854 x 480 (referencia del HUD)", 854, 480}, {"960 x 540", 960, 540},   {"1280 x 720", 1280, 720},
                              {"1600 x 900", 1600, 900},                   {"1920 x 1080", 1920, 1080}, {"2560 x 1440", 2560, 1440},
                              {"3840 x 2160", 3840, 2160}};
constexpr const char* kLanguages[] = {"English", "Francais", "Deutsch", "Espanol", "Italiano", "Portugues", "Japanese", "Korean", "Chinese", "Russian"};

std::string Join(const nlohmann::json& arr) {
  std::string s;
  if (!arr.is_array()) return s;
  for (const auto& k : arr)
    if (k.is_string()) s += (s.empty() ? "" : ", ") + k.get<std::string>();
  return s;
}

// ¿Hay un mando de PlayStation conectado? (para mostrar Cruz/Circulo/L1... en vez de a/b/leftshoulder).
bool PlayStationPad() {
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (!SDL_IsGameController(i)) continue;
    const auto t = SDL_GameControllerTypeForIndex(i);
    if (t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5) return true;
  }
  return false;
}

std::string PadLabel(const std::string& n, bool ps) {
  if (!ps) return n;
  static const std::pair<const char*, const char*> kPs[] = {
      {"a", "Cruz"}, {"b", "Circulo"}, {"x", "Cuadrado"}, {"y", "Triangulo"}, {"leftshoulder", "L1"},
      {"rightshoulder", "R1"}, {"lefttrigger", "L2"}, {"righttrigger", "R2"}, {"leftstick", "L3"},
      {"rightstick", "R3"}, {"back", "Create"}, {"start", "Options"}, {"guide", "PS"}, {"touchpad", "Touchpad"},
      {"dpup", "Cruceta arriba"}, {"dpdown", "Cruceta abajo"}, {"dpleft", "Cruceta izq."}, {"dpright", "Cruceta der."},
      {"misc1", "Mute"}};
  for (const auto& [k, v] : kPs)
    if (n == k) return v;
  return n;
}

std::string JoinPad(const nlohmann::json& arr, bool ps) {
  std::string s;
  if (!arr.is_array()) return s;
  for (const auto& k : arr)
    if (k.is_string()) s += (s.empty() ? "" : ", ") + PadLabel(k.get<std::string>(), ps);
  return s;
}

// json efectivo del input (defaults + usuario), como lo interpreta el Mapper.
nlohmann::json EffectiveInput(const nlohmann::json& user) {
  nlohmann::json cfg = input::Mapper::DefaultConfig();
  if (user.is_object()) cfg.merge_patch(user);
  return cfg;
}

}  // namespace

void Menu::Init(SDL_Window* win, Settings& s, input::Mapper& mapper, MenuActions actions) {
  win_ = win;
  s_ = &s;
  mapper_ = &mapper;
  act_ = std::move(actions);
  bar_ = s.menu_bar;
  g_start_backend = s.backend;
  g_start_language = s.language;
  IMGUI_CHECKVERSION();
  ctx_ = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  static std::string ini;
  ini = (paths::Home() / "imgui.ini").string();
  io.IniFilename = ini.c_str();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // el cursor lo gestiona el hilo UI
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 4;
  st.FrameRounding = 3;
  st.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.10f, 0.08f, 0.94f);
  ImGui_ImplSDL2_InitForOpenGL(win_, SDL_GL_GetCurrentContext());
  ImGui_ImplOpenGL3_Init("#version 100");
}

void Menu::Shutdown() {
  if (!ctx_) return;
  ImGui::SetCurrentContext(ctx_);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext(ctx_);
  ctx_ = nullptr;
}

void Menu::Toggle() {
  bar_ = !bar_;
  capture_.clear();
  log::Info("menu: barra {}", bar_ ? "visible (F1 para ocultar)" : "oculta");
}

void Menu::StartCapture(const std::string& target, bool add) {
  bar_ = true;
  show_controls_ = true;
  capture_ = target;
  capture_add_ = add;
}

void Menu::Open(const std::string& w) {
  bar_ = true;
  if (w == "graphics") show_graphics_ = true;
  else if (w == "audio") show_audio_ = true;
  else if (w == "controls") show_controls_ = true;
  else if (w == "about") show_about_ = true;
  else if (w == "none") show_graphics_ = show_audio_ = show_controls_ = show_about_ = false;
}

bool Menu::Active() const { return ctx_ && (bar_ || s_->show_fps); }

bool Menu::ProcessEvent(const SDL_Event& e) {
  if (!ctx_ || !bar_) return false;
  ImGui::SetCurrentContext(ctx_);
  // Captura para remapear: la siguiente tecla / boton de mando es la asignacion.
  if (!capture_.empty() && (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN)) {
    if (!wizard_.empty() && e.type == SDL_KEYDOWN) {  // el asistente solo acepta el mando; Esc lo cancela
      if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        wizard_.clear();
        capture_.clear();
        status_ = "Asistente cancelado";
        status_until_ = ImGui::GetTime() + 3;
      }
      return true;
    }
    auto cfg = EffectiveInput(s_->input);
    std::string key, pad;
    if (e.type == SDL_KEYDOWN) key = SDL_GetScancodeName(e.key.keysym.scancode);
    else if (const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button))) pad = n;
    if (key.empty() && pad.empty()) return true;
    // capture_: "move_stick.up" / "aim_stick.left" / "buttons.N" / "android_keys.N"
    const auto dot = capture_.find('.');
    const std::string grp = capture_.substr(0, dot), sub = capture_.substr(dot + 1);
    if (grp == "move_stick" || grp == "aim_stick") {
      if (!key.empty()) cfg[grp]["keys"][sub] = key;
    } else {
      auto& item = cfg[grp][std::stoi(sub)];
      const char* field = key.empty() ? "pad" : "keys";
      const std::string val = key.empty() ? pad : key;
      if (!capture_add_ || !item[field].is_array()) item[field] = nlohmann::json::array();
      if (std::find(item[field].begin(), item[field].end(), val) == item[field].end()) item[field].push_back(val);
    }
    s_->input = cfg;
    if (act_.apply_input) act_.apply_input();
    status_ = std::format("Asignado: {}", key.empty() ? "mando " + pad : key);
    status_until_ = ImGui::GetTime() + 3;
    capture_.clear();
    Changed();
    if (!wizard_.empty()) WizardAdvance();
    return true;
  }
  ImGui_ImplSDL2_ProcessEvent(&e);
  const ImGuiIO& io = ImGui::GetIO();
  switch (e.type) {
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEWHEEL:
      return io.WantCaptureMouse;
    case SDL_MOUSEMOTION:
      return io.WantCaptureMouse && !(e.motion.state & SDL_BUTTON_LMASK);
    case SDL_KEYDOWN:
    case SDL_TEXTINPUT:
      return io.WantCaptureKeyboard || !capture_.empty();
    default:
      return false;  // los "up" siempre llegan al juego (no dejar toques/teclas colgados)
  }
}

void Menu::Changed() {
  if (!s_->Save()) {
    status_ = "No se pudo guardar zombiewood.json";
    status_until_ = ImGui::GetTime() + 5;
  }
}

void Menu::Draw(int dw, int dh, const MenuStats& st) {
  if (!Active()) return;
  ImGui::SetCurrentContext(ctx_);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  if (bar_) {
    DrawMenuBar(st);
    if (show_graphics_) DrawGraphics();
    if (show_audio_) DrawAudio();
    if (show_controls_) DrawControls();
    if (show_about_) DrawAbout(st);
  }
  if (s_->show_fps) DrawFps(st);
  ImGui::Render();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  glViewport(0, 0, dw, dh);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void Menu::DrawMenuBar(const MenuStats& st) {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("Zombiewood")) {
    if (ImGui::MenuItem(st.paused ? "Reanudar" : "Pausar", "")) act_.set_paused(!st.paused);
    if (ImGui::MenuItem("Captura de pantalla", "F12")) act_.screenshot();
    ImGui::Separator();
    if (ImGui::MenuItem("Ocultar menu", "F1")) Toggle();
    if (ImGui::MenuItem("Salir")) act_.quit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Graficos")) {
    ImGui::MenuItem("Opciones de graficos...", nullptr, &show_graphics_);
    if (ImGui::MenuItem("Pantalla completa", "F11", s_->fullscreen)) {
      s_->fullscreen = !s_->fullscreen;
      act_.set_fullscreen(s_->fullscreen);
      Changed();
    }
    if (ImGui::MenuItem("VSync", nullptr, s_->vsync)) {
      s_->vsync = !s_->vsync;
      act_.apply_vsync();
      Changed();
    }
    if (ImGui::MenuItem("Mostrar FPS", nullptr, s_->show_fps)) {
      s_->show_fps = !s_->show_fps;
      Changed();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Audio")) {
    ImGui::MenuItem("Opciones de audio...", nullptr, &show_audio_);
    if (ImGui::MenuItem("Silenciar", nullptr, s_->mute)) {
      s_->mute = !s_->mute;
      act_.apply_audio();
      Changed();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Controles")) {
    ImGui::MenuItem("Remapear controles...", nullptr, &show_controls_);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Carpetas")) {
    if (ImGui::MenuItem("Partidas guardadas (saves)")) paths::OpenFolder(s_->vfs.saves_dir);
    if (ImGui::MenuItem("Copias de seguridad de saves")) paths::OpenFolder(paths::Home() / "saves_backup");
    if (ImGui::MenuItem("Crear copia de seguridad ahora")) {
      act_.backup_now();
      status_ = "Copia de seguridad creada";
      status_until_ = ImGui::GetTime() + 3;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Logs")) paths::OpenFolder(paths::LogsDir());
    if (ImGui::MenuItem("Capturas")) paths::OpenFolder(s_->screens_dir);
    if (ImGui::MenuItem("Datos del juego")) paths::OpenFolder(s_->game_dir);
    if (ImGui::MenuItem("Carpeta del port (zombiewood.json)")) paths::OpenFolder(paths::Home());
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Ayuda")) {
    ImGui::MenuItem("Acerca de / estado", nullptr, &show_about_);
    ImGui::EndMenu();
  }
  // Derecha: estado / fps.
  std::string right = std::format("{:.0f} fps | {}x{}", st.fps, st.internal_w, st.internal_h);
  if (!status_.empty() && ImGui::GetTime() < status_until_) right = status_ + "   " + right;
  const float tw = ImGui::CalcTextSize(right.c_str()).x;
  ImGui::SameLine(ImGui::GetWindowWidth() - tw - 16);
  ImGui::TextDisabled("%s", right.c_str());
  ImGui::EndMainMenuBar();
}

void Menu::DrawGraphics() {
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(40, 50), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Graficos", &show_graphics_)) return ImGui::End();
  // Backend ANGLE
  const char* backends[] = {"d3d11", "gl", "vulkan", "d3d9"};
  const char* backend_names[] = {"Direct3D 11 (recomendado)", "OpenGL (escritorio, via ANGLE)", "Vulkan (via ANGLE)", "Direct3D 9"};
  int bi = 0;
  for (int i = 0; i < 4; ++i)
    if (s_->backend == backends[i]) bi = i;
  if (ImGui::Combo("Backend", &bi, backend_names, 4)) {
    s_->backend = backends[bi];
    Changed();
  }
  if (s_->backend != g_start_backend) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Se aplica al reiniciar el juego.");
  ImGui::Separator();
  // Resolucion interna
  bool follow = s_->follow_window;
  if (ImGui::Checkbox("Resolucion interna = ventana", &follow)) {
    s_->follow_window = follow;
    act_.apply_resolution();
    Changed();
  }
  ImGui::BeginDisabled(s_->follow_window);
  int ri = -1;
  for (int i = 0; i < static_cast<int>(std::size(kRes)); ++i)
    if (kRes[i].w == s_->internal_w && kRes[i].h == s_->internal_h) ri = i;
  const std::string cur = ri >= 0 ? kRes[ri].name : std::format("{} x {} (personalizada)", s_->internal_w, s_->internal_h);
  if (ImGui::BeginCombo("Resolucion interna", cur.c_str())) {
    for (int i = 0; i < static_cast<int>(std::size(kRes)); ++i)
      if (ImGui::Selectable(kRes[i].name, i == ri)) {
        s_->internal_w = kRes[i].w;
        s_->internal_h = kRes[i].h;
        act_.apply_resolution();
        Changed();
      }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (ImGui::Checkbox("Filtrado lineal al escalar", &s_->linear_filter)) Changed();
  ImGui::Separator();
  if (ImGui::Checkbox("VSync", &s_->vsync)) {
    act_.apply_vsync();
    Changed();
  }
  int lim = s_->fps_limit;
  if (ImGui::SliderInt("Limite FPS (0 = libre)", &lim, 0, 240)) s_->fps_limit = lim;
  if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
  bool fs = s_->fullscreen;
  if (ImGui::Checkbox("Pantalla completa (F11)", &fs)) {
    s_->fullscreen = fs;
    act_.set_fullscreen(fs);
    Changed();
  }
  if (ImGui::Checkbox("Mostrar FPS", &s_->show_fps)) Changed();
  if (ImGui::Checkbox("Pausar al perder el foco", &s_->pause_on_focus_loss)) Changed();
  ImGui::Separator();
  bool zcur = s_->cursor == "zombie";
  if (ImGui::Checkbox("Cursor zombi", &zcur)) {
    s_->cursor = zcur ? "zombie" : "system";
    if (act_.apply_cursor) act_.apply_cursor();
    Changed();
  }
  ImGui::BeginDisabled(!zcur);
  static const int kCursorSizes[] = {32, 48, 64, 96};
  static const char* kCursorNames[] = {"Pequeno (32 px)", "Normal (48 px)", "Grande (64 px)", "Muy grande (96 px)"};
  int ci = 1;
  for (int i = 0; i < 4; ++i)
    if (s_->cursor_size == kCursorSizes[i]) ci = i;
  if (ImGui::Combo("Tamano del cursor", &ci, kCursorNames, 4)) {
    s_->cursor_size = kCursorSizes[ci];
    if (act_.apply_cursor) act_.apply_cursor();
    Changed();
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  if (ImGui::Checkbox("Saltar videos", &s_->skip_videos)) Changed();
  ImGui::BeginDisabled(s_->skip_videos);
  if (ImGui::Checkbox("Logo de Gameloft al arrancar", &s_->play_logo)) Changed();
  ImGui::EndDisabled();
  int lang = std::clamp(s_->language, 0, 9);
  if (ImGui::Combo("Idioma", &lang, kLanguages, 10)) {
    s_->language = lang;
    Changed();
  }
  if (s_->language != g_start_language) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "El idioma se aplica al reiniciar.");
  ImGui::End();
}

void Menu::DrawAudio() {
  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(80, 70), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Audio", &show_audio_)) return ImGui::End();
  float v = s_->volume * 100;
  if (ImGui::SliderFloat("Volumen del juego", &v, 0, 100, "%.0f %%")) {
    s_->volume = v / 100;
    act_.apply_audio();
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
  float vv = s_->video_volume * 100;
  if (ImGui::SliderFloat("Volumen de los videos", &vv, 0, 100, "%.0f %%")) s_->video_volume = vv / 100;
  if (ImGui::IsItemDeactivatedAfterEdit()) Changed();
  if (ImGui::Checkbox("Silenciar", &s_->mute)) {
    act_.apply_audio();
    Changed();
  }
  ImGui::Separator();
  static const char* kLatIds[] = {"low", "normal", "safe"};
  static const char* kLatNames[] = {"Baja (80 ms)", "Normal (150 ms)", "Segura (250 ms)"};
  int li = 1;
  for (int i = 0; i < 3; ++i)
    if (s_->audio_latency == kLatIds[i]) li = i;
  if (ImGui::Combo("Latencia de audio", &li, kLatNames, 3)) {
    s_->audio_latency = kLatIds[li];
    Changed();
  }
  ImGui::TextDisabled("Si el audio se corta, usa Segura. Se aplica al reiniciar.");
  ImGui::Text("Cortes de audio en esta sesion: %llu", (unsigned long long)audio::underrun_count());
  ImGui::End();
}

void Menu::DrawControls() {
  ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(60, 50), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Controles", &show_controls_)) return ImGui::End();
  auto cfg = EffectiveInput(s_->input);
  bool changed = false;
  const bool ps = PlayStationPad();
  DrawGamepad(cfg, changed);
  ImGui::Separator();
  auto capture_label = [&](const std::string& id) -> std::string {
    const auto dot = id.find('.');
    if (dot == std::string::npos) return id;
    const std::string grp = id.substr(0, dot);
    const std::size_t i = static_cast<std::size_t>(std::atoi(id.c_str() + dot + 1));
    if (grp == "buttons" && cfg["buttons"].is_array() && i < cfg["buttons"].size())
      return "Boton HUD: " + cfg["buttons"][i].value("name", std::string("?"));
    if (grp == "android_keys" && cfg["android_keys"].is_array() && i < cfg["android_keys"].size()) {
      const int c = cfg["android_keys"][i].value("code", 0);
      return c == 4 ? "Atras (BACK)" : c == 82 ? "Menu (MENU)" : std::format("Tecla Android {}", c);
    }
    return id;
  };
  if (!capture_.empty() && !wizard_.empty())
    ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "Asistente (%zu/%zu): pulsa en el MANDO el boton para \"%s\"  (Esc cancela)",
                       wizard_pos_ + 1, wizard_.size(), capture_label(capture_).c_str());
  else if (!capture_.empty())
    ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "Pulsa una tecla o un boton del mando para \"%s\"...", capture_.c_str());
  else
    ImGui::TextDisabled("\"Cambiar\" sustituye; \"+\" anade otra tecla/boton; \"x\" borra.");
  auto capture_btn = [&](const std::string& id, bool add) {
    ImGui::PushID((id + (add ? "+" : "=")).c_str());
    const bool waiting = capture_ == id && capture_add_ == add;
    if (ImGui::SmallButton(waiting ? "..." : add ? "+" : "Cambiar")) {
      capture_ = waiting ? "" : id;
      capture_add_ = add;
    }
    ImGui::PopID();
  };
  if (ImGui::BeginTable("ctl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Accion", ImGuiTableColumnFlags_WidthFixed, 170);
    ImGui::TableSetupColumn("Teclado");
    ImGui::TableSetupColumn("Mando");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableHeadersRow();
    for (const char* stick : {"move_stick", "aim_stick"}) {
      const bool move = std::string(stick) == "move_stick";
      for (const char* dir : {"up", "down", "left", "right"}) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const char* dn = !std::strcmp(dir, "up") ? "arriba" : !std::strcmp(dir, "down") ? "abajo" : !std::strcmp(dir, "left") ? "izquierda" : "derecha";
        ImGui::Text("%s %s", move ? "Mover" : "Apuntar", dn);
        ImGui::TableNextColumn();
        const auto& k = cfg[stick]["keys"];
        ImGui::TextUnformatted(k.contains(dir) && k[dir].is_string() ? k[dir].get<std::string>().c_str() : "-");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("stick %s", cfg[stick].value("pad_stick", std::string("-")).c_str());
        ImGui::TableNextColumn();
        capture_btn(std::string(stick) + "." + dir, false);
      }
    }
    auto list_rows = [&](const char* grp, auto&& label) {
      auto& arr = cfg[grp];
      if (!arr.is_array()) return;
      for (std::size_t i = 0; i < arr.size(); ++i) {
        auto& item = arr[i];
        const std::string id = std::format("{}.{}", grp, i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label(item).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Join(item["keys"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(JoinPad(item["pad"], ps).c_str());
        ImGui::TableNextColumn();
        capture_btn(id, false);
        ImGui::SameLine();
        capture_btn(id, true);
        ImGui::SameLine();
        ImGui::PushID((id + "x").c_str());
        if (ImGui::SmallButton("x")) {
          item["keys"] = nlohmann::json::array();
          item["pad"] = nlohmann::json::array();
          changed = true;
        }
        ImGui::PopID();
      }
    };
    list_rows("buttons", [](const nlohmann::json& b) { return "Boton HUD: " + b.value("name", std::string("?")); });
    list_rows("android_keys", [](const nlohmann::json& k) {
      const int c = k.value("code", 0);
      return c == 4 ? std::string("Atras (BACK)") : c == 82 ? std::string("Menu (MENU)") : std::format("Tecla Android {}", c);
    });
    ImGui::EndTable();
  }
  ImGui::Separator();
  float dz = cfg.value("deadzone", 0.25f);
  if (ImGui::SliderFloat("Zona muerta del mando", &dz, 0.05f, 0.6f, "%.2f")) {
    cfg["deadzone"] = dz;
    changed = true;
  }
  bool mt = cfg.value("mouse_touch", true);
  if (ImGui::Checkbox("Clic izquierdo = tocar la pantalla", &mt)) {
    cfg["mouse_touch"] = mt;
    changed = true;
  }
  const char* aim_opts[] = {"right", "middle", "none"};
  const char* aim_names[] = {"clic derecho", "clic central", "desactivado"};
  int ai = 2;
  for (int i = 0; i < 3; ++i)
    if (cfg.value("mouse_aim_button", std::string("right")) == aim_opts[i]) ai = i;
  if (ImGui::Combo("Apuntar/disparar con el raton", &ai, aim_names, 3)) {
    cfg["mouse_aim_button"] = aim_opts[ai];
    changed = true;
  }
  if (ImGui::Button("Restaurar controles por defecto")) {
    cfg = input::Mapper::DefaultConfig();
    changed = true;
    capture_.clear();
  }
  if (changed) {
    s_->input = cfg;
    act_.apply_input();
    Changed();
  }
  ImGui::End();
}

void Menu::WizardAdvance() {
  if (++wizard_pos_ < wizard_.size()) {
    capture_ = wizard_[wizard_pos_];
    capture_add_ = false;
  } else {
    wizard_.clear();
    capture_.clear();
    status_ = "Mando configurado. Prueba los botones en la partida.";
    status_until_ = ImGui::GetTime() + 5;
  }
}

void Menu::DrawGamepad(nlohmann::json& cfg, bool& changed) {
  if (!ImGui::CollapsingHeader("Mando", ImGuiTreeNodeFlags_DefaultOpen)) return;
  const bool ps = PlayStationPad();
  int pads = 0;
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    const char* name = SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i) : SDL_JoystickNameForIndex(i);
    ImGui::BulletText("%s%s", name ? name : "?", SDL_IsGameController(i) ? "" : "  (sin mapeo: modo crudo)");
    ++pads;
  }
  if (!pads) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "No hay ningun mando conectado.");

  // Probador en vivo: lo que recibe el juego.
  const float lx = mapper_->pad_axis(SDL_CONTROLLER_AXIS_LEFTX), ly = mapper_->pad_axis(SDL_CONTROLLER_AXIS_LEFTY);
  const float rx = mapper_->pad_axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = mapper_->pad_axis(SDL_CONTROLLER_AXIS_RIGHTY);
  ImGui::Text("Stick izq. %+.2f %+.2f   Stick der. %+.2f %+.2f   %s %.2f  %s %.2f", lx, ly, rx, ry, ps ? "L2" : "LT",
              mapper_->pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), ps ? "R2" : "RT", mapper_->pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
  std::string held;
  for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
    if (mapper_->pad_button(b))
      if (const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(b)))
        held += (held.empty() ? "" : " ") + PadLabel(n, ps);
  ImGui::Text("Botones pulsados: %s", held.empty() ? "-" : held.c_str());
  if (pads && !mapper_->any_pad_input())
    ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                       "Aun no llega nada del mando. Si pulsas y no cambia: cierra Steam/DS4Windows o usa el driver \"Compatibilidad\".");

  // Sticks: cual mueve y cual apunta.
  bool swap = cfg["move_stick"].value("pad_stick", std::string("left")) == "right";
  if (ImGui::Checkbox("Intercambiar sticks (derecho = mover, izquierdo = apuntar)", &swap)) {
    cfg["move_stick"]["pad_stick"] = swap ? "right" : "left";
    cfg["aim_stick"]["pad_stick"] = swap ? "left" : "right";
    changed = true;
  }
  // Driver.
  static const char* kDrv[] = {"auto", "compat"};
  static const char* kDrvNames[] = {"Automatico (recomendado)", "Compatibilidad (sin HIDAPI)"};
  int di = s_->pad_driver == "compat" ? 1 : 0;
  if (ImGui::Combo("Driver de mandos", &di, kDrvNames, 2)) {
    s_->pad_driver = kDrv[di];
    Changed();
  }
  ImGui::TextDisabled("El driver se aplica al reiniciar el juego.");
  // Asistente.
  if (ImGui::Button(wizard_.empty() ? "Asistente: mapear con el mando" : "Cancelar asistente")) {
    if (wizard_.empty()) {
      for (std::size_t i = 0; cfg["buttons"].is_array() && i < cfg["buttons"].size(); ++i) wizard_.push_back(std::format("buttons.{}", i));
      for (std::size_t i = 0; cfg["android_keys"].is_array() && i < cfg["android_keys"].size(); ++i)
        wizard_.push_back(std::format("android_keys.{}", i));
      wizard_pos_ = 0;
      if (!wizard_.empty()) {
        capture_ = wizard_[0];
        capture_add_ = false;
      }
    } else {
      wizard_.clear();
      capture_.clear();
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Pide cada accion en orden; los sticks se configuran arriba.");
}

void Menu::DrawAbout(const MenuStats& st) {
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(120, 90), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Acerca de", &show_about_)) return ImGui::End();
  ImGui::TextUnformatted("Zombiewood - port nativo para Windows (so-loader + JIT ARM)");
  ImGui::TextDisabled("Los datos del juego son de tu copia; el port no distribuye nada de Gameloft.");
  ImGui::Separator();
  ImGui::Text("Renderer: %s", st.renderer.c_str());
  ImGui::Text("Backend configurado: %s", s_->backend.c_str());
  ImGui::Text("Interna %dx%d, ventana %dx%d", st.internal_w, st.internal_h, st.window_w, st.window_h);
  ImGui::Text("%.1f fps, step %.2f ms, frame %lld", st.fps, st.step_ms, st.frame);
  ImGui::Text("Datos: %s", s_->game_dir.string().c_str());
  ImGui::Text("Saves: %s", s_->vfs.saves_dir.string().c_str());
  ImGui::Separator();
  ImGui::TextUnformatted("F1 menu | F11 pantalla completa | F12 captura | Esc atras");
  ImGui::End();
}

void Menu::DrawFps(const MenuStats& st) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 10, vp->Pos.y + (bar_ ? 30.0f : 10.0f)), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.45f);
  ImGui::Begin("##fps", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
  ImGui::Text("%.0f fps  %.1f ms", st.fps, st.step_ms);
  ImGui::End();
}

}  // namespace zw
