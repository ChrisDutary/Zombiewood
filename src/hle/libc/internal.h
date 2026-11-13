// Utilidades internas compartidas por los ficheros de zw_hle_libc (no es API publica).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"

namespace zw::hle::libc {

// errno de Linux/bionic (no usar los del host).
namespace err {
inline constexpr int PERM = 1, NOENT = 2, SRCH = 3, INTR = 4, IO = 5, BADF = 9, CHILD = 10, AGAIN = 11, NOMEM = 12,
                     ACCES = 13, FAULT = 14, BUSY = 16, EXIST = 17, XDEV = 18, NOTDIR = 20, ISDIR = 21, INVAL = 22,
                     NFILE = 23, MFILE = 24, NOTTY = 25, FBIG = 27, NOSPC = 28, SPIPE = 29, ROFS = 30, PIPE = 32,
                     DOM = 33, RANGE = 34, DEADLK = 35, NAMETOOLONG = 36, NOSYS = 38, NOTEMPTY = 39, NOTSOCK = 88,
                     OPNOTSUPP = 95, AFNOSUPPORT = 97, NETDOWN = 100, NETUNREACH = 101, CONNREFUSED = 111,
                     TIMEDOUT = 110, HOSTUNREACH = 113, INPROGRESS = 115;
}

inline void SetErrno(int e) { mem::W32(Cpu::Current().errno_addr(), static_cast<std::uint32_t>(e)); }
inline void SetErrno(Cpu& c, int e) { mem::W32(c.errno_addr(), static_cast<std::uint32_t>(e)); }
inline int GetErrno() { return static_cast<int>(mem::R32(Cpu::Current().errno_addr())); }

// Estado por hilo del modulo (cpu.user(UserSlot::Libc)). Se libera en el exit hook.
struct ThreadState {
  gaddr tm_buf = 0;       // struct tm de gmtime/localtime (44 bytes)
  gaddr text_buf = 0;     // strerror / inet_ntoa / tmpnam / asctime (256 bytes)
  gaddr dlerror_buf = 0;  // dlerror (256 bytes)
  bool dlerror_set = false;
  gaddr strtok_save = 0;  // estado de strtok
  std::uint32_t pthread_keys[128] = {};
};
ThreadState& TS(Cpu& c);
ThreadState& TS();
gaddr ThreadBuffer(gaddr ThreadState::*field, gsize size);

// Cadena guest estable (internada, nunca se libera): para strerror, setlocale, tm_zone...
gaddr InternString(std::string_view s);

// Registro de cada area (un fichero por area).
void RegisterMemory();
void RegisterString();
void RegisterCtype();
void RegisterWchar();
void RegisterStdio();
void RegisterPosix();
void RegisterTime();
void RegisterPthread();
void RegisterRuntime();
void RegisterMath();
void RegisterDl();
void RegisterLog();
void RegisterNet();

// vfs.cpp: true si ya se llamo a ConfigureVfs().
bool VfsConfigured();

// --- format.cpp ---------------------------------------------------------------------------
// printf de bionic sobre argumentos guest. `fmt` es una cadena guest (ya traducida a host).
std::string FormatPrintf(const char* fmt, VaList& ap);
std::u32string FormatPrintfW(const std::uint32_t* fmt, VaList& ap);

// Fuente de caracteres para scanf.
struct ScanInput {
  virtual ~ScanInput() = default;
  virtual int Get() = 0;          // -1 = EOF
  virtual void Unget(int c) = 0;  // devuelve un caracter leido
};
int ScanfCore(ScanInput& in, const char* fmt, VaList& ap);

// --- stdio.cpp (usado por wchar.cpp) -------------------------------------------------------
int StdioGetc(gaddr fp);
int StdioUngetc(int c, gaddr fp);
int StdioPutc(int c, gaddr fp);
int StdioWrite(gaddr fp, const void* data, std::uint32_t n);  // bytes escritos o -1
void StdioFlushAll();

// --- time.cpp (usado por wchar.cpp) --------------------------------------------------------
std::string Strftime(const char* fmt, gaddr tm);

}  // namespace zw::hle::libc
