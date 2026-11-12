#include "cpu/deps_check.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

namespace zw {
namespace {
struct Env final : Dynarmic::A32::UserCallbacks {
  std::array<std::uint8_t, 4096> mem{};
  std::uint64_t ticks = 0;
  std::uint8_t MemoryRead8(std::uint32_t a) override { return a < mem.size() ? mem[a] : 0; }
  std::uint16_t MemoryRead16(std::uint32_t a) override { std::uint16_t v = 0; if (a + 2 <= mem.size()) std::memcpy(&v, &mem[a], 2); return v; }
  std::uint32_t MemoryRead32(std::uint32_t a) override { std::uint32_t v = 0; if (a + 4 <= mem.size()) std::memcpy(&v, &mem[a], 4); return v; }
  std::uint64_t MemoryRead64(std::uint32_t a) override { std::uint64_t v = 0; if (a + 8 <= mem.size()) std::memcpy(&v, &mem[a], 8); return v; }
  void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { if (a < mem.size()) mem[a] = v; }
  void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { if (a + 2 <= mem.size()) std::memcpy(&mem[a], &v, 2); }
  void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { if (a + 4 <= mem.size()) std::memcpy(&mem[a], &v, 4); }
  void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { if (a + 8 <= mem.size()) std::memcpy(&mem[a], &v, 8); }
  void InterpreterFallback(std::uint32_t, std::size_t) override {}
  void CallSVC(std::uint32_t) override {}
  void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override {}
  void AddTicks(std::uint64_t t) override { ticks += t; }
  std::uint64_t GetTicksRemaining() override { return ticks < 100 ? 100 - ticks : 0; }
};
}  // namespace

int CpuDepsSelfTest() {
  Env env;
  const std::uint32_t prog[] = {0xE3A0002A /* mov r0,#42 */, 0xEAFFFFFE /* b . */};
  std::memcpy(env.mem.data(), prog, sizeof prog);
  Dynarmic::A32::UserConfig cfg;
  cfg.callbacks = &env;
  Dynarmic::A32::Jit jit{cfg};
  jit.Regs()[15] = 0;
  jit.SetCpsr(0x000001D0);
  jit.Run();
  return static_cast<int>(jit.Regs()[0]);
}
}  // namespace zw
