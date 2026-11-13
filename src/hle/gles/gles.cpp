// HLE de libGLESv2 -> ANGLE (T5). Ver gles.h y docs/ANALYSIS.md §7.
//
// Reglas:
// - Todos los handlers son FAST (corren dentro del JIT): solo llaman a ANGLE, nunca a call_guest.
// - Punteros: los parametros que siempre son punteros llegan ya traducidos (T* = g2h). Los que pueden ser
//   offsets en un buffer (glVertexAttribPointer, glDrawElements) llegan como gaddr y se traducen segun el
//   buffer ligado (seguimiento propio de GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER).
// - La memoria guest esta mapeada 1:1 (host = g_membase + guest), asi que un array de cliente traducido en
//   glVertexAttribPointer sigue valido en el draw: ANGLE lee directamente la memoria guest.
// - GLsizeiptr/GLintptr son de 32 bits en el guest: se declaran int32_t (el GLsizeiptr del host es de 64).
#include "hle/gles/gles.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "hle/gles/texdecode.h"

namespace zw::hle::gles {
namespace {

// ------------------------------------------------------------------ estado
bool g_check = false;        // ZW_GL_CHECK
bool g_force_soft_etc1 = false;
bool g_no_mapbuffer = false;
bool g_log_shaders = false;

std::mutex g_mu;             // caps, cadenas, sombras
Caps g_caps;
std::array<gaddr, 8> g_strings{};  // VENDOR, RENDERER, VERSION, SLV, EXTENSIONS

// Estado GL que el traductor necesita conocer (un solo contexto, un solo hilo GL).
GLuint g_array_buffer = 0;
GLuint g_element_buffer = 0;
GLenum g_pending_error = GL_NO_ERROR;  // error sintetizado por la HLE o capturado por ZW_GL_CHECK
GLuint g_default_fbo = 0;              // T8: FBO del frontend que hace de framebuffer 0 del guest

struct Shadow {
  gaddr ptr = 0;
  gsize size = 0;
  bool mapped = false;
};
std::unordered_map<GLuint, Shadow> g_shadows;  // buffers con glMapBufferOES

std::vector<std::string> g_names;

thread_local bool t_has_ctx = false;
thread_local bool t_warned_no_ctx = false;

void SetError(GLenum e) {
  if (g_pending_error == GL_NO_ERROR) g_pending_error = e;
}

// Comprueba que el hilo actual tiene contexto EGL. Sin contexto, la llamada se descarta.
bool EnterGL(const char* fn) {
  if (t_has_ctx) return true;
  if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
    t_has_ctx = true;
    return true;
  }
  if (!t_warned_no_ctx) {
    t_warned_no_ctx = true;
    log::Error("[gles] {} llamado desde un hilo sin contexto GL actual: se ignora (y las siguientes de este hilo)", fn);
  }
  return false;
}

void DrainErrors(const char* fn) {
  for (int i = 0; i < 8; ++i) {
    const GLenum e = ::glGetError();
    if (e == GL_NO_ERROR) break;
    log::Warn("[gles] GL error 0x{:04X} en {}", e, fn);
    SetError(e);
  }
}

struct CheckScope {
  const char* fn;
  ~CheckScope() {
    if (g_check) DrainErrors(fn);
  }
};

#define GL_ENTER(name, ...)              \
  if (!EnterGL(name)) return __VA_ARGS__; \
  CheckScope zw_gl_scope_{name}

// ------------------------------------------------------------------ capacidades
bool HasExt(const std::string& list, std::string_view e) {
  std::size_t p = 0;
  while ((p = list.find(e, p)) != std::string::npos) {
    const bool start = p == 0 || list[p - 1] == ' ';
    const bool end = p + e.size() == list.size() || list[p + e.size()] == ' ';
    if (start && end) return true;
    p += e.size();
  }
  return false;
}

// Sube un bloque comprimido a una textura temporal para ver si ANGLE acepta el formato.
bool ProbeCompressed(GLenum fmt) {
  GLint old_tex = 0;
  ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  GLuint t = 0;
  ::glGenTextures(1, &t);
  ::glBindTexture(GL_TEXTURE_2D, t);
  const std::uint8_t blk[8] = {};
  ::glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, 4, 4, 0, 8, blk);
  const bool ok = ::glGetError() == GL_NO_ERROR;
  ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
  ::glDeleteTextures(1, &t);
  return ok;
}

const Caps& ProbeLocked() {
  if (g_caps.probed) return g_caps;
  // Conservar errores pendientes del guest antes de sondear.
  for (int i = 0; i < 8; ++i) {
    const GLenum e = ::glGetError();
    if (e == GL_NO_ERROR) break;
    SetError(e);
  }
  Caps c;
  c.probed = true;
  const char* ver = reinterpret_cast<const char*>(::glGetString(GL_VERSION));
  if (ver) std::sscanf(ver, "OpenGL ES %d.%d", &c.angle_major, &c.angle_minor);
  const char* ext = reinterpret_cast<const char*>(::glGetString(GL_EXTENSIONS));
  const std::string angle_ext = ext ? ext : "";
  const bool es3 = c.angle_major >= 3;

  c.etc1_native = HasExt(angle_ext, "GL_OES_compressed_ETC1_RGB8_texture");
  c.etc2 = !c.etc1_native && es3 && ProbeCompressed(GL_COMPRESSED_RGB8_ETC2);
  c.standard_derivatives = HasExt(angle_ext, "GL_OES_standard_derivatives") || es3;
  c.mapbuffer = !g_no_mapbuffer && (es3 || HasExt(angle_ext, "GL_EXT_map_buffer_range"));
  c.depth_texture = false;  // no se anuncia: ANGLE/D3D11 no expone GL_OES_depth_texture (ver reporte T5)

  // Lista blanca (ANALYSIS §7). ETC1 siempre: si ANGLE no lo tiene, ETC2 o decodificacion software.
  std::string out = "GL_OES_compressed_ETC1_RGB8_texture";
  auto add = [&](const char* name, bool cond) {
    if (cond) {
      out += ' ';
      out += name;
    }
  };
  add("GL_OES_depth24", es3 || HasExt(angle_ext, "GL_OES_depth24"));
  add("GL_OES_packed_depth_stencil", es3 || HasExt(angle_ext, "GL_OES_packed_depth_stencil"));
  add("GL_OES_rgb8_rgba8", es3 || HasExt(angle_ext, "GL_OES_rgb8_rgba8"));
  add("GL_OES_element_index_uint", es3 || HasExt(angle_ext, "GL_OES_element_index_uint"));
  add("GL_OES_texture_npot", es3 || HasExt(angle_ext, "GL_OES_texture_npot"));
  add("GL_OES_standard_derivatives", c.standard_derivatives);
  add("GL_OES_mapbuffer", c.mapbuffer);
  c.extensions = out;
  g_caps = c;
  ::glGetError();
  log::Info("[gles] ANGLE {}.{}; ETC1: {}; extensiones para el guest: {}", c.angle_major, c.angle_minor,
            g_force_soft_etc1 ? "software (forzado)"
            : c.etc1_native   ? "nativo"
            : c.etc2          ? "como ETC2"
                              : "software",
            c.extensions);
  return g_caps;
}

const Caps& CapsNow() {
  std::lock_guard lk(g_mu);
  return ProbeLocked();
}

// ------------------------------------------------------------------ cadenas
gaddr GuestString(int slot, const std::string& s) {
  std::lock_guard lk(g_mu);
  if (!g_strings[slot]) g_strings[slot] = mem::StrDup(s);
  return g_strings[slot];
}

// ------------------------------------------------------------------ handlers: estado simple
void h_glActiveTexture(GLenum t) { GL_ENTER("glActiveTexture"); ::glActiveTexture(t); }
void h_glAttachShader(GLuint p, GLuint s) { GL_ENTER("glAttachShader"); ::glAttachShader(p, s); }
void h_glBindBuffer(GLenum target, GLuint b) {
  GL_ENTER("glBindBuffer");
  ::glBindBuffer(target, b);
  if (target == GL_ARRAY_BUFFER) g_array_buffer = b;
  else if (target == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
}
void h_glBindFramebuffer(GLenum t, GLuint f) { GL_ENTER("glBindFramebuffer"); ::glBindFramebuffer(t, f ? f : g_default_fbo); }
void h_glBindRenderbuffer(GLenum t, GLuint r) { GL_ENTER("glBindRenderbuffer"); ::glBindRenderbuffer(t, r); }
void h_glBindTexture(GLenum t, GLuint tex) { GL_ENTER("glBindTexture"); ::glBindTexture(t, tex); }
void h_glBlendColor(float r, float g, float b, float a) { GL_ENTER("glBlendColor"); ::glBlendColor(r, g, b, a); }
void h_glBlendEquation(GLenum m) { GL_ENTER("glBlendEquation"); ::glBlendEquation(m); }
void h_glBlendFunc(GLenum s, GLenum d) { GL_ENTER("glBlendFunc"); ::glBlendFunc(s, d); }
GLenum h_glCheckFramebufferStatus(GLenum t) { GL_ENTER("glCheckFramebufferStatus", 0); return ::glCheckFramebufferStatus(t); }
void h_glClear(GLbitfield m) { GL_ENTER("glClear"); ::glClear(m); }
void h_glClearColor(float r, float g, float b, float a) { GL_ENTER("glClearColor"); ::glClearColor(r, g, b, a); }
void h_glClearDepthf(float d) { GL_ENTER("glClearDepthf"); ::glClearDepthf(d); }
void h_glClearStencil(GLint s) { GL_ENTER("glClearStencil"); ::glClearStencil(s); }
void h_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { GL_ENTER("glColorMask"); ::glColorMask(r, g, b, a); }
void h_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
  GL_ENTER("glCopyTexSubImage2D");
  ::glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}
GLuint h_glCreateProgram() { GL_ENTER("glCreateProgram", 0); return ::glCreateProgram(); }
GLuint h_glCreateShader(GLenum t) { GL_ENTER("glCreateShader", 0); return ::glCreateShader(t); }
void h_glCullFace(GLenum m) { GL_ENTER("glCullFace"); ::glCullFace(m); }
void h_glDeleteFramebuffers(GLsizei n, const GLuint* f) { GL_ENTER("glDeleteFramebuffers"); if (n > 0 && f) ::glDeleteFramebuffers(n, f); }
void h_glDeleteProgram(GLuint p) { GL_ENTER("glDeleteProgram"); ::glDeleteProgram(p); }
void h_glDeleteRenderbuffers(GLsizei n, const GLuint* r) { GL_ENTER("glDeleteRenderbuffers"); if (n > 0 && r) ::glDeleteRenderbuffers(n, r); }
void h_glDeleteShader(GLuint s) { GL_ENTER("glDeleteShader"); ::glDeleteShader(s); }
void h_glDeleteTextures(GLsizei n, const GLuint* t) { GL_ENTER("glDeleteTextures"); if (n > 0 && t) ::glDeleteTextures(n, t); }
void h_glDepthFunc(GLenum f) { GL_ENTER("glDepthFunc"); ::glDepthFunc(f); }
void h_glDepthMask(GLboolean f) { GL_ENTER("glDepthMask"); ::glDepthMask(f); }
void h_glDepthRangef(float n, float f) { GL_ENTER("glDepthRangef"); ::glDepthRangef(n, f); }
void h_glDisable(GLenum c) { GL_ENTER("glDisable"); ::glDisable(c); }
void h_glDisableVertexAttribArray(GLuint i) { GL_ENTER("glDisableVertexAttribArray"); ::glDisableVertexAttribArray(i); }
void h_glDrawArrays(GLenum m, GLint f, GLsizei c) { GL_ENTER("glDrawArrays"); ::glDrawArrays(m, f, c); }
void h_glEnable(GLenum c) { GL_ENTER("glEnable"); ::glEnable(c); }
void h_glEnableVertexAttribArray(GLuint i) { GL_ENTER("glEnableVertexAttribArray"); ::glEnableVertexAttribArray(i); }
void h_glFinish() { GL_ENTER("glFinish"); ::glFinish(); }
void h_glFlush() { GL_ENTER("glFlush"); ::glFlush(); }
void h_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r) {
  GL_ENTER("glFramebufferRenderbuffer");
  ::glFramebufferRenderbuffer(t, a, rt, r);
}
void h_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) {
  GL_ENTER("glFramebufferTexture2D");
  ::glFramebufferTexture2D(t, a, tt, tex, l);
}
void h_glFrontFace(GLenum m) { GL_ENTER("glFrontFace"); ::glFrontFace(m); }
void h_glGenBuffers(GLsizei n, GLuint* b) { GL_ENTER("glGenBuffers"); if (n > 0 && b) ::glGenBuffers(n, b); }
void h_glGenFramebuffers(GLsizei n, GLuint* f) { GL_ENTER("glGenFramebuffers"); if (n > 0 && f) ::glGenFramebuffers(n, f); }
void h_glGenRenderbuffers(GLsizei n, GLuint* r) { GL_ENTER("glGenRenderbuffers"); if (n > 0 && r) ::glGenRenderbuffers(n, r); }
void h_glGenTextures(GLsizei n, GLuint* t) { GL_ENTER("glGenTextures"); if (n > 0 && t) ::glGenTextures(n, t); }
void h_glGenerateMipmap(GLenum t) { GL_ENTER("glGenerateMipmap"); ::glGenerateMipmap(t); }
void h_glGetActiveAttrib(GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLint* sz, GLenum* ty, GLchar* name) {
  GL_ENTER("glGetActiveAttrib");
  ::glGetActiveAttrib(p, i, bs, len, sz, ty, name);
}
void h_glGetActiveUniform(GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLint* sz, GLenum* ty, GLchar* name) {
  GL_ENTER("glGetActiveUniform");
  ::glGetActiveUniform(p, i, bs, len, sz, ty, name);
}
GLint h_glGetAttribLocation(GLuint p, const GLchar* n) { GL_ENTER("glGetAttribLocation", -1); return n ? ::glGetAttribLocation(p, n) : -1; }
GLenum h_glGetError() {
  if (!EnterGL("glGetError")) return GL_NO_ERROR;
  if (g_pending_error != GL_NO_ERROR) {
    const GLenum e = g_pending_error;
    g_pending_error = GL_NO_ERROR;
    return e;
  }
  return ::glGetError();
}

// glGet* con pnames que no deben verse tal cual: formatos comprimidos (ANGLE lista ETC2/DXT..., que no
// anunciamos y que ademas podrian desbordar el buffer del guest) y formatos binarios.
bool SpecialGet(GLenum pname, GLint* out, bool& handled) {
  handled = true;
  switch (pname) {
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: if (out) out[0] = 1; return true;
    case GL_COMPRESSED_TEXTURE_FORMATS: if (out) out[0] = static_cast<GLint>(texdecode::kETC1_RGB8); return true;
    case GL_NUM_SHADER_BINARY_FORMATS:
    case GL_NUM_PROGRAM_BINARY_FORMATS_OES: if (out) out[0] = 0; return true;
    case GL_SHADER_BINARY_FORMATS:
    case GL_PROGRAM_BINARY_FORMATS_OES: return true;  // lista vacia: no escribir nada
    default: handled = false; return false;
  }
}
void h_glGetIntegerv(GLenum pname, GLint* params) {
  GL_ENTER("glGetIntegerv");
  if (!params) return;
  bool handled;
  SpecialGet(pname, params, handled);
  if (!handled) ::glGetIntegerv(pname, params);
  // El FBO del frontend es el "framebuffer 0" del guest.
  if (pname == GL_FRAMEBUFFER_BINDING && g_default_fbo && static_cast<GLuint>(params[0]) == g_default_fbo) params[0] = 0;
}
void h_glGetFloatv(GLenum pname, GLfloat* params) {
  GL_ENTER("glGetFloatv");
  if (!params) return;
  GLint v = 0;
  bool handled;
  SpecialGet(pname, &v, handled);
  if (handled) {
    if (pname != GL_SHADER_BINARY_FORMATS && pname != GL_PROGRAM_BINARY_FORMATS_OES) params[0] = static_cast<float>(v);
    return;
  }
  ::glGetFloatv(pname, params);
}
void h_glGetProgramInfoLog(GLuint p, GLsizei bs, GLsizei* len, GLchar* log) { GL_ENTER("glGetProgramInfoLog"); ::glGetProgramInfoLog(p, bs, len, log); }
void h_glGetProgramiv(GLuint p, GLenum pn, GLint* v) { GL_ENTER("glGetProgramiv"); if (v) ::glGetProgramiv(p, pn, v); }
void h_glGetShaderInfoLog(GLuint s, GLsizei bs, GLsizei* len, GLchar* log) { GL_ENTER("glGetShaderInfoLog"); ::glGetShaderInfoLog(s, bs, len, log); }
void h_glGetShaderSource(GLuint s, GLsizei bs, GLsizei* len, GLchar* src) { GL_ENTER("glGetShaderSource"); ::glGetShaderSource(s, bs, len, src); }
void h_glGetShaderiv(GLuint s, GLenum pn, GLint* v) { GL_ENTER("glGetShaderiv"); if (v) ::glGetShaderiv(s, pn, v); }

gaddr h_glGetString(GLenum name) {
  GL_ENTER("glGetString", 0);
  switch (name) {
    case GL_VENDOR: return GuestString(0, "Qualcomm");
    case GL_RENDERER: return GuestString(1, "Adreno (TM) 330");
    case GL_VERSION: return GuestString(2, "OpenGL ES 2.0 Zombiewood-ANGLE");
    case GL_SHADING_LANGUAGE_VERSION: return GuestString(3, "OpenGL ES GLSL ES 1.00");
    case GL_EXTENSIONS: {
      const std::string ext = CapsNow().extensions;
      return GuestString(4, ext);
    }
    default: SetError(GL_INVALID_ENUM); return 0;
  }
}
GLint h_glGetUniformLocation(GLuint p, const GLchar* n) { GL_ENTER("glGetUniformLocation", -1); return n ? ::glGetUniformLocation(p, n) : -1; }
void h_glLineWidth(float w) { GL_ENTER("glLineWidth"); ::glLineWidth(w); }
void h_glPixelStorei(GLenum pn, GLint v) { GL_ENTER("glPixelStorei"); ::glPixelStorei(pn, v); }
void h_glPolygonOffset(float f, float u) { GL_ENTER("glPolygonOffset"); ::glPolygonOffset(f, u); }
void h_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* px) { GL_ENTER("glReadPixels"); ::glReadPixels(x, y, w, h, f, t, px); }
void h_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { GL_ENTER("glRenderbufferStorage"); ::glRenderbufferStorage(t, f, w, h); }
void h_glSampleCoverage(float v, GLboolean inv) { GL_ENTER("glSampleCoverage"); ::glSampleCoverage(v, inv); }
void h_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { GL_ENTER("glScissor"); ::glScissor(x, y, w, h); }
void h_glStencilFunc(GLenum f, GLint r, GLuint m) { GL_ENTER("glStencilFunc"); ::glStencilFunc(f, r, m); }
void h_glStencilMask(GLuint m) { GL_ENTER("glStencilMask"); ::glStencilMask(m); }
void h_glStencilOp(GLenum a, GLenum b, GLenum c) { GL_ENTER("glStencilOp"); ::glStencilOp(a, b, c); }
void h_glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void* px) {
  GL_ENTER("glTexImage2D");
  ::glTexImage2D(t, l, ifmt, w, h, b, f, ty, px);
}
void h_glTexParameterf(GLenum t, GLenum pn, float v) { GL_ENTER("glTexParameterf"); ::glTexParameterf(t, pn, v); }
void h_glTexParameteri(GLenum t, GLenum pn, GLint v) { GL_ENTER("glTexParameteri"); ::glTexParameteri(t, pn, v); }
void h_glTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void* px) {
  GL_ENTER("glTexSubImage2D");
  ::glTexSubImage2D(t, l, xo, yo, w, h, f, ty, px);
}
void h_glUniform1f(GLint l, float x) { GL_ENTER("glUniform1f"); ::glUniform1f(l, x); }
void h_glUniform1fv(GLint l, GLsizei c, const GLfloat* v) { GL_ENTER("glUniform1fv"); ::glUniform1fv(l, c, v); }
void h_glUniform1i(GLint l, GLint x) { GL_ENTER("glUniform1i"); ::glUniform1i(l, x); }
void h_glUniform1iv(GLint l, GLsizei c, const GLint* v) { GL_ENTER("glUniform1iv"); ::glUniform1iv(l, c, v); }
void h_glUniform2fv(GLint l, GLsizei c, const GLfloat* v) { GL_ENTER("glUniform2fv"); ::glUniform2fv(l, c, v); }
void h_glUniform2iv(GLint l, GLsizei c, const GLint* v) { GL_ENTER("glUniform2iv"); ::glUniform2iv(l, c, v); }
void h_glUniform3fv(GLint l, GLsizei c, const GLfloat* v) { GL_ENTER("glUniform3fv"); ::glUniform3fv(l, c, v); }
void h_glUniform3iv(GLint l, GLsizei c, const GLint* v) { GL_ENTER("glUniform3iv"); ::glUniform3iv(l, c, v); }
void h_glUniform4fv(GLint l, GLsizei c, const GLfloat* v) { GL_ENTER("glUniform4fv"); ::glUniform4fv(l, c, v); }
void h_glUniform4iv(GLint l, GLsizei c, const GLint* v) { GL_ENTER("glUniform4iv"); ::glUniform4iv(l, c, v); }
void h_glUniformMatrix4fv(GLint l, GLsizei c, GLboolean tr, const GLfloat* v) { GL_ENTER("glUniformMatrix4fv"); ::glUniformMatrix4fv(l, c, tr, v); }
void h_glUseProgram(GLuint p) { GL_ENTER("glUseProgram"); ::glUseProgram(p); }
void h_glVertexAttrib4f(GLuint i, float x, float y, float z, float w) { GL_ENTER("glVertexAttrib4f"); ::glVertexAttrib4f(i, x, y, z, w); }
void h_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { GL_ENTER("glViewport"); ::glViewport(x, y, w, h); }

// ------------------------------------------------------------------ arrays de cliente
void h_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, gaddr ptr) {
  GL_ENTER("glVertexAttribPointer");
  // Con VBO ligado `ptr` es un offset; sin VBO es una direccion guest (array de cliente).
  const void* p = g_array_buffer ? reinterpret_cast<const void*>(static_cast<std::uintptr_t>(ptr)) : g2h<const void>(ptr);
  ::glVertexAttribPointer(i, size, type, norm, stride, p);
}
void h_glDrawElements(GLenum mode, GLsizei count, GLenum type, gaddr indices) {
  GL_ENTER("glDrawElements");
  const void* p = g_element_buffer ? reinterpret_cast<const void*>(static_cast<std::uintptr_t>(indices))
                                   : g2h<const void>(indices);
  ::glDrawElements(mode, count, type, p);
}

// ------------------------------------------------------------------ buffers (+ sombra de glMapBufferOES)
GLuint BoundBuffer(GLenum target) {
  if (target == GL_ARRAY_BUFFER) return g_array_buffer;
  if (target == GL_ELEMENT_ARRAY_BUFFER) return g_element_buffer;
  return 0;
}

void h_glBufferData(GLenum target, std::int32_t size, const void* data, GLenum usage) {
  GL_ENTER("glBufferData");
  ::glBufferData(target, static_cast<GLsizeiptr>(size), data, usage);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(BoundBuffer(target));
  if (it == g_shadows.end() || size < 0) return;
  Shadow& s = it->second;
  if (s.size != static_cast<gsize>(size)) {
    GuestMemory::Free(s.ptr);
    s.ptr = GuestMemory::Alloc(static_cast<gsize>(size));
    s.size = s.ptr ? static_cast<gsize>(size) : 0;
  }
  if (data && s.ptr) std::memcpy(g2h(s.ptr), data, s.size);
}
void h_glBufferSubData(GLenum target, std::int32_t off, std::int32_t size, const void* data) {
  GL_ENTER("glBufferSubData");
  ::glBufferSubData(target, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(size), data);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(BoundBuffer(target));
  if (it == g_shadows.end() || !data || off < 0 || size < 0) return;
  Shadow& s = it->second;
  if (static_cast<std::uint64_t>(off) + static_cast<std::uint64_t>(size) <= s.size) std::memcpy(g2h<std::uint8_t>(s.ptr) + off, data, size);
}
void h_glDeleteBuffers(GLsizei n, const GLuint* b) {
  GL_ENTER("glDeleteBuffers");
  if (n <= 0 || !b) return;
  ::glDeleteBuffers(n, b);
  std::lock_guard lk(g_mu);
  for (GLsizei i = 0; i < n; ++i) {
    if (!b[i]) continue;
    if (g_array_buffer == b[i]) g_array_buffer = 0;
    if (g_element_buffer == b[i]) g_element_buffer = 0;
    auto it = g_shadows.find(b[i]);
    if (it != g_shadows.end()) {
      GuestMemory::Free(it->second.ptr);
      g_shadows.erase(it);
    }
  }
}

// GL_OES_mapbuffer: el guest necesita un puntero guest. Se entrega una copia sombra en memoria guest que se
// mantiene sincronizada con glBufferData/SubData; en glUnmapBufferOES se sube entera con glBufferSubData.
gaddr h_glMapBufferOES(GLenum target, GLenum access) {
  GL_ENTER("glMapBufferOES", 0);
  const GLuint buf = BoundBuffer(target);
  if (!buf || access != GL_WRITE_ONLY_OES) {
    SetError(buf ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
    return 0;
  }
  GLint size = 0;
  ::glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  if (size <= 0) {
    SetError(GL_INVALID_OPERATION);
    return 0;
  }
  const bool es3 = CapsNow().angle_major >= 3;
  std::lock_guard lk(g_mu);
  Shadow& s = g_shadows[buf];
  if (s.mapped) {
    SetError(GL_INVALID_OPERATION);
    return 0;
  }
  if (s.size != static_cast<gsize>(size) || !s.ptr) {
    GuestMemory::Free(s.ptr);
    s.ptr = GuestMemory::Alloc(static_cast<gsize>(size));
    s.size = s.ptr ? static_cast<gsize>(size) : 0;
    if (!s.ptr) {
      g_shadows.erase(buf);
      SetError(GL_OUT_OF_MEMORY);
      return 0;
    }
    // Contenido inicial = contenido actual del buffer (OES_mapbuffer lo conserva).
    void* src = es3 ? ::glMapBufferRange(target, 0, size, GL_MAP_READ_BIT) : nullptr;
    if (src) {
      std::memcpy(g2h(s.ptr), src, s.size);
      ::glUnmapBuffer(target);
    } else {
      std::memset(g2h(s.ptr), 0, s.size);
      static bool warned = false;
      if (!warned) {
        warned = true;
        log::Warn("[gles] glMapBufferOES: no se pudo leer el contenido previo del buffer {} (sombra a cero)", buf);
      }
    }
  }
  s.mapped = true;
  return s.ptr;
}
GLboolean h_glUnmapBufferOES(GLenum target) {
  GL_ENTER("glUnmapBufferOES", GL_FALSE);
  const GLuint buf = BoundBuffer(target);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(buf);
  if (!buf || it == g_shadows.end() || !it->second.mapped) {
    SetError(GL_INVALID_OPERATION);
    return GL_FALSE;
  }
  it->second.mapped = false;
  ::glBufferSubData(target, 0, static_cast<GLsizeiptr>(it->second.size), g2h(it->second.ptr));
  return GL_TRUE;
}

// ------------------------------------------------------------------ shaders
void h_glShaderSource(GLuint shader, GLsizei count, gaddr strings, gaddr lengths) {
  GL_ENTER("glShaderSource");
  if (count < 0) {
    SetError(GL_INVALID_VALUE);
    return;
  }
  std::vector<const GLchar*> ptrs(static_cast<std::size_t>(count));
  std::vector<GLint> lens(static_cast<std::size_t>(count));
  for (GLsizei i = 0; i < count; ++i) {
    ptrs[i] = g2h<const GLchar>(mem::R32(strings + 4 * i));
    lens[i] = lengths ? static_cast<GLint>(mem::R32(lengths + 4 * i)) : -1;
    if (!ptrs[i]) {  // GL: puntero nulo con longitud 0 es valido; normalizamos a cadena vacia
      ptrs[i] = "";
      lens[i] = 0;
    }
  }
  ::glShaderSource(shader, count, ptrs.data(), lens.data());
}

std::string ShaderSource(GLuint s) {
  GLint n = 0;
  ::glGetShaderiv(s, GL_SHADER_SOURCE_LENGTH, &n);
  std::string src(n > 0 ? n : 0, '\0');
  if (n > 0) ::glGetShaderSource(s, n, nullptr, src.data());
  if (!src.empty() && src.back() == '\0') src.pop_back();
  std::ostringstream os;
  std::istringstream is(src);
  std::string line;
  for (int ln = 1; std::getline(is, line); ++ln) os << std::format("{:4} | {}\n", ln, line);
  return os.str();
}

void h_glCompileShader(GLuint s) {
  GL_ENTER("glCompileShader");
  ::glCompileShader(s);
  GLint ok = GL_FALSE;
  ::glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok || g_log_shaders) {
    GLint n = 0;
    ::glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
    std::string info(n > 0 ? n : 0, '\0');
    if (n > 0) ::glGetShaderInfoLog(s, n, nullptr, info.data());
    GLint type = 0;
    ::glGetShaderiv(s, GL_SHADER_TYPE, &type);
    const char* kind = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
    if (!ok)
      log::Error("[gles] fallo al compilar el {} shader {}:\n{}\n--- fuente ---\n{}", kind, s, info.c_str(), ShaderSource(s));
    else
      log::Info("[gles] {} shader {} compilado:\n{}", kind, s, ShaderSource(s));
  }
}
void h_glLinkProgram(GLuint p) {
  GL_ENTER("glLinkProgram");
  ::glLinkProgram(p);
  GLint ok = GL_FALSE;
  ::glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint n = 0;
    ::glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
    std::string info(n > 0 ? n : 0, '\0');
    if (n > 0) ::glGetProgramInfoLog(p, n, nullptr, info.data());
    log::Error("[gles] fallo al enlazar el programa {}: {}", p, info.c_str());
  }
}

// ------------------------------------------------------------------ texturas comprimidas
// Resultado de traducir un formato comprimido del guest.
enum class Route { Pass, Etc2, Soft };
Route RouteFor(GLenum fmt) {
  if (fmt == texdecode::kETC1_RGB8) {
    const Caps& c = CapsNow();
    if (g_force_soft_etc1) return Route::Soft;
    if (c.etc1_native) return Route::Pass;
    return c.etc2 ? Route::Etc2 : Route::Soft;
  }
  if (texdecode::IsPVRTC(fmt) || texdecode::IsATC(fmt)) return Route::Soft;  // ANGLE/D3D11 no los soporta
  return Route::Pass;
}

bool SoftDecode(GLenum fmt, GLsizei w, GLsizei h, GLsizei size, const void* data, std::vector<std::uint8_t>& out) {
  if (!texdecode::Decode(fmt, static_cast<const std::uint8_t*>(data), static_cast<std::uint32_t>(size), w, h, out)) {
    log::Warn("[gles] no se pudo decodificar textura comprimida 0x{:04X} {}x{} ({} bytes)", fmt, w, h, size);
    SetError(GL_INVALID_VALUE);
    return false;
  }
  return true;
}

void h_glCompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei size, const void* data) {
  GL_ENTER("glCompressedTexImage2D");
  switch (RouteFor(ifmt)) {
    case Route::Pass: ::glCompressedTexImage2D(t, l, ifmt, w, h, b, size, data); return;
    case Route::Etc2: ::glCompressedTexImage2D(t, l, GL_COMPRESSED_RGB8_ETC2, w, h, b, size, data); return;
    case Route::Soft: {
      if (w < 0 || h < 0 || b != 0) {
        SetError(GL_INVALID_VALUE);
        return;
      }
      std::vector<std::uint8_t> rgba;
      if (data && w > 0 && h > 0) {
        if (!SoftDecode(ifmt, w, h, size, data, rgba)) return;
      }
      GLint align = 4;
      ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      ::glTexImage2D(t, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.empty() ? nullptr : rgba.data());
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, align);
      return;
    }
  }
}
void h_glCompressedTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void* data) {
  GL_ENTER("glCompressedTexSubImage2D");
  switch (RouteFor(fmt)) {
    case Route::Pass: ::glCompressedTexSubImage2D(t, l, xo, yo, w, h, fmt, size, data); return;
    case Route::Etc2: ::glCompressedTexSubImage2D(t, l, xo, yo, w, h, GL_COMPRESSED_RGB8_ETC2, size, data); return;
    case Route::Soft: {
      std::vector<std::uint8_t> rgba;
      if (!data || w <= 0 || h <= 0 || !SoftDecode(fmt, w, h, size, data, rgba)) return;
      GLint align = 4;
      ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      ::glTexSubImage2D(t, l, xo, yo, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, align);
      return;
    }
  }
}

// ------------------------------------------------------------------ GL_OES_texture_3D (no anunciada)
// Inalcanzables con la lista de extensiones que damos (ANALYSIS §7). Si el contexto ANGLE es ES3 se
// reenvian a las funciones 3D del nucleo (GL_TEXTURE_3D_OES == GL_TEXTURE_3D); si no, GL_INVALID_OPERATION.
bool Have3D() {
  if (CapsNow().angle_major >= 3) return true;
  SetError(GL_INVALID_OPERATION);
  return false;
}
void h_glTexImage3DOES(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum f, GLenum ty, const void* px) {
  GL_ENTER("glTexImage3DOES");
  if (Have3D()) ::glTexImage3D(t, l, static_cast<GLint>(ifmt), w, h, d, b, f, ty, px);
}
void h_glTexSubImage3DOES(GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum ty, const void* px) {
  GL_ENTER("glTexSubImage3DOES");
  if (Have3D()) ::glTexSubImage3D(t, l, xo, yo, zo, w, h, d, f, ty, px);
}
void h_glCompressedTexImage3DOES(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei size, const void* data) {
  GL_ENTER("glCompressedTexImage3DOES");
  if (Have3D()) ::glCompressedTexImage3D(t, l, ifmt, w, h, d, b, size, data);
}
void h_glCompressedTexSubImage3DOES(GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLsizei size, const void* data) {
  GL_ENTER("glCompressedTexSubImage3DOES");
  if (Have3D()) ::glCompressedTexSubImage3D(t, l, xo, yo, zo, w, h, d, f, size, data);
}

bool EnvOn(const char* n) {
  const char* v = std::getenv(n);
  return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

const std::vector<std::string>& ImportNames() { return g_names; }

void SetDefaultFramebuffer(std::uint32_t fbo) { g_default_fbo = fbo; }
void SetGlCheck(bool on) { g_check = on; }
void SetForceSoftwareETC1(bool on) { g_force_soft_etc1 = on; }

const Caps& GetCaps() { return CapsNow(); }

void ResetContextState() {
  std::lock_guard lk(g_mu);
  g_caps = Caps{};
  g_strings[4] = 0;  // la lista de extensiones puede cambiar; el resto son constantes
  g_array_buffer = g_element_buffer = 0;
  g_pending_error = GL_NO_ERROR;
  for (auto& [id, s] : g_shadows) GuestMemory::Free(s.ptr);
  g_shadows.clear();
}

void Register() {
  g_check = EnvOn("ZW_GL_CHECK");
  g_force_soft_etc1 = EnvOn("ZW_GL_ETC1_SOFT");
  g_no_mapbuffer = EnvOn("ZW_GL_NO_MAPBUFFER");
  g_log_shaders = EnvOn("ZW_GL_LOG_SHADERS");
  g_names.clear();
#define R(fn)                        \
  do {                               \
    ZW_HLE_FAST(#fn, h_##fn);        \
    g_names.emplace_back(#fn);       \
  } while (0)
  R(glActiveTexture);
  R(glAttachShader);
  R(glBindBuffer);
  R(glBindFramebuffer);
  R(glBindRenderbuffer);
  R(glBindTexture);
  R(glBlendColor);
  R(glBlendEquation);
  R(glBlendFunc);
  R(glBufferData);
  R(glBufferSubData);
  R(glCheckFramebufferStatus);
  R(glClear);
  R(glClearColor);
  R(glClearDepthf);
  R(glClearStencil);
  R(glColorMask);
  R(glCompileShader);
  R(glCompressedTexImage2D);
  R(glCompressedTexImage3DOES);
  R(glCompressedTexSubImage2D);
  R(glCompressedTexSubImage3DOES);
  R(glCopyTexSubImage2D);
  R(glCreateProgram);
  R(glCreateShader);
  R(glCullFace);
  R(glDeleteBuffers);
  R(glDeleteFramebuffers);
  R(glDeleteProgram);
  R(glDeleteRenderbuffers);
  R(glDeleteShader);
  R(glDeleteTextures);
  R(glDepthFunc);
  R(glDepthMask);
  R(glDepthRangef);
  R(glDisable);
  R(glDisableVertexAttribArray);
  R(glDrawArrays);
  R(glDrawElements);
  R(glEnable);
  R(glEnableVertexAttribArray);
  R(glFinish);
  R(glFlush);
  R(glFramebufferRenderbuffer);
  R(glFramebufferTexture2D);
  R(glFrontFace);
  R(glGenBuffers);
  R(glGenFramebuffers);
  R(glGenRenderbuffers);
  R(glGenTextures);
  R(glGenerateMipmap);
  R(glGetActiveAttrib);
  R(glGetActiveUniform);
  R(glGetAttribLocation);
  R(glGetError);
  R(glGetFloatv);
  R(glGetIntegerv);
  R(glGetProgramInfoLog);
  R(glGetProgramiv);
  R(glGetShaderInfoLog);
  R(glGetShaderSource);
  R(glGetShaderiv);
  R(glGetString);
  R(glGetUniformLocation);
  R(glLineWidth);
  R(glLinkProgram);
  R(glMapBufferOES);
  R(glPixelStorei);
  R(glPolygonOffset);
  R(glReadPixels);
  R(glRenderbufferStorage);
  R(glSampleCoverage);
  R(glScissor);
  R(glShaderSource);
  R(glStencilFunc);
  R(glStencilMask);
  R(glStencilOp);
  R(glTexImage2D);
  R(glTexImage3DOES);
  R(glTexParameterf);
  R(glTexParameteri);
  R(glTexSubImage2D);
  R(glTexSubImage3DOES);
  R(glUniform1f);
  R(glUniform1fv);
  R(glUniform1i);
  R(glUniform1iv);
  R(glUniform2fv);
  R(glUniform2iv);
  R(glUniform3fv);
  R(glUniform3iv);
  R(glUniform4fv);
  R(glUniform4iv);
  R(glUniformMatrix4fv);
  R(glUnmapBufferOES);
  R(glUseProgram);
  R(glVertexAttrib4f);
  R(glVertexAttribPointer);
  R(glViewport);
#undef R
  log::Info("[gles] {} imports gl* registrados (ZW_GL_CHECK={})", g_names.size(), g_check ? 1 : 0);
}

}  // namespace zw::hle::gles
