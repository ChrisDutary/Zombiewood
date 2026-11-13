#pragma once
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace zw::log {
enum class Level { Debug, Info, Warn, Error };

// Rota los logs (<Home>/logs/zombiewood.log -> zombiewood.1.log ... se conservan 5; tambien rota en caliente
// al pasar de 32 MiB), abre uno nuevo y habilita salida por consola.
void Init();
// Escribe los resumenes de repeticiones pendientes y cierra el fichero.
void Shutdown();
// Mensajes Info/Debug identicos repetidos mas de 3 veces en 30 s se suprimen y se resumen como "(xN ...)".
// Activo por defecto; ZW_LOG_NODEDUP=1 lo desactiva.
void SetDedup(bool on);
void Write(Level lvl, std::string_view msg);

template <class... A>
void Debug(std::format_string<A...> f, A&&... a) { Write(Level::Debug, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void Info(std::format_string<A...> f, A&&... a) { Write(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void Warn(std::format_string<A...> f, A&&... a) { Write(Level::Warn, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void Error(std::format_string<A...> f, A&&... a) { Write(Level::Error, std::format(f, std::forward<A>(a)...)); }
}  // namespace zw::log
