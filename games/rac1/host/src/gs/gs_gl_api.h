/* gs_gl_api.h: the OpenGL 3.3 core declarations the GL renderer uses. No
 * system GL headers: the entry points are loaded at run time through
 * plat_gl_proc (wglGetProcAddress on Windows, eglGetProcAddress in the
 * Linux test harness), so nothing links against a GL library. */
#ifndef GS_GL_API_H
#define GS_GL_API_H
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
typedef struct __GLsync *GLsync;
typedef uint64_t GLuint64;

#define GL_NONE_ 0
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_SRC1_ALPHA 0x8589
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_DITHER 0x0BD0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED 0x1903
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGBA8 0x8058
#define GL_RGBA8UI 0x8D7C
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_UNSIGNED_SHORT 0x1403
#define GL_RGBA32I 0x8D82
#define GL_RGBA_INTEGER 0x8D99
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_FIRST_VERTEX_CONVENTION 0x8E4D
#define GL_LAST_VERTEX_CONVENTION 0x8E4E
#define GL_BACK 0x0405
#define GL_PROGRAM_POINT_SIZE 0x8642

/* name, return type, arguments */
#define GSGL_FUNCS(X) \
    X(glGetError, GLenum, (void)) \
    X(glGetString, const GLubyte *, (GLenum)) \
    X(glGetIntegerv, void, (GLenum, GLint *)) \
    X(glEnable, void, (GLenum)) \
    X(glDisable, void, (GLenum)) \
    X(glViewport, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(glScissor, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glClear, void, (GLbitfield)) \
    X(glColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(glDepthMask, void, (GLboolean)) \
    X(glDepthFunc, void, (GLenum)) \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum)) \
    X(glBlendEquationSeparate, void, (GLenum, GLenum)) \
    X(glBlendColor, void, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glPixelStorei, void, (GLenum, GLint)) \
    X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(glGenTextures, void, (GLsizei, GLuint *)) \
    X(glDeleteTextures, void, (GLsizei, const GLuint *)) \
    X(glBindTexture, void, (GLenum, GLuint)) \
    X(glActiveTexture, void, (GLenum)) \
    X(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(glTexParameteri, void, (GLenum, GLenum, GLint)) \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei)) \
    X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *)) \
    X(glUniform4fv, void, (GLint, GLsizei, const GLfloat *)) \
    X(glFlush, void, (void)) \
    X(glFinish, void, (void)) \
    X(glGenBuffers, void, (GLsizei, GLuint *)) \
    X(glBindBuffer, void, (GLenum, GLuint)) \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(glBufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(glTexBuffer, void, (GLenum, GLenum, GLuint)) \
    X(glMapBufferRange, void *, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    X(glUnmapBuffer, GLboolean, (GLenum)) \
    X(glGenVertexArrays, void, (GLsizei, GLuint *)) \
    X(glBindVertexArray, void, (GLuint)) \
    X(glEnableVertexAttribArray, void, (GLuint)) \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(glVertexAttribIPointer, void, (GLuint, GLint, GLenum, GLsizei, const void *)) \
    X(glCreateShader, GLuint, (GLenum)) \
    X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(glCompileShader, void, (GLuint)) \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *)) \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(glCreateProgram, GLuint, (void)) \
    X(glAttachShader, void, (GLuint, GLuint)) \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *)) \
    X(glBindFragDataLocationIndexed, void, (GLuint, GLuint, GLuint, const GLchar *)) \
    X(glLinkProgram, void, (GLuint)) \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *)) \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(glUseProgram, void, (GLuint)) \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *)) \
    X(glUniform1i, void, (GLint, GLint)) \
    X(glUniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glUniform2f, void, (GLint, GLfloat, GLfloat)) \
    X(glUniform2i, void, (GLint, GLint, GLint)) \
    X(glUniform4iv, void, (GLint, GLsizei, const GLint *)) \
    X(glUniform1iv, void, (GLint, GLsizei, const GLint *)) \
    X(glGenFramebuffers, void, (GLsizei, GLuint *)) \
    X(glDeleteFramebuffers, void, (GLsizei, const GLuint *)) \
    X(glBindFramebuffer, void, (GLenum, GLuint)) \
    X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(glCheckFramebufferStatus, GLenum, (GLenum)) \
    X(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(glProvokingVertex, void, (GLenum)) \
    X(glDrawBuffer, void, (GLenum)) \
    X(glReadBuffer, void, (GLenum))

#define GSGL_DECL(name, ret, args) typedef ret (GLAPIENTRY *PFN_##name) args;
GSGL_FUNCS(GSGL_DECL)
#undef GSGL_DECL

/* the platform's loader: returns 0 for a missing entry point */
void *plat_gl_proc(const char *name);
#endif
