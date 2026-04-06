#include "ForwardFeature.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/GLFWPlatform.hpp"
#include "platform/NullPlatform.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"

using namespace engine;

int main()
{
#if KROM_HAS_GLFW
    Debug::MinLevel = LogLevel::Info;
    RegisterAllComponents();

    platform::GLFWPlatform platform;
    if (!platform.Initialize())
        return -1;

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()))
    {
        Debug::LogError("forward feature registration failed");
        return 1;
    }

    platform::WindowDesc desc{};
    desc.title = "KROM GLFW Render Loop";
    desc.width = 1280;
    desc.height = 720;
    desc.resizable = true;

    renderer::IDevice::DeviceDesc dd{};
    dd.enableDebugLayer = true;
    dd.appName = "KROM GLFW Runtime";

    if (!loop.Initialize(renderer::DeviceFactory::BackendType::Null, platform, desc, &bus, dd))
    {
        platform.Shutdown();
        return -2;
    }

    ecs::World world;
    renderer::MaterialSystem materials;

    renderer::MaterialDesc matDesc{};
    matDesc.name = "Default";
    matDesc.passTag = renderer::RenderPassTag::Opaque;
    matDesc.vertexShader = renderer::ShaderHandle::Make(1u, 1u);
    matDesc.fragmentShader = renderer::ShaderHandle::Make(2u, 1u);
    const auto material = materials.RegisterMaterial(std::move(matDesc));

    const auto entity = world.CreateEntity();
    world.Add<ecs::TransformComponent>(entity);
    world.Add<ecs::WorldTransformComponent>(entity);
    world.Add<ecs::MeshComponent>(entity, renderer::MeshHandle::Make(1u, 1u));
    world.Add<ecs::MaterialComponent>(entity, material);
    world.Add<ecs::BoundsComponent>(entity, ecs::BoundsComponent{
        .centerWorld={0.f, 0.f, 0.f},
        .extentsWorld={0.5f, 0.5f, 0.5f},
        .boundingSphere=0.87f});

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH({0.f, 0.f, -5.f}, {0.f, 0.f, 0.f}, math::Vec3::Up());
    view.projection = math::Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f / 9.f, 0.1f, 100.f);
    view.cameraPosition = {0.f, 0.f, -5.f};
    view.cameraForward = {0.f, 0.f, 1.f};

    platform::StdTiming timing;
    while (!loop.ShouldExit())
    {
        if (auto* input = loop.GetInput(); input && input->IsKeyPressed(platform::Key::Escape))
            loop.GetWindow()->RequestClose();

        loop.Tick(world, materials, view, timing);
    }

    loop.Shutdown();
    platform.Shutdown();
    return 0;
#else
    return 0;
#endif
}
