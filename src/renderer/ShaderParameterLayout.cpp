#include "renderer/ShaderParameterLayout.hpp"

namespace engine::renderer {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

template<typename T>
void HashValue(uint64_t& hash, const T& value) noexcept
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
}

void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
}

} // namespace

const ParameterSlot* ShaderParameterLayout::FindByName(std::string_view name) const noexcept
{
    const int32_t index = FindSlotIndexByName(name);
    return index >= 0 ? &slots[static_cast<size_t>(index)] : nullptr;
}

ParameterSlot* ShaderParameterLayout::FindByName(std::string_view name) noexcept
{
    const int32_t index = FindSlotIndexByName(name);
    return index >= 0 ? &slots[static_cast<size_t>(index)] : nullptr;
}

const ParameterSlot* ShaderParameterLayout::FindByBinding(uint32_t binding, ParameterType type) const noexcept
{
    const int32_t index = FindSlotIndexByBinding(binding, type);
    return index >= 0 ? &slots[static_cast<size_t>(index)] : nullptr;
}

ParameterSlot* ShaderParameterLayout::FindByBinding(uint32_t binding, ParameterType type) noexcept
{
    const int32_t index = FindSlotIndexByBinding(binding, type);
    return index >= 0 ? &slots[static_cast<size_t>(index)] : nullptr;
}

int32_t ShaderParameterLayout::FindSlotIndexByName(std::string_view name) const noexcept
{
    for (uint32_t i = 0u; i < slotCount; ++i)
    {
        if (slots[i].Name() == name)
            return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t ShaderParameterLayout::FindSlotIndexByBinding(uint32_t binding, ParameterType type) const noexcept
{
    for (uint32_t i = 0u; i < slotCount; ++i)
    {
        if (slots[i].binding == binding && slots[i].type == type)
            return static_cast<int32_t>(i);
    }
    return -1;
}

uint32_t ShaderParameterLayout::CountSlotsOfType(ParameterType type) const noexcept
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < slotCount; ++i)
    {
        if (slots[i].type == type)
            ++count;
    }
    return count;
}

bool ShaderParameterLayout::AddSlot(const ParameterSlot& slot) noexcept
{
    if (!slot.IsValid() || slotCount >= kMaxSlots)
        return false;

    slots[slotCount++] = slot;
    RecalculateHash();
    return true;
}

void ShaderParameterLayout::Clear() noexcept
{
    slotCount = 0u;
    layoutHash = 0ull;
    slots.fill(ParameterSlot{});
}

void ShaderParameterLayout::RecalculateHash() noexcept
{
    layoutHash = HashShaderParameterLayout(*this);
}

uint64_t HashShaderParameterLayout(const ShaderParameterLayout& layout) noexcept
{
    uint64_t hash = kFnvOffset;
    HashValue(hash, layout.slotCount);
    for (uint32_t i = 0u; i < layout.slotCount; ++i)
    {
        const ParameterSlot& slot = layout.slots[i];
        HashBytes(hash, slot.name, sizeof(slot.name));
        HashValue(hash, slot.type);
        HashValue(hash, slot.binding);
        HashValue(hash, slot.set);
        const uint32_t stageBits = static_cast<uint32_t>(slot.stageFlags);
        HashValue(hash, stageBits);
        HashValue(hash, slot.byteOffset);
        HashValue(hash, slot.byteSize);
        HashValue(hash, slot.elementCount);
    }
    return hash;
}

} // namespace engine::renderer
