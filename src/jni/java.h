// Modelo de objetos de la capa Java emulada (T7). Lo usan las clases emuladas (classes_*.cpp) y
// cualquier modulo que quiera anadir o sustituir metodos (jni::DefineClass(...).Static(...)).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jni/jni.h"

namespace zw::jni {

struct Class;
struct Method;
struct Field;
struct Value;

// Objeto Java. Las subclases C++ (String, Array, Bundle, AudioTrack...) llevan su estado.
struct Object : std::enable_shared_from_this<Object> {
  Class* cls = nullptr;
  explicit Object(Class* c) : cls(c) {}
  virtual ~Object() = default;

  // Campos de instancia (GetFieldID/Get/Set<T>Field).
  std::mutex fields_mu;
  std::unordered_map<const Field*, std::shared_ptr<Value>> fields;
};
using ObjPtr = std::shared_ptr<Object>;

// Valor Java (argumento, retorno o campo).
struct Value {
  enum class K : std::uint8_t { None, Int, Float, Obj } k = K::None;
  std::int64_t i = 0;
  double d = 0;
  ObjPtr o;

  Value() = default;
  static Value Int(std::int64_t v) { Value r; r.k = K::Int; r.i = v; return r; }
  static Value Bool(bool v) { return Int(v ? 1 : 0); }
  static Value Float(double v) { Value r; r.k = K::Float; r.d = v; return r; }
  static Value Obj(ObjPtr p) { Value r; r.k = K::Obj; r.o = std::move(p); return r; }
  static Value Null() { Value r; r.k = K::Obj; return r; }
  static Value Str(std::string_view s);  // nuevo java/lang/String

  std::int64_t AsInt() const { return k == K::Float ? static_cast<std::int64_t>(d) : i; }
  double AsFloat() const { return k == K::Float ? d : static_cast<double>(i); }
};

struct String : Object {
  std::string utf8;
  String(Class* c, std::string s) : Object(c), utf8(std::move(s)) {}
};

// Array Java. Los primitivos viven en memoria guest (Get*ArrayElements/Critical devuelven el puntero
// directo, sin copia); los de objetos en un vector host.
struct Array : Object {
  char elem;  // 'Z','B','C','S','I','J','F','D' o 'L'
  std::uint32_t length = 0;
  std::uint32_t elem_size = 0;
  gaddr data = 0;               // primitivos
  std::vector<ObjPtr> objects;  // 'L'
  Array(Class* c, char e, std::uint32_t n);
  ~Array() override;
  std::uint8_t* host() const { return data ? g2h<std::uint8_t>(data) : nullptr; }
};

// android/os/Bundle: mapa clave -> valor.
struct Bundle : Object {
  using Object::Object;
  std::mutex mu;
  std::map<std::string, Value> m;
  Value Get(const std::string& k) {
    std::lock_guard lk(mu);
    auto it = m.find(k);
    return it == m.end() ? Value() : it->second;
  }
  void Put(const std::string& k, Value v) {
    std::lock_guard lk(mu);
    m[k] = std::move(v);
  }
};

// Estado de una llamada a un metodo emulado.
struct Call {
  const Method* method = nullptr;
  ObjPtr self;  // null en metodos estaticos
  Class* cls = nullptr;
  std::vector<Value> args;

  std::int32_t I(std::size_t n) const { return n < args.size() ? static_cast<std::int32_t>(args[n].AsInt()) : 0; }
  std::int64_t J(std::size_t n) const { return n < args.size() ? args[n].AsInt() : 0; }
  bool Z(std::size_t n) const { return I(n) != 0; }
  double F(std::size_t n) const { return n < args.size() ? args[n].AsFloat() : 0.0; }
  ObjPtr O(std::size_t n) const { return n < args.size() ? args[n].o : nullptr; }
  // Contenido de un argumento String ("" si es null).
  std::string S(std::size_t n) const;
  bool IsNull(std::size_t n) const { return !O(n); }
  template <class T>
  std::shared_ptr<T> SelfAs() const { return std::dynamic_pointer_cast<T>(self); }
};
using Impl = std::function<Value(Call&)>;

struct Method {
  Class* cls = nullptr;
  std::string name, sig;
  bool is_static = false;
  Impl impl;              // vacio => no implementado (log una vez, retorno por defecto)
  std::vector<char> params;  // tipos de parametro ('I','J','F','D','L', ...; '[' se guarda como 'L')
  char ret = 'V';
  gaddr native = 0;       // RegisterNatives (no se llama desde la capa Java; solo para FindNative)
  std::uint32_t id = 0;   // jmethodID
  std::atomic<std::uint64_t> calls{0};
  std::atomic<bool> warned{false};
};

struct Field {
  Class* cls = nullptr;
  std::string name, sig;
  bool is_static = false;
  std::uint32_t id = 0;  // jfieldID
  bool defined = false;  // declarado por la emulacion (cobertura)
  std::mutex mu;
  Value static_value;  // solo estaticos (los de instancia viven en Object::fields)
};

struct Class : Object {
  std::string name;  // con barras: "java/lang/String"
  Class* super = nullptr;
  bool emulated = false;  // definida por la emulacion (para cobertura)
  std::function<ObjPtr(Class*)> factory;  // AllocObject/NewObject; por defecto Object generico
  std::mutex mu;
  std::map<std::string, std::unique_ptr<Method>> methods;  // clave nombre+sig (+ "s" estatico)
  std::map<std::string, std::unique_ptr<Field>> fields;

  Class(Class* meta, std::string n) : Object(meta), name(std::move(n)) {}
  bool IsSubclassOf(const Class* other) const;
};

// ---------------------------------------------------------------------------------------------
// Definicion de clases emuladas (DSL)
// ---------------------------------------------------------------------------------------------
class ClassDef {
 public:
  explicit ClassDef(Class* c) : c_(c) {}
  // Metodo estatico/de instancia. `sig` = firma JNI "(II)V". Redefinir sustituye la implementacion.
  ClassDef& Static(std::string_view name, std::string_view sig, Impl impl);
  ClassDef& Virtual(std::string_view name, std::string_view sig, Impl impl);
  // Constructor: equivale a Virtual("<init>", sig, impl).
  ClassDef& Ctor(std::string_view sig, Impl impl) { return Virtual("<init>", sig, std::move(impl)); }
  // Varios metodos estaticos con el mismo comportamiento.
  ClassDef& StaticNoop(std::initializer_list<std::pair<const char*, const char*>> name_sig);
  ClassDef& StaticField(std::string_view name, std::string_view sig, Value v);
  ClassDef& InstanceField(std::string_view name, std::string_view sig);
  ClassDef& Super(std::string_view super_name);
  ClassDef& Factory(std::function<ObjPtr(Class*)> f);
  Class* cls() const { return c_; }

 private:
  Class* c_;
};
// Crea (o devuelve) la clase y la marca como emulada.
ClassDef DefineClass(std::string_view name);
// Busca la clase; si no existe la crea vacia (no emulada) con super java/lang/Object.
Class* GetClass(std::string_view name);

// Valor por defecto de un tipo de retorno ('V','I','L'...): 0/null.
Value DefaultFor(char ret);

// Referencias <-> objetos
ObjPtr Resolve(jref r);                  // null si r == 0 o es invalida
jref NewLocal(const ObjPtr& o);          // 0 si o es null
jref NewGlobal(const ObjPtr& o);
std::shared_ptr<String> MakeString(std::string_view utf8);
std::shared_ptr<Array> MakeArray(char elem, std::uint32_t n);
std::shared_ptr<Object> NewBundle();

// Ejecuta un metodo emulado (dispatch virtual si `virt` y self no es null).
Value Invoke(Method* m, ObjPtr self, Class* cls, std::vector<Value>&& args, bool virt);

// Registro de las clases emuladas (classes_*.cpp).
void RegisterAndroidClasses();
void RegisterGameClasses();
void RegisterAudioTrack();
void RegisterSocialAndMisc();

// Cobertura interna
void NoteCoverage(CoverageEntry::Kind k, const std::string& cls, const std::string& name, const std::string& sig, bool covered);

}  // namespace zw::jni
