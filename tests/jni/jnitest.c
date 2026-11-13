// Libreria guest de prueba para tests/jni (T7). gen_blob.py la compila (clang ARMv7 Thumb softfp) e
// incrusta el .so en jnitest_blob.inc. Usa la tabla JNIEnv por indice de slot (JNI 1.6), como el juego.
#include <stdarg.h>

#define EXPORT __attribute__((visibility("default"), noinline, used))

typedef void* jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jarray;
typedef void* jmethodID;
typedef void* jfieldID;
typedef int jint;
typedef long long jlong;
typedef unsigned char jboolean;
typedef signed char jbyte;
typedef unsigned short jchar;
typedef float jfloat;
typedef double jdouble;
typedef union { jboolean z; jbyte b; jchar c; short s; jint i; jlong j; jfloat f; jdouble d; jobject l; } jvalue;
typedef void* const* JNIEnv;  // *env = tabla
typedef void* const* JavaVM;  // *vm = tabla
#define F(env, idx, T) ((T)((*(env))[idx]))

// slots
enum {
  kFindClass = 6, kExceptionOccurred = 15, kExceptionClear = 17, kPushLocalFrame = 19, kPopLocalFrame = 20,
  kNewGlobalRef = 21, kDeleteGlobalRef = 22, kDeleteLocalRef = 23, kIsSameObject = 24, kNewObject = 28,
  kGetObjectClass = 31, kIsInstanceOf = 32, kGetMethodID = 33, kCallObjectMethod = 34, kCallIntMethod = 49,
  kCallIntMethodV = 50, kCallVoidMethod = 61, kCallNonvirtualIntMethod = 79, kCallNonvirtualVoidMethod = 91,
  kGetFieldID = 94, kGetIntField = 100, kGetFloatField = 102, kSetIntField = 109, kSetFloatField = 111,
  kGetStaticMethodID = 113, kCallStaticObjectMethod = 114, kCallStaticIntMethod = 129, kCallStaticLongMethod = 132,
  kCallStaticFloatMethod = 135, kCallStaticDoubleMethod = 138, kCallStaticDoubleMethodV = 139,
  kCallStaticDoubleMethodA = 140, kCallStaticVoidMethod = 141, kGetStaticFieldID = 144, kGetStaticObjectField = 145,
  kGetStaticLongField = 151, kSetStaticLongField = 160,
  kGetStringLength = 164, kGetStringChars = 165, kReleaseStringChars = 166, kNewStringUTF = 167, kGetStringUTFLength = 168,
  kGetStringUTFChars = 169, kReleaseStringUTFChars = 170, kGetArrayLength = 171, kNewObjectArray = 172,
  kGetObjectArrayElement = 173, kSetObjectArrayElement = 174, kNewByteArray = 176, kNewIntArray = 179,
  kGetIntArrayElements = 187, kReleaseIntArrayElements = 195, kGetIntArrayRegion = 203, kSetIntArrayRegion = 211,
  kRegisterNatives = 215, kGetJavaVM = 219, kGetStringUTFRegion = 221, kGetPrimitiveArrayCritical = 222,
  kReleasePrimitiveArrayCritical = 223, kExceptionCheck = 228, kGetObjectRefType = 232,
};

static JavaVM* g_vm;
static char g_hello[64];

static void scpy(char* d, const char* s, int cap) {
  int i = 0;
  for (; s[i] && i < cap - 1; ++i) d[i] = s[i];
  d[i] = 0;
}
static int seq(const char* a, const char* b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

static jclass FindClass(JNIEnv* e, const char* n) { return F(e, kFindClass, jclass (*)(JNIEnv*, const char*))(e, n); }
static jmethodID SMID(JNIEnv* e, jclass c, const char* n, const char* s) {
  return F(e, kGetStaticMethodID, jmethodID (*)(JNIEnv*, jclass, const char*, const char*))(e, c, n, s);
}
static jmethodID MID(JNIEnv* e, jclass c, const char* n, const char* s) {
  return F(e, kGetMethodID, jmethodID (*)(JNIEnv*, jclass, const char*, const char*))(e, c, n, s);
}
static jstring NewStr(JNIEnv* e, const char* s) { return F(e, kNewStringUTF, jstring (*)(JNIEnv*, const char*))(e, s); }
static jint UtfLen(JNIEnv* e, jstring s) { return F(e, kGetStringUTFLength, jint (*)(JNIEnv*, jstring))(e, s); }
static void DelLocal(JNIEnv* e, jobject o) { F(e, kDeleteLocalRef, void (*)(JNIEnv*, jobject))(e, o); }

EXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env = 0;
  g_vm = vm;
  if (F(vm, 6, jint (*)(JavaVM*, JNIEnv**, jint))(vm, &env, 0x10004) != 0 || !env) return -1;
  jclass c = FindClass(env, "zw/Test");
  jmethodID m = SMID(env, c, "hello", "()Ljava/lang/String;");
  jstring s = F(env, kCallStaticObjectMethod, jobject (*)(JNIEnv*, jclass, jmethodID, ...))(env, c, m);
  const char* p = F(env, kGetStringUTFChars, const char* (*)(JNIEnv*, jstring, jboolean*))(env, s, 0);
  scpy(g_hello, p, sizeof g_hello);
  F(env, kReleaseStringUTFChars, void (*)(JNIEnv*, jstring, const char*))(env, s, p);
  return reserved ? -1 : 0x10004;
}
EXPORT const char* get_hello(void) { return g_hello; }

// ---- llamadas: variadica, va_list (como los wrappers inline de _JNIEnv) y jvalue[] ----
static const char* const kMixSig = "(IJFDZLjava/lang/String;)D";
static jdouble CallStaticDoubleWrap(JNIEnv* env, jclass c, jmethodID m, ...) {
  va_list ap;
  va_start(ap, m);
  jdouble r = F(env, kCallStaticDoubleMethodV, jdouble (*)(JNIEnv*, jclass, jmethodID, va_list))(env, c, m, ap);
  va_end(ap);
  return r;
}
EXPORT jdouble t_call_va(JNIEnv* env) {
  jclass c = FindClass(env, "zw/Test");
  jmethodID m = SMID(env, c, "mix", kMixSig);
  return F(env, kCallStaticDoubleMethod, jdouble (*)(JNIEnv*, jclass, jmethodID, ...))(env, c, m, 7, (jlong)1 << 40, 1.5f,
                                                                                       2.25, (jboolean)1, NewStr(env, "abc"));
}
EXPORT jdouble t_call_v(JNIEnv* env) {
  jclass c = FindClass(env, "zw/Test");
  jmethodID m = SMID(env, c, "mix", kMixSig);
  return CallStaticDoubleWrap(env, c, m, 7, (jlong)1 << 40, 1.5f, 2.25, (jboolean)1, NewStr(env, "abc"));
}
EXPORT jdouble t_call_a(JNIEnv* env) {
  jclass c = FindClass(env, "zw/Test");
  jmethodID m = SMID(env, c, "mix", kMixSig);
  jvalue v[6];
  v[0].j = 0;
  v[0].i = 7;
  v[1].j = (jlong)1 << 40;
  v[2].j = 0;
  v[2].f = 1.5f;
  v[3].d = 2.25;
  v[4].j = 0;
  v[4].z = 1;
  v[5].j = 0;
  v[5].l = NewStr(env, "abc");
  return F(env, kCallStaticDoubleMethodA, jdouble (*)(JNIEnv*, jclass, jmethodID, const jvalue*))(env, c, m, v);
}
EXPORT jfloat t_call_float(JNIEnv* env, jfloat x) {
  jclass c = FindClass(env, "zw/Test");
  return F(env, kCallStaticFloatMethod, jfloat (*)(JNIEnv*, jclass, jmethodID, ...))(env, c, SMID(env, c, "half", "(F)F"), x);
}
EXPORT jlong t_call_long(JNIEnv* env, jint x) {
  jclass c = FindClass(env, "zw/Test");
  return F(env, kCallStaticLongMethod, jlong (*)(JNIEnv*, jclass, jmethodID, ...))(env, c, SMID(env, c, "big", "(I)J"), x);
}
// metodo no implementado: debe devolver 0 y quedar en la cobertura
EXPORT jint t_missing(JNIEnv* env) {
  jclass c = FindClass(env, "zw/Test");
  jmethodID m = SMID(env, c, "missing", "(I)I");
  jclass u = FindClass(env, "zw/Unknown");
  jmethodID m2 = SMID(env, u, "foo", "()Ljava/lang/String;");
  jobject r = F(env, kCallStaticObjectMethod, jobject (*)(JNIEnv*, jclass, jmethodID, ...))(env, u, m2);
  if (!c || !m || !u || !m2 || r) return -1;
  F(env, kCallStaticVoidMethod, void (*)(JNIEnv*, jclass, jmethodID, ...))(env, 0, 0);  // IDs nulos tolerados
  return F(env, kCallStaticIntMethod, jint (*)(JNIEnv*, jclass, jmethodID, ...))(env, c, m, 5) + 1000;
}

// ---- strings ----
EXPORT jint t_strings(JNIEnv* env) {
  jint ok = 0;
  jstring s = NewStr(env, "h\xc3\xa9llo");  // "hello" con e acentuada
  if (UtfLen(env, s) == 6) ok |= 1;
  if (F(env, kGetStringLength, jint (*)(JNIEnv*, jstring))(env, s) == 5) ok |= 2;
  const jchar* u = F(env, kGetStringChars, const jchar* (*)(JNIEnv*, jstring, jboolean*))(env, s, 0);
  if (u[0] == 'h' && u[1] == 0xE9 && u[4] == 'o' && u[5] == 0) ok |= 4;
  F(env, kReleaseStringChars, void (*)(JNIEnv*, jstring, const jchar*))(env, s, u);
  char buf[16];
  F(env, kGetStringUTFRegion, void (*)(JNIEnv*, jstring, jint, jint, char*))(env, s, 1, 2, buf);
  if (seq(buf, "\xc3\xa9l")) ok |= 8;
  if (!NewStr(env, 0)) ok |= 16;
  return ok;
}

// ---- arrays ----
EXPORT jint t_arrays(JNIEnv* env) {
  jint ok = 0;
  jarray a = F(env, kNewIntArray, jarray (*)(JNIEnv*, jint))(env, 4);
  jint src[4];
  src[0] = 1;
  src[1] = 2;
  src[2] = 3;
  src[3] = 4;
  F(env, kSetIntArrayRegion, void (*)(JNIEnv*, jarray, jint, jint, const jint*))(env, a, 0, 4, src);
  if (F(env, kGetArrayLength, jint (*)(JNIEnv*, jarray))(env, a) == 4) ok |= 1;
  jint* p = F(env, kGetIntArrayElements, jint* (*)(JNIEnv*, jarray, jboolean*))(env, a, 0);
  if (p[0] + p[1] + p[2] + p[3] == 10) ok |= 2;
  p[2] = 30;
  F(env, kReleaseIntArrayElements, void (*)(JNIEnv*, jarray, jint*, jint))(env, a, p, 0);
  jint dst[2];
  F(env, kGetIntArrayRegion, void (*)(JNIEnv*, jarray, jint, jint, jint*))(env, a, 2, 2, dst);
  if (dst[0] == 30 && dst[1] == 4) ok |= 4;
  jarray b = F(env, kNewByteArray, jarray (*)(JNIEnv*, jint))(env, 8);
  unsigned char* q = F(env, kGetPrimitiveArrayCritical, unsigned char* (*)(JNIEnv*, jarray, jboolean*))(env, b, 0);
  if (q && q[0] == 0 && q[7] == 0) ok |= 8;
  F(env, kReleasePrimitiveArrayCritical, void (*)(JNIEnv*, jarray, void*, jint))(env, b, q, 0);
  jclass sc = FindClass(env, "java/lang/String");
  jarray oa = F(env, kNewObjectArray, jarray (*)(JNIEnv*, jint, jclass, jobject))(env, 3, sc, 0);
  F(env, kSetObjectArrayElement, void (*)(JNIEnv*, jarray, jint, jobject))(env, oa, 1, NewStr(env, "uno"));
  jstring e1 = F(env, kGetObjectArrayElement, jobject (*)(JNIEnv*, jarray, jint))(env, oa, 1);
  jstring e0 = F(env, kGetObjectArrayElement, jobject (*)(JNIEnv*, jarray, jint))(env, oa, 0);
  if (!e0 && UtfLen(env, e1) == 3) ok |= 16;
  return ok;
}

// ---- referencias, marcos, excepciones ----
EXPORT jint t_refs(JNIEnv* env) {
  jint ok = 0;
  jstring s = NewStr(env, "x");
  jobject g = F(env, kNewGlobalRef, jobject (*)(JNIEnv*, jobject))(env, s);
  if (F(env, kIsSameObject, jboolean (*)(JNIEnv*, jobject, jobject))(env, s, g)) ok |= 1;
  if (F(env, kGetObjectRefType, jint (*)(JNIEnv*, jobject))(env, s) == 1 &&
      F(env, kGetObjectRefType, jint (*)(JNIEnv*, jobject))(env, g) == 2)
    ok |= 2;
  DelLocal(env, s);
  if (UtfLen(env, g) == 1) ok |= 4;  // la global sigue viva
  F(env, kDeleteGlobalRef, void (*)(JNIEnv*, jobject))(env, g);
  if (F(env, kGetObjectRefType, jint (*)(JNIEnv*, jobject))(env, g) == 0) ok |= 8;
  F(env, kPushLocalFrame, jint (*)(JNIEnv*, jint))(env, 4);
  jstring inner = NewStr(env, "keep");
  NewStr(env, "drop");
  jstring kept = F(env, kPopLocalFrame, jobject (*)(JNIEnv*, jobject))(env, inner);
  if (UtfLen(env, kept) == 4) ok |= 16;
  if (!F(env, kExceptionCheck, jboolean (*)(JNIEnv*))(env) && !F(env, kExceptionOccurred, jobject (*)(JNIEnv*))(env)) ok |= 32;
  F(env, kExceptionClear, void (*)(JNIEnv*))(env);
  JavaVM* vm = 0;
  if (F(env, kGetJavaVM, jint (*)(JNIEnv*, JavaVM**))(env, &vm) == 0 && vm == g_vm) ok |= 64;
  return ok;
}

// ---- objetos, Bundle, campos, nonvirtual ----
EXPORT jint t_objects(JNIEnv* env) {
  jint ok = 0;
  jclass bc = FindClass(env, "android/os/Bundle");
  jobject b = F(env, kNewObject, jobject (*)(JNIEnv*, jclass, jmethodID, ...))(env, bc, MID(env, bc, "<init>", "()V"));
  jmethodID put = MID(env, bc, "putInt", "(Ljava/lang/String;I)V");
  jmethodID get = MID(env, bc, "getInt", "(Ljava/lang/String;)I");
  jstring k = NewStr(env, "npDataType");
  F(env, kCallVoidMethod, void (*)(JNIEnv*, jobject, jmethodID, ...))(env, b, put, k, 42);
  if (F(env, kCallIntMethod, jint (*)(JNIEnv*, jobject, jmethodID, ...))(env, b, get, k) == 42) ok |= 1;
  if (F(env, kCallNonvirtualIntMethod, jint (*)(JNIEnv*, jobject, jclass, jmethodID, ...))(env, b, bc, get, k) == 42) ok |= 2;
  jclass oc = F(env, kGetObjectClass, jclass (*)(JNIEnv*, jobject))(env, b);
  if (F(env, kIsSameObject, jboolean (*)(JNIEnv*, jobject, jobject))(env, oc, bc) &&
      F(env, kIsInstanceOf, jboolean (*)(JNIEnv*, jobject, jclass))(env, b, FindClass(env, "java/lang/Object")))
    ok |= 4;
  // campos de instancia y estaticos
  jclass tc = FindClass(env, "zw/Test");
  jobject t = F(env, kNewObject, jobject (*)(JNIEnv*, jclass, jmethodID, ...))(env, tc, MID(env, tc, "<init>", "(I)V"), 9);
  jfieldID fc = F(env, kGetFieldID, jfieldID (*)(JNIEnv*, jclass, const char*, const char*))(env, tc, "count", "I");
  if (F(env, kGetIntField, jint (*)(JNIEnv*, jobject, jfieldID))(env, t, fc) == 9) ok |= 8;  // lo puso el constructor
  F(env, kSetIntField, void (*)(JNIEnv*, jobject, jfieldID, jint))(env, t, fc, 123);
  jfieldID ff = F(env, kGetFieldID, jfieldID (*)(JNIEnv*, jclass, const char*, const char*))(env, tc, "ratio", "F");
  F(env, kSetFloatField, void (*)(JNIEnv*, jobject, jfieldID, jfloat))(env, t, ff, 0.25f);
  if (F(env, kCallIntMethod, jint (*)(JNIEnv*, jobject, jmethodID, ...))(env, t, MID(env, tc, "getCount", "()I")) == 123 &&
      F(env, kGetFloatField, jfloat (*)(JNIEnv*, jobject, jfieldID))(env, t, ff) == 0.25f)
    ok |= 16;
  jfieldID sf = F(env, kGetStaticFieldID, jfieldID (*)(JNIEnv*, jclass, const char*, const char*))(env, tc, "counter", "J");
  F(env, kSetStaticLongField, void (*)(JNIEnv*, jclass, jfieldID, jlong))(env, tc, sf, (jlong)5 << 33);
  if (F(env, kGetStaticLongField, jlong (*)(JNIEnv*, jclass, jfieldID))(env, tc, sf) == ((jlong)5 << 33)) ok |= 32;
  jclass gi = FindClass(env, "com/gameloft/android/ANMP/GloftZOHM/installer/GameInstaller");
  jfieldID pn = F(env, kGetStaticFieldID, jfieldID (*)(JNIEnv*, jclass, const char*, const char*))(env, gi, "mPreferencesName",
                                                                                                  "Ljava/lang/String;");
  jstring pv = F(env, kGetStaticObjectField, jobject (*)(JNIEnv*, jclass, jfieldID))(env, gi, pn);
  const char* p = F(env, kGetStringUTFChars, const char* (*)(JNIEnv*, jstring, jboolean*))(env, pv, 0);
  if (seq(p, "GamePrefs")) ok |= 64;
  F(env, kReleaseStringUTFChars, void (*)(JNIEnv*, jstring, const char*))(env, pv, p);
  return ok;
}

// ---- RegisterNatives + nativos llamados desde el host (reentrancia) ----
static jint native_twice(JNIEnv* env, jclass c, jint x) {
  (void)env;
  (void)c;
  return 2 * x;
}
struct NativeMethod {
  const char* name;
  const char* sig;
  void* fn;
};
EXPORT jint t_register(JNIEnv* env) {
  struct NativeMethod m[1];
  m[0].name = "twice";
  m[0].sig = "(I)I";
  m[0].fn = (void*)native_twice;
  return F(env, kRegisterNatives, jint (*)(JNIEnv*, jclass, const struct NativeMethod*, jint))(env, FindClass(env, "zw/Test"), m, 1);
}
EXPORT jint Java_zw_Test_nativeLeaf(JNIEnv* env, jclass c, jint x) {
  (void)env;
  (void)c;
  return x + 1;
}
// a + len(s) + Test.cb(a)   (cb, en el host, llama a su vez a nativeLeaf)
EXPORT jint Java_zw_Test_nativeSum(JNIEnv* env, jclass c, jint a, jstring s) {
  jint n = UtfLen(env, s);
  jint cb = F(env, kCallStaticIntMethod, jint (*)(JNIEnv*, jclass, jmethodID, ...))(env, c, SMID(env, c, "cb", "(I)I"), a);
  return a + n + cb;
}

// ---- AudioTrack como vox::DriverAndroid::UpdateThreadedAT ----
EXPORT jint t_audio(JNIEnv* env) {
  jclass at = FindClass(env, "android/media/AudioTrack");
  jint rate = F(env, kCallStaticIntMethod, jint (*)(JNIEnv*, jclass, jmethodID, ...))(
      env, at, SMID(env, at, "getNativeOutputSampleRate", "(I)I"), 3);
  jint minb = F(env, kCallStaticIntMethod, jint (*)(JNIEnv*, jclass, jmethodID, ...))(
      env, at, SMID(env, at, "getMinBufferSize", "(III)I"), rate, 12, 2);
  jmethodID ctor = MID(env, at, "<init>", "(IIIIII)V");
  jmethodID play = MID(env, at, "play", "()V"), stop = MID(env, at, "stop", "()V"), rel = MID(env, at, "release", "()V");
  jmethodID write = MID(env, at, "write", "([BII)I");
  F(env, kPushLocalFrame, jint (*)(JNIEnv*, jint))(env, 2);
  jobject t = F(env, kNewObject, jobject (*)(JNIEnv*, jclass, jmethodID, ...))(env, at, ctor, 3, rate, 12, 2, 4096, 1);
  jobject g = F(env, kNewGlobalRef, jobject (*)(JNIEnv*, jobject))(env, t);
  F(env, kCallNonvirtualVoidMethod, void (*)(JNIEnv*, jobject, jclass, jmethodID, ...))(env, g, at, play);
  jarray arr = F(env, kNewByteArray, jarray (*)(JNIEnv*, jint))(env, 4096);
  unsigned char* p = F(env, kGetPrimitiveArrayCritical, unsigned char* (*)(JNIEnv*, jarray, jboolean*))(env, arr, 0);
  for (int i = 0; i < 4096; ++i) p[i] = (unsigned char)i;
  F(env, kReleasePrimitiveArrayCritical, void (*)(JNIEnv*, jarray, void*, jint))(env, arr, p, 0);
  jint n = F(env, kCallNonvirtualIntMethod, jint (*)(JNIEnv*, jobject, jclass, jmethodID, ...))(env, g, at, write, arr, 0, 4096);
  F(env, kCallNonvirtualVoidMethod, void (*)(JNIEnv*, jobject, jclass, jmethodID, ...))(env, g, at, stop);
  F(env, kCallNonvirtualVoidMethod, void (*)(JNIEnv*, jobject, jclass, jmethodID, ...))(env, g, at, rel);
  F(env, kDeleteGlobalRef, void (*)(JNIEnv*, jobject))(env, g);
  F(env, kPopLocalFrame, jobject (*)(JNIEnv*, jobject))(env, 0);
  return (rate == 44100 && minb >= 4096) ? n : -1;
}

// ---- hilo guest: AttachCurrentThread / DetachCurrentThread ----
EXPORT unsigned t_thread(void* arg) {
  JNIEnv* env = 0;
  if (F(g_vm, 4, jint (*)(JavaVM*, JNIEnv**, void*))(g_vm, &env, 0) != 0 || !env) return 1;
  jstring s = NewStr(env, (const char*)arg);
  jint n = UtfLen(env, s);
  F(g_vm, 5, jint (*)(JavaVM*))(g_vm);
  return (unsigned)n + 100;
}
EXPORT void* get_env_of_thread(void) {
  JNIEnv* env = 0;
  F(g_vm, 6, jint (*)(JavaVM*, JNIEnv**, jint))(g_vm, &env, 0x10006);
  return env;
}
