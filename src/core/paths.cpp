#include "core/paths.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <string>
#endif

namespace zw::paths {

#ifdef _WIN32

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::filesystem::path Home() {
  wchar_t* v = nullptr;
  size_t len = 0;
  if (_wdupenv_s(&v, &len, L"ZOMBIEWOOD_HOME") == 0 && v && *v) {
    std::filesystem::path p(v);
    free(v);
    return p;
  }
  free(v);
  return ExeDir();
}

bool IsPortable() { return true; }

void OpenFolder(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  ShellExecuteW(nullptr, L"explore", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else  // Linux

namespace {
const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? v : nullptr;
}

// Carpeta de datos del usuario segun XDG: $XDG_DATA_HOME/zombiewood o ~/.local/share/zombiewood.
std::filesystem::path XdgDataDir() {
  if (const char* x = Env("XDG_DATA_HOME")) return std::filesystem::path(x) / "zombiewood";
  if (const char* h = Env("HOME")) return std::filesystem::path(h) / ".local" / "share" / "zombiewood";
  return std::filesystem::temp_directory_path() / "zombiewood";
}

struct HomeInfo {
  std::filesystem::path dir;
  bool portable = false;
};

// 1) $ZOMBIEWOOD_HOME. 2) Dentro de un AppImage ($APPIMAGE/$APPDIR: el ejecutable vive en un squashfs de solo
// lectura) o si la carpeta del ejecutable no se puede escribir (/usr/bin, /opt...): carpeta XDG. 3) Si no, la
// carpeta del ejecutable (modo portable, como en Windows: el .tar.gz extraido).
const HomeInfo& ResolveHome() {
  static const HomeInfo info = [] {
    HomeInfo h;
    if (const char* v = Env("ZOMBIEWOOD_HOME")) {
      h.dir = v;
      return h;
    }
    const auto exe = ExeDir();
    if (!Env("APPIMAGE") && !Env("APPDIR") && !exe.empty() && access(exe.c_str(), W_OK) == 0) {
      h.dir = exe;
      h.portable = true;
      return h;
    }
    h.dir = XdgDataDir();
    return h;
  }();
  return info;
}
}  // namespace

std::filesystem::path ExeDir() {
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return std::filesystem::current_path();
  return std::filesystem::path(std::string(buf, static_cast<std::size_t>(n))).parent_path();
}

std::filesystem::path Home() { return ResolveHome().dir; }
bool IsPortable() { return ResolveHome().portable; }

void OpenFolder(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  // xdg-open en un proceso nieto (doble fork: no deja zombis ni bloquea el juego).
  const pid_t pid = fork();
  if (pid == 0) {
    if (fork() == 0) {
      // Dentro de un AppImage, LD_LIBRARY_PATH apunta a las bibliotecas empaquetadas: no heredarlo.
      unsetenv("LD_LIBRARY_PATH");
      execlp("xdg-open", "xdg-open", p.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    _exit(0);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
}

#endif

std::filesystem::path LogsDir() { return Home() / "logs"; }
std::filesystem::path ConfigFile() { return Home() / "zombiewood.json"; }
std::filesystem::path DefaultGameDir() { return Home() / "data"; }
std::filesystem::path SavesDir() { return Home() / "saves"; }

}  // namespace zw::paths
