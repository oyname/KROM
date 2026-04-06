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
// DX11CommandList
// =============================================================================

DX11CommandList::DX11CommandList(ID3D11DeviceContext* ctx, DX11DeviceResources* res,
                                  bool deferred, uint32_t* devCounter)
    : m_ctx(ctx), m_res(res), m_deferred(deferred), m_devCounter(devCounter) {}

DX11CommandList::~DX11CommandList()
{
#ifdef _WIN32
    SafeRelease(m_cmdList);
#endif
}

void DX11CommandList::Begin()
{
    m_draws = 0u;
#ifdef _WIN32
    if (m_deferred && m_ctx)
        m_ctx->ClearState();
#endif
}

void DX11CommandList::End()
{
#ifdef _WIN32
    if (m_deferred && m_ctx)
        m_ctx->FinishCommandList(FALSE, &m_cmdList);
#endif
}

void DX11CommandList::BeginRenderPass(const RenderPassBeginInfo& info)
{
#ifdef _WIN32
    m_activeRT = info.renderTarget;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;

    if (info.renderTarget.IsValid())
    {
        auto* rt = m_res->renderTargets.Get(info.renderTarget);
        if (rt) { rtv = rt->rtv; dsv = rt->dsv; }
    }
    ID3D11ShaderResourceView* nullSRVs[16] = {};
    m_ctx->VSSetShaderResources(0, 16, nullSRVs);
    m_ctx->PSSetShaderResources(0, 16, nullSRVs);
    m_ctx->CSSetShaderResources(0, 16, nullSRVs);
    m_ctx->OMSetRenderTargets(rtv ? 1u : 0u, rtv ? &rtv : nullptr, dsv);

    if (rtv && info.clearColor)
        m_ctx->ClearRenderTargetView(rtv, info.colorClear.color);
    if (dsv && (info.clearDepth || info.clearStencil))
    {
        UINT flags = 0;
        if (info.clearDepth)   flags |= D3D11_CLEAR_DEPTH;
        if (info.clearStencil) flags |= D3D11_CLEAR_STENCIL;
        m_ctx->ClearDepthStencilView(dsv, flags, info.depthClear.depth, info.depthClear.stencil);
    }
#else
    (void)info;
#endif
}

void DX11CommandList::EndRenderPass()
{
#ifdef _WIN32
    // RT unbinden -- wichtig fuer RT -> SRV-Uebergang
    ID3D11RenderTargetView* nullRTV = nullptr;
    m_ctx->OMSetRenderTargets(0, &nullRTV, nullptr);
    m_activeRT = RenderTargetHandle::Invalid();
#endif
}

void DX11CommandList::SetPipeline(PipelineHandle pipeline)
{
    m_activePipeline = pipeline;
#ifdef _WIN32
    auto* p = m_res->pipelines.Get(pipeline);
    if (!p || !p->isValid()) return;

    m_ctx->VSSetShader(p->vs, nullptr, 0);
    m_ctx->PSSetShader(p->ps, nullptr, 0);
    m_ctx->IASetInputLayout(p->il);  // null = kein Vertex-Input (SV_VertexID, Fullscreen-Triangle)
    if (p->bs) { const float bf[4]{}; m_ctx->OMSetBlendState(p->bs, bf, 0xFFFFFFFFu); }
    if (p->rs)  m_ctx->RSSetState(p->rs);
    if (p->dss) m_ctx->OMSetDepthStencilState(p->dss, 0u);
    m_ctx->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
#else
    (void)pipeline;
#endif
}

void DX11CommandList::SetVertexBuffer(uint32_t slot, BufferHandle h, uint32_t offset)
{
#ifdef _WIN32
    auto* e = m_res->buffers.Get(h);
    if (!e) return;

    // Stride aus BufferDesc::stride (gesetzt bei CreateBuffer aus dem VertexLayout)
    UINT stride = e->stride;

    // Fallback: Stride aus der aktiven Pipeline ableiten
    if (stride == 0u) {
        auto* p = m_res->pipelines.Get(m_activePipeline);
        if (p) {
            for (const auto& binding : p->vertexLayout.bindings) {
                if (binding.binding == slot) {
                    stride = binding.stride;
                    break;
                }
            }
        }
    }

    UINT off = offset;
    m_ctx->IASetVertexBuffers(slot, 1, &e->buffer, &stride, &off);
#else
    (void)slot; (void)h; (void)offset;
#endif
}

void DX11CommandList::SetIndexBuffer(BufferHandle h, bool is32bit, uint32_t offset)
{
#ifdef _WIN32
    auto* e = m_res->buffers.Get(h);
    if (!e) return;
    m_ctx->IASetIndexBuffer(e->buffer,
        is32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, offset);
#else
    (void)h; (void)is32bit; (void)offset;
#endif
}

void DX11CommandList::SetConstantBuffer(uint32_t slot, BufferHandle h, ShaderStageMask stages)
{
#ifdef _WIN32
    auto* e = m_res->buffers.Get(h);
    if (!e) return;
    const uint8_t s = static_cast<uint8_t>(stages);
    if (s & static_cast<uint8_t>(ShaderStageMask::Vertex))   m_ctx->VSSetConstantBuffers(slot, 1, &e->buffer);
    if (s & static_cast<uint8_t>(ShaderStageMask::Fragment))  m_ctx->PSSetConstantBuffers(slot, 1, &e->buffer);
    if (s & static_cast<uint8_t>(ShaderStageMask::Compute))   m_ctx->CSSetConstantBuffers(slot, 1, &e->buffer);
#else
    (void)slot; (void)h; (void)stages;
#endif
}

void DX11CommandList::SetShaderResource(uint32_t slot, TextureHandle h, ShaderStageMask stages)
{
#ifdef _WIN32
    auto* e = m_res->textures.Get(h);
    ID3D11ShaderResourceView* srv = e ? e->srv : nullptr;
    const uint8_t s = static_cast<uint8_t>(stages);
    if (s & static_cast<uint8_t>(ShaderStageMask::Vertex))   m_ctx->VSSetShaderResources(slot, 1, &srv);
    if (s & static_cast<uint8_t>(ShaderStageMask::Fragment))  m_ctx->PSSetShaderResources(slot, 1, &srv);
    if (s & static_cast<uint8_t>(ShaderStageMask::Compute))   m_ctx->CSSetShaderResources(slot, 1, &srv);
#else
    (void)slot; (void)h; (void)stages;
#endif
}

void DX11CommandList::SetSampler(uint32_t slot, uint32_t samplerIdx, ShaderStageMask stages)
{
#ifdef _WIN32
    ID3D11SamplerState* ss = (samplerIdx < m_res->samplers.size())
                             ? m_res->samplers[samplerIdx].sampler : nullptr;
    const uint8_t s = static_cast<uint8_t>(stages);
    if (s & static_cast<uint8_t>(ShaderStageMask::Vertex))   m_ctx->VSSetSamplers(slot, 1, &ss);
    if (s & static_cast<uint8_t>(ShaderStageMask::Fragment))  m_ctx->PSSetSamplers(slot, 1, &ss);
    if (s & static_cast<uint8_t>(ShaderStageMask::Compute))   m_ctx->CSSetSamplers(slot, 1, &ss);
#else
    (void)slot; (void)samplerIdx; (void)stages;
#endif
}

void DX11CommandList::SetViewport(float x, float y, float w, float h, float mn, float mx)
{
#ifdef _WIN32
    const D3D11_VIEWPORT vp{ x, y, w, h, mn, mx };
    m_ctx->RSSetViewports(1, &vp);
#else
    (void)x; (void)y; (void)w; (void)h; (void)mn; (void)mx;
#endif
}

void DX11CommandList::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
#ifdef _WIN32
    const D3D11_RECT r{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
    m_ctx->RSSetScissorRects(1, &r);
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

void DX11CommandList::Draw(uint32_t verts, uint32_t inst, uint32_t firstVert, uint32_t /*firstInst*/)
{
#ifdef _WIN32
    m_ctx->DrawInstanced(verts, inst, firstVert, 0u);
#else
    (void)verts; (void)inst; (void)firstVert;
#endif
    ++m_draws;
}

void DX11CommandList::DrawIndexed(uint32_t idx, uint32_t inst, uint32_t firstIdx,
                                   int32_t voff, uint32_t /*firstInst*/)
{
#ifdef _WIN32
    m_ctx->DrawIndexedInstanced(idx, inst, firstIdx, voff, 0u);
#else
    (void)idx; (void)inst; (void)firstIdx; (void)voff;
#endif
    ++m_draws;
}

void DX11CommandList::Dispatch(uint32_t gx, uint32_t gy, uint32_t gz)
{
#ifdef _WIN32
    m_ctx->Dispatch(gx, gy, gz);
#else
    (void)gx; (void)gy; (void)gz;
#endif
}

// Transitions: DX11 ist implizit -- RT->SRV wird durch EndRenderPass abgedeckt.
void DX11CommandList::TransitionResource(BufferHandle, ResourceState, ResourceState) {}
void DX11CommandList::TransitionResource(TextureHandle tex, ResourceState before, ResourceState after)
{
#ifdef _WIN32
    // RT -> ShaderRead: sicherstellen dass RT ungebunden ist.
    // Wird durch EndRenderPass() + OMSetRenderTargets(0,...) gehandhabt.
    if (before == ResourceState::RenderTarget && after == ResourceState::ShaderRead)
        if (m_activeRT.IsValid()) m_ctx->Flush();
#else
    (void)tex; (void)before; (void)after;
#endif
}
void DX11CommandList::TransitionRenderTarget(RenderTargetHandle, ResourceState, ResourceState) {}

void DX11CommandList::CopyBuffer(BufferHandle dst, uint64_t dstOff,
                                  BufferHandle src, uint64_t srcOff, uint64_t size)
{
#ifdef _WIN32
    auto* d = m_res->buffers.Get(dst);
    auto* s = m_res->buffers.Get(src);
    if (!d || !s) return;
    D3D11_BOX box{};
    box.left = static_cast<UINT>(srcOff); box.right = static_cast<UINT>(srcOff + size);
    box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
    m_ctx->CopySubresourceRegion(d->buffer, 0, static_cast<UINT>(dstOff), 0, 0, s->buffer, 0, &box);
#else
    (void)dst; (void)dstOff; (void)src; (void)srcOff; (void)size;
#endif
}

void DX11CommandList::CopyTexture(TextureHandle dst, uint32_t dstMip,
                                   TextureHandle src, uint32_t srcMip)
{
#ifdef _WIN32
    auto* d = m_res->textures.Get(dst);
    auto* s = m_res->textures.Get(src);
    if (!d || !s) return;
    m_ctx->CopySubresourceRegion(d->tex, dstMip, 0, 0, 0, s->tex, srcMip, nullptr);
#else
    (void)dst; (void)dstMip; (void)src; (void)srcMip;
#endif
}

void DX11CommandList::Submit(QueueType)
{
#ifdef _WIN32
    if (m_deferred && m_cmdList)
    {
        // Voraussetzung: der Aufrufer muss den Immediate Context haben
        // um ExecuteCommandList aufzurufen. Hier kein Zugriff darauf --
        // Deferred Context ist ein optionaler Pfad, Immediate ist Standard.
    }
#endif
    if (m_devCounter) *m_devCounter += m_draws;
    m_draws = 0u;
}


} // namespace engine::renderer::dx11
