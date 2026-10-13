// =============================================================================
// KROM Engine - addons/null/NullDevice.cpp
// Null-Backend: vollständige Implementierung ohne GPU-API.
// =============================================================================
#include "NullDevice.hpp"
#include "core/Debug.hpp"

using engine::Debug;

namespace engine::renderer::null_backend {

// =============================================================================
// NullCommandList
// =============================================================================

NullCommandList::NullCommandList(QueueType queue, uint32_t* deviceDrawCalls)
    : m_queue(queue), m_deviceCounter(deviceDrawCalls) {}

void NullCommandList::Begin()
{
    m_frameDrawCalls = 0u;
    Debug::LogVerbose("null_cmd.cpp: Begin (queue=%d)", static_cast<int>(m_queue));
}

void NullCommandList::End()
{
    Debug::LogVerbose("null_cmd.cpp: End - %u draw calls this frame", m_frameDrawCalls);
}

void NullCommandList::BeginRenderPass(const RenderPassBeginInfo& info)
{
    Debug::LogVerbose("null_cmd.cpp: BeginRenderPass rt=%u", info.renderTarget.value);
}

void NullCommandList::EndRenderPass()
{
    Debug::LogVerbose("null_cmd.cpp: EndRenderPass");
}

void NullCommandList::SetPipeline(PipelineHandle pipeline)
{
    m_currentPipeline = pipeline;
}

void NullCommandList::SetVertexBuffer(uint32_t, BufferHandle, uint32_t) {}
void NullCommandList::SetIndexBuffer(BufferHandle, bool, uint32_t) {}
void NullCommandList::SetConstantBuffer(uint32_t, BufferHandle, ShaderStageMask) {}
void NullCommandList::SetShaderResource(uint32_t, TextureHandle, ShaderStageMask) {}
void NullCommandList::SetSampler(uint32_t, uint32_t, ShaderStageMask) {}
void NullCommandList::SetViewport(float, float, float w, float h, float, float)
{
    Debug::LogVerbose("null_cmd.cpp: SetViewport %.0fx%.0f", w, h);
}
void NullCommandList::SetScissor(int32_t, int32_t, uint32_t, uint32_t) {}

void NullCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount,
                            uint32_t, uint32_t)
{
    Debug::LogVerbose("null_cmd.cpp: Draw verts=%u instances=%u", vertexCount, instanceCount);
    ++m_frameDrawCalls;
}

void NullCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                   uint32_t, int32_t, uint32_t)
{
    Debug::LogVerbose("null_cmd.cpp: DrawIndexed idx=%u instances=%u", indexCount, instanceCount);
    ++m_frameDrawCalls;
}

void NullCommandList::Dispatch(uint32_t gx, uint32_t gy, uint32_t gz)
{
    Debug::LogVerbose("null_cmd.cpp: Dispatch %ux%ux%u", gx, gy, gz);
}

void NullCommandList::TransitionResource(BufferHandle buf, ResourceState before, ResourceState after)
{
    Debug::LogVerbose("null_cmd.cpp: Transition Buffer %u: %u→%u",
        buf.value,
        static_cast<uint32_t>(before),
        static_cast<uint32_t>(after));
}

void NullCommandList::TransitionResource(TextureHandle tex, ResourceState before, ResourceState after)
{
    Debug::LogVerbose("null_cmd.cpp: Transition Texture %u: %u→%u",
        tex.value,
        static_cast<uint32_t>(before),
        static_cast<uint32_t>(after));
}

void NullCommandList::TransitionRenderTarget(RenderTargetHandle rt,
                                              ResourceState before,
                                              ResourceState after)
{
    Debug::LogVerbose("null_cmd.cpp: Transition RT %u: %u→%u",
        rt.value,
        static_cast<uint32_t>(before),
        static_cast<uint32_t>(after));
}

void NullCommandList::CopyBuffer(BufferHandle, uint64_t, BufferHandle, uint64_t, uint64_t) {}
void NullCommandList::CopyTexture(TextureHandle, uint32_t, TextureHandle, uint32_t) {}

void NullCommandList::Submit(QueueType)
{
    Debug::Log("null_cmd.cpp: Submit - %u draw calls", m_frameDrawCalls);
    // Akkumuliere in Device-Zähler (shared pointer, gesetzt von CreateCommandList)
    if (m_deviceCounter) *m_deviceCounter += m_frameDrawCalls;
    m_frameDrawCalls = 0u;
}

// =============================================================================
// NullFence
// =============================================================================

NullFence::NullFence(uint64_t init) : m_value(init) {}
void    NullFence::Signal(uint64_t v) { m_value.store(v); }
void    NullFence::Wait(uint64_t v, uint64_t) { while (m_value.load() < v) {} }
uint64_t NullFence::GetValue() const { return m_value.load(); }

// =============================================================================
// NullSwapchain
// =============================================================================

NullSwapchain::NullSwapchain(uint32_t width, uint32_t height, uint32_t bufferCount)
    : m_width(width), m_height(height), m_bufferCount(bufferCount)
{
    for (uint32_t i = 0; i < bufferCount; ++i)
    {
        m_texHandles.push_back(TextureHandle::Make(i + 1u, 1u));
        m_rtHandles.push_back(RenderTargetHandle::Make(i + 1u, 1u));
    }
    Debug::Log("null_swapchain.cpp: Created %ux%u, %u buffers", width, height, bufferCount);
}

bool NullSwapchain::AcquireForFrame()
{
    return CanRenderFrame();
}

SwapchainFrameStatus NullSwapchain::QueryFrameStatus() const
{
    SwapchainFrameStatus status{};
    status.phase = CanRenderFrame() ? SwapchainFramePhase::Acquired : SwapchainFramePhase::Uninitialized;
    status.currentBackbufferIndex = m_currentBuffer;
    status.bufferCount = static_cast<uint32_t>(m_texHandles.size());
    status.hasRenderableBackbuffer = CanRenderFrame();
    return status;
}

SwapchainRuntimeDesc NullSwapchain::GetRuntimeDesc() const
{
    SwapchainRuntimeDesc desc{};
    desc.presentQueue = QueueType::Graphics;
    desc.explicitAcquire = false;
    desc.explicitPresentTransition = false;
    desc.tracksPerBufferOwnership = true;
    desc.resizeRequiresRecreate = false;
    desc.destructionRequiresFenceRetirement = false;
    return desc;
}

void NullSwapchain::Present(bool vsync)
{
    Debug::LogVerbose("null_swapchain.cpp: Present vsync=%d frame=%llu",
        vsync, static_cast<unsigned long long>(m_frameIndex++));
    m_currentBuffer = (m_currentBuffer + 1u) % m_bufferCount;
}

void NullSwapchain::Resize(uint32_t w, uint32_t h)
{
    m_width = w; m_height = h;
    Debug::Log("null_swapchain.cpp: Resize %ux%u", w, h);
}

uint32_t          NullSwapchain::GetCurrentBackbufferIndex()           const { return m_currentBuffer; }
uint32_t          NullSwapchain::GetWidth()                            const { return m_width; }
uint32_t          NullSwapchain::GetHeight()                           const { return m_height; }

TextureHandle NullSwapchain::GetBackbufferTexture(uint32_t i) const
{
    return i < m_texHandles.size() ? m_texHandles[i] : TextureHandle::Invalid();
}

RenderTargetHandle NullSwapchain::GetBackbufferRenderTarget(uint32_t i) const
{
    return i < m_rtHandles.size() ? m_rtHandles[i] : RenderTargetHandle::Invalid();
}

// =============================================================================
// NullDevice
// =============================================================================

NullDevice::~NullDevice() { Shutdown(); }

bool NullDevice::Initialize(const DeviceDesc& desc)
{
    Debug::Log("null_device.cpp: Initialize - app='%s' debug=%d",
        desc.appName.c_str(), desc.enableDebugLayer);
    m_initialized = true;
    return true;
}

void NullDevice::Shutdown()
{
    if (!m_initialized) return;
    Debug::Log("null_device.cpp: Shutdown - total draw calls: %u", m_totalDrawCalls);
    m_initialized = false;
}

void NullDevice::WaitIdle() {}

std::unique_ptr<ISwapchain> NullDevice::CreateSwapchain(const SwapchainDesc& desc)
{
    Debug::Log("null_device.cpp: CreateSwapchain %ux%u", desc.width, desc.height);
    return std::make_unique<NullSwapchain>(desc.width, desc.height, desc.bufferCount);
}

BufferHandle NullDevice::CreateBuffer(const BufferDesc& desc)
{
    BufferHandle h = BufferHandle::Make(m_nextBufferIdx++, 1u);
    Debug::LogVerbose("null_device.cpp: CreateBuffer '%s' %llu bytes → %u",
        desc.debugName.c_str(),
        static_cast<unsigned long long>(desc.byteSize),
        h.value);
    return h;
}

void  NullDevice::DestroyBuffer(BufferHandle) {}
void* NullDevice::MapBuffer(BufferHandle)     { m_mappedBuffer.resize(65536u, 0u); return m_mappedBuffer.data(); }
void  NullDevice::UnmapBuffer(BufferHandle)   {}

TextureHandle NullDevice::CreateTexture(const TextureDesc& desc)
{
    TextureHandle h = TextureHandle::Make(m_nextTexIdx++, 1u);
    Debug::LogVerbose("null_device.cpp: CreateTexture '%s' %ux%u → %u",
        desc.debugName.c_str(), desc.width, desc.height, h.value);
    return h;
}

void NullDevice::DestroyTexture(TextureHandle) {}

RenderTargetHandle NullDevice::CreateRenderTarget(const RenderTargetDesc& desc)
{
    RenderTargetHandle h = RenderTargetHandle::Make(m_nextRTIdx++, 1u);
    Debug::Log("null_device.cpp: CreateRenderTarget '%s' %ux%u → handle %u",
        desc.debugName.c_str(), desc.width, desc.height, h.value);
    return h;
}

void NullDevice::DestroyRenderTarget(RenderTargetHandle) {}

TextureHandle NullDevice::GetRenderTargetColorTexture(RenderTargetHandle rt) const
{
    return TextureHandle::Make(rt.Index(), rt.Generation());
}

TextureHandle NullDevice::GetRenderTargetDepthTexture(RenderTargetHandle rt) const
{
    return TextureHandle::Make(rt.Index() + 0x100u, rt.Generation());
}

ShaderHandle NullDevice::CreateShaderFromSource(const std::string&,
                                                  ShaderStageMask stage,
                                                  const std::string& entry,
                                                  const std::string& debugName)
{
    ShaderHandle h = ShaderHandle::Make(m_nextShaderIdx++, 1u);
    Debug::Log("null_device.cpp: CreateShaderFromSource '%s' stage=%u entry='%s' → %u",
        debugName.c_str(), static_cast<uint32_t>(stage), entry.c_str(), h.value);
    return h;
}

ShaderHandle NullDevice::CreateShaderFromBytecode(const void*, size_t sz,
                                                    ShaderStageMask,
                                                    const std::string& debugName)
{
    ShaderHandle h = ShaderHandle::Make(m_nextShaderIdx++, 1u);
    Debug::Log("null_device.cpp: CreateShaderFromBytecode '%s' %zu bytes → %u",
        debugName.c_str(), sz, h.value);
    return h;
}

void NullDevice::DestroyShader(ShaderHandle) {}

PipelineHandle NullDevice::CreatePipeline(const PipelineDesc& desc)
{
    PipelineHandle h = PipelineHandle::Make(m_nextPipelineIdx++, 1u);
    Debug::Log("null_device.cpp: CreatePipeline '%s' → %u", desc.debugName.c_str(), h.value);
    return h;
}

void     NullDevice::DestroyPipeline(PipelineHandle) {}
uint32_t NullDevice::CreateSampler(const SamplerDesc&) { return m_nextSamplerIdx++; }

std::unique_ptr<ICommandList> NullDevice::CreateCommandList(QueueType queue)
{
    // Übergibt Zeiger auf Device-Zähler → Submit() akkumuliert direkt
    return std::make_unique<NullCommandList>(queue, &m_totalDrawCalls);
}

std::unique_ptr<IFence> NullDevice::CreateFence(uint64_t initialValue)
{
    return std::make_unique<NullFence>(initialValue);
}

void NullDevice::UploadBufferData(BufferHandle h, const void*, size_t sz, size_t off)
{
    Debug::LogVerbose("null_device.cpp: UploadBufferData %u, %zu bytes @ %zu", h.value, sz, off);
}

void NullDevice::UploadTextureData(TextureHandle h, const void*, size_t sz,
                                    uint32_t mip, uint32_t slice)
{
    Debug::LogVerbose("null_device.cpp: UploadTextureData %u mip=%u slice=%u %zu bytes",
        h.value, mip, slice, sz);
}

void NullDevice::BeginFrame()
{
    ++m_frameIndex;
    Debug::LogVerbose("null_device.cpp: BeginFrame #%llu",
        static_cast<unsigned long long>(m_frameIndex));
}

void NullDevice::EndFrame()
{
    Debug::LogVerbose("null_device.cpp: EndFrame #%llu",
        static_cast<unsigned long long>(m_frameIndex));
}

uint32_t NullDevice::GetDrawCallCount() const
{
    // m_totalDrawCalls wird von jedem Submit() akkumuliert
    return m_totalDrawCalls;
}
const char* NullDevice::GetBackendName()   const { return "Null"; }

} // namespace engine::renderer::null_backend

namespace engine::renderer::null_backend {
namespace {
std::unique_ptr<IDevice> CreateNullDeviceInstance()
{
    return std::make_unique<NullDevice>();
}

// Registriert sich automatisch beim Linken — Core muss AddOn nicht kennen
struct AutoRegister {
    AutoRegister() {
        static DeviceFactory::Registrar registrar(DeviceFactory::BackendType::Null, &CreateNullDeviceInstance);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;
}

} // namespace engine::renderer::null_backend
