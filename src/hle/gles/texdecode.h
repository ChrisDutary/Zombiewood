// Decodificadores software de texturas comprimidas -> RGBA8 (T5).
// Se usan cuando ANGLE no soporta el formato (PVRTC, ATC) o como respaldo de ETC1.
#pragma once
#include <cstdint>
#include <vector>

namespace zw::hle::gles::texdecode {

// Formatos GL (gl2ext.h) que sabemos decodificar.
constexpr std::uint32_t kETC1_RGB8 = 0x8D64;                 // GL_ETC1_RGB8_OES
constexpr std::uint32_t kPVRTC_RGB_4BPP = 0x8C00;            // GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
constexpr std::uint32_t kPVRTC_RGB_2BPP = 0x8C01;
constexpr std::uint32_t kPVRTC_RGBA_4BPP = 0x8C02;
constexpr std::uint32_t kPVRTC_RGBA_2BPP = 0x8C03;
constexpr std::uint32_t kATC_RGB = 0x8C92;                   // GL_ATC_RGB_AMD
constexpr std::uint32_t kATC_RGBA_EXPLICIT = 0x8C93;         // GL_ATC_RGBA_EXPLICIT_ALPHA_AMD
constexpr std::uint32_t kATC_RGBA_INTERPOLATED = 0x87EE;     // GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD

bool IsPVRTC(std::uint32_t fmt);
bool IsATC(std::uint32_t fmt);
bool CanDecode(std::uint32_t fmt);  // ETC1, PVRTC o ATC

// Tamano en bytes de los datos comprimidos de una imagen w x h (0 si el formato no se conoce).
std::uint32_t CompressedSize(std::uint32_t fmt, int w, int h);

// Decodifica `src` (size bytes) a RGBA8 (w*h*4 bytes, filas de abajo a arriba como GL: fila 0 = primera
// fila de datos). Devuelve false si el formato no se conoce o faltan datos.
bool Decode(std::uint32_t fmt, const std::uint8_t* src, std::uint32_t size, int w, int h,
            std::vector<std::uint8_t>& out_rgba);

// Bloques individuales (expuestos para tests).
void DecodeETC1Block(const std::uint8_t block[8], std::uint8_t out_rgba[4 * 4 * 4]);  // fila mayor, 4x4

}  // namespace zw::hle::gles::texdecode
