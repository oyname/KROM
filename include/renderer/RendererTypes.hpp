#pragma once
// =============================================================================
// KROM Engine - renderer/RendererTypes.hpp
// Alle API-neutralen Render-Datenmodelle.
// Kein DX11/DX12/GL/Vulkan-Include. Orientiert an DX12/Vulkan-Mentalmodell.
// =============================================================================
#include "core/Types.hpp"
#include "core/Math.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace engine::renderer {

// =============================================================================
// Format - Textur- und Vertex-Datenformate
// =============================================================================
enum class Format : uint32_t
{
    Unknown = 0,

    // 8-Bit
    R8_UNORM, R8_SNORM, R8_UINT, R8_SINT,
    RG8_UNORM, RGBA8_UNORM, RGBA8_SNORM,
    RGBA8_UNORM_SRGB, BGRA8_UNORM, BGRA8_UNORM_SRGB,

    // 16-Bit
    R16_FLOAT, RG16_FLOAT, RGBA16_FLOAT,
    R16_UINT, RG16_UINT, RGBA16_UINT,
    R16_SNORM, RG16_SNORM, RGBA16_SNORM,

    // 32-Bit
    R32_FLOAT, RG32_FLOAT, RGB32_FLOAT, RGBA32_FLOAT,
    R32_UINT,  RG32_UINT,  RGB32_UINT,  RGBA32_UINT,
    R32_SINT,

    // Packed
    R11G11B10_FLOAT, RGB10A2_UNORM,

    // Block-Komprimierung
    BC1_UNORM, BC1_UNORM_SRGB,
    BC2_UNORM, BC3_UNORM, BC3_UNORM_SRGB,
    BC4_UNORM, BC5_UNORM, BC5_SNORM, BC7_UNORM, BC7_UNORM_SRGB,

    // Depth/Stencil
    D16_UNORM, D24_UNORM_S8_UINT, D32_FLOAT, D32_FLOAT_S8X24_UINT,
};

// =============================================================================
// ResourceState - für Barrieren / Transitions
// =============================================================================
enum class ResourceState : uint32_t
{
    Unknown        = 0,
    Common         = 1 << 0,
    VertexBuffer   = 1 << 1,
    IndexBuffer    = 1 << 2,
    ConstantBuffer = 1 << 3,
    RenderTarget   = 1 << 4,
    UnorderedAccess= 1 << 5,
    DepthWrite     = 1 << 6,
    DepthRead      = 1 << 7,
    ShaderRead     = 1 << 8,  // SRV
    CopySource     = 1 << 9,
    CopyDest       = 1 << 10,
    Present        = 1 << 11,
    IndirectArg    = 1 << 12,
};

// =============================================================================
// Expliziter Resource-State-Vertrag
//
// currentState:
//   Der letzte physisch bekannte GPU-Zustand der konkreten Ressource.
// authoritativeOwner:
//   Die Instanz, die diesen physischen Zustand verbindlich kennt.
//   Autoritativ dürfen nur Backend-Ressource oder externe Runtime-Quellen
//   (z.B. Swapchain) sein. RenderGraph plant auf diesem Zustand, besitzt ihn
//   aber nicht. Descriptor-/Binding-Schichten dürfen ihn nur referenzieren,
//   niemals separat spiegeln oder unabhängig fortschreiben.
// lastSubmissionFenceValue:
//   Fence-Wert der letzten Queue-Submission, in der dieser Zustand benutzt oder
//   hergestellt wurde. Dient der Lifetime-/Retirement-Entkopplung.
// =============================================================================
enum class ResourceStateAuthority : uint8_t
{
    Unknown = 0,
    RenderGraph,
    GpuRuntime,
    BackendResource,
    ExternalSwapchain,
};

struct ResourceStateRecord
{
    ResourceState          currentState = ResourceState::Unknown;
    ResourceStateAuthority authoritativeOwner = ResourceStateAuthority::Unknown;
    uint64_t               lastSubmissionFenceValue = 0u;
};

// =============================================================================
// ResourceUsage - wie ein Buffer/Texture verwendet wird (Bitmaske)
// =============================================================================
enum class ResourceUsage : uint32_t
{
    None            = 0,
    VertexBuffer    = 1 << 0,
    IndexBuffer     = 1 << 1,
    ConstantBuffer  = 1 << 2,
    ShaderResource  = 1 << 3,
    UnorderedAccess = 1 << 4,
    RenderTarget    = 1 << 5,
    DepthStencil    = 1 << 6,
    CopySource      = 1 << 7,
    CopyDest        = 1 << 8,
};
inline ResourceUsage operator|(ResourceUsage a, ResourceUsage b)
{ return static_cast<ResourceUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
inline ResourceUsage operator&(ResourceUsage a, ResourceUsage b) noexcept
{ return static_cast<ResourceUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); }
inline bool HasFlag(ResourceUsage flags, ResourceUsage bit) noexcept
{ return (static_cast<uint32_t>(flags & bit)) != 0u; }

// =============================================================================
// Buffer
// =============================================================================
enum class BufferType : uint8_t { Vertex, Index, Constant, Structured, Raw };
enum class MemoryAccess : uint8_t { GpuOnly, CpuWrite, CpuRead };
enum class MemoryHeapKind : uint8_t { Unknown, Default, Upload, Readback };
enum class ResourceLifetimeClass : uint8_t { Persistent, PerFrameTransient, External };

struct ResourceAllocationInfo
{
    MemoryHeapKind        heapKind = MemoryHeapKind::Unknown;
    ResourceLifetimeClass lifetime = ResourceLifetimeClass::Persistent;
    ResourceState         initialState = ResourceState::Unknown;
    bool                  cpuVisible = false;
    bool                  prefersExplicitCopy = false;
};

struct BufferDesc
{
    uint64_t      byteSize    = 0u;
    uint32_t      stride      = 0u;  // für Structured Buffers
    BufferType    type        = BufferType::Vertex;
    ResourceUsage usage       = ResourceUsage::VertexBuffer;
    MemoryAccess  access      = MemoryAccess::GpuOnly;
    ResourceState initialState = ResourceState::Unknown;
    ResourceLifetimeClass lifetime = ResourceLifetimeClass::Persistent;
    std::string   debugName;
};

// =============================================================================
// BufferBinding - backendneutrale Range-Binding-Beschreibung
// Wird für Constant/Uniform-Buffer-Bindings mit Suballokation verwendet.
// offset und size in Bytes; beide müssen kConstantBufferAlignment-konform sein.
// =============================================================================
static constexpr uint32_t kConstantBufferAlignment = 256u;

struct BufferBinding
{
    BufferHandle buffer;
    uint32_t     offset = 0u;
    uint32_t     size   = 0u;

    [[nodiscard]] bool IsValid() const noexcept { return buffer.IsValid() && size > 0u; }
};

// =============================================================================
// Texture
// =============================================================================
enum class TextureDimension : uint8_t { Tex1D, Tex2D, Tex3D, Cubemap, Tex2DArray };

struct TextureDesc
{
    uint32_t          width      = 1u;
    uint32_t          height     = 1u;
    uint32_t          depth      = 1u;
    uint32_t          mipLevels  = 1u;
    uint32_t          arraySize  = 1u;
    uint32_t          sampleCount = 1u;
    Format            format     = Format::RGBA8_UNORM;
    TextureDimension  dimension  = TextureDimension::Tex2D;
    ResourceUsage     usage      = ResourceUsage::ShaderResource;
    ResourceState     initialState = ResourceState::Common;
    ResourceLifetimeClass lifetime = ResourceLifetimeClass::Persistent;
    std::string       debugName;
};

// =============================================================================
// RenderTarget
// =============================================================================
struct ClearValue
{
    enum class Type : uint8_t { Color, Depth };
    Type type = Type::Color;
    float color[4] = { 0.f, 0.f, 0.f, 1.f };
    float depth    = 1.f;
    uint8_t stencil = 0u;
};

struct RenderTargetDesc
{
    uint32_t    width       = 0u;
    uint32_t    height      = 0u;
    Format      colorFormat = Format::RGBA8_UNORM_SRGB;
    Format      depthFormat = Format::D24_UNORM_S8_UINT;
    uint32_t    sampleCount = 1u;
    bool        hasDepth    = true;
    bool        hasColor    = true;
    ClearValue  colorClear;
    ClearValue  depthClear;
    std::string debugName;
};

// =============================================================================
// Sampler
// =============================================================================
enum class FilterMode  : uint8_t { Nearest, Linear, Anisotropic };
enum class WrapMode    : uint8_t { Repeat, Clamp, Mirror, Border };
enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

struct SamplerDesc
{
    FilterMode  minFilter  = FilterMode::Linear;
    FilterMode  magFilter  = FilterMode::Linear;
    FilterMode  mipFilter  = FilterMode::Linear;
    WrapMode    addressU   = WrapMode::Repeat;
    WrapMode    addressV   = WrapMode::Repeat;
    WrapMode    addressW   = WrapMode::Repeat;
    float       mipLodBias = 0.f;
    uint32_t    maxAniso   = 1u;
    CompareFunc compareFunc = CompareFunc::Never;
    float       minLod     = 0.f;
    float       maxLod     = 1000.f;
    float       borderColor[4] = { 0.f, 0.f, 0.f, 0.f };
};

// =============================================================================
// Pipeline State
// =============================================================================
enum class FillMode    : uint8_t { Solid, Wireframe };
enum class CullMode    : uint8_t { None, Front, Back };
enum class WindingOrder : uint8_t { CW, CCW };
enum class BlendFactor : uint8_t { Zero, One, SrcColor, InvSrcColor, DstColor, InvDstColor, SrcAlpha, InvSrcAlpha, DstAlpha, InvDstAlpha };
enum class BlendOp     : uint8_t { Add, Subtract, RevSubtract, Min, Max };
enum class DepthFunc   : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

struct RasterizerState
{
    FillMode    fillMode        = FillMode::Solid;
    CullMode    cullMode        = CullMode::Back;
    WindingOrder frontFace      = WindingOrder::CCW;
    bool        depthClip       = true;
    bool        scissorTest     = false;
    float       depthBias       = 0.f;
    float       depthBiasClamp  = 0.f;
    float       slopeScaledBias = 0.f;
};

struct BlendState
{
    bool        blendEnable    = false;
    BlendFactor srcBlend       = BlendFactor::One;
    BlendFactor dstBlend       = BlendFactor::Zero;
    BlendOp     blendOp        = BlendOp::Add;
    BlendFactor srcBlendAlpha  = BlendFactor::One;
    BlendFactor dstBlendAlpha  = BlendFactor::Zero;
    BlendOp     blendOpAlpha   = BlendOp::Add;
    uint8_t     writeMask      = 0xF;
};

struct DepthStencilState
{
    bool        depthEnable   = true;
    bool        depthWrite    = true;
    DepthFunc   depthFunc     = DepthFunc::Less;
    bool        stencilEnable = false;
};

// =============================================================================
// Vertex Layout
// =============================================================================
enum class VertexSemantic : uint8_t
{
    Position = 0, Normal, Tangent, Bitangent,
    TexCoord0, TexCoord1, Color0, BoneWeight, BoneIndex,
};

enum class VertexInputRate : uint8_t { PerVertex, PerInstance };

struct VertexAttribute
{
    VertexSemantic semantic;
    Format         format;
    uint32_t       binding;    // Vertex-Buffer-Slot
    uint32_t       offset;     // Byte-Offset im Vertex
};

struct VertexBinding
{
    uint32_t       binding;
    uint32_t       stride;
    VertexInputRate inputRate = VertexInputRate::PerVertex;
};

struct VertexLayout
{
    std::vector<VertexAttribute> attributes;
    std::vector<VertexBinding>   bindings;
};

// =============================================================================
// ShaderStage + PipelineDesc
// =============================================================================
enum class ShaderStageMask : uint8_t
{
    None     = 0,
    Vertex   = 1 << 0,
    Fragment = 1 << 1,
    Compute  = 1 << 2,
    Geometry = 1 << 3,
    Hull     = 1 << 4,
    Domain   = 1 << 5,
};
inline ShaderStageMask operator|(ShaderStageMask a, ShaderStageMask b)
{ return static_cast<ShaderStageMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
inline ShaderStageMask operator&(ShaderStageMask a, ShaderStageMask b)
{ return static_cast<ShaderStageMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b)); }
inline bool HasFlag(ShaderStageMask flags, ShaderStageMask bit) noexcept
{ return static_cast<uint8_t>(flags & bit) != 0u; }

struct ShaderStageDesc
{
    ShaderHandle    handle;
    ShaderStageMask stage  = ShaderStageMask::Vertex;
    std::string     entry  = "main";
};

enum class PrimitiveTopology : uint8_t
{
    TriangleList, TriangleStrip, LineList, LineStrip, PointList,
};

enum class PipelineClass : uint8_t
{
    Graphics = 0,
    Compute = 1,
};

struct PipelineDesc
{
    std::vector<ShaderStageDesc> shaderStages;
    VertexLayout                 vertexLayout;
    RasterizerState              rasterizer;
    std::array<BlendState, 8>    blendStates;
    DepthStencilState            depthStencil;
    PrimitiveTopology            topology    = PrimitiveTopology::TriangleList;
    Format                       colorFormat = Format::RGBA8_UNORM_SRGB;
    Format                       depthFormat = Format::D24_UNORM_S8_UINT;
    uint32_t                     sampleCount = 1u;
    std::string                  debugName;
    PipelineClass                pipelineClass = PipelineClass::Graphics;
    uint64_t                     shaderContractHash = 0ull;
    uint64_t                     pipelineLayoutHash = 0ull;
};

// =============================================================================
// Descriptor / Bindings (API-neutral - DX12/Vulkan Mentalmodell)
// =============================================================================
enum class DescriptorType : uint8_t
{
    ConstantBuffer, ShaderResource, UnorderedAccess, Sampler
};

struct DescriptorBinding
{
    uint32_t       slot;
    uint32_t       space = 0u;
    DescriptorType type;
    ShaderStageMask stages;
};

struct DescriptorSetDesc
{
    std::vector<DescriptorBinding> bindings;
    std::string                    debugName;
};

// =============================================================================
// Queue-Typen
// =============================================================================
enum class QueueType : uint8_t { Graphics, Compute, Transfer };

struct QueueCapabilities
{
    QueueType type = QueueType::Graphics;
    bool supported = false;
    bool dedicated = false;
    bool canPresent = false;
};


enum class QueueSyncPrimitive : uint8_t
{
    None = 0,
    Fence,
    Semaphore,
};

enum class QueueDependencyScope : uint8_t
{
    None = 0,
    Submission,
    ResourceOwnership,
    Present,
};

enum class QueueOwnershipTransferPoint : uint8_t
{
    None = 0,
    BeforeSubmit,
    AfterSubmit,
    BeforePresent,
};

struct InterQueueDependencyDesc
{
    uint32_t producerSubmissionId = UINT32_MAX;
    QueueType producerQueue = QueueType::Graphics;
    uint32_t consumerSubmissionId = UINT32_MAX;
    QueueType consumerQueue = QueueType::Graphics;
    QueueSyncPrimitive primitive = QueueSyncPrimitive::None;
    QueueDependencyScope scope = QueueDependencyScope::None;
    QueueOwnershipTransferPoint ownershipTransferPoint = QueueOwnershipTransferPoint::None;
    bool requiresQueueWait = false;
    bool requiresOwnershipTransfer = false;
};

struct CommandSubmissionDesc
{
    QueueType queue = QueueType::Graphics;
    bool waitForSwapchainAcquire = false;
    bool signalSwapchainReady = false;
    uint32_t submissionId = UINT32_MAX;
    uint32_t frameSlot = UINT32_MAX;
    bool requiresQueueOwnership = false;
    bool exportsQueueSignal = false;
    std::vector<uint32_t> waitSubmissionIds;
};

struct QueueSyncRuntimeDesc
{
    bool graphicsFirstV1 = true;
    bool queueLocalFenceSignalSupported = true;
    bool queueLocalFenceWaitSupported = true;
    bool interQueueDependenciesPrepared = true;
    bool interQueueSemaphoreSignalSupported = false;
    bool interQueueSemaphoreWaitSupported = false;
    bool ownershipTransfersPrepared = true;
    bool ownershipTransfersMaterialized = false;
    bool graphToSubmissionMappingPrepared = true;
    bool graphToSubmissionMappingMaterialized = false;
};

// =============================================================================
// Compute Runtime (Graphics-first v1, Compute später ohne Architekturbruch)
// =============================================================================
enum class ComputePathMaturity : uint8_t
{
    Disabled = 0,
    Prepared,
    Enabled,
};

enum class ComputeQueueRoutingPolicy : uint8_t
{
    GraphicsQueueOnly = 0,
    PreferDedicatedCompute,
};

enum class ComputeSynchronizationModel : uint8_t
{
    GraphicsQueueSerial = 0,
    CrossQueueFenceAndBarrier,
};

enum class ComputeUavBarrierPolicy : uint8_t
{
    ExplicitPerDispatch = 0,
    ExplicitPerPass,
};

struct ComputeRuntimeDesc
{
    ComputePathMaturity maturity = ComputePathMaturity::Prepared;
    QueueType recordingQueue = QueueType::Graphics;
    ComputeQueueRoutingPolicy queueRouting = ComputeQueueRoutingPolicy::GraphicsQueueOnly;
    ComputeSynchronizationModel synchronization = ComputeSynchronizationModel::GraphicsQueueSerial;
    ComputeUavBarrierPolicy uavBarrierPolicy = ComputeUavBarrierPolicy::ExplicitPerDispatch;
    bool computePipelinesSupported = false;
    bool computeDispatchSupported = false;
    bool dedicatedComputeQueueEnabled = false;
    bool crossQueueSynchronizationEnabled = false;
};

// =============================================================================
// Swapchain Runtime (API-neutraler Frame-Ressourcenvertrag)
// =============================================================================
enum class SwapchainFramePhase : uint8_t
{
    Uninitialized = 0,
    Idle,
    Acquired,
    Submitted,
    Presented,
    ResizePending,
};

struct SwapchainRuntimeDesc
{
    QueueType presentQueue = QueueType::Graphics;
    bool explicitAcquire = false;
    bool explicitPresentTransition = true;
    bool tracksPerBufferOwnership = true;
    bool resizeRequiresRecreate = false;
    bool destructionRequiresFenceRetirement = false;
};

struct SwapchainFrameStatus
{
    SwapchainFramePhase phase = SwapchainFramePhase::Uninitialized;
    uint32_t currentBackbufferIndex = 0u;
    uint32_t bufferCount = 0u;
    bool hasRenderableBackbuffer = false;
    bool resizePending = false;
    uint64_t lastSubmissionFenceValue = 0u;
};

// =============================================================================
// RenderPassTag - welchen Pass ein Material/Draw-Call bedient.
// Hier in RendererTypes definiert damit Backends (z.B. DX11Device) ohne
// MaterialSystem.hpp auskommen. MaterialSystem.hpp bekommt es über RendererTypes.
// =============================================================================
enum class RenderPassTag : uint8_t
{
    Opaque       = 0,
    AlphaCutout  = 1,
    Transparent  = 2,
    Shadow       = 3,
    UI           = 4,
    Particle     = 5,
    Postprocess  = 6,
    COUNT        = 7,
};

inline const char* PassTagName(RenderPassTag t) noexcept
{
    switch (t) {
    case RenderPassTag::Opaque:      return "Opaque";
    case RenderPassTag::AlphaCutout: return "AlphaCutout";
    case RenderPassTag::Transparent: return "Transparent";
    case RenderPassTag::Shadow:      return "Shadow";
    case RenderPassTag::UI:          return "UI";
    case RenderPassTag::Particle:    return "Particle";
    case RenderPassTag::Postprocess: return "Postprocess";
    default:                         return "Unknown";
    }
}

// =============================================================================
// PipelineKey - Hash aller Pipeline-Zustände, Cache-Schlüssel für Backends.
// Hier in RendererTypes definiert (nicht in MaterialSystem.hpp) damit Backends
// ohne das vollständige Materialsystem auskommen.
// =============================================================================
struct PipelineKey
{
    uint32_t vertexShader     = 0u;
    uint32_t fragmentShader   = 0u;
    uint32_t computeShader    = 0u;

    uint8_t  fillMode         = 0u;
    uint8_t  cullMode         = 0u;
    uint8_t  frontFace        = 0u;
    uint8_t  _rpad            = 0u;

    uint8_t  depthEnable      = 1u;
    uint8_t  depthWrite       = 1u;
    uint8_t  depthFunc        = 0u;
    uint8_t  stencilEnable    = 0u;

    uint8_t  blendEnable      = 0u;
    uint8_t  srcBlend         = 0u;
    uint8_t  dstBlend         = 0u;
    uint8_t  blendOp          = 0u;
    uint8_t  srcBlendAlpha    = 0u;
    uint8_t  dstBlendAlpha    = 0u;
    uint8_t  blendOpAlpha     = 0u;
    uint8_t  writeMask        = 0xFu;

    uint8_t  colorFormat      = 0u;
    uint8_t  depthFormat      = 0u;
    uint8_t  sampleCount      = 1u;
    uint8_t  topology         = 0u;

    uint32_t vertexLayoutHash = 0u;
    uint32_t shaderContractHash = 0u;
    uint32_t pipelineLayoutHash = 0u;
    uint8_t  pipelineClass    = 0u;
    uint8_t  _reserved0       = 0u;
    uint16_t _reserved1       = 0u;
    RenderPassTag passTag     = RenderPassTag::Opaque;

    [[nodiscard]] bool     operator==(const PipelineKey& o) const noexcept;
    [[nodiscard]] uint64_t Hash() const noexcept;

    static PipelineKey From(const PipelineDesc& desc, RenderPassTag pass) noexcept;
};

// =============================================================================
// ShaderVariantFlags - Bitfeld für aktive Shader-Defines.
// ShaderRuntime und ShaderCompiler arbeiten ausschließlich mit diesem neutralen
// Modell. Backends übersetzen die Bits in konkrete Präprozessor-Symbole.
// =============================================================================
enum class ShaderVariantFlag : uint32_t
{
    None             = 0u,
    Skinned          = 1u << 0,
    VertexColor      = 1u << 1,
    AlphaTest        = 1u << 2,
    NormalMap        = 1u << 3,
    Unlit            = 1u << 4,
    ShadowPass       = 1u << 5,
    Instanced        = 1u << 6,
    BaseColorMap     = 1u << 7,
    MetallicMap      = 1u << 8,
    RoughnessMap     = 1u << 9,
    OcclusionMap     = 1u << 10,
    EmissiveMap      = 1u << 11,
    OpacityMap       = 1u << 12,
    PBRMetalRough    = 1u << 13,
    DoubleSided      = 1u << 14,
    // Packed ORM (Occlusion/Roughness/Metallic) map at slot t2.
    // Replaces separate MetallicMap/RoughnessMap/OcclusionMap in the shader path.
    ORMMap           = 1u << 15,
    IBLMap           = 1u << 16,
};

static_assert(static_cast<uint32_t>(ShaderVariantFlag::IBLMap) == (1u << 16),
              "ShaderVariantFlag::IBLMap bit changed unexpectedly");

inline ShaderVariantFlag operator|(ShaderVariantFlag a, ShaderVariantFlag b) noexcept
{
    return static_cast<ShaderVariantFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ShaderVariantFlag operator&(ShaderVariantFlag a, ShaderVariantFlag b) noexcept
{
    return static_cast<ShaderVariantFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasFlag(ShaderVariantFlag flags, ShaderVariantFlag bit) noexcept
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0u;
}

enum class ShaderPassType : uint8_t
{
    Main   = 0,
    Shadow = 1,
    Depth  = 2,
    UI     = 3,
};

struct ShaderVariantKey
{
    ShaderHandle      baseShader;
    ShaderPassType    pass  = ShaderPassType::Main;
    ShaderVariantFlag flags = ShaderVariantFlag::None;

    [[nodiscard]] ShaderVariantKey Normalized() const noexcept;
    [[nodiscard]] uint64_t Hash() const noexcept;

    bool operator==(const ShaderVariantKey& o) const noexcept
    {
        return baseShader == o.baseShader && pass == o.pass && flags == o.flags;
    }
};

} // namespace engine::renderer

namespace std {
    template<> struct hash<engine::renderer::PipelineKey> {
        size_t operator()(const engine::renderer::PipelineKey& k) const noexcept {
            return static_cast<size_t>(k.Hash());
        }
    };
    template<> struct hash<engine::renderer::ShaderVariantKey> {
        size_t operator()(const engine::renderer::ShaderVariantKey& k) const noexcept {
            return static_cast<size_t>(k.Hash());
        }
    };
}
