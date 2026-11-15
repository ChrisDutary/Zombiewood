// Frontend: piezas especificas de Windows (ver platform.h).
#include "frontend/platform.h"

#include <windows.h>
#include <commdlg.h>
#include <dbghelp.h>

#include <SDL_syswm.h>

#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/paths.h"

namespace zw::platform {
namespace {

// v0.2.2: informe de cierre inesperado. Si el proceso muere por una excepcion no controlada (p. ej. dentro del driver
// grafico), escribe logs/crash-<fecha>.dmp y una linea en el log con el codigo, la direccion y el modulo culpable.
// Los fallos del codigo del juego (guest) ya los maneja el runtime; esto cubre el lado host (drivers, DLLs, el port).
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
  const auto* rec = ep->ExceptionRecord;
  char module[MAX_PATH] = "?";
  HMODULE hm = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(rec->ExceptionAddress), &hm))
    GetModuleFileNameA(hm, module, MAX_PATH);
  const uintptr_t off = hm ? reinterpret_cast<uintptr_t>(rec->ExceptionAddress) - reinterpret_cast<uintptr_t>(hm) : 0;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char name[64];
  wsprintfA(name, "crash-%04d%02d%02d-%02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  const auto dmp = zw::paths::Home() / "logs" / name;
  HANDLE f = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpWithThreadInfo, &mei, nullptr, nullptr);
    CloseHandle(f);
  }
  zw::log::Error("CIERRE INESPERADO: excepcion 0x{:08X} en {}+0x{:X} (direccion {}). Informe: logs/{}", rec->ExceptionCode,
                 std::filesystem::path(module).filename().string(), off, rec->ExceptionAddress, name);
  zw::log::Shutdown();
  return EXCEPTION_CONTINUE_SEARCH;
}

HWND Hwnd(SDL_Window* w) {
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  return w && SDL_GetWindowWMInfo(w, &info) ? info.info.win.window : nullptr;
}

std::wstring Wide(const char* s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

}  // namespace

void EarlyInit() {
  SetUnhandledExceptionFilter(CrashFilter);
  // v0.2.2: el exe es una app de ventana (subsistema WINDOWS), asi Windows nunca crea una consola negra al abrirlo con
  // doble clic (antes era de consola y la soltaba con FreeConsole, pero a veces se veia un instante). Si se lanza desde
  // un terminal, se engancha a su consola para que la salida de --install/--load-only siga apareciendo ahi.
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
  // Icono de ventana/barra de tareas = recurso 101 del exe (resources/zombiewood.rc), si se incrusto. Debe ir antes de
  // SDL_Init(VIDEO), que es cuando SDL registra la clase de ventana.
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
}

void ApplyWindowIcon(SDL_Window*) {}

bool EmbeddedCursor(const std::uint8_t*& data, std::size_t& size) {
  constexpr int kResourceId = 102;  // resources/cursor.rc
  HMODULE mod = GetModuleHandleW(nullptr);
  HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(kResourceId), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
  if (!res) return false;
  HGLOBAL h = LoadResource(mod, res);
  data = static_cast<const std::uint8_t*>(h ? LockResource(h) : nullptr);
  size = SizeofResource(mod, res);
  return data != nullptr && size > 0;
}

bool HasFileDialog() { return true; }

std::vector<std::filesystem::path> OpenFileDialog(SDL_Window* owner, const char* title, const char* filter_name,
                                                  const char* filter_ext, bool multi) {
  // Filtro: "Nombre (*.ext)\0*.ext\0Todos los archivos\0*.*\0\0".
  const std::wstring ext = Wide(filter_ext);
  std::wstring filter = Wide(filter_name) + L" (*." + ext + L")";
  filter.push_back(L'\0');
  filter += L"*." + ext;
  filter.push_back(L'\0');
  filter += L"Todos los archivos";
  filter.push_back(L'\0');
  filter += L"*.*";
  filter.push_back(L'\0');
  const std::wstring wtitle = Wide(title);

  std::vector<wchar_t> buf(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = Hwnd(owner);
  ofn.lpstrFilter = filter.c_str();
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.lpstrTitle = wtitle.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT : 0);
  std::vector<std::filesystem::path> out;
  if (!GetOpenFileNameW(&ofn)) return out;
  // Multi: "carpeta\0fich1\0fich2\0\0"; uno solo: "ruta completa\0\0".
  const wchar_t* p = buf.data();
  const std::filesystem::path first(p);
  p += wcslen(p) + 1;
  if (!*p) {
    out.push_back(first);
    return out;
  }
  for (; *p; p += wcslen(p) + 1) out.push_back(first / p);
  return out;
}

}  // namespace zw::platform
