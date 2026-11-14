#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorAnimationControllers.hpp
// Animation controllers (*.animctrl) in the editor: creating the file, and a
// cached, compiled view for the inspector (states, parameters, problems).
// The graph editor (step 4) builds on the same functions.
// =============================================================================
#include "addons/animation/AnimationController.hpp"
#include "addons/editor/EditorFeature.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace engine::renderer::addons::editor {

struct EditorAnimationController
{
    bool        fileFound = false;
    std::string error;        // file could not be read
    engine::addons::animation::AnimationControllerAsset    asset;
    engine::addons::animation::CompiledAnimationController compiled;   // problems, clip lookup
};

// Controller at a path relative to Assets; cached until the file changes.
[[nodiscard]] const EditorAnimationController& GetEditorAnimationController(EditorFrameContext& ctx,
                                                                            const std::string& assetPath);

// Clip lookup for compiling in the editor: registry first, else imports the model
// or .anim the path names.
[[nodiscard]] engine::addons::animation::AnimationClipResolver MakeEditorClipResolver(EditorFrameContext& ctx);

// Short label of a clip reference. A bare .anim path refers to its first clip.
[[nodiscard]] std::string EditorClipLabel(EditorFrameContext& ctx, const std::string& clipAssetPath);

// Writes an empty controller (unique name) into the directory; returns its path
// relative to Assets, empty on failure.
std::string CreateAnimationControllerAsset(EditorFrameContext& ctx, const std::filesystem::path& directory);

} // namespace engine::renderer::addons::editor
