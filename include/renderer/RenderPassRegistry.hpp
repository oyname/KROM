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
    static RenderPassRegistry& Instance();

    [[nodiscard]] RenderPassID Register(std::string name, RenderPassSortMode sortMode);
    [[nodiscard]] RenderPassID FindByName(std::string_view name) const noexcept;
    [[nodiscard]] const RenderPassDesc* Get(RenderPassID id) const noexcept;
    [[nodiscard]] std::string_view GetName(RenderPassID id) const noexcept;
    [[nodiscard]] RenderPassSortMode GetSortMode(RenderPassID id) const noexcept;
    [[nodiscard]] const std::vector<RenderPassDesc>& GetAll() const noexcept { return m_passes; }

private:
    std::vector<RenderPassDesc> m_passes;
};

namespace StandardRenderPasses {

[[nodiscard]] RenderPassID Opaque();
[[nodiscard]] RenderPassID AlphaCutout();
[[nodiscard]] RenderPassID Transparent();
[[nodiscard]] RenderPassID Shadow();
[[nodiscard]] RenderPassID UI();
[[nodiscard]] RenderPassID Postprocess();

} // namespace StandardRenderPasses

[[nodiscard]] inline std::string_view RenderPassName(RenderPassID id) noexcept
{
    return RenderPassRegistry::Instance().GetName(id);
}

[[nodiscard]] inline RenderPassSortMode RenderPassSort(RenderPassID id) noexcept
{
    return RenderPassRegistry::Instance().GetSortMode(id);
}

} // namespace engine::renderer
