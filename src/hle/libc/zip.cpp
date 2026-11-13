// Ficheros host (Win32, lectura/escritura posicionada thread-safe), lector ZIP de solo lectura
// (OBB y .bar) e inflate propio (sin dependencias nuevas).
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "core/log.h"
#include "hle/libc/vfs.h"

namespace zw::hle::libc::vfs {

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return s;
}

int HostErrno(unsigned long e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME:
      return err::NOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return err::ACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return err::EXIST;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return err::NOSPC;
    case ERROR_DIR_NOT_EMPTY: return err::NOTEMPTY;
    case ERROR_FILENAME_EXCED_RANGE: return err::NAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return err::MFILE;
    case ERROR_WRITE_PROTECT: return err::ROFS;
    default: return err::IO;
  }
}

std::int64_t FileTimeToUnix(std::uint64_t ft) {
  return ft ? static_cast<std::int64_t>((ft - 116444736000000000ull) / 10000000ull) : 0;
}

// ---------------------------------------------------------------------------------------------
class HostFile final : public Node {
 public:
  explicit HostFile(HANDLE h) : h_(h) {}
  ~HostFile() override { CloseHandle(h_); }
  std::int64_t Size() override {
    LARGE_INTEGER li{};
    return GetFileSizeEx(h_, &li) ? li.QuadPart : 0;
  }
  std::int32_t Pread(void* buf, std::uint32_t n, std::int64_t off) override {
    if (off < 0) return -err::INVAL;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h_, buf, n, &got, &ov)) {
      const DWORD e = GetLastError();
      if (e == ERROR_HANDLE_EOF) return 0;
      return -HostErrno(e);
    }
    return static_cast<std::int32_t>(got);
  }
  std::int32_t Pwrite(const void* buf, std::uint32_t n, std::int64_t off) override {
    if (off < 0) return -err::INVAL;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD put = 0;
    if (!WriteFile(h_, buf, n, &put, &ov)) return -HostErrno(GetLastError());
    return static_cast<std::int32_t>(put);
  }
  int Truncate(std::int64_t size) override {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = size;
    return SetFileInformationByHandle(h_, FileEndOfFileInfo, &info, sizeof info) ? 0 : -HostErrno(GetLastError());
  }
  int Sync() override {
    FlushFileBuffers(h_);
    return 0;
  }
  StatInfo Stat() override {
    StatInfo s;
    BY_HANDLE_FILE_INFORMATION bi{};
    if (GetFileInformationByHandle(h_, &bi)) {
      s.size = (static_cast<std::int64_t>(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow;
      s.mtime = FileTimeToUnix((static_cast<std::uint64_t>(bi.ftLastWriteTime.dwHighDateTime) << 32) |
                               bi.ftLastWriteTime.dwLowDateTime);
      s.ino = (static_cast<std::uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
      s.read_only = (bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    }
    return s;
  }

 private:
  HANDLE h_;
};

std::shared_ptr<Node> OpenHostFile(const std::filesystem::path& p, int flags, int& e) {
  const int acc = flags & kO_ACCMODE;
  DWORD access = GENERIC_READ;
  if (acc == kO_WRONLY) access = GENERIC_WRITE;
  else if (acc == kO_RDWR) access = GENERIC_READ | GENERIC_WRITE;
  DWORD disp = OPEN_EXISTING;
  if (flags & kO_CREAT) {
    if (flags & kO_EXCL) disp = CREATE_NEW;
    else if (flags & kO_TRUNC) disp = CREATE_ALWAYS;
    else disp = OPEN_ALWAYS;
  } else if (flags & kO_TRUNC) {
    disp = TRUNCATE_EXISTING;
  }
  if (disp == TRUNCATE_EXISTING && !(access & GENERIC_WRITE)) access |= GENERIC_WRITE;
  HANDLE h = CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    e = HostErrno(GetLastError());
    return nullptr;
  }
  e = 0;
  return std::make_shared<HostFile>(h);
}

// ---------------------------------------------------------------------------------------------
// Inflate (RFC 1951), al estilo de puff.c de zlib.
namespace {
constexpr int kMaxBits = 15;
struct Huff {
  std::uint16_t count[kMaxBits + 1];
  std::uint16_t symbol[288];
};
struct InState {
  const std::uint8_t* in;
  std::size_t inlen, incnt = 0;
  std::uint32_t bitbuf = 0;
  int bitcnt = 0;
  std::uint8_t* out;
  std::size_t outlen, outcnt = 0;
  bool err = false;
  int Bits(int need) {
    std::uint64_t val = bitbuf;
    while (bitcnt < need) {
      if (incnt >= inlen) {
        err = true;
        return 0;
      }
      val |= static_cast<std::uint64_t>(in[incnt++]) << bitcnt;
      bitcnt += 8;
    }
    bitbuf = static_cast<std::uint32_t>(val >> need);
    bitcnt -= need;
    return static_cast<int>(val & ((1ull << need) - 1));
  }
};
int Decode(InState& s, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    code |= s.Bits(1);
    if (s.err) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}
int Construct(Huff& h, const std::uint16_t* length, int n) {
  std::memset(h.count, 0, sizeof h.count);
  for (int s = 0; s < n; ++s) h.count[length[s]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return left;
  }
  std::uint16_t offs[kMaxBits + 1];
  offs[1] = 0;
  for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + h.count[len]);
  for (int s = 0; s < n; ++s)
    if (length[s]) h.symbol[offs[length[s]]++] = static_cast<std::uint16_t>(s);
  return left;
}
const std::uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint16_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::uint16_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(InState& s, const Huff& lc, const Huff& dc) {
  for (;;) {
    int sym = Decode(s, lc);
    if (sym < 0) return false;
    if (sym < 256) {
      if (s.outcnt >= s.outlen) return false;
      s.out[s.outcnt++] = static_cast<std::uint8_t>(sym);
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = kLBase[sym] + s.Bits(kLExt[sym]);
      const int ds = Decode(s, dc);
      if (ds < 0 || ds >= 30) return false;
      const std::size_t dist = kDBase[ds] + static_cast<std::size_t>(s.Bits(kDExt[ds]));
      if (s.err || dist > s.outcnt || s.outcnt + len > s.outlen) return false;
      for (int i = 0; i < len; ++i, ++s.outcnt) s.out[s.outcnt] = s.out[s.outcnt - dist];
    }
  }
}
}  // namespace

bool Inflate(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& dst) {
  InState s{src, n};
  s.out = dst.data();
  s.outlen = dst.size();
  static Huff fixed_l, fixed_d;
  static const bool fixed_ready = [] {
    std::uint16_t lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    Construct(fixed_l, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    Construct(fixed_d, lengths, 30);
    return true;
  }();
  (void)fixed_ready;
  int last;
  do {
    last = s.Bits(1);
    const int type = s.Bits(2);
    if (s.err) return false;
    if (type == 0) {
      s.bitbuf = 0;
      s.bitcnt = 0;
      if (s.incnt + 4 > s.inlen) return false;
      const unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8);
      const unsigned nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8);
      s.incnt += 4;
      if (len != (~nlen & 0xFFFFu) || s.incnt + len > s.inlen || s.outcnt + len > s.outlen) return false;
      std::memcpy(s.out + s.outcnt, s.in + s.incnt, len);
      s.incnt += len;
      s.outcnt += len;
    } else if (type == 1) {
      if (!Codes(s, fixed_l, fixed_d)) return false;
    } else if (type == 2) {
      static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const int nlen = s.Bits(5) + 257, ndist = s.Bits(5) + 1, ncode = s.Bits(4) + 4;
      if (s.err || nlen > 286 || ndist > 30) return false;
      std::uint16_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<std::uint16_t>(s.Bits(3));
      Huff lencode, distcode;
      if (Construct(lencode, lengths, 19) != 0) return false;
      int idx = 0;
      while (idx < nlen + ndist) {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
          lengths[idx++] = static_cast<std::uint16_t>(sym);
        } else {
          int len = 0, rep;
          if (sym == 16) {
            if (!idx) return false;
            len = lengths[idx - 1];
            rep = 3 + s.Bits(2);
          } else if (sym == 17) {
            rep = 3 + s.Bits(3);
          } else {
            rep = 11 + s.Bits(7);
          }
          if (idx + rep > nlen + ndist) return false;
          while (rep--) lengths[idx++] = static_cast<std::uint16_t>(len);
        }
      }
      if (lengths[256] == 0) return false;
      const int el = Construct(lencode, lengths, nlen);
      if (el < 0 || (el > 0 && nlen - lencode.count[0] != 1)) return false;
      const int ed = Construct(distcode, lengths + nlen, ndist);
      if (ed < 0 || (ed > 0 && ndist - distcode.count[0] != 1)) return false;
      if (!Codes(s, lencode, distcode)) return false;
    } else {
      return false;
    }
  } while (!last);
  return s.outcnt == s.outlen;
}

// ---------------------------------------------------------------------------------------------
namespace {
class SliceNode final : public Node {
 public:
  SliceNode(std::shared_ptr<Node> raw, std::int64_t off, std::int64_t size) : raw_(std::move(raw)), off_(off), size_(size) {}
  std::int64_t Size() override { return size_; }
  std::int32_t Pread(void* buf, std::uint32_t n, std::int64_t off) override {
    if (off < 0) return -err::INVAL;
    if (off >= size_) return 0;
    const std::int64_t left = size_ - off;
    if (n > left) n = static_cast<std::uint32_t>(left);
    return raw_->Pread(buf, n, off_ + off);
  }
  StatInfo Stat() override {
    StatInfo s;
    s.size = size_;
    s.read_only = true;
    s.mtime = mtime;
    return s;
  }
  std::int64_t mtime = 0;

 private:
  std::shared_ptr<Node> raw_;
  std::int64_t off_, size_;
};

class SharedMemNode final : public Node {
 public:
  SharedMemNode(std::shared_ptr<std::vector<std::uint8_t>> d, std::int64_t mt) : d_(std::move(d)), mtime_(mt) {}
  std::int64_t Size() override { return static_cast<std::int64_t>(d_->size()); }
  std::int32_t Pread(void* buf, std::uint32_t n, std::int64_t off) override {
    if (off < 0) return -err::INVAL;
    if (off >= Size()) return 0;
    const std::int64_t left = Size() - off;
    if (n > left) n = static_cast<std::uint32_t>(left);
    std::memcpy(buf, d_->data() + off, n);
    return static_cast<std::int32_t>(n);
  }
  StatInfo Stat() override {
    StatInfo s;
    s.size = Size();
    s.read_only = true;
    s.mtime = mtime_;
    return s;
  }

 private:
  std::shared_ptr<std::vector<std::uint8_t>> d_;
  std::int64_t mtime_;
};

std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Le32(const std::uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int64_t DosTime(std::uint16_t t, std::uint16_t d) {
  std::tm tm{};
  tm.tm_sec = (t & 31) * 2;
  tm.tm_min = (t >> 5) & 63;
  tm.tm_hour = t >> 11;
  tm.tm_mday = d & 31;
  tm.tm_mon = ((d >> 5) & 15) - 1;
  tm.tm_year = (d >> 9) + 80;
  return static_cast<std::int64_t>(_mkgmtime(&tm));
}
}  // namespace

std::shared_ptr<ZipArchive> ZipArchive::Open(const std::filesystem::path& p) {
  int e = 0;
  auto raw = OpenHostFile(p, kO_RDONLY, e);
  if (!raw) {
    log::Warn("vfs: no se pudo abrir el zip {} (errno {})", p.string(), e);
    return nullptr;
  }
  const std::int64_t size = raw->Size();
  if (size < 22) return nullptr;
  const std::int64_t tail_n = std::min<std::int64_t>(size, 65536 + 22);
  std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_n));
  if (raw->Pread(tail.data(), static_cast<std::uint32_t>(tail_n), size - tail_n) != tail_n) return nullptr;
  std::int64_t eocd = -1;
  for (std::int64_t i = tail_n - 22; i >= 0; --i)
    if (Le32(&tail[i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    log::Warn("vfs: {} no es un zip (sin EOCD)", p.string());
    return nullptr;
  }
  const std::uint16_t count = Le16(&tail[eocd + 10]);
  const std::uint32_t cd_size = Le32(&tail[eocd + 12]), cd_off = Le32(&tail[eocd + 16]);
  if (cd_off == 0xFFFFFFFFu || static_cast<std::int64_t>(cd_off) + cd_size > size) {
    log::Warn("vfs: {}: zip64 o directorio central invalido", p.string());
    return nullptr;
  }
  std::vector<std::uint8_t> cd(cd_size);
  if (raw->Pread(cd.data(), cd_size, cd_off) != static_cast<std::int32_t>(cd_size)) return nullptr;

  auto z = std::shared_ptr<ZipArchive>(new ZipArchive());
  z->path_ = p;
  z->raw_ = raw;
  std::size_t off = 0;
  // Para no ser cuadratico con miles de entradas, primero se recolecta y luego se ordena.
  std::vector<std::tuple<std::string, std::string, bool>> children;  // (dir lc, nombre, is_dir)
  for (std::uint32_t i = 0; i < count && off + 46 <= cd.size(); ++i) {
    const std::uint8_t* h = &cd[off];
    if (Le32(h) != 0x02014b50) break;
    Entry en;
    en.method = Le16(h + 10);
    en.mtime = DosTime(Le16(h + 12), Le16(h + 14));
    en.crc = Le32(h + 16);
    en.csize = Le32(h + 20);
    en.usize = Le32(h + 24);
    const std::uint16_t nl = Le16(h + 28), xl = Le16(h + 30), cl = Le16(h + 32);
    en.lho = Le32(h + 42);
    if (off + 46 + nl > cd.size()) break;
    en.name.assign(reinterpret_cast<const char*>(h + 46), nl);
    std::replace(en.name.begin(), en.name.end(), '\\', '/');
    off += 46u + nl + xl + cl;
    // Directorios implicitos
    std::string name = en.name;
    const bool explicit_dir = !name.empty() && name.back() == '/';
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (name.empty()) continue;
    std::string cur = name;
    bool is_dir = explicit_dir;
    for (;;) {
      const auto slash = cur.rfind('/');
      const std::string parent = slash == std::string::npos ? "" : cur.substr(0, slash);
      children.emplace_back(Lower(parent), cur.substr(slash == std::string::npos ? 0 : slash + 1), is_dir);
      if (slash == std::string::npos) break;
      cur = parent;
      is_dir = true;
    }
    if (!explicit_dir) {
      z->index_.emplace_back(Lower(name), static_cast<std::uint32_t>(z->entries_.size()));
      z->entries_.push_back(std::move(en));
    }
  }
  std::sort(z->index_.begin(), z->index_.end());
  std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
    if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
    return Lower(std::get<1>(a)) < Lower(std::get<1>(b));
  });
  for (std::size_t i = 0; i < children.size(); ++i) {
    const auto& [d, n, isdir] = children[i];
    if (i && std::get<0>(children[i - 1]) == d && Lower(std::get<1>(children[i - 1])) == Lower(n)) {
      if (isdir) z->dirs_.back().second.back().is_dir = true;
      continue;
    }
    if (z->dirs_.empty() || z->dirs_.back().first != d) z->dirs_.emplace_back(d, std::vector<DirEntry>{});
    z->dirs_.back().second.push_back(DirEntry{n, isdir});
  }
  z->cache_.resize(z->entries_.size());
  log::Info("vfs: zip {} montado ({} entradas)", p.filename().string(), z->entries_.size());
  return z;
}

const ZipArchive::Entry* ZipArchive::Find(const std::string& rel) const {
  const std::string k = Lower(rel);
  auto it = std::lower_bound(index_.begin(), index_.end(), k, [](const auto& a, const std::string& b) { return a.first < b; });
  if (it == index_.end() || it->first != k) return nullptr;
  return &entries_[it->second];
}

bool ZipArchive::IsDir(const std::string& rel) const {
  const std::string k = Lower(rel);
  auto it = std::lower_bound(dirs_.begin(), dirs_.end(), k, [](const auto& a, const std::string& b) { return a.first < b; });
  return it != dirs_.end() && it->first == k;
}

void ZipArchive::List(const std::string& rel_dir, std::vector<DirEntry>& out) const {
  const std::string k = Lower(rel_dir);
  auto it = std::lower_bound(dirs_.begin(), dirs_.end(), k, [](const auto& a, const std::string& b) { return a.first < b; });
  if (it == dirs_.end() || it->first != k) return;
  out.insert(out.end(), it->second.begin(), it->second.end());
}

std::shared_ptr<Node> ZipArchive::OpenEntry(const Entry& e, int& err_out) {
  err_out = 0;
  std::uint8_t lh[30];
  if (raw_->Pread(lh, 30, e.lho) != 30 || Le32(lh) != 0x04034b50) {
    err_out = err::IO;
    return nullptr;
  }
  const std::int64_t data = static_cast<std::int64_t>(e.lho) + 30 + Le16(lh + 26) + Le16(lh + 28);
  if (e.method == 0) {
    auto n = std::make_shared<SliceNode>(raw_, data, e.usize);
    n->mtime = e.mtime;
    return n;
  }
  if (e.method != 8) {
    log::Warn("vfs: {}: metodo de compresion {} no soportado", e.name, e.method);
    err_out = err::IO;
    return nullptr;
  }
  const std::size_t idx = static_cast<std::size_t>(&e - entries_.data());
  {
    std::lock_guard lk(cache_mu_);
    if (auto sp = cache_[idx].lock()) return std::make_shared<SharedMemNode>(sp, e.mtime);
  }
  std::vector<std::uint8_t> comp(e.csize);
  if (e.csize && raw_->Pread(comp.data(), e.csize, data) != static_cast<std::int32_t>(e.csize)) {
    err_out = err::IO;
    return nullptr;
  }
  auto out = std::make_shared<std::vector<std::uint8_t>>(e.usize);
  if (!Inflate(comp.data(), comp.size(), *out)) {
    log::Error("vfs: {}: error descomprimiendo {}", path_.filename().string(), e.name);
    err_out = err::IO;
    return nullptr;
  }
  {
    std::lock_guard lk(cache_mu_);
    cache_[idx] = out;
  }
  return std::make_shared<SharedMemNode>(out, e.mtime);
}

}  // namespace zw::hle::libc::vfs
