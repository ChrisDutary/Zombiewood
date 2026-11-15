// Piezas del frontend que dependen del sistema operativo. Implementadas en platform_win.cpp (Windows) y
// platform_linux.cpp (Linux); el resto del frontend no necesita #ifdef.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <SDL.h>

namespace zw::platform {

// Lo primero de main(), antes de log::Init y de SDL_Init:
//  - Windows: informe de cierre (minidump logs/crash-<fecha>.dmp), consola del terminal padre, hints del icono.
//  - Linux: informe de cierre (logs/crash-<fecha>.txt con backtrace), gamecontrollerdb.txt junto al ejecutable.
void EarlyInit();

// Icono de la ventana. Windows: no hace nada (recurso 101 del exe via hint). Linux: icono incrustado en el build
// (resources/zombiewood.ico), si existe.
void ApplyWindowIcon(SDL_Window* w);

// Cursor incrustado en formato "ZWCU" (tools/make_cursor.ps1). false si el ejecutable no lo trae.
// Windows: recurso RCDATA 102. Linux: datos generados por tools/embed/zw_embed.cpp.
bool EmbeddedCursor(const std::uint8_t*& data, std::size_t& size);

// ¿Hay un dialogo nativo de "abrir archivo"? Windows: siempre. Linux: si estan zenity o kdialog.
// Si no lo hay, el asistente ofrece un campo de texto para escribir o pegar la ruta.
bool HasFileDialog();
// Dialogo "abrir archivo". filter_ext sin punto ("apk", "obb"). Vacio = cancelado o sin dialogo.
std::vector<std::filesystem::path> OpenFileDialog(SDL_Window* owner, const char* title, const char* filter_name,
                                                  const char* filter_ext, bool multi);

}  // namespace zw::platform
