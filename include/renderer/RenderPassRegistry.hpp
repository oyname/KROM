#pragma once

#include "renderer/RendererTypes.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace engine::renderer {

enum class RenderPassSortMode : uint8_t
{
    FrontToBack = 0,
    BackToFront,
    SubmissionOrder,
};

struct RenderPassDesc
{
    RenderPassID       id = RenderPassID::Invalid();
    std::string        name;
    RenderPassSortMode sortMode = RenderPassSortMode::FrontToBack;
};

class RenderPassRegistry
{
public:
    RenderPassRegistry();

    [[nodiscard]] RenderPassID Register(std::string name, RenderPassSortMode sortMode);
    [[nodiscard]] RenderPassID FindByName(std::string_view name) const noexcept;
    [[nodiscard]] const RenderPassDesc* Get(RenderPassID id) const noexcept;
    [[nodiscard]] std::string_view GetName(RenderPassID id) const noexcept;
    [[nodiscard]] RenderPassSortMode GetSortMode(RenderPassID id) const noexcept;
    [[nodiscard]] const std::vector<RenderPassDesc>& GetAll() const noexcept { return m_passes; }
    void CopyFrom(const RenderPassRegistry& other);

private:
    std::vector<RenderPassDesc> m_passes;
};

namespace StandardRenderPasses {

[[nodiscard]] RenderPassID Opaque() noexcept;
[[nodiscard]] RenderPassID AlphaCutout() noexcept;
[[nodiscard]] RenderPassID Transparent() noexcept;
[[nodiscard]] RenderPassID Shadow() noexcept;
[[nodiscard]] RenderPassID UI() noexcept;
[[nodiscard]] RenderPassID Postprocess() noexcept;

} // namespace StandardRenderPasses

[[nodiscard]] inline std::string_view RenderPassName(const RenderPassRegistry& registry, RenderPassID id) noexcept
{
    return registry.GetName(id);
}

[[nodiscard]] inline RenderPassSortMode RenderPassSort(const RenderPassRegistry& registry, RenderPassID id) noexcept
{
    return registry.GetSortMode(id);
}

} // namespace engine::renderer
