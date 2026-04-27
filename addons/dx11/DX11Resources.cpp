#include "renderer/TextureFormatUtils.hpp"
#include "DX11Device.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "core/Debug.hpp"
#include <cassert>
#include <cstring>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <d3d11.h>
#   include <d3dcompiler.h>
#   include <dxgi.h>
#endif

namespace engine::renderer::dx11 {

namespace {

#ifdef _WIN32
[[nodiscard]] static uint32_t BytesPerPixel(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM: return 1u;
    case DXGI_FORMAT_R8G8_UNORM: return 2u;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
        return 4u;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
        return 8u;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return 8u;
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return 12u;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16u;
    default:
        return 4u;
    }
}
#endif

} // namespace

// =============================================================================
// Buffer
// =============================================================================

BufferHandle DX11Device::CreateBuffer(const BufferDesc& desc)
{
#ifdef _WIN32
    D3D11_BUFFER_DESC d{};
    d.ByteWidth           = static_cast<UINT>(desc.byteSize);
    d.Usage               = static_cast<D3D11_USAGE>(ToD3D11Usage(desc.access));
    d.BindFlags           = ToD3D11BindFlags(desc.usage);
    d.CPUAccessFlags      = (desc.access == MemoryAccess::CpuWrite) ? D3D11_CPU_ACCESS_WRITE
                          : (desc.access == MemoryAccess::CpuRead)  ? D3D11_CPU_ACCESS_READ : 0u;

    // Constant Buffers brauchen 16-Byte-Alignment
    if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
        d.ByteWidth = (d.ByteWidth + 15u) & ~15u;

    ID3D11Buffer* buf = nullptr;
    const HRESULT hr = m_device->CreateBuffer(&d, nullptr, &buf);
    if (FAILED(hr)) {
        Debug::LogError("DX11Device.cpp: CreateBuffer '%s' FAILED 0x%08X", desc.debugName.c_str(), static_cast<unsigned>(hr));
        return BufferHandle::Invalid();
    }

    DX11BufferEntry entry;
    entry.buffer   = buf;
    entry.byteSize = static_cast<uint32_t>(desc.byteSize);
    entry.stride   = desc.stride;
    entry.dynamic  = (desc.access == MemoryAccess::CpuWrite);
    Debug::LogVerbose("DX11Device.cpp: CreateBuffer '%s' %zu bytes", desc.debugName.c_str(), desc.byteSize);
    return m_resources.buffers.Add(std::move(entry));
#else
    (void)desc; return BufferHandle::Invalid();
#endif
}

void DX11Device::DestroyBuffer(BufferHandle h)
{
    auto* e = m_resources.buffers.Get(h);
    if (!e) return;
#ifdef _WIN32
    SafeRelease(e->buffer);
#endif
    m_resources.buffers.Remove(h);
}

void* DX11Device::MapBuffer(BufferHandle h)
{
#ifdef _WIN32
    auto* e = m_resources.buffers.Get(h);
    if (!e || !e->dynamic) return nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(e->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return nullptr;
    e->mapped = mapped.pData;
    return mapped.pData;
#else
    (void)h; return nullptr;
#endif
}

void DX11Device::UnmapBuffer(BufferHandle h)
{
#ifdef _WIN32
    auto* e = m_resources.buffers.Get(h);
    if (!e || !e->mapped) return;
    m_context->Unmap(e->buffer, 0);
    e->mapped = nullptr;
#else
    (void)h;
#endif
}

// =============================================================================
// Texture
// =============================================================================

TextureHandle DX11Device::CreateTexture(const TextureDesc& desc)
{
#ifdef _WIN32
    D3D11_TEXTURE2D_DESC d{};
    d.Width     = desc.width;
    d.Height    = desc.height;
    d.MipLevels = desc.mipLevels;
    d.ArraySize = std::max(1u, desc.dimension == TextureDimension::Cubemap ? 6u : desc.arraySize);
    d.Format    = static_cast<DXGI_FORMAT>(ToDXGIFormat(desc.format));
    d.SampleDesc.Count = desc.sampleCount;
    d.Usage     = D3D11_USAGE_DEFAULT;
    d.BindFlags = ToD3D11BindFlags(desc.usage);
    if (desc.dimension == TextureDimension::Cubemap)
        d.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_device->CreateTexture2D(&d, nullptr, &tex))) {
        Debug::LogError("DX11Device.cpp: CreateTexture '%s' FAILED", desc.debugName.c_str());
        return TextureHandle::Invalid();
    }

    DX11TextureEntry entry;
    entry.tex = tex;
    entry.format = static_cast<uint32_t>(d.Format);
    entry.engineFormat = desc.format;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mipLevels = std::max(1u, desc.mipLevels);
    entry.arraySize = d.ArraySize;
    entry.dimension = desc.dimension;

    const bool isDepthFormat = d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT;
    if (isDepthFormat)
    {
        Debug::Log("ShadowTexture(dx11): name='%s' format=%d bindFlags=%u size=%ux%u mips=%u layers=%u",
            desc.debugName.c_str(),
            static_cast<int>(d.Format),
            static_cast<unsigned>(d.BindFlags),
            d.Width,
            d.Height,
            d.MipLevels,
            d.ArraySize);
    }

    // SRV erstellen wenn ShaderResource-Usage
    if (HasFlag(desc.usage, ResourceUsage::ShaderResource))
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = d.Format;
        if (desc.dimension == TextureDimension::Cubemap)
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MostDetailedMip = 0u;
            sd.TextureCube.MipLevels = d.MipLevels;
        }
        else if (d.ArraySize > 1)
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MostDetailedMip = 0u;
            sd.Texture2DArray.MipLevels = d.MipLevels;
            sd.Texture2DArray.FirstArraySlice = 0u;
            sd.Texture2DArray.ArraySize = d.ArraySize;
        }
        else
        {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MostDetailedMip = 0u;
            sd.Texture2D.MipLevels = d.MipLevels;
        }
        m_device->CreateShaderResourceView(tex, &sd, &entry.srv);
    }

    // UAV erstellen wenn UnorderedAccess-Usage
    if (HasFlag(desc.usage, ResourceUsage::UnorderedAccess))
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format        = d.Format;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(tex, &ud, &entry.uav);
    }

    Debug::LogVerbose("DX11Device.cpp: CreateTexture '%s' %ux%u", desc.debugName.c_str(), desc.width, desc.height);
    return m_resources.textures.Add(std::move(entry));
#else
    (void)desc; return TextureHandle::Invalid();
#endif
}

void DX11Device::DestroyTexture(TextureHandle h)
{
    auto* e = m_resources.textures.Get(h);
    if (!e) return;
#ifdef _WIN32
    SafeRelease(e->uav); SafeRelease(e->srv); SafeRelease(e->tex);
#endif
    m_resources.textures.Remove(h);
}

// =============================================================================
// RenderTarget
// =============================================================================

RenderTargetHandle DX11Device::CreateRenderTarget(const RenderTargetDesc& desc)
{
#ifdef _WIN32
    DX11RenderTargetEntry entry;
    entry.colorHandle = TextureHandle::Invalid();
    entry.depthHandle = TextureHandle::Invalid();

    if (desc.hasColor)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = desc.width;
        td.Height           = desc.height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = static_cast<DXGI_FORMAT>(ToDXGIFormat(desc.colorFormat));
        td.SampleDesc.Count = desc.sampleCount;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &entry.colorTex))) {
            Debug::LogError("DX11Device.cpp: CreateRenderTarget '%s' -- color tex FAILED", desc.debugName.c_str());
            return RenderTargetHandle::Invalid();
        }
        m_device->CreateRenderTargetView(entry.colorTex, nullptr, &entry.rtv);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format             = td.Format;
        sd.ViewDimension      = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(entry.colorTex, &sd, &entry.colorSRV);

        // Color-Texture als separaten TextureHandle registrieren
        DX11TextureEntry te; te.tex = entry.colorTex; te.srv = entry.colorSRV;
        entry.colorHandle = m_resources.textures.Add(std::move(te));
    }

    if (desc.hasDepth)
    {
        // Typless-Format fuer SRV+DSV gleichzeitig
        DXGI_FORMAT texFmt = DXGI_FORMAT_R24G8_TYPELESS;
        DXGI_FORMAT dsvFmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
        DXGI_FORMAT srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

        if (desc.depthFormat == Format::D32_FLOAT) {
            texFmt = DXGI_FORMAT_R32_TYPELESS;
            dsvFmt = DXGI_FORMAT_D32_FLOAT;
            srvFmt = DXGI_FORMAT_R32_FLOAT;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = desc.width;
        td.Height           = desc.height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = texFmt;
        td.SampleDesc.Count = desc.sampleCount;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &entry.depthTex))) {
            Debug::LogError("DX11Device.cpp: CreateRenderTarget '%s' -- depth tex FAILED", desc.debugName.c_str());
            // Color-Ressourcen freigeben
            SafeRelease(entry.colorSRV); SafeRelease(entry.rtv); SafeRelease(entry.colorTex);
            return RenderTargetHandle::Invalid();
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format        = dsvFmt;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(entry.depthTex, &dd, &entry.dsv);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format             = srvFmt;
        sd.ViewDimension      = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(entry.depthTex, &sd, &entry.depthSRV);

        DX11TextureEntry te; te.tex = entry.depthTex; te.srv = entry.depthSRV;
        entry.depthHandle = m_resources.textures.Add(std::move(te));
    }

    Debug::LogVerbose("DX11Device.cpp: CreateRenderTarget '%s' %ux%u", desc.debugName.c_str(), desc.width, desc.height);
    return m_resources.renderTargets.Add(std::move(entry));
#else
    (void)desc; return RenderTargetHandle::Invalid();
#endif
}

void DX11Device::DestroyRenderTarget(RenderTargetHandle h)
{
    auto* e = m_resources.renderTargets.Get(h);
    if (!e) return;

    // Farb- und Tiefen-TextureHandles aus dem Texture-Store entfernen
    // (colorTex/depthTex werden hier mit freigegeben)
    if (e->colorHandle.IsValid()) {
        m_resources.textures.Remove(e->colorHandle);  // leer; tex-Ptr Release unten
    }
    if (e->depthHandle.IsValid()) {
        m_resources.textures.Remove(e->depthHandle);
    }
#ifdef _WIN32
    SafeRelease(e->rtv); SafeRelease(e->dsv);
    SafeRelease(e->colorSRV); SafeRelease(e->depthSRV);
    SafeRelease(e->colorTex); SafeRelease(e->depthTex);
#endif
    m_resources.renderTargets.Remove(h);
}

TextureHandle DX11Device::GetRenderTargetColorTexture(RenderTargetHandle h) const
{
    const auto* e = m_resources.renderTargets.Get(h);
    return e ? e->colorHandle : TextureHandle::Invalid();
}

TextureHandle DX11Device::GetRenderTargetDepthTexture(RenderTargetHandle h) const
{
    const auto* e = m_resources.renderTargets.Get(h);
    return e ? e->depthHandle : TextureHandle::Invalid();
}

// =============================================================================
// Shader
// =============================================================================

ShaderHandle DX11Device::CreateShaderFromSource(const std::string& src, ShaderStageMask stage,
                                                 const std::string& entry, const std::string& dbg)
{
#ifdef _WIN32
    const bool isVS = (stage == ShaderStageMask::Vertex);
    const bool isPS = (stage == ShaderStageMask::Fragment);
    const bool isCS = (stage == ShaderStageMask::Compute);

    const char* target = isVS ? "vs_5_0" : isPS ? "ps_5_0" : isCS ? "cs_5_0" : nullptr;
    if (!target) {
        Debug::LogError("DX11Device.cpp: CreateShaderFromSource '%s' -- unbekannte Stage", dbg.c_str());
        return ShaderHandle::Invalid();
    }

    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors   = nullptr;
    UINT compileFlags  = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = D3DCompile(
        src.c_str(), src.size(), dbg.c_str(),
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry.c_str(), target, compileFlags, 0,
        &bytecode, &errors);

    if (FAILED(hr)) {
        const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)";
        Debug::LogError("DX11Device.cpp: CreateShaderFromSource '%s' -- D3DCompile FAILED: %s", dbg.c_str(), msg);
        if (errors) errors->Release();
        return ShaderHandle::Invalid();
    }
    if (errors) errors->Release();

    ShaderHandle h = CreateShaderFromBytecode(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(), stage, dbg);
    bytecode->Release();
    return h;
#else
    (void)src; (void)stage; (void)entry; (void)dbg;
    return ShaderHandle::Invalid();
#endif
}

ShaderHandle DX11Device::CreateShaderFromBytecode(const void* data, size_t sz,
                                                    ShaderStageMask stage, const std::string& dbg)
{
#ifdef _WIN32
    DX11ShaderEntry entry;
    entry.stage    = stage;
    entry.bytecode.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + sz);

    HRESULT hr = E_FAIL;
    if (stage == ShaderStageMask::Vertex) {
        hr = m_device->CreateVertexShader(data, sz, nullptr, &entry.vs);
    } else if (stage == ShaderStageMask::Fragment) {
        hr = m_device->CreatePixelShader(data, sz, nullptr, &entry.ps);
    } else if (stage == ShaderStageMask::Compute) {
        hr = m_device->CreateComputeShader(data, sz, nullptr, &entry.cs);
    }

    if (FAILED(hr)) {
        Debug::LogError("DX11Device.cpp: CreateShaderFromBytecode '%s' FAILED 0x%08X", dbg.c_str(), static_cast<unsigned>(hr));
        return ShaderHandle::Invalid();
    }
    Debug::LogVerbose("DX11Device.cpp: CreateShaderFromBytecode '%s' %zu bytes", dbg.c_str(), sz);
    return m_resources.shaders.Add(std::move(entry));
#else
    (void)data; (void)sz; (void)stage; (void)dbg;
    return ShaderHandle::Invalid();
#endif
}

void DX11Device::DestroyShader(ShaderHandle h)
{
    auto* e = m_resources.shaders.Get(h);
    if (!e) return;
#ifdef _WIN32
    SafeRelease(e->vs); SafeRelease(e->ps); SafeRelease(e->cs);
#endif
    m_resources.shaders.Remove(h);
}

// =============================================================================
// Pipeline -- 5 separate DX11-State-Objekte
// =============================================================================

#ifdef _WIN32
static const char* SemanticName(VertexSemantic s)
{
    switch (s) {
    case VertexSemantic::Position:  return "POSITION";
    case VertexSemantic::Normal:    return "NORMAL";
    case VertexSemantic::Tangent:   return "TANGENT";
    case VertexSemantic::Bitangent: return "BITANGENT";
    case VertexSemantic::TexCoord0: return "TEXCOORD";
    case VertexSemantic::TexCoord1: return "TEXCOORD";
    case VertexSemantic::Color0:    return "COLOR";
    case VertexSemantic::BoneWeight:return "BLENDWEIGHT";
    case VertexSemantic::BoneIndex: return "BLENDINDICES";
    default:                        return "TEXCOORD";
    }
}
static UINT SemanticIndex(VertexSemantic s) {
    return (s == VertexSemantic::TexCoord1) ? 1u : 0u;
}
#endif

ID3D11InputLayout* DX11Device::BuildInputLayout(const VertexLayout& vl, const DX11ShaderEntry& vs)
{
#ifdef _WIN32
    if (vl.attributes.empty() || vs.bytecode.empty()) return nullptr;

    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    elems.reserve(vl.attributes.size());
    for (const auto& a : vl.attributes)
    {
        D3D11_INPUT_ELEMENT_DESC el{};
        el.SemanticName      = SemanticName(a.semantic);
        el.SemanticIndex     = SemanticIndex(a.semantic);
        el.Format            = static_cast<DXGI_FORMAT>(ToDXGIFormat(a.format));
        el.InputSlot         = a.binding;
        el.AlignedByteOffset = a.offset;
        el.InputSlotClass    = D3D11_INPUT_PER_VERTEX_DATA;
        elems.push_back(el);
    }

    ID3D11InputLayout* il = nullptr;
    m_device->CreateInputLayout(
        elems.data(), static_cast<UINT>(elems.size()),
        vs.bytecode.data(), vs.bytecode.size(), &il);
    return il;
#else
    (void)vl; (void)vs; return nullptr;
#endif
}

ID3D11BlendState* DX11Device::BuildBlendState(const BlendState& bs)
{
#ifdef _WIN32
    D3D11_BLEND_DESC d{};
    d.RenderTarget[0].BlendEnable           = bs.blendEnable ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend              = static_cast<D3D11_BLEND>(static_cast<uint8_t>(bs.srcBlend) + 1);
    d.RenderTarget[0].DestBlend             = static_cast<D3D11_BLEND>(static_cast<uint8_t>(bs.dstBlend) + 1);
    d.RenderTarget[0].BlendOp               = static_cast<D3D11_BLEND_OP>(static_cast<uint8_t>(bs.blendOp) + 1);
    d.RenderTarget[0].SrcBlendAlpha         = static_cast<D3D11_BLEND>(static_cast<uint8_t>(bs.srcBlendAlpha) + 1);
    d.RenderTarget[0].DestBlendAlpha        = static_cast<D3D11_BLEND>(static_cast<uint8_t>(bs.dstBlendAlpha) + 1);
    d.RenderTarget[0].BlendOpAlpha          = static_cast<D3D11_BLEND_OP>(static_cast<uint8_t>(bs.blendOpAlpha) + 1);
    d.RenderTarget[0].RenderTargetWriteMask = bs.writeMask;
    ID3D11BlendState* state = nullptr;
    m_device->CreateBlendState(&d, &state);
    return state;
#else
    (void)bs; return nullptr;
#endif
}

ID3D11RasterizerState* DX11Device::BuildRasterizerState(const RasterizerState& rs)
{
#ifdef _WIN32
    D3D11_RASTERIZER_DESC d{};
    d.FillMode              = (rs.fillMode == FillMode::Wireframe) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    d.CullMode              = (rs.cullMode == CullMode::None)  ? D3D11_CULL_NONE
                            : (rs.cullMode == CullMode::Front) ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
    d.FrontCounterClockwise = (rs.frontFace == WindingOrder::CCW) ? TRUE : FALSE;
    d.DepthBias             = static_cast<INT>(rs.depthBias);
    d.SlopeScaledDepthBias  = rs.slopeScaledBias;
    d.DepthClipEnable       = rs.depthClip ? TRUE : FALSE;
    d.ScissorEnable         = rs.scissorTest   ? TRUE : FALSE;
    d.MultisampleEnable     = FALSE;
    d.AntialiasedLineEnable = FALSE;
    ID3D11RasterizerState* state = nullptr;
    m_device->CreateRasterizerState(&d, &state);
    return state;
#else
    (void)rs; return nullptr;
#endif
}

ID3D11DepthStencilState* DX11Device::BuildDepthStencilState(const DepthStencilState& ds)
{
#ifdef _WIN32
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable    = ds.depthEnable  ? TRUE  : FALSE;
    d.DepthWriteMask = ds.depthWrite   ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc      = static_cast<D3D11_COMPARISON_FUNC>(ToD3D11ComparisonFunc(ds.depthFunc));
    d.StencilEnable  = ds.stencilEnable ? TRUE : FALSE;
    ID3D11DepthStencilState* state = nullptr;
    m_device->CreateDepthStencilState(&d, &state);
    return state;
#else
    (void)ds; return nullptr;
#endif
}

PipelineHandle DX11Device::CreatePipeline(const PipelineDesc& desc)
{
#ifdef _WIN32
    DX11PipelineState state;
    state.key = PipelineKey::From(desc, StandardRenderPasses::Opaque());
    state.vertexLayout = desc.vertexLayout;

    // VS und PS aus ShaderHandles holen
    for (const auto& stage : desc.shaderStages)
    {
        auto* se = m_resources.shaders.Get(stage.handle);
        if (!se) continue;
        if (se->stage == ShaderStageMask::Vertex)   state.vs = se->vs;
        if (se->stage == ShaderStageMask::Fragment)  state.ps = se->ps;
    }

    if (!state.vs) {
        Debug::LogError("DX11Device.cpp: CreatePipeline '%s' -- VS fehlt", desc.debugName.c_str());
        return PipelineHandle::Invalid();
    }

    // VS-Entry fuer InputLayout suchen
    for (const auto& stage : desc.shaderStages) {
        auto* se = m_resources.shaders.Get(stage.handle);
        if (se && se->stage == ShaderStageMask::Vertex) {
            state.il = BuildInputLayout(desc.vertexLayout, *se);
            break;
        }
    }

    state.bs  = BuildBlendState(desc.blendStates[0]);
    state.rs  = BuildRasterizerState(desc.rasterizer);
    state.dss = BuildDepthStencilState(desc.depthStencil);
    state.topology = ToD3D11Topology(desc.topology);

    Debug::LogVerbose("DX11Device.cpp: CreatePipeline '%s'", desc.debugName.c_str());
    return m_resources.pipelines.Add(std::move(state));
#else
    (void)desc; return PipelineHandle::Invalid();
#endif
}

void DX11Device::DestroyPipeline(PipelineHandle h)
{
    auto* e = m_resources.pipelines.Get(h);
    if (!e) return;
#ifdef _WIN32
    // VS/PS werden NICHT released -- sie gehoeren dem ShaderStore
    SafeRelease(e->il); SafeRelease(e->bs);
    SafeRelease(e->rs); SafeRelease(e->dss);
#endif
    m_resources.pipelines.Remove(h);
}

// =============================================================================
// Sampler
// =============================================================================

uint32_t DX11Device::CreateSampler(const SamplerDesc& desc)
{
#ifdef _WIN32
    D3D11_SAMPLER_DESC d{};
    d.Filter         = static_cast<D3D11_FILTER>(ToD3D11Filter(desc));
    d.AddressU       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(ToD3D11AddressMode(desc.addressU));
    d.AddressV       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(ToD3D11AddressMode(desc.addressV));
    d.AddressW       = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(ToD3D11AddressMode(desc.addressW));
    d.MipLODBias     = desc.mipLodBias;
    d.MaxAnisotropy  = desc.maxAniso;
    d.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(ToD3D11ComparisonFunc(desc.compareFunc));
    d.MinLOD         = desc.minLod;
    d.MaxLOD         = desc.maxLod;
    std::memcpy(d.BorderColor, desc.borderColor, sizeof(d.BorderColor));

    DX11SamplerEntry entry;
    if (FAILED(m_device->CreateSamplerState(&d, &entry.sampler))) {
        Debug::LogError("DX11Device.cpp: CreateSampler FAILED");
        return 0u;
    }
    const uint32_t idx = static_cast<uint32_t>(m_resources.samplers.size());
    m_resources.samplers.push_back(std::move(entry));
    if (desc.compareFunc != CompareFunc::Never)
    {
        Debug::Log("ShadowSamplerDesc(dx11): handle=%u filter=%d addr=(%d,%d,%d) cmp=%d border=(%.1f %.1f %.1f %.1f)",
            idx,
            static_cast<int>(d.Filter),
            static_cast<int>(d.AddressU),
            static_cast<int>(d.AddressV),
            static_cast<int>(d.AddressW),
            static_cast<int>(d.ComparisonFunc),
            d.BorderColor[0],
            d.BorderColor[1],
            d.BorderColor[2],
            d.BorderColor[3]);
    }
    return idx;
#else
    (void)desc; return 0u;
#endif
}

// =============================================================================
// Upload
// =============================================================================

void DX11Device::UploadBufferData(BufferHandle h, const void* data, size_t sz, size_t dstOff)
{
#ifdef _WIN32
    auto* e = m_resources.buffers.Get(h);
    if (!e || !e->buffer || !data) return;

    if (e->dynamic) {
        // Dynamic Buffer: Map/Discard (Offset wird nicht unterstuetzt)
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(m_context->Map(e->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        {
            if (dstOff + sz <= e->byteSize)
                std::memcpy(static_cast<uint8_t*>(ms.pData) + dstOff, data, sz);
            m_context->Unmap(e->buffer, 0);
        }
    } else {
        D3D11_BUFFER_DESC desc{};
        e->buffer->GetDesc(&desc);
        if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0u)
        {
            m_context->UpdateSubresource(e->buffer, 0, nullptr, data, 0, 0);
        }
        else
        {
            D3D11_BOX box{};
            box.left  = static_cast<UINT>(dstOff);
            box.right = static_cast<UINT>(dstOff + sz);
            box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
            m_context->UpdateSubresource(e->buffer, 0, &box, data, 0, 0);
        }
    }
#else
    (void)h; (void)data; (void)sz; (void)dstOff;
#endif
}

void DX11Device::UploadTextureData(TextureHandle h, const void* data, size_t sz,
                                    uint32_t mip, uint32_t slice)
{
#ifdef _WIN32
    auto* e = m_resources.textures.Get(h);
    if (!e || !e->tex || !data || mip >= e->mipLevels || slice >= e->arraySize) return;

    const uint32_t mipWidth = std::max(1u, e->width >> mip);
    const uint32_t mipHeight = std::max(1u, e->height >> mip);
    const auto layout = ComputeTextureUploadLayout(e->engineFormat, mipWidth, mipHeight, 1u);
    const size_t minByteSize = static_cast<size_t>(layout.byteSize);
    if (layout.byteSize == 0u || sz < minByteSize)
        return;

    D3D11_TEXTURE2D_DESC td{};
    e->tex->GetDesc(&td);
    const UINT subresource = D3D11CalcSubresource(mip, slice, td.MipLevels);
    m_context->UpdateSubresource(e->tex, subresource, nullptr, data, static_cast<UINT>(layout.rowPitch), static_cast<UINT>(layout.sliceSize));
#else
    (void)h; (void)data; (void)sz; (void)mip; (void)slice;
#endif
}


} // namespace engine::renderer::dx11
