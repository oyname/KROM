#include "addons/runtime/EngineAddonAdapters.hpp"

#include "addons/camera/CameraSerialization.hpp"
#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "core/AddonContext.hpp"
#include "core/Logger.hpp"
#include "renderer/RenderSystem.hpp"
#include "ForwardFeature.hpp"

#include <memory>
#include <utility>

namespace engine {

namespace {

bool RegisterRenderFeature(AddonContext& ctx,
                           const char* addonName,
                           std::unique_ptr<renderer::IEngineFeature> feature)
{
    auto* renderSystem = ctx.Services.TryGet<renderer::RenderSystem>();
    if (renderSystem == nullptr)
    {
        ctx.Logger.Error("Addon adapter requires renderer::RenderSystem service");
        return false;
    }

    if (!renderSystem->RegisterFeature(std::move(feature)))
    {
        (void)addonName;
        ctx.Logger.Error("Addon adapter failed to register render feature");
        return false;
    }

    return true;
}

class CameraAddon final : public IEngineAddon
{
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Camera"; }

    bool Register(AddonContext& ctx) override
    {
        if (!ctx.Services.Has<ecs::ComponentMetaRegistry>())
        {
            ctx.Logger.Error("CameraAddon requires ecs::ComponentMetaRegistry service");
            return false;
        }
        addons::camera::RegisterCameraAddon(
            ctx.Services.TryGet<ecs::ComponentMetaRegistry>(),
            ctx.Services.TryGet<serialization::SceneSerializer>(),
            ctx.Services.TryGet<serialization::SceneDeserializer>());
        return true;
    }

    void Unregister(AddonContext& ctx) override
    {
        addons::camera::UnregisterCameraAddon(
            ctx.Services.TryGet<ecs::ComponentMetaRegistry>(),
            ctx.Services.TryGet<serialization::SceneSerializer>(),
            ctx.Services.TryGet<serialization::SceneDeserializer>());
    }
};

class MeshRendererAddon final : public IEngineAddon
{
public:
    [[nodiscard]] const char* Name() const noexcept override { return "MeshRenderer"; }

    bool Register(AddonContext& ctx) override
    {
        addons::mesh_renderer::RegisterMeshRendererAddon(
            ctx.Services.TryGet<ecs::ComponentMetaRegistry>(),
            ctx.Services.TryGet<serialization::SceneSerializer>(),
            ctx.Services.TryGet<serialization::SceneDeserializer>());
        return RegisterRenderFeature(ctx, Name(), addons::mesh_renderer::CreateMeshRendererFeature());
    }

    void Unregister(AddonContext& ctx) override
    {
        addons::mesh_renderer::UnregisterMeshRendererAddon(
            ctx.Services.TryGet<ecs::ComponentMetaRegistry>(),
            ctx.Services.TryGet<serialization::SceneSerializer>(),
            ctx.Services.TryGet<serialization::SceneDeserializer>());
    }
};

class LightingAddon final : public IEngineAddon
{
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Lighting"; }

    bool Register(AddonContext& ctx) override
    {
        addons::lighting::RegisterLightingAddon(
            ctx.Services.TryGet<ecs::ComponentMetaRegistry>(),
            ctx.Services.TryGet<serialization::SceneSerializer>(),
            ctx.Services.TryGet<serialization::SceneDeserializer>());
        return RegisterRenderFeature(ctx, Name(), addons::lighting::CreateLightingFeature());
    }

    void Unregister(AddonContext& ctx) override
    {
        addons::lighting::UnregisterLightingAddon(
            ctx.Services.TryGet<ecs::ComponentMetaRegistry>(),
            ctx.Services.TryGet<serialization::SceneSerializer>(),
            ctx.Services.TryGet<serialization::SceneDeserializer>());
    }
};

class ShadowAddon final : public IEngineAddon
{
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Shadow"; }

    bool Register(AddonContext& ctx) override
    {
        return RegisterRenderFeature(ctx, Name(), addons::shadow::CreateShadowFeature());
    }

    void Unregister(AddonContext& ctx) override
    {
        (void)ctx;
    }
};

class ForwardAddon final : public IEngineAddon
{
public:
    ForwardAddon(std::array<float, 4> clearColorValue, bool enableEnvironmentBackground) noexcept
    {
        m_config.clearColorValue = clearColorValue;
        m_config.enableEnvironmentBackground = enableEnvironmentBackground;
    }

    [[nodiscard]] const char* Name() const noexcept override { return "Forward"; }

    bool Register(AddonContext& ctx) override
    {
        return RegisterRenderFeature(ctx, Name(), renderer::addons::forward::CreateForwardFeature(m_config));
    }

    void Unregister(AddonContext& ctx) override
    {
        (void)ctx;
    }

private:
    renderer::addons::forward::ForwardFeatureConfig m_config{};
};

} // namespace

std::unique_ptr<IEngineAddon> CreateCameraAddon()
{
    return std::make_unique<CameraAddon>();
}

std::unique_ptr<IEngineAddon> CreateLightingAddon()
{
    return std::make_unique<LightingAddon>();
}

std::unique_ptr<IEngineAddon> CreateMeshRendererAddon()
{
    return std::make_unique<MeshRendererAddon>();
}

std::unique_ptr<IEngineAddon> CreateShadowAddon()
{
    return std::make_unique<ShadowAddon>();
}

std::unique_ptr<IEngineAddon> CreateForwardAddon(std::array<float, 4> clearColorValue,
                                                 bool enableEnvironmentBackground)
{
    return std::make_unique<ForwardAddon>(clearColorValue, enableEnvironmentBackground);
}

} // namespace engine
