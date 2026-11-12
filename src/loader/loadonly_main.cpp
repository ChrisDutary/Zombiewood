// zw_loadonly <ruta .so> [mas .so...]: mismo diagnostico que `zombiewood --load-only`.
#include <cstdio>
#include <filesystem>
#include <vector>

#include "core/log.h"
#include "loader/load_only.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::puts("uso: zw_loadonly <ruta .so> [mas .so...]");
    return 2;
  }
  zw::log::Init();
  std::vector<std::filesystem::path> libs;
  for (int i = 1; i < argc; ++i) libs.emplace_back(argv[i]);
  return zw::loader::RunLoadOnly(libs);
}
