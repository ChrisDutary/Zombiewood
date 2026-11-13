// Punto de entrada del modulo: zw::hle::libc::Register() + estado por hilo comun.
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "hle/libc/internal.h"
#include "hle/libc/libc.h"

namespace zw::hle::libc {

namespace {
std::mutex g_intern_mu;
std::unordered_map<std::string, gaddr>* g_intern = nullptr;
std::once_flag g_hooks_once;
}  // namespace

gaddr InternString(std::string_view s) {
  std::lock_guard lk(g_intern_mu);
  if (!g_intern) g_intern = new std::unordered_map<std::string, gaddr>();
  auto it = g_intern->find(std::string(s));
  if (it != g_intern->end()) return it->second;
  const gaddr a = mem::StrDup(s);
  g_intern->emplace(std::string(s), a);
  return a;
}

ThreadState& TS(Cpu& c) {
  void*& p = c.user(UserSlot::Libc);
  if (!p) p = new ThreadState();
  return *static_cast<ThreadState*>(p);
}
ThreadState& TS() { return TS(Cpu::Current()); }

gaddr ThreadBuffer(gaddr ThreadState::*field, gsize size) {
  ThreadState& t = TS();
  if (!(t.*field)) t.*field = GuestMemory::Calloc(1, size);
  return t.*field;
}

// pthread.cpp: destructores de claves al terminar un hilo.
void RunKeyDestructors(Cpu& c);

void Register() {
  GuestMemory::Init();
  std::call_once(g_hooks_once, [] {
    threads::AddExitHook([](Cpu& c) {
      try {
        RunKeyDestructors(c);
      } catch (const GuestFault& f) {
        log::Error("libc: fallo en un destructor de pthread_key: {}", f.what());
      } catch (const GuestExit&) {
      }
      if (auto* t = static_cast<ThreadState*>(c.user(UserSlot::Libc))) {
        GuestMemory::Free(t->tm_buf);
        GuestMemory::Free(t->text_buf);
        GuestMemory::Free(t->dlerror_buf);
        delete t;
        c.user(UserSlot::Libc) = nullptr;
      }
    });
  });
  RegisterMemory();
  RegisterString();
  RegisterCtype();
  RegisterWchar();
  RegisterStdio();
  RegisterPosix();
  RegisterTime();
  RegisterPthread();
  RegisterRuntime();
  RegisterMath();
  RegisterDl();
  RegisterLog();
  RegisterNet();
}

}  // namespace zw::hle::libc
