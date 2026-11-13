// HLE de bionic (T4): libc, libm, libdl, pthread, libstdc++ (runtime), liblog y sockets.
// API publica del modulo zw_hle_libc. Ver docs/CONTRACTS.md (§0 registro) y docs/ANALYSIS.md §3/§6.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace zw::hle::libc {

// Registra todos los handlers y datos (__sF, _ctype_, __stack_chk_guard...). Llamar ANTES de
// loader::Load(). Inicializa la memoria guest si hace falta y configura el VFS con
// DefaultVfsConfig() si nadie lo ha configurado antes. Idempotente (re-registrar sustituye).
void Register();

// --- VFS: rutas Android -> host ------------------------------------------------------------
struct VfsConfig {
  // /sdcard/android/data/<pkg>/files  (comparacion sin mayusculas) y /sdcard/gameloft/games
  std::filesystem::path data_dir;
  // /data/data/<pkg>
  std::filesystem::path saves_dir;
  // /sdcard/Android/obb/<pkg>  (solo lectura). Vacio = sin OBB.
  std::filesystem::path obb_dir;
  // Monta los .obb de obb_dir (patch encima de main) en solo lectura bajo la carpeta de datos
  // (/sdcard/android/data/<pkg>/files), como si el instalador Java los hubiera extraido.
  bool mount_obbs_in_data = true;
};

// Valores por defecto: <home>/data, <home>/saves, <home>/obb; sobreescribibles en zombiewood.json:
//   { "vfs": { "data_dir": "...", "saves_dir": "...", "obb_dir": "...", "mount_obbs_in_data": true } }
VfsConfig DefaultVfsConfig();
// (Re)configura las monturas. Crea data_dir/saves_dir si no existen. Thread-safe.
void ConfigureVfs(const VfsConfig& cfg);

namespace vfs {
// Monturas adicionales (T8/T9/tests). `guest_prefix` absoluto, sin '/' final.
void MountHostDir(const std::string& guest_prefix, const std::filesystem::path& host_dir, bool read_only = false);
// Monta un ZIP (entradas STORED o DEFLATED) en solo lectura. Las monturas ZIP sobre el mismo
// prefijo se consultan despues de la carpeta host y en orden de montaje (el primero gana).
bool MountZip(const std::string& guest_prefix, const std::filesystem::path& zip_path);
void UnmountAll();
// Ruta host para una ruta guest dentro de una carpeta host montada ("" si no aplica).
std::filesystem::path HostPathFor(const std::string& guest_path);
// Directorio de trabajo virtual (chdir/getcwd). Por defecto "/".
void SetCwd(const std::string& guest_dir);
std::string Cwd();
}  // namespace vfs

// Ejecuta (en orden inverso) las funciones registradas con __cxa_atexit (para un cierre ordenado
// opcional de T8). No se llama automaticamente.
void RunAtexit();

}  // namespace zw::hle::libc
