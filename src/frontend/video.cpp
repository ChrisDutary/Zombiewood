#include "frontend/video.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

#include "core/log.h"

namespace zw::video {
namespace {
using Clock = std::chrono::steady_clock;

template <class T>
struct Com {
  T* p = nullptr;
  ~Com() {
    if (p) p->Release();
  }
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

struct MfSession {
  bool ok = false, com = false;
  MfSession() {
    com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~MfSession() {
    if (ok) MFShutdown();
    if (com) CoUninitialize();
  }
};

constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

// Dibuja la textura del frame en el framebuffer 0 con letterbox (blit desde un FBO de lectura).
struct GlTarget {
  GLuint tex = 0, fbo = 0;
  int w = 0, h = 0;
  GLint old_tex = 0, old_draw = 0, old_read = 0, old_unpack = 4;
  GLboolean scissor = GL_FALSE;
  GLboolean mask[4]{};
  GLfloat cc[4]{};
  GLint vp[4]{};

  void Save() {
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetIntegerv(GL_VIEWPORT, vp);
  }
  void Restore() {
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (tex) glDeleteTextures(1, &tex);
  }
  void Upload(const std::uint8_t* rgba, int fw, int fh) {
    if (!tex) {
      glGenTextures(1, &tex);
      glGenFramebuffers(1, &fbo);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (fw != w || fh != h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      w = fw;
      h = fh;
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
  }
  void Draw(int dw, int dh, bool linear) {
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!tex || !w || !h) return;
    const double sc = std::min(static_cast<double>(dw) / w, static_cast<double>(dh) / h);
    const int rw = static_cast<int>(w * sc + 0.5), rh = static_cast<int>(h * sc + 0.5);
    const int rx = (dw - rw) / 2, ry = (dh - rh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    // La imagen de MF va de arriba abajo: invertir Y en el blit.
    glBlitFramebuffer(0, 0, w, h, rx, ry + rh, rx + rw, ry, GL_COLOR_BUFFER_BIT, linear ? GL_LINEAR : GL_NEAREST);
  }
};

}  // namespace

Result Play(const std::filesystem::path& file, const PlayParams& p) {
  const auto t0 = Clock::now();
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    log::Warn("video: no existe {} (se salta)", file.string());
    return Result::Failed;
  }
  MfSession mf;
  if (!mf.ok) {
    log::Warn("video: Media Foundation no disponible (Windows N sin Media Feature Pack?): se salta");
    return Result::Failed;
  }
  Com<IMFAttributes> attr;
  MFCreateAttributes(&attr, 2);
  attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // conversion YUV -> RGB32 por software
  Com<IMFSourceReader> rd;
  HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), attr.p, &rd);
  if (FAILED(hr)) {
    log::Warn("video: no se puede abrir {} (0x{:08x})", file.string(), static_cast<unsigned>(hr));
    return Result::Failed;
  }
  rd->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  rd->SetStreamSelection(kVideo, TRUE);
  {
    Com<IMFMediaType> mt;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr = rd->SetCurrentMediaType(kVideo, nullptr, mt.p))) {
      log::Warn("video: sin decodificador H.264 -> RGB32 (0x{:08x})", static_cast<unsigned>(hr));
      return Result::Failed;
    }
  }
  UINT32 vw = 0, vh = 0;
  LONG stride = 0;
  {
    Com<IMFMediaType> cur;
    rd->GetCurrentMediaType(kVideo, &cur);
    MFGetAttributeSize(cur.p, MF_MT_FRAME_SIZE, &vw, &vh);
    UINT32 s = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = static_cast<LONG>(s);
    else stride = static_cast<LONG>(vw * 4);
  }
  // Audio (opcional), con un lector PROPIO (v0.2.2): asi el audio se decodifica por delante sin depender de como
  // esten intercalados audio y video en el fichero (con un solo lector, el audio se quedaba sin datos: el logo sonaba
  // entrecortado, 246 cortes medidos en 12 s).
  Com<IMFSourceReader> ra;
  bool has_audio = false;
  int a_rate = 44100, a_ch = 2;
  SDL_AudioDeviceID dev = 0;
  if (SUCCEEDED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &ra))) {
    ra->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    has_audio = SUCCEEDED(ra->SetStreamSelection(kAudio, TRUE));
  }
  if (has_audio) {
    Com<IMFMediaType> mt;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(ra->SetCurrentMediaType(kAudio, nullptr, mt.p))) {
      has_audio = false;
    } else {
      Com<IMFMediaType> cur;
      ra->GetCurrentMediaType(kAudio, &cur);
      a_rate = static_cast<int>(MFGetAttributeUINT32(cur.p, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100));
      a_ch = static_cast<int>(MFGetAttributeUINT32(cur.p, MF_MT_AUDIO_NUM_CHANNELS, 2));
      SDL_AudioSpec want{}, got{};
      want.freq = a_rate;
      want.format = AUDIO_S16SYS;
      want.channels = static_cast<Uint8>(a_ch);
      want.samples = 2048;
      if (SDL_WasInit(SDL_INIT_AUDIO)) dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
      if (!dev) log::Warn("video: sin dispositivo de audio ({}), video sin sonido", SDL_GetError());
    }
  }
  log::Info("video: {} {}x{} (stride {}), audio {}", file.filename().string(), vw, vh, stride,
            has_audio ? std::format("{} Hz x{}", a_rate, a_ch) : std::string("no"));
  if (!vw || !vh) return Result::Failed;

  GlTarget gl;
  gl.Save();
  const float vol = std::clamp(p.volume, 0.0f, 1.0f);
  Result result = Result::Finished;
  bool video_eos = false, audio_eos = !has_audio || !dev || !ra, clock_started = false;
  Clock::time_point start;
  long long frames = 0;
  int dw = 0, dh = 0;

  // v0.2.2: lectura adelantada. Antes el audio se leia "justo a tiempo" (unos ms por delante del frame) y el
  // dispositivo arrancaba casi vacio: con el PC ocupado arrancando el juego (JIT), el audio del logo sonaba
  // entrecortado. Ahora se decodifica por delante (hasta ~0.5 s de audio y kMaxFrames frames) y el reloj/sonido no
  // arranca hasta tener ~250 ms de audio en cola.
  constexpr std::size_t kMaxFrames = 8;
  constexpr int kAudioAheadMs = 500, kPrerollMs = 250;
  struct Frame {
    LONGLONG ts;
    std::vector<std::uint8_t> rgba;
  };
  std::deque<Frame> queue;
  int audio_dropouts = 0;
  bool audio_starved = false;
  std::vector<std::vector<std::uint8_t>> pool;  // buffers reutilizables (evita reservar 3 MB por frame)
  const int bytes_per_ms = dev ? a_rate * a_ch * 2 / 1000 : 1;
  auto audio_ms = [&] { return dev ? static_cast<int>(SDL_GetQueuedAudioSize(dev)) / std::max(1, bytes_per_ms) : 0; };

  while (true) {
    if (p.should_skip && p.should_skip()) {
      result = Result::Skipped;
      break;
    }
    bool did_work = false;
    // 1a) Audio: mantener ~0.5 s decodificado por delante (lector independiente).
    while (!audio_eos && audio_ms() < kAudioAheadMs) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      Com<IMFSample> sample;
      if (FAILED(ra->ReadSample(kAudio, 0, nullptr, &flags, &ts, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        audio_eos = true;
        break;
      }
      did_work = true;
      if (!sample) continue;
      Com<IMFMediaBuffer> buf;
      BYTE* data = nullptr;
      DWORD len = 0;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
        if (vol < 0.999f) {
          auto* s = reinterpret_cast<std::int16_t*>(data);
          for (DWORD i = 0; i < len / 2; ++i) s[i] = static_cast<std::int16_t>(s[i] * vol);
        }
        SDL_QueueAudio(dev, data, len);
        buf->Unlock();
      }
    }
    // 1b) Video: decodificar unos frames por delante.
    if (!video_eos && queue.size() < kMaxFrames) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      Com<IMFSample> sample;
      hr = rd->ReadSample(kVideo, 0, nullptr, &flags, &ts, &sample);
      if (FAILED(hr)) {
        log::Warn("video: ReadSample fallo (0x{:08x})", static_cast<unsigned>(hr));
        if (!frames && queue.empty()) result = Result::Failed;
        video_eos = true;
      } else {
        did_work = true;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
          video_eos = true;
        } else if (sample) {
          Com<IMFMediaBuffer> buf;
          BYTE* data = nullptr;
          DWORD len = 0;
          if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
            {
              Frame f{ts, {}};
              if (!pool.empty()) {
                f.rgba = std::move(pool.back());
                pool.pop_back();
              }
              f.rgba.resize(static_cast<std::size_t>(vw) * vh * 4);
              // RGB32 de MF = BGRX en memoria -> RGBA.
              const LONG abs_stride = stride < 0 ? -stride : stride;
              for (UINT32 y = 0; y < vh; ++y) {
                const BYTE* src = data + static_cast<std::size_t>(stride < 0 ? (vh - 1 - y) : y) * abs_stride;
                if (static_cast<std::size_t>(src - data) + vw * 4 > len) break;
                std::uint8_t* dst = &f.rgba[static_cast<std::size_t>(y) * vw * 4];
                for (UINT32 x = 0; x < vw; ++x) {
                  dst[0] = src[2];
                  dst[1] = src[1];
                  dst[2] = src[0];
                  dst[3] = 255;
                  src += 4;
                  dst += 4;
                }
              }
              queue.push_back(std::move(f));
            }
            buf->Unlock();
          }
        }
      }
    }
    // 2) Arrancar reloj y sonido cuando haya colchon de audio y al menos un frame (o no pueda haber mas).
    if (!clock_started && (!queue.empty() || video_eos) && (audio_eos || audio_ms() >= kPrerollMs)) {
      clock_started = true;
      start = Clock::now() - std::chrono::microseconds(queue.empty() ? 0 : queue.front().ts / 10);
      if (dev) SDL_PauseAudioDevice(dev, 0);
    }
    // 3) Presentar el frame que toque.
    if (clock_started && !queue.empty() && Clock::now() >= start + std::chrono::microseconds(queue.front().ts / 10)) {
      Frame& f = queue.front();
      SDL_GL_GetDrawableSize(p.window, &dw, &dh);
      gl.Upload(f.rgba.data(), static_cast<int>(vw), static_cast<int>(vh));
      gl.Draw(dw, dh, p.linear_filter);
      if (p.overlay) p.overlay(dw, dh);
      SDL_GL_SwapWindow(p.window);
      ++frames;
      pool.push_back(std::move(f.rgba));
      queue.pop_front();
      did_work = true;
    }
    // Diagnostico: el dispositivo se quedo sin audio antes de terminar el stream (= corte audible).
    if (clock_started && dev && !audio_eos) {
      const bool empty = SDL_GetQueuedAudioSize(dev) == 0;
      if (empty && !audio_starved) ++audio_dropouts;
      audio_starved = empty;
    }
    // 4) Fin: todo leido y todos los frames mostrados (el audio pendiente se drena abajo).
    if (video_eos && audio_eos && queue.empty()) break;
    if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Dejar que termine el audio encolado (si no se salto).
  if (dev) {
    while (result == Result::Finished && SDL_GetQueuedAudioSize(dev) > 0) {
      if (p.should_skip && p.should_skip()) {
        result = Result::Skipped;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SDL_CloseAudioDevice(dev);
  }
  gl.Restore();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  log::Info("video: {} {} ({} frames en {:.1f} s, {} cortes de audio)", file.filename().string(),
            result == Result::Finished ? "terminado" : result == Result::Skipped ? "saltado" : "fallo", frames, secs,
            audio_dropouts);
  return result;
}

}  // namespace zw::video
