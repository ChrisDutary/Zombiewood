# T5 — HLE OpenGL ES 2.0 → ANGLE

Lee `docs/PLAN.md`, `docs/ANALYSIS.md` (sección gráficos), `docs/CONTRACTS.md`, `docs/BUILD.md`. Repo en `main`.

## Objetivo
Implementar los 99 imports `gl*` de `libGLESv2` (más los `*OES` que consulte el juego vía `eglGetProcAddress`/extensiones si T1 lo indica)
llamando a ANGLE en el contexto GLES2 que crea el frontend (T2).

## Requisitos
- Traducción de punteros guest→host en todo parámetro puntero (`glBufferData`, `glTexImage2D`, `glShaderSource` — array de punteros guest a strings —, `glGet*v`, `glUniform*v`, `glVertexAttribPointer`).
- **Client-side vertex arrays**: si `glVertexAttribPointer`/`glDrawElements` reciben punteros (no offsets) con buffer 0 bound, los punteros son direcciones guest: tradúcelos al dibujar (ANGLE soporta client arrays en GLES2; si hay problemas, sube a un VBO temporal).
- `glGetString`: devuelve strings en memoria guest (cacheadas), con un `GL_EXTENSIONS` que **anuncie solo lo que de verdad soportamos**.
- Texturas comprimidas: según T1, implementa conversión por software de los formatos que ANGLE no soporte (p. ej. PVRTC/ATC → RGBA8; ETC1 usa la extensión de ANGLE si existe o decodifica). `glCompressedTexImage3DOES`: implementar o degradar según uso real.
- Shaders GLSL ES 1.00: pasar tal cual a ANGLE; loguea errores de compilación con el código fuente.
- Seguridad del contexto: todas las llamadas GL deben ocurrir en el hilo que posee el contexto (el hilo de render guest que llama a `step/nativeRender`). Si T1 muestra GL desde otros hilos (`getNumExtraContext`/`setNumExtraContext` sugiere contextos compartidos para carga), implementa contextos compartidos de ANGLE por hilo.
- Modo debug `ZW_GL_CHECK=1`: `glGetError` tras cada llamada con nombre de función.

## Entregables
`src/hle/gles/`, tests en `tests/hle_gles/` (contexto ANGLE offscreen/pbuffer: subir textura, compilar shader, dibujar un triángulo con client arrays desde memoria guest, leer píxeles).

## Evidencia
Tests en verde con salida; lista de los 99 imports con su handler (ninguno sin implementar salvo justificados en el reporte).

## Restricciones
Edita solo `src/hle/gles/`, `tests/hle_gles/` y su CMake. Si necesitas algo del frontend (acceso al contexto/ventana) `ask`. Commits `zombiewood(T5): ...`, sin push.
