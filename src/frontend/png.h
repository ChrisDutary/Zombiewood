#pragma once
#include <cstdint>
#include <filesystem>

namespace zw {
// Escribe un PNG RGB8 sin comprimir a partir de un buffer RGBA8 (p. ej. glReadPixels; flip_y = true
// para pasar del origen abajo-izquierda de GL al de la imagen).
bool WritePngRgba(const std::filesystem::path& path, const std::uint8_t* rgba, int w, int h, bool flip_y);
}  // namespace zw
