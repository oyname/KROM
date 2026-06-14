#include "addons/forward/ForwardPlusLighting.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "core/Math.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderFeatureDataViews.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace engine::renderer::addons::forward {
namespace {

[[nodiscard]] math::Mat4 BuildMat4FromArray(const float* raw) noexcept
{
    math::Mat4 matrix = math::Mat4::Zero();
    if (raw)
        std::memcpy(matrix.Data(), raw, sizeof(float) * 16u);
    return matrix;
}

[[nodiscard]] uint32_t NextCapacity(uint32_t required) noexcept
{
    uint32_t capacity = 1u;
    while (capacity < required)
        capacity <<= 1u;
    return capacity;
}

struct alignas(16) ForwardPlusComputeParamsGpu
{
    uint32_t lightCount      = 0u;
    uint32_t tileCountX      = 0u;
    uint32_t tileCountY      = 0u;
    uint32_t maxLightsPerTile = 0u;
    uint32_t viewportWidth   = 0u;
    uint32_t viewportHeight  = 0u;
    uint32_t _pad0           = 0u;
    uint32_t _pad1           = 0u;
};

constexpr uint32_t kForwardPlusComputeGroupSize = 64u;

constexpr const char* kForwardPlusCullCsHlsl = R"(struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

struct TileHeaderRw
{
    uint offset;
    uint count;
    uint reserved0;
    uint reserved1;
};

#ifdef __spirv__
[[vk::binding(74)]] StructuredBuffer<GpuLightData> forwardPlusLights;
[[vk::binding(48)]] RWStructuredBuffer<TileHeaderRw> outTileHeaders;
[[vk::binding(49)]] RWStructuredBuffer<uint> outLightIndices;
#else
StructuredBuffer<GpuLightData> forwardPlusLights : register(t10);
RWStructuredBuffer<TileHeaderRw> outTileHeaders  : register(u0);
RWStructuredBuffer<uint> outLightIndices         : register(u1);
#endif

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeData;
    float4       ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    float4x4     shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    float2       _shadowArrayPad;
};

cbuffer ForwardPlusParams : register(b3)
{
    uint fpLightCount;
    uint fpTileCountX;
    uint fpTileCountY;
    uint fpMaxLightsPerTile;
    uint fpViewportWidth;
    uint fpViewportHeight;
    uint fpPad0;
    uint fpPad1;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadID.x;
    if (lightIndex >= fpLightCount)
        return;

    const GpuLightData light = forwardPlusLights[lightIndex];
    const float type = light.params.w;
    if (type < 0.5f)
    {
        for (uint tileY = 0u; tileY < fpTileCountY; ++tileY)
        {
            for (uint tileX = 0u; tileX < fpTileCountX; ++tileX)
            {
                const uint tileLinearIndex = tileY * fpTileCountX + tileX;
                uint writeIndex = 0u;
                InterlockedAdd(outTileHeaders[tileLinearIndex].count, 1u, writeIndex);
                if (writeIndex < fpMaxLightsPerTile)
                {
                    outLightIndices[outTileHeaders[tileLinearIndex].offset + writeIndex] = lightIndex;
                    InterlockedMax(outTileHeaders[tileLinearIndex].reserved0, writeIndex + 1u);
                }
            }
        }
        return;
    }

    const float4 viewSpace = mul(viewMatrix, float4(light.positionWS.xyz, 1.0f));
    const float viewDepthSigned = -viewSpace.z;
    const float lightRange = max(light.params.z, 0.05f);
    if (viewDepthSigned + lightRange <= nearPlane)
        return;
    if (viewDepthSigned - lightRange > farPlane)
        return;

    if (viewDepthSigned <= 0.0f)
    {
        for (uint tileY = 0u; tileY < fpTileCountY; ++tileY)
        {
            for (uint tileX = 0u; tileX < fpTileCountX; ++tileX)
            {
                const uint tileLinearIndex = tileY * fpTileCountX + tileX;
                uint writeIndex = 0u;
                InterlockedAdd(outTileHeaders[tileLinearIndex].count, 1u, writeIndex);
                if (writeIndex < fpMaxLightsPerTile)
                {
                    outLightIndices[outTileHeaders[tileLinearIndex].offset + writeIndex] = lightIndex;
                    InterlockedMax(outTileHeaders[tileLinearIndex].reserved0, writeIndex + 1u);
                }
            }
        }
        return;
    }

    const float projX = abs(projMatrix[0][0]);
    const float projY = abs(projMatrix[1][1]);
    const float projDepth = max(viewDepthSigned, nearPlane);
    const float radiusPxX = projX * lightRange / projDepth * float(fpViewportWidth) * 0.5f;
    const float radiusPxY = projY * lightRange / projDepth * float(fpViewportHeight) * 0.5f;

    const float4 clip = mul(viewProjMatrix, float4(light.positionWS.xyz, 1.0f));
    if (clip.w <= 0.0f)
        return;

    const float invW = 1.0f / clip.w;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    const float screenX = (ndcX * 0.5f + 0.5f) * float(fpViewportWidth);
    const float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * float(fpViewportHeight);

    const float minPx = screenX - radiusPxX;
    const float maxPx = screenX + radiusPxX;
    const float minPy = screenY - radiusPxY;
    const float maxPy = screenY + radiusPxY;
    if (maxPx < 0.0f || maxPy < 0.0f || minPx > float(fpViewportWidth) || minPy > float(fpViewportHeight))
        return;

    const float tileSize = 16.0f;
    const uint tileMinX = min(fpTileCountX - 1u, (uint)max(0.0f, floor(minPx / tileSize)));
    const uint tileMaxX = min(fpTileCountX - 1u, (uint)max(0.0f, floor(maxPx / tileSize)));
    const uint tileMinY = min(fpTileCountY - 1u, (uint)max(0.0f, floor(minPy / tileSize)));
    const uint tileMaxY = min(fpTileCountY - 1u, (uint)max(0.0f, floor(maxPy / tileSize)));

    for (uint tileY = tileMinY; tileY <= tileMaxY; ++tileY)
    {
        for (uint tileX = tileMinX; tileX <= tileMaxX; ++tileX)
        {
            const uint tileLinearIndex = tileY * fpTileCountX + tileX;
            uint writeIndex = 0u;
            InterlockedAdd(outTileHeaders[tileLinearIndex].count, 1u, writeIndex);
            if (writeIndex < fpMaxLightsPerTile)
            {
                outLightIndices[outTileHeaders[tileLinearIndex].offset + writeIndex] = lightIndex;
                InterlockedMax(outTileHeaders[tileLinearIndex].reserved0, writeIndex + 1u);
            }
        }
    }
})";

void EnsureForwardPlusBuffer(IDevice& device,
                             BufferHandle& buffer,
                             uint32_t& capacity,
                             uint32_t requiredCount,
                             uint32_t stride,
                             const char* debugName,
                             ResourceUsage usage = ResourceUsage::ShaderResource,
                             MemoryAccess access = MemoryAccess::CpuWrite,
                             ResourceState initialState = ResourceState::ShaderRead)
{
    if (requiredCount == 0u)
        requiredCount = 1u;
    if (buffer.IsValid() && capacity >= requiredCount)
        return;

    if (buffer.IsValid())
        device.DestroyBuffer(buffer);

    BufferDesc desc{};
    desc.byteSize     = static_cast<uint64_t>(NextCapacity(requiredCount)) * stride;
    desc.stride       = stride;
    desc.type         = BufferType::Structured;
    desc.usage        = usage;
    desc.access       = access;
    desc.initialState = initialState;
    desc.debugName    = debugName;
    buffer   = device.CreateBuffer(desc);
    capacity = buffer.IsValid() ? NextCapacity(requiredCount) : 0u;
}

void EnsureForwardPlusConstantBuffer(IDevice& device,
                                     BufferHandle& buffer,
                                     const char* debugName)
{
    if (buffer.IsValid())
        return;

    BufferDesc desc{};
    desc.byteSize = sizeof(ForwardPlusComputeParamsGpu);
    desc.stride = sizeof(uint32_t);
    desc.type = BufferType::Constant;
    desc.usage = ResourceUsage::ConstantBuffer;
    desc.access = MemoryAccess::CpuWrite;
    desc.initialState = ResourceState::ConstantBuffer;
    desc.debugName = debugName;
    buffer = device.CreateBuffer(desc);
}

void BuildFixedOffsetTileHeaders(uint32_t tileCount,
                                 uint32_t maxLightsPerTile,
                                 std::vector<ForwardPlusTileHeaderGpu>& outHeaders) 
{
    outHeaders.assign(tileCount, {});
    for (uint32_t tileIndex = 0u; tileIndex < tileCount; ++tileIndex)
        outHeaders[tileIndex].offset = tileIndex * maxLightsPerTile;
}

[[nodiscard]] bool BuildForwardPlusTileList(
    const engine::addons::lighting::LightingFrameData& lighting,
    const FrameConstants& frameConstants,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    uint32_t tileCountX,
    uint32_t tileCountY,
    std::vector<ForwardPlusTileHeaderGpu>& outHeaders,
    std::vector<uint32_t>&                outIndices,
    ForwardPlusTileMetrics&               outMetrics)
{
    outHeaders.assign(static_cast<size_t>(tileCountX) * static_cast<size_t>(tileCountY), {});
    outIndices.clear();
    outMetrics             = {};
    outMetrics.tileCount   = tileCountX * tileCountY;

    if (lighting.lights.empty() || tileCountX == 0u || tileCountY == 0u)
        return true;

    const math::Mat4 viewMatrix     = BuildMat4FromArray(frameConstants.viewMatrix);
    const math::Mat4 viewProjMatrix = BuildMat4FromArray(frameConstants.viewProjMatrix);
    const math::Mat4 projMatrix     = BuildMat4FromArray(frameConstants.projMatrix);
    const float projX = std::abs(projMatrix.m[0][0]);
    const float projY = std::abs(projMatrix.m[1][1]);

    std::vector<std::vector<uint32_t>> perTile(outHeaders.size());
    for (uint32_t lightIndex = 0u;
         lightIndex < static_cast<uint32_t>(lighting.lights.size());
         ++lightIndex)
    {
        const auto& light = lighting.lights[lightIndex];
        if (light.type == engine::addons::lighting::ExtractedLightType::Directional)
        {
            for (auto& tile : perTile)
                tile.push_back(lightIndex);
            continue;
        }

        const math::Vec4 viewSpace      = viewMatrix * math::Vec4(light.positionWorld, 1.0f);
        const float      viewDepthSigned = -viewSpace.z;
        const float      lightRange      = std::max(light.range, 0.05f);

        if (viewDepthSigned + lightRange <= frameConstants.nearPlane)
            continue;
        if (viewDepthSigned - lightRange > frameConstants.farPlane)
            continue;

        // Licht hinter Kamera, aber Sphere schneidet Near-Plane → alle Tiles konservativ
        if (viewDepthSigned <= 0.0f)
        {
            for (auto& tile : perTile)
                tile.push_back(lightIndex);
            continue;
        }

        // Divisor klemmen: extremen Pixel-Radius für Lichter nahe an der Near-Plane verhindern
        const float projDepth = std::max(viewDepthSigned, frameConstants.nearPlane);
        const float radiusPxX = projX * lightRange / projDepth * static_cast<float>(viewportWidth)  * 0.5f;
        const float radiusPxY = projY * lightRange / projDepth * static_cast<float>(viewportHeight) * 0.5f;

        const math::Vec4 clip = viewProjMatrix * math::Vec4(light.positionWorld, 1.0f);
        if (clip.w <= 0.0f)
            continue;

        const float invW    = 1.0f / clip.w;
        const float ndcX    = clip.x * invW;
        const float ndcY    = clip.y * invW;
        const float screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
        // Y invertiert: NDC +1 = oben = Screen-Y 0
        const float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(viewportHeight);

        const float minPx = screenX - radiusPxX;
        const float maxPx = screenX + radiusPxX;
        const float minPy = screenY - radiusPxY;
        const float maxPy = screenY + radiusPxY;

        if (maxPx < 0.0f || maxPy < 0.0f ||
            minPx > static_cast<float>(viewportWidth) ||
            minPy > static_cast<float>(viewportHeight))
        {
            continue;
        }

        const float tileF = static_cast<float>(kForwardPlusTileSize);
        const uint32_t tileMinX = std::min(tileCountX - 1u,
            static_cast<uint32_t>(std::max(0.0f, std::floor(minPx / tileF))));
        const uint32_t tileMaxX = std::min(tileCountX - 1u,
            static_cast<uint32_t>(std::max(0.0f, std::floor(maxPx / tileF))));
        const uint32_t tileMinY = std::min(tileCountY - 1u,
            static_cast<uint32_t>(std::max(0.0f, std::floor(minPy / tileF))));
        const uint32_t tileMaxY = std::min(tileCountY - 1u,
            static_cast<uint32_t>(std::max(0.0f, std::floor(maxPy / tileF))));

        for (uint32_t tileY = tileMinY; tileY <= tileMaxY; ++tileY)
        {
            for (uint32_t tileX = tileMinX; tileX <= tileMaxX; ++tileX)
                perTile[tileY * tileCountX + tileX].push_back(lightIndex);
        }
    }

    uint32_t runningOffset = 0u;
    uint32_t minCount      = std::numeric_limits<uint32_t>::max();
    uint32_t maxCount      = 0u;
    uint64_t totalAssign   = 0u;

    for (size_t tileIdx = 0u; tileIdx < perTile.size(); ++tileIdx)
    {
        const uint32_t count       = static_cast<uint32_t>(perTile[tileIdx].size());
        outHeaders[tileIdx].offset = runningOffset;
        outHeaders[tileIdx].count  = count;
        // Der Pixel-Shader liest reserved0 als "tatsächlich gespeicherte Einträge".
        // Im CPU-Pfad ist das identisch zur exakten Tile-Länge.
        outHeaders[tileIdx].reserved0 = count;
        outIndices.insert(outIndices.end(), perTile[tileIdx].begin(), perTile[tileIdx].end());
        runningOffset += count;
        minCount       = std::min(minCount, count);
        maxCount       = std::max(maxCount, count);
        totalAssign   += count;
    }

    const uint32_t totalTiles          = tileCountX * tileCountY;
    outMetrics.minLightsPerTile        = perTile.empty() ? 0u : minCount;
    outMetrics.maxLightsPerTile        = maxCount;
    outMetrics.totalLightAssignments   = static_cast<uint32_t>(
        std::min<uint64_t>(totalAssign, UINT32_MAX));
    outMetrics.avgLightsPerTile        = totalTiles > 0u
        ? static_cast<float>(totalAssign) / static_cast<float>(totalTiles)
        : 0.0f;

    return true;
}

} // namespace

// -----------------------------------------------------------------------------

void DestroyForwardPlusGpuResources(IDevice& device, ForwardPlusGpuResources& resources)
{
    if (resources.tileHeaderBuffer.IsValid())
        device.DestroyBuffer(resources.tileHeaderBuffer);
    if (resources.tileIndexBuffer.IsValid())
        device.DestroyBuffer(resources.tileIndexBuffer);
    if (resources.computeParamsBuffer.IsValid())
        device.DestroyBuffer(resources.computeParamsBuffer);
    if (resources.resetStagingBuffer.IsValid())
        device.DestroyBuffer(resources.resetStagingBuffer);
    if (resources.computePipeline.IsValid())
        device.DestroyPipeline(resources.computePipeline);
    if (resources.computeShader.IsValid())
        device.DestroyShader(resources.computeShader);
    resources = {};
}

bool UpdateForwardPlusResources(IDevice& device,
                                RenderFrameDataView frameData,
                                const FrameConstants& frameConstants,
                                uint32_t viewportWidth,
                                uint32_t viewportHeight,
                                ForwardPlusGpuResources& resources)
{
    const auto* lighting =
        frameData.GetFrameData<engine::addons::lighting::LightingFrameData>();
    if (!lighting)
        return false;

    resources.tileCountX = std::max(1u,
        (viewportWidth  + kForwardPlusTileSize - 1u) / kForwardPlusTileSize);
    resources.tileCountY = std::max(1u,
        (viewportHeight + kForwardPlusTileSize - 1u) / kForwardPlusTileSize);

    std::vector<ForwardPlusTileHeaderGpu> tileHeaders;
    std::vector<uint32_t>                 tileIndices;
    if (!BuildForwardPlusTileList(*lighting,
                                  frameConstants,
                                  viewportWidth, viewportHeight,
                                  resources.tileCountX, resources.tileCountY,
                                  tileHeaders, tileIndices,
                                  resources.metrics))
        return false;

    EnsureForwardPlusBuffer(device,
                            resources.tileHeaderBuffer,
                            resources.tileHeaderCapacity,
                            static_cast<uint32_t>(tileHeaders.size()),
                            sizeof(ForwardPlusTileHeaderGpu),
                            "ForwardPlus_TileHeaders");
    EnsureForwardPlusBuffer(device,
                            resources.tileIndexBuffer,
                            resources.tileIndexCapacity,
                            static_cast<uint32_t>(tileIndices.size()),
                            sizeof(uint32_t),
                            "ForwardPlus_TileIndices");

    if (!resources.tileHeaderBuffer.IsValid() || !resources.tileIndexBuffer.IsValid())
        return false;

    device.UploadBufferData(resources.tileHeaderBuffer,
                            tileHeaders.data(),
                            tileHeaders.size() * sizeof(ForwardPlusTileHeaderGpu));
    if (!tileIndices.empty())
    {
        device.UploadBufferData(resources.tileIndexBuffer,
                                tileIndices.data(),
                                tileIndices.size() * sizeof(uint32_t));
    }

    resources.initialized = true;
    return true;
}

bool EnsureForwardPlusComputePipeline(IDevice& device,
                                      ForwardPlusGpuResources& resources)
{
    if (resources.computePipeline.IsValid() && resources.computeShader.IsValid())
        return true;

    resources.computeShader = device.CreateShaderFromSource(
        kForwardPlusCullCsHlsl,
        ShaderStageMask::Compute,
        "main",
        "ForwardPlus_CullCS");
    if (!resources.computeShader.IsValid())
        return false;

    PipelineDesc pipelineDesc{};
    pipelineDesc.pipelineClass = PipelineClass::Compute;
    pipelineDesc.debugName = "ForwardPlus_CullPipeline";
    pipelineDesc.shaderStages.push_back(ShaderStageDesc{
        resources.computeShader,
        ShaderStageMask::Compute,
        "main"
    });
    resources.computePipeline = device.CreatePipeline(pipelineDesc);
    return resources.computePipeline.IsValid();
}

bool PrepareForwardPlusComputeResources(IDevice& device,
                                        RenderFrameDataView frameData,
                                        const FrameConstants& frameConstants,
                                        uint32_t viewportWidth,
                                        uint32_t viewportHeight,
                                        ForwardPlusGpuResources& resources)
{
    const auto* lighting =
        frameData.GetFrameData<engine::addons::lighting::LightingFrameData>();
    if (!lighting)
        return false;

    const uint32_t lightCount = lighting->gpu.lightCount;
    if (!EnsureForwardPlusComputePipeline(device, resources))
        return false;

    resources.tileCountX = std::max(1u,
        (viewportWidth + kForwardPlusTileSize - 1u) / kForwardPlusTileSize);
    resources.tileCountY = std::max(1u,
        (viewportHeight + kForwardPlusTileSize - 1u) / kForwardPlusTileSize);
    resources.maxLightsPerTile = std::max(1u, lightCount);

    const uint32_t tileCount = resources.tileCountX * resources.tileCountY;
    EnsureForwardPlusBuffer(device,
                            resources.tileHeaderBuffer,
                            resources.tileHeaderCapacity,
                            tileCount,
                            sizeof(ForwardPlusTileHeaderGpu),
                            "ForwardPlus_TileHeaders",
                            ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess | ResourceUsage::CopyDest,
                            MemoryAccess::GpuOnly,
                            ResourceState::ShaderRead);
    EnsureForwardPlusBuffer(device,
                            resources.tileIndexBuffer,
                            resources.tileIndexCapacity,
                            tileCount * resources.maxLightsPerTile,
                            sizeof(uint32_t),
                            "ForwardPlus_TileIndices",
                            ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess,
                            MemoryAccess::GpuOnly,
                            ResourceState::ShaderRead);
    EnsureForwardPlusConstantBuffer(device,
                                    resources.computeParamsBuffer,
                                    "ForwardPlus_ComputeParams");

    EnsureForwardPlusBuffer(device,
                            resources.resetStagingBuffer,
                            resources.resetStagingCapacity,
                            tileCount,
                            sizeof(ForwardPlusTileHeaderGpu),
                            "ForwardPlus_ResetStaging",
                            ResourceUsage::CopySource,
                            MemoryAccess::CpuWrite,
                            ResourceState::CopySource);

    if (!resources.tileHeaderBuffer.IsValid() ||
        !resources.tileIndexBuffer.IsValid() ||
        !resources.computeParamsBuffer.IsValid() ||
        !resources.resetStagingBuffer.IsValid())
    {
        return false;
    }

    // Reset-Staging-Buffer mit count=0, reserved0=0 und festen Offsets befüllen.
    // CpuWrite-Buffer: reines memcpy, kein ImmediateSubmit + vkQueueWaitIdle.
    // ForwardFeature führt CopyBuffer + Barrier im main Command Buffer aus.
    std::vector<ForwardPlusTileHeaderGpu> tileHeaders;
    BuildFixedOffsetTileHeaders(tileCount, resources.maxLightsPerTile, tileHeaders);
    device.UploadBufferData(resources.resetStagingBuffer,
                            tileHeaders.data(),
                            tileHeaders.size() * sizeof(ForwardPlusTileHeaderGpu));

    ForwardPlusComputeParamsGpu params{};
    params.lightCount = lightCount;
    params.tileCountX = resources.tileCountX;
    params.tileCountY = resources.tileCountY;
    params.maxLightsPerTile = resources.maxLightsPerTile;
    params.viewportWidth = viewportWidth;
    params.viewportHeight = viewportHeight;
    device.UploadBufferData(resources.computeParamsBuffer, &params, sizeof(params));

    resources.metrics = {};
    resources.metrics.tileCount = tileCount;
    resources.metrics.maxLightsPerTile = lightCount;
    resources.initialized = true;
    (void)frameConstants;
    (void)lighting;
    return true;
}

} // namespace engine::renderer::addons::forward
