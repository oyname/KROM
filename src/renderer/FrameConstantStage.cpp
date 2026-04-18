#include "renderer/internal/FrameConstantStage.hpp"
#include "renderer/Environment.hpp"
#include <algorithm>
#include <cstring>

namespace engine::renderer {
namespace {

void FillMatrixRowMajor(const math::Mat4& m, float out[16]) noexcept
{
    std::memcpy(out, m.Data(), sizeof(float) * 16u);
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

    fc.featureCount0 = 0u;
    fc.featureCount1 = 0u;
    std::memset(fc.featurePayload, 0, sizeof(fc.featurePayload));
    fc.nearPlane = context.view.nearPlane;
    fc.farPlane = context.view.farPlane;
    fc.iblPrefilterLevels = static_cast<float>(kIBLPrefilterMipCount) - 1.0f;
    fc._padFC[0] = fc._padFC[1] = fc._padFC[2] = 0.0f;

    const FrameConstantsContributionContext contributionContext{
        context.viewportWidth,
        context.viewportHeight,
        context.view,
        context.timing,
        context.renderWorld
    };
    for (const IFrameConstantsContributor* contributor : context.contributors)
    {
        if (!contributor)
            continue;
        contributor->Contribute(contributionContext, fc);
    }

    result.frameConstants = fc;
    return true;
}

} // namespace engine::renderer
