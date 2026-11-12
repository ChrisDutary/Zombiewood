// Registro HLE del modulo de audio (T6): imports de OpenSL ES (stubs) + datos SL_IID_*.
#pragma once
namespace zw::hle::audio {
// Registra slCreateEngine y define SL_IID_ENGINE / SL_IID_PLAY / SL_IID_BUFFERQUEUE. Llamar antes de loader::Load().
void Register();
}  // namespace zw::hle::audio
