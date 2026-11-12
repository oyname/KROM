#pragma once
// =============================================================================
// KROM Engine - addons/dx11/DX11GpuTimestamps.hpp
// GPU timestamps for DX11 (IDevice/ICommandList GPU timestamp interface).
// A ring of query sets, one per device frame: a TIMESTAMP_DISJOINT query around
// the frame plus up to kCapacity TIMESTAMP queries. Results are read with
// D3D11_ASYNC_GETDATA_DONOTFLUSH, so the CPU never waits for the GPU.
// =============================================================================
#include "renderer/IDevice.hpp"
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Query;

namespace engine::renderer::dx11 {

class DX11GpuTimestamps
{
public:
    static constexpr uint32_t kCapacity = 256u;
    static constexpr uint32_t kFrameSets = 5u;

    ~DX11GpuTimestamps() { Release(); }

    void Init(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
    void Release() noexcept;

    void SetEnabled(bool enabled);
    [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }

    void BeginFrame(uint64_t frameId);
    void EndFrame();
    uint32_t Write();
    void TakeCompleted(std::vector<GpuTimestampFrame>& out);

private:
    struct FrameSet
    {
        ID3D11Query*              disjoint = nullptr;
        std::vector<ID3D11Query*> stamps;
        uint64_t                  frameId = 0u;
        uint32_t                  written = 0u;
        bool                      open    = false;  // disjoint begun, not ended
        bool                      pending = false;  // ended, results not read
    };

    bool CreateQueries();
    void DestroyQueries() noexcept;

    ID3D11Device*         m_device  = nullptr;
    ID3D11DeviceContext*  m_context = nullptr;
    std::vector<FrameSet> m_sets;
    uint32_t              m_current = 0u;
    bool                  m_enabled = false;
};

} // namespace engine::renderer::dx11
