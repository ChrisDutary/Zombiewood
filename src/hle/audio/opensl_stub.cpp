// OpenSL ES: camino muerto en el juego (s_androidAPILevel = 3 -> AudioTrack). Solo hay que resolver
// los 4 imports; slCreateEngine falla con SL_RESULT_FEATURE_UNSUPPORTED y el juego cae a AudioTrack.
#include <cstdint>

#include "audio_hle.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"

namespace {
constexpr uint32_t kSlResultFeatureUnsupported = 12;

// SLInterfaceID_ = { u32 time_low; u16 time_mid; u16 time_hi_and_version; u16 clock_seq; u8 node[6]; } (16 bytes)
// `extern const SLInterfaceID SL_IID_X` es una variable PUNTERO (4 bytes) a ese struct.
void define_iid(const char* name, uint32_t lo, uint16_t mid, uint16_t hi, uint16_t seq) {
  const zw::gaddr guid = zw::GuestMemory::Calloc(1, 16);
  zw::mem::W32(guid, lo);
  zw::mem::W16(guid + 4, mid);
  zw::mem::W16(guid + 6, hi);
  zw::mem::W16(guid + 8, seq);
  const uint8_t node[6] = {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b};
  for (int i = 0; i < 6; ++i) zw::mem::W8(guid + 10 + (zw::gaddr)i, node[i]);
  zw::mem::W32(zw::hle::DefineData(name, 4), guid);
}

// SLresult slCreateEngine(SLObjectItf* pEngine, SLuint32 numOptions, const SLEngineOption*, SLuint32 numIfaces,
//                         const SLInterfaceID*, const SLboolean*)
uint32_t hle_slCreateEngine(zw::GPtr<uint32_t> engine, uint32_t, zw::gaddr, uint32_t, zw::gaddr, zw::gaddr) {
  if (engine.addr) zw::mem::W32(engine.addr, 0);
  return kSlResultFeatureUnsupported;
}
}  // namespace

void zw::hle::audio::Register() {
  define_iid("SL_IID_ENGINE", 0x8d97c260, 0xddd4, 0x11db, 0x958f);
  define_iid("SL_IID_PLAY", 0xef0bd9c0, 0xddd4, 0x11db, 0xbf49);
  define_iid("SL_IID_BUFFERQUEUE", 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99);
  ZW_HLE("slCreateEngine", hle_slCreateEngine);
}
