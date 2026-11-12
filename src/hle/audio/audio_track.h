// Backend de audio de Zombiewood (T6): emula android.media.AudioTrack (modo STREAM) sobre SDL2.
// C++ puro, sin JNI: T7 lo envuelve como la clase Java emulada. Ver CONTRACTS.md "Audio (T6)".
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zw::audio {

// Constantes de Android.
enum : int {
  kChannelOutMono = 4,     // CHANNEL_OUT_MONO
  kChannelOutStereo = 12,  // CHANNEL_OUT_STEREO (0xC)
  kEncodingPcm16 = 2,      // ENCODING_PCM_16BIT
  kEncodingPcm8 = 3,       // ENCODING_PCM_8BIT
  kStreamMusic = 3,
  kModeStatic = 0,
  kModeStream = 1,
  kStateStopped = 1,  // PLAYSTATE_STOPPED
  kStatePaused = 2,   // PLAYSTATE_PAUSED
  kStatePlaying = 3,  // PLAYSTATE_PLAYING
};

int native_output_sample_rate();                               // 44100
int min_buffer_size(int rate, int channel_cfg, int encoding);  // bytes, >= 8192

// ---- Control global (menu de T9, nativeVoxMute, minimizar ventana) ----
void set_master_volume(float v);  // 0..1 (se recorta)
float master_volume();
void set_muted(bool m);
bool muted();
void set_global_paused(bool p);  // pausa/reanuda TODAS las pistas (no cambia su play_state)
bool global_paused();
// Solo tests/diagnostico: fuerza el driver SDL ("dummy", "disk"...) antes de abrir la primera pista.
void set_driver(const char* name);
// Solo tests: copia (tras volumen/mute) de lo que se entrega al dispositivo (formato de la pista).
void set_tap(std::function<void(const void* data, int bytes)> fn);

class Track {
 public:
  Track(int stream_type, int rate, int channel_cfg, int encoding, int buffer_bytes, int mode = kModeStream);
  ~Track();
  Track(const Track&) = delete;
  Track& operator=(const Track&) = delete;

  void play();
  void pause();
  void stop();     // detiene y descarta la cola
  void release();  // libera el dispositivo; despierta writers bloqueados
  // Copia a la cola; bloquea mientras haya > ~100 ms en cola y la pista este sonando. Si esta en
  // pausa/parada nunca bloquea: acepta hasta un tope y devuelve los bytes aceptados. -1 si liberada.
  int write(const void* pcm, int bytes);
  int play_state() const { return state_.load(); }
  bool valid() const { return !released_.load(); }
  bool has_device() const { return dev_ != 0; }
  uint64_t bytes_consumed() const { return consumed_.load(); }
  int queued_bytes();
  int frame_bytes() const { return frame_bytes_; }

  void apply_device_state();  // interno (control global)

 private:
  static void OnAudio(void* ud, uint8_t* out, int len);
  void fill(uint8_t* out, int len);
  void fallback_loop();
  bool should_run() const;

  int rate_, channels_, bits_, frame_bytes_;
  uint32_t dev_ = 0;
  std::atomic<int> state_{kStateStopped};
  std::atomic<bool> released_{false};
  std::atomic<uint64_t> consumed_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<uint8_t> ring_;
  size_t rhead_ = 0;
  size_t max_queue_;  // ~100 ms
  size_t hard_cap_;
  std::thread fallback_;  // sin dispositivo SDL: consume a tiempo real
  std::atomic<bool> fb_stop_{false};
};

}  // namespace zw::audio
