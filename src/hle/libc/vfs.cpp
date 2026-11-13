// VFS: monturas (carpetas host + ZIP superpuestos), ficheros sinteticos, cwd virtual y tabla de fds.
#include "hle/libc/vfs.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <shared_mutex>
#include <thread>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "hle/libc/libc.h"

namespace zw::hle::libc {
namespace vfs {
namespace {

constexpr const char* kPkg = "com.gameloft.android.ANMP.GloftZOHM";

struct Mount {
  std::string prefix;     // "/data/data/<pkg>" (sin '/' final)
  std::string prefix_lc;  // en minusculas
  std::filesystem::path host;
  bool has_host = false;
  bool read_only = false;
  std::vector<std::shared_ptr<ZipArchive>> zips;
};

std::shared_mutex g_mu;
std::vector<Mount>* g_mounts = nullptr;
std::mutex g_cwd_mu;  // separado de g_mu: Normalize() se llama con g_mu tomado
std::string* g_cwd = nullptr;
bool g_configured = false;

std::vector<Mount>& Mounts() {
  if (!g_mounts) g_mounts = new std::vector<Mount>();
  return *g_mounts;
}

std::filesystem::path FromUtf8(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string ToUtf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// Busca la montura con el prefijo mas largo que contenga `p` (normalizada). rel = resto sin '/'.
const Mount* FindMount(const std::string& p, std::string& rel) {
  const std::string lc = Lower(p);
  const Mount* best = nullptr;
  for (const auto& m : Mounts()) {
    const auto& pre = m.prefix_lc;
    if (lc.size() >= pre.size() && lc.compare(0, pre.size(), pre) == 0 &&
        (lc.size() == pre.size() || lc[pre.size()] == '/' || pre == "/")) {
      if (!best || pre.size() > best->prefix_lc.size()) best = &m;
    }
  }
  if (best) {
    rel = p.size() > best->prefix.size() ? p.substr(best->prefix.size() + (best->prefix == "/" ? 0 : 1)) : "";
  }
  return best;
}

// Directorios virtuales: ancestros de los prefijos de montura ("/", "/sdcard", "/data/data"...).
bool VirtualChildren(const std::string& p, std::vector<DirEntry>* out) {
  const std::string lc = Lower(p);
  bool any = lc == "/";
  for (const auto& m : Mounts()) {
    const auto& pre = m.prefix_lc;
    const std::string base = lc == "/" ? "/" : lc + "/";
    if (pre.size() > base.size() && pre.compare(0, base.size(), base) == 0) {
      any = true;
      if (out) {
        const auto rest = m.prefix.substr(base.size());
        const auto name = rest.substr(0, rest.find('/'));
        bool dup = false;
        for (const auto& e : *out)
          if (Lower(e.name) == Lower(name)) dup = true;
        if (!dup) out->push_back(DirEntry{name, true});
      }
    }
  }
  return any;
}

// --- nodos sinteticos --------------------------------------------------------------------------
class MemNode final : public Node {
 public:
  MemNode(std::vector<std::uint8_t> d, bool w) : d_(std::move(d)), writable_(w) {}
  std::int64_t Size() override {
    std::lock_guard lk(mu_);
    return static_cast<std::int64_t>(d_.size());
  }
  std::int32_t Pread(void* buf, std::uint32_t n, std::int64_t off) override {
    std::lock_guard lk(mu_);
    if (off < 0) return -err::INVAL;
    if (off >= static_cast<std::int64_t>(d_.size())) return 0;
    const std::int64_t left = static_cast<std::int64_t>(d_.size()) - off;
    if (n > left) n = static_cast<std::uint32_t>(left);
    std::memcpy(buf, d_.data() + off, n);
    return static_cast<std::int32_t>(n);
  }
  std::int32_t Pwrite(const void* buf, std::uint32_t n, std::int64_t off) override {
    if (!writable_) return -err::BADF;
    std::lock_guard lk(mu_);
    if (off < 0 || off + n > (1ll << 30)) return -err::FBIG;
    if (d_.size() < static_cast<std::size_t>(off + n)) d_.resize(static_cast<std::size_t>(off + n));
    std::memcpy(d_.data() + off, buf, n);
    return static_cast<std::int32_t>(n);
  }
  int Truncate(std::int64_t s) override {
    if (!writable_) return -err::INVAL;
    std::lock_guard lk(mu_);
    d_.resize(static_cast<std::size_t>(s));
    return 0;
  }
  StatInfo Stat() override {
    StatInfo s;
    s.size = Size();
    s.read_only = !writable_;
    return s;
  }

 private:
  std::mutex mu_;
  std::vector<std::uint8_t> d_;
  bool writable_;
};

class StreamNode final : public Node {
 public:
  explicit StreamNode(int which) : which_(which) {}
  std::int64_t Size() override { return 0; }
  std::int32_t Pread(void*, std::uint32_t, std::int64_t) override { return 0; }  // stdin: EOF
  std::int32_t Pwrite(const void* buf, std::uint32_t n, std::int64_t) override {
    if (which_ == 0) return -err::BADF;
    std::lock_guard lk(mu_);
    line_.append(static_cast<const char*>(buf), n);
    std::size_t nl;
    while ((nl = line_.find('\n')) != std::string::npos) {
      Emit(line_.substr(0, nl));
      line_.erase(0, nl + 1);
    }
    if (line_.size() > 4096) {
      Emit(line_);
      line_.clear();
    }
    return static_cast<std::int32_t>(n);
  }
  int Sync() override {
    std::lock_guard lk(mu_);
    if (!line_.empty()) Emit(line_);
    line_.clear();
    return 0;
  }
  bool IsStream() const override { return true; }
  StatInfo Stat() override {
    StatInfo s;
    s.is_char_dev = true;
    return s;
  }

 private:
  void Emit(const std::string& s) {
    if (which_ == 2) log::Warn("[guest stderr] {}", s);
    else log::Info("[guest stdout] {}", s);
  }
  int which_;
  std::mutex mu_;
  std::string line_;
};

class DevNode final : public Node {
 public:
  enum Kind { Null, Zero, Random };
  explicit DevNode(Kind k) : k_(k) {}
  std::int64_t Size() override { return 0; }
  std::int32_t Pread(void* buf, std::uint32_t n, std::int64_t) override {
    if (k_ == Null) return 0;
    if (k_ == Zero) {
      std::memset(buf, 0, n);
      return static_cast<std::int32_t>(n);
    }
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard lk(mu);
    auto* p = static_cast<std::uint8_t*>(buf);
    for (std::uint32_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(rng());
    return static_cast<std::int32_t>(n);
  }
  std::int32_t Pwrite(const void*, std::uint32_t n, std::int64_t) override { return static_cast<std::int32_t>(n); }
  bool IsStream() const override { return true; }
  StatInfo Stat() override {
    StatInfo s;
    s.is_char_dev = true;
    return s;
  }

 private:
  Kind k_;
};

class DirNode final : public Node {
 public:
  std::int64_t Size() override { return 4096; }
  std::int32_t Pread(void*, std::uint32_t, std::int64_t) override { return -err::ISDIR; }
  bool IsDir() const override { return true; }
  StatInfo Stat() override {
    StatInfo s;
    s.is_dir = true;
    s.size = 4096;
    return s;
  }
};

std::string MemInfo() {
  MEMORYSTATUSEX ms{sizeof ms};
  GlobalMemoryStatusEx(&ms);
  // Perfil de memoria del juego: se anuncia como maximo 2 GiB (dispositivo de gama alta de 2014).
  const unsigned long long total_kb = std::min<unsigned long long>(ms.ullTotalPhys / 1024, 2097152ull);
  const unsigned long long free_kb = std::min<unsigned long long>(ms.ullAvailPhys / 1024, total_kb / 2);
  return std::format("MemTotal:       {} kB\nMemFree:        {} kB\nBuffers:            0 kB\nCached:         {} kB\n",
                     total_kb, free_kb, total_kb / 8);
}
std::string CpuInfo() {
  std::string s;
  const unsigned n = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  for (unsigned i = 0; i < n; ++i)
    s += std::format("processor\t: {}\nBogoMIPS\t: 2000.00\nFeatures\t: swp half thumb fastmult vfp edsp neon vfpv3 tls\n\n", i);
  s += "Hardware\t: Zombiewood\n";
  return s;
}

std::shared_ptr<Node> Special(const std::string& p, bool* is_special) {
  *is_special = true;
  if (p == "/proc/meminfo") {
    const auto s = MemInfo();
    return std::make_shared<MemNode>(std::vector<std::uint8_t>(s.begin(), s.end()), false);
  }
  if (p == "/proc/cpuinfo") {
    const auto s = CpuInfo();
    return std::make_shared<MemNode>(std::vector<std::uint8_t>(s.begin(), s.end()), false);
  }
  if (p == "/dev/null") return std::make_shared<DevNode>(DevNode::Null);
  if (p == "/dev/zero") return std::make_shared<DevNode>(DevNode::Zero);
  if (p == "/dev/urandom" || p == "/dev/random") return std::make_shared<DevNode>(DevNode::Random);
  *is_special = false;
  return nullptr;
}

struct HostStat {
  bool exists = false, is_dir = false, read_only = false;
  std::int64_t size = 0, mtime = 0;
};
HostStat StatHost(const std::filesystem::path& hp) {
  HostStat s;
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(hp.c_str(), GetFileExInfoStandard, &d)) return s;
  s.exists = true;
  s.is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  s.read_only = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  s.size = (static_cast<std::int64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  s.mtime = FileTimeToUnix((static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime);
  return s;
}

std::uint64_t HashPath(const std::string& p) {
  std::uint64_t h = 1469598103934665603ull;
  for (char c : Lower(p)) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  return h | 1;
}

// Resultado de buscar una ruta guest en las monturas.
struct Lookup {
  const Mount* m = nullptr;
  std::string rel;
  std::filesystem::path host;  // si m->has_host
  HostStat hs;
  std::shared_ptr<ZipArchive> zip;  // zip que contiene la entrada/dir
  const ZipArchive::Entry* entry = nullptr;
  bool zip_dir = false;
  bool is_virtual_dir = false;
};

Lookup Find(const std::string& p) {
  Lookup L;
  L.m = FindMount(p, L.rel);
  if (!L.m) {
    L.is_virtual_dir = VirtualChildren(p, nullptr);
    return L;
  }
  if (L.m->has_host) {
    L.host = L.rel.empty() ? L.m->host : L.m->host / FromUtf8(L.rel);
    L.hs = StatHost(L.host);
    if (L.hs.exists) return L;
  }
  for (const auto& z : L.m->zips) {
    if (L.rel.empty() || z->IsDir(L.rel)) {
      L.zip = z;
      L.zip_dir = true;
      return L;
    }
    if (const auto* e = z->Find(L.rel)) {
      L.zip = z;
      L.entry = e;
      return L;
    }
  }
  if (L.rel.empty()) L.zip_dir = true;  // la raiz de una montura siempre es un directorio
  if (!L.zip_dir) L.is_virtual_dir = VirtualChildren(p, nullptr);
  return L;
}

bool ParentIsDir(const std::string& p) {
  const auto slash = p.rfind('/');
  const std::string parent = slash == 0 ? "/" : p.substr(0, slash);
  StatInfo st;
  return Stat(parent, st) == 0 && st.is_dir;
}

// Crea en el host la carpeta padre de `host` si en el guest existe (p. ej. solo en el zip).
void EnsureHostParent(const Lookup& L) {
  std::error_code ec;
  std::filesystem::create_directories(L.host.parent_path(), ec);
}

// --- fds -----------------------------------------------------------------------------------------
std::mutex g_fd_mu;
std::vector<std::shared_ptr<OpenFile>>* g_fds = nullptr;
std::vector<std::shared_ptr<OpenFile>>& Fds() {
  if (!g_fds) {
    g_fds = new std::vector<std::shared_ptr<OpenFile>>(3);
    for (int i = 0; i < 3; ++i) {
      auto f = std::make_shared<OpenFile>();
      f->node = std::make_shared<StreamNode>(i);
      f->can_read = i == 0;
      f->can_write = i != 0;
      f->flags = i == 0 ? kO_RDONLY : kO_WRONLY;
      f->path = i == 0 ? "/dev/stdin" : i == 1 ? "/dev/stdout" : "/dev/stderr";
      (*g_fds)[i] = f;
    }
  }
  return *g_fds;
}
constexpr int kMaxFds = 1024;

}  // namespace

// ------------------------------------------------------------------------------------------------
std::shared_ptr<Node> MakeMemNode(std::vector<std::uint8_t> data, bool writable) {
  return std::make_shared<MemNode>(std::move(data), writable);
}
std::shared_ptr<Node> MakeStdStream(int which) {
  std::lock_guard lk(g_fd_mu);
  return Fds()[which] ? Fds()[which]->node : std::make_shared<StreamNode>(which);
}

int FdInstall(std::shared_ptr<OpenFile> f, int min_fd) {
  std::lock_guard lk(g_fd_mu);
  auto& t = Fds();
  for (int i = std::max(0, min_fd); i < kMaxFds; ++i) {
    if (i >= static_cast<int>(t.size())) t.resize(i + 1);
    if (!t[i]) {
      t[i] = std::move(f);
      return i;
    }
  }
  return -err::MFILE;
}
std::shared_ptr<OpenFile> FdGet(int fd) {
  std::lock_guard lk(g_fd_mu);
  auto& t = Fds();
  return fd >= 0 && fd < static_cast<int>(t.size()) ? t[fd] : nullptr;
}
int FdClose(int fd) {
  std::shared_ptr<OpenFile> f;
  {
    std::lock_guard lk(g_fd_mu);
    auto& t = Fds();
    if (fd < 0 || fd >= static_cast<int>(t.size()) || !t[fd]) return -err::BADF;
    f = std::move(t[fd]);
    t[fd] = nullptr;
  }
  if (f.use_count() == 1) f->node->Sync();
  return 0;
}
int FdDup(int fd, int min_fd) {
  auto f = FdGet(fd);
  if (!f) return -err::BADF;
  return FdInstall(f, min_fd);
}

std::string Normalize(const std::string& path) {
  std::string in = path;
  if (in.empty()) return "";
  std::replace(in.begin(), in.end(), '\\', '/');
  if (in[0] != '/') in = Cwd() + "/" + in;
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < in.size()) {
    std::size_t j = in.find('/', i);
    if (j == std::string::npos) j = in.size();
    const std::string comp = in.substr(i, j - i);
    if (comp.empty() || comp == ".") {
    } else if (comp == "..") {
      if (!parts.empty()) parts.pop_back();
    } else {
      parts.push_back(comp);
    }
    i = j + 1;
  }
  std::string out;
  for (const auto& p : parts) out += "/" + p;
  if (out.empty()) out = "/";
  // Alias de la tarjeta SD.
  static const char* kAliases[] = {"/mnt/sdcard", "/storage/sdcard0", "/storage/emulated/0", "/storage/emulated/legacy",
                                   "/mnt/sdcard0"};
  const std::string lc = Lower(out);
  for (const char* a : kAliases) {
    const std::size_t n = std::strlen(a);
    if (lc.compare(0, n, a) == 0 && (lc.size() == n || lc[n] == '/')) {
      out = "/sdcard" + out.substr(n);
      break;
    }
  }
  return out;
}

int Open(const std::string& path, int flags, std::shared_ptr<OpenFile>& out) {
  const std::string p = Normalize(path);
  if (p.empty()) return err::NOENT;
  const int acc = flags & kO_ACCMODE;
  const bool wants_write = acc != kO_RDONLY || (flags & (kO_TRUNC | kO_CREAT));
  auto make = [&](std::shared_ptr<Node> n) {
    out = std::make_shared<OpenFile>();
    out->node = std::move(n);
    out->flags = flags;
    out->can_read = acc != kO_WRONLY;
    out->can_write = acc != kO_RDONLY;
    out->append = (flags & kO_APPEND) != 0;
    out->path = p;
    return 0;
  };
  bool special = false;
  if (auto n = Special(p, &special)) return make(n);

  std::shared_lock lk(g_mu);
  Lookup L = Find(p);
  if (!L.m) {
    if (L.is_virtual_dir) return wants_write ? err::ISDIR : make(std::make_shared<DirNode>());
    return wants_write ? err::ACCES : err::NOENT;
  }
  if ((flags & kO_CREAT) && (flags & kO_EXCL) && (L.hs.exists || L.entry || L.zip_dir || L.is_virtual_dir)) return err::EXIST;
  if (L.hs.exists) {
    if (L.hs.is_dir) return wants_write ? err::ISDIR : make(std::make_shared<DirNode>());
    if (wants_write && L.m->read_only) return err::ROFS;
    if (flags & kO_DIRECTORY) return err::NOTDIR;
    int e = 0;
    auto n = OpenHostFile(L.host, flags, e);
    return n ? make(n) : e;
  }
  if (L.zip_dir || L.is_virtual_dir) return wants_write ? err::ISDIR : make(std::make_shared<DirNode>());
  if (L.entry) {
    if (flags & kO_DIRECTORY) return err::NOTDIR;
    if (!wants_write) {
      int e = 0;
      auto n = L.zip->OpenEntry(*L.entry, e);
      return n ? make(n) : e;
    }
    if (!L.m->has_host || L.m->read_only) return err::ROFS;
    // Copia al host (copy-up) para poder escribir sobre un fichero que solo existe en el zip.
    EnsureHostParent(L);
    int e = 0;
    auto dst = OpenHostFile(L.host, kO_RDWR | kO_CREAT | kO_TRUNC, e);
    if (!dst) return e;
    if (!(flags & kO_TRUNC)) {
      auto src = L.zip->OpenEntry(*L.entry, e);
      if (!src) return e;
      std::vector<std::uint8_t> buf(1 << 16);
      for (std::int64_t off = 0;;) {
        const auto r = src->Pread(buf.data(), static_cast<std::uint32_t>(buf.size()), off);
        if (r <= 0) break;
        dst->Pwrite(buf.data(), static_cast<std::uint32_t>(r), off);
        off += r;
      }
    }
    dst.reset();
    auto n = OpenHostFile(L.host, flags & ~(kO_CREAT | kO_EXCL | kO_TRUNC), e);
    return n ? make(n) : e;
  }
  // No existe.
  if (!(flags & kO_CREAT)) return err::NOENT;
  if (!L.m->has_host) return err::ROFS;
  if (L.m->read_only) return err::ROFS;
  lk.unlock();
  if (!ParentIsDir(p)) return err::NOENT;
  EnsureHostParent(L);
  int e = 0;
  auto n = OpenHostFile(L.host, flags, e);
  return n ? make(n) : e;
}

int Stat(const std::string& path, StatInfo& st) {
  const std::string p = Normalize(path);
  if (p.empty()) return err::NOENT;
  st = StatInfo{};
  bool special = false;
  if (auto n = Special(p, &special)) {
    st = n->Stat();
    st.ino = HashPath(p);
    return 0;
  }
  std::shared_lock lk(g_mu);
  Lookup L = Find(p);
  st.ino = HashPath(p);
  if (L.hs.exists) {
    st.is_dir = L.hs.is_dir;
    st.size = L.hs.is_dir ? 4096 : L.hs.size;
    st.mtime = L.hs.mtime;
    st.read_only = L.m->read_only || L.hs.read_only;
    return 0;
  }
  if (L.entry) {
    st.size = L.entry->usize;
    st.mtime = L.entry->mtime;
    st.read_only = true;
    return 0;
  }
  if (L.zip_dir || L.is_virtual_dir) {
    st.is_dir = true;
    st.size = 4096;
    st.read_only = !L.m || !L.m->has_host;
    return 0;
  }
  return err::NOENT;
}

int Mkdir(const std::string& path) {
  const std::string p = Normalize(path);
  StatInfo st;
  if (Stat(p, st) == 0) return err::EXIST;
  if (!ParentIsDir(p)) return err::NOENT;
  std::shared_lock lk(g_mu);
  Lookup L = Find(p);
  if (!L.m) return err::ACCES;
  if (!L.m->has_host || L.m->read_only) return err::ROFS;
  std::error_code ec;
  std::filesystem::create_directories(L.host, ec);
  return ec ? err::ACCES : 0;
}

int Rmdir(const std::string& path) {
  std::shared_lock lk(g_mu);
  Lookup L = Find(Normalize(path));
  if (L.hs.exists) {
    if (!L.hs.is_dir) return err::NOTDIR;
    if (L.m->read_only) return err::ROFS;
    if (!RemoveDirectoryW(L.host.c_str())) return HostErrno(GetLastError());
    return 0;
  }
  if (L.zip_dir || L.is_virtual_dir) return err::ROFS;
  return err::NOENT;
}

int Unlink(const std::string& path) {
  std::shared_lock lk(g_mu);
  Lookup L = Find(Normalize(path));
  if (L.hs.exists) {
    if (L.hs.is_dir) return err::ISDIR;
    if (L.m->read_only) return err::ROFS;
    if (!DeleteFileW(L.host.c_str())) return HostErrno(GetLastError());
    return 0;
  }
  if (L.entry) return err::ROFS;
  if (L.zip_dir || L.is_virtual_dir) return err::ISDIR;
  return err::NOENT;
}

int Rename(const std::string& from, const std::string& to) {
  std::shared_lock lk(g_mu);
  Lookup A = Find(Normalize(from));
  Lookup B = Find(Normalize(to));
  if (!A.hs.exists) return A.entry || A.zip_dir ? err::ROFS : err::NOENT;
  if (!B.m || !B.m->has_host) return err::ACCES;
  if (A.m->read_only || B.m->read_only) return err::ROFS;
  if (!MoveFileExW(A.host.c_str(), B.host.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
    return HostErrno(GetLastError());
  return 0;
}

int Truncate(const std::string& path, std::int64_t size) {
  std::shared_ptr<OpenFile> f;
  const int e = Open(path, kO_WRONLY, f);
  if (e) return e;
  const int r = f->node->Truncate(size);
  return r < 0 ? -r : 0;
}

int ListDir(const std::string& path, std::vector<DirEntry>& out) {
  const std::string p = Normalize(path);
  std::shared_lock lk(g_mu);
  Lookup L = Find(p);
  bool is_dir = false;
  auto add = [&](const DirEntry& e) {
    for (const auto& x : out)
      if (Lower(x.name) == Lower(e.name)) return;
    out.push_back(e);
  };
  if (L.hs.exists) {
    if (!L.hs.is_dir) return err::NOTDIR;
    is_dir = true;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(L.host, ec))
      add(DirEntry{ToUtf8(de.path().filename()), de.is_directory(ec)});
  } else if (L.entry) {
    return err::NOTDIR;
  }
  if (L.m) {
    for (const auto& z : L.m->zips)
      if (L.rel.empty() || z->IsDir(L.rel)) {
        is_dir = true;
        std::vector<DirEntry> tmp;
        z->List(L.rel, tmp);
        for (const auto& e : tmp) add(e);
      }
    if (L.rel.empty()) is_dir = true;
  }
  std::vector<DirEntry> virt;
  if (VirtualChildren(p, &virt)) {
    is_dir = true;
    for (const auto& e : virt) add(e);
  }
  return is_dir ? 0 : err::NOENT;
}

// --- API publica ------------------------------------------------------------------------------
void MountHostDir(const std::string& guest_prefix, const std::filesystem::path& host_dir, bool read_only) {
  const std::string pre = Normalize(guest_prefix);
  std::unique_lock lk(g_mu);
  for (auto& m : Mounts())
    if (m.prefix_lc == Lower(pre)) {
      m.host = host_dir;
      m.has_host = true;
      m.read_only = read_only;
      return;
    }
  Mount m;
  m.prefix = pre;
  m.prefix_lc = Lower(pre);
  m.host = host_dir;
  m.has_host = true;
  m.read_only = read_only;
  Mounts().push_back(std::move(m));
  log::Info("vfs: {} -> {}{}", pre, host_dir.string(), read_only ? " (solo lectura)" : "");
}

bool MountZip(const std::string& guest_prefix, const std::filesystem::path& zip_path) {
  auto z = ZipArchive::Open(zip_path);
  if (!z) return false;
  const std::string pre = Normalize(guest_prefix);
  std::unique_lock lk(g_mu);
  for (auto& m : Mounts())
    if (m.prefix_lc == Lower(pre)) {
      m.zips.push_back(z);
      return true;
    }
  Mount m;
  m.prefix = pre;
  m.prefix_lc = Lower(pre);
  m.zips.push_back(z);
  Mounts().push_back(std::move(m));
  return true;
}

void UnmountAll() {
  std::unique_lock lk(g_mu);
  Mounts().clear();
}

std::filesystem::path HostPathFor(const std::string& guest_path) {
  std::shared_lock lk(g_mu);
  std::string rel;
  const Mount* m = FindMount(Normalize(guest_path), rel);
  if (!m || !m->has_host) return {};
  return rel.empty() ? m->host : m->host / FromUtf8(rel);
}

void SetCwd(const std::string& guest_dir) {
  const std::string p = Normalize(guest_dir);
  std::lock_guard lk(g_cwd_mu);
  if (!g_cwd) g_cwd = new std::string("/");
  *g_cwd = p;
}
std::string Cwd() {
  std::lock_guard lk(g_cwd_mu);
  return g_cwd ? *g_cwd : std::string("/");
}

}  // namespace vfs

// ------------------------------------------------------------------------------------------------
VfsConfig DefaultVfsConfig() {
  VfsConfig c;
  const auto home = paths::Home();
  c.data_dir = home / "data";
  c.saves_dir = home / "saves";
  c.obb_dir = home / "obb";
  const auto cfg = LoadConfig();
  if (cfg.is_object() && cfg.contains("vfs") && cfg["vfs"].is_object()) {
    const auto& v = cfg["vfs"];
    auto path_of = [&](const char* k, std::filesystem::path& dst) {
      if (v.contains(k) && v[k].is_string()) {
        const std::string s = v[k].get<std::string>();
        std::filesystem::path p = vfs::FromUtf8(s);
        dst = p.is_absolute() ? p : home / p;
      }
    };
    path_of("data_dir", c.data_dir);
    path_of("saves_dir", c.saves_dir);
    path_of("obb_dir", c.obb_dir);
    if (v.contains("mount_obbs_in_data") && v["mount_obbs_in_data"].is_boolean())
      c.mount_obbs_in_data = v["mount_obbs_in_data"].get<bool>();
  }
  return c;
}

void ConfigureVfs(const VfsConfig& cfg) {
  using namespace vfs;
  UnmountAll();
  std::error_code ec;
  std::filesystem::create_directories(cfg.data_dir, ec);
  std::filesystem::create_directories(cfg.saves_dir, ec);
  std::filesystem::create_directories(cfg.data_dir / "gameloft", ec);
  const std::string pkg = kPkg;
  const std::string files = "/sdcard/android/data/" + pkg + "/files";
  MountHostDir("/data/data/" + pkg, cfg.saves_dir);
  MountHostDir(files, cfg.data_dir);
  MountHostDir("/sdcard/gameloft/games", cfg.data_dir / "gameloft");
  if (!cfg.obb_dir.empty()) {
    MountHostDir("/sdcard/android/obb/" + pkg, cfg.obb_dir, /*read_only=*/true);
    if (cfg.mount_obbs_in_data && std::filesystem::is_directory(cfg.obb_dir, ec)) {
      // patch primero (gana sobre main), luego main.
      std::vector<std::filesystem::path> patch, main;
      for (const auto& de : std::filesystem::directory_iterator(cfg.obb_dir, ec)) {
        const std::string n = Lower(vfs::ToUtf8(de.path().filename()));
        if (n.size() < 4 || n.substr(n.size() - 4) != ".obb") continue;
        if (n.rfind("patch.", 0) == 0) patch.push_back(de.path());
        else if (n.rfind("main.", 0) == 0) main.push_back(de.path());
      }
      for (const auto& p : patch) MountZip(files, p);
      for (const auto& p : main) MountZip(files, p);
    }
  }
  {
    std::unique_lock lk(vfs::g_mu);
    vfs::g_configured = true;
  }
}

bool VfsConfigured() {
  std::shared_lock lk(vfs::g_mu);
  return vfs::g_configured;
}

}  // namespace zw::hle::libc
