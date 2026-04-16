#include "renderer/FrameConstantStage.hpp"
#include "renderer/Environment.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {
namespace {

void FillMatrixRowMajor(const math::Mat4& m, float out[16]) noexcept
{
    std::memcpy(out, m.Data(), sizeof(float) * 16u);
}

void PackLightData(const LightProxy& src, GpuLightData& dst) noexcept
{
    const bool isDirectional = (src.lightType == LightType::Directional);
    const bool isSpot = (src.lightType == LightType::Spot);

    if (isDirectional)
    {
        dst.positionWS[0] = src.directionWorld.x;
        dst.positionWS[1] = src.directionWorld.y;
        dst.positionWS[2] = src.directionWorld.z;
        dst.positionWS[3] = 0.f;
    }
    else
    {
        dst.positionWS[0] = src.positionWorld.x;
        dst.positionWS[1] = src.positionWorld.y;
        dst.positionWS[2] = src.positionWorld.z;
        dst.positionWS[3] = 1.f;
    }

    dst.directionWS[0] = src.directionWorld.x;
    dst.directionWS[1] = src.directionWorld.y;
    dst.directionWS[2] = src.directionWorld.z;
    dst.directionWS[3] = 0.f;

    dst.colorIntensity[0] = src.color.x;
    dst.colorIntensity[1] = src.color.y;
    dst.colorIntensity[2] = src.color.z;
    dst.colorIntensity[3] = src.intensity;

    dst.params[0] = isSpot ? src.spotInner : 1.f;
    dst.params[1] = isSpot ? src.spotOuter : 1.f;
    dst.params[2] = src.range;
    dst.params[3] = isDirectional ? 0.f : (isSpot ? 2.f : 1.f);
}

} // namespace

bool FrameConstantStage::PrepareFrameData(const FrameConstantStageContext& context,
                                          FrameConstantsResult& result) const
{
    result.projectionForBackend = context.clipSpaceAdjustment * context.view.projection;

    result.viewProjForBackend = result.projectionForBackend * context.view.view;

    FrameConstants fc{};
    FillMatrixRowMajor(context.view.view, fc.viewMatrix);
    FillMatrixRowMajor(result.projectionForBackend, fc.projMatrix);
    FillMatrixRowMajor(result.viewProjForBackend, fc.viewProjMatrix);
    FillMatrixRowMajor(result.viewProjForBackend.Inverse(), fc.invViewProjMatrix);

    fc.cameraPosition[0] = context.view.cameraPosition.x;
    fc.cameraPosition[1] = context.view.cameraPosition.y;
    fc.cameraPosition[2] = context.view.cameraPosition.z;
    fc.cameraPosition[3] = 1.f;

    fc.cameraForward[0] = context.view.cameraForward.x;
    fc.cameraForward[1] = context.view.cameraForward.y;
    fc.cameraForward[2] = context.view.cameraForward.z;
    fc.cameraForward[3] = 0.f;

    fc.screenSize[0] = static_cast<float>(context.viewportWidth);
    fc.screenSize[1] = static_cast<float>(context.viewportHeight);
    fc.screenSize[2] = context.viewportWidth ? 1.f / static_cast<float>(context.viewportWidth) : 0.f;
    fc.screenSize[3] = context.viewportHeight ? 1.f / static_cast<float>(context.viewportHeight) : 0.f;

    fc.timeData[0] = static_cast<float>(context.timing.GetTimeSeconds());
    fc.timeData[1] = context.timing.GetDeltaSecondsF();
    fc.timeData[2] = static_cast<float>(context.timing.GetFrameCount());
    fc.timeData[3] = 0.f;

    fc.ambientColor[0] = context.view.ambientColor.x;
    fc.ambientColor[1] = context.view.ambientColor.y;
    fc.ambientColor[2] = context.view.ambientColor.z;
    fc.ambientColor[3] = context.view.ambientIntensity;

    fc.nearPlane = context.view.nearPlane;
    fc.farPlane = context.view.farPlane;
    fc.shadowCascadeCount = 0u;

    const auto& lights = context.renderWorld.GetLights();
    fc.lightCount = static_cast<uint32_t>(
        std::min(lights.size(), static_cast<size_t>(kMaxLightsPerFrame)));

    for (uint32_t i = 0u; i < fc.lightCount; ++i)
        PackLightData(lights[i], fc.lights[i]);

    fc.iblPrefilterLevels = static_cast<float>(kIBLPrefilterMipCount) - 1.0f;
    fc._padFC[0] = fc._padFC[1] = fc._padFC[2] = 0.0f;

    result.frameConstants = fc;
    return true;
}

} // namespace engine::renderer
