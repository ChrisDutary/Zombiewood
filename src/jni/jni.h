// JNI falso (T7): JavaVM/JNIEnv en memoria guest + capa de clases Java emuladas.
// Contrato: docs/CONTRACTS.md §11 (JNI, T7).
//
// Uso tipico (T8):
//   zw::GuestMemory::Init();
//   zw::hle::libc::Register(); ...; zw::jni::Register();      // antes de loader::Load
//   auto* m = zw::loader::Load(so);  zw::loader::RunInit(*m);
//   zw::loader::CallJniOnLoad(*m, zw::jni::JavaVM());
//   zw::jni::call_native("Game_nativeSetGPUInfo", zw::jni::config().gpu_name);
//   zw::jni::call_native("GL2JNILib_init");
//   zw::jni::call_native("Game_nativeInit");
//   zw::jni::call_native("GL2JNILib_resize", 1280, 720);
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "cpu/cpu.h"
#include "cpu/guest_memory.h"

namespace zw::jni {

// Referencia JNI (jobject/jclass/jstring/jarray...) tal como la ve el guest: 32 bits opacos.
// Codificacion: (indice << 2) | tipo, tipo 1 = local, 2 = global, 3 = global debil (= GetObjectRefType).
using jref = std::uint32_t;

// ---------------------------------------------------------------------------------------------
// Registro y objetos globales
// ---------------------------------------------------------------------------------------------

// Registra los thunks de JNIEnv (233 slots, JNI 1.6) y JavaVM, crea las clases emuladas e instala el
// exit hook por hilo. Idempotente. Debe llamarse con la memoria guest ya iniciada (GuestMemory::Init).
void Register();

// JavaVM* guest (puntero a puntero a JNIInvokeInterface). Para JNI_OnLoad / VoxSetJavaVM.
gaddr JavaVM();
// JNIEnv* guest del hilo actual (lo crea si hace falta; equivale a AttachCurrentThread).
gaddr Env();

// ---------------------------------------------------------------------------------------------
// Configuracion del "dispositivo" (valores que devuelven las clases Java emuladas)
// Modificar ANTES de llamar a los nativos que la leen (Game.nativeInit copia las cadenas).
// ---------------------------------------------------------------------------------------------
struct Config {
  std::string package = "com.gameloft.android.ANMP.GloftZOHM";
  std::string version_name = "1.5.3";
  // Rutas GUEST (Android); la traduccion a rutas del host es cosa de T4.
  std::string sd_folder = "/sdcard/android/data/com.gameloft.android.ANMP.GloftZOHM/files";  // sin '/' final
  std::string save_folder = "/data/data/com.gameloft.android.ANMP.GloftZOHM";                // sin '/' final
  std::string obb_path = "/sdcard/Android/obb/com.gameloft.android.ANMP.GloftZOHM/";          // CON '/' final
  std::string main_obb = "main.1501.com.gameloft.android.ANMP.GloftZOHM.obb";
  std::string patch_obb = "patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb";  // "" si no existe
  std::string extra_obb;
  // Dispositivo (perfil de gama alta, ver ANALYSIS R10).
  std::string manufacturer = "samsung";
  std::string model = "GT-I9500";
  std::string device = "ja3g";
  std::string product = "ja3gxx";
  std::string firmware = "4.4.2";
  std::string cpu_name = "ARMv7 Processor rev 3 (v7l)";
  float cpu_freq_khz = 1900000.0f;
  int cpu_cores = 4;
  std::string gpu_name = "Adreno (TM) 330";
  std::string imei = "000000000000000";
  std::string android_id = "0123456789abcdef";
  std::string mac = "00:00:00:00:00:00";
  std::string user_agent = "Mozilla/5.0 (Linux; Android 4.4.2; GT-I9500 Build/KOT49H) AppleWebKit/537.36 (KHTML, like Gecko)";
  int texture_format = 4;  // 1 PVRTC, 2 ATC, 3 DXT, 4 ETC1
  int manufacture_code = 0;
  // Idioma: 0 en,1 fr,2 de,3 es,4 it,5 pt/br,6 ja,7 ko,8 zh,9 ru.
  int language = 0;
  std::string language_name = "English (United States)";
  std::string locale = "en_US";
  std::string lang_code = "en";
  std::string country = "US";
  // Pantalla (GameRenderer.nativeInit desde createView).
  int screen_w = 1280;
  int screen_h = 720;
  // Directorio del host con los assets/ del APK (ResLoader / getAssetAsString). Vacio = sin assets.
  std::filesystem::path assets_dir;
  // Fichero del host donde persistir las preferencias (SUtils.nativeSet/GetPreference). Vacio = solo memoria.
  std::filesystem::path prefs_file;
  // Valores de preferencias por defecto. GamePrefs.SDFolder vacio => el nativo usa su ruta por defecto.
  bool verbose_calls = false;  // log de cada llamada a metodo Java (ademas del log de cobertura)
};
Config& config();

// Callbacks hacia el frontend (T8). Todos opcionales; los valores por defecto son seguros.
struct Hooks {
  // GL2JNILib.createView(): por defecto llama a GameRenderer.nativeInit(manufacture, screen_w, screen_h, version).
  std::function<void()> create_view;
  // GL2JNILib.setViewSettings(pixelSize, depth, stencil, csaa, flag)
  std::function<void(int, int, int, int, int)> set_view_settings;
  // GL2JNILib.setCurrentContext(i) -> bool (por defecto true)
  std::function<bool(int)> set_current_context;
  // Game.Exit()
  std::function<void()> exit;
  // Game.sendAppToBackground() / Game.Pause()
  std::function<void()> send_to_background;
  // Game.playCinematicAndroid(nombre) (por defecto no-op = video saltado)
  std::function<void(const std::string&)> play_cinematic;
  // Game.openBrowser(url)
  std::function<void(const std::string&)> open_browser;
  // Game.keepScreenOn(b)
  std::function<void(bool)> keep_screen_on;
  // Game.HideScreen(i) / ShowScreen(i)
  std::function<void(int, bool)> show_screen;
};
Hooks& hooks();

// ---------------------------------------------------------------------------------------------
// AudioTrack (android/media/AudioTrack, ANALYSIS §8 opcion A). T8 enchufa aqui el Track de T6.
// ---------------------------------------------------------------------------------------------
// Misma forma que zw::audio::Track de T6 (src/hle/audio/audio_track.h): el adaptador es 1:1.
class AudioTrackBackend {
 public:
  virtual ~AudioTrackBackend() = default;
  virtual void Play() = 0;
  virtual void Pause() = 0;
  virtual void Stop() = 0;     // detiene y descarta la cola
  virtual void Release() = 0;  // libera el dispositivo; despierta writers bloqueados
  // PCM (16 bits con signo LE si encoding == 2), en BYTES. Puede bloquear para regular el ritmo.
  // Devuelve los bytes aceptados o < 0 si la pista esta liberada.
  virtual int Write(const void* pcm, int bytes) = 0;
  virtual int PlayState() const = 0;  // 1 stopped, 2 paused, 3 playing
};
// Argumentos = los del constructor Java AudioTrack(streamType, sampleRate, channelConfig, audioFormat,
// bufferSizeInBytes, mode). Devolver nullptr = pista sin salida (se descarta el audio).
using AudioTrackFactory = std::function<std::unique_ptr<AudioTrackBackend>(int stream_type, int rate, int channel_cfg,
                                                                           int encoding, int buffer_bytes, int mode)>;
// Por defecto: backend nulo que descarta el audio pero lo consume a tiempo real (como un dispositivo).
void SetAudioTrackBackend(AudioTrackFactory f);
std::unique_ptr<AudioTrackBackend> MakeNullAudioTrackBackend(int rate, int channel_cfg, int encoding);

// ---------------------------------------------------------------------------------------------
// Objetos desde el host
// ---------------------------------------------------------------------------------------------
// Crea un java/lang/String (referencia local en el marco actual del hilo).
jref NewString(std::string_view utf8);
// Contenido de un jstring ("" si es null o no es String).
std::string GetString(jref s);
// Referencia local a una clase (la crea vacia si no esta emulada).
jref FindClass(std::string_view name);
void DeleteRef(jref r);

// ---------------------------------------------------------------------------------------------
// Llamar a nativos Java_* desde el host (T8)
// ---------------------------------------------------------------------------------------------
// Direccion guest del nativo. `name` puede ser el simbolo completo ("Java_com_..._Game_nativeInit"),
// relativo al paquete del juego ("Game_nativeInit", "GLUtils_SUtils_nativeInit") o, si se registro con
// RegisterNatives, "clase/con/barras.metodo". 0 si no existe.
gaddr FindNative(std::string_view name);
// jclass (local) de la clase a la que pertenece un nativo (por el nombre mangled).
jref ClassOfNative(std::string_view name);

namespace detail {
struct LocalFrameGuard {
  LocalFrameGuard();
  ~LocalFrameGuard();
  LocalFrameGuard(const LocalFrameGuard&) = delete;
  LocalFrameGuard& operator=(const LocalFrameGuard&) = delete;
};
[[noreturn]] void ThrowNoNative(std::string_view name);

template <class T>
auto ToGuestArg(const T& v) {
  using D = std::decay_t<T>;
  if constexpr (std::is_same_v<D, std::string> || std::is_same_v<D, std::string_view> || std::is_same_v<D, const char*> ||
                std::is_same_v<D, char*>) {
    return static_cast<std::uint32_t>(NewString(v));
  } else {
    return v;
  }
}
}  // namespace detail

// Llama al nativo `name` con (JNIEnv*, jclass, args...) en el hilo actual (reentrante).
// Cadenas host (std::string, const char*, string_view) se convierten en jstring. Las referencias
// locales creadas durante la llamada se liberan al volver (marco local propio). Lanza
// std::runtime_error si el nativo no existe; GuestFault/GuestExit se propagan.
template <class R = void, class... A>
R call_native(std::string_view name, const A&... args) {
  const gaddr fn = FindNative(name);
  if (!fn) detail::ThrowNoNative(name);
  detail::LocalFrameGuard frame;
  const jref cls = ClassOfNative(name);
  auto conv = std::make_tuple(detail::ToGuestArg(args)...);
  return std::apply([&](const auto&... a) { return zw::call_guest<R>(fn, Env(), cls, a...); }, conv);
}
// Igual, para nativos de instancia: pasa `self` (jobject) en lugar de la clase.
template <class R = void, class... A>
R call_native_obj(std::string_view name, jref self, const A&... args) {
  const gaddr fn = FindNative(name);
  if (!fn) detail::ThrowNoNative(name);
  detail::LocalFrameGuard frame;
  auto conv = std::make_tuple(detail::ToGuestArg(args)...);
  return std::apply([&](const auto&... a) { return zw::call_guest<R>(fn, Env(), self, a...); }, conv);
}

// ---------------------------------------------------------------------------------------------
// Cobertura (para T8: que pidio el juego y que quedo sin emular)
// ---------------------------------------------------------------------------------------------
struct CoverageEntry {
  enum class Kind { Class, Method, StaticMethod, Field, StaticField, Call } kind;
  std::string cls, name, sig;  // name/sig vacios para Kind::Class
  bool covered = false;
  std::uint64_t count = 0;  // veces pedido (Get*ID / FindClass) o llamado (Call)
};
std::vector<CoverageEntry> Coverage();
// Informe legible (una linea por entrada; las no cubiertas marcadas con "NO").
std::string CoverageReport();

}  // namespace zw::jni
