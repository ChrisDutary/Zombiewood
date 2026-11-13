// Registro del modulo JNI (T7).
#include <atomic>
#include <mutex>

#include "core/log.h"
#include "cpu/cpu.h"
#include "jni/internal.h"

namespace zw::jni {

void Register() {
  static std::once_flag once;
  std::call_once(once, [] {
    InitCoreClasses();
    BuildTables();
    RegisterAndroidClasses();
    RegisterAudioTrack();
    RegisterGameClasses();
    RegisterSocialAndMisc();
    threads::AddExitHook([](Cpu& cpu) { DestroyThreadState(cpu); });
    log::Info("[jni] JNIEnv/JavaVM falsos listos (JavaVM = 0x{:08x})", JavaVMAddr());
  });
}

gaddr JavaVM() {
  Register();
  return JavaVMAddr();
}

gaddr Env() {
  Register();
  return TS().env;
}

}  // namespace zw::jni
