// Tests de zw_hle_audio (T6): driver SDL "dummy" (sin dispositivo real, apto para CI).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "audio_hle.h"
#include "audio_track.h"
#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"

using namespace zw;
using Clock = std::chrono::steady_clock;
static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    ++g_checks;                                                      \
    if (!(cond)) {                                                   \
      ++g_failed;                                                    \
      std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

static double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::vector<int16_t> sine(int frames, int rate = 44100, double hz = 440.0, int ch = 2) {
  std::vector<int16_t> v((size_t)frames * ch);
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c)
      v[(size_t)i * ch + c] = (int16_t)(12000.0 * std::sin(2 * 3.14159265358979 * hz * i / rate));
  return v;
}

// Escribe en trozos de 1024 frames como hace el juego (UpdateThreadedAT).
static void write_all(audio::Track& t, const std::vector<int16_t>& pcm, int ch = 2) {
  const size_t chunk = 1024 * (size_t)ch;
  for (size_t o = 0; o < pcm.size(); o += chunk) {
    const int n = (int)std::min<size_t>(chunk, pcm.size() - o);
    t.write(pcm.data() + o, n * 2);
  }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  log::Init();
  audio::set_driver("dummy");

  CHECK(audio::native_output_sample_rate() == 44100);
  CHECK(audio::min_buffer_size(44100, audio::kChannelOutStereo, audio::kEncodingPcm16) >= 8192);

  // --- 1 s de seno 440 Hz: duracion (bloqueo ~100 ms de cola), contenido y volumen ---
  {
    std::atomic<int> peak{0};
    audio::set_tap([&](const void* d, int n) {
      const int16_t* s = (const int16_t*)d;
      int p = peak.load();
      for (int i = 0; i < n / 2; ++i) p = std::max(p, std::abs((int)s[i]));
      peak = p;
    });
    audio::Track t(audio::kStreamMusic, 44100, audio::kChannelOutStereo, audio::kEncodingPcm16, 8192);
    CHECK(t.has_device());
    CHECK(t.play_state() == audio::kStateStopped);
    t.play();
    CHECK(t.play_state() == audio::kStatePlaying);
    const auto t0 = Clock::now();
    write_all(t, sine(44100));
    const double dt = secs(t0);
    std::printf("  1 s de seno: write() tardo %.3f s (esperado ~0.85-1.0)\n", dt);
    CHECK(dt > 0.70 && dt < 1.6);  // bloquea: la cola no deja adelantarse mas de ~100 ms
    while (secs(t0) < 1.6 && t.queued_bytes() > 0) sleep_ms(5);
    sleep_ms(50);
    std::printf("  bytes consumidos: %llu de 176400; pico = %d\n", (unsigned long long)t.bytes_consumed(), peak.load());
    CHECK(t.bytes_consumed() >= 176400 - 4096 * 2);
    CHECK(peak.load() > 11000 && peak.load() <= 12000);

    audio::set_master_volume(0.f);  // volumen 0: al dispositivo solo llega silencio
    peak = 0;
    write_all(t, sine(8820));
    sleep_ms(150);
    CHECK(peak.load() == 0);
    audio::set_master_volume(0.5f);
    write_all(t, sine(8820));
    sleep_ms(150);
    CHECK(peak.load() > 5000 && peak.load() <= 6100);
    audio::set_master_volume(1.f);
    audio::set_muted(true);  // mute
    peak = 0;
    write_all(t, sine(8820));
    sleep_ms(150);
    CHECK(peak.load() == 0);
    audio::set_muted(false);
    t.release();
    audio::set_tap(nullptr);
  }

  // --- pausa/reanudacion: no consume en pausa, write no bloquea, reanuda ---
  {
    audio::Track t(audio::kStreamMusic, 44100, audio::kChannelOutStereo, audio::kEncodingPcm16, 8192);
    t.play();
    write_all(t, sine(2205));
    sleep_ms(100);
    t.pause();
    CHECK(t.play_state() == audio::kStatePaused);
    sleep_ms(80);  // deja terminar un callback en curso
    const auto c0 = t.bytes_consumed();
    const auto w0 = Clock::now();
    auto big = sine(44100);
    const int acc = t.write(big.data(), (int)big.size() * 2);
    CHECK(secs(w0) < 0.2);  // en pausa jamas bloquea
    CHECK(acc > 0 && acc < (int)big.size() * 2);
    sleep_ms(150);
    CHECK(t.bytes_consumed() == c0);
    t.play();
    sleep_ms(200);
    CHECK(t.bytes_consumed() > c0);
    write_all(t, sine(4410));  // la cola anterior ya se vacio
    audio::set_global_paused(true);  // pausa global (minimizar ventana)
    sleep_ms(80);
    const auto c1 = t.bytes_consumed();
    sleep_ms(150);
    CHECK(t.bytes_consumed() == c1 && t.play_state() == audio::kStatePlaying);
    audio::set_global_paused(false);
    sleep_ms(150);
    CHECK(t.bytes_consumed() > c1);
    t.stop();  // stop descarta la cola
    CHECK(t.play_state() == audio::kStateStopped && t.queued_bytes() == 0);
  }

  // --- release despierta a un writer bloqueado; write tras release = -1 ---
  {
    audio::Track t(audio::kStreamMusic, 44100, audio::kChannelOutStereo, audio::kEncodingPcm16, 8192);
    t.play();
    std::atomic<bool> done{false};
    std::thread w([&] {
      auto v = sine(44100 * 5);
      t.write(v.data(), (int)v.size() * 2);
      done = true;
    });
    sleep_ms(300);
    CHECK(!done.load());
    const auto t0 = Clock::now();
    t.release();
    w.join();
    CHECK(done.load() && secs(t0) < 1.0);
    int16_t z[4] = {};
    CHECK(t.write(z, 8) == -1 && !t.valid());
  }

  // --- mono 8 bit a 22050 Hz: SDL convierte, la pista consume ---
  {
    audio::Track t(audio::kStreamMusic, 22050, audio::kChannelOutMono, audio::kEncodingPcm8, 4096);
    CHECK(t.frame_bytes() == 1);
    t.play();
    std::vector<uint8_t> v(4410);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)(128 + 100 * std::sin(2 * 3.14159265 * 440 * (double)i / 22050));
    t.write(v.data(), (int)v.size());
    sleep_ms(400);
    CHECK(t.bytes_consumed() >= v.size());
  }

  // --- OpenSL: imports resueltos ---
  {
    GuestMemory::Init();
    hle::audio::Register();
    const char* iids[] = {"SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_BUFFERQUEUE"};
    gaddr seen[3] = {};
    for (int i = 0; i < 3; ++i) {
      gaddr var = hle::FindData(iids[i]);
      CHECK(var != 0);
      gaddr guid = mem::R32(var);
      CHECK(guid != 0 && GuestMemory::IsMapped(guid, 16));
      seen[i] = guid;
      std::printf("  %s -> %08x-%04x-%04x-%04x\n", iids[i], mem::R32(guid), mem::R16(guid + 4), mem::R16(guid + 6),
                  mem::R16(guid + 8));
    }
    CHECK(mem::R32(seen[0]) == 0x8d97c260 && mem::R32(seen[1]) == 0xef0bd9c0 && mem::R32(seen[2]) == 0x2bc99cc0);
    gaddr out = GuestMemory::Calloc(1, 4);
    mem::W32(out, 0xDEADBEEF);
    const gaddr stub = hle::StubFor("slCreateEngine");
    const uint32_t r = call_guest<uint32_t>(stub, out, 0u, 0u, 0u, 0u, 0u);
    std::printf("  slCreateEngine -> %u (SL_RESULT_FEATURE_UNSUPPORTED = 12), *pEngine = %u\n", r, mem::R32(out));
    CHECK(r == 12 && mem::R32(out) == 0);
  }

  std::printf("%s: %d comprobaciones, %d fallos\n", g_failed ? "FALLO" : "OK", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
