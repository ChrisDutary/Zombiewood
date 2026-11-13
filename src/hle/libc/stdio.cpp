// stdio de bionic: FILE guest real de 84 bytes (_flags @12 con __SEOF/__SERR, _file @14 = fd) +
// estado host en una tabla lateral; __sF[3]; printf/scanf-family sobre el VFS.
#include <algorithm>
#include <atomic>
#include <format>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "core/log.h"
#include "hle/libc/internal.h"
#include "hle/libc/vfs.h"

namespace zw::hle::libc {
namespace {

constexpr gsize kFileSize = 84;
constexpr std::uint16_t kSLBF = 0x0001, kSNBF = 0x0002, kSRD = 0x0004, kSWR = 0x0008, kSRW = 0x0010, kSEOF = 0x0020,
                        kSERR = 0x0040, kSAPP = 0x0100;
constexpr std::uint32_t kReadAhead = 64 * 1024;
constexpr int kEOF = -1;

struct FState {
  std::mutex mu;
  gaddr fp = 0;
  int fd = -1;
  std::shared_ptr<vfs::OpenFile> of;
  std::uint16_t base_flags = 0;
  bool eof = false, err = false;
  bool owned = true;  // FILE guest reservado por fopen (se libera en fclose)
  std::vector<std::uint8_t> unget;
  std::vector<std::uint8_t> rbuf;
  std::int64_t rbuf_off = -1;
  std::uint32_t rbuf_len = 0;

  void Sync() const {
    std::uint16_t f = base_flags;
    if (eof) f |= kSEOF;
    if (err) f |= kSERR;
    mem::W16(fp + 12, f);
    mem::W16(fp + 14, static_cast<std::uint16_t>(static_cast<std::int16_t>(fd)));
  }
  bool stream() const { return of->node->IsStream(); }

  // Lee hasta n bytes. Devuelve bytes leidos; marca eof/err.
  std::uint32_t Read(std::uint8_t* dst, std::uint32_t n) {
    if (!of->can_read) {
      err = true;
      SetErrno(err::BADF);
      return 0;
    }
    std::uint32_t got = 0;
    while (got < n && !unget.empty()) {
      dst[got++] = unget.back();
      unget.pop_back();
    }
    std::lock_guard lk(of->mu);
    while (got < n) {
      const std::uint32_t want = n - got;
      if (rbuf_off >= 0 && of->pos >= rbuf_off && of->pos < rbuf_off + rbuf_len) {
        const std::uint32_t at = static_cast<std::uint32_t>(of->pos - rbuf_off);
        const std::uint32_t c = std::min(want, rbuf_len - at);
        std::memcpy(dst + got, rbuf.data() + at, c);
        got += c;
        of->pos += c;
        continue;
      }
      if (want >= kReadAhead || stream()) {
        const std::int32_t r = of->node->Pread(dst + got, want, of->pos);
        if (r < 0) {
          err = true;
          SetErrno(-r);
          break;
        }
        if (r == 0) {
          eof = true;
          break;
        }
        got += static_cast<std::uint32_t>(r);
        if (!stream()) of->pos += r;
        continue;
      }
      rbuf.resize(kReadAhead);
      const std::int32_t r = of->node->Pread(rbuf.data(), kReadAhead, of->pos);
      if (r < 0) {
        err = true;
        SetErrno(-r);
        rbuf_off = -1;
        break;
      }
      if (r == 0) {
        eof = true;
        rbuf_off = -1;
        break;
      }
      rbuf_off = of->pos;
      rbuf_len = static_cast<std::uint32_t>(r);
    }
    return got;
  }

  std::uint32_t Write(const std::uint8_t* src, std::uint32_t n) {
    if (!of->can_write) {
      err = true;
      SetErrno(err::BADF);
      return 0;
    }
    if (!n) return 0;
    unget.clear();
    rbuf_off = -1;
    std::lock_guard lk(of->mu);
    if (of->append && !stream()) of->pos = of->node->Size();
    const std::int32_t r = of->node->Pwrite(src, n, of->pos);
    if (r < 0) {
      err = true;
      SetErrno(-r);
      return 0;
    }
    if (!stream()) of->pos += r;
    return static_cast<std::uint32_t>(r);
  }

  int Getc() {
    std::uint8_t c;
    return Read(&c, 1) == 1 ? c : kEOF;
  }
};

std::shared_mutex g_files_mu;
std::unordered_map<gaddr, std::shared_ptr<FState>>* g_files = nullptr;

std::shared_ptr<FState> Get(gaddr fp) {
  std::shared_lock lk(g_files_mu);
  if (!g_files || !fp) return nullptr;
  auto it = g_files->find(fp);
  return it == g_files->end() ? nullptr : it->second;
}
void Put(const std::shared_ptr<FState>& s) {
  std::unique_lock lk(g_files_mu);
  if (!g_files) g_files = new std::unordered_map<gaddr, std::shared_ptr<FState>>();
  (*g_files)[s->fp] = s;
}

std::uint16_t FlagsFor(const vfs::OpenFile& of) {
  std::uint16_t f = of.can_read && of.can_write ? kSRW : of.can_read ? kSRD : kSWR;
  if (of.append) f |= kSAPP;
  return f;
}

// Modo de fopen -> flags de open. false si el modo es invalido.
bool ParseMode(const char* mode, int& flags) {
  if (!mode) return false;
  const bool plus = std::strchr(mode, '+') != nullptr;
  switch (mode[0]) {
    case 'r': flags = plus ? vfs::kO_RDWR : vfs::kO_RDONLY; break;
    case 'w': flags = (plus ? vfs::kO_RDWR : vfs::kO_WRONLY) | vfs::kO_CREAT | vfs::kO_TRUNC; break;
    case 'a': flags = (plus ? vfs::kO_RDWR : vfs::kO_WRONLY) | vfs::kO_CREAT | vfs::kO_APPEND; break;
    default: return false;
  }
  if (std::strchr(mode, 'x')) flags |= vfs::kO_EXCL;
  return true;
}

gaddr NewFile(std::shared_ptr<vfs::OpenFile> of, int fd, gaddr reuse = 0) {
  auto s = std::make_shared<FState>();
  s->fp = reuse ? reuse : GuestMemory::Calloc(1, kFileSize);
  if (!s->fp) return 0;
  s->owned = true;
  s->fd = fd;
  s->of = std::move(of);
  s->base_flags = FlagsFor(*s->of);
  mem::W32(s->fp + 76, 4096);  // _blksize
  s->Sync();
  Put(s);
  return s->fp;
}

// --- API de FILE ---------------------------------------------------------------------------------
gaddr hle_fopen(const char* path, const char* mode) {
  int flags = 0;
  if (!path || !ParseMode(mode, flags)) {
    SetErrno(err::INVAL);
    return 0;
  }
  std::shared_ptr<vfs::OpenFile> of;
  const int e = vfs::Open(path, flags, of);
  if (e) {
    SetErrno(e);
    return 0;
  }
  const int fd = vfs::FdInstall(of);
  if (fd < 0) {
    SetErrno(-fd);
    return 0;
  }
  return NewFile(of, fd);
}

gaddr hle_fdopen(int fd, const char* mode) {
  auto of = vfs::FdGet(fd);
  int flags = 0;
  if (!of || !ParseMode(mode, flags)) {
    SetErrno(of ? err::INVAL : err::BADF);
    return 0;
  }
  return NewFile(of, fd);
}

int CloseState(const std::shared_ptr<FState>& s, bool free_guest) {
  {
    std::unique_lock lk(g_files_mu);
    if (g_files) g_files->erase(s->fp);
  }
  int r = 0;
  if (s->fd >= 0) r = vfs::FdClose(s->fd) < 0 ? -1 : 0;
  if (free_guest && s->owned) GuestMemory::Free(s->fp);
  return r;
}

int hle_fclose(gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return kEOF;
  }
  std::lock_guard lk(s->mu);
  if (s->stream()) s->of->node->Sync();
  return CloseState(s, true) ? kEOF : 0;
}

gaddr hle_freopen(const char* path, const char* mode, gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return 0;
  }
  if (!path) return fp;  // solo cambio de modo
  const bool owned = s->owned;
  {
    std::lock_guard lk(s->mu);
    CloseState(s, false);
  }
  int flags = 0;
  std::shared_ptr<vfs::OpenFile> of;
  int e = ParseMode(mode, flags) ? vfs::Open(path, flags, of) : err::INVAL;
  if (e) {
    SetErrno(e);
    if (owned) GuestMemory::Free(fp);
    return 0;
  }
  const int fd = vfs::FdInstall(of);
  const gaddr r = NewFile(of, fd, fp);
  if (auto ns = Get(r)) ns->owned = owned;
  return r;
}

gsize hle_fread(GPtr<std::uint8_t> buf, gsize size, gsize n, gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return 0;
  }
  const std::uint64_t total = static_cast<std::uint64_t>(size) * n;
  if (!total) return 0;
  std::lock_guard lk(s->mu);
  const std::uint32_t got = s->Read(buf.get(), static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0xFFFFFFFFu)));
  s->Sync();
  return got / size;
}

gsize hle_fwrite(GPtr<const std::uint8_t> buf, gsize size, gsize n, gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return 0;
  }
  const std::uint64_t total = static_cast<std::uint64_t>(size) * n;
  if (!total) return 0;
  std::lock_guard lk(s->mu);
  const std::uint32_t put = s->Write(buf.get(), static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0xFFFFFFFFu)));
  s->Sync();
  return put / size;
}

int SeekState(FState& s, std::int64_t off, int whence) {
  std::lock_guard lk(s.of->mu);
  if (s.stream()) {
    SetErrno(err::SPIPE);
    return -1;
  }
  std::int64_t base = 0;
  if (whence == 1) base = s.of->pos - static_cast<std::int64_t>(s.unget.size());
  else if (whence == 2) base = s.of->node->Size();
  else if (whence != 0) {
    SetErrno(err::INVAL);
    return -1;
  }
  const std::int64_t np = base + off;
  if (np < 0) {
    SetErrno(err::INVAL);
    return -1;
  }
  s.of->pos = np;
  s.unget.clear();
  s.eof = false;
  return 0;
}

int hle_fseek(gaddr fp, int off, int whence) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return -1;
  }
  std::lock_guard lk(s->mu);
  const int r = SeekState(*s, off, whence);
  s->Sync();
  return r;
}
int hle_fseeko64(gaddr fp, gi64 off, int whence) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return -1;
  }
  std::lock_guard lk(s->mu);
  const int r = SeekState(*s, off.v, whence);
  s->Sync();
  return r;
}
std::int64_t TellState(FState& s) {
  std::lock_guard lk(s.of->mu);
  if (s.stream()) return 0;
  return std::max<std::int64_t>(0, s.of->pos - static_cast<std::int64_t>(s.unget.size()));
}
int hle_ftell(gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return -1;
  }
  std::lock_guard lk(s->mu);
  const std::int64_t p = TellState(*s);
  if (p > 0x7FFFFFFF) {
    SetErrno(75 /*EOVERFLOW*/);
    return -1;
  }
  return static_cast<int>(p);
}
gi64 hle_ftello64(gaddr fp) {
  auto s = Get(fp);
  if (!s) return gi64(-1);
  std::lock_guard lk(s->mu);
  return gi64(TellState(*s));
}
void hle_rewind(gaddr fp) {
  if (auto s = Get(fp)) {
    std::lock_guard lk(s->mu);
    SeekState(*s, 0, 0);
    s->err = false;
    s->Sync();
  }
}
int hle_fgetpos(gaddr fp, GPtr<std::uint32_t> pos) {
  const int p = hle_ftell(fp);
  if (p < 0) return -1;
  *pos = static_cast<std::uint32_t>(p);
  return 0;
}
int hle_fsetpos(gaddr fp, GPtr<std::uint32_t> pos) { return hle_fseek(fp, static_cast<int>(*pos), 0); }
int hle_feof(gaddr fp) {
  auto s = Get(fp);
  return s && s->eof ? 1 : 0;
}
int hle_ferror(gaddr fp) {
  auto s = Get(fp);
  return s && s->err ? 1 : 0;
}
void hle_clearerr(gaddr fp) {
  if (auto s = Get(fp)) {
    std::lock_guard lk(s->mu);
    s->eof = s->err = false;
    s->Sync();
  }
}
int hle_fileno(gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return -1;
  }
  return s->fd;
}
int FlushOne(const std::shared_ptr<FState>& s) {
  std::lock_guard lk(s->mu);
  // Solo los flujos de log tienen buffer propio; los ficheros host se escriben al momento
  // (fsync() es quien fuerza al disco).
  if (s->stream()) s->of->node->Sync();
  s->rbuf_off = -1;
  return 0;
}
int hle_fflush(gaddr fp) {
  if (!fp) {
    std::vector<std::shared_ptr<FState>> all;
    {
      std::shared_lock lk(g_files_mu);
      if (g_files)
        for (auto& [k, v] : *g_files) all.push_back(v);
    }
    for (auto& s : all) FlushOne(s);
    return 0;
  }
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return kEOF;
  }
  return FlushOne(s);
}
int hle_setvbuf(gaddr, gaddr, int, gsize) { return 0; }
void hle_setbuf(gaddr, gaddr) {}
void hle_setbuffer(gaddr, gaddr, int) {}
int hle_setlinebuf(gaddr) { return 0; }

}  // namespace

// --- caracteres (tambien para wchar.cpp) ------------------------------------------------------
int StdioGetc(gaddr fp) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return kEOF;
  }
  std::lock_guard lk(s->mu);
  const int c = s->Getc();
  s->Sync();
  return c;
}
int StdioUngetc(int c, gaddr fp) {
  auto s = Get(fp);
  if (!s || c == kEOF) return kEOF;
  std::lock_guard lk(s->mu);
  s->unget.push_back(static_cast<std::uint8_t>(c));
  s->eof = false;
  s->Sync();
  return static_cast<std::uint8_t>(c);
}
int StdioWrite(gaddr fp, const void* data, std::uint32_t n) {
  auto s = Get(fp);
  if (!s) {
    SetErrno(err::BADF);
    return -1;
  }
  std::lock_guard lk(s->mu);
  const std::uint32_t put = s->Write(static_cast<const std::uint8_t*>(data), n);
  s->Sync();
  return put == n ? static_cast<int>(put) : -1;
}
void StdioFlushAll() { hle_fflush(0); }
int StdioPutc(int c, gaddr fp) {
  const std::uint8_t b = static_cast<std::uint8_t>(c);
  return StdioWrite(fp, &b, 1) == 1 ? b : kEOF;
}

namespace {

gaddr StdoutFp() { return FindData("__sF") + kFileSize; }
gaddr StderrFp() { return FindData("__sF") + 2 * kFileSize; }
gaddr StdinFp() { return FindData("__sF"); }

int hle_fgetc(gaddr fp) { return StdioGetc(fp); }
int hle_getchar() { return StdioGetc(StdinFp()); }
int hle_ungetc(int c, gaddr fp) { return StdioUngetc(c, fp); }
int hle_fputc(int c, gaddr fp) { return StdioPutc(c, fp); }
int hle_putchar(int c) { return StdioPutc(c, StdoutFp()); }
int hle_swbuf(int c, gaddr fp) { return StdioPutc(c, fp); }
int hle_fputs(const char* str, gaddr fp) {
  const auto n = static_cast<std::uint32_t>(std::strlen(str));
  return StdioWrite(fp, str, n) < 0 ? kEOF : 1;
}
int hle_puts(const char* str) {
  std::string line = str ? str : "(null)";
  line += '\n';
  return StdioWrite(StdoutFp(), line.data(), static_cast<std::uint32_t>(line.size())) < 0 ? kEOF : 1;
}
gaddr hle_fgets(GPtr<char> dst, int n, gaddr fp) {
  auto s = Get(fp);
  if (!s || n <= 0 || !dst) {
    if (!s) SetErrno(err::BADF);
    return 0;
  }
  std::lock_guard lk(s->mu);
  int i = 0;
  char* d = dst.get();
  while (i < n - 1) {
    const int c = s->Getc();
    if (c == kEOF) break;
    d[i++] = static_cast<char>(c);
    if (c == '\n') break;
  }
  s->Sync();
  if (i == 0) return 0;
  d[i] = 0;
  return dst.addr;
}
gi64 GetDelim(GPtr<std::uint32_t> lineptr, GPtr<std::uint32_t> np, int delim, gaddr fp) {
  auto s = Get(fp);
  if (!s || !lineptr || !np) {
    SetErrno(err::INVAL);
    return gi64(-1);
  }
  std::lock_guard lk(s->mu);
  std::string line;
  for (;;) {
    const int c = s->Getc();
    if (c == kEOF) break;
    line += static_cast<char>(c);
    if (c == delim) break;
  }
  s->Sync();
  if (line.empty()) return gi64(-1);
  if (!*lineptr || *np < line.size() + 1) {
    const gaddr nb = GuestMemory::Realloc(*lineptr, static_cast<gsize>(line.size() + 1));
    if (!nb) return gi64(-1);
    *lineptr = nb;
    *np = static_cast<std::uint32_t>(line.size() + 1);
  }
  mem::WriteCStr(*lineptr, line, *np);
  return gi64(static_cast<std::int64_t>(line.size()));
}
int hle_getdelim(GPtr<std::uint32_t> lp, GPtr<std::uint32_t> np, int delim, gaddr fp) {
  return static_cast<int>(GetDelim(lp, np, delim, fp).v);
}
int hle_getline(GPtr<std::uint32_t> lp, GPtr<std::uint32_t> np, gaddr fp) {
  return static_cast<int>(GetDelim(lp, np, '\n', fp).v);
}

// --- printf ------------------------------------------------------------------------------------
int StoreN(char* dst, gsize cap, const std::string& s) {
  if (dst && cap) {
    const std::size_t n = std::min<std::size_t>(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
  }
  return static_cast<int>(s.size());
}
int ToFile(gaddr fp, const std::string& s) {
  if (s.empty()) return Get(fp) ? 0 : -1;
  return StdioWrite(fp, s.data(), static_cast<std::uint32_t>(s.size())) < 0 ? -1 : static_cast<int>(s.size());
}
int hle_sprintf(char* d, const char* f, Varargs va) {
  const std::string s = FormatPrintf(f, va);
  std::memcpy(d, s.c_str(), s.size() + 1);
  return static_cast<int>(s.size());
}
int hle_snprintf(char* d, gsize n, const char* f, Varargs va) { return StoreN(d, n, FormatPrintf(f, va)); }
int hle_vsprintf(char* d, const char* f, VaList ap) {
  const std::string s = FormatPrintf(f, ap);
  std::memcpy(d, s.c_str(), s.size() + 1);
  return static_cast<int>(s.size());
}
int hle_vsnprintf(char* d, gsize n, const char* f, VaList ap) { return StoreN(d, n, FormatPrintf(f, ap)); }
int hle_printf(const char* f, Varargs va) { return ToFile(StdoutFp(), FormatPrintf(f, va)); }
int hle_vprintf(const char* f, VaList ap) { return ToFile(StdoutFp(), FormatPrintf(f, ap)); }
int hle_fprintf(gaddr fp, const char* f, Varargs va) { return ToFile(fp, FormatPrintf(f, va)); }
int hle_vfprintf(gaddr fp, const char* f, VaList ap) { return ToFile(fp, FormatPrintf(f, ap)); }
int DWrite(int fd, const std::string& s) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  std::lock_guard lk(of->mu);
  const auto r = of->node->Pwrite(s.data(), static_cast<std::uint32_t>(s.size()), of->pos);
  if (r > 0 && !of->node->IsStream()) of->pos += r;
  return r < 0 ? -1 : r;
}
int hle_dprintf(int fd, const char* f, Varargs va) { return DWrite(fd, FormatPrintf(f, va)); }
int hle_vdprintf(int fd, const char* f, VaList ap) { return DWrite(fd, FormatPrintf(f, ap)); }
int Asprintf(GPtr<std::uint32_t> out, const std::string& s) {
  const gaddr p = mem::StrDup(s);
  if (!p) return -1;
  *out = p;
  return static_cast<int>(s.size());
}
int hle_asprintf(GPtr<std::uint32_t> out, const char* f, Varargs va) { return Asprintf(out, FormatPrintf(f, va)); }
int hle_vasprintf(GPtr<std::uint32_t> out, const char* f, VaList ap) { return Asprintf(out, FormatPrintf(f, ap)); }
void hle_perror(const char* s) {
  const int e = GetErrno();
  const std::string msg = (s && *s ? std::string(s) + ": " : std::string()) +
                          mem::ReadCStr(call_guest<gaddr>(hle::StubFor("strerror"), e)) + "\n";
  ToFile(StderrFp(), msg);
}

// --- scanf -------------------------------------------------------------------------------------
struct StrInput final : ScanInput {
  const unsigned char* s;
  std::vector<int> back;
  explicit StrInput(const char* p) : s(reinterpret_cast<const unsigned char*>(p)) {}
  int Get() override {
    if (!back.empty()) {
      const int c = back.back();
      back.pop_back();
      return c;
    }
    return *s ? *s++ : -1;
  }
  void Unget(int c) override { back.push_back(c); }
};
struct FileInput final : ScanInput {
  gaddr fp;
  explicit FileInput(gaddr f) : fp(f) {}
  int Get() override { return StdioGetc(fp); }
  void Unget(int c) override { StdioUngetc(c, fp); }
};
int hle_sscanf(const char* s, const char* f, Varargs va) {
  StrInput in(s);
  return ScanfCore(in, f, va);
}
int hle_vsscanf(const char* s, const char* f, VaList ap) {
  StrInput in(s);
  return ScanfCore(in, f, ap);
}
int hle_fscanf(gaddr fp, const char* f, Varargs va) {
  if (!Get(fp)) return -1;
  FileInput in(fp);
  return ScanfCore(in, f, va);
}
int hle_vfscanf(gaddr fp, const char* f, VaList ap) {
  if (!Get(fp)) return -1;
  FileInput in(fp);
  return ScanfCore(in, f, ap);
}
int hle_scanf(const char* f, Varargs va) {
  FileInput in(StdinFp());
  return ScanfCore(in, f, va);
}

// --- ficheros temporales y rutas ------------------------------------------------------------
gaddr hle_tmpfile() {
  auto of = std::make_shared<vfs::OpenFile>();
  of->node = vfs::MakeMemNode({}, true);
  of->can_read = of->can_write = true;
  of->flags = vfs::kO_RDWR;
  of->path = "/tmp/tmpfile";
  const int fd = vfs::FdInstall(of);
  if (fd < 0) return 0;
  return NewFile(of, fd);
}
gaddr hle_tmpnam(GPtr<char> s) {
  static std::atomic<unsigned> counter{0};
  const std::string name = std::format("/data/data/com.gameloft.android.ANMP.GloftZOHM/cache/tmp.{:06}", ++counter);
  const gaddr dst = s ? s.addr : ThreadBuffer(&ThreadState::text_buf, 256);
  mem::WriteCStr(dst, name, 256);
  return dst;
}
int hle_remove(const char* path) {
  int e = vfs::Unlink(path);
  if (e == err::ISDIR) e = vfs::Rmdir(path);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}
int hle_rename(const char* a, const char* b) {
  const int e = vfs::Rename(a, b);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}

}  // namespace

void RegisterStdio() {
  // FILE __sF[3]: stdin, stdout, stderr con _file = 0/1/2.
  const gaddr sF = DefineData("__sF", 3 * kFileSize);
  for (int i = 0; i < 3; ++i) {
    auto s = std::make_shared<FState>();
    s->fp = sF + i * kFileSize;
    s->fd = i;
    s->owned = false;
    s->of = vfs::FdGet(i);
    s->base_flags = i == 0 ? kSRD : static_cast<std::uint16_t>(kSWR | (i == 1 ? kSLBF : kSNBF));
    mem::W32(s->fp + 76, 1024);
    s->Sync();
    Put(s);
  }

  ZW_HLE("fopen", hle_fopen);
  ZW_HLE("fopen64", hle_fopen);
  ZW_HLE("fdopen", hle_fdopen);
  ZW_HLE("freopen", hle_freopen);
  ZW_HLE("fclose", hle_fclose);
  ZW_HLE("fread", hle_fread);
  ZW_HLE("fread_unlocked", hle_fread);
  ZW_HLE("fwrite", hle_fwrite);
  ZW_HLE("fwrite_unlocked", hle_fwrite);
  ZW_HLE("fseek", hle_fseek);
  ZW_HLE("fseeko", hle_fseek);
  ZW_HLE("fseeko64", hle_fseeko64);
  ZW_HLE("ftell", hle_ftell);
  ZW_HLE("ftello", hle_ftell);
  ZW_HLE("ftello64", hle_ftello64);
  ZW_HLE("rewind", hle_rewind);
  ZW_HLE("fgetpos", hle_fgetpos);
  ZW_HLE("fsetpos", hle_fsetpos);
  ZW_HLE("feof", hle_feof);
  ZW_HLE("ferror", hle_ferror);
  ZW_HLE("clearerr", hle_clearerr);
  ZW_HLE("fileno", hle_fileno);
  ZW_HLE("fflush", hle_fflush);
  ZW_HLE("setvbuf", hle_setvbuf);
  ZW_HLE("setbuf", hle_setbuf);
  ZW_HLE("setbuffer", hle_setbuffer);
  ZW_HLE("setlinebuf", hle_setlinebuf);
  ZW_HLE("fgetc", hle_fgetc);
  ZW_HLE("getc", hle_fgetc);
  ZW_HLE("getc_unlocked", hle_fgetc);
  ZW_HLE("__srget", hle_fgetc);
  ZW_HLE("getchar", hle_getchar);
  ZW_HLE("ungetc", hle_ungetc);
  ZW_HLE("fputc", hle_fputc);
  ZW_HLE("putc", hle_fputc);
  ZW_HLE("putc_unlocked", hle_fputc);
  ZW_HLE("__swbuf", hle_swbuf);
  ZW_HLE("putchar", hle_putchar);
  ZW_HLE("fputs", hle_fputs);
  ZW_HLE("puts", hle_puts);
  ZW_HLE("fgets", hle_fgets);
  ZW_HLE("getline", hle_getline);
  ZW_HLE("getdelim", hle_getdelim);

  ZW_HLE("sprintf", hle_sprintf);
  ZW_HLE("snprintf", hle_snprintf);
  ZW_HLE("vsprintf", hle_vsprintf);
  ZW_HLE("vsnprintf", hle_vsnprintf);
  ZW_HLE("printf", hle_printf);
  ZW_HLE("vprintf", hle_vprintf);
  ZW_HLE("fprintf", hle_fprintf);
  ZW_HLE("vfprintf", hle_vfprintf);
  ZW_HLE("dprintf", hle_dprintf);
  ZW_HLE("vdprintf", hle_vdprintf);
  ZW_HLE("asprintf", hle_asprintf);
  ZW_HLE("vasprintf", hle_vasprintf);
  ZW_HLE("perror", hle_perror);
  ZW_HLE("sscanf", hle_sscanf);
  ZW_HLE("vsscanf", hle_vsscanf);
  ZW_HLE("fscanf", hle_fscanf);
  ZW_HLE("vfscanf", hle_vfscanf);
  ZW_HLE("scanf", hle_scanf);

  ZW_HLE("tmpfile", hle_tmpfile);
  ZW_HLE("tmpnam", hle_tmpnam);
  ZW_HLE("remove", hle_remove);
  ZW_HLE("rename", hle_rename);
}

}  // namespace zw::hle::libc
