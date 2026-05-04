#pragma once

#include "ExampleScene.hpp"
#include "PbrMasterMaterial.hpp"
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

    EntityID m_cubeEntity   = NULL_ENTITY;
    EntityID m_sphereEntity = NULL_ENTITY;
    EntityID m_diamondEntity = NULL_ENTITY;
    EntityID m_floorEntity  = NULL_ENTITY;
    EntityID m_cameraEntity = NULL_ENTITY;
    MeshHandle m_cubeMeshHandle = MeshHandle::Invalid();
    Vec3  m_cameraPos = { 0.f, 1.4f, 4.6f };
    float m_kMoveSpeed = 5.0f;
    float m_rotationYawDeg   = 0.0f;
    float m_rotationPitchDeg = 12.0f;
    float m_cameraYawDeg     = 0.0f;
    float m_cameraPitchDeg   = -12.0f;
    renderer::EnvironmentHandle m_environmentHandle = renderer::EnvironmentHandle::Invalid();
    MaterialHandle m_cubeMaterial  = MaterialHandle::Invalid();
    MaterialHandle m_floorMaterial = MaterialHandle::Invalid();
    MaterialHandle m_tonemapMaterial = MaterialHandle::Invalid();
    std::vector<EntityID> m_stressEntities;
    bool     m_stressModeActive      = false;
    bool     m_stressEntitiesCreated = false;
    bool     m_stressStatsEnabled    = false;
    float    m_stressStatsAccumulator = 0.0f;
    uint32_t m_debugFlags            = 0u;
};

} // namespace engine::examples
