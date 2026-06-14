// =============================================================================
// KROM Engine - addons/animation/SkinningSystem.cpp
// =============================================================================
#include "SkinningSystem.hpp"
#include "renderer/RendererTypes.hpp"
#include "core/Debug.hpp"

namespace engine::addons::animation {

SkinningSystem::~SkinningSystem()
{
    for (auto& [entity, entry] : m_buffers)
    {
        (void)entity;
        DestroyEntry(entry);
    }
}

void SkinningSystem::Upload(ecs::World& world)
{
    world.View<SkinComponent>(
        [&](EntityID entity, SkinComponent& skin)
        {
            if (skin.bonePalette.empty())
            {
                auto existing = m_buffers.find(entity);
                if (existing != m_buffers.end())
                {
                    DestroyEntry(existing->second);
                    m_buffers.erase(existing);
                }
                skin.gpuBuffer = BufferHandle{};
                return;
            }

            const uint32_t boneCount =
                static_cast<uint32_t>(skin.bonePalette.size());
            const size_t byteSize = boneCount * sizeof(math::Mat4);

            auto it = m_buffers.find(entity);

            // Buffer (neu) anlegen wenn er fehlt oder die Knochen-Anzahl sich geaendert hat
            if (it == m_buffers.end() || it->second.boneCount != boneCount)
            {
                if (it != m_buffers.end() && it->second.handle.IsValid())
                    m_device.DestroyBuffer(it->second.handle);

                renderer::BufferDesc desc{};
                desc.byteSize     = byteSize;
                desc.stride       = sizeof(math::Mat4);
                desc.type         = renderer::BufferType::Structured;
                desc.usage        = renderer::ResourceUsage::ShaderResource;
                desc.access       = renderer::MemoryAccess::CpuWrite;
                desc.initialState = renderer::ResourceState::ShaderRead;
                desc.debugName    = "Animation_BonePalette";

                const BufferHandle handle = m_device.CreateBuffer(desc);
                if (!handle.IsValid())
                {
                    Debug::LogError("SkinningSystem: CreateBuffer failed for entity %u",
                                    entity.value);
                    return;
                }

                m_buffers[entity] = { handle, boneCount };
                it = m_buffers.find(entity);
            }

            m_device.UploadBufferData(
                it->second.handle,
                skin.bonePalette.data(),
                byteSize);
            skin.gpuBuffer = it->second.handle;
        });
}

BufferHandle SkinningSystem::GetBoneBuffer(EntityID entity) const noexcept
{
    const auto it = m_buffers.find(entity);
    return (it != m_buffers.end()) ? it->second.handle : BufferHandle{};
}

void SkinningSystem::GarbageCollect(const ecs::World& world)
{
    for (auto it = m_buffers.begin(); it != m_buffers.end(); )
    {
        if (!world.IsAlive(it->first) || !world.Has<SkinComponent>(it->first))
        {
            DestroyEntry(it->second);
            it = m_buffers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void SkinningSystem::DestroyEntry(BoneBufferEntry& entry) noexcept
{
    if (entry.handle.IsValid())
        m_device.DestroyBuffer(entry.handle);
    entry.handle = BufferHandle::Invalid();
    entry.boneCount = 0u;
}

} // namespace engine::addons::animation
