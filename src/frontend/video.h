// Reproductor de los videos del juego (T9): res/raw/*.mp4 con Windows Media Foundation.
// IMFSourceReader (H.264 -> RGB32 con el procesador de video de MF, AAC -> PCM 16 bits) -> textura GLES (ANGLE)
// escalada con letterbox en el framebuffer 0 + audio por un dispositivo SDL propio. Sin dependencias nuevas
// (mfplat/mfreadwrite/mfuuid del SDK de Windows).
#pragma once
#include <filesystem>
#include <functional>

#include <SDL.h>

namespace zw::video {

struct PlayParams {
  SDL_Window* window = nullptr;  // contexto GL actual en el hilo que llama
  float volume = 1.0f;           // 0..1
  bool linear_filter = true;
  // Llamado ~cada frame: true = saltar el video (clic/tecla) o salir. Puede bombear eventos.
  std::function<bool()> should_skip;
  // Llamado tras dibujar cada frame del video y antes del swap (p. ej. overlay ImGui). Opcional.
  std::function<void(int dw, int dh)> overlay;
};

enum class Result { Finished, Skipped, Failed };

// Bloqueante: reproduce `file` a pantalla completa en el contexto GL actual. Restaura el estado GL que toca.
Result Play(const std::filesystem::path& file, const PlayParams& p);

}  // namespace zw::video
