// Nucleo de la capa Java emulada: tabla de referencias, clases/metodos/campos, marcos locales por
// hilo, cobertura y busqueda de nativos (T7).
#include <algorithm>
#include <stdexcept>

#include "core/log.h"
#include "cpu/hle.h"
#include "jni/internal.h"
#include "loader/loader.h"

namespace zw::jni {

// ---------------------------------------------------------------------------------------------
// Tabla de referencias
// ---------------------------------------------------------------------------------------------
namespace {
struct RefTable {
  std::mutex mu;
  std::vector<ObjPtr> slots{nullptr};  // indice 0 reservado
  std::vector<std::uint32_t> free;
};
RefTable& Refs() {
  static RefTable t;
  return t;
}
}  // namespace

jref AddRef(const ObjPtr& o, RefKind kind) {
  if (!o) return 0;
  auto& t = Refs();
  std::lock_guard lk(t.mu);
  std::uint32_t idx;
  if (!t.free.empty()) {
    idx = t.free.back();
    t.free.pop_back();
    t.slots[idx] = o;
  } else {
    idx = static_cast<std::uint32_t>(t.slots.size());
    if (idx >= (1u << 29)) return 0;
    t.slots.push_back(o);
  }
  return (idx << 2) | static_cast<std::uint32_t>(kind);
}

bool RemoveRef(jref r) {
  const std::uint32_t idx = r >> 2;
  if (!r || !(r & 3)) return false;
  ObjPtr dead;  // se destruye fuera del cerrojo (el destructor puede liberar memoria guest)
  {
    auto& t = Refs();
    std::lock_guard lk(t.mu);
    if (idx == 0 || idx >= t.slots.size() || !t.slots[idx]) return false;
    dead = std::move(t.slots[idx]);
    t.slots[idx] = nullptr;
    t.free.push_back(idx);
  }
  return true;
}

ObjPtr Resolve(jref r) {
  const std::uint32_t idx = r >> 2;
  if (!r || !(r & 3)) return nullptr;
  auto& t = Refs();
  std::lock_guard lk(t.mu);
  if (idx == 0 || idx >= t.slots.size()) return nullptr;
  return t.slots[idx];
}

std::size_t LiveRefs() {
  auto& t = Refs();
  std::lock_guard lk(t.mu);
  return t.slots.size() - 1 - t.free.size();
}

// ---------------------------------------------------------------------------------------------
// Estado por hilo y marcos locales
// ---------------------------------------------------------------------------------------------
ThreadState& TS() {
  Cpu& cpu = Cpu::Current();
  void*& u = cpu.user(UserSlot::Jni);
  if (!u) {
    auto* ts = new ThreadState();
    ts->env = GuestMemory::Calloc(2, 4);
    mem::W32(ts->env, EnvTable());
    mem::W32(ts->env + 4, cpu.thread_id());
    ts->frames.emplace_back();
    u = ts;
  }
  return *static_cast<ThreadState*>(u);
}

void DestroyThreadState(Cpu& cpu) {
  void*& u = cpu.user(UserSlot::Jni);
  if (!u) return;
  auto* ts = static_cast<ThreadState*>(u);
  for (auto& f : ts->frames)
    for (jref r : f) RemoveRef(r);
  if (ts->env) GuestMemory::Free(ts->env);
  delete ts;
  u = nullptr;
}

jref NewLocal(const ObjPtr& o) {
  const jref r = AddRef(o, RefKind::Local);
  if (r) {
    auto& ts = TS();
    ts.frames.back().push_back(r);
    if (ts.frames.size() == 1 && ts.frames.back().size() == 100000)
      log::Warn("[jni] el hilo {} acumula 100000 referencias locales sin liberar en su marco base", Cpu::Current().thread_id());
  }
  return r;
}

jref NewGlobal(const ObjPtr& o) { return AddRef(o, RefKind::Global); }

void DeleteLocal(jref r) {
  if ((r & 3) != static_cast<std::uint32_t>(RefKind::Local)) return;
  auto& ts = TS();
  for (auto f = ts.frames.rbegin(); f != ts.frames.rend(); ++f) {
    auto it = std::find(f->rbegin(), f->rend(), r);
    if (it != f->rend()) {
      f->erase(std::next(it).base());
      break;
    }
  }
  RemoveRef(r);
}

void PushFrame() { TS().frames.emplace_back(); }

void PopFrame() {
  auto& ts = TS();
  if (ts.frames.size() <= 1) {
    log::Warn("[jni] PopLocalFrame sin PushLocalFrame correspondiente");
    return;
  }
  std::vector<jref> f = std::move(ts.frames.back());
  ts.frames.pop_back();
  for (jref r : f) RemoveRef(r);
}

void ClearBaseFrame() {
  auto& ts = TS();
  for (jref r : ts.frames.front()) RemoveRef(r);
  ts.frames.front().clear();
}

void DeleteRef(jref r) {
  if ((r & 3) == static_cast<std::uint32_t>(RefKind::Local)) DeleteLocal(r);
  else RemoveRef(r);
}

namespace detail {
LocalFrameGuard::LocalFrameGuard() { PushFrame(); }
LocalFrameGuard::~LocalFrameGuard() { PopFrame(); }
void ThrowNoNative(std::string_view name) {
  throw std::runtime_error(std::format("jni::call_native: no existe el nativo '{}'", name));
}
}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Clases
// ---------------------------------------------------------------------------------------------
namespace {
struct Registry {
  std::recursive_mutex mu;
  std::map<std::string, std::shared_ptr<Class>, std::less<>> classes;
  std::vector<Method*> methods;  // por id
  std::vector<Field*> fields;
  std::map<std::string, gaddr, std::less<>> natives;  // RegisterNatives: nombre mangled -> fn
};
Registry& Reg() {
  static Registry r;
  return r;
}
Class* g_meta = nullptr;    // java/lang/Class
Class* g_object = nullptr;  // java/lang/Object
Class* g_string = nullptr;  // java/lang/String

std::string NormalizeClassName(std::string_view n) {
  std::string s(n);
  std::replace(s.begin(), s.end(), '.', '/');
  if (s.size() > 2 && s.front() == 'L' && s.back() == ';') s = s.substr(1, s.size() - 2);
  return s;
}

Class* GetClassLocked(Registry& r, std::string_view raw) {
  const std::string name = NormalizeClassName(raw);
  if (auto it = r.classes.find(name); it != r.classes.end()) return it->second.get();
  auto c = std::make_shared<Class>(g_meta, name);
  if (!g_meta) {  // arranque: java/lang/Class es su propia metaclase
    c->cls = c.get();
    g_meta = c.get();
  }
  Class* raw_ptr = c.get();
  r.classes.emplace(name, std::move(c));
  if (name != "java/lang/Object") raw_ptr->super = g_object ? g_object : GetClassLocked(r, "java/lang/Object");
  return raw_ptr;
}

void ParseSig(std::string_view sig, std::vector<char>& params, char& ret) {
  params.clear();
  ret = 'V';
  std::size_t i = sig.find('(');
  if (i == std::string_view::npos) return;
  ++i;
  auto skip_type = [&](std::size_t& p) -> char {
    char t = sig[p];
    if (t == '[') {
      while (p < sig.size() && sig[p] == '[') ++p;
      if (p < sig.size() && sig[p] == 'L') p = sig.find(';', p);
      if (p == std::string_view::npos) p = sig.size() - 1;
      ++p;
      return 'L';
    }
    if (t == 'L') {
      p = sig.find(';', p);
      if (p == std::string_view::npos) p = sig.size() - 1;
      ++p;
      return 'L';
    }
    ++p;
    return t;
  };
  while (i < sig.size() && sig[i] != ')') params.push_back(skip_type(i));
  if (i < sig.size() && sig[i] == ')') {
    ++i;
    if (i < sig.size()) ret = skip_type(i);
  }
}

Method* AddMethodLocked(Registry& r, Class* c, std::string_view name, std::string_view sig, bool is_static) {
  const std::string key = std::string(name) + std::string(sig);
  auto& slot = c->methods[key];
  if (!slot) {
    slot = std::make_unique<Method>();
    slot->cls = c;
    slot->name = name;
    slot->sig = sig;
    slot->is_static = is_static;
    ParseSig(sig, slot->params, slot->ret);
    r.methods.push_back(slot.get());
    slot->id = kMethodIdBase + 8u * static_cast<std::uint32_t>(r.methods.size());
  }
  return slot.get();
}

Field* AddFieldLocked(Registry& r, Class* c, std::string_view name, std::string_view sig, bool is_static) {
  const std::string key = std::string(name) + ":" + std::string(sig);
  auto& slot = c->fields[key];
  if (!slot) {
    slot = std::make_unique<Field>();
    slot->cls = c;
    slot->name = name;
    slot->sig = sig;
    slot->is_static = is_static;
    r.fields.push_back(slot.get());
    slot->id = kFieldIdBase + 8u * static_cast<std::uint32_t>(r.fields.size());
  }
  return slot.get();
}
}  // namespace

bool Class::IsSubclassOf(const Class* other) const {
  for (const Class* c = this; c; c = c->super)
    if (c == other) return true;
  return false;
}

Class* GetClass(std::string_view name) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  return GetClassLocked(r, name);
}

Class* FindLoadedClass(std::string_view name) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  auto it = r.classes.find(NormalizeClassName(name));
  return it == r.classes.end() ? nullptr : it->second.get();
}

ObjPtr ClassObj(Class* c) { return c ? c->shared_from_this() : nullptr; }

void InitCoreClasses() {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  if (g_object) return;
  GetClassLocked(r, "java/lang/Class");
  g_object = GetClassLocked(r, "java/lang/Object");
  g_object->super = nullptr;
  g_meta->super = g_object;
  g_string = GetClassLocked(r, "java/lang/String");
  g_meta->emulated = g_object->emulated = g_string->emulated = true;
}
Class* StringClass() { return g_string; }

ClassDef DefineClass(std::string_view name) {
  Class* c = GetClass(name);
  c->emulated = true;
  return ClassDef(c);
}

ClassDef& ClassDef::Static(std::string_view name, std::string_view sig, Impl impl) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  Method* m = AddMethodLocked(r, c_, name, sig, true);
  m->is_static = true;
  m->impl = std::move(impl);
  return *this;
}
ClassDef& ClassDef::Virtual(std::string_view name, std::string_view sig, Impl impl) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  Method* m = AddMethodLocked(r, c_, name, sig, false);
  m->is_static = false;
  m->impl = std::move(impl);
  return *this;
}
ClassDef& ClassDef::StaticNoop(std::initializer_list<std::pair<const char*, const char*>> name_sig) {
  for (const auto& [n, s] : name_sig) {
    std::vector<char> p;
    char ret;
    ParseSig(s, p, ret);
    Static(n, s, [ret](Call&) { return DefaultFor(ret); });
  }
  return *this;
}
ClassDef& ClassDef::StaticField(std::string_view name, std::string_view sig, Value v) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  Field* f = AddFieldLocked(r, c_, name, sig, true);
  f->defined = true;
  std::lock_guard fl(f->mu);
  f->static_value = std::move(v);
  return *this;
}
ClassDef& ClassDef::InstanceField(std::string_view name, std::string_view sig) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  AddFieldLocked(r, c_, name, sig, false)->defined = true;
  return *this;
}
ClassDef& ClassDef::Super(std::string_view super_name) {
  c_->super = GetClass(super_name);
  return *this;
}
ClassDef& ClassDef::Factory(std::function<ObjPtr(Class*)> f) {
  c_->factory = std::move(f);
  return *this;
}

Method* LookupMethod(Class* c, std::string_view name, std::string_view sig) {
  if (!c) return nullptr;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::string key = std::string(name) + std::string(sig);
  for (Class* k = c; k; k = k->super)
    if (auto it = k->methods.find(key); it != k->methods.end()) return it->second.get();
  return nullptr;
}

Method* GetOrAddMethod(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered) {
  Method* m = LookupMethod(c, name, sig);
  if (m) {
    *covered = static_cast<bool>(m->impl);
    return m;
  }
  *covered = false;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  return AddMethodLocked(r, c, name, sig, is_static);
}

Method* MethodById(std::uint32_t id) {
  if (id <= kMethodIdBase || (id - kMethodIdBase) % 8) return nullptr;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::uint32_t i = (id - kMethodIdBase) / 8 - 1;
  return i < r.methods.size() ? r.methods[i] : nullptr;
}

zw::jni::Field* GetOrAddField(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::string key = std::string(name) + ":" + std::string(sig);
  for (Class* k = c; k; k = k->super)
    if (auto it = k->fields.find(key); it != k->fields.end()) {
      *covered = it->second->defined;
      return it->second.get();
    }
  *covered = false;
  return AddFieldLocked(r, c, name, sig, is_static);
}

zw::jni::Field* FieldById(std::uint32_t id) {
  if (id <= kFieldIdBase || (id - kFieldIdBase) % 8) return nullptr;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::uint32_t i = (id - kFieldIdBase) / 8 - 1;
  return i < r.fields.size() ? r.fields[i] : nullptr;
}

ObjPtr AllocInstance(Class* c) {
  if (!c) return nullptr;
  for (Class* k = c; k; k = k->super)
    if (k->factory) {
      ObjPtr o = k->factory(c);
      if (o) o->cls = c;
      return o;
    }
  return std::make_shared<Object>(c);
}

// ---------------------------------------------------------------------------------------------
// Valores, strings, arrays
// ---------------------------------------------------------------------------------------------
Value DefaultFor(char ret) {
  switch (ret) {
    case 'V': return Value();
    case 'L': case '[': return Value::Null();
    case 'F': case 'D': return Value::Float(0);
    default: return Value::Int(0);
  }
}

Value Value::Str(std::string_view s) { return Value::Obj(MakeString(s)); }

std::shared_ptr<String> MakeString(std::string_view utf8) {
  return std::make_shared<String>(g_string ? g_string : GetClass("java/lang/String"), std::string(utf8));
}

std::string Call::S(std::size_t n) const {
  auto s = std::dynamic_pointer_cast<String>(O(n));
  return s ? s->utf8 : std::string();
}

std::uint32_t ElemSize(char e) {
  switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
  }
}

Array::Array(Class* c, char e, std::uint32_t n) : Object(c), elem(e), length(n), elem_size(ElemSize(e)) {
  if (e == 'L') objects.resize(n);
  else data = GuestMemory::Calloc(n ? n : 1, elem_size);
}
Array::~Array() {
  if (data) GuestMemory::Free(data);
}

std::shared_ptr<Array> MakeArray(char elem, std::uint32_t n) {
  const std::string cname = elem == 'L' ? std::string("[Ljava/lang/Object;") : std::string("[") + elem;
  auto a = std::make_shared<Array>(GetClass(cname), elem, n);
  if (elem != 'L' && !a->data) return nullptr;
  return a;
}

// ---------------------------------------------------------------------------------------------
// Invocacion
// ---------------------------------------------------------------------------------------------
Value Invoke(Method* m, ObjPtr self, Class* cls, std::vector<Value>&& args, bool virt) {
  if (!m) return Value();
  if (virt && self && self->cls && self->cls != m->cls) {
    if (Method* o = LookupMethod(self->cls, m->name, m->sig); o && o->impl) m = o;
  }
  ++m->calls;
  if (config().verbose_calls) log::Debug("[jni] llamada {}.{}{}", m->cls->name, m->name, m->sig);
  if (!m->impl) {
    if (!m->warned.exchange(true)) {
      log::Warn("[jni] metodo NO implementado: {}.{}{} -> {} (por defecto)", m->cls->name, m->name, m->sig,
                m->ret == 'V' ? "void" : (m->ret == 'L' ? "null" : "0"));
      NoteCoverage(CoverageEntry::Kind::Call, m->cls->name, m->name, m->sig, false);
    }
    return DefaultFor(m->ret);
  }
  Call c;
  c.method = m;
  c.self = std::move(self);
  c.cls = cls ? cls : m->cls;
  c.args = std::move(args);
  return m->impl(c);
}

// ---------------------------------------------------------------------------------------------
// Cobertura
// ---------------------------------------------------------------------------------------------
namespace {
struct Cov {
  std::mutex mu;
  std::map<std::string, CoverageEntry> entries;
  std::vector<std::string> order;
};
Cov& Covg() {
  static Cov c;
  return c;
}
const char* KindName(CoverageEntry::Kind k) {
  switch (k) {
    case CoverageEntry::Kind::Class: return "FindClass";
    case CoverageEntry::Kind::Method: return "GetMethodID";
    case CoverageEntry::Kind::StaticMethod: return "GetStaticMethodID";
    case CoverageEntry::Kind::Field: return "GetFieldID";
    case CoverageEntry::Kind::StaticField: return "GetStaticFieldID";
    case CoverageEntry::Kind::Call: return "Call(no impl.)";
  }
  return "?";
}
}  // namespace

void NoteCoverage(CoverageEntry::Kind k, const std::string& cls, const std::string& name, const std::string& sig, bool covered) {
  auto& c = Covg();
  const std::string key = std::to_string(static_cast<int>(k)) + "|" + cls + "|" + name + "|" + sig;
  bool first = false;
  {
    std::lock_guard lk(c.mu);
    auto [it, inserted] = c.entries.try_emplace(key);
    if (inserted) {
      it->second.kind = k;
      it->second.cls = cls;
      it->second.name = name;
      it->second.sig = sig;
      it->second.covered = covered;
      c.order.push_back(key);
      first = true;
    }
    ++it->second.count;
  }
  if (first && k != CoverageEntry::Kind::Call) {
    if (k == CoverageEntry::Kind::Class)
      log::Info("[jni] {}({}) -> {}", KindName(k), cls, covered ? "cubierta" : "NO cubierta (clase vacia)");
    else
      log::Info("[jni] {}({}.{} {}) -> {}", KindName(k), cls, name, sig, covered ? "cubierto" : "NO cubierto");
  }
}

std::vector<CoverageEntry> Coverage() {
  auto& c = Covg();
  std::lock_guard lk(c.mu);
  std::vector<CoverageEntry> v;
  v.reserve(c.order.size());
  for (const auto& k : c.order) v.push_back(c.entries.at(k));
  return v;
}

std::string CoverageReport() {
  std::string out;
  std::size_t n = 0, ok = 0;
  for (const auto& e : Coverage()) {
    ++n;
    if (e.covered) ++ok;
    if (e.kind == CoverageEntry::Kind::Class)
      out += std::format("  {:3} {:18} {}  (x{})\n", e.covered ? "ok" : "NO", KindName(e.kind), e.cls, e.count);
    else
      out += std::format("  {:3} {:18} {}.{} {}  (x{})\n", e.covered ? "ok" : "NO", KindName(e.kind), e.cls, e.name, e.sig, e.count);
  }
  out += std::format("  total: {} entradas, {} cubiertas, {} sin cubrir\n", n, ok, n - ok);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Nativos
// ---------------------------------------------------------------------------------------------
namespace {
constexpr std::string_view kGamePrefix = "Java_com_gameloft_android_ANMP_GloftZOHM_";

std::string Mangle(std::string_view s) {
  std::string o;
  for (char ch : s) {
    if (ch == '/' || ch == '.') o += '_';
    else if (ch == '_') o += "_1";
    else if (ch == ';') o += "_2";
    else if (ch == '[') o += "_3";
    else o += ch;
  }
  return o;
}

std::string FullNativeName(std::string_view name) {
  if (name.starts_with("Java_")) return std::string(name);
  return std::string(kGamePrefix) + std::string(name);
}
}  // namespace

void RegisterNativeMethod(Class* c, std::string_view name, std::string_view sig, gaddr fn) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  r.natives["Java_" + Mangle(c->name) + "_" + Mangle(name)] = fn;
  r.natives[c->name + "." + std::string(name)] = fn;
  Method* m = AddMethodLocked(r, c, name, sig, false);
  m->native = fn;
}

void UnregisterNatives(Class* c) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::string pre1 = "Java_" + Mangle(c->name) + "_", pre2 = c->name + ".";
  for (auto it = r.natives.begin(); it != r.natives.end();)
    it = (it->first.starts_with(pre1) || it->first.starts_with(pre2)) ? r.natives.erase(it) : std::next(it);
}

gaddr FindNative(std::string_view name) {
  {
    auto& r = Reg();
    std::lock_guard lk(r.mu);
    if (auto it = r.natives.find(name); it != r.natives.end()) return it->second;
    if (auto it = r.natives.find(FullNativeName(name)); it != r.natives.end()) return it->second;
  }
  if (name.find('.') != std::string_view::npos) return 0;
  return loader::FindSymbol(FullNativeName(name));
}

jref ClassOfNative(std::string_view name) {
  std::string full;
  if (const auto dot = name.find('.'); dot != std::string_view::npos) {
    return NewLocal(ClassObj(GetClass(name.substr(0, dot))));
  }
  full = FullNativeName(name);
  Class* best = nullptr;
  std::size_t best_len = 0;
  {
    auto& r = Reg();
    std::lock_guard lk(r.mu);
    for (auto& [cname, c] : r.classes) {
      const std::string pre = "Java_" + Mangle(cname) + "_";
      if (pre.size() > best_len && full.starts_with(pre) && full.find('_', pre.size()) == std::string::npos) {
        best = c.get();
        best_len = pre.size();
      }
    }
  }
  if (!best) {
    // Clase no emulada: deducirla del nombre (sin escapes `_1`): Java_a_b_C_m -> a/b/C
    std::string body = full.substr(5);
    const auto last = body.rfind('_');
    std::string cls = last == std::string::npos ? body : body.substr(0, last);
    std::replace(cls.begin(), cls.end(), '_', '/');
    best = GetClass(cls);
  }
  return NewLocal(ClassObj(best));
}

// ---------------------------------------------------------------------------------------------
// API de host
// ---------------------------------------------------------------------------------------------
jref NewString(std::string_view utf8) { return NewLocal(MakeString(utf8)); }

std::string GetString(jref s) {
  auto o = std::dynamic_pointer_cast<String>(Resolve(s));
  return o ? o->utf8 : std::string();
}

jref FindClass(std::string_view name) {
  Class* c = GetClass(name);
  return NewLocal(ClassObj(c));
}

Config& config() {
  static Config c;
  return c;
}
Hooks& hooks() {
  static Hooks h;
  return h;
}

}  // namespace zw::jni
