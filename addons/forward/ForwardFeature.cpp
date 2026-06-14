#include "ForwardFeature.hpp"
#include "addons/forward/ForwardPlusLighting.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RenderLayers.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RenderPipelineInterfaces.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

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
    const float2 focal = max(abs(float2(projMatrix._11, projMatrix._22)), float2(1e-6f, 1e-6f));
    const float3 cameraRightWS = normalize(float3(viewMatrix._11, viewMatrix._12, viewMatrix._13));
    const float3 cameraUpWS = normalize(float3(viewMatrix._21, viewMatrix._22, viewMatrix._23));
    const float3 viewDir = normalize(
        cameraRightWS * (clipXY.x / focal.x) +
        cameraUpWS * (clipXY.y / focal.y) +
        cameraForwardWS.xyz);
    const float3 color = uEnvironment.SampleLevel(uSampler, viewDir, 0.0f).rgb;
    return float4(color, 1.0f);
})";

        constexpr const char* kSkyFullscreenVsGlsl = R"(#version 410 core

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

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(std140) uniform PerFrame
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

uniform samplerCube uEnvironment;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 clipXY = vec2(vTexCoord.x * 2.0 - 1.0, 1.0 - vTexCoord.y * 2.0);
    vec2 focal = max(abs(vec2(projMatrix[0][0], projMatrix[1][1])), vec2(1e-6));
    vec3 cameraRightWS = normalize(vec3(viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]));
    vec3 cameraUpWS = normalize(vec3(viewMatrix[0][1], viewMatrix[1][1], viewMatrix[2][1]));
    vec3 viewDir = normalize(
        cameraRightWS * (clipXY.x / focal.x) +
        cameraUpWS * (clipXY.y / focal.y) +
        cameraForwardWS.xyz);
    vec3 color = textureLod(uEnvironment, viewDir, 0.0).rgb;
    fragColor = vec4(color, 1.0);
})";

        constexpr const char* kBloomExtractPsHlsl = R"(Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1);

cbuffer PerPass : register(b3)
{
    float4 bloomParams; // x=threshold, y=intensity, z=blurRadius
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    const float3 hdr = uHDRInput.Sample(uSampler, IN.uv).rgb;
    const float luminance = dot(hdr, float3(0.2126f, 0.7152f, 0.0722f));
    const float knee = max(bloomParams.x * 0.25f, 1e-4f);
    const float soft = saturate((luminance - bloomParams.x + knee) / (2.0f * knee));
    const float contribution = max(luminance - bloomParams.x, 0.0f) + soft * soft * knee;
    const float scale = contribution / max(luminance, 1e-4f);
    return float4(hdr * scale, 1.0f);
})";

        constexpr const char* kBloomBlurHPsHlsl = R"(Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1);

cbuffer PerPass : register(b3)
{
    float4 bloomParams; // x=threshold, y=intensity, z=blurRadius
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    uint width;
    uint height;
    uHDRInput.GetDimensions(width, height);
    const float2 texel = float2(max(bloomParams.z, 0.0f) / max(float(width), 1.0f), 0.0f);
    float3 color = uHDRInput.Sample(uSampler, IN.uv).rgb * 0.227027f;
    color += uHDRInput.Sample(uSampler, IN.uv + texel * 1.384615f).rgb * 0.316216f;
    color += uHDRInput.Sample(uSampler, IN.uv - texel * 1.384615f).rgb * 0.316216f;
    color += uHDRInput.Sample(uSampler, IN.uv + texel * 3.230769f).rgb * 0.070270f;
    color += uHDRInput.Sample(uSampler, IN.uv - texel * 3.230769f).rgb * 0.070270f;
    return float4(color, 1.0f);
})";

        constexpr const char* kBloomBlurVPsHlsl = R"(Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1);

cbuffer PerPass : register(b3)
{
    float4 bloomParams; // x=threshold, y=intensity, z=blurRadius
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    uint width;
    uint height;
    uHDRInput.GetDimensions(width, height);
    const float2 texel = float2(0.0f, max(bloomParams.z, 0.0f) / max(float(height), 1.0f));
    float3 color = uHDRInput.Sample(uSampler, IN.uv).rgb * 0.227027f;
    color += uHDRInput.Sample(uSampler, IN.uv + texel * 1.384615f).rgb * 0.316216f;
    color += uHDRInput.Sample(uSampler, IN.uv - texel * 1.384615f).rgb * 0.316216f;
    color += uHDRInput.Sample(uSampler, IN.uv + texel * 3.230769f).rgb * 0.070270f;
    color += uHDRInput.Sample(uSampler, IN.uv - texel * 3.230769f).rgb * 0.070270f;
    return float4(color, 1.0f);
})";

        constexpr const char* kBloomExtractPsGlsl = R"(#version 410 core

layout(std140) uniform PerPass
{
    vec4 bloomParams;
};

uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 hdr = texture(uHDRInput, vTexCoord).rgb;
    float luminance = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
    float knee = max(bloomParams.x * 0.25, 0.0001);
    float soft = clamp((luminance - bloomParams.x + knee) / (2.0 * knee), 0.0, 1.0);
    float contribution = max(luminance - bloomParams.x, 0.0) + soft * soft * knee;
    float scale = contribution / max(luminance, 0.0001);
    fragColor = vec4(hdr * scale, 1.0);
})";

        constexpr const char* kBloomBlurHPsGlsl = R"(#version 410 core

layout(std140) uniform PerPass
{
    vec4 bloomParams;
};

uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 size = vec2(max(textureSize(uHDRInput, 0).x, 1), max(textureSize(uHDRInput, 0).y, 1));
    vec2 texel = vec2(max(bloomParams.z, 0.0) / size.x, 0.0);
    vec3 color = texture(uHDRInput, vTexCoord).rgb * 0.227027;
    color += texture(uHDRInput, vTexCoord + texel * 1.384615).rgb * 0.316216;
    color += texture(uHDRInput, vTexCoord - texel * 1.384615).rgb * 0.316216;
    color += texture(uHDRInput, vTexCoord + texel * 3.230769).rgb * 0.070270;
    color += texture(uHDRInput, vTexCoord - texel * 3.230769).rgb * 0.070270;
    fragColor = vec4(color, 1.0);
})";

        constexpr const char* kBloomBlurVPsGlsl = R"(#version 410 core

layout(std140) uniform PerPass
{
    vec4 bloomParams;
};

uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 size = vec2(max(textureSize(uHDRInput, 0).x, 1), max(textureSize(uHDRInput, 0).y, 1));
    vec2 texel = vec2(0.0, max(bloomParams.z, 0.0) / size.y);
    vec3 color = texture(uHDRInput, vTexCoord).rgb * 0.227027;
    color += texture(uHDRInput, vTexCoord + texel * 1.384615).rgb * 0.316216;
    color += texture(uHDRInput, vTexCoord - texel * 1.384615).rgb * 0.316216;
    color += texture(uHDRInput, vTexCoord + texel * 3.230769).rgb * 0.070270;
    color += texture(uHDRInput, vTexCoord - texel * 3.230769).rgb * 0.070270;
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

        struct BloomPassConstants
        {
            float threshold = 1.0f;
            float intensity = 0.0f;
            float blurRadius = 1.0f;
            float pad = 0.0f;
        };

        struct BloomGpuResources
        {
            ShaderHandle vertexShader = ShaderHandle::Invalid();
            ShaderHandle extractShader = ShaderHandle::Invalid();
            ShaderHandle blurHShader = ShaderHandle::Invalid();
            ShaderHandle blurVShader = ShaderHandle::Invalid();
            PipelineHandle extractPipeline = PipelineHandle::Invalid();
            PipelineHandle blurHPipeline = PipelineHandle::Invalid();
            PipelineHandle blurVPipeline = PipelineHandle::Invalid();
            BufferHandle perPassCB = BufferHandle::Invalid();
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

            if (target != assets::ShaderTargetProfile::OpenGL_GLSL450)
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
                    if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                    {
                        Debug::LogError("ForwardRenderPipeline: compiled sky shader '%s' produced no bytecode",
                            debugName);
                        return ShaderHandle::Invalid();
                    }

                    // DX11/DX12 should normally land here with bytecode; if a future
                    // target only returns source text we still keep a functional fallback.
                    return device.CreateShaderFromSource(compiled.sourceText.empty() ? source : compiled.sourceText,
                        stageMask,
                        compiled.entryPoint.empty() ? "main" : compiled.entryPoint,
                        debugName);
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

        void DestroyBloomGpuResources(IDevice& device, BloomGpuResources& resources)
        {
            if (resources.perPassCB.IsValid())
                device.DestroyBuffer(resources.perPassCB);
            if (resources.extractPipeline.IsValid())
                device.DestroyPipeline(resources.extractPipeline);
            if (resources.blurHPipeline.IsValid())
                device.DestroyPipeline(resources.blurHPipeline);
            if (resources.blurVPipeline.IsValid())
                device.DestroyPipeline(resources.blurVPipeline);
            if (resources.extractShader.IsValid())
                device.DestroyShader(resources.extractShader);
            if (resources.blurHShader.IsValid())
                device.DestroyShader(resources.blurHShader);
            if (resources.blurVShader.IsValid())
                device.DestroyShader(resources.blurVShader);
            if (resources.vertexShader.IsValid())
                device.DestroyShader(resources.vertexShader);
            resources = {};
        }

        [[nodiscard]] PipelineHandle CreateBloomPipeline(IDevice& device,
                                                         ShaderHandle vertexShader,
                                                         ShaderHandle fragmentShader,
                                                         const char* debugName)
        {
            PipelineDesc pipelineDesc{};
            pipelineDesc.shaderStages.push_back({ vertexShader, ShaderStageMask::Vertex });
            pipelineDesc.shaderStages.push_back({ fragmentShader, ShaderStageMask::Fragment });
            pipelineDesc.topology = PrimitiveTopology::TriangleList;
            pipelineDesc.colorFormat = Format::RGBA16_FLOAT;
            pipelineDesc.depthFormat = Format::Unknown;
            pipelineDesc.sampleCount = 1u;
            pipelineDesc.rasterizer.cullMode = CullMode::None;
            pipelineDesc.depthStencil.depthEnable = false;
            pipelineDesc.depthStencil.depthWrite = false;
            pipelineDesc.debugName = debugName;
            return device.CreatePipeline(pipelineDesc);
        }

        bool InitializeBloomGpuResources(IDevice& device, BloomGpuResources& resources)
        {
            const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
            const bool useOpenGlShaders = target == assets::ShaderTargetProfile::OpenGL_GLSL450;

            resources.vertexShader = CreateSkyShader(device,
                target,
                assets::ShaderStage::Vertex,
                useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                useOpenGlShaders ? kSkyFullscreenVsGlsl : kSkyFullscreenVsHlsl,
                useOpenGlShaders ? "ForwardBloomFullscreenVS_GL" : "ForwardBloomFullscreenVS");
            resources.extractShader = CreateSkyShader(device,
                target,
                assets::ShaderStage::Fragment,
                useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                useOpenGlShaders ? kBloomExtractPsGlsl : kBloomExtractPsHlsl,
                useOpenGlShaders ? "ForwardBloomExtractPS_GL" : "ForwardBloomExtractPS");
            resources.blurHShader = CreateSkyShader(device,
                target,
                assets::ShaderStage::Fragment,
                useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                useOpenGlShaders ? kBloomBlurHPsGlsl : kBloomBlurHPsHlsl,
                useOpenGlShaders ? "ForwardBloomBlurHPS_GL" : "ForwardBloomBlurHPS");
            resources.blurVShader = CreateSkyShader(device,
                target,
                assets::ShaderStage::Fragment,
                useOpenGlShaders ? assets::ShaderSourceLanguage::GLSL : assets::ShaderSourceLanguage::HLSL,
                useOpenGlShaders ? kBloomBlurVPsGlsl : kBloomBlurVPsHlsl,
                useOpenGlShaders ? "ForwardBloomBlurVPS_GL" : "ForwardBloomBlurVPS");
            if (!resources.vertexShader.IsValid() || !resources.extractShader.IsValid() ||
                !resources.blurHShader.IsValid() || !resources.blurVShader.IsValid())
            {
                DestroyBloomGpuResources(device, resources);
                return false;
            }

            BufferDesc perPassDesc{};
            perPassDesc.byteSize = kConstantBufferAlignment;
            perPassDesc.type = BufferType::Constant;
            perPassDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
            perPassDesc.access = MemoryAccess::CpuWrite;
            perPassDesc.debugName = "ForwardBloom_PerPassCB";
            resources.perPassCB = device.CreateBuffer(perPassDesc);
            if (!resources.perPassCB.IsValid())
            {
                Debug::LogError("ForwardRenderPipeline: failed to create bloom per-pass CB");
                DestroyBloomGpuResources(device, resources);
                return false;
            }

            resources.extractPipeline = CreateBloomPipeline(device, resources.vertexShader, resources.extractShader, "ForwardBloomExtract_Pipeline");
            resources.blurHPipeline = CreateBloomPipeline(device, resources.vertexShader, resources.blurHShader, "ForwardBloomBlurH_Pipeline");
            resources.blurVPipeline = CreateBloomPipeline(device, resources.vertexShader, resources.blurVShader, "ForwardBloomBlurV_Pipeline");
            if (!resources.extractPipeline.IsValid() || !resources.blurHPipeline.IsValid() || !resources.blurVPipeline.IsValid())
            {
                Debug::LogError("ForwardRenderPipeline: failed to create bloom pipelines");
                DestroyBloomGpuResources(device, resources);
                return false;
            }

            resources.initialized = true;
            return true;
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
                                  std::shared_ptr<SkyGpuResources> skyResources,
                                  std::shared_ptr<BloomGpuResources> bloomResources,
                                  std::shared_ptr<ForwardPlusGpuResources> forwardPlusResources,
                                  std::shared_ptr<ForwardRendererMode> effectiveMode,
                                  std::shared_ptr<ForwardPlusCullingMode> effectiveCullingMode)
                : m_config(config)
                , m_skyResources(std::move(skyResources))
                , m_bloomResources(std::move(bloomResources))
                , m_forwardPlusResources(std::move(forwardPlusResources))
                , m_effectiveMode(std::move(effectiveMode))
                , m_effectiveCullingMode(std::move(effectiveCullingMode))
            {
            }

            std::string_view GetName() const noexcept override
            {
                return *m_effectiveMode == ForwardRendererMode::ForwardPlus
                    ? "forward_plus"
                    : "forward";
            }

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
                params.presentEnabled = context.presentOutput;
                const bool wantsShadow = context.renderQueue.activeShadowResolution > 0u;
                const bool hasShadowContributor = wantsShadow &&
                    std::any_of(context.passContributors.begin(), context.passContributors.end(),
                                [](const IPassContributor* c) {
                                    return c->DeclaresResource(StandardFrameResources::ShadowMap);
                                });
                params.shadowEnabled     = wantsShadow && !hasShadowContributor;
                params.shadowReadEnabled = hasShadowContributor;
                params.shadowMapSize     = wantsShadow
                    ? context.renderQueue.activeShadowResolution
                    : 2048u;
                params.skyEnabled = (context.backgroundMode == BackgroundMode::Skybox)
                    && m_config.enableEnvironmentBackground
                    && m_skyResources
                    && m_skyResources->initialized
                    && m_skyResources->pipeline.IsValid();
                params.bloomEnabled = context.enableBloom && m_config.enableBloom;
                params.transparentEnabled = true;
                params.debugDrawEnabled = context.externalCallbacks.Has(StandardFrameExecutors::DebugDraw);
                params.uiEnabled = context.presentOutput && (
                    context.externalCallbacks.Has(StandardFrameExecutors::UI)
                    || context.externalCallbacks.Has(StandardFrameExecutors::Present)
                    || std::any_of(context.passContributors.begin(), context.passContributors.end(),
                                   [](const IPassContributor* c) {
                                       return c->DeclaresResource(StandardFrameResources::UIOverlay);
                                   }));
                params.clearColorValue = context.clearColor;
                params.gtaoEnabled = context.enableAmbientOcclusion && std::any_of(
                    context.passContributors.begin(), context.passContributors.end(),
                    [](const IPassContributor* c) {
                        return c->DeclaresResource("GTAOComposite");
                    });

                FramePipelineCallbacks callbacks = context.externalCallbacks;

                auto runtime = context.runtimeBindings;

                auto executeDrawList = std::make_shared<std::function<void(const DrawList&, const rendergraph::RGExecContext&)>>(
                    [runtime](const DrawList& list, const rendergraph::RGExecContext& execCtx)
                    {
                        if (!execCtx.cmd || list.items.empty() || !runtime ||
                            !runtime->material.shaderRuntime || !runtime->material.materials)
                            return;

                        // materialBound: wurde BindMaterialWithRange mindestens einmal
                        // erfolgreich aufgerufen? Nur dann darf UpdatePerObjectBinding
                        // genutzt werden — das setzt voraus, dass eine Pipeline gebunden ist.
                        // BUG-FIX: lastMaterial startete als Invalid() (value=0). Hat das
                        // erste Item ebenfalls Invalid, lautete die Prüfung
                        //   item.material != lastMaterial  →  0 != 0  →  false
                        // → UpdatePerObjectBinding + DrawIndexed ohne Pipeline
                        // → VK_ERROR_DEVICE_LOST.
                        MaterialHandle lastMaterial  = MaterialHandle::Invalid();
                        bool           materialBound = false;
                        BufferHandle   lastVertexBuffer = BufferHandle::Invalid();
                        BufferHandle   lastIndexBuffer  = BufferHandle::Invalid();

                        for (const auto& item : list.items)
                        {
                            if (!item.hasGpuData())
                                continue;

                            const PerObjectConstants* perObjectConstants = nullptr;
                            if (runtime->scene.renderQueue && item.cbOffset < runtime->scene.renderQueue->objectConstants.size())
                                perObjectConstants = &runtime->scene.renderQueue->objectConstants[item.cbOffset];

                            BufferBinding perObjBinding{};
                            if (runtime->resources.perObjectArena.IsValid() && runtime->resources.perObjectStride > 0u)
                            {
                                perObjBinding = BufferBinding{
                                    runtime->resources.perObjectArena,
                                    item.cbOffset * runtime->resources.perObjectStride,
                                    static_cast<uint32_t>(sizeof(PerObjectConstants))
                                };
                            }

                            if (!materialBound || item.material != lastMaterial)
                            {
                                const MaterialSystemShaderMaterialSource materialSource(*runtime->material.materials);
                                if (!runtime->material.shaderRuntime->BindMaterialWithRange(*execCtx.cmd,
                                    materialSource,
                                    item.material,
                                    runtime->resources.perFrameCB,
                                    runtime->resources.perFrameBinding,
                                    perObjBinding,
                                    {},
                                    perObjectConstants,
                                    list.passId))
                                {
                                    // Binding fehlgeschlagen → Pipeline-Status unbekannt.
                                    // Beim nächsten Item neu versuchen statt lastMaterial
                                    // als "gebunden" zu markieren.
                                    materialBound = false;
                                    lastMaterial  = MaterialHandle::Invalid();
                                    continue;
                                }
                                lastMaterial  = item.material;
                                materialBound = true;
                            }
                            else
                            {
                                runtime->material.shaderRuntime->UpdatePerObjectBinding(*execCtx.cmd,
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
                            if (item.boneBuffer.IsValid())
                                execCtx.cmd->SetShaderResource(BufSRVSlots::BonePalette,
                                                               item.boneBuffer,
                                                               ShaderStageMask::Vertex);
                            execCtx.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                        }
                    });
                auto executeSceneDrawList = std::make_shared<std::function<void(const DrawList&, const rendergraph::RGExecContext&)>>(
                    [executeDrawList](const DrawList& list, const rendergraph::RGExecContext& execCtx)
                    {
                        if (list.items.empty())
                            return;

                        DrawList sceneList{};
                        sceneList.passId = list.passId;
                        sceneList.sorted = list.sorted;
                        sceneList.items.reserve(list.items.size());
                        for (const DrawItem& item : list.items)
                        {
                            if ((item.layerMask & LAYER_EDITOR_GIZMO) != 0u)
                                continue;
                            sceneList.items.push_back(item);
                        }
                        if (!sceneList.items.empty())
                            (*executeDrawList)(sceneList, execCtx);
                    });

                struct PipelineState
                {
                    StandardFrameBuildResult resources{};
                    bool ready = false;
                };
                auto pipelineState = std::make_shared<PipelineState>();

                EnsureSkyResources(context);
                EnsureBloomResources(context);

                if (!callbacks.Has(StandardFrameExecutors::Sky))
                {
                    auto runtimeSky = runtime;
                    auto skyResources = m_skyResources;
                    callbacks.Register(StandardFrameExecutors::Sky,
                        [runtimeSky, skyResources](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !runtimeSky || !runtimeSky->material.shaderRuntime || !skyResources)
                                return;

                            const EnvironmentRuntimeState& environmentState =
                                runtimeSky->material.shaderRuntime->GetEnvironmentState();
                            if (!environmentState.environment.IsValid() || !skyResources->pipeline.IsValid())
                                return;

                            execCtx.cmd->SetPipeline(skyResources->pipeline);
                            if (runtimeSky->resources.perFrameCB.IsValid())
                                execCtx.cmd->SetConstantBuffer(CBSlots::PerFrame, runtimeSky->resources.perFrameCB, ShaderStageMask::Fragment);
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
                        [runtime, executeSceneDrawList, pipelineState, this,
                         viewportWidth  = context.viewportWidth,
                         viewportHeight = context.viewportHeight](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->scene.renderQueue || !pipelineState->ready)
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

                            // Screen-space ambient occlusion from the previous frame
                            // (GTAO Option B). The lit shader samples this at PassSRV0
                            // and attenuates only the ambient/indirect term. Invalid
                            // until GTAO has produced at least one frame.
                            if (runtime->resources.ambientOcclusion.IsValid())
                            {
                                execCtx.cmd->TransitionResource(runtime->resources.ambientOcclusion,
                                    ResourceState::Unknown, ResourceState::ShaderRead);
                                execCtx.cmd->SetShaderResource(TexSlots::PassSRV0,
                                    runtime->resources.ambientOcclusion, ShaderStageMask::Fragment);
                                execCtx.cmd->SetSampler(SamplerSlots::LinearClamp,
                                    SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            }

                            if (*m_effectiveMode == ForwardRendererMode::ForwardPlus &&
                                runtime->scene.frameData.featureRegistry &&
                                runtime->scene.perFrameConstantsData &&
                                m_forwardPlusResources &&
                                execCtx.device)
                            {
                                bool forwardPlusReady = false;
                                const auto* lighting =
                                    runtime->scene.frameData.GetFrameData<engine::addons::lighting::LightingFrameData>();
                                const BufferHandle lightBuffer = lighting
                                    ? lighting->gpu.lightBuffer
                                    : BufferHandle::Invalid();

                                if (*m_effectiveCullingMode == ForwardPlusCullingMode::Compute)
                                {
                                    forwardPlusReady = lightBuffer.IsValid() &&
                                        PrepareForwardPlusComputeResources(*execCtx.device,
                                                                           runtime->scene.frameData,
                                                                           *runtime->scene.perFrameConstantsData,
                                                                           viewportWidth,
                                                                           viewportHeight,
                                                                           *m_forwardPlusResources) &&
                                        m_forwardPlusResources->computePipeline.IsValid() &&
                                        runtime->resources.perFrameCB.IsValid();
                                    if (forwardPlusReady)
                                    {
                                        // Per-Frame-Reset der Tile-Header (count/reserved0 → 0) im main
                                        // Command Buffer via CopyBuffer statt UploadBufferData + vkQueueWaitIdle.
                                        const uint32_t tileCount = m_forwardPlusResources->tileCountX
                                                                  * m_forwardPlusResources->tileCountY;
                                        const uint64_t headerBytes = static_cast<uint64_t>(tileCount)
                                                                   * sizeof(ForwardPlusTileHeaderGpu);
                                        execCtx.cmd->TransitionResource(
                                            m_forwardPlusResources->tileHeaderBuffer,
                                            ResourceState::ShaderRead,
                                            ResourceState::CopyDest);
                                        execCtx.cmd->CopyBuffer(
                                            m_forwardPlusResources->tileHeaderBuffer, 0u,
                                            m_forwardPlusResources->resetStagingBuffer, 0u,
                                            headerBytes);
                                        execCtx.cmd->TransitionResource(
                                            m_forwardPlusResources->tileHeaderBuffer,
                                            ResourceState::CopyDest,
                                            ResourceState::UnorderedAccess);
                                        execCtx.cmd->TransitionResource(
                                            m_forwardPlusResources->tileIndexBuffer,
                                            ResourceState::ShaderRead,
                                            ResourceState::UnorderedAccess);

                                        execCtx.cmd->SetPipeline(m_forwardPlusResources->computePipeline);
                                        execCtx.cmd->SetConstantBuffer(CBSlots::PerFrame,
                                                                       runtime->resources.perFrameCB,
                                                                       ShaderStageMask::Compute);
                                        execCtx.cmd->SetConstantBuffer(CBSlots::PerPass,
                                                                       m_forwardPlusResources->computeParamsBuffer,
                                                                       ShaderStageMask::Compute);
                                        execCtx.cmd->SetShaderResource(BufSRVSlots::LightBuffer,
                                                                       lightBuffer,
                                                                       ShaderStageMask::Compute);
                                        execCtx.cmd->SetUnorderedAccess(UAVSlots::Output0,
                                                                        m_forwardPlusResources->tileHeaderBuffer,
                                                                        ShaderStageMask::Compute);
                                        execCtx.cmd->SetUnorderedAccess(UAVSlots::Output1,
                                                                        m_forwardPlusResources->tileIndexBuffer,
                                                                        ShaderStageMask::Compute);

                                        const uint32_t lightCount = lighting ? lighting->gpu.lightCount : 0u;
                                        const uint32_t groupsX = std::max(1u, (lightCount + 63u) / 64u);
                                        execCtx.cmd->Dispatch(groupsX, 1u, 1u);
                                        execCtx.cmd->SetUnorderedAccess(UAVSlots::Output0,
                                                                        BufferHandle::Invalid(),
                                                                        ShaderStageMask::Compute);
                                        execCtx.cmd->SetUnorderedAccess(UAVSlots::Output1,
                                                                        BufferHandle::Invalid(),
                                                                        ShaderStageMask::Compute);
                                        execCtx.cmd->TransitionResource(
                                            m_forwardPlusResources->tileHeaderBuffer,
                                            ResourceState::UnorderedAccess,
                                            ResourceState::ShaderRead);
                                        execCtx.cmd->TransitionResource(
                                            m_forwardPlusResources->tileIndexBuffer,
                                            ResourceState::UnorderedAccess,
                                            ResourceState::ShaderRead);
                                    }
                                }
                                else
                                {
                                    forwardPlusReady = UpdateForwardPlusResources(*execCtx.device,
                                                                                  runtime->scene.frameData,
                                                                                  *runtime->scene.perFrameConstantsData,
                                                                                  viewportWidth,
                                                                                  viewportHeight,
                                                                                  *m_forwardPlusResources);
                                }

                                if (forwardPlusReady)
                                {
                                    if (lightBuffer.IsValid())
                                        execCtx.cmd->SetShaderResource(BufSRVSlots::LightBuffer,
                                                                       lightBuffer,
                                                                       ShaderStageMask::Fragment);
                                    if (m_forwardPlusResources->tileHeaderBuffer.IsValid())
                                        execCtx.cmd->SetShaderResource(BufSRVSlots::TileHeaders,
                                                                       m_forwardPlusResources->tileHeaderBuffer,
                                                                       ShaderStageMask::Fragment);
                                    if (m_forwardPlusResources->tileIndexBuffer.IsValid())
                                        execCtx.cmd->SetShaderResource(BufSRVSlots::LightIndices,
                                                                       m_forwardPlusResources->tileIndexBuffer,
                                                                       ShaderStageMask::Fragment);

                                    if (++m_metricsLogFrameCounter >= 300u)
                                    {
                                        m_metricsLogFrameCounter = 0u;
                                        const auto& met = m_forwardPlusResources->metrics;
                                        Debug::LogVerbose("ForwardPlus tiles=%u lights/tile: min=%u avg=%.1f max=%u total=%u",
                                                          met.tileCount,
                                                          met.minLightsPerTile,
                                                          met.avgLightsPerTile,
                                                          met.maxLightsPerTile,
                                                          met.totalLightAssignments);
                                    }
                                }
                            }

                            const DrawList* list = runtime->scene.renderQueue->FindList(StandardRenderPasses::Opaque());
                            if (list)
                                (*executeSceneDrawList)(*list, execCtx);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Transparent))
                {
                    callbacks.Register(StandardFrameExecutors::Transparent,
                        [runtime, executeSceneDrawList](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->scene.renderQueue)
                                return;
                            const DrawList* list = runtime->scene.renderQueue->FindList(StandardRenderPasses::Transparent());
                            if (list)
                                (*executeSceneDrawList)(*list, execCtx);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Present))
                {
                    callbacks.Register(StandardFrameExecutors::Present,
                        [](const rendergraph::RGExecContext& /*execCtx*/) {});
                }

                if (!callbacks.Has(StandardFrameExecutors::BloomExtract))
                {
                    auto state = pipelineState;
                    auto bloomResources = m_bloomResources;
                    const BloomPassConstants bloomConstants{
                        std::max(0.0f, m_config.bloomThreshold),
                        std::max(0.0f, m_config.bloomIntensity),
                        std::max(0.0f, m_config.bloomBlurRadius),
                        0.0f
                    };
                    callbacks.Register(StandardFrameExecutors::BloomExtract,
                        [state, bloomResources, bloomConstants](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !execCtx.device || !state->ready || !bloomResources ||
                                !bloomResources->initialized || !bloomResources->extractPipeline.IsValid())
                                return;

                            const TextureHandle sourceTex = execCtx.GetTexture(state->resources.hdrSceneColor);
                            if (!sourceTex.IsValid())
                                return;

                            execCtx.device->UploadBufferData(bloomResources->perPassCB, &bloomConstants, sizeof(bloomConstants));
                            execCtx.cmd->SetPipeline(bloomResources->extractPipeline);
                            execCtx.cmd->SetConstantBuffer(CBSlots::PerPass, bloomResources->perPassCB, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::BloomBlurH))
                {
                    auto state = pipelineState;
                    auto bloomResources = m_bloomResources;
                    const BloomPassConstants bloomConstants{
                        std::max(0.0f, m_config.bloomThreshold),
                        std::max(0.0f, m_config.bloomIntensity),
                        std::max(0.0f, m_config.bloomBlurRadius),
                        0.0f
                    };
                    callbacks.Register(StandardFrameExecutors::BloomBlurH,
                        [state, bloomResources, bloomConstants](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !execCtx.device || !state->ready || !bloomResources ||
                                !bloomResources->initialized || !bloomResources->blurHPipeline.IsValid())
                                return;

                            const TextureHandle sourceTex = execCtx.GetTexture(state->resources.bloomExtracted);
                            if (!sourceTex.IsValid())
                                return;

                            execCtx.device->UploadBufferData(bloomResources->perPassCB, &bloomConstants, sizeof(bloomConstants));
                            execCtx.cmd->SetPipeline(bloomResources->blurHPipeline);
                            execCtx.cmd->SetConstantBuffer(CBSlots::PerPass, bloomResources->perPassCB, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::BloomBlurV))
                {
                    auto state = pipelineState;
                    auto bloomResources = m_bloomResources;
                    const BloomPassConstants bloomConstants{
                        std::max(0.0f, m_config.bloomThreshold),
                        std::max(0.0f, m_config.bloomIntensity),
                        std::max(0.0f, m_config.bloomBlurRadius),
                        0.0f
                    };
                    callbacks.Register(StandardFrameExecutors::BloomBlurV,
                        [state, bloomResources, bloomConstants](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !execCtx.device || !state->ready || !bloomResources ||
                                !bloomResources->initialized || !bloomResources->blurVPipeline.IsValid())
                                return;

                            const TextureHandle sourceTex = execCtx.GetTexture(state->resources.bloomBlurH);
                            if (!sourceTex.IsValid())
                                return;

                            execCtx.device->UploadBufferData(bloomResources->perPassCB, &bloomConstants, sizeof(bloomConstants));
                            execCtx.cmd->SetPipeline(bloomResources->blurVPipeline);
                            execCtx.cmd->SetConstantBuffer(CBSlots::PerPass, bloomResources->perPassCB, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Tonemap) && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem)
                {
                    auto state = pipelineState;
                    auto runtimeTonemap = runtime;
                    auto bloomResources = m_bloomResources;
                    const BloomPassConstants bloomConstants{
                        std::max(0.0f, m_config.bloomThreshold),
                        std::max(0.0f, m_config.bloomIntensity),
                        std::max(0.0f, m_config.bloomBlurRadius),
                        0.0f
                    };
                    callbacks.Register(StandardFrameExecutors::Tonemap,
                        [state, runtimeTonemap, bloomResources, bloomConstants](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready)
                                return;
                            // GTAO (Option B) bakes AO into HDR in the lit pass, then
                            // copies HDR into GTAOComposite (the overlay/scene buffer).
                            // Read that when present; otherwise the raw HDR colour.
                            const rendergraph::RGResourceID tonemapSrcId =
                                (state->resources.gtaoComposite != rendergraph::RG_INVALID_RESOURCE)
                                ? state->resources.gtaoComposite
                                : state->resources.hdrSceneColor;
                            TextureHandle sourceTex = execCtx.GetTexture(tonemapSrcId);
                            if (!sourceTex.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap source texture invalid");
                                return;
                            }
                            const MaterialHandle material = runtimeTonemap ? runtimeTonemap->material.defaultTonemapMaterial
                                                                           : MaterialHandle::Invalid();
                            if (!runtimeTonemap || !runtimeTonemap->material.shaderRuntime ||
                                !runtimeTonemap->material.tonemapMaterialSystem || !material.IsValid())
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap runtime/material invalid");
                                return;
                            }

                            const MaterialSystemShaderMaterialSource tonemapMaterialSource(*runtimeTonemap->material.tonemapMaterialSystem);
                            const bool bloomAvailable =
                                state->resources.bloomBlurV != rendergraph::RG_INVALID_RESOURCE &&
                                bloomResources &&
                                bloomResources->perPassCB.IsValid();
                            const TextureHandle bloomTex = bloomAvailable
                                ? execCtx.GetTexture(state->resources.bloomBlurV)
                                : TextureHandle::Invalid();
                            const BufferHandle perPassCB = bloomAvailable
                                ? bloomResources->perPassCB
                                : BufferHandle::Invalid();
                            if (bloomAvailable && execCtx.device)
                                execCtx.device->UploadBufferData(bloomResources->perPassCB, &bloomConstants, sizeof(bloomConstants));
                            if (!runtimeTonemap->material.shaderRuntime->BindMaterial(*execCtx.cmd,
                                tonemapMaterialSource,
                                material,
                                BufferHandle::Invalid(),
                                BufferHandle::Invalid(),
                                perPassCB,
                                StandardRenderPasses::Postprocess()))
                            {
                                Debug::LogError("ForwardRenderPipeline: tonemap material bind failed");
                                return;
                            }

                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::BloomTexture, bloomTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }


                // Contributors tragen Ressourcen, Passes und Executoren ein.
                // drawObjects kapselt die generische per-Objekt-Zeichenlogik dieser Pipeline.
                FrameRecipe preContributorRecipe{};
                FrameRecipe postContributorRecipe{};
                if (!context.passContributors.empty())
                {
                    auto drawObjectsFn = std::function<void(const DrawList&, const rendergraph::RGExecContext&)>(
                        [executeDrawList](const DrawList& list, const rendergraph::RGExecContext& ec)
                        {
                            (*executeDrawList)(list, ec);
                        });
                    const PassBuildContext::PassFrameView passFrame{
                        context.viewportWidth,
                        context.viewportHeight,
                        &context.renderQueue,
                        context.runtimeBindings,
                        context.enableAmbientOcclusion
                    };
                    for (const IPassContributor* c : context.passContributors)
                    {
                        if (c->GetPhase() == PassPhase::BeforeOpaque)
                        {
                            PassBuildContext pbc{passFrame, preContributorRecipe, callbacks, drawObjectsFn};
                            c->BuildPass(pbc);
                        }
                        else if (c->GetPhase() == PassPhase::AfterOpaque)
                        {
                            PassBuildContext pbc{passFrame, postContributorRecipe, callbacks, drawObjectsFn};
                            c->BuildPass(pbc);
                        }
                    }
                }

                const FrameRecipe* prePasses = (preContributorRecipe.resources.empty() &&
                                                preContributorRecipe.passes.empty())
                    ? nullptr : &preContributorRecipe;
                const FrameRecipe* postPasses = (postContributorRecipe.resources.empty() &&
                                                 postContributorRecipe.passes.empty())
                    ? nullptr : &postContributorRecipe;

                const StandardFrameBuildResult builtResources =
                    StandardFrameRecipeBuilder::Build(context.renderGraph, params, callbacks,
                                                      prePasses, postPasses);
                result.backbuffer = builtResources.backbuffer;
                pipelineState->resources = builtResources;
                pipelineState->ready = true;
                return true;
            }

            void OnDeviceShutdown() noexcept override
            {
                if (m_device && m_skyResources)
                    DestroySkyGpuResources(*m_device, *m_skyResources);
                if (m_device && m_bloomResources)
                    DestroyBloomGpuResources(*m_device, *m_bloomResources);
                if (m_device && m_forwardPlusResources)
                    DestroyForwardPlusGpuResources(*m_device, *m_forwardPlusResources);
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

            void EnsureBloomResources(const RenderPipelineBuildContext& context) const
            {
                if (!m_config.enableBloom || !m_bloomResources || m_bloomResources->initialized)
                    return;
                m_device = context.shaderRuntime.GetDevice();
                if (!m_device)
                {
                    Debug::LogWarning("ForwardRenderPipeline: bloom requested but device unavailable");
                    return;
                }
                (void)InitializeBloomGpuResources(*m_device, *m_bloomResources);
            }

            ForwardFeatureConfig m_config;
            mutable std::shared_ptr<SkyGpuResources> m_skyResources = std::make_shared<SkyGpuResources>();
            mutable std::shared_ptr<BloomGpuResources> m_bloomResources = std::make_shared<BloomGpuResources>();
            mutable std::shared_ptr<ForwardPlusGpuResources> m_forwardPlusResources = std::make_shared<ForwardPlusGpuResources>();
            std::shared_ptr<ForwardRendererMode>    m_effectiveMode;
            std::shared_ptr<ForwardPlusCullingMode> m_effectiveCullingMode;
            mutable IDevice* m_device = nullptr;
            mutable uint32_t m_metricsLogFrameCounter = 0u;
        };

        class ForwardFeature final : public IEngineFeature
        {
        public:
            explicit ForwardFeature(ForwardFeatureConfig config)
                : m_skyResources(std::make_shared<SkyGpuResources>())
                , m_bloomResources(std::make_shared<BloomGpuResources>())
                , m_forwardPlusResources(std::make_shared<ForwardPlusGpuResources>())
                , m_pipelineConfig(config)
                , m_effectiveMode(std::make_shared<ForwardRendererMode>(config.mode))
                , m_effectiveCullingMode(std::make_shared<ForwardPlusCullingMode>(config.cullingMode))
                , m_pipeline(std::make_shared<ForwardRenderPipeline>(
                      config,
                      m_skyResources,
                      m_bloomResources,
                      m_forwardPlusResources,
                      m_effectiveMode,
                      m_effectiveCullingMode))
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
                // Forward+ auf OpenGL nicht unterstützt → einmalig Fallback setzen
                if (*m_effectiveMode == ForwardRendererMode::ForwardPlus &&
                    context.device.GetShaderTargetProfile() == assets::ShaderTargetProfile::OpenGL_GLSL450)
                {
                    Debug::LogWarning("ForwardFeature: Forward+ not supported on OpenGL, using Forward");
                    *m_effectiveMode = ForwardRendererMode::Forward;
                }

                if (*m_effectiveMode == ForwardRendererMode::ForwardPlus &&
                    *m_effectiveCullingMode == ForwardPlusCullingMode::Compute)
                {
                    const auto computeRuntime = context.device.GetComputeRuntime();
                    if (!computeRuntime.computePipelinesSupported || !computeRuntime.computeDispatchSupported)
                    {
                        Debug::LogWarning("ForwardFeature: Compute tile culling not supported by backend, using CPU culling");
                        *m_effectiveCullingMode = ForwardPlusCullingMode::CPU;
                    }
                    else if (!EnsureForwardPlusComputePipeline(context.device, *m_forwardPlusResources))
                    {
                        Debug::LogWarning("ForwardFeature: Compute tile culling pipeline setup failed, using CPU culling");
                        *m_effectiveCullingMode = ForwardPlusCullingMode::CPU;
                    }
                }

                if (m_skyResources && !m_skyResources->initialized && m_pipelineConfig.enableEnvironmentBackground)
                {
                    if (!InitializeSkyGpuResources(context.device, *m_skyResources))
                        return false;
                }
                if (m_bloomResources && !m_bloomResources->initialized && m_pipelineConfig.enableBloom)
                {
                    if (!InitializeBloomGpuResources(context.device, *m_bloomResources))
                        return false;
                }
                return true;
            }

            void Shutdown(const FeatureShutdownContext& context) override
            {
                (void)context;
                m_pipeline.reset();
                m_skyResources.reset();
                m_bloomResources.reset();
                m_forwardPlusResources.reset();
            }

        private:
            ForwardFeatureConfig m_pipelineConfig{};
            std::shared_ptr<SkyGpuResources>         m_skyResources;
            std::shared_ptr<BloomGpuResources>       m_bloomResources;
            std::shared_ptr<ForwardPlusGpuResources>  m_forwardPlusResources;
            std::shared_ptr<ForwardRendererMode>      m_effectiveMode;
            std::shared_ptr<ForwardPlusCullingMode>   m_effectiveCullingMode;
            RenderPipelinePtr                         m_pipeline;
        };

    } // namespace

    std::unique_ptr<IEngineFeature> CreateForwardFeature(ForwardFeatureConfig config)
    {
        return std::make_unique<ForwardFeature>(config);
    }

} // namespace engine::renderer::addons::forward
