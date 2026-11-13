// zw_libc_loadonly <ruta .so> [...]: `--load-only` con la libc de T4 registrada (en vez de solo la
// HLE minima de arranque). Lo que quede "sin handler" deberia ser GLES/OpenSL/JNI.
#include <filesystem>
#include <vector>

#include "core/log.h"
#include "cpu/guest_memory.h"
#include "hle/libc/libc.h"
#include "loader/load_only.h"

int main(int argc, char** argv) {
  zw::log::Init();
  zw::GuestMemory::Init();
  zw::hle::libc::Register();
  std::vector<std::filesystem::path> libs;
  for (int i = 1; i < argc; ++i) libs.emplace_back(argv[i]);
  return zw::loader::RunLoadOnly(libs);
}
