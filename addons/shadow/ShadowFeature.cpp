#include "addons/shadow/ShadowFeature.hpp"

#include "addons/lighting/LightingFrameData.hpp"
#include "addons/shadow/ShadowExtraction.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "renderer/FeatureID.hpp"
#include "renderer/RenderWorld.hpp"
#include <cstring>

namespace {

[[nodiscard]] uint32_t NextShadowCapacity(uint32_t required) noexcept
{
    uint32_t capacity = 1u;
    while (capacity < required)
        capacity <<= 1u;
    return capacity;
}

void FillMatrixRowMajor(const engine::math::Mat4& m, float out[16]) noexcept
{
    std::memcpy(out, m.Data(), sizeof(float) * 16u);
}

void FillFloat4(const float x, const float y, const float z, const float w, float out[4]) noexcept
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

struct ShadowHardwareDepthBias
{
    float constantFactor = 0.0f;
    float slopeFactor = 0.0f;
};

struct alignas(16) GpuShadowLightData
{
    float meta[4];
    float extra[4];
};

struct alignas(16) GpuShadowViewData
{
    float rect[4];
    float viewProj[16];
};

[[nodiscard]] ShadowHardwareDepthBias GetShadowHardwareDepthBias(const engine::renderer::IDevice& device) noexcept
{
    // slopeFactor wird auf allen Backends konsistent als Max-Depth-Slope-Multiplikator
    // interpretiert (DX11: SlopeScaledDepthBias, GL: glPolygonOffset factor,
    // Vulkan: depthBiasSlopeFactor). Er kompensiert den Tiefenfehler an Polygon-
    // Kanten automatisch proportional zum Einfallswinkel und beseitigt damit
    // Shadow Acne ohne fixen Bias-Wert pro Material.
    //
    // constantFactor bleibt 0: auf DX11+D32_FLOAT ist DepthBias ein INT und
    // hat bei Float-Tiefenpuffern keine Wirkung; Vulkan/GL wuerden abweichende
    // Einheiten benoetigen.
    const char* backendName = device.GetBackendName();
    if (backendName && std::string_view(backendName) == "Vulkan")
    {
        // Vulkan wendet depthBiasSlopeFactor im Shadow-Pass deutlich direkter an
        // als unser aktueller DX11/OpenGL-Pfad. Ein geringerer Slope-Bias vermeidet
        // hier sichtbares Peter-Panning bei Spot-Shadows im Atlas-Pfad.
        return { .constantFactor = 0.0f, .slopeFactor = 1.0f };
    }

    return { .constantFactor = 0.0f, .slopeFactor = 3.0f };
}

} // namespace

namespace engine::addons::shadow {
namespace {

class ShadowExtractionStep final : public renderer::ISceneExtractionStep
{
public:
    std::string_view GetName() const noexcept override { return "shadow.extract"; }

    void Extract(const renderer::SceneExtractionContext& ctx) const override
    {
        if (ctx.snapshot)
            ExtractShadow(ctx.world, *ctx.snapshot);
        else if (ctx.renderWorld)
            ExtractShadow(ctx.world, *ctx.renderWorld);
    }
};

class ShadowFrameConstantsContributor final : public renderer::IFrameConstantsContributor
{
public:
    ~ShadowFrameConstantsContributor() override
    {
        // GPU resources must be released explicitly while the device is still alive.
        m_device = nullptr;
        m_shadowLightBuffer = BufferHandle::Invalid();
        m_shadowViewBuffer = BufferHandle::Invalid();
        m_shadowLightCapacity = 0u;
        m_shadowViewCapacity = 0u;
    }

    std::string_view GetName() const noexcept override { return "shadow.frame_constants"; }

    void OnDeviceShutdown() noexcept override
    {
        ReleaseBuffers();
        m_device = nullptr;
    }

    void Contribute(const renderer::FrameConstantsContributionContext& context,
                    renderer::FrameConstants& fc) const override
    {
        m_device = context.device;
        const ShadowFrameData* shadow = context.GetRenderWorld().GetFeatureData<ShadowFrameData>();
        ShadowFrameData* mutableShadow = const_cast<ShadowFrameData*>(shadow);
        const ShadowRequest* request = shadow ? shadow->GetCurrentRenderPathPrimaryRequest() : nullptr;
        const ShadowView* selectedView = shadow ? shadow->GetCurrentRenderPathPrimaryView() : nullptr;
        std::vector<GpuShadowLightData> gpuLights;
        std::vector<GpuShadowViewData> gpuViews;

        fc.shadowLightCount = 0u;
        fc.shadowViewCount = 0u;
        for (uint32_t i = 0u; i < renderer::kMaxShadowLightsPerFrame; ++i)
        {
            std::memset(fc.shadowLightMeta[i], 0, sizeof(fc.shadowLightMeta[i]));
            std::memset(fc.shadowLightExtra[i], 0, sizeof(fc.shadowLightExtra[i]));
        }
        for (uint32_t i = 0u; i < renderer::kMaxShadowViewsPerFrame; ++i)
        {
            std::memset(fc.shadowViewRect[i], 0, sizeof(fc.shadowViewRect[i]));
            std::memset(fc.shadowViewProj[i], 0, sizeof(fc.shadowViewProj[i]));
        }

        if (shadow && shadow->HasCurrentRenderPathRequests())
        {
            const uint32_t shadowCount = static_cast<uint32_t>(
                std::min<size_t>(renderer::kMaxShadowLightsPerFrame, shadow->currentRenderPath.requestIndices.size()));
            uint32_t shadowViewCount = 0u;
            for (uint32_t i = 0u; i < shadowCount; ++i)
            {
                const size_t requestIndex = shadow->currentRenderPath.requestIndices[i];
                if (requestIndex >= shadow->requests.size())
                    continue;
                shadowViewCount += static_cast<uint32_t>(std::min<size_t>(
                    renderer::kMaxShadowViewsPerFrame - shadowViewCount,
                    shadow->requests[requestIndex].views.size()));
                if (shadowViewCount >= renderer::kMaxShadowViewsPerFrame)
                    break;
            }
            const uint32_t gridDim = std::max(1u, static_cast<uint32_t>(
                std::ceil(std::sqrt(static_cast<float>(std::max(1u, shadowViewCount))))));
            const float tileScale = 1.0f / static_cast<float>(gridDim);
            const float atlasTexelSize = shadowViewCount > 0u && context.GetRenderWorld().GetQueue().activeShadowResolution > 0u
                ? (1.0f / static_cast<float>(context.GetRenderWorld().GetQueue().activeShadowResolution))
                : 0.0f;

            fc.shadowLightCount = shadowCount;
            fc.shadowViewCount = shadowViewCount;
            gpuLights.reserve(shadowCount);
            gpuViews.reserve(shadowViewCount);
            uint32_t atlasViewIndex = 0u;
            for (uint32_t i = 0u; i < shadowCount; ++i)
            {
                const size_t requestIndex = shadow->currentRenderPath.requestIndices[i];
                if (requestIndex >= shadow->requests.size())
                    continue;

                const ShadowRequest& atlasRequest = shadow->requests[requestIndex];
                if (atlasRequest.views.empty())
                    continue;

                FillFloat4(static_cast<float>(atlasRequest.visibleLightIndex),
                           atlasRequest.settings.bias,
                           atlasRequest.settings.normalBias,
                           atlasRequest.settings.strength,
                           fc.shadowLightMeta[i]);
                const uint32_t firstViewIndex = atlasViewIndex;
                const uint32_t requestViewCount = static_cast<uint32_t>(std::min<size_t>(
                    renderer::kMaxShadowViewsPerFrame - atlasViewIndex,
                    atlasRequest.views.size()));
                FillFloat4(static_cast<float>(firstViewIndex),
                           static_cast<float>(requestViewCount),
                           0.0f,
                           0.0f,
                           fc.shadowLightExtra[i]);
                GpuShadowLightData gpuLight{};
                std::memcpy(gpuLight.meta, fc.shadowLightMeta[i], sizeof(gpuLight.meta));
                std::memcpy(gpuLight.extra, fc.shadowLightExtra[i], sizeof(gpuLight.extra));
                gpuLights.push_back(gpuLight);

                for (uint32_t viewIndex = 0u; viewIndex < requestViewCount; ++viewIndex, ++atlasViewIndex)
                {
                    const uint32_t col = atlasViewIndex % gridDim;
                    const uint32_t row = atlasViewIndex / gridDim;
                    const math::Mat4 adjustedShadowVP =
                        context.shadowClipSpaceAdjustment * atlasRequest.views[viewIndex].viewProj;
                    FillFloat4(static_cast<float>(col) * tileScale,
                               static_cast<float>(row) * tileScale,
                               tileScale,
                               tileScale,
                               fc.shadowViewRect[atlasViewIndex]);
                    FillMatrixRowMajor(adjustedShadowVP, fc.shadowViewProj[atlasViewIndex]);
                    GpuShadowViewData gpuView{};
                    std::memcpy(gpuView.rect, fc.shadowViewRect[atlasViewIndex], sizeof(gpuView.rect));
                    std::memcpy(gpuView.viewProj, fc.shadowViewProj[atlasViewIndex], sizeof(gpuView.viewProj));
                    gpuViews.push_back(gpuView);
                }
                fc.shadowTexelSize = atlasTexelSize;
            }
        }

        if (mutableShadow)
            UpdateGpuBuffers(*mutableShadow, gpuLights, gpuViews);

        if (!request || !selectedView)
        {
            fc.shadowCascadeCount = 0u;
            fc.shadowBias         = 0.f;
            fc.shadowNormalBias   = 0.f;
            fc.shadowStrength     = 1.f;
            fc.shadowTexelSize    = 0.f;
            std::memset(fc.featurePayload + lighting::kShadowVPOffset,
                        0, lighting::kShadowVPBytes);
            return;
        }

        fc.shadowCascadeCount = 1u;
        fc.shadowBias         = request->settings.bias;
        fc.shadowNormalBias   = request->settings.normalBias;
        fc.shadowStrength     = request->settings.strength;
        if (fc.shadowLightCount == 0u)
            fc.shadowTexelSize = 1.f / static_cast<float>(std::max(1u, request->settings.resolution));

        const math::Mat4 adjustedShadowVP = context.shadowClipSpaceAdjustment * selectedView->viewProj;
        FillMatrixRowMajor(adjustedShadowVP,
                           reinterpret_cast<float*>(fc.featurePayload + lighting::kShadowVPOffset));
    }

private:
    void UpdateGpuBuffers(ShadowFrameData& shadow,
                          const std::vector<GpuShadowLightData>& gpuLights,
                          const std::vector<GpuShadowViewData>& gpuViews) const
    {
        shadow.gpu.lightCount = static_cast<uint32_t>(gpuLights.size());
        shadow.gpu.viewCount = static_cast<uint32_t>(gpuViews.size());

        if (!gpuLights.empty())
        {
            EnsureLightBuffer(static_cast<uint32_t>(gpuLights.size()));
            if (m_shadowLightBuffer.IsValid())
            {
                m_device->UploadBufferData(m_shadowLightBuffer,
                                           gpuLights.data(),
                                           gpuLights.size() * sizeof(GpuShadowLightData));
                shadow.gpu.lightBuffer = m_shadowLightBuffer;
                shadow.gpu.lightCapacity = m_shadowLightCapacity;
            }
        }

        if (!gpuViews.empty())
        {
            EnsureViewBuffer(static_cast<uint32_t>(gpuViews.size()));
            if (m_shadowViewBuffer.IsValid())
            {
                m_device->UploadBufferData(m_shadowViewBuffer,
                                           gpuViews.data(),
                                           gpuViews.size() * sizeof(GpuShadowViewData));
                shadow.gpu.viewBuffer = m_shadowViewBuffer;
                shadow.gpu.viewCapacity = m_shadowViewCapacity;
            }
        }
    }

    void EnsureLightBuffer(uint32_t requiredLights) const
    {
        if (!m_device || requiredLights == 0u)
            return;
        if (!m_shadowLightBuffer.IsValid() || m_shadowLightCapacity < requiredLights)
        {
            if (m_shadowLightBuffer.IsValid())
                m_device->DestroyBuffer(m_shadowLightBuffer);
            renderer::BufferDesc desc{};
            desc.byteSize = static_cast<uint64_t>(NextShadowCapacity(requiredLights)) * sizeof(GpuShadowLightData);
            desc.stride = sizeof(GpuShadowLightData);
            desc.type = renderer::BufferType::Structured;
            desc.usage = renderer::ResourceUsage::ShaderResource;
            desc.access = renderer::MemoryAccess::CpuWrite;
            desc.initialState = renderer::ResourceState::ShaderRead;
            desc.debugName = "Shadow_LightBuffer";
            m_shadowLightBuffer = m_device->CreateBuffer(desc);
            m_shadowLightCapacity = m_shadowLightBuffer.IsValid() ? NextShadowCapacity(requiredLights) : 0u;
        }
    }

    void EnsureViewBuffer(uint32_t requiredViews) const
    {
        if (!m_device || requiredViews == 0u)
            return;
        if (!m_shadowViewBuffer.IsValid() || m_shadowViewCapacity < requiredViews)
        {
            if (m_shadowViewBuffer.IsValid())
                m_device->DestroyBuffer(m_shadowViewBuffer);
            renderer::BufferDesc desc{};
            desc.byteSize = static_cast<uint64_t>(NextShadowCapacity(requiredViews)) * sizeof(GpuShadowViewData);
            desc.stride = sizeof(GpuShadowViewData);
            desc.type = renderer::BufferType::Structured;
            desc.usage = renderer::ResourceUsage::ShaderResource;
            desc.access = renderer::MemoryAccess::CpuWrite;
            desc.initialState = renderer::ResourceState::ShaderRead;
            desc.debugName = "Shadow_ViewBuffer";
            m_shadowViewBuffer = m_device->CreateBuffer(desc);
            m_shadowViewCapacity = m_shadowViewBuffer.IsValid() ? NextShadowCapacity(requiredViews) : 0u;
        }
    }

    void ReleaseBuffers() const
    {
        if (!m_device)
            return;
        if (m_shadowLightBuffer.IsValid())
            m_device->DestroyBuffer(m_shadowLightBuffer);
        if (m_shadowViewBuffer.IsValid())
            m_device->DestroyBuffer(m_shadowViewBuffer);
        m_shadowLightBuffer = BufferHandle::Invalid();
        m_shadowViewBuffer = BufferHandle::Invalid();
        m_shadowLightCapacity = 0u;
        m_shadowViewCapacity = 0u;
    }

    mutable renderer::IDevice* m_device = nullptr;
    mutable BufferHandle m_shadowLightBuffer = BufferHandle::Invalid();
    mutable BufferHandle m_shadowViewBuffer = BufferHandle::Invalid();
    mutable uint32_t m_shadowLightCapacity = 0u;
    mutable uint32_t m_shadowViewCapacity = 0u;
};

class ShadowFeature final : public renderer::IEngineFeature
{
public:
    ShadowFeature()
        : m_extractionStep(std::make_shared<ShadowExtractionStep>())
        , m_frameContributor(std::make_shared<ShadowFrameConstantsContributor>())
    {
    }

    std::string_view GetName() const noexcept override { return "krom-shadow"; }

    renderer::FeatureID GetID() const noexcept override
    {
        return renderer::FeatureID::FromString("krom-shadow");
    }

    std::vector<renderer::FeatureID> GetDependencies() const noexcept override
    {
        return { renderer::FeatureID::FromString("krom-lighting") };
    }

    void Register(renderer::FeatureRegistrationContext& context) override
    {
        context.RegisterSceneExtractionStep(m_extractionStep);
        context.RegisterFrameConstantsContributor(m_frameContributor);
    }

    bool Initialize(const renderer::FeatureInitializationContext& context) override
    {
        const ShadowHardwareDepthBias bias = GetShadowHardwareDepthBias(context.device);
        context.shaderRuntime.SetShadowDepthBias(bias.constantFactor, bias.slopeFactor);
        return true;
    }

    void Shutdown(const renderer::FeatureShutdownContext& context) override
    {
        (void)context;
        m_frameContributor.reset();
        m_extractionStep.reset();
    }

private:
    renderer::SceneExtractionStepPtr       m_extractionStep;
    renderer::FrameConstantsContributorPtr m_frameContributor;
};

} // namespace

std::unique_ptr<renderer::IEngineFeature> CreateShadowFeature()
{
    return std::make_unique<ShadowFeature>();
}

} // namespace engine::addons::shadow
