#pragma once
#include "assets/AssetRegistry.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include "rendergraph/RenderGraph.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderWorld.hpp"
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <thread>

namespace engine::renderer {

class GpuResourceRuntime
{
public:
    struct PendingBufferUpload
    {
        BufferHandle stagingBuffer = BufferHandle::Invalid();
        BufferHandle destinationBuffer = BufferHandle::Invalid();
        uint64_t sourceOffset = 0u;
        uint64_t destinationOffset = 0u;
        uint64_t byteSize = 0u;
        ResourceState destinationStateBeforeCopy = ResourceState::CopyDest;
        ResourceState destinationStateAfterCopy = ResourceState::Common;
        std::string debugName;
    };

    // GPU-Repräsentation eines hochgeladenen Submesh
    struct GpuMeshEntry
    {
        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
        uint32_t     indexCount    = 0u;
        uint32_t     vertexStride  = 0u;  // Byte-Stride des tatsächlich hochgeladenen VB
        uint32_t     layoutHash    = 0u;  // Hash des VertexLayout, mit dem dieses Entry gebaut wurde
        bool         uploaded      = false;
    };

    struct MeshUploadRequest
    {
        MeshHandle mesh;
        uint32_t submeshIndex = 0u;

        bool operator==(const MeshUploadRequest& o) const noexcept
        {
            return mesh == o.mesh && submeshIndex == o.submeshIndex;
        }

        bool operator<(const MeshUploadRequest& o) const noexcept
        {
            if (mesh.value != o.mesh.value)
                return mesh.value < o.mesh.value;
            return submeshIndex < o.submeshIndex;
        }
    };

    struct Stats
    {
        uint32_t pooledTransientTargets = 0u;
        uint32_t liveFrameUploadBuffers = 0u;
        uint32_t liveMeshBuffers        = 0u;
        uint64_t uploadedBytesThisFrame = 0u;
        uint64_t uploadedBytesTotal     = 0u;
        uint64_t deviceLocalBytes       = 0u;
        uint64_t uploadHeapBytes        = 0u;
        uint64_t readbackHeapBytes      = 0u;
    };

    bool Initialize(IDevice& device, uint32_t framesInFlight = 3u);
    void Shutdown();

    void BeginFrame(uint64_t completedFenceValue, IFence* frameFence = nullptr);
    void EndFrame(uint64_t submittedFenceValue);

    void AllocateTransientTargets(rendergraph::RenderGraph& rg);
    void ReleaseTransientTargets(const rendergraph::CompiledFrame& frame,
                                 uint64_t retirementFenceValue);

    BufferHandle AllocateUploadBuffer(uint64_t byteSize,
                                      BufferType type = BufferType::Constant,
                                      const char* debugName = "FrameUpload");
    void UploadBuffer(BufferHandle dst, const void* data,
                      size_t byteSize, size_t dstOffset = 0u);
    void EnqueueBufferUpload(BufferHandle dst,
                             const void* data,
                             size_t byteSize,
                             size_t dstOffset,
                             ResourceState dstStateAfterCopy,
                             const char* debugName = "BufferUpload");

    // Alloziert einen frame-lokalen Constant-Buffer-Arena für elementCount Elemente.
    // Stride wird auf kConstantBufferAlignment aufgerundet (DX12/Vulkan-kompatibel).
    struct ConstantArenaResult
    {
        BufferHandle buffer;
        uint32_t     alignedStride = 0u;
    };
    [[nodiscard]] ConstantArenaResult AllocateConstantArena(uint32_t elementSize,
                                                            uint32_t elementCount,
                                                            const char* debugName = "ConstantArena");

    [[nodiscard]] bool CollectUploadRequests(const RenderWorld& renderWorld,
                                             std::vector<MeshUploadRequest>& outRequests) const;

    // Vorwärmt eine Batch von Mesh-Uploads mit leerem (kanonischem) Layout.
    // Für layout-spezifische Uploads: GetOrUploadMesh(mesh, sub, layout, registry) verwenden.
    [[nodiscard]] bool CommitUploads(const std::vector<MeshUploadRequest>& requests,
                                     assets::AssetRegistry& registry);

    // Gibt GPU-Buffer-Handles für ein Submesh zurück.
    // layout beschreibt den gewünschten Vertex-Vertrag; der VB wird exakt dazu passend gebaut.
    // Verschiedene Layouts für dasselbe Mesh erzeugen separate Cache-Einträge.
    // Gibt nullptr zurück wenn MeshHandle ungültig oder Submesh nicht vorhanden.
    [[nodiscard]] const GpuMeshEntry* GetOrUploadMesh(
        MeshHandle mesh, uint32_t submeshIndex,
        const VertexLayout& layout,
        assets::AssetRegistry& registry);

    // Rückwärtskompatible Überladung: verwendet kanonisches Layout (Position + Normal + UV).
    [[nodiscard]] const GpuMeshEntry* GetOrUploadMesh(
        MeshHandle mesh, uint32_t submeshIndex,
        assets::AssetRegistry& registry);

    // Gibt true zurück wenn das Mesh bereits mit dem angegebenen Layout hochgeladen ist
    [[nodiscard]] bool IsMeshUploaded(MeshHandle mesh, uint32_t submeshIndex,
                                      uint32_t layoutHash = 0u) const noexcept;

    void ScheduleDestroy(BufferHandle handle, uint64_t retirementFenceValue);
    void ScheduleDestroy(TextureHandle handle, uint64_t retirementFenceValue);
    void ScheduleDestroy(RenderTargetHandle handle, uint64_t retirementFenceValue);
    void ScheduleDestroy(ShaderHandle handle, uint64_t retirementFenceValue);
    void ScheduleDestroy(PipelineHandle handle, uint64_t retirementFenceValue);

    [[nodiscard]] uint64_t GetCompletedFenceValue() const noexcept { return m_completedFenceValue; }
    [[nodiscard]] uint64_t GetSubmittedFenceValue() const noexcept { return m_submittedFenceValue; }
    [[nodiscard]] uint64_t GetRetirementFenceForCurrentFrame() const noexcept { return m_submittedFenceValue; }

    [[nodiscard]] uint32_t     GetFramesInFlight() const noexcept { return m_framesInFlight; }
    [[nodiscard]] const Stats& GetStats()          const noexcept { return m_stats; }
    [[nodiscard]] bool         IsRenderThread()    const noexcept;
    [[nodiscard]] bool         HasPendingUploads() const noexcept;
    [[nodiscard]] const std::vector<PendingBufferUpload>& GetPendingBufferUploads() const noexcept;
    void ClearPendingUploads() noexcept;

    // Berechnet einen stabilen Hash für ein VertexLayout.
    // Identische Layouts (gleiche Attribute + Bindings) ergeben denselben Hash.
    static uint32_t ComputeVertexLayoutHash(const VertexLayout& layout) noexcept;

private:
    // Mesh-Cache-Schlüssel: MeshHandle + Submesh-Index + VertexLayout-Hash
    // Verschiedene Layouts für dasselbe Mesh erzeugen separate Einträge.
    struct MeshCacheKey
    {
        uint32_t meshHandleValue = 0u;
        uint32_t submeshIndex    = 0u;
        uint32_t layoutHash      = 0u;  // 0 = kanonisches Layout (Position + Normal + UV)

        bool operator==(const MeshCacheKey& o) const noexcept
        {
            return meshHandleValue == o.meshHandleValue
                && submeshIndex    == o.submeshIndex
                && layoutHash      == o.layoutHash;
        }
    };

    struct MeshCacheKeyHash {
        size_t operator()(const MeshCacheKey& k) const noexcept {
            // FNV-1a-artiger Mix für drei 32-Bit-Felder
            uint64_t h = (static_cast<uint64_t>(k.meshHandleValue) << 32)
                       | static_cast<uint64_t>(k.submeshIndex);
            h ^= static_cast<uint64_t>(k.layoutHash) * 2654435761ull;
            return std::hash<uint64_t>{}(h);
        }
    };

    struct PooledTransientRT
    {
        RenderTargetDesc   desc;
        RenderTargetHandle renderTarget  = RenderTargetHandle::Invalid();
        TextureHandle      colorTexture  = TextureHandle::Invalid();
        uint64_t           availableAfterFence = 0u;
        bool               inUse         = false;
    };

    struct FrameUploadBuffer
    {
        BufferHandle handle;
        uint64_t     byteSize = 0u;
    };

    struct FrameSlot
    {
        uint64_t                         fenceValue = 0u;
        std::vector<FrameUploadBuffer>   uploadBuffers;
        std::vector<PendingBufferUpload> pendingBufferUploads;
    };

    struct PendingDestroy
    {
        enum class Type : uint8_t { Buffer, Texture, RenderTarget, Shader, Pipeline } type = Type::Buffer;
        BufferHandle       buffer       = BufferHandle::Invalid();
        TextureHandle      texture      = TextureHandle::Invalid();
        RenderTargetHandle renderTarget = RenderTargetHandle::Invalid();
        ShaderHandle       shader       = ShaderHandle::Invalid();
        PipelineHandle     pipeline     = PipelineHandle::Invalid();
        uint64_t           retireAfterFence = 0u;
    };

    [[nodiscard]] bool Matches(const RenderTargetDesc& a,
                                const RenderTargetDesc& b) const noexcept;
    void RetireCompleted(uint64_t completedFenceValue);
    void DestroyNow(const PendingDestroy& pending);
    void WaitForCompletedValue(uint64_t fenceValue);
    [[nodiscard]] uint64_t GetMaxOutstandingFenceValue() const noexcept;
    [[nodiscard]] bool RequireRenderThread(const char* opName) const noexcept;
    void RefreshCompletedFenceValue() noexcept;

    void TrackAllocation(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept;
    void TrackRelease(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept;

    // Interne Impl, die von beiden GetOrUploadMesh-Überladungen verwendet wird.
    [[nodiscard]] const GpuMeshEntry* GetOrUploadMeshImpl(
        MeshHandle mesh, uint32_t submeshIndex,
        const VertexLayout& layout, uint32_t layoutHash,
        assets::AssetRegistry& registry);

    IDevice*  m_device              = nullptr;
    uint32_t  m_framesInFlight      = 0u;
    uint32_t  m_currentFrameSlot    = 0u;
    uint64_t  m_completedFenceValue = 0u;
    uint64_t  m_submittedFenceValue = 0u;
    IFence*   m_frameFence          = nullptr;
    std::thread::id m_renderThreadId{};

    std::vector<FrameSlot>           m_frameSlots;
    std::vector<PooledTransientRT>   m_transientRTPool;
    std::vector<PendingDestroy>      m_pendingDestroy;

    // Persistenter Mesh-GPU-Cache (bleibt bis Shutdown).
    // Key enthält layoutHash, sodass verschiedene Layouts separate VBs erhalten.
    std::unordered_map<MeshCacheKey, GpuMeshEntry, MeshCacheKeyHash> m_meshCache;

    Stats m_stats{};
};

} // namespace engine::renderer
