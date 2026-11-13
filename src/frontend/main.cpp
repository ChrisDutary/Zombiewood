#include <GLES2/gl2.h>
#include <SDL.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "cpu/deps_check.h"
#include "frontend/game.h"
#include "frontend/window.h"
#include "hle/audio/audio_hle.h"
#include "hle/gles/gles.h"
#include "hle/libc/libc.h"
#include "jni/jni.h"
#include "imgui.h"
#include "loader/load_only.h"

int main(int argc, char** argv) {
  zw::log::Init();

  // --load-only <ruta .so> [mas .so...]: diagnostico del loader/CPU (T3), sin ventana ni SDL.
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--load-only")) {
      std::vector<std::filesystem::path> libs(argv + i + 1, argv + argc);
      if (libs.empty()) { zw::log::Error("uso: zombiewood --load-only <ruta .so> [mas .so...]"); return 2; }
      // Con la HLE real (T4-T7) y el JavaVM de T7; la HLE minima de T3 solo rellena huecos.
      zw::GuestMemory::Init();
      zw::hle::libc::Register();
      zw::hle::gles::Register();
      zw::hle::audio::Register();
      zw::jni::Register();
      return zw::loader::RunLoadOnly(libs, zw::jni::JavaVM());
    }
  }
  // Sin --smoke-test: arrancar el juego (T8). --smoke-test[=N] conserva la prueba de ventana + ImGui de T2.
  int smoke_frames = -1;
  zw::GameOptions opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](const char* k) { return a.rfind(k, 0) == 0 ? a.substr(std::strlen(k)) : std::string(); };
    if (!std::strncmp(argv[i], "--smoke-test", 12)) smoke_frames = argv[i][12] == '=' ? std::atoi(argv[i] + 13) : 120;
    else if (auto v = val("--lib="); !v.empty()) opt.lib = v;
    else if (auto v = val("--frames="); !v.empty()) opt.max_frames = std::atoi(v.c_str());
    else if (auto v = val("--script="); !v.empty()) opt.script = v;
    else if (auto v = val("--control="); !v.empty()) opt.control = v;
    else if (auto v = val("--shot-every="); !v.empty()) opt.shot_every = std::atoi(v.c_str());
    else if (a == "--no-audio") opt.no_audio = true;
    else if (a == "--hidden") opt.hidden = true;
    else zw::log::Warn("argumento desconocido: {}", a);
  }
  if (smoke_frames < 0) return zw::RunGame(opt);
  zw::log::Info("Zombiewood (prueba de ventana) - home: {}", zw::paths::Home().string());

  auto cfg = zw::LoadConfig();
  int w = zw::ConfigGet(cfg, "width", 1280);
  int h = zw::ConfigGet(cfg, "height", 720);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    zw::log::Error("SDL_Init fallo: {}", SDL_GetError());
    return 1;
  }

  int rc = 0;
  {
    zw::Window win;
    if (!win.Create(w, h, "Zombiewood")) {
      SDL_Quit();
      return 1;
    }

    zw::log::Info("dynarmic self-test (mov r0,#42): r0 = {}", zw::CpuDepsSelfTest());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win.sdl(), win.gl());
    ImGui_ImplOpenGL3_Init("#version 100");

    bool running = true;
    bool show_demo = true;
    int frame = 0;
    float clear[3] = {0.10f, 0.35f, 0.20f};
    while (running) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) running = false;
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
      }

      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      ImGui::Begin("Zombiewood - prueba");
      ImGui::Text("Frame %d  (%.1f FPS)", frame, ImGui::GetIO().Framerate);
      ImGui::Text("GL_VERSION: %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
      ImGui::Text("GL_RENDERER: %s", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
      ImGui::Text("ETC1: %s", win.HasExtension("GL_OES_compressed_ETC1_RGB8_texture") ? "si" : "no");
      ImGui::ColorEdit3("clear", clear);
      ImGui::Checkbox("demo", &show_demo);
      ImGui::End();
      if (show_demo) ImGui::ShowDemoWindow(&show_demo);
      ImGui::Render();

      int dw, dh;
      SDL_GL_GetDrawableSize(win.sdl(), &dw, &dh);
      glViewport(0, 0, dw, dh);
      glClearColor(clear[0], clear[1], clear[2], 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      win.SwapBuffers();

      ++frame;
      if (smoke_frames >= 0 && frame >= smoke_frames) {
        zw::log::Info("smoke-test: {} frames renderizados, saliendo", frame);
        running = false;
      }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
  }
  SDL_Quit();
  return rc;
}
