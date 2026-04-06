#pragma once
// =============================================================================
// KROM Engine - renderer/SceneSnapshot.hpp
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "renderer/RendererTypes.hpp"
#include "ecs/Components.hpp"   // LightType, LightComponent::type
#include <string>
#include <string_view>
#include <vector>

namespace engine::renderer {

using math::Mat4;
using math::Vec3;

struct RenderableEntry
{
    EntityID       entity        = {};
    MeshHandle     mesh          = {};
    MaterialHandle material      = {};
    uint32_t       submeshIndex  = 0u;
    Mat4           worldMatrix     = Mat4::Identity();
    Mat4           worldMatrixInvT = Mat4::Identity();
    Vec3           boundsCenter    = Vec3(0.f, 0.f, 0.f);
    Vec3           boundsExtents   = Vec3(1.f, 1.f, 1.f);
    float          boundsRadius    = 1.f;
    uint32_t       layerMask       = 0xFFFFFFFFu;
    bool           castShadows     = true;
    bool           receiveShadows  = true;
};

struct LightEntry
{
    EntityID   entity                   = {};
    LightType  lightType                = LightType::Point;
    Vec3       positionWorld            = Vec3(0.f, 0.f, 0.f);
    Vec3       directionWorld           = Vec3(0.f, -1.f, 0.f);
    Vec3       color                    = Vec3(1.f, 1.f, 1.f);
    float      intensity                = 1.f;
    float      range                    = 10.f;
    float      spotInnerDeg             = 15.f;
    float      spotOuterDeg             = 30.f;
    bool       castShadows              = false;
};

struct SceneSnapshotContribution
{
    std::string stepName;
    size_t renderableOffset = 0u;
    size_t renderableCount = 0u;
    size_t lightOffset = 0u;
    size_t lightCount = 0u;
};

struct SceneSnapshot
{
    std::vector<RenderableEntry> renderables;
    std::vector<LightEntry>      lights;
    std::vector<SceneSnapshotContribution> contributions;

    void Clear()
    {
        renderables.clear();
        lights.clear();
        contributions.clear();
    }

    void RecordContribution(std::string_view stepName,
                            size_t renderableOffset,
                            size_t lightOffset,
                            size_t renderableCount,
                            size_t lightCount)
    {
        contributions.push_back(SceneSnapshotContribution{
            std::string(stepName), renderableOffset, renderableCount, lightOffset, lightCount});
    }

    [[nodiscard]] bool IsEmpty() const noexcept { return renderables.empty() && lights.empty(); }
};

} // namespace engine::renderer
