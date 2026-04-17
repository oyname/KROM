#include "addons/lighting/LightingFrameData.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace engine::addons::lighting {
namespace {

void PackLightData(const ExtractedLight& src, GpuLightData& dst) noexcept
{
    const bool isDirectional = (src.type == ExtractedLightType::Directional);
    const bool isSpot = (src.type == ExtractedLightType::Spot);

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

class LightingFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    std::string_view GetName() const noexcept override { return "lighting.frame_constants"; }

    void Contribute(const renderer::FrameConstantsContributionContext& context,
                    renderer::FrameConstants& frameConstants) const override
    {
        const LightingFrameData* lighting = context.renderWorld.GetFeatureData<LightingFrameData>();
        if (!lighting || lighting->lights.empty())
            return;

        frameConstants.featureCount0 = static_cast<uint32_t>(
            std::min(lighting->lights.size(), static_cast<size_t>(kMaxLightsPerFrame)));

        auto* gpuLights = reinterpret_cast<GpuLightData*>(frameConstants.featurePayload);
        const size_t bytesToClear = sizeof(GpuLightData) * frameConstants.featureCount0;
        std::memset(gpuLights, 0, bytesToClear);

        for (uint32_t i = 0u; i < frameConstants.featureCount0; ++i)
            PackLightData(lighting->lights[i], gpuLights[i]);
    }
};

} // namespace

size_t GetExtractedLightCount(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->lights.size() : 0u;
}

renderer::FrameConstantsContributorPtr CreateLightingFrameConstantsContributor()
{
    return std::make_shared<LightingFrameConstantsContributor>();
}

} // namespace engine::addons::lighting
