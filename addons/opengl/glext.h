#include <cstddef>
#ifndef KROM_LOCAL_GLEXT_H_
#define KROM_LOCAL_GLEXT_H_

#if defined(_WIN32)
#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef GLAPI
#define GLAPI extern
#endif
#else
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP *
#endif
#ifndef GLAPI
#define GLAPI extern
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned long long GLuint64;
typedef struct __GLsync* GLsync;
typedef char GLchar;

typedef unsigned int GLbitfield;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;

#ifndef GL_FALSE
#define GL_FALSE 0
#endif
#ifndef GL_TRUE
#define GL_TRUE 1
#endif
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_RG 0x8227
#define GL_RED 0x1903
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_VENDOR 0x1F00
#define GL_VERSION 0x1F02
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_READ 0x88E1
#define GL_WRITE_ONLY 0x88B9
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPUTE_SHADER 0x91B9
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_RGB32F 0x8815
#define GL_RG8 0x822B
#define GL_RG16F 0x822F
#define GL_RG32F 0x8230
#define GL_R8 0x8229
#define GL_R16F 0x822D
#define GL_R32F 0x822E
#define GL_R32UI 0x8236
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_STENCIL 0x84F9
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH32F_STENCIL8 0x8CAD
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#define GL_ALL_BARRIER_BITS 0xFFFFFFFF
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE

GLAPI const unsigned char* APIENTRY glGetString(GLenum name);
GLAPI void APIENTRY glFinish(void);
GLAPI void APIENTRY glClear(GLbitfield mask);
GLAPI void APIENTRY glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
GLAPI void APIENTRY glClearDepth(GLdouble depth);
GLAPI void APIENTRY glClearStencil(GLint s);
GLAPI void APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
GLAPI void APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
GLAPI void APIENTRY glEnable(GLenum cap);
GLAPI void APIENTRY glDisable(GLenum cap);
GLAPI void APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor);
GLAPI void APIENTRY glDepthMask(GLboolean flag);
GLAPI void APIENTRY glDepthFunc(GLenum func);
GLAPI void APIENTRY glCullFace(GLenum mode);
GLAPI void APIENTRY glFrontFace(GLenum mode);
GLAPI void APIENTRY glBindTexture(GLenum target, GLuint texture);
GLAPI void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param);
GLAPI void APIENTRY glBindBuffer(GLenum target, GLuint buffer);
GLAPI void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
GLAPI void APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
GLAPI void* APIENTRY glMapBuffer(GLenum target, GLenum access);
GLAPI GLboolean APIENTRY glUnmapBuffer(GLenum target);
GLAPI void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
GLAPI void APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
GLAPI void APIENTRY glEnableVertexAttribArray(GLuint index);

GLAPI void APIENTRY glActiveTexture(GLenum texture);
GLAPI void APIENTRY glAttachShader(GLuint program, GLuint shader);
GLAPI void APIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
GLAPI void APIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
GLAPI void APIENTRY glBindFramebuffer(GLenum target, GLuint framebuffer);
GLAPI void APIENTRY glBindSampler(GLuint unit, GLuint sampler);
GLAPI void APIENTRY glBindVertexArray(GLuint array);
GLAPI void APIENTRY glBlendEquation(GLenum mode);
GLAPI void APIENTRY glClearDepthf(GLfloat depth);
GLAPI GLenum APIENTRY glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
GLAPI void APIENTRY glCompileShader(GLuint shader);
GLAPI void APIENTRY glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
GLAPI void APIENTRY glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
GLAPI void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI GLuint APIENTRY glCreateProgram(void);
GLAPI GLuint APIENTRY glCreateShader(GLenum type);
GLAPI void APIENTRY glDeleteBuffers(GLsizei n, const GLuint* buffers);
GLAPI void APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
GLAPI void APIENTRY glDeleteProgram(GLuint program);
GLAPI void APIENTRY glDeleteSamplers(GLsizei count, const GLuint* samplers);
GLAPI void APIENTRY glDeleteShader(GLuint shader);
GLAPI void APIENTRY glDeleteSync(GLsync sync);
GLAPI void APIENTRY glDeleteTextures(GLsizei n, const GLuint* textures);
GLAPI void APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
GLAPI void APIENTRY glDepthRangef(GLfloat n, GLfloat f);
GLAPI void APIENTRY glDetachShader(GLuint program, GLuint shader);
GLAPI void APIENTRY glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
GLAPI void APIENTRY glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei primcount);
GLAPI void APIENTRY glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex);
GLAPI void APIENTRY glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLint basevertex);
GLAPI GLsync APIENTRY glFenceSync(GLenum condition, GLbitfield flags);
GLAPI void APIENTRY glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLAPI void APIENTRY glGenBuffers(GLsizei n, GLuint* buffers);
GLAPI void APIENTRY glGenFramebuffers(GLsizei n, GLuint* ids);
GLAPI void APIENTRY glGenSamplers(GLsizei count, GLuint* samplers);
GLAPI void APIENTRY glGenTextures(GLsizei n, GLuint* textures);
GLAPI void APIENTRY glGenVertexArrays(GLsizei n, GLuint* arrays);
GLAPI void APIENTRY glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei* length, GLchar* infoLog);
GLAPI void APIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint* params);
GLAPI void APIENTRY glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei* length, GLchar* infoLog);
GLAPI void APIENTRY glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
GLAPI void APIENTRY glLinkProgram(GLuint program);
GLAPI void APIENTRY glMemoryBarrier(GLbitfield barriers);
GLAPI void APIENTRY glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
GLAPI void APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
GLAPI void APIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
GLAPI void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
GLAPI void APIENTRY glUseProgram(GLuint program);

#ifdef __cplusplus
}
#endif

#endif
