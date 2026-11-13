// Integracion (T8): arranque del juego, hilo GL con el bucle step/swap, input y capturas.
#pragma once
#include <filesystem>

namespace zw {

struct GameOptions {
  std::filesystem::path lib;      // --lib=<ruta libZombieWood.so> (por defecto, de zombiewood.json)
  int max_frames = -1;            // --frames=N: salir tras N frames (pruebas)
  std::filesystem::path script;   // --script=<fichero>: comandos de prueba (ver game.cpp, ExecCommand)
  std::filesystem::path control;  // --control=<fichero>: igual, pero se leen las lineas que se van anadiendo
  int shot_every = 0;             // --shot-every=N: captura automatica cada N frames
  bool no_audio = false;          // --no-audio: AudioTrack nulo
  bool uncapped = false;          // --uncapped: sin vsync ni limite de fps (medir rendimiento)
  bool hidden = false;            // --hidden: ventana oculta (pruebas automaticas)
};

// Devuelve el codigo de salida del proceso. No vuelve hasta que se cierra la ventana o el juego sale.
int RunGame(const GameOptions& opt);

}  // namespace zw
