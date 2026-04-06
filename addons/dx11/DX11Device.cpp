#include "DX11Device.hpp"
#include "core/Debug.hpp"
#include <cassert>
#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <d3d11.h>
#   include <d3dcompiler.h>
#   include <dxgi.h>
#endif
namespace engine::renderer::dx11 {
DX11Device::DX11Device() = default;
DX11Device::~DX11Device() { Shutdown(); }
bool DX11Device::Initialize(const DeviceDesc& desc)
{
#ifdef _WIN32
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&m_factory)))) { Debug::LogError("DX11Device.cpp: Initialize -- CreateDXGIFactory failed"); return false; }
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(m_factory->EnumAdapters(desc.adapterIndex, &adapter))) { Debug::LogWarning("DX11Device.cpp: adapterIndex=%u nicht gefunden, Fallback auf Default", desc.adapterIndex); adapter = nullptr; }
    UINT flags = desc.enableDebugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0u;
    D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_10_0;
    const HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &m_device, &achieved, &m_context);
    if (adapter) adapter->Release();
    if (FAILED(hr)) { Debug::LogError("DX11Device.cpp: D3D11CreateDevice failed 0x%08X", static_cast<unsigned>(hr)); return false; }
    m_featureLevel = static_cast<uint32_t>(achieved);
    if (desc.enableDebugLayer) m_device->QueryInterface(__uuidof(ID3D11Debug), reinterpret_cast<void**>(&m_debug));
    m_hasDeferredContext = achieved >= D3D_FEATURE_LEVEL_11_0;
    Debug::Log("DX11Device.cpp: Initialize OK '%s' FL=0x%04X debug=%d", desc.appName.c_str(), m_featureLevel, static_cast<int>(desc.enableDebugLayer));
#else
    Debug::LogWarning("DX11Device.cpp: Initialize -- DX11 nicht verfuegbar (kein Windows)");
    return false;
#endif
    m_initialized = true;
    return true;
}
void DX11Device::Shutdown()
{
    if (!m_initialized) return;
#ifdef _WIN32
    m_resources.pipelines.ForEach([](DX11PipelineState& p){ SafeRelease(p.vs); SafeRelease(p.ps); SafeRelease(p.il); SafeRelease(p.bs); SafeRelease(p.rs); SafeRelease(p.dss); });
    m_resources.shaders.ForEach([](DX11ShaderEntry& s){ SafeRelease(s.vs); SafeRelease(s.ps); SafeRelease(s.cs); });
    m_resources.renderTargets.ForEach([](DX11RenderTargetEntry& rt){ SafeRelease(rt.rtv); SafeRelease(rt.dsv); SafeRelease(rt.colorSRV); SafeRelease(rt.depthSRV); SafeRelease(rt.colorTex); SafeRelease(rt.depthTex); });
    m_resources.textures.ForEach([](DX11TextureEntry& t){ SafeRelease(t.uav); SafeRelease(t.srv); SafeRelease(t.tex); });
    m_resources.buffers.ForEach([](DX11BufferEntry& b){ SafeRelease(b.buffer); });
    for (auto& s : m_resources.samplers) SafeRelease(s.sampler);
    m_resources.samplers.clear();
    if (m_context) { m_context->ClearState(); m_context->Flush(); }
    SafeRelease(m_debug); SafeRelease(m_context); SafeRelease(m_device); SafeRelease(m_factory);
#endif
    m_initialized = false;
    Debug::Log("DX11Device.cpp: Shutdown");
}
void DX11Device::WaitIdle()
{
#ifdef _WIN32
    if (m_context)
        m_context->Flush();
#endif
}
bool DX11Device::SupportsFeature(const char* feature) const
{
    if (!feature) return false;
    const std::string f(feature);
    if (f == "compute") return SupportsComputeShaders();
    if (f == "deferred_context") return SupportsDeferredContexts();
    if (f == "tessellation") return m_featureLevel >= 0xB000u;
    return false;
}
std::unique_ptr<ICommandList> DX11Device::CreateCommandList(QueueType queue)
{
    (void)queue;
#ifdef _WIN32
    return m_context ? std::make_unique<DX11CommandList>(m_context, &m_resources, false, &m_totalDrawCalls) : nullptr;
#else
    return nullptr;
#endif
}
std::unique_ptr<IFence> DX11Device::CreateFence(uint64_t initialValue)
{
#ifdef _WIN32
    return (m_device && m_context) ? std::make_unique<DX11Fence>(m_device, m_context, initialValue) : nullptr;
#else
    (void)initialValue; return nullptr;
#endif
}
void DX11Device::BeginFrame() { ++m_frameIndex; }
void DX11Device::EndFrame() {}
uint32_t DX11Device::GetDrawCallCount() const { return m_totalDrawCalls; }
namespace {
std::unique_ptr<IDevice> CreateDX11DeviceInstance()
{
    return std::make_unique<DX11Device>();
}

// Registriert sich automatisch beim Linken — Core muss AddOn nicht kennen.
struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::DirectX11,
            &CreateDX11DeviceInstance,
            &DX11Device::EnumerateAdaptersImpl);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;
} // namespace
} // namespace engine::renderer::dx11
