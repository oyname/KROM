#include "ForwardFeature.hpp"
#include "addons/forward/ForwardAutoExposure.hpp"
#include "addons/forward/ForwardPlusLighting.hpp"
#include "addons/forward/ForwardSmaa.hpp"
#include "addons/forward/ForwardSceneBindings.hpp"
#include "addons/forward/StandardFramePipeline.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/FrameRingBuffer.hpp"
#include "renderer/RenderFrameConstants.hpp"
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
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace engine::renderer::addons::forward {
    namespace {

        bool CanUseStructuredPerObject(const DrawItem& item,
                                       RenderPassID passId,
                                       const MaterialSystem& materials,
                                       BufferHandle perObjectStructured) noexcept
        {
            (void)passId;
            if (!perObjectStructured.IsValid())
                return false;
            const ShaderVariantFlag flags = MaterialRuntimeBridge::BuildShaderVariantFlags(materials, item.material);
            return HasFlag(flags, ShaderVariantFlag::Instanced);
        }

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

        // MSAA resolve: luminance-weighted average (1 / (1 + luma)) so a single very
        // bright sample does not dominate an edge, plus the depth of sample 0 so
        // later passes (GTAO, overlays, particles) find a single-sample depth.
        constexpr const char* kMsaaResolvePsHlsl = R"(
Texture2DMS<float4> uSceneColorMS : register(t8);
Texture2DMS<float>  uSceneDepthMS : register(t9);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSOutput
{
    float4 color : SV_TARGET;
    float  depth : SV_DEPTH;
};

PSOutput main(PSInput IN)
{
    uint width, height, samples;
    uSceneColorMS.GetDimensions(width, height, samples);
    const int2 pixel = int2(IN.pos.xy);

    float3 weighted = 0.0f;
    float  weightSum = 0.0f;
    float  alpha = 0.0f;
    [loop]
    for (uint i = 0u; i < samples; ++i)
    {
        const float4 c = uSceneColorMS.Load(pixel, i);
        const float luma = dot(max(c.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
        const float w = 1.0f / (1.0f + luma);
        weighted += c.rgb * w;
        weightSum += w;
        alpha += c.a;
    }

    PSOutput o;
    o.color = float4(weighted / max(weightSum, 1e-6f), alpha / max(float(samples), 1.0f));
    o.depth = uSceneDepthMS.Load(pixel, 0);
    return o;
})";

        // FXAA after the tonemap pass (own implementation of the FXAA 3 quality
        // algorithm: local contrast test, edge direction, search along the edge for
        // both ends, sub-pixel blend). The input is an sRGB texture, so samples are
        // linear; luma uses sqrt as a cheap perceptual curve.
        constexpr const char* kFxaaPsHlsl = R"(
Texture2D    uInput   : register(t8);
SamplerState uSampler : register(s1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

static const float kEdgeThreshold    = 0.125f;
static const float kEdgeThresholdMin = 0.0312f;
static const float kSubpixQuality    = 0.75f;
static const uint  kSearchSteps      = 12u;
static const float kStepSize[12] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f };

float Luma(float3 c)
{
    return sqrt(max(dot(c, float3(0.299f, 0.587f, 0.114f)), 0.0f));
}

float LumaAt(float2 uv)
{
    return Luma(uInput.SampleLevel(uSampler, uv, 0.0f).rgb);
}

float4 main(PSInput IN) : SV_TARGET
{
    float w, h;
    uInput.GetDimensions(w, h);
    const float2 texel = 1.0f / float2(w, h);
    // Pixel position instead of the interpolated uv: same on every backend.
    const float2 uv = IN.pos.xy * texel;

    const float4 center = uInput.SampleLevel(uSampler, uv, 0.0f);
    const float lumaM = Luma(center.rgb);
    const float lumaN = LumaAt(uv + float2(0.0f, -texel.y));
    const float lumaS = LumaAt(uv + float2(0.0f,  texel.y));
    const float lumaW = LumaAt(uv + float2(-texel.x, 0.0f));
    const float lumaE = LumaAt(uv + float2( texel.x, 0.0f));

    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range = lumaMax - lumaMin;
    if (range < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold))
        return center;

    const float lumaNW = LumaAt(uv + float2(-texel.x, -texel.y));
    const float lumaNE = LumaAt(uv + float2( texel.x, -texel.y));
    const float lumaSW = LumaAt(uv + float2(-texel.x,  texel.y));
    const float lumaSE = LumaAt(uv + float2( texel.x,  texel.y));

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float edgeH = abs(-2.0f * lumaW + lumaNW + lumaSW)
                      + abs(-2.0f * lumaM + lumaNS) * 2.0f
                      + abs(-2.0f * lumaE + lumaNE + lumaSE);
    const float edgeV = abs(-2.0f * lumaN + lumaNW + lumaNE)
                      + abs(-2.0f * lumaM + lumaWE) * 2.0f
                      + abs(-2.0f * lumaS + lumaSW + lumaSE);
    const bool horizontal = edgeH >= edgeV;

    // Neighbours across the edge; step towards the one with the larger gradient.
    const float luma1 = horizontal ? lumaN : lumaW;
    const float luma2 = horizontal ? lumaS : lumaE;
    const float grad1 = luma1 - lumaM;
    const float grad2 = luma2 - lumaM;
    const bool  side1 = abs(grad1) >= abs(grad2);
    const float gradScaled = 0.25f * max(abs(grad1), abs(grad2));

    float stepLength = horizontal ? texel.y : texel.x;
    float lumaLocalAvg;
    if (side1)
    {
        stepLength = -stepLength;
        lumaLocalAvg = 0.5f * (luma1 + lumaM);
    }
    else
    {
        lumaLocalAvg = 0.5f * (luma2 + lumaM);
    }

    // Walk along the edge, half a pixel towards the chosen side.
    float2 edgeUv = uv;
    if (horizontal) edgeUv.y += stepLength * 0.5f;
    else            edgeUv.x += stepLength * 0.5f;
    const float2 along = horizontal ? float2(texel.x, 0.0f) : float2(0.0f, texel.y);

    float2 uv1 = edgeUv - along * kStepSize[0];
    float2 uv2 = edgeUv + along * kStepSize[0];
    float end1 = LumaAt(uv1) - lumaLocalAvg;
    float end2 = LumaAt(uv2) - lumaLocalAvg;
    bool reached1 = abs(end1) >= gradScaled;
    bool reached2 = abs(end2) >= gradScaled;
    [loop]
    for (uint i = 1u; i < kSearchSteps && !(reached1 && reached2); ++i)
    {
        if (!reached1)
        {
            uv1 -= along * kStepSize[i];
            end1 = LumaAt(uv1) - lumaLocalAvg;
            reached1 = abs(end1) >= gradScaled;
        }
        if (!reached2)
        {
            uv2 += along * kStepSize[i];
            end2 = LumaAt(uv2) - lumaLocalAvg;
            reached2 = abs(end2) >= gradScaled;
        }
    }

    const float dist1 = horizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    const float dist2 = horizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    const bool  nearer1 = dist1 < dist2;
    const float edgeLength = max(dist1 + dist2, 1e-6f);
    const float pixelOffset = -min(dist1, dist2) / edgeLength + 0.5f;
    // Only blend when the luma at the nearer end varies the right way.
    const bool centerSmaller = lumaM < lumaLocalAvg;
    const bool correctVariation = ((nearer1 ? end1 : end2) < 0.0f) != centerSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0f;

    // Sub-pixel aliasing (thin lines, single bright pixels).
    const float lumaAvg = (2.0f * (lumaNS + lumaWE) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0f;
    const float sub1 = saturate(abs(lumaAvg - lumaM) / range);
    const float sub2 = (-2.0f * sub1 + 3.0f) * sub1 * sub1;
    finalOffset = max(finalOffset, sub2 * sub2 * kSubpixQuality);

    float2 finalUv = uv;
    if (horizontal) finalUv.y += finalOffset * stepLength;
    else            finalUv.x += finalOffset * stepLength;
    return float4(uInput.SampleLevel(uSampler, finalUv, 0.0f).rgb, center.a);
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
            PipelineHandle pipeline2x = PipelineHandle::Invalid();   // MSAA scene target
            PipelineHandle pipeline4x = PipelineHandle::Invalid();
            BufferHandle perMaterialCB = BufferHandle::Invalid();
            bool initialized = false;

            [[nodiscard]] PipelineHandle ForSamples(uint32_t samples) const noexcept
            {
                return samples >= 4u ? pipeline4x : (samples == 2u ? pipeline2x : pipeline);
            }
        };

        struct MsaaResolveGpuResources
        {
            ShaderHandle vertexShader = ShaderHandle::Invalid();
            ShaderHandle fragmentShader = ShaderHandle::Invalid();
            PipelineHandle pipeline = PipelineHandle::Invalid();
            bool attempted = false;
        };

        // Post-AA pipelines. FXAA and the SMAA blend pass write the output and are
        // rebuilt when its format changes; the SMAA edge/weight pipelines have fixed
        // formats. Shaders and lookup textures are created on first use per mode.
        struct PostAAGpuResources
        {
            ShaderHandle vertexShader = ShaderHandle::Invalid();
            ShaderHandle fxaaShader = ShaderHandle::Invalid();
            PipelineHandle fxaaPipeline = PipelineHandle::Invalid();
            Format fxaaFormat = Format::Unknown;
            bool shadersAttempted = false;

            ShaderHandle smaaEdgeShader = ShaderHandle::Invalid();
            ShaderHandle smaaWeightShader = ShaderHandle::Invalid();
            ShaderHandle smaaBlendShader = ShaderHandle::Invalid();
            PipelineHandle smaaEdgePipeline = PipelineHandle::Invalid();
            PipelineHandle smaaWeightPipeline = PipelineHandle::Invalid();
            PipelineHandle smaaBlendPipeline = PipelineHandle::Invalid();
            Format smaaBlendFormat = Format::Unknown;
            TextureHandle smaaAreaTex = TextureHandle::Invalid();
            TextureHandle smaaSearchTex = TextureHandle::Invalid();
            bool smaaAttempted = false;
        };

        void DestroyPostAAGpuResources(IDevice& device, PostAAGpuResources& res)
        {
            for (PipelineHandle* pipeline : { &res.fxaaPipeline, &res.smaaEdgePipeline,
                                              &res.smaaWeightPipeline, &res.smaaBlendPipeline })
            {
                if (pipeline->IsValid())
                    device.DestroyPipeline(*pipeline);
            }
            for (ShaderHandle* shader : { &res.vertexShader, &res.fxaaShader, &res.smaaEdgeShader,
                                          &res.smaaWeightShader, &res.smaaBlendShader })
            {
                if (shader->IsValid())
                    device.DestroyShader(*shader);
            }
            for (TextureHandle* texture : { &res.smaaAreaTex, &res.smaaSearchTex })
            {
                if (texture->IsValid())
                    device.DestroyTexture(*texture);
            }
            res = PostAAGpuResources{};
        }

        // Automatic exposure (ForwardAutoExposure.hpp): two compute pipelines, the
        // histogram and state buffers (GPU only, kept across frames), the per-frame
        // constants and a few CPU-readable copies of the state for the display.
        struct AutoExposureGpuResources
        {
            static constexpr uint32_t kReadbackCopies = 4u;   // more than frames in flight

            ShaderHandle histogramShader = ShaderHandle::Invalid();
            ShaderHandle adaptShader = ShaderHandle::Invalid();
            PipelineHandle histogramPipeline = PipelineHandle::Invalid();
            PipelineHandle adaptPipeline = PipelineHandle::Invalid();
            BufferHandle histogram = BufferHandle::Invalid();
            BufferHandle state = BufferHandle::Invalid();
            FrameRingBuffer constants;
            std::array<BufferHandle, kReadbackCopies> readback{};
            std::array<bool, kReadbackCopies> readbackWritten{};
            uint32_t readbackIndex = 0u;
            bool attempted = false;
            bool ready = false;

            // Adaptation bookkeeping (CPU side).
            bool measured = false;              // false: the next measurement jumps to its target
            uint32_t resetSerial = 0u;
            std::chrono::steady_clock::time_point lastTime{};
            bool hasLastTime = false;
        };

        struct AutoExposureViewState
        {
            std::shared_ptr<AutoExposureGpuResources> resources = std::make_shared<AutoExposureGpuResources>();
            RenderTargetHandle outputRT;
            bool enabled = false;
        };

        void DestroyAutoExposureGpuResources(IDevice& device, AutoExposureGpuResources& res)
        {
            for (PipelineHandle* pipeline : { &res.histogramPipeline, &res.adaptPipeline })
                if (pipeline->IsValid())
                    device.DestroyPipeline(*pipeline);
            for (ShaderHandle* shader : { &res.histogramShader, &res.adaptShader })
                if (shader->IsValid())
                    device.DestroyShader(*shader);
            for (BufferHandle* buffer : { &res.histogram, &res.state })
                if (buffer->IsValid())
                    device.DestroyBuffer(*buffer);
            for (BufferHandle& buffer : res.readback)
                if (buffer.IsValid())
                    device.DestroyBuffer(buffer);
            res.constants.Release(&device);
            res.histogramShader = res.adaptShader = ShaderHandle::Invalid();
            res.histogramPipeline = res.adaptPipeline = PipelineHandle::Invalid();
            res.histogram = res.state = BufferHandle::Invalid();
            res.readback = {};
            res.readbackWritten = {};
            res.readbackIndex = 0u;
            res.attempted = false;
            res.ready = false;
            res.measured = false;
            res.hasLastTime = false;
        }

        [[nodiscard]] ShaderHandle CreateAutoExposureShader(IDevice& device, const char* source, const char* debugName)
        {
            assets::ShaderAsset shader{};
            shader.debugName = debugName;
            shader.stage = assets::ShaderStage::Compute;
            shader.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
            shader.entryPoint = "main";
            shader.sourceCode = source;
            assets::CompiledShaderArtifact compiled{};
            std::string error;
            if (!ShaderCompiler::CompileForTarget(shader, ShaderCompiler::ResolveTargetProfile(device), compiled, &error))
            {
                Debug::LogError("ForwardFeature.cpp: failed to compile auto-exposure shader '%s': %s",
                                debugName, error.c_str());
                return ShaderHandle::Invalid();
            }
            return compiled.bytecode.empty()
                ? device.CreateShaderFromSource(compiled.sourceText, ShaderStageMask::Compute,
                                                compiled.entryPoint.empty() ? "main" : compiled.entryPoint, debugName)
                : device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(),
                                                  ShaderStageMask::Compute, debugName);
        }

        [[nodiscard]] PipelineHandle CreateAutoExposurePipeline(IDevice& device, ShaderHandle shader, const char* debugName)
        {
            PipelineDesc desc{};
            desc.pipelineClass = PipelineClass::Compute;
            desc.debugName = debugName;
            desc.shaderStages.push_back(ShaderStageDesc{ shader, ShaderStageMask::Compute, "main" });
            return device.CreatePipeline(desc);
        }

        // Creates shaders, pipelines and buffers once. False on backends without
        // compute (OpenGL is frozen and keeps the fixed exposure).
        bool EnsureAutoExposureGpuResources(IDevice& device, AutoExposureGpuResources& res)
        {
            if (res.attempted)
                return res.ready;
            res.attempted = true;
            if (ShaderCompiler::ResolveTargetProfile(device) == assets::ShaderTargetProfile::OpenGL_GLSL450)
                return false;
            const ComputeRuntimeDesc compute = device.GetComputeRuntime();
            if (!compute.computePipelinesSupported || !compute.computeDispatchSupported)
                return false;

            res.histogramShader = CreateAutoExposureShader(device, auto_exposure::kHistogramCsHlsl, "ForwardAutoExposureHistogramCS");
            res.adaptShader = CreateAutoExposureShader(device, auto_exposure::kAdaptCsHlsl, "ForwardAutoExposureAdaptCS");
            if (res.histogramShader.IsValid())
                res.histogramPipeline = CreateAutoExposurePipeline(device, res.histogramShader, "ForwardAutoExposureHistogram_Pipeline");
            if (res.adaptShader.IsValid())
                res.adaptPipeline = CreateAutoExposurePipeline(device, res.adaptShader, "ForwardAutoExposureAdapt_Pipeline");

            const auto makeBuffer = [&device](uint32_t count, uint32_t stride, const char* name)
            {
                BufferDesc desc{};
                desc.byteSize = static_cast<uint64_t>(count) * stride;
                desc.stride = stride;
                desc.type = BufferType::Structured;
                desc.usage = ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess | ResourceUsage::CopyDest
                           | ResourceUsage::CopySource;
                desc.access = MemoryAccess::GpuOnly;
                desc.initialState = ResourceState::ShaderRead;
                desc.debugName = name;
                return device.CreateBuffer(desc);
            };
            res.histogram = makeBuffer(auto_exposure::kHistogramBins, sizeof(uint32_t), "ForwardAutoExposure_Histogram");
            res.state = makeBuffer(1u, sizeof(auto_exposure::StateGpu), "ForwardAutoExposure_State");
            if (res.histogram.IsValid())
            {
                const std::array<uint32_t, auto_exposure::kHistogramBins> zeros{};
                device.UploadBufferData(res.histogram, zeros.data(), sizeof(zeros));
            }
            if (res.state.IsValid())
            {
                const auto_exposure::StateGpu zero{};
                device.UploadBufferData(res.state, &zero, sizeof(zero));
            }

            res.ready = res.histogramPipeline.IsValid() && res.adaptPipeline.IsValid()
                     && res.histogram.IsValid() && res.state.IsValid();
            if (!res.ready)
            {
                Debug::LogError("ForwardFeature.cpp: auto-exposure resources failed, the fixed exposure is used");
                DestroyAutoExposureGpuResources(device, res);
                res.attempted = true;
                return false;
            }

            // Readback copies for the display; without them the exposure still works.
            for (BufferHandle& buffer : res.readback)
            {
                BufferDesc desc{};
                desc.byteSize = sizeof(auto_exposure::StateGpu);
                desc.stride = sizeof(auto_exposure::StateGpu);
                desc.type = BufferType::Structured;
                desc.usage = ResourceUsage::CopyDest;
                desc.access = MemoryAccess::CpuRead;
                desc.initialState = ResourceState::CopyDest;
                desc.debugName = "ForwardAutoExposure_Readback";
                buffer = device.CreateBuffer(desc);
            }
            return true;
        }

        // Writes the BRDF options into the add-on's frame parameter block
        // (kShadingFrameParameterBlock); the PBR shaders read featureParams[2].
        class ShadingFrameConstantsContributor final : public IFrameConstantsContributor
        {
        public:
            explicit ShadingFrameConstantsContributor(std::shared_ptr<ShadingSettings> settings)
                : m_settings(std::move(settings))
            {}

            std::string_view GetName() const noexcept override { return "krom-forward-shading-constants"; }

            void Contribute(const FrameConstantsContributionContext& /*context*/,
                            FrameConstants& frameConstants) const override
            {
                ShadingFrameParams params{};
                if (m_settings)
                    params.flags = ShadingFlags(*m_settings);
                WriteFrameParameterBlock(frameConstants, kShadingFrameParameterBlock, params);
            }

        private:
            std::shared_ptr<ShadingSettings> m_settings;
        };

        // Shared between feature and pipeline: MSAA is off on backends that cannot do it.
        struct MsaaRuntime
        {
            std::shared_ptr<MsaaSettings> settings;
            bool supported = true;
            // Post-process AA; off on backends without it (OpenGL).
            std::shared_ptr<PostAASettings> postAA;

            [[nodiscard]] uint32_t EffectiveSamples() const noexcept
            {
                return (supported && settings) ? NormalizeMsaaSamples(settings->sampleCount) : 1u;
            }
            [[nodiscard]] PostAAMode EffectivePostAA() const noexcept
            {
                return (supported && postAA) ? postAA->mode : PostAAMode::Off;
            }
        };

        struct BloomPassConstants
        {
            float threshold = 1.0f;
            float intensity = 0.0f;
            float blurRadius = 1.0f;
            float pad = 0.0f;
        };

        // PerPass block of the tonemap shader (shaders/passthrough.ps.hlsl).
        struct TonemapPassConstants
        {
            BloomPassConstants bloom{};
            float exposureScale = 1.0f;
            float op = 0.0f;
            float autoExposure = 0.0f;   // 1: exposure from the auto-exposure state buffer
            float correctionEv = 0.0f;   // added to the automatic exposure
        };
        static_assert(sizeof(TonemapPassConstants) == 32u, "TonemapPassConstants must match PerPass in passthrough.ps.hlsl");

        // Constants that rarely change, kept in one CPU-written buffer. Writing that buffer
        // while the GPU may still read it makes the backend wait for the previous frame
        // (Vulkan), so it is only written when the value differs from the last upload.
        template <typename T>
        struct UploadedConstants
        {
            T value{};
            bool valid = false;

            void Upload(IDevice& device, BufferHandle buffer, const T& next)
            {
                if (valid && std::memcmp(&value, &next, sizeof(T)) == 0)
                    return;
                device.UploadBufferData(buffer, &next, sizeof(T));
                value = next;
                valid = true;
            }
        };

        // Own constant buffer: bound on every tonemap draw, with or without bloom.
        struct TonemapGpuResources
        {
            BufferHandle perPassCB = BufferHandle::Invalid();
            UploadedConstants<TonemapPassConstants> perPassContents;
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
            UploadedConstants<BloomPassConstants> perPassContents;
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
            // HDRSceneColor hat ein D24S8-Depth-Attachment; Depth-Test bleibt aus,
            // aber das Format muss zum Ziel passen (Vulkan VUID-08914).
            pipelineDesc.depthFormat = Format::D24_UNORM_S8_UINT;
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
            if (!useOpenGlShaders)
            {
                // Variants for the multisampled scene target; failure only disables
                // the sky while MSAA is on.
                pipelineDesc.sampleCount = 2u;
                pipelineDesc.debugName = "ForwardFullscreenSky_Pipeline_2x";
                resources.pipeline2x = device.CreatePipeline(pipelineDesc);
                pipelineDesc.sampleCount = 4u;
                pipelineDesc.debugName = "ForwardFullscreenSky_Pipeline_4x";
                resources.pipeline4x = device.CreatePipeline(pipelineDesc);
            }

            resources.initialized = true;
            return true;
        }

        // Buffer SRV slot of the auto-exposure state in the tonemap pixel shader
        // (shaders/passthrough.ps.hlsl, t13): the bone palette there is vertex-only.
        constexpr uint32_t kAutoExposureBufferSlot = BufSRVSlots::BonePalette;

        // Histogram of the HDR image, then adaptation of the exposure (both compute,
        // outside any render pass). Also copies the state for the display and reads
        // back the copy written kReadbackCopies frames ago.
        void RecordAutoExposure(IDevice& device, ICommandList& cmd, AutoExposureGpuResources& res,
                                const TonemapSettings& settings, AutoExposureStatus* status, bool publishStatus,
                                TextureHandle sourceTex, uint32_t width, uint32_t height)
        {
            AutoExposureSettings limits = settings.autoExposure;
            limits.Clamp();

            const auto now = std::chrono::steady_clock::now();
            float dt = res.hasLastTime ? std::chrono::duration<float>(now - res.lastTime).count() : 0.0f;
            dt = std::clamp(dt, 0.0f, 0.25f);   // a stall must not skip the whole adaptation
            res.lastTime = now;
            res.hasLastTime = true;

            bool reset = !res.measured;
            if (status)
            {
                const uint32_t serial = status->resetSerial.load(std::memory_order_relaxed);
                if (serial != res.resetSerial)
                {
                    res.resetSerial = serial;
                    reset = true;
                }
            }
            res.measured = true;

            auto_exposure::PassConstants constants{};
            constants.width = static_cast<float>(width);
            constants.height = static_cast<float>(height);
            constants.deltaTime = dt;
            constants.toBrightSpeed = limits.toBrightSpeed;
            constants.toDarkSpeed = limits.toDarkSpeed;
            constants.reset = reset ? 1.0f : 0.0f;
            constants.minEv = limits.minEv;
            constants.maxEv = limits.maxEv;
            constants.freeze = settings.freezeAutoExposure ? 1.0f : 0.0f;

            BufferDesc cbDesc{};
            cbDesc.byteSize = kConstantBufferAlignment;
            cbDesc.type = BufferType::Constant;
            cbDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
            cbDesc.access = MemoryAccess::CpuWrite;
            cbDesc.initialState = ResourceState::ConstantBuffer;
            cbDesc.debugName = "ForwardAutoExposure_PerPassCB";
            const BufferHandle cb = res.constants.Next(device, cbDesc);
            if (!cb.IsValid())
                return;
            device.UploadBufferData(cb, &constants, sizeof(constants));

            // Display: the copy of kReadbackCopies frames ago is finished by now.
            const uint32_t slot = res.readbackIndex;
            res.readbackIndex = (res.readbackIndex + 1u) % AutoExposureGpuResources::kReadbackCopies;
            const BufferHandle readback = res.readback[slot];
            if (status && publishStatus && readback.IsValid() && res.readbackWritten[slot])
            {
                if (const void* mapped = device.MapBuffer(readback))
                {
                    auto_exposure::StateGpu value{};
                    std::memcpy(&value, mapped, sizeof(value));
                    device.UnmapBuffer(readback);
                    if (value.valid > 0.5f)
                    {
                        const float correction = std::clamp(settings.exposureEv, kMinExposureEv, kMaxExposureEv);
                        status->currentEv.store(value.exposureEv + correction, std::memory_order_relaxed);
                        status->valid.store(true, std::memory_order_relaxed);
                    }
                }
            }

            const uint32_t groupsX = (std::max(width, 1u) + auto_exposure::kHistogramGroupSize - 1u) / auto_exposure::kHistogramGroupSize;
            const uint32_t groupsY = (std::max(height, 1u) + auto_exposure::kHistogramGroupSize - 1u) / auto_exposure::kHistogramGroupSize;

            cmd.TransitionResource(res.histogram, ResourceState::ShaderRead, ResourceState::UnorderedAccess);
            cmd.SetPipeline(res.histogramPipeline);
            cmd.SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Compute);
            cmd.SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Compute);
            cmd.SetUnorderedAccess(UAVSlots::Output0, res.histogram, ShaderStageMask::Compute);
            cmd.Dispatch(groupsX, groupsY, 1u);
            cmd.SetShaderResource(TexSlots::PassSRV0, TextureHandle::Invalid(), ShaderStageMask::Compute);

            cmd.TransitionResource(res.state, ResourceState::ShaderRead, ResourceState::UnorderedAccess);
            cmd.SetPipeline(res.adaptPipeline);
            cmd.SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Compute);
            cmd.SetUnorderedAccess(UAVSlots::Output0, res.histogram, ShaderStageMask::Compute);
            cmd.SetUnorderedAccess(UAVSlots::Output1, res.state, ShaderStageMask::Compute);
            cmd.Dispatch(1u, 1u, 1u);
            cmd.SetUnorderedAccess(UAVSlots::Output0, BufferHandle::Invalid(), ShaderStageMask::Compute);
            cmd.SetUnorderedAccess(UAVSlots::Output1, BufferHandle::Invalid(), ShaderStageMask::Compute);
            cmd.TransitionResource(res.histogram, ResourceState::UnorderedAccess, ResourceState::ShaderRead);

            if (status && publishStatus && readback.IsValid())
            {
                cmd.TransitionResource(res.state, ResourceState::UnorderedAccess, ResourceState::CopySource);
                cmd.CopyBuffer(readback, 0u, res.state, 0u, sizeof(auto_exposure::StateGpu));
                cmd.TransitionResource(res.state, ResourceState::CopySource, ResourceState::ShaderRead);
                res.readbackWritten[slot] = true;
            }
            else
            {
                cmd.TransitionResource(res.state, ResourceState::UnorderedAccess, ResourceState::ShaderRead);
            }
        }

        class ForwardRenderPipeline final : public IRenderPipeline
        {
        public:
            ForwardRenderPipeline(ForwardFeatureConfig config,
                                  std::shared_ptr<SkyGpuResources> skyResources,
                                  std::shared_ptr<BloomGpuResources> bloomResources,
                                  std::shared_ptr<ForwardPlusGpuResources> forwardPlusResources,
                                  std::shared_ptr<ForwardRendererMode> effectiveMode,
                                  std::shared_ptr<ForwardPlusCullingMode> effectiveCullingMode,
                                  std::shared_ptr<TonemapGpuResources> tonemapResources,
                                  std::shared_ptr<MsaaRuntime> msaa)
                : m_config(config)
                , m_skyResources(std::move(skyResources))
                , m_bloomResources(std::move(bloomResources))
                , m_forwardPlusResources(std::move(forwardPlusResources))
                , m_effectiveMode(std::move(effectiveMode))
                , m_effectiveCullingMode(std::move(effectiveCullingMode))
                , m_tonemapResources(std::move(tonemapResources))
                , m_msaa(std::move(msaa))
                , m_resolveResources(std::make_shared<MsaaResolveGpuResources>())
                , m_postAAResources(std::make_shared<PostAAGpuResources>())
            {
            }

            [[nodiscard]] uint64_t GetStructureRevision() const noexcept override
            {
                const uint64_t autoExposure = AutoExposureWanted() ? 1u : 0u;
                if (!m_msaa)
                    return 1u | (autoExposure << 16u);
                return static_cast<uint64_t>(m_msaa->EffectiveSamples())
                    | (static_cast<uint64_t>(m_msaa->EffectivePostAA()) << 8u)
                    | (autoExposure << 16u);
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
                params.sceneOutputRT = context.sceneOutputRT;
                params.sceneOutputTex = context.sceneOutputTex;
                params.backbufferWidth = context.backbufferWidth;
                params.backbufferHeight = context.backbufferHeight;
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
                params.particlesEnabled = context.externalCallbacks.Has(StandardFrameExecutors::Particles);
                params.uiEnabled = context.presentOutput && (
                    context.externalCallbacks.Has(StandardFrameExecutors::UI)
                    || context.externalCallbacks.Has(StandardFrameExecutors::Present)
                    || std::any_of(context.passContributors.begin(), context.passContributors.end(),
                                   [](const IPassContributor* c) {
                                       return c->DeclaresResource(StandardFrameResources::UIOverlay);
                                   }));
                params.clearColorValue = context.clearColor;

                EnsureSkyResources(context);
                uint32_t msaaSamples = m_msaa ? m_msaa->EffectiveSamples() : 1u;
                if (msaaSamples > 1u && !EnsureMsaaResolveResources(context))
                    msaaSamples = 1u;   // no resolve pipeline: draw without MSAA
                params.msaaSamples = msaaSamples;
                IDevice* buildDevice = context.shaderRuntime.GetDevice();
                if (buildDevice)
                    ReleaseExpiredExposureViews(*buildDevice);
                // The main view keeps one history across swapchain images and resizes.
                // Offscreen views have their own persistent output handles. A shared
                // histogram/history lets a material preview meter the main scene.
                const uint64_t exposureViewKey = context.presentOutput ? 0u
                    : (static_cast<uint64_t>(context.backbufferRT.value) << 32u) | context.backbufferTex.value;
                AutoExposureViewState& exposureView = m_autoExposureViews[exposureViewKey];
                exposureView.outputRT = context.presentOutput ? RenderTargetHandle{} : context.backbufferRT;
                const auto viewAutoExposure = exposureView.resources;
                // Post-AA needs the built-in tonemap pass (bakes bring their own).
                const PostAAMode postAAMode = m_msaa ? m_msaa->EffectivePostAA() : PostAAMode::Off;
                const bool postAAWanted = postAAMode != PostAAMode::Off
                    && !context.externalCallbacks.Has(StandardFrameExecutors::Tonemap)
                    && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem
                    && buildDevice;
                if (postAAWanted)
                {
                    // Same format the tonemap material pipeline is built for
                    // (the swapchain format); the post-AA output uses it as well.
                    const MaterialRuntimeDesc* tonemapRuntime = MaterialRuntimeBridge::GetRuntimeDesc(
                        *context.tonemapMaterialSystem, context.defaultTonemapMaterial);
                    if (tonemapRuntime && tonemapRuntime->colorFormat != Format::Unknown)
                    {
                        params.postAAFormat = tonemapRuntime->colorFormat;
                        params.postAAEnabled = EnsurePostAAResources(context, postAAMode, params.postAAFormat);
                        params.postAASmaa = params.postAAEnabled && postAAMode == PostAAMode::Smaa;
                    }
                }
                // Auto-exposure runs before the built-in tonemap draw (bakes bring their own).
                if (AutoExposureWanted() && buildDevice && viewAutoExposure
                    && !context.externalCallbacks.Has(StandardFrameExecutors::Tonemap)
                    && context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem)
                {
                    IDevice* device = context.shaderRuntime.GetDevice();
                    if (device)
                    {
                        m_device = device;
                        params.autoExposureEnabled = EnsureAutoExposureGpuResources(*device, *viewAutoExposure);
                    }
                }
                // Switched on (again): the first measurement jumps to its target.
                if (params.autoExposureEnabled && !exposureView.enabled && viewAutoExposure)
                    viewAutoExposure->measured = false;
                exposureView.enabled = params.autoExposureEnabled;
                if (context.presentOutput && !params.autoExposureEnabled && m_config.autoExposureStatus)
                    m_config.autoExposureStatus->valid.store(false, std::memory_order_relaxed);
                const bool mrtSupported = buildDevice && buildDevice->SupportsFeature("mrt");

                FramePipelineCallbacks callbacks = context.externalCallbacks;

                auto runtime = context.runtimeBindings;

                const auto executeDrawList =
                    [runtime](const DrawList& list, const rendergraph::RGExecContext& execCtx, SceneDepthMode depthMode)
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
                        // Bone palette slot is set for every draw whose palette differs,
                        // including "none": a skinned draw without palette must not reuse
                        // the previous draw's bones (the shader then keeps the bind pose).
                        BufferHandle   lastBoneBuffer   = BufferHandle::Invalid();
                        bool           boneSlotSet      = false;

                        for (const auto& item : list.items)
                        {
                            if (!item.hasGpuData())
                                continue;

                            using CpuClock = std::chrono::steady_clock;
                            const bool profileOpaque = list.passId == StandardRenderPasses::Opaque() &&
                                depthMode != SceneDepthMode::Prepass;
                            const auto materialStart = CpuClock::now();

                            const bool structuredPerObject =
                                runtime->material.materials &&
                                CanUseStructuredPerObject(item,
                                                          list.passId,
                                                          *runtime->material.materials,
                                                          runtime->resources.perObjectStructured);
                            const PerObjectConstants* perObjectConstants = nullptr;
                            if (!structuredPerObject &&
                                runtime->scene.renderQueue &&
                                item.cbOffset < runtime->scene.renderQueue->objectConstants.size())
                                perObjectConstants = &runtime->scene.renderQueue->objectConstants[item.cbOffset];

                            BufferBinding perObjBinding{};
                            const bool needsPerDrawInstanceOffset =
                                structuredPerObject &&
                                runtime->material.shaderRuntime &&
                                runtime->material.shaderRuntime->GetDevice() &&
                                runtime->material.shaderRuntime->GetDevice()->GetShaderTargetProfile() != assets::ShaderTargetProfile::Vulkan_SPIRV;

                            if (((!structuredPerObject) || needsPerDrawInstanceOffset) &&
                                runtime->resources.perObjectArena.IsValid() &&
                                runtime->resources.perObjectStride > 0u)
                            {
                                const uint32_t perDrawOffset =
                                    needsPerDrawInstanceOffset ? item.instanceDrawOffset : item.cbOffset;
                                perObjBinding = BufferBinding{
                                    runtime->resources.perObjectArena,
                                    perDrawOffset * runtime->resources.perObjectStride,
                                    static_cast<uint32_t>(sizeof(PerObjectConstants))
                                };
                            }

                            if (!materialBound || item.material != lastMaterial)
                            {
                                if (profileOpaque)
                                    ++runtime->opaqueCpu.materialBinds;
                                const MaterialSystemShaderMaterialSource materialSource(*runtime->material.materials);
                                if (!runtime->material.shaderRuntime->BindMaterialWithRange(*execCtx.cmd,
                                    materialSource,
                                    item.material,
                                    runtime->resources.perFrameCB,
                                    runtime->resources.perFrameBinding,
                                    perObjBinding,
                                    {},
                                    perObjectConstants,
                                    list.passId, depthMode))
                                {
                                    // Binding fehlgeschlagen → Pipeline-Status unbekannt.
                                    // Beim nächsten Item neu versuchen statt lastMaterial
                                    // als "gebunden" zu markieren.
                                    materialBound = false;
                                    lastMaterial  = MaterialHandle::Invalid();
                                    if (profileOpaque)
                                        runtime->opaqueCpu.materialMs += std::chrono::duration<float, std::milli>(CpuClock::now() - materialStart).count();
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

                            const auto drawStart = CpuClock::now();
                            if (profileOpaque)
                                runtime->opaqueCpu.materialMs += std::chrono::duration<float, std::milli>(drawStart - materialStart).count();

                            if (structuredPerObject)
                            {
                                execCtx.cmd->SetShaderResource(BufSRVSlots::ObjectConstants,
                                                               runtime->resources.perObjectStructured,
                                                               ShaderStageMask::Vertex);
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
                            if (!boneSlotSet || item.boneBuffer != lastBoneBuffer)
                            {
                                execCtx.cmd->SetShaderResource(BufSRVSlots::BonePalette,
                                                               item.boneBuffer,
                                                               ShaderStageMask::Vertex);
                                lastBoneBuffer = item.boneBuffer;
                                boneSlotSet    = true;
                            }
                            execCtx.cmd->DrawIndexed(item.gpuIndexCount, item.instanceCount, 0u, 0, item.firstInstance);
                            if (profileOpaque)
                            {
                                runtime->opaqueCpu.drawMs += std::chrono::duration<float, std::milli>(CpuClock::now() - drawStart).count();
                                ++runtime->opaqueCpu.draws;
                            }
                        }
                    };
                const auto executeSceneDrawList =
                    [executeDrawList, runtime](const DrawList& list, const rendergraph::RGExecContext& execCtx,
                                               uint32_t samples, SceneDepthMode depthMode)
                    {
                        // Material pipelines must match the active scene target's sample count.
                        ShaderRuntime* shaderRuntime = runtime ? runtime->material.shaderRuntime : nullptr;
                        struct SampleScope
                        {
                            ShaderRuntime* sr;
                            ~SampleScope() { if (sr) sr->SetTargetSampleCount(1u); }
                        } sampleScope{ shaderRuntime };
                        if (shaderRuntime)
                            shaderRuntime->SetTargetSampleCount(samples);

                        if (list.items.empty())
                            return;

                        DrawList sceneList{};
                        sceneList.passId = list.passId;
                        sceneList.sorted = list.sorted;
                        sceneList.items.reserve(list.items.size());
                        for (const DrawItem& item : list.items)
                        {
                            if ((item.layerMask & LAYER_EDITOR_GIZMO_OVERLAY_MASK) != 0u)
                                continue;
                            sceneList.items.push_back(item);
                        }
                        if (!sceneList.items.empty())
                            executeDrawList(sceneList, execCtx, depthMode);
                    };

                struct PipelineState
                {
                    StandardFrameBuildResult resources{};
                    bool ready = false;
                    bool depthPrepass = false;
                    bool sceneComposite = false;
                    // Formats of the extra colour targets the opaque pass binds
                    // (FrameRecipe::sceneOutputs of the contributors).
                    std::array<Format, kMaxExtraColorTargets> opaqueOutputFormats{};
                    uint32_t opaqueOutputCount = 0u;
                };
                auto pipelineState = std::make_shared<PipelineState>();

                EnsureBloomResources(context);

                if (!callbacks.Has(StandardFrameExecutors::Sky))
                {
                    auto runtimeSky = runtime;
                    auto skyResources = m_skyResources;
                    callbacks.Register(StandardFrameExecutors::Sky,
                        [runtimeSky, skyResources, msaaSamples](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !runtimeSky || !runtimeSky->material.shaderRuntime || !skyResources)
                                return;

                            const EnvironmentRuntimeState& environmentState =
                                runtimeSky->material.shaderRuntime->GetEnvironmentState();
                            const PipelineHandle skyPipeline = skyResources->ForSamples(msaaSamples);
                            if (!environmentState.environment.IsValid() || !skyPipeline.IsValid())
                                return;

                            execCtx.cmd->SetPipeline(skyPipeline);
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

                const auto drawOpaqueList =
                    [runtime, executeSceneDrawList, pipelineState, msaaSamples](
                        const rendergraph::RGExecContext& ec, SceneDepthMode depthMode)
                    {
                        const DrawList* list = runtime->scene.renderQueue->FindList(StandardRenderPasses::Opaque());
                        ShaderRuntime* sr = runtime->material.shaderRuntime;
                        if (!list || !sr)
                            return;

                        // Resolve both material pipelines against the same target formats.
                        sr->SetTargetExtraColorFormats(pipelineState->opaqueOutputFormats.data(),
                                                       pipelineState->opaqueOutputCount);
                        executeSceneDrawList(*list, ec, msaaSamples, depthMode);
                        sr->SetTargetExtraColorFormats(nullptr, 0u);
                    };

                if (!callbacks.Has(StandardFrameExecutors::DepthPrepass))
                {
                    callbacks.Register(StandardFrameExecutors::DepthPrepass,
                        [runtime, drawOpaqueList](const rendergraph::RGExecContext& ec)
                        {
                            if (runtime && runtime->scene.renderQueue && runtime->material.shaderRuntime)
                            {
                                ec.cmd->SetShaderResource(TexSlots::PassSRV0,
                                    runtime->material.shaderRuntime->GetWhiteFallbackTexture(),
                                    ShaderStageMask::Fragment);
                                drawOpaqueList(ec, SceneDepthMode::Prepass);
                            }
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Opaque))
                {
                    callbacks.Register(StandardFrameExecutors::Opaque,
                        [runtime, drawOpaqueList, pipelineState, this,
                         viewportWidth  = context.viewportWidth,
                         viewportHeight = context.viewportHeight](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->scene.renderQueue || !pipelineState->ready)
                                return;

                            const auto setupStart = std::chrono::steady_clock::now();

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

                            // Inputs of this frame's AfterDepthPrepass contributors
                            // (e.g. GTAO on PassSRV0). PassSRV0 is cleared first: the
                            // sky pass leaves its cube map there.
                            const TextureHandle whiteFallback =
                                runtime->material.shaderRuntime
                                    ? runtime->material.shaderRuntime->GetWhiteFallbackTexture()
                                    : TextureHandle::Invalid();
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, whiteFallback,
                                ShaderStageMask::Fragment);
                            for (const auto& input : pipelineState->resources.opaqueInputs)
                            {
                                const TextureHandle tex = execCtx.GetTexture(input.resource);
                                execCtx.cmd->SetShaderResource(input.textureSlot,
                                    tex.IsValid() ? tex : whiteFallback, ShaderStageMask::Fragment);
                            }

                            // Textures other add-ons publish for the scene passes
                            // (ForwardSceneBindings). They stay bound for the
                            // transparent pass, which follows on the same list.
                            if (const auto* sceneBindings =
                                    runtime->scene.frameData.GetFrameData<ForwardSceneBindings>())
                            {
                                for (uint32_t i = 0u; i < sceneBindings->textureCount; ++i)
                                {
                                    // Invalid entries clear the slot, so no
                                    // stale or destroyed texture stays bound.
                                    const ForwardSceneTextureBinding& b = sceneBindings->textures[i];
                                    if (b.texture.IsValid())
                                        execCtx.cmd->TransitionResource(b.texture,
                                            ResourceState::Unknown, ResourceState::ShaderRead);
                                    execCtx.cmd->SetShaderResource(b.slot, b.texture,
                                        ShaderStageMask::Fragment);
                                }
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
                                    const auto* shadow =
                                        runtime->scene.frameData.GetFrameData<engine::addons::shadow::ShadowFrameData>();
                                    if (shadow && shadow->gpu.lightBuffer.IsValid())
                                        execCtx.cmd->SetShaderResource(BufSRVSlots::ShadowLights,
                                                                       shadow->gpu.lightBuffer,
                                                                       ShaderStageMask::Fragment);
                                    if (shadow && shadow->gpu.viewBuffer.IsValid())
                                        execCtx.cmd->SetShaderResource(BufSRVSlots::ShadowViews,
                                                                       shadow->gpu.viewBuffer,
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

                            runtime->opaqueCpu.setupMs += std::chrono::duration<float, std::milli>(
                                std::chrono::steady_clock::now() - setupStart).count();
                            drawOpaqueList(execCtx, pipelineState->depthPrepass
                                ? SceneDepthMode::ReadOnly : SceneDepthMode::Material);
                        });
                }

                if (!callbacks.Has(StandardFrameExecutors::Transparent))
                {
                    callbacks.Register(StandardFrameExecutors::Transparent,
                        [runtime, executeSceneDrawList, pipelineState, msaaSamples](
                            const rendergraph::RGExecContext& execCtx)
                        {
                            if (!runtime || !runtime->scene.renderQueue)
                                return;
                            const DrawList* list = runtime->scene.renderQueue->FindList(StandardRenderPasses::Transparent());
                            if (!list || !runtime->material.shaderRuntime)
                                return;

                            // A transparent surface isn't represented in the opaque-pass
                            // inputs (GTAO, contact shadows): white on all their slots.
                            const TextureHandle transparentWhite =
                                runtime->material.shaderRuntime->GetWhiteFallbackTexture();
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, transparentWhite,
                                ShaderStageMask::Fragment);
                            for (const auto& input : pipelineState->resources.opaqueInputs)
                                execCtx.cmd->SetShaderResource(input.textureSlot, transparentWhite,
                                    ShaderStageMask::Fragment);
                            if (pipelineState->resources.shadowMap != rendergraph::RG_INVALID_RESOURCE)
                            {
                                const TextureHandle shadow = execCtx.GetTexture(pipelineState->resources.shadowMap);
                                execCtx.cmd->SetShaderResource(TexSlots::ShadowMap, shadow, ShaderStageMask::Fragment);
                                execCtx.cmd->SetShaderResource(TexSlots::PassSRV1, shadow, ShaderStageMask::Fragment);
                                execCtx.cmd->SetSampler(SamplerSlots::ShadowPCF, SamplerSlots::ShadowPCF, ShaderStageMask::Fragment);
                                execCtx.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp, ShaderStageMask::Fragment);
                            }
                            if (const auto* sceneBindings = runtime->scene.frameData.GetFrameData<ForwardSceneBindings>())
                            {
                                for (uint32_t i = 0u; i < sceneBindings->textureCount; ++i)
                                {
                                    const auto& binding = sceneBindings->textures[i];
                                    execCtx.cmd->SetShaderResource(binding.slot, binding.texture, ShaderStageMask::Fragment);
                                }
                            }
                            executeSceneDrawList(*list, execCtx,
                                pipelineState->sceneComposite ? 1u : msaaSamples, SceneDepthMode::Material);
                        });
                }

                if (msaaSamples > 1u && !callbacks.Has(StandardFrameRecipeBuilder::kMsaaResolveExecutor))
                {
                    auto resolveResources = m_resolveResources;
                    callbacks.Register(StandardFrameRecipeBuilder::kMsaaResolveExecutor,
                        [resolveResources,
                         width = context.viewportWidth,
                         height = context.viewportHeight](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !execCtx.device || !execCtx.resources || !resolveResources ||
                                !resolveResources->pipeline.IsValid())
                                return;
                            RenderTargetHandle msTarget = RenderTargetHandle::Invalid();
                            RenderTargetHandle outTarget = RenderTargetHandle::Invalid();
                            const std::string_view outName = StandardFrameResources::HDRSceneColor;
                            for (const rendergraph::CompiledResourceSnapshot& snapshot : *execCtx.resources)
                            {
                                if (snapshot.debugName == StandardFrameRecipeBuilder::kHDRSceneColorMSResource)
                                    msTarget = snapshot.renderTarget;
                                else if (snapshot.debugName == outName)
                                    outTarget = snapshot.renderTarget;
                            }
                            if (!msTarget.IsValid() || !outTarget.IsValid())
                                return;
                            const TextureHandle msColor = execCtx.device->GetRenderTargetColorTexture(msTarget);
                            const TextureHandle msDepth = execCtx.device->GetRenderTargetDepthTexture(msTarget);
                            if (!msColor.IsValid() || !msDepth.IsValid())
                                return;

                            // Colour is planned by the graph (ReadTexture). The depth
                            // attachment is read directly, like GTAO does; its barrier
                            // must come before the render pass opens.
                            execCtx.cmd->TransitionResource(msDepth, ResourceState::Unknown, ResourceState::ShaderRead);

                            ICommandList::RenderPassBeginInfo rp{};
                            rp.renderTarget = outTarget;
                            rp.clearColor   = false;   // every pixel is written
                            rp.clearDepth   = true;
                            execCtx.cmd->BeginRenderPass(rp);
                            execCtx.cmd->SetViewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f);
                            execCtx.cmd->SetScissor(0, 0, width, height);
                            execCtx.cmd->SetPipeline(resolveResources->pipeline);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, msColor, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV1, msDepth, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                            execCtx.cmd->EndRenderPass();

                            // Back to an attachment state for the next frame's scene passes.
                            execCtx.cmd->TransitionResource(msDepth, ResourceState::Unknown, ResourceState::DepthWrite);
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

                            bloomResources->perPassContents.Upload(*execCtx.device, bloomResources->perPassCB, bloomConstants);
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

                            bloomResources->perPassContents.Upload(*execCtx.device, bloomResources->perPassCB, bloomConstants);
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

                            bloomResources->perPassContents.Upload(*execCtx.device, bloomResources->perPassCB, bloomConstants);
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
                    auto tonemapResources = m_tonemapResources;
                    auto tonemapSettings = m_config.tonemapSettings;
                    auto autoExposureResources = params.autoExposureEnabled ? viewAutoExposure
                                                                            : std::shared_ptr<AutoExposureGpuResources>{};
                    const BloomPassConstants bloomConstants{
                        std::max(0.0f, m_config.bloomThreshold),
                        std::max(0.0f, m_config.bloomIntensity),
                        std::max(0.0f, m_config.bloomBlurRadius),
                        0.0f
                    };
                    callbacks.Register(StandardFrameExecutors::Tonemap,
                        [state, runtimeTonemap, bloomResources, bloomConstants, tonemapResources, tonemapSettings,
                         autoExposureResources](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready)
                                return;
                            // The composite includes reflections, transparent draws and overlays.
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
                            // Settings are read every frame: the owner may change them at any time.
                            TonemapPassConstants tonemapConstants{};
                            tonemapConstants.bloom = bloomConstants;
                            if (!bloomAvailable)
                                tonemapConstants.bloom.intensity = 0.0f;
                            const TonemapSettings settings = tonemapSettings ? *tonemapSettings : TonemapSettings{};
                            tonemapConstants.exposureScale = std::exp2(std::clamp(settings.exposureEv, kMinExposureEv, kMaxExposureEv));
                            tonemapConstants.op = static_cast<float>(static_cast<uint32_t>(settings.op));
                            // Auto: the shader reads the exposure from the GPU state; exposureEv is the correction.
                            const bool autoExposure = autoExposureResources && autoExposureResources->ready
                                && settings.exposureMode == ExposureMode::Auto;
                            if (autoExposure)
                            {
                                tonemapConstants.autoExposure = 1.0f;
                                tonemapConstants.correctionEv = std::clamp(settings.exposureEv, kMinExposureEv, kMaxExposureEv);
                            }
                            const BufferHandle perPassCB = tonemapResources ? tonemapResources->perPassCB
                                                                            : BufferHandle::Invalid();
                            if (perPassCB.IsValid() && execCtx.device)
                                tonemapResources->perPassContents.Upload(*execCtx.device, perPassCB, tonemapConstants);
                            if (!runtimeTonemap->material.shaderRuntime->BindMaterial(*execCtx.cmd,
                                tonemapMaterialSource,
                                material,
                                BufferHandle::Invalid(),
                                BufferHandle::Invalid(),
                                perPassCB,
                                StandardRenderPasses::Postprocess()))
                            {
                                // Shader still compiling in the background (cold cache):
                                // skip this frame like any other waiting material.
                                const MaterialGpuState* tonemapState =
                                    runtimeTonemap->material.shaderRuntime->GetMaterialState(material);
                                if (tonemapState && tonemapState->waitingForShaders)
                                    return;
                                Debug::LogError("ForwardRenderPipeline: tonemap material bind failed");
                                return;
                            }

                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::BloomTexture, bloomTex, ShaderStageMask::Fragment);
                            if (autoExposure)
                                execCtx.cmd->SetShaderResource(kAutoExposureBufferSlot, autoExposureResources->state,
                                                               ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                            // Next frame the adaptation writes the buffer as UAV; DX11 must not
                            // find it still bound as pixel shader input.
                            if (autoExposure)
                                execCtx.cmd->SetShaderResource(kAutoExposureBufferSlot, BufferHandle::Invalid(),
                                                               ShaderStageMask::Fragment);
                        });
                }

                if (params.autoExposureEnabled && !callbacks.Has(StandardFrameExecutors::AutoExposure))
                {
                    auto state = pipelineState;
                    auto aeResources = viewAutoExposure;
                    auto tonemapSettings = m_config.tonemapSettings;
                    auto status = m_config.autoExposureStatus;
                    const bool publishStatus = context.presentOutput;
                    const uint32_t width = params.viewportWidth;
                    const uint32_t height = params.viewportHeight;
                    callbacks.Register(StandardFrameExecutors::AutoExposure,
                        [state, aeResources, tonemapSettings, status, publishStatus, width, height](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !execCtx.device || !state->ready || !aeResources || !aeResources->ready)
                                return;
                            const TonemapSettings settings = tonemapSettings ? *tonemapSettings : TonemapSettings{};
                            if (settings.exposureMode != ExposureMode::Auto)
                                return;
                            const rendergraph::RGResourceID srcId =
                                (state->resources.gtaoComposite != rendergraph::RG_INVALID_RESOURCE)
                                ? state->resources.gtaoComposite
                                : state->resources.hdrSceneColor;
                            const TextureHandle sourceTex = execCtx.GetTexture(srcId);
                            if (!sourceTex.IsValid())
                                return;
                            RecordAutoExposure(*execCtx.device, *execCtx.cmd, *aeResources, settings, status.get(), publishStatus,
                                               sourceTex, width, height);
                        });
                }


                if (params.postAASmaa && !callbacks.Has(StandardFrameExecutors::SmaaEdges))
                {
                    auto state = pipelineState;
                    auto postAAResources = m_postAAResources;
                    callbacks.Register(StandardFrameExecutors::SmaaEdges,
                        [state, postAAResources](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready || !postAAResources ||
                                !postAAResources->smaaEdgePipeline.IsValid())
                                return;
                            const TextureHandle colorTex = execCtx.GetTexture(state->resources.tonemapped);
                            if (!colorTex.IsValid())
                                return;
                            execCtx.cmd->SetPipeline(postAAResources->smaaEdgePipeline);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, colorTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }
                if (params.postAASmaa && !callbacks.Has(StandardFrameExecutors::SmaaWeights))
                {
                    auto state = pipelineState;
                    auto postAAResources = m_postAAResources;
                    callbacks.Register(StandardFrameExecutors::SmaaWeights,
                        [state, postAAResources](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready || !postAAResources ||
                                !postAAResources->smaaWeightPipeline.IsValid())
                                return;
                            const TextureHandle edgesTex = execCtx.GetTexture(state->resources.smaaEdges);
                            if (!edgesTex.IsValid())
                                return;
                            execCtx.cmd->SetPipeline(postAAResources->smaaWeightPipeline);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, edgesTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV1, postAAResources->smaaAreaTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV2, postAAResources->smaaSearchTex, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->SetSampler(SamplerSlots::PointClamp, SamplerSlots::PointClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }
                if (params.postAAEnabled && !callbacks.Has(StandardFrameExecutors::PostAA))
                {
                    auto state = pipelineState;
                    auto postAAResources = m_postAAResources;
                    const bool smaa = params.postAASmaa;
                    callbacks.Register(StandardFrameExecutors::PostAA,
                        [state, postAAResources, smaa](const rendergraph::RGExecContext& execCtx)
                        {
                            if (!execCtx.cmd || !state->ready || !postAAResources)
                                return;
                            const PipelineHandle pipeline = smaa ? postAAResources->smaaBlendPipeline
                                                                 : postAAResources->fxaaPipeline;
                            if (!pipeline.IsValid())
                                return;
                            const TextureHandle sourceTex = execCtx.GetTexture(state->resources.tonemapped);
                            if (!sourceTex.IsValid())
                                return;
                            execCtx.cmd->SetPipeline(pipeline);
                            execCtx.cmd->SetShaderResource(TexSlots::PassSRV0, sourceTex, ShaderStageMask::Fragment);
                            if (smaa)
                            {
                                // SMAA neighborhood blending: colour + blending weights.
                                const TextureHandle weightsTex = execCtx.GetTexture(state->resources.smaaWeights);
                                if (!weightsTex.IsValid())
                                    return;
                                execCtx.cmd->SetShaderResource(TexSlots::PassSRV1, weightsTex, ShaderStageMask::Fragment);
                            }
                            execCtx.cmd->SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, ShaderStageMask::Fragment);
                            execCtx.cmd->Draw(3u, 1u, 0u, 0u);
                        });
                }

                // Contributors tragen Ressourcen, Passes und Executoren ein.
                // drawObjects kapselt die generische per-Objekt-Zeichenlogik dieser Pipeline.
                FrameRecipe preContributorRecipe{};
                FrameRecipe afterDepthRecipe{};
                FrameRecipe postContributorRecipe{};
                FrameRecipe overlayContributorRecipe{};
                size_t beforeTransparentCount = 0u;
                if (!context.passContributors.empty())
                {
                    auto drawObjectsFn = std::function<void(const DrawList&, const rendergraph::RGExecContext&)>(
                        [executeDrawList, pipelineState](const DrawList& list, const rendergraph::RGExecContext& ec)
                        {
                            // DX11 nullt alle SRVs in BeginRenderPass. Shadow-Map hier
                            // erneut binden damit lit.ps.hlsl nicht auf eine null-SRV
                            // sampelt (→ shadowVisibility=0 → nur Ambient → flat look).
                            // Nicht im Shadow-Pass selbst: dort ist die Shadow-Map das
                            // Depth-Attachment (Lesen+Schreiben zugleich; unter Vulkan
                            // zudem eine Layout-Barrier im aktiven Render-Pass).
                            if (ec.cmd && pipelineState &&
                                list.passId != StandardRenderPasses::Shadow() &&
                                pipelineState->resources.shadowMap != rendergraph::RG_INVALID_RESOURCE)
                            {
                                const TextureHandle shadowTex = ec.GetTexture(pipelineState->resources.shadowMap);
                                if (shadowTex.IsValid())
                                    ec.cmd->SetShaderResource(TexSlots::ShadowMap,
                                        shadowTex, ShaderStageMask::Fragment);
                            }
                            executeDrawList(list, ec, SceneDepthMode::Material);
                        });
                    const PassBuildContext::PassFrameView passFrame{
                        context.viewportWidth,
                        context.viewportHeight,
                        &context.renderQueue,
                        context.runtimeBindings,
                        context.enableAmbientOcclusion,
                        context.enableSelectionOutline,
                        context.enableScreenSpaceReflections,
                        msaaSamples,
                        mrtSupported,
                        context.enableDepthPrepass,
                        params.HasSceneOutput(),
                        context.defaultTonemapMaterial.IsValid() && context.tonemapMaterialSystem &&
                            MaterialRuntimeBridge::GetRuntimeDesc(*context.tonemapMaterialSystem, context.defaultTonemapMaterial)
                            ? MaterialRuntimeBridge::GetRuntimeDesc(*context.tonemapMaterialSystem, context.defaultTonemapMaterial)->colorFormat
                            : Format::RGBA8_UNORM_SRGB
                    };
                    const auto buildPhase = [&](PassPhase phase, FrameRecipe& recipe)
                    {
                        PassBuildContext pbc{passFrame, recipe, callbacks, drawObjectsFn};
                        for (const IPassContributor* c : context.passContributors)
                            if (c->GetPhase() == phase)
                                c->BuildPass(pbc);
                    };
                    buildPhase(PassPhase::BeforeOpaque, preContributorRecipe);
                    buildPhase(PassPhase::AfterDepthPrepass, afterDepthRecipe);
                    buildPhase(PassPhase::BeforeTransparent, postContributorRecipe);
                    beforeTransparentCount = postContributorRecipe.passes.size();
                    buildPhase(PassPhase::AfterOpaque, postContributorRecipe);
                    buildPhase(PassPhase::AfterPostProcess, overlayContributorRecipe);
                }

                const auto hasResource = [](const FrameRecipe& recipe, std::string_view name)
                {
                    return std::any_of(recipe.resources.begin(), recipe.resources.end(),
                        [name](const FrameRecipeResourceDesc& r) { return r.name == name; });
                };
                // Opaque inputs count only from the AfterDepthPrepass phase; they
                // are the only reason for a depth prepass.
                preContributorRecipe.opaqueInputs.clear();
                postContributorRecipe.opaqueInputs.clear();
                if (!context.enableDepthPrepass)
                    afterDepthRecipe.opaqueInputs.clear();
                params.depthPrepassEnabled = !afterDepthRecipe.opaqueInputs.empty();
                params.sceneCompositeEnabled = hasResource(postContributorRecipe, StandardFrameRecipeBuilder::kSceneCompositeResource);
                pipelineState->depthPrepass = params.depthPrepassEnabled;
                pipelineState->sceneComposite = params.sceneCompositeEnabled;

                // Extra outputs of the opaque pass (MRT). Without backend support
                // they are dropped; the contributor sees that through
                // PassFrameView::mrtSupported and does not request them.
                pipelineState->opaqueOutputCount = 0u;
                for (FrameRecipe* contributorRecipe : { &preContributorRecipe, &afterDepthRecipe, &postContributorRecipe })
                {
                    if (!mrtSupported)
                        contributorRecipe->sceneOutputs.clear();
                    for (const FrameRecipeSceneOutput& output : contributorRecipe->sceneOutputs)
                        if (pipelineState->opaqueOutputCount < kMaxExtraColorTargets)
                            pipelineState->opaqueOutputFormats[pipelineState->opaqueOutputCount++] = output.format;
                }

                const StandardFrameBuildResult builtResources =
                    StandardFrameRecipeBuilder::Build(context.renderGraph, params, callbacks,
                        &preContributorRecipe, &postContributorRecipe, &afterDepthRecipe, beforeTransparentCount, &overlayContributorRecipe);
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
                if (m_device && m_postAAResources)
                    DestroyPostAAGpuResources(*m_device, *m_postAAResources);
                if (m_device)
                    for (auto& [key, view] : m_autoExposureViews)
                        DestroyAutoExposureGpuResources(*m_device, *view.resources);
                m_autoExposureViews.clear();
                if (m_config.autoExposureStatus)
                    m_config.autoExposureStatus->valid.store(false, std::memory_order_relaxed);
                m_device = nullptr;
            }

        private:
            void ReleaseExpiredExposureViews(IDevice& device) const
            {
                for (auto it = m_autoExposureViews.begin(); it != m_autoExposureViews.end();)
                {
                    if (it->first != 0u && it->second.outputRT.IsValid() &&
                        !device.GetRenderTargetColorTexture(it->second.outputRT).IsValid())
                    {
                        DestroyAutoExposureGpuResources(device, *it->second.resources);
                        it = m_autoExposureViews.erase(it);
                    }
                    else
                        ++it;
                }
            }

            [[nodiscard]] bool AutoExposureWanted() const noexcept
            {
                return m_config.tonemapSettings && m_config.tonemapSettings->exposureMode == ExposureMode::Auto;
            }

            // Post-AA pipelines for the given mode and output format. Shaders (and the
            // SMAA lookup textures) are created once; the pipelines writing the output
            // are rebuilt when the format changes.
            bool EnsurePostAAResources(const RenderPipelineBuildContext& context, PostAAMode mode, Format format) const
            {
                if (!m_postAAResources)
                    return false;
                IDevice* device = context.shaderRuntime.GetDevice();
                if (!device)
                    return false;
                PostAAGpuResources& res = *m_postAAResources;
                if (!res.shadersAttempted)
                {
                    res.shadersAttempted = true;
                    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(*device);
                    if (target == assets::ShaderTargetProfile::OpenGL_GLSL450)
                        return false;
                    res.vertexShader = CreateSkyShader(*device, target, assets::ShaderStage::Vertex,
                        assets::ShaderSourceLanguage::HLSL, kSkyFullscreenVsHlsl, "ForwardPostAAVS");
                    res.fxaaShader = CreateSkyShader(*device, target, assets::ShaderStage::Fragment,
                        assets::ShaderSourceLanguage::HLSL, kFxaaPsHlsl, "ForwardFxaaPS");
                    if (!res.vertexShader.IsValid() || !res.fxaaShader.IsValid())
                        Debug::LogError("ForwardFeature.cpp: FXAA shaders failed, post-AA stays off");
                }
                if (!res.vertexShader.IsValid())
                    return false;
                if (mode == PostAAMode::Smaa)
                    return EnsureSmaaResources(*device, res, format);
                if (!res.fxaaShader.IsValid())
                    return false;
                if (res.fxaaPipeline.IsValid() && res.fxaaFormat == format)
                    return true;
                if (res.fxaaPipeline.IsValid())
                {
                    device->DestroyPipeline(res.fxaaPipeline);
                    res.fxaaPipeline = PipelineHandle::Invalid();
                }
                res.fxaaPipeline = CreatePostAAPipeline(*device, res.vertexShader, res.fxaaShader, format, "ForwardFxaa_Pipeline");
                res.fxaaFormat = format;
                if (!res.fxaaPipeline.IsValid())
                {
                    Debug::LogError("ForwardFeature.cpp: FXAA pipeline failed, post-AA stays off");
                    return false;
                }
                return true;
            }

            // SMAA 1x: three shaders, the edge (RG8) and weight (RGBA8) pipelines, the
            // blend pipeline in the output format, and the area/search lookup textures.
            static bool EnsureSmaaResources(IDevice& device, PostAAGpuResources& res, Format format)
            {
                if (!res.smaaAttempted)
                {
                    res.smaaAttempted = true;
                    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
                    res.smaaEdgeShader = CreateSkyShader(device, target, assets::ShaderStage::Fragment,
                        assets::ShaderSourceLanguage::HLSL, smaa::kEdgeDetectionPsHlsl, "ForwardSmaaEdgePS");
                    res.smaaWeightShader = CreateSkyShader(device, target, assets::ShaderStage::Fragment,
                        assets::ShaderSourceLanguage::HLSL, smaa::kBlendingWeightPsHlsl, "ForwardSmaaWeightPS");
                    res.smaaBlendShader = CreateSkyShader(device, target, assets::ShaderStage::Fragment,
                        assets::ShaderSourceLanguage::HLSL, smaa::kNeighborhoodBlendingPsHlsl, "ForwardSmaaBlendPS");
                    if (res.smaaEdgeShader.IsValid() && res.smaaWeightShader.IsValid())
                    {
                        res.smaaEdgePipeline = CreatePostAAPipeline(device, res.vertexShader, res.smaaEdgeShader,
                            Format::RG8_UNORM, "ForwardSmaaEdge_Pipeline");
                        res.smaaWeightPipeline = CreatePostAAPipeline(device, res.vertexShader, res.smaaWeightShader,
                            Format::RGBA8_UNORM, "ForwardSmaaWeight_Pipeline");
                    }
                    res.smaaAreaTex = CreateLookupTexture(device, smaa::kAreaTexWidth, smaa::kAreaTexHeight,
                        Format::RG8_UNORM, smaa::AreaTexData(), smaa::AreaTexSize(), "SMAA_AreaTex");
                    res.smaaSearchTex = CreateLookupTexture(device, smaa::kSearchTexWidth, smaa::kSearchTexHeight,
                        Format::R8_UNORM, smaa::SearchTexData(), smaa::SearchTexSize(), "SMAA_SearchTex");
                    if (!SmaaBaseResourcesValid(res))
                        Debug::LogError("ForwardFeature.cpp: SMAA resources failed, post-AA stays off");
                }
                if (!SmaaBaseResourcesValid(res))
                    return false;
                if (res.smaaBlendPipeline.IsValid() && res.smaaBlendFormat == format)
                    return true;
                if (res.smaaBlendPipeline.IsValid())
                {
                    device.DestroyPipeline(res.smaaBlendPipeline);
                    res.smaaBlendPipeline = PipelineHandle::Invalid();
                }
                res.smaaBlendPipeline = CreatePostAAPipeline(device, res.vertexShader, res.smaaBlendShader,
                    format, "ForwardSmaaBlend_Pipeline");
                res.smaaBlendFormat = format;
                if (!res.smaaBlendPipeline.IsValid())
                {
                    Debug::LogError("ForwardFeature.cpp: SMAA blend pipeline failed, post-AA stays off");
                    return false;
                }
                return true;
            }

            [[nodiscard]] static bool SmaaBaseResourcesValid(const PostAAGpuResources& res) noexcept
            {
                return res.smaaEdgePipeline.IsValid() && res.smaaWeightPipeline.IsValid()
                    && res.smaaBlendShader.IsValid() && res.smaaAreaTex.IsValid() && res.smaaSearchTex.IsValid();
            }

            // Fullscreen triangle without depth, blending or MSAA.
            static PipelineHandle CreatePostAAPipeline(IDevice& device, ShaderHandle vs, ShaderHandle ps,
                                                       Format format, const char* debugName)
            {
                PipelineDesc desc{};
                desc.shaderStages.push_back({ vs, ShaderStageMask::Vertex });
                desc.shaderStages.push_back({ ps, ShaderStageMask::Fragment });
                desc.topology = PrimitiveTopology::TriangleList;
                desc.colorFormat = format;
                desc.depthFormat = Format::Unknown;
                desc.sampleCount = 1u;
                desc.rasterizer.cullMode = CullMode::None;
                desc.depthStencil.depthEnable = false;
                desc.depthStencil.depthWrite = false;
                desc.debugName = debugName;
                return device.CreatePipeline(desc);
            }

            static TextureHandle CreateLookupTexture(IDevice& device, uint32_t width, uint32_t height, Format format,
                                                     const uint8_t* data, size_t byteSize, const char* debugName)
            {
                TextureDesc desc{};
                desc.width = width;
                desc.height = height;
                desc.format = format;
                desc.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
                desc.initialState = ResourceState::ShaderRead;
                desc.debugName = debugName;
                const TextureHandle texture = device.CreateTexture(desc);
                if (texture.IsValid())
                    device.UploadTextureData(texture, data, byteSize, 0u, 0u);
                return texture;
            }

            // Resolve pipeline: colour RGBA16F + D24S8 (written via SV_Depth). Built once.
            bool EnsureMsaaResolveResources(const RenderPipelineBuildContext& context) const
            {
                if (!m_resolveResources)
                    return false;
                if (m_resolveResources->attempted)
                    return m_resolveResources->pipeline.IsValid();
                m_resolveResources->attempted = true;

                IDevice* device = context.shaderRuntime.GetDevice();
                if (!device)
                    return false;
                const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(*device);
                if (target == assets::ShaderTargetProfile::OpenGL_GLSL450)
                    return false;

                m_resolveResources->vertexShader = CreateSkyShader(*device, target, assets::ShaderStage::Vertex,
                    assets::ShaderSourceLanguage::HLSL, kSkyFullscreenVsHlsl, "ForwardMsaaResolveVS");
                m_resolveResources->fragmentShader = CreateSkyShader(*device, target, assets::ShaderStage::Fragment,
                    assets::ShaderSourceLanguage::HLSL, kMsaaResolvePsHlsl, "ForwardMsaaResolvePS");
                if (!m_resolveResources->vertexShader.IsValid() || !m_resolveResources->fragmentShader.IsValid())
                {
                    Debug::LogError("ForwardFeature.cpp: MSAA resolve shaders failed, MSAA stays off");
                    return false;
                }

                PipelineDesc desc{};
                desc.shaderStages.push_back({ m_resolveResources->vertexShader, ShaderStageMask::Vertex });
                desc.shaderStages.push_back({ m_resolveResources->fragmentShader, ShaderStageMask::Fragment });
                desc.topology = PrimitiveTopology::TriangleList;
                desc.colorFormat = Format::RGBA16_FLOAT;
                desc.depthFormat = Format::D24_UNORM_S8_UINT;
                desc.sampleCount = 1u;
                desc.rasterizer.cullMode = CullMode::None;
                desc.depthStencil.depthEnable = true;
                desc.depthStencil.depthWrite = true;
                desc.depthStencil.depthFunc = DepthFunc::Always;
                desc.debugName = "ForwardMsaaResolve_Pipeline";
                m_resolveResources->pipeline = device->CreatePipeline(desc);
                if (!m_resolveResources->pipeline.IsValid())
                {
                    Debug::LogError("ForwardFeature.cpp: MSAA resolve pipeline failed, MSAA stays off");
                    return false;
                }
                return true;
            }

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
            std::shared_ptr<TonemapGpuResources> m_tonemapResources;
            std::shared_ptr<MsaaRuntime> m_msaa;
            std::shared_ptr<MsaaResolveGpuResources> m_resolveResources;
            std::shared_ptr<PostAAGpuResources> m_postAAResources;
            mutable std::unordered_map<uint64_t, AutoExposureViewState> m_autoExposureViews;
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
                , m_tonemapResources(std::make_shared<TonemapGpuResources>())
                , m_pipelineConfig(WithTonemapSettings(std::move(config)))
                , m_msaa(std::make_shared<MsaaRuntime>(MsaaRuntime{ m_pipelineConfig.msaaSettings, true, m_pipelineConfig.postAASettings }))
                , m_shadingContributor(std::make_shared<ShadingFrameConstantsContributor>(m_pipelineConfig.shadingSettings))
                , m_effectiveMode(std::make_shared<ForwardRendererMode>(m_pipelineConfig.mode))
                , m_effectiveCullingMode(std::make_shared<ForwardPlusCullingMode>(m_pipelineConfig.cullingMode))
                , m_pipeline(std::make_shared<ForwardRenderPipeline>(
                      m_pipelineConfig,
                      m_skyResources,
                      m_bloomResources,
                      m_forwardPlusResources,
                      m_effectiveMode,
                      m_effectiveCullingMode,
                      m_tonemapResources,
                      m_msaa))
            {
            }

            static ForwardFeatureConfig WithTonemapSettings(ForwardFeatureConfig config)
            {
                if (!config.tonemapSettings)
                    config.tonemapSettings = std::make_shared<TonemapSettings>();
                if (!config.msaaSettings)
                    config.msaaSettings = std::make_shared<MsaaSettings>();
                if (!config.postAASettings)
                    config.postAASettings = std::make_shared<PostAASettings>();
                if (!config.shadingSettings)
                    config.shadingSettings = std::make_shared<ShadingSettings>();
                return config;
            }

            std::string_view GetName() const noexcept override { return "krom-forward"; }
            FeatureID GetID() const noexcept override { return FeatureID::FromString("krom-forward"); }

            void Register(FeatureRegistrationContext& context) override
            {
                context.RegisterRenderPipeline(m_pipeline, true);
                if (context.RegisterFrameParameterBlock(kShadingFrameParameterBlock))
                    context.RegisterFrameConstantsContributor(m_shadingContributor);
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
                // OpenGL is frozen for this series: no MSAA resolve shader there.
                if (m_msaa && context.device.GetShaderTargetProfile() == assets::ShaderTargetProfile::OpenGL_GLSL450)
                    m_msaa->supported = false;

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
                if (m_tonemapResources && !m_tonemapResources->perPassCB.IsValid())
                {
                    BufferDesc desc{};
                    desc.byteSize = kConstantBufferAlignment;
                    desc.type = BufferType::Constant;
                    desc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
                    desc.access = MemoryAccess::CpuWrite;
                    desc.debugName = "ForwardTonemap_PerPassCB";
                    m_tonemapResources->perPassCB = context.device.CreateBuffer(desc);
                    m_tonemapResources->perPassContents = {};
                    // Not fatal: the tonemap pass then binds no PerPass block.
                    if (!m_tonemapResources->perPassCB.IsValid())
                        Debug::LogWarning("ForwardFeature.cpp: failed to create tonemap per-pass CB");
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
            std::shared_ptr<TonemapGpuResources>      m_tonemapResources;
            std::shared_ptr<MsaaRuntime>              m_msaa;
            std::shared_ptr<ShadingFrameConstantsContributor> m_shadingContributor;
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
