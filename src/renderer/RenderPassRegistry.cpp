#include "renderer/RenderPassRegistry.hpp"

namespace engine::renderer {

RenderPassRegistry& RenderPassRegistry::Instance()
{
    static RenderPassRegistry registry;
    return registry;
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

namespace StandardRenderPasses {

RenderPassID Opaque()
{
    static const RenderPassID id = RenderPassRegistry::Instance().Register("forward.opaque", RenderPassSortMode::FrontToBack);
    return id;
}

RenderPassID AlphaCutout()
{
    static const RenderPassID id = RenderPassRegistry::Instance().Register("forward.alpha_cutout", RenderPassSortMode::FrontToBack);
    return id;
}

RenderPassID Transparent()
{
    static const RenderPassID id = RenderPassRegistry::Instance().Register("forward.transparent", RenderPassSortMode::BackToFront);
    return id;
}

RenderPassID Shadow()
{
    static const RenderPassID id = RenderPassRegistry::Instance().Register("forward.shadow", RenderPassSortMode::FrontToBack);
    return id;
}

RenderPassID UI()
{
    static const RenderPassID id = RenderPassRegistry::Instance().Register("forward.ui", RenderPassSortMode::SubmissionOrder);
    return id;
}

RenderPassID Postprocess()
{
    static const RenderPassID id = RenderPassRegistry::Instance().Register("forward.postprocess", RenderPassSortMode::SubmissionOrder);
    return id;
}

} // namespace StandardRenderPasses

} // namespace engine::renderer
