#include "frontend/i18n.h"

#include <cstdlib>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace zw::i18n {
namespace {

std::string g_lang = "es";

// Espanol (clave) -> ingles. Mantener ordenado por pantalla.
const std::unordered_map<std::string_view, const char*>& English() {
  static const std::unordered_map<std::string_view, const char*> m = {
      // Barra de menu
      {"Reanudar", "Resume"},
      {"Pausar", "Pause"},
      {"Captura de pantalla", "Screenshot"},
      {"Ocultar menu", "Hide menu"},
      {"Salir", "Quit"},
      {"Graficos", "Graphics"},
      {"Opciones de graficos...", "Graphics options..."},
      {"Pantalla completa", "Fullscreen"},
      {"Mostrar FPS", "Show FPS"},
      {"Opciones de audio...", "Audio options..."},
      {"Silenciar", "Mute"},
      {"Controles", "Controls"},
      {"Remapear controles...", "Remap controls..."},
      {"Carpetas", "Folders"},
      {"Partidas guardadas (saves)", "Saved games (saves)"},
      {"Copias de seguridad de saves", "Save backups"},
      {"Crear copia de seguridad ahora", "Back up now"},
      {"Copia de seguridad creada", "Backup created"},
      {"Capturas", "Screenshots"},
      {"Datos del juego", "Game data"},
      {"Carpeta del port (zombiewood.json)", "Port folder (zombiewood.json)"},
      {"Ayuda", "Help"},
      {"Acerca de / estado", "About / status"},
      {"No se pudo guardar zombiewood.json", "Could not save zombiewood.json"},
      {"Nueva version disponible: ", "New version available: "},
      {"Descargar", "Download"},
      // Graficos
      {"Direct3D 11 (recomendado)", "Direct3D 11 (recommended)"},
      {"OpenGL (escritorio, via ANGLE)", "OpenGL (desktop, via ANGLE)"},
      {"Se aplica al reiniciar el juego.", "Applies after restarting the game."},
      {"Resolucion interna = ventana", "Internal resolution = window"},
      {"Resolucion interna", "Internal resolution"},
      {"854 x 480 (referencia del HUD)", "854 x 480 (HUD reference)"},
      {"(personalizada)", "(custom)"},
      {"Filtrado lineal al escalar", "Linear filtering when scaling"},
      {"Limite FPS (0 = libre)", "FPS limit (0 = unlimited)"},
      {"Pantalla completa (F11)", "Fullscreen (F11)"},
      {"Pausar al perder el foco", "Pause when the window loses focus"},
      {"Cursor zombi", "Zombie cursor"},
      {"Pequeno (32 px)", "Small (32 px)"},
      {"Grande (64 px)", "Large (64 px)"},
      {"Muy grande (96 px)", "Extra large (96 px)"},
      {"Tamano del cursor", "Cursor size"},
      {"Saltar videos", "Skip videos"},
      {"Logo de Gameloft al arrancar", "Gameloft logo at startup"},
      {"Idioma del juego", "Game language"},
      {"El idioma del juego se aplica al reiniciar.", "The game language applies after restarting."},
      {"Idioma del menu", "Menu language"},
      {"Automatico (sistema)", "Automatic (system)"},
      {"Sticks tactiles en pantalla", "On-screen touch sticks"},
      {"Automatico (ocultar con teclado/mando)", "Automatic (hide with keyboard/gamepad)"},
      {"Mostrar siempre", "Always show"},
      {"Ocultar siempre", "Always hide"},
      {"Solo dentro de las escenas. Con el raton como toque se muestran.",
       "Only inside scenes. They are shown while you use the mouse as touch."},
      {"Ayudas de botones en pantalla", "On-screen button hints"},
      {"Buscar actualizaciones al iniciar", "Check for updates at startup"},
      {"Consulta las releases publicas de GitHub (una vez al dia). No envia datos tuyos.",
       "Queries the public GitHub releases (once a day). It sends no data about you."},
      // Audio
      {"Volumen del juego", "Game volume"},
      {"Volumen de los videos", "Video volume"},
      {"Baja (80 ms)", "Low (80 ms)"},
      {"Segura (250 ms)", "Safe (250 ms)"},
      {"Latencia de audio", "Audio latency"},
      {"Si el audio se corta, usa Segura. Se aplica al reiniciar.", "If the audio stutters, use Safe. Applies after restarting."},
      {"Cortes de audio en esta sesion: %llu", "Audio dropouts this session: %llu"},
      // Controles
      {"Asistente (%zu/%zu): pulsa en el MANDO el boton para \"%s\"  (Esc cancela)",
       "Wizard (%zu/%zu): press the GAMEPAD button for \"%s\"  (Esc cancels)"},
      {"Pulsa una tecla o un boton del mando para \"%s\"...", "Press a key or gamepad button for \"%s\"..."},
      {"\"Cambiar\" sustituye; \"+\" anade otra tecla/boton; \"x\" borra.",
       "\"Change\" replaces; \"+\" adds another key/button; \"x\" clears."},
      {"Cambiar", "Change"},
      {"Accion", "Action"},
      {"Teclado", "Keyboard"},
      {"Mando", "Gamepad"},
      {"Mover", "Move"},
      {"Apuntar", "Aim"},
      {"arriba", "up"},
      {"abajo", "down"},
      {"izquierda", "left"},
      {"derecha", "right"},
      {"Boton HUD: ", "HUD button: "},
      {"botiquin", "medkit"},
      {"arma", "weapon"},
      {"pausa", "pause"},
      {"aceptar", "accept"},
      {"Atras (BACK)", "Back (BACK)"},
      {"Tecla Android ", "Android key "},
      {"Zona muerta del mando", "Gamepad dead zone"},
      {"Clic izquierdo = tocar la pantalla", "Left click = touch the screen"},
      {"clic derecho", "right click"},
      {"clic central", "middle click"},
      {"desactivado", "disabled"},
      {"Apuntar/disparar con el raton", "Aim/shoot with the mouse"},
      {"Restaurar controles por defecto", "Restore default controls"},
      {"Asistente cancelado", "Wizard cancelled"},
      {"Asignado: ", "Assigned: "},
      {"mando ", "gamepad "},
      {"Mando configurado. Prueba los botones en la partida.", "Gamepad configured. Try the buttons in a match."},
      {"  (sin mapeo: modo crudo)", "  (no mapping: raw mode)"},
      {"No hay ningun mando conectado.", "No gamepad connected."},
      {"Stick izq. %+.2f %+.2f   Stick der. %+.2f %+.2f   %s %.2f  %s %.2f",
       "Left stick %+.2f %+.2f   Right stick %+.2f %+.2f   %s %.2f  %s %.2f"},
      {"Botones pulsados: %s", "Pressed buttons: %s"},
      {"Aun no llega nada del mando. Si pulsas y no cambia: cierra Steam/DS4Windows o usa el driver \"Compatibilidad\".",
       "Nothing received from the gamepad yet. If pressing does not change it: close Steam/DS4Windows or use the "
       "\"Compatibility\" driver."},
      {"Intercambiar sticks (derecho = mover, izquierdo = apuntar)", "Swap sticks (right = move, left = aim)"},
      {"Automatico (recomendado)", "Automatic (recommended)"},
      {"Compatibilidad (sin HIDAPI)", "Compatibility (no HIDAPI)"},
      {"Driver de mandos", "Gamepad driver"},
      {"El driver se aplica al reiniciar el juego.", "The driver applies after restarting the game."},
      {"Asistente: mapear con el mando", "Wizard: map with the gamepad"},
      {"Cancelar asistente", "Cancel wizard"},
      {"Pide cada accion en orden; los sticks se configuran arriba.", "Asks for each action in turn; sticks are configured above."},
      {"Cruz", "Cross"},
      {"Circulo", "Circle"},
      {"Cuadrado", "Square"},
      {"Triangulo", "Triangle"},
      {"Cruceta arriba", "D-pad up"},
      {"Cruceta abajo", "D-pad down"},
      {"Cruceta izq.", "D-pad left"},
      {"Cruceta der.", "D-pad right"},
      // Acerca de
      {"Acerca de", "About"},
      {"Zombiewood - port nativo para PC (so-loader + JIT ARM)", "Zombiewood - native PC port (so-loader + ARM JIT)"},
      {"Los datos del juego son de tu copia; el port no distribuye nada de Gameloft.",
       "Game data comes from your own copy; the port distributes nothing from Gameloft."},
      {"Backend configurado: %s", "Configured backend: %s"},
      {"Interna %dx%d, ventana %dx%d", "Internal %dx%d, window %dx%d"},
      {"Datos: %s", "Data: %s"},
      {"Version: %s", "Version: %s"},
      {"F1 menu | F11 pantalla completa | F12 captura | Esc atras", "F1 menu | F11 fullscreen | F12 screenshot | Esc back"},
      // Asistente de instalacion
      {"ZOMBIEWOOD  -  port nativo para PC", "ZOMBIEWOOD  -  native PC port"},
      {"No se encontraron los datos del juego.", "Game data not found."},
      {"Este port no incluye ningun archivo de Zombiewood: necesitas tu propia copia del juego para Android "
       "(version 1.5.3). Elige el APK:",
       "This port does not include any Zombiewood files: you need your own copy of the Android game (version 1.5.3). "
       "Choose the APK:"},
      {"el APK grande (incluye assets/archiveObbCache.zip.00N con los OBB), o",
       "the large APK (it includes assets/archiveObbCache.zip.00N with the OBBs), or"},
      {"el APK original + los archivos main.1501...obb y patch.15325...obb.",
       "the original APK + the main.1501...obb and patch.15325...obb files."},
      {"Se extraera en: %s", "It will be extracted to: %s"},
      {"Elegir APK... (Enter)", "Choose APK... (Enter)"},
      {"Instalando desde %s", "Installing from %s"},
      {"Cancelar", "Cancel"},
      {"Instalacion completada (%zu archivos).", "Installation complete (%zu files)."},
      {"version 1.5.3 verificada (SHA-256)", "version 1.5.3 verified (SHA-256)"},
      {"VERSION NO VERIFICADA", "VERSION NOT VERIFIED"},
      {"Aviso: %s", "Warning: %s"},
      {"Pulsa F1 durante el juego para abrir el menu (graficos, audio, controles, carpetas).",
       "Press F1 in game to open the menu (graphics, audio, controls, folders)."},
      {"Jugar (Enter)", "Play (Enter)"},
      {"Abrir carpeta de datos", "Open data folder"},
      {"No se pudo instalar:", "Installation failed:"},
      {"Elegir otro APK...", "Choose another APK..."},
      {"Ver logs", "View logs"},
      {"El archivo elegido no es un APK de Zombiewood valido:\n", "The chosen file is not a valid Zombiewood APK:\n"},
      {"ZIP ilegible", "unreadable ZIP"},
      {"Este APK no trae los datos (OBB).\n\nElige ahora los archivos main.1501...obb y patch.15325...obb de tu copia del "
       "juego (normalmente en Android/obb/com.gameloft.android.ANMP.GloftZOHM).",
       "This APK does not include the data (OBB).\n\nNow choose the main.1501...obb and patch.15325...obb files from your "
       "copy of the game (usually in Android/obb/com.gameloft.android.ANMP.GloftZOHM)."},
      {"Sin los archivos .obb no se puede instalar el juego.", "The game cannot be installed without the .obb files."},
      {"Elige el APK de Zombiewood (1.5.3)", "Choose the Zombiewood APK (1.5.3)"},
      {"Elige los OBB de Zombiewood (main y patch)", "Choose the Zombiewood OBBs (main and patch)"},
  };
  return m;
}

std::string SystemLanguage() {
#ifdef _WIN32
  const LANGID id = GetUserDefaultUILanguage();
  return PRIMARYLANGID(id) == LANG_SPANISH ? "es" : "en";
#else
  for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (const char* v = std::getenv(var); v && *v) return std::string_view(v).substr(0, 2) == "es" ? "es" : "en";
  return "en";
#endif
}

}  // namespace

std::string SetLanguage(const std::string& pref) {
  g_lang = pref == "es" || pref == "en" ? pref : SystemLanguage();
  return g_lang;
}

const std::string& Language() { return g_lang; }

}  // namespace zw::i18n

namespace zw {
const char* T(const char* es) {
  if (i18n::Language() != "en" || !es) return es;
  const auto& m = i18n::English();
  const auto it = m.find(es);
  return it == m.end() ? es : it->second;
}
}  // namespace zw
