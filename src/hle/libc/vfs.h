// VFS interno de la HLE de libc: nodos de fichero, tabla de descriptores y traduccion de rutas
// Android -> host (carpetas host + ZIP/OBB en solo lectura + ficheros sinteticos).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hle/libc/internal.h"

namespace zw::hle::libc::vfs {

// Flags de open() de Linux ARM.
inline constexpr int kO_ACCMODE = 3, kO_RDONLY = 0, kO_WRONLY = 1, kO_RDWR = 2, kO_CREAT = 0x40, kO_EXCL = 0x80,
                     kO_TRUNC = 0x200, kO_APPEND = 0x400, kO_NONBLOCK = 0x800, kO_DIRECTORY = 0x4000;

struct StatInfo {
  bool is_dir = false;
  bool read_only = false;
  std::int64_t size = 0;
  std::int64_t mtime = 0;  // segundos Unix
  std::uint64_t ino = 0;
  bool is_char_dev = false;
};

class Node {
 public:
  virtual ~Node() = default;
  virtual std::int64_t Size() = 0;
  // >= 0: bytes leidos/escritos; < 0: -errno.
  virtual std::int32_t Pread(void* buf, std::uint32_t n, std::int64_t off) = 0;
  virtual std::int32_t Pwrite(const void*, std::uint32_t, std::int64_t) { return -err::BADF; }
  virtual int Truncate(std::int64_t) { return -err::INVAL; }
  virtual int Sync() { return 0; }
  virtual StatInfo Stat() {
    StatInfo s;
    s.size = Size();
    return s;
  }
  // Flujo (stdout/stderr/stdin, /dev/*): no tiene posicion.
  virtual bool IsStream() const { return false; }
  virtual bool IsDir() const { return false; }
};

struct OpenFile {
  std::shared_ptr<Node> node;
  std::mutex mu;
  std::int64_t pos = 0;
  int flags = 0;  // flags de open (Linux)
  bool can_read = true, can_write = false, append = false;
  std::string path;  // ruta guest normalizada
};

// --- descriptores ----------------------------------------------------------------------------
int FdInstall(std::shared_ptr<OpenFile> f, int min_fd = 0);  // fd, o -errno
std::shared_ptr<OpenFile> FdGet(int fd);
int FdClose(int fd);  // 0 o -errno
int FdDup(int fd, int min_fd = 0);

// --- rutas -----------------------------------------------------------------------------------
std::string Normalize(const std::string& path);
// 0 o errno (positivo).
int Open(const std::string& path, int flags, std::shared_ptr<OpenFile>& out);
int Stat(const std::string& path, StatInfo& st);
int Mkdir(const std::string& path);
int Rmdir(const std::string& path);
int Unlink(const std::string& path);
int Rename(const std::string& from, const std::string& to);
int Truncate(const std::string& path, std::int64_t size);
struct DirEntry {
  std::string name;
  bool is_dir = false;
};
int ListDir(const std::string& path, std::vector<DirEntry>& out);

// Nodos auxiliares.
std::shared_ptr<Node> MakeMemNode(std::vector<std::uint8_t> data, bool writable);
std::shared_ptr<Node> MakeStdStream(int which);  // 0 stdin, 1 stdout, 2 stderr

// zip.cpp ---------------------------------------------------------------------------------------
class HostFile;
std::shared_ptr<Node> OpenHostFile(const std::filesystem::path& p, int flags, int& err_out);
int HostErrno(unsigned long win_err);
std::int64_t FileTimeToUnix(std::uint64_t ft);

class ZipArchive {
 public:
  struct Entry {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t csize = 0, usize = 0, lho = 0, crc = 0;
    std::int64_t mtime = 0;
  };
  static std::shared_ptr<ZipArchive> Open(const std::filesystem::path& p);
  const std::filesystem::path& path() const { return path_; }
  std::size_t size() const { return entries_.size(); }
  // `rel` relativa a la raiz del zip, sin '/' inicial. Comparacion sin mayusculas.
  const Entry* Find(const std::string& rel) const;
  bool IsDir(const std::string& rel) const;
  void List(const std::string& rel_dir, std::vector<DirEntry>& out) const;
  // Nodo de solo lectura para una entrada (STORED: lectura directa por offset; DEFLATED: se
  // descomprime en memoria). nullptr + err si falla.
  std::shared_ptr<Node> OpenEntry(const Entry& e, int& err);

 private:
  std::filesystem::path path_;
  std::shared_ptr<Node> raw_;
  std::vector<Entry> entries_;
  std::vector<std::pair<std::string, std::uint32_t>> index_;          // nombre en minusculas -> entrada
  std::vector<std::pair<std::string, std::vector<DirEntry>>> dirs_;  // dir en minusculas -> hijos
  std::mutex cache_mu_;
  std::vector<std::weak_ptr<std::vector<std::uint8_t>>> cache_;
};

// Descompresion DEFLATE cruda (metodo 8 de ZIP). true si produjo exactamente dst.size() bytes.
bool Inflate(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& dst);

std::string Lower(std::string s);

}  // namespace zw::hle::libc::vfs
