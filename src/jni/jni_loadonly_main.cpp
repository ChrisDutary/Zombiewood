// zw_jni_loadonly <ruta libZombieWood.so> [--no-extra] [--trace-calls]
//
// Diagnostico de T7: como `zombiewood --load-only`, pero con el JNI real de zw_jni en lugar del
// JavaVM falso de T3. Ejecuta .init_array, JNI_OnLoad y la parte de la secuencia de arranque Java que no
// necesita GL (ANALYSIS §4: pasos 2, 4/4a, 5 y los nativeInit opcionales de 7), y al final imprime
// cada FindClass/Get*ID que hizo el juego y si quedo cubierto por la emulacion.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "jni/jni.h"
#include "loader/load_only.h"
#include "loader/loader.h"

using namespace zw;

namespace {
template <class... A>
void Out(std::format_string<A...> f, A&&... a) {
  const std::string s = std::format(f, std::forward<A>(a)...);
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}

int g_rc = 0;

template <class Fn>
bool Step(const char* what, Fn&& fn, bool required) {
  const auto t0 = std::chrono::steady_clock::now();
  try {
    fn();
    Out("  [ok] {} ({} ms)", what,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return true;
  } catch (const GuestFault& f) {
    Out("  [FALLO] {}: fallo guest:\n{}", what, f.what());
  } catch (const GuestExit& e) {
    Out("  [FALLO] {}: el guest llamo a exit/abort (codigo {})", what, e.code);
  } catch (const std::exception& e) {
    Out("  [FALLO] {}: {}", what, e.what());
  }
  if (required) g_rc = 1;
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::puts("uso: zw_jni_loadonly <ruta libZombieWood.so> [--no-extra] [--trace-calls]");
    return 2;
  }
  bool extra = true;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--no-extra")) extra = false;
    if (!std::strcmp(argv[i], "--trace-calls")) jni::config().verbose_calls = true;
  }
  log::Init();
  if (!GuestMemory::Init()) {
    Out("no se pudo reservar la memoria guest");
    return 2;
  }
  // Orden de CONTRACTS §9: modulos reales primero; la HLE minima de T3 solo rellena huecos de libc.
  jni::Register();
  loader::bootstrap::Register();

  Out("== carga ==");
  loader::Module* m = loader::Load(argv[1]);
  if (!m) {
    Out("FALLO al cargar {}", argv[1]);
    return 2;
  }
  Out("  {} en 0x{:08x}, {} imports", m->name(), m->base(), m->imports().size());

  Out("== arranque (ANALYSIS §4) ==");
  if (!Step("paso 0: .init_array", [&] { loader::RunInit(*m); }, true)) return 1;
  Step("paso 0: JNI_OnLoad(vm, NULL)",
       [&] {
         auto v = loader::CallJniOnLoad(*m, jni::JavaVM());
         if (!v) throw std::runtime_error("JNI_OnLoad no exportado");
         Out("       -> 0x{:x}", static_cast<std::uint32_t>(*v));
         if (*v != 0x10004) throw std::runtime_error("version JNI inesperada");
       },
       true);
  Step("paso 2: Game.nativeSetGPUInfo", [] { jni::call_native("Game_nativeSetGPUInfo", jni::config().gpu_name); }, true);
  Step("paso 4: GL2JNILib.init (-> createView -> GameRenderer.nativeInit)", [] { jni::call_native("GL2JNILib_init"); }, true);
  Step("paso 5: Game.nativeInit", [] { jni::call_native("Game_nativeInit"); }, true);
  if (extra) {
    const char* opt[] = {"GLUtils_SUtils_nativeInit",
                         "GLUtils_Device_nativeInit",
                         "DataSharing_nativeInit",
                         "PushNotification_C2DMAndroidUtils_nativeInit",
                         "SendInfo_initMethods",
                         "Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit",
                         "Java_com_gameloft_GLSocialLib_facebook_FacebookAndroidGLSocialLib_nativeInit",
                         "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeInit"};
    for (const char* n : opt) {
      const std::string what = std::string("opcional: ") + n;
      if (!jni::FindNative(n)) {
        Out("  [--] {} (no existe)", what);
        continue;
      }
      Step(what.c_str(), [n] { jni::call_native(n); }, false);
    }
  }

  Out("== cobertura JNI (cada FindClass/Get*ID pedido por el juego) ==");
  std::printf("%s", jni::CoverageReport().c_str());

  Out("== resumen ==");
  std::string called_missing;
  std::uint64_t jni_calls = 0;
  for (const auto& [name, n] : hle::CallCounts()) {
    if (name.starts_with("JNIEnv::") || name.starts_with("JavaVM::")) jni_calls += n;
    if (n && !hle::IsRegistered(name)) called_missing += std::format("{}x{} ", name, n);
  }
  Out("  llamadas JNIEnv/JavaVM: {}", jni_calls);
  Out("  imports sin handler invocados: {}", called_missing.empty() ? "(ninguno)" : called_missing);
  Out("  resultado: {}", g_rc == 0 ? "OK" : "DETENIDO (ver arriba)");
  return g_rc;
}
