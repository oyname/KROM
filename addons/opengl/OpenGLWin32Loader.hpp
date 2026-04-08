#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include "glext.h"

namespace engine::renderer::opengl {
bool EnsureWin32OpenGLFunctionsLoaded();
}

#define KROM_OGL_PROC_LIST(X) X(void, glActiveTexture, GLenum) X(void, glAttachShader, GLuint, GLuint) X(void, glBindBuffer, GLenum, GLuint) X(void, glBindBufferBase, GLenum, GLuint, GLuint) X(void, glBindBufferRange, GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) X(void, glBindFramebuffer, GLenum, GLuint) X(void, glBindSampler, GLuint, GLuint) X(void, glBindVertexArray, GLuint) X(void, glBlendEquation, GLenum) X(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum) X(void, glBufferSubData, GLenum, GLintptr, GLsizeiptr, const void*) X(GLenum, glCheckFramebufferStatus, GLenum) X(GLenum, glClientWaitSync, GLsync, GLbitfield, GLuint64) X(void, glCompileShader, GLuint) X(void, glCopyBufferSubData, GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr) X(GLuint, glCreateProgram, void) X(GLuint, glCreateShader, GLenum) X(void, glDeleteBuffers, GLsizei, const GLuint*) X(void, glDeleteFramebuffers, GLsizei, const GLuint*) X(void, glDeleteProgram, GLuint) X(void, glDeleteSamplers, GLsizei, const GLuint*) X(void, glDeleteShader, GLuint) X(void, glDeleteSync, GLsync) X(void, glDeleteVertexArrays, GLsizei, const GLuint*) X(void, glDetachShader, GLuint, GLuint) X(void, glDispatchCompute, GLuint, GLuint, GLuint) X(void, glDrawArraysInstanced, GLenum, GLint, GLsizei, GLsizei) X(void, glDrawBuffers, GLsizei, const GLenum*) X(void, glDrawElementsBaseVertex, GLenum, GLsizei, GLenum, const void*, GLint) X(void, glDrawElementsInstancedBaseVertex, GLenum, GLsizei, GLenum, const void*, GLsizei, GLint) X(void, glEnableVertexAttribArray, GLuint) X(GLsync, glFenceSync, GLenum, GLbitfield) X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) X(void, glGenBuffers, GLsizei, GLuint*) X(void, glGenFramebuffers, GLsizei, GLuint*) X(void, glGenSamplers, GLsizei, GLuint*) X(void, glGenVertexArrays, GLsizei, GLuint*) X(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) X(void, glGetProgramiv, GLuint, GLenum, GLint*) X(GLint, glGetUniformLocation, GLuint, const GLchar*) X(GLuint, glGetUniformBlockIndex, GLuint, const GLchar*) X(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) X(void, glGetShaderiv, GLuint, GLenum, GLint*) X(void, glLinkProgram, GLuint) X(void*, glMapBuffer, GLenum, GLenum) X(void, glMemoryBarrier, GLbitfield) X(void, glSamplerParameteri, GLuint, GLenum, GLint) X(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*) X(void, glTexStorage2D, GLenum, GLsizei, GLenum, GLsizei, GLsizei) X(GLboolean, glUnmapBuffer, GLenum) X(void, glUniform1i, GLint, GLint) X(void, glUniformBlockBinding, GLuint, GLuint, GLuint) X(void, glUseProgram, GLuint) X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) X(void, glBindAttribLocation, GLuint, GLuint, const GLchar*)

namespace engine::renderer::opengl {
#define DECL_PROC(ret, name, ...) using name##_fn = ret (APIENTRYP)(__VA_ARGS__); extern name##_fn krom_##name;
KROM_OGL_PROC_LIST(DECL_PROC)
#undef DECL_PROC

inline void APIENTRY krom_glClearDepthf(GLfloat depth) { glClearDepth(static_cast<GLdouble>(depth)); }
inline void APIENTRY krom_glDepthRangef(GLfloat n, GLfloat f) { glDepthRange(static_cast<GLdouble>(n), static_cast<GLdouble>(f)); }
} // namespace engine::renderer::opengl

#define glClearDepthf ::engine::renderer::opengl::krom_glClearDepthf
#define glDepthRangef ::engine::renderer::opengl::krom_glDepthRangef
#define MAP_PROC(name)     static_assert(true, "map proc");
#define glActiveTexture ::engine::renderer::opengl::krom_glActiveTexture
#define glAttachShader ::engine::renderer::opengl::krom_glAttachShader
#define glBindBuffer ::engine::renderer::opengl::krom_glBindBuffer
#define glBindBufferBase  ::engine::renderer::opengl::krom_glBindBufferBase
#define glBindBufferRange ::engine::renderer::opengl::krom_glBindBufferRange
#define glBindFramebuffer ::engine::renderer::opengl::krom_glBindFramebuffer
#define glBindSampler ::engine::renderer::opengl::krom_glBindSampler
#define glBindVertexArray ::engine::renderer::opengl::krom_glBindVertexArray
#define glBlendEquation ::engine::renderer::opengl::krom_glBlendEquation
#define glBufferData ::engine::renderer::opengl::krom_glBufferData
#define glBufferSubData ::engine::renderer::opengl::krom_glBufferSubData
#define glCheckFramebufferStatus ::engine::renderer::opengl::krom_glCheckFramebufferStatus
#define glClientWaitSync ::engine::renderer::opengl::krom_glClientWaitSync
#define glCompileShader ::engine::renderer::opengl::krom_glCompileShader
#define glCopyBufferSubData ::engine::renderer::opengl::krom_glCopyBufferSubData
#define glCreateProgram ::engine::renderer::opengl::krom_glCreateProgram
#define glCreateShader ::engine::renderer::opengl::krom_glCreateShader
#define glDeleteBuffers ::engine::renderer::opengl::krom_glDeleteBuffers
#define glDeleteFramebuffers ::engine::renderer::opengl::krom_glDeleteFramebuffers
#define glDeleteProgram ::engine::renderer::opengl::krom_glDeleteProgram
#define glDeleteSamplers ::engine::renderer::opengl::krom_glDeleteSamplers
#define glDeleteShader ::engine::renderer::opengl::krom_glDeleteShader
#define glDeleteSync ::engine::renderer::opengl::krom_glDeleteSync
#define glDeleteVertexArrays ::engine::renderer::opengl::krom_glDeleteVertexArrays
#define glDetachShader ::engine::renderer::opengl::krom_glDetachShader
#define glDispatchCompute ::engine::renderer::opengl::krom_glDispatchCompute
#define glDrawArraysInstanced ::engine::renderer::opengl::krom_glDrawArraysInstanced
#define glDrawBuffers ::engine::renderer::opengl::krom_glDrawBuffers
#define glDrawElementsBaseVertex ::engine::renderer::opengl::krom_glDrawElementsBaseVertex
#define glDrawElementsInstancedBaseVertex ::engine::renderer::opengl::krom_glDrawElementsInstancedBaseVertex
#define glEnableVertexAttribArray ::engine::renderer::opengl::krom_glEnableVertexAttribArray
#define glFenceSync ::engine::renderer::opengl::krom_glFenceSync
#define glFramebufferTexture2D ::engine::renderer::opengl::krom_glFramebufferTexture2D
#define glGenBuffers ::engine::renderer::opengl::krom_glGenBuffers
#define glGenFramebuffers ::engine::renderer::opengl::krom_glGenFramebuffers
#define glGenSamplers ::engine::renderer::opengl::krom_glGenSamplers
#define glGenVertexArrays ::engine::renderer::opengl::krom_glGenVertexArrays
#define glGetProgramInfoLog ::engine::renderer::opengl::krom_glGetProgramInfoLog
#define glGetProgramiv ::engine::renderer::opengl::krom_glGetProgramiv
#define glGetUniformLocation ::engine::renderer::opengl::krom_glGetUniformLocation
#define glGetUniformBlockIndex ::engine::renderer::opengl::krom_glGetUniformBlockIndex
#define glGetShaderInfoLog ::engine::renderer::opengl::krom_glGetShaderInfoLog
#define glGetShaderiv ::engine::renderer::opengl::krom_glGetShaderiv
#define glLinkProgram ::engine::renderer::opengl::krom_glLinkProgram
#define glMapBuffer ::engine::renderer::opengl::krom_glMapBuffer
#define glMemoryBarrier ::engine::renderer::opengl::krom_glMemoryBarrier
#define glSamplerParameteri ::engine::renderer::opengl::krom_glSamplerParameteri
#define glShaderSource ::engine::renderer::opengl::krom_glShaderSource
#define glTexStorage2D ::engine::renderer::opengl::krom_glTexStorage2D
#define glUnmapBuffer ::engine::renderer::opengl::krom_glUnmapBuffer
#define glUniform1i ::engine::renderer::opengl::krom_glUniform1i
#define glUniformBlockBinding ::engine::renderer::opengl::krom_glUniformBlockBinding
#define glUseProgram ::engine::renderer::opengl::krom_glUseProgram
#define glVertexAttribPointer ::engine::renderer::opengl::krom_glVertexAttribPointer
#define glBindAttribLocation ::engine::renderer::opengl::krom_glBindAttribLocation

#endif
