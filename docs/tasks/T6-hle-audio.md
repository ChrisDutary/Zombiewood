# T6 — Audio: SDL2 backend for `android.media.AudioTrack` (+ OpenSL stubs)

> Historical task spec (November 2026).

Read `docs/PLAN.md`, `docs/ANALYSIS.md` **§8 (option A)**, `docs/CONTRACTS.md`. Repo on `main`.

> Change from the initial plan: T1 confirmed the game outputs audio through `android.media.AudioTrack` via JNI
> (`s_androidAPILevel = 3` → `_InitAT`), not through OpenSL. We follow the original behavior.

## Goal
A pure C++ audio backend (unaware of JNI) that T7 will wrap as the emulated Java class `android/media/AudioTrack`.

## Requirements
- API in `src/hle/audio/audio_track.h` (namespace `zw::audio`), for example:
  `int native_output_sample_rate()` → 44100; `int min_buffer_size(rate, channelCfg, encoding)` → ≥ 8192 bytes;
  `class Track { Track(streamType, rate, channelCfg, encoding, bufferBytes, mode); void play(); void pause(); void stop(); void release(); int write(const int16_t* pcm, int bytes); int play_state(); }`.
  Document the API in `docs/CONTRACTS.md` in an **"Audio (T6)"** section (only that section; `ask` if T3 has not created the file yet).
- SDL2 output (`SDL_OpenAudioDevice` + `SDL_AudioStream` or a queue), 16-bit stereo PCM at 44.1 kHz (convert if `channelCfg`/rate differ).
- `write` queues into a ring buffer and **blocks** when more than ~100 ms is queued (ANALYSIS §8); it never blocks forever if the device is paused/closed.
- Master volume and mute (for `nativeVoxMute` / T9's menu), global pause/resume (window minimized).
- **OpenSL ES**: the 4 imports (`slCreateEngine`, `SL_IID_ENGINE/PLAY/BUFFERQUEUE`) must resolve so the loader does not fail: `slCreateEngine` returns `SL_RESULT_FEATURE_UNSUPPORTED` and the `SL_IID_*` point to valid guest structs. Register these HLE entries with the `CONTRACTS.md` API.

## Deliverables
`src/hle/audio/`, tests in `tests/hle_audio/` (SDL `dummy` or `disk` driver: write 1 s of a 440 Hz sine through the API, check duration/blocking, pause/resume, volume 0).

## Proof
Green tests with their output pasted; the final API signature.

## Constraints
Edit only `src/hle/audio/`, `tests/hle_audio/`, their CMake and your section of `docs/CONTRACTS.md`. Commits `zombiewood(T6): ...`, no push.
