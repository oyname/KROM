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
#include "assets/AssetRegistry.hpp"

#include <unordered_map>

namespace engine::addons::animation {

class SkinningSystem
{
public:
    // registry: lets skins without a pose yet (edit mode, clip not started) upload a
    // bind-pose palette, so skinning shaders never read an unbound bone buffer.
    explicit SkinningSystem(renderer::IDevice& device,
                            const assets::AssetRegistry* registry = nullptr) noexcept
        : m_device(device)
        , m_registry(registry)
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
        // Last uploaded palette: unchanged poses (edit mode, pause, bind pose) are
        // not written again, which would wait for the GPU to finish the last draw.
        std::vector<math::Mat4> uploaded;
    };

    renderer::IDevice&                          m_device;
    const assets::AssetRegistry*                m_registry = nullptr;
    std::vector<math::Mat4>                     m_bindPose;
    std::unordered_map<EntityID, BoneBufferEntry> m_buffers;

    void DestroyEntry(BoneBufferEntry& entry) noexcept;
};

} // namespace engine::addons::animation
