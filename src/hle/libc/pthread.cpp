// pthread de bionic 4.x sobre zw::threads y primitivas host.
// Tipos guest: pthread_t = long (ThreadId), pthread_mutex_t = pthread_cond_t = { int value } (4 bytes),
// pthread_attr_t = 24 bytes, pthread_key_t = pthread_once_t = int, pthread_mutexattr_t = long.
// Los objetos host viven en tablas laterales; la palabra guest guarda una etiqueta 0xA<id de 28 bits>
// (los inicializadores estaticos 0 / 0x4000 / 0x8000 se convierten en la primera operacion).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "core/log.h"
#include "hle/libc/internal.h"

namespace zw::hle::libc {
namespace {

constexpr int kEBUSY = 16, kEDEADLK = 35, kEPERM = 1, kEINVAL = 22, kEAGAIN = 11, kESRCH = 3, kETIMEDOUT = 110;
constexpr std::uint32_t kTagMask = 0xF0000000u, kTag = 0xA0000000u, kIdMask = 0x0FFFFFFFu;

std::atomic_ref<std::uint32_t> Word(gaddr a) { return std::atomic_ref<std::uint32_t>(*g2h<std::uint32_t>(a)); }

// --- mutex ------------------------------------------------------------------------------------
struct GMutex {
  std::mutex m;
  std::condition_variable cv;
  std::uint32_t owner = 0, count = 0;
  int type = 0;  // 0 normal, 1 recursivo, 2 errorcheck

  int Lock(std::uint32_t self, bool try_only) {
    std::unique_lock lk(m);
    if (owner == self) {
      if (type == 1) {
        ++count;
        return 0;
      }
      if (type == 2) return kEDEADLK;
      if (try_only) return kEBUSY;
      // NORMAL: autobloqueo real en Linux. Se avisa y se espera (como el original).
      log::Warn("pthread: autobloqueo de mutex normal en el hilo {}", self);
    }
    if (owner != 0) {
      if (try_only) return kEBUSY;
      cv.wait(lk, [&] { return owner == 0; });
    }
    owner = self;
    count = 1;
    return 0;
  }
  int Unlock(std::uint32_t self) {
    std::unique_lock lk(m);
    if (owner != self) {
      if (type == 0 && owner != 0) {  // bionic no comprueba el dueno en mutex normales
        owner = 0;
        count = 0;
        lk.unlock();
        cv.notify_one();
        return 0;
      }
      return kEPERM;
    }
    if (--count == 0) {
      owner = 0;
      lk.unlock();
      cv.notify_one();
    }
    return 0;
  }
};

struct GCond {
  std::condition_variable_any cv;
};

template <class T>
struct Table {
  std::shared_mutex mu;
  std::unordered_map<std::uint32_t, std::shared_ptr<T>> map;
  std::uint32_t next = 1;
  std::shared_ptr<T> Get(std::uint32_t id) {
    std::shared_lock lk(mu);
    auto it = map.find(id);
    return it == map.end() ? nullptr : it->second;
  }
};
Table<GMutex>& Mutexes() {
  static auto* t = new Table<GMutex>();
  return *t;
}
Table<GCond>& Conds() {
  static auto* t = new Table<GCond>();
  return *t;
}

int TypeFromInit(std::uint32_t v) {
  if ((v & 0xC000) == 0x4000) return 1;
  if ((v & 0xC000) == 0x8000) return 2;
  return 0;
}

// Devuelve el objeto host de la palabra guest `a`, creandolo si la palabra no tiene etiqueta.
template <class T, class Init>
std::shared_ptr<T> Resolve(Table<T>& tab, gaddr a, Init&& init) {
  if (!a) return nullptr;
  auto w = Word(a);
  std::uint32_t v = w.load();
  if ((v & kTagMask) == kTag)
    if (auto p = tab.Get(v & kIdMask)) return p;
  std::unique_lock lk(tab.mu);
  v = w.load();
  if ((v & kTagMask) == kTag) {
    auto it = tab.map.find(v & kIdMask);
    if (it != tab.map.end()) return it->second;
  }
  auto obj = std::make_shared<T>();
  init(*obj, v);
  const std::uint32_t id = tab.next++ & kIdMask;
  tab.map[id] = obj;
  w.store(kTag | id);
  return obj;
}
std::shared_ptr<GMutex> M(gaddr a) {
  return Resolve(Mutexes(), a, [](GMutex& m, std::uint32_t v) { m.type = TypeFromInit(v); });
}
std::shared_ptr<GCond> C(gaddr a) {
  return Resolve(Conds(), a, [](GCond&, std::uint32_t) {});
}
template <class T>
void Erase(Table<T>& tab, gaddr a) {
  const std::uint32_t v = mem::R32(a);
  if ((v & kTagMask) == kTag) {
    std::unique_lock lk(tab.mu);
    tab.map.erase(v & kIdMask);
  }
}

int hle_mutexattr_init(GPtr<std::uint32_t> a) {
  *a = 0;
  return 0;
}
int hle_mutexattr_destroy(GPtr<std::uint32_t> a) {
  *a = 0;
  return 0;
}
int hle_mutexattr_settype(GPtr<std::uint32_t> a, int type) {
  if (type < 0 || type > 2) return kEINVAL;
  *a = static_cast<std::uint32_t>(type);
  return 0;
}
int hle_mutexattr_gettype(GPtr<std::uint32_t> a, GPtr<std::int32_t> type) {
  *type = static_cast<std::int32_t>(*a & 3);
  return 0;
}
int hle_mutexattr_setpshared(GPtr<std::uint32_t>, int) { return 0; }

int hle_mutex_init(gaddr m, GPtr<std::uint32_t> attr) {
  if (!m) return kEINVAL;
  Erase(Mutexes(), m);
  const int type = attr ? static_cast<int>(*attr & 3) : 0;
  mem::W32(m, type == 1 ? 0x4000 : type == 2 ? 0x8000 : 0);
  M(m);
  return 0;
}
int hle_mutex_destroy(gaddr m) {
  if (!m) return kEINVAL;
  Erase(Mutexes(), m);
  mem::W32(m, 0xDEAD0000u);  // bionic marca el mutex destruido
  return 0;
}
int hle_mutex_lock(Cpu& c, gaddr m) {
  auto mx = M(m);
  return mx ? mx->Lock(c.thread_id(), false) : kEINVAL;
}
int hle_mutex_trylock(Cpu& c, gaddr m) {
  auto mx = M(m);
  return mx ? mx->Lock(c.thread_id(), true) : kEINVAL;
}
int hle_mutex_unlock(Cpu& c, gaddr m) {
  auto mx = M(m);
  return mx ? mx->Unlock(c.thread_id()) : kEINVAL;
}
int hle_mutex_lock_timeout_np(Cpu& c, gaddr m, std::uint32_t ms) {
  auto mx = M(m);
  if (!mx) return kEINVAL;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    const int r = mx->Lock(c.thread_id(), true);
    if (r != kEBUSY) return r;
    if (std::chrono::steady_clock::now() >= deadline) return kEBUSY;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// --- cond ---------------------------------------------------------------------------------------
int hle_cond_init(gaddr c, gaddr) {
  if (!c) return kEINVAL;
  Erase(Conds(), c);
  mem::W32(c, 0);
  C(c);
  return 0;
}
int hle_cond_destroy(gaddr c) {
  if (!c) return kEINVAL;
  Erase(Conds(), c);
  mem::W32(c, 0xDEAD0000u);
  return 0;
}
int hle_cond_signal(gaddr c) {
  auto cv = C(c);
  if (!cv) return kEINVAL;
  cv->cv.notify_one();
  return 0;
}
int hle_cond_broadcast(gaddr c) {
  auto cv = C(c);
  if (!cv) return kEINVAL;
  cv->cv.notify_all();
  return 0;
}
// Espera; `deadline` vacio = sin limite. Devuelve 0 o ETIMEDOUT.
template <class Clock>
int CondWait(Cpu& cpu, gaddr c, gaddr m, const typename Clock::time_point* deadline) {
  auto cv = C(c);
  auto mx = M(m);
  if (!cv || !mx) return kEINVAL;
  const std::uint32_t self = cpu.thread_id();
  std::unique_lock lk(mx->m);
  if (mx->owner != self) return kEPERM;
  const std::uint32_t saved = mx->count;
  mx->owner = 0;
  mx->count = 0;
  mx->cv.notify_one();
  int rc = 0;
  if (deadline) {
    if (cv->cv.wait_until(lk, *deadline) == std::cv_status::timeout) rc = kETIMEDOUT;
  } else {
    cv->cv.wait(lk);
  }
  mx->cv.wait(lk, [&] { return mx->owner == 0; });
  mx->owner = self;
  mx->count = saved;
  return rc;
}
int hle_cond_wait(Cpu& cpu, gaddr c, gaddr m) { return CondWait<std::chrono::steady_clock>(cpu, c, m, nullptr); }
int hle_cond_timedwait(Cpu& cpu, gaddr c, gaddr m, GPtr<std::int32_t> abstime) {
  if (!abstime) return hle_cond_wait(cpu, c, m);
  if (abstime[1] < 0 || abstime[1] >= 1000000000) return kEINVAL;
  // CLOCK_REALTIME absoluto.
  const auto tp = std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(abstime[0]) +
                                                                      std::chrono::nanoseconds(abstime[1])));
  return CondWait<std::chrono::system_clock>(cpu, c, m, &tp);
}
int hle_cond_timedwait_monotonic(Cpu& cpu, gaddr c, gaddr m, GPtr<std::int32_t> abstime) {
  if (!abstime) return hle_cond_wait(cpu, c, m);
  const auto tp = std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::seconds(abstime[0]) + std::chrono::nanoseconds(abstime[1])));
  return CondWait<std::chrono::steady_clock>(cpu, c, m, &tp);
}
int hle_cond_timeout_np(Cpu& cpu, gaddr c, gaddr m, std::uint32_t ms) {
  const auto tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  return CondWait<std::chrono::steady_clock>(cpu, c, m, &tp);
}
int hle_condattr_noop(gaddr) { return 0; }
int hle_condattr_set(gaddr, int) { return 0; }

// --- attr ---------------------------------------------------------------------------------------
// {u32 flags; void* stack_base; size_t stack_size; size_t guard_size; int sched_policy; int sched_priority}
int hle_attr_init(gaddr a) {
  const std::uint32_t init[6] = {0, 0, threads::kDefaultStackSize, 4096, 0, 0};
  std::memcpy(g2h(a), init, sizeof init);
  return 0;
}
int hle_attr_destroy(gaddr a) {
  std::memset(g2h(a), 0x42, 24);
  return 0;
}
int hle_attr_setdetachstate(gaddr a, int s) {
  if (s != 0 && s != 1) return kEINVAL;
  mem::W32(a, (mem::R32(a) & ~1u) | static_cast<std::uint32_t>(s));
  return 0;
}
int hle_attr_getdetachstate(gaddr a, GPtr<std::int32_t> s) {
  *s = static_cast<std::int32_t>(mem::R32(a) & 1);
  return 0;
}
int hle_attr_setstacksize(gaddr a, gsize sz) {
  if (sz < 16384) return kEINVAL;
  mem::W32(a + 8, sz);
  return 0;
}
int hle_attr_getstacksize(gaddr a, GPtr<std::uint32_t> sz) {
  *sz = mem::R32(a + 8);
  return 0;
}
int hle_attr_setstack(gaddr a, gaddr base, gsize sz) {
  mem::W32(a + 4, base);
  mem::W32(a + 8, sz);
  return 0;
}
int hle_attr_getstack(gaddr a, GPtr<std::uint32_t> base, GPtr<std::uint32_t> sz) {
  *base = mem::R32(a + 4);
  *sz = mem::R32(a + 8);
  return 0;
}
int hle_attr_setguardsize(gaddr a, gsize sz) {
  mem::W32(a + 12, sz);
  return 0;
}
int hle_attr_getguardsize(gaddr a, GPtr<std::uint32_t> sz) {
  *sz = mem::R32(a + 12);
  return 0;
}
int hle_attr_setschedpolicy(gaddr a, int p) {
  mem::W32(a + 16, static_cast<std::uint32_t>(p));
  return 0;
}
int hle_attr_getschedpolicy(gaddr a, GPtr<std::int32_t> p) {
  *p = static_cast<std::int32_t>(mem::R32(a + 16));
  return 0;
}
int hle_attr_setschedparam(gaddr a, GPtr<std::int32_t> prm) {
  mem::W32(a + 20, static_cast<std::uint32_t>(prm ? *prm : 0));
  return 0;
}
int hle_attr_getschedparam(gaddr a, GPtr<std::int32_t> prm) {
  *prm = static_cast<std::int32_t>(mem::R32(a + 20));
  return 0;
}
int hle_attr_setscope(gaddr, int s) { return s == 0 ? 0 : 95 /*ENOTSUP*/; }

// --- hilos -------------------------------------------------------------------------------------
int hle_create(GPtr<std::uint32_t> out, gaddr attr, gaddr start, gaddr arg) {
  threads::CreateParams p;
  p.entry = start;
  p.arg = arg;
  p.stack_size = attr ? mem::R32(attr + 8) : 0;
  p.detached = attr && (mem::R32(attr) & 1);
  p.name = "pthread";
  const threads::ThreadId id = threads::Create(p);
  if (!id) return kEAGAIN;
  if (out) *out = id;
  return 0;
}
int hle_join(std::uint32_t t, GPtr<std::uint32_t> ret) {
  if (t == threads::CurrentId()) return kEDEADLK;
  std::uint32_t v = 0;
  if (!threads::Join(t, &v)) return kESRCH;
  if (ret) *ret = v;
  return 0;
}
int hle_detach(std::uint32_t t) { return threads::Detach(t) ? 0 : kESRCH; }
std::uint32_t hle_self(Cpu& c) { return c.thread_id(); }
int hle_equal(std::uint32_t a, std::uint32_t b) { return a == b; }
int hle_kill(std::uint32_t t, int sig) {
  if (sig == 0) return threads::IsAlive(t) ? 0 : kESRCH;
  log::Warn("pthread_kill({}, {}) ignorado", t, sig);
  return 0;
}
int hle_getschedparam(std::uint32_t, GPtr<std::int32_t> pol, GPtr<std::int32_t> prm) {
  if (pol) *pol = 0;
  if (prm) *prm = 0;
  return 0;
}
int hle_setschedparam(std::uint32_t, int, gaddr) { return 0; }
int hle_setname_np(std::uint32_t, const char*) { return 0; }
int hle_sigmask(int, gaddr, GPtr<std::uint32_t> old) {
  if (old) *old = 0;
  return 0;
}
int hle_getattr_np(std::uint32_t, gaddr attr) {
  Cpu& c = Cpu::Current();
  hle_attr_init(attr);
  mem::W32(attr + 4, c.stack_base());
  mem::W32(attr + 8, c.stack_size());
  return 0;
}

// --- once ---------------------------------------------------------------------------------------
std::mutex g_once_mu;
std::condition_variable g_once_cv;
int hle_once(gaddr once, gaddr fn) {
  auto w = Word(once);
  // 0 = sin ejecutar, 1 = en curso, 2 = hecho
  if (w.load() == 2) return 0;
  std::uint32_t expected = 0;
  if (w.compare_exchange_strong(expected, 1)) {
    try {
      call_guest<void>(fn);
    } catch (...) {
      w.store(0);
      g_once_cv.notify_all();
      throw;
    }
    {
      std::lock_guard lk(g_once_mu);
      w.store(2);
    }
    g_once_cv.notify_all();
    return 0;
  }
  std::unique_lock lk(g_once_mu);
  g_once_cv.wait(lk, [&] { return w.load() != 1; });
  if (w.load() == 0) {  // el inicializador fallo: reintentar
    lk.unlock();
    return hle_once(once, fn);
  }
  return 0;
}

// --- claves (TLS) --------------------------------------------------------------------------------
constexpr int kMaxKeys = 128;
std::mutex g_keys_mu;
struct KeySlot {
  bool used = false;
  gaddr dtor = 0;
};
KeySlot g_keys[kMaxKeys];

int hle_key_create(GPtr<std::uint32_t> key, gaddr dtor) {
  std::lock_guard lk(g_keys_mu);
  for (int i = 0; i < kMaxKeys; ++i)
    if (!g_keys[i].used) {
      g_keys[i] = KeySlot{true, dtor};
      *key = static_cast<std::uint32_t>(i);
      return 0;
    }
  return kEAGAIN;
}
int hle_key_delete(std::uint32_t key) {
  std::lock_guard lk(g_keys_mu);
  if (key >= kMaxKeys || !g_keys[key].used) return kEINVAL;
  g_keys[key] = KeySlot{};
  return 0;
}
// FAST (79 sitios): solo toca el estado del hilo actual.
gaddr hle_getspecific(Cpu& c, std::uint32_t key) { return key < kMaxKeys ? TS(c).pthread_keys[key] : 0; }
int hle_setspecific(Cpu& c, std::uint32_t key, gaddr v) {
  if (key >= kMaxKeys) return kEINVAL;
  TS(c).pthread_keys[key] = v;
  return 0;
}

}  // namespace

// Exit hook (register.cpp): destructores de pthread_key (hasta 4 rondas, como POSIX).
void RunKeyDestructors(Cpu& c) {
  auto* ts = static_cast<ThreadState*>(c.user(UserSlot::Libc));
  if (!ts) return;
  for (int round = 0; round < 4; ++round) {
    bool any = false;
    for (int k = 0; k < kMaxKeys; ++k) {
      const gaddr v = ts->pthread_keys[k];
      if (!v) continue;
      gaddr dtor;
      {
        std::lock_guard lk(g_keys_mu);
        dtor = g_keys[k].used ? g_keys[k].dtor : 0;
      }
      ts->pthread_keys[k] = 0;
      if (dtor) {
        any = true;
        c.Call<void>(dtor, v);
      }
    }
    if (!any) break;
  }
}

void RegisterPthread() {
  ZW_HLE_FAST("pthread_mutexattr_init", hle_mutexattr_init);
  ZW_HLE_FAST("pthread_mutexattr_destroy", hle_mutexattr_destroy);
  ZW_HLE_FAST("pthread_mutexattr_settype", hle_mutexattr_settype);
  ZW_HLE_FAST("pthread_mutexattr_gettype", hle_mutexattr_gettype);
  ZW_HLE_FAST("pthread_mutexattr_setpshared", hle_mutexattr_setpshared);
  ZW_HLE("pthread_mutex_init", hle_mutex_init);
  ZW_HLE("pthread_mutex_destroy", hle_mutex_destroy);
  ZW_HLE("pthread_mutex_lock", hle_mutex_lock);
  ZW_HLE("pthread_mutex_trylock", hle_mutex_trylock);
  ZW_HLE("pthread_mutex_unlock", hle_mutex_unlock);
  ZW_HLE("pthread_mutex_lock_timeout_np", hle_mutex_lock_timeout_np);

  ZW_HLE("pthread_cond_init", hle_cond_init);
  ZW_HLE("pthread_cond_destroy", hle_cond_destroy);
  ZW_HLE("pthread_cond_signal", hle_cond_signal);
  ZW_HLE("pthread_cond_broadcast", hle_cond_broadcast);
  ZW_HLE("pthread_cond_wait", hle_cond_wait);
  ZW_HLE("pthread_cond_timedwait", hle_cond_timedwait);
  ZW_HLE("pthread_cond_timedwait_monotonic", hle_cond_timedwait_monotonic);
  ZW_HLE("pthread_cond_timedwait_monotonic_np", hle_cond_timedwait_monotonic);
  ZW_HLE("pthread_cond_timeout_np", hle_cond_timeout_np);
  ZW_HLE_FAST("pthread_condattr_init", hle_condattr_noop);
  ZW_HLE_FAST("pthread_condattr_destroy", hle_condattr_noop);
  ZW_HLE_FAST("pthread_condattr_setpshared", hle_condattr_set);
  ZW_HLE_FAST("pthread_condattr_setclock", hle_condattr_set);

  ZW_HLE_FAST("pthread_attr_init", hle_attr_init);
  ZW_HLE_FAST("pthread_attr_destroy", hle_attr_destroy);
  ZW_HLE_FAST("pthread_attr_setdetachstate", hle_attr_setdetachstate);
  ZW_HLE_FAST("pthread_attr_getdetachstate", hle_attr_getdetachstate);
  ZW_HLE_FAST("pthread_attr_setstacksize", hle_attr_setstacksize);
  ZW_HLE_FAST("pthread_attr_getstacksize", hle_attr_getstacksize);
  ZW_HLE_FAST("pthread_attr_setstack", hle_attr_setstack);
  ZW_HLE_FAST("pthread_attr_getstack", hle_attr_getstack);
  ZW_HLE_FAST("pthread_attr_setguardsize", hle_attr_setguardsize);
  ZW_HLE_FAST("pthread_attr_getguardsize", hle_attr_getguardsize);
  ZW_HLE_FAST("pthread_attr_setschedpolicy", hle_attr_setschedpolicy);
  ZW_HLE_FAST("pthread_attr_getschedpolicy", hle_attr_getschedpolicy);
  ZW_HLE_FAST("pthread_attr_setschedparam", hle_attr_setschedparam);
  ZW_HLE_FAST("pthread_attr_getschedparam", hle_attr_getschedparam);
  ZW_HLE_FAST("pthread_attr_setscope", hle_attr_setscope);

  ZW_HLE("pthread_create", hle_create);
  ZW_HLE("pthread_join", hle_join);
  ZW_HLE("pthread_detach", hle_detach);
  ZW_HLE_FAST("pthread_self", hle_self);
  ZW_HLE_FAST("pthread_equal", hle_equal);
  ZW_HLE("pthread_kill", hle_kill);
  ZW_HLE_FAST("pthread_getschedparam", hle_getschedparam);
  ZW_HLE_FAST("pthread_setschedparam", hle_setschedparam);
  ZW_HLE_FAST("pthread_setname_np", hle_setname_np);
  ZW_HLE_FAST("pthread_sigmask", hle_sigmask);
  ZW_HLE_FAST("sigprocmask", hle_sigmask);
  ZW_HLE("pthread_getattr_np", hle_getattr_np);
  ZW_HLE("pthread_once", hle_once);
  ZW_HLE("pthread_key_create", hle_key_create);
  ZW_HLE("pthread_key_delete", hle_key_delete);
  ZW_HLE_FAST("pthread_getspecific", hle_getspecific);
  ZW_HLE_FAST("pthread_setspecific", hle_setspecific);
}

}  // namespace zw::hle::libc
