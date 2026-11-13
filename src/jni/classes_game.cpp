// Clases Java propias del juego (ANALYSIS §5.1-5.3, §11): Game, GL2JNILib, GLUtils/*, installer/*,
// DataSharing, PushNotification, SplashScreen, PortingJNI, social e IAB (sin red ni compras) (T7).
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "jni/internal.h"

namespace zw::jni {
namespace {

constexpr const char* kPkg = "com/gameloft/android/ANMP/GloftZOHM/";
std::string P(const char* suffix) { return std::string(kPkg) + suffix; }

const char* const kS = "()Ljava/lang/String;";
const char* const kV = "()V";
const char* const kSV = "(Ljava/lang/String;)V";
const char* const kIV = "(I)V";

Impl RetStr(std::string Config::*field) {
  return [field](Call&) { return Value::Str(config().*field); };
}
Impl RetConst(std::string s) {
  return [s](Call&) { return Value::Str(s); };
}
Impl RetInt(std::int64_t v) {
  return [v](Call&) { return Value::Int(v); };
}
Impl RetNull() {
  return [](Call&) { return Value::Null(); };
}
Value Void() { return Value(); }

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::int64_t UptimeMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() + 3600000;
}

Value ByteArrayOf(const std::string& data) {
  auto a = MakeArray('B', static_cast<std::uint32_t>(data.size()));
  if (!a) return Value::Null();
  if (!data.empty()) std::memcpy(a->host(), data.data(), data.size());
  return Value::Obj(a);
}
Value IntArrayOf(std::initializer_list<std::int32_t> v) {
  auto a = MakeArray('I', static_cast<std::uint32_t>(v.size()));
  std::int32_t i = 0;
  for (std::int32_t x : v) std::memcpy(a->host() + 4 * i++, &x, 4);
  return Value::Obj(a);
}

// Asset del APK desde config().assets_dir (o nullopt).
bool ReadAsset(const std::string& name, std::string& out) {
  const auto& dir = config().assets_dir;
  if (dir.empty() || name.empty() || name.find("..") != std::string::npos) return false;
  std::string rel = name;
  if (rel.starts_with("assets/")) rel = rel.substr(7);
  while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
  std::ifstream f(dir / std::filesystem::path(std::u8string(rel.begin(), rel.end())), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// ---------------------------------------------------------------------------------------------
// Preferencias (SharedPreferences): prefName -> clave -> valor (tipo 0 int, 1 long, 2 bool, 3 string)
// ---------------------------------------------------------------------------------------------
struct PrefVal {
  int type = 3;
  std::int64_t i = 0;
  std::string s;
};
std::mutex g_prefs_mu;
std::map<std::string, std::map<std::string, PrefVal>> g_prefs;
bool g_prefs_loaded = false;

std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}
std::string Unesc(const std::string& s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}
void LoadPrefsLocked() {
  if (g_prefs_loaded) return;
  g_prefs_loaded = true;
  const auto& path = config().prefs_file;
  if (path.empty()) return;
  std::ifstream f(path, std::ios::binary);
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> parts;
    std::size_t p = 0;
    for (int k = 0; k < 3; ++k) {
      const auto q = line.find('\t', p);
      if (q == std::string::npos) break;
      parts.push_back(line.substr(p, q - p));
      p = q + 1;
    }
    if (parts.size() != 3) continue;
    PrefVal v;
    v.type = std::atoi(parts[2].c_str());
    const std::string val = Unesc(line.substr(p));
    if (v.type == 3) v.s = val;
    else v.i = std::strtoll(val.c_str(), nullptr, 10);
    g_prefs[Unesc(parts[0])][Unesc(parts[1])] = v;
  }
}
void SavePrefsLocked() {
  const auto& path = config().prefs_file;
  if (path.empty()) return;
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp = std::filesystem::path(path).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    for (const auto& [pref, kv] : g_prefs)
      for (const auto& [k, v] : kv)
        f << Esc(pref) << '\t' << Esc(k) << '\t' << v.type << '\t' << (v.type == 3 ? Esc(v.s) : std::to_string(v.i)) << '\n';
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) log::Warn("[jni] no se pudieron guardar las preferencias en {}: {}", path.string(), ec.message());
}
bool GetPref(const std::string& pref, const std::string& key, PrefVal& out) {
  std::lock_guard lk(g_prefs_mu);
  LoadPrefsLocked();
  auto it = g_prefs.find(pref);
  if (it == g_prefs.end()) return false;
  auto jt = it->second.find(key);
  if (jt == it->second.end()) return false;
  out = jt->second;
  return true;
}
void SetPref(const std::string& pref, const std::string& key, const PrefVal& v) {
  std::lock_guard lk(g_prefs_mu);
  LoadPrefsLocked();
  g_prefs[pref][key] = v;
  SavePrefsLocked();
}

// SUtils.nativeSetPreference(Bundle) / nativeGetPreference(Bundle) (copiado del Java original).
Value NativeSetPreference(Call& c) {
  auto b = std::dynamic_pointer_cast<Bundle>(c.O(0));
  if (!b) return Void();
  auto str = [&](const char* k) {
    auto s = std::dynamic_pointer_cast<String>(b->Get(k).o);
    return s ? s->utf8 : std::string();
  };
  PrefVal v;
  v.type = static_cast<int>(b->Get("npDataType").AsInt());
  if (v.type == 3) v.s = str("npData");
  else v.i = b->Get("npData").AsInt();
  if (v.type < 0 || v.type > 3) return Void();
  SetPref(str("npPrefName"), str("npKey"), v);
  return Void();
}
Value NativeGetPreference(Call& c) {
  auto b = std::dynamic_pointer_cast<Bundle>(c.O(0));
  if (!b) return Value::Null();
  auto str = [&](const char* k) {
    auto s = std::dynamic_pointer_cast<String>(b->Get(k).o);
    return s ? s->utf8 : std::string();
  };
  const int type = static_cast<int>(b->Get("npDataType").AsInt());
  PrefVal v;
  const bool found = GetPref(str("npPrefName"), str("npKey"), v);
  const Value def = b->Get("npDefaultValue");
  switch (type) {
    case 0: case 1: case 2:
      b->Put("npResult", found && v.type != 3 ? (type == 2 ? Value::Bool(v.i != 0) : Value::Int(v.i))
                                              : Value::Int(def.k == Value::K::None ? 0 : def.AsInt()));
      break;
    case 3: {
      auto ds = std::dynamic_pointer_cast<String>(def.o);
      b->Put("npResult", Value::Str(found && v.type == 3 ? v.s : (ds ? ds->utf8 : std::string())));
      break;
    }
    default: break;
  }
  return Value::Obj(c.O(0));
}
Value GetPreferenceString(const std::string& key, const std::string& pref, const std::string& def) {
  PrefVal v;
  if (GetPref(pref, key, v) && v.type == 3) return Value::Str(v.s);
  return Value::Str(def);
}

// DataSharing (valores compartidos entre juegos Gameloft): en memoria.
std::mutex g_shared_mu;
std::map<std::string, std::string> g_shared;

ObjPtr ContextSingleton() {
  static ObjPtr ctx = std::make_shared<Object>(GetClass("android/content/Context"));
  return ctx;
}

void DefaultCreateView() {
  const auto& cfg = config();
  if (FindNative("GameRenderer_nativeInit")) {
    log::Info("[jni] GL2JNILib.createView -> GameRenderer.nativeInit({}, {}, {}, \"{}\")", cfg.manufacture_code, cfg.screen_w,
              cfg.screen_h, cfg.version_name);
    call_native("GameRenderer_nativeInit", cfg.manufacture_code, cfg.screen_w, cfg.screen_h, cfg.version_name);
  } else {
    log::Warn("[jni] GL2JNILib.createView: no existe GameRenderer_nativeInit");
  }
}

}  // namespace

void RegisterGameClasses() {
  // ---- Game (§5.1) ----
  DefineClass(P("Game"))
      .Static("getSDFolder", kS, RetStr(&Config::sd_folder))
      .Static("getSaveFolder", kS, RetStr(&Config::save_folder))
      .Static("GetObbPath", kS, RetStr(&Config::obb_path))
      .Static("GetMainObbName", kS, RetStr(&Config::main_obb))
      .Static("GetPatchObbName", kS, RetStr(&Config::patch_obb))
      .Static("GetExtraObbName", kS, RetStr(&Config::extra_obb))
      .Static("GetPhoneManufacturer", kS, RetStr(&Config::manufacturer))
      .Static("GetPhoneModel", kS, RetStr(&Config::model))
      .Static("GetPhoneDevice", kS, RetStr(&Config::device))
      .Static("GetPhoneFirmware", kS, RetStr(&Config::firmware))
      .Static("GetPhoneCPUName", kS, RetStr(&Config::cpu_name))
      .Static("GetPhoneCPUFreq", "()F", [](Call&) { return Value::Float(config().cpu_freq_khz); })
      .Static("GetPhoneGPUName", kS, RetStr(&Config::gpu_name))
      .Static("GetIMEI", kS, RetStr(&Config::imei))
      .Static("GetWSClose", "()Z", RetInt(0))
      .Static("getNumberOfCPUCores", "()I", [](Call&) { return Value::Int(config().cpu_cores); })
      .Static("getManufacture", "()I", [](Call&) { return Value::Int(config().manufacture_code); })
      .Static("GetTextureFormat", "()I", [](Call&) { return Value::Int(config().texture_format); })
      .Static("GetPhoneLanguage", "()I", [](Call&) { return Value::Int(config().language); })
      .Static("GetPhoneLanguageS", kS, RetStr(&Config::language_name))
      .Static("GetCurrentLocale", kS, RetStr(&Config::locale))
      .Static("GetCurrentTime", "()J", [](Call&) { return Value::Int(NowMs()); })
      .Static("isWifiEnabled", "()I", RetInt(0))
      .Static("IsJailBroken", "()I", RetInt(0))
      .Static("GetXperiaSlideout", "()I", RetInt(2))
      .Static("keepScreenOn", "(Z)Z",
              [](Call& c) {
                if (hooks().keep_screen_on) hooks().keep_screen_on(c.Z(0));
                return Value::Bool(c.Z(0));
              })
      .Static("hasClosedActivity", "()I", RetInt(0))
      .Static("checkFreeCashReady", "()Z", RetInt(0))
      .Static("getRewardsAvailable", kS, RetConst(""))
      .Static("getRewardsUser", kS, RetConst(""))
      .Static("getInstalledGames", kS, RetConst(""))
      .Static("getGLLivePassword", kS, RetConst(""))
      .Static("getGLLiveUsername", kS, RetConst(""))
      .Static("Exit", kV,
              [](Call&) -> Value {
                log::Info("[jni] Game.Exit()");
                if (hooks().exit) {
                  hooks().exit();
                  return Void();
                }
                throw GuestExit{0};
              })
      .Static("sendAppToBackground", kV,
              [](Call&) {
                if (hooks().send_to_background) hooks().send_to_background();
                return Void();
              })
      .Static("Pause", kV,
              [](Call&) {
                if (hooks().send_to_background) hooks().send_to_background();
                return Void();
              })
      .Static("openBrowser", kSV,
              [](Call& c) {
                log::Info("[jni] Game.openBrowser(\"{}\")", c.S(0));
                if (hooks().open_browser) hooks().open_browser(c.S(0));
                return Void();
              })
      .Static("playCinematicAndroid", kSV,
              [](Call& c) {
                log::Info("[jni] Game.playCinematicAndroid(\"{}\"){}", c.S(0), hooks().play_cinematic ? "" : " -> video saltado");
                if (hooks().play_cinematic) hooks().play_cinematic(c.S(0));
                return Void();
              })
      .Static("HideScreen", kIV,
              [](Call& c) {
                if (hooks().show_screen) hooks().show_screen(c.I(0), false);
                return Void();
              })
      .Static("ShowScreen", kIV,
              [](Call& c) {
                if (hooks().show_screen) hooks().show_screen(c.I(0), true);
                return Void();
              })
      .Static("PrintDebug", kSV,
              [](Call& c) {
                log::Debug("[java] {}", c.S(0));
                return Void();
              })
      .StaticNoop({{"getHttpResponse", kSV},
                   {"ShowPlusOne", kSV},
                   {"PopupTrophyGL", kIV},
                   {"notifyTrophy", kIV},
                   {"launchIGP", kIV},
                   {"downloadWelcomeScr", kIV},
                   {"showAds", kIV},
                   {"loadFreeCash", kIV},
                   {"launchGLLive", "(IZ)V"},
                   {"enableLaunchPN", "(Z)V"},
                   {"logoutGLLive", kV},
                   {"displayWelcomeBackUser", kV},
                   {"launchWelcomeScr", kV},
                   {"setIsPayingUser", kV},
                   {"hideAds", kV},
                   {"showInterstitial", kV},
                   {"showFreeCash", kV}});

  // ---- GL2JNILib (§5.2) ----
  DefineClass(P("GL2JNILib"))
      .Static("createView", kV,
              [](Call&) {
                if (hooks().create_view) hooks().create_view();
                else DefaultCreateView();
                return Void();
              })
      .Static("setViewSettings", "(IIIII)V",
              [](Call& c) {
                log::Info("[jni] GL2JNILib.setViewSettings(pixelSize {}, depth {}, stencil {}, csaa {}, flag {})", c.I(0), c.I(1),
                          c.I(2), c.I(3), c.I(4));
                if (hooks().set_view_settings) hooks().set_view_settings(c.I(0), c.I(1), c.I(2), c.I(3), c.I(4));
                return Void();
              })
      .Static("setCurrentContext", "(I)Z",
              [](Call& c) { return Value::Bool(hooks().set_current_context ? hooks().set_current_context(c.I(0)) : true); })
      .Static("setupPaths", kV, [](Call&) { return Void(); })
      .Static("enableAccelerometer", "(ZF)V", [](Call&) { return Void(); })
      .Static("getResource", "(Ljava/lang/String;)[B", RetNull());
  DefineClass(P("GameRenderer"));
  DefineClass(P("GL2JNIView"));

  // ---- GLUtils/SUtils (§5.2) ----
  DefineClass(P("GLUtils/SUtils"))
      .Static("getPreferenceString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
              [](Call& c) { return GetPreferenceString(c.S(0), c.S(1), ""); })
      .Static("getPreferenceString", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
              [](Call& c) { return GetPreferenceString(c.S(0), c.S(2), c.S(1)); })
      .Static("getPreferenceInt", "(Ljava/lang/String;ILjava/lang/String;)I",
              [](Call& c) {
                PrefVal v;
                return Value::Int(GetPref(c.S(2), c.S(0), v) && v.type != 3 ? v.i : c.I(1));
              })
      .Static("getPreferenceBoolean", "(Ljava/lang/String;ZLjava/lang/String;)Z",
              [](Call& c) {
                PrefVal v;
                return Value::Bool(GetPref(c.S(2), c.S(0), v) && v.type != 3 ? v.i != 0 : c.Z(1));
              })
      .Static("getPackage", kS, RetStr(&Config::package))
      .Static("getSaveFolder", kS, RetStr(&Config::save_folder))
      .Static("getSDFolder", kS, RetStr(&Config::sd_folder))
      .Static("getContext", "()Landroid/content/Context;", [](Call&) { return Value::Obj(ContextSingleton()); })
      .Static("playVideo", "(Ljava/lang/String;Z)Z", RetInt(0))
      .Static("deleteFile", kSV,
              [](Call& c) {
                log::Info("[jni] SUtils.deleteFile(\"{}\") (ignorado: los ficheros guest los gestiona libc)", c.S(0));
                return Void();
              })
      .Static("getAssetAsString", "(Ljava/lang/String;)[B",
              [](Call& c) {
                std::string data;
                return ReadAsset(c.S(0), data) ? ByteArrayOf(data) : Value::Null();
              })
      .Static("getGameName", kS, RetConst(""))
      .Static("getInjectedIGP", kS, RetConst(""))
      .Static("getInjectedSerialKey", kS, RetConst(""))
      .Static("getMetaDataValue", "(Ljava/lang/String;)Ljava/lang/String;", RetConst(""))
      .Static("retrieveBarrels", "()[I", [](Call&) { return IntArrayOf({0, 0, 0, 0}); })
      .Static("getGLUID", "(Ljava/lang/String;)[I", [](Call&) { return IntArrayOf({0x5A57A1D0, 0x12345678, 0x0BADF00D, 0x7E57C0DE}); })
      .Static("initCheckConnectionType", "()I", RetInt(0))
      .Static("nativeSetPreference", "(Landroid/os/Bundle;)V", NativeSetPreference)
      .Static("nativeGetPreference", "(Landroid/os/Bundle;)Landroid/os/Bundle;", NativeGetPreference)
      .Static("genericUnzipArchive", "(Ljava/lang/String;Ljava/lang/String;)Z", RetInt(0))
      .Static("removeDirectoryRecursively", "(Ljava/lang/String;)Z", RetInt(1))
      .StaticNoop({{"stopVideo", kV},
                   {"inGameVideoSetSkipEnabled", "(Z)V"},
                   {"shareInfo", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"},
                   {"showCantGoBackPopup", kIV}});

  // ---- GLUtils/Device ----
  {
    auto d = DefineClass(P("GLUtils/Device"));
    d.Static("getUserAgent", kS, RetStr(&Config::user_agent))
        .Static("d1", kS, RetStr(&Config::android_id))
        .Static("getAndroidId", kS, RetStr(&Config::android_id))
        .Static("getSerial", kS, RetStr(&Config::android_id))
        .Static("getSerialNo", kS, RetStr(&Config::android_id))
        .Static("getDeviceFirmware", kS, RetStr(&Config::firmware))
        .Static("getMacAddress", kS, RetStr(&Config::mac))
        .Static("getDeviceIMEI", kS, RetStr(&Config::imei))
        .Static("getHDIDFV", kS, RetStr(&Config::android_id))
        .Static("getHDIDFVVersion", kS, RetConst("1"))
        .Static("getGLDID", kS, RetStr(&Config::android_id))
        .Static("getDeviceName", kS, RetStr(&Config::model))
        .Static("getPhoneManufacturer", kS, RetStr(&Config::manufacturer))
        .Static("getPhoneModel", kS, RetStr(&Config::model))
        .Static("getPhoneDevice", kS, RetStr(&Config::device))
        .Static("getPhoneProduct", kS, RetStr(&Config::product))
        .Static("retrieveDeviceCarrier", kS, RetConst(""))
        .Static("retrieveDeviceCountry", kS, RetStr(&Config::country))
        .Static("retrieveDeviceRegion", kS, RetStr(&Config::country))
        .Static("retrieveDeviceLanguage", kS, RetStr(&Config::lang_code))
        .Static("retrieveCPUSerial", kS, RetConst("0000000000000000"));
  }

  // ---- GLUtils/ResLoader ----
  DefineClass(P("GLUtils/ResLoader"))
      .Static("getLength", "(Ljava/lang/String;)I",
              [](Call& c) {
                std::string data;
                return Value::Int(ReadAsset(c.S(0), data) ? static_cast<std::int64_t>(data.size()) : -1);
              })
      .Static("getBytes", "(Ljava/lang/String;)[B",
              [](Call& c) {
                std::string data;
                return ReadAsset(c.S(0), data) ? ByteArrayOf(data) : Value::Null();
              })
      .Static("getAssetList", "(Ljava/lang/String;)[Ljava/lang/String;", RetNull());
  DefineClass(P("GLUtils/NetworkStateReceiver"));  // setConnectionType es nativo: no se llama (sin red)

  // ---- installer (no se ejecuta el instalador/DRM; solo lo que lee AndroidOS) ----
  DefineClass(P("installer/GameInstaller"))
      .StaticField("mPreferencesName", "Ljava/lang/String;", Value::Str("GamePrefs"))
      .StaticField("m_sInstance", "Landroid/content/Context;", Value::Obj(ContextSingleton()))
      .StaticField("mDeviceInfo", "Landroid/telephony/TelephonyManager;", Value::Null())
      .Static("startGame", kV, [](Call&) { return Void(); })
      .Static("getSDFolder", kS, RetStr(&Config::sd_folder));
  DefineClass(P("installer/GDRMPolicy"))
      .StaticNoop({{"UpdatePreferences", "(Ljava/lang/String;Ljava/lang/String;I)V"}, {"UpdatePreferences2", "(Ljava/lang/String;JI)V"}});

  // ---- SendInfo (tracking) ----
  DefineClass(P("SendInfo"))
      .Static("getSDFolder", kS, RetStr(&Config::sd_folder))
      .Static("getGLDID", kS, RetStr(&Config::android_id))
      .Static("getPhoneCarrier", kS, RetConst(""))
      .Static("getLocaleCountry", kS, RetStr(&Config::country))
      .Static("getLocaleLanguage", kS, RetStr(&Config::lang_code))
      .Static("getManufacturerModel", kS, [](Call&) { return Value::Str(config().manufacturer + " " + config().model); })
      .Static("getGLUID", "()[I", [](Call&) { return IntArrayOf({0x5A57A1D0, 0x12345678, 0x0BADF00D, 0x7E57C0DE}); });

  // ---- DataSharing ----
  DefineClass(P("DataSharing"))
      .Static("setSharedValue", "(Ljava/lang/String;Ljava/lang/String;)V",
              [](Call& c) {
                std::lock_guard lk(g_shared_mu);
                g_shared[c.S(0)] = c.S(1);
                return Void();
              })
      .Static("getSharedValue", "(Ljava/lang/String;)Ljava/lang/String;",
              [](Call& c) {
                std::lock_guard lk(g_shared_mu);
                auto it = g_shared.find(c.S(0));
                return Value::Str(it == g_shared.end() ? std::string() : it->second);
              })
      .Static("deleteSharedValue", kSV, [](Call& c) {
        std::lock_guard lk(g_shared_mu);
        g_shared.erase(c.S(0));
        return Void();
      });

  // ---- PushNotification (sin servicio) ----
  DefineClass(P("PushNotification/C2DMAndroidUtils"))
      .Static("HasPushNotification", "()I", RetInt(0))
      .Static("IsEnable", "()Z", RetInt(0))
      .Static("GetUserID", kS, RetConst(""))
      .Static("GetStatus", "(Z)I", RetInt(0))
      .Static("SendPush", "(Landroid/os/Bundle;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/String;", RetConst(""))
      .Static("SendPushToMyself", "(Landroid/os/Bundle;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/String;", RetConst(""))
      .Static("SetOfflineDeviceCredential", "()I", RetInt(0))
      .Static("SetUserCredential", "(Ljava/lang/String;Ljava/lang/String;)I", RetInt(0))
      .Static("SetCustomUserCredential", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", RetInt(0))
      .Static("DeletePush", "(Ljava/lang/String;Z)I", RetInt(0))
      .Static("GetJanusToken", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", RetConst(""))
      .Static("GetBundleData", "()Landroid/os/Bundle;", RetNull())
      .Static("IsDontDisturbEnable", "()Z", RetInt(0))
      .Static("GetRegistrationID", kS, RetConst(""))
      .StaticNoop({{"ResetNotificationStatus", kV}, {"SetEnable", "(Z)V"}, {"CancelAll", kV}, {"SetDontDisturbEnable", "(Z)V"}});

  // ---- Welcome screen / browser / ads ----
  DefineClass(P("SplashScreenActivity"))
      .Static("isWSReady", "(Ljava/lang/String;)I", RetInt(0))
      .Static("isActive", "()I", RetInt(0))
      .StaticNoop({{"downloadWS", kSV}, {"showLocalWS", kSV}, {"deleteWS", kSV}, {"SetWSLanguage", kIV}});
  DefineClass(P("InGameBrowser")).StaticNoop({{"showForum", kV}});
  DefineClass(P("AdServer")).StaticNoop({{"staticShowInterstitialWithTags", kSV}});

  // ---- GLOT ----
  DefineClass("com/gameloft/glot/PortingJNI")
      .Static("getSystemUpTimeMillis", "()J", [](Call&) { return Value::Int(UptimeMs()); })
      .Static("hasConnectivity", "()Z", RetInt(0))
      .Static("getIdentifier", kS, RetStr(&Config::android_id))
      .Static("getMac", kS, RetStr(&Config::mac))
      .Static("getDeviceName", kS, RetStr(&Config::model))
      .Static("getDeviceFirmware", kS, RetStr(&Config::firmware))
      .Static("getDeviceLanguage", kS, RetStr(&Config::lang_code))
      .Static("getDeviceCountry", kS, RetStr(&Config::country))
      .Static("getDeviceCarrier", kS, RetConst(""));
}

void RegisterSocialAndMisc() {
  // Social (§5.3): sin sesion. Los metodos void no listados quedan sin implementar (log una vez, no-op).
  DefineClass("com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib")
      .Static("IsLoggedIn", "()Z", RetInt(0))
      .Static("hasPermission", "(Ljava/lang/String;)Z", RetInt(0))
      .Static("getAccessToken", kS, RetConst(""))
      .StaticNoop({{"Init", "(ZLjava/lang/String;)V"}, {"Login", kV}, {"Logout", kV}, {"GetMyInfo", kV}});
  DefineClass("com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib")
      .Static("IsLoggedIn", "()Z", RetInt(0))
      .Static("GetAccessToken", kS, RetConst(""))
      .StaticNoop({{"InitGameAPI", kV}, {"ConnectToService", kV}, {"ShowAchievements", kV}});
  DefineClass("com/gameloft/GLSocialLib/PlatformAndroid").Static("GetDataFromURL", "(Ljava/lang/String;)[B", RetNull());
  DefineClass("com/gameloft/GLSocialLib/renren/RenrenAndroidGLSocialLib");
  // IAB (§11.4): metodos ofuscados [NC]; la clase existe para que FindClass no falle, sin compras.
  DefineClass(P("iab/InAppBilling"));
  DefineClass(P("iab/s"));
}

}  // namespace zw::jni
