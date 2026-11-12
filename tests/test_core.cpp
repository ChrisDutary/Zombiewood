#include <cstdio>

#include "core/paths.h"
#include "cpu/deps_check.h"

int main() {
  if (zw::paths::ExeDir().empty()) { std::puts("ExeDir vacio"); return 1; }
  int r0 = zw::CpuDepsSelfTest();
  if (r0 != 42) { std::printf("dynarmic r0=%d (esperado 42)\n", r0); return 2; }
  std::puts("ok");
  return 0;
}
