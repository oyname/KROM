#pragma once
// =============================================================================
// KROM Engine - addons/gameplay/MaterialParameterInstances.hpp
// Per-entity material instances for script parameters. The first write gives an
// entity its own copy of the shared material; later writes reuse that copy.
// =============================================================================
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/script/IMaterialService.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"
#include <algorithm>
#include <vector>

namespace engine::gameplay {

class MaterialParameterInstances
{
public:
    bool Set(renderer::MaterialSystem& materials, ecs::World& world, EntityID entity,
             std::string_view name, const math::Vec3& value)
    {
        m_materials = &materials;
        auto* component = world.Get<MaterialComponent>(entity);
        if (!component)
            return false;

        bool applied = Apply(materials, entity, component->material, name, value);
        for (auto& slot : component->slotOverrides)
            applied = Apply(materials, entity, slot.material, name, value) || applied;
        return applied;
    }

    // Frees the copies of destroyed entities.
    void CollectDestroyed(const ecs::World& world)
    {
        if (!m_materials)
            return;
        std::erase_if(m_entries, [&](const Entry& entry) {
            if (world.IsAlive(entry.entity))
                return false;
            m_materials->DestroyMaterial(entry.instance);
            return true;
        });
    }

    void Release()
    {
        if (m_materials)
            for (const Entry& entry : m_entries)
                m_materials->DestroyMaterial(entry.instance);
        m_entries.clear();
    }

    [[nodiscard]] size_t Count() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        EntityID       entity;
        MaterialHandle base;
        MaterialHandle instance;
    };

    bool Apply(renderer::MaterialSystem& materials, EntityID entity, MaterialHandle& handle,
               std::string_view name, const math::Vec3& value)
    {
        if (!handle.IsValid())
            return false;
        const std::string* parameter = materials.FindParameterAlias(handle, name);
        if (!parameter)
            return false;
        const std::string target = *parameter; // The alias map may move when a copy is made.
        handle = InstanceFor(materials, entity, handle);
        if (!handle.IsValid())
            return false;
        materials.SetVec4(handle, target, {value.x, value.y, value.z, 0.f});
        materials.MarkDirty(handle);
        return true;
    }

    MaterialHandle InstanceFor(renderer::MaterialSystem& materials, EntityID entity, MaterialHandle handle)
    {
        const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry) {
            return entry.entity == entity && (entry.instance == handle || entry.base == handle);
        });
        if (it != m_entries.end())
            return it->instance;

        const MaterialHandle instance = materials.CreateInstance(handle);
        const auto* source = materials.GetInstance(handle);
        auto* copy = materials.GetInstance(instance);
        if (!source || !copy)
            return MaterialHandle::Invalid();
        // CreateInstance starts from the defaults; keep the values of the shared material.
        copy->parameters = source->parameters;
        copy->parameterOverrides = source->parameterOverrides;
        copy->featureOverrides = source->featureOverrides;
        materials.MarkDirty(instance);
        m_entries.push_back({entity, handle, instance});
        return instance;
    }

    renderer::MaterialSystem* m_materials = nullptr;
    std::vector<Entry>        m_entries;
};

class MaterialScriptService final : public script::IMaterialService
{
public:
    MaterialScriptService(MaterialParameterInstances& instances, renderer::MaterialSystem& materials, ecs::World& world)
        : m_instances(instances), m_materials(materials), m_world(world) {}

    bool SetParameter(EntityID entity, std::string_view name, const math::Vec3& value) override
    {
        return m_instances.Set(m_materials, m_world, entity, name, value);
    }

private:
    MaterialParameterInstances& m_instances;
    renderer::MaterialSystem&   m_materials;
    ecs::World&                 m_world;
};

} // namespace engine::gameplay
