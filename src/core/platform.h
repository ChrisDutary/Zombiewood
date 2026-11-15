// Pequenas diferencias entre Windows (MSVC) y Linux (GCC/Clang) reunidas en un sitio: hora local, apertura de
// ficheros por std::filesystem::path, entorno, comparaciones sin mayusculas y memoria del sistema.
// El resto del codigo no deberia necesitar #ifdef _WIN32 para estas cosas.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

namespace zw::plat {

// true si se pudo convertir. Thread-safe (localtime_s / localtime_r).
bool LocalTime(std::int64_t t, std::tm& out);
bool GmTime(std::int64_t t, std::tm& out);
// Inversa de GmTime (_mkgmtime64 / timegm).
std::int64_t TimeGm(std::tm& t);

// fopen con ruta Unicode (en Windows _wfopen). mode: "rb", "wb", "ab"...
std::FILE* OpenFile(const std::filesystem::path& p, const char* mode);
// fseek/ftell de 64 bits.
int Seek64(std::FILE* f, std::int64_t off, int whence);
std::int64_t Tell64(std::FILE* f);

void SetEnv(const char* name, const char* value);

int StrCaseCmp(const char* a, const char* b);
int StrNCaseCmp(const char* a, const char* b, std::size_t n);

// RAM fisica total / disponible en bytes (0 si no se sabe).
std::uint64_t TotalRamBytes();
std::uint64_t AvailRamBytes();

// Nombre corto de la plataforma para logs y "Acerca de": "Windows x64" / "Linux x86_64".
const char* Name();

}  // namespace zw::plat
