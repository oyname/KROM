#pragma once

#include "renderer/IDevice.hpp"
#include "renderer/RenderFramePassInterfaces.hpp"
#include "renderer/RendererTypes.hpp"
#include "ecs/World.hpp"
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace engine::renderer::addons::outline {

struct OutlineGpuResources
{
    ShaderHandle   vsMask;
    ShaderHandle   psMask;
    ShaderHandle   vsFullscreen;
    ShaderHandle   psDilate;
    PipelineHandle pipelineDilate;
    // Per-vertex-stride mask pipeline cache (POSITION always at offset 0, but stride varies)
    std::unordered_map<uint32_t, PipelineHandle> maskPipelines;
    // Small CPU-writable CB for per-object data — used on backends that don't have a
    // persistent perObjectArena (e.g. Vulkan which uses Push Constants instead).
    BufferHandle   perObjectCB;
    bool initialized = false;
};

bool InitializeOutlineResources(IDevice& device, OutlineGpuResources& res);
void DestroyOutlineResources(IDevice& device, OutlineGpuResources& res);

class OutlinePass final : public IFramePass
{
public:
    OutlinePass(uint32_t id,
                std::shared_ptr<OutlineGpuResources> resources,
                std::function<EntityID()> getSelected,
                std::function<bool(EntityID)> shouldOutlineEntity);
    ~OutlinePass() override = default;

    uint32_t  GetContributorId()  const noexcept override { return m_id; }
    PassPhase GetPhase()          const noexcept override { return PassPhase::AfterOpaque; }
    bool      DeclaresResource(std::string_view name) const noexcept override;
    void      BuildPass(const PassBuildContext& ctx) const override;

private:
    uint32_t m_id;
    std::shared_ptr<OutlineGpuResources> m_resources;
    std::function<EntityID()> m_getSelected;
    std::function<bool(EntityID)> m_shouldOutlineEntity;
};

} // namespace engine::renderer::addons::outline
