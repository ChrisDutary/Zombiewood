#pragma once
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace zw::log {
enum class Level { Debug, Info, Warn, Error };

// Abre <Home>/logs/zombiewood.log (truncado) y habilita salida por consola.
void Init();
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
