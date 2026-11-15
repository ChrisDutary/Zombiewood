// SHA-256 portable (FIPS 180-4), sin dependencias: sustituye a BCrypt para que el instalador compile igual en
// Windows y Linux. Se usa para verificar libZombieWood.so y los OBB de la version 1.5.3.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace zw::installer {

class Sha256 {
 public:
  Sha256();
  void Update(const void* data, std::size_t n);
  // Finaliza y devuelve el hash en hexadecimal (minusculas). No reutilizar el objeto despues.
  std::string HexDigest();

 private:
  void Block(const std::uint8_t* p);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t buf_len_ = 0;
  std::uint64_t total_ = 0;
};

}  // namespace zw::installer
