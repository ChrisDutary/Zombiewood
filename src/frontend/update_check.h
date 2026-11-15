// Aviso de version nueva (v0.3.0). Consulta en segundo plano las releases publicas de GitHub
// (api.github.com/repos/ChrisDutary/Zombiewood/releases), como mucho una vez al dia; el resultado se guarda en
// <home>/update_check.json. No envia ningun dato del usuario (solo una peticion GET anonima con User-Agent generico).
// Se ignoran las builds de prueba (tags con "-test").
#pragma once
#include <filesystem>
#include <string>

namespace zw::update {

void Start(const std::filesystem::path& home);  // lanza la consulta en un hilo (no bloquea)
// true si hay una version publicada mas nueva que ZW_VERSION; rellena tag (p. ej. "v0.3.1") y url de la release.
bool Available(std::string& tag, std::string& url);

}  // namespace zw::update
