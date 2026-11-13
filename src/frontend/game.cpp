// Integracion (T8). Hilos, como en Android (ANALYSIS §4):
//   - hilo UI  = hilo principal: SDL (ventana + eventos) y los pasos 0-7 del arranque (JNI_OnLoad, GL2JNILib.init,
//     Game.nativeInit...). Despues solo traduce eventos SDL a una cola.
//   - hilo GL  = hilo del juego: tiene el contexto GLES, ejecuta los pasos 8-13 y el bucle step() + swap.
//     El input se inyecta entre frames desde este hilo (recomendacion de ANALYSIS §10).
// El juego dibuja en un FBO de resolucion interna (el "framebuffer 0" del guest, ver gles::SetDefaultFramebuffer)
// que se escala a la ventana conservando el aspecto.
#include "frontend/game.h"

#include <GLES3/gl3.h>
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "frontend/cursor.h"
#include "frontend/input.h"
#include "frontend/menu.h"
#include "frontend/png.h"
#include "frontend/saves.h"
#include "frontend/settings.h"
#include "frontend/setup.h"
#include "frontend/video.h"
#include "frontend/window.h"
#include "hle/audio/audio_hle.h"
#include "hle/audio/audio_track.h"
#include "hle/gles/gles.h"
#include "hle/libc/libc.h"
#include "jni/jni.h"
#include "loader/loader.h"

namespace zw {
namespace {
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------- eventos UI -> GL
struct Ev {
  enum Type { Key, PadButton, PadAxis, MouseButton, MouseMove, Finger, Drawable, Pause, Resume, Shot, Quit, ToggleMenu, Fullscreen, Sdl } type;
  int a = 0, b = 0, c = 0, d = 0;
  float f = 0;
  SDL_Event raw{};  // evento SDL original (para ImGui); raw.type == 0 si no hay
};
enum UserCode { kUserMinimize = 1, kUserDone = 2, kUserFullscreen = 3, kUserCursor = 4 };

struct Shared {
  std::mutex mu;
  std::vector<Ev> queue;
  std::atomic<bool> done{false};
  std::atomic<int> rc{0};
  std::atomic<float> fps{0}, step_ms{0};
  void Push(const Ev& e) {
    std::lock_guard lk(mu);
    queue.push_back(e);
  }
};
Shared g_sh;
Uint32 g_user_event = 0;

void PostUser(int code, int data = 0) {
  SDL_Event e{};
  e.type = g_user_event;
  e.user.code = code;
  e.user.data1 = reinterpret_cast<void*>(static_cast<std::intptr_t>(data));
  SDL_PushEvent(&e);
}

// Estadisticas de audio para el log (bytes PCM entregados por Vox y pico de amplitud).
std::atomic<std::uint64_t> g_audio_bytes{0};
std::atomic<int> g_audio_peak{0};
std::FILE* g_audio_dump = nullptr;  // ZW_AUDIO_DUMP=<fichero>: PCM crudo que Vox escribe en AudioTrack (diagnostico)

// T6 -> T7: android.media.AudioTrack sobre SDL.
struct SdlTrack final : jni::AudioTrackBackend {
  audio::Track t;
  bool pcm16;
  SdlTrack(int s, int r, int c, int e, int b, int m) : t(s, r, c, e, b, m), pcm16(e == audio::kEncodingPcm16) {
    log::Info("audio: pista {} Hz, {} canales, {} -> {}", r, c == audio::kChannelOutStereo ? 2 : 1, pcm16 ? "PCM16" : "PCM8",
              t.has_device() ? "dispositivo SDL" : "SIN dispositivo (se descarta a tiempo real)");
  }
  void Play() override { t.play(); }
  void Pause() override { t.pause(); }
  void Stop() override { t.stop(); }
  void Release() override { t.release(); }
  int Write(const void* p, int n) override {
    if (pcm16 && n > 1) {
      const auto* smp = static_cast<const std::int16_t*>(p);
      int peak = g_audio_peak.load(std::memory_order_relaxed);
      for (int i = 0; i < n / 2; i += 8) peak = std::max(peak, std::abs(static_cast<int>(smp[i])));
      g_audio_peak.store(peak, std::memory_order_relaxed);
    }
    g_audio_bytes += static_cast<std::uint64_t>(n > 0 ? n : 0);
    if (g_audio_dump && n > 0) std::fwrite(p, 1, static_cast<std::size_t>(n), g_audio_dump);
    return t.write(p, n);
  }
  int PlayState() const override { return t.play_state(); }
};

// ---------------------------------------------------------------------------------- hilo GL
class GlThread {
 public:
  GlThread(const Settings& s, const GameOptions& opt, Window& win, saves::Backup* backup)
      : s_(s), opt_(opt), win_(win), backup_(backup) {}

  void Run() {
    int rc = 0;
    try {
      Init();
      Loop();
    } catch (const GuestFault& f) {
      log::Error("fallo del guest en el hilo GL:\n{}", f.what());
      rc = 70;
    } catch (const GuestExit& e) {
      log::Info("el juego termino (exit {})", e.code);
      rc = e.code;
    } catch (const std::exception& e) {
      log::Error("error en el hilo GL: {}", e.what());
      rc = 1;
    }
    hle::gles::SetDefaultFramebuffer(0);
    menu_.Shutdown();
    SDL_GL_MakeCurrent(win_.sdl(), nullptr);
    g_sh.rc = rc;
    g_sh.done = true;
    PostUser(kUserDone);
  }

 private:
  // ---- framebuffer interno ----
  void AllocFboStorage(int w, int h) {
    GLint old_tex = 0, old_rb = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(old_rb));
    iw_ = w;
    ih_ = h;
  }

  void CreateFbo() {
    int major = 0;
    if (const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION))) std::sscanf(v, "OpenGL ES %d", &major);
    if (major < 3) {
      // Sin ES3 no hay glBlitFramebuffer: el juego dibuja directamente en la ventana.
      log::Warn("contexto GLES {} < 3: sin framebuffer interno (resolucion interna = ventana)", major);
      direct_ = true;
      iw_ = dw_;
      ih_ = dh_;
      return;
    }
    glGenTextures(1, &tex_);
    glGenRenderbuffers(1, &rbo_);
    glGenFramebuffers(1, &fbo_);
    AllocFboStorage(iw_, ih_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo_);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
      log::Error("FBO interno incompleto (0x{:x}): se dibuja directamente en la ventana", st);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      direct_ = true;
      iw_ = dw_;
      ih_ = dh_;
      return;
    }
    hle::gles::SetDefaultFramebuffer(fbo_);
    glViewport(0, 0, iw_, ih_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  }

  void UpdateRect() {
    if (direct_) {
      rx_ = ry_ = 0;
      rw_ = dw_;
      rh_ = dh_;
      return;
    }
    const double sc = std::min(static_cast<double>(dw_) / iw_, static_cast<double>(dh_) / ih_);
    rw_ = std::max(1, static_cast<int>(iw_ * sc + 0.5));
    rh_ = std::max(1, static_cast<int>(ih_ * sc + 0.5));
    rx_ = (dw_ - rw_) / 2;
    ry_ = (dh_ - rh_) / 2;
  }

  // Copia el FBO interno a la ventana (letterbox) y presenta. Solo toca estado GL que restaura.
  void Present() {
    if (!direct_) {
      GLint draw_fb = 0, read_fb = 0;
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
      const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
      GLboolean mask[4];
      GLfloat cc[4];
      glGetBooleanv(GL_COLOR_WRITEMASK, mask);
      glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
      glDisable(GL_SCISSOR_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glBlitFramebuffer(0, 0, iw_, ih_, rx_, ry_, rx_ + rw_, ry_ + rh_, GL_COLOR_BUFFER_BIT,
                        s_.linear_filter ? GL_LINEAR : GL_NEAREST);
      DrawMenu();
      win_.SwapBuffers();
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fb));
      glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fb));
      glColorMask(mask[0], mask[1], mask[2], mask[3]);
      glClearColor(cc[0], cc[1], cc[2], cc[3]);
      if (scissor) glEnable(GL_SCISSOR_TEST);
    } else {
      DrawMenu();
      win_.SwapBuffers();
    }
  }

  void DrawMenu() {
    DrawMenuOnly();
    for (const auto& n : window_shots_) WindowShot(n);
    window_shots_.clear();
  }

  void WindowShot(const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(s_.screens_dir, ec);
    std::vector<std::uint8_t> px(static_cast<std::size_t>(dw_) * dh_ * 4);
    GLint read_fb = 0, pack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, dw_, dh_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fb));
    const auto path = s_.screens_dir / (name + ".png");
    if (WritePngRgba(path, px.data(), dw_, dh_, true)) log::Info("captura de ventana: {}", path.string());
  }

  void DrawMenuOnly() {
    if (!menu_.Active()) return;
    MenuStats st;
    st.fps = g_sh.fps;
    st.step_ms = g_sh.step_ms;
    st.frame = frame_;
    st.internal_w = iw_;
    st.internal_h = ih_;
    st.window_w = dw_;
    st.window_h = dh_;
    st.paused = paused_;
    st.renderer = renderer_;
    menu_.Draw(dw_, dh_, st);
  }

  // ---- videos (T9) ----
  // Eventos durante un video: cualquier pulsacion lo salta; Quit sale. Los "up" y demas se conservan para el juego.
  bool VideoShouldSkip() {
    bool skip = false;
    std::lock_guard lk(g_sh.mu);
    std::vector<Ev> keep;
    for (const Ev& e : g_sh.queue) {
      switch (e.type) {
        case Ev::Quit:
          quit_ = true;
          skip = true;
          keep.push_back(e);
          break;
        case Ev::Key:
        case Ev::PadButton:
        case Ev::MouseButton:
          if (e.b) skip = true;
          else keep.push_back(e);
          break;
        case Ev::Finger:
          if (e.a == 1) skip = true;
          break;
        case Ev::MouseMove:
        case Ev::Sdl:
        case Ev::PadAxis: break;
        case Ev::Drawable:
          dw_ = std::max(1, e.a);
          dh_ = std::max(1, e.b);
          if (s_.follow_window || direct_) pending_resize_ = true;
          UpdateRect();
          break;
        default: keep.push_back(e); break;
      }
    }
    g_sh.queue.swap(keep);
    return skip;
  }

  // Reproduce res/raw/<file>. Como en Android, el juego queda "en segundo plano" (audio de Vox en pausa).
  void PlayVideoFile(const std::string& file) {
    if (s_.skip_videos) {
      log::Info("video: {} saltado (skip_videos)", file);
      return;
    }
    const bool was_paused = audio::global_paused();
    audio::set_global_paused(true);
    video::PlayParams pp;
    pp.window = win_.sdl();
    pp.volume = s_.mute ? 0.0f : s_.video_volume;
    pp.linear_filter = s_.linear_filter;
    pp.should_skip = [this] { return VideoShouldSkip(); };
    video::Play(s_.videos_dir / file, pp);
    audio::set_global_paused(was_paused);
    next_frame_ = Clock::now();
  }

  // Game.playCinematicAndroid(nombre): Java lanza MyVideoView con intro_hd.mp4 si el nombre contiene "intro",
  // si no moviecomplete_hd.mp4 (ANALYSIS §8); al cerrarse llama a SUtils.onVideoFinished.
  void PlayCinematic(const std::string& name) {
    std::string lower = name;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string file = lower.find("intro") != std::string::npos ? "intro_hd.mp4" : "moviecomplete_hd.mp4";
    std::error_code ec;
    if (file == "moviecomplete_hd.mp4" && !std::filesystem::exists(s_.videos_dir / file, ec)) file = "moviecomplete_ld.mp4";
    log::Info("video: playCinematicAndroid(\"{}\") -> {}", name, file);
    PlayVideoFile(file);
    if (jni::FindNative("GLUtils_SUtils_onVideoFinished")) jni::call_native("GLUtils_SUtils_onVideoFinished");
  }

  void Screenshot(std::string name) {
    std::error_code ec;
    std::filesystem::create_directories(s_.screens_dir, ec);
    if (name.empty()) {
      const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm{};
      localtime_s(&tm, &t);
      char buf[64];
      std::strftime(buf, sizeof buf, "zw_%Y%m%d_%H%M%S", &tm);
      name = std::format("{}_{:06}", buf, frame_);
    }
    std::vector<std::uint8_t> px(static_cast<std::size_t>(iw_) * ih_ * 4);
    GLint read_fb = 0, pack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, direct_ ? 0 : fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, iw_, ih_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fb));
    const auto path = s_.screens_dir / (name + ".png");
    if (WritePngRgba(path, px.data(), iw_, ih_, true)) log::Info("captura: {} (frame {})", path.string(), frame_);
    else log::Error("no se pudo escribir la captura {}", path.string());
  }

  // ---- nativos ----
  void Touch(int action, int x, int y, int id) {
    if (trace_input_) log::Info("[input] touch action {} ({}, {}) id {}", action, x, y, id);
    jni::call_native("Game_nativeOnTouch", action, x, y, id);
  }
  void AndroidKey(int code) {
    log::Info("[input] tecla Android {} (down+up)", code);
    jni::call_native("Game_nativeOnKeyDown", code);
    jni::call_native("Game_nativeOnKeyUp", code);
  }
  input::TouchFn TouchSink() {
    return [this](int a, int x, int y, int id) { Touch(a, x, y, id); };
  }

  void SetPaused(bool p) {
    if (p == paused_) return;
    paused_ = p;
    if (p) {
      if (backup_) backup_->Poll();
      mapper_.ReleaseAll(TouchSink());
      log::Info("pausa: GL2JNILib.pause + nativeVoxSuspend");
      jni::call_native("GL2JNILib_pause");
      jni::call_native("Game_nativeVoxSuspend");
      audio::set_global_paused(true);
    } else {
      log::Info("reanudar: GL2JNILib.resume + nativeVoxResume");
      audio::set_global_paused(false);
      jni::call_native("GL2JNILib_resume");
      jni::call_native("Game_nativeVoxResume");
      next_frame_ = Clock::now();
    }
  }

  void Resize(int w, int h) {
    if (w == iw_ && h == ih_) return;
    if (!direct_) AllocFboStorage(w, h);
    iw_ = w;
    ih_ = h;
    mapper_.SetScreen(iw_, ih_);
    glViewport(0, 0, iw_, ih_);
    log::Info("resize: GL2JNILib.resize({}, {})", iw_, ih_);
    jni::call_native("GL2JNILib_resize", iw_, ih_);
  }

  // Pixeles de la ventana -> pixeles internos.
  void ToInternal(int mx, int my, int& ix, int& iy) const {
    ix = std::clamp(static_cast<int>((mx - rx_) * static_cast<double>(iw_) / rw_), 0, iw_ - 1);
    iy = std::clamp(static_cast<int>((my - ry_) * static_cast<double>(ih_) / rh_), 0, ih_ - 1);
  }

  void InitMenu() {
    MenuActions a;
    a.apply_vsync = [this] { SDL_GL_SetSwapInterval(s_.vsync ? 1 : 0); };
    a.set_fullscreen = [](bool on) { PostUser(kUserFullscreen, on ? 1 : 0); };
    // Se pasa el valor en el evento (no se lee Settings desde el hilo UI): tamano > 0 = zombi, 0 = Windows.
    a.apply_cursor = [this] { PostUser(kUserCursor, s_.cursor == "zombie" ? s_.cursor_size : 0); };
    a.apply_resolution = [this] {
      if (direct_) return;
      if (s_.follow_window) pending_resize_ = true;
      else Resize(s_.internal_w, s_.internal_h);
      UpdateRect();
    };
    a.apply_audio = [this] {
      audio::set_master_volume(s_.volume);
      audio::set_muted(s_.mute);
    };
    a.apply_input = [this] {
      mapper_.ReleaseAll(TouchSink());
      mapper_.Configure(s_.input);
      mapper_.SetScreen(iw_, ih_);
    };
    a.set_paused = [this](bool p) { want_pause_ = p; };
    a.screenshot = [this] { shot_names_.push_back(""); };
    a.backup_now = [this] {
      if (backup_) backup_->Snapshot("manual");
    };
    a.quit = [this] { quit_ = true; };
    menu_.Init(win_.sdl(), s_, mapper_, std::move(a));
  }

  void Init() {
    SDL_GL_MakeCurrent(win_.sdl(), win_.gl());
    SDL_GL_SetSwapInterval(s_.vsync ? 1 : 0);
    SDL_GL_GetDrawableSize(win_.sdl(), &dw_, &dh_);
    if (const auto* r = glGetString(GL_RENDERER)) renderer_ = reinterpret_cast<const char*>(r);
    iw_ = s_.follow_window ? dw_ : s_.internal_w;
    ih_ = s_.follow_window ? dh_ : s_.internal_h;
    CreateFbo();
    UpdateRect();
    mapper_.Configure(s_.input);
    mapper_.SetScreen(iw_, ih_);
    audio::set_master_volume(s_.volume);
    audio::set_muted(s_.mute);
    InitMenu();
    jni::hooks().play_cinematic = [this](const std::string& n) { PlayCinematic(n); };
    // Splash de Java (logo de Gameloft) antes de que el juego dibuje nada.
    if (s_.play_logo && !opt_.hidden) PlayVideoFile("logo_hd.mp4");
    trace_input_ = std::getenv("ZW_TRACE_INPUT") != nullptr;
    if (!opt_.script.empty()) {
      std::ifstream f(opt_.script);
      for (std::string l; std::getline(f, l);) cmds_.push_back(l);
      log::Info("script: {} comandos de {}", cmds_.size(), opt_.script.string());
    }

    const auto& jc = jni::config();
    auto step = [](const char* what, auto&& fn) {
      const auto t0 = Clock::now();
      fn();
      log::Info("arranque GL: {} ({} ms)", what, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    };
    step("paso 8: GL2JNILib.initGL", [] { jni::call_native("GL2JNILib_initGL"); });
    step("paso 9: GL2JNILib.getViewSettings", [] { jni::call_native("GL2JNILib_getViewSettings"); });
    step("paso 10: GL2JNILib.getNumExtraContext", [] {
      const int n = jni::call_native<int>("GL2JNILib_getNumExtraContext");
      if (n != 0) log::Warn("el juego pide {} contextos GL extra (no soportado: un solo contexto)", n);
    });
    step("paso 11: GL2JNILib.stateChanged(true)", [] { jni::call_native("GL2JNILib_stateChanged", true); });
    step("paso 12: GetPhoneInfo (modelo, IMEI, GPU, CPU)", [&] {
      jni::call_native("Game_nativeSetPhoneModel", jc.model);
      jni::call_native("Game_nativeSetIMEI", jc.imei);
      jni::call_native("Game_nativeSetGPUInfo", jc.gpu_name);
      jni::call_native("Game_nativeSetCPUInfo", jc.cpu_freq_khz);
    });
    step("paso 13: GL2JNILib.resize", [&] {
      glViewport(0, 0, iw_, ih_);
      jni::call_native("GL2JNILib_resize", iw_, ih_);
    });
    log::Info("bucle de juego: {}x{} interno, ventana {}x{}, vsync {}, limite {} fps", iw_, ih_, dw_, dh_, s_.vsync, s_.fps_limit);
  }

  void DrainEvents() {
    std::vector<Ev> evs;
    {
      std::lock_guard lk(g_sh.mu);
      evs.swap(g_sh.queue);
    }
    for (const Ev& e : evs) {
      if (e.raw.type && menu_.ProcessEvent(e.raw)) continue;  // el menu se queda el evento
      switch (e.type) {
        case Ev::Sdl: break;
        case Ev::ToggleMenu: menu_.Toggle(); break;
        case Ev::Fullscreen: s_.fullscreen = e.a != 0; break;
        case Ev::Key: mapper_.Key(e.a, e.b != 0); break;
        case Ev::PadButton: mapper_.PadButton(e.a, e.b != 0); break;
        case Ev::PadAxis: mapper_.PadAxis(e.a, e.f); break;
        case Ev::MouseButton: {
          int x, y;
          ToInternal(e.c, e.d, x, y);
          mapper_.MouseButton(e.a, e.b != 0, x, y);
          break;
        }
        case Ev::MouseMove: {
          int x, y;
          ToInternal(e.c, e.d, x, y);
          mapper_.MouseMove(x, y);
          break;
        }
        case Ev::Finger: {
          int x, y;
          ToInternal(e.c, e.d, x, y);
          if (!paused_) Touch(e.a, x, y, e.b);
          break;
        }
        case Ev::Drawable:
          dw_ = std::max(1, e.a);
          dh_ = std::max(1, e.b);
          if (s_.follow_window || direct_) pending_resize_ = true;
          UpdateRect();
          break;
        case Ev::Pause: want_pause_ = true; break;
        case Ev::Resume: want_pause_ = false; break;
        case Ev::Shot: shot_names_.push_back(""); break;
        case Ev::Quit: quit_ = true; break;
      }
    }
  }

  // Comandos de prueba (--script / --control). Coordenadas en pixeles internos.
  //   wait <frames> | tap <x> <y> [frames] | down|move|up <id> <x> <y> | key <keycode Android>
  //   sdlkey <nombre SDL> down|up | pad <boton> down|up | axis <eje> <valor> | shot [nombre]
  //   mouse <left|right> <down|up> <x> <y> | mousemove <x> <y>
  //   pause | resume | resize <w> <h> | quit | # comentario
  void ExecCommand(const std::string& line) {
    std::istringstream ss(line);
    std::string cmd;
    if (!(ss >> cmd) || cmd[0] == '#') return;
    log::Info("[script] {}", line);
    if (cmd == "wait") {  // en "frames" de 1/60 s (tiempo real: no depende de los fps)
      int n = 0;
      ss >> n;
      wait_until_ = Clock::now() + std::chrono::microseconds(16667LL * std::max(0, n));
    } else if (cmd == "tap") {
      int x = 0, y = 0, n = 4;
      ss >> x >> y >> n;
      Touch(1, x, y, 0);
      cmds_.push_front(std::format("up 0 {} {}", x, y));
      cmds_.push_front(std::format("wait {}", n));
    } else if (cmd == "down" || cmd == "move" || cmd == "up") {
      int id = 0, x = 0, y = 0;
      ss >> id >> x >> y;
      Touch(cmd == "down" ? 1 : cmd == "move" ? 2 : 0, x, y, id);
    } else if (cmd == "key") {
      int code = 4;
      ss >> code;
      AndroidKey(code);
    } else if (cmd == "sdlkey") {
      std::string rest;
      std::getline(ss, rest);
      const bool down = rest.size() < 3 || rest.substr(rest.size() - 3) != " up";
      const auto cut = rest.rfind(' ');
      std::string name = cut == std::string::npos ? rest : rest.substr(0, cut);
      while (!name.empty() && name[0] == ' ') name.erase(0, 1);
      mapper_.Key(SDL_GetScancodeFromName(name.c_str()), down);
    } else if (cmd == "pad") {
      std::string b, st;
      ss >> b >> st;
      mapper_.PadButton(SDL_GameControllerGetButtonFromString(b.c_str()), st != "up");
    } else if (cmd == "axis") {
      std::string a;
      float v = 0;
      ss >> a >> v;
      mapper_.PadAxis(SDL_GameControllerGetAxisFromString(a.c_str()), v);
    } else if (cmd == "mouse") {  // mouse <left|right> <down|up> <x> <y>
      std::string b, st;
      int x = 0, y = 0;
      ss >> b >> st >> x >> y;
      mapper_.MouseButton(b == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, st != "up", x, y);
    } else if (cmd == "mousemove") {
      int x = 0, y = 0;
      ss >> x >> y;
      mapper_.MouseMove(x, y);
    } else if (cmd == "shot") {
      std::string name;
      ss >> name;
      shot_names_.push_back(name);
    } else if (cmd == "menu") {  // menu [graphics|audio|controls|about|none]: muestra la barra / abre una ventana
      std::string w;
      ss >> w;
      if (w.empty()) menu_.Toggle();
      else menu_.Open(w);
    } else if (cmd == "remap") {  // remap <destino> [add]: el menu espera la siguiente tecla/boton (ver Menu::StartCapture)
      std::string t, add;
      ss >> t >> add;
      menu_.StartCapture(t, add == "add");
    } else if (cmd == "ui") {  // ui key <nombre SDL> | ui pad <boton>: evento SDL real (pasa por el hilo UI y el menu)
      std::string kind, name;
      ss >> kind;
      std::getline(ss, name);
      while (!name.empty() && name[0] == ' ') name.erase(0, 1);
      SDL_Event e{};
      if (kind == "key") {
        e.type = SDL_KEYDOWN;
        e.key.keysym.scancode = SDL_GetScancodeFromName(name.c_str());
        e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
        e.key.state = SDL_PRESSED;
        SDL_PushEvent(&e);
        e.type = SDL_KEYUP;
        e.key.state = SDL_RELEASED;
        SDL_PushEvent(&e);
      } else if (kind == "pad") {
        e.type = SDL_CONTROLLERBUTTONDOWN;
        e.cbutton.button = static_cast<Uint8>(SDL_GameControllerGetButtonFromString(name.c_str()));
        e.cbutton.state = SDL_PRESSED;
        SDL_PushEvent(&e);
        e.type = SDL_CONTROLLERBUTTONUP;
        e.cbutton.state = SDL_RELEASED;
        SDL_PushEvent(&e);
      }
    } else if (cmd == "wshot") {  // captura de la VENTANA (con el menu ImGui), no del framebuffer interno
      std::string name;
      ss >> name;
      window_shots_.push_back(name.empty() ? std::format("win_{:06}", frame_) : name);
      yield_frame_ = true;  // la captura se hace al presentar: no ejecutar mas comandos en este frame
    } else if (cmd == "pause") {
      want_pause_ = true;
    } else if (cmd == "resume") {
      want_pause_ = false;
    } else if (cmd == "resize") {
      int w = 0, h = 0;
      ss >> w >> h;
      if (w > 0 && h > 0) Resize(w, h);
    } else if (cmd == "quit") {
      quit_ = true;
    } else {
      log::Warn("[script] comando desconocido: {}", cmd);
    }
  }

  void PollControl() {
    if (opt_.control.empty() || frame_ % 6) return;
    std::ifstream f(opt_.control, std::ios::binary);
    if (!f) return;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= control_off_) return;
    f.seekg(control_off_);
    std::string data(static_cast<std::size_t>(size - control_off_), '\0');
    f.read(data.data(), static_cast<std::streamsize>(data.size()));
    const auto last_nl = data.rfind('\n');
    if (last_nl == std::string::npos) return;  // linea a medio escribir
    control_off_ += static_cast<std::streamoff>(last_nl + 1);
    std::istringstream ss(data.substr(0, last_nl));
    for (std::string l; std::getline(ss, l);) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      cmds_.push_back(l);
    }
  }

  void RunCommands() {
    PollControl();
    if (Clock::now() < wait_until_) return;
    yield_frame_ = false;
    while (!cmds_.empty() && Clock::now() >= wait_until_ && !quit_ && !yield_frame_) {
      const std::string l = cmds_.front();
      cmds_.pop_front();
      ExecCommand(l);
    }
  }

  void Loop() {
    next_frame_ = Clock::now();
    auto stat_t0 = Clock::now();
    int stat_frames = 0;
    double stat_step = 0, stat_step_max = 0;
    while (!quit_) {
      DrainEvents();
      if (quit_) break;
      RunCommands();
      SetPaused(want_pause_);
      if (paused_) {
        // Pausado desde el menu (ventana visible): seguir presentando la ultima imagen + menu.
        if (menu_.Active() && !(SDL_GetWindowFlags(win_.sdl()) & SDL_WINDOW_MINIMIZED)) {
          for (auto& n : shot_names_) Screenshot(n);
          shot_names_.clear();
          Present();
          std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        continue;
      }
      if (pending_resize_) {
        pending_resize_ = false;
        Resize(dw_, dh_);
        UpdateRect();
      }
      mapper_.Update(TouchSink(), [this](int code) { AndroidKey(code); });

      const auto t0 = Clock::now();
      jni::call_native("GL2JNILib_step");
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      ++frame_;
      if (opt_.shot_every > 0 && frame_ % opt_.shot_every == 0) shot_names_.push_back(std::format("auto_{:06}", frame_));
      for (auto& n : shot_names_) Screenshot(n);
      shot_names_.clear();
      Present();

      // estadisticas
      ++stat_frames;
      stat_step += ms;
      stat_step_max = std::max(stat_step_max, ms);
      const double el = std::chrono::duration<double>(Clock::now() - stat_t0).count();
      if (el >= 5.0 || (frame_ <= 3)) {
        const double fps = stat_frames / std::max(el, 1e-6);
        if (frame_ > 3) {
          const std::uint64_t ab = g_audio_bytes.exchange(0);
          const int peak = g_audio_peak.exchange(0);
          log::Info("fps {:.1f} (frame {}; step medio {:.2f} ms, max {:.2f} ms; hilos guest {}; audio {:.0f} KB/s, pico {})", fps,
                    frame_, stat_step / stat_frames, stat_step_max, threads::Count(), ab / 1024.0 / std::max(el, 1e-6), peak);
        }
        else
          log::Info("frame {}: step {:.1f} ms", frame_, ms);
        if (frame_ > 3) {
          g_sh.fps = static_cast<float>(fps);
          g_sh.step_ms = static_cast<float>(stat_step / stat_frames);
        }
        stat_t0 = Clock::now();
        stat_frames = 0;
        stat_step = stat_step_max = 0;
      }
      if (opt_.max_frames > 0 && frame_ >= opt_.max_frames) {
        log::Info("--frames: {} frames ejecutados, saliendo", frame_);
        quit_ = true;
      }

      // frame pacing
      if (s_.fps_limit > 0) {
        const auto period = std::chrono::nanoseconds(1'000'000'000LL / s_.fps_limit);
        next_frame_ += period;
        const auto now = Clock::now();
        if (next_frame_ > now) {
          // Con vsync el swap ya espera: solo dormir si vamos claramente por delante.
          if (!s_.vsync || next_frame_ - now > std::chrono::milliseconds(2)) std::this_thread::sleep_until(next_frame_);
        } else if (now - next_frame_ > std::chrono::milliseconds(100)) {
          next_frame_ = now;  // frame largo (carga): no intentar recuperar
        }
      }
    }
    // Cierre ordenado: como onPause de Android (el juego guarda la partida al pausar).
    if (!paused_) {
      try {
        SetPaused(true);
      } catch (const GuestExit&) {
      }
    }
    // El juego guarda al pausar: copia de seguridad de lo que acaba de sobrescribir.
    if (backup_) {
      backup_->Poll();
      backup_->Poll();
    }
    log::Info("bucle de juego terminado tras {} frames", frame_);
  }

  Settings s_;  // copia propia: el menu la edita
  const GameOptions& opt_;
  Window& win_;
  saves::Backup* backup_ = nullptr;
  input::Mapper mapper_;
  Menu menu_;
  std::string renderer_;
  GLuint fbo_ = 0, tex_ = 0, rbo_ = 0;
  bool direct_ = false;
  int iw_ = 1280, ih_ = 720;        // resolucion interna
  int dw_ = 1280, dh_ = 720;        // drawable de la ventana
  int rx_ = 0, ry_ = 0, rw_ = 1280, rh_ = 720;  // rectangulo de la imagen en la ventana
  bool paused_ = false, want_pause_ = false, quit_ = false, pending_resize_ = false, trace_input_ = false;
  long long frame_ = 0;
  Clock::time_point next_frame_;
  std::deque<std::string> cmds_;
  Clock::time_point wait_until_{};
  std::streamoff control_off_ = 0;
  std::vector<std::string> shot_names_, window_shots_;
  bool yield_frame_ = false;
};

// ---------------------------------------------------------------------------------- hilo UI
template <class Fn>
bool BootStep(const char* what, Fn&& fn) {
  const auto t0 = Clock::now();
  try {
    fn();
    log::Info("arranque: {} ({} ms)", what, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    return true;
  } catch (const GuestFault& f) {
    log::Error("arranque: {}: fallo del guest:\n{}", what, f.what());
  } catch (const GuestExit& e) {
    log::Error("arranque: {}: el guest llamo a exit({})", what, e.code);
  } catch (const std::exception& e) {
    log::Error("arranque: {}: {}", what, e.what());
  }
  return false;
}

void Fatal(const std::string& msg) {
  log::Error("{}", msg);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Zombiewood", msg.c_str(), nullptr);
}

void ReportUnimplemented() {
  std::string missing;
  for (const auto& [name, n] : hle::CallCounts())
    if (n && !hle::IsRegistered(name)) missing += std::format("{}x{} ", name, n);
  log::Info("imports sin handler invocados: {}", missing.empty() ? "(ninguno)" : missing);
  // JNI: IDs pedidos sin emulacion (inofensivo mientras no se llamen) y metodos LLAMADOS sin emulacion.
  std::string ids, calls;
  std::size_t n_ids = 0;
  for (const auto& c : jni::Coverage()) {
    if (c.covered) continue;
    if (c.kind == jni::CoverageEntry::Kind::Call) calls += std::format("{}.{}{}x{} ", c.cls, c.name, c.sig, c.count);
    else if (++n_ids <= 200) ids += std::format("{}.{} ", c.cls.substr(c.cls.rfind('/') + 1), c.name);
  }
  log::Info("JNI: metodos Java llamados sin emular: {}", calls.empty() ? "(ninguno)" : calls);
  log::Info("JNI: {} clases/IDs pedidos sin emular (no llamados): {}", n_ids, ids.empty() ? "(ninguno)" : ids);
}

}  // namespace

int RunGame(const GameOptions& opt) {
  Settings s = Settings::Load(&opt);
  log::Info("Zombiewood - home: {}", paths::Home().string());

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    log::Error("SDL_Init fallo: {}", SDL_GetError());
    return 1;
  }
  std::error_code ec;
  g_user_event = SDL_RegisterEvents(1);

  // --- ventana (antes que nada: el asistente de instalacion se dibuja en ella) ---
  s.ApplyBackendEnv();
  Window win;
  if (!win.Create(s.win_w, s.win_h, "Zombiewood", opt.hidden)) {
    Fatal(std::string("No se pudo crear la ventana / contexto GLES (ANGLE): ") + SDL_GetError());
    SDL_Quit();
    return 1;
  }
  if (s.fullscreen) SDL_SetWindowFullscreen(win.sdl(), SDL_WINDOW_FULLSCREEN_DESKTOP);
  cursor::Apply(s.cursor, s.cursor_size);  // hilo UI; el juego no lo ve (solo cambia el puntero del sistema)

  // --- primer arranque: instalar los datos del APK del usuario ---
  SetupOptions so;
  so.install_apk = opt.install_apk;
  so.obbs = opt.obbs;
  so.force = opt.reinstall;
  if (!EnsureGameData(win.sdl(), s, so)) {
    log::Info("instalacion cancelada o fallida: saliendo");
    log::Shutdown();
    std::fflush(nullptr);
    std::_Exit(0);
  }
  if (opt.install_only) {
    log::Info("--install-only: datos instalados, saliendo");
    log::Shutdown();
    std::fflush(nullptr);
    std::_Exit(0);
  }
  log::Info("lib: {}", s.lib.string());
  log::Info("obb: {}  datos: {}  saves: {}  videos: {}", s.vfs.obb_dir.string(), s.vfs.data_dir.string(), s.vfs.saves_dir.string(),
            s.videos_dir.string());
  if (!std::filesystem::exists(s.lib, ec)) {
    Fatal("No se encuentra libZombieWood.so en:\n" + s.lib.string() +
          "\n\nIndica \"game_dir\" (o \"lib\") en zombiewood.json o borra la carpeta data/ para reinstalar desde el APK.");
    return 2;
  }
  if (const auto db = paths::Home() / "gamecontrollerdb.txt"; std::filesystem::exists(db, ec)) {
    const int n = SDL_GameControllerAddMappingsFromFile(db.string().c_str());
    log::Info("gamecontrollerdb.txt: {} mapeos", n);
  }

  // Copias de seguridad de la partida (antes de que el juego toque nada).
  static saves::Backup backup;
  backup.Start(s.vfs.saves_dir, paths::Home() / "saves_backup", s.backups_keep);

  // --- HLE + carga (CONTRACTS §9) ---
  if (!GuestMemory::Init()) {
    Fatal("No se pudo reservar la memoria guest (4 GiB de espacio de direcciones).");
    return 2;
  }
  hle::libc::ConfigureVfs(s.vfs);
  hle::libc::Register();
  hle::gles::Register();
  hle::audio::Register();
  jni::Register();
  if (const char* dump = std::getenv("ZW_AUDIO_DUMP")) g_audio_dump = std::fopen(dump, "wb");
  if (!opt.no_audio)
    jni::SetAudioTrackBackend(
        [](int st, int r, int c, int e, int b, int m) { return std::make_unique<SdlTrack>(st, r, c, e, b, m); });

  auto& jc = jni::config();
  jc.screen_w = s.internal_w;
  jc.screen_h = s.internal_h;
  jc.language = s.language;
  jc.assets_dir = s.assets_dir;
  jc.prefs_file = s.vfs.saves_dir / "zw_prefs.json";
  if (!std::filesystem::exists(s.vfs.obb_dir / jc.main_obb, ec))
    log::Error("no existe el OBB principal: {}", (s.vfs.obb_dir / jc.main_obb).string());
  if (!std::filesystem::exists(s.vfs.obb_dir / jc.patch_obb, ec)) {
    log::Warn("no existe el OBB de parche ({}): se omite", jc.patch_obb);
    jc.patch_obb.clear();
  }
  jni::hooks().send_to_background = [] { PostUser(kUserMinimize); };
  jni::hooks().set_current_context = [](int) { return true; };

  if (s.follow_window) {
    int dw, dh;
    SDL_GL_GetDrawableSize(win.sdl(), &dw, &dh);
    jc.screen_w = dw;
    jc.screen_h = dh;
  }
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  win.SwapBuffers();

  loader::Module* m = loader::Load(s.lib);
  if (!m) {
    Fatal("No se pudo cargar " + s.lib.string() + " (ver el log).");
    return 2;
  }
  std::size_t no_handler = 0;
  for (const auto& imp : m->imports())
    if (imp.via == loader::ImportInfo::Via::HleStub && !imp.has_handler) {
      ++no_handler;
      log::Warn("import sin handler: {}", imp.name);
    }
  log::Info("{} cargada en 0x{:08x}: {} imports, {} sin handler", m->name(), m->base(), m->imports().size(), no_handler);

  // --- pasos 0-7 (hilo UI) ---
  bool ok = BootStep("paso 0: .init_array", [&] { loader::RunInit(*m); }) &&
            BootStep("paso 0: JNI_OnLoad", [&] {
              const auto v = loader::CallJniOnLoad(*m, jni::JavaVM());
              if (!v || *v != 0x10004) throw std::runtime_error("JNI_OnLoad no devolvio 0x10004");
            }) &&
            BootStep("paso 2: Game.nativeSetGPUInfo", [&] { jni::call_native("Game_nativeSetGPUInfo", jc.gpu_name); }) &&
            BootStep("paso 4: GL2JNILib.init (createView -> GameRenderer.nativeInit)", [] { jni::call_native("GL2JNILib_init"); }) &&
            BootStep("paso 5: Game.nativeInit", [] { jni::call_native("Game_nativeInit"); });
  if (ok) {
    // Paso 7 y nativeInit de las clases de utilidades (opcionales: solo cachean clases/IDs).
    for (const char* n : {"GLUtils_SUtils_nativeInit", "GLUtils_Device_nativeInit", "DataSharing_nativeInit",
                          "PushNotification_C2DMAndroidUtils_nativeInit", "SendInfo_initMethods",
                          "Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit",
                          "Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeInit",
                          "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeInit"}) {
      if (!jni::FindNative(n)) continue;
      const std::string what = std::string("paso 7: ") + n;
      BootStep(what.c_str(), [n] { jni::call_native(n); });
    }
  }
  if (!ok) {
    Fatal("El arranque del juego fallo (ver logs/zombiewood.log).");
    log::Shutdown();
    std::fflush(nullptr);
    std::_Exit(3);
  }

  // --- hilo GL ---
  SDL_GL_MakeCurrent(win.sdl(), nullptr);
  GlThread gl(s, opt, win, &backup);
  std::thread gl_thread([&gl] {
    gl.Run();
    threads::DetachCurrent();
  });

  // --- bucle de eventos (hilo UI) ---
  auto push_drawable = [&] {
    int dw, dh;
    SDL_GL_GetDrawableSize(win.sdl(), &dw, &dh);
    g_sh.Push({Ev::Drawable, dw, dh});
  };
  auto to_drawable = [&](int x, int y, int& ox, int& oy) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(win.sdl(), &ww, &wh);
    SDL_GL_GetDrawableSize(win.sdl(), &dw, &dh);
    ox = ww > 0 ? x * dw / ww : x;
    oy = wh > 0 ? y * dh / wh : y;
  };
  std::vector<SDL_FingerID> fingers;  // dedo SDL -> puntero 10 + indice
  auto finger_slot = [&](SDL_FingerID id, bool add) {
    for (std::size_t i = 0; i < fingers.size(); ++i)
      if (fingers[i] == id) return static_cast<int>(i);
    if (!add) return -1;
    for (std::size_t i = 0; i < fingers.size(); ++i)
      if (fingers[i] == -1) {
        fingers[i] = id;
        return static_cast<int>(i);
      }
    fingers.push_back(id);
    return static_cast<int>(fingers.size() - 1);
  };
  bool fullscreen = s.fullscreen;
  auto push_raw = [](Ev e, const SDL_Event& raw) {
    e.raw = raw;
    g_sh.Push(e);
  };
  auto last_title = Clock::now();
  bool quit_sent = false;
  while (!g_sh.done) {
    SDL_Event ev;
    if (!SDL_WaitEventTimeout(&ev, 100)) ev.type = 0;
    do {
      switch (ev.type) {
        case SDL_QUIT:
          if (!quit_sent) g_sh.Push({Ev::Quit});
          quit_sent = true;
          break;
        case SDL_WINDOWEVENT:
          switch (ev.window.event) {
            case SDL_WINDOWEVENT_CLOSE:
              if (!quit_sent) g_sh.Push({Ev::Quit});
              quit_sent = true;
              break;
            case SDL_WINDOWEVENT_SIZE_CHANGED: push_drawable(); break;
            case SDL_WINDOWEVENT_MINIMIZED: g_sh.Push({Ev::Pause}); break;
            case SDL_WINDOWEVENT_RESTORED: g_sh.Push({Ev::Resume}); break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
              if (s.pause_on_focus_loss) g_sh.Push({Ev::Pause});
              break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
              if (s.pause_on_focus_loss) g_sh.Push({Ev::Resume});
              break;
          }
          break;
        case SDL_KEYDOWN:
          if (ev.key.repeat) break;
          if (ev.key.keysym.scancode == SDL_SCANCODE_F12) {
            g_sh.Push({Ev::Shot});
          } else if (ev.key.keysym.scancode == SDL_SCANCODE_F1) {
            g_sh.Push({Ev::ToggleMenu});
          } else if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                     (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
            fullscreen = !fullscreen;
            SDL_SetWindowFullscreen(win.sdl(), fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            g_sh.Push({Ev::Fullscreen, fullscreen ? 1 : 0});
          } else {
            push_raw({Ev::Key, ev.key.keysym.scancode, 1}, ev);
          }
          break;
        case SDL_KEYUP: push_raw({Ev::Key, ev.key.keysym.scancode, 0}, ev); break;
        case SDL_TEXTINPUT:
        case SDL_MOUSEWHEEL:
          push_raw({Ev::Sdl}, ev);
          break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
          if (ev.button.which == SDL_TOUCH_MOUSEID) break;
          int x, y;
          to_drawable(ev.button.x, ev.button.y, x, y);
          push_raw({Ev::MouseButton, ev.button.button, ev.type == SDL_MOUSEBUTTONDOWN, x, y}, ev);
          break;
        }
        case SDL_MOUSEMOTION: {
          if (ev.motion.which == SDL_TOUCH_MOUSEID) break;
          int x, y;
          to_drawable(ev.motion.x, ev.motion.y, x, y);
          push_raw({Ev::MouseMove, 0, 0, x, y}, ev);
          break;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
          const int slot = finger_slot(ev.tfinger.fingerId, ev.type == SDL_FINGERDOWN);
          if (slot < 0) break;
          int dw, dh;
          SDL_GL_GetDrawableSize(win.sdl(), &dw, &dh);
          const int action = ev.type == SDL_FINGERDOWN ? 1 : ev.type == SDL_FINGERMOTION ? 2 : 0;
          g_sh.Push({Ev::Finger, action, 10 + slot, static_cast<int>(ev.tfinger.x * dw), static_cast<int>(ev.tfinger.y * dh)});
          if (ev.type == SDL_FINGERUP) fingers[slot] = -1;
          break;
        }
        case SDL_CONTROLLERDEVICEADDED:
          if (SDL_GameController* c = SDL_GameControllerOpen(ev.cdevice.which))
            log::Info("mando conectado: {}", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
          break;
        case SDL_CONTROLLERDEVICEREMOVED:
          if (SDL_GameController* c = SDL_GameControllerFromInstanceID(ev.cdevice.which)) SDL_GameControllerClose(c);
          break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
          push_raw({Ev::PadButton, ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN}, ev);
          break;
        case SDL_CONTROLLERAXISMOTION: {
          Ev e{Ev::PadAxis, ev.caxis.axis};
          e.f = std::clamp(ev.caxis.value / 32767.0f, -1.0f, 1.0f);
          g_sh.Push(e);
          break;
        }
        default:
          if (ev.type == g_user_event && ev.user.code == kUserMinimize) SDL_MinimizeWindow(win.sdl());
          if (ev.type == g_user_event && ev.user.code == kUserFullscreen) {
            fullscreen = reinterpret_cast<std::intptr_t>(ev.user.data1) != 0;
            SDL_SetWindowFullscreen(win.sdl(), fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
          }
          if (ev.type == g_user_event && ev.user.code == kUserCursor) {
            const int size = static_cast<int>(reinterpret_cast<std::intptr_t>(ev.user.data1));
            cursor::Apply(size > 0 ? "zombie" : "system", size);
          }
          break;
      }
    } while (SDL_PollEvent(&ev));
    if (Clock::now() - last_title > std::chrono::seconds(1)) {
      last_title = Clock::now();
      const std::string t = std::format("Zombiewood - {:.0f} fps (step {:.1f} ms)", g_sh.fps.load(), g_sh.step_ms.load());
      SDL_SetWindowTitle(win.sdl(), t.c_str());
    }
  }
  gl_thread.join();
  ReportUnimplemented();
  const int rc = g_sh.rc;
  backup.Stop();
  log::Info("saliendo con codigo {}", rc);
  log::Shutdown();
  // Quedan hilos guest vivos (Vox, pool de tareas...): no se destruye nada, se termina el proceso.
  std::fflush(nullptr);
  std::_Exit(rc);
}

}  // namespace zw
