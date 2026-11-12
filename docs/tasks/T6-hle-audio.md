# T6 — Audio: backend SDL2 para `android.media.AudioTrack` (+ stubs OpenSL)

Lee `docs/PLAN.md`, `docs/ANALYSIS.md` **§8 (opción A)**, `docs/CONTRACTS.md`. Repo en `main`.

> Cambio respecto al plan inicial: T1 confirmó que el juego emite audio por `android.media.AudioTrack` vía JNI
> (`s_androidAPILevel = 3` → `_InitAT`), no por OpenSL. Seguimos el comportamiento original.

## Objetivo
Un backend de audio en C++ puro (sin conocer JNI) que T7 envolverá como la clase Java emulada `android/media/AudioTrack`.

## Requisitos
- API en `src/hle/audio/audio_track.h` (namespace `zw::audio`), por ejemplo:
  `int native_output_sample_rate()` → 44100; `int min_buffer_size(rate, channelCfg, encoding)` → ≥ 8192 bytes;
  `class Track { Track(streamType, rate, channelCfg, encoding, bufferBytes, mode); void play(); void pause(); void stop(); void release(); int write(const int16_t* pcm, int bytes); int play_state(); }`.
  Documenta la API en `docs/CONTRACTS.md` en una sección **"Audio (T6)"** (solo esa sección; avisa con `ask` si T3 aún no creó el archivo).
- Salida SDL2 (`SDL_OpenAudioDevice` + `SDL_AudioStream` o cola), PCM 16-bit estéreo 44.1 kHz (convertir si `channelCfg`/rate difieren).
- `write` encola en un ring buffer y **bloquea** cuando hay más de ~100 ms en cola (§8 de ANALYSIS); nunca bloquea indefinidamente si el dispositivo está pausado/cerrado.
- Volumen maestro y mute (para `nativeVoxMute` / menú de T9), pausa/reanudar global (minimizar ventana).
- **OpenSL ES**: los 4 imports (`slCreateEngine`, `SL_IID_ENGINE/PLAY/BUFFERQUEUE`) deben resolverse para que el loader no falle: `slCreateEngine` devuelve `SL_RESULT_FEATURE_UNSUPPORTED` y los `SL_IID_*` apuntan a structs guest válidos. Registra estos HLE con la API de `CONTRACTS.md`.

## Entregables
`src/hle/audio/`, tests en `tests/hle_audio/` (driver SDL `dummy` o `disk`: escribir 1 s de seno 440 Hz por la API, comprobar duración/bloqueo, pausa/reanudar, volumen 0).

## Evidencia
Tests en verde con salida pegada; firma final de la API.

## Restricciones
Edita solo `src/hle/audio/`, `tests/hle_audio/`, su CMake y tu sección de `docs/CONTRACTS.md`. Commits `zombiewood(T6): ...`, sin push.
