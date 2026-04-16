#pragma once

#include "renderer/RendererTypes.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace engine::renderer {

enum class ParameterType : uint8_t
{
    Unknown = 0,
    Float,
    Vec2,
    Vec3,
    Vec4,
    Int,
    Bool,
    Texture2D,
    TextureCube,
    Sampler,
    ConstantBuffer,
    StructuredBuffer,
};

struct ParameterSlot
{
    static constexpr uint32_t kMaxNameLength = 63u;

    char            name[kMaxNameLength + 1u]{};
    ParameterType   type = ParameterType::Unknown;
    uint32_t        binding = 0u;
    uint32_t        set = 0u;
    ShaderStageMask stageFlags = ShaderStageMask::None;
    uint32_t        byteOffset = 0u;
    uint32_t        byteSize = 0u;
    uint32_t        elementCount = 1u;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return type != ParameterType::Unknown && name[0] != '\0';
    }

    void SetName(std::string_view value) noexcept
    {
        const size_t count = value.size() < kMaxNameLength ? value.size() : kMaxNameLength;
        if (count > 0u)
            std::memcpy(name, value.data(), count);
        name[count] = '\0';
    }

    [[nodiscard]] std::string_view Name() const noexcept
    {
        return std::string_view(name, std::char_traits<char>::length(name));
    }
};

struct ShaderParameterLayout
{
    static constexpr uint32_t kMaxSlots = 32u;

    std::array<ParameterSlot, kMaxSlots> slots{};
    uint32_t slotCount = 0u;
    uint64_t layoutHash = 0ull;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return slotCount > 0u && slotCount <= kMaxSlots && layoutHash != 0ull;
    }

    [[nodiscard]] const ParameterSlot* FindByName(std::string_view name) const noexcept;
    [[nodiscard]] ParameterSlot* FindByName(std::string_view name) noexcept;
    [[nodiscard]] const ParameterSlot* FindByBinding(uint32_t binding, ParameterType type) const noexcept;
    [[nodiscard]] ParameterSlot* FindByBinding(uint32_t binding, ParameterType type) noexcept;
    [[nodiscard]] int32_t FindSlotIndexByName(std::string_view name) const noexcept;
    [[nodiscard]] int32_t FindSlotIndexByBinding(uint32_t binding, ParameterType type) const noexcept;
    [[nodiscard]] uint32_t CountSlotsOfType(ParameterType type) const noexcept;

    bool AddSlot(const ParameterSlot& slot) noexcept;
    void Clear() noexcept;
    void RecalculateHash() noexcept;
};

[[nodiscard]] uint64_t HashShaderParameterLayout(const ShaderParameterLayout& layout) noexcept;

} // namespace engine::renderer
