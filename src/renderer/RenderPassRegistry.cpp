#include "renderer/RenderPassRegistry.hpp"

namespace engine::renderer {

namespace {

constexpr uint16_t kOpaquePassValue = 1u;
constexpr uint16_t kAlphaCutoutPassValue = 2u;
constexpr uint16_t kTransparentPassValue = 3u;
constexpr uint16_t kShadowPassValue = 4u;
constexpr uint16_t kUiPassValue = 5u;
constexpr uint16_t kPostprocessPassValue = 6u;

} // namespace

RenderPassRegistry::RenderPassRegistry()
{
    m_passes = {
        { RenderPassID{kOpaquePassValue}, "forward.opaque", RenderPassSortMode::FrontToBack },
        { RenderPassID{kAlphaCutoutPassValue}, "forward.alpha_cutout", RenderPassSortMode::FrontToBack },
        { RenderPassID{kTransparentPassValue}, "forward.transparent", RenderPassSortMode::BackToFront },
        { RenderPassID{kShadowPassValue}, "forward.shadow", RenderPassSortMode::FrontToBack },
        { RenderPassID{kUiPassValue}, "forward.ui", RenderPassSortMode::SubmissionOrder },
        { RenderPassID{kPostprocessPassValue}, "forward.postprocess", RenderPassSortMode::SubmissionOrder },
    };
}

RenderPassID RenderPassRegistry::Register(std::string name, RenderPassSortMode sortMode)
{
    for (const RenderPassDesc& pass : m_passes)
    {
        if (pass.name == name)
            return pass.id;
    }

    const uint16_t nextId = static_cast<uint16_t>(m_passes.size() + 1u);
    RenderPassDesc pass{};
    pass.id = RenderPassID{nextId};
    pass.name = std::move(name);
    pass.sortMode = sortMode;
    m_passes.push_back(std::move(pass));
    return m_passes.back().id;
}

RenderPassID RenderPassRegistry::FindByName(std::string_view name) const noexcept
{
    for (const RenderPassDesc& pass : m_passes)
    {
        if (pass.name == name)
            return pass.id;
    }
    return RenderPassID::Invalid();
}

const RenderPassDesc* RenderPassRegistry::Get(RenderPassID id) const noexcept
{
    if (!id.IsValid())
        return nullptr;

    const size_t index = static_cast<size_t>(id.value - 1u);
    if (index >= m_passes.size())
        return nullptr;
    return &m_passes[index];
}

std::string_view RenderPassRegistry::GetName(RenderPassID id) const noexcept
{
    const RenderPassDesc* pass = Get(id);
    return pass ? std::string_view(pass->name) : std::string_view("Invalid");
}

RenderPassSortMode RenderPassRegistry::GetSortMode(RenderPassID id) const noexcept
{
    const RenderPassDesc* pass = Get(id);
    return pass ? pass->sortMode : RenderPassSortMode::FrontToBack;
}

void RenderPassRegistry::CopyFrom(const RenderPassRegistry& other)
{
    if (this == &other)
        return;
    m_passes = other.m_passes;
}

namespace StandardRenderPasses {

RenderPassID Opaque() noexcept
{
    return RenderPassID{kOpaquePassValue};
}

RenderPassID AlphaCutout() noexcept
{
    return RenderPassID{kAlphaCutoutPassValue};
}

RenderPassID Transparent() noexcept
{
    return RenderPassID{kTransparentPassValue};
}

RenderPassID Shadow() noexcept
{
    return RenderPassID{kShadowPassValue};
}

RenderPassID UI() noexcept
{
    return RenderPassID{kUiPassValue};
}

RenderPassID Postprocess() noexcept
{
    return RenderPassID{kPostprocessPassValue};
}

} // namespace StandardRenderPasses

} // namespace engine::renderer
