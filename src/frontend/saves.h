// Copias de seguridad de la partida (T9).
// El guest escribe sus saves en /data/data/<pkg>/ = <home>/saves/ (VFS de T4). Este vigilante mantiene en
// <home>/saves_backup/shadow/ la ultima version estable de cada fichero; cuando el juego sobrescribe uno,
// la version anterior (la del shadow) se mueve a <home>/saves_backup/<fecha_hora>/ ANTES de actualizar el
// shadow: siempre queda la version previa a cada sobrescritura. Al arrancar se hace tambien un conjunto
// "<fecha_hora>_inicio" con todo. Se conservan los `keep` conjuntos mas recientes.
// Un fichero se considera "estable" cuando su tamano y fecha no cambian entre dos sondeos (cada 2 s), para
// no copiar un save a medio escribir.
#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace zw::saves {

class Backup {
 public:
  ~Backup() { Stop(); }
  void Start(const std::filesystem::path& saves_dir, const std::filesystem::path& backup_dir, int keep);
  void Stop();
  // Copia completa inmediata ("manual", "salida"...). Devuelve la carpeta creada ("" si no habia nada).
  std::filesystem::path Snapshot(const std::string& tag);
  // Fuerza un sondeo ahora (p. ej. justo antes de pausar el juego, que guarda al pausar).
  void Poll();
  const std::filesystem::path& backup_dir() const { return backup_dir_; }
  int sets_created() const { return sets_; }

 private:
  struct Stamp {
    std::uintmax_t size = 0;
    long long mtime = 0;
    bool operator==(const Stamp&) const = default;
  };
  struct State {
    Stamp seen, shadow;
    bool has_shadow = false;
  };
  void PollLocked();
  std::filesystem::path NewSetDir(const std::string& tag);
  void Prune();

  std::filesystem::path saves_dir_, backup_dir_;
  int keep_ = 20;
  std::mutex mu_;
  std::unordered_map<std::string, State> files_;  // ruta relativa (generic)
  std::thread th_;
  std::atomic<bool> stop_{false};
  int sets_ = 0;
};

}  // namespace zw::saves
