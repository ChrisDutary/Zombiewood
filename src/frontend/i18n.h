// Idioma de la interfaz del port (v0.3.0): menu F1, asistente de instalacion y avisos. NO es el idioma del juego
// (ese lo controla "language" en zombiewood.json y lo aplica el propio juego).
//
// Uso: T("Texto en espanol") devuelve el mismo texto (espanol) o su traduccion inglesa segun el idioma activo.
// El texto espanol es la clave, asi un texto sin traducir simplemente se ve en espanol (nunca vacio).
#pragma once
#include <string>

namespace zw::i18n {

// "auto" (idioma del sistema), "es" o "en". Devuelve el idioma efectivo ("es" | "en").
std::string SetLanguage(const std::string& pref);
const std::string& Language();

}  // namespace zw::i18n

namespace zw {
// Traduce un texto de la interfaz. El puntero devuelto es estable durante todo el programa.
const char* T(const char* es);
}  // namespace zw
