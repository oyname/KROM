#include "addons/editor/EditorFeature.hpp"

#include <algorithm>
#include <cmath>

namespace engine::renderer::addons::editor {

bool BuildEditorCameraRenderView(
    const EditorState& state,
    uint32_t           viewportWidth,
    uint32_t           viewportHeight,
    renderer::RenderView& outView,
    math::Vec3         ambientColor,
    float              ambientIntensity) noexcept
{
    const EditorCameraState& cam = state.editorCamera;

    const float aspect = viewportHeight > 0u
        ? static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight)
        : 16.f / 9.f;

    const float fovY = cam.fovDeg    * math::DEG_TO_RAD;
    const float near = cam.nearPlane;
    const float far  = cam.farPlane;

    const math::Quat rot         = math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f);
    const math::Mat4 worldMatrix = math::Mat4::TRS(cam.position, rot, {1.f, 1.f, 1.f});

    outView                  = renderer::RenderView{};
    outView.view             = worldMatrix.InverseAffine();
    outView.projection       = math::Mat4::PerspectiveFovRH(fovY, aspect, near, far);
    outView.cameraPosition   = cam.position;
    outView.cameraForward    = worldMatrix.TransformDirection({0.f, 0.f, -1.f}).Normalized();
    outView.ambientColor     = ambientColor;
    outView.ambientIntensity = ambientIntensity;
    outView.nearPlane        = near;
    outView.farPlane         = far;
    outView.backgroundMode = state.showSkyboxBackground
        ? BackgroundMode::Skybox
        : BackgroundMode::ClearColor;
    outView.clearColor     = state.backgroundColor;
    return true;
}

} // namespace engine::renderer::addons::editor
