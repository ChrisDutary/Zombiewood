// Tests de zw_jni (T7): una .so ARM real (tests/jni/jnitest.c) usa el JNIEnv/JavaVM falsos por slot,
// igual que libZombieWood.so: FindClass/Get*ID, Call* variadico / V (va_list) / A (jvalue[]), retornos
// float/double/long (softfp), strings UTF-8/UTF-16, arrays, referencias y marcos locales, Bundle,
// campos, NewObject, CallNonvirtual*, RegisterNatives, AudioTrack, hilos guest con Attach/Detach,
// call_native reentrante y la capa de clases del juego (Game.*).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "jni/internal.h"
#include "jni/jni.h"
#include "jnitest_blob.inc"
#include "loader/load_only.h"
#include "loader/loader.h"

using namespace zw;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    ++g_checks;                                                      \
    if (!(cond)) {                                                   \
      ++g_failed;                                                    \
      std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)
#define CHECK_EQ(a, b)                                                                                        \
  do {                                                                                                        \
    ++g_checks;                                                                                               \
    const auto _a = (a);                                                                                      \
    const auto _b = (b);                                                                                      \
    if (!(_a == _b)) {                                                                                        \
      ++g_failed;                                                                                             \
      std::printf("  FALLO %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a, #b, std::to_string(_a).c_str(), \
                  std::to_string(_b).c_str());                                                                \
    }                                                                                                         \
  } while (0)

namespace {
// Backend de audio de prueba: captura lo escrito.
struct CaptureTrack final : jni::AudioTrackBackend {
  static inline std::vector<std::uint8_t> data;
  static inline int opened_rate = 0, opened_cfg = 0, plays = 0, releases = 0;
  int state = 1;
  void Play() override { state = 3, ++plays; }
  void Pause() override { state = 2; }
  void Stop() override { state = 1; }
  void Release() override { ++releases; }
  int Write(const void* p, int n) override {
    data.insert(data.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
    return n;
  }
  int PlayState() const override { return state; }
};

void DefineTestClass() {
  using namespace jni;
  DefineClass("zw/Test")
      .InstanceField("count", "I")
      .InstanceField("ratio", "F")
      .Static("hello", "()Ljava/lang/String;", [](Call&) { return Value::Str("hola jni"); })
      .Static("mix", "(IJFDZLjava/lang/String;)D",
              [](Call& c) {
                return Value::Float(c.I(0) + static_cast<double>(c.J(1) >> 40) * 10 + c.F(2) * 100 + c.F(3) * 1000 +
                                    (c.Z(4) ? 10000 : 0) + static_cast<double>(c.S(5).size()) * 100000);
              })
      .Static("half", "(F)F", [](Call& c) { return Value::Float(c.F(0) / 2); })
      .Static("big", "(I)J", [](Call& c) { return Value::Int((static_cast<std::int64_t>(c.I(0)) << 32) | 7); })
      .Static("cb", "(I)I",
              [](Call& c) {
                // reentrancia: Java falso -> nativo -> Java falso -> nativo
                return Value::Int(call_native<std::int32_t>("Java_zw_Test_nativeLeaf", c.I(0)) * 10);
              })
      .Ctor("(I)V",
            [](Call& c) {
              bool cov;
              Field* f = GetOrAddField(c.cls, "count", "I", false, &cov);
              std::lock_guard lk(c.self->fields_mu);
              c.self->fields[f] = std::make_shared<Value>(Value::Int(c.I(0)));
              return Value();
            })
      .Virtual("getCount", "()I", [](Call& c) {
        bool cov;
        Field* f = GetOrAddField(c.self->cls, "count", "I", false, &cov);
        std::lock_guard lk(c.self->fields_mu);
        auto it = c.self->fields.find(f);
        return it == c.self->fields.end() ? Value::Int(-1) : *it->second;
      });
}

gaddr Sym(const char* n) {
  const gaddr a = loader::FindSymbol(n);
  if (!a) std::printf("  FALLO: simbolo %s no encontrado\n", n);
  return a;
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  log::Init();
  GuestMemory::Init();
  jni::Register();
  jni::Register();  // idempotente
  loader::bootstrap::Register();
  DefineTestClass();
  jni::SetAudioTrackBackend([](int, int rate, int cfg, int, int, int) -> std::unique_ptr<jni::AudioTrackBackend> {
    CaptureTrack::opened_rate = rate;
    CaptureTrack::opened_cfg = cfg;
    return std::make_unique<CaptureTrack>();
  });

  try {
    // ---- tablas ----
    const gaddr vm = jni::JavaVM();
    const gaddr env = jni::Env();
    CHECK(vm && env);
    const gaddr table = mem::R32(env);
    CHECK_EQ(mem::R32(table + 0), 0u);  // reservados = NULL
    bool all_slots = true;
    for (int i = 4; i < 233; ++i)
      if (!mem::R32(table + 4 * i)) all_slots = false;
    CHECK(all_slots);
    CHECK(hle::StubName(mem::R32(table + 4 * 6)) == "JNIEnv::FindClass");
    CHECK(hle::StubName(mem::R32(table + 4 * 129)) == "JNIEnv::CallStaticIntMethod");
    CHECK(hle::StubName(mem::R32(table + 4 * 0x238 / 4)) == "JNIEnv::CallStaticVoidMethodV");
    CHECK(hle::StubName(mem::R32(table + 4 * 0x1c8 / 4)) == "JNIEnv::CallStaticObjectMethod");
    CHECK(hle::StubName(mem::R32(table + 4 * 232)) == "JNIEnv::GetObjectRefType");
    const gaddr vt = mem::R32(vm);
    CHECK(hle::StubName(mem::R32(vt + 0x10)) == "JavaVM::AttachCurrentThread");
    CHECK(hle::StubName(mem::R32(vt + 0x18)) == "JavaVM::GetEnv");

    // ---- carga + JNI_OnLoad ----
    loader::Module* m = loader::LoadFromMemory("libjnitest.so", kJniTestLib, sizeof kJniTestLib);
    CHECK(m != nullptr);
    if (!m) return 1;
    loader::RunInit(*m);
    auto ver = loader::CallJniOnLoad(*m, vm);
    CHECK(ver && *ver == 0x10004);
    CHECK(std::string(call_guest<const char*>(Sym("get_hello"))) == "hola jni");
    CHECK_EQ(call_guest<gaddr>(Sym("get_env_of_thread")), env);

    // ---- Call*: variadica, V, A ----
    const double expect = 7 + 10 + 150 + 2250 + 10000 + 300000;
    CHECK_EQ(call_guest<double>(Sym("t_call_va"), env), expect);
    CHECK_EQ(call_guest<double>(Sym("t_call_v"), env), expect);
    CHECK_EQ(call_guest<double>(Sym("t_call_a"), env), expect);
    CHECK_EQ(call_guest<float>(Sym("t_call_float"), env, 3.5f), 1.75f);
    CHECK_EQ(call_guest<std::int64_t>(Sym("t_call_long"), env, 3), (std::int64_t{3} << 32) | 7);
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_missing"), env), 1000);

    // ---- strings, arrays, refs, objetos ----
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_strings"), env), 31);
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_arrays"), env), 31);
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_refs"), env), 127);
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_objects"), env), 127);

    // ---- call_native + reentrancia + RegisterNatives ----
    CHECK_EQ(jni::call_native<std::int32_t>("Java_zw_Test_nativeSum", 5, "hola"), 5 + 4 + 60);
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_register"), env), 0);
    CHECK(jni::FindNative("zw/Test.twice") != 0);
    CHECK_EQ(jni::call_native<std::int32_t>("zw/Test.twice", 21), 42);
    CHECK(jni::FindNative("Game_nativeInit") == 0);  // no es el .so del juego
    bool threw = false;
    try {
      jni::call_native("Game_nativeInit");
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);
    // los marcos locales de call_native no dejan referencias vivas
    const std::size_t refs0 = jni::LiveRefs();
    for (int i = 0; i < 200; ++i) jni::call_native<std::int32_t>("Java_zw_Test_nativeSum", i, std::string("abc"));
    CHECK_EQ(jni::LiveRefs(), refs0);

    // ---- AudioTrack ----
    CHECK_EQ(call_guest<std::int32_t>(Sym("t_audio"), env), 4096);
    CHECK_EQ(CaptureTrack::opened_rate, 44100);
    CHECK_EQ(CaptureTrack::opened_cfg, 12);
    CHECK_EQ(CaptureTrack::plays, 1);
    CHECK_EQ(CaptureTrack::releases, 1);
    CHECK_EQ(CaptureTrack::data.size(), std::size_t{4096});
    CHECK(CaptureTrack::data.size() == 4096 && CaptureTrack::data[1] == 1 && CaptureTrack::data[300] == (300 & 0xFF));
    // backend nulo: no revienta y regula el ritmo sin bloquear de mas
    auto nul = jni::MakeNullAudioTrackBackend(44100, 12, 2);
    std::vector<std::uint8_t> silence(4096);
    nul->Play();
    CHECK_EQ(nul->Write(silence.data(), 4096), 4096);
    CHECK_EQ(nul->PlayState(), 3);
    nul->Release();
    CHECK_EQ(nul->Write(silence.data(), 4096), -1);

    // ---- hilo guest con AttachCurrentThread/DetachCurrentThread ----
    threads::CreateParams p{Sym("t_thread"), mem::StrDup("hilo!"), 0, false, "jni-test"};
    const auto tid = threads::Create(p);
    CHECK(tid != 0);
    std::uint32_t rv = 0;
    CHECK(threads::Join(tid, &rv));
    CHECK_EQ(rv, 105u);

    // ---- cobertura ----
    bool miss_cls = false, miss_m = false, hit_m = false, miss_call = false;
    for (const auto& e : jni::Coverage()) {
      if (e.kind == jni::CoverageEntry::Kind::Class && e.cls == "zw/Unknown" && !e.covered) miss_cls = true;
      if (e.kind == jni::CoverageEntry::Kind::StaticMethod && e.name == "missing" && !e.covered) miss_m = true;
      if (e.kind == jni::CoverageEntry::Kind::StaticMethod && e.name == "mix" && e.covered && e.count == 3) hit_m = true;
      if (e.kind == jni::CoverageEntry::Kind::Call && e.name == "missing") miss_call = true;
    }
    CHECK(miss_cls && miss_m && hit_m && miss_call);
    CHECK(jni::CoverageReport().find("NO  GetStaticMethodID") != std::string::npos);

    // ---- clases del juego (sin el .so del juego: llamadas desde el host por la misma via) ----
    {
      jni::detail::LocalFrameGuard frame;
      auto call_static = [](const char* cls, const char* name, const char* sig, std::vector<jni::Value> args = {}) {
        jni::Class* c = jni::GetClass(cls);
        bool cov = false;
        jni::Method* mm = jni::GetOrAddMethod(c, name, sig, true, &cov);
        if (!cov) std::printf("  (no cubierto: %s.%s%s)\n", cls, name, sig);
        return jni::Invoke(mm, nullptr, c, std::move(args), false);
      };
      auto str = [](const jni::Value& v) {
        auto s = std::dynamic_pointer_cast<jni::String>(v.o);
        return s ? s->utf8 : std::string("<null>");
      };
      const char* G = "com/gameloft/android/ANMP/GloftZOHM/Game";
      CHECK(str(call_static(G, "getSDFolder", "()Ljava/lang/String;")) == jni::config().sd_folder);
      CHECK(str(call_static(G, "GetObbPath", "()Ljava/lang/String;")).ends_with("/"));
      CHECK(str(call_static(G, "GetPhoneGPUName", "()Ljava/lang/String;")) == "Adreno (TM) 330");
      CHECK_EQ(call_static(G, "isWifiEnabled", "()I").AsInt(), 0);
      CHECK_EQ(call_static(G, "GetTextureFormat", "()I").AsInt(), 4);
      CHECK_EQ(call_static(G, "getNumberOfCPUCores", "()I").AsInt(), 4);
      CHECK(call_static(G, "GetPhoneCPUFreq", "()F").AsFloat() == 1900000.0);
      CHECK(call_static(G, "GetCurrentTime", "()J").AsInt() > 1600000000000LL);
      CHECK_EQ(call_static(G, "keepScreenOn", "(Z)Z", {jni::Value::Bool(true)}).AsInt(), 1);
      const char* SU = "com/gameloft/android/ANMP/GloftZOHM/GLUtils/SUtils";
      CHECK(str(call_static(SU, "getPreferenceString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                            {jni::Value::Str("SDFolder"), jni::Value::Str("GamePrefs")})) == "");
      CHECK(call_static(SU, "getContext", "()Landroid/content/Context;").o != nullptr);
      // preferencias via Bundle (nativeSetPreference / nativeGetPreference)
      auto b = std::dynamic_pointer_cast<jni::Bundle>(jni::NewBundle());
      b->Put("npDataType", jni::Value::Int(0));
      b->Put("npKey", jni::Value::Str("volumen"));
      b->Put("npPrefName", jni::Value::Str("GamePrefs"));
      b->Put("npData", jni::Value::Int(77));
      call_static(SU, "nativeSetPreference", "(Landroid/os/Bundle;)V", {jni::Value::Obj(b)});
      auto q = std::dynamic_pointer_cast<jni::Bundle>(jni::NewBundle());
      q->Put("npDataType", jni::Value::Int(0));
      q->Put("npKey", jni::Value::Str("volumen"));
      q->Put("npPrefName", jni::Value::Str("GamePrefs"));
      q->Put("npDefaultValue", jni::Value::Int(5));
      call_static(SU, "nativeGetPreference", "(Landroid/os/Bundle;)Landroid/os/Bundle;", {jni::Value::Obj(q)});
      CHECK_EQ(q->Get("npResult").AsInt(), 77);
      q->Put("npKey", jni::Value::Str("otra"));
      call_static(SU, "nativeGetPreference", "(Landroid/os/Bundle;)Landroid/os/Bundle;", {jni::Value::Obj(q)});
      CHECK_EQ(q->Get("npResult").AsInt(), 5);
      const char* PJ = "com/gameloft/glot/PortingJNI";
      CHECK_EQ(call_static(PJ, "hasConnectivity", "()Z").AsInt(), 0);
      CHECK_EQ(call_static("com/gameloft/android/ANMP/GloftZOHM/PushNotification/C2DMAndroidUtils", "HasPushNotification", "()I")
                   .AsInt(),
               0);
      // Game.Exit sin hook -> GuestExit
      bool exited = false;
      try {
        call_static(G, "Exit", "()V");
      } catch (const GuestExit& e) {
        exited = e.code == 0;
      }
      CHECK(exited);
      // GL2JNILib.createView con hook del frontend
      int created = 0;
      jni::hooks().create_view = [&] { ++created; };
      call_static("com/gameloft/android/ANMP/GloftZOHM/GL2JNILib", "createView", "()V");
      CHECK_EQ(created, 1);
      jni::hooks().create_view = nullptr;
    }
  } catch (const GuestFault& f) {
    std::printf("  FALLO: GuestFault: %s\n", f.what());
    ++g_failed;
  } catch (const GuestExit& e) {
    std::printf("  FALLO: GuestExit %d\n", e.code);
    ++g_failed;
  } catch (const std::exception& e) {
    std::printf("  FALLO: excepcion: %s\n", e.what());
    ++g_failed;
  }

  std::printf("\n%s", jni::CoverageReport().c_str());
  std::printf("jni: %d comprobaciones, %d fallos\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
