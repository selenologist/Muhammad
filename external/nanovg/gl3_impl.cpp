// Our vendoring glue for memononen/nanovg (pinned ce3bf745): one translation
// unit that instantiates the NanoVG core + the GL3 backend against Mesa's
// glext prototypes. Everything else includes <nanovg.h> + nanovg_gl.h with
// NANOVG_GL3 defined and links against this.
#define GL_GLEXT_PROTOTYPES 1
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#ifdef _WIN32
// Windows Kits' GL/gl.h assumes windows.h came first (it defines neither
// WINGDIAPI nor APIENTRY itself); mingw-w64's copy includes it for us, so
// this only matters for MSVC but is harmless everywhere.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#ifdef _MSC_VER
// The Kits glext.h is typedefs-only (Microsoft never shipped prototypes);
// mingw's honors GL_GLEXT_PROTOTYPES. These declarations mirror, exactly,
// the extern "C" thunks defined in src/gui_win32.cpp - those definitions
// resolve every reference at link time (lazy wglGetProcAddress), so this
// block is purely the compile-time view. Keep in sync with GLTHUNK there;
// an unsynced call fails HERE as an undeclared-identifier, not subtly.
extern "C" {
WINGDIAPI void APIENTRY glActiveTexture(GLenum texture);
WINGDIAPI void APIENTRY glAttachShader(GLuint program, GLuint shader);
WINGDIAPI void APIENTRY glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
WINGDIAPI void APIENTRY glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
WINGDIAPI GLuint APIENTRY glCreateProgram(void);
WINGDIAPI GLuint APIENTRY glCreateShader(GLenum type);
WINGDIAPI void APIENTRY glDeleteProgram(GLuint program);
WINGDIAPI void APIENTRY glDeleteShader(GLuint shader);
WINGDIAPI void APIENTRY glLinkProgram(GLuint program);
WINGDIAPI void APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
WINGDIAPI void APIENTRY glCompileShader(GLuint shader);
WINGDIAPI void APIENTRY glUseProgram(GLuint program);
WINGDIAPI GLint APIENTRY glGetUniformLocation(GLuint program, const GLchar *name);
WINGDIAPI void APIENTRY glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
WINGDIAPI void APIENTRY glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
WINGDIAPI void APIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint *params);
WINGDIAPI void APIENTRY glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
WINGDIAPI void APIENTRY glGenBuffers(GLsizei n, GLuint *buffers);
WINGDIAPI void APIENTRY glBindBuffer(GLenum target, GLuint buffer);
WINGDIAPI void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
WINGDIAPI void APIENTRY glDeleteBuffers(GLsizei n, const GLuint *buffers);
WINGDIAPI void APIENTRY glEnableVertexAttribArray(GLuint index);
WINGDIAPI void APIENTRY glDisableVertexAttribArray(GLuint index);
WINGDIAPI void APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
WINGDIAPI void APIENTRY glGenVertexArrays(GLsizei n, GLuint *arrays);
WINGDIAPI void APIENTRY glBindVertexArray(GLuint array);
WINGDIAPI void APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *arrays);
WINGDIAPI void APIENTRY glUniform1i(GLint location, GLint v0);
WINGDIAPI void APIENTRY glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
WINGDIAPI void APIENTRY glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
WINGDIAPI GLuint APIENTRY glGetUniformBlockIndex(GLuint program, const GLchar *uniformBlockName);
WINGDIAPI void APIENTRY glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
WINGDIAPI void APIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
WINGDIAPI void APIENTRY glGenerateMipmap(GLenum target);
WINGDIAPI void APIENTRY glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
}
#endif
#endif

#include <nanovg.h>

#define NANOVG_GL3_IMPLEMENTATION 1
#include <nanovg_gl.h>
