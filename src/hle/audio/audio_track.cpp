#include "audio_track.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <list>
#include <string>

#include "core/log.h"

namespace zw::audio {
namespace {
std::atomic<float> g_vol{1.0f};
std::atomic<bool> g_muted{false};
std::atomic<bool> g_gpaused{false};
std::atomic<int> g_latency_ms{150};
std::atomic<uint64_t> g_underruns{0};
std::mutex g_mu;  // protege g_tracks, g_driver, g_tap
std::list<Track*> g_tracks;
std::string g_driver;
std::function<void(const void*, int)> g_tap;
bool g_sdl_ready = false;

bool ensure_sdl() {
  std::lock_guard lk(g_mu);
  if (!g_driver.empty()) SDL_setenv("SDL_AUDIODRIVER", g_driver.c_str(), 1);
  if (!g_sdl_ready) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      log::Warn("audio: SDL_InitSubSystem(AUDIO) fallo: {}", SDL_GetError());
      return false;
    }
    g_sdl_ready = true;
  }
  return true;
}

void refresh_all() {
  std::lock_guard lk(g_mu);
  for (Track* t : g_tracks) t->apply_device_state();
}
}  // namespace

int native_output_sample_rate() { return 44100; }
int min_buffer_size(int rate, int channel_cfg, int encoding) {
  const int ch = channel_cfg == kChannelOutMono ? 1 : 2;
  const int bps = encoding == kEncodingPcm8 ? 1 : 2;
  const int frames = std::max(1024, rate / 20);  // ~50 ms
  return std::max(8192, frames * ch * bps);
}

void set_master_volume(float v) { g_vol = std::clamp(v, 0.f, 1.f); }
float master_volume() { return g_vol.load(); }
void set_muted(bool m) { g_muted = m; }
bool muted() { return g_muted.load(); }
void set_global_paused(bool p) {
  g_gpaused = p;
  refresh_all();
}
bool global_paused() { return g_gpaused.load(); }
void set_latency_ms(int ms) { g_latency_ms = std::clamp(ms, 40, 500); }
int latency_ms() { return g_latency_ms.load(); }
uint64_t underrun_count() { return g_underruns.load(); }
void set_driver(const char* name) {
  std::lock_guard lk(g_mu);
  g_driver = name ? name : "";
}
void set_tap(std::function<void(const void*, int)> fn) {
  std::lock_guard lk(g_mu);
  g_tap = std::move(fn);
}

Track::Track(int, int rate, int channel_cfg, int encoding, int buffer_bytes, int) {
  rate_ = rate > 0 ? rate : 44100;
  channels_ = channel_cfg == kChannelOutMono || channel_cfg == 2 ? 1 : 2;
  bits_ = encoding == kEncodingPcm8 ? 8 : 16;
  frame_bytes_ = channels_ * bits_ / 8;
  const int lat = g_latency_ms.load();
  max_queue_ = (size_t)rate_ * (size_t)lat / 1000 * frame_bytes_;
  hard_cap_ = std::max<size_t>(max_queue_ * 2, (size_t)std::max(buffer_bytes, 0));
  prime_bytes_ = max_queue_ / 2;  // reanudar con media cola llena
  if (ensure_sdl()) {
    SDL_AudioSpec want{}, have{};
    want.freq = rate_;
    want.format = bits_ == 8 ? AUDIO_U8 : AUDIO_S16SYS;
    want.channels = (Uint8)channels_;
    // Bloque del dispositivo: 1024 frames (~23 ms) en latencia baja; 2048 (~46 ms) en normal/segura.
    want.samples = lat < 120 ? 1024 : 2048;
    want.callback = &Track::OnAudio;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL convierte formato/frecuencia
    if (!dev_) log::Warn("audio: SDL_OpenAudioDevice fallo: {} (fallback silencioso a tiempo real)", SDL_GetError());
    else log::Info("audio: latencia {} ms, bloque del dispositivo {} frames ({} Hz)", lat, have.samples, have.freq);
  }
  if (!dev_) fallback_ = std::thread(&Track::fallback_loop, this);
  std::lock_guard lk(g_mu);
  g_tracks.push_back(this);
}

Track::~Track() {
  release();
  std::lock_guard lk(g_mu);
  g_tracks.remove(this);
}

bool Track::should_run() const { return state_.load() == kStatePlaying && !g_gpaused.load() && !released_.load(); }

void Track::apply_device_state() {
  if (dev_) SDL_PauseAudioDevice(dev_, should_run() ? 0 : 1);
  cv_.notify_all();
}

void Track::play() {
  if (released_.load()) return;
  state_ = kStatePlaying;
  apply_device_state();
}
void Track::pause() {
  if (state_.load() == kStatePlaying) state_ = kStatePaused;
  apply_device_state();
}
void Track::stop() {
  state_ = kStateStopped;
  {
    std::lock_guard lk(mu_);
    ring_.clear();
    rhead_ = 0;
    primed_ = false;
  }
  apply_device_state();
}
void Track::release() {
  if (released_.exchange(true)) return;
  state_ = kStateStopped;
  cv_.notify_all();
  if (dev_) {
    SDL_PauseAudioDevice(dev_, 1);
    SDL_CloseAudioDevice(dev_);  // espera al fin del callback en curso
    dev_ = 0;
  }
  fb_stop_ = true;
  if (fallback_.joinable()) fallback_.join();
}

int Track::queued_bytes() {
  std::lock_guard lk(mu_);
  return (int)(ring_.size() - rhead_);
}

int Track::write(const void* pcm, int bytes) {
  if (released_.load()) return -1;
  if (bytes <= 0 || !pcm) return 0;
  const uint8_t* p = (const uint8_t*)pcm;
  bytes -= bytes % frame_bytes_;
  int done = 0;
  std::unique_lock lk(mu_);
  while (done < bytes) {
    if (released_.load()) return done;
    const size_t q = ring_.size() - rhead_;
    const bool running = should_run();
    const size_t limit = running ? max_queue_ : hard_cap_;
    if (q >= limit) {
      if (!running) return done;  // pausada/parada: no bloquear jamas
      cv_.wait_for(lk, std::chrono::milliseconds(20));
      continue;
    }
    const size_t n = std::min<size_t>(limit - q, (size_t)(bytes - done));
    if (rhead_ > 0 && rhead_ >= ring_.size() / 2) {
      ring_.erase(ring_.begin(), ring_.begin() + (std::ptrdiff_t)rhead_);
      rhead_ = 0;
    }
    ring_.insert(ring_.end(), p + done, p + done + n);
    done += (int)n;
    last_write_ = std::chrono::steady_clock::now();
  }
  return done;
}

void Track::fill(uint8_t* out, int len) {
  int got = 0;
  {
    std::lock_guard lk(mu_);
    const size_t q = ring_.size() - rhead_;
    // Cebado: tras arrancar o quedarse sin datos, esperar a tener media cola antes de sonar. Asi un PC
    // cargado produce un silencio breve en vez de audio entrecortado (antes sonaba "lageado" al arrancar).
    // Si el juego dejo de escribir (fin de un sonido, pausa), lo que quede se reproduce igual.
    const auto idle = std::chrono::steady_clock::now() - last_write_;
    if (!primed_ && (q >= prime_bytes_ || (q > 0 && idle > std::chrono::milliseconds(60)))) primed_ = true;
    got = primed_ ? (int)std::min<size_t>(q, (size_t)len) : 0;
    if (primed_ && got < len) {
      primed_ = false;  // se vacio la cola: volver a cebar
      if (should_run()) g_underruns.fetch_add(1);
    }
    if (got > 0) {
      std::memcpy(out, ring_.data() + rhead_, (size_t)got);
      rhead_ += (size_t)got;
      if (rhead_ == ring_.size()) {
        ring_.clear();
        rhead_ = 0;
      }
    }
  }
  if (got < len) std::memset(out + got, bits_ == 8 ? 0x80 : 0, (size_t)(len - got));  // underrun -> silencio
  consumed_ += (uint64_t)got;
  const float v = g_muted.load() ? 0.f : g_vol.load();
  if (v < 1.f) {
    if (bits_ == 16) {
      auto* s = (int16_t*)out;
      for (int i = 0; i < len / 2; ++i) s[i] = (int16_t)((float)s[i] * v);
    } else {
      for (int i = 0; i < len; ++i) out[i] = (uint8_t)(128 + (int)((float)(out[i] - 128) * v));
    }
  }
  {
    std::lock_guard lk(g_mu);
    if (g_tap) g_tap(out, len);
  }
  cv_.notify_all();
}

void Track::OnAudio(void* ud, uint8_t* out, int len) { static_cast<Track*>(ud)->fill(out, len); }

void Track::fallback_loop() {
  std::vector<uint8_t> tmp;
  auto last = std::chrono::steady_clock::now();
  while (!fb_stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (!should_run()) continue;
    const int n = (int)(dt * rate_) * frame_bytes_;
    if (n <= 0) continue;
    tmp.assign((size_t)n, 0);
    fill(tmp.data(), n);
  }
}

}  // namespace zw::audio
