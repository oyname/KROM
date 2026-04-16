#pragma once
// =============================================================================
// KROM Engine -- addons/dx11/DX11Device.hpp (privat)
// =============================================================================
#include "renderer/IDevice.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include <memory>
#include <vector>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11CommandList;
struct IDXGIFactory;
struct IDXGISwapChain;
struct ID3D11Debug;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11UnorderedAccessView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11ComputeShader;
struct ID3D11InputLayout;
struct ID3D11BlendState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;
struct ID3D11SamplerState;
struct ID3D11Query;

namespace engine::renderer::dx11 {


#ifdef _WIN32
template<typename T>
inline void SafeRelease(T*& p) noexcept
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}
#endif


// =============================================================================
// DX11Store<Tag, T>  -- generationssicherer Slot-Allocator
// =============================================================================
template<typename Tag, typename T>
class DX11Store
{
    struct Slot { T data{}; uint32_t generation = 0u; bool alive = false; };
    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_freeList;
public:
    DX11Store() { m_slots.emplace_back(); } // Slot 0 = Invalid

    Handle<Tag> Add(T data)
    {
        uint32_t idx;
        if (!m_freeList.empty()) { idx = m_freeList.back(); m_freeList.pop_back(); }
        else                     { idx = static_cast<uint32_t>(m_slots.size()); m_slots.emplace_back(); }
        auto& s = m_slots[idx];
        s.data  = std::move(data);
        s.alive = true;
        ++s.generation;
        return Handle<Tag>::Make(idx, s.generation & Handle<Tag>::GEN_MASK);
    }

    T* Get(Handle<Tag> h) noexcept
    {
        const uint32_t idx = h.Index();
        if (idx == 0u || idx >= m_slots.size()) return nullptr;
        auto& s = m_slots[idx];
        return (s.alive && s.generation == h.Generation()) ? &s.data : nullptr;
    }
    const T* Get(Handle<Tag> h) const noexcept { return const_cast<DX11Store*>(this)->Get(h); }

    bool Remove(Handle<Tag> h) noexcept
    {
        const uint32_t idx = h.Index();
        if (idx == 0u || idx >= m_slots.size()) return false;
        auto& s = m_slots[idx];
        if (!s.alive || s.generation != h.Generation()) return false;
        s.alive = false; s.data = T{}; m_freeList.push_back(idx); return true;
    }

    template<typename Fn> void ForEach(Fn&& fn) { for (auto& s : m_slots) if (s.alive) fn(s.data); }
};

// =============================================================================
// Ressourcen-Structs
// =============================================================================
struct DX11BufferEntry {
    ID3D11Buffer* buffer    = nullptr;
    uint32_t      byteSize  = 0u;
    uint32_t      stride    = 0u;   // Vertex-Stride für IASetVertexBuffers
    uint32_t      bindFlags = 0u;
    bool          dynamic   = false;
    void*         mapped    = nullptr;
};

struct DX11TextureEntry {
    ID3D11Texture2D*           tex  = nullptr;
    ID3D11ShaderResourceView*  srv  = nullptr;
    ID3D11UnorderedAccessView* uav  = nullptr;
    uint32_t                   format = 0u;
    uint32_t                   width = 0u;
    uint32_t                   height = 0u;
    uint32_t                   mipLevels = 1u;
    uint32_t                   arraySize = 1u;
    TextureDimension           dimension = TextureDimension::Tex2D;
};

struct DX11RenderTargetEntry {
    ID3D11RenderTargetView*   rtv      = nullptr;
    ID3D11DepthStencilView*   dsv      = nullptr;
    ID3D11Texture2D*          colorTex = nullptr;
    ID3D11Texture2D*          depthTex = nullptr;
    ID3D11ShaderResourceView* colorSRV = nullptr;
    ID3D11ShaderResourceView* depthSRV = nullptr;
    TextureHandle             colorHandle;
    TextureHandle             depthHandle;
};

struct DX11ShaderEntry {
    ID3D11VertexShader*  vs = nullptr;
    ID3D11PixelShader*   ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    std::vector<uint8_t> bytecode;   // VS-Bytecode fuer InputLayout
    ShaderStageMask      stage = ShaderStageMask::None;
};

struct DX11PipelineState {
    PipelineKey              key{};
    VertexLayout             vertexLayout;
    ID3D11VertexShader*      vs  = nullptr;
    ID3D11PixelShader*       ps  = nullptr;
    ID3D11InputLayout*       il  = nullptr;
    ID3D11BlendState*        bs  = nullptr;
    ID3D11RasterizerState*   rs  = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    uint32_t                 topology = 4u; // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    [[nodiscard]] bool isValid() const noexcept { return vs != nullptr; }
};

struct DX11SamplerEntry { ID3D11SamplerState* sampler = nullptr; };

// =============================================================================
// DX11DeviceResources -- Device besitzt, CommandList bekommt Zeiger
// =============================================================================
struct DX11DeviceResources {
    DX11Store<BufferTag,       DX11BufferEntry>       buffers;
    DX11Store<TextureTag,      DX11TextureEntry>      textures;
    DX11Store<RenderTargetTag, DX11RenderTargetEntry> renderTargets;
    DX11Store<ShaderTag,       DX11ShaderEntry>       shaders;
    DX11Store<PipelineTag,     DX11PipelineState>     pipelines;
    std::vector<DX11SamplerEntry>                     samplers;
};

// =============================================================================
// DX11Swapchain
// =============================================================================
class DX11Swapchain final : public ISwapchain
{
public:
    DX11Swapchain(ID3D11Device* device, ID3D11DeviceContext* ctx,
                  IDXGISwapChain* sc, DX11DeviceResources& res,
                  uint32_t w, uint32_t h, uint32_t bufferCount);
    ~DX11Swapchain() override;

    bool               AcquireForFrame()               override;
    void               Present(bool vsync)              override;
    void               Resize(uint32_t w, uint32_t h)  override;
    uint32_t           GetCurrentBackbufferIndex() const override { return m_currentIdx; }
    TextureHandle      GetBackbufferTexture(uint32_t i) const override;
    RenderTargetHandle GetBackbufferRenderTarget(uint32_t i) const override;
    uint32_t           GetWidth()  const override { return m_width;  }
    uint32_t           GetHeight() const override { return m_height; }
    bool               CanRenderFrame() const override { return !m_bbRTs.empty() && !m_bbTex.empty() && m_width > 0u && m_height > 0u; }
    bool               NeedsRecreate() const override { return false; }
    SwapchainFrameStatus QueryFrameStatus() const override;
    SwapchainRuntimeDesc GetRuntimeDesc() const override;

private:
    void AcquireBackbufferViews();
    void ReleaseBackbufferViews();

    ID3D11Device*        m_device;
    ID3D11DeviceContext* m_ctx;
    IDXGISwapChain*      m_sc;
    DX11DeviceResources* m_res;
    std::vector<RenderTargetHandle> m_bbRTs;
    std::vector<TextureHandle>      m_bbTex;
    uint32_t m_width = 0u, m_height = 0u, m_bufferCount = 0u, m_currentIdx = 0u;
};

// =============================================================================
// DX11Fence -- ID3D11Query(D3D11_QUERY_EVENT)
// =============================================================================
class DX11Fence final : public IFence
{
public:
    DX11Fence(ID3D11Device* device, ID3D11DeviceContext* ctx, uint64_t init);
    ~DX11Fence() override;
    void     Signal(uint64_t value)               override;
    void     Wait(uint64_t value, uint64_t tNs)   override;
    uint64_t GetValue() const                     override;
private:
    struct PendingQuery
    {
        ID3D11Query* query = nullptr;
        uint64_t     value = 0ull;
    };

    void PollCompleted() const;

    ID3D11Device*              m_device         = nullptr;
    ID3D11DeviceContext*       m_ctx            = nullptr;
    mutable std::vector<PendingQuery> m_pending;
    mutable uint64_t           m_completedValue = 0ull;
};

// =============================================================================
// DX11CommandList
// =============================================================================
class DX11CommandList final : public ICommandList
{
public:
    explicit DX11CommandList(ID3D11DeviceContext* ctx, DX11DeviceResources* res,
                             bool deferred = false, uint32_t* devCounter = nullptr);
    ~DX11CommandList() override;

    void Begin() override;
    void End()   override;
    void BeginRenderPass(const RenderPassBeginInfo& info) override;
    void EndRenderPass() override;
    void SetPipeline(PipelineHandle pipeline) override;
    void SetVertexBuffer(uint32_t slot, BufferHandle buf, uint32_t offset) override;
    void SetIndexBuffer(BufferHandle buf, bool is32bit, uint32_t offset)   override;
    void SetConstantBuffer(uint32_t slot, BufferHandle buf, ShaderStageMask stages) override;
    void SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask stages) override;
    void SetShaderResource(uint32_t slot, TextureHandle tex, ShaderStageMask stages) override;
    void SetSampler(uint32_t slot, uint32_t samplerIdx, ShaderStageMask stages) override;
    void SetViewport(float x, float y, float w, float h, float mn, float mx) override;
    void SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) override;
    void Draw(uint32_t verts, uint32_t inst, uint32_t firstVert, uint32_t firstInst) override;
    void DrawIndexed(uint32_t idx, uint32_t inst, uint32_t firstIdx, int32_t voff, uint32_t firstInst) override;
    void Dispatch(uint32_t gx, uint32_t gy, uint32_t gz) override;
    void TransitionResource(BufferHandle  buf, ResourceState b, ResourceState a) override;
    void TransitionResource(TextureHandle tex, ResourceState b, ResourceState a) override;
    void TransitionRenderTarget(RenderTargetHandle rt, ResourceState b, ResourceState a) override;
    void CopyBuffer(BufferHandle dst, uint64_t dstOff, BufferHandle src, uint64_t srcOff, uint64_t size) override;
    void CopyTexture(TextureHandle dst, uint32_t dstMip, TextureHandle src, uint32_t srcMip) override;
    void Submit(QueueType queue) override;
    [[nodiscard]] QueueType GetQueueType() const override { return QueueType::Graphics; }

    [[nodiscard]] uint32_t GetFrameDrawCalls() const noexcept { return m_draws; }

private:
    ID3D11DeviceContext* m_ctx        = nullptr;
    DX11DeviceResources* m_res        = nullptr;
    ID3D11CommandList*   m_cmdList    = nullptr;
    bool                 m_deferred   = false;
    uint32_t             m_draws      = 0u;
    uint32_t*            m_devCounter = nullptr;
    RenderTargetHandle   m_activeRT;
    PipelineHandle       m_activePipeline;  // für Stride-Fallback in SetVertexBuffer
};

// =============================================================================
// DX11Device
// =============================================================================
class DX11Device final : public IDevice
{
public:
     DX11Device();
    ~DX11Device() override;

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
                                        const std::string& entry,
                                        const std::string& dbg) override;
    ShaderHandle CreateShaderFromBytecode(const void* data, size_t sz,
                                          ShaderStageMask stage,
                                          const std::string& dbg) override;
    void DestroyShader(ShaderHandle h) override;

    PipelineHandle CreatePipeline(const PipelineDesc& desc) override;
    void           DestroyPipeline(PipelineHandle h)        override;

    uint32_t CreateSampler(const SamplerDesc& desc) override;

    std::unique_ptr<ICommandList> CreateCommandList(QueueType queue) override;
    std::unique_ptr<IFence>       CreateFence(uint64_t initialValue)  override;

    void UploadBufferData(BufferHandle h, const void* data, size_t sz, size_t off) override;
    void UploadTextureData(TextureHandle h, const void* data, size_t sz,
                           uint32_t mip, uint32_t slice) override;

    void BeginFrame() override;
    void EndFrame()   override;

    [[nodiscard]] uint32_t    GetDrawCallCount() const override;
    [[nodiscard]] const char* GetBackendName()   const override { return "DirectX11"; }
    [[nodiscard]] assets::ShaderTargetProfile GetShaderTargetProfile() const override
    {
        return assets::ShaderTargetProfile::DirectX11_SM5;
    }
    [[nodiscard]] bool        SupportsFeature(const char* feature) const override;

    [[nodiscard]] bool SupportsComputeShaders()   const noexcept { return m_featureLevel >= 0xB000u; }
    [[nodiscard]] bool SupportsDeferredContexts() const noexcept { return m_hasDeferredContext; }

    static std::vector<engine::renderer::AdapterInfo> EnumerateAdaptersImpl();

private:
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIFactory*        m_factory = nullptr;
    ID3D11Debug*         m_debug   = nullptr;

    uint32_t m_featureLevel       = 0u;
    bool     m_hasDeferredContext = false;
    bool     m_initialized        = false;
    uint64_t m_frameIndex         = 0ull;
    uint32_t m_totalDrawCalls     = 0u;

    DX11DeviceResources m_resources;

    static uint32_t ToDXGIFormat(Format fmt) noexcept;
    static uint32_t ToD3D11Usage(MemoryAccess access) noexcept;
    static uint32_t ToD3D11BindFlags(ResourceUsage usage) noexcept;
    static uint32_t ToD3D11Filter(const SamplerDesc& desc) noexcept;
    static uint32_t ToD3D11AddressMode(WrapMode mode) noexcept;
    static uint32_t ToD3D11ComparisonFunc(CompareFunc func) noexcept;
    static uint32_t ToD3D11ComparisonFunc(DepthFunc func) noexcept;
    static uint32_t ToD3D11Topology(PrimitiveTopology topo) noexcept;

    ID3D11BlendState*        BuildBlendState(const BlendState& bs);
    ID3D11RasterizerState*   BuildRasterizerState(const RasterizerState& rs);
    ID3D11DepthStencilState* BuildDepthStencilState(const DepthStencilState& ds);
    ID3D11InputLayout*       BuildInputLayout(const VertexLayout& vl,
                                              const DX11ShaderEntry& vs);
};

} // namespace engine::renderer::dx11
