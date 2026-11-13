// Clases del framework Java/Android emuladas (ANALYSIS §5.4): java/lang/*, android/os/Bundle,
// android/os/Process, android/content/Context, android/media/AudioTrack (T7).
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>

#include "core/log.h"
#include "jni/internal.h"

namespace zw::jni {

// ---------------------------------------------------------------------------------------------
// Bundle
// ---------------------------------------------------------------------------------------------
std::shared_ptr<Object> NewBundle() {
  return std::make_shared<Bundle>(GetClass("android/os/Bundle"));
}

namespace {
std::shared_ptr<Bundle> SelfBundle(Call& c) { return c.SelfAs<Bundle>(); }

Value BundleGet(Call& c, char t, bool has_default) {
  auto b = SelfBundle(c);
  Value v = b ? b->Get(c.S(0)) : Value();
  if (v.k == Value::K::None) {
    if (has_default) return c.args.size() > 1 ? c.args[1] : DefaultFor(t);
    return DefaultFor(t);
  }
  if (t == 'L' && v.k != Value::K::Obj) return Value::Null();
  if (t != 'L' && v.k == Value::K::Obj) return DefaultFor(t);
  return v;
}
Value BundlePut(Call& c) {
  if (auto b = SelfBundle(c)) b->Put(c.S(0), c.args.size() > 1 ? c.args[1] : Value());
  return Value();
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// AudioTrack
// ---------------------------------------------------------------------------------------------
namespace {
// Backend nulo: descarta el audio pero lo "consume" a tiempo real, como un dispositivo.
class NullAudioTrack final : public AudioTrackBackend {
 public:
  NullAudioTrack(int rate, int channel_cfg, int encoding) {
    const int ch = channel_cfg == 4 || channel_cfg == 2 ? 1 : 2;
    const int bps = encoding == 3 ? 1 : 2;
    bytes_per_sec_ = static_cast<double>(std::max(rate, 1)) * ch * bps;
  }
  void Play() override {
    std::lock_guard lk(mu_);
    if (state_ != 3) {
      state_ = 3;
      t0_ = Clock::now();
      written_ = 0;
    }
  }
  void Pause() override {
    std::lock_guard lk(mu_);
    if (state_ == 3) state_ = 2;
  }
  void Stop() override {
    std::lock_guard lk(mu_);
    state_ = 1;
  }
  void Release() override {
    std::lock_guard lk(mu_);
    state_ = 1;
    released_ = true;
  }
  int Write(const void*, int bytes) override {
    double ahead_s;
    {
      std::lock_guard lk(mu_);
      if (released_) return -1;
      if (state_ != 3 || bytes <= 0) return std::max(bytes, 0);
      written_ += bytes;
      const double played = std::chrono::duration<double>(Clock::now() - t0_).count();
      const double queued = written_ / bytes_per_sec_;
      if (queued < played) {  // subdesbordamiento: reiniciar el reloj
        t0_ = Clock::now();
        written_ = bytes;
        ahead_s = written_ / bytes_per_sec_;
      } else {
        ahead_s = queued - played;
      }
    }
    if (ahead_s > 0.1) std::this_thread::sleep_for(std::chrono::duration<double>(ahead_s - 0.1));
    return bytes;
  }
  int PlayState() const override {
    std::lock_guard lk(mu_);
    return state_;
  }

 private:
  using Clock = std::chrono::steady_clock;
  mutable std::mutex mu_;
  int state_ = 1;
  bool released_ = false;
  double bytes_per_sec_ = 176400.0;
  double written_ = 0;
  Clock::time_point t0_ = Clock::now();
};

std::mutex g_audio_mu;
AudioTrackFactory g_audio_factory;

struct AudioTrackObj : Object {
  using Object::Object;
  std::unique_ptr<AudioTrackBackend> backend;
  int state = 1;  // si no hay backend
  bool released = false;
  int rate = 44100, channel_cfg = 12, encoding = 2;
  ~AudioTrackObj() override {
    if (backend && !released) backend->Release();
  }
};
}  // namespace

void SetAudioTrackBackend(AudioTrackFactory f) {
  std::lock_guard lk(g_audio_mu);
  g_audio_factory = std::move(f);
}
std::unique_ptr<AudioTrackBackend> MakeNullAudioTrackBackend(int rate, int channel_cfg, int encoding) {
  return std::make_unique<NullAudioTrack>(rate, channel_cfg, encoding);
}

void RegisterAudioTrack() {
  DefineClass("android/media/AudioTrack")
      .Factory([](Class* c) { return std::make_shared<AudioTrackObj>(c); })
      .Static("getMinBufferSize", "(III)I", [](Call&) { return Value::Int(8192); })
      .Static("getNativeOutputSampleRate", "(I)I", [](Call&) { return Value::Int(44100); })
      .Ctor("(IIIIII)V",
            [](Call& c) {
              auto t = c.SelfAs<AudioTrackObj>();
              if (!t) return Value();
              t->rate = c.I(1);
              t->channel_cfg = c.I(2);
              t->encoding = c.I(3);
              AudioTrackFactory f;
              {
                std::lock_guard lk(g_audio_mu);
                f = g_audio_factory;
              }
              t->backend = f ? f(c.I(0), c.I(1), c.I(2), c.I(3), c.I(4), c.I(5))
                             : MakeNullAudioTrackBackend(c.I(1), c.I(2), c.I(3));
              log::Info("[jni] AudioTrack(stream {}, {} Hz, canales 0x{:x}, formato {}, buffer {} B, modo {}) -> {}", c.I(0),
                        c.I(1), c.I(2), c.I(3), c.I(4), c.I(5), t->backend ? (f ? "backend del frontend" : "backend nulo") : "sin salida");
              return Value();
            })
      .Virtual("play", "()V",
               [](Call& c) {
                 if (auto t = c.SelfAs<AudioTrackObj>()) {
                   if (t->backend) t->backend->Play();
                   t->state = 3;
                 }
                 return Value();
               })
      .Virtual("pause", "()V",
               [](Call& c) {
                 if (auto t = c.SelfAs<AudioTrackObj>()) {
                   if (t->backend) t->backend->Pause();
                   t->state = 2;
                 }
                 return Value();
               })
      .Virtual("stop", "()V",
               [](Call& c) {
                 if (auto t = c.SelfAs<AudioTrackObj>()) {
                   if (t->backend) t->backend->Stop();
                   t->state = 1;
                 }
                 return Value();
               })
      .Virtual("flush", "()V", [](Call&) { return Value(); })
      .Virtual("release", "()V",
               [](Call& c) {
                 if (auto t = c.SelfAs<AudioTrackObj>(); t && !t->released) {
                   if (t->backend) t->backend->Release();
                   t->released = true;
                   t->state = 1;
                 }
                 return Value();
               })
      .Virtual("write", "([BII)I",
               [](Call& c) {
                 auto t = c.SelfAs<AudioTrackObj>();
                 auto a = std::dynamic_pointer_cast<Array>(c.O(0));
                 const int off = c.I(1), n = c.I(2);
                 if (!t || !a || off < 0 || n < 0 || static_cast<std::uint64_t>(off) + n > a->length) return Value::Int(-2);
                 if (t->released) return Value::Int(-3);
                 if (!t->backend) return Value::Int(n);
                 return Value::Int(t->backend->Write(a->host() + off, n));
               })
      .Virtual("write", "([SII)I",
               [](Call& c) {
                 auto t = c.SelfAs<AudioTrackObj>();
                 auto a = std::dynamic_pointer_cast<Array>(c.O(0));
                 const int off = c.I(1), n = c.I(2);
                 if (!t || !a || off < 0 || n < 0 || static_cast<std::uint64_t>(off) + n > a->length) return Value::Int(-2);
                 if (t->released) return Value::Int(-3);
                 if (!t->backend) return Value::Int(n);
                 const int r = t->backend->Write(a->host() + 2 * off, 2 * n);
                 return Value::Int(r < 0 ? r : r / 2);
               })
      .Virtual("getPlayState", "()I",
               [](Call& c) {
                 auto t = c.SelfAs<AudioTrackObj>();
                 if (!t) return Value::Int(1);
                 return Value::Int(t->backend ? t->backend->PlayState() : t->state);
               })
      .Virtual("getState", "()I", [](Call& c) { return Value::Int(c.SelfAs<AudioTrackObj>() ? 1 : 0); })
      .Virtual("getSampleRate", "()I",
               [](Call& c) {
                 auto t = c.SelfAs<AudioTrackObj>();
                 return Value::Int(t ? t->rate : 44100);
               })
      .Virtual("setStereoVolume", "(FF)I", [](Call&) { return Value::Int(0); });
}

// ---------------------------------------------------------------------------------------------
// Resto del framework
// ---------------------------------------------------------------------------------------------
void RegisterAndroidClasses() {
  DefineClass("java/lang/Object")
      .Ctor("()V", [](Call&) { return Value(); })
      .Virtual("hashCode", "()I",
               [](Call& c) { return Value::Int(static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(c.self.get()) >> 3)); })
      .Virtual("equals", "(Ljava/lang/Object;)Z", [](Call& c) { return Value::Bool(c.self == c.O(0)); })
      .Virtual("toString", "()Ljava/lang/String;",
               [](Call& c) { return Value::Str(c.self && c.self->cls ? c.self->cls->name : "null"); });

  DefineClass("java/lang/String")
      .Ctor("()V", [](Call&) { return Value(); })
      .Virtual("length", "()I",
               [](Call& c) {
                 auto s = c.SelfAs<String>();
                 return Value::Int(s ? static_cast<std::int64_t>(s->utf8.size()) : 0);
               })
      .Virtual("toString", "()Ljava/lang/String;", [](Call& c) { return Value::Obj(c.self); })
      .Virtual("getBytes", "()[B",
               [](Call& c) {
                 auto s = c.SelfAs<String>();
                 const std::string u = s ? s->utf8 : std::string();
                 auto a = MakeArray('B', static_cast<std::uint32_t>(u.size()));
                 if (!u.empty()) std::memcpy(a->host(), u.data(), u.size());
                 return Value::Obj(a);
               })
      .Virtual("equals", "(Ljava/lang/Object;)Z",
               [](Call& c) {
                 auto a = c.SelfAs<String>();
                 auto b = std::dynamic_pointer_cast<String>(c.O(0));
                 return Value::Bool(a && b && a->utf8 == b->utf8);
               });

  DefineClass("java/lang/System")
      .Static("currentTimeMillis", "()J",
              [](Call&) {
                return Value::Int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
              })
      .Static("nanoTime", "()J", [](Call&) {
        return Value::Int(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      });

  DefineClass("android/os/Bundle")
      .Factory([](Class* c) { return std::make_shared<Bundle>(c); })
      .Ctor("()V", [](Call&) { return Value(); })
      .Virtual("putString", "(Ljava/lang/String;Ljava/lang/String;)V", BundlePut)
      .Virtual("putInt", "(Ljava/lang/String;I)V", BundlePut)
      .Virtual("putLong", "(Ljava/lang/String;J)V", BundlePut)
      .Virtual("putBoolean", "(Ljava/lang/String;Z)V", BundlePut)
      .Virtual("putFloat", "(Ljava/lang/String;F)V", BundlePut)
      .Virtual("putByteArray", "(Ljava/lang/String;[B)V", BundlePut)
      .Virtual("putIntArray", "(Ljava/lang/String;[I)V", BundlePut)
      .Virtual("getString", "(Ljava/lang/String;)Ljava/lang/String;", [](Call& c) { return BundleGet(c, 'L', false); })
      .Virtual("getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
               [](Call& c) { return BundleGet(c, 'L', true); })
      .Virtual("getInt", "(Ljava/lang/String;)I", [](Call& c) { return BundleGet(c, 'I', false); })
      .Virtual("getInt", "(Ljava/lang/String;I)I", [](Call& c) { return BundleGet(c, 'I', true); })
      .Virtual("getLong", "(Ljava/lang/String;)J", [](Call& c) { return BundleGet(c, 'J', false); })
      .Virtual("getLong", "(Ljava/lang/String;J)J", [](Call& c) { return BundleGet(c, 'J', true); })
      .Virtual("getBoolean", "(Ljava/lang/String;)Z", [](Call& c) { return BundleGet(c, 'Z', false); })
      .Virtual("getBoolean", "(Ljava/lang/String;Z)Z", [](Call& c) { return BundleGet(c, 'Z', true); })
      .Virtual("getFloat", "(Ljava/lang/String;)F", [](Call& c) { return BundleGet(c, 'F', false); })
      .Virtual("getByteArray", "(Ljava/lang/String;)[B", [](Call& c) { return BundleGet(c, 'L', false); })
      .Virtual("getIntArray", "(Ljava/lang/String;)[I", [](Call& c) { return BundleGet(c, 'L', false); })
      .Virtual("containsKey", "(Ljava/lang/String;)Z",
               [](Call& c) {
                 auto b = SelfBundle(c);
                 if (!b) return Value::Bool(false);
                 std::lock_guard lk(b->mu);
                 return Value::Bool(b->m.count(c.S(0)) != 0);
               })
      .Virtual("remove", "(Ljava/lang/String;)V",
               [](Call& c) {
                 if (auto b = SelfBundle(c)) {
                   std::lock_guard lk(b->mu);
                   b->m.erase(c.S(0));
                 }
                 return Value();
               })
      .Virtual("size", "()I",
               [](Call& c) {
                 auto b = SelfBundle(c);
                 if (!b) return Value::Int(0);
                 std::lock_guard lk(b->mu);
                 return Value::Int(static_cast<std::int64_t>(b->m.size()));
               })
      .Virtual("clear", "()V", [](Call& c) {
        if (auto b = SelfBundle(c)) {
          std::lock_guard lk(b->mu);
          b->m.clear();
        }
        return Value();
      });

  DefineClass("android/os/Process")
      .Static("setThreadPriority", "(I)V", [](Call&) { return Value(); })
      .Static("setThreadPriority", "(II)V", [](Call&) { return Value(); })
      .Static("myPid", "()I", [](Call&) { return Value::Int(4242); })
      .Static("myTid", "()I", [](Call&) { return Value::Int(static_cast<std::int64_t>(Cpu::Current().thread_id())); });

  DefineClass("android/content/Context");
  DefineClass("android/view/ViewRoot").Virtual("setProcessPositionEvents", "(Z)V", [](Call&) { return Value(); });
}

}  // namespace zw::jni
