# T5 — OpenGL ES 2.0 HLE → ANGLE

> Historical task spec (November 2026).

Read `docs/PLAN.md`, `docs/ANALYSIS.md` (graphics section), `docs/CONTRACTS.md`, `docs/BUILD.md`. Repo on `main`.

## Goal
Implement the 99 `gl*` imports from `libGLESv2` (plus any `*OES` functions the game queries via `eglGetProcAddress`/extensions, if T1 says so)
by calling ANGLE in the GLES2 context created by the frontend (T2).

## Requirements
- Guest→host pointer translation for every pointer parameter (`glBufferData`, `glTexImage2D`, `glShaderSource` — an array of guest pointers to strings —, `glGet*v`, `glUniform*v`, `glVertexAttribPointer`).
- **Client-side vertex arrays**: if `glVertexAttribPointer`/`glDrawElements` receive pointers (not offsets) with buffer 0 bound, those pointers are guest addresses: translate them at draw time (ANGLE supports client arrays in GLES2; if that causes problems, upload to a temporary VBO).
- `glGetString`: return strings in guest memory (cached), with a `GL_EXTENSIONS` that **advertises only what we really support**.
- Compressed textures (ANALYSIS §7): the data = 1,918 ETC1 (KTX) + 19 PVRTC4; the engine decompresses PVRTC in software if
  `GL_IMG_texture_compression_pvrtc` is not advertised. Advertise only what §7 recommends (ETC1 if ANGLE supports it; otherwise decode
  ETC1 in software to RGB8). To be safe, if PVRTC (0x8C00–0x8C03) arrives, decode it in software. Also, following T1, implement software conversion of any format ANGLE does not support (e.g. PVRTC/ATC → RGBA8; ETC1 uses ANGLE's extension if present, otherwise decode). `glCompressedTexImage3DOES`: implement or degrade according to real usage.
- GLSL ES 1.00 shaders: pass them to ANGLE unchanged; log compile errors along with the source code.
- Context safety: every GL call must happen on the thread that owns the context (the guest render thread that calls `step/nativeRender`). If T1 shows GL calls from other threads (`getNumExtraContext`/`setNumExtraContext` hints at shared contexts for loading), implement per-thread ANGLE shared contexts.
- Debug mode `ZW_GL_CHECK=1`: `glGetError` after each call, with the function name.

## Deliverables
`src/hle/gles/`, tests in `tests/hle_gles/` (offscreen/pbuffer ANGLE context: upload a texture, compile a shader, draw a triangle with client arrays from guest memory, read back the pixels).

## Proof
Green tests with their output; the list of the 99 imports with their handlers (none left unimplemented unless justified in the report).

## Constraints
Edit only `src/hle/gles/`, `tests/hle_gles/` and their CMake. If you need something from the frontend (context/window access), `ask`. Commits `zombiewood(T5): ...`, no push.
