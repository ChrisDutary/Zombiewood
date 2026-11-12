// HLE de libGLESv2 (99 imports del juego) -> ANGLE (T5). Ver docs/CONTRACTS.md §0.4.
//
// Modelo de contexto (ANALYSIS §7): un unico contexto GLES2 (el que crea el frontend con SDL/ANGLE) y un unico
// hilo GL: el hilo host que llama a step/resize/... por call_guest. Todas las llamadas gl* del guest se
// ejecutan directamente en el hilo que las hace; si ese hilo no tiene contexto EGL actual la llamada se
// descarta y se registra un error (una vez por hilo).
//
// Variables de entorno:
//   ZW_GL_CHECK=1          glGetError tras cada llamada, con el nombre de la funcion en el log (el error se
//                          conserva para el siguiente glGetError del guest).
//   ZW_GL_ETC1_SOFT=1      decodificar ETC1 por software (por defecto se sube como ETC2 RGB8 si ANGLE lo soporta).
//   ZW_GL_NO_MAPBUFFER=1   no anunciar GL_OES_mapbuffer (las funciones siguen funcionando).
//   ZW_GL_LOG_SHADERS=1    loguear el codigo de todos los shaders compilados (por defecto solo los que fallan).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace zw::hle::gles {

// Registra los handlers de los 99 imports gl*. Llamar antes de loader::Load (no necesita contexto GL).
void Register();

// Nombres de todos los imports que registra Register() (para evidencia/tests).
const std::vector<std::string>& ImportNames();

// Olvida el estado cacheado (bindings, buffers sombra, capacidades, cadenas). Llamar si el frontend
// destruye y recrea el contexto GL. Las cadenas guest ya entregadas siguen siendo validas.
void ResetContextState();

struct Caps {
  bool probed = false;
  int angle_major = 0, angle_minor = 0;  // version real del contexto ANGLE
  bool etc1_native = false;              // ANGLE expone GL_OES_compressed_ETC1_RGB8_texture
  bool etc2 = false;                     // ETC1 se sube como GL_COMPRESSED_RGB8_ETC2
  bool depth_texture = false;            // anunciamos GL_OES_depth_texture (sondeado)
  bool standard_derivatives = false;
  bool mapbuffer = false;
  std::string extensions;                // la lista que ve el guest
};
// Capacidades detectadas (sondea si hace falta; requiere contexto actual).
const Caps& GetCaps();

// Opciones para tests (normalmente vienen del entorno en Register()).
void SetGlCheck(bool on);
void SetForceSoftwareETC1(bool on);

}  // namespace zw::hle::gles
