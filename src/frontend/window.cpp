#include "frontend/window.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <sstream>

#include "core/log.h"

namespace zw {

bool Window::Create(int w, int h, const char* title, bool hidden) {
  // Pedir a SDL el driver EGL/GLES (ANGLE: libEGL.dll + libGLESv2.dll junto al exe) en vez de WGL.
  SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  // ES 3.0 si ANGLE lo da (el frontend usa glBlitFramebuffer para escalar); el juego solo usa ES 2.0.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                 (hidden ? SDL_WINDOW_HIDDEN : 0));
  if (!window_) {
    log::Error("SDL_CreateWindow fallo: {}", SDL_GetError());
    return false;
  }
  ctx_ = SDL_GL_CreateContext(window_);
  if (!ctx_) {
    // Reintento con ES 2.0 (hay que recrear la ventana: la config EGL depende de la version).
    log::Warn("contexto GLES 3.0 no disponible ({}), probando 2.0", SDL_GetError());
    SDL_DestroyWindow(window_);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                   (hidden ? SDL_WINDOW_HIDDEN : 0));
    ctx_ = window_ ? SDL_GL_CreateContext(window_) : nullptr;
  }
  if (!ctx_) {
    log::Error("SDL_GL_CreateContext (GLES2/ANGLE) fallo: {}", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(window_, ctx_);
  SDL_GL_SetSwapInterval(1);

  auto s = [](GLenum e) {
    const GLubyte* p = glGetString(e);
    return std::string(p ? reinterpret_cast<const char*>(p) : "(null)");
  };
  log::Info("GL_VERSION  : {}", s(GL_VERSION));
  log::Info("GL_RENDERER : {}", s(GL_RENDERER));
  log::Info("GL_VENDOR   : {}", s(GL_VENDOR));
  log::Info("GL_SHADING_LANGUAGE_VERSION: {}", s(GL_SHADING_LANGUAGE_VERSION));

  std::istringstream ss(s(GL_EXTENSIONS));
  for (std::string e; ss >> e;) exts_.push_back(e);
  log::Info("GL_EXTENSIONS: {}", exts_.size());
  for (const char* want : {"GL_OES_compressed_ETC1_RGB8_texture", "GL_EXT_texture_compression_dxt1",
                           "GL_ANGLE_texture_compression_dxt5", "GL_OES_packed_depth_stencil",
                           "GL_OES_vertex_array_object", "GL_OES_depth_texture"})
    log::Info("ext {} : {}", want, HasExtension(want) ? "SI" : "NO");
  return true;
}

bool Window::HasExtension(const std::string& name) const {
  return std::find(exts_.begin(), exts_.end(), name) != exts_.end();
}

void Window::SwapBuffers() { SDL_GL_SwapWindow(window_); }

Window::~Window() {
  if (ctx_) SDL_GL_DeleteContext(ctx_);
  if (window_) SDL_DestroyWindow(window_);
}

}  // namespace zw
