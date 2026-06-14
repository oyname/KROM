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

// Beschreibt States die ein Pass absolut erzwingt — kein Material darf diese überschreiben.
// Fullscreen/Postprocess: DepthTest=OFF, DepthWrite=OFF, Cull=OFF (sonst schwarzes Bild auf OpenGL).
// Shadow: DepthTest=ON, DepthWrite=ON (zwingend für korrekte Shadow Maps).
struct PassLocks
{
    bool depthTestLocked  = false;
    bool depthWriteLocked = false;
    bool cullModeLocked   = false;

    bool     depthTestValue  = false;
    bool     depthWriteValue = false;
    CullMode cullModeValue   = CullMode::None;
};

struct RenderPassDesc
{
    RenderPassID       id = RenderPassID::Invalid();
    std::string        name;
    RenderPassSortMode sortMode = RenderPassSortMode::FrontToBack;
    PassLocks          locks{};
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
    [[nodiscard]] const PassLocks* GetLocks(RenderPassID id) const noexcept;
    [[nodiscard]] const std::vector<RenderPassDesc>& GetAll() const noexcept { return m_passes; }
    void CopyFrom(const RenderPassRegistry& other);

private:
    std::vector<RenderPassDesc> m_passes;
};

namespace StandardRenderPasses {

inline constexpr uint16_t OpaqueValue = 1u;
inline constexpr uint16_t AlphaCutoutValue = 2u;
inline constexpr uint16_t TransparentValue = 3u;
inline constexpr uint16_t ShadowValue = 4u;
inline constexpr uint16_t UiValue = 5u;
inline constexpr uint16_t PostprocessValue = 6u;
inline constexpr uint16_t EditorGizmoValue = 7u;

[[nodiscard]] constexpr RenderPassID Opaque() noexcept { return RenderPassID{OpaqueValue}; }
[[nodiscard]] constexpr RenderPassID AlphaCutout() noexcept { return RenderPassID{AlphaCutoutValue}; }
[[nodiscard]] constexpr RenderPassID Transparent() noexcept { return RenderPassID{TransparentValue}; }
[[nodiscard]] constexpr RenderPassID Shadow() noexcept { return RenderPassID{ShadowValue}; }
[[nodiscard]] constexpr RenderPassID UI() noexcept { return RenderPassID{UiValue}; }
[[nodiscard]] constexpr RenderPassID Postprocess() noexcept { return RenderPassID{PostprocessValue}; }
[[nodiscard]] constexpr RenderPassID EditorGizmo() noexcept { return RenderPassID{EditorGizmoValue}; }

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
