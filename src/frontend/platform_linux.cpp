// Frontend: piezas especificas de Linux (ver platform.h).
#include "frontend/platform.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "core/log.h"
#include "core/paths.h"
#include "core/platform.h"

// Generados en el build por tools/embed/zw_embed.cpp (tamano 0 si no habia arte en resources/).
extern const unsigned char zw_embedded_cursor[];
extern const unsigned zw_embedded_cursor_size;
extern const unsigned char zw_embedded_icon_rgba[];
extern const unsigned zw_embedded_icon_rgba_size;
extern const int zw_embedded_icon_size;

namespace zw::platform {
namespace {

// ---------------------------------------------------------------------------------------------
// Informe de cierre inesperado: equivalente al minidump de Windows. Escribe logs/crash-<fecha>.txt con la senal,
// la direccion, el modulo culpable, un backtrace y el mapa de memoria, y deja una linea en zombiewood.log.
// Cadena de manejadores de SIGSEGV: dynarmic (fastmem) -> cpu.cpp (estado del guest) -> este -> accion por defecto.
const char* SigName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "senal";
  }
}

void WriteStr(int fd, const std::string& s) {
  const ssize_t r = ::write(fd, s.data(), s.size());
  (void)r;
}

void CrashSignal(int sig, siginfo_t* info, void* raw) {
  static std::atomic<bool> once{false};
  if (!once.exchange(true)) {
    const auto* uc = static_cast<const ucontext_t*>(raw);
    void* rip = uc ? reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]) : nullptr;
    void* addr = info ? info->si_addr : nullptr;

    std::string module = "?";
    std::uintptr_t off = 0;
    Dl_info dl{};
    if (rip && dladdr(rip, &dl) && dl.dli_fname) {
      module = std::filesystem::path(dl.dli_fname).filename().string();
      off = reinterpret_cast<std::uintptr_t>(rip) - reinterpret_cast<std::uintptr_t>(dl.dli_fbase);
    }

    std::tm tm{};
    plat::LocalTime(static_cast<std::int64_t>(std::time(nullptr)), tm);
    char name[64];
    std::strftime(name, sizeof name, "crash-%Y%m%d-%H%M%S.txt", &tm);
    std::error_code ec;
    std::filesystem::create_directories(paths::LogsDir(), ec);
    const auto file = paths::LogsDir() / name;
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      WriteStr(fd, std::format("Zombiewood ({}) - cierre inesperado\nsenal: {} ({}), codigo {}\ndireccion del fallo: {}\n"
                               "rip: {} = {}+0x{:X}\n\n",
                               plat::Name(), SigName(sig), sig, info ? info->si_code : 0, addr, rip, module, off));
      if (uc) {
        const auto reg = [uc](int r) { return static_cast<std::uint64_t>(uc->uc_mcontext.gregs[r]); };
        WriteStr(fd, std::format("rax {:016x} rbx {:016x} rcx {:016x} rdx {:016x}\nrsi {:016x} rdi {:016x} rbp {:016x} rsp {:016x}\n"
                                 "r8  {:016x} r9  {:016x} r10 {:016x} r11 {:016x}\nr12 {:016x} r13 {:016x} r14 {:016x} r15 {:016x}\n\n",
                                 reg(REG_RAX), reg(REG_RBX), reg(REG_RCX), reg(REG_RDX), reg(REG_RSI), reg(REG_RDI),
                                 reg(REG_RBP), reg(REG_RSP), reg(REG_R8), reg(REG_R9), reg(REG_R10), reg(REG_R11),
                                 reg(REG_R12), reg(REG_R13), reg(REG_R14), reg(REG_R15)));
      }
      WriteStr(fd, "backtrace (hilo que fallo; resolver con addr2line -e zombiewood <offset>):\n");
      void* frames[64];
      const int n = backtrace(frames, 64);
      backtrace_symbols_fd(frames, n, fd);
      WriteStr(fd, "\n/proc/self/maps:\n");
      if (const int maps = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC); maps >= 0) {
        char buf[4096];
        for (ssize_t r; (r = ::read(maps, buf, sizeof buf)) > 0;) {
          const ssize_t w = ::write(fd, buf, static_cast<std::size_t>(r));
          (void)w;
        }
        ::close(maps);
      }
      ::close(fd);
    }
    log::Error("CIERRE INESPERADO: {} en {}+0x{:X} (direccion {}). Informe: logs/{}", SigName(sig), module, off, addr, name);
    log::Shutdown();
  }
  // Accion por defecto y volver: la instruccion se repite (o abort() relanza) y el proceso muere con la senal original.
  signal(sig, SIG_DFL);
  if (sig == SIGABRT) raise(sig);
}

void InstallCrashHandler() {
  // Pila alternativa para el hilo principal: permite informar tambien de un desbordamiento de pila.
  static char alt_stack[64 * 1024];
  stack_t ss{};
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof alt_stack;
  sigaltstack(&ss, nullptr);
  struct sigaction sa {};
  sa.sa_sigaction = CrashSignal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
  // backtrace() carga libgcc la primera vez (malloc): hacerlo ahora y no dentro del manejador.
  void* warm[2];
  backtrace(warm, 2);
}

// ---------------------------------------------------------------------------------------------
// Dialogos de archivo: zenity (GNOME y la mayoria) o kdialog (KDE), si estan instalados.
bool InPath(const char* exe) {
  const char* path = std::getenv("PATH");
  if (!path) return false;
  std::string p = path;
  for (std::size_t i = 0; i <= p.size();) {
    std::size_t j = p.find(':', i);
    if (j == std::string::npos) j = p.size();
    const std::string full = p.substr(i, j - i) + "/" + exe;
    if (::access(full.c_str(), X_OK) == 0) return true;
    i = j + 1;
  }
  return false;
}

enum class Tool { None, Zenity, Kdialog };
Tool DialogTool() {
  static const Tool t = [] {
    // Sin servidor grafico (p. ej. --hidden por ssh) no tiene sentido lanzarlos.
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return Tool::None;
    const char* desk = std::getenv("XDG_CURRENT_DESKTOP");
    const bool kde = desk && std::strstr(desk, "KDE");
    if (kde && InPath("kdialog")) return Tool::Kdialog;
    if (InPath("zenity")) return Tool::Zenity;
    if (InPath("kdialog")) return Tool::Kdialog;
    return Tool::None;
  }();
  return t;
}

// Ejecuta argv y devuelve su stdout. ok = termino con codigo 0 (Aceptar).
std::string RunCapture(const std::vector<std::string>& args, bool& ok) {
  ok = false;
  int fds[2];
  if (pipe(fds) != 0) return {};
  const pid_t pid = fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return {};
  }
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    // Dentro de un AppImage LD_LIBRARY_PATH apunta a las bibliotecas empaquetadas: las herramientas del sistema
    // deben usar las suyas.
    unsetenv("LD_LIBRARY_PATH");
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  ::close(fds[1]);
  std::string out;
  char buf[4096];
  for (ssize_t r; (r = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<std::size_t>(r));
  ::close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return out;
}

}  // namespace

void EarlyInit() {
  InstallCrashHandler();
  // Mapeos extra de mandos: gamecontrollerdb.txt junto al ejecutable (tar.gz y AppImage). El de <home>, si existe,
  // lo carga game.cpp despues. SDL trae ademas sus mapeos internos para Linux.
  std::error_code ec;
  if (const auto db = paths::ExeDir() / "gamecontrollerdb.txt"; std::filesystem::exists(db, ec) && !std::getenv("SDL_GAMECONTROLLERCONFIG_FILE"))
    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, db.c_str());
  // Nombre de la app para el compositor (agrupa la ventana con el .desktop del AppImage) y para PulseAudio/PipeWire.
  SDL_SetHint(SDL_HINT_APP_NAME, "Zombiewood");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Zombiewood");
  if (!std::getenv("SDL_VIDEO_X11_WMCLASS")) plat::SetEnv("SDL_VIDEO_X11_WMCLASS", "zombiewood");
  if (!std::getenv("SDL_VIDEO_WAYLAND_WMCLASS")) plat::SetEnv("SDL_VIDEO_WAYLAND_WMCLASS", "zombiewood");
}

void ApplyWindowIcon(SDL_Window* w) {
  const int n = zw_embedded_icon_size;
  if (!w || n <= 0 || zw_embedded_icon_rgba_size != static_cast<unsigned>(n) * n * 4) return;
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<unsigned char*>(zw_embedded_icon_rgba), n, n, 32, n * 4,
                                                      SDL_PIXELFORMAT_RGBA32);
  if (!s) return;
  SDL_SetWindowIcon(w, s);  // X11: _NET_WM_ICON. Wayland: lo ignora (alli manda el .desktop del AppImage).
  SDL_FreeSurface(s);
}

bool EmbeddedCursor(const std::uint8_t*& data, std::size_t& size) {
  if (zw_embedded_cursor_size == 0) return false;
  data = zw_embedded_cursor;
  size = zw_embedded_cursor_size;
  return true;
}

bool HasFileDialog() { return DialogTool() != Tool::None; }

std::vector<std::filesystem::path> OpenFileDialog(SDL_Window*, const char* title, const char* filter_name, const char* filter_ext,
                                                  bool multi) {
  std::vector<std::filesystem::path> out;
  const Tool tool = DialogTool();
  if (tool == Tool::None) return out;
  const std::string ext = filter_ext;
  std::string upper = ext;
  for (char& c : upper)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  std::vector<std::string> args;
  if (tool == Tool::Zenity) {
    args = {"zenity", "--file-selection", std::string("--title=") + title,
            std::format("--file-filter={} (*.{}) | *.{} *.{}", filter_name, ext, ext, upper), "--file-filter=Todos los archivos | *"};
    if (multi) {
      args.push_back("--multiple");
      args.push_back("--separator=\n");
    }
  } else {
    args = {"kdialog", "--title", title, "--getopenfilename", ".", std::format("{} (*.{} *.{})", filter_name, ext, upper)};
    if (multi) {
      args.push_back("--multiple");
      args.push_back("--separate-output");
    }
  }
  bool ok = false;
  const std::string res = RunCapture(args, ok);
  if (!ok) return out;
  for (std::size_t i = 0; i < res.size();) {
    std::size_t j = res.find('\n', i);
    if (j == std::string::npos) j = res.size();
    if (j > i) out.emplace_back(res.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

}  // namespace zw::platform
