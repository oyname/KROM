#include "app/editor/EditorScene.hpp"

#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "ecs/Components.hpp"

namespace engine::app {

bool EditorScene::Build(AppSceneContext& context)
{
    // Keine Default-Kamera mehr — der User erstellt seine eigene im Editor.
    // Der Editor rendert über EditorState.editorCamera (Fly-Camera), nicht über
    // eine Szene-Kamera.
    m_cameraEntity = NULL_ENTITY;

    const EntityID light = context.world.CreateEntity();
    context.world.Add<NameComponent>(light, NameComponent{"Sun"});
    auto& lt = context.world.Add<TransformComponent>(light);
    lt.localPosition = { 5.f, 8.f, 5.f };
    lt.localScale    = { 1.f, 1.f, 1.f };
    lt.SetEulerDeg(-50.f, -30.f, 0.f);
    lt.dirty = true;
    context.world.Add<WorldTransformComponent>(light);

    LightComponent lc{};
    lc.type                      = LightType::Directional;
    lc.color                     = { 1.f, 0.98f, 0.95f };
    lc.intensity                 = 3.f;
    lc.castShadows               = true;
    lc.shadowSettings.enabled    = true;
    lc.shadowSettings.resolution = 2048u;
    lc.shadowSettings.bias       = 0.0015f;
    lc.shadowSettings.normalBias = 0.001f;
    lc.shadowSettings.maxDistance = 100.f;
    context.world.Add<LightComponent>(light, lc);

    return true;
}

bool EditorScene::Update(AppSceneContext& context, float deltaSeconds)
{
    (void)context;
    (void)deltaSeconds;
    // Die Spiel-Kamera wird hier nicht bewegt.
    // Steuerung der Editor-Flugkamera erfolgt ueber EditorState.editorCamera (Rechts-Maus + WASD).
    return true;
}

} // namespace engine::app
