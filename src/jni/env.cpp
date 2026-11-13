// JNIEnv (JNINativeInterface, 233 slots de JNI 1.6) y JavaVM (JNIInvokeInterface) en memoria guest.
// Cada slot es un stub HLE "JNIEnv::<Nombre>" (T7).
#include <cstring>

#include "core/log.h"
#include "cpu/hle.h"
#include "jni/internal.h"

namespace zw::jni {
namespace {

gaddr g_env_table = 0;
gaddr g_vm = 0;

// ---------------------------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------------------------
Class* ClassFromRef(jref r) { return dynamic_cast<Class*>(Resolve(r).get()); }

template <class T>
std::shared_ptr<T> As(jref r) {
  return std::dynamic_pointer_cast<T>(Resolve(r));
}

std::string SafeCStr(gaddr a) { return a && GuestMemory::IsMapped(a) ? mem::ReadCStr(a, 4096) : std::string(); }

Value ReadOne(char t, VaList& va) {
  switch (t) {
    case 'Z': return Value::Bool((va.NextWord() & 0xFF) != 0);
    case 'B': return Value::Int(static_cast<std::int8_t>(va.NextWord()));
    case 'C': return Value::Int(static_cast<std::uint16_t>(va.NextWord()));
    case 'S': return Value::Int(static_cast<std::int16_t>(va.NextWord()));
    case 'I': return Value::Int(static_cast<std::int32_t>(va.NextWord()));
    case 'J': return Value::Int(static_cast<std::int64_t>(va.NextDword()));
    case 'F':  // float se promociona a double en "..." y en va_list
    case 'D': return Value::Float(std::bit_cast<double>(va.NextDword()));
    default: return Value::Obj(Resolve(va.NextWord()));
  }
}

std::vector<Value> ReadArgs(const Method* m, VaList& va) {
  std::vector<Value> v;
  if (!m) return v;
  v.reserve(m->params.size());
  for (char t : m->params) v.push_back(ReadOne(t, va));
  return v;
}

std::vector<Value> ReadArgsA(const Method* m, gaddr jv) {
  std::vector<Value> v;
  if (!m || !jv) return v;
  v.reserve(m->params.size());
  for (std::size_t i = 0; i < m->params.size(); ++i) {
    const gaddr a = jv + 8 * static_cast<gaddr>(i);
    switch (m->params[i]) {
      case 'Z': v.push_back(Value::Bool(mem::R8(a) != 0)); break;
      case 'B': v.push_back(Value::Int(static_cast<std::int8_t>(mem::R8(a)))); break;
      case 'C': v.push_back(Value::Int(mem::R16(a))); break;
      case 'S': v.push_back(Value::Int(static_cast<std::int16_t>(mem::R16(a)))); break;
      case 'I': v.push_back(Value::Int(static_cast<std::int32_t>(mem::R32(a)))); break;
      case 'J': v.push_back(Value::Int(static_cast<std::int64_t>(mem::R64(a)))); break;
      case 'F': v.push_back(Value::Float(std::bit_cast<float>(mem::R32(a)))); break;
      case 'D': v.push_back(Value::Float(std::bit_cast<double>(mem::R64(a)))); break;
      default: v.push_back(Value::Obj(Resolve(mem::R32(a)))); break;
    }
  }
  return v;
}

// Valor Java -> retorno del slot (R = tipo C++ del jXXX del slot).
template <class R>
R Ret(const Value& v) {
  if constexpr (std::is_void_v<R>) {
    return;
  } else if constexpr (std::is_same_v<R, jref>) {
    return NewLocal(v.o);
  } else if constexpr (std::is_same_v<R, float>) {
    return static_cast<float>(v.AsFloat());
  } else if constexpr (std::is_same_v<R, double>) {
    return v.AsFloat();
  } else if constexpr (std::is_same_v<R, gi64>) {
    return gi64(v.AsInt());
  } else if constexpr (std::is_same_v<R, std::uint8_t>) {
    return static_cast<std::uint8_t>(v.AsInt() != 0 ? 1 : 0);
  } else {
    return static_cast<R>(v.AsInt());
  }
}

template <class R>
Value ToValue(R v) {
  if constexpr (std::is_same_v<R, jref>) return Value::Obj(Resolve(v));
  else if constexpr (std::is_same_v<R, float> || std::is_same_v<R, double>) return Value::Float(v);
  else if constexpr (std::is_same_v<R, gi64>) return Value::Int(v.v);
  else return Value::Int(static_cast<std::int64_t>(v));
}

enum CallKind : int { kVirtual = 0, kStatic = 1, kNonvirtual = 2 };

Method* CheckedMethod(std::uint32_t mid, const char* what) {
  Method* m = MethodById(mid);
  if (!m && mid) log::Warn("[jni] {}: jmethodID invalido 0x{:x}", what, mid);
  return m;
}

template <class R>
R Finish(Method* m, ObjPtr self, Class* cls, std::vector<Value>&& args, bool virt) {
  if (!m) {
    if constexpr (std::is_void_v<R>) return;
    else return R{};
  }
  Value v = Invoke(m, std::move(self), cls, std::move(args), virt);
  return Ret<R>(v);
}

template <class R, int K>
R CallVa(gaddr, jref o, std::uint32_t mid, Varargs va) {
  Method* m = CheckedMethod(mid, "Call*Method");
  auto args = ReadArgs(m, va);
  if constexpr (K == kStatic) return Finish<R>(m, nullptr, ClassFromRef(o), std::move(args), false);
  else return Finish<R>(m, Resolve(o), nullptr, std::move(args), true);
}
template <class R, int K>
R CallV(gaddr, jref o, std::uint32_t mid, VaList ap) {
  Method* m = CheckedMethod(mid, "Call*MethodV");
  auto args = ReadArgs(m, ap);
  if constexpr (K == kStatic) return Finish<R>(m, nullptr, ClassFromRef(o), std::move(args), false);
  else return Finish<R>(m, Resolve(o), nullptr, std::move(args), true);
}
template <class R, int K>
R CallA(gaddr, jref o, std::uint32_t mid, gaddr jv) {
  Method* m = CheckedMethod(mid, "Call*MethodA");
  auto args = ReadArgsA(m, jv);
  if constexpr (K == kStatic) return Finish<R>(m, nullptr, ClassFromRef(o), std::move(args), false);
  else return Finish<R>(m, Resolve(o), nullptr, std::move(args), true);
}
template <class R>
R CallNvVa(gaddr, jref o, jref cls, std::uint32_t mid, Varargs va) {
  Method* m = CheckedMethod(mid, "CallNonvirtual*Method");
  auto args = ReadArgs(m, va);
  return Finish<R>(m, Resolve(o), ClassFromRef(cls), std::move(args), false);
}
template <class R>
R CallNvV(gaddr, jref o, jref cls, std::uint32_t mid, VaList ap) {
  Method* m = CheckedMethod(mid, "CallNonvirtual*MethodV");
  auto args = ReadArgs(m, ap);
  return Finish<R>(m, Resolve(o), ClassFromRef(cls), std::move(args), false);
}
template <class R>
R CallNvA(gaddr, jref o, jref cls, std::uint32_t mid, gaddr jv) {
  Method* m = CheckedMethod(mid, "CallNonvirtual*MethodA");
  auto args = ReadArgsA(m, jv);
  return Finish<R>(m, Resolve(o), ClassFromRef(cls), std::move(args), false);
}

// ---- Campos ----
template <class R>
R GetFieldT(gaddr, jref obj, std::uint32_t fid) {
  ObjPtr o = Resolve(obj);
  Field* f = FieldById(fid);
  if (!o || !f) return R{};
  std::shared_ptr<Value> v;
  {
    std::lock_guard lk(o->fields_mu);
    if (auto it = o->fields.find(f); it != o->fields.end()) v = it->second;
  }
  return v ? Ret<R>(*v) : R{};
}
template <class R>
void SetFieldT(gaddr, jref obj, std::uint32_t fid, R val) {
  ObjPtr o = Resolve(obj);
  Field* f = FieldById(fid);
  if (!o || !f) return;
  auto v = std::make_shared<Value>(ToValue<R>(val));
  std::lock_guard lk(o->fields_mu);
  o->fields[f] = std::move(v);
}
template <class R>
R GetStaticFieldT(gaddr, jref, std::uint32_t fid) {
  Field* f = FieldById(fid);
  if (!f) return R{};
  Value v;
  {
    std::lock_guard lk(f->mu);
    v = f->static_value;
  }
  return Ret<R>(v);
}
template <class R>
void SetStaticFieldT(gaddr, jref, std::uint32_t fid, R val) {
  Field* f = FieldById(fid);
  if (!f) return;
  std::lock_guard lk(f->mu);
  f->static_value = ToValue<R>(val);
}

// ---- Arrays primitivos ----
template <char E>
jref NewPrimArray(gaddr, std::int32_t n) {
  if (n < 0) return 0;
  return NewLocal(MakeArray(E, static_cast<std::uint32_t>(n)));
}
std::shared_ptr<Array> PrimArray(jref r) {
  auto a = As<Array>(r);
  return a && a->elem != 'L' ? a : nullptr;
}
gaddr GetArrayElems(gaddr, jref arr, GPtr<std::uint8_t> is_copy) {
  auto a = PrimArray(arr);
  if (is_copy) *is_copy = 0;
  return a ? a->data : 0;
}
void ReleaseArrayElems(gaddr, jref, gaddr, std::int32_t) {}
bool RangeOk(const Array& a, std::int32_t start, std::int32_t len) {
  return start >= 0 && len >= 0 && static_cast<std::uint64_t>(start) + static_cast<std::uint64_t>(len) <= a.length;
}
void GetArrayRegion(gaddr, jref arr, std::int32_t start, std::int32_t len, gaddr buf) {
  auto a = PrimArray(arr);
  if (!a || !RangeOk(*a, start, len)) {
    log::Warn("[jni] Get<T>ArrayRegion fuera de rango (start {}, len {}, length {})", start, len, a ? a->length : 0);
    return;
  }
  if (len) std::memcpy(g2h<void>(buf), a->host() + start * a->elem_size, static_cast<std::size_t>(len) * a->elem_size);
}
void SetArrayRegion(gaddr, jref arr, std::int32_t start, std::int32_t len, gaddr buf) {
  auto a = PrimArray(arr);
  if (!a || !RangeOk(*a, start, len)) {
    log::Warn("[jni] Set<T>ArrayRegion fuera de rango (start {}, len {}, length {})", start, len, a ? a->length : 0);
    return;
  }
  if (len) std::memcpy(a->host() + start * a->elem_size, g2h<void>(buf), static_cast<std::size_t>(len) * a->elem_size);
}

// ---------------------------------------------------------------------------------------------
// Slots sueltos
// ---------------------------------------------------------------------------------------------
std::int32_t jGetVersion(gaddr) { return 0x10006; }
jref jDefineClass(gaddr, const char* name, jref, gaddr, std::int32_t) {
  log::Warn("[jni] DefineClass({}) no soportado", name ? name : "");
  return 0;
}
jref jFindClass(gaddr, gaddr pname) {
  const std::string name = SafeCStr(pname);
  if (name.empty()) return 0;
  Class* c = GetClass(name);
  NoteCoverage(CoverageEntry::Kind::Class, c->name, "", "", c->emulated);
  return NewLocal(ClassObj(c));
}
std::uint32_t jFromReflected(gaddr, jref) { return 0; }
jref jToReflected(gaddr, jref, std::uint32_t, std::uint8_t) { return 0; }
jref jGetSuperclass(gaddr, jref c) {
  Class* k = ClassFromRef(c);
  return k && k->super ? NewLocal(ClassObj(k->super)) : 0;
}
std::uint8_t jIsAssignableFrom(gaddr, jref a, jref b) {
  Class *ka = ClassFromRef(a), *kb = ClassFromRef(b);
  return ka && kb && ka->IsSubclassOf(kb);
}
std::int32_t jThrow(gaddr, jref t) {
  ObjPtr o = Resolve(t);
  log::Warn("[jni] Throw({}) ignorado (no hay excepciones)", o && o->cls ? o->cls->name : "?");
  return 0;
}
std::int32_t jThrowNew(gaddr, jref c, gaddr msg) {
  Class* k = ClassFromRef(c);
  log::Warn("[jni] ThrowNew({}, \"{}\") ignorado (no hay excepciones)", k ? k->name : "?", SafeCStr(msg));
  return 0;
}
jref jExceptionOccurred(gaddr) { return 0; }
void jExceptionNoop(gaddr) {}
void jFatalError(gaddr, gaddr msg) {
  log::Error("[jni] FatalError: {}", SafeCStr(msg));
  throw GuestExit{134};
}
std::int32_t jPushLocalFrame(gaddr, std::int32_t) {
  PushFrame();
  return 0;
}
jref jPopLocalFrame(gaddr, jref result) {
  ObjPtr keep = Resolve(result);
  PopFrame();
  return NewLocal(keep);
}
jref jNewGlobalRef(gaddr, jref r) { return NewGlobal(Resolve(r)); }
void jDeleteGlobalRef(gaddr, jref r) {
  if ((r & 3) == static_cast<std::uint32_t>(RefKind::Global) || (r & 3) == static_cast<std::uint32_t>(RefKind::Weak)) RemoveRef(r);
}
void jDeleteLocalRef(gaddr, jref r) { DeleteLocal(r); }
std::uint8_t jIsSameObject(gaddr, jref a, jref b) { return Resolve(a) == Resolve(b); }
jref jNewLocalRef(gaddr, jref r) { return NewLocal(Resolve(r)); }
std::int32_t jEnsureLocalCapacity(gaddr, std::int32_t) { return 0; }
jref jAllocObject(gaddr, jref c) { return NewLocal(AllocInstance(ClassFromRef(c))); }

jref NewObjectImpl(jref c, Method* m, std::vector<Value>&& args) {
  Class* k = ClassFromRef(c);
  if (!k) return 0;
  ObjPtr o = AllocInstance(k);
  if (m) Invoke(m, o, k, std::move(args), false);
  return NewLocal(o);
}
jref jNewObject(gaddr, jref c, std::uint32_t mid, Varargs va) {
  Method* m = CheckedMethod(mid, "NewObject");
  return NewObjectImpl(c, m, ReadArgs(m, va));
}
jref jNewObjectV(gaddr, jref c, std::uint32_t mid, VaList ap) {
  Method* m = CheckedMethod(mid, "NewObjectV");
  return NewObjectImpl(c, m, ReadArgs(m, ap));
}
jref jNewObjectA(gaddr, jref c, std::uint32_t mid, gaddr jv) {
  Method* m = CheckedMethod(mid, "NewObjectA");
  return NewObjectImpl(c, m, ReadArgsA(m, jv));
}
jref jGetObjectClass(gaddr, jref o) {
  ObjPtr p = Resolve(o);
  return p && p->cls ? NewLocal(ClassObj(p->cls)) : 0;
}
std::uint8_t jIsInstanceOf(gaddr, jref o, jref c) {
  ObjPtr p = Resolve(o);
  Class* k = ClassFromRef(c);
  if (!p) return 1;
  return k && p->cls && p->cls->IsSubclassOf(k);
}

std::uint32_t GetMethodIdImpl(jref c, gaddr pname, gaddr psig, bool is_static) {
  Class* k = ClassFromRef(c);
  const std::string name = SafeCStr(pname), sig = SafeCStr(psig);
  if (!k) {
    log::Warn("[jni] Get{}MethodID({} {}) con jclass nulo/invalido (0x{:x})", is_static ? "Static" : "", name, sig, c);
    return 0;
  }
  bool covered = false;
  Method* m = GetOrAddMethod(k, name, sig, is_static, &covered);
  NoteCoverage(is_static ? CoverageEntry::Kind::StaticMethod : CoverageEntry::Kind::Method, k->name, name, sig, covered);
  return m->id;
}
std::uint32_t jGetMethodID(gaddr, jref c, gaddr n, gaddr s) { return GetMethodIdImpl(c, n, s, false); }
std::uint32_t jGetStaticMethodID(gaddr, jref c, gaddr n, gaddr s) { return GetMethodIdImpl(c, n, s, true); }

std::uint32_t GetFieldIdImpl(jref c, gaddr pname, gaddr psig, bool is_static) {
  Class* k = ClassFromRef(c);
  const std::string name = SafeCStr(pname), sig = SafeCStr(psig);
  if (!k) {
    log::Warn("[jni] Get{}FieldID({} {}) con jclass nulo/invalido", is_static ? "Static" : "", name, sig);
    return 0;
  }
  bool covered = false;
  Field* f = GetOrAddField(k, name, sig, is_static, &covered);
  NoteCoverage(is_static ? CoverageEntry::Kind::StaticField : CoverageEntry::Kind::Field, k->name, name, sig, covered);
  return f->id;
}
std::uint32_t jGetFieldID(gaddr, jref c, gaddr n, gaddr s) { return GetFieldIdImpl(c, n, s, false); }
std::uint32_t jGetStaticFieldID(gaddr, jref c, gaddr n, gaddr s) { return GetFieldIdImpl(c, n, s, true); }

// ---- Strings ----
std::u16string ToU16(std::string_view s) {
  std::u16string o;
  for (std::size_t i = 0; i < s.size();) {
    std::uint32_t c = static_cast<std::uint8_t>(s[i]);
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c >> 5) == 6) { c &= 0x1F; n = 1; }
    else if ((c >> 4) == 14) { c &= 0x0F; n = 2; }
    else if ((c >> 3) == 30) { c &= 0x07; n = 3; }
    else { c = 0xFFFD; n = 0; }
    ++i;
    for (int k = 0; k < n && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<std::uint8_t>(s[i]) & 0x3F);
    if (c >= 0x10000) {
      c -= 0x10000;
      o += static_cast<char16_t>(0xD800 + (c >> 10));
      o += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
    } else {
      o += static_cast<char16_t>(c);
    }
  }
  return o;
}
std::string ToU8(const char16_t* p, std::size_t n) {
  std::string o;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t c = p[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (p[i + 1] - 0xDC00);
      ++i;
    }
    if (c < 0x80) o += static_cast<char>(c);
    else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
      o += static_cast<char>(0xE0 | (c >> 12));
      o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      o += static_cast<char>(0xF0 | (c >> 18));
      o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return o;
}
jref jNewString(gaddr, gaddr chars, std::int32_t len) {
  if (len < 0 || (len && !chars)) return 0;
  return NewLocal(MakeString(ToU8(g2h<char16_t>(chars), static_cast<std::size_t>(len))));
}
std::int32_t jGetStringLength(gaddr, jref s) {
  auto o = As<String>(s);
  return o ? static_cast<std::int32_t>(ToU16(o->utf8).size()) : 0;
}
gaddr jGetStringChars(gaddr, jref s, GPtr<std::uint8_t> is_copy) {
  auto o = As<String>(s);
  if (!o) return 0;
  const std::u16string u = ToU16(o->utf8);
  if (is_copy) *is_copy = 1;
  const gaddr p = GuestMemory::Calloc(static_cast<gsize>(u.size() + 1), 2);
  if (p && !u.empty()) std::memcpy(g2h<void>(p), u.data(), u.size() * 2);
  return p;
}
void jReleaseStringChars(gaddr, jref, gaddr chars) { GuestMemory::Free(chars); }
jref jNewStringUTF(gaddr, gaddr p) {
  if (!p) return 0;
  return NewLocal(MakeString(mem::ReadCStr(p)));
}
std::int32_t jGetStringUTFLength(gaddr, jref s) {
  auto o = As<String>(s);
  return o ? static_cast<std::int32_t>(o->utf8.size()) : 0;
}
gaddr jGetStringUTFChars(gaddr, jref s, GPtr<std::uint8_t> is_copy) {
  auto o = As<String>(s);
  if (!o) return 0;
  if (is_copy) *is_copy = 1;
  return mem::StrDup(o->utf8);
}
void jReleaseStringUTFChars(gaddr, jref, gaddr chars) { GuestMemory::Free(chars); }
void jGetStringRegion(gaddr, jref s, std::int32_t start, std::int32_t len, gaddr buf) {
  auto o = As<String>(s);
  if (!o) return;
  const std::u16string u = ToU16(o->utf8);
  if (start < 0 || len < 0 || static_cast<std::size_t>(start) + len > u.size()) {
    log::Warn("[jni] GetStringRegion fuera de rango");
    return;
  }
  if (len) std::memcpy(g2h<void>(buf), u.data() + start, static_cast<std::size_t>(len) * 2);
}
void jGetStringUTFRegion(gaddr, jref s, std::int32_t start, std::int32_t len, gaddr buf) {
  auto o = As<String>(s);
  if (!o) return;
  const std::u16string u = ToU16(o->utf8);
  if (start < 0 || len < 0 || static_cast<std::size_t>(start) + len > u.size()) {
    log::Warn("[jni] GetStringUTFRegion fuera de rango");
    return;
  }
  const std::string r = ToU8(u.data() + start, static_cast<std::size_t>(len));
  std::memcpy(g2h<void>(buf), r.c_str(), r.size() + 1);
}

// ---- Arrays genericos ----
std::int32_t jGetArrayLength(gaddr, jref a) {
  auto o = As<Array>(a);
  return o ? static_cast<std::int32_t>(o->length) : 0;
}
jref jNewObjectArray(gaddr, std::int32_t n, jref elem_cls, jref init) {
  if (n < 0) return 0;
  auto a = MakeArray('L', static_cast<std::uint32_t>(n));
  if (Class* k = ClassFromRef(elem_cls)) a->cls = GetClass("[L" + k->name + ";");
  if (ObjPtr i = Resolve(init))
    for (auto& e : a->objects) e = i;
  return NewLocal(a);
}
jref jGetObjectArrayElement(gaddr, jref a, std::int32_t i) {
  auto o = As<Array>(a);
  if (!o || o->elem != 'L' || i < 0 || static_cast<std::uint32_t>(i) >= o->length) return 0;
  return NewLocal(o->objects[i]);
}
void jSetObjectArrayElement(gaddr, jref a, std::int32_t i, jref v) {
  auto o = As<Array>(a);
  if (!o || o->elem != 'L' || i < 0 || static_cast<std::uint32_t>(i) >= o->length) return;
  o->objects[i] = Resolve(v);
}

// ---- Otros ----
struct GuestNativeMethod {
  std::uint32_t name, sig, fn;
};
std::int32_t jRegisterNatives(gaddr, jref c, gaddr methods, std::int32_t n) {
  Class* k = ClassFromRef(c);
  if (!k || n < 0) return -1;
  for (std::int32_t i = 0; i < n; ++i) {
    const auto m = mem::Read<GuestNativeMethod>(methods + 12 * static_cast<gaddr>(i));
    const std::string name = SafeCStr(m.name), sig = SafeCStr(m.sig);
    RegisterNativeMethod(k, name, sig, m.fn);
    log::Info("[jni] RegisterNatives {}.{}{} -> 0x{:08x}", k->name, name, sig, m.fn);
  }
  return 0;
}
std::int32_t jUnregisterNatives(gaddr, jref c) {
  if (Class* k = ClassFromRef(c)) UnregisterNatives(k);
  return 0;
}
std::int32_t jMonitor(gaddr, jref) { return 0; }
std::int32_t jGetJavaVM(gaddr, GPtr<std::uint32_t> out) {
  if (out) *out = g_vm;
  return 0;
}
gaddr jGetCritical(gaddr, jref arr, GPtr<std::uint8_t> is_copy) { return GetArrayElems(0, arr, is_copy); }
void jReleaseCritical(gaddr, jref, gaddr, std::int32_t) {}
jref jNewWeakGlobalRef(gaddr, jref r) { return AddRef(Resolve(r), RefKind::Weak); }
std::uint8_t jExceptionCheck(gaddr) { return 0; }

struct DirectBuffer : Object {
  gaddr addr;
  std::int64_t cap;
  DirectBuffer(Class* c, gaddr a, std::int64_t n) : Object(c), addr(a), cap(n) {}
};
jref jNewDirectByteBuffer(gaddr, gaddr addr, gi64 cap) {
  return NewLocal(std::make_shared<DirectBuffer>(GetClass("java/nio/DirectByteBuffer"), addr, cap.v));
}
gaddr jGetDirectBufferAddress(gaddr, jref b) {
  auto o = As<DirectBuffer>(b);
  return o ? o->addr : 0;
}
gi64 jGetDirectBufferCapacity(gaddr, jref b) {
  auto o = As<DirectBuffer>(b);
  return gi64(o ? o->cap : -1);
}
std::int32_t jGetObjectRefType(gaddr, jref r) { return Resolve(r) ? static_cast<std::int32_t>(r & 3) : 0; }

// ---- JavaVM ----
std::int32_t vmDestroy(gaddr) { return 0; }
std::int32_t vmAttach(gaddr, GPtr<std::uint32_t> penv, gaddr) {
  if (penv) *penv = TS().env;
  return 0;
}
std::int32_t vmDetach(gaddr) {
  ClearBaseFrame();
  return 0;
}
std::int32_t vmGetEnv(gaddr, GPtr<std::uint32_t> penv, std::int32_t version) {
  if (version > 0x10006) log::Warn("[jni] JavaVM::GetEnv con version desconocida 0x{:x}", static_cast<std::uint32_t>(version));
  if (penv) *penv = TS().env;
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Registro
// ---------------------------------------------------------------------------------------------
template <auto Fn>
void Slot(gaddr table, int slot, const char* name) {
  const std::string n = std::string("JNIEnv::") + name;
  hle::Register(n, &hle::detail::Thunk<Fn>::Call, hle::kDefault);
  mem::W32(table + 4 * slot, hle::StubFor(n));
}

template <class R, int T>
void TypeSlots(gaddr t, const char* tn) {
  const std::string s(tn);
  auto N = [&](const char* pre, const char* post) {
    static thread_local std::string buf;
    buf = std::string(pre) + s + post;
    return buf.c_str();
  };
  Slot<&CallVa<R, kVirtual>>(t, 34 + 3 * T, N("Call", "Method"));
  Slot<&CallV<R, kVirtual>>(t, 35 + 3 * T, N("Call", "MethodV"));
  Slot<&CallA<R, kVirtual>>(t, 36 + 3 * T, N("Call", "MethodA"));
  Slot<&CallNvVa<R>>(t, 64 + 3 * T, N("CallNonvirtual", "Method"));
  Slot<&CallNvV<R>>(t, 65 + 3 * T, N("CallNonvirtual", "MethodV"));
  Slot<&CallNvA<R>>(t, 66 + 3 * T, N("CallNonvirtual", "MethodA"));
  Slot<&CallVa<R, kStatic>>(t, 114 + 3 * T, N("CallStatic", "Method"));
  Slot<&CallV<R, kStatic>>(t, 115 + 3 * T, N("CallStatic", "MethodV"));
  Slot<&CallA<R, kStatic>>(t, 116 + 3 * T, N("CallStatic", "MethodA"));
  if constexpr (!std::is_void_v<R>) {
    Slot<&GetFieldT<R>>(t, 95 + T, N("Get", "Field"));
    Slot<&SetFieldT<R>>(t, 104 + T, N("Set", "Field"));
    Slot<&GetStaticFieldT<R>>(t, 145 + T, N("GetStatic", "Field"));
    Slot<&SetStaticFieldT<R>>(t, 154 + T, N("SetStatic", "Field"));
  }
}

template <char E, int P>
void ArraySlots(gaddr t, const char* tn) {
  const std::string s(tn);
  auto N = [&](const char* pre, const char* post) {
    static thread_local std::string buf;
    buf = std::string(pre) + s + post;
    return buf.c_str();
  };
  Slot<&NewPrimArray<E>>(t, 175 + P, N("New", "Array"));
  // Los slots Get/Release/Region son identicos para todos los tipos: un stub por nombre igualmente,
  // para que las trazas digan que tipo pidio el guest.
  Slot<&GetArrayElems>(t, 183 + P, N("Get", "ArrayElements"));
  Slot<&ReleaseArrayElems>(t, 191 + P, N("Release", "ArrayElements"));
  Slot<&GetArrayRegion>(t, 199 + P, N("Get", "ArrayRegion"));
  Slot<&SetArrayRegion>(t, 207 + P, N("Set", "ArrayRegion"));
}

}  // namespace

gaddr EnvTable() { return g_env_table; }

gaddr BuildTables() {
  if (g_vm) return g_vm;
  const gaddr t = GuestMemory::Calloc(kJniSlots + 3, 4);  // slots 0..3 reservados = NULL
  Slot<&jGetVersion>(t, 4, "GetVersion");
  Slot<&jDefineClass>(t, 5, "DefineClass");
  Slot<&jFindClass>(t, 6, "FindClass");
  Slot<&jFromReflected>(t, 7, "FromReflectedMethod");
  Slot<&jFromReflected>(t, 8, "FromReflectedField");
  Slot<&jToReflected>(t, 9, "ToReflectedMethod");
  Slot<&jGetSuperclass>(t, 10, "GetSuperclass");
  Slot<&jIsAssignableFrom>(t, 11, "IsAssignableFrom");
  Slot<&jToReflected>(t, 12, "ToReflectedField");
  Slot<&jThrow>(t, 13, "Throw");
  Slot<&jThrowNew>(t, 14, "ThrowNew");
  Slot<&jExceptionOccurred>(t, 15, "ExceptionOccurred");
  Slot<&jExceptionNoop>(t, 16, "ExceptionDescribe");
  Slot<&jExceptionNoop>(t, 17, "ExceptionClear");
  Slot<&jFatalError>(t, 18, "FatalError");
  Slot<&jPushLocalFrame>(t, 19, "PushLocalFrame");
  Slot<&jPopLocalFrame>(t, 20, "PopLocalFrame");
  Slot<&jNewGlobalRef>(t, 21, "NewGlobalRef");
  Slot<&jDeleteGlobalRef>(t, 22, "DeleteGlobalRef");
  Slot<&jDeleteLocalRef>(t, 23, "DeleteLocalRef");
  Slot<&jIsSameObject>(t, 24, "IsSameObject");
  Slot<&jNewLocalRef>(t, 25, "NewLocalRef");
  Slot<&jEnsureLocalCapacity>(t, 26, "EnsureLocalCapacity");
  Slot<&jAllocObject>(t, 27, "AllocObject");
  Slot<&jNewObject>(t, 28, "NewObject");
  Slot<&jNewObjectV>(t, 29, "NewObjectV");
  Slot<&jNewObjectA>(t, 30, "NewObjectA");
  Slot<&jGetObjectClass>(t, 31, "GetObjectClass");
  Slot<&jIsInstanceOf>(t, 32, "IsInstanceOf");
  Slot<&jGetMethodID>(t, 33, "GetMethodID");
  TypeSlots<jref, 0>(t, "Object");
  TypeSlots<std::uint8_t, 1>(t, "Boolean");
  TypeSlots<std::int8_t, 2>(t, "Byte");
  TypeSlots<std::uint16_t, 3>(t, "Char");
  TypeSlots<std::int16_t, 4>(t, "Short");
  TypeSlots<std::int32_t, 5>(t, "Int");
  TypeSlots<gi64, 6>(t, "Long");
  TypeSlots<float, 7>(t, "Float");
  TypeSlots<double, 8>(t, "Double");
  TypeSlots<void, 9>(t, "Void");
  Slot<&jGetFieldID>(t, 94, "GetFieldID");
  Slot<&jGetStaticMethodID>(t, 113, "GetStaticMethodID");
  Slot<&jGetStaticFieldID>(t, 144, "GetStaticFieldID");
  Slot<&jNewString>(t, 163, "NewString");
  Slot<&jGetStringLength>(t, 164, "GetStringLength");
  Slot<&jGetStringChars>(t, 165, "GetStringChars");
  Slot<&jReleaseStringChars>(t, 166, "ReleaseStringChars");
  Slot<&jNewStringUTF>(t, 167, "NewStringUTF");
  Slot<&jGetStringUTFLength>(t, 168, "GetStringUTFLength");
  Slot<&jGetStringUTFChars>(t, 169, "GetStringUTFChars");
  Slot<&jReleaseStringUTFChars>(t, 170, "ReleaseStringUTFChars");
  Slot<&jGetArrayLength>(t, 171, "GetArrayLength");
  Slot<&jNewObjectArray>(t, 172, "NewObjectArray");
  Slot<&jGetObjectArrayElement>(t, 173, "GetObjectArrayElement");
  Slot<&jSetObjectArrayElement>(t, 174, "SetObjectArrayElement");
  ArraySlots<'Z', 0>(t, "Boolean");
  ArraySlots<'B', 1>(t, "Byte");
  ArraySlots<'C', 2>(t, "Char");
  ArraySlots<'S', 3>(t, "Short");
  ArraySlots<'I', 4>(t, "Int");
  ArraySlots<'J', 5>(t, "Long");
  ArraySlots<'F', 6>(t, "Float");
  ArraySlots<'D', 7>(t, "Double");
  Slot<&jRegisterNatives>(t, 215, "RegisterNatives");
  Slot<&jUnregisterNatives>(t, 216, "UnregisterNatives");
  Slot<&jMonitor>(t, 217, "MonitorEnter");
  Slot<&jMonitor>(t, 218, "MonitorExit");
  Slot<&jGetJavaVM>(t, 219, "GetJavaVM");
  Slot<&jGetStringRegion>(t, 220, "GetStringRegion");
  Slot<&jGetStringUTFRegion>(t, 221, "GetStringUTFRegion");
  Slot<&jGetCritical>(t, 222, "GetPrimitiveArrayCritical");
  Slot<&jReleaseCritical>(t, 223, "ReleasePrimitiveArrayCritical");
  Slot<&jGetStringChars>(t, 224, "GetStringCritical");
  Slot<&jReleaseStringChars>(t, 225, "ReleaseStringCritical");
  Slot<&jNewWeakGlobalRef>(t, 226, "NewWeakGlobalRef");
  Slot<&jDeleteGlobalRef>(t, 227, "DeleteWeakGlobalRef");
  Slot<&jExceptionCheck>(t, 228, "ExceptionCheck");
  Slot<&jNewDirectByteBuffer>(t, 229, "NewDirectByteBuffer");
  Slot<&jGetDirectBufferAddress>(t, 230, "GetDirectBufferAddress");
  Slot<&jGetDirectBufferCapacity>(t, 231, "GetDirectBufferCapacity");
  Slot<&jGetObjectRefType>(t, 232, "GetObjectRefType");
  // Ningun slot 4..232 puede quedar a 0.
  for (int i = 4; i < kJniSlots; ++i)
    if (!mem::R32(t + 4 * i)) {
      const std::string n = "JNIEnv::slot_" + std::to_string(i);
      mem::W32(t + 4 * i, hle::StubFor(n));
      log::Warn("[jni] slot {} sin handler", i);
    }
  g_env_table = t;

  const gaddr vt = GuestMemory::Calloc(8, 4);
  mem::W32(vt + 4 * 3, ZW_HLE_THUNK("JavaVM::DestroyJavaVM", vmDestroy));
  mem::W32(vt + 4 * 4, ZW_HLE_THUNK("JavaVM::AttachCurrentThread", vmAttach));
  mem::W32(vt + 4 * 5, ZW_HLE_THUNK("JavaVM::DetachCurrentThread", vmDetach));
  mem::W32(vt + 4 * 6, ZW_HLE_THUNK("JavaVM::GetEnv", vmGetEnv));
  mem::W32(vt + 4 * 7, ZW_HLE_THUNK("JavaVM::AttachCurrentThreadAsDaemon", vmAttach));
  g_vm = GuestMemory::Alloc(4);
  mem::W32(g_vm, vt);
  return g_vm;
}

gaddr JavaVMAddr() { return g_vm; }

}  // namespace zw::jni
