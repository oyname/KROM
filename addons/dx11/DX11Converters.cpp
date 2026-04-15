#include "DX11Device.hpp"
#include <d3d11.h>
#include "core/Debug.hpp"

namespace engine::renderer::dx11 {

// =============================================================================
// Format / State Mapping
// =============================================================================

uint32_t DX11Device::ToDXGIFormat(Format fmt) noexcept
{
    switch (fmt) {
    case Format::Unknown:              return 0;
    case Format::R8_UNORM:             return 61;
    case Format::RG8_UNORM:            return 49;
    case Format::RGBA8_UNORM:          return 28;
    case Format::RGBA8_UNORM_SRGB:     return 29;
    case Format::BGRA8_UNORM:          return 87;
    case Format::BGRA8_UNORM_SRGB:     return 91;
    case Format::R16_FLOAT:            return 54;
    case Format::RG16_FLOAT:           return 34;
    case Format::RGBA16_FLOAT:         return 10;
    case Format::R32_FLOAT:            return 41;
    case Format::RG32_FLOAT:           return 16;
    case Format::RGB32_FLOAT:          return 6;
    case Format::RGBA32_FLOAT:         return 2;
    case Format::R32_UINT:             return 42;
    case Format::R11G11B10_FLOAT:      return 26;
    case Format::BC1_UNORM:            return 71;
    case Format::BC1_UNORM_SRGB:       return 72;
    case Format::BC3_UNORM:            return 77;
    case Format::BC3_UNORM_SRGB:       return 78;
    case Format::BC4_UNORM:            return 80;
    case Format::BC5_UNORM:            return 83;
    case Format::BC7_UNORM:            return 98;
    case Format::BC7_UNORM_SRGB:       return 99;
    case Format::D16_UNORM:            return 55;
    case Format::D24_UNORM_S8_UINT:    return 45;
    case Format::D32_FLOAT:            return 40;
    case Format::D32_FLOAT_S8X24_UINT: return 20;
    default:
        Debug::LogWarning("DX11Device.cpp: ToDXGIFormat -- unbekanntes Format %u", static_cast<uint32_t>(fmt));
        return 0;
    }
}

uint32_t DX11Device::ToD3D11Usage(MemoryAccess access) noexcept
{
    switch (access) {
    case MemoryAccess::GpuOnly:  return 0; // D3D11_USAGE_DEFAULT
    case MemoryAccess::CpuWrite: return 2; // D3D11_USAGE_DYNAMIC
    case MemoryAccess::CpuRead:  return 3; // D3D11_USAGE_STAGING
    default:                     return 0;
    }
}

uint32_t DX11Device::ToD3D11BindFlags(ResourceUsage usage) noexcept
{
    uint32_t f = 0u;
        if (HasFlag(usage, ResourceUsage::VertexBuffer))    f |= 0x01u;
    if (HasFlag(usage, ResourceUsage::IndexBuffer))     f |= 0x02u;
    if (HasFlag(usage, ResourceUsage::ConstantBuffer))  f |= 0x04u;
    if (HasFlag(usage, ResourceUsage::ShaderResource))  f |= 0x08u;
    if (HasFlag(usage, ResourceUsage::RenderTarget))    f |= 0x20u;
    if (HasFlag(usage, ResourceUsage::DepthStencil))    f |= 0x40u;
    if (HasFlag(usage, ResourceUsage::UnorderedAccess)) f |= 0x80u;
    return f;
}

uint32_t DX11Device::ToD3D11Filter(const SamplerDesc& desc) noexcept
{
    // D3D11_FILTER Werte: Bit 2=mip, Bit 4=mag, Bit 6=min, Bit 7=aniso, Bit 8=comparison
    const bool aniso = desc.maxAniso > 1u;
    const bool cmp   = desc.compareFunc != CompareFunc::Never;
    if (aniso) return cmp ? 0xD5u : 0x55u; // D3D11_FILTER_ANISOTROPIC / _COMPARISON_ANISOTROPIC
    // Bilinear / Trilinear
    const bool linMin = desc.minFilter != FilterMode::Nearest;
    const bool linMag = desc.magFilter != FilterMode::Nearest;
    const bool linMip = desc.mipFilter != FilterMode::Nearest;
    uint32_t f = (linMip ? 0x01u : 0u) | (linMag ? 0x04u : 0u) | (linMin ? 0x10u : 0u);
    if (cmp) f |= 0x80u;
    return f;
}

uint32_t DX11Device::ToD3D11AddressMode(WrapMode mode) noexcept
{
    switch (mode) {
    case WrapMode::Repeat: return 1; // D3D11_TEXTURE_ADDRESS_WRAP
    case WrapMode::Mirror: return 2; // D3D11_TEXTURE_ADDRESS_MIRROR
    case WrapMode::Clamp:  return 3; // D3D11_TEXTURE_ADDRESS_CLAMP
    case WrapMode::Border: return 4; // D3D11_TEXTURE_ADDRESS_BORDER
    default:               return 1;
    }
}

uint32_t DX11Device::ToD3D11ComparisonFunc(CompareFunc func) noexcept
{
    switch (func) {
    case CompareFunc::Never:        return 1;
    case CompareFunc::Less:         return 2;
    case CompareFunc::Equal:        return 3;
    case CompareFunc::LessEqual:    return 4;
    case CompareFunc::Greater:      return 5;
    case CompareFunc::NotEqual:     return 6;
    case CompareFunc::GreaterEqual: return 7;
    case CompareFunc::Always:       return 8;
    default:                        return 1;
    }
}

uint32_t DX11Device::ToD3D11ComparisonFunc(DepthFunc func) noexcept
{
    switch (func) {
    case DepthFunc::Never:        return D3D11_COMPARISON_NEVER;
    case DepthFunc::Less:         return D3D11_COMPARISON_LESS;
    case DepthFunc::Equal:        return D3D11_COMPARISON_EQUAL;
    case DepthFunc::LessEqual:    return D3D11_COMPARISON_LESS_EQUAL;
    case DepthFunc::Greater:      return D3D11_COMPARISON_GREATER;
    case DepthFunc::NotEqual:     return D3D11_COMPARISON_NOT_EQUAL;
    case DepthFunc::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case DepthFunc::Always:       return D3D11_COMPARISON_ALWAYS;
    default:                      return D3D11_COMPARISON_NEVER;
    }
}

uint32_t DX11Device::ToD3D11Topology(PrimitiveTopology topo) noexcept
{
    switch (topo) {
    case PrimitiveTopology::TriangleList:  return 4;  // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    case PrimitiveTopology::TriangleStrip: return 5;  // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
    case PrimitiveTopology::LineList:      return 2;  // D3D11_PRIMITIVE_TOPOLOGY_LINELIST
    case PrimitiveTopology::LineStrip:     return 3;  // D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP
    case PrimitiveTopology::PointList:     return 1;  // D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
    default:                               return 4;
    }
}


} // namespace engine::renderer::dx11
