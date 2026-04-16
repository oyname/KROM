#include "renderer/ParameterBlob.hpp"
#include <cstring>

namespace engine::renderer {

void ParameterBlob::Reset(const ShaderParameterLayout& layout)
{
    m_slotCount = layout.slotCount;
    uint32_t maxBytes = 0u;
    for (uint32_t i = 0u; i < layout.slotCount; ++i)
    {
        const ParameterSlot& slot = layout.slots[i];
        if (slot.type == ParameterType::Texture2D || slot.type == ParameterType::TextureCube ||
            slot.type == ParameterType::Sampler || slot.type == ParameterType::StructuredBuffer)
        {
            continue;
        }
        const uint32_t end = slot.byteOffset + slot.byteSize * (slot.elementCount == 0u ? 1u : slot.elementCount);
        if (end > maxBytes)
            maxBytes = end;
    }

    m_constantData.assign(maxBytes, 0u);
    m_textures.fill(TextureHandle::Invalid());
    m_buffers.fill(BufferHandle::Invalid());
    m_samplers.fill(0u);
}

bool ParameterBlob::SetBytes(uint32_t slotIndex, const void* data, uint32_t byteSize)
{
    if (!data || slotIndex >= m_slotCount || byteSize == 0u)
        return false;
    if (byteSize > m_constantData.size())
        return false;
    std::memcpy(m_constantData.data(), data, byteSize);
    return true;
}

bool ParameterBlob::SetTexture(uint32_t slotIndex, TextureHandle texture) noexcept
{
    if (slotIndex >= m_slotCount)
        return false;
    m_textures[slotIndex] = texture;
    return true;
}

bool ParameterBlob::SetBuffer(uint32_t slotIndex, BufferHandle buffer) noexcept
{
    if (slotIndex >= m_slotCount)
        return false;
    m_buffers[slotIndex] = buffer;
    return true;
}

bool ParameterBlob::SetSampler(uint32_t slotIndex, uint32_t samplerIndex) noexcept
{
    if (slotIndex >= m_slotCount)
        return false;
    m_samplers[slotIndex] = samplerIndex;
    return true;
}

TextureHandle ParameterBlob::GetTexture(uint32_t slotIndex) const noexcept
{
    return slotIndex < m_slotCount ? m_textures[slotIndex] : TextureHandle::Invalid();
}

BufferHandle ParameterBlob::GetBuffer(uint32_t slotIndex) const noexcept
{
    return slotIndex < m_slotCount ? m_buffers[slotIndex] : BufferHandle::Invalid();
}

uint32_t ParameterBlob::GetSampler(uint32_t slotIndex) const noexcept
{
    return slotIndex < m_slotCount ? m_samplers[slotIndex] : 0u;
}

} // namespace engine::renderer
