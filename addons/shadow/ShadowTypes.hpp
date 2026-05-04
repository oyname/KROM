#pragma once
// =============================================================================
// KROM Engine - addons/shadow/ShadowTypes.hpp
// Gemeinsame, backend-neutrale Shadow-Datenstrukturen.
// Render-/Backend-spezifische Umsetzung lebt bewusst außerhalb dieses Headers.
// =============================================================================
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/ShadowTypes.hpp"
#include "collision/SceneQueries.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::addons::shadow {

struct ShadowViewport
{
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
};

struct ShadowScissorRect
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

enum class ShadowTechnique : uint8_t
{
    None = 0,
    ShadowMap2D,
    ShadowMapCube,
    CascadedShadowMap,
};

enum class ShadowMapDimension : uint8_t
{
    Tex2D = 0,
    Tex2DArray,
    Cube,
};

struct ShadowMapDesc
{
    ShadowMapDimension dimension = ShadowMapDimension::Tex2D;
    uint32_t width = 1024u;
    uint32_t height = 1024u;
    uint32_t layers = 1u;
    bool comparisonSampling = true;
};

struct ShadowMapHandle
{
    uint32_t index = 0u;
    bool valid = false;
};

struct ShadowAllocation
{
    ShadowMapHandle handle{};
    ShadowMapDesc desc{};
};

struct ShadowView
{
    math::Mat4 view = math::Mat4::Identity();
    math::Mat4 proj = math::Mat4::Identity();
    math::Mat4 viewProj = math::Mat4::Identity();

    uint32_t arrayLayer = 0u;
    uint32_t faceIndex = 0u;
    uint32_t cascadeIndex = 0u;

    float nearPlane = 0.1f;
    float farPlane = 100.f;

    ShadowViewport viewport{};
    ShadowScissorRect scissor{};
};

using ShadowLightID = uint32_t;

struct ShadowRequest
{
    ShadowLightID id = 0u;
    EntityID lightEntity = NULL_ENTITY;
    uint32_t visibleLightIndex = UINT32_MAX;

    LightType lightType = LightType::Point;
    ShadowTechnique technique = ShadowTechnique::None;

    ShadowSettings settings{};
    ShadowAllocation allocation{};
    std::vector<ShadowView> views;

    collision::AABB casterBoundsWorld{};
    collision::AABB receiverBoundsWorld{};
    bool cacheable = false;
    bool needsUpdate = true;
};

[[nodiscard]] inline ShadowTechnique ChooseShadowTechnique(const LightComponent& light) noexcept
{
    if (!light.castShadows || !light.shadowSettings.enabled)
        return ShadowTechnique::None;

    switch (light.type)
    {
    case LightType::Directional: return ShadowTechnique::ShadowMap2D;
    case LightType::Spot:        return ShadowTechnique::ShadowMap2D;
    case LightType::Point:       return ShadowTechnique::ShadowMapCube;
    default:                     return ShadowTechnique::None;
    }
}

[[nodiscard]] inline ShadowMapDesc BuildShadowMapDesc(const ShadowRequest& request) noexcept
{
    ShadowMapDesc desc{};
    desc.width = std::max(1u, request.settings.resolution);
    desc.height = std::max(1u, request.settings.resolution);
    desc.comparisonSampling = true;

    switch (request.technique)
    {
    case ShadowTechnique::ShadowMapCube:
        desc.dimension = ShadowMapDimension::Cube;
        desc.layers = 6u;
        break;
    case ShadowTechnique::CascadedShadowMap:
        desc.dimension = ShadowMapDimension::Tex2DArray;
        desc.layers = static_cast<uint32_t>(std::max<size_t>(1u, request.views.size()));
        break;
    case ShadowTechnique::ShadowMap2D:
    default:
        desc.dimension = ShadowMapDimension::Tex2D;
        desc.layers = 1u;
        break;
    }

    return desc;
}

} // namespace engine::addons::shadow
