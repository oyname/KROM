#include "VulkanDevice.hpp"

namespace engine::renderer::vulkan {

VkFormat ToVkFormat(Format format) noexcept
{
    switch (format)
    {
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case Format::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RGB32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkImageAspectFlags ToVkAspectFlags(Format format) noexcept
{
    switch (format)
    {
    case Format::D24_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case Format::D32_FLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkPrimitiveTopology ToVkTopology(PrimitiveTopology topology) noexcept
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveTopology::TriangleList:
    default:                               return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkShaderStageFlagBits ToVkShaderStage(ShaderStageMask stage) noexcept
{
    switch (stage)
    {
    case ShaderStageMask::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStageMask::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStageMask::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStageMask::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case ShaderStageMask::Hull:     return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStageMask::Domain:   return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    default:                        return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

VkSamplerAddressMode ToVkAddressMode(WrapMode mode) noexcept
{
    switch (mode)
    {
    case WrapMode::Clamp:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case WrapMode::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case WrapMode::Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case WrapMode::Repeat:
    default:               return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFilter ToVkFilter(FilterMode mode) noexcept
{
    switch (mode)
    {
    case FilterMode::Nearest: return VK_FILTER_NEAREST;
    case FilterMode::Anisotropic:
    case FilterMode::Linear:
    default:                  return VK_FILTER_LINEAR;
    }
}

VkSamplerMipmapMode ToVkMipmapMode(FilterMode mode) noexcept
{
    return mode == FilterMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkCompareOp ToVkCompareOp(CompareFunc func) noexcept
{
    switch (func)
    {
    case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
    case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
    case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
    case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
    case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
    case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareFunc::Always:
    default:                        return VK_COMPARE_OP_ALWAYS;
    }
}

VkCompareOp ToVkCompareOp(DepthFunc func) noexcept
{
    switch (func)
    {
    case DepthFunc::Never:        return VK_COMPARE_OP_NEVER;
    case DepthFunc::Less:         return VK_COMPARE_OP_LESS;
    case DepthFunc::Equal:        return VK_COMPARE_OP_EQUAL;
    case DepthFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case DepthFunc::Greater:      return VK_COMPARE_OP_GREATER;
    case DepthFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
    case DepthFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case DepthFunc::Always:
    default:                      return VK_COMPARE_OP_ALWAYS;
    }
}

VkBlendFactor ToVkBlendFactor(BlendFactor factor) noexcept
{
    switch (factor)
    {
    case BlendFactor::Zero:         return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:          return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor:     return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InvSrcColor:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor:     return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InvDstColor:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha:     return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:     return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InvDstAlpha:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:                        return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp ToVkBlendOp(BlendOp op) noexcept
{
    switch (op)
    {
    case BlendOp::Subtract:    return VK_BLEND_OP_SUBTRACT;
    case BlendOp::RevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min:         return VK_BLEND_OP_MIN;
    case BlendOp::Max:         return VK_BLEND_OP_MAX;
    case BlendOp::Add:
    default:                   return VK_BLEND_OP_ADD;
    }
}

VkCullModeFlags ToVkCullMode(CullMode mode) noexcept
{
    switch (mode)
    {
    case CullMode::None:  return VK_CULL_MODE_NONE;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back:
    default:              return VK_CULL_MODE_BACK_BIT;
    }
}

VkFrontFace ToVkFrontFace(WindingOrder order) noexcept
{
    return order == WindingOrder::CW ? VK_FRONT_FACE_CLOCKWISE
                                     : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkBufferUsageFlags ToVkBufferUsage(const BufferDesc& desc) noexcept
{
    VkBufferUsageFlags flags = 0u;
    if (HasFlag(desc.usage, ResourceUsage::VertexBuffer))    flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (HasFlag(desc.usage, ResourceUsage::IndexBuffer))     flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (HasFlag(desc.usage, ResourceUsage::ConstantBuffer))  flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (HasFlag(desc.usage, ResourceUsage::CopySource))      flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (HasFlag(desc.usage, ResourceUsage::CopyDest))        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (flags == 0u)
        flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkImageUsageFlags ToVkImageUsage(const TextureDesc& desc) noexcept
{
    VkImageUsageFlags flags = 0u;
    const bool isShaderResource = HasFlag(desc.usage, ResourceUsage::ShaderResource);
    const bool isDepthStencil = HasFlag(desc.usage, ResourceUsage::DepthStencil);

    if (isShaderResource)
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (HasFlag(desc.usage, ResourceUsage::RenderTarget))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (isDepthStencil)
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (HasFlag(desc.usage, ResourceUsage::CopySource))
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (HasFlag(desc.usage, ResourceUsage::CopyDest))
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (HasFlag(desc.usage, ResourceUsage::UnorderedAccess))
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;

    // Normale Shader-Texturen werden engine-weit über denselben Uploadpfad erzeugt.
    // Vulkan braucht dafür explizit TRANSFER_DST statt eines separaten Sonderwegs im Asset-/Materialcode.
    if (isShaderResource && !isDepthStencil)
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (flags == 0u)
        flags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkImageLayout ToVkImageLayout(ResourceState state) noexcept
{
    switch (state)
    {
    case ResourceState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthWrite:   return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthRead:    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case ResourceState::ShaderRead:   return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ResourceState::UnorderedAccess:return VK_IMAGE_LAYOUT_GENERAL;
    case ResourceState::CopySource:   return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ResourceState::CopyDest:     return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case ResourceState::Present:      return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case ResourceState::Common:       return VK_IMAGE_LAYOUT_GENERAL;
    case ResourceState::Unknown:
    default:                         return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

VkAccessFlags ToVkAccessFlags(ResourceState state) noexcept
{
    switch (state)
    {
    case ResourceState::RenderTarget: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    case ResourceState::DepthWrite:   return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    case ResourceState::DepthRead:    return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    case ResourceState::ShaderRead:   return VK_ACCESS_SHADER_READ_BIT;
    case ResourceState::UnorderedAccess:return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case ResourceState::CopySource:   return VK_ACCESS_TRANSFER_READ_BIT;
    case ResourceState::CopyDest:     return VK_ACCESS_TRANSFER_WRITE_BIT;
    case ResourceState::VertexBuffer: return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    case ResourceState::IndexBuffer:  return VK_ACCESS_INDEX_READ_BIT;
    case ResourceState::ConstantBuffer:return VK_ACCESS_UNIFORM_READ_BIT;
    default:                         return 0u;
    }
}

VkPipelineStageFlags ToVkPipelineStage(ResourceState state) noexcept
{
    switch (state)
    {
    case ResourceState::RenderTarget:  return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case ResourceState::DepthWrite:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case ResourceState::DepthRead:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    case ResourceState::ShaderRead:    return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ResourceState::UnorderedAccess:return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case ResourceState::CopySource:
    case ResourceState::CopyDest:      return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case ResourceState::VertexBuffer:  return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    case ResourceState::IndexBuffer:   return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    case ResourceState::ConstantBuffer:return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case ResourceState::Present:       return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    default:                           return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

} // namespace engine::renderer::vulkan
