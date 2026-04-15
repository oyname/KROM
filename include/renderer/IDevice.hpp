#pragma once
// =============================================================================
// KROM Engine - renderer/IDevice.hpp
// API-neutrale Renderer-Abstraktion.
// Keine DX11/DX12/GL/Vulkan-Headers hier oder in Implementierungsdateien die
// diesen Header includen. Backend-spezifisches lebt in addons/<name>/.
//
// Designprinzip: DX12/Vulkan-artiges explizites Modell als Schnittstelle,
// DX11 und OpenGL werden über kompatible Adapter angebunden.
// =============================================================================
#include "renderer/CommandListRuntime.hpp"
#include "renderer/RendererTypes.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/UploadRuntime.hpp"
#include <memory>
#include <string>

namespace engine::renderer {

// Forward-Deklarationen
class ICommandList;
class ISwapchain;
class IFence;

// =============================================================================
// IDevice - Haupt-Geräte-Interface
// Erstellt Ressourcen, verwaltet Queues.
// =============================================================================
class IDevice
{
public:
    virtual ~IDevice() = default;

    // --- Lifecycle -----------------------------------------------------------
    struct DeviceDesc
    {
        bool        enableDebugLayer  = false;
        bool        enableGpuValidation = false;
        uint32_t    adapterIndex      = 0u;
        std::string appName;
    };

    virtual bool Initialize(const DeviceDesc& desc) = 0;
    virtual void Shutdown() = 0;
    virtual void WaitIdle() = 0;

    // --- Swapchain -----------------------------------------------------------
    struct SwapchainDesc
    {
        void*    nativeWindowHandle = nullptr; // HWND, NSView, xcb_window_t, ...
        uint32_t width              = 1280u;
        uint32_t height             = 720u;
        uint32_t bufferCount        = 2u;
        Format   format             = Format::BGRA8_UNORM_SRGB;
        bool     vsync              = true;
        std::string debugName;

        // OpenGL context version and debug flag.
        // Ignored by non-OpenGL backends.
        int  openglMajor        = 4;
        int  openglMinor        = 1;
        bool openglDebugContext = false;
    };

    virtual std::unique_ptr<ISwapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;

    // --- Buffer-Erstellung ---------------------------------------------------
    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual void         DestroyBuffer(BufferHandle handle) = 0;
    virtual void*        MapBuffer(BufferHandle handle) = 0;
    virtual void         UnmapBuffer(BufferHandle handle) = 0;

    // --- Texture-Erstellung --------------------------------------------------
    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual void          DestroyTexture(TextureHandle handle) = 0;

    // --- RenderTarget --------------------------------------------------------
    virtual RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) = 0;
    virtual void               DestroyRenderTarget(RenderTargetHandle handle) = 0;
    virtual TextureHandle      GetRenderTargetColorTexture(RenderTargetHandle rt) const = 0;
    virtual TextureHandle      GetRenderTargetDepthTexture(RenderTargetHandle rt) const = 0;

    // --- Shader --------------------------------------------------------------
    virtual ShaderHandle CreateShaderFromSource(
        const std::string& source,
        ShaderStageMask stage,
        const std::string& entryPoint = "main",
        const std::string& debugName  = "") = 0;

    virtual ShaderHandle CreateShaderFromBytecode(
        const void* data,
        size_t byteSize,
        ShaderStageMask stage,
        const std::string& debugName = "") = 0;

    virtual void DestroyShader(ShaderHandle handle) = 0;

    // --- Pipeline ------------------------------------------------------------
    virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;
    virtual void           DestroyPipeline(PipelineHandle handle) = 0;

    // --- Sampler -------------------------------------------------------------
    // Sampler sind in diesem Modell Device-Level (DX11-Stil) oder
    // Teil der generischen Descriptor-Bindungssignatur.
    // Gemeinsamer Nenner: Device alloziert, CommandList bindet.
    virtual uint32_t CreateSampler(const SamplerDesc& desc) = 0;

    // --- CommandList ---------------------------------------------------------
    virtual std::unique_ptr<ICommandList> CreateCommandList(QueueType queue = QueueType::Graphics) = 0;

    // --- Fence ---------------------------------------------------------------
    virtual std::unique_ptr<IFence> CreateFence(uint64_t initialValue = 0u) = 0;

    // --- Daten-Upload (vereinfachter Staging-Pfad) ---------------------------
    virtual void UploadBufferData(BufferHandle handle, const void* data, size_t byteSize, size_t dstOffset = 0u) = 0;
    virtual void UploadTextureData(TextureHandle handle, const void* data, size_t byteSize,
                                   uint32_t mipLevel = 0u, uint32_t arraySlice = 0u) = 0;

    // --- Frame-Boundaries ----------------------------------------------------
    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;

    // --- Resource-State-Introspection ---------------------------------------
    // Backend-Ressourcen sind die autoritative Quelle des aktuellen physischen
    // Resource-States. RenderGraph darf diese States lesen und darauf planen,
    // schreibt sie aber nicht selbst als zweite Wahrheitsquelle fort.
    [[nodiscard]] virtual ResourceStateRecord QueryBufferState(BufferHandle handle) const
    {
        (void)handle;
        return {};
    }
    [[nodiscard]] virtual ResourceStateRecord QueryTextureState(TextureHandle handle) const
    {
        (void)handle;
        return {};
    }
    [[nodiscard]] virtual ResourceStateRecord QueryRenderTargetState(RenderTargetHandle handle) const
    {
        (void)handle;
        return {};
    }
    [[nodiscard]] virtual ResourceAllocationInfo QueryBufferAllocation(BufferHandle handle) const
    {
        (void)handle;
        return {};
    }
    [[nodiscard]] virtual ResourceAllocationInfo QueryTextureAllocation(TextureHandle handle) const
    {
        (void)handle;
        return {};
    }

    // --- Diagnostics ---------------------------------------------------------
    [[nodiscard]] virtual uint32_t GetDrawCallCount() const = 0;
    [[nodiscard]] virtual const char* GetBackendName() const = 0;
    [[nodiscard]] virtual bool SupportsFeature(const char* feature) const { (void)feature; return false; }
    [[nodiscard]] virtual QueueCapabilities GetQueueCapabilities(QueueType queue) const
    {
        return QueueCapabilities{queue, queue == QueueType::Graphics, false, queue == QueueType::Graphics};
    }
    [[nodiscard]] virtual QueueType GetPreferredUploadQueue() const
    {
        return GetUploadRuntime().recordingQueue;
    }

    [[nodiscard]] virtual UploadRuntimeDesc GetUploadRuntime() const
    {
        UploadRuntimeDesc desc = BuildDefaultUploadRuntimeDesc();
        const QueueCapabilities transfer = GetQueueCapabilities(QueueType::Transfer);
        desc.recordingQueue = transfer.supported ? QueueType::Transfer : QueueType::Graphics;
        desc.submissionModel = transfer.supported
            ? UploadSubmissionModel::MultiQueueDeferred
            : UploadSubmissionModel::SingleGraphicsQueueV1;
        return desc;
    }

    [[nodiscard]] virtual CommandListRuntimeDesc GetCommandListRuntime() const
    {
        CommandListRuntimeDesc desc = BuildDefaultCommandListRuntimeDesc();
        desc.queues[0].supported = GetQueueCapabilities(QueueType::Graphics).supported;
        desc.queues[0].dedicated = GetQueueCapabilities(QueueType::Graphics).dedicated;
        desc.queues[0].canPresent = GetQueueCapabilities(QueueType::Graphics).canPresent;
        desc.queues[1].supported = GetQueueCapabilities(QueueType::Compute).supported;
        desc.queues[1].dedicated = GetQueueCapabilities(QueueType::Compute).dedicated;
        desc.queues[2].supported = GetQueueCapabilities(QueueType::Transfer).supported;
        desc.queues[2].dedicated = GetQueueCapabilities(QueueType::Transfer).dedicated;
        desc.compute.runtime = GetComputeRuntime();
        const QueueCapabilities graphics = GetQueueCapabilities(QueueType::Graphics);
        const QueueCapabilities compute = GetQueueCapabilities(QueueType::Compute);
        const QueueCapabilities transfer = GetQueueCapabilities(QueueType::Transfer);
        desc.queueSync.graphicsFirstV1 = true;
        desc.queueSync.queueLocalFenceSignalSupported = graphics.supported;
        desc.queueSync.queueLocalFenceWaitSupported = graphics.supported;
        desc.queueSync.interQueueDependenciesPrepared = true;
        desc.queueSync.interQueueSemaphoreSignalSupported = false;
        desc.queueSync.interQueueSemaphoreWaitSupported = false;
        desc.queueSync.ownershipTransfersPrepared = compute.supported || transfer.supported;
        desc.queueSync.ownershipTransfersMaterialized = compute.supported || transfer.supported;
        desc.queueSync.graphToSubmissionMappingPrepared = true;
        desc.queueSync.graphToSubmissionMappingMaterialized = compute.supported || transfer.supported;
        desc.multiQueue.preparedForCopyToGraphics = transfer.supported || graphics.supported;
        desc.multiQueue.preparedForGraphicsToPresent = graphics.canPresent;
        desc.multiQueue.preparedForAsyncCompute = compute.supported;
        desc.multiQueue.queueOwnershipTransfersPrepared = compute.supported || transfer.supported;
        desc.multiQueue.queueOwnershipTransfersMaterialized = compute.supported || transfer.supported;
        desc.multiQueue.interQueueDependenciesMaterialized = compute.supported || transfer.supported;
        return desc;
    }

    [[nodiscard]] virtual DescriptorRuntimeLayoutDesc GetDescriptorRuntimeLayout() const
    {
        return BuildEngineDescriptorRuntimeLayout();
    }

    [[nodiscard]] virtual ComputeRuntimeDesc GetComputeRuntime() const
    {
        ComputeRuntimeDesc desc{};
        const QueueCapabilities graphics = GetQueueCapabilities(QueueType::Graphics);
        const QueueCapabilities compute = GetQueueCapabilities(QueueType::Compute);
        desc.recordingQueue = compute.supported ? QueueType::Compute : QueueType::Graphics;
        desc.queueRouting = compute.supported
            ? ComputeQueueRoutingPolicy::PreferDedicatedCompute
            : ComputeQueueRoutingPolicy::GraphicsQueueOnly;
        desc.synchronization = compute.supported
            ? ComputeSynchronizationModel::CrossQueueFenceAndBarrier
            : ComputeSynchronizationModel::GraphicsQueueSerial;
        desc.uavBarrierPolicy = ComputeUavBarrierPolicy::ExplicitPerDispatch;
        desc.maturity = graphics.supported ? ComputePathMaturity::Enabled : ComputePathMaturity::Disabled;
        desc.computePipelinesSupported = graphics.supported;
        desc.computeDispatchSupported = graphics.supported;
        desc.dedicatedComputeQueueEnabled = compute.supported && compute.dedicated;
        desc.crossQueueSynchronizationEnabled = compute.supported;
        return desc;
    }

    [[nodiscard]] virtual SwapchainRuntimeDesc GetSwapchainRuntime() const
    {
        return {};
    }
};

// =============================================================================
// ICommandList
// =============================================================================
class ICommandList
{
public:
    virtual ~ICommandList() = default;

    virtual void Begin() = 0;
    virtual void End()   = 0;

    // --- Render Pass (vereinfacht, für DX11-Kompatibilität) ------------------
    struct RenderPassBeginInfo
    {
        RenderTargetHandle renderTarget;  // Invalid = Backbuffer
        ClearValue         colorClear;
        ClearValue         depthClear;
        bool               clearColor  = true;
        bool               clearDepth  = true;
        bool               clearStencil = false;
    };

    virtual void BeginRenderPass(const RenderPassBeginInfo& info) = 0;
    virtual void EndRenderPass() = 0;

    // --- State Setting -------------------------------------------------------
    virtual void SetPipeline(PipelineHandle pipeline) = 0;
    virtual void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint32_t offset = 0u) = 0;
    virtual void SetIndexBuffer(BufferHandle buffer, bool is32bit = true, uint32_t offset = 0u) = 0;
    virtual void SetConstantBuffer(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) = 0;

    // Range-Binding für Constant/Uniform-Buffer (Suballokation aus einem Arena-Buffer).
    // Default-Implementierung fällt auf SetConstantBuffer zurück (ignoriert Offset/Size).
    // Backends mit nativer Range-Unterstützung (DX11.1, OpenGL glBindBufferRange) überschreiben dies.
    virtual void SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask stages)
    {
        if (binding.IsValid())
            SetConstantBuffer(slot, binding.buffer, stages);
    }
    virtual void SetShaderResource(uint32_t slot, TextureHandle texture, ShaderStageMask stages) = 0;
    virtual void SetSampler(uint32_t slot, uint32_t samplerIndex, ShaderStageMask stages) = 0;
    virtual void SetViewport(float x, float y, float width, float height,
                             float minDepth = 0.f, float maxDepth = 1.f) = 0;
    virtual void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;

    // --- Draw Calls ----------------------------------------------------------
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1u,
                      uint32_t firstVertex = 0u, uint32_t firstInstance = 0u) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1u,
                             uint32_t firstIndex = 0u, int32_t vertexOffset = 0,
                             uint32_t firstInstance = 0u) = 0;

    // --- Compute -------------------------------------------------------------
    virtual void Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

    // --- Resource Barriers (vereinfacht) -------------------------------------
    virtual void TransitionResource(BufferHandle  buffer,  ResourceState before, ResourceState after) = 0;
    virtual void TransitionResource(TextureHandle texture, ResourceState before, ResourceState after) = 0;
    virtual void TransitionRenderTarget(RenderTargetHandle rt, ResourceState before, ResourceState after) = 0;

    // --- Kopieren ------------------------------------------------------------
    virtual void CopyBuffer(BufferHandle  dst, uint64_t dstOffset, BufferHandle  src, uint64_t srcOffset, uint64_t size) = 0;
    virtual void CopyTexture(TextureHandle dst, uint32_t dstMip, TextureHandle src, uint32_t srcMip) = 0;

    // --- Queue Ownership -----------------------------------------------------
    virtual void ReleaseQueueOwnership(BufferHandle buffer, QueueType dstQueue, ResourceState state)
    {
        (void)buffer; (void)dstQueue; (void)state;
    }
    virtual void AcquireQueueOwnership(BufferHandle buffer, QueueType srcQueue, ResourceState state)
    {
        (void)buffer; (void)srcQueue; (void)state;
    }
    virtual void ReleaseQueueOwnership(TextureHandle texture, QueueType dstQueue, ResourceState state)
    {
        (void)texture; (void)dstQueue; (void)state;
    }
    virtual void AcquireQueueOwnership(TextureHandle texture, QueueType srcQueue, ResourceState state)
    {
        (void)texture; (void)srcQueue; (void)state;
    }
    virtual void ReleaseQueueOwnership(RenderTargetHandle rt, QueueType dstQueue, ResourceState state)
    {
        (void)rt; (void)dstQueue; (void)state;
    }
    virtual void AcquireQueueOwnership(RenderTargetHandle rt, QueueType srcQueue, ResourceState state)
    {
        (void)rt; (void)srcQueue; (void)state;
    }

    // --- Submit --------------------------------------------------------------
    virtual void Submit(const CommandSubmissionDesc& submission)
    {
        Submit(submission.queue);
    }
    virtual void Submit(QueueType queue = QueueType::Graphics) = 0;
    [[nodiscard]] virtual QueueType GetQueueType() const { return QueueType::Graphics; }
    [[nodiscard]] virtual uint64_t GetLastSubmittedFenceValue() const { return 0u; }
};

// =============================================================================
// ISwapchain
// =============================================================================
class ISwapchain
{
public:
    virtual ~ISwapchain() = default;

    virtual bool AcquireForFrame() = 0;
    virtual void Present(bool vsync) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    [[nodiscard]] virtual uint32_t       GetCurrentBackbufferIndex() const = 0;
    [[nodiscard]] virtual TextureHandle  GetBackbufferTexture(uint32_t index) const = 0;
    [[nodiscard]] virtual RenderTargetHandle GetBackbufferRenderTarget(uint32_t index) const = 0;
    [[nodiscard]] virtual uint32_t GetWidth()  const = 0;
    [[nodiscard]] virtual uint32_t GetHeight() const = 0;
    [[nodiscard]] virtual bool CanRenderFrame() const = 0;
    [[nodiscard]] virtual bool NeedsRecreate() const = 0;
    [[nodiscard]] virtual SwapchainFrameStatus QueryFrameStatus() const = 0;
    [[nodiscard]] virtual SwapchainRuntimeDesc GetRuntimeDesc() const = 0;
};

// =============================================================================
// IFence
// =============================================================================
class IFence
{
public:
    virtual ~IFence() = default;
    virtual void     Signal(uint64_t value) = 0;
    virtual void     Wait(uint64_t value, uint64_t timeoutNs = UINT64_MAX) = 0;
    [[nodiscard]] virtual uint64_t GetValue() const = 0;
};

// =============================================================================
// AdapterInfo - plattformneutrale Beschreibung eines GPU-Adapters.
// Wird von DeviceFactory::EnumerateAdapters() zurückgegeben.
// Kein API-spezifischer Typ - alle Backends liefern dieselbe Struktur.
// =============================================================================
struct AdapterInfo
{
    uint32_t    index         = 0u;   // Übergeben an DeviceDesc::adapterIndex
    std::string name;                 // UTF-8, z.B. "NVIDIA GeForce RTX 4090"
    size_t      dedicatedVRAM = 0u;   // Bytes; 0 = unbekannt (z.B. iGPU oder GL ohne Kontext)
    bool        isDiscrete    = true; // false = integrierte GPU
    int         featureLevel  = 0;    // DX11: 100/101/110/111/120/121
                                      // OpenGL: major*10+minor (z.B. GL 4.6 → 46), 0 = unbekannt
};

// =============================================================================
// DeviceFactory - erstellt das gewählte Backend
// =============================================================================
class DeviceFactory
{
public:
    enum class BackendType { Null, DirectX11, DirectX12, OpenGL, Vulkan };
    using FactoryFn   = std::unique_ptr<IDevice>(*)();
    using EnumerateFn = std::vector<AdapterInfo>(*)();   // Kein Device nötig

    struct BackendEntry
    {
        FactoryFn   factory   = nullptr;
        EnumerateFn enumerate = nullptr;
        bool        isStub    = false;
    };

    class Registrar
    {
    public:
        // enumFn optional - Backends ohne Enumeration registrieren nullptr.
        Registrar(BackendType backend, FactoryFn fn, EnumerateFn enumFn = nullptr, bool isStub = false)
        {
            DeviceFactory::Register(backend, fn, enumFn, isStub);
        }
    };

    // enumFn = nullptr erlaubt (Null-Backend hat keine Enumeration).
    static void Register(BackendType backend, FactoryFn fn, EnumerateFn enumFn = nullptr, bool isStub = false);
    static void Unregister(BackendType backend);
    [[nodiscard]] static bool IsRegistered(BackendType backend);
    [[nodiscard]] static bool IsAvailable(BackendType backend);
    [[nodiscard]] static std::unique_ptr<IDevice> Create(BackendType backend);

    // Listet verfügbare Hardware-Adapter ohne Device-Erstellung.
    // Gibt leeren Vektor zurück wenn das Backend nicht registriert ist
    // oder keine Enumeration unterstützt.
    [[nodiscard]] static std::vector<AdapterInfo> EnumerateAdapters(BackendType backend);

    // Gibt den Index des Adapters mit dem höchsten featureLevel zurück.
    // Bei Gleichstand gewinnt isDiscrete. Gibt 0 zurück wenn adapters leer ist.
    [[nodiscard]] static uint32_t FindBestAdapter(const std::vector<AdapterInfo>& adapters);
};

} // namespace engine::renderer
