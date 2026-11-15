#include "core/platform.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>
#include <unistd.h>
#endif

namespace zw::plat {

bool LocalTime(std::int64_t t, std::tm& out) {
#ifdef _WIN32
  const __time64_t tt = t;
  return _localtime64_s(&out, &tt) == 0;
#else
  const time_t tt = static_cast<time_t>(t);
  return localtime_r(&tt, &out) != nullptr;
#endif
}

bool GmTime(std::int64_t t, std::tm& out) {
#ifdef _WIN32
  const __time64_t tt = t;
  return _gmtime64_s(&out, &tt) == 0;
#else
  const time_t tt = static_cast<time_t>(t);
  return gmtime_r(&tt, &out) != nullptr;
#endif
}

std::int64_t TimeGm(std::tm& t) {
#ifdef _WIN32
  return _mkgmtime64(&t);
#else
  return static_cast<std::int64_t>(timegm(&t));
#endif
}

std::FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
  wchar_t wmode[8] = {};
  for (int i = 0; i < 7 && mode[i]; ++i) wmode[i] = static_cast<wchar_t>(mode[i]);
  return _wfopen(p.c_str(), wmode);
#else
  return std::fopen(p.c_str(), mode);
#endif
}

int Seek64(std::FILE* f, std::int64_t off, int whence) {
#ifdef _WIN32
  return _fseeki64(f, off, whence);
#else
  return fseeko(f, static_cast<off_t>(off), whence);
#endif
}

std::int64_t Tell64(std::FILE* f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return static_cast<std::int64_t>(ftello(f));
#endif
}

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

int StrCaseCmp(const char* a, const char* b) {
#ifdef _WIN32
  return _stricmp(a, b);
#else
  return strcasecmp(a, b);
#endif
}

int StrNCaseCmp(const char* a, const char* b, std::size_t n) {
#ifdef _WIN32
  return _strnicmp(a, b, n);
#else
  return strncasecmp(a, b, n);
#endif
}

std::uint64_t TotalRamBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX ms{sizeof ms};
  return GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
  return pages > 0 && psz > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(psz) : 0;
#endif
}

std::uint64_t AvailRamBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX ms{sizeof ms};
  return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : 0;
#else
  const long pages = sysconf(_SC_AVPHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
  return pages > 0 && psz > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(psz) : 0;
#endif
}

const char* Name() {
#ifdef _WIN32
  return "Windows x64";
#else
  return "Linux x86_64";
#endif
}

}  // namespace zw::plat
