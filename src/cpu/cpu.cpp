#include "cpu/cpu.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "core/log.h"
#include "cpu/hle.h"
#include "cpu/internal.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace zw {
using internal::HleEntry;
using internal::kReturnStub;
using internal::RangeHas;

namespace {
constexpr std::uint32_t kNoSvc = 0xFFFFFFFFu;

Dynarmic::ExclusiveMonitor& Monitor() {
  static Dynarmic::ExclusiveMonitor m(threads::kMaxThreads);
  return m;
}

std::mutex g_cpus_mu;
detail::CpuImpl* g_slots[threads::kMaxThreads] = {};
std::atomic<std::uint32_t> g_next_tid{1};
std::atomic<std::size_t> g_live{0};

std::mutex g_hooks_mu;
std::vector<std::function<void(Cpu&)>> g_start_hooks, g_exit_hooks;
std::function<void(const std::string&)> g_fatal;
std::function<void(int)> g_exit;

std::mutex g_sym_mu;
std::vector<debug::Symbolizer> g_symbolizers;
std::atomic<int> g_trace{-1};
std::string g_trace_filter;

bool EnvFlag(const char* name, bool def) {
  const char* e = std::getenv(name);
  if (!e || !*e) return def;
  return *e != '0';
}

// longjmp que cruza fronteras de call_guest: se desenrolla la pila host con una excepcion.
struct LongjmpUnwind {
  int depth;
};

// CP15: solo los registros de TLS de usuario; el resto lee 0 / ignora escrituras.
class Cp15 final : public Dynarmic::A32::Coprocessor {
 public:
  using CoprocReg = Dynarmic::A32::CoprocReg;
  std::uint32_t tpidruro = 0, tpidrurw = 0;

  std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
    return std::nullopt;
  }
  CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
    if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 2) return &tpidrurw;
    return Callback{[](void*, std::uint32_t, std::uint32_t) -> std::uint64_t { return 0; }, std::nullopt};
  }
  CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
  CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
    if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0) {
      if (opc2 == 3) return &tpidruro;
      if (opc2 == 2) return &tpidrurw;
    }
    return Callback{[](void*, std::uint32_t, std::uint32_t) -> std::uint64_t { return 0; }, std::nullopt};
  }
  CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
  std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
  std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
};

bool TraceMatches(const std::string& name) {
  return g_trace_filter.empty() || name.find(g_trace_filter) != std::string::npos;
}
}  // namespace

// ---------------------------------------------------------------------------
struct detail::CpuImpl final : Dynarmic::A32::UserCallbacks {
  Cpu* cpu = nullptr;
  std::unique_ptr<Dynarmic::A32::Jit> jit;
  std::shared_ptr<Cp15> cp15;
  std::size_t slot = 0;
  std::uint32_t pending_svc = kNoSvc;
  bool fault_pending = false;
  bool in_fast = false;
  std::string fault_msg;
  gaddr stack_map = 0, tls_map = 0;
  gsize stack_map_size = 0;

  static Cpu* Create(gsize stack_size, std::string name);
  static void Destroy(Cpu* c);

  void Fault(std::string msg) {
    if (!fault_pending) {
      fault_pending = true;
      fault_msg = std::move(msg) + "\n" + debug::DumpState(*cpu);
    }
    jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
  }

  // --- memoria (ruta lenta: paginas no mapeadas, cruces de pagina, codigo y exclusivos) ---
  template <class T>
  T Rd(std::uint32_t a) {
    if (!RangeHas(a, sizeof(T), kProtRead)) {
      Fault(std::format("lectura de {} bytes en direccion guest no mapeada 0x{:08x}", sizeof(T), a));
      return T{};
    }
    T v;
    std::memcpy(&v, g_membase + a, sizeof(T));
    return v;
  }
  template <class T>
  void Wr(std::uint32_t a, T v) {
    if (!RangeHas(a, sizeof(T), kProtWrite)) {
      Fault(std::format("escritura de {} bytes en direccion guest {} 0x{:08x}", sizeof(T),
                        internal::PageProt(a) ? "de solo lectura" : "no mapeada", a));
      return;
    }
    std::memcpy(g_membase + a, &v, sizeof(T));
  }
  std::optional<std::uint32_t> MemoryReadCode(std::uint32_t a) override {
    if (!(internal::PageProt(a) & kProtExec)) return std::nullopt;
    std::uint32_t v;
    std::memcpy(&v, g_membase + a, 4);
    return v;
  }
  std::uint8_t MemoryRead8(std::uint32_t a) override { return Rd<std::uint8_t>(a); }
  std::uint16_t MemoryRead16(std::uint32_t a) override { return Rd<std::uint16_t>(a); }
  std::uint32_t MemoryRead32(std::uint32_t a) override { return Rd<std::uint32_t>(a); }
  std::uint64_t MemoryRead64(std::uint32_t a) override { return Rd<std::uint64_t>(a); }
  void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { Wr(a, v); }
  void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { Wr(a, v); }
  void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { Wr(a, v); }
  void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { Wr(a, v); }

  bool ExclOk(std::uint32_t a, std::uint32_t n) {
    if (RangeHas(a, n, kProtWrite)) return true;
    Fault(std::format("escritura exclusiva en direccion guest no escribible 0x{:08x}", a));
    return false;
  }
  bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override {
    return ExclOk(a, 1) && _InterlockedCompareExchange8(reinterpret_cast<char*>(g_membase + a), char(v), char(e)) == char(e);
  }
  bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override {
    return ExclOk(a, 2) && _InterlockedCompareExchange16(reinterpret_cast<short*>(g_membase + a), short(v), short(e)) == short(e);
  }
  bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override {
    return ExclOk(a, 4) && _InterlockedCompareExchange(reinterpret_cast<long*>(g_membase + a), long(v), long(e)) == long(e);
  }
  bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override {
    return ExclOk(a, 8) &&
           _InterlockedCompareExchange64(reinterpret_cast<long long*>(g_membase + a), (long long)v, (long long)e) == (long long)e;
  }
  bool IsReadOnlyMemory(std::uint32_t a) override {
    const std::uint8_t p = internal::PageProt(a);
    return p != 0 && !(p & kProtWrite);
  }

  void InterpreterFallback(std::uint32_t pc, std::size_t n) override {
    Fault(std::format("instruccion no soportada por el JIT en 0x{:08x} ({} instr.)", pc, n));
  }
  void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception ex) override {
    using E = Dynarmic::A32::Exception;
    const char* what = "excepcion";
    switch (ex) {
      case E::UndefinedInstruction: what = "instruccion indefinida"; break;
      case E::UnpredictableInstruction: what = "instruccion impredecible"; break;
      case E::DecodeError: what = "error de decodificacion"; break;
      case E::Breakpoint: what = "bkpt"; break;
      case E::NoExecuteFault: what = "ejecucion en direccion no mapeada/no ejecutable"; break;
      default: return;  // hints (yield, wfi, pld...): ignorar
    }
    cpu->regs_[15] = pc;
    Fault(std::format("{} en pc = 0x{:08x} ({})", what, pc, debug::addr2sym(pc)));
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1u << 30; }

  void CallSVC(std::uint32_t swi) override {
    if (swi != 0) {
      HleEntry* e = internal::HleEntryByIndex(swi);
      if (e && (e->flags.load(std::memory_order_relaxed) & hle::kFast) && g_trace.load(std::memory_order_relaxed) <= 0) {
        if (hle::RawFn fn = e->fn.load(std::memory_order_relaxed)) {
          e->calls.fetch_add(1, std::memory_order_relaxed);
          const char* prev = cpu->cur_import_;
          cpu->cur_import_ = e->name.c_str();
          in_fast = true;
          fn(*cpu);
          in_fast = false;
          cpu->cur_import_ = prev;
          return;
        }
      }
    }
    pending_svc = swi;
    jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }

  void Dispatch(std::uint32_t swi) {
    HleEntry* e = internal::HleEntryByIndex(swi);
    if (!e) throw GuestFault(std::format("svc #{} desconocido\n{}", swi, debug::DumpState(*cpu)));
    e->calls.fetch_add(1, std::memory_order_relaxed);
    hle::RawFn fn = e->fn.load();
    if (!fn) {
      if (internal::UnimplementedAborts())
        throw GuestFault(std::format("import sin handler: {} (lr = {})\n{}", e->name, debug::addr2sym(cpu->lr()),
                                     debug::DumpState(*cpu)));
      if (!e->warned.exchange(true))
        log::Warn("import sin handler: {} (lr = {}) -> devuelve 0", e->name, debug::addr2sym(cpu->lr()));
      cpu->regs_[0] = 0;
      return;
    }
    const bool trace = g_trace.load(std::memory_order_relaxed) > 0 && TraceMatches(e->name);
    if (trace)
      log::Info("[t{}] {}(0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}) lr={}", cpu->thread_id_, e->name, cpu->regs_[0], cpu->regs_[1],
                cpu->regs_[2], cpu->regs_[3], debug::addr2sym(cpu->lr()));
    const char* prev = cpu->cur_import_;
    cpu->cur_import_ = e->name.c_str();
    struct Restore {
      Cpu* c;
      const char* p;
      ~Restore() { c->cur_import_ = p; }
    } restore{cpu, prev};
    fn(*cpu);
    if (trace) log::Info("[t{}] {} -> 0x{:x}", cpu->thread_id_, e->name, cpu->regs_[0]);
  }

  // Ejecuta hasta que el guest vuelve al stub de retorno del nivel actual.
  void RunUntilReturn() {
    for (;;) {
      pending_svc = kNoSvc;
      jit->Run();
      if (fault_pending) {
        fault_pending = false;
        log::Error("fallo guest: {}", fault_msg);
        throw GuestFault(fault_msg);
      }
      const std::uint32_t svc = pending_svc;
      if (svc == kNoSvc) continue;  // invalidacion de cache u otra parada espuria
      if (svc == 0) return;
      try {
        Dispatch(svc);
      } catch (const LongjmpUnwind& u) {
        if (u.depth != cpu->depth_) throw;
        // El destino del longjmp esta en este nivel: los registros ya estan puestos.
      }
    }
  }
};

// ---------------------------------------------------------------------------
namespace {
thread_local Cpu* t_cpu = nullptr;
struct TlsHolder {
  ~TlsHolder() {
    if (t_cpu) {
      detail::CpuImpl::Destroy(t_cpu);
      t_cpu = nullptr;
    }
  }
};
thread_local TlsHolder t_holder;

LONG WINAPI HostCrashFilter(EXCEPTION_POINTERS* ep) {
  static std::atomic<bool> busy{false};
  if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !busy.exchange(true)) {
    const auto addr = static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
    std::string msg = std::format("violacion de acceso del host en {} (rip = {})", static_cast<const void*>(p),
                                  ep->ExceptionRecord->ExceptionAddress);
    if (GuestMemory::Contains(p)) msg += std::format(" = direccion guest 0x{:08x}", static_cast<gaddr>(p - g_membase));
    if (t_cpu) {
      msg += std::format("\nimport en curso: '{}'\n", t_cpu->current_import());
      msg += debug::DumpState(*t_cpu);
    }
    log::Error("{}", msg);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Pagina de "kuser helpers" del kernel Linux ARM (0xffff0000): codigo que el userspace llama por
// direccion fija (atomicos de protobuf/gcc en ARMv7 antiguo). Implementados con LDREX/STREX.
void MapKuserHelpers() {
  constexpr gaddr kPage = 0xFFFF0000u;
  if (!GuestMemory::Map(kPage, 0x1000, kProtRW, "kuser")) return;
  auto put = [](gaddr at, std::initializer_list<std::uint32_t> code) {
    for (std::uint32_t w : code) {
      mem::W32(at, w);
      at += 4;
    }
  };
  // __kuser_cmpxchg64(const int64* old, const int64* new, int64* ptr) -> 0 si intercambio (C = 1)
  put(0xFFFF0F60u, {0xE92D00F0u,   // push {r4-r7}
                    0xE8900030u,   // ldmia r0, {r4, r5}
                    0xE89100C0u,   // ldmia r1, {r6, r7}
                    0xE1B20F9Fu,   // 1: ldrexd r0, r1, [r2]
                    0xE0303004u,   // eors r3, r0, r4
                    0x00313005u,   // eorseq r3, r1, r5
                    0x01A23F96u,   // strexdeq r3, r6, r7, [r2]
                    0x03330001u,   // teqeq r3, #1
                    0x0AFFFFF9u,   // beq 1b
                    0xE2730000u,   // rsbs r0, r3, #0
                    0xE8BD00F0u,   // pop {r4-r7}
                    0xE12FFF1Eu}); // bx lr
  // __kuser_memory_barrier
  put(0xFFFF0FA0u, {0xF57FF05Fu, 0xE12FFF1Eu});  // dmb sy; bx lr
  // __kuser_cmpxchg(int old, int new, int* ptr) -> 0 si intercambio (C = 1)
  put(0xFFFF0FC0u, {0xE1923F9Fu,   // 1: ldrex r3, [r2]
                    0xE0533000u,   // subs r3, r3, r0
                    0x01823F91u,   // strexeq r3, r1, [r2]
                    0x03330001u,   // teqeq r3, #1
                    0x0AFFFFFAu,   // beq 1b
                    0xE2730000u,   // rsbs r0, r3, #0
                    0xE12FFF1Eu}); // bx lr
  // __kuser_get_tls
  put(0xFFFF0FE0u, {0xEE1D0F70u, 0xE12FFF1Eu});  // mrc p15, 0, r0, c13, c0, 3; bx lr
  mem::W32(0xFFFF0FFCu, 5);                      // __kuser_helper_version
  GuestMemory::Protect(kPage, 0x1000, kProtRX);
}

LONG WINAPI DebugVeh(EXCEPTION_POINTERS* ep) {
  std::fprintf(stderr, "[veh] code=%08lx rip=%p addr=%p\n", ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress,
               reinterpret_cast<void*>(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0));
  return EXCEPTION_CONTINUE_SEARCH;
}

void RunHooks(std::vector<std::function<void(Cpu&)>>& list, Cpu& c) {
  std::vector<std::function<void(Cpu&)>> copy;
  {
    std::lock_guard lk(g_hooks_mu);
    copy = list;
  }
  for (auto& f : copy) f(c);
}
}  // namespace

Cpu::Cpu() = default;
Cpu::~Cpu() = default;

Cpu* detail::CpuImpl::Create(gsize stack_size, std::string name) {
  static std::once_flag once;
  std::call_once(once, [] {
    internal::HleEnsureInit();
    MapKuserHelpers();
    SetUnhandledExceptionFilter(HostCrashFilter);
    if (EnvFlag("ZW_DEBUG_VEH", false)) AddVectoredExceptionHandler(1, DebugVeh);
    if (g_trace.load() < 0) {
      if (const char* f = std::getenv("ZW_TRACE_FILTER")) g_trace_filter = f;
      g_trace.store(EnvFlag("ZW_TRACE_IMPORTS", false) ? 1 : 0);
    }
  });

  auto* c = new Cpu();
  c->impl_ = std::make_unique<CpuImpl>();
  CpuImpl& I = *c->impl_;
  I.cpu = c;
  {
    std::lock_guard lk(g_cpus_mu);
    std::size_t s = 0;
    while (s < threads::kMaxThreads && g_slots[s]) ++s;
    if (s == threads::kMaxThreads) {
      log::Error("Cpu: demasiados hilos guest (max {})", threads::kMaxThreads);
      delete c;
      return nullptr;
    }
    g_slots[s] = &I;
    I.slot = s;
  }
  c->thread_id_ = g_next_tid.fetch_add(1);
  c->name_ = name.empty() ? std::format("hilo{}", c->thread_id_) : std::move(name);

  if (stack_size == 0) stack_size = threads::kDefaultStackSize;
  if (stack_size < threads::kMinStackSize) stack_size = threads::kMinStackSize;
  stack_size = (stack_size + 0xFFF) & ~0xFFFu;
  I.stack_map_size = stack_size + GuestMemory::kPageSize;  // + pagina de guarda
  I.stack_map = GuestMemory::MapAnywhere(Zone::Stacks, I.stack_map_size, kProtRW, "stack");
  I.tls_map = GuestMemory::MapAnywhere(Zone::Stacks, GuestMemory::kPageSize, kProtRW, "tls");
  if (!I.stack_map || !I.tls_map) {
    log::Error("Cpu: sin memoria para la pila/TLS del hilo");
    Destroy(c);
    return nullptr;
  }
  GuestMemory::Protect(I.stack_map, GuestMemory::kPageSize, kProtNone);
  c->stack_base_ = I.stack_map + GuestMemory::kPageSize;
  c->stack_size_ = stack_size;
  c->tls_ = I.tls_map;
  c->errno_addr_ = I.tls_map + 8;  // TLS_SLOT_ERRNO
  mem::W32(c->tls_, c->tls_);
  mem::W32(c->tls_ + 4, c->thread_id_);

  I.cp15 = std::make_shared<Cp15>();
  I.cp15->tpidruro = c->tls_;

  Dynarmic::A32::UserConfig cfg;
  cfg.callbacks = &I;
  cfg.processor_id = I.slot;
  cfg.global_monitor = &Monitor();
  cfg.arch_version = Dynarmic::A32::ArchVersion::v8;
  if (EnvFlag("ZW_FASTMEM", true)) {
    // Requiere el parche de shadow space a dynarmic que aplica cmake/ZwDependencies.cmake.
    cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(g_membase);
    cfg.fastmem_exclusive_access = EnvFlag("ZW_FASTMEM_EXCLUSIVE", true);
  } else {
    using PT = std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
    cfg.page_table = reinterpret_cast<PT*>(internal::g_page_table);
    cfg.absolute_offset_page_table = true;
    // Los accesos que cruzan una frontera de pagina van por los callbacks (que validan ambas paginas).
    cfg.detect_misaligned_access_via_page_table = 16 | 32 | 64;
    cfg.only_detect_misalignment_via_page_table_on_page_boundary = true;
  }
  cfg.coprocessors[15] = I.cp15;
  cfg.define_unpredictable_behaviour = true;
  cfg.always_little_endian = true;
  cfg.enable_cycle_counting = false;
  std::size_t mb = 64;
  if (const char* e = std::getenv("ZW_JIT_CACHE_MB")) mb = std::max(16, std::atoi(e));
  cfg.code_cache_size = mb << 20;
  I.jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
  c->regs_ = I.jit->Regs().data();
  c->ext_ = I.jit->ExtRegs().data();
  c->regs_[13] = c->stack_base_ + c->stack_size_;
  I.jit->SetCpsr(0x00000010);
  I.jit->SetFpscr(0);
  g_live.fetch_add(1);
  return c;
}

void detail::CpuImpl::Destroy(Cpu* c) {
  if (!c) return;
  CpuImpl& I = *c->impl_;
  {
    std::lock_guard lk(g_cpus_mu);
    if (g_slots[I.slot] == &I) g_slots[I.slot] = nullptr;
  }
  if (I.jit) {
    I.jit.reset();
    g_live.fetch_sub(1);
  }
  Monitor().ClearProcessor(I.slot);
  if (I.stack_map) GuestMemory::Unmap(I.stack_map, I.stack_map_size);
  if (I.tls_map) GuestMemory::Unmap(I.tls_map, GuestMemory::kPageSize);
  delete c;
}

Cpu* Cpu::CurrentOrNull() { return t_cpu; }

Cpu& Cpu::Current() {
  if (!t_cpu) {
    (void)t_holder;  // fuerza la construccion del holder en este hilo
    t_cpu = detail::CpuImpl::Create(0, {});
    if (!t_cpu) {
      log::Error("Cpu::Current: no se pudo crear el Cpu del hilo");
      std::abort();
    }
    RunHooks(g_start_hooks, *t_cpu);
  }
  return *t_cpu;
}

std::uint32_t Cpu::cpsr() const { return impl_->jit->Cpsr(); }
void Cpu::set_cpsr(std::uint32_t v) { impl_->jit->SetCpsr(v); }
std::uint32_t Cpu::fpscr() const { return impl_->jit->Fpscr(); }
void Cpu::set_fpscr(std::uint32_t v) { impl_->jit->SetFpscr(v); }

std::uint64_t Cpu::CallRaw(gaddr fn, const std::uint32_t* words, std::size_t nwords) {
  detail::CpuImpl& I = *impl_;
  if (I.in_fast) {
    log::Error("call_guest invocado desde el handler FAST '{}': registralo con ZW_HLE (no FAST)", cur_import_);
    std::abort();
  }
  if (!fn) throw GuestFault(std::format("call_guest(NULL)\n{}", debug::DumpState(*this)));

  std::uint32_t saved[16];
  std::memcpy(saved, regs_, sizeof saved);
  const std::uint32_t saved_cpsr = I.jit->Cpsr();
  const char* saved_import = cur_import_;

  std::uint32_t sp = depth_ == 0 ? stack_base_ + stack_size_ : regs_[13];
  const std::size_t nstack = nwords > 4 ? nwords - 4 : 0;
  sp = (sp - static_cast<std::uint32_t>(4 * nstack)) & ~7u;
  for (std::size_t i = 0; i < nstack; ++i) mem::W32(sp + static_cast<std::uint32_t>(4 * i), words[4 + i]);
  for (std::size_t i = 0; i < 4; ++i) regs_[i] = i < nwords ? words[i] : 0;
  regs_[13] = sp;
  regs_[14] = kReturnStub;
  regs_[15] = fn & ~1u;
  I.jit->SetCpsr(0x00000010 | ((fn & 1) ? 0x20 : 0));

  ++depth_;
  cur_import_ = "";
  try {
    I.RunUntilReturn();
  } catch (...) {
    --depth_;
    cur_import_ = saved_import;
    throw;
  }
  --depth_;
  cur_import_ = saved_import;
  const std::uint64_t result = regs_[0] | (static_cast<std::uint64_t>(regs_[1]) << 32);
  std::memcpy(regs_, saved, sizeof saved);
  I.jit->SetCpsr(saved_cpsr);
  return result;
}

// ---------------------------------------------------------------------------
namespace threads {
namespace {
struct Rec {
  std::thread th;
  bool detached = false;
  std::atomic<bool> finished{false};
  std::uint32_t ret = 0;
};
std::mutex g_threads_mu;
std::map<ThreadId, std::shared_ptr<Rec>> g_threads;

void DefaultFatal(const std::string& what) {
  log::Error("hilo guest muerto por fallo; terminando el proceso.\n{}", what);
  std::_Exit(70);
}
}  // namespace

ThreadId Create(const CreateParams& p) {
  if (!p.entry) return 0;
  auto rec = std::make_shared<Rec>();
  rec->detached = p.detached;
  std::promise<ThreadId> started;
  auto fut = started.get_future();
  rec->th = std::thread([rec, p, pr = std::move(started)]() mutable {
    (void)t_holder;
    Cpu* c = detail::CpuImpl::Create(p.stack_size, p.name);
    if (!c) {
      pr.set_value(0);
      return;
    }
    t_cpu = c;
    const ThreadId id = c->thread_id();
    {
      // Registrar antes de avisar al creador: asi Join(id) inmediato lo encuentra.
      std::lock_guard lk(g_threads_mu);
      g_threads[id] = rec;
    }
    pr.set_value(id);
    try {
      RunHooks(g_start_hooks, *c);
      rec->ret = c->Call<std::uint32_t>(p.entry, p.arg);
      RunHooks(g_exit_hooks, *c);
    } catch (const GuestFault& f) {
      (g_fatal ? g_fatal : DefaultFatal)(f.what());
    } catch (const GuestExit& e) {
      if (g_exit) g_exit(e.code);
      else std::_Exit(e.code);
    }
    t_cpu = nullptr;
    detail::CpuImpl::Destroy(c);
    rec->finished.store(true);
    if (rec->detached) {
      std::lock_guard lk(g_threads_mu);
      auto it = g_threads.find(id);
      if (it != g_threads.end() && it->second->detached) g_threads.erase(it);
    }
  });
  const ThreadId id = fut.get();
  if (!id) {
    rec->th.join();
    return 0;
  }
  if (p.detached) rec->th.detach();
  return id;
}

bool Join(ThreadId id, std::uint32_t* retval) {
  std::shared_ptr<Rec> rec;
  {
    std::lock_guard lk(g_threads_mu);
    auto it = g_threads.find(id);
    if (it == g_threads.end() || it->second->detached) return false;
    rec = it->second;
    g_threads.erase(it);
  }
  if (rec->th.joinable()) rec->th.join();
  if (retval) *retval = rec->ret;
  return true;
}

bool Detach(ThreadId id) {
  std::lock_guard lk(g_threads_mu);
  auto it = g_threads.find(id);
  if (it == g_threads.end() || it->second->detached) return false;
  it->second->detached = true;
  if (it->second->th.joinable()) it->second->th.detach();
  if (it->second->finished.load()) g_threads.erase(it);
  return true;
}

bool IsAlive(ThreadId id) {
  std::lock_guard lk(g_threads_mu);
  auto it = g_threads.find(id);
  return it != g_threads.end() && !it->second->finished.load();
}

ThreadId CurrentId() { return Cpu::Current().thread_id(); }
std::size_t Count() { return g_live.load(); }

void AddStartHook(std::function<void(Cpu&)> fn) {
  std::lock_guard lk(g_hooks_mu);
  g_start_hooks.push_back(std::move(fn));
}
void AddExitHook(std::function<void(Cpu&)> fn) {
  std::lock_guard lk(g_hooks_mu);
  g_exit_hooks.push_back(std::move(fn));
}
void DetachCurrent() {
  if (!t_cpu || t_cpu->call_depth() != 0) return;
  RunHooks(g_exit_hooks, *t_cpu);
  detail::CpuImpl::Destroy(t_cpu);
  t_cpu = nullptr;
}
}  // namespace threads

// ---------------------------------------------------------------------------
namespace cpu {
void InvalidateCode(gaddr addr, gsize size) {
  std::lock_guard lk(g_cpus_mu);
  for (auto* I : g_slots)
    if (I && I->jit) I->jit->InvalidateCacheRange(addr, size);
}
void SetFatalHandler(std::function<void(const std::string&)> fn) { g_fatal = std::move(fn); }
void SetExitHandler(std::function<void(int)> fn) { g_exit = std::move(fn); }
}  // namespace cpu

// ---------------------------------------------------------------------------
// setjmp / longjmp sobre el estado guest.
namespace {
constexpr std::uint32_t kJmpMagic = 0x424A575Au;  // "ZWJB"

void hle_setjmp(Cpu& c) {
  const gaddr b = c.reg(0);
  mem::W32(b, kJmpMagic);
  mem::W32(b + 4, static_cast<std::uint32_t>(c.call_depth()));
  for (int i = 0; i < 8; ++i) mem::W32(b + 8 + 4 * i, c.reg(4 + i));
  mem::W32(b + 40, c.sp());
  mem::W32(b + 44, c.lr());
  mem::W32(b + 48, c.fpscr());
  for (int i = 0; i < 8; ++i) mem::W64(b + 56 + 8 * i, c.d(8 + i));
  c.reg(0) = 0;
}

void hle_longjmp(Cpu& c) {
  const gaddr b = c.reg(0);
  const std::uint32_t val = c.reg(1);
  if (!GuestMemory::IsMapped(b, 120) || mem::R32(b) != kJmpMagic)
    throw GuestFault(std::format("longjmp con jmp_buf invalido 0x{:08x}\n{}", b, debug::DumpState(c)));
  const int depth = static_cast<int>(mem::R32(b + 4));
  for (int i = 0; i < 8; ++i) c.reg(4 + i) = mem::R32(b + 8 + 4 * i);
  c.sp() = mem::R32(b + 40);
  c.lr() = mem::R32(b + 44);  // el `bx lr` del stub vuelve al punto de retorno de setjmp
  c.set_fpscr(mem::R32(b + 48));
  for (int i = 0; i < 8; ++i) c.set_d(8 + i, mem::R64(b + 56 + 8 * i));
  c.reg(0) = val ? val : 1;
  if (depth > c.call_depth())
    throw GuestFault(std::format("longjmp a un marco ya terminado\n{}", debug::DumpState(c)));
  if (depth < c.call_depth()) throw LongjmpUnwind{depth};
}
}  // namespace

void internal::RegisterBuiltins() {
  for (const char* n : {"setjmp", "_setjmp", "sigsetjmp"}) HleRegisterNoInit(n, hle_setjmp, hle::kFast);
  for (const char* n : {"longjmp", "_longjmp", "siglongjmp"}) HleRegisterNoInit(n, hle_longjmp, hle::kDefault);
}

// ---------------------------------------------------------------------------
namespace debug {
void AddSymbolizer(Symbolizer fn) {
  std::lock_guard lk(g_sym_mu);
  g_symbolizers.push_back(std::move(fn));
}

std::string addr2sym(gaddr addr) {
  if (addr >= GuestMemory::kThunksBase && addr < GuestMemory::kThunksEnd) {
    std::string n = hle::StubName(addr & ~1u);
    if (!n.empty()) return "hle!" + n;
  }
  if (addr >= 0xFFFF0F60u) {
    const gaddr a = addr & ~3u;
    if (a >= 0xFFFF0FE0u) return "kuser!__kuser_get_tls";
    if (a >= 0xFFFF0FC0u) return "kuser!__kuser_cmpxchg";
    if (a >= 0xFFFF0FA0u) return "kuser!__kuser_memory_barrier";
    if (a < 0xFFFF0F90u) return "kuser!__kuser_cmpxchg64";
  }
  std::vector<Symbolizer> copy;
  {
    std::lock_guard lk(g_sym_mu);
    copy = g_symbolizers;
  }
  std::string out;
  for (auto& s : copy)
    if (s(addr, out)) return out;
  return std::format("0x{:08x}", addr);
}

namespace {
bool LooksLikeReturnAddress(gaddr v) {
  const gaddr a = v & ~1u;
  if (a < 8 || !(internal::PageProt(a) & kProtExec) || !(internal::PageProt(a - 4) & kProtExec)) return false;
  if (a >= GuestMemory::kThunksBase && a < GuestMemory::kThunksEnd) return false;
  if (v & 1) {
    const std::uint16_t h1 = mem::R16(a - 4), h2 = mem::R16(a - 2);
    if ((h1 & 0xF800) == 0xF000 && ((h2 & 0xD000) == 0xD000 || (h2 & 0xD001) == 0xC000)) return true;  // bl / blx imm
    return (h2 & 0xFF87) == 0x4780;  // blx rX
  }
  if (v & 3) return false;
  const std::uint32_t w = mem::R32(a - 4);
  return (w & 0x0F000000u) == 0x0B000000u || (w & 0xFE000000u) == 0xFA000000u || (w & 0x0FFFFFF0u) == 0x012FFF30u;
}
}  // namespace

std::vector<gaddr> Backtrace(const Cpu& cpu, std::size_t max_frames) {
  std::vector<gaddr> out;
  out.push_back(cpu.reg(15));
  out.push_back(cpu.reg(14));
  gaddr sp = cpu.reg(13) & ~3u;
  const gaddr top = cpu.stack_base() + cpu.stack_size();
  if (sp < cpu.stack_base() || sp > top) return out;
  for (gaddr a = sp; a + 4 <= top && out.size() < max_frames; a += 4) {
    if (!GuestMemory::IsMapped(a, 4)) break;
    const gaddr v = mem::R32(a);
    if (v != out.back() && LooksLikeReturnAddress(v)) out.push_back(v);
  }
  return out;
}

std::string DumpState(const Cpu& cpu) {
  std::string s = std::format("--- estado guest: hilo {} '{}'", cpu.thread_id(), cpu.name());
  if (*cpu.current_import()) s += std::format(", en import '{}'", cpu.current_import());
  s += std::format(", profundidad {} ---\n", cpu.call_depth());
  for (int i = 0; i < 16; ++i) {
    static const char* names[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
    s += std::format("{:>3}={:08x}{}", names[i], cpu.reg(i), (i % 4 == 3) ? "\n" : "  ");
  }
  s += std::format("cpsr={:08x} fpscr={:08x}\n", cpu.cpsr(), cpu.fpscr());
  s += "backtrace (heuristico):\n";
  int n = 0;
  for (gaddr a : Backtrace(cpu)) s += std::format("  #{:<2} {:08x}  {}\n", n++, a, addr2sym(a));
  return s;
}

void SetTraceImports(bool on) { g_trace.store(on ? 1 : 0); }
bool TraceImports() { return g_trace.load() > 0; }
}  // namespace debug

}  // namespace zw
