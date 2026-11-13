#include "core/log.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "core/paths.h"

namespace zw::log {
namespace {
using Clock = std::chrono::steady_clock;

constexpr int kKeepLogs = 5;                              // zombiewood.log + .1 ... .4
constexpr std::uintmax_t kMaxBytes = 32ull * 1024 * 1024;  // rotar en caliente al pasar de 32 MiB
// Repeticiones: un mensaje identico visto mas de kBurst veces en kWindow se suprime; cada kWindow se
// escribe un resumen "(xN) mensaje". Cubre el spam del juego sin red ("Sending TapJoy request...",
// "Response not valid", ~10 lineas/s alternadas) sin perder la primera aparicion de nada.
constexpr int kBurst = 3;
constexpr auto kWindow = std::chrono::seconds(30);

std::mutex g_mu;
std::ofstream g_file;
std::uintmax_t g_bytes = 0;
bool g_dedup = true;

struct Rep {
  Clock::time_point first;
  int seen = 0;        // en la ventana actual
  int suppressed = 0;  // no escritos
  Level lvl = Level::Info;
};
std::unordered_map<std::string, Rep> g_reps;
Clock::time_point g_last_flush = Clock::now();

const char* Name(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    default: return "ERROR";
  }
}

std::filesystem::path LogPath(int i) {
  return paths::LogsDir() / (i == 0 ? std::string("zombiewood.log") : std::format("zombiewood.{}.log", i));
}

// zombiewood.log -> .1 -> .2 ... (el mas viejo se borra). Llamar con g_mu tomado y el fichero cerrado.
void RotateFiles() {
  std::error_code ec;
  std::filesystem::remove(LogPath(kKeepLogs - 1), ec);
  for (int i = kKeepLogs - 2; i >= 0; --i)
    if (std::filesystem::exists(LogPath(i), ec)) std::filesystem::rename(LogPath(i), LogPath(i + 1), ec);
}

void Emit(Level lvl, std::string_view msg) {
  using namespace std::chrono;
  auto now = floor<milliseconds>(system_clock::now());
  std::string line = std::format("[{:%H:%M:%S}] [{}] {}\n", now, Name(lvl), msg);
  FILE* out = lvl >= Level::Warn ? stderr : stdout;
  std::fwrite(line.data(), 1, line.size(), out);
  std::fflush(out);
  if (g_file.is_open()) {
    g_file << line;
    g_file.flush();
    g_bytes += line.size();
    if (g_bytes > kMaxBytes) {
      g_file.close();
      RotateFiles();
      g_file.open(LogPath(0), std::ios::trunc);
      g_bytes = 0;
    }
  }
}

// Escribe los resumenes de repeticiones cuya ventana ha vencido. g_mu tomado.
void FlushReps(bool all) {
  const auto now = Clock::now();
  for (auto it = g_reps.begin(); it != g_reps.end();) {
    if (all || now - it->second.first >= kWindow) {
      if (it->second.suppressed > 0)
        Emit(it->second.lvl, std::format("(x{} en {} s, suprimidas) {}", it->second.suppressed,
                                         std::chrono::duration_cast<std::chrono::seconds>(now - it->second.first).count(), it->first));
      it = g_reps.erase(it);
    } else {
      ++it;
    }
  }
  g_last_flush = now;
}
}  // namespace

void Init() {
  std::lock_guard lk(g_mu);
  SetConsoleOutputCP(CP_UTF8);
  std::error_code ec;
  std::filesystem::create_directories(paths::LogsDir(), ec);
  if (g_file.is_open()) g_file.close();
  RotateFiles();
  g_file.open(LogPath(0), std::ios::trunc);
  g_bytes = 0;
  if (const char* v = std::getenv("ZW_LOG_NODEDUP"); v && *v == '1') g_dedup = false;
}

void Shutdown() {
  std::lock_guard lk(g_mu);
  FlushReps(true);
  if (g_file.is_open()) g_file.close();
}

void SetDedup(bool on) {
  std::lock_guard lk(g_mu);
  g_dedup = on;
}

void Write(Level lvl, std::string_view msg) {
  std::lock_guard lk(g_mu);
  if (Clock::now() - g_last_flush > std::chrono::seconds(5)) FlushReps(false);
  if (g_dedup && lvl <= Level::Info) {
    auto [it, fresh] = g_reps.try_emplace(std::string(msg));
    Rep& r = it->second;
    if (fresh) {
      r.first = Clock::now();
      r.lvl = lvl;
    }
    if (++r.seen > kBurst) {
      ++r.suppressed;
      return;
    }
    // Evitar que el mapa crezca sin limite con mensajes unicos (p. ej. "fps ..."): solo se guardan
    // durante la ventana; FlushReps los purga.
  }
  Emit(lvl, msg);
}
}  // namespace zw::log
