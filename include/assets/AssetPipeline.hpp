#pragma once
#include "assets/AssetRegistry.hpp"
#include "scene/Scene.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "platform/IFilesystem.hpp"
#include "platform/StdFilesystem.hpp"
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace engine::assets {

class AssetPipeline
{
public:
    struct SceneDirectiveContext
    {
        ecs::World& world;
        EntityID entity = NULL_ENTITY;
        AssetPipeline& pipeline;
    };

    using SceneDirectiveHandler = std::function<bool(const std::string& directive,
                                                     const std::vector<std::string>& parts,
                                                     const SceneDirectiveContext& context)>;

    // fs: optionaler IFilesystem - bei nullptr wird intern StdFilesystem verwendet.
    // Für Tests: NullFilesystem injizieren → kein echtes Dateisystem nötig.
    AssetPipeline(AssetRegistry& registry,
                  renderer::IDevice* device = nullptr,
                  platform::IFilesystem* fs = nullptr);

    void SetAssetRoot(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& GetAssetRoot() const noexcept { return m_assetRoot; }

    [[nodiscard]] MeshHandle LoadMesh(const std::string& path);
    [[nodiscard]] TextureHandle LoadTexture(const std::string& path);
    [[nodiscard]] ShaderHandle LoadShader(const std::string& path, ShaderStage fallbackStage = ShaderStage::Vertex);
    [[nodiscard]] MaterialHandle LoadMaterial(const std::string& path);
    bool LoadScene(const std::string& path, Scene& scene);

    void SetSceneDirectiveHandler(SceneDirectiveHandler handler) { m_sceneDirectiveHandler = std::move(handler); }

    void PollHotReload();
    bool BuildShaderCache(ShaderHandle handle, ShaderTargetProfile target);
    void BuildPendingShaderCaches();
    void UploadPendingGpuAssets();

    [[nodiscard]] TextureHandle GetGpuTexture(TextureHandle handle) const noexcept;
    [[nodiscard]] ShaderHandle GetGpuShader(ShaderHandle handle) const noexcept;

private:
    AssetRegistry& m_registry;
    renderer::IDevice* m_device = nullptr;
    platform::IFilesystem* m_fs = nullptr;
    platform::StdFilesystem m_ownedFs; // Fallback wenn kein IFilesystem injiziert wurde
    std::filesystem::path m_assetRoot;

    std::unordered_map<TextureHandle, TextureHandle> m_gpuTextures;
    std::unordered_map<ShaderHandle, ShaderHandle> m_gpuShaders;

    std::filesystem::path Resolve(const std::string& path) const;

    SceneDirectiveHandler m_sceneDirectiveHandler;

    bool ReloadMesh(MeshHandle handle, const std::filesystem::path& path);
    bool ReloadTexture(TextureHandle handle, const std::filesystem::path& path);
    bool ReloadShader(ShaderHandle handle, const std::filesystem::path& path, ShaderStage fallbackStage);
    static ShaderSourceLanguage InferShaderLanguage(const std::filesystem::path& path, const std::string& source);
    bool ReloadMaterial(MaterialHandle handle, const std::filesystem::path& path);
};

}
