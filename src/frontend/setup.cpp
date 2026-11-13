#include "frontend/setup.h"

#include <windows.h>
#include <commdlg.h>

#include <GLES2/gl2.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "frontend/settings.h"
#include "imgui.h"
#include "installer/installer.h"

namespace zw {
namespace fs = std::filesystem;
namespace {

HWND Hwnd(SDL_Window* w) {
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  return w && SDL_GetWindowWMInfo(w, &info) ? info.info.win.window : nullptr;
}

std::vector<fs::path> OpenDialog(SDL_Window* owner, const wchar_t* title, const wchar_t* filter, bool multi) {
  std::vector<wchar_t> buf(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = Hwnd(owner);
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT : 0);
  std::vector<fs::path> out;
  if (!GetOpenFileNameW(&ofn)) return out;
  // Multi: "carpeta\0fich1\0fich2\0\0"; uno solo: "ruta completa\0\0".
  const wchar_t* p = buf.data();
  const fs::path first(p);
  p += wcslen(p) + 1;
  if (!*p) {
    out.push_back(first);
    return out;
  }
  for (; *p; p += wcslen(p) + 1) out.push_back(first / p);
  return out;
}

std::string U8(const fs::path& p) { return PathToUtf8(p); }

class SetupUi {
 public:
  SetupUi(SDL_Window* w, Settings& s, const SetupOptions& o) : win_(w), s_(s), o_(o) {}

  bool Run() {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.25f);
    ImGui::GetIO().FontGlobalScale = 1.25f;
    ImGui_ImplSDL2_InitForOpenGL(win_, SDL_GL_GetCurrentContext());
    ImGui_ImplOpenGL3_Init("#version 100");
    if (!o_.install_apk.empty()) StartInstall(o_.install_apk, o_.obbs);
    bool ok = false;
    while (!done_) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_KEYDOWN) {
          log::Debug("setup: tecla {}", SDL_GetScancodeName(ev.key.keysym.scancode));
          // Enter: accion principal (tambien sin foco de ImGui; util para automatizar la prueba).
          if (ev.key.keysym.scancode == SDL_SCANCODE_RETURN || ev.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) enter_ = true;
        }
        if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
          prog_.cancel = true;
          quit_ = true;
        }
      }
      if (quit_ && !worker_.joinable()) break;
      if (state_ == State::Installing && prog_.finished) {
        worker_.join();
        state_ = res_.ok ? State::Done : State::Error;
        if (res_.ok) OnInstalled();
        if (quit_) break;
        // --install sin avisos: continuar directamente.
        if (res_.ok && !o_.install_apk.empty()) {
          ok = true;
          done_ = true;
        }
      }
      Frame(ok);
      std::this_thread::sleep_for(std::chrono::milliseconds(state_ == State::Installing ? 16 : 8));
    }
    if (worker_.joinable()) {
      prog_.cancel = true;
      worker_.join();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(ctx_);
    return ok && !quit_;
  }

 private:
  enum class State { Welcome, Installing, Done, Error };

  void StartInstall(const fs::path& apk, std::vector<fs::path> obbs) {
    const auto info = installer::Inspect(apk);
    log::Info("setup: APK {}: zip {}, lib {}, archiveObbCache {} partes, game.apk {}, videos {}", U8(apk), info.ok, info.has_lib,
              info.obb_cache_parts, info.has_inner_apk, info.has_videos);
    if (!info.ok || !info.error.empty()) {
      Fail("El archivo elegido no es un APK de Zombiewood valido:\n" + (info.error.empty() ? std::string("ZIP ilegible") : info.error));
      return;
    }
    if (!info.has_obb_cache && obbs.empty()) {
      obbs = installer::FindObbsNear(apk);
      if (obbs.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Zombiewood",
                                 "Este APK no trae los datos (OBB).\n\nElige ahora los archivos main.1501...obb y "
                                 "patch.15325...obb de tu copia del juego (normalmente en Android/obb/"
                                 "com.gameloft.android.ANMP.GloftZOHM).",
                                 win_);
        obbs = PickObbDialog(win_);
        if (obbs.empty()) {
          Fail("Sin los archivos .obb no se puede instalar el juego.");
          return;
        }
      }
    }
    src_ = {apk, obbs};
    prog_.done = 0;
    prog_.total = 0;
    prog_.cancel = false;
    prog_.finished = false;
    state_ = State::Installing;
    worker_ = std::thread([this] { res_ = installer::Install(src_, paths::DefaultGameDir(), prog_); });
  }

  void Fail(std::string msg) {
    res_ = {};
    res_.error = std::move(msg);
    state_ = State::Error;
    log::Error("setup: {}", res_.error);
  }

  void OnInstalled() {
    // zombiewood.json: game_dir -> <home>/data (relativo), sin tocar el resto; recargar ajustes.
    auto cfg = LoadConfig();
    cfg["game_dir"] = "data";
    if (cfg.contains("vfs") && cfg["vfs"].is_object()) cfg["vfs"].erase("obb_dir");
    for (const char* k : {"lib", "assets_dir", "videos_dir"}) cfg.erase(k);
    SaveConfig(cfg);
    s_ = Settings::Load();
    s_.Save();  // escribe todas las claves con sus valores (documenta las opciones al usuario)
  }

  void Frame(bool& ok) {
    if (state_ == State::Installing || state_ == State::Error) enter_ = false;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetCursorPos(ImVec2(40, 30));
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.35f, 1), "ZOMBIEWOOD  -  port nativo para PC");
    ImGui::Separator();
    ImGui::Spacing();
    const float w = std::min(vp->Size.x - 80, 900.0f);
    ImGui::PushTextWrapPos(40 + w);
    switch (state_) {
      case State::Welcome:
        ImGui::TextUnformatted("No se encontraron los datos del juego.");
        ImGui::Spacing();
        ImGui::TextUnformatted(
            "Este port no incluye ningun archivo de Zombiewood: necesitas tu propia copia del juego para Android "
            "(version 1.5.3). Elige el APK:");
        ImGui::BulletText("el APK grande (incluye assets/archiveObbCache.zip.00N con los OBB), o");
        ImGui::BulletText("el APK original + los archivos main.1501...obb y patch.15325...obb.");
        ImGui::Spacing();
        ImGui::TextDisabled("Se extraera en: %s", U8(paths::DefaultGameDir()).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Elegir APK... (Enter)", ImVec2(260, 0)) || std::exchange(enter_, false)) {
          const auto apk = PickApkDialog(win_);
          if (!apk.empty()) StartInstall(apk, o_.obbs);
        }
        ImGui::SameLine();
        if (ImGui::Button("Salir", ImVec2(120, 0))) done_ = true;
        break;
      case State::Installing: {
        const double tot = static_cast<double>(prog_.total.load()), dn = static_cast<double>(prog_.done.load());
        const float frac = tot > 0 ? static_cast<float>(std::min(1.0, dn / tot)) : 0.0f;
        ImGui::Text("Instalando desde %s", U8(src_.apk.filename()).c_str());
        ImGui::Spacing();
        ImGui::ProgressBar(frac, ImVec2(w, 0), std::format("{:.0f} / {:.0f} MB", dn / 1048576.0, tot / 1048576.0).c_str());
        ImGui::TextUnformatted(prog_.Stage().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Cancelar", ImVec2(160, 0))) prog_.cancel = true;
        break;
      }
      case State::Done:
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Instalacion completada (%zu archivos).", res_.files.size());
        ImGui::Text("libZombieWood.so: %s", res_.version_ok ? "version 1.5.3 verificada (SHA-256)" : "VERSION NO VERIFICADA");
        for (const auto& wmsg : res_.warnings) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Aviso: %s", wmsg.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Pulsa F1 durante el juego para abrir el menu (graficos, audio, controles, carpetas).");
        ImGui::Spacing();
        if (ImGui::Button("Jugar (Enter)", ImVec2(180, 0)) || std::exchange(enter_, false)) {
          ok = true;
          done_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Abrir carpeta de datos", ImVec2(260, 0))) paths::OpenFolder(paths::DefaultGameDir());
        break;
      case State::Error:
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No se pudo instalar:");
        ImGui::TextUnformatted(res_.error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Elegir otro APK...", ImVec2(240, 0))) {
          const auto apk = PickApkDialog(win_);
          if (!apk.empty()) StartInstall(apk, {});
        }
        ImGui::SameLine();
        if (ImGui::Button("Ver logs", ImVec2(140, 0))) paths::OpenFolder(paths::LogsDir());
        ImGui::SameLine();
        if (ImGui::Button("Salir", ImVec2(120, 0))) done_ = true;
        break;
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::End();
    ImGui::Render();
    int dw, dh;
    SDL_GL_GetDrawableSize(win_, &dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glClearColor(0.06f, 0.07f, 0.06f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(win_);
  }

  SDL_Window* win_;
  Settings& s_;
  const SetupOptions& o_;
  ImGuiContext* ctx_ = nullptr;
  State state_ = State::Welcome;
  installer::Progress prog_;
  installer::Source src_;
  installer::Result res_;
  std::thread worker_;
  bool done_ = false, quit_ = false, enter_ = false;
};

}  // namespace

fs::path PickApkDialog(SDL_Window* owner) {
  auto v = OpenDialog(owner, L"Elige el APK de Zombiewood (1.5.3)", L"APK de Android (*.apk)\0*.apk\0Todos los archivos\0*.*\0\0", false);
  return v.empty() ? fs::path() : v.front();
}

std::vector<fs::path> PickObbDialog(SDL_Window* owner) {
  return OpenDialog(owner, L"Elige los OBB de Zombiewood (main y patch)", L"OBB (*.obb)\0*.obb\0Todos los archivos\0*.*\0\0", true);
}

bool EnsureGameData(SDL_Window* win, Settings& s, const SetupOptions& o) {
  std::error_code ec;
  const bool have = fs::exists(s.lib, ec) && fs::exists(s.vfs.obb_dir / installer::kMainObbName, ec);
  if (have && !o.force && o.install_apk.empty()) return true;
  if (!have) log::Info("setup: faltan datos (lib {} / obb {}): asistente de instalacion", U8(s.lib), U8(s.vfs.obb_dir));
  SetupUi ui(win, s, o);
  return ui.Run();
}

}  // namespace zw
