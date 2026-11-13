// POSIX de ficheros (open/read/write/lseek/pread/stat/dirs...), entorno, sysconf, ids, cwd.
// Layouts bionic ARM: struct stat = 104 bytes, struct dirent = 280 bytes (d_name @19).
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/log.h"
#include "hle/libc/internal.h"
#include "hle/libc/libc.h"
#include "hle/libc/vfs.h"

namespace zw::hle::libc {
namespace {

// --- descriptores --------------------------------------------------------------------------
int hle_open(const char* path, int flags, Varargs) {
  if (!path) {
    SetErrno(err::FAULT);
    return -1;
  }
  std::shared_ptr<vfs::OpenFile> of;
  const int e = vfs::Open(path, flags, of);
  if (e) {
    SetErrno(e);
    return -1;
  }
  const int fd = vfs::FdInstall(of);
  if (fd < 0) {
    SetErrno(-fd);
    return -1;
  }
  return fd;
}
int hle_creat(const char* path, int mode) {
  (void)mode;
  return hle_open(path, vfs::kO_WRONLY | vfs::kO_CREAT | vfs::kO_TRUNC, Varargs());
}
int hle_close(int fd) {
  const int r = vfs::FdClose(fd);
  if (r < 0) {
    SetErrno(-r);
    return -1;
  }
  return 0;
}

int DoRead(int fd, std::uint8_t* buf, gsize n, std::int64_t off, bool use_pos) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  if (!of->can_read) {
    SetErrno(err::BADF);
    return -1;
  }
  if (!n) return 0;
  std::unique_lock lk(of->mu, std::defer_lock);
  if (use_pos) {
    lk.lock();
    off = of->pos;
  } else if (of->node->IsStream()) {
    SetErrno(err::SPIPE);
    return -1;
  }
  const std::int32_t r = of->node->Pread(buf, n, off);
  if (r < 0) {
    SetErrno(-r);
    return -1;
  }
  if (use_pos && !of->node->IsStream()) of->pos += r;
  return r;
}
int DoWrite(int fd, const std::uint8_t* buf, gsize n, std::int64_t off, bool use_pos) {
  auto of = vfs::FdGet(fd);
  if (!of || !of->can_write) {
    SetErrno(err::BADF);
    return -1;
  }
  if (!n) return 0;
  std::unique_lock lk(of->mu, std::defer_lock);
  if (use_pos) {
    lk.lock();
    if (of->append && !of->node->IsStream()) of->pos = of->node->Size();
    off = of->pos;
  }
  const std::int32_t r = of->node->Pwrite(buf, n, off);
  if (r < 0) {
    SetErrno(-r);
    return -1;
  }
  if (use_pos && !of->node->IsStream()) of->pos += r;
  return r;
}
int hle_read(int fd, GPtr<std::uint8_t> buf, gsize n) { return DoRead(fd, buf.get(), n, 0, true); }
int hle_write(int fd, GPtr<const std::uint8_t> buf, gsize n) {
  return DoWrite(fd, const_cast<std::uint8_t*>(buf.get()), n, 0, true);
}
int hle_pread(int fd, GPtr<std::uint8_t> buf, gsize n, int off) { return DoRead(fd, buf.get(), n, off, false); }
int hle_pread64(int fd, GPtr<std::uint8_t> buf, gsize n, gi64 off) { return DoRead(fd, buf.get(), n, off.v, false); }
int hle_pwrite(int fd, GPtr<const std::uint8_t> buf, gsize n, int off) {
  return DoWrite(fd, const_cast<std::uint8_t*>(buf.get()), n, off, false);
}
int hle_pwrite64(int fd, GPtr<const std::uint8_t> buf, gsize n, gi64 off) {
  return DoWrite(fd, const_cast<std::uint8_t*>(buf.get()), n, off.v, false);
}
struct GuestIovec {
  GPtr<std::uint8_t> base;
  std::uint32_t len;
};
int hle_writev(int fd, GPtr<GuestIovec> iov, int cnt) {
  int total = 0;
  for (int i = 0; i < cnt; ++i) {
    const GuestIovec v = mem::Read<GuestIovec>(iov.addr + 8 * i);
    if (!v.len) continue;
    const int r = DoWrite(fd, v.base.get(), v.len, 0, true);
    if (r < 0) return total ? total : -1;
    total += r;
  }
  return total;
}
int hle_readv(int fd, GPtr<GuestIovec> iov, int cnt) {
  int total = 0;
  for (int i = 0; i < cnt; ++i) {
    const GuestIovec v = mem::Read<GuestIovec>(iov.addr + 8 * i);
    if (!v.len) continue;
    const int r = DoRead(fd, v.base.get(), v.len, 0, true);
    if (r < 0) return total ? total : -1;
    total += r;
    if (static_cast<std::uint32_t>(r) < v.len) break;
  }
  return total;
}
std::int64_t DoSeek(int fd, std::int64_t off, int whence) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  if (of->node->IsStream()) {
    SetErrno(err::SPIPE);
    return -1;
  }
  std::lock_guard lk(of->mu);
  std::int64_t base = 0;
  if (whence == 1) base = of->pos;
  else if (whence == 2) base = of->node->Size();
  else if (whence != 0) {
    SetErrno(err::INVAL);
    return -1;
  }
  if (base + off < 0) {
    SetErrno(err::INVAL);
    return -1;
  }
  of->pos = base + off;
  return of->pos;
}
int hle_lseek(int fd, int off, int whence) {
  const std::int64_t r = DoSeek(fd, off, whence);
  if (r > 0x7FFFFFFF) {
    SetErrno(75 /*EOVERFLOW*/);
    return -1;
  }
  return static_cast<int>(r);
}
gi64 hle_lseek64(int fd, gi64 off, int whence) { return gi64(DoSeek(fd, off.v, whence)); }
int hle_fsync(int fd) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  of->node->Sync();
  return 0;
}
int hle_ftruncate(int fd, int size) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  const int r = of->node->Truncate(size);
  if (r < 0) {
    SetErrno(-r);
    return -1;
  }
  return 0;
}
int hle_truncate(const char* path, int size) {
  const int e = vfs::Truncate(path, size);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}
int hle_dup(int fd) {
  const int r = vfs::FdDup(fd);
  if (r < 0) {
    SetErrno(-r);
    return -1;
  }
  return r;
}
int hle_dup2(int fd, int fd2) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  if (fd == fd2) return fd2;
  vfs::FdClose(fd2);
  const int r = vfs::FdInstall(of, fd2);
  return r == fd2 ? r : -1;
}
// fcntl: F_DUPFD 0, F_GETFD 1, F_SETFD 2, F_GETFL 3, F_SETFL 4.
int hle_fcntl(int fd, int cmd, Varargs va) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    // Sockets inexistentes: el juego solo pone O_NONBLOCK. No molestar.
    SetErrno(err::BADF);
    return -1;
  }
  switch (cmd) {
    case 0: return hle_dup(fd);
    case 1: return 0;
    case 2: return 0;
    case 3: return of->flags;
    case 4: {
      const int nf = static_cast<int>(va.NextWord());
      of->flags = (of->flags & ~(vfs::kO_APPEND | vfs::kO_NONBLOCK)) | (nf & (vfs::kO_APPEND | vfs::kO_NONBLOCK));
      of->append = (of->flags & vfs::kO_APPEND) != 0;
      return 0;
    }
    default: return 0;
  }
}
int hle_ioctl(int fd, int req, Varargs) {
  (void)fd;
  (void)req;
  SetErrno(err::NOTTY);
  return -1;
}
int hle_isatty(int fd) { return fd >= 0 && fd <= 2 ? 1 : 0; }

// --- stat --------------------------------------------------------------------------------------
void FillStat(gaddr st, const vfs::StatInfo& s) {
  std::uint8_t b[104] = {};
  auto w32 = [&](int o, std::uint32_t v) { std::memcpy(b + o, &v, 4); };
  auto w64 = [&](int o, std::uint64_t v) { std::memcpy(b + o, &v, 8); };
  w64(0, 0xb301);  // st_dev
  w32(12, static_cast<std::uint32_t>(s.ino));
  std::uint32_t mode;
  if (s.is_dir) mode = 0x4000 | 0771;
  else if (s.is_char_dev) mode = 0x2000 | 0666;
  else mode = 0x8000 | (s.read_only ? 0444 : 0660);
  w32(16, mode);
  w32(20, s.is_dir ? 2 : 1);  // st_nlink
  w32(24, 10123);             // st_uid (uid de app)
  w32(28, 10123);             // st_gid
  w64(48, static_cast<std::uint64_t>(s.size));
  w32(56, 4096);
  w64(64, static_cast<std::uint64_t>((s.size + 511) / 512));
  const auto t = static_cast<std::uint32_t>(s.mtime);
  w32(72, t);
  w32(80, t);
  w32(88, t);
  w64(96, s.ino);
  std::memcpy(g2h(st), b, sizeof b);
}
int hle_stat(const char* path, gaddr st) {
  vfs::StatInfo s;
  const int e = path ? vfs::Stat(path, s) : err::FAULT;
  if (e) {
    SetErrno(e);
    return -1;
  }
  FillStat(st, s);
  return 0;
}
int hle_fstat(int fd, gaddr st) {
  auto of = vfs::FdGet(fd);
  if (!of) {
    SetErrno(err::BADF);
    return -1;
  }
  vfs::StatInfo s = of->node->Stat();
  if (!s.ino) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : of->path) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    s.ino = h | 1;
  }
  if (of->node->IsDir()) s.is_dir = true;
  FillStat(st, s);
  return 0;
}
int hle_access(const char* path, int mode) {
  vfs::StatInfo s;
  const int e = path ? vfs::Stat(path, s) : err::FAULT;
  if (e) {
    SetErrno(e);
    return -1;
  }
  if ((mode & 2) && s.read_only) {
    SetErrno(err::ACCES);
    return -1;
  }
  return 0;
}
int hle_mkdir(const char* path, int) {
  const int e = vfs::Mkdir(path);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}
int hle_rmdir(const char* path) {
  const int e = vfs::Rmdir(path);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}
int hle_unlink(const char* path) {
  const int e = vfs::Unlink(path);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}
int hle_chmod(const char* path, int) {
  vfs::StatInfo s;
  const int e = vfs::Stat(path, s);
  if (e) {
    SetErrno(e);
    return -1;
  }
  return 0;
}
int hle_fchmod(int, int) { return 0; }
int hle_chown(const char*, int, int) { return 0; }
int hle_umask(int) { return 022; }
int hle_utime(const char*, gaddr) { return 0; }

// --- directorios -----------------------------------------------------------------------------
constexpr gsize kDirentSize = 280;
struct DirState {
  std::vector<vfs::DirEntry> entries;
  std::size_t next = 0;
  std::string path;
};
std::mutex g_dir_mu;
std::unordered_map<gaddr, std::shared_ptr<DirState>>* g_dirs = nullptr;

gaddr hle_opendir(const char* path) {
  auto d = std::make_shared<DirState>();
  d->path = vfs::Normalize(path ? path : "");
  vfs::StatInfo st;
  int e = vfs::Stat(d->path, st);
  if (!e && !st.is_dir) e = err::NOTDIR;
  if (!e) e = vfs::ListDir(d->path, d->entries);
  if (e) {
    SetErrno(e);
    return 0;
  }
  d->entries.insert(d->entries.begin(), {vfs::DirEntry{".", true}, vfs::DirEntry{"..", true}});
  // DIR guest: [0] = fd ficticio, [8..] = struct dirent para readdir.
  const gaddr dir = GuestMemory::Calloc(1, 8 + kDirentSize);
  if (!dir) {
    SetErrno(err::NOMEM);
    return 0;
  }
  mem::W32(dir, 0x7FFF);
  std::lock_guard lk(g_dir_mu);
  if (!g_dirs) g_dirs = new std::unordered_map<gaddr, std::shared_ptr<DirState>>();
  (*g_dirs)[dir] = d;
  return dir;
}
std::shared_ptr<DirState> GetDir(gaddr dir) {
  std::lock_guard lk(g_dir_mu);
  if (!g_dirs) return nullptr;
  auto it = g_dirs->find(dir);
  return it == g_dirs->end() ? nullptr : it->second;
}
void FillDirent(gaddr de, const DirState& d, std::size_t idx) {
  const auto& e = d.entries[idx];
  std::uint8_t b[kDirentSize] = {};
  std::uint64_t ino = 1469598103934665603ull;
  for (char c : d.path + "/" + e.name) ino = (ino ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  ino |= 1;
  const std::int64_t off = static_cast<std::int64_t>(idx + 1);
  std::memcpy(b + 0, &ino, 8);
  std::memcpy(b + 8, &off, 8);
  const std::uint16_t reclen = static_cast<std::uint16_t>((19 + e.name.size() + 1 + 7) & ~7u);
  std::memcpy(b + 16, &reclen, 2);
  b[18] = e.is_dir ? 4 : 8;  // DT_DIR / DT_REG
  std::memcpy(b + 19, e.name.data(), std::min<std::size_t>(e.name.size(), 255));
  std::memcpy(g2h(de), b, sizeof b);
}
gaddr hle_readdir(gaddr dir) {
  auto d = GetDir(dir);
  if (!d) {
    SetErrno(err::BADF);
    return 0;
  }
  std::lock_guard lk(g_dir_mu);
  if (d->next >= d->entries.size()) return 0;
  FillDirent(dir + 8, *d, d->next++);
  return dir + 8;
}
int hle_readdir_r(gaddr dir, gaddr entry, GPtr<std::uint32_t> result) {
  auto d = GetDir(dir);
  if (!d) return err::BADF;
  std::lock_guard lk(g_dir_mu);
  if (d->next >= d->entries.size()) {
    *result = 0;
    return 0;
  }
  FillDirent(entry, *d, d->next++);
  *result = entry;
  return 0;
}
int hle_closedir(gaddr dir) {
  {
    std::lock_guard lk(g_dir_mu);
    if (!g_dirs || !g_dirs->erase(dir)) {
      SetErrno(err::BADF);
      return -1;
    }
  }
  GuestMemory::Free(dir);
  return 0;
}
void hle_rewinddir(gaddr dir) {
  if (auto d = GetDir(dir)) {
    std::lock_guard lk(g_dir_mu);
    d->next = 0;
  }
}
int hle_dirfd(gaddr dir) { return GetDir(dir) ? 0x7FFF : -1; }

// --- cwd ----------------------------------------------------------------------------------------
gaddr hle_getcwd(GPtr<char> buf, gsize n) {
  const std::string c = vfs::Cwd();
  if (!buf) {
    const gsize sz = n ? n : static_cast<gsize>(c.size() + 1);
    if (sz < c.size() + 1) {
      SetErrno(err::RANGE);
      return 0;
    }
    const gaddr p = GuestMemory::Alloc(sz);
    mem::WriteCStr(p, c, sz);
    return p;
  }
  if (n < c.size() + 1) {
    SetErrno(err::RANGE);
    return 0;
  }
  mem::WriteCStr(buf.addr, c, n);
  return buf.addr;
}
int hle_chdir(const char* path) {
  vfs::StatInfo s;
  const std::string p = vfs::Normalize(path ? path : "");
  int e = vfs::Stat(p, s);
  if (!e && !s.is_dir) e = err::NOTDIR;
  if (e) {
    SetErrno(e);
    return -1;
  }
  vfs::SetCwd(p);
  return 0;
}

// --- entorno ----------------------------------------------------------------------------------
std::mutex g_env_mu;
std::map<std::string, gaddr>* g_env = nullptr;  // nombre -> "NOMBRE=valor" en memoria guest
std::map<std::string, gaddr>& Env() {
  if (!g_env) {
    g_env = new std::map<std::string, gaddr>();
    for (auto [k, v] : {std::pair{"PATH", "/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin"},
                        std::pair{"ANDROID_ROOT", "/system"}, std::pair{"ANDROID_DATA", "/data"},
                        std::pair{"EXTERNAL_STORAGE", "/sdcard"}, std::pair{"ANDROID_ASSETS", "/system/app"}})
      (*g_env)[k] = mem::StrDup(std::string(k) + "=" + v);
  }
  return *g_env;
}
gaddr hle_getenv(const char* name) {
  if (!name) return 0;
  std::lock_guard lk(g_env_mu);
  auto& e = Env();
  auto it = e.find(name);
  return it == e.end() ? 0 : it->second + static_cast<gaddr>(std::strlen(name)) + 1;
}
int hle_setenv(const char* name, const char* value, int overwrite) {
  if (!name || !*name || std::strchr(name, '=')) {
    SetErrno(err::INVAL);
    return -1;
  }
  std::lock_guard lk(g_env_mu);
  auto& e = Env();
  auto it = e.find(name);
  if (it != e.end() && !overwrite) return 0;
  // Las cadenas viejas no se liberan (el guest puede conservar el puntero de getenv).
  e[name] = mem::StrDup(std::string(name) + "=" + (value ? value : ""));
  return 0;
}
int hle_unsetenv(const char* name) {
  if (!name) return -1;
  std::lock_guard lk(g_env_mu);
  Env().erase(name);
  return 0;
}
int hle_putenv(GPtr<char> s) {
  if (!s) return -1;
  const std::string kv = s.get();
  const auto eq = kv.find('=');
  if (eq == std::string::npos) return hle_unsetenv(kv.c_str());
  std::lock_guard lk(g_env_mu);
  Env()[kv.substr(0, eq)] = s.addr;  // putenv usa la cadena del llamador
  return 0;
}
int hle_clearenv() {
  std::lock_guard lk(g_env_mu);
  Env().clear();
  return 0;
}

// --- sysconf / ids --------------------------------------------------------------------------------
int hle_sysconf(int name) {
  const unsigned cores = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  switch (name) {
    case 0x06: return 100;               // _SC_CLK_TCK
    case 0x27: case 0x28: return 4096;   // _SC_PAGESIZE / _SC_PAGE_SIZE
    case 0x60: case 0x61: return static_cast<int>(cores);  // _SC_NPROCESSORS_CONF / ONLN
    case 0x62: return 2097152 / 4;       // _SC_PHYS_PAGES (2 GiB)
    case 0x63: return 1048576 / 4;       // _SC_AVPHYS_PAGES
    case 0x00: return 131072;            // _SC_ARG_MAX
    case 0x05: return 1024;              // _SC_OPEN_MAX
    default:
      SetErrno(err::INVAL);
      return -1;
  }
}
int hle_getpagesize() { return 4096; }
int hle_getpid() { return 4242; }
int hle_getppid() { return 1; }
int hle_gettid(Cpu& c) { return static_cast<int>(4242 + c.thread_id()); }
int hle_getuid() { return 10123; }
int hle_getpgrp() { return 4242; }
int hle_setsid() { return 4242; }

// struct passwd de bionic: {pw_name, pw_passwd, pw_uid, pw_gid, pw_dir, pw_shell} = 24 bytes.
gaddr hle_getpwuid(int uid) {
  static gaddr pw = 0;
  static std::once_flag once;
  std::call_once(once, [] {
    pw = GuestMemory::Calloc(1, 24);
    mem::W32(pw + 0, InternString("app_123"));
    mem::W32(pw + 4, InternString(""));
    mem::W32(pw + 8, 10123);
    mem::W32(pw + 12, 10123);
    mem::W32(pw + 16, InternString("/data/data/com.gameloft.android.ANMP.GloftZOHM"));
    mem::W32(pw + 20, InternString("/system/bin/sh"));
  });
  if (uid != 10123 && uid != 0) return 0;
  return pw;
}
gaddr hle_getpwnam(const char*) { return hle_getpwuid(10123); }
gaddr hle_getlogin() { return InternString("app_123"); }

int hle_system(const char* cmd) {
  if (!cmd) return 0;  // no hay shell
  log::Warn("libc: system(\"{}\") ignorado", cmd);
  SetErrno(err::NOSYS);
  return -1;
}

}  // namespace

void RegisterPosix() {
  if (!VfsConfigured()) ConfigureVfs(DefaultVfsConfig());

  ZW_HLE("open", hle_open);
  ZW_HLE("open64", hle_open);
  ZW_HLE("__open_2", hle_open);
  ZW_HLE("creat", hle_creat);
  ZW_HLE("close", hle_close);
  ZW_HLE("read", hle_read);
  ZW_HLE("write", hle_write);
  ZW_HLE("pread", hle_pread);
  ZW_HLE("pread64", hle_pread64);
  ZW_HLE("pwrite", hle_pwrite);
  ZW_HLE("pwrite64", hle_pwrite64);
  ZW_HLE("writev", hle_writev);
  ZW_HLE("readv", hle_readv);
  ZW_HLE("lseek", hle_lseek);
  ZW_HLE("lseek64", hle_lseek64);
  ZW_HLE("fsync", hle_fsync);
  ZW_HLE("fdatasync", hle_fsync);
  ZW_HLE("ftruncate", hle_ftruncate);
  ZW_HLE("truncate", hle_truncate);
  ZW_HLE("dup", hle_dup);
  ZW_HLE("dup2", hle_dup2);
  ZW_HLE("fcntl", hle_fcntl);
  ZW_HLE("ioctl", hle_ioctl);
  ZW_HLE("isatty", hle_isatty);

  ZW_HLE("stat", hle_stat);
  ZW_HLE("stat64", hle_stat);
  ZW_HLE("lstat", hle_stat);
  ZW_HLE("lstat64", hle_stat);
  ZW_HLE("fstat", hle_fstat);
  ZW_HLE("fstat64", hle_fstat);
  ZW_HLE("access", hle_access);
  ZW_HLE("mkdir", hle_mkdir);
  ZW_HLE("rmdir", hle_rmdir);
  ZW_HLE("unlink", hle_unlink);
  ZW_HLE("chmod", hle_chmod);
  ZW_HLE("fchmod", hle_fchmod);
  ZW_HLE("chown", hle_chown);
  ZW_HLE("lchown", hle_chown);
  ZW_HLE("umask", hle_umask);
  ZW_HLE("utime", hle_utime);
  ZW_HLE("utimes", hle_utime);

  ZW_HLE("opendir", hle_opendir);
  ZW_HLE("readdir", hle_readdir);
  ZW_HLE("readdir64", hle_readdir);
  ZW_HLE("readdir_r", hle_readdir_r);
  ZW_HLE("closedir", hle_closedir);
  ZW_HLE("rewinddir", hle_rewinddir);
  ZW_HLE("dirfd", hle_dirfd);
  ZW_HLE("getcwd", hle_getcwd);
  ZW_HLE("chdir", hle_chdir);

  ZW_HLE("getenv", hle_getenv);
  ZW_HLE("setenv", hle_setenv);
  ZW_HLE("unsetenv", hle_unsetenv);
  ZW_HLE("putenv", hle_putenv);
  ZW_HLE("clearenv", hle_clearenv);

  ZW_HLE_FAST("sysconf", hle_sysconf);
  ZW_HLE_FAST("getpagesize", hle_getpagesize);
  ZW_HLE_FAST("getpid", hle_getpid);
  ZW_HLE_FAST("getppid", hle_getppid);
  ZW_HLE_FAST("gettid", hle_gettid);
  ZW_HLE_FAST("getuid", hle_getuid);
  ZW_HLE_FAST("geteuid", hle_getuid);
  ZW_HLE_FAST("getgid", hle_getuid);
  ZW_HLE_FAST("getegid", hle_getuid);
  ZW_HLE_FAST("getpgrp", hle_getpgrp);
  ZW_HLE_FAST("setsid", hle_setsid);
  ZW_HLE("getpwuid", hle_getpwuid);
  ZW_HLE("getpwnam", hle_getpwnam);
  ZW_HLE("getlogin", hle_getlogin);
  ZW_HLE("system", hle_system);
}

}  // namespace zw::hle::libc
