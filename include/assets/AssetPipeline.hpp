#pragma once
#include "assets/AssetImporter.hpp"
#include "assets/AssetRegistry.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
class Scene;
}

namespace engine::ecs {
class World;
}

namespace engine::platform {
class IFilesystem;
class StdFilesystem;
}

namespace engine::renderer {
class IDevice;
}

namespace engine::assets {

class AssetPipeline
{
public:
    // AssetPipeline ist bewusst eine engine-runtime-nahe Schicht:
    // Sie verbindet Asset-Registry, Dateisystem, Shader-Compile-Pfad und
    // optionalen GPU-Upload. Sie ist kein minimaler Core-Mechanismus.
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
    ~AssetPipeline();

    void SetAssetRoot(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& GetAssetRoot() const noexcept { return m_assetRoot; }

    // Registriert einen Importer für bestimmte Dateiendungen (z.B. GltfImporter für .gltf/.glb).
    // Der Aufrufer muss das Format nicht kennen — ImportBundle() wählt automatisch.
    void RegisterMeshImporter(std::unique_ptr<IAssetImporter> importer);

    // Importiert eine 3D-Asset-Datei über den passenden registrierten Importer.
    // Format-agnostisch: .glb, .fbx, .obj, .usd — alles was einen Importer hat.
    // Gibt bundle.Ok()==false zurück wenn kein Importer registriert ist.
    [[nodiscard]] ImportedAssetBundle ImportBundle(const std::string& path);

    [[nodiscard]] MeshHandle LoadMesh(const std::string& path);
    [[nodiscard]] TextureHandle LoadTexture(const std::string& path);
    [[nodiscard]] ShaderHandle LoadShader(const std::string& path, ShaderStage fallbackStage = ShaderStage::Vertex);
    [[nodiscard]] MaterialHandle LoadMaterial(const std::string& path);
    bool LoadScene(const std::string& path, Scene& scene);

    void RegisterSceneDirectiveHandler(SceneDirectiveHandler handler);
    void ClearSceneDirectiveHandlers() { m_sceneDirectiveHandlers.clear(); }

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
    std::unique_ptr<platform::StdFilesystem> m_ownedFs; // Fallback wenn kein IFilesystem injiziert wurde
    std::filesystem::path m_assetRoot;

    std::vector<std::unique_ptr<IAssetImporter>> m_meshImporters;

    std::unordered_map<TextureHandle, TextureHandle> m_gpuTextures;
    std::vector<TextureHandle>                      m_retiredGpuTextures;
    std::unordered_map<ShaderHandle, ShaderHandle> m_gpuShaders;

    std::filesystem::path Resolve(const std::string& path) const;

    std::vector<SceneDirectiveHandler> m_sceneDirectiveHandlers;

    bool ReloadMesh(MeshHandle handle, const std::filesystem::path& path);
    bool ReloadTexture(TextureHandle handle, const std::filesystem::path& path);
    bool ReloadShader(ShaderHandle handle, const std::filesystem::path& path, ShaderStage fallbackStage);
    static ShaderSourceLanguage InferShaderLanguage(const std::filesystem::path& path, const std::string& source);
    bool ReloadMaterial(MaterialHandle handle, const std::filesystem::path& path);

    // Lädt die Textur unter ref.path (falls nicht leer), setzt die richtige
    // TextureMetadata für den jeweiligen PBR-Slot und speichert den Handle in ref.texture.
    void ResolveTextureRef(MaterialTextureRef& ref,
                           ColorSpace colorSpace,
                           TextureSemantic semantic,
                           NormalEncoding normalEncoding);
};

}
