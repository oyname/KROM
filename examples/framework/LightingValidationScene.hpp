#pragma once

#include "ExampleScene.hpp"
#include "PbrMasterMaterial.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "core/Types.hpp"
#include <vector>

namespace engine::examples {

class LightingValidationScene final : public IExampleScene
{
public:
    [[nodiscard]] bool Build(ExampleSceneContext& context) override;
    [[nodiscard]] bool Update(ExampleSceneContext& context, float deltaSeconds) override;

private:
    struct ShadowRigConfig
    {
        bool enabled = true;
        ShadowType type = ShadowType::PCF;
        ShadowFilter filter = ShadowFilter::PCF3x3;
        uint32_t resolution = 1024u;
        float bias = 0.001f;
        float normalBias = 0.003f;
        float maxDistance = 10.0f;
        float strength = 1.0f;
    };

    struct LightPoseConfig
    {
        Vec3 position{};
        Vec3 eulerDeg{};
    };

    struct LightRigEntryConfig
    {
        LightType type = LightType::Point;
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range = 10.0f;
        float spotInnerDeg = 15.0f;
        float spotOuterDeg = 30.0f;
        bool castShadows = true;
        bool activeByDefault = false;
        LightPoseConfig spawnPose{};
        LightPoseConfig runtimePose{};
        bool runtimeAnimated = false;
        float orbitRadiusX = 0.0f;
        float orbitHeight = 0.0f;
        float orbitCenterZ = 0.0f;
        float orbitRadiusZ = 0.0f;
        ShadowRigConfig shadow{};
    };

    struct LightRigConfig
    {
        float orbitSpeedDeg = 18.0f;
        LightRigEntryConfig directional{};
        LightRigEntryConfig point{};
        LightRigEntryConfig spot{};
    };

    enum class SceneMode : uint8_t
    {
        BrdfGrid = 0,
        LightTypes,
        NormalResponse,
    };

    enum class LightMode : uint8_t
    {
        Directional = 0,
        Point,
        Spot,
        All,
    };

    [[nodiscard]] MeshHandle CreateSphereMesh(assets::AssetRegistry& registry) const;
    [[nodiscard]] MeshHandle CreatePlaneMesh(assets::AssetRegistry& registry) const;
    [[nodiscard]] renderer::VertexLayout CreatePbrVertexLayout() const;
    [[nodiscard]] static LightRigConfig BuildDefaultLightRigConfig();
    [[nodiscard]] bool CreateMaterials(ExampleSceneContext& context,
                                       const renderer::VertexLayout& vertexLayout);
    void ApplyLightConfig(ExampleSceneContext& context,
                          EntityID entity,
                          const LightRigEntryConfig& config) const;

    void BuildBrdfGrid(ExampleSceneContext& context, MeshHandle sphereMesh, MeshHandle planeMesh);
    void BuildLightTypeTest(ExampleSceneContext& context, MeshHandle sphereMesh, MeshHandle planeMesh);
    void BuildNormalResponseTest(ExampleSceneContext& context, MeshHandle sphereMesh, MeshHandle planeMesh);
    void BuildLights(ExampleSceneContext& context);
    void BuildCamera(ExampleSceneContext& context);

    void SetSceneMode(SceneMode mode, ExampleSceneContext& context);
    void SetLightMode(LightMode mode, ExampleSceneContext& context);
    void ApplyCameraPreset();
    void UpdateLightRig(ExampleSceneContext& context, float deltaSeconds);
    void SetEntitiesActive(ExampleSceneContext& context,
                           const std::vector<EntityID>& entities,
                           bool active) const;
    void EnsureActiveComponent(ExampleSceneContext& context, EntityID entity, bool active) const;
    [[nodiscard]] EntityID CreateRenderableEntity(ExampleSceneContext& context,
                                                  MeshHandle mesh,
                                                  MaterialHandle material,
                                                  const Vec3& position,
                                                  const Vec3& scale,
                                                  const Vec3& boundsExtents,
                                                  float boundingSphere,
                                                  const Quat& rotation = Quat::Identity()) const;
    [[nodiscard]] MaterialHandle BuildGridMaterial(uint32_t row,
                                                   uint32_t col,
                                                   uint32_t rowCount,
                                                   uint32_t colCount);

    renderer::pbr::PbrMasterMaterial m_masterMaterial{};
    std::vector<MaterialHandle> m_gridMaterials;
    MaterialHandle m_groundMaterial = MaterialHandle::Invalid();
    MaterialHandle m_whiteDielectricMaterial = MaterialHandle::Invalid();
    MaterialHandle m_copperMetalMaterial = MaterialHandle::Invalid();
    MaterialHandle m_normalPlaneMaterial = MaterialHandle::Invalid();
    MaterialHandle m_normalSphereMaterial = MaterialHandle::Invalid();

    std::vector<EntityID> m_brdfGridEntities;
    std::vector<EntityID> m_lightTypeEntities;
    std::vector<EntityID> m_normalResponseEntities;

    EntityID m_directionalLightEntity = NULL_ENTITY;
    EntityID m_pointLightEntity       = NULL_ENTITY;
    EntityID m_spotLightEntity        = NULL_ENTITY;
    EntityID m_cameraEntity           = NULL_ENTITY;

    SceneMode m_sceneMode = SceneMode::BrdfGrid;
    LightMode m_lightMode = LightMode::Directional;
    uint32_t  m_debugFlags = renderer::DBG_DISABLE_IBL | renderer::DBG_DISABLE_AO;

    Vec3  m_cameraPos      = { 0.0f, 2.6f, 10.5f };
    float m_cameraYawDeg   = 0.0f;
    float m_cameraPitchDeg = -10.0f;
    float m_lightOrbitDeg  = 0.0f;
    LightRigConfig m_lightRigConfig = BuildDefaultLightRigConfig();
};

} // namespace engine::examples
