#include "addons/editor/EditorAnimationControllers.hpp"

#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/AtomicTextFile.hpp"
#include "core/PathUtf8.hpp"

#include <fstream>
#include <iterator>
#include <unordered_map>

namespace engine::renderer::addons::editor {

namespace anim = ::engine::addons::animation;

const EditorAnimationController& GetEditorAnimationController(EditorFrameContext& ctx, const std::string& assetPath)
{
    struct Cached
    {
        std::filesystem::file_time_type stamp{};
        bool valid = false;
        EditorAnimationController controller;
    };
    static std::unordered_map<std::string, Cached> cache;
    static const EditorAnimationController empty;
    if (!ctx.assetPipeline || assetPath.empty())
        return empty;

    const std::filesystem::path path = ctx.assetPipeline->GetAssetRoot() / PathFromUtf8(assetPath);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    Cached& cached = cache[PathToGenericUtf8(ctx.assetPipeline->GetAssetRoot()) + "|" + assetPath];
    if (cached.valid && (ec ? std::filesystem::file_time_type{} : stamp) == cached.stamp)
        return cached.controller;
    cached.valid = true;
    cached.stamp = ec ? std::filesystem::file_time_type{} : stamp;
    cached.controller = {};
    if (ec)
        return cached.controller;

    cached.controller.fileFound = true;
    std::ifstream in(path, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!anim::ParseAnimationController(source, cached.controller.asset, &cached.controller.error))
        return cached.controller;
    // Clips the controller names are imported like the animator's own clip.
    auto& pipeline = *ctx.assetPipeline;
    auto& registry = ctx.registry;
    cached.controller.compiled = anim::CompileAnimationController(cached.controller.asset,
        [&](const std::string& clipPath) {
            anim::AnimationClipInfo info;
            AnimationHandle handle = registry.FindAnimationByPath(clipPath);
            if (!handle.IsValid())
            {
                (void)engine::addons::mesh_renderer::ImportModelAnimationAssets(pipeline, registry, clipPath);
                handle = registry.FindAnimationByPath(clipPath);
            }
            if (const auto* clip = registry.animations.Get(handle))
            {
                info.handle = handle;
                info.duration = clip->duration;
                info.looping = clip->looping;
                info.found = true;
            }
            return info;
        });
    return cached.controller;
}

std::string CreateAnimationControllerAsset(EditorFrameContext& ctx, const std::filesystem::path& directory)
{
    if (!ctx.assetPipeline)
        return {};
    const std::filesystem::path file = MakeUniqueFilesystemPath(directory, "NewController", anim::kAnimationControllerExtension);
    if (!WriteTextFileAtomically(file, anim::SerializeAnimationController(anim::AnimationControllerAsset{})))
    {
        ctx.lastFileMessage = Translate("status.save_failed");
        return {};
    }
    return PathToGenericUtf8(file.lexically_relative(ctx.assetPipeline->GetAssetRoot()));
}

} // namespace engine::renderer::addons::editor
