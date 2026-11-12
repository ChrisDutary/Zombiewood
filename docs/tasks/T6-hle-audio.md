# T6 — HLE OpenSL ES → SDL2 audio

Lee `docs/PLAN.md`, `docs/ANALYSIS.md` (sección audio), `docs/CONTRACTS.md`. Repo en `main`.

## Objetivo
Implementar `slCreateEngine` y los objetos/interfaces OpenSL ES que usa el juego (`SL_IID_ENGINE`, `SL_IID_PLAY`, `SL_IID_BUFFERQUEUE`, y cualquiera
adicional que T1 identifique: volume, outputmix…) como **objetos guest**: estructuras con vtables cuyos punteros a función son thunks HLE,
siguiendo el layout C de OpenSL ES 1.0.1 (`SLObjectItf` = puntero a puntero a struct de punteros a función, 32-bit).
`SL_IID_*` son variables importadas: deben resolver a structs `SLInterfaceID` en memoria guest.

## Requisitos
- Player PCM con buffer queue: `Enqueue` copia o referencia el buffer guest; un `SDL_AudioStream`/callback consume y, al terminar cada buffer, invoca el callback guest de `RegisterCallback` vía `call_guest` **desde un hilo guest dedicado** (no desde el hilo de audio de SDL directamente si el contrato de T3 lo prohíbe).
- `SetPlayState`, `GetState`, `Clear`, `Destroy`, `Realize`, `GetInterface`; formatos PCM 8/16 bits, mono/estéreo, sample rate del data locator con resampling por SDL.
- Volumen/mute global configurable (para `nativeVoxMute` y el menú de opciones de T9).

## Entregables
`src/hle/audio/`, tests en `tests/hle_audio/` (simular desde código guest de prueba o llamadas directas: encolar seno 440 Hz, verificar que se llama el callback N veces; modo sin dispositivo con driver `dummy` de SDL para CI).

## Evidencia
Tests en verde con salida; lista de imports OpenSL cubiertos.

## Restricciones
Edita solo `src/hle/audio/`, `tests/hle_audio/` y su CMake. Commits `zombiewood(T6): ...`, sin push.
