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
        uint32_t     indexCount   = 0u;
        uint32_t     vertexStride = 0u;  // Byte-Stride für den Vertex-Buffer
        bool         uploaded     = false;
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
    // Für DX11 gilt: SetConstantBufferRange fällt bei Bedarf intern auf den ganzen Buffer zurück.
    struct ConstantArenaResult
    {
        BufferHandle buffer;
        uint32_t     alignedStride = 0u; // Bytes pro Slot inkl. Padding
    };
    [[nodiscard]] ConstantArenaResult AllocateConstantArena(uint32_t elementSize,
                                                            uint32_t elementCount,
                                                            const char* debugName = "ConstantArena");

    [[nodiscard]] bool CollectUploadRequests(const RenderWorld& renderWorld,
                                             std::vector<MeshUploadRequest>& outRequests) const;

    [[nodiscard]] bool CommitUploads(const std::vector<MeshUploadRequest>& requests,
                                     assets::AssetRegistry& registry);

    // Gibt GPU-Buffer-Handles für ein Submesh zurück.
    // Lädt das Mesh hoch falls noch nicht geschehen (lazy, persistent).
    // Gibt nullptr zurück wenn MeshHandle ungültig oder Submesh nicht vorhanden.
    [[nodiscard]] const GpuMeshEntry* GetOrUploadMesh(
        MeshHandle mesh, uint32_t submeshIndex,
        assets::AssetRegistry& registry);

    // Gibt true zurück wenn das Mesh bereits hochgeladen ist
    [[nodiscard]] bool IsMeshUploaded(MeshHandle mesh, uint32_t submeshIndex) const noexcept;

    void ScheduleDestroy(BufferHandle handle, uint64_t retirementFenceValue);
    void ScheduleDestroy(TextureHandle handle, uint64_t retirementFenceValue);
    void ScheduleDestroy(RenderTargetHandle handle, uint64_t retirementFenceValue);

    [[nodiscard]] uint32_t     GetFramesInFlight() const noexcept { return m_framesInFlight; }
    [[nodiscard]] const Stats& GetStats()          const noexcept { return m_stats; }
    [[nodiscard]] bool         IsRenderThread()    const noexcept;
    [[nodiscard]] bool         HasPendingUploads() const noexcept;
    [[nodiscard]] const std::vector<PendingBufferUpload>& GetPendingBufferUploads() const noexcept;
    void ClearPendingUploads() noexcept;

private:
    // Mesh-Cache-Schlüssel: MeshHandle + Submesh-Index
    struct MeshCacheKey
    {
        uint32_t meshHandleValue = 0u;
        uint32_t submeshIndex    = 0u;
        bool operator==(const MeshCacheKey& o) const noexcept
        { return meshHandleValue == o.meshHandleValue && submeshIndex == o.submeshIndex; }
    };
    struct MeshCacheKeyHash {
        size_t operator()(const MeshCacheKey& k) const noexcept {
            return std::hash<uint64_t>{}(
                (static_cast<uint64_t>(k.meshHandleValue) << 32) | k.submeshIndex);
        }
    };

    struct PooledTransientRT
    {
        RenderTargetDesc  desc;
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
        uint64_t                        fenceValue = 0u;
        std::vector<FrameUploadBuffer>  uploadBuffers;
        std::vector<PendingBufferUpload> pendingBufferUploads;
    };

    struct PendingDestroy
    {
        enum class Type : uint8_t { Buffer, Texture, RenderTarget } type = Type::Buffer;
        BufferHandle       buffer      = BufferHandle::Invalid();
        TextureHandle      texture     = TextureHandle::Invalid();
        RenderTargetHandle renderTarget = RenderTargetHandle::Invalid();
        uint64_t           retireAfterFence = 0u;
    };

    [[nodiscard]] bool Matches(const RenderTargetDesc& a,
                                const RenderTargetDesc& b) const noexcept;
    void RetireCompleted(uint64_t completedFenceValue);
    void DestroyNow(const PendingDestroy& pending);
    void WaitForCompletedValue(uint64_t fenceValue);
    [[nodiscard]] uint64_t GetMaxOutstandingFenceValue() const noexcept;
    [[nodiscard]] bool RequireRenderThread(const char* opName) const noexcept;

    void TrackAllocation(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept;
    void TrackRelease(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept;

    IDevice*  m_device              = nullptr;
    uint32_t  m_framesInFlight      = 0u;
    uint32_t  m_currentFrameSlot    = 0u;
    uint64_t  m_completedFenceValue = 0u;
    uint64_t  m_submittedFenceValue = 0u;
    IFence*    m_frameFence          = nullptr;
    std::thread::id m_renderThreadId{};

    std::vector<FrameSlot>           m_frameSlots;
    std::vector<PooledTransientRT>   m_transientRTPool;
    std::vector<PendingDestroy>      m_pendingDestroy;

    // Persistenter Mesh-GPU-Cache (bleibt bis Shutdown)
    std::unordered_map<MeshCacheKey, GpuMeshEntry, MeshCacheKeyHash> m_meshCache;

    Stats m_stats{};
};

} // namespace engine::renderer
