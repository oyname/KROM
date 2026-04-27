#pragma once

#include "ExampleScene.hpp"
#include "core/Types.hpp"
#include "renderer/Environment.hpp"
#include <vector>

namespace engine::examples {

class PbrShadowScene final : public IExampleScene
{
public:
    [[nodiscard]] bool Build(ExampleSceneContext& context) override;
    [[nodiscard]] bool Update(ExampleSceneContext& context, float deltaSeconds) override;

private:
    enum class DebugViewMode : uint8_t
    {
        Full = 0,
        NoNormal,
        NoIBL,
        NoNormalNoIBL,
        Prefilter,
        SpecularIBL,
        DirectSpecular,
        FullRawResolve,
        ShadowVisibility,
        DiffuseIBL,
    };

    [[nodiscard]] MeshHandle CreateCubeMesh(assets::AssetRegistry& registry) const;
    [[nodiscard]] MeshHandle CreateSphereMesh(assets::AssetRegistry& registry) const;
    [[nodiscard]] MeshHandle CreateDiamondMesh(assets::AssetRegistry& registry) const;
    [[nodiscard]] MeshHandle CreateFloorMesh(assets::AssetRegistry& registry) const;
    [[nodiscard]] renderer::VertexLayout CreatePbrVertexLayout() const;
    [[nodiscard]] bool CreateEnvironment(ExampleSceneContext& context) const;
    [[nodiscard]] bool CreateMaterials(ExampleSceneContext& context,
                                       const renderer::VertexLayout& vertexLayout);
    void CreateSceneEntities(ExampleSceneContext& context,
                             MeshHandle cubeMesh,
                             MeshHandle sphereMesh,
                             MeshHandle diamondMesh,
                             MeshHandle floorMesh);
    void CreateStressEntities(ExampleSceneContext& context, MeshHandle cubeMesh);
    void UpdateStressEntities(ExampleSceneContext& context, float deltaSeconds);
    void LogStressStats(ExampleSceneContext& context, float deltaSeconds);
    void ApplyDebugViewMode(ExampleSceneContext& context, DebugViewMode mode);

    EntityID m_cubeEntity = NULL_ENTITY;
    EntityID m_sphereEntity = NULL_ENTITY;
    EntityID m_diamondEntity = NULL_ENTITY;
    EntityID m_floorEntity = NULL_ENTITY;
    MeshHandle m_cubeMeshHandle = MeshHandle::Invalid();
    float m_rotationYawDeg = 0.0f;
    float m_rotationPitchDeg = 12.0f;
    renderer::EnvironmentHandle m_environmentHandle = renderer::EnvironmentHandle::Invalid();
    MaterialHandle m_cubeMaterialNormal = MaterialHandle::Invalid();
    MaterialHandle m_cubeMaterialFlat = MaterialHandle::Invalid();
    MaterialHandle m_cubeMaterialPrefilter = MaterialHandle::Invalid();
    MaterialHandle m_cubeMaterialSpecularIBL = MaterialHandle::Invalid();
    MaterialHandle m_cubeMaterialDirectSpecular = MaterialHandle::Invalid();
    MaterialHandle m_cubeMaterialShadowVisibility = MaterialHandle::Invalid();
    MaterialHandle m_cubeMaterialDiffuseIBL = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialNormal = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialFlat = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialPrefilter = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialSpecularIBL = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialDirectSpecular = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialShadowVisibility = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterialDiffuseIBL = MaterialHandle::Invalid();
    MaterialHandle m_tonemapMaterialDefault = MaterialHandle::Invalid();
    MaterialHandle m_tonemapMaterialRaw = MaterialHandle::Invalid();
    DebugViewMode m_debugViewMode = DebugViewMode::Full;
    std::vector<EntityID> m_stressEntities;
    bool m_stressModeActive = false;
    bool m_stressEntitiesCreated = false;
    bool m_stressStatsEnabled = false;
    float m_stressStatsAccumulator = 0.0f;
};

} // namespace engine::examples
