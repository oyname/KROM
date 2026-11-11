#pragma once

#include <cstdint>
#include <functional>

namespace engine::math { struct Vec2; }

namespace engine::assets {
struct MaterialAsset;
enum class ShaderStage : uint8_t;
}

namespace engine::renderer::addons::editor {
struct EditorFrameContext;
enum class EditorMaterialTemplate : uint8_t;

using DrawMaterialUVInput = std::function<bool(const char*, math::Vec2&, uint32_t&, float, float)>;

bool DrawMaterialGraph(EditorFrameContext& ctx, assets::MaterialAsset& asset,
                       EditorMaterialTemplate materialType,
                       const std::function<EditorMaterialTemplate()>& drawMaterialType = {},
                       const std::function<bool(EditorMaterialTemplate, const DrawMaterialUVInput&)>& drawSettings = {},
                       const std::function<bool(assets::ShaderStage, float)>& drawShaderSource = {});

} // namespace engine::renderer::addons::editor
