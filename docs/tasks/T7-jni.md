# T7 — JNI and the emulated Java layer

> Historical task spec (November 2026).

Read `docs/PLAN.md`, `docs/ANALYSIS.md` (startup and native→Java sections), `docs/CONTRACTS.md`. Repo on `main`.

## Goal
Provide a fake `JavaVM*` and `JNIEnv*` in guest memory (32-bit JNI 1.6 function tables with ~230 entries, each an HLE thunk) and
a layer of "emulated Java classes" in C++ that answers whatever the `.so` asks for.

## Requirements
- `JavaVM`: GetEnv, AttachCurrentThread(AsDaemon), DetachCurrentThread.
- `JNIEnv`: FindClass, GetMethodID/GetStaticMethodID, GetFieldID/GetStaticFieldID, Call*Method(V/A) and CallStatic* (every return-type variant, with variadic args and a guest `va_list`), New/GetStringUTFChars/Release, NewObject, arrays (New*Array, Get/Set*ArrayRegion, Get/Release*ArrayElements, GetArrayLength), global/local references (opaque handles), ExceptionCheck/Occurred/Clear (no exceptions), RegisterNatives, Get/Set*Field.
- Java objects represented as handles to C++ objects (`jobject` = an index into a table). Java strings with real contents.
- **Emulated classes** according to T1's table: implement with reasonable values those that affect the game (Device/model/GPU/CPU, paths, language, screen size, `SUtils` videos → mark finished immediately or play them if feasible, `Game` callbacks, `GL2JNILib`, `DataSharing`, `NetworkStateReceiver` = no network). Social/IAB/PushNotification/GDRMPolicy/GameInstaller: neutral "service unavailable / offline" stubs; we do **not** implement purchases or online services.
- **`android/media/AudioTrack`** (ANALYSIS §8, option A): an emulated class that delegates to T6's C++ API (`zw::audio::Track`,
  see the "Audio (T6)" section of `CONTRACTS.md`). Includes the `CallNonvirtual*Method`, `NewByteArray`, `Get/ReleasePrimitiveArrayCritical`
  and `PushLocalFrame/PopLocalFrame` calls used by `UpdateThreadedAT`. If T6 has not published its API yet, program against a minimal
  interface and coordinate via `ask`.
- The other framework classes listed in ANALYSIS §5.4 (those the native code uses directly).
- Unimplemented method: log once with class+method+signature and return the default value (0/null) so T8 knows what is missing.
- API for T8: `jni::call_native(name, args...)` that looks up `Java_...` in the .so and calls it with `(JNIEnv*, jclass/jobject, args)` on the current thread.

## Deliverables
`src/jni/`, tests in `tests/jni/`.

## Proof
Green tests; `zombiewood --load-only` calling `JNI_OnLoad` + `Game.nativeInit` (if T1 says it is safe without GL), logging every FindClass/GetMethodID the game makes and whether it was covered.

## Constraints
Edit only `src/jni/`, `tests/jni/` and their CMake. Commits `zombiewood(T7): ...`, no push.
