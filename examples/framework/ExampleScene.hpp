#pragma once

#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "scene/TransformSystem.hpp"

namespace engine::examples {

struct ExampleSceneContext
{
    assets::AssetRegistry& assetRegistry;
    assets::AssetPipeline& assetPipeline;
    renderer::PlatformRenderLoop& renderLoop;
    renderer::MaterialSystem& materialSystem;
    ecs::World& world;
    TransformSystem& transformSystem;
    uint32_t debugFlags = 0u;  // Szene schreibt hier; App liest nach Update() in RenderView
};

class IExampleScene
{
public:
    virtual ~IExampleScene() = default;

    [[nodiscard]] virtual bool Build(ExampleSceneContext& context) = 0;
    [[nodiscard]] virtual bool Update(ExampleSceneContext& context, float deltaSeconds) = 0;
};

} // namespace engine::examples
