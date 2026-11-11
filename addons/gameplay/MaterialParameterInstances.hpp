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
#include <cmath>
#include <vector>

namespace engine::gameplay {

class MaterialParameterInstances
{
public:
    bool Set(renderer::MaterialSystem& materials, ecs::World& world, EntityID entity,
             std::string_view name, const math::Vec3& value)
    {
        return EditMaterials(materials, world, entity, [&](MaterialHandle& handle) {
            const auto* parameter = materials.FindParameterAlias(handle, name);
            if (!parameter)
                return false;
            const std::string target = *parameter; // The alias map may move when a copy is made.
            return EditInstance(materials, entity, handle, [&](MaterialHandle instance) {
                materials.SetVec4(instance, target, {value.x, value.y, value.z, 0.f});
            });
        });
    }

    bool SetUV(renderer::MaterialSystem& materials, ecs::World& world, EntityID entity,
               const math::Vec2& scale, const math::Vec2& offset)
    {
        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) ||
            !std::isfinite(offset.x) || !std::isfinite(offset.y))
            return false;
        return EditMaterials(materials, world, entity, [&](MaterialHandle& handle) {
            const auto& layout = materials.GetCBLayout(handle);
            const auto hasVec2 = [&](std::string_view name) {
                return std::any_of(layout.fields.begin(), layout.fields.end(), [&](const renderer::CbFieldDesc& field) {
                    return field.name == name && field.type == renderer::MaterialValueType::Vec2;
                });
            };
            if (!hasVec2("uvScale") || !hasVec2("uvOffset"))
                return false;
            return EditInstance(materials, entity, handle, [&](MaterialHandle instance) {
                materials.SetVec2(instance, "uvScale", scale);
                materials.SetVec2(instance, "uvOffset", offset);
            });
        });
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

    bool EditMaterials(renderer::MaterialSystem& materials, ecs::World& world, EntityID entity, auto&& edit)
    {
        m_materials = &materials;
        auto* component = world.Get<MaterialComponent>(entity);
        if (!component)
            return false;
        bool applied = component->material.IsValid() && edit(component->material);
        for (auto& slot : component->slotOverrides)
            if (slot.material.IsValid())
                applied = edit(slot.material) || applied;
        return applied;
    }

    bool EditInstance(renderer::MaterialSystem& materials, EntityID entity, MaterialHandle& handle, auto&& edit)
    {
        const MaterialHandle instance = InstanceFor(materials, entity, handle);
        if (!instance.IsValid())
            return false;
        edit(instance);
        materials.MarkDirty(instance);
        handle = instance;
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

    bool SetUV(EntityID entity, const math::Vec2& scale, const math::Vec2& offset) override
    {
        return m_instances.SetUV(m_materials, m_world, entity, scale, offset);
    }

private:
    MaterialParameterInstances& m_instances;
    renderer::MaterialSystem&   m_materials;
    ecs::World&                 m_world;
};

} // namespace engine::gameplay
