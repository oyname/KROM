#pragma once
// =============================================================================
// KROM Engine - addons/opengl/OpenGLDevice.hpp  (privat)
//
// GL-Objekt-IDs (GLuint = unsigned int) in Ressource-Structs.
// Echter GL-Code nur hinter #ifdef KROM_OPENGL_BACKEND.
// GL 4.1 Core Profile - minimum für macOS-Kompatibilität.
//
// VAO / VertexAttribPointer statt ARB_vertex_attrib_binding (4.3),
// damit macOS unterstützt wird.
// =============================================================================
#include "renderer/IDevice.hpp"
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

// Vorwärtsdeklaration: kein GLFW-Header im privaten Header nötig
struct GLFWwindow;

// GLenum / GLuint sind beide unsigned int - direkte typedef vermeidet glad-Include im Header
using GLenum  = unsigned int;
using GLuint  = unsigned int;
using GLint   = int;
using GLsizei = int;

namespace engine::renderer::opengl {

#if defined(_WIN32)
bool EnsureOpenGLMainContextCurrent();
void RegisterOpenGLMainContext(void* deviceContext, void* renderContext);
void ClearOpenGLMainContextRegistration(void* deviceContext, void* renderContext);
#endif

// =============================================================================
// OGLStore - generationssicherer Slot-Allocator (analog DX11Store)
// =============================================================================
template<typename HandleT, typename EntryT>
class OGLStore
{
    struct Slot { EntryT data{}; uint32_t gen = 0u; bool alive = false; };
    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_free;
public:
    OGLStore() { m_slots.emplace_back(); }  // Slot 0 = Invalid

    HandleT Add(EntryT e)
    {
        uint32_t idx;
        if (!m_free.empty()) { idx = m_free.back(); m_free.pop_back(); }
        else                 { idx = static_cast<uint32_t>(m_slots.size()); m_slots.emplace_back(); }
        auto& s = m_slots[idx];
        s.data = std::move(e); s.alive = true; ++s.gen;
        if ((s.gen & HandleT::GEN_MASK) == 0u)
            ++s.gen;
        return HandleT::Make(idx, s.gen & HandleT::GEN_MASK);
    }

    EntryT* Get(HandleT h) noexcept
    {
        const uint32_t i = h.Index();
        if (i == 0u || i >= m_slots.size()) return nullptr;
        auto& s = m_slots[i];
        return (s.alive && ((s.gen & HandleT::GEN_MASK) == h.Generation())) ? &s.data : nullptr;
    }
    const EntryT* Get(HandleT h) const noexcept { return const_cast<OGLStore*>(this)->Get(h); }

    bool Remove(HandleT h) noexcept
    {
        const uint32_t i = h.Index();
        if (i == 0u || i >= m_slots.size()) return false;
        auto& s = m_slots[i];
        if (!s.alive || ((s.gen & HandleT::GEN_MASK) != h.Generation())) return false;
        s.alive = false; s.data = EntryT{}; m_free.push_back(i); return true;
    }

    template<typename Fn> void ForEach(Fn&& fn)
    { for (auto& s : m_slots) if (s.alive) fn(s.data); }

    void Clear() { m_slots.clear(); m_free.clear(); m_slots.emplace_back(); }
};

// =============================================================================
// Ressourcen-Structs - echte GL-Objekt-IDs
// =============================================================================

struct OGLBufferEntry
{
    GLuint   glId    = 0u;  // glGenBuffers
    GLenum   target  = 0u;  // GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER / GL_UNIFORM_BUFFER
    GLenum   usage   = 0u;  // GL_STATIC_DRAW / GL_DYNAMIC_DRAW
    GLsizei  size    = 0;
    uint32_t stride  = 0u;  // Byte-Stride für glVertexAttribPointer (aus BufferDesc::stride)
    bool     dynamic = false;
    bool     mapped  = false;
};

struct OGLTextureEntry
{
    GLuint glId    = 0u;  // glGenTextures
    GLenum target  = 0u;  // GL_TEXTURE_2D / GL_TEXTURE_2D_ARRAY / GL_TEXTURE_CUBE_MAP
    GLenum intFmt  = 0u;  // internalFormat
    GLenum baseFmt = 0u;  // baseFormat (GL_RGBA, GL_DEPTH_COMPONENT, ...)
    GLenum type    = 0u;  // GL_UNSIGNED_BYTE, GL_FLOAT, ...
    engine::renderer::Format format = engine::renderer::Format::Unknown;
    uint32_t width     = 0u;
    uint32_t height    = 0u;
    uint32_t depth     = 1u;
    uint32_t arraySize = 1u;
    uint32_t mips      = 1u;
    TextureDimension dimension = TextureDimension::Tex2D;
};

struct OGLRenderTargetEntry
{
    GLuint fbo       = 0u;  // Framebuffer Object
    GLuint colorTex  = 0u;  // Owned color texture (0 = backbuffer)
    GLuint depthTex  = 0u;  // Owned depth texture (0 = kein Depth)
    TextureHandle colorHandle;
    TextureHandle depthHandle;
    uint32_t width  = 0u;
    uint32_t height = 0u;
    bool     hasColor = false;
    bool     hasDepth = false;
    bool     isBackbuffer = false;
};

struct OGLShaderEntry
{
    GLuint          glId  = 0u;  // glCreateShader
    GLenum          type  = 0u;  // GL_VERTEX_SHADER / GL_FRAGMENT_SHADER / GL_COMPUTE_SHADER
    ShaderStageMask stage = ShaderStageMask::None;
    std::string     debugName;
};

struct OGLPipelineState
{
    GLuint   program  = 0u;  // glCreateProgram (linked VS+PS)
    GLuint   vao      = 0u;  // Vertex Array Object
    GLenum   topology = 0u;  // GL_TRIANGLES, GL_LINES, ...
    // Render-States (in SetPipeline angewendet)
    bool     depthTest    = true;
    bool     depthWrite   = true;
    GLenum   depthFunc    = 0u;   // GL_LESS
    bool     blendEnable  = false;
    GLenum   blendSrc     = 0u;   // GL_SRC_ALPHA
    GLenum   blendDst     = 0u;   // GL_ONE_MINUS_SRC_ALPHA
    GLenum   blendOp      = 0u;   // GL_FUNC_ADD
    bool     cullEnable   = true;
    GLenum   cullFace     = 0u;   // GL_BACK
    GLenum   frontFace    = 0u;   // GL_CCW
    bool     polygonOffsetEnable = false;
    float    polygonOffsetFactor = 0.f;
    float    polygonOffsetUnits  = 0.f;
    // Vertex-Layout - gespeichert für SetVertexBuffer (GL 4.1 braucht glVertexAttribPointer)
    VertexLayout vertexLayout;

    [[nodiscard]] bool isValid() const noexcept { return program != 0u; }
};

struct OGLSamplerEntry
{
    GLuint glId = 0u;  // glGenSamplers
};

// =============================================================================
// OGLDeviceResources
// =============================================================================
struct OGLDeviceResources
{
    OGLStore<BufferHandle,       OGLBufferEntry>       buffers;
    OGLStore<TextureHandle,      OGLTextureEntry>      textures;
    OGLStore<RenderTargetHandle, OGLRenderTargetEntry> renderTargets;
    OGLStore<ShaderHandle,       OGLShaderEntry>       shaders;
    OGLStore<PipelineHandle,     OGLPipelineState>     pipelines;
    std::vector<OGLSamplerEntry>                       samplers;
};

// =============================================================================
// Converter-Deklarationen (Implementierung in OpenGLConverters.cpp)
// =============================================================================
GLenum   ToGLTopology(PrimitiveTopology topo) noexcept;
GLenum   ToGLBlendFactor(BlendFactor factor) noexcept;
GLenum   ToGLBlendOp(BlendOp op) noexcept;
GLenum   ToGLCompareFunc(DepthFunc func) noexcept;
GLenum   ToGLBufferTarget(BufferType type) noexcept;
GLenum   ToGLBufferUsage(MemoryAccess access) noexcept;
GLenum   ToGLInternalFormat(Format fmt) noexcept;
GLenum   ToGLBaseFormat(Format fmt) noexcept;
GLenum   ToGLPixelType(Format fmt) noexcept;
GLenum   ToGLAttribType(Format fmt) noexcept;
GLint    ToGLComponentCount(Format fmt) noexcept;
GLenum   ToGLWrapMode(WrapMode mode) noexcept;
GLenum   ToGLMinFilter(FilterMode min, FilterMode mip) noexcept;
GLenum   ToGLMagFilter(FilterMode mag) noexcept;
GLenum   ToGLShaderType(ShaderStageMask stage) noexcept;
GLenum   ToGLFrontFace(WindingOrder order) noexcept;

// =============================================================================
// OpenGLSwapchain
// =============================================================================
class OpenGLSwapchain final : public ISwapchain
{
public:
    OpenGLSwapchain(void* nativeWindowHandle, OGLDeviceResources& res,
                    uint32_t width, uint32_t height,
                    int openglMajor = 4, int openglMinor = 1,
                    bool openglDebugContext = false);
    ~OpenGLSwapchain() override;

    bool               AcquireForFrame()              override;
    void               Present(bool vsync)             override;
    void               Resize(uint32_t w, uint32_t h) override;
    uint32_t           GetCurrentBackbufferIndex() const override { return 0u; }
    TextureHandle      GetBackbufferTexture(uint32_t)  const override;
    RenderTargetHandle GetBackbufferRenderTarget(uint32_t) const override;
    uint32_t           GetWidth()  const override;
    uint32_t           GetHeight() const override;
    bool               CanRenderFrame() const override { return m_bbRT.IsValid() && m_width > 0u && m_height > 0u; }
    bool               NeedsRecreate() const override { return false; }
    SwapchainFrameStatus QueryFrameStatus() const override;
    SwapchainRuntimeDesc GetRuntimeDesc() const override;
    [[nodiscard]] Format GetBackbufferFormat() const override { return Format::RGBA8_UNORM_SRGB; }

private:
    void SyncWindowSizeFromNative() const;
    bool InitializeNativeContext();
    void DestroyNativeContext();

    void*                m_nativeWindowHandle = nullptr;
    OGLDeviceResources*  m_res        = nullptr;
    RenderTargetHandle   m_bbRT;       // Pseudo-RT für Backbuffer (FBO=0)
    uint32_t             m_width  = 0u;
    uint32_t             m_height = 0u;
    bool                 m_glLoaded = false;
    int                  m_openglMajor        = 4;
    int                  m_openglMinor        = 1;
    bool                 m_openglDebugContext = false;
#if defined(_WIN32)
    void*                m_win32DeviceContext = nullptr;
    void*                m_win32GlContext     = nullptr;
#endif
};

// =============================================================================
// OpenGLFence - glFenceSync / glClientWaitSync (GL 3.2+)
// =============================================================================
class OpenGLFence final : public IFence
{
public:
    explicit OpenGLFence(uint64_t init) : m_completedValue(init), m_lastSignaledValue(init) {}
    ~OpenGLFence() override;

    void     Signal(uint64_t value)                  override;
    void     Wait(uint64_t value, uint64_t timeoutNs) override;
    uint64_t GetValue() const                        override;

private:
    void PollCompletion(uint64_t timeoutNs) const;

    mutable std::atomic<uint64_t> m_completedValue{0u};
    mutable std::atomic<uint64_t> m_lastSignaledValue{0u};
    void*                 m_sync = nullptr;  // GLsync = struct __GLsync* = void*
};

// =============================================================================
// OpenGLCommandList
// =============================================================================
class OpenGLCommandList final : public ICommandList
{
public:
    explicit OpenGLCommandList(OGLDeviceResources& resources,
                                uint32_t* deviceDrawCalls);

    void Begin() override;
    void End()   override;
    void BeginRenderPass(const RenderPassBeginInfo& info) override;
    void EndRenderPass() override;
    void SetPipeline(PipelineHandle pipeline) override;
    void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint32_t offset) override;
    void SetIndexBuffer(BufferHandle buffer, bool is32bit, uint32_t offset)   override;
    void SetConstantBuffer(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override;
    void SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask stages) override;
    void SetShaderResource(uint32_t slot, TextureHandle texture, ShaderStageMask stages) override;
    void SetShaderResource(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override;
    void SetSampler(uint32_t slot, uint32_t samplerIndex, ShaderStageMask stages) override;
    void SetViewport(float x, float y, float w, float h, float mn, float mx) override;
    void SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) override;
    void Draw(uint32_t verts, uint32_t inst, uint32_t first, uint32_t firstInst) override;
    void DrawIndexed(uint32_t idx, uint32_t inst, uint32_t firstIdx,
                     int32_t voff, uint32_t firstInst) override;
    void Dispatch(uint32_t gx, uint32_t gy, uint32_t gz) override;
    void TransitionResource(BufferHandle, ResourceState, ResourceState) override;
    void TransitionResource(TextureHandle, ResourceState, ResourceState) override;
    void TransitionRenderTarget(RenderTargetHandle, ResourceState, ResourceState) override;
    void CopyBuffer(BufferHandle dst, uint64_t dstOff,
                    BufferHandle src, uint64_t srcOff, uint64_t size) override;
    void CopyTexture(TextureHandle dst, uint32_t dstMip,
                     TextureHandle src, uint32_t srcMip) override;
    void Submit(QueueType queue) override;
    [[nodiscard]] QueueType GetQueueType() const override { return QueueType::Graphics; }

private:
    // Beim Draw: glVertexAttribPointer pro Attribut setzen (GL 4.1)
    void BindVertexAttributes();

    OGLDeviceResources*  m_res        = nullptr;
    uint32_t*            m_devCounter = nullptr;
    PipelineHandle       m_pipeline;
    // Slot → (BufferHandle, offset) - maximal 8 Vertex-Buffer-Slots
    static constexpr uint32_t kMaxVBSlots = 8u;
    BufferHandle m_vb[kMaxVBSlots];
    uint32_t     m_vbOffset[kMaxVBSlots]{};
    BufferHandle m_ib;
    bool         m_index32  = true;
    GLenum       m_topology = 0x0004u; // GL_TRIANGLES default
    uint32_t     m_draws    = 0u;
    RenderTargetHandle m_activeRT;
    uint32_t     m_activeRTWidth = 0u;
    uint32_t     m_activeRTHeight = 0u;
    bool         m_activeRTIsBackbuffer = false;
    bool         m_depthTest = false;
    bool         m_depthWrite = false;
    bool         m_blend = false;
    bool         m_cullFace = false;
    bool         m_scissorEnabled = false;
    int32_t      m_scissor[4] = {0, 0, 0, 0};
    float        m_viewport[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 1.f};
};

// =============================================================================
// OpenGLDevice
// =============================================================================
class OpenGLDevice final : public IDevice
{
public:
     OpenGLDevice() = default;
    ~OpenGLDevice() override;

    bool Initialize(const DeviceDesc& desc) override;
    void Shutdown()                          override;
    void WaitIdle()                          override;

    std::unique_ptr<ISwapchain> CreateSwapchain(const SwapchainDesc& desc) override;

    BufferHandle CreateBuffer(const BufferDesc& desc)  override;
    void         DestroyBuffer(BufferHandle h)         override;
    void*        MapBuffer(BufferHandle h)              override;
    void         UnmapBuffer(BufferHandle h)            override;

    TextureHandle CreateTexture(const TextureDesc& desc)  override;
    void          DestroyTexture(TextureHandle h)         override;

    RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) override;
    void               DestroyRenderTarget(RenderTargetHandle h)        override;
    TextureHandle      GetRenderTargetColorTexture(RenderTargetHandle h) const override;
    TextureHandle      GetRenderTargetDepthTexture(RenderTargetHandle h) const override;

    ShaderHandle CreateShaderFromSource(const std::string& src, ShaderStageMask stage,
                                        const std::string& entry, const std::string& dbg) override;
    ShaderHandle CreateShaderFromBytecode(const void* data, size_t sz,
                                          ShaderStageMask stage,
                                          const std::string& dbg) override;
    void DestroyShader(ShaderHandle h) override;

    PipelineHandle CreatePipeline(const PipelineDesc& desc) override;
    void           DestroyPipeline(PipelineHandle h)        override;

    uint32_t CreateSampler(const SamplerDesc& desc) override;

    std::unique_ptr<ICommandList> CreateCommandList(QueueType queue) override;
    std::unique_ptr<IFence>       CreateFence(uint64_t initialValue)  override;

    void UploadBufferData(BufferHandle h, const void* data,
                          size_t sz, size_t off) override;
    void UploadTextureData(TextureHandle h, const void* data, size_t sz,
                           uint32_t mip, uint32_t slice) override;

    void BeginFrame() override;
    void EndFrame()   override;

    [[nodiscard]] uint32_t    GetDrawCallCount() const override { return m_totalDrawCalls; }
    [[nodiscard]] const char* GetBackendName()   const override { return "OpenGL"; }
    [[nodiscard]] math::Mat4 GetClipSpaceAdjustment() const override;
    [[nodiscard]] math::Mat4 GetShadowClipSpaceAdjustment() const override;
    [[nodiscard]] assets::ShaderTargetProfile GetShaderTargetProfile() const override;
    [[nodiscard]] bool        SupportsFeature(const char* feature) const override;
    [[nodiscard]] bool        SupportsTextureFormat(Format format, ResourceUsage usage = ResourceUsage::ShaderResource) const override;

    static std::vector<AdapterInfo> EnumerateAdaptersImpl();

private:
    OGLDeviceResources m_resources;
    bool     m_initialized    = false;
    uint64_t m_frameIndex     = 0ull;
    uint32_t m_totalDrawCalls = 0u;
    int      m_glMajor        = 4;
    int      m_glMinor        = 1;
    bool     m_glDebugContext = false;
};

} // namespace engine::renderer::opengl
