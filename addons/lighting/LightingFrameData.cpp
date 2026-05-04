#include "addons/lighting/LightingFrameData.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace engine::addons::lighting {
namespace {

[[nodiscard]] uint32_t NextLightCapacity(uint32_t requiredLights) noexcept
{
    uint32_t capacity = 1u;
    while (capacity < requiredLights)
        capacity <<= 1u;
    return capacity;
}

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
    ~LightingFrameConstantsContributor() override
    {
        // GPU resources must be released explicitly while the device is still alive.
        m_device = nullptr;
        m_lightBuffer = BufferHandle::Invalid();
        m_lightCapacity = 0u;
    }

    std::string_view GetName() const noexcept override { return "lighting.frame_constants"; }

    void OnDeviceShutdown() noexcept override
    {
        ReleaseBuffer();
        m_device = nullptr;
    }

    void Contribute(const renderer::FrameConstantsContributionContext& context,
                    renderer::FrameConstants& frameConstants) const override
    {
        m_device = context.device;
        const LightingFrameData* lightingConst = context.GetRenderWorld().GetFeatureData<LightingFrameData>();
        if (!lightingConst)
            return;
        LightingFrameData* lighting = const_cast<LightingFrameData*>(lightingConst);

        const uint32_t extractedLightCount = static_cast<uint32_t>(lighting->lights.size());
        lighting->gpu.lightCount = extractedLightCount;
        if (!lighting->lights.empty())
            EnsureLightBuffer(*lighting);
        else
            lighting->gpu.lightBuffer = BufferHandle::Invalid();

        lighting->packedCount = static_cast<uint32_t>(
            std::min(lighting->lights.size(), static_cast<size_t>(kMaxLightsPerFrame)));
        lighting->droppedCount = lighting->extractedCount > lighting->packedCount
            ? (lighting->extractedCount - lighting->packedCount)
            : 0u;

        frameConstants.featureCount0 = lighting->packedCount;

        auto* gpuLights = reinterpret_cast<GpuLightData*>(frameConstants.featurePayload);
        const size_t bytesToClear = sizeof(GpuLightData) * frameConstants.featureCount0;
        std::memset(gpuLights, 0, bytesToClear);

        for (uint32_t i = 0u; i < frameConstants.featureCount0; ++i)
            PackLightData(lighting->lights[i], gpuLights[i]);
    }

private:
    void EnsureLightBuffer(LightingFrameData& lighting) const
    {
        if (!m_device)
            return;

        const uint32_t requiredLights = static_cast<uint32_t>(lighting.lights.size());
        if (requiredLights == 0u)
            return;

        if (!m_lightBuffer.IsValid() || m_lightCapacity < requiredLights)
            RecreateBuffer(NextLightCapacity(requiredLights));

        if (!m_lightBuffer.IsValid())
            return;

        std::vector<GpuLightData> packedLights(requiredLights);
        for (uint32_t i = 0u; i < requiredLights; ++i)
            PackLightData(lighting.lights[i], packedLights[i]);

        m_device->UploadBufferData(m_lightBuffer, packedLights.data(), packedLights.size() * sizeof(GpuLightData));
        lighting.gpu.lightBuffer = m_lightBuffer;
        lighting.gpu.lightCount = requiredLights;
        lighting.gpu.lightCapacity = m_lightCapacity;
    }

    void RecreateBuffer(uint32_t newCapacity) const
    {
        ReleaseBuffer();
        if (!m_device || newCapacity == 0u)
            return;

        renderer::BufferDesc desc{};
        desc.byteSize = static_cast<uint64_t>(newCapacity) * sizeof(GpuLightData);
        desc.stride = sizeof(GpuLightData);
        desc.type = renderer::BufferType::Structured;
        desc.usage = renderer::ResourceUsage::ShaderResource;
        desc.access = renderer::MemoryAccess::CpuWrite;
        desc.initialState = renderer::ResourceState::ShaderRead;
        desc.debugName = "Lighting_LightBuffer";
        m_lightBuffer = m_device->CreateBuffer(desc);
        if (m_lightBuffer.IsValid())
            m_lightCapacity = newCapacity;
    }

    void ReleaseBuffer() const
    {
        if (m_device && m_lightBuffer.IsValid())
            m_device->DestroyBuffer(m_lightBuffer);
        m_lightBuffer = BufferHandle::Invalid();
        m_lightCapacity = 0u;
    }

    mutable renderer::IDevice* m_device = nullptr;
    mutable BufferHandle m_lightBuffer = BufferHandle::Invalid();
    mutable uint32_t m_lightCapacity = 0u;
};

} // namespace
size_t GetExtractedLightCount(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->lights.size() : 0u;
}

size_t GetExtractedLightCount(const renderer::RenderSceneSnapshot& snapshot) noexcept
{
    return GetExtractedLightCount(snapshot.GetWorld());
}

uint32_t GetPackedLightCount(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->packedCount : 0u;
}

uint32_t GetDroppedLightCount(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->droppedCount : 0u;
}

uint32_t GetShadowCastingLightCount(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->shadowCastingCount : 0u;
}

BufferHandle GetLightBuffer(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->gpu.lightBuffer : BufferHandle::Invalid();
}

uint32_t GetLightBufferCount(const renderer::RenderWorld& renderWorld) noexcept
{
    const LightingFrameData* lighting = renderWorld.GetFeatureData<LightingFrameData>();
    return lighting ? lighting->gpu.lightCount : 0u;
}

renderer::FrameConstantsContributorPtr CreateLightingFrameConstantsContributor()
{
    return std::make_shared<LightingFrameConstantsContributor>();
}

} // namespace engine::addons::lighting
