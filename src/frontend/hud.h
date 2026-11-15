// Estado del juego visto desde el port (v0.3.0), leido de la memoria del juego SIN modificar nada.
// Sirve para mostrar ayudas de botones y ocultar los sticks tactiles solo dentro de una escena.
//
// Datos (ANALYSIS.md / disasm de ZombiesGame):
//   SingletonFast<ZombiesGame>::s_instance  -> ZombiesGame*
//   ZombiesGame+0x1c4  CLevel* (no nulo con una escena cargada; CleanupLevel lo pone a 0)
//   ZombiesGame+0x100  byte = 1 tras OnLoadLevelEnd (escena terminada de cargar)
//   ZombiesGame+0x1d4  byte = 1 en PauseLevel (menu de pausa)
// Application::XperiaToggleJoysticks(int) (via Game.nativeXperiaToggleJoysticks) cambia el frame de
// "menu_Hud.myJoystick" / "menu_Hud.myTrigger" del HUD Flash; solo actua en el estado "GSLevel".
#pragma once

namespace zw::hud {

struct GameState {
  bool valid = false;     // se pudo leer el estado
  bool in_level = false;  // escena cargada (jugando o en su menu de pausa)
  bool paused = false;
};

// Llamar desde el hilo GL entre frames (lee memoria guest; no ejecuta codigo del juego).
GameState Read();

}  // namespace zw::hud
