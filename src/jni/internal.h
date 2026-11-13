// Declaraciones internas de zw_jni (no usar fuera de src/jni).
#pragma once
#include <vector>

#include "jni/java.h"

namespace zw::jni {

enum class RefKind : std::uint32_t { Local = 1, Global = 2, Weak = 3 };

// jmethodID / jfieldID: valores opacos en la zona nula del guest (0x0A..., 0x0B...): si el guest
// los desreferencia, falla de forma visible.
constexpr std::uint32_t kMethodIdBase = 0x0A000000u;
constexpr std::uint32_t kFieldIdBase = 0x0B000000u;
constexpr int kJniSlots = 233;  // JNINativeInterface de JNI 1.6 (0..232, GetObjectRefType = 232)

struct ThreadState {
  gaddr env = 0;
  std::vector<std::vector<jref>> frames;  // [0] = marco base del hilo
};

// runtime.cpp
jref AddRef(const ObjPtr& o, RefKind kind);
bool RemoveRef(jref r);
std::size_t LiveRefs();
ThreadState& TS();
void DestroyThreadState(Cpu& cpu);
void DeleteLocal(jref r);
void PushFrame();
void PopFrame();
void ClearBaseFrame();
Class* FindLoadedClass(std::string_view name);
ObjPtr ClassObj(Class* c);
void InitCoreClasses();
Class* StringClass();
Method* LookupMethod(Class* c, std::string_view name, std::string_view sig);
Method* GetOrAddMethod(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered);
Method* MethodById(std::uint32_t id);
Field* GetOrAddField(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered);
Field* FieldById(std::uint32_t id);
ObjPtr AllocInstance(Class* c);
std::uint32_t ElemSize(char e);
void RegisterNativeMethod(Class* c, std::string_view name, std::string_view sig, gaddr fn);
void UnregisterNatives(Class* c);

// env.cpp
gaddr EnvTable();
gaddr BuildTables();
gaddr JavaVMAddr();

}  // namespace zw::jni
