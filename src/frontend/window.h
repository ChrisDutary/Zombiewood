#pragma once
#include <SDL.h>

#include <string>
#include <vector>

namespace zw {
// Ventana SDL2 + contexto OpenGL ES 2.0 de ANGLE (EGL, backend D3D11).
class Window {
 public:
  ~Window();
  bool Create(int w, int h, const char* title);
  void SwapBuffers();
  SDL_Window* sdl() const { return window_; }
  SDL_GLContext gl() const { return ctx_; }
  // Cadena GL_EXTENSIONS partida en nombres individuales.
  const std::vector<std::string>& Extensions() const { return exts_; }
  bool HasExtension(const std::string& name) const;

 private:
  SDL_Window* window_ = nullptr;
  SDL_GLContext ctx_ = nullptr;
  std::vector<std::string> exts_;
};
}  // namespace zw
