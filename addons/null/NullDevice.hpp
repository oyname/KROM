#pragma once
// =============================================================================
// KROM Engine - addons/null/NullDevice.hpp
// Null-Backend: Deklaration. Implementierung: addons/null/NullDevice.cpp
// =============================================================================
#include "renderer/IDevice.hpp"
#include <vector>
#include <atomic>

namespace engine::renderer::null_backend {

void RegisterNullBackend();


// ---------------------------------------------------------------------------
class NullCommandList final : public ICommandList
{
public:
    // deviceDrawCalls: Zeiger auf Device-seitigen Zähler - wird in Submit() aktualisiert.
    // Übergibt NullDevice seinen Zähler; CommandList akkumuliert direkt hinein.
    explicit NullCommandList(QueueType queue, uint32_t* deviceDrawCalls = nullptr);

    void Begin()   override;
    void End()     override;
    void BeginRenderPass(const RenderPassBeginInfo& info) override;
    void EndRenderPass() override;
    void SetPipeline(PipelineHandle pipeline) override;
    void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint32_t offset) override;
    void SetIndexBuffer(BufferHandle buffer, bool is32bit, uint32_t offset)   override;
    void SetConstantBuffer(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override;
    void SetShaderResource(uint32_t slot, TextureHandle texture, ShaderStageMask stages) override;
    void SetSampler(uint32_t slot, uint32_t samplerIndex, ShaderStageMask stages) override;
    void SetViewport(float x, float y, float width, float height,
                     float minDepth, float maxDepth) override;
    void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
    void Draw(uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset,
                     uint32_t firstInstance) override;
    void Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
    void TransitionResource(BufferHandle  buffer,  ResourceState before, ResourceState after) override;
    void TransitionResource(TextureHandle texture, ResourceState before, ResourceState after) override;
    void TransitionRenderTarget(RenderTargetHandle rt, ResourceState before, ResourceState after) override;
    void CopyBuffer(BufferHandle dst, uint64_t dstOff, BufferHandle src, uint64_t srcOff, uint64_t size) override;
    void CopyTexture(TextureHandle dst, uint32_t dstMip, TextureHandle src, uint32_t srcMip) override;
    void Submit(QueueType queue) override;
    [[nodiscard]] QueueType GetQueueType() const override { return m_queue; }

    [[nodiscard]] uint32_t GetFrameDrawCalls() const noexcept { return m_frameDrawCalls; }

private:
    QueueType      m_queue;
    PipelineHandle m_currentPipeline;
    uint32_t       m_frameDrawCalls = 0u; // Draw-Calls dieses Begin/End-Blocks
    uint32_t*      m_deviceCounter  = nullptr; // Zeiger auf NullDevice::m_totalDrawCalls
};

// ---------------------------------------------------------------------------
class NullFence final : public IFence
{
public:
    explicit NullFence(uint64_t init);
    void     Signal(uint64_t value)                    override;
    void     Wait(uint64_t value, uint64_t timeoutNs)  override;
    uint64_t GetValue() const                          override;
private:
    std::atomic<uint64_t> m_value;
};

// ---------------------------------------------------------------------------
class NullSwapchain final : public ISwapchain
{
public:
    NullSwapchain(uint32_t width, uint32_t height, uint32_t bufferCount);

    bool               AcquireForFrame()               override;
    void               Present(bool vsync)              override;
    void               Resize(uint32_t w, uint32_t h)   override;
    uint32_t           GetCurrentBackbufferIndex() const override;
    TextureHandle      GetBackbufferTexture(uint32_t i)  const override;
    RenderTargetHandle GetBackbufferRenderTarget(uint32_t i) const override;
    uint32_t           GetWidth()  const override;
    uint32_t           GetHeight() const override;
    bool               CanRenderFrame() const override { return !m_rtHandles.empty() && !m_texHandles.empty() && m_width > 0u && m_height > 0u; }
    bool               NeedsRecreate() const override { return false; }
    SwapchainFrameStatus QueryFrameStatus() const override;
    SwapchainRuntimeDesc GetRuntimeDesc() const override;
    [[nodiscard]] Format GetBackbufferFormat() const override { return Format::RGBA8_UNORM_SRGB; }

private:
    uint32_t m_width, m_height, m_bufferCount;
    uint32_t m_currentBuffer = 0u;
    uint64_t m_frameIndex    = 0ull;
    std::vector<TextureHandle>      m_texHandles;
    std::vector<RenderTargetHandle> m_rtHandles;
};

// ---------------------------------------------------------------------------
class NullDevice final : public IDevice
{
public:
     NullDevice() = default;
    ~NullDevice();

    bool Initialize(const DeviceDesc& desc)                  override;
    void Shutdown()                                          override;
    void WaitIdle()                                          override;

    std::unique_ptr<ISwapchain>   CreateSwapchain(const SwapchainDesc& desc)    override;

    BufferHandle  CreateBuffer(const BufferDesc& desc)       override;
    void          DestroyBuffer(BufferHandle handle)         override;
    void*         MapBuffer(BufferHandle handle)             override;
    void          UnmapBuffer(BufferHandle handle)           override;

    TextureHandle CreateTexture(const TextureDesc& desc)     override;
    void          DestroyTexture(TextureHandle handle)       override;

    RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) override;
    void               DestroyRenderTarget(RenderTargetHandle handle)   override;
    TextureHandle      GetRenderTargetColorTexture(RenderTargetHandle rt) const override;
    TextureHandle      GetRenderTargetDepthTexture(RenderTargetHandle rt) const override;

    ShaderHandle CreateShaderFromSource(const std::string& source,
                                         ShaderStageMask stage,
                                         const std::string& entryPoint,
                                         const std::string& debugName) override;
    ShaderHandle CreateShaderFromBytecode(const void* data, size_t byteSize,
                                           ShaderStageMask stage,
                                           const std::string& debugName) override;
    void DestroyShader(ShaderHandle handle) override;

    PipelineHandle CreatePipeline(const PipelineDesc& desc) override;
    void           DestroyPipeline(PipelineHandle handle)   override;

    uint32_t CreateSampler(const SamplerDesc& desc) override;

    std::unique_ptr<ICommandList> CreateCommandList(QueueType queue) override;
    std::unique_ptr<IFence>       CreateFence(uint64_t initialValue) override;

    void UploadBufferData(BufferHandle handle, const void* data,
                          size_t byteSize, size_t dstOffset) override;
    void UploadTextureData(TextureHandle handle, const void* data,
                           size_t byteSize, uint32_t mipLevel,
                           uint32_t arraySlice) override;

    void BeginFrame() override;
    void EndFrame()   override;

    uint32_t    GetDrawCallCount() const override;
    const char* GetBackendName()   const override;
    [[nodiscard]] assets::ShaderTargetProfile GetShaderTargetProfile() const override
    {
        return assets::ShaderTargetProfile::Null;
    }

private:
    bool                 m_initialized    = false;
    uint64_t             m_frameIndex     = 0ull;
    uint32_t             m_totalDrawCalls = 0u;
    uint32_t             m_nextBufferIdx  = 1u;
    uint32_t             m_nextTexIdx     = 1u;
    uint32_t             m_nextRTIdx      = 1u;
    uint32_t             m_nextShaderIdx  = 1u;
    uint32_t             m_nextPipelineIdx = 1u;
    uint32_t             m_nextSamplerIdx  = 1u;
    std::vector<uint8_t> m_mappedBuffer;
};

} // namespace engine::renderer::null_backend
