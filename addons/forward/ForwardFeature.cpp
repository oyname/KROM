#include "ForwardFeature.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Debug.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderRuntime.hpp"
#include <array>
#include <cstring>
#include <memory>

namespace engine::renderer::addons::forward {
    namespace {

        constexpr const char* kSkyFullscreenVsHlsl = R"(struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput main(uint id : SV_VertexID)
{
    VSOutput o;

    if (id == 0u)
    {
        o.pos = float4(-1.0f,  1.0f, 0.0f, 1.0f);
        o.uv  = float2(0.0f, 0.0f);
    }
    else if (id == 1u)
    {
        o.pos = float4(-1.0f, -3.0f, 0.0f, 1.0f);
        o.uv  = float2(0.0f, 2.0f);
    }
    else
    {
        o.pos = float4( 3.0f,  1.0f, 0.0f, 1.0f);
        o.uv  = float2(2.0f, 0.0f);
    }

    return o;
})";

        constexpr const char* kSkyFullscreenPsHlsl = R"(TextureCube  uEnvironment : register(t8);
SamplerState uSampler     : register(s1);

struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

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
    float2       _shadowPad;
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    const float2 clipXY = float2(IN.uv.x * 2.0f - 1.0f, 1.0f - IN.uv.y * 2.0f);
    const float4 nearClipPos = float4(clipXY, 0.0f, 1.0f);
    const float4 farClipPos  = float4(clipXY, 1.0f, 1.0f);
    const float4 worldNearH = mul(invViewProjMatrix, nearClipPos);
    const float4 worldFarH  = mul(invViewProjMatrix, farClipPos);
    const float3 worldNear = worldNearH.xyz / max(worldNearH.w, 1e-6f);
    const float3 worldFar  = worldFarH.xyz / max(worldFarH.w, 1e-6f);
    const float3 viewDir = normalize(worldFar - worldNear);
    const float3 color = uEnvironment.SampleLevel(uSampler, viewDir, 0.0f).rgb;
    return float4(color, 1.0f);
})";

        constexpr const char* kSkyFullscreenVsGlsl = R"(#version 410 core
#extension GL_ARB_shading_language_420pack : enable

out vec2 vTexCoord;

void main()
{
    // Y-Positionen invertiert: OpenGL-Szene-Geometrie wird durch GetClipSpaceAdjustment()
    // (m[1][1]=-1) Y-gespiegelt ins HDR-FBO gerendert. Der Sky-VS umgeht die
    // Projektionsmatrix komplett und muss dieselbe Spiegelung manuell anwenden,
    // damit der Tonemap-Pass sky und scene konsistent aus dem FBO liest.
    const vec2 pos[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2(-1.0,  3.0),
        vec2( 3.0, -1.0)
    );
    const vec2 uv[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(0.0, 2.0),
        vec2(2.0, 0.0)
    );

    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    vTexCoord = uv[gl_VertexID];
})";

        constexpr const char* kSkyFullscreenPsGlsl = R"(#version 410 core
#extension GL_ARB_shading_language_420pack : enable

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(std140, binding = 0) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeData;
    vec4         ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    mat4         shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
    uint         debugFlags;
    vec2         _shadowPad;
};

layout(binding = 8) uniform samplerCube uEnvironment;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 clipXY = vec2(vTexCoord.x * 2.0 - 1.0, vTexCoord.y * 2.0 - 1.0);
    vec4 nearClipPos = vec4(clipXY, -1.0, 1.0);
    vec4 farClipPos = vec4(clipXY, 1.0, 1.0);
    vec4 worldNearH = invViewProjMatrix * nearClipPos;
    vec4 worldFarH = invViewProjMatrix * farClipPos;
    vec3 worldNear = worldNearH.xyz / max(worldNearH.w, 1e-6);
    vec3 worldFar = worldFarH.xyz / max(worldFarH.w, 1e-6);
    vec3 viewDir = normalize(worldFar - worldNear);
    vec3 color = textureLod(uEnvironment, viewDir, 0.0).rgb;
    fragColor = vec4(color, 1.0);
})";

        struct SkyGpuResources
        {
            ShaderHandle vertexShader = ShaderHandle::Invalid();
            ShaderHandle fragmentShader = ShaderHandle::Invalid();
            PipelineHandle pipeline = PipelineHandle::Invalid();
            BufferHandle perMaterialCB = BufferHandle::Invalid();
            bool initialized = false;
        };

        [[nodiscard]] ShaderHandle CreateSkyShader(IDevice& device,
                                                   assets::ShaderTargetProfile target,
                                                   assets::ShaderStage stage,
                                                   assets::ShaderSourceLanguage language,
                                                   const char* source,
                                                   const char* debugName)
        {
            assets::ShaderAsset shader{};
            shader.debugName = debugName;
            shader.stage = stage;
            shader.sourceLanguage = language;
            shader.entryPoint = "main";
            shader.sourceCode = source;

            const ShaderStageMask stageMask = stage == assets::ShaderStage::Vertex
                ? ShaderStageMask::Vertex
                : ShaderStageMask::Fragment;

            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
            {
                assets::CompiledShaderArtifact compiled{};
                std::string error;
                if (!ShaderCompiler::CompileForTarget(shader, target, compiled, &error))
                {
                    Debug::LogError("ForwardRenderPipeline: failed to compile sky shader '%s': %s",
                        debugName, error.c_str());
                    return ShaderHandle::Invalid();
                }
                if (compiled.bytecode.empty())
                {
                    Debug::LogError("ForwardRenderPipeline: compiled sky shader '%s' produced no bytecode",
                        debugName);
                    return ShaderHandle::Invalid();
                }
                return device.CreateShaderFromBytecode(compiled.bytecode.data(),
                    compiled.bytecode.size(),
                    stageMask,
                    debugName);
            }

            return device.CreateShaderFromSource(source, stageMask, "main", debugName);
        }

        void DestroySkyGpuResources(IDevice& device, SkyGpuResources& resources)
        {
            if (resources.perMaterialCB.IsValid())
                device.DestroyBuffer(resources.perMaterialCB);
            if (resources.pipeline.IsValid())
                device.DestroyPipeline(resources.pipeline);
            if (resources.fragmentShader.IsValid())
                device.DestroyShader(resources.fragmentShader);
            if (resources.vertexShader.IsValid())
                device.DestroyShader(resources.vertexShader);
            resources = {};
        }

        bool InitializeSkyGpuResources(IDevice& device, SkyGpuResources& resources)
        {
            const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
            const bool useOpenGlShaders = target == assets::ShaderTargetProfile::OpenGL_GLSL450;

            resources.vertexShader = CreateSkyShader(device,
                target,
                assets::ShaderStage::Vertex,
                useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                useOpenGlShaders ? kSkyFullscreenVsGlsl : kSkyFullscreenVsHlsl,
                useOpenGlShaders ? "ForwardFullscreenSkyVS_GL" : "ForwardFullscreenSkyVS");
            resources.fragmentShader = CreateSkyShader(device,
                target,
                assets::ShaderStage::Fragment,
                useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                useOpenGlShaders ? kSkyFullscreenPsGlsl : kSkyFullscreenPsHlsl,
                useOpenGlShaders ? "ForwardFullscreenSkyPS_GL" : "ForwardFullscreenSkyPS");
            if (!resources.vertexShader.IsValid() || !resources.fragmentShader.IsValid())
            {
                DestroySkyGpuResources(device, resources);
                return false;
            }

            BufferDesc perMaterialDesc{};
            perMaterialDesc.byteSize = kConstantBufferAlignment;
            perMaterialDesc.type = BufferType::Constant;
            perMaterialDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
            perMaterialDesc.access = MemoryAccess::CpuWrite;
            perMaterialDesc.debugName = "ForwardFullscreenSky_PerMaterialCB";
            resources.perMaterialCB = device.CreateBuffer(perMaterialDesc);
            if (!resources.perMaterialCB.IsValid())
            {
                Debug::LogError("ForwardRenderPipeline: failed to create sky per-material fallback CB");
                DestroySkyGpuResources(device, resources);
                return false;
            }
            const std::array<uint8_t, kConstantBufferAlignment> zeroCB{};
            device.UploadBufferData(resources.perMaterialCB, zeroCB.data(), zeroCB.size());

            PipelineDesc pipelineDesc{};
            pipelineDesc.shaderStages.push_back({ resources.vertexShader, ShaderStageMask::Vertex });
            pipelineDesc.shaderStages.push_back({ resources.fragmentShader, ShaderStageMask::Fragment });
            pipelineDesc.topology = PrimitiveTopology::TriangleList;
            pipelineDesc.colorFormat = Format::RGBA16_FLOAT;
            pipelineDesc.depthFormat = Format::Unknown;
            pipelineDesc.sampleCount = 1u;
            pipelineDesc.rasterizer.cullMode = CullMode::None;
            pipelineDesc.depthStencil.depthEnable = false;
            pipelineDesc.depthStencil.depthWrite = false;
            pipelineDesc.debugName = "ForwardFullscreenSky_Pipeline";

            resources.pipeline = device.CreatePipeline(pipelineDesc);
            if (!resources.pipeline.IsValid())
            {
                Debug::LogError("ForwardRenderPipeline: failed to create sky pipeline");
                DestroySkyGpuResources(device, resources);
                return false;
            }

            resources.initialized = true;
            return true;
        }

        class ForwardRenderPipeline final : public IRenderPipeline
        {
        public:
            ForwardRenderPipeline(ForwardFeatureConfig config,
                                  std::shared_ptr<SkyGpuResources> skyResources)
                : m_config(config)
                , m_skyResources(std::move(skyResources))
            {
            }

            std::string_view GetName() const noexcept override { return "forward"; }

            bool Build(const RenderPipelineBuildContext& context,
                RenderPipelineBuildResult& result) const override
            {
                StandardFrameRecipeBuilder::BuildParams params;
                params.viewportWidth = context.viewportWidth;
                params.viewportHeight = context.viewportHeight;
                params.bloomWidth = context.viewportWidth > 1u ? context.viewportWidth / 2u : 1u;
                params.bloomHeight = context.viewportHeight > 1u ? context.viewportHeight / 2u : 1u;
                params.backbufferRT = context.backbufferRT;
                params.backbufferTex = context.backbufferTex;
                params.shadowEnabled = context.renderQueue.activeShadowResolution > 0u;
                params.shadowMapSize = params.shadowEnabled
                    ? context.renderQueue.activeShadowResolution
                    : 2048u;
                params.skyEnabled = m_config.enableEnvironmentBackground
                    && m_skyResources
                    && m_skyResources->initialized
                    && m_skyResources->pipeline.IsValid();
                params.transparentEnabled = true;
                params.uiEnabled = context.externalCallbacks.Has(StandardFrameExecutors::UI)
                    || context.externalCallbacks.Has(StandardFrameExecutors::Present);
                params.clearColorValue = m_config.clearColorValue;

                FramePipelineCallbacks callbacks = context.externalCallbacks;

                auto runtime = context.runtimeBindings;

                auto executeDrawList = std::make_shared<std::function<void(const DrawList&, const rendergraph::RGExecContext&)>>(
                    [runtime](const DrawList& list, const rendergraph::RGExecContext& execCtx)
                    {
                        if (!execCtx.cmd || list.items.empty() || !runtime || !runtime->shaderRuntime || !runtime->materials)
                            return;

                        MaterialHandle lastMaterial = MaterialHandle::Invalid();
                        BufferHandle   lastVertexBuffer = BufferHandle::Invalid();
                        BufferHandle   lastIndexBuffer  = BufferHandle::Invalid();

                        for (const auto& item : list.items)
                        {
                            if (!item.hasGpuData())
                                continue;

                            const PerObjectConstants* perObjectConstants = nullptr;
                            if (runtime->renderQueue && item.cbOffset < runtime->renderQueue->objectConstants.size())
                                perObjectConstants = &runtime->renderQueue->objectConstants[item.cbOffset];

                            BufferBinding perObjBinding{};
                            if (runtime->perObjectArena.IsValid() && runtime->perObjectStride > 0u)
                            {
                                perObjBinding = BufferBinding{
                                    runtime->perObjectArena,
                                    item.cbOffset * runtime->perObjectStride,
                                    static_cast<uint32_t>(sizeof(PerObjectConstants))
                                };
                            }

                            if (item.material != lastMaterial)
                            {
                                if (!runtime->shaderRuntime->BindMaterialWithRange(*execCtx.cmd,
                                    *runtime->materials,
                                    item.material,
                                    runtime->perFrameCB,
                                    runtime->perFrameBinding,
                                    perObjBinding,
                                    {},
                                    perObjectConstants,
                                    list.passId))
                                    continue;
                                lastMaterial = item.material;
                            }
                            else
                            {
                                runtime->shaderRuntime->UpdatePerObjectBinding(*execCtx.cmd,
                                    perObjBinding,
                                    perObjectConstants);
                            }

                            if (item.gpuVertexBuffer != lastVertexBuffer)
                            {
                                execCtx.cmd->SetVertexBuffer(0u, item.gpuVertexBuffer, 0u);
                                lastVertexBuffer = item.gpuVertexBuffer;
                            }
                            if (item.gpuIndexBuffer != lastIndexBuffer)
                            {
                                execCtx.cmd->SetIndexBuffer(item.gpuIndexBuffer, true, 0u);
                                lastIndexBuffer = item.gpuIndexBuffer;
                            }
                            execCtx.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                        }
                    });

                struct PipelineState
                {
                    StandardFrameBuildResult resources{};
                    bool ready = false;
                };
                auto pipelineState = std::make_shared<PipelineState>();

                EnsureSkyResources(context);

                if (!callbacks.Has(StandardFrameExecutors::Sky))
                {
                    auto runtimeSky = runtime;
                    auto skyResources = m_skyResources;
                    callbacks.Register(StandardFrameExecutors::Sky,
                        [runtimeSky, skyResources](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !runtimeSky || !runtimeSky->shaderRuntime || !skyResources)
                                return;

                            const EnvironmentRuntimeState& environmentState =
                                runtimeSky->shaderRuntime->GetEnvironmentState();
                            if (!environmentState.environment.IsValid() || !skyResources->pipeline.IsValid())
                                return;

                            execCtx.cmd->SetPipeline(skyResources->pipeline);
                            if (runtimeSky->perFrameCB.IsValid())
                                execCtx.cmd->SetConstantBuffer(CBSlots::PerFrame, runtimeSky->perFrameCB, ShaderStageMask::Fragment);
                            if (skyResources->perMaterialCB.IsValid())
                                execCtx.cmd->SetConstantBuffer(CBSlots::PerMaterial, skyResources->perMaterialCB, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0,
                                environmentState.environment,
                                ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp,
                                SamplerSlots::LinearClamp,
                                ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Opaque))
                {
                    callbacks.Register(StandardFrameExecutors::Opaque,
                        [runtime, executeDrawList, pipelineState](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->renderQueue || !pipelineState->ready)
                                return;

                            if (pipelineState->resources.shadowMap != rendergraph::RG_INVALID_RESOURCE)
                            {
                                const TextureHandle shadowTex =
                                    execCtx.GetTexture(pipelineState->resources.shadowMap);
                                if (shadowTex.IsValid())
                                {
                                    execCtx.cmd->SetShaderResource(TexSlots::ShadowMap,
                                        shadowTex, ShaderStageMask::Fragment);
                                    execCtx.cmd->SetShaderResource(TexSlots::PassSRV1,
                                        shadowTex, ShaderStageMask::Fragment);
                                    execCtx.cmd->SetSampler(SamplerSlots::ShadowPCF,
                                        SamplerSlots::ShadowPCF,
                                        ShaderStageMask::Fragment);
                                    execCtx.cmd->SetSampler(SamplerSlots::PointClamp,
                                        SamplerSlots::PointClamp,
                                        ShaderStageMask::Fragment);
                                }
                            }

                            const DrawList* list = runtime->renderQueue->FindList(StandardRenderPasses::Opaque());
                            if (list)
                                (*executeDrawList)(*list, execCtx);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Shadow))
                {
                    callbacks.Register(StandardFrameExecutors::Shadow,
                        [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->renderQueue)
                                return;
                            const DrawList* list = runtime->renderQueue->FindList(StandardRenderPasses::Shadow());
                            if (!list || !runtime->renderWorld || !runtime->perFrameConstantsData || !runtime->perFrameCB.IsValid())
                                return;

                            const auto* shadowData =
                                runtime->renderWorld->GetFeatureData<engine::addons::shadow::ShadowFrameData>();
                            if (!shadowData || !shadowData->HasCurrentRenderPathRequests())
                            {
                                (*executeDrawList)(*list, execCtx);
                                return;
                            }

                            const uint32_t shadowCount = static_cast<uint32_t>(
                                std::min<size_t>(renderer::kMaxShadowLightsPerFrame,
                                                 shadowData->currentRenderPath.requestIndices.size()));
                            uint32_t shadowViewCount = 0u;
                            for (uint32_t i = 0u; i < shadowCount; ++i)
                            {
                                const size_t requestIndex = shadowData->currentRenderPath.requestIndices[i];
                                if (requestIndex >= shadowData->requests.size())
                                    continue;
                                shadowViewCount += static_cast<uint32_t>(std::min<size_t>(
                                    renderer::kMaxShadowViewsPerFrame - shadowViewCount,
                                    shadowData->requests[requestIndex].views.size()));
                                if (shadowViewCount >= renderer::kMaxShadowViewsPerFrame)
                                    break;
                            }
                            const uint32_t atlasSize = std::max(1u, runtime->renderQueue->activeShadowResolution);
                            const uint32_t gridDim = std::max(1u, static_cast<uint32_t>(
                                std::ceil(std::sqrt(static_cast<float>(std::max(1u, shadowViewCount))))));
                            const uint32_t tileSize = std::max(1u, atlasSize / gridDim);
                            const BufferBinding previousPerFrameBinding = runtime->perFrameBinding;
                            const auto shadowPerFrameArena =
                                runtime->gpuRuntime
                                    ? runtime->gpuRuntime->AllocateConstantArena(sizeof(renderer::FrameConstants),
                                                                                 shadowViewCount,
                                                                                 "ShadowPerFrameArena")
                                    : renderer::GpuResourceRuntime::ConstantArenaResult{};
                            uint32_t atlasViewIndex = 0u;
                            for (uint32_t shadowIndex = 0u; shadowIndex < shadowCount; ++shadowIndex)
                            {
                                const size_t requestIndex = shadowData->currentRenderPath.requestIndices[shadowIndex];
                                if (requestIndex >= shadowData->requests.size())
                                    continue;

                                const auto& request = shadowData->requests[requestIndex];
                                if (request.views.empty())
                                    continue;
                                const uint32_t requestViewCount = static_cast<uint32_t>(std::min<size_t>(
                                    renderer::kMaxShadowViewsPerFrame - atlasViewIndex,
                                    request.views.size()));
                                for (uint32_t requestViewIndex = 0u; requestViewIndex < requestViewCount; ++requestViewIndex, ++atlasViewIndex)
                                {
                                    const auto& view = request.views[requestViewIndex];
                                    renderer::FrameConstants shadowFrame = *runtime->perFrameConstantsData;
                                    const math::Mat4 adjustedShadowVP =
                                        execCtx.device->GetShadowClipSpaceAdjustment() * view.viewProj;
                                    std::memcpy(shadowFrame.featurePayload + engine::addons::lighting::kShadowVPOffset,
                                                adjustedShadowVP.Data(),
                                                sizeof(float) * 16u);
                                    if (shadowPerFrameArena.buffer.IsValid() && shadowPerFrameArena.alignedStride > 0u)
                                    {
                                        const uint32_t shadowFrameOffset = atlasViewIndex * shadowPerFrameArena.alignedStride;
                                        runtime->gpuRuntime->UploadBuffer(shadowPerFrameArena.buffer,
                                                                          &shadowFrame,
                                                                          sizeof(renderer::FrameConstants),
                                                                          shadowFrameOffset);
                                        runtime->perFrameBinding = renderer::BufferBinding{
                                            shadowPerFrameArena.buffer,
                                            shadowFrameOffset,
                                            static_cast<uint32_t>(sizeof(renderer::FrameConstants))
                                        };
                                    }
                                    else
                                    {
                                        runtime->perFrameBinding = {};
                                        execCtx.device->UploadBufferData(runtime->perFrameCB,
                                                                         &shadowFrame,
                                                                         sizeof(renderer::FrameConstants));
                                    }

                                    const uint32_t col = atlasViewIndex % gridDim;
                                    const uint32_t row = atlasViewIndex / gridDim;
                                    execCtx.cmd->SetViewport(static_cast<float>(col * tileSize),
                                                             static_cast<float>(row * tileSize),
                                                             static_cast<float>(tileSize),
                                                             static_cast<float>(tileSize),
                                                             0.0f, 1.0f);
                                    execCtx.cmd->SetScissor(static_cast<int32_t>(col * tileSize),
                                                            static_cast<int32_t>(row * tileSize),
                                                            tileSize,
                                                            tileSize);
                                    (*executeDrawList)(*list, execCtx);
                                }
                            }

                            runtime->perFrameBinding = previousPerFrameBinding;
                            if (!shadowPerFrameArena.buffer.IsValid())
                            {
                                execCtx.device->UploadBufferData(runtime->perFrameCB,
                                                                 runtime->perFrameConstantsData,
                                                                 sizeof(renderer::FrameConstants));
                            }
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Transparent))
                {
                    callbacks.Register(StandardFrameExecutors::Transparent,
                        [runtime, executeDrawList](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->renderQueue)
                                return;
                            const DrawList* list = runtime->renderQueue->FindList(StandardRenderPasses::Transparent());
                            if (list)
                                (*executeDrawList)(*list, execCtx);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Present))
                {
                    callbacks.Register(StandardFrameExecutors::Present,
                        [](const rendergraph::RGExecContext& /*execCtx*/) {});
                }

                if (!callbacks.Has(StandardFrameExecutors::Tonemap) && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem)
                {
                    auto state = pipelineState;
                    auto runtimeTonemap = runtime;
                    callbacks.Register(StandardFrameExecutors::Tonemap,
                        [state, runtimeTonemap](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready)
                                return;
                            TextureHandle sourceTex = execCtx.GetTexture(state->resources.hdrSceneColor);
                            if (!sourceTex.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap source texture invalid");
                                return;
                            }
                            const MaterialHandle material = runtimeTonemap ? runtimeTonemap->defaultTonemapMaterial
                                                                           : MaterialHandle::Invalid();
                            if (!runtimeTonemap || !runtimeTonemap->shaderRuntime || !runtimeTonemap->tonemapMaterialSystem || !material.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap runtime/material invalid");
                                return;
                            }

                            if (!runtimeTonemap->shaderRuntime->BindMaterial(*execCtx.cmd,
                                *runtimeTonemap->tonemapMaterialSystem,
                                material,
                                BufferHandle::Invalid(),
                                BufferHandle::Invalid(),
                                BufferHandle::Invalid()))
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap material bind failed");
                                return;
                            }

                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }

                const StandardFrameBuildResult builtResources =
                    StandardFrameRecipeBuilder::Build(context.renderGraph, params, callbacks);
                result.backbuffer = builtResources.backbuffer;
                pipelineState->resources = builtResources;
                pipelineState->ready = true;
                return true;
            }

            void OnDeviceShutdown() noexcept override
            {
                if (m_device && m_skyResources)
                    DestroySkyGpuResources(*m_device, *m_skyResources);
                m_device = nullptr;
            }

        private:
            void EnsureSkyResources(const RenderPipelineBuildContext& context) const
            {
                if (!m_config.enableEnvironmentBackground || !m_skyResources || m_skyResources->initialized)
                    return;
                m_device = context.shaderRuntime.GetDevice();
                if (!m_device)
                {
                    Debug::LogWarning("ForwardRenderPipeline: sky background requested but device unavailable");
                    return;
                }
                (void)InitializeSkyGpuResources(*m_device, *m_skyResources);
            }

            ForwardFeatureConfig m_config;
            mutable std::shared_ptr<SkyGpuResources> m_skyResources = std::make_shared<SkyGpuResources>();
            mutable IDevice* m_device = nullptr;
        };

        class ForwardFeature final : public IEngineFeature
        {
        public:
            explicit ForwardFeature(ForwardFeatureConfig config)
                : m_skyResources(std::make_shared<SkyGpuResources>())
                , m_pipelineConfig(config)
                , m_pipeline(std::make_shared<ForwardRenderPipeline>(config, m_skyResources))
            {
            }

            std::string_view GetName() const noexcept override { return "krom-forward"; }
            FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-forward"); }

            void Register(FeatureRegistrationContext& context) override
            {
                context.RegisterRenderPipeline(m_pipeline, true);
            }

            bool Initialize(const FeatureInitializationContext& context) override
            {
                if (m_skyResources && !m_skyResources->initialized && m_pipelineConfig.enableEnvironmentBackground)
                    return InitializeSkyGpuResources(context.device, *m_skyResources);
                return true;
            }

            void Shutdown(const FeatureShutdownContext& context) override
            {
                (void)context;
                m_pipeline.reset();
                m_skyResources.reset();
            }

        private:
            ForwardFeatureConfig m_pipelineConfig{};
            std::shared_ptr<SkyGpuResources> m_skyResources;
            RenderPipelinePtr m_pipeline;
        };

    } // namespace

    std::unique_ptr<IEngineFeature> CreateForwardFeature(ForwardFeatureConfig config)
    {
        return std::make_unique<ForwardFeature>(config);
    }

} // namespace engine::renderer::addons::forward
