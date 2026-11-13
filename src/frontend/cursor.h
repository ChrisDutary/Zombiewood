// Cursor del raton del port (v0.2.0): cursor "zombi" incrustado en el exe (recurso RCDATA 102, generado con
// tools/make_cursor.ps1) o el cursor normal de Windows. Solo cambia la imagen del puntero del sistema: el juego
// no lo dibuja ni lo ve, asi que no afecta al rendimiento ni a la logica.
#pragma once
#include <string>

namespace zw::cursor {

// ¿El exe trae el cursor zombi incrustado?
bool Available();
// Aplica el cursor segun la configuracion: mode = "zombie" | "system"; size en px (se usa el tamano incrustado
// mas cercano: 32/48/64/96). Llamar SIEMPRE desde el hilo UI (el que bombea los eventos de SDL).
void Apply(const std::string& mode, int size);
// Libera el cursor creado (antes de SDL_Quit).
void Shutdown();

}  // namespace zw::cursor
