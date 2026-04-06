#include "DX11Device.hpp"
#include "core/Debug.hpp"

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <d3d11.h>
#   include <d3dcompiler.h>
#   include <dxgi.h>
#endif

namespace engine::renderer::dx11 {

// =============================================================================
// Swapchain
// =============================================================================

std::unique_ptr<ISwapchain> DX11Device::CreateSwapchain(const SwapchainDesc& desc)
{
#ifdef _WIN32
    if (!m_device || !m_factory) return nullptr;

    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferDesc.Width                   = desc.width;
    sc.BufferDesc.Height                  = desc.height;
    sc.BufferDesc.Format                  = static_cast<DXGI_FORMAT>(ToDXGIFormat(desc.format));
    sc.BufferDesc.RefreshRate.Numerator   = 0;
    sc.BufferDesc.RefreshRate.Denominator = 1;
    sc.SampleDesc.Count                   = 1;
    sc.SampleDesc.Quality                 = 0;
    sc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount                        = desc.bufferCount;
    sc.OutputWindow                       = static_cast<HWND>(desc.nativeWindowHandle);
    sc.Windowed                           = TRUE;
    sc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
    sc.Flags                              = 0;

    IDXGISwapChain* swapChain = nullptr;
    if (FAILED(m_factory->CreateSwapChain(m_device, &sc, &swapChain)))
    {
        Debug::LogError("DX11Device.cpp: CreateSwapchain FAILED");
        return nullptr;
    }

    Debug::Log("DX11Device.cpp: CreateSwapchain %ux%u buffers=%u", desc.width, desc.height, desc.bufferCount);
    return std::make_unique<DX11Swapchain>(
        m_device, m_context, swapChain, m_resources,
        desc.width, desc.height, desc.bufferCount);
#else
    (void)desc;
    Debug::LogWarning("DX11Device.cpp: CreateSwapchain -- kein Windows");
    return nullptr;
#endif
}

// =============================================================================
// DX11Swapchain
// =============================================================================

DX11Swapchain::DX11Swapchain(ID3D11Device* device, ID3D11DeviceContext* ctx,
                              IDXGISwapChain* sc, DX11DeviceResources& res,
                              uint32_t w, uint32_t h, uint32_t bufferCount)
    : m_device(device), m_ctx(ctx), m_sc(sc), m_res(&res)
    , m_width(w), m_height(h), m_bufferCount(bufferCount)
{
    AcquireBackbufferViews();
}

DX11Swapchain::~DX11Swapchain()
{
    ReleaseBackbufferViews();
#ifdef _WIN32
    if (m_sc) m_sc->SetFullscreenState(FALSE, nullptr);
    SafeRelease(m_sc);
#endif
}

void DX11Swapchain::AcquireBackbufferViews()
{
#ifdef _WIN32
    // DX11 DXGI_SWAP_EFFECT_DISCARD hat immer Backbuffer-Index 0
    ID3D11Texture2D* bbTex = nullptr;
    m_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bbTex));

    DX11RenderTargetEntry rte;
    rte.colorTex = bbTex;
    m_device->CreateRenderTargetView(bbTex, nullptr, &rte.rtv);

    DX11TextureEntry te; te.tex = bbTex;
    rte.colorHandle = m_res->textures.Add(std::move(te));

    const RenderTargetHandle rtH = m_res->renderTargets.Add(std::move(rte));
    m_bbRTs.push_back(rtH);
    m_bbTex.push_back(m_res->renderTargets.Get(rtH)->colorHandle);
#endif
}

void DX11Swapchain::ReleaseBackbufferViews()
{
    for (auto h : m_bbRTs)
    {
        auto* e = m_res->renderTargets.Get(h);
        if (e)
        {
            if (e->colorHandle.IsValid()) m_res->textures.Remove(e->colorHandle);
#ifdef _WIN32
            SafeRelease(e->rtv);
            // bbTex wird von DXGI besessen -- nicht selber releasen
            e->colorTex = nullptr;
#endif
        }
        m_res->renderTargets.Remove(h);
    }
    m_bbRTs.clear();
    m_bbTex.clear();
}

void DX11Swapchain::Present(bool vsync)
{
#ifdef _WIN32
    m_sc->Present(vsync ? 1u : 0u, 0u);
#else
    (void)vsync;
#endif
}

void DX11Swapchain::Resize(uint32_t w, uint32_t h)
{
#ifdef _WIN32
    if (m_ctx) m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ReleaseBackbufferViews();
    m_sc->ResizeBuffers(m_bufferCount, w, h, DXGI_FORMAT_UNKNOWN, 0);
    m_width = w; m_height = h;
    AcquireBackbufferViews();
    Debug::Log("DX11Swapchain: Resize %ux%u", w, h);
#else
    (void)w; (void)h;
#endif
}

TextureHandle DX11Swapchain::GetBackbufferTexture(uint32_t i) const
{
    return (i < m_bbTex.size()) ? m_bbTex[i] : TextureHandle::Invalid();
}

RenderTargetHandle DX11Swapchain::GetBackbufferRenderTarget(uint32_t i) const
{
    return (i < m_bbRTs.size()) ? m_bbRTs[i] : RenderTargetHandle::Invalid();
}

// =============================================================================
// DX11Fence
// =============================================================================

DX11Fence::DX11Fence(ID3D11Device* device, ID3D11DeviceContext* ctx, uint64_t init)
    : m_device(device), m_ctx(ctx), m_completedValue(init) {}

DX11Fence::~DX11Fence()
{
#ifdef _WIN32
    for (auto& pending : m_pending)
        SafeRelease(pending.query);
    m_pending.clear();
#endif
}

void DX11Fence::PollCompleted() const
{
#ifdef _WIN32
    if (!m_ctx)
        return;

    while (!m_pending.empty())
    {
        auto& pending = m_pending.front();
        BOOL result = FALSE;
        const HRESULT hr = m_ctx->GetData(pending.query, &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE)
            break;

        m_completedValue = pending.value;
        SafeRelease(pending.query);
        m_pending.erase(m_pending.begin());
    }
#endif
}

void DX11Fence::Signal(uint64_t value)
{
#ifdef _WIN32
    if (!m_device || !m_ctx)
        return;

    PollCompleted();

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;

    ID3D11Query* query = nullptr;
    if (FAILED(m_device->CreateQuery(&qd, &query)) || !query)
        return;

    m_ctx->End(query);
    m_pending.push_back(PendingQuery{query, value});
#else
    (void)value;
#endif
}

void DX11Fence::Wait(uint64_t value, uint64_t /*timeoutNs*/)
{
#ifdef _WIN32
    while (GetValue() < value)
    {
        if (m_pending.empty())
            break;

        auto& pending = m_pending.front();
        BOOL result = FALSE;
        while (m_ctx->GetData(pending.query, &result, sizeof(result), 0) == S_FALSE)
        { }
        PollCompleted();
    }
#else
    (void)value;
#endif
}

uint64_t DX11Fence::GetValue() const
{
    PollCompleted();
    return m_completedValue;
}


} // namespace engine::renderer::dx11
