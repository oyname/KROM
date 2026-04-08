// =============================================================================
// KROM Engine - OpenGLResources.cpp
// Ressourcen-Erstellung: Buffer, Texture, RenderTarget, Shader, Pipeline, Sampler.
// Echter GL-Code hinter KROM_OPENGL_BACKEND.
// =============================================================================
#include "OpenGLDevice.hpp"
#include "core/Debug.hpp"
#include <cstring>

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

// =============================================================================
// Buffer
// =============================================================================

BufferHandle OpenGLDevice::CreateBuffer(const BufferDesc& desc)
{
    OGLBufferEntry e;
    e.target  = ToGLBufferTarget(desc.type);
    e.usage   = ToGLBufferUsage(desc.access);
    e.size    = static_cast<GLsizei>(desc.byteSize);
    e.stride  = desc.stride;
    e.dynamic = (desc.access == MemoryAccess::CpuWrite);

#ifdef KROM_OPENGL_BACKEND
    glGenBuffers(1, &e.glId);
    glBindBuffer(e.target, e.glId);
    glBufferData(e.target, static_cast<GLsizeiptr>(desc.byteSize), nullptr, e.usage);
    glBindBuffer(e.target, 0u);
    Debug::LogVerbose("OpenGLResources.cpp: CreateBuffer '%s' %zu bytes GL=%u",
        desc.debugName.c_str(), desc.byteSize, e.glId);
#endif

    return m_resources.buffers.Add(std::move(e));
}

void OpenGLDevice::DestroyBuffer(BufferHandle h)
{
    auto* e = m_resources.buffers.Get(h);
    if (!e) return;
#ifdef KROM_OPENGL_BACKEND
    if (e->glId) glDeleteBuffers(1, &e->glId);
#endif
    m_resources.buffers.Remove(h);
}

void* OpenGLDevice::MapBuffer(BufferHandle h)
{
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_resources.buffers.Get(h);
    if (!e || e->mapped) return nullptr;
    glBindBuffer(e->target, e->glId);
    void* ptr = glMapBuffer(e->target, GL_WRITE_ONLY);
    glBindBuffer(e->target, 0u);
    e->mapped = (ptr != nullptr);
    return ptr;
#else
    (void)h; return nullptr;
#endif
}

void OpenGLDevice::UnmapBuffer(BufferHandle h)
{
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_resources.buffers.Get(h);
    if (!e || !e->mapped) return;
    glBindBuffer(e->target, e->glId);
    glUnmapBuffer(e->target);
    glBindBuffer(e->target, 0u);
    e->mapped = false;
#else
    (void)h;
#endif
}

void OpenGLDevice::UploadBufferData(BufferHandle h, const void* data,
                                     size_t sz, size_t off)
{
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_resources.buffers.Get(h);
    if (!e || !data) return;
    glBindBuffer(e->target, e->glId);
    glBufferSubData(e->target, static_cast<GLintptr>(off),
                    static_cast<GLsizeiptr>(sz), data);
    glBindBuffer(e->target, 0u);
#else
    (void)h; (void)data; (void)sz; (void)off;
#endif
}

// =============================================================================
// Texture
// =============================================================================

TextureHandle OpenGLDevice::CreateTexture(const TextureDesc& desc)
{
    OGLTextureEntry e;
    e.target  = 0x0DE1u; // GL_TEXTURE_2D
    e.intFmt  = ToGLInternalFormat(desc.format);
    e.baseFmt = ToGLBaseFormat(desc.format);
    e.type    = ToGLPixelType(desc.format);
    e.width   = desc.width;
    e.height  = desc.height;
    e.mips    = desc.mipLevels;

#ifdef KROM_OPENGL_BACKEND
    glGenTextures(1, &e.glId);
    glBindTexture(e.target, e.glId);
    glTexStorage2D(e.target,
                   static_cast<GLsizei>(desc.mipLevels),
                   e.intFmt,
                   static_cast<GLsizei>(desc.width),
                   static_cast<GLsizei>(desc.height));
    // Standard-Sampler-Parameter
    glTexParameteri(e.target, 0x2800u, 0x2601u); // TEXTURE_MAG_FILTER = LINEAR
    glTexParameteri(e.target, 0x2801u, desc.mipLevels > 1 ? 0x2703u : 0x2601u); // LINEAR_MIPMAP_LINEAR / LINEAR
    glTexParameteri(e.target, 0x2802u, 0x812Fu); // TEXTURE_WRAP_S = CLAMP_TO_EDGE
    glTexParameteri(e.target, 0x2803u, 0x812Fu); // TEXTURE_WRAP_T = CLAMP_TO_EDGE
    glTexParameteri(e.target, 0x813Cu, 0); // GL_TEXTURE_BASE_LEVEL
    glTexParameteri(e.target, 0x813Du, static_cast<GLint>(desc.mipLevels > 0 ? desc.mipLevels - 1 : 0)); // GL_TEXTURE_MAX_LEVEL
    glBindTexture(e.target, 0u);
    Debug::LogVerbose("OpenGLResources.cpp: CreateTexture '%s' %ux%u GL=%u",
        desc.debugName.c_str(), desc.width, desc.height, e.glId);
#endif

    return m_resources.textures.Add(std::move(e));
}

void OpenGLDevice::DestroyTexture(TextureHandle h)
{
    auto* e = m_resources.textures.Get(h);
    if (!e) return;
#ifdef KROM_OPENGL_BACKEND
    if (e->glId) glDeleteTextures(1, &e->glId);
#endif
    m_resources.textures.Remove(h);
}

void OpenGLDevice::UploadTextureData(TextureHandle h, const void* data,
                                      size_t, uint32_t mip, uint32_t)
{
#ifdef KROM_OPENGL_BACKEND
    auto* e = m_resources.textures.Get(h);
    if (!e || !data) return;
    const uint32_t w = std::max(1u, e->width  >> mip);
    const uint32_t h2= std::max(1u, e->height >> mip);
    glBindTexture(e->target, e->glId);
    glTexSubImage2D(e->target, static_cast<GLint>(mip),
                    0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h2),
                    e->baseFmt, e->type, data);
    glBindTexture(e->target, 0u);
#else
    (void)h; (void)data; (void)mip;
#endif
}

// =============================================================================
// RenderTarget
// =============================================================================

RenderTargetHandle OpenGLDevice::CreateRenderTarget(const RenderTargetDesc& desc)
{
    OGLRenderTargetEntry e;
    e.width  = desc.width;
    e.height = desc.height;
    e.hasColor = desc.hasColor;
    e.hasDepth = desc.hasDepth;

    // Color-Textur
    if (desc.hasColor) {
        TextureDesc td{};
        td.width = desc.width; td.height = desc.height;
        td.format = desc.colorFormat; td.mipLevels = 1;
        td.usage  = ResourceUsage::RenderTarget | ResourceUsage::ShaderResource;
        td.debugName = desc.debugName + "_Color";
        e.colorHandle = CreateTexture(td);
        if (auto* t = m_resources.textures.Get(e.colorHandle))
            e.colorTex = t->glId;
    }

    // Depth-Textur
    if (desc.hasDepth) {
        TextureDesc td{};
        td.width = desc.width; td.height = desc.height;
        td.format = desc.depthFormat; td.mipLevels = 1;
        td.usage  = ResourceUsage::DepthStencil | ResourceUsage::ShaderResource;
        td.debugName = desc.debugName + "_Depth";
        e.depthHandle = CreateTexture(td);
        if (auto* t = m_resources.textures.Get(e.depthHandle))
            e.depthTex = t->glId;
    }

#ifdef KROM_OPENGL_BACKEND
    glGenFramebuffers(1, &e.fbo);
    glBindFramebuffer(0x8D40u, e.fbo); // GL_FRAMEBUFFER

    if (e.colorTex)
        glFramebufferTexture2D(0x8D40u, 0x8CE0u, 0x0DE1u, e.colorTex, 0); // COLOR_ATTACHMENT0

    if (e.depthTex) {
        const bool hasStencil = (desc.depthFormat == Format::D24_UNORM_S8_UINT ||
                                 desc.depthFormat == Format::D32_FLOAT_S8X24_UINT);
        const GLenum attach = hasStencil ? 0x821Au : 0x8D00u; // DEPTH_STENCIL_ATTACHMENT / DEPTH_ATTACHMENT
        glFramebufferTexture2D(0x8D40u, attach, 0x0DE1u, e.depthTex, 0);
    }
    if (e.colorTex)
    {
        const GLenum drawBuffers[1] = { 0x8CE0u }; // GL_COLOR_ATTACHMENT0
        glDrawBuffers(1, drawBuffers);
    }
    else
    {
        glDrawBuffer(0);
        glReadBuffer(0);
    }

    const GLenum fboStatus = glCheckFramebufferStatus(0x8D40u);
    if (fboStatus != 0x8CD5u)
        Debug::LogError("OpenGLResources.cpp: CreateRenderTarget '%s' incomplete FBO status=0x%04X",
            desc.debugName.c_str(), static_cast<unsigned>(fboStatus));

    glBindFramebuffer(0x8D40u, 0u);
    Debug::Log("OpenGLResources.cpp: CreateRenderTarget '%s' %ux%u FBO=%u",
        desc.debugName.c_str(), desc.width, desc.height, e.fbo);
#endif

    return m_resources.renderTargets.Add(std::move(e));
}

void OpenGLDevice::DestroyRenderTarget(RenderTargetHandle h)
{
    auto* e = m_resources.renderTargets.Get(h);
    if (!e) return;
    if (e->colorHandle.IsValid()) DestroyTexture(e->colorHandle);
    if (e->depthHandle.IsValid()) DestroyTexture(e->depthHandle);
#ifdef KROM_OPENGL_BACKEND
    if (e->fbo) glDeleteFramebuffers(1, &e->fbo);
#endif
    m_resources.renderTargets.Remove(h);
}

TextureHandle OpenGLDevice::GetRenderTargetColorTexture(RenderTargetHandle h) const {
    const auto* e = m_resources.renderTargets.Get(h);
    return e ? e->colorHandle : TextureHandle::Invalid();
}

TextureHandle OpenGLDevice::GetRenderTargetDepthTexture(RenderTargetHandle h) const {
    const auto* e = m_resources.renderTargets.Get(h);
    return e ? e->depthHandle : TextureHandle::Invalid();
}

// =============================================================================
// Shader
// =============================================================================

ShaderHandle OpenGLDevice::CreateShaderFromSource(const std::string& src,
                                                   ShaderStageMask stage,
                                                   const std::string& /*entry*/,
                                                   const std::string& dbg)
{
    OGLShaderEntry e;
    e.stage    = stage;
    e.debugName = dbg;
    e.type     = ToGLShaderType(stage);

#ifdef KROM_OPENGL_BACKEND
    e.glId = glCreateShader(e.type);
    const char* src_ptr = src.c_str();
    const GLint src_len = static_cast<GLint>(src.size());
    glShaderSource(e.glId, 1, &src_ptr, &src_len);
    glCompileShader(e.glId);

    GLint status = 0;
    glGetShaderiv(e.glId, 0x8B81u, &status); // GL_COMPILE_STATUS
    if (status == 0) {
        GLint logLen = 0;
        glGetShaderiv(e.glId, 0x8B84u, &logLen); // GL_INFO_LOG_LENGTH
        std::string log(static_cast<size_t>(logLen), '\0');
        glGetShaderInfoLog(e.glId, logLen, nullptr, log.data());
        Debug::LogError("OpenGLResources.cpp: Shader compile error '%s':\n%s",
            dbg.c_str(), log.c_str());
        glDeleteShader(e.glId);
        return ShaderHandle::Invalid();
    }
    Debug::LogVerbose("OpenGLResources.cpp: CreateShader '%s' GL=%u", dbg.c_str(), e.glId);
#endif

    return m_resources.shaders.Add(std::move(e));
}

ShaderHandle OpenGLDevice::CreateShaderFromBytecode(const void* data, size_t sz,
                                                     ShaderStageMask stage,
                                                     const std::string& dbg)
{
    // GLSL-Bytecode = Quelltext als Bytes
    const std::string src(static_cast<const char*>(data), sz);
    return CreateShaderFromSource(src, stage, "main", dbg);
}

void OpenGLDevice::DestroyShader(ShaderHandle h)
{
    auto* e = m_resources.shaders.Get(h);
    if (!e) return;
#ifdef KROM_OPENGL_BACKEND
    if (e->glId) glDeleteShader(e->glId);
#endif
    m_resources.shaders.Remove(h);
}

// =============================================================================
// Pipeline (Program + VAO)
// =============================================================================

PipelineHandle OpenGLDevice::CreatePipeline(const PipelineDesc& desc)
{
    OGLPipelineState p;
    p.topology    = ToGLTopology(desc.topology);
    p.depthTest   = desc.depthStencil.depthEnable;
    p.depthWrite  = desc.depthStencil.depthWrite;
    p.depthFunc   = ToGLCompareFunc(desc.depthStencil.depthFunc);
    p.blendEnable = desc.blendStates[0].blendEnable;
    p.blendSrc    = ToGLBlendFactor(desc.blendStates[0].srcBlend);
    p.blendDst    = ToGLBlendFactor(desc.blendStates[0].dstBlend);
    p.blendOp     = ToGLBlendOp(desc.blendStates[0].blendOp);
    p.cullEnable  = (desc.rasterizer.cullMode != CullMode::None);
    p.cullFace    = (desc.rasterizer.cullMode == CullMode::Front) ? 0x0404u : 0x0405u; // FRONT / BACK
    p.frontFace   = (desc.rasterizer.frontFace == WindingOrder::CW) ? 0x0901u : 0x0900u; // CW / CCW
    p.vertexLayout = desc.vertexLayout;

#ifdef KROM_OPENGL_BACKEND
    // Vertex- und Fragment-Shader-IDs sammeln
    GLuint vsId = 0u, psId = 0u;
    for (const auto& stage : desc.shaderStages) {
        auto* se = m_resources.shaders.Get(stage.handle);
        if (!se) continue;
        if (se->type == 0x8B31u) vsId = se->glId; // GL_VERTEX_SHADER
        if (se->type == 0x8B30u) psId = se->glId; // GL_FRAGMENT_SHADER
    }
    if (!vsId) {
        Debug::LogError("OpenGLResources.cpp: CreatePipeline '%s' - VS fehlt",
            desc.debugName.c_str());
        return PipelineHandle::Invalid();
    }

    p.program = glCreateProgram();
    glAttachShader(p.program, vsId);
    if (psId)
        glAttachShader(p.program, psId);

    // GL 4.1: Bindings explizit setzen statt auf layout(binding=...) zu vertrauen.
    glBindAttribLocation(p.program, 0u, "aPosition");
    glBindAttribLocation(p.program, 1u, "aNormal");
    glBindAttribLocation(p.program, 2u, "aTangent");
    glBindAttribLocation(p.program, 3u, "aBitangent");
    glBindAttribLocation(p.program, 4u, "aTexCoord");
    glBindAttribLocation(p.program, 5u, "aTexCoord1");
    glBindAttribLocation(p.program, 6u, "aColor");
    glLinkProgram(p.program);

    GLint linked = 0;
    glGetProgramiv(p.program, 0x8B82u, &linked); // GL_LINK_STATUS
    if (linked == 0) {
        GLint logLen = 0;
        glGetProgramiv(p.program, 0x8B84u, &logLen); // GL_INFO_LOG_LENGTH
        std::string log(static_cast<size_t>(logLen), '\0');
        glGetProgramInfoLog(p.program, logLen, nullptr, log.data());
        Debug::LogError("OpenGLResources.cpp: Program link error '%s':\n%s",
            desc.debugName.c_str(), log.c_str());
        glDeleteProgram(p.program);
        return PipelineHandle::Invalid();
    }
    glDetachShader(p.program, vsId);
    if (psId) glDetachShader(p.program, psId);

    glUseProgram(p.program);
    auto bindBlock = [&](const char* name, GLuint slot)
    {
        const GLuint idx = glGetUniformBlockIndex(p.program, name);
        if (idx != 0xFFFFFFFFu)
            glUniformBlockBinding(p.program, idx, slot);
    };
    bindBlock("PerFrame", 0u);
    bindBlock("PerObject", 1u);
    bindBlock("PerMaterial", 2u);
    bindBlock("PerPass", 3u);

    auto bindSampler = [&](const char* name, GLint slot)
    {
        const GLint loc = glGetUniformLocation(p.program, name);
        if (loc >= 0)
            glUniform1i(loc, slot);
    };
    bindSampler("uAlbedo", 0);
    bindSampler("uHDRInput", 0);
    glUseProgram(0u);

    // VAO erstellen - Attribute werden in SetVertexBuffer über glVertexAttribPointer gesetzt
    glGenVertexArrays(1, &p.vao);
    glBindVertexArray(p.vao);
    // Alle genutzten Attrib-Locations aktivieren
    for (const auto& attr : desc.vertexLayout.attributes)
        glEnableVertexAttribArray(static_cast<GLuint>(attr.semantic));
    glBindVertexArray(0u);

    Debug::LogVerbose("OpenGLResources.cpp: CreatePipeline '%s' program=%u vao=%u",
        desc.debugName.c_str(), p.program, p.vao);
#endif

    return m_resources.pipelines.Add(std::move(p));
}

void OpenGLDevice::DestroyPipeline(PipelineHandle h)
{
    auto* e = m_resources.pipelines.Get(h);
    if (!e) return;
#ifdef KROM_OPENGL_BACKEND
    if (e->program) glDeleteProgram(e->program);
    if (e->vao)     glDeleteVertexArrays(1, &e->vao);
#endif
    m_resources.pipelines.Remove(h);
}

// =============================================================================
// Sampler
// =============================================================================

uint32_t OpenGLDevice::CreateSampler(const SamplerDesc& desc)
{
    OGLSamplerEntry e;
#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
    if (!glGenSamplers || !glSamplerParameteri)
    {
        Debug::LogError("OpenGLResources.cpp: sampler functions are not loaded");
        const uint32_t idx = static_cast<uint32_t>(m_resources.samplers.size());
        m_resources.samplers.push_back(std::move(e));
        return idx;
    }
#   endif
    glGenSamplers(1, &e.glId);
    glSamplerParameteri(e.glId, 0x2801u, static_cast<GLint>(ToGLMinFilter(desc.minFilter, desc.mipFilter)));
    glSamplerParameteri(e.glId, 0x2800u, static_cast<GLint>(ToGLMagFilter(desc.magFilter)));
    glSamplerParameteri(e.glId, 0x2802u, static_cast<GLint>(ToGLWrapMode(desc.addressU)));
    glSamplerParameteri(e.glId, 0x2803u, static_cast<GLint>(ToGLWrapMode(desc.addressV)));
    glSamplerParameteri(e.glId, 0x8072u, static_cast<GLint>(ToGLWrapMode(desc.addressW)));
    if (desc.maxAniso > 1u) {
        glSamplerParameteri(e.glId, 0x84FEu, static_cast<GLint>(desc.maxAniso)); // GL_TEXTURE_MAX_ANISOTROPY_EXT
    }
#endif
    const uint32_t idx = static_cast<uint32_t>(m_resources.samplers.size());
    m_resources.samplers.push_back(std::move(e));
    return idx;
}

// =============================================================================
// Shutdown - alle GL-Objekte freigeben
// =============================================================================

void OpenGLDevice::Shutdown()
{
    if (!m_initialized) return;

#ifdef KROM_OPENGL_BACKEND
    m_resources.pipelines.ForEach([](OGLPipelineState& p) {
        if (p.program) glDeleteProgram(p.program);
        if (p.vao)     glDeleteVertexArrays(1, &p.vao);
    });
    m_resources.shaders.ForEach([](OGLShaderEntry& s) {
        if (s.glId) glDeleteShader(s.glId);
    });
    m_resources.textures.ForEach([](OGLTextureEntry& t) {
        if (t.glId) glDeleteTextures(1, &t.glId);
    });
    m_resources.renderTargets.ForEach([](OGLRenderTargetEntry& rt) {
        if (rt.fbo) glDeleteFramebuffers(1, &rt.fbo);
        // colorTex/depthTex werden über TextureStore freigegeben
    });
    m_resources.buffers.ForEach([](OGLBufferEntry& b) {
        if (b.glId) glDeleteBuffers(1, &b.glId);
    });
    for (auto& s : m_resources.samplers)
        if (s.glId) glDeleteSamplers(1, &s.glId);
#endif

    m_resources.pipelines.Clear();
    m_resources.shaders.Clear();
    m_resources.textures.Clear();
    m_resources.renderTargets.Clear();
    m_resources.buffers.Clear();
    m_resources.samplers.clear();
    m_initialized = false;
    Debug::Log("OpenGLDevice.cpp: Shutdown");
}

} // namespace engine::renderer::opengl
