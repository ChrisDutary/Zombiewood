# T7 — JNI y capa Java emulada

Lee `docs/PLAN.md`, `docs/ANALYSIS.md` (secciones arranque y nativo→Java), `docs/CONTRACTS.md`. Repo en `main`.

## Objetivo
Proveer un `JavaVM*` y `JNIEnv*` falsos en memoria guest (tablas de función de JNI 1.6 de 32 bits con ~230 entradas, cada una un thunk HLE) y
una capa de "clases Java emuladas" en C++ que responda a lo que el `.so` pide.

## Requisitos
- `JavaVM`: GetEnv, AttachCurrentThread(AsDaemon), DetachCurrentThread.
- `JNIEnv`: FindClass, GetMethodID/GetStaticMethodID, GetFieldID/GetStaticFieldID, Call*Method(V/A) y CallStatic* (todas las variantes de tipo de retorno, con args variádicos y `va_list` guest), New/GetStringUTFChars/Release, NewObject, arrays (New*Array, Get/Set*ArrayRegion, Get/Release*ArrayElements, GetArrayLength), referencias globales/locales (handles opacos), ExceptionCheck/Occurred/Clear (sin excepciones), RegisterNatives, Get/Set*Field.
- Objetos Java representados como handles a objetos C++ (`jobject` = índice en tabla). Strings Java con contenido real.
- **Clases emuladas** según la tabla de T1: implementa con valores razonables las que afectan al juego (Device/model/GPU/CPU, rutas, idioma, tamaño de pantalla, `SUtils` vídeos → marcar terminado al instante o reproducir si viable, `Game` callbacks, `GL2JNILib`, `DataSharing`, `NetworkStateReceiver` = sin red). Social/IAB/PushNotification/GDRMPolicy/GameInstaller: stubs neutros de "servicio no disponible / sin conexión"; **no** implementamos compras ni servicios online.
- **`android/media/AudioTrack`** (ANALYSIS §8, opción A): clase emulada que delega en la API C++ de T6 (`zw::audio::Track`,
  ver sección "Audio (T6)" de `CONTRACTS.md`). Incluye `CallNonvirtual*Method`, `NewByteArray`, `Get/ReleasePrimitiveArrayCritical`,
  `PushLocalFrame/PopLocalFrame` que usa `UpdateThreadedAT`. Si T6 aún no publicó su API, programa contra una interfaz mínima y
  coordina con `ask`.
- Otras clases del framework que lista ANALYSIS §5.4 (las que el nativo usa directamente).
- Método no implementado: log una vez con clase+método+firma y retorno por defecto (0/null) para que T8 sepa qué falta.
- API para T8: `jni::call_native(name, args...)` que busca `Java_...` en el .so y la llama con `(JNIEnv*, jclass/jobject, args)` en el hilo actual.

## Entregables
`src/jni/`, tests en `tests/jni/`.

## Evidencia
Tests en verde; `zombiewood --load-only` llamando `JNI_OnLoad` + `Game.nativeInit` (si T1 indica que es seguro sin GL) con log de cada FindClass/GetMethodID que hace el juego y si quedó cubierto.

## Restricciones
Edita solo `src/jni/`, `tests/jni/` y su CMake. Commits `zombiewood(T7): ...`, sin push.
