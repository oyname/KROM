// =============================================================================
// KROM Engine - OpenGLCommandList.cpp
// GL 4.1 Core - glVertexAttribPointer für Vertex-Layout (kein 4.3 ARB_vertex_attrib_binding).
// =============================================================================
#include "OpenGLDevice.hpp"
#include "core/Debug.hpp"

#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
#       ifndef WIN32_LEAN_AND_MEAN
#           define WIN32_LEAN_AND_MEAN
#       endif
#       ifndef NOMINMAX
#           define NOMINMAX
#       endif
#       include <windows.h>
#   endif
#   if defined(_WIN32)
#       include "OpenGLWin32Loader.hpp"
#   endif
#   if defined(__APPLE__)
#       include <OpenGL/gl3.h>
#   else
#       include <GL/gl.h>
#       include "glext.h"
#   endif
#endif

namespace engine::renderer::opengl {


namespace {
#ifdef KROM_OPENGL_BACKEND
void LogFirstGLError(const char* where)
{
    const GLenum err = glGetError();
    if (err != 0u)
        Debug::LogError("OpenGLCommandList.cpp: %s GL error 0x%04X", where, static_cast<unsigned>(err));
}

bool ValidateFramebuffer(GLuint fbo, const char* where, bool hasColor, bool hasDepth)
{
    const GLenum status = glCheckFramebufferStatus(0x8D40u); // GL_FRAMEBUFFER
    if (status == 0x8CD5u) // GL_FRAMEBUFFER_COMPLETE
        return true;
    Debug::LogError("OpenGLCommandList.cpp: %s incomplete FBO=%u status=0x%04X color=%u depth=%u",
        where,
        static_cast<unsigned>(fbo),
        static_cast<unsigned>(status),
        static_cast<unsigned>(hasColor ? 1u : 0u),
        static_cast<unsigned>(hasDepth ? 1u : 0u));
    return false;
}
#else
void LogFirstGLError(const char*) {}
#endif
}


OpenGLCommandList::OpenGLCommandList(OGLDeviceResources& res, uint32_t* devCounter)
    : m_res(&res), m_devCounter(devCounter)
{
    for (auto& h : m_vb) h = BufferHandle::Invalid();
}

void OpenGLCommandList::Begin()
{
    m_pipeline = PipelineHandle::Invalid();
    m_ib       = BufferHandle::Invalid();
    m_draws    = 0u;
    for (auto& h : m_vb) h = BufferHandle::Invalid();
}

void OpenGLCommandList::End() {}

void OpenGLCommandList::BeginRenderPass(const RenderPassBeginInfo& info)
{
#ifdef KROM_OPENGL_BACKEND
    GLuint fbo = 0u;
    uint32_t rtWidth = 0u;
    uint32_t rtHeight = 0u;
    bool hasColor = true;
    bool hasDepth = true;
    if (info.renderTarget.IsValid()) {
        auto* rt = m_res->renderTargets.Get(info.renderTarget);
        if (rt) {
            fbo = rt->fbo;
            rtWidth = rt->width;
            rtHeight = rt->height;
            hasColor = rt->hasColor;
            hasDepth = rt->hasDepth;
        }
    }
    glBindFramebuffer(0x8D40u, fbo); // GL_FRAMEBUFFER
    if (fbo != 0u)
    {
        if (hasColor)
        {
            const GLenum drawBuffers[1] = { 0x8CE0u }; // GL_COLOR_ATTACHMENT0
            glDrawBuffers(1, drawBuffers);
            glReadBuffer(0x8CE0u); // GL_COLOR_ATTACHMENT0
        }
        else
        {
            glDrawBuffer(0);
            glReadBuffer(0);
        }
        ValidateFramebuffer(fbo, "BeginRenderPass", hasColor, hasDepth);
    }
    else
    {
        glDrawBuffer(0x0405u); // GL_BACK
        glReadBuffer(0x0405u); // GL_BACK
    }

    if (rtWidth == 0u || rtHeight == 0u)
    {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(0x0BA2u, vp); // GL_VIEWPORT
        rtWidth  = static_cast<uint32_t>(vp[2] > 0 ? vp[2] : 1);
        rtHeight = static_cast<uint32_t>(vp[3] > 0 ? vp[3] : 1);
    }

    glEnable(0x0C11u); // GL_SCISSOR_TEST
    glScissor(0, 0, static_cast<GLsizei>(rtWidth), static_cast<GLsizei>(rtHeight));

    GLbitfield clearMask = 0u;
    if (info.clearColor && hasColor) {
        glClearColor(info.colorClear.color[0], info.colorClear.color[1],
                     info.colorClear.color[2], info.colorClear.color[3]);
        clearMask |= 0x00004000u; // GL_COLOR_BUFFER_BIT
    }
    if (info.clearDepth && hasDepth) {
        glClearDepth(static_cast<GLdouble>(info.depthClear.depth));
        clearMask |= 0x00000100u; // GL_DEPTH_BUFFER_BIT
    }
    if (info.clearStencil && hasDepth) {
        glClearStencil(static_cast<GLint>(info.depthClear.stencil));
        clearMask |= 0x00000400u; // GL_STENCIL_BUFFER_BIT
    }
    if (clearMask) glClear(clearMask);
    LogFirstGLError("BeginRenderPass");
#else
    (void)info;
#endif
}

void OpenGLCommandList::EndRenderPass()
{
#ifdef KROM_OPENGL_BACKEND
    glDisable(0x0C11u); // GL_SCISSOR_TEST
    glBindFramebuffer(0x8D40u, 0u);
#endif
}

void OpenGLCommandList::SetPipeline(PipelineHandle pipeline)
{
    auto* p = m_res->pipelines.Get(pipeline);
    if (!p || !p->isValid()) return;
    m_pipeline = pipeline;
    m_topology = p->topology;

#ifdef KROM_OPENGL_BACKEND
    glUseProgram(p->program);
    glBindVertexArray(p->vao);

    // Depth State
    if (p->depthTest) { glEnable(0x0B71u); } // GL_DEPTH_TEST
    else              { glDisable(0x0B71u); }
    glDepthMask(p->depthWrite ? GL_TRUE : GL_FALSE);
    glDepthFunc(p->depthFunc);

    // Blend State
    if (p->blendEnable) {
        glEnable(0x0BE2u); // GL_BLEND
        glBlendEquation(p->blendOp);
        glBlendFunc(p->blendSrc, p->blendDst);
    } else {
        glDisable(0x0BE2u);
    }

    // Cull State
    if (p->cullEnable) {
        glEnable(0x0B44u); // GL_CULL_FACE
        glCullFace(p->cullFace);
    } else {
        glDisable(0x0B44u);
    }
    glFrontFace(p->frontFace);
    LogFirstGLError("SetPipeline");
#else
    (void)pipeline;
#endif
}

void OpenGLCommandList::SetVertexBuffer(uint32_t slot, BufferHandle buf, uint32_t offset)
{
    if (slot < kMaxVBSlots) {
        m_vb[slot]       = buf;
        m_vbOffset[slot] = offset;
    }
    // Tatsächliche glVertexAttribPointer-Calls kommen in BindVertexAttributes() vor Draw
}

void OpenGLCommandList::SetIndexBuffer(BufferHandle buf, bool is32bit, uint32_t /*offset*/)
{
    m_ib      = buf;
    m_index32 = is32bit;
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_res->buffers.Get(buf);
    if (e) glBindBuffer(0x8893u, e->glId); // GL_ELEMENT_ARRAY_BUFFER
#endif
}

void OpenGLCommandList::SetConstantBuffer(uint32_t slot, BufferHandle buf, ShaderStageMask)
{
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_res->buffers.Get(buf);
    if (e) glBindBufferBase(0x8A11u, slot, e->glId); // GL_UNIFORM_BUFFER
#else
    (void)slot; (void)buf;
#endif
}

void OpenGLCommandList::SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask)
{
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_res->buffers.Get(binding.buffer);
    if (!e) return;
    if (binding.offset == 0u)
    {
        // Kein Offset: glBindBufferBase reicht (kein Alignment-Check nötig)
        glBindBufferBase(0x8A11u, slot, e->glId);
    }
    else
    {
        // glBindBufferRange erfordert GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT-konformen Offset.
        // kConstantBufferAlignment (256) erfüllt typische Hardware-Anforderungen.
        glBindBufferRange(0x8A11u,                              // GL_UNIFORM_BUFFER
                          slot,
                          e->glId,
                          static_cast<GLintptr>(binding.offset),
                          static_cast<GLsizeiptr>(binding.size));
    }
#else
    (void)slot; (void)binding;
#endif
}

void OpenGLCommandList::SetShaderResource(uint32_t slot, TextureHandle tex, ShaderStageMask)
{
#ifdef KROM_OPENGL_BACKEND
    glActiveTexture(0x84C0u + slot); // GL_TEXTURE0 + slot
    auto* e = m_res->textures.Get(tex);
    glBindTexture(0x0DE1u, e ? e->glId : 0u); // GL_TEXTURE_2D
    LogFirstGLError("SetShaderResource");
#else
    (void)slot; (void)tex;
#endif
}

void OpenGLCommandList::SetSampler(uint32_t slot, uint32_t samplerIdx, ShaderStageMask)
{
#ifdef KROM_OPENGL_BACKEND
    if (samplerIdx < m_res->samplers.size())
        glBindSampler(slot, m_res->samplers[samplerIdx].glId);
#else
    (void)slot; (void)samplerIdx;
#endif
}

void OpenGLCommandList::SetViewport(float x, float y, float w, float h, float mn, float mx)
{
#ifdef KROM_OPENGL_BACKEND
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glDepthRangef(mn, mx);
#else
    (void)x; (void)y; (void)w; (void)h; (void)mn; (void)mx;
#endif
}

void OpenGLCommandList::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
#ifdef KROM_OPENGL_BACKEND
    glScissor(x, y, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

// ---------------------------------------------------------------------------
// BindVertexAttributes - GL 4.1: glVertexAttribPointer pro Attribut
// Wird vor jedem Draw aufgerufen um aktuelle VB-Bindings zu aktivieren.
// ---------------------------------------------------------------------------
void OpenGLCommandList::BindVertexAttributes()
{
#ifdef KROM_OPENGL_BACKEND
    auto* p = m_res->pipelines.Get(m_pipeline);
    if (!p) return;

    for (const auto& attr : p->vertexLayout.attributes)
    {
        const uint32_t slot = attr.binding < kMaxVBSlots ? attr.binding : 0u;
        auto* buf = m_res->buffers.Get(m_vb[slot]);
        if (!buf) continue;

        glBindBuffer(0x8892u, buf->glId); // GL_ARRAY_BUFFER
        const GLuint loc     = static_cast<GLuint>(attr.semantic);
        const GLint  count   = ToGLComponentCount(attr.format);
        const GLenum atype   = ToGLAttribType(attr.format);
        const GLsizei stride = static_cast<GLsizei>(buf->stride > 0u
            ? buf->stride
            : p->vertexLayout.bindings.empty() ? 0
            : p->vertexLayout.bindings[slot].stride);
        const void* ptr = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(attr.offset + m_vbOffset[slot]));

        glVertexAttribPointer(loc, count, atype, GL_FALSE, stride, ptr);
    }
    glBindBuffer(0x8892u, 0u);
    LogFirstGLError("BindVertexAttributes");
#endif
}

void OpenGLCommandList::Draw(uint32_t verts, uint32_t inst, uint32_t first, uint32_t)
{
#ifdef KROM_OPENGL_BACKEND
    BindVertexAttributes();
    if (inst > 1u)
        glDrawArraysInstanced(m_topology, static_cast<GLint>(first),
                              static_cast<GLsizei>(verts), static_cast<GLsizei>(inst));
    else
        glDrawArrays(m_topology, static_cast<GLint>(first), static_cast<GLsizei>(verts));
    LogFirstGLError("Draw");
#else
    (void)verts; (void)inst; (void)first;
#endif
    ++m_draws;
}

void OpenGLCommandList::DrawIndexed(uint32_t idx, uint32_t inst,
                                     uint32_t firstIdx, int32_t voff, uint32_t)
{
#ifdef KROM_OPENGL_BACKEND
    BindVertexAttributes();
    const GLenum idxType    = m_index32 ? 0x1405u : 0x1403u; // GL_UNSIGNED_INT / GL_UNSIGNED_SHORT
    const uintptr_t byteOff = static_cast<uintptr_t>(firstIdx) * (m_index32 ? 4u : 2u);
    const void* ptr         = reinterpret_cast<const void*>(byteOff);

    if (inst > 1u)
        glDrawElementsInstancedBaseVertex(
            m_topology, static_cast<GLsizei>(idx), idxType,
            ptr, static_cast<GLsizei>(inst), voff);
    else
        glDrawElementsBaseVertex(
            m_topology, static_cast<GLsizei>(idx), idxType, ptr, voff);
    LogFirstGLError("DrawIndexed");
#else
    (void)idx; (void)inst; (void)firstIdx; (void)voff;
#endif
    ++m_draws;
}

void OpenGLCommandList::Dispatch(uint32_t gx, uint32_t gy, uint32_t gz)
{
#ifdef KROM_OPENGL_BACKEND
    glDispatchCompute(gx, gy, gz);
    glMemoryBarrier(0xFFFFFFFFu); // GL_ALL_BARRIER_BITS
#else
    (void)gx; (void)gy; (void)gz;
#endif
}

// OpenGL ist implizit - keine expliziten Barriers nötig
void OpenGLCommandList::TransitionResource(BufferHandle, ResourceState, ResourceState) {}
void OpenGLCommandList::TransitionResource(TextureHandle, ResourceState, ResourceState) {}
void OpenGLCommandList::TransitionRenderTarget(RenderTargetHandle, ResourceState, ResourceState) {}

void OpenGLCommandList::CopyBuffer(BufferHandle dst, uint64_t dstOff,
                                    BufferHandle src, uint64_t srcOff, uint64_t size)
{
#ifdef KROM_OPENGL_BACKEND
    auto* d = m_res->buffers.Get(dst);
    auto* s = m_res->buffers.Get(src);
    if (!d || !s) return;
    glBindBuffer(0x8F36u, s->glId); // GL_COPY_READ_BUFFER
    glBindBuffer(0x8F37u, d->glId); // GL_COPY_WRITE_BUFFER
    glCopyBufferSubData(0x8F36u, 0x8F37u,
                        static_cast<GLintptr>(srcOff),
                        static_cast<GLintptr>(dstOff),
                        static_cast<GLsizeiptr>(size));
#else
    (void)dst; (void)dstOff; (void)src; (void)srcOff; (void)size;
#endif
}

void OpenGLCommandList::CopyTexture(TextureHandle dst, uint32_t dstMip,
                                     TextureHandle src, uint32_t srcMip)
{
#ifdef KROM_OPENGL_BACKEND
    auto* d = m_res->textures.Get(dst);
    auto* s = m_res->textures.Get(src);
    if (!d || !s) return;
    // glCopyImageSubData (GL 4.3) - bei 4.1 Fallback über FBO-Blit
    glBindFramebuffer(0x8CA8u, s->glId); // GL_READ_FRAMEBUFFER (missbraucht als src)
    glBindTexture(0x0DE1u, d->glId);
    const uint32_t mw = std::max(1u, s->width  >> srcMip);
    const uint32_t mh = std::max(1u, s->height >> srcMip);
    glCopyTexSubImage2D(0x0DE1u, static_cast<GLint>(dstMip),
                        0, 0, 0, 0,
                        static_cast<GLsizei>(mw), static_cast<GLsizei>(mh));
    glBindFramebuffer(0x8CA8u, 0u);
    glBindTexture(0x0DE1u, 0u);
#else
    (void)dst; (void)dstMip; (void)src; (void)srcMip;
#endif
}

void OpenGLCommandList::Submit(QueueType)
{
    if (m_devCounter) *m_devCounter += m_draws;
    m_draws = 0u;
}

} // namespace engine::renderer::opengl
