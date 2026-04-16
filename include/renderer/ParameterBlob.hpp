#pragma once

#include "renderer/RendererTypes.hpp"
#include "renderer/ShaderParameterLayout.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace engine::renderer {

class ParameterBlob
{
public:
    static constexpr uint32_t kMaxSlots = ShaderParameterLayout::kMaxSlots;

    void Reset(const ShaderParameterLayout& layout);

    [[nodiscard]] bool IsInitialized() const noexcept { return m_slotCount > 0u || !m_constantData.empty(); }
    [[nodiscard]] uint32_t SlotCount() const noexcept { return m_slotCount; }

    [[nodiscard]] bool SetBytes(uint32_t slotIndex, const void* data, uint32_t byteSize);
    [[nodiscard]] bool SetTexture(uint32_t slotIndex, TextureHandle texture) noexcept;
    [[nodiscard]] bool SetBuffer(uint32_t slotIndex, BufferHandle buffer) noexcept;
    [[nodiscard]] bool SetSampler(uint32_t slotIndex, uint32_t samplerIndex) noexcept;

    [[nodiscard]] const std::vector<uint8_t>& ConstantData() const noexcept { return m_constantData; }
    [[nodiscard]] TextureHandle GetTexture(uint32_t slotIndex) const noexcept;
    [[nodiscard]] BufferHandle GetBuffer(uint32_t slotIndex) const noexcept;
    [[nodiscard]] uint32_t GetSampler(uint32_t slotIndex) const noexcept;

private:
    uint32_t m_slotCount = 0u;
    std::vector<uint8_t> m_constantData;
    std::array<TextureHandle, kMaxSlots> m_textures{};
    std::array<BufferHandle, kMaxSlots> m_buffers{};
    std::array<uint32_t, kMaxSlots> m_samplers{};
};

} // namespace engine::renderer
