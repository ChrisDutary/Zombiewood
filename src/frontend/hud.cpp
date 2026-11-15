#include "frontend/hud.h"

#include "cpu/guest_memory.h"
#include "loader/loader.h"

namespace zw::hud {

GameState Read() {
  static const gaddr sym = loader::FindSymbol("_ZN13SingletonFastI11ZombiesGameE10s_instanceE");
  GameState st;
  if (!sym || !GuestMemory::IsMapped(sym, 4)) return st;
  const gaddr game = mem::R32(sym);
  if (!game || !GuestMemory::IsMapped(game, 0x1d8)) return st;
  st.valid = true;
  const gaddr level = mem::R32(game + 0x1c4);
  st.in_level = level != 0 && mem::R8(game + 0x100) != 0;
  st.paused = st.in_level && mem::R8(game + 0x1d4) != 0;
  return st;
}

}  // namespace zw::hud
