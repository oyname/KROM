#include "SurfaceCachePass.hpp"

#include "addons/forward/StandardFramePipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "addons/forward/ForwardSceneBindings.hpp"
#include "renderer/Environment.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "rendergraph/RenderGraph.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace engine::renderer::addons::surface_cache {

// ─── Shaders ─────────────────────────────────────────────────────────────────

const char* const kFullscreenVsHlsl = R"(
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOutput main(uint id : SV_VertexID)
{
    VSOutput o;
    if      (id == 0u) { o.pos = float4(-1.0f,  1.0f, 0.0f, 1.0f); o.uv = float2(0.0f, 0.0f); }
    else if (id == 1u) { o.pos = float4(-1.0f, -3.0f, 0.0f, 1.0f); o.uv = float2(0.0f, 2.0f); }
    else               { o.pos = float4( 3.0f,  1.0f, 0.0f, 1.0f); o.uv = float2(2.0f, 0.0f); }
    return o;
}
)";

// Ray kinds (SurfaceCacheRayKind): 0 primary, 1 diffuse closest, 2 diffuse any,
// 3 a 16 x 16 grid of camera rays written out for the CPU check.
const char* const kTraceCsHlsl = R"(#include "surface_cache_trace.hlsl"

// Prefix of the PerFrame block (FrameConstants, RenderFrameConstants.hpp).
cbuffer PerFrame : register(b0)
{
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 viewProjMatrix;
    float4x4 invViewProjMatrix;
    float4   cameraPositionWS;
};

cbuffer SurfaceCacheDispatch : register(b3)
{
    uint  scWidth;
    uint  scHeight;
    uint  scTlasNodeCount;
    uint  scMode;
    float scTMax;
    uint  scFrame;
    uint  scPrimaryBase;
    uint  scStatsSlot;
};

RWStructuredBuffer<uint4> gPrimary  : register(u0);
RWStructuredBuffer<uint>  gBench    : register(u1);
RWStructuredBuffer<uint>  gStats    : register(u2);
RWStructuredBuffer<uint>  gValidate : register(u3);

groupshared uint gsMaxStack;
groupshared uint gsOverflows;
groupshared uint gsHits;

static const uint kMiss = 0xFFFFFFFFu;

// Same mapping as the screen-space passes: uv from the top left, NDC y up.
float3 CameraRayDirection(float2 uv)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(invViewProjMatrix, float4(ndc, 0.5f, 1.0f));
    return normalize(h.xyz / h.w - cameraPositionWS.xyz);
}

uint OctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
    float2 e = n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    const uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 65535.0f);
    return q.x | (q.y << 16u);
}

float3 OctDecode(uint packed)
{
    const float2 e = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return normalize(n);
}

uint PcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float3 CosineDirection(float3 n, uint seed)
{
    const uint a = PcgHash(seed);
    const uint b = PcgHash(a);
    const float u1 = (a & 0xFFFFFFu) / 16777216.0f;
    const float u2 = (b & 0xFFFFFFu) / 16777216.0f;
    const float r = sqrt(u1);
    const float phi = 6.28318530718f * u2;
    const float3 t = normalize(abs(n.x) > 0.5f ? cross(n, float3(0.0f, 1.0f, 0.0f)) : cross(n, float3(1.0f, 0.0f, 0.0f)));
    const float3 bt = cross(n, t);
    return normalize(t * (r * cos(phi)) + bt * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - u1)));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0u)
    {
        gsMaxStack = 0u;
        gsOverflows = 0u;
        gsHits = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    TraceStats stats = TraceStatsZero();
    bool hitAny = false;
    const float tMax = scTMax > 0.0f ? scTMax : 1e30f;
    const float3 origin = cameraPositionWS.xyz;

    if (scMode == 3u)
    {
        if (dtid.x < 16u && dtid.y < 16u)
        {
            const float2 uv = (float2(dtid.xy) + 0.5f) / 16.0f;
            const float3 dir = CameraRayDirection(uv);
            TraceHit hit;
            const bool found = TraceClosest(origin, dir, 0.0f, tMax, scTlasNodeCount, hit, stats);
            const uint base = (dtid.y * 16u + dtid.x) * 12u;
            gValidate[base + 0u] = asuint(origin.x);
            gValidate[base + 1u] = asuint(origin.y);
            gValidate[base + 2u] = asuint(origin.z);
            gValidate[base + 3u] = asuint(dir.x);
            gValidate[base + 4u] = asuint(dir.y);
            gValidate[base + 5u] = asuint(dir.z);
            gValidate[base + 6u] = found ? asuint(hit.t) : kMiss;
            gValidate[base + 7u] = found ? hit.instance : kMiss;
            gValidate[base + 8u] = found ? gBlasTriangles[hit.triangleIndex].primitive : kMiss;
            gValidate[base + 9u] = found ? gInstances[hit.instance].entity : kMiss;
            gValidate[base + 10u] = stats.nodesVisited;
            gValidate[base + 11u] = stats.overflows;
            hitAny = found;
        }
    }
    else if (dtid.x < scWidth && dtid.y < scHeight)
    {
        const uint index = dtid.y * scWidth + dtid.x;
        const float2 uv = (float2(dtid.xy) + 0.5f) / float2(scWidth, scHeight);
        const float3 dir = CameraRayDirection(uv);
        if (scMode == 0u)
        {
            TraceHit hit;
            const bool found = TraceClosest(origin, dir, 0.0f, tMax, scTlasNodeCount, hit, stats);
            const uint heat = min(stats.nodesVisited, 65535u) | (min(stats.maxStack, 65535u) << 16u);
            gPrimary[scPrimaryBase + index] = found
                ? uint4(asuint(hit.t), OctEncode(hit.normalWS), hit.instance, heat)
                : uint4(kMiss, 0u, kMiss, heat);
            hitAny = found;
        }
        else
        {
            const uint4 primary = gPrimary[scPrimaryBase + index];
            uint result = kMiss - 1u;   // no primary hit: no secondary ray
            if (primary.x != kMiss)
            {
                const float t = asfloat(primary.x);
                float3 n = OctDecode(primary.y);
                if (dot(n, dir) > 0.0f)
                    n = -n;
                const float3 p = origin + dir * t + n * (1e-3f + 1e-4f * t);
                const float3 d = CosineDirection(n, index ^ PcgHash(scFrame + 0x9E3779B9u));
                if (scMode == 1u)
                {
                    TraceHit hit;
                    const bool found = TraceClosest(p, d, 0.0f, tMax, scTlasNodeCount, hit, stats);
                    result = found ? asuint(hit.t) : kMiss;
                    hitAny = found;
                }
                else
                {
                    const bool found = TraceAny(p, d, 0.0f, tMax, scTlasNodeCount, stats);
                    result = found ? 1u : 0u;
                    hitAny = found;
                }
            }
            gBench[index] = result;
        }
    }

    InterlockedMax(gsMaxStack, stats.maxStack);
    InterlockedAdd(gsOverflows, stats.overflows);
    if (hitAny)
        InterlockedAdd(gsHits, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u)
    {
        uint previous;
        InterlockedMax(gStats[0], gsMaxStack, previous);
        InterlockedAdd(gStats[1], gsOverflows, previous);
        InterlockedAdd(gStats[2u + 2u * scStatsSlot], gsHits, previous);
        if (all(dtid == uint3(0u, 0u, 0u)))
            InterlockedAdd(gStats[3u + 2u * scStatsSlot], 1u, previous);
    }
}
)";

// Debug view over the finished image. Results come from the primary trace of
// this view (one uint4 per pixel, see kTraceCsHlsl); the cache views look the
// primary hit up in the hash grid of the radiance cache.
const char* const kOverlayPsHlsl = R"(#include "surface_cache_common.hlsl"

KROM_BUFFER_SRV(StructuredBuffer<uint4>, gPrimaryResults, 4);
KROM_BUFFER_SRV(StructuredBuffer<uint>,  gOverlayKeys,    12);
KROM_BUFFER_SRV(StructuredBuffer<uint4>, gOverlayCellA,   13);

SC_DEFINE_FIND(ScFindOverlay, gOverlayKeys)

float3 HeatColor(float x)
{
    x = saturate(x);
    return saturate(float3(1.5f - abs(4.0f * x - 3.0f), 1.5f - abs(4.0f * x - 2.0f), 1.5f - abs(4.0f * x - 1.0f)));
}

float3 InstanceColor(uint id)
{
    uint h = id * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    return float3((h & 0xFFu) / 255.0f, ((h >> 8u) & 0xFFu) / 255.0f, ((h >> 16u) & 0xFFu) / 255.0f) * 0.8f + 0.2f;
}

float4 main(float4 pos : SV_POSITION) : SV_Target
{
    const uint2 p = uint2(pos.xy);
    if (p.x >= scViewportWidth || p.y >= scViewportHeight)
        discard;
    const uint4 r = gPrimaryResults[p.y * scViewportWidth + p.x];
    const bool hit = r.x != 0xFFFFFFFFu;
    float3 color = float3(0.05f, 0.05f, 0.09f);
    if (scView == 4u)   // nodes visited
        color = HeatColor((r.w & 0xFFFFu) / 160.0f);
    else if (hit && scView == 1u)
        color = ScOctDecode(r.y) * 0.5f + 0.5f;
    else if (hit && scView == 2u)
        color = saturate(asfloat(r.x) / max(scViewScale, 1e-3f)).xxx;
    else if (hit && scView == 3u)
        color = InstanceColor(r.z);
    else if (hit && scView >= 5u)
    {
        // Same cell as the registration: primary hit, normal towards the camera.
        const float2 uv = (float2(p) + 0.5f) / float2(scViewportWidth, scViewportHeight);
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float4 h = mul(invViewProjMatrix, float4(ndc, 0.5f, 1.0f));
        const float3 dir = normalize(h.xyz / h.w - cameraPositionWS.xyz);
        const float3 hitPos = cameraPositionWS.xyz + dir * asfloat(r.x);
        float3 n = ScOctDecode(r.y);
        if (dot(n, dir) > 0.0f)
            n = -n;
        const ScKey key = ScMakeKey(hitPos, n);
        const uint slot = ScFindOverlay(key);
        if (scView == 5u)
            color = InstanceColor(key.key) * (slot != SC_NONE ? 1.0f : 0.25f);
        else if (slot == SC_NONE)
            color = float3(0.35f, 0.0f, 0.35f);   // no cell (not registered yet or table full)
        else if (scView == 6u)
        {
            // Exposed so the mean of the visible cells shows as mid grey.
            const float3 radiance = asfloat(gOverlayCellA[slot].xyz) / SC_PI * scViewScale;
            color = radiance / (1.0f + radiance);
        }
        else
            color = HeatColor((float)gOverlayCellA[slot].w / (float)max(scMaxSamples, 1u));
    }
    return float4(color, scOverlayAlpha);
}
)";

// Radiance cache kernels: one source file, one kernel per define
// (SurfaceCacheGpuResources::CacheKernel order).
const char* const kCacheKernelHlsl[SurfaceCacheGpuResources::kCacheKernelCount] = {
    "#define SC_KERNEL_BEGIN 1\n#include \"surface_cache_update.hlsl\"\n",
    "#define SC_KERNEL_REGISTER 1\n#include \"surface_cache_update.hlsl\"\n",
    "#define SC_KERNEL_TRACE 1\n#include \"surface_cache_update.hlsl\"\n",
    "#define SC_KERNEL_RESOLVE 1\n#include \"surface_cache_update.hlsl\"\n",
    "#define SC_KERNEL_AGE 1\n#include \"surface_cache_update.hlsl\"\n",
    "#define SC_KERNEL_MATERIAL 1\n#include \"surface_cache_update.hlsl\"\n",
    "#define SC_KERNEL_PENDING 1\n#include \"surface_cache_update.hlsl\"\n",
};
// Resolve for shading (package 4): [0] depth, [1] MSAA depth (sample 0).
const char* const kApplyPsHlsl[2] = {
    "#include \"surface_cache_apply.hlsl\"\n",
    "#define SC_APPLY_MSAA 1\n#include \"surface_cache_apply.hlsl\"\n",
};
const char* const kCacheKernelNames[SurfaceCacheGpuResources::kCacheKernelCount] = {
    "surface_cache_begin.cs", "surface_cache_register.cs", "surface_cache_raytrace.cs",
    "surface_cache_resolve.cs", "surface_cache_age.cs", "surface_cache_material.cs", "surface_cache_pending.cs",
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

constexpr std::string_view kDoneResource   = "SurfaceCacheDone";
constexpr const char*      kTokenPrefix    = "SurfaceCacheToken";
constexpr const char*      kExecPrefix     = "frame.surface_cache.";
constexpr uint32_t         kGroupSize      = 8u;
constexpr uint32_t         kLinearGroup    = 64u;
constexpr uint32_t         kConstantCopies = 128u;  // round-robin per dispatch
constexpr uint32_t         kPrimaryBufferSlot = 4u; // BufSRV slots of the overlay
constexpr uint32_t         kOverlayKeysSlot   = 12u;
constexpr uint32_t         kOverlayCellASlot  = 13u;
constexpr uint32_t         kMaterialsSlot     = 10u; // BufSRV slots of the cache kernels
constexpr uint32_t         kDirtyBoxesSlot    = 11u;
constexpr uint32_t         kGridUavCount      = 8u;
constexpr std::string_view kShadowMapResource = "ShadowMap";
constexpr uint32_t         kApplyKeysSlot     = 12u; // BufSRV slots of the resolve for shading
constexpr uint32_t         kApplyCellASlot    = 13u;
constexpr uint32_t         kApplyCellBSlot    = 14u;

struct float4Value { float v[4]; };

ShaderHandle CompileShader(IDevice& device, assets::ShaderTargetProfile target, assets::ShaderStage stage,
                           const char* source, const char* debugName)
{
    assets::ShaderAsset asset{};
    asset.debugName      = debugName;
    asset.stage          = stage;
    asset.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    asset.entryPoint     = "main";
    asset.sourceCode     = source;
    const ShaderStageMask mask = stage == assets::ShaderStage::Vertex ? ShaderStageMask::Vertex
                               : stage == assets::ShaderStage::Compute ? ShaderStageMask::Compute
                               : ShaderStageMask::Fragment;
    assets::CompiledShaderArtifact compiled{};
    std::string error;
    if (!ShaderCompiler::CompileForTarget(asset, target, compiled, &error))
    {
        Debug::LogError("SurfaceCachePass.cpp: shader '%s' failed to compile: %s", debugName, error.c_str());
        return ShaderHandle::Invalid();
    }
    if (!compiled.bytecode.empty())
        return device.CreateShaderFromBytecode(compiled.bytecode.data(), compiled.bytecode.size(), mask, debugName);
    return device.CreateShaderFromSource(compiled.sourceText.empty() ? source : compiled.sourceText, mask,
                                         compiled.entryPoint.empty() ? "main" : compiled.entryPoint.c_str(), debugName);
}

PipelineHandle CreateComputePipeline(IDevice& device, ShaderHandle shader, const char* debugName)
{
    if (!shader.IsValid())
        return PipelineHandle::Invalid();
    PipelineDesc desc{};
    desc.pipelineClass = PipelineClass::Compute;
    desc.debugName = debugName;
    desc.shaderStages.push_back(ShaderStageDesc{ shader, ShaderStageMask::Compute, "main" });
    return device.CreatePipeline(desc);
}

PipelineHandle OverlayPipeline(IDevice& device, SurfaceCacheGpuResources& res, Format format)
{
    const uint32_t key = static_cast<uint32_t>(format);
    if (const auto it = res.overlayPipelines.find(key); it != res.overlayPipelines.end())
        return it->second;
    PipelineDesc desc{};
    desc.shaderStages.push_back({ res.vsFullscreen, ShaderStageMask::Vertex });
    desc.shaderStages.push_back({ res.psOverlay, ShaderStageMask::Fragment });
    desc.topology                     = PrimitiveTopology::TriangleList;
    desc.colorFormat                  = format;
    desc.depthFormat                  = Format::Unknown;
    desc.sampleCount                  = 1u;
    desc.rasterizer.cullMode          = CullMode::None;
    desc.depthStencil.depthEnable     = false;
    desc.depthStencil.depthWrite      = false;
    desc.blendStates[0].blendEnable   = true;
    desc.blendStates[0].srcBlend      = BlendFactor::SrcAlpha;
    desc.blendStates[0].dstBlend      = BlendFactor::InvSrcAlpha;
    desc.blendStates[0].blendOp       = BlendOp::Add;
    desc.blendStates[0].srcBlendAlpha = BlendFactor::Zero;
    desc.blendStates[0].dstBlendAlpha = BlendFactor::One;
    desc.blendStates[0].blendOpAlpha  = BlendOp::Add;
    desc.debugName                    = "SurfaceCacheOverlay";
    const PipelineHandle pipeline = device.CreatePipeline(desc);
    res.overlayPipelines.emplace(key, pipeline);
    return pipeline;
}

// Resolve for shading: half-resolution RGBA16F target, no depth attachment.
PipelineHandle ApplyPipeline(IDevice& device, ShaderHandle vs, ShaderHandle ps, const char* debugName)
{
    if (!vs.IsValid() || !ps.IsValid())
        return PipelineHandle::Invalid();
    PipelineDesc desc{};
    desc.shaderStages.push_back({ vs, ShaderStageMask::Vertex });
    desc.shaderStages.push_back({ ps, ShaderStageMask::Fragment });
    desc.topology                 = PrimitiveTopology::TriangleList;
    desc.colorFormat              = Format::RGBA16_FLOAT;
    desc.depthFormat              = Format::Unknown;
    desc.sampleCount              = 1u;
    desc.rasterizer.cullMode      = CullMode::None;
    desc.depthStencil.depthEnable = false;
    desc.depthStencil.depthWrite  = false;
    desc.debugName                = debugName;
    return device.CreatePipeline(desc);
}

BufferHandle NextConstants(IDevice& device, SurfaceCacheGpuResources& res, const void* data, size_t size)
{
    if (res.constants.empty())
    {
        res.constants.resize(kConstantCopies);
        for (BufferHandle& cb : res.constants)
        {
            BufferDesc desc{};
            desc.byteSize     = kConstantBufferAlignment;
            desc.type         = BufferType::Constant;
            desc.usage        = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
            desc.access       = MemoryAccess::CpuWrite;
            desc.initialState = ResourceState::ConstantBuffer;
            desc.debugName    = "SurfaceCache_DispatchCB";
            cb = device.CreateBuffer(desc);
        }
    }
    res.constantIndex = (res.constantIndex + 1u) % kConstantCopies;
    const BufferHandle cb = res.constants[res.constantIndex];
    if (cb.IsValid())
        device.UploadBufferData(cb, data, size);
    return cb;
}

uint32_t Groups(uint32_t n) { return (std::max(n, 1u) + kGroupSize - 1u) / kGroupSize; }
uint32_t LinearGroups(uint32_t n) { return (std::max(n, 1u) + kLinearGroup - 1u) / kLinearGroup; }

rendergraph::RGResourceID FindResourceId(const rendergraph::RGExecContext& ec, std::string_view name)
{
    if (!ec.resources)
        return rendergraph::RG_INVALID_RESOURCE;
    for (const auto& snapshot : *ec.resources)
        if (snapshot.debugName == name)
            return snapshot.id;
    return rendergraph::RG_INVALID_RESOURCE;
}

// One compute dispatch of the trace kernel with every buffer bound.
void DispatchTrace(const rendergraph::RGExecContext& ec, SurfaceCacheGpuResources& res,
                   const FrameGraphRuntimeBindings& bindings, const SurfaceCacheSceneGpu& scene,
                   SurfaceCacheRayKind kind, uint32_t width, uint32_t height, float tMax, uint32_t primaryBase,
                   uint32_t statsSlot)
{
    IDevice& device = *ec.device;
    ICommandList& cmd = *ec.cmd;
    TraceDispatchConstants constants{};
    constants.width = width;
    constants.height = height;
    constants.tlasNodeCount = scene.tlasNodeCount;
    constants.mode = static_cast<uint32_t>(kind);
    constants.tMax = tMax;
    constants.frame = res.frameCounter;
    constants.primaryBase = primaryBase;
    constants.statsSlot = statsSlot;
    const BufferHandle cb = NextConstants(device, res, &constants, sizeof(constants));
    if (!cb.IsValid())
        return;

    const BufferHandle uavs[4] = { res.primary, res.benchOut, res.stats, res.validate };
    for (const BufferHandle uav : uavs)
        cmd.TransitionResource(uav, ResourceState::ShaderRead, ResourceState::UnorderedAccess);
    cmd.SetPipeline(res.traceCompute);
    cmd.SetConstantBuffer(CBSlots::PerFrame, bindings.resources.perFrameCB, ShaderStageMask::Compute);
    cmd.SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Compute);
    cmd.SetShaderResource(0u, scene.tlasNodes, ShaderStageMask::Compute);
    cmd.SetShaderResource(1u, scene.blasNodes, ShaderStageMask::Compute);
    cmd.SetShaderResource(2u, scene.blasTriangles, ShaderStageMask::Compute);
    cmd.SetShaderResource(3u, scene.instances, ShaderStageMask::Compute);
    for (uint32_t i = 0u; i < 4u; ++i)
        cmd.SetUnorderedAccess(i, uavs[i], ShaderStageMask::Compute);
    if (kind == SurfaceCacheRayKind::Validate)
        cmd.Dispatch(2u, 2u, 1u);
    else
        cmd.Dispatch(Groups(width), Groups(height), 1u);
    for (uint32_t i = 0u; i < 4u; ++i)
        cmd.SetUnorderedAccess(i, BufferHandle::Invalid(), ShaderStageMask::Compute);
    for (uint32_t i = 0u; i < 4u; ++i)
        cmd.SetShaderResource(i, BufferHandle::Invalid(), ShaderStageMask::Compute);
    for (const BufferHandle uav : uavs)
        cmd.TransitionResource(uav, ResourceState::UnorderedAccess, ResourceState::ShaderRead);
}

bool BenchDispatching(const SurfaceCacheBench& bench) noexcept
{
    return bench.state == SurfaceCacheBench::State::Warmup || bench.state == SurfaceCacheBench::State::Measuring;
}

bool DebugTraceWanted(const SurfaceCacheGpuResources& res) noexcept
{
    return res.settings.debugView != SurfaceCacheDebugView::None && !IsShadedDebugView(res.settings.debugView) &&
           res.settings.overlay > 0.0f;
}

// ─── Radiance cache: grid buffers ───────────────────────────────────────────

bool CreateGridBuffer(IDevice& device, BufferHandle& buffer, uint64_t count, uint32_t stride, const char* debugName)
{
    BufferDesc desc{};
    desc.byteSize = count * stride;
    desc.stride = stride;
    desc.type = BufferType::Structured;
    desc.usage = ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess | ResourceUsage::CopyDest |
                 ResourceUsage::CopySource;
    desc.access = MemoryAccess::GpuOnly;
    desc.initialState = ResourceState::ShaderRead;
    desc.debugName = debugName;
    buffer = device.CreateBuffer(desc);
    if (!buffer.IsValid())
        Debug::LogError("SurfaceCachePass.cpp: buffer '%s' (%llu bytes) could not be created", debugName,
                        static_cast<unsigned long long>(desc.byteSize));
    return buffer.IsValid();
}

void ClearGrid(IDevice& device, SurfaceCacheGrid& g)
{
    const uint64_t n = SurfaceCacheGrid::kCapacity;
    const std::vector<uint8_t> zeros(n * 16u, 0u);
    device.UploadBufferData(g.keys, zeros.data(), n * 4u);
    device.UploadBufferData(g.cellA, zeros.data(), n * 16u);
    device.UploadBufferData(g.cellB, zeros.data(), n * 16u);
    device.UploadBufferData(g.cellFrame, zeros.data(), n * 4u);
    device.UploadBufferData(g.accum, zeros.data(), n * 16u);
    device.UploadBufferData(g.activeList, zeros.data(), n * 4u);
    device.UploadBufferData(g.pending, zeros.data(), n * 4u);
    device.UploadBufferData(g.counters, zeros.data(), SurfaceCacheGrid::kCounterWords * 4u);
    g.frame = 0u;
    g.pendingBoxes.clear();
    g.lastCounters = {};
    g.lastCountersFrame = 0u;
}

bool EnsureGrid(IDevice& device, SurfaceCacheGrid& g)
{
    if (g.ready)
        return true;
    const uint64_t n = SurfaceCacheGrid::kCapacity;
    bool ok = CreateGridBuffer(device, g.keys, n, 4u, "SurfaceCache_CellKeys") &&
              CreateGridBuffer(device, g.cellA, n, 16u, "SurfaceCache_CellA") &&
              CreateGridBuffer(device, g.cellB, n, 16u, "SurfaceCache_CellB") &&
              CreateGridBuffer(device, g.cellFrame, n, 4u, "SurfaceCache_CellFrame") &&
              CreateGridBuffer(device, g.accum, n * 4u, 4u, "SurfaceCache_Accum") &&
              CreateGridBuffer(device, g.activeList, n, 4u, "SurfaceCache_ActiveList") &&
              CreateGridBuffer(device, g.pending, n, 4u, "SurfaceCache_Pending") &&
              CreateGridBuffer(device, g.counters, SurfaceCacheGrid::kCounterWords, 4u, "SurfaceCache_Counters");
    if (ok)
    {
        BufferDesc desc{};
        desc.byteSize = SurfaceCacheGrid::kCounterWords * sizeof(uint32_t);
        desc.stride = sizeof(uint32_t);
        desc.type = BufferType::Structured;
        desc.usage = ResourceUsage::CopyDest;
        desc.access = MemoryAccess::CpuRead;
        desc.initialState = ResourceState::CopyDest;
        desc.debugName = "SurfaceCache_CountersReadback";
        g.readback = device.CreateBuffer(desc);
        ok = g.readback.IsValid();
    }
    if (!ok)
    {
        ReleaseSurfaceCacheGrid(device, g);
        return false;
    }
    ClearGrid(device, g);
    g.ready = true;
    Debug::Log("SurfaceCachePass.cpp: radiance cache created (%u cells, %.1f MB)", SurfaceCacheGrid::kCapacity,
               static_cast<double>(n * (4u + 16u + 16u + 4u + 16u + 4u + 4u)) / (1024.0 * 1024.0));
    return true;
}

// Far field of ScFarFieldIrradiance (surface_cache_common.hlsl): IBL irradiance
// (needs the sampler) and the local probes. Returns kSurfaceCacheFlag* bits.
uint32_t BindFarField(ICommandList& cmd, const FrameGraphRuntimeBindings& bindings, ShaderStageMask stage)
{
    uint32_t flags = 0u;
    if (bindings.material.shaderRuntime)
    {
        const EnvironmentRuntimeState& env = bindings.material.shaderRuntime->GetEnvironmentState();
        if (env.active && env.irradiance.IsValid())
        {
            cmd.SetShaderResource(TexSlots::IBLIrradiance, env.irradiance, stage);
            flags |= kSurfaceCacheFlagIbl;
        }
    }
    cmd.SetSampler(SamplerSlots::LinearClamp, SamplerSlots::LinearClamp, stage);
    if (const auto* scene = bindings.scene.frameData.GetFrameData<forward::ForwardSceneBindings>())
        for (uint32_t i = 0u; i < scene->textureCount; ++i)
            if (scene->textures[i].slot == TexSlots::FeatureTexture && scene->textures[i].texture.IsValid())
            {
                cmd.TransitionResource(scene->textures[i].texture, ResourceState::Unknown, ResourceState::ShaderRead);
                cmd.SetShaderResource(TexSlots::FeatureTexture, scene->textures[i].texture, stage);
                flags |= kSurfaceCacheFlagProbes;
            }
    return flags;
}

// Everything the cache kernels read and write, bound for one executor.
struct CacheBinding
{
    uint32_t flags = 0u;
};

CacheBinding BindCache(const rendergraph::RGExecContext& ec, SurfaceCacheGpuResources& res,
                       const FrameGraphRuntimeBindings& bindings, SurfaceCacheWorldState& world, bool withShadowMap)
{
    ICommandList& cmd = *ec.cmd;
    SurfaceCacheGrid& g = world.grid;
    CacheBinding b{};
    const BufferHandle uavs[kGridUavCount] = { g.keys, g.cellA, g.cellB, g.cellFrame, g.accum, g.activeList, g.counters, g.pending };
    for (const BufferHandle uav : uavs)
        cmd.TransitionResource(uav, ResourceState::ShaderRead, ResourceState::UnorderedAccess);
    for (uint32_t i = 0u; i < kGridUavCount; ++i)
        cmd.SetUnorderedAccess(i, uavs[i], ShaderStageMask::Compute);
    cmd.SetConstantBuffer(CBSlots::PerFrame, bindings.resources.perFrameCB, ShaderStageMask::Compute);
    cmd.SetShaderResource(0u, world.gpu.tlasNodes, ShaderStageMask::Compute);
    cmd.SetShaderResource(1u, world.gpu.blasNodes, ShaderStageMask::Compute);
    cmd.SetShaderResource(2u, world.gpu.blasTriangles, ShaderStageMask::Compute);
    cmd.SetShaderResource(3u, world.gpu.instances, ShaderStageMask::Compute);
    cmd.SetShaderResource(kMaterialsSlot, res.materials.buffer, ShaderStageMask::Compute);
    b.flags |= BindFarField(cmd, bindings, ShaderStageMask::Compute);
    if (withShadowMap && res.settings.useCascades)
    {
        const TextureHandle shadow = ec.GetTexture(FindResourceId(ec, kShadowMapResource));
        if (shadow.IsValid())
        {
            cmd.SetShaderResource(TexSlots::ShadowMap, shadow, ShaderStageMask::Compute);
            b.flags |= kSurfaceCacheFlagShadowMap;
        }
    }
    return b;
}

void UnbindCache(const rendergraph::RGExecContext& ec, SurfaceCacheWorldState& world)
{
    ICommandList& cmd = *ec.cmd;
    SurfaceCacheGrid& g = world.grid;
    for (uint32_t i = 0u; i < kGridUavCount; ++i)
        cmd.SetUnorderedAccess(i, BufferHandle::Invalid(), ShaderStageMask::Compute);
    for (uint32_t slot : { 0u, 1u, 2u, 3u, kMaterialsSlot, kDirtyBoxesSlot })
        cmd.SetShaderResource(slot, BufferHandle::Invalid(), ShaderStageMask::Compute);
    for (uint32_t slot : { TexSlots::ShadowMap, TexSlots::IBLIrradiance, TexSlots::FeatureTexture })
        cmd.SetShaderResource(slot, TextureHandle::Invalid(), ShaderStageMask::Compute);
    const BufferHandle uavs[kGridUavCount] = { g.keys, g.cellA, g.cellB, g.cellFrame, g.accum, g.activeList, g.counters, g.pending };
    for (const BufferHandle uav : uavs)
        cmd.TransitionResource(uav, ResourceState::UnorderedAccess, ResourceState::ShaderRead);
}

SurfaceCacheConstants MakeCacheConstants(const SurfaceCacheGpuResources& res, const SurfaceCacheWorldState& world,
                                         uint32_t width, uint32_t height, uint32_t flags)
{
    const SurfaceCacheSettings& s = res.settings;
    SurfaceCacheConstants c{};
    c.capacityMask = SurfaceCacheGrid::kCapacity - 1u;
    c.frame = world.grid.frame;
    c.rayBudget = std::max(s.rayBudget, 1u);
    c.tlasNodeCount = world.gpu.tlasNodeCount;
    c.cellSize = std::max(s.cellSize, 0.01f);
    c.levelDistance = std::max(s.levelDistance, 0.1f);
    c.rayLength = std::max(s.rayLength, 0.0f);
    c.maxRaysPerCell = std::max(s.maxRaysPerCell, 1u);
    c.viewportWidth = width;
    c.viewportHeight = height;
    c.registerStep = std::clamp(s.registerStep, 1u, 16u);
    c.maxAge = std::max(s.maxAge, 1u);
    c.flags = flags;
    c.maxSamples = std::max(s.maxSamples, 1u);
    c.materialCount = static_cast<uint32_t>(std::min<uint64_t>(res.materials.entries.size(), res.materials.capacity / 2u));
    return c;
}

void DispatchCache(const rendergraph::RGExecContext& ec, SurfaceCacheGpuResources& res, uint32_t kernel,
                   const SurfaceCacheConstants& constants, uint32_t groupsX, uint32_t groupsY = 1u)
{
    const BufferHandle cb = NextConstants(*ec.device, res, &constants, sizeof(constants));
    if (!cb.IsValid() || !res.cachePipelines[kernel].IsValid())
        return;
    ec.cmd->SetPipeline(res.cachePipelines[kernel]);
    ec.cmd->SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Compute);
    ec.cmd->Dispatch(groupsX, groupsY, 1u);
}

// ─── Radiance cache: material table ─────────────────────────────────────────

float4Value ReadVec4(const MaterialParameterLayout& layout, const MaterialInstance& inst, std::string_view name,
                     float4Value fallback)
{
    const MaterialParameterSlot* slot = layout.FindByName(name);
    const std::vector<uint8_t>& data = inst.parameters.ConstantData();
    if (!slot || slot->byteSize < 12u || slot->byteOffset + 12u > data.size())
        return fallback;
    float4Value v = fallback;
    std::memcpy(v.v, data.data() + slot->byteOffset, std::min<size_t>(slot->byteSize, 16u));
    return v;
}

TextureHandle MaterialTexture(const MaterialParameterLayout& layout, const MaterialInstance& inst, uint32_t binding)
{
    const int32_t index = layout.FindSlotIndexByBinding(binding, MaterialParameterType::Texture2D);
    return index >= 0 ? inst.parameters.GetTexture(static_cast<uint32_t>(index)) : TextureHandle::Invalid();
}

// Mean albedo and emission of new or changed materials (at most
// SurfaceCacheMaterialTable::kUpdatesPerFrame per frame).
void UpdateMaterialTable(const rendergraph::RGExecContext& ec, SurfaceCacheGpuResources& res,
                         const FrameGraphRuntimeBindings& bindings)
{
    SurfaceCacheMaterialTable& table = res.materials;
    if (table.entries.empty())
        return;
    const uint64_t needed = table.entries.size() * 2u;
    if (!table.buffer.IsValid() || table.capacity < needed)
    {
        if (table.buffer.IsValid())
            ec.device->DestroyBuffer(table.buffer);
        table.buffer = {};
        const uint64_t grown = std::max<uint64_t>(needed * 2u, 128u);
        if (!CreateGridBuffer(*ec.device, table.buffer, grown, 16u, "SurfaceCache_Materials"))
        {
            table.capacity = 0u;
            return;
        }
        table.capacity = grown;
        for (SurfaceCacheMaterialTable::Entry& e : table.entries)
            e.dirty = true;
    }
    const MaterialSystem* materials = bindings.material.materials;
    ICommandList& cmd = *ec.cmd;
    uint32_t updates = 0u;
    bool bound = false;
    for (uint32_t i = 0u; i < table.entries.size() && updates < SurfaceCacheMaterialTable::kUpdatesPerFrame; ++i)
    {
        SurfaceCacheMaterialTable::Entry& entry = table.entries[i];
        const MaterialHandle handle(entry.materialId);
        const uint64_t revision = materials && handle.IsValid() ? materials->GetRevision(handle) : 0u;
        if (!entry.dirty && revision == entry.revision)
            continue;
        SurfaceCacheConstants c{};
        c.materialIndex = i;
        // Unknown material: mid grey, no emission.
        float4Value albedo{ { 0.5f, 0.5f, 0.5f, 0.0f } };
        float4Value emission{ { 0.0f, 0.0f, 0.0f, 0.0f } };
        TextureHandle albedoTex, emissionTex;
        const MaterialInstance* inst = materials && handle.IsValid() ? materials->GetInstance(handle) : nullptr;
        const MaterialParameterLayout* layout = materials && handle.IsValid() ? materials->GetParameterLayout(handle) : nullptr;
        if (inst && layout)
        {
            albedo = ReadVec4(*layout, *inst, "baseColorFactor", float4Value{ { 1.0f, 1.0f, 1.0f, 1.0f } });
            emission = ReadVec4(*layout, *inst, "emissiveFactor", float4Value{ { 0.0f, 0.0f, 0.0f, 0.0f } });
            albedoTex = MaterialTexture(*layout, *inst, TexSlots::Albedo);
            emissionTex = MaterialTexture(*layout, *inst, TexSlots::Emissive);
        }
        for (int k = 0; k < 3; ++k)
        {
            c.albedoFactor[k] = albedo.v[k];
            c.emissionFactor[k] = emission.v[k];
        }
        c.albedoFactor[3] = albedoTex.IsValid() ? 1.0f : 0.0f;
        c.emissionFactor[3] = emissionTex.IsValid() ? 1.0f : 0.0f;
        if (!bound)
        {
            cmd.TransitionResource(table.buffer, ResourceState::ShaderRead, ResourceState::UnorderedAccess);
            cmd.SetUnorderedAccess(0u, table.buffer, ShaderStageMask::Compute);
            bound = true;
        }
        cmd.SetShaderResource(TexSlots::PassSRV0, albedoTex, ShaderStageMask::Compute);
        cmd.SetShaderResource(TexSlots::PassSRV1, emissionTex, ShaderStageMask::Compute);
        DispatchCache(ec, res, SurfaceCacheGpuResources::kMaterial, c, 1u);
        entry.revision = revision;
        entry.dirty = false;
        ++updates;
    }
    if (bound)
    {
        cmd.SetShaderResource(TexSlots::PassSRV0, TextureHandle::Invalid(), ShaderStageMask::Compute);
        cmd.SetShaderResource(TexSlots::PassSRV1, TextureHandle::Invalid(), ShaderStageMask::Compute);
        cmd.SetUnorderedAccess(0u, BufferHandle::Invalid(), ShaderStageMask::Compute);
        cmd.TransitionResource(table.buffer, ResourceState::UnorderedAccess, ResourceState::ShaderRead);
    }
}

// ─── Radiance cache: the three steps of a frame ─────────────────────────────

enum class CacheStep : uint8_t { Register, Trace, Resolve };

void ExecuteCacheStep(const rendergraph::RGExecContext& ec, SurfaceCacheGpuResources& res,
                      const FrameGraphRuntimeBindings& bindings, SurfaceCacheWorldState& world, CacheStep step,
                      uint32_t width, uint32_t height)
{
    IDevice& device = *ec.device;
    SurfaceCacheGrid& g = world.grid;
    if (step == CacheStep::Register)
    {
        if (!EnsureGrid(device, g))
            return;
        if (res.resetRequested)
        {
            ClearGrid(device, g);
            res.resetRequested = false;
            Debug::Log("SurfaceCachePass.cpp: radiance cache cleared");
        }
        UpdateMaterialTable(ec, res, bindings);
        g.frame = g.frame + 1u == 0u ? 1u : g.frame + 1u;
    }
    if (!g.ready || g.frame == 0u)
        return;

    const CacheBinding binding = BindCache(ec, res, bindings, world, step == CacheStep::Trace);
    SurfaceCacheConstants c = MakeCacheConstants(res, world, width, height, binding.flags);
    switch (step)
    {
    case CacheStep::Register:
    {
        DispatchCache(ec, res, SurfaceCacheGpuResources::kBegin, c, 1u);
        const uint32_t gridW = (width + c.registerStep - 1u) / c.registerStep;
        const uint32_t gridH = (height + c.registerStep - 1u) / c.registerStep;
        DispatchCache(ec, res, SurfaceCacheGpuResources::kRegister, c, Groups(gridW), Groups(gridH));
        // Cells last frame's cache rays hit outside the view.
        DispatchCache(ec, res, SurfaceCacheGpuResources::kPending, c, LinearGroups(SurfaceCacheGrid::kCapacity));
        break;
    }
    case CacheStep::Trace:
        DispatchCache(ec, res, SurfaceCacheGpuResources::kTrace, c, LinearGroups(c.rayBudget));
        break;
    case CacheStep::Resolve:
    {
        DispatchCache(ec, res, SurfaceCacheGpuResources::kResolve, c, LinearGroups(SurfaceCacheGrid::kCapacity));
        // Changed regions of the scene BVH since the last update.
        const uint32_t boxes = std::min<uint32_t>(static_cast<uint32_t>(g.pendingBoxes.size() / 8u),
                                                  SurfaceCacheGrid::kMaxDirtyBoxes);
        if (boxes > 0u)
        {
            BufferDesc desc{};
            desc.byteSize = SurfaceCacheGrid::kMaxDirtyBoxes * 2u * 16u;
            desc.stride = 16u;
            desc.type = BufferType::Structured;
            desc.usage = ResourceUsage::ShaderResource;
            desc.access = MemoryAccess::CpuWrite;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName = "SurfaceCache_DirtyBoxes";
            const BufferHandle buffer = g.dirtyBoxes.Next(device, desc);
            if (buffer.IsValid())
            {
                device.UploadBufferData(buffer, g.pendingBoxes.data(), boxes * 8u * sizeof(float));
                ec.cmd->SetShaderResource(kDirtyBoxesSlot, buffer, ShaderStageMask::Compute);
                c.dirtyBoxCount = boxes;
            }
        }
        g.pendingBoxes.clear();
        DispatchCache(ec, res, SurfaceCacheGpuResources::kAge, c, LinearGroups(SurfaceCacheGrid::kCapacity));
        break;
    }
    }
    UnbindCache(ec, world);

    // Counters for the editor and the measuring run, every few frames.
    if (step == CacheStep::Resolve && !g.readbackPending &&
        (g.frame % SurfaceCacheGrid::kReadbackInterval == 0u || BenchDispatching(res.bench)) &&
        (g.frame % 4u == 0u))
    {
        ec.cmd->TransitionResource(g.counters, ResourceState::ShaderRead, ResourceState::CopySource);
        ec.cmd->CopyBuffer(g.readback, 0u, g.counters, 0u, SurfaceCacheGrid::kCounterWords * sizeof(uint32_t));
        ec.cmd->TransitionResource(g.counters, ResourceState::CopySource, ResourceState::ShaderRead);
        g.readbackPending = true;
        g.readbackWait = SurfaceCacheGrid::kReadbackFrames;
        g.readbackFrameOfCopy = g.frame;
    }
}

} // namespace

std::vector<SurfaceCacheBenchPass> MakeBenchPasses()
{
    std::vector<SurfaceCacheBenchPass> passes;
    const struct { const char* name; uint32_t w, h; } resolutions[] = { { "1080p", 1920u, 1080u }, { "540p", 960u, 540u } };
    const struct { const char* name; float tMax; } lengths[] = { { "inf", 0.0f }, { "15m", 15.0f }, { "5m", 5.0f } };
    for (const auto& r : resolutions)
    {
        passes.push_back({ std::string("SurfaceCacheBench_") + r.name + "_primary", SurfaceCacheRayKind::Primary, r.w, r.h, 0.0f });
        for (const auto& l : lengths)
            passes.push_back({ std::string("SurfaceCacheBench_") + r.name + "_diffuse_" + l.name,
                               SurfaceCacheRayKind::Diffuse, r.w, r.h, l.tMax });
        for (const auto& l : lengths)
            passes.push_back({ std::string("SurfaceCacheBench_") + r.name + "_occlusion_" + l.name,
                               SurfaceCacheRayKind::Occlusion, r.w, r.h, l.tMax });
    }
    return passes;
}

bool EnsureStructuredBuffer(IDevice& device, BufferHandle& buffer, uint64_t& capacity, uint64_t count,
                            uint32_t stride, bool unorderedAccess, const char* debugName)
{
    const uint64_t needed = std::max<uint64_t>(count, 1u);
    if (buffer.IsValid() && capacity >= needed)
        return true;
    if (buffer.IsValid())
        device.DestroyBuffer(buffer);
    const uint64_t grown = std::max<uint64_t>(needed + needed / 2u, 64u);
    BufferDesc desc{};
    desc.byteSize = grown * stride;
    desc.stride = stride;
    desc.type = BufferType::Structured;
    desc.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest | ResourceUsage::CopySource;
    if (unorderedAccess)
        desc.usage = desc.usage | ResourceUsage::UnorderedAccess;
    desc.access = MemoryAccess::GpuOnly;
    desc.initialState = ResourceState::ShaderRead;
    desc.debugName = debugName;
    buffer = device.CreateBuffer(desc);
    capacity = buffer.IsValid() ? grown : 0u;
    if (!buffer.IsValid())
        Debug::LogError("SurfaceCachePass.cpp: buffer '%s' (%llu bytes) could not be created", debugName,
                        static_cast<unsigned long long>(desc.byteSize));
    return buffer.IsValid();
}

bool EnsureSurfaceCachePrograms(IDevice& device, SurfaceCacheGpuResources& res)
{
    if (res.programsReady)
        return true;
    if (res.programsFailed)
        return false;
    const ComputeRuntimeDesc compute = device.GetComputeRuntime();
    if (!compute.computePipelinesSupported || !compute.computeDispatchSupported)
    {
        Debug::LogWarning("SurfaceCachePass.cpp: backend has no compute, surface cache not available");
        res.programsFailed = true;
        return false;
    }
    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(device);
    res.csTrace      = CompileShader(device, target, assets::ShaderStage::Compute,  kTraceCsHlsl,      "surface_cache_trace.cs");
    res.vsFullscreen = CompileShader(device, target, assets::ShaderStage::Vertex,   kFullscreenVsHlsl, "surface_cache_fullscreen.vs");
    res.psOverlay    = CompileShader(device, target, assets::ShaderStage::Fragment, kOverlayPsHlsl,    "surface_cache_overlay.ps");
    res.traceCompute = CreateComputePipeline(device, res.csTrace, "SurfaceCacheTrace_Pipeline");
    static const char* const kApplyNames[2] = { "surface_cache_apply.ps", "surface_cache_apply_ms.ps" };
    for (uint32_t i = 0u; i < 2u; ++i)
    {
        res.psApply[i] = CompileShader(device, target, assets::ShaderStage::Fragment, kApplyPsHlsl[i], kApplyNames[i]);
        res.applyPipelines[i] = ApplyPipeline(device, res.vsFullscreen, res.psApply[i], kApplyNames[i]);
    }
    bool cacheReady = res.applyPipelines[0].IsValid() && res.applyPipelines[1].IsValid();
    for (uint32_t k = 0u; k < SurfaceCacheGpuResources::kCacheKernelCount; ++k)
    {
        res.cacheShaders[k] = CompileShader(device, target, assets::ShaderStage::Compute, kCacheKernelHlsl[k], kCacheKernelNames[k]);
        res.cachePipelines[k] = CreateComputePipeline(device, res.cacheShaders[k], kCacheKernelNames[k]);
        cacheReady = cacheReady && res.cachePipelines[k].IsValid();
    }
    // Statistics, check results and their CPU copies.
    uint64_t statsCapacity = 0u, validateCapacity = 0u;
    EnsureStructuredBuffer(device, res.stats, statsCapacity, SurfaceCacheGpuResources::kStatsWords, sizeof(uint32_t), true, "SurfaceCache_Stats");
    EnsureStructuredBuffer(device, res.validate, validateCapacity,
                           SurfaceCacheValidation::kRays * SurfaceCacheValidation::kWordsPerRay,
                           sizeof(uint32_t), true, "SurfaceCache_Validate");
    for (uint32_t i = 0u; i < 2u; ++i)
    {
        BufferDesc desc{};
        desc.byteSize = i == 0u ? SurfaceCacheGpuResources::kStatsWords * sizeof(uint32_t)
                                : SurfaceCacheValidation::kRays * SurfaceCacheValidation::kWordsPerRay * sizeof(uint32_t);
        desc.stride = sizeof(uint32_t);
        desc.type = BufferType::Structured;
        desc.usage = ResourceUsage::CopyDest;
        desc.access = MemoryAccess::CpuRead;
        desc.initialState = ResourceState::CopyDest;
        desc.debugName = i == 0u ? "SurfaceCache_StatsReadback" : "SurfaceCache_ValidateReadback";
        res.readback[i] = device.CreateBuffer(desc);
    }

    res.programsReady = res.csTrace.IsValid() && res.vsFullscreen.IsValid() && res.psOverlay.IsValid() &&
                        res.traceCompute.IsValid() && cacheReady && res.stats.IsValid() && res.validate.IsValid() &&
                        res.readback[0].IsValid() && res.readback[1].IsValid();
    if (!res.programsReady)
    {
        Debug::LogError("SurfaceCachePass.cpp: programs could not be created, surface cache stays off");
        ReleaseSurfaceCacheGpu(device, res);
        res.programsFailed = true;
        return false;
    }
    const std::array<uint32_t, SurfaceCacheGpuResources::kStatsWords> zeros{};
    device.UploadBufferData(res.stats, zeros.data(), sizeof(zeros));
    return true;
}

void ReleaseSurfaceCacheGrid(IDevice& device, SurfaceCacheGrid& g)
{
    for (BufferHandle* b : { &g.keys, &g.cellA, &g.cellB, &g.cellFrame, &g.accum, &g.activeList, &g.pending, &g.counters, &g.readback })
        if (b->IsValid())
        {
            device.DestroyBuffer(*b);
            *b = {};
        }
    g.dirtyBoxes.Release(&device);
    g.ready = false;
    g.frame = 0u;
    g.pendingBoxes.clear();
    g.readbackPending = false;
    g.readbackWait = 0u;
    g.lastCounters = {};
    g.lastCountersFrame = 0u;
}

void ReleaseSurfaceCacheGpu(IDevice& device, SurfaceCacheGpuResources& res)
{
    const auto destroyBuffer = [&](BufferHandle& b) { if (b.IsValid()) device.DestroyBuffer(b); b = {}; };
    for (auto& [world, state] : res.worlds)
    {
        if (!state)
            continue;
        destroyBuffer(state->gpu.tlasNodes);
        destroyBuffer(state->gpu.blasNodes);
        destroyBuffer(state->gpu.blasTriangles);
        destroyBuffer(state->gpu.instances);
        state->gpu = {};
        ReleaseSurfaceCacheGrid(device, state->grid);
    }
    res.worlds.clear();
    for (auto& [format, pipeline] : res.overlayPipelines)
        if (pipeline.IsValid())
            device.DestroyPipeline(pipeline);
    res.overlayPipelines.clear();
    for (uint32_t k = 0u; k < SurfaceCacheGpuResources::kCacheKernelCount; ++k)
    {
        if (res.cachePipelines[k].IsValid()) { device.DestroyPipeline(res.cachePipelines[k]); res.cachePipelines[k] = {}; }
        if (res.cacheShaders[k].IsValid())   { device.DestroyShader(res.cacheShaders[k]);     res.cacheShaders[k] = {}; }
    }
    destroyBuffer(res.materials.buffer);
    res.materials = SurfaceCacheMaterialTable{};
    for (uint32_t i = 0u; i < 2u; ++i)
    {
        if (res.applyPipelines[i].IsValid()) { device.DestroyPipeline(res.applyPipelines[i]); res.applyPipelines[i] = {}; }
        if (res.psApply[i].IsValid())        { device.DestroyShader(res.psApply[i]);          res.psApply[i] = {}; }
    }
    if (res.traceCompute.IsValid()) { device.DestroyPipeline(res.traceCompute); res.traceCompute = {}; }
    if (res.csTrace.IsValid())      { device.DestroyShader(res.csTrace);        res.csTrace = {}; }
    if (res.vsFullscreen.IsValid()) { device.DestroyShader(res.vsFullscreen);   res.vsFullscreen = {}; }
    if (res.psOverlay.IsValid())    { device.DestroyShader(res.psOverlay);      res.psOverlay = {}; }
    destroyBuffer(res.primary);
    destroyBuffer(res.benchOut);
    destroyBuffer(res.stats);
    destroyBuffer(res.validate);
    for (BufferHandle& b : res.readback)
        destroyBuffer(b);
    for (BufferHandle& b : res.constants)
        destroyBuffer(b);
    res.constants.clear();
    res.primaryCapacity = 0u;
    res.benchCapacity = 0u;
    res.programsReady = false;
}

uint64_t SurfaceCacheStructureRevision(const SurfaceCacheGpuResources& res) noexcept
{
    if (!res.settings.enabled || !res.supported)
        return 0u;
    uint64_t revision = 1u;
    if (BenchDispatching(res.bench))
        revision |= 2u;
    if (res.validation.state == SurfaceCacheValidation::State::Requested)
        revision |= 4u;
    if (DebugTraceWanted(res))
        revision |= 8u;
    if (res.settings.apply)
        revision |= 16u;
    return revision;
}

// ─── SurfaceCacheComputePass ─────────────────────────────────────────────────

bool SurfaceCacheComputePass::DeclaresResource(std::string_view name) const noexcept
{
    return m_resources && SurfaceCacheStructureRevision(*m_resources) != 0u &&
           (name == kDoneResource || name == kSurfaceCacheGiResource ||
            name.substr(0, std::string_view(kTokenPrefix).size()) == kTokenPrefix);
}

uint64_t SurfaceCacheComputePass::GetStructureRevision() const noexcept
{
    return m_resources ? SurfaceCacheStructureRevision(*m_resources) : 0u;
}

void SurfaceCacheComputePass::BuildPass(const PassBuildContext& ctx) const
{
    if (!m_resources || SurfaceCacheStructureRevision(*m_resources) == 0u)
        return;

    enum class JobKind : uint8_t { Cache, Trace };
    struct Job
    {
        std::string         name;
        JobKind             jobKind;
        CacheStep           step;
        SurfaceCacheRayKind kind;
        uint32_t            width;
        uint32_t            height;
        float               tMax;
        uint32_t            primaryBase;
        uint32_t            statsSlot;
    };
    // The cache steps first, then the debug trace of this view (it owns
    // primary[0, w*h)); the measuring passes put their primary results behind it.
    const uint32_t w = std::max(ctx.frame.viewportWidth, 1u);
    const uint32_t h = std::max(ctx.frame.viewportHeight, 1u);
    const bool shadows = ctx.frame.ActiveShadowResolution() > 0u;
    std::vector<Job> jobs;
    jobs.push_back({ "SurfaceCacheRegister", JobKind::Cache, CacheStep::Register, SurfaceCacheRayKind::Primary, w, h, 0.0f, 0u, 0u });
    jobs.push_back({ "SurfaceCacheRayTrace", JobKind::Cache, CacheStep::Trace, SurfaceCacheRayKind::Primary, w, h, 0.0f, 0u, 0u });
    jobs.push_back({ "SurfaceCacheResolve", JobKind::Cache, CacheStep::Resolve, SurfaceCacheRayKind::Primary, w, h, 0.0f, 0u, 0u });
    if (DebugTraceWanted(*m_resources))
        jobs.push_back({ "SurfaceCacheDebugTrace", JobKind::Trace, CacheStep::Register, SurfaceCacheRayKind::Primary, w, h, 0.0f, 0u,
                         SurfaceCacheGpuResources::kDebugTraceSlot });
    if (BenchDispatching(m_resources->bench))
        for (size_t k = 0u; k < m_resources->bench.passes.size(); ++k)
        {
            const SurfaceCacheBenchPass& p = m_resources->bench.passes[k];
            jobs.push_back({ p.name, JobKind::Trace, CacheStep::Register, p.kind, p.width, p.height, p.tMax, w * h,
                             static_cast<uint32_t>(k) });
        }
    if (m_resources->validation.state == SurfaceCacheValidation::State::Requested)
        jobs.push_back({ "SurfaceCacheValidate", JobKind::Trace, CacheStep::Register, SurfaceCacheRayKind::Validate, 16u, 16u, 0.0f, 0u,
                         SurfaceCacheGpuResources::kValidateSlot });

    auto res = m_resources;
    auto bindings = ctx.frame.runtimeBindings;
    std::string previous;
    for (size_t i = 0u; i < jobs.size(); ++i)
    {
        const Job job = jobs[i];
        const std::string token = i + 1u == jobs.size() ? std::string(kDoneResource)
                                                        : std::string(kTokenPrefix) + std::to_string(i);
        ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ token, false, 1u, 1u, Format::R8_UNORM,
                                                                RGResourceKind::ColorTexture });
        FrameRecipePassDesc pass{};
        pass.name = job.name;
        pass.executorName = std::string(kExecPrefix) + job.name;
        if (!previous.empty())
            pass.accesses.push_back({ previous, FrameRecipeAccessKind::ReadTexture });
        // The cache rays shade their hits with this frame's cascades.
        if (job.jobKind == JobKind::Cache && job.step == CacheStep::Trace && shadows)
            pass.accesses.push_back({ std::string(kShadowMapResource), FrameRecipeAccessKind::ReadTexture });
        pass.accesses.push_back({ token, FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled = false;   // compute only
        ctx.recipe.passes.push_back(std::move(pass));
        previous = token;

        ctx.callbacks.Register(std::string(kExecPrefix) + job.name,
            [res, bindings, job](const rendergraph::RGExecContext& ec)
            {
                if (!ec.cmd || !ec.device || !res || !res->programsReady || !bindings)
                    return;
                const auto* frame = bindings->scene.frameData.GetFrameData<SurfaceCacheFrameData>();
                if (!frame || !frame->world || !bindings->resources.perFrameCB.IsValid())
                    return;
                IDevice& device = *ec.device;
                // After a measuring run: copy the statistics once; the feature reads
                // them a few frames later (SurfaceCacheBench::kReadbackFrames). The
                // resolve step runs every frame, the trace passes only on demand.
                const auto copyStats = [&]
                {
                    if (!res->statsReadbackPending || res->statsCopied || !res->stats.IsValid())
                        return;
                    ec.cmd->TransitionResource(res->stats, ResourceState::ShaderRead, ResourceState::CopySource);
                    ec.cmd->CopyBuffer(res->readback[0], 0u, res->stats, 0u, SurfaceCacheGpuResources::kStatsWords * sizeof(uint32_t));
                    ec.cmd->TransitionResource(res->stats, ResourceState::CopySource, ResourceState::ShaderRead);
                    res->statsCopied = true;
                    res->statsReadbackWait = SurfaceCacheBench::kReadbackFrames;
                };
                if (job.jobKind == JobKind::Cache)
                {
                    ExecuteCacheStep(ec, *res, *bindings, *frame->world, job.step, job.width, job.height);
                    if (job.step == CacheStep::Resolve)
                        copyStats();
                    return;
                }
                const uint64_t pixels = static_cast<uint64_t>(job.primaryBase == 0u ? job.width * job.height : job.primaryBase);
                uint64_t benchPixels = 0u;
                if (BenchDispatching(res->bench))
                    for (const SurfaceCacheBenchPass& p : res->bench.passes)
                        benchPixels = std::max<uint64_t>(benchPixels, static_cast<uint64_t>(p.width) * p.height);
                if (!EnsureStructuredBuffer(device, res->primary, res->primaryCapacity, pixels + benchPixels,
                                            4u * sizeof(uint32_t), true, "SurfaceCache_Primary") ||
                    !EnsureStructuredBuffer(device, res->benchOut, res->benchCapacity, std::max<uint64_t>(benchPixels, 1u),
                                            sizeof(uint32_t), true, "SurfaceCache_BenchOut"))
                    return;
                DispatchTrace(ec, *res, *bindings, frame->world->gpu, job.kind, job.width, job.height,
                              job.tMax, job.primaryBase, job.statsSlot);
                if (job.kind == SurfaceCacheRayKind::Validate)
                {
                    ec.cmd->TransitionResource(res->validate, ResourceState::ShaderRead, ResourceState::CopySource);
                    ec.cmd->CopyBuffer(res->readback[1], 0u, res->validate, 0u,
                        SurfaceCacheValidation::kRays * SurfaceCacheValidation::kWordsPerRay * sizeof(uint32_t));
                    ec.cmd->TransitionResource(res->validate, ResourceState::CopySource, ResourceState::ShaderRead);
                    res->validation.state = SurfaceCacheValidation::State::Dispatched;
                    res->validation.waitFrames = SurfaceCacheValidation::kReadbackFrames;
                    res->validation.world = frame->world;
                    res->validation.version = frame->world->bvh.Scene().GeometryVersion();
                }
            });
    }

    if (m_resources->settings.apply && ctx.frame.enableDepthPrepass)
        BuildApplyPass(ctx, jobs.size() == 3u ? std::string(kDoneResource) : std::string(kTokenPrefix) + "2");
}

// Resolve for shading (package 4): after the cache steps of this frame, before
// the opaque pass, which reads the result on TexSlots::GlobalIllumination.
void SurfaceCacheComputePass::BuildApplyPass(const PassBuildContext& ctx, const std::string& resolveToken) const
{
    const uint32_t w = std::max(ctx.frame.viewportWidth, 1u);
    const uint32_t h = std::max(ctx.frame.viewportHeight, 1u);
    const uint32_t halfW = (w + 1u) / 2u;
    const uint32_t halfH = (h + 1u) / 2u;
    const bool msaa = ctx.frame.msaaSamples > 1u;
    ctx.recipe.resources.push_back(FrameRecipeResourceDesc{ std::string(kSurfaceCacheGiResource), false, halfW, halfH,
                                                            Format::RGBA16_FLOAT, RGResourceKind::ColorTexture });
    FrameRecipePassDesc pass{};
    pass.name = "SurfaceCacheApply";
    pass.executorName = std::string(kExecPrefix) + "apply";
    pass.accesses.push_back({ resolveToken, FrameRecipeAccessKind::ReadTexture });
    pass.accesses.push_back({ StandardFrameRecipeBuilder::kOpaqueDepthResource, FrameRecipeAccessKind::ReadDepthStencil });
    pass.accesses.push_back({ std::string(kSurfaceCacheGiResource), FrameRecipeAccessKind::WriteRenderTarget });
    pass.renderPass.enabled = true;
    pass.renderPass.targetResourceName = std::string(kSurfaceCacheGiResource);
    pass.renderPass.viewportWidth = halfW;
    pass.renderPass.viewportHeight = halfH;
    // Cleared to 0 (= sky, not applied) in case the executor has nothing to draw.
    pass.renderPass.clearColor = true;
    ctx.recipe.passes.push_back(std::move(pass));
    ctx.recipe.opaqueInputs.push_back({ std::string(kSurfaceCacheGiResource), TexSlots::GlobalIllumination });

    auto res = m_resources;
    auto bindings = ctx.frame.runtimeBindings;
    ctx.callbacks.Register(std::string(kExecPrefix) + "apply",
        [res, bindings, w, h, msaa](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !ec.device || !res || !res->programsReady || !bindings ||
                !bindings->resources.perFrameCB.IsValid())
                return;
            const auto* frame = bindings->scene.frameData.GetFrameData<SurfaceCacheFrameData>();
            if (!frame || !frame->world || !frame->world->grid.ready)
                return;
            const TextureHandle depth = ec.GetTexture(FindResourceId(ec, StandardFrameRecipeBuilder::kOpaqueDepthResource));
            const PipelineHandle pipeline = res->applyPipelines[msaa ? 1u : 0u];
            if (!depth.IsValid() || !pipeline.IsValid())
                return;
            ICommandList& cmd = *ec.cmd;
            const SurfaceCacheGrid& g = frame->world->grid;
            const uint32_t flags = BindFarField(cmd, *bindings, ShaderStageMask::Fragment);
            const SurfaceCacheConstants c = MakeCacheConstants(*res, *frame->world, w, h, flags);
            const BufferHandle cb = NextConstants(*ec.device, *res, &c, sizeof(c));
            if (!cb.IsValid())
                return;
            cmd.SetPipeline(pipeline);
            cmd.SetConstantBuffer(CBSlots::PerFrame, bindings->resources.perFrameCB, ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            cmd.SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Fragment);
            cmd.SetShaderResource(TexSlots::PassSRV0, depth, ShaderStageMask::Fragment);
            cmd.SetShaderResource(kApplyKeysSlot, g.keys, ShaderStageMask::Fragment);
            cmd.SetShaderResource(kApplyCellASlot, g.cellA, ShaderStageMask::Fragment);
            cmd.SetShaderResource(kApplyCellBSlot, g.cellB, ShaderStageMask::Fragment);
            cmd.Draw(3u, 1u, 0u, 0u);
            for (uint32_t slot : { kApplyKeysSlot, kApplyCellASlot, kApplyCellBSlot })
                cmd.SetShaderResource(slot, BufferHandle::Invalid(), ShaderStageMask::Fragment);
            for (uint32_t slot : { TexSlots::PassSRV0, TexSlots::IBLIrradiance, TexSlots::FeatureTexture })
                cmd.SetShaderResource(slot, TextureHandle::Invalid(), ShaderStageMask::Fragment);
        });
}

// ─── SurfaceCacheOverlayPass ─────────────────────────────────────────────────

uint64_t SurfaceCacheOverlayPass::GetStructureRevision() const noexcept
{
    return m_resources ? SurfaceCacheStructureRevision(*m_resources) : 0u;
}

void SurfaceCacheOverlayPass::BuildPass(const PassBuildContext& ctx) const
{
    if (!m_resources || SurfaceCacheStructureRevision(*m_resources) == 0u)
        return;
    // Always in the graph while enabled: it reads the token chain, which keeps
    // the compute passes from being culled. Draws only with a debug view.
    const uint32_t w = std::max(ctx.frame.viewportWidth, 1u);
    const uint32_t h = std::max(ctx.frame.viewportHeight, 1u);
    // Display overlay like the editor gizmos: on the finished image, after tonemapping.
    const std::string target = ctx.frame.hasSceneOutput ? "EditorSceneView" : "Backbuffer";
    const Format format = ctx.frame.outputColorFormat;

    FrameRecipePassDesc pass{};
    pass.name = "SurfaceCacheOverlay";
    pass.executorName = std::string(kExecPrefix) + "overlay";
    pass.accesses.push_back({ std::string(kDoneResource), FrameRecipeAccessKind::ReadTexture });
    pass.accesses.push_back({ target, FrameRecipeAccessKind::WriteRenderTarget });
    pass.renderPass.enabled = true;
    pass.renderPass.targetResourceName = target;
    pass.renderPass.viewportWidth = w;
    pass.renderPass.viewportHeight = h;
    pass.renderPass.clearColor = false;
    pass.renderPass.clearDepth = false;
    ctx.recipe.passes.push_back(std::move(pass));

    auto res = m_resources;
    auto bindings = ctx.frame.runtimeBindings;
    ctx.callbacks.Register(std::string(kExecPrefix) + "overlay",
        [res, bindings, w, h, format](const rendergraph::RGExecContext& ec)
        {
            if (!ec.cmd || !ec.device || !res || !res->programsReady || !bindings || !res->primary.IsValid())
                return;
            const SurfaceCacheSettings& s = res->settings;
            if (s.debugView == SurfaceCacheDebugView::None || IsShadedDebugView(s.debugView) || s.overlay <= 0.0f)
                return;
            const auto* frame = bindings->scene.frameData.GetFrameData<SurfaceCacheFrameData>();
            if (!frame || !frame->world || !bindings->resources.perFrameCB.IsValid())
                return;
            SurfaceCacheConstants constants = MakeCacheConstants(*res, *frame->world, w, h, 0u);
            constants.view = static_cast<uint32_t>(s.debugView);
            constants.overlayAlpha = std::clamp(s.overlay, 0.0f, 1.0f);
            constants.viewScale = std::max(s.distanceRange, 0.01f);
            if (s.debugView == SurfaceCacheDebugView::CacheIrradiance)
            {
                const float mean = frame->world->grid.lastCounters.meanRadiance;
                constants.viewScale = mean > 1e-5f ? 1.0f / mean : 1.0f;
            }
            const BufferHandle cb = NextConstants(*ec.device, *res, &constants, sizeof(constants));
            const PipelineHandle pipeline = OverlayPipeline(*ec.device, *res, format);
            if (!cb.IsValid() || !pipeline.IsValid())
                return;
            const SurfaceCacheGrid& g = frame->world->grid;
            const bool cacheView = static_cast<uint32_t>(s.debugView) >= static_cast<uint32_t>(SurfaceCacheDebugView::CacheCells);
            if (cacheView && !g.ready)
                return;
            ec.cmd->SetPipeline(pipeline);
            ec.cmd->SetConstantBuffer(CBSlots::PerFrame, bindings->resources.perFrameCB, ShaderStageMask::Fragment);
            ec.cmd->SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(kPrimaryBufferSlot, res->primary, ShaderStageMask::Fragment);
            if (g.ready)
            {
                ec.cmd->SetShaderResource(kOverlayKeysSlot, g.keys, ShaderStageMask::Fragment);
                ec.cmd->SetShaderResource(kOverlayCellASlot, g.cellA, ShaderStageMask::Fragment);
            }
            ec.cmd->Draw(3u, 1u, 0u, 0u);
            for (uint32_t slot : { kPrimaryBufferSlot, kOverlayKeysSlot, kOverlayCellASlot })
                ec.cmd->SetShaderResource(slot, BufferHandle::Invalid(), ShaderStageMask::Fragment);
        });
}

} // namespace engine::renderer::addons::surface_cache
