#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace engine::renderer {

enum class MaterialParameterType : uint8_t
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

enum class MaterialShaderStageMask : uint8_t
{
    None     = 0,
    Vertex   = 1 << 0,
    Fragment = 1 << 1,
    Compute  = 1 << 2,
    Geometry = 1 << 3,
    Hull     = 1 << 4,
    Domain   = 1 << 5,
};

inline MaterialShaderStageMask operator|(MaterialShaderStageMask a, MaterialShaderStageMask b) noexcept
{
    return static_cast<MaterialShaderStageMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline MaterialShaderStageMask operator&(MaterialShaderStageMask a, MaterialShaderStageMask b) noexcept
{
    return static_cast<MaterialShaderStageMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool HasFlag(MaterialShaderStageMask flags, MaterialShaderStageMask bit) noexcept
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(bit)) != 0u;
}

struct MaterialParameterSlot
{
    static constexpr uint32_t kMaxNameLength = 63u;

    char                  name[kMaxNameLength + 1u]{};
    MaterialParameterType type = MaterialParameterType::Unknown;
    uint32_t              binding = 0u;
    uint32_t              set = 0u;
    MaterialShaderStageMask stageFlags = MaterialShaderStageMask::None;
    uint32_t              byteOffset = 0u;
    uint32_t              byteSize = 0u;
    uint32_t              elementCount = 1u;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return type != MaterialParameterType::Unknown && name[0] != '\0';
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

struct MaterialParameterLayout
{
    static constexpr uint32_t kMaxSlots = 32u;

    std::array<MaterialParameterSlot, kMaxSlots> slots{};
    uint32_t slotCount = 0u;
    uint64_t layoutHash = 0ull;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return slotCount > 0u && slotCount <= kMaxSlots && layoutHash != 0ull;
    }

    [[nodiscard]] const MaterialParameterSlot* FindByName(std::string_view name) const noexcept;
    [[nodiscard]] MaterialParameterSlot* FindByName(std::string_view name) noexcept;
    [[nodiscard]] const MaterialParameterSlot* FindByBinding(uint32_t binding, MaterialParameterType type) const noexcept;
    [[nodiscard]] MaterialParameterSlot* FindByBinding(uint32_t binding, MaterialParameterType type) noexcept;
    [[nodiscard]] int32_t FindSlotIndexByName(std::string_view name) const noexcept;
    [[nodiscard]] int32_t FindSlotIndexByBinding(uint32_t binding, MaterialParameterType type) const noexcept;
    [[nodiscard]] uint32_t CountSlotsOfType(MaterialParameterType type) const noexcept;

    bool AddSlot(const MaterialParameterSlot& slot) noexcept;
    void Clear() noexcept;
    void RecalculateHash() noexcept;
};

[[nodiscard]] uint64_t HashMaterialParameterLayout(const MaterialParameterLayout& layout) noexcept;

} // namespace engine::renderer
