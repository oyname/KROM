#pragma once
// =============================================================================
// KROM Engine - addons/animation/SkinningSystem.hpp
// Laedt SkinComponent::bonePalette als StructuredBuffer auf die GPU.
//
// Pro Entity mit SkinComponent wird ein persistenter StructuredBuffer<Mat4>
// verwaltet. Bei geaenderter Palette wird der Buffer per UploadBufferData
// aktualisiert. Der konkrete Shader-Slot wird vom Render-Feature konfiguriert.
// =============================================================================
#include "addons/animation/AnimationComponents.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/IDevice.hpp"

#include <unordered_map>

namespace engine::addons::animation {

class SkinningSystem
{
public:
    explicit SkinningSystem(renderer::IDevice& device) noexcept
        : m_device(device)
    {}
    ~SkinningSystem();

    // Einmal pro Frame aufrufen — nach AnimationSystem::Update(), vor RenderFrame().
    // Laedt geaenderte Bone-Paletten auf die GPU hoch.
    void Upload(ecs::World& world);

    // Gibt den GPU-Buffer fuer eine Entity zurueck (ungueltig wenn keine Palette).
    [[nodiscard]] BufferHandle GetBoneBuffer(EntityID entity) const noexcept;

    // Raeumt GPU-Buffer fuer nicht mehr existierende Entities auf.
    void GarbageCollect(const ecs::World& world);

private:
    struct BoneBufferEntry
    {
        BufferHandle handle;
        uint32_t     boneCount = 0u;
    };

    renderer::IDevice&                          m_device;
    std::unordered_map<EntityID, BoneBufferEntry> m_buffers;

    void DestroyEntry(BoneBufferEntry& entry) noexcept;
};

} // namespace engine::addons::animation
