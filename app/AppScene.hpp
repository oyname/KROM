#pragma once

#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "scene/TransformSystem.hpp"

namespace engine::app {

struct AppSceneContext
{
    assets::AssetRegistry& assetRegistry;
    assets::AssetPipeline& assetPipeline;
    renderer::PlatformRenderLoop& renderLoop;
    renderer::MaterialSystem& materialSystem;
    ecs::World& world;
    TransformSystem& transformSystem;
    bool forwardPlusEnabled = false;
    uint32_t debugFlags = 0u;  // Szene schreibt hier; App liest nach Update() in RenderView
};

class IAppScene
{
public:
    virtual ~IAppScene() = default;

    [[nodiscard]] virtual bool Build(AppSceneContext& context) = 0;
    [[nodiscard]] virtual bool Update(AppSceneContext& context, float deltaSeconds) = 0;
};

} // namespace engine::app
