// Modo de diagnostico `zombiewood --load-only <ruta .so> [mas .so...]` (T3).
#pragma once
#include <filesystem>
#include <vector>

#include "cpu/guest_memory.h"

namespace zw::loader {

namespace bootstrap {
// HLE minima de arranque (malloc, mem*/str*, printf, pthread_key, __cxa_*, datos bionic...).
// Solo registra los nombres que aun no tengan handler: llamala DESPUES de los modulos reales.
void Register();
// JavaVM* falso (guest) cuyo GetEnv/AttachCurrentThread devuelven un JNIEnv con 232 slots
// genericos: registran cada llamada y devuelven handles ficticios no nulos para
// FindClass/Get*ID/NewGlobalRef/NewStringUTF y 0 para el resto.
gaddr FakeJavaVM();
}  // namespace bootstrap

// Carga cada .so (en orden), lista imports, ejecuta constructores y JNI_OnLoad con el JavaVM falso,
// e imprime un informe por stdout/log. Devuelve 0 si todo llego al final, != 0 si algo se detuvo.
// `java_vm` != 0 (p. ej. zw::jni::JavaVM()) sustituye al JavaVM falso.
int RunLoadOnly(const std::vector<std::filesystem::path>& libs, gaddr java_vm = 0);

}  // namespace zw::loader
