#include "core/log.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

#include "core/paths.h"

namespace zw::log {
namespace {
std::mutex g_mu;
std::ofstream g_file;
const char* Name(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    default: return "ERROR";
  }
}
}  // namespace

void Init() {
  std::lock_guard lk(g_mu);
  SetConsoleOutputCP(CP_UTF8);
  std::error_code ec;
  std::filesystem::create_directories(paths::LogsDir(), ec);
  g_file.open(paths::LogsDir() / "zombiewood.log", std::ios::trunc);
}

void Write(Level lvl, std::string_view msg) {
  using namespace std::chrono;
  auto now = floor<milliseconds>(system_clock::now());
  std::string line = std::format("[{:%H:%M:%S}] [{}] {}\n", now, Name(lvl), msg);
  std::lock_guard lk(g_mu);
  FILE* out = lvl >= Level::Warn ? stderr : stdout;
  std::fwrite(line.data(), 1, line.size(), out);
  std::fflush(out);
  if (g_file.is_open()) {
    g_file << line;
    g_file.flush();
  }
}
}  // namespace zw::log
