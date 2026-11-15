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
  st.level = mem::R32(game + 0x1c4);
  st.loaded = mem::R8(game + 0x100);
  st.pause_flag = mem::R8(game + 0x1d4);
  // +0x100 solo vale 1 un instante al terminar de cargar (QA 0.3.0): "en escena" = hay CLevel y ya termino de cargar
  // al menos una vez desde que aparecio ese CLevel. Durante la pantalla de carga el CLevel ya existe pero no cuenta.
  static unsigned loaded_for = 0;  // CLevel* que ya termino de cargar
  if (st.level == 0) loaded_for = 0;
  else if (st.loaded) loaded_for = st.level;
  st.in_level = st.level != 0 && loaded_for == st.level;
  st.paused = st.in_level && st.pause_flag != 0;
  return st;
}

}  // namespace zw::hud
