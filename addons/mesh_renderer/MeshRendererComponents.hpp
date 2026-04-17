#pragma once

#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"
#include <cstdint>

namespace engine {

struct MeshComponent
{
    MeshHandle mesh;
    bool       castShadows    = true;
    bool       receiveShadows = true;
    uint32_t   layerMask      = 0xFFFFFFFFu;

    MeshComponent() = default;
    explicit MeshComponent(MeshHandle m) : mesh(m) {}
};

struct MaterialComponent
{
    MaterialHandle material;
    uint32_t       submeshIndex = 0u;

    MaterialComponent() = default;
    explicit MaterialComponent(MaterialHandle m) : material(m) {}
};

inline void RegisterMeshRendererComponents()
{
    using namespace ecs;
    RegisterComponent<MeshComponent>("MeshComponent");
    RegisterComponent<MaterialComponent>("MaterialComponent");
}

} // namespace engine
