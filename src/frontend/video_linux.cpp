// Videos del juego en Linux: todavia no hay reproductor (en Windows es Media Foundation, video.cpp).
// Los videos (logo de inicio, intro, "movie complete") se saltan y el juego continua como si hubieran terminado
// o el usuario los hubiera saltado. Ver docs/LINUX.md ("Known limits").
#include "frontend/video.h"

#include "core/log.h"

namespace zw::video {

Result Play(const std::filesystem::path& file, const PlayParams&) {
  static bool told = false;
  if (!told) {
    told = true;
    log::Warn("video: la version de Linux aun no reproduce videos; se saltan todos (equivale a skip_videos)");
  }
  log::Info("video: {} saltado (sin reproductor en Linux)", file.filename().string());
  return Result::Skipped;
}

}  // namespace zw::video
