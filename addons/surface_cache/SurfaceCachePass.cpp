#include "SurfaceCachePass.hpp"

#include "addons/forward/StandardFramePipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/RenderPipelineRecipe.hpp"
#include "renderer/RenderRuntimeFrameBindings.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderCompiler.hpp"
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
// this view (one uint4 per pixel, see kTraceCsHlsl).
const char* const kOverlayPsHlsl = R"(#include "krom_platform.hlsl"

KROM_BUFFER_SRV(StructuredBuffer<uint4>, gPrimaryResults, 4);

cbuffer SurfaceCacheOverlay : register(b3)
{
    uint  ovWidth;
    uint  ovHeight;
    uint  ovView;      // SurfaceCacheDebugView
    float ovAlpha;
    float ovDistanceRange;
    float ovPad0;
    float ovPad1;
    float ovPad2;
};

float3 OverlayOctDecode(uint packed)
{
    const float2 e = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return normalize(n);
}

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
    if (p.x >= ovWidth || p.y >= ovHeight)
        discard;
    const uint4 r = gPrimaryResults[p.y * ovWidth + p.x];
    const bool hit = r.x != 0xFFFFFFFFu;
    float3 color = float3(0.05f, 0.05f, 0.09f);
    if (ovView == 4u)   // nodes visited
        color = HeatColor((r.w & 0xFFFFu) / 160.0f);
    else if (hit && ovView == 1u)
        color = OverlayOctDecode(r.y) * 0.5f + 0.5f;
    else if (hit && ovView == 2u)
        color = saturate(asfloat(r.x) / max(ovDistanceRange, 1e-3f)).xxx;
    else if (hit && ovView == 3u)
        color = InstanceColor(r.z);
    return float4(color, ovAlpha);
}
)";

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

constexpr std::string_view kDoneResource   = "SurfaceCacheDone";
constexpr const char*      kTokenPrefix    = "SurfaceCacheToken";
constexpr const char*      kExecPrefix     = "frame.surface_cache.";
constexpr uint32_t         kGroupSize      = 8u;
constexpr uint32_t         kConstantCopies = 96u;   // round-robin per dispatch
constexpr uint32_t         kPrimaryBufferSlot = 4u; // BufSRV slot of the overlay

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
    if (res.csTrace.IsValid())
    {
        PipelineDesc desc{};
        desc.pipelineClass = PipelineClass::Compute;
        desc.debugName = "SurfaceCacheTrace_Pipeline";
        desc.shaderStages.push_back(ShaderStageDesc{ res.csTrace, ShaderStageMask::Compute, "main" });
        res.traceCompute = device.CreatePipeline(desc);
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
                        res.traceCompute.IsValid() && res.stats.IsValid() && res.validate.IsValid() &&
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
    }
    res.worlds.clear();
    for (auto& [format, pipeline] : res.overlayPipelines)
        if (pipeline.IsValid())
            device.DestroyPipeline(pipeline);
    res.overlayPipelines.clear();
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
    return revision;
}

// ─── SurfaceCacheComputePass ─────────────────────────────────────────────────

bool SurfaceCacheComputePass::DeclaresResource(std::string_view name) const noexcept
{
    return m_resources && SurfaceCacheStructureRevision(*m_resources) != 0u &&
           (name == kDoneResource || name.substr(0, std::string_view(kTokenPrefix).size()) == kTokenPrefix);
}

uint64_t SurfaceCacheComputePass::GetStructureRevision() const noexcept
{
    return m_resources ? SurfaceCacheStructureRevision(*m_resources) : 0u;
}

void SurfaceCacheComputePass::BuildPass(const PassBuildContext& ctx) const
{
    if (!m_resources || SurfaceCacheStructureRevision(*m_resources) == 0u)
        return;

    struct Job
    {
        std::string         name;
        SurfaceCacheRayKind kind;
        uint32_t            width;
        uint32_t            height;
        float               tMax;
        uint32_t            primaryBase;
        uint32_t            statsSlot;
    };
    // The debug trace of this view always runs; it owns primary[0, w*h). The
    // measuring passes put their primary results behind it.
    const uint32_t w = std::max(ctx.frame.viewportWidth, 1u);
    const uint32_t h = std::max(ctx.frame.viewportHeight, 1u);
    std::vector<Job> jobs;
    jobs.push_back({ "SurfaceCacheTrace", SurfaceCacheRayKind::Primary, w, h, 0.0f, 0u,
                     SurfaceCacheGpuResources::kDebugTraceSlot });
    if (BenchDispatching(m_resources->bench))
        for (size_t k = 0u; k < m_resources->bench.passes.size(); ++k)
        {
            const SurfaceCacheBenchPass& p = m_resources->bench.passes[k];
            jobs.push_back({ p.name, p.kind, p.width, p.height, p.tMax, w * h, static_cast<uint32_t>(k) });
        }
    if (m_resources->validation.state == SurfaceCacheValidation::State::Requested)
        jobs.push_back({ "SurfaceCacheValidate", SurfaceCacheRayKind::Validate, 16u, 16u, 0.0f, 0u,
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
        pass.accesses.push_back({ token, FrameRecipeAccessKind::WriteRenderTarget });
        pass.renderPass.enabled = false;   // compute only
        ctx.recipe.passes.push_back(std::move(pass));
        previous = token;

        const bool isDebugTrace = i == 0u;
        ctx.callbacks.Register(std::string(kExecPrefix) + job.name,
            [res, bindings, job, isDebugTrace](const rendergraph::RGExecContext& ec)
            {
                if (!ec.cmd || !ec.device || !res || !res->programsReady || !bindings)
                    return;
                const auto* frame = bindings->scene.frameData.GetFrameData<SurfaceCacheFrameData>();
                if (!frame || !frame->world || !bindings->resources.perFrameCB.IsValid())
                    return;
                IDevice& device = *ec.device;
                if (isDebugTrace)
                {
                    const uint64_t pixels = static_cast<uint64_t>(job.width) * job.height;
                    uint64_t benchPixels = 0u;
                    if (BenchDispatching(res->bench))
                        for (const SurfaceCacheBenchPass& p : res->bench.passes)
                            benchPixels = std::max<uint64_t>(benchPixels, static_cast<uint64_t>(p.width) * p.height);
                    if (!EnsureStructuredBuffer(device, res->primary, res->primaryCapacity, pixels + benchPixels,
                                                4u * sizeof(uint32_t), true, "SurfaceCache_Primary") ||
                        !EnsureStructuredBuffer(device, res->benchOut, res->benchCapacity, std::max<uint64_t>(benchPixels, 1u),
                                                sizeof(uint32_t), true, "SurfaceCache_BenchOut"))
                        return;
                }
                if (!res->primary.IsValid() || !res->benchOut.IsValid())
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
                // After a measuring run: copy the statistics once; the feature reads
                // them a few frames later (SurfaceCacheBench::kReadbackFrames).
                if (isDebugTrace && res->statsReadbackPending && !res->statsCopied)
                {
                    ec.cmd->TransitionResource(res->stats, ResourceState::ShaderRead, ResourceState::CopySource);
                    ec.cmd->CopyBuffer(res->readback[0], 0u, res->stats, 0u, SurfaceCacheGpuResources::kStatsWords * sizeof(uint32_t));
                    ec.cmd->TransitionResource(res->stats, ResourceState::CopySource, ResourceState::ShaderRead);
                    res->statsCopied = true;
                    res->statsReadbackWait = SurfaceCacheBench::kReadbackFrames;
                }
            });
    }
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
            if (s.debugView == SurfaceCacheDebugView::None || s.overlay <= 0.0f)
                return;
            const auto* frame = bindings->scene.frameData.GetFrameData<SurfaceCacheFrameData>();
            if (!frame || !frame->world)
                return;
            struct OverlayConstants
            {
                uint32_t width, height, view;
                float    alpha, distanceRange, pad0, pad1, pad2;
            } constants{ w, h, static_cast<uint32_t>(s.debugView), std::clamp(s.overlay, 0.0f, 1.0f),
                         std::max(s.distanceRange, 0.01f), 0.0f, 0.0f, 0.0f };
            const BufferHandle cb = NextConstants(*ec.device, *res, &constants, sizeof(constants));
            const PipelineHandle pipeline = OverlayPipeline(*ec.device, *res, format);
            if (!cb.IsValid() || !pipeline.IsValid())
                return;
            ec.cmd->SetPipeline(pipeline);
            ec.cmd->SetConstantBuffer(CBSlots::PerPass, cb, ShaderStageMask::Vertex | ShaderStageMask::Fragment);
            ec.cmd->SetShaderResource(kPrimaryBufferSlot, res->primary, ShaderStageMask::Fragment);
            ec.cmd->Draw(3u, 1u, 0u, 0u);
            ec.cmd->SetShaderResource(kPrimaryBufferSlot, BufferHandle::Invalid(), ShaderStageMask::Fragment);
        });
}

} // namespace engine::renderer::addons::surface_cache
