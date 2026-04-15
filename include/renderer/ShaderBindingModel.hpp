#pragma once
// =============================================================================
// KROM Engine - renderer/ShaderBindingModel.hpp
// Explizites Binding-Modell für alle Backends und Shader.
//
// ALLE Backends müssen exakt diese Slot-Konventionen einhalten.
// Shader müssen diese Register nutzen.
//
// Constant Buffer Slots:
//   CB0 = PerFrame     - Kamera, Lichter, Zeit, Viewport (einmal pro Frame)
//   CB1 = PerObject    - WorldMatrix, InvTranspose, EntityID (pro Draw Call)
//   CB2 = PerMaterial  - Albedo, Roughness, Metallic, etc. (pro Materialwechsel)
//   CB3 = PerPass      - Pass-spezifische Konstanten (Shadow-Kamera, PostProc-Params)
//
// Texture/SRV Slots:
//   t0  = Albedo / BaseColor Map
//   t1  = Normal Map
//   t2  = ORM (Occlusion/Roughness/Metallic)
//   t3  = Emissive Map
//   t4  = Shadow Map (Depth)
//   t5  = IBL Irradiance (aktueller Pfad: 2D equirectangular)
//   t6  = IBL Prefiltered (aktueller Pfad: 2D equirectangular, darf dieselbe Quelle wie t5 nutzen)
//   t7  = BRDF LUT / aktuelles BRDF-Domaenen-LUT-Zwischenmodell
//   t8..t15 = Pass-spezifische SRVs (History Buffer, Bloom etc.)
//
// Sampler Slots:
//   s0  = LinearWrap   (Default für Texturen)
//   s1  = LinearClamp  (PostProcess, UI)
//   s2  = PointClamp   (Debug, Nearest)
//   s3  = ShadowPCF    (comparison sampler für Shadow Map)
//
// UAV Slots (Compute):
//   u0..u7 = Compute-Output-Texturen / Buffers
// =============================================================================
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include "renderer/RendererTypes.hpp"

namespace engine::renderer {

// ---------------------------------------------------------------------------
// CB-Slots
// ---------------------------------------------------------------------------
struct CBSlots
{
    static constexpr uint32_t PerFrame    = 0u;
    static constexpr uint32_t PerObject   = 1u;
    static constexpr uint32_t PerMaterial = 2u;
    static constexpr uint32_t PerPass     = 3u;

    static constexpr uint32_t COUNT       = 4u;
};

// ---------------------------------------------------------------------------
// Texture-Slots
// ---------------------------------------------------------------------------
struct TexSlots
{
    static constexpr uint32_t Albedo          = 0u;
    static constexpr uint32_t Normal          = 1u;
    static constexpr uint32_t ORM             = 2u; // R=Occlusion, G=Roughness, B=Metallic
    static constexpr uint32_t Emissive        = 3u;
    static constexpr uint32_t ShadowMap       = 4u;
    static constexpr uint32_t IBLIrradiance   = 5u;
    static constexpr uint32_t IBLPrefiltered  = 6u;
    static constexpr uint32_t BRDFLUT         = 7u;

    // Pass-dynamische Slots
    static constexpr uint32_t PassSRV0        = 8u;
    static constexpr uint32_t PassSRV1        = 9u;
    static constexpr uint32_t PassSRV2        = 10u;
    static constexpr uint32_t HistoryBuffer   = 11u;
    static constexpr uint32_t BloomTexture    = 12u;

    static constexpr uint32_t COUNT           = 16u;
};

// ---------------------------------------------------------------------------
// Sampler-Slots
// ---------------------------------------------------------------------------
struct SamplerSlots
{
    static constexpr uint32_t LinearWrap  = 0u;
    static constexpr uint32_t LinearClamp = 1u;
    static constexpr uint32_t PointClamp  = 2u;
    static constexpr uint32_t ShadowPCF   = 3u;

    static constexpr uint32_t COUNT       = 4u;
};

// ---------------------------------------------------------------------------
// UAV-Slots (Compute)
// ---------------------------------------------------------------------------
struct UAVSlots
{
    static constexpr uint32_t Output0 = 0u;
    static constexpr uint32_t Output1 = 1u;
    static constexpr uint32_t COUNT   = 8u;
};


// ---------------------------------------------------------------------------
// BindingRegisterRanges
//
// Fachlicher Binding-Vertrag: abstrakte Slots (CBSlots/TexSlots/SamplerSlots/UAVSlots)
// werden auf lineare API-Registerbereiche abgebildet.
//
// Motivation:
//   - Vulkan materialisiert daraus DescriptorSetLayouts / DescriptorWrites.
//   - DX12 kann daraus später Root Signatures + Descriptor Heaps ableiten.
//   - DX11/OpenGL nutzen weiterhin die abstrakten Slots direkt.
//
// Wichtig: Diese Register-Offsets sind Teil des Engine-Vertrags und müssen
// mit den Shader-Layouts (HLSL register(...) / GLSL layout(binding=...))
// übereinstimmen.
// ---------------------------------------------------------------------------
struct BindingRegisterRanges
{
    static constexpr uint32_t RegisterSpace      = 0u;

    static constexpr uint32_t ConstantBufferBase = 0u;
    static constexpr uint32_t ShaderResourceBase = 16u;
    static constexpr uint32_t SamplerBase        = 32u;
    static constexpr uint32_t UnorderedAccessBase= 48u;

    static constexpr uint32_t CB(uint32_t slot)  noexcept { return ConstantBufferBase + slot; }
    static constexpr uint32_t SRV(uint32_t slot) noexcept { return ShaderResourceBase + slot; }
    static constexpr uint32_t SMP(uint32_t slot) noexcept { return SamplerBase + slot; }
    static constexpr uint32_t UAV(uint32_t slot) noexcept { return UnorderedAccessBase + slot; }
};

inline uint32_t ToStageMaskBits(ShaderStageMask mask) noexcept
{
    return static_cast<uint32_t>(mask);
}

enum class BindingHeapKind : uint8_t
{
    Resource = 0,
    Sampler,
};

struct BindingRangeDesc
{
    DescriptorType   type = DescriptorType::ConstantBuffer;
    uint32_t         logicalBaseSlot = 0u;
    uint32_t         registerBase = 0u;
    uint32_t         descriptorCount = 0u;
    uint32_t         registerSpace = 0u;
    ShaderStageMask  visibility = ShaderStageMask::None;
    bool             usesDynamicOffset = false;
};

struct BindingTableDesc
{
    BindingHeapKind heap = BindingHeapKind::Resource;
    uint32_t        rangeIndex = 0u;
    uint32_t        rangeCount = 0u;
};

struct PipelineBindingLayoutDesc
{
    std::array<BindingRangeDesc, 4> ranges{};
    std::array<BindingTableDesc, 2> tables{};
    uint32_t rangeCount = 0u;
    uint32_t tableCount = 0u;

    [[nodiscard]] uint32_t CountDescriptors(BindingHeapKind heap, DescriptorType type) const noexcept
    {
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < rangeCount; ++i)
        {
            const BindingRangeDesc& range = ranges[i];
            const bool resourceHeap = range.type != DescriptorType::Sampler;
            const BindingHeapKind rangeHeap = resourceHeap ? BindingHeapKind::Resource : BindingHeapKind::Sampler;
            if (rangeHeap == heap && range.type == type)
                count += range.descriptorCount;
        }
        return count;
    }

    [[nodiscard]] uint32_t CountDynamicOffsets() const noexcept
    {
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < rangeCount; ++i)
        {
            if (ranges[i].type == DescriptorType::ConstantBuffer && ranges[i].usesDynamicOffset)
                count += ranges[i].descriptorCount;
        }
        return count;
    }
};




enum class DescriptorBindingInvalidationReason : uint32_t
{
    None                  = 0u,
    BoundValueChanged     = 1u << 0u,
    MaterializedValueChanged = 1u << 1u,
    AllocationEpochChanged= 1u << 2u,
    ExplicitDirty         = 1u << 3u,
};

inline DescriptorBindingInvalidationReason operator|(DescriptorBindingInvalidationReason a,
                                                     DescriptorBindingInvalidationReason b) noexcept
{
    return static_cast<DescriptorBindingInvalidationReason>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline DescriptorBindingInvalidationReason& operator|=(DescriptorBindingInvalidationReason& a,
                                                       DescriptorBindingInvalidationReason b) noexcept
{
    a = a | b;
    return a;
}

[[nodiscard]] inline bool HasDescriptorBindingInvalidationReason(DescriptorBindingInvalidationReason value,
                                                                 DescriptorBindingInvalidationReason bit) noexcept
{
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(bit)) != 0u;
}

struct DescriptorBindingState
{
    std::array<BufferBinding, CBSlots::COUNT> constantBuffers{};
    std::array<TextureHandle, TexSlots::COUNT> textures{};
    std::array<uint32_t, SamplerSlots::COUNT> samplers{};
};

struct DescriptorMaterializationState
{
    std::array<BufferBinding, CBSlots::COUNT> constantBuffers{};
    std::array<TextureHandle, TexSlots::COUNT> textures{};
    std::array<uint32_t, SamplerSlots::COUNT> samplers{};
    std::array<ResourceState, TexSlots::COUNT> textureStates{};
    std::array<uint64_t, TexSlots::COUNT> textureRevisionKeys{};
};

[[nodiscard]] inline DescriptorBindingState BuildDefaultDescriptorBindingState() noexcept
{
    DescriptorBindingState state{};
    for (auto& texture : state.textures)
        texture = TextureHandle::Invalid();
    return state;
}

[[nodiscard]] inline DescriptorMaterializationState BuildDefaultDescriptorMaterializationState() noexcept
{
    DescriptorMaterializationState state{};
    for (auto& texture : state.textures)
        texture = TextureHandle::Invalid();
    for (auto& resourceState : state.textureStates)
        resourceState = ResourceState::Unknown;
    return state;
}

[[nodiscard]] inline bool BufferBindingsEqual(const BufferBinding& a, const BufferBinding& b) noexcept
{
    return a.buffer == b.buffer && a.offset == b.offset && a.size == b.size;
}

[[nodiscard]] inline bool DescriptorBindingStatesEqual(const DescriptorBindingState& a,
                                                       const DescriptorBindingState& b) noexcept
{
    for (size_t i = 0u; i < a.constantBuffers.size(); ++i)
    {
        if (!BufferBindingsEqual(a.constantBuffers[i], b.constantBuffers[i]))
            return false;
    }
    return a.textures == b.textures &&
           a.samplers == b.samplers;
}

[[nodiscard]] inline bool DescriptorMaterializationStatesEqual(const DescriptorMaterializationState& a,
                                                               const DescriptorMaterializationState& b) noexcept
{
    for (size_t i = 0u; i < a.constantBuffers.size(); ++i)
    {
        if (!BufferBindingsEqual(a.constantBuffers[i], b.constantBuffers[i]))
            return false;
    }
    return a.textures == b.textures &&
           a.samplers == b.samplers &&
           a.textureStates == b.textureStates &&
           a.textureRevisionKeys == b.textureRevisionKeys;
}

[[nodiscard]] inline DescriptorBindingInvalidationReason ComputeDescriptorBindingInvalidation(const DescriptorBindingState& previousBoundState,
                                                                                              const DescriptorBindingState& currentBoundState,
                                                                                              const DescriptorMaterializationState& previousMaterialization,
                                                                                              const DescriptorMaterializationState& currentMaterialization,
                                                                                              bool allocationEpochChanged,
                                                                                              bool explicitDirty) noexcept
{
    DescriptorBindingInvalidationReason reasons = DescriptorBindingInvalidationReason::None;
    if (!DescriptorBindingStatesEqual(previousBoundState, currentBoundState))
        reasons |= DescriptorBindingInvalidationReason::BoundValueChanged;
    if (!DescriptorMaterializationStatesEqual(previousMaterialization, currentMaterialization))
        reasons |= DescriptorBindingInvalidationReason::MaterializedValueChanged;
    if (allocationEpochChanged)
        reasons |= DescriptorBindingInvalidationReason::AllocationEpochChanged;
    if (explicitDirty)
        reasons |= DescriptorBindingInvalidationReason::ExplicitDirty;
    return reasons;
}

[[nodiscard]] inline std::string DescribeDescriptorBindingInvalidation(DescriptorBindingInvalidationReason reasons)
{
    if (reasons == DescriptorBindingInvalidationReason::None)
        return "none";

    std::string text;
    const auto append = [&](const char* label)
    {
        if (!text.empty())
            text += '|';
        text += label;
    };

    if (HasDescriptorBindingInvalidationReason(reasons, DescriptorBindingInvalidationReason::BoundValueChanged))
        append("binding");
    if (HasDescriptorBindingInvalidationReason(reasons, DescriptorBindingInvalidationReason::MaterializedValueChanged))
        append("materialization");
    if (HasDescriptorBindingInvalidationReason(reasons, DescriptorBindingInvalidationReason::AllocationEpochChanged))
        append("allocation");
    if (HasDescriptorBindingInvalidationReason(reasons, DescriptorBindingInvalidationReason::ExplicitDirty))
        append("explicit");
    return text;
}

enum class DescriptorVisibilityModel : uint8_t
{
    EmulatedShaderVisible = 0,
    NativeShaderVisible,
};

enum class DescriptorUpdatePolicy : uint8_t
{
    DirectWrite = 0,
    StageAndCopyPerFrame,
};

// CPU-seitige oder backend-interne Descriptor-Repräsentation.
// DX12: CPU-Heap/Cache, Vulkan: Layout-/Write-Metadaten bzw. Pool-Materialisierung.
enum class DescriptorStorageClass : uint8_t
{
    PersistentDevice = 0,
    TransientPerFrame,
};

enum class DescriptorArenaModel : uint8_t
{
    LinearPerFrame = 0,
    RingPerFrame,
};

enum class DescriptorRetirementModel : uint8_t
{
    PerFrameSlotFence = 0,
    ExplicitFenceValue,
};

enum class SamplerMaterializationPolicy : uint8_t
{
    DedicatedHeap = 0,
    InlineWithDescriptorTable,
    StaticBindOnly,
};

enum class RootParameterKind : uint8_t
{
    DescriptorTable = 0,
    RootConstantBufferView,
    RootConstants,
};

// Gemeinsamer Vertrag für transient shader-visible Descriptor-Suballokation.
// v1: pro Frame-Slot linear, Freigabe über abgeschlossene Frame-Fence.
struct FrameDescriptorArenaDesc
{
    uint32_t maxSetsPerFrame = 256u;
    uint32_t resourceDescriptorCount = 0u;
    uint32_t samplerDescriptorCount = 0u;
    DescriptorArenaModel arenaModel = DescriptorArenaModel::LinearPerFrame;
    DescriptorRetirementModel retirementModel = DescriptorRetirementModel::PerFrameSlotFence;
};

// Backend-neutrale Heap-Politik. Vulkan materialisiert daraus Pools/Sets,
// DX12 später CPU-Heaps + shader-visible Heaps/Tables.
struct BindingHeapRuntimeDesc
{
    BindingHeapKind            heap = BindingHeapKind::Resource;
    DescriptorVisibilityModel  visibility = DescriptorVisibilityModel::EmulatedShaderVisible;
    DescriptorUpdatePolicy     updatePolicy = DescriptorUpdatePolicy::DirectWrite;
    DescriptorStorageClass     cpuDescriptorStorage = DescriptorStorageClass::PersistentDevice;
    DescriptorStorageClass     shaderVisibleDescriptorStorage = DescriptorStorageClass::TransientPerFrame;
    DescriptorArenaModel       shaderVisibleArenaModel = DescriptorArenaModel::LinearPerFrame;
    DescriptorRetirementModel  retirementModel = DescriptorRetirementModel::PerFrameSlotFence;
    SamplerMaterializationPolicy samplerPolicy = SamplerMaterializationPolicy::InlineWithDescriptorTable;
    uint32_t                   persistentDescriptorCapacity = 0u;
    uint32_t                   frameVisibleDescriptorCapacity = 0u;
    bool                       bindOncePerCommandList = true;
    bool                       retirePerFrameFence = true;
};

struct RootParameterDesc
{
    RootParameterKind kind = RootParameterKind::DescriptorTable;
    BindingHeapKind   heap = BindingHeapKind::Resource;
    uint32_t          tableIndex = 0u;
    ShaderStageMask   visibility = ShaderStageMask::None;
    bool              bindPerDraw = true;
};



enum class StaticSamplerPolicy : uint8_t
{
    None = 0,
    EngineStaticSamplers,
};

// API-neutrale Pipeline-Bindungssignatur. DX12 materialisiert daraus später
// Root-Parameter / Descriptor-Tabellen, Vulkan Layout-/Set-Metadaten, andere
// Backends eine vereinfachte Bindungsform. Der Engine-Vertrag benennt bewusst
// keine API-spezifischen Objekte als Primärabstraktion.
struct PipelineBindingTableDesc
{
    BindingHeapKind heap = BindingHeapKind::Resource;
    uint32_t        tableIndex = 0u;
    uint32_t        descriptorCount = 0u;
    uint32_t        registerSpace = 0u;
};

struct StaticSamplerBindingDesc
{
    uint32_t        logicalSlot = 0u;
    uint32_t        registerBase = 0u;
    uint32_t        registerSpace = 0u;
    ShaderStageMask visibility = ShaderStageMask::None;
    bool            comparisonSampler = false;
};

struct PipelineBindingSignatureDesc
{
    PipelineBindingLayoutDesc                   bindingLayout{};
    std::array<RootParameterDesc, 4u>          parameters{};
    std::array<PipelineBindingTableDesc, 4u>   tables{};
    std::array<StaticSamplerBindingDesc, 8u>   staticSamplers{};
    uint32_t                                   parameterCount = 0u;
    uint32_t                                   tableCount = 0u;
    uint32_t                                   staticSamplerCount = 0u;
    StaticSamplerPolicy                        staticSamplerPolicy = StaticSamplerPolicy::None;
};

using RootDescriptorTableDesc = PipelineBindingTableDesc;
using RootSignatureLayoutDesc = PipelineBindingSignatureDesc;
struct DescriptorRuntimeLayoutDesc
{
    PipelineBindingLayoutDesc              bindingLayout{};
    FrameDescriptorArenaDesc               frameArena{};
    std::array<BindingHeapRuntimeDesc, 2u> heaps{};
    std::array<RootParameterDesc, 4u>      rootParameters{};
    uint32_t                               heapCount = 0u;
    uint32_t                               rootParameterCount = 0u;
};


[[nodiscard]] inline bool ValidateDescriptorRuntimeLayout(const DescriptorRuntimeLayoutDesc& desc,
                                                          std::string* errorMessage = nullptr) noexcept
{
    auto fail = [&](const char* message) -> bool
    {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };

    if (desc.bindingLayout.rangeCount < 4u)
        return fail("descriptor runtime layout is missing required binding ranges");
    if (desc.bindingLayout.tableCount < 2u)
        return fail("descriptor runtime layout is missing required binding tables");
    if (desc.bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ConstantBuffer) < CBSlots::COUNT)
        return fail("descriptor runtime layout does not expose all engine constant-buffer slots");
    if (desc.bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ShaderResource) < TexSlots::COUNT)
        return fail("descriptor runtime layout does not expose all engine shader-resource slots");
    if (desc.bindingLayout.CountDescriptors(BindingHeapKind::Sampler, DescriptorType::Sampler) < SamplerSlots::COUNT)
        return fail("descriptor runtime layout does not expose all engine sampler slots");
    if (desc.bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::UnorderedAccess) < UAVSlots::COUNT)
        return fail("descriptor runtime layout does not expose all engine UAV slots");
    if (desc.bindingLayout.CountDynamicOffsets() < CBSlots::COUNT)
        return fail("descriptor runtime layout does not provide dynamic offsets for all engine constant-buffer slots");
    return true;
}

[[nodiscard]] inline const BindingHeapRuntimeDesc* FindBindingHeapRuntimeDesc(const DescriptorRuntimeLayoutDesc& desc,
                                                                              BindingHeapKind heap) noexcept
{
    for (uint32_t i = 0u; i < desc.heapCount; ++i)
    {
        if (desc.heaps[i].heap == heap)
            return &desc.heaps[i];
    }
    return nullptr;
}

[[nodiscard]] inline uint32_t CountBindingRangeDescriptors(const PipelineBindingLayoutDesc& layout,
                                                           uint32_t rangeIndex) noexcept
{
    if (rangeIndex >= layout.rangeCount)
        return 0u;
    return layout.ranges[rangeIndex].descriptorCount;
}

[[nodiscard]] inline uint32_t CountBindingTableDescriptors(const PipelineBindingLayoutDesc& layout,
                                                           uint32_t tableIndex) noexcept
{
    if (tableIndex >= layout.tableCount)
        return 0u;

    const BindingTableDesc& table = layout.tables[tableIndex];
    uint32_t descriptorCount = 0u;
    for (uint32_t i = 0u; i < table.rangeCount; ++i)
        descriptorCount += CountBindingRangeDescriptors(layout, table.rangeIndex + i);
    return descriptorCount;
}

[[nodiscard]] inline uint32_t CountHeapDescriptorsPerSet(const DescriptorRuntimeLayoutDesc& desc,
                                                         BindingHeapKind heap) noexcept
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < desc.bindingLayout.tableCount; ++i)
    {
        const BindingTableDesc& table = desc.bindingLayout.tables[i];
        if (table.heap == heap)
            count += CountBindingTableDescriptors(desc.bindingLayout, i);
    }
    return count;
}

[[nodiscard]] inline uint32_t GetFrameVisibleDescriptorCapacity(const DescriptorRuntimeLayoutDesc& desc,
                                                                BindingHeapKind heap) noexcept
{
    if (const BindingHeapRuntimeDesc* heapDesc = FindBindingHeapRuntimeDesc(desc, heap))
        return heapDesc->frameVisibleDescriptorCapacity;
    return 0u;
}

[[nodiscard]] inline uint32_t GetPersistentDescriptorCapacity(const DescriptorRuntimeLayoutDesc& desc,
                                                              BindingHeapKind heap) noexcept
{
    if (const BindingHeapRuntimeDesc* heapDesc = FindBindingHeapRuntimeDesc(desc, heap))
        return heapDesc->persistentDescriptorCapacity;
    return 0u;
}


[[nodiscard]] inline uint64_t HashBindingModelValue(uint64_t seed, uint64_t value) noexcept
{
    return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u));
}

[[nodiscard]] inline uint64_t HashPipelineBindingLayoutDesc(const PipelineBindingLayoutDesc& desc) noexcept
{
    uint64_t hash = 1469598103934665603ull;
    hash = HashBindingModelValue(hash, desc.rangeCount);
    hash = HashBindingModelValue(hash, desc.tableCount);
    for (uint32_t i = 0u; i < desc.rangeCount; ++i)
    {
        const auto& range = desc.ranges[i];
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(range.type));
        hash = HashBindingModelValue(hash, range.logicalBaseSlot);
        hash = HashBindingModelValue(hash, range.registerBase);
        hash = HashBindingModelValue(hash, range.descriptorCount);
        hash = HashBindingModelValue(hash, range.registerSpace);
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(ToStageMaskBits(range.visibility)));
        hash = HashBindingModelValue(hash, range.usesDynamicOffset ? 1ull : 0ull);
    }
    for (uint32_t i = 0u; i < desc.tableCount; ++i)
    {
        const auto& table = desc.tables[i];
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(table.heap));
        hash = HashBindingModelValue(hash, table.rangeIndex);
        hash = HashBindingModelValue(hash, table.rangeCount);
    }
    return hash;
}

[[nodiscard]] inline uint64_t HashDescriptorRuntimeLayoutDesc(const DescriptorRuntimeLayoutDesc& desc) noexcept
{
    uint64_t hash = HashPipelineBindingLayoutDesc(desc.bindingLayout);
    hash = HashBindingModelValue(hash, desc.frameArena.maxSetsPerFrame);
    hash = HashBindingModelValue(hash, desc.frameArena.resourceDescriptorCount);
    hash = HashBindingModelValue(hash, desc.frameArena.samplerDescriptorCount);
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.frameArena.arenaModel));
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.frameArena.retirementModel));
    hash = HashBindingModelValue(hash, desc.heapCount);
    for (uint32_t i = 0u; i < desc.heapCount; ++i)
    {
        const auto& heap = desc.heaps[i];
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.heap));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.visibility));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.updatePolicy));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.cpuDescriptorStorage));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.shaderVisibleDescriptorStorage));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.shaderVisibleArenaModel));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.retirementModel));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(heap.samplerPolicy));
        hash = HashBindingModelValue(hash, heap.persistentDescriptorCapacity);
        hash = HashBindingModelValue(hash, heap.frameVisibleDescriptorCapacity);
        hash = HashBindingModelValue(hash, heap.bindOncePerCommandList ? 1ull : 0ull);
        hash = HashBindingModelValue(hash, heap.retirePerFrameFence ? 1ull : 0ull);
    }
    hash = HashBindingModelValue(hash, desc.rootParameterCount);
    for (uint32_t i = 0u; i < desc.rootParameterCount; ++i)
    {
        const auto& param = desc.rootParameters[i];
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(param.kind));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(param.heap));
        hash = HashBindingModelValue(hash, param.tableIndex);
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(ToStageMaskBits(param.visibility)));
        hash = HashBindingModelValue(hash, param.bindPerDraw ? 1ull : 0ull);
    }
    return hash;
}

[[nodiscard]] inline PipelineBindingSignatureDesc DerivePipelineBindingSignatureDesc(const DescriptorRuntimeLayoutDesc& runtimeLayout,
                                                                          StaticSamplerPolicy staticSamplerPolicy = StaticSamplerPolicy::None) noexcept
{
    PipelineBindingSignatureDesc desc{};
    desc.bindingLayout = runtimeLayout.bindingLayout;
    desc.parameterCount = runtimeLayout.rootParameterCount;
    desc.staticSamplerPolicy = staticSamplerPolicy;
    for (uint32_t i = 0u; i < runtimeLayout.rootParameterCount; ++i)
    {
        const RootParameterDesc& src = runtimeLayout.rootParameters[i];
        desc.parameters[i] = src;
        if (src.kind != RootParameterKind::DescriptorTable)
            continue;
        PipelineBindingTableDesc tableDesc{};
        tableDesc.heap = src.heap;
        tableDesc.tableIndex = src.tableIndex;
        tableDesc.descriptorCount = CountBindingTableDescriptors(runtimeLayout.bindingLayout, src.tableIndex);
        if (src.tableIndex < runtimeLayout.bindingLayout.tableCount)
        {
            const BindingTableDesc& table = runtimeLayout.bindingLayout.tables[src.tableIndex];
            if (table.rangeCount > 0u && table.rangeIndex < runtimeLayout.bindingLayout.rangeCount)
                tableDesc.registerSpace = runtimeLayout.bindingLayout.ranges[table.rangeIndex].registerSpace;
        }
        desc.tables[desc.tableCount++] = tableDesc;
    }
    return desc;
}

[[nodiscard]] inline uint64_t HashPipelineBindingSignatureDesc(const PipelineBindingSignatureDesc& desc) noexcept
{
    uint64_t hash = HashPipelineBindingLayoutDesc(desc.bindingLayout);
    hash = HashBindingModelValue(hash, desc.parameterCount);
    hash = HashBindingModelValue(hash, desc.tableCount);
    hash = HashBindingModelValue(hash, desc.staticSamplerCount);
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.staticSamplerPolicy));
    for (uint32_t i = 0u; i < desc.parameterCount; ++i)
    {
        const auto& param = desc.parameters[i];
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(param.kind));
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(param.heap));
        hash = HashBindingModelValue(hash, param.tableIndex);
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(ToStageMaskBits(param.visibility)));
        hash = HashBindingModelValue(hash, param.bindPerDraw ? 1ull : 0ull);
    }
    for (uint32_t i = 0u; i < desc.tableCount; ++i)
    {
        const auto& table = desc.tables[i];
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(table.heap));
        hash = HashBindingModelValue(hash, table.tableIndex);
        hash = HashBindingModelValue(hash, table.descriptorCount);
        hash = HashBindingModelValue(hash, table.registerSpace);
    }
    for (uint32_t i = 0u; i < desc.staticSamplerCount; ++i)
    {
        const auto& sampler = desc.staticSamplers[i];
        hash = HashBindingModelValue(hash, sampler.logicalSlot);
        hash = HashBindingModelValue(hash, sampler.registerBase);
        hash = HashBindingModelValue(hash, sampler.registerSpace);
        hash = HashBindingModelValue(hash, static_cast<uint64_t>(ToStageMaskBits(sampler.visibility)));
        hash = HashBindingModelValue(hash, sampler.comparisonSampler ? 1ull : 0ull);
    }
    return hash;
}

[[nodiscard]] inline uint64_t BuildPipelineBindingSignatureKey(const DescriptorRuntimeLayoutDesc& runtimeLayout,
                                                    uint64_t shaderInterfaceLayoutHash,
                                                    StaticSamplerPolicy staticSamplerPolicy = StaticSamplerPolicy::None) noexcept
{
    const PipelineBindingSignatureDesc signature = DerivePipelineBindingSignatureDesc(runtimeLayout, staticSamplerPolicy);
    uint64_t key = HashPipelineBindingSignatureDesc(signature);
    key = HashBindingModelValue(key, shaderInterfaceLayoutHash);
    key = HashBindingModelValue(key, HashDescriptorRuntimeLayoutDesc(runtimeLayout));
    return key;
}


[[nodiscard]] inline RootSignatureLayoutDesc DeriveRootSignatureLayoutDesc(const DescriptorRuntimeLayoutDesc& runtimeLayout,
                                                                          StaticSamplerPolicy staticSamplerPolicy = StaticSamplerPolicy::None) noexcept
{
    return DerivePipelineBindingSignatureDesc(runtimeLayout, staticSamplerPolicy);
}

[[nodiscard]] inline uint64_t HashRootSignatureLayoutDesc(const RootSignatureLayoutDesc& desc) noexcept
{
    return HashPipelineBindingSignatureDesc(desc);
}

[[nodiscard]] inline uint64_t BuildRootSignatureKey(const DescriptorRuntimeLayoutDesc& runtimeLayout,
                                                    uint64_t shaderInterfaceLayoutHash,
                                                    StaticSamplerPolicy staticSamplerPolicy = StaticSamplerPolicy::None) noexcept
{
    return BuildPipelineBindingSignatureKey(runtimeLayout, shaderInterfaceLayoutHash, staticSamplerPolicy);
}


[[nodiscard]] inline uint64_t HashComputeRuntimeDesc(const ComputeRuntimeDesc& desc) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.maturity));
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.recordingQueue));
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.queueRouting));
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.synchronization));
    hash = HashBindingModelValue(hash, static_cast<uint64_t>(desc.uavBarrierPolicy));
    hash = HashBindingModelValue(hash, desc.computePipelinesSupported ? 1ull : 0ull);
    hash = HashBindingModelValue(hash, desc.computeDispatchSupported ? 1ull : 0ull);
    hash = HashBindingModelValue(hash, desc.dedicatedComputeQueueEnabled ? 1ull : 0ull);
    hash = HashBindingModelValue(hash, desc.crossQueueSynchronizationEnabled ? 1ull : 0ull);
    return hash;
}
[[nodiscard]] inline PipelineBindingLayoutDesc BuildEnginePipelineBindingLayout() noexcept
{
    PipelineBindingLayoutDesc desc{};
    desc.rangeCount = 4u;
    desc.ranges[0] = BindingRangeDesc{ DescriptorType::ConstantBuffer, 0u, BindingRegisterRanges::CB(CBSlots::PerFrame), CBSlots::COUNT, BindingRegisterRanges::RegisterSpace, ShaderStageMask::Vertex | ShaderStageMask::Fragment, true };
    desc.ranges[1] = BindingRangeDesc{ DescriptorType::ShaderResource, 0u, BindingRegisterRanges::SRV(0u), TexSlots::COUNT, BindingRegisterRanges::RegisterSpace, ShaderStageMask::Fragment, false };
    desc.ranges[2] = BindingRangeDesc{ DescriptorType::Sampler, 0u, BindingRegisterRanges::SMP(0u), SamplerSlots::COUNT, BindingRegisterRanges::RegisterSpace, ShaderStageMask::Fragment, false };
    desc.ranges[3] = BindingRangeDesc{ DescriptorType::UnorderedAccess, 0u, BindingRegisterRanges::UAV(0u), UAVSlots::COUNT, BindingRegisterRanges::RegisterSpace, ShaderStageMask::Compute, false };
    desc.tableCount = 2u;
    desc.tables[0] = BindingTableDesc{ BindingHeapKind::Resource, 0u, 2u };
    desc.tables[1] = BindingTableDesc{ BindingHeapKind::Sampler, 2u, 1u };
    return desc;
}

[[nodiscard]] inline FrameDescriptorArenaDesc BuildEngineFrameDescriptorArenaDesc(uint32_t maxSetsPerFrame = 256u) noexcept
{
    const PipelineBindingLayoutDesc layout = BuildEnginePipelineBindingLayout();
    FrameDescriptorArenaDesc desc{};
    desc.maxSetsPerFrame = maxSetsPerFrame;
    desc.resourceDescriptorCount = maxSetsPerFrame * (
        layout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ConstantBuffer) +
        layout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ShaderResource) +
        layout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::UnorderedAccess));
    desc.samplerDescriptorCount = maxSetsPerFrame *
        layout.CountDescriptors(BindingHeapKind::Sampler, DescriptorType::Sampler);
    return desc;
}


[[nodiscard]] inline DescriptorRuntimeLayoutDesc BuildEngineDescriptorRuntimeLayout(uint32_t maxSetsPerFrame = 256u) noexcept
{
    DescriptorRuntimeLayoutDesc desc{};
    desc.bindingLayout = BuildEnginePipelineBindingLayout();
    desc.frameArena = BuildEngineFrameDescriptorArenaDesc(maxSetsPerFrame);
    desc.frameArena.arenaModel = DescriptorArenaModel::LinearPerFrame;
    desc.frameArena.retirementModel = DescriptorRetirementModel::PerFrameSlotFence;
    desc.heapCount = 2u;
    desc.heaps[0] = BindingHeapRuntimeDesc{
        BindingHeapKind::Resource,
        DescriptorVisibilityModel::EmulatedShaderVisible,
        DescriptorUpdatePolicy::StageAndCopyPerFrame,
        DescriptorStorageClass::PersistentDevice,
        DescriptorStorageClass::TransientPerFrame,
        DescriptorArenaModel::LinearPerFrame,
        DescriptorRetirementModel::PerFrameSlotFence,
        SamplerMaterializationPolicy::InlineWithDescriptorTable,
        CountHeapDescriptorsPerSet(desc, BindingHeapKind::Resource),
        desc.frameArena.resourceDescriptorCount,
        true,
        true
    };
    desc.heaps[1] = BindingHeapRuntimeDesc{
        BindingHeapKind::Sampler,
        DescriptorVisibilityModel::EmulatedShaderVisible,
        DescriptorUpdatePolicy::StageAndCopyPerFrame,
        DescriptorStorageClass::PersistentDevice,
        DescriptorStorageClass::TransientPerFrame,
        DescriptorArenaModel::LinearPerFrame,
        DescriptorRetirementModel::PerFrameSlotFence,
        SamplerMaterializationPolicy::DedicatedHeap,
        CountHeapDescriptorsPerSet(desc, BindingHeapKind::Sampler),
        desc.frameArena.samplerDescriptorCount,
        true,
        true
    };
    desc.rootParameterCount = 2u;
    desc.rootParameters[0] = RootParameterDesc{ RootParameterKind::DescriptorTable, BindingHeapKind::Resource, 0u, ShaderStageMask::Vertex | ShaderStageMask::Fragment | ShaderStageMask::Compute, true };
    desc.rootParameters[1] = RootParameterDesc{ RootParameterKind::DescriptorTable, BindingHeapKind::Sampler, 1u, ShaderStageMask::Fragment | ShaderStageMask::Compute, true };
    return desc;
}

} // namespace engine::renderer