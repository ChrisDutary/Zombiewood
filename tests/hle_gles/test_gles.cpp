// Tests de zw_hle_gles (T5). Contexto ANGLE offscreen (pbuffer EGL, GLES2 pedido) y llamadas a los imports
// gl* a traves de su stub guest (call_guest -> svc -> handler FAST con marshalling softfp), con todos los
// datos (vertices, indices, texturas, cadenas de shader, salidas de glGet*) en memoria guest.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "hle/gles/gles.h"
#include "hle/gles/texdecode.h"

using namespace zw;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    ++g_checks;                                                      \
    if (!(cond)) {                                                   \
      ++g_failed;                                                    \
      std::printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

// Llama al import `n` como lo haria el juego (por su stub guest).
template <class R = std::uint32_t, class... A>
static R G(const char* n, A... a) {
  return call_guest<R>(hle::StubFor(n), a...);
}

static gaddr GAlloc(std::size_t n) { return GuestMemory::Calloc(static_cast<gsize>(n), 1); }
template <class T>
static gaddr GPut(const std::vector<T>& v) {
  gaddr p = GAlloc(v.size() * sizeof(T));
  std::memcpy(g2h(p), v.data(), v.size() * sizeof(T));
  return p;
}

// Los 99 imports gl* del .so (docs/imports.json, category = gles2).
static const char* kImports[] = {
    "glActiveTexture", "glAttachShader", "glBindBuffer", "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture",
    "glBlendColor", "glBlendEquation", "glBlendFunc", "glBufferData", "glBufferSubData", "glCheckFramebufferStatus",
    "glClear", "glClearColor", "glClearDepthf", "glClearStencil", "glColorMask", "glCompileShader",
    "glCompressedTexImage2D", "glCompressedTexImage3DOES", "glCompressedTexSubImage2D", "glCompressedTexSubImage3DOES",
    "glCopyTexSubImage2D", "glCreateProgram", "glCreateShader", "glCullFace", "glDeleteBuffers", "glDeleteFramebuffers",
    "glDeleteProgram", "glDeleteRenderbuffers", "glDeleteShader", "glDeleteTextures", "glDepthFunc", "glDepthMask",
    "glDepthRangef", "glDisable", "glDisableVertexAttribArray", "glDrawArrays", "glDrawElements", "glEnable",
    "glEnableVertexAttribArray", "glFinish", "glFlush", "glFramebufferRenderbuffer", "glFramebufferTexture2D",
    "glFrontFace", "glGenBuffers", "glGenFramebuffers", "glGenRenderbuffers", "glGenTextures", "glGenerateMipmap",
    "glGetActiveAttrib", "glGetActiveUniform", "glGetAttribLocation", "glGetError", "glGetFloatv", "glGetIntegerv",
    "glGetProgramInfoLog", "glGetProgramiv", "glGetShaderInfoLog", "glGetShaderSource", "glGetShaderiv", "glGetString",
    "glGetUniformLocation", "glLineWidth", "glLinkProgram", "glMapBufferOES", "glPixelStorei", "glPolygonOffset",
    "glReadPixels", "glRenderbufferStorage", "glSampleCoverage", "glScissor", "glShaderSource", "glStencilFunc",
    "glStencilMask", "glStencilOp", "glTexImage2D", "glTexImage3DOES", "glTexParameterf", "glTexParameteri",
    "glTexSubImage2D", "glTexSubImage3DOES", "glUniform1f", "glUniform1fv", "glUniform1i", "glUniform1iv",
    "glUniform2fv", "glUniform2iv", "glUniform3fv", "glUniform3iv", "glUniform4fv", "glUniform4iv",
    "glUniformMatrix4fv", "glUnmapBufferOES", "glUseProgram", "glVertexAttrib4f", "glVertexAttribPointer", "glViewport",
};

static const char* kVS =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_col;\n"
    "attribute vec2 a_uv;\n"
    "varying lowp vec4 v_col;\n"
    "varying mediump vec2 v_uv;\n"
    "void main() { v_col = a_col; v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char* kFSBody =
    "uniform lowp sampler2D u_tex;\n"
    "uniform float u_mix;\n"
    "varying lowp vec4 v_col;\n"
    "varying mediump vec2 v_uv;\n"
    "void main() { gl_FragColor = mix(v_col, texture2D(u_tex, v_uv), u_mix); }\n";

static gaddr g_name_buf = 0;
static gaddr Str(const char* s) { return mem::StrDup(s); }

// Compila un shader pasando `parts` como varias cadenas guest (como hace Glitch: #defines + cuerpo).
// lengths: -1 = terminada en NUL, si no la longitud exacta.
static std::uint32_t Shader(GLenum type, const std::vector<std::string>& parts, bool use_lengths, bool expect_ok = true) {
  const std::uint32_t s = G("glCreateShader", type);
  std::vector<std::uint32_t> ptrs, lens;
  for (const auto& p : parts) {
    ptrs.push_back(mem::StrDup(p));
    lens.push_back(use_lengths ? static_cast<std::uint32_t>(p.size()) : 0xFFFFFFFFu);
  }
  G<void>("glShaderSource", s, static_cast<int>(parts.size()), GPut(ptrs), use_lengths ? GPut(lens) : gaddr{0});
  G<void>("glCompileShader", s);
  const gaddr st = GAlloc(4);
  G<void>("glGetShaderiv", s, GL_COMPILE_STATUS, st);
  CHECK((mem::R32(st) == 1) == expect_ok);
  if (!expect_ok) {
    const gaddr len = GAlloc(4), log = GAlloc(512);
    G<void>("glGetShaderInfoLog", s, 512, len, log);
    CHECK(mem::R32(len) > 0 && !mem::ReadCStr(log).empty());
    std::printf("  (info log esperado del shader roto: %s)\n", mem::ReadCStr(log).c_str());
  }
  return s;
}

struct Px { int r, g, b, a; };
static Px ReadPx(int x, int y) {
  const gaddr p = GAlloc(4);
  G<void>("glReadPixels", x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
  const auto* b = g2h<std::uint8_t>(p);
  return {b[0], b[1], b[2], b[3]};
}
static bool Near(Px p, int r, int g, int b, int tol = 2) {
  return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  log::Init();
  GuestMemory::Init();
  hle::gles::Register();

  // --- 1. los 99 imports tienen handler
  std::printf("[1] registro\n");
  CHECK(sizeof(kImports) / sizeof(kImports[0]) == 99);
  CHECK(hle::gles::ImportNames().size() == 99);
  int missing = 0;
  for (const char* n : kImports) {
    if (!hle::IsRegistered(n)) {
      ++missing;
      std::printf("  sin handler: %s\n", n);
    }
  }
  CHECK(missing == 0);
  std::printf("  %zu/99 imports gl* con handler\n", sizeof(kImports) / sizeof(kImports[0]) - missing);

  // --- contexto ANGLE offscreen
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  CHECK(dpy != EGL_NO_DISPLAY && eglInitialize(dpy, nullptr, nullptr));
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                             EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
  EGLConfig cfg = nullptr;
  EGLint ncfg = 0;
  CHECK(eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) && ncfg == 1);
  const EGLint pb_attr[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  CHECK(surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT);

  // --- 2. sin contexto actual: la llamada se descarta sin romper nada
  std::printf("[2] llamada sin contexto\n");
  CHECK(G("glGetError") == GL_NO_ERROR);
  if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
    std::printf("  eglMakeCurrent fallo 0x%x\n", eglGetError());
    return 1;
  }
  // Hilo que nunca tuvo contexto: se ignora (log de error) y no revienta.
  std::thread([] {
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    threads::DetachCurrent();
  }).join();

  try {
    // --- 3. cadenas en memoria guest + lista blanca de extensiones
    std::printf("[3] glGetString / glGetIntegerv\n");
    const gaddr ver = G("glGetString", GL_VERSION);
    CHECK(GuestMemory::IsMapped(ver) && mem::ReadCStr(ver).rfind("OpenGL ES 2.0", 0) == 0);
    CHECK(G("glGetString", GL_VERSION) == ver);  // cacheada
    CHECK(mem::ReadCStr(G("glGetString", GL_SHADING_LANGUAGE_VERSION)) == "OpenGL ES GLSL ES 1.00");
    CHECK(mem::ReadCStr(G("glGetString", GL_RENDERER)) == "Adreno (TM) 330");
    const std::string ext = mem::ReadCStr(G("glGetString", GL_EXTENSIONS));
    std::printf("  VERSION=%s\n  EXTENSIONS=%s\n", mem::ReadCStr(ver).c_str(), ext.c_str());
    CHECK(ext.find("GL_OES_compressed_ETC1_RGB8_texture") != std::string::npos);
    for (const char* no : {"pvrtc", "ATC", "GL_OES_texture_3D", "vertex_array_object", "get_program_binary", "dxt", "s3tc"})
      CHECK(ext.find(no) == std::string::npos);
    const auto& caps = hle::gles::GetCaps();
    std::printf("  ANGLE ES %d.%d, etc1_native=%d etc2=%d mapbuffer=%d\n", caps.angle_major, caps.angle_minor,
                caps.etc1_native, caps.etc2, caps.mapbuffer);
    const gaddr iv = GAlloc(64);
    G<void>("glGetIntegerv", GL_NUM_COMPRESSED_TEXTURE_FORMATS, iv);
    CHECK(mem::R32(iv) == 1);
    G<void>("glGetIntegerv", GL_COMPRESSED_TEXTURE_FORMATS, iv);
    CHECK(mem::R32(iv) == GL_ETC1_RGB8_OES && mem::R32(iv + 4) == 0);
    G<void>("glGetIntegerv", GL_MAX_TEXTURE_SIZE, iv);
    CHECK(static_cast<std::int32_t>(mem::R32(iv)) >= 2048);
    G<void>("glGetFloatv", GL_ALIASED_LINE_WIDTH_RANGE, iv);
    CHECK(mem::Read<float>(iv) >= 1.0f);
    CHECK(G("glGetString", 0x1234) == 0 && G("glGetError") == GL_INVALID_ENUM && G("glGetError") == GL_NO_ERROR);

    // --- 4. errores y ZW_GL_CHECK
    std::printf("[4] glGetError / ZW_GL_CHECK\n");
    G<void>("glEnable", 0x1234);
    CHECK(G("glGetError") == GL_INVALID_ENUM);
    CHECK(G("glGetError") == GL_NO_ERROR);
    hle::gles::SetGlCheck(true);
    G<void>("glEnable", 0x1234);  // log: "GL error 0x0500 en glEnable"
    CHECK(G("glGetError") == GL_INVALID_ENUM);  // el error capturado se conserva para el guest
    CHECK(G("glGetError") == GL_NO_ERROR);

    // --- 5. shaders (varias cadenas guest, con y sin longitudes) + shader roto
    std::printf("[5] shaders\n");
    const std::uint32_t vs = Shader(GL_VERTEX_SHADER, {kVS}, false);
    const std::uint32_t fs = Shader(GL_FRAGMENT_SHADER, {"#define ZW_TEST 1\n", "precision mediump float;\n", kFSBody}, true);
    Shader(GL_FRAGMENT_SHADER, {"void main() { gl_FragColor = undefined_thing; }\n"}, false, false);
    CHECK(G("glGetError") == GL_NO_ERROR);
    const std::uint32_t prog = G("glCreateProgram");
    G<void>("glAttachShader", prog, vs);
    G<void>("glAttachShader", prog, fs);
    G<void>("glLinkProgram", prog);
    const gaddr st = GAlloc(4);
    G<void>("glGetProgramiv", prog, GL_LINK_STATUS, st);
    CHECK(mem::R32(st) == 1);
    G<void>("glUseProgram", prog);
    const int a_pos = G<int>("glGetAttribLocation", prog, Str("a_pos"));
    const int a_col = G<int>("glGetAttribLocation", prog, Str("a_col"));
    const int a_uv = G<int>("glGetAttribLocation", prog, Str("a_uv"));
    const int u_mix = G<int>("glGetUniformLocation", prog, Str("u_mix"));
    const int u_tex = G<int>("glGetUniformLocation", prog, Str("u_tex"));
    CHECK(a_pos >= 0 && a_col >= 0 && a_uv >= 0 && u_mix >= 0 && u_tex >= 0);
    G<void>("glGetProgramiv", prog, GL_ACTIVE_UNIFORMS, st);
    CHECK(mem::R32(st) == 2);
    {
      g_name_buf = GAlloc(64);
      const gaddr len = GAlloc(4), sz = GAlloc(4), ty = GAlloc(4);
      bool saw_mix = false;
      for (std::uint32_t i = 0; i < 2; ++i) {
        G<void>("glGetActiveUniform", prog, i, 64, len, sz, ty, g_name_buf);
        if (mem::ReadCStr(g_name_buf) == "u_mix") saw_mix = mem::R32(ty) == GL_FLOAT && mem::R32(sz) == 1;
      }
      CHECK(saw_mix);
      G<void>("glGetActiveAttrib", prog, 0u, 64, len, sz, ty, g_name_buf);
      CHECK(mem::R32(len) > 0 && mem::ReadCStr(g_name_buf).rfind("a_", 0) == 0);
      const gaddr src = GAlloc(1024);
      G<void>("glGetShaderSource", fs, 1024, len, src);
      CHECK(mem::ReadCStr(src).find("ZW_TEST") != std::string::npos);
    }
    G<void>("glUniform1i", u_tex, 0);

    // Geometria: triangulo que cubre todo el viewport.
    const gaddr pos = GPut(std::vector<float>{-1, -1, 3, -1, -1, 3});
    const gaddr uv = GPut(std::vector<float>{0, 0, 2, 0, 0, 2});
    G<void>("glViewport", 0, 0, 64, 64);
    G<void>("glDisable", GL_DEPTH_TEST);
    G<void>("glClearColor", 0.0f, 0.0f, 0.0f, 1.0f);

    // --- 6. arrays de cliente desde memoria guest + glDrawArrays + glReadPixels
    std::printf("[6] client arrays: glDrawArrays\n");
    G<void>("glBindBuffer", GL_ARRAY_BUFFER, 0u);
    G<void>("glEnableVertexAttribArray", a_pos);
    G<void>("glVertexAttribPointer", a_pos, 2, GL_FLOAT, 0, 0, pos);
    G<void>("glEnableVertexAttribArray", a_col);
    const gaddr red = GPut(std::vector<float>{1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1});
    G<void>("glVertexAttribPointer", a_col, 4, GL_FLOAT, 0, 0, red);
    G<void>("glDisableVertexAttribArray", a_uv);
    G<void>("glVertexAttrib4f", a_uv, 0.5f, 0.5f, 0.0f, 1.0f);  // floats softfp (r1-r3 + pila)
    G<void>("glUniform1f", u_mix, 0.0f);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
    Px p = ReadPx(32, 32);
    std::printf("  pixel = %d,%d,%d,%d\n", p.r, p.g, p.b, p.a);
    CHECK(Near(p, 255, 0, 0));

    // --- 7. glDrawElements con indices en memoria guest (sin ELEMENT_ARRAY_BUFFER)
    std::printf("[7] client arrays: glDrawElements\n");
    const gaddr green = GPut(std::vector<std::uint8_t>{0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255});
    G<void>("glVertexAttribPointer", a_col, 4, GL_UNSIGNED_BYTE, 1, 0, green);
    const gaddr idx = GPut(std::vector<std::uint16_t>{0, 1, 2});
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawElements", GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
    p = ReadPx(32, 32);
    CHECK(Near(p, 0, 255, 0));

    // --- 8. VBO + IBO (offsets, no punteros) y glMapBufferOES
    std::printf("[8] VBO/IBO + glMapBufferOES\n");
    const gaddr ids = GAlloc(8);
    G<void>("glGenBuffers", 2, ids);
    const std::uint32_t vbo = mem::R32(ids), ibo = mem::R32(ids + 4);
    CHECK(vbo && ibo && vbo != ibo);
    const std::vector<float> blue_v{0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
    G<void>("glBindBuffer", GL_ARRAY_BUFFER, vbo);
    G<void>("glBufferData", GL_ARRAY_BUFFER, 48, GPut(blue_v), GL_DYNAMIC_DRAW);
    G<void>("glVertexAttribPointer", a_col, 4, GL_FLOAT, 0, 0, gaddr{0});  // offset 0
    G<void>("glBindBuffer", GL_ARRAY_BUFFER, 0u);
    G<void>("glBindBuffer", GL_ELEMENT_ARRAY_BUFFER, ibo);
    G<void>("glBufferData", GL_ELEMENT_ARRAY_BUFFER, 8, GPut(std::vector<std::uint16_t>{9, 0, 1, 2}), GL_STATIC_DRAW);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawElements", GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, gaddr{2});  // offset 2 bytes
    p = ReadPx(32, 32);
    CHECK(Near(p, 0, 0, 255));
    // Mapear: sombra en memoria guest con el contenido actual; escribir amarillo y desmapear.
    G<void>("glBindBuffer", GL_ARRAY_BUFFER, vbo);
    G<void>("glBufferSubData", GL_ARRAY_BUFFER, 0, 16, GPut(std::vector<float>{0, 0, 1, 1}));
    const gaddr mp = G("glMapBufferOES", GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES);
    CHECK(mp >= GuestMemory::kHeapBase && GuestMemory::IsMapped(mp, 48));
    if (mp) {
      CHECK(std::memcmp(g2h(mp), blue_v.data(), 48) == 0);
      const float yellow[12] = {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1};
      std::memcpy(g2h(mp), yellow, 48);
    }
    CHECK(G("glUnmapBufferOES", GL_ARRAY_BUFFER) == 1);
    CHECK(G("glUnmapBufferOES", GL_ARRAY_BUFFER) == 0 && G("glGetError") == GL_INVALID_OPERATION);
    G<void>("glBindBuffer", GL_ARRAY_BUFFER, 0u);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawElements", GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, gaddr{2});
    p = ReadPx(32, 32);
    CHECK(Near(p, 255, 255, 0));
    G<void>("glDeleteBuffers", 2, ids);
    CHECK(G("glGetError") == GL_NO_ERROR);
    // El IBO borrado estaba ligado: el siguiente glDrawElements vuelve a usar punteros guest.
    G<void>("glVertexAttribPointer", a_col, 4, GL_FLOAT, 0, 0, red);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawElements", GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
    CHECK(Near(ReadPx(32, 32), 255, 0, 0));

    // --- 9. textura RGBA desde memoria guest
    std::printf("[9] glTexImage2D desde memoria guest\n");
    const gaddr tex_ids = GAlloc(16);
    G<void>("glGenTextures", 4, tex_ids);
    G<void>("glActiveTexture", GL_TEXTURE0);
    G<void>("glBindTexture", GL_TEXTURE_2D, mem::R32(tex_ids));
    G<void>("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    G<void>("glTexParameterf", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<float>(GL_NEAREST));
    G<void>("glPixelStorei", GL_UNPACK_ALIGNMENT, 1);
    G<void>("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, GPut(std::vector<std::uint8_t>{10, 200, 30}));
    G<void>("glUniform1f", u_mix, 1.0f);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
    p = ReadPx(32, 32);
    CHECK(Near(p, 10, 200, 30));
    G<void>("glTexSubImage2D", GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, GPut(std::vector<std::uint8_t>{90, 80, 70}));
    G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
    CHECK(Near(ReadPx(32, 32), 90, 80, 70));
    CHECK(G("glGetError") == GL_NO_ERROR);

    // --- 10. ETC1: ANGLE (ETC2) y decodificador software frente al decodificador de referencia
    std::printf("[10] ETC1 16x16 (16 bloques aleatorios)\n");
    std::mt19937 rng(1234);
    std::vector<std::uint8_t> etc(16 * 8);
    for (int b = 0; b < 16; ++b) {
      std::uint8_t* blk = &etc[b * 8];
      for (int i = 0; i < 8; ++i) blk[i] = static_cast<std::uint8_t>(rng());
      if (b & 1) {  // modo diferencial sin desbordar (si desborda, ETC2 lo interpreta como modo T/H/planar)
        for (int c = 0; c < 3; ++c) {
          const int base = 8 + (rng() % 16);
          blk[c] = static_cast<std::uint8_t>((base << 3) | (rng() & 7));
        }
        blk[3] |= 2;
      } else {
        blk[3] &= ~2;
      }
    }
    std::vector<std::uint8_t> ref;
    CHECK(hle::gles::texdecode::Decode(GL_ETC1_RGB8_OES, etc.data(), 128, 16, 16, ref));
    G<void>("glUseProgram", prog);
    G<void>("glEnableVertexAttribArray", a_uv);
    G<void>("glVertexAttribPointer", a_uv, 2, GL_FLOAT, 0, 0, uv);
    G<void>("glViewport", 0, 0, 16, 16);
    const gaddr etc_g = GPut(etc);
    for (int pass = 0; pass < 2; ++pass) {
      hle::gles::SetForceSoftwareETC1(pass == 1);
      G<void>("glBindTexture", GL_TEXTURE_2D, mem::R32(tex_ids + 4 + 4 * pass));
      G<void>("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      G<void>("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      G<void>("glCompressedTexImage2D", GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, 16, 16, 0, 128, etc_g);
      CHECK(G("glGetError") == GL_NO_ERROR);
      G<void>("glClear", GL_COLOR_BUFFER_BIT);
      G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
      const gaddr out = GAlloc(16 * 16 * 4);
      G<void>("glReadPixels", 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, out);
      int diff = 0, maxd = 0;
      for (int i = 0; i < 16 * 16 * 4; ++i) {
        if ((i & 3) == 3) continue;
        const int d = std::abs(int(g2h<std::uint8_t>(out)[i]) - int(ref[i]));
        maxd = std::max(maxd, d);
        diff += d > 1;
      }
      std::printf("  %s: %d componentes distintas (max %d)\n", pass ? "software" : (caps.etc1_native ? "nativo" : "ETC2"), diff, maxd);
      CHECK(diff == 0);
    }
    hle::gles::SetForceSoftwareETC1(false);
    // Sub-imagen ETC1 (un bloque) sobre la textura de la ruta por defecto, y por software sobre la otra.
    G<void>("glBindTexture", GL_TEXTURE_2D, mem::R32(tex_ids + 4));
    G<void>("glCompressedTexSubImage2D", GL_TEXTURE_2D, 0, 4, 4, 4, 4, GL_ETC1_RGB8_OES, 8, etc_g);
    CHECK(G("glGetError") == GL_NO_ERROR);
    hle::gles::SetForceSoftwareETC1(true);
    G<void>("glBindTexture", GL_TEXTURE_2D, mem::R32(tex_ids + 8));
    G<void>("glCompressedTexSubImage2D", GL_TEXTURE_2D, 0, 4, 4, 4, 4, GL_ETC1_RGB8_OES, 8, etc_g);
    CHECK(G("glGetError") == GL_NO_ERROR);
    hle::gles::SetForceSoftwareETC1(false);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
    p = ReadPx(5, 5);  // texel (5,5) = pixel (1,1) del bloque 0 copiado en (4,4)
    CHECK(Near(p, ref[(1 * 16 + 1) * 4], ref[(1 * 16 + 1) * 4 + 1], ref[(1 * 16 + 1) * 4 + 2], 1));

    // --- 11. PVRTC 4bpp (decodificacion software): colores A/B planos
    std::printf("[11] PVRTC 4bpp 8x8\n");
    std::vector<std::uint32_t> pvr(2 * 4);  // 4 bloques: {modulacion, color}
    for (int b = 0; b < 4; ++b) {
      pvr[b * 2] = 0;                                            // modulacion 0 -> color A
      pvr[b * 2 + 1] = 0x8000u | (31u << 10) | 0x80000000u | (31u << 16);  // A = rojo opaco, B = azul opaco
    }
    G<void>("glBindTexture", GL_TEXTURE_2D, mem::R32(tex_ids + 12));
    G<void>("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    G<void>("glCompressedTexImage2D", GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 8, 8, 0, 32, GPut(pvr));
    CHECK(G("glGetError") == GL_NO_ERROR);
    G<void>("glClear", GL_COLOR_BUFFER_BIT);
    G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
    p = ReadPx(8, 8);
    CHECK(Near(p, 255, 0, 0, 3));
    for (int b = 0; b < 4; ++b) pvr[b * 2] = 0xFFFFFFFFu;  // modulacion 3 -> color B
    G<void>("glCompressedTexImage2D", GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 8, 8, 0, 32, GPut(pvr));
    G<void>("glDrawArrays", GL_TRIANGLES, 0, 3);
    p = ReadPx(8, 8);
    CHECK(Near(p, 0, 0, 255, 3));
    // Datos insuficientes -> GL_INVALID_VALUE (no lee fuera del buffer).
    G<void>("glCompressedTexImage2D", GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, 64, 64, 0, 32, GPut(pvr));
    CHECK(G("glGetError") == GL_INVALID_VALUE);

    // --- 12. FBO + renderbuffer + glCopyTexSubImage2D + glGenerateMipmap
    std::printf("[12] FBO / renderbuffer\n");
    const gaddr fb = GAlloc(4), rb = GAlloc(4);
    G<void>("glGenFramebuffers", 1, fb);
    G<void>("glGenRenderbuffers", 1, rb);
    G<void>("glBindTexture", GL_TEXTURE_2D, mem::R32(tex_ids));
    G<void>("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, gaddr{0});
    G<void>("glBindFramebuffer", GL_FRAMEBUFFER, mem::R32(fb));
    G<void>("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mem::R32(tex_ids), 0);
    G<void>("glBindRenderbuffer", GL_RENDERBUFFER, mem::R32(rb));
    G<void>("glRenderbufferStorage", GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, 32, 32);
    G<void>("glFramebufferRenderbuffer", GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mem::R32(rb));
    G<void>("glFramebufferRenderbuffer", GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mem::R32(rb));
    CHECK(G("glCheckFramebufferStatus", GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    G<void>("glViewport", 0, 0, 32, 32);
    G<void>("glClearColor", 0.0f, 1.0f, 1.0f, 1.0f);
    G<void>("glClearDepthf", 1.0f);
    G<void>("glClearStencil", 0);
    G<void>("glClear", GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    CHECK(Near(ReadPx(5, 5), 0, 255, 255));
    G<void>("glBindFramebuffer", GL_FRAMEBUFFER, 0u);
    G<void>("glDeleteFramebuffers", 1, fb);
    G<void>("glDeleteRenderbuffers", 1, rb);
    G<void>("glGenerateMipmap", GL_TEXTURE_2D);
    G<void>("glCopyTexSubImage2D", GL_TEXTURE_2D, 0, 0, 0, 0, 0, 8, 8);
    CHECK(G("glGetError") == GL_NO_ERROR);

    // --- 13. estado variado (floats softfp) + uniformes vectoriales
    std::printf("[13] estado y uniformes\n");
    G<void>("glEnable", GL_BLEND);
    G<void>("glBlendFunc", GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    G<void>("glBlendEquation", GL_FUNC_ADD);
    G<void>("glBlendColor", 0.1f, 0.2f, 0.3f, 0.4f);
    G<void>("glGetFloatv", GL_BLEND_COLOR, iv);
    CHECK(std::fabs(mem::Read<float>(iv + 8) - 0.3f) < 1e-6f && std::fabs(mem::Read<float>(iv + 12) - 0.4f) < 1e-6f);
    G<void>("glDisable", GL_BLEND);
    G<void>("glDepthRangef", 0.25f, 0.75f);
    G<void>("glGetFloatv", GL_DEPTH_RANGE, iv);
    CHECK(mem::Read<float>(iv) == 0.25f && mem::Read<float>(iv + 4) == 0.75f);
    G<void>("glPolygonOffset", 1.5f, 2.0f);
    G<void>("glGetFloatv", GL_POLYGON_OFFSET_FACTOR, iv);
    CHECK(mem::Read<float>(iv) == 1.5f);
    G<void>("glSampleCoverage", 0.5f, 1);
    G<void>("glLineWidth", 1.0f);
    G<void>("glColorMask", 1, 1, 1, 1);
    G<void>("glDepthMask", 1);
    G<void>("glDepthFunc", GL_LEQUAL);
    G<void>("glCullFace", GL_BACK);
    G<void>("glFrontFace", GL_CCW);
    G<void>("glScissor", 0, 0, 8, 8);
    G<void>("glStencilFunc", GL_ALWAYS, 1, 0xFFu);
    G<void>("glStencilMask", 0xFFu);
    G<void>("glStencilOp", GL_KEEP, GL_KEEP, GL_REPLACE);
    G<void>("glUniform1fv", u_mix, 1, GPut(std::vector<float>{0.0f}));
    G<void>("glUniform1iv", u_tex, 1, GPut(std::vector<int>{0}));
    G<void>("glFlush");
    G<void>("glFinish");
    CHECK(G("glGetError") == GL_NO_ERROR);
    {
      // Programa con uniformes vec2/3/4, ivec2/3/4 y mat4.
      const char* vs2 = "uniform mat4 u_m; uniform vec2 u_a; uniform vec3 u_b; uniform vec4 u_c;\n"
                        "attribute vec4 p; varying vec4 v;\n"
                        "void main() { v = vec4(u_a, 0.0, 0.0) + vec4(u_b, 0.0) + u_c; gl_Position = u_m * p; }\n";
      const char* fs2 = "precision mediump float; uniform ivec2 u_i2; uniform ivec3 u_i3; uniform ivec4 u_i4; varying vec4 v;\n"
                        "void main() { gl_FragColor = v + vec4(float(u_i2.x + u_i3.x + u_i4.x)); }\n";
      const std::uint32_t p2 = G("glCreateProgram");
      G<void>("glAttachShader", p2, Shader(GL_VERTEX_SHADER, {vs2}, false));
      G<void>("glAttachShader", p2, Shader(GL_FRAGMENT_SHADER, {fs2}, false));
      G<void>("glLinkProgram", p2);
      G<void>("glUseProgram", p2);
      const std::vector<float> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      G<void>("glUniformMatrix4fv", G<int>("glGetUniformLocation", p2, Str("u_m")), 1, 0, GPut(m));
      G<void>("glUniform2fv", G<int>("glGetUniformLocation", p2, Str("u_a")), 1, GPut(std::vector<float>{1, 2}));
      G<void>("glUniform3fv", G<int>("glGetUniformLocation", p2, Str("u_b")), 1, GPut(std::vector<float>{1, 2, 3}));
      G<void>("glUniform4fv", G<int>("glGetUniformLocation", p2, Str("u_c")), 1, GPut(std::vector<float>{1, 2, 3, 4}));
      G<void>("glUniform2iv", G<int>("glGetUniformLocation", p2, Str("u_i2")), 1, GPut(std::vector<int>{1, 2}));
      G<void>("glUniform3iv", G<int>("glGetUniformLocation", p2, Str("u_i3")), 1, GPut(std::vector<int>{1, 2, 3}));
      G<void>("glUniform4iv", G<int>("glGetUniformLocation", p2, Str("u_i4")), 1, GPut(std::vector<int>{1, 2, 3, 4}));
      CHECK(G("glGetError") == GL_NO_ERROR);
      G<void>("glUseProgram", 0u);
      G<void>("glDeleteProgram", p2);
    }
    G<void>("glDeleteShader", vs);

    // --- 14. GL_OES_texture_3D (no anunciada): reenviada al nucleo ES3 si existe, si no INVALID_OPERATION
    std::printf("[14] *3DOES\n");
    const gaddr t3 = GAlloc(4);
    G<void>("glGenTextures", 1, t3);
    G<void>("glBindTexture", GL_TEXTURE_3D_OES, mem::R32(t3));
    G<void>("glTexImage3DOES", GL_TEXTURE_3D_OES, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, GAlloc(32));
    G<void>("glTexSubImage3DOES", GL_TEXTURE_3D_OES, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, GAlloc(4));
    const std::uint32_t e3 = G("glGetError");
    std::printf("  glTexImage3DOES/glTexSubImage3DOES -> 0x%x\n", e3);
    CHECK(caps.angle_major >= 3 ? e3 == GL_NO_ERROR : e3 == GL_INVALID_OPERATION);
    G<void>("glCompressedTexImage3DOES", GL_TEXTURE_3D_OES, 0, GL_ETC1_RGB8_OES, 4, 4, 1, 0, 8, GAlloc(8));
    G<void>("glCompressedTexSubImage3DOES", GL_TEXTURE_3D_OES, 0, 0, 0, 0, 4, 4, 1, GL_ETC1_RGB8_OES, 8, GAlloc(8));
    std::printf("  glCompressed*3DOES(ETC1) -> 0x%x (error esperado: ETC1 no es valido en 3D)\n", G("glGetError"));
    G<void>("glDeleteTextures", 4, tex_ids);
    G<void>("glDeleteTextures", 1, t3);
    CHECK(G("glGetError") == GL_NO_ERROR);
  } catch (const GuestFault& f) {
    std::printf("GuestFault: %s\n", f.what());
    ++g_failed;
  }

  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglDestroySurface(dpy, surf);
  eglTerminate(dpy);

  std::printf("%s: %d/%d comprobaciones OK\n", g_failed ? "FALLO" : "OK", g_checks - g_failed, g_checks);
  return g_failed ? 1 : 0;
}
