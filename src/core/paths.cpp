#include "core/paths.h"

#include <windows.h>

#include <cstdlib>

namespace zw::paths {

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

std::filesystem::path LogsDir() { return Home() / "logs"; }
std::filesystem::path ConfigFile() { return Home() / "zombiewood.json"; }

}  // namespace zw::paths
