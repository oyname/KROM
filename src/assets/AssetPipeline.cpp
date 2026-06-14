#include "assets/AssetPipeline.hpp"
#include "assets/KMeshSerializer.hpp"
#include "assets/MeshTangents.hpp"
#include "assets/TextureImporter.hpp"
#include "core/Debug.hpp"
#include "platform/StdFilesystem.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/TextureFormatUtils.hpp"
#include "scene/Scene.hpp"
#include <sstream>
#include <algorithm>
#include <future>
#include <memory>
#include <vector>

namespace engine::assets {
    namespace fs = std::filesystem;
    using namespace engine::renderer;
    using engine::math::Vec3;
    using engine::math::Vec4;

    static std::string Trim(const std::string& s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static std::vector<std::string> SplitWs(const std::string& s)
    {
        std::istringstream iss(s);
        std::vector<std::string> parts;
        std::string p;
        while (iss >> p) parts.push_back(p);
        return parts;
    }

    static bool HasAnySuffix(const std::string& value, std::initializer_list<const char*> suffixes)
    {
        for (const char* suffix : suffixes)
        {
            const size_t suffixLen = std::char_traits<char>::length(suffix);
            if (value.size() >= suffixLen && value.compare(value.size() - suffixLen, suffixLen, suffix) == 0)
                return true;
        }
        return false;
    }

    static std::string ReplaceSuffix(const std::string& value, const char* suffix, const char* replacement)
    {
        const size_t suffixLen = std::char_traits<char>::length(suffix);
        if (value.size() < suffixLen || value.compare(value.size() - suffixLen, suffixLen, suffix) != 0)
            return {};
        return value.substr(0u, value.size() - suffixLen) + replacement;
    }

    static std::string OpenGlShaderVariantPath(const std::string& path, ShaderStage stage)
    {
        if (path.empty())
            return {};
        if (stage == ShaderStage::Vertex)
        {
            if (std::string candidate = ReplaceSuffix(path, ".vs.hlsl", ".opengl.vs.glsl"); !candidate.empty())
                return candidate;
        }
        if (stage == ShaderStage::Fragment)
        {
            if (std::string candidate = ReplaceSuffix(path, ".ps.hlsl", ".opengl.fs.glsl"); !candidate.empty())
                return candidate;
        }
        return {};
    }

    static ShaderStage InferShaderStage(const fs::path& path, ShaderStage fallback)
    {
        const std::string filename = path.filename().string();
        if (HasAnySuffix(filename, {".vs.hlsl", ".vs.glsl", ".vert", ".vs", ".hlslvs"})) return ShaderStage::Vertex;
        if (HasAnySuffix(filename, {".ps.hlsl", ".fs.glsl", ".frag", ".fs", ".ps", ".hlslps"})) return ShaderStage::Fragment;
        if (HasAnySuffix(filename, {".cs.hlsl", ".cs.glsl", ".comp", ".cs"})) return ShaderStage::Compute;
        return fallback;
    }

    static ShaderSourceLanguage InferLanguageFromExt(const fs::path& path)
    {
        const std::string filename = path.filename().string();
        const auto ext = path.extension().string();
        if (HasAnySuffix(filename, {".hlsl", ".vs.hlsl", ".ps.hlsl", ".cs.hlsl", ".hlslvs", ".hlslps", ".vs", ".ps", ".cs"}))
            return ShaderSourceLanguage::HLSL;
        if (HasAnySuffix(filename, {".glsl", ".vs.glsl", ".fs.glsl", ".cs.glsl", ".vert", ".frag", ".comp"}))
            return ShaderSourceLanguage::GLSL;
        if (ext == ".wgsl")
            return ShaderSourceLanguage::WGSL;
        return ShaderSourceLanguage::Unknown;
    }

    static Format ToFormat(TextureFormat f, bool srgb)
    {
        switch (f)
        {
        case TextureFormat::RGBA8_SRGB: return Format::RGBA8_UNORM_SRGB;
        case TextureFormat::RGBA8_UNORM: return srgb ? Format::RGBA8_UNORM_SRGB : Format::RGBA8_UNORM;
        case TextureFormat::R8_UNORM: return Format::R8_UNORM;
        case TextureFormat::RG8_UNORM: return Format::RG8_UNORM;
        case TextureFormat::RGBA16F: return Format::RGBA16_FLOAT;
        case TextureFormat::R11G11B10F: return Format::R11G11B10_FLOAT;
        case TextureFormat::DEPTH24_STENCIL8: return Format::D24_UNORM_S8_UINT;
        case TextureFormat::DEPTH32F: return Format::D32_FLOAT;
        case TextureFormat::BC1: return srgb ? Format::BC1_UNORM_SRGB : Format::BC1_UNORM;
        case TextureFormat::BC3: return srgb ? Format::BC3_UNORM_SRGB : Format::BC3_UNORM;
        case TextureFormat::BC4: return Format::BC4_UNORM;
        case TextureFormat::BC5: return Format::BC5_UNORM;
        case TextureFormat::BC7: return srgb ? Format::BC7_UNORM_SRGB : Format::BC7_UNORM;
        default: return Format::Unknown;
        }
    }


    static size_t ComputeTotalTextureByteSize(const TextureAsset& asset)
    {
        size_t total = 0u;
        for (uint32_t arrayLayer = 0u; arrayLayer < std::max(1u, asset.arraySize); ++arrayLayer)
        {
            for (uint32_t mip = 0u; mip < std::max(1u, asset.mipLevels); ++mip)
            {
                const auto layout = ComputeTextureUploadLayout(
                    asset.format,
                    std::max(1u, asset.width >> mip),
                    std::max(1u, asset.height >> mip),
                    std::max(1u, asset.depth >> mip));
                total += static_cast<size_t>(layout.byteSize);
            }
        }
        return total;
    }

    static ShaderStageMask ToStageMask(ShaderStage s)
    {
        switch (s)
        {
        case ShaderStage::Vertex: return ShaderStageMask::Vertex;
        case ShaderStage::Fragment: return ShaderStageMask::Fragment;
        case ShaderStage::Compute: return ShaderStageMask::Compute;
        case ShaderStage::Geometry: return ShaderStageMask::Geometry;
        case ShaderStage::Hull: return ShaderStageMask::Hull;
        case ShaderStage::Domain: return ShaderStageMask::Domain;
        default: return ShaderStageMask::Vertex;
        }
    }

    ShaderSourceLanguage AssetPipeline::InferShaderLanguage(const fs::path& path, const std::string& source)
    {
        ShaderSourceLanguage language = InferLanguageFromExt(path);
        if (language != ShaderSourceLanguage::Unknown)
            return language;

        if (source.find("SV_Position") != std::string::npos || source.find("cbuffer") != std::string::npos)
            return ShaderSourceLanguage::HLSL;
        if (source.find("gl_Position") != std::string::npos || source.find("#version") != std::string::npos)
            return ShaderSourceLanguage::GLSL;
        if (source.find("@vertex") != std::string::npos || source.find("@fragment") != std::string::npos)
            return ShaderSourceLanguage::WGSL;
        return ShaderSourceLanguage::Unknown;
    }

    AssetPipeline::AssetPipeline(AssetRegistry& registry, IDevice* device, platform::IFilesystem* fs)
        : m_registry(registry), m_device(device)
    {
        if (fs)
        {
            m_fs = fs;
        }
        else
        {
            m_ownedFs = std::make_unique<platform::StdFilesystem>();
            m_fs = m_ownedFs.get();
        }
    }

    AssetPipeline::~AssetPipeline()
    {
        if (!m_device)
            return;

        for (auto& [_, tex] : m_gpuTextures)
            if (tex.IsValid())
                m_device->DestroyTexture(tex);
        m_gpuTextures.clear();

        for (TextureHandle tex : m_retiredGpuTextures)
            if (tex.IsValid())
                m_device->DestroyTexture(tex);
        m_retiredGpuTextures.clear();
    }

    void AssetPipeline::RegisterMeshImporter(std::unique_ptr<IAssetImporter> importer)
    {
        if (importer)
            m_meshImporters.push_back(std::move(importer));
    }

    ImportedAssetBundle AssetPipeline::ImportBundle(const std::string& path)
    {
        const auto resolved = Resolve(path);
        const std::string ext = resolved.extension().string();

        for (auto& importer : m_meshImporters)
        {
            if (!importer->CanImport(ext))
                continue;
            ImportedAssetBundle bundle = importer->Import(resolved.string());
            if (!bundle.Ok())
                Debug::LogError("AssetPipeline::ImportBundle: '%s' Fehler: %s",
                    resolved.string().c_str(), bundle.error.c_str());
            return bundle;
        }

        ImportedAssetBundle empty;
        empty.error = "Kein Importer registriert fuer Endung '" + ext +
                      "' (Datei: " + resolved.string() + ")";
        return empty;
    }

    void AssetPipeline::RegisterSceneDirectiveHandler(SceneDirectiveHandler handler)
    {
        if (handler)
            m_sceneDirectiveHandlers.push_back(std::move(handler));
    }

    void AssetPipeline::SetAssetRoot(const fs::path& root)
    {
        m_assetRoot = root;
    }

    fs::path AssetPipeline::Resolve(const std::string& path) const
    {
        fs::path p(path);
        if (p.is_absolute() || m_assetRoot.empty()) return p;
        return m_assetRoot / p;
    }

    MeshHandle AssetPipeline::LoadMesh(const std::string& path)
    {
        auto asset = std::make_unique<MeshAsset>();
        const MeshHandle h = m_registry.GetOrAddMesh(path, std::move(asset));
        ReloadMesh(h, Resolve(path));
        return h;
    }

    TextureHandle AssetPipeline::LoadTexture(const std::string& path)
    {
        auto asset = std::make_unique<TextureAsset>();
        const TextureHandle h = m_registry.GetOrAddTexture(path, std::move(asset));
        if (auto* existing = m_registry.textures.Get(h))
        {
            const auto resolved = Resolve(path);
            const auto stats = m_fs->GetFileStats(resolved.string().c_str());
            if (existing->state == AssetState::Loaded &&
                stats.exists &&
                existing->lastModifiedTimestamp != 0ull &&
                existing->lastModifiedTimestamp == stats.lastModifiedTimestamp)
            {
                return h;
            }
        }
        if (!ReloadTexture(h, Resolve(path)))
            return TextureHandle::Invalid();
        return h;
    }

    ShaderHandle AssetPipeline::LoadShader(const std::string& path, ShaderStage fallbackStage)
    {
        auto asset = std::make_unique<ShaderAsset>();
        const ShaderHandle h = m_registry.GetOrAddShader(path, std::move(asset));
        if (!ReloadShader(h, Resolve(path), fallbackStage))
            return ShaderHandle::Invalid();
        return h;
    }

    MaterialHandle AssetPipeline::LoadMaterial(const std::string& path)
    {
        auto asset = std::make_unique<MaterialAsset>();
        const MaterialHandle h = m_registry.GetOrAddMaterial(path, std::move(asset));
        if (auto* existing = m_registry.materials.Get(h))
        {
            const auto resolved = Resolve(path);
            const auto stats = m_fs->GetFileStats(resolved.string().c_str());
            if (existing->state == AssetState::Loaded &&
                stats.exists &&
                existing->lastModifiedTimestamp != 0ull &&
                existing->lastModifiedTimestamp == stats.lastModifiedTimestamp)
            {
                return h;
            }
        }
        ReloadMaterial(h, Resolve(path));
        return h;
    }

    bool AssetPipeline::ReloadMesh(MeshHandle handle, const fs::path& path)
    {
        auto* mesh = m_registry.meshes.Get(handle);
        if (!mesh) return false;

        // .kmesh cache check: skip for files that are already .kmesh
        const std::string ext = path.extension().string();
        if (ext == ".kmesh")
        {
            std::vector<SubMeshData> cached;
            if (KMeshTryLoad(path, cached))
            {
                MeshAsset loaded;
                loaded.path                   = mesh->path;
                loaded.debugName              = path.filename().string();
                loaded.state                  = AssetState::Loaded;
                const auto stats = m_fs->GetFileStats(path.string().c_str());
                if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;
                loaded.submeshes              = std::move(cached);
                loaded.gpuStatus.dirty        = true;
                loaded.gpuStatus.uploaded     = false;
                *mesh = std::move(loaded);
                m_registry.NotifyMeshReloaded(handle);
                return true;
            }
            mesh->state = AssetState::Failed;
            return false;
        }

        {
            const fs::path cachePath = KMeshCachePath(path);
            const auto srcStats   = m_fs->GetFileStats(path.string().c_str());
            const auto cacheStats = m_fs->GetFileStats(cachePath.string().c_str());
            if (srcStats.exists && cacheStats.exists
                && cacheStats.lastModifiedTimestamp >= srcStats.lastModifiedTimestamp)
            {
                std::vector<SubMeshData> cached;
                if (KMeshTryLoad(cachePath, cached))
                {
                    MeshAsset loaded;
                    loaded.path                   = mesh->path;
                    loaded.debugName              = path.filename().string();
                    loaded.state                  = AssetState::Loaded;
                    loaded.lastModifiedTimestamp  = srcStats.lastModifiedTimestamp;
                    loaded.submeshes              = std::move(cached);
                    loaded.gpuStatus.dirty        = true;
                    loaded.gpuStatus.uploaded     = false;
                    *mesh = std::move(loaded);
                    m_registry.NotifyMeshReloaded(handle);
                    return true;
                }
                // Cache invalid — fall through to full import
            }
        }

        for (auto& importer : m_meshImporters)
        {
            if (!importer->CanImport(ext))
                continue;

            ImportedAssetBundle bundle = importer->Import(path.string());
            if (!bundle.Ok())
            {
                Debug::LogError("AssetPipeline: import failed for '%s': %s",
                    path.string().c_str(), bundle.error.c_str());
                mesh->state = AssetState::Failed;
                return false;
            }
            if (bundle.meshes.empty())
            {
                Debug::LogError("AssetPipeline: '%s' contains no meshes", path.string().c_str());
                mesh->state = AssetState::Failed;
                return false;
            }

            MeshAsset loaded;
            loaded.path      = mesh->path;
            loaded.debugName = path.filename().string();
            loaded.state     = AssetState::Loaded;
            const auto stats = m_fs->GetFileStats(path.string().c_str());
            if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;

            for (MeshAsset& src : bundle.meshes)
                for (SubMeshData& sm : src.submeshes)
                    loaded.submeshes.push_back(std::move(sm));

            const std::string materialBasePath = path.lexically_normal().string();
            std::vector<MaterialHandle> materialHandles;
            materialHandles.reserve(bundle.materials.size());
            for (size_t mi = 0; mi < bundle.materials.size(); ++mi)
            {
                auto material = std::make_unique<MaterialAsset>(std::move(bundle.materials[mi]));
                material->path = materialBasePath + "#material/" + std::to_string(mi);
                if (material->debugName.empty())
                    material->debugName = path.stem().string() + "_mat_" + std::to_string(mi);
                material->state = AssetState::Loaded;
                material->gpuStatus.dirty = true;
                material->gpuStatus.uploaded = false;

                const std::string materialPath = material->path;
                const MaterialHandle mh =
                    m_registry.GetOrAddMaterial(materialPath, std::move(material));
                materialHandles.push_back(mh);

                // Textur-Referenzen auflösen und Slot-spezifische Metadata setzen.
                auto* mat = m_registry.materials.Get(mh);
                if (mat)
                {
                    ResolveTextureRef(mat->baseColorTexture,
                        ColorSpace::SRGB,   TextureSemantic::Color,  NormalEncoding::None);
                    ResolveTextureRef(mat->emissiveTexture,
                        ColorSpace::SRGB,   TextureSemantic::Color,  NormalEncoding::None);
                    ResolveTextureRef(mat->metallicRoughnessTexture,
                        ColorSpace::Linear, TextureSemantic::Data,   NormalEncoding::None);
                    ResolveTextureRef(mat->occlusionTexture,
                        ColorSpace::Linear, TextureSemantic::Data,   NormalEncoding::None);
                    ResolveTextureRef(mat->normalTexture,
                        ColorSpace::Linear, TextureSemantic::Normal, NormalEncoding::RGB);
                }
            }

            loaded.materialHandles    = std::move(materialHandles);
            loaded.gpuStatus.dirty    = true;
            loaded.gpuStatus.uploaded = false;
            *mesh = std::move(loaded);
            m_registry.NotifyMeshReloaded(handle);

            if (ext != ".kmesh")
                KMeshSave(KMeshCachePath(path), mesh->submeshes);

            return true;
        }

        std::string source;
        if (!m_fs->ReadText(path.string().c_str(), source))
        {
            mesh->state = AssetState::Failed; return false;
        }

        MeshAsset loaded;
        loaded.path = mesh->path;
        loaded.debugName = path.filename().string();
        loaded.state = AssetState::Loaded;
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;

        SubMeshData sm;
        std::istringstream in(source);
        std::string line;
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto parts = SplitWs(line);
            if (parts.empty()) continue;
            if (parts[0] == "v" && parts.size() >= 4)
            {
                sm.positions.push_back(std::stof(parts[1]));
                sm.positions.push_back(std::stof(parts[2]));
                sm.positions.push_back(std::stof(parts[3]));
            }
            else if (parts[0] == "vt" && parts.size() >= 3)
            {
                sm.uvs.push_back(std::stof(parts[1]));
                sm.uvs.push_back(std::stof(parts[2]));
            }
            else if (parts[0] == "vn" && parts.size() >= 4)
            {
                sm.normals.push_back(std::stof(parts[1]));
                sm.normals.push_back(std::stof(parts[2]));
                sm.normals.push_back(std::stof(parts[3]));
            }
            else if (parts[0] == "vc" && parts.size() >= 5)
            {
                // Vertex-Farbe: vc r g b a  (normalisierte Floats, 4 Komponenten)
                sm.colors.push_back(std::stof(parts[1]));
                sm.colors.push_back(std::stof(parts[2]));
                sm.colors.push_back(std::stof(parts[3]));
                sm.colors.push_back(std::stof(parts[4]));
            }
            else if (parts[0] == "i" && parts.size() >= 4)
            {
                sm.indices.push_back(static_cast<uint32_t>(std::stoul(parts[1])));
                sm.indices.push_back(static_cast<uint32_t>(std::stoul(parts[2])));
                sm.indices.push_back(static_cast<uint32_t>(std::stoul(parts[3])));
            }
        }
        if (sm.positions.empty()) { mesh->state = AssetState::Failed; return false; }
        EnsureTangents(sm);
        loaded.submeshes = { std::move(sm) };
        loaded.gpuStatus.dirty = true;
        loaded.gpuStatus.uploaded = false;
        *mesh = std::move(loaded);
        m_registry.NotifyMeshReloaded(handle);

        if (ext != ".kmesh")
            KMeshSave(KMeshCachePath(path), mesh->submeshes);

        return true;
    }

    bool AssetPipeline::ReloadTexture(TextureHandle handle, const fs::path& path)
    {
        auto* tex = m_registry.textures.Get(handle);
        if (!tex) return false;

        TextureImportRequest request{};
        request.sourcePath = path;

        if (path.extension() == ".tex")
        {
            std::string source;
            if (!m_fs->ReadText(path.string().c_str(), source))
            {
                Debug::LogError("AssetPipeline: failed to read texture file '%s'",
                    path.string().c_str());
                tex->state = AssetState::Failed;
                return false;
            }
            request.fileBytes.assign(source.begin(), source.end());
        }
        else
        {
            if (!m_fs->ReadFile(path.string().c_str(), request.fileBytes))
            {
                Debug::LogError("AssetPipeline: failed to read texture file '%s'",
                    path.string().c_str());
                tex->state = AssetState::Failed;
                return false;
            }
        }

        TextureAsset loaded;
        if (!TextureImporter::Import(request, loaded))
        {
            tex->state = AssetState::Failed;
            return false;
        }

        loaded.path = tex->path;
        loaded.debugName = path.filename().string();
        loaded.state = AssetState::Loaded;
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;
        loaded.gpuStatus.dirty = true;
        loaded.gpuStatus.uploaded = false;

        *tex = std::move(loaded);
        return true;
    }

    bool AssetPipeline::ReloadShader(ShaderHandle handle, const fs::path& path, ShaderStage fallbackStage)
    {
        auto* shader = m_registry.shaders.Get(handle);
        if (!shader) return false;

        std::string source;
        if (!m_fs->ReadText(path.string().c_str(), source))
        {
            Debug::LogError("AssetPipeline: failed to read shader file '%s'",
                            path.string().c_str());
            shader->state = AssetState::Failed;
            return false;
        }

        ShaderAsset loaded;
        loaded.path = shader->path;
        loaded.debugName = path.filename().string();
        loaded.state = AssetState::Loaded;
        loaded.stage = InferShaderStage(path, fallbackStage);
        loaded.sourceCode = std::move(source);
        loaded.resolvedPath = path.lexically_normal().string();
        loaded.sourceLanguage = InferShaderLanguage(path, loaded.sourceCode);
        loaded.entryPoint = "main";
        loaded.compiledArtifacts.clear();
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;
        loaded.gpuStatus.dirty = true;
        loaded.gpuStatus.uploaded = false;
        *shader = std::move(loaded);
        return true;
    }

    bool AssetPipeline::ReloadMaterial(MaterialHandle handle, const fs::path& path)
    {
        auto* mat = m_registry.materials.Get(handle);
        if (!mat) return false;

        std::string source;
        if (!m_fs->ReadText(path.string().c_str(), source))
        {
            mat->state = AssetState::Failed; return false;
        }

        MaterialAsset loaded;
        loaded.path = mat->path;
        loaded.debugName = path.filename().string();
        loaded.state = AssetState::Loaded;
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;

        std::istringstream in(source);
        std::string line;
        auto loadMaterialShader = [&](const std::string& shaderPath, ShaderStage stage) -> ShaderHandle
        {
            if (m_device && m_device->GetShaderTargetProfile() == ShaderTargetProfile::OpenGL_GLSL450)
            {
                const std::string variant = OpenGlShaderVariantPath(shaderPath, stage);
                if (!variant.empty())
                {
                    const fs::path resolvedVariant = Resolve(variant);
                    const auto variantStats = m_fs->GetFileStats(resolvedVariant.string().c_str());
                    if (variantStats.exists)
                        return LoadShader(variant, stage);
                }
            }
            return LoadShader(shaderPath, stage);
        };
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto parts = SplitWs(line);
            if (parts.empty()) continue;
            if (parts[0] == "template" && parts.size() >= 2) loaded.templateName = parts[1];
            else if (parts[0] == "vertex" && parts.size() >= 2)
            {
                // Rest der Zeile nach "vertex " nehmen — Pfade koennen Leerzeichen enthalten
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                const std::string shaderPath = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
                loaded.vertexShaderPath = shaderPath;
                loaded.vertexShader     = loadMaterialShader(shaderPath, ShaderStage::Vertex);
            }
            else if (parts[0] == "fragment" && parts.size() >= 2)
            {
                // Rest der Zeile nach "fragment " nehmen — Pfade koennen Leerzeichen enthalten
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                const std::string shaderPath = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
                loaded.fragmentShaderPath = shaderPath;
                loaded.fragmentShader     = loadMaterialShader(shaderPath, ShaderStage::Fragment);
            }
            else if (parts[0] == "transparent" && parts.size() >= 2) loaded.transparent = (parts[1] == "1" || parts[1] == "true");
            else if (parts[0] == "doubleSided" && parts.size() >= 2) loaded.doubleSided = (parts[1] == "1" || parts[1] == "true");
            else if (parts[0] == "castShadows" && parts.size() >= 2) loaded.castShadows = (parts[1] == "1" || parts[1] == "true");
            else if (parts[0] == "baseColorFactor" && parts.size() >= 5)
            {
                loaded.baseColorFactor = {
                    std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3]), std::stof(parts[4])
                };
            }
            else if (parts[0] == "emissiveFactor" && parts.size() >= 4)
            {
                loaded.emissiveFactor = {
                    std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3])
                };
            }
            else if (parts[0] == "metallicFactor" && parts.size() >= 2) loaded.metallicFactor = std::stof(parts[1]);
            else if (parts[0] == "roughnessFactor" && parts.size() >= 2) loaded.roughnessFactor = std::stof(parts[1]);
            else if (parts[0] == "alphaCutoff" && parts.size() >= 2) loaded.alphaCutoff = std::stof(parts[1]);
            else if (parts[0] == "normalScale" && parts.size() >= 2) loaded.normalScale = std::stof(parts[1]);
            else if (parts[0] == "occlusionStrength" && parts.size() >= 2) loaded.occlusionStrength = std::stof(parts[1]);
            else if (parts[0] == "uvScale" && parts.size() >= 3)
                loaded.uvScale = { std::stof(parts[1]), std::stof(parts[2]) };
            else if (parts[0] == "uvOffset" && parts.size() >= 3)
                loaded.uvOffset = { std::stof(parts[1]), std::stof(parts[2]) };
            else if (parts[0] == "alphaMode" && parts.size() >= 2)
            {
                if (parts[1] == "mask") loaded.alphaMode = MaterialAlphaMode::Mask;
                else if (parts[1] == "blend") loaded.alphaMode = MaterialAlphaMode::Blend;
                else loaded.alphaMode = MaterialAlphaMode::Opaque;
            }
            else if (parts[0] == "baseColorTexture" && parts.size() >= 2) {
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                loaded.baseColorTexture.path = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
            }
            else if (parts[0] == "metallicRoughnessTexture" && parts.size() >= 2) {
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                loaded.metallicRoughnessTexture.path = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
            }
            else if (parts[0] == "normalTexture" && parts.size() >= 2) {
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                loaded.normalTexture.path = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
            }
            else if (parts[0] == "occlusionTexture" && parts.size() >= 2) {
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                loaded.occlusionTexture.path = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
            }
            else if (parts[0] == "emissiveTexture" && parts.size() >= 2) {
                const auto pos = line.find_first_not_of(" \t", parts[0].size());
                loaded.emissiveTexture.path = (pos != std::string::npos) ? Trim(line.substr(pos)) : parts[1];
            }
            else if (parts[0] == "float" && parts.size() >= 3)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Float; p.value.f[0] = std::stof(parts[2]); loaded.params.push_back(p);
            }
            else if (parts[0] == "vec2" && parts.size() >= 4)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Vec2;
                p.value.f[0] = std::stof(parts[2]); p.value.f[1] = std::stof(parts[3]);
                loaded.params.push_back(p);
            }
            else if (parts[0] == "vec3" && parts.size() >= 5)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Vec3;
                p.value.f[0] = std::stof(parts[2]); p.value.f[1] = std::stof(parts[3]); p.value.f[2] = std::stof(parts[4]);
                loaded.params.push_back(p);
            }
            else if (parts[0] == "vec4" && parts.size() >= 6)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Vec4;
                p.value.f[0] = std::stof(parts[2]); p.value.f[1] = std::stof(parts[3]); p.value.f[2] = std::stof(parts[4]); p.value.f[3] = std::stof(parts[5]);
                loaded.params.push_back(p);
            }
            else if (parts[0] == "int" && parts.size() >= 3)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Int; p.value.i = static_cast<int32_t>(std::stoi(parts[2])); loaded.params.push_back(p);
            }
            else if (parts[0] == "bool" && parts.size() >= 3)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Bool; p.value.b = (parts[2] == "1" || parts[2] == "true"); loaded.params.push_back(p);
            }
            else if (parts[0] == "texture" && parts.size() >= 3)
            {
                MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Texture; p.texturePath = parts[2]; p.texture = LoadTexture(parts[2]); loaded.params.push_back(p);
            }
        }
        ResolveTextureRef(loaded.baseColorTexture,
            ColorSpace::SRGB,   TextureSemantic::Color,  NormalEncoding::None);
        ResolveTextureRef(loaded.emissiveTexture,
            ColorSpace::SRGB,   TextureSemantic::Color,  NormalEncoding::None);
        ResolveTextureRef(loaded.metallicRoughnessTexture,
            ColorSpace::Linear, TextureSemantic::Data,   NormalEncoding::None);
        ResolveTextureRef(loaded.occlusionTexture,
            ColorSpace::Linear, TextureSemantic::Data,   NormalEncoding::None);
        ResolveTextureRef(loaded.normalTexture,
            ColorSpace::Linear, TextureSemantic::Normal, NormalEncoding::RGB);
        loaded.gpuStatus.dirty = true;
        loaded.gpuStatus.uploaded = false;
        *mat = std::move(loaded);
        return true;
    }

    bool AssetPipeline::LoadScene(const std::string& path, Scene& scene)
    {
        const auto resolved = Resolve(path).string();
        std::string source;
        if (!m_fs->ReadText(resolved.c_str(), source)) return false;

        std::unordered_map<std::string, EntityID> entities;
        std::istringstream in(source);
        std::string line;
        EntityID current = NULL_ENTITY;
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto parts = SplitWs(line);
            if (parts.empty()) continue;
            if (parts[0] == "entity" && parts.size() >= 2)
            {
                current = scene.CreateEntity(parts[1]);
                entities[parts[1]] = current;
            }
            else if (parts[0] == "position" && parts.size() >= 4 && current.IsValid())
            {
                scene.SetLocalPosition(current, Vec3{ std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3]) });
            }
            else if (parts[0] == "scale" && parts.size() >= 4 && current.IsValid())
            {
                scene.SetLocalScale(current, Vec3{ std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3]) });
            }
            else if (current.IsValid() && !m_sceneDirectiveHandlers.empty())
            {
                const SceneDirectiveContext context{ scene.GetWorld(), current, *this };
                bool handled = false;
                for (const SceneDirectiveHandler& handler : m_sceneDirectiveHandlers)
                {
                    if (handler && handler(parts[0], parts, context))
                    {
                        handled = true;
                        break;
                    }
                }
                if (handled)
                    continue;
            }
            else if (parts[0] == "parent" && parts.size() >= 2 && current.IsValid())
            {
                auto it = entities.find(parts[1]);
                if (it != entities.end()) scene.SetParent(current, it->second);
            }
        }
        scene.PropagateTransforms();
        return true;
    }

    void AssetPipeline::PollHotReload()
    {
        m_registry.meshes.ForEach([&](MeshHandle h, MeshAsset& a) {
            const auto path = Resolve(a.path);
            const auto stats = m_fs->GetFileStats(path.string().c_str());
            if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
                ReloadMesh(h, path);
            });
        m_registry.textures.ForEach([&](TextureHandle h, TextureAsset& a) {
            const auto path = Resolve(a.path);
            const auto stats = m_fs->GetFileStats(path.string().c_str());
            if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
                ReloadTexture(h, path);
            });
        m_registry.shaders.ForEach([&](ShaderHandle h, ShaderAsset& a) {
            const auto path = Resolve(a.path);
            const auto stats = m_fs->GetFileStats(path.string().c_str());
            if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
                ReloadShader(h, path, a.stage);
            });
        m_registry.materials.ForEach([&](MaterialHandle h, MaterialAsset& a) {
            const auto path = Resolve(a.path);
            const auto stats = m_fs->GetFileStats(path.string().c_str());
            if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
                ReloadMaterial(h, path);
            });
    }

    bool AssetPipeline::BuildShaderCache(ShaderHandle handle, ShaderTargetProfile target)
    {
        auto* shader = m_registry.shaders.Get(handle);
        if (!shader || shader->state != AssetState::Loaded)
            return false;

        CompiledShaderArtifact compiled{};
        std::string error;
        if (!renderer::ShaderCompiler::CompileForTarget(*shader, target, compiled, &error))
        {
            shader->state = AssetState::Failed;
            Debug::LogError("AssetPipeline.cpp: failed to compile shader cache '%s' for %s: %s",
                shader->debugName.c_str(), renderer::ShaderCompiler::ToString(target), error.c_str());
            return false;
        }

        auto it = std::find_if(shader->compiledArtifacts.begin(), shader->compiledArtifacts.end(), [&](const CompiledShaderArtifact& existing) {
            return existing.target == target && existing.stage == shader->stage && existing.entryPoint == compiled.entryPoint;
            });
        if (it != shader->compiledArtifacts.end())
            *it = std::move(compiled);
        else
            shader->compiledArtifacts.push_back(std::move(compiled));
        return true;
    }

    void AssetPipeline::BuildPendingShaderCaches()
    {
        if (!m_device) return;
        const auto target = renderer::ShaderCompiler::ResolveTargetProfile(*m_device);

        std::vector<ShaderHandle> pending;
        m_registry.shaders.ForEach([&](ShaderHandle h, ShaderAsset& a) {
            if (a.state != AssetState::Loaded) return;
            const bool hasCache = std::any_of(a.compiledArtifacts.begin(), a.compiledArtifacts.end(),
                [&](const CompiledShaderArtifact& artifact) {
                    return artifact.target == target && artifact.IsValid();
                });
            if (!hasCache || a.gpuStatus.dirty)
                pending.push_back(h);
        });

        if (pending.empty()) return;

        std::vector<std::future<void>> futures;
        futures.reserve(pending.size());
        for (ShaderHandle h : pending)
            futures.push_back(std::async(std::launch::async,
                [this, h, target]() { BuildShaderCache(h, target); }));

        for (auto& f : futures)
            f.get();
    }

    void AssetPipeline::UploadPendingGpuAssets()
    {
        if (!m_device) return;
        BuildPendingShaderCaches();
        m_registry.textures.ForEach([&](TextureHandle h, TextureAsset& a) {
            if (a.state != AssetState::Loaded || (!a.gpuStatus.dirty && a.gpuStatus.uploaded)) return;
            TextureDesc td{};
            td.width = a.width; td.height = a.height; td.depth = a.depth; td.mipLevels = a.mipLevels;
            td.arraySize = a.isCubemap ? 6u : a.arraySize;
            td.dimension = a.isCubemap ? TextureDimension::Cubemap : TextureDimension::Tex2D;
            td.format = ToFormat(a.format, IsSRGBColorSpace(a.metadata)); td.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest; td.initialState = ResourceState::ShaderRead; td.debugName = a.debugName;
            if (td.format == Format::Unknown)
            {
                Debug::LogError("AssetPipeline.cpp: texture '%s' uses unsupported GPU format=%u",
                    a.debugName.c_str(), static_cast<unsigned>(a.format));
                a.gpuStatus.uploaded = false;
                a.gpuStatus.dirty = true;
                return;
            }
            if (!m_device->SupportsTextureFormat(td.format, ResourceUsage::ShaderResource))
            {
                Debug::LogError("AssetPipeline.cpp: texture '%s' requests unsupported sampled format=%u on backend '%s'",
                    a.debugName.c_str(), static_cast<unsigned>(td.format), m_device->GetBackendName());
                if (a.metadata.normalEncoding == NormalEncoding::BC5_XY)
                {
                    Debug::LogError("AssetPipeline.cpp: BC5 normal map fallback is unavailable because no RGB source payload exists in this runtime path");
                }
                a.gpuStatus.uploaded = false;
                a.gpuStatus.dirty = true;
                return;
            }
            auto it = m_gpuTextures.find(h);
            if (it != m_gpuTextures.end() && it->second.IsValid() && a.gpuStatus.dirty)
            {
                // Nicht sofort zerstören: Material-Instanzen/Descriptoren können
                // den alten GPU-TextureHandle noch für bereits aufgebaute Frames
                // referenzieren. Sofortiges Destroy/Recreate erzeugt stale Handles
                // und kann Vulkan in VK_ERROR_DEVICE_LOST treiben. Der Asset-Handle
                // zeigt ab jetzt auf die neue GPU-Textur; alte Handles werden erst
                // beim AssetPipeline-Shutdown freigegeben.
                m_retiredGpuTextures.push_back(it->second);
                it->second = {};
            }
            if (it == m_gpuTextures.end())
                it = m_gpuTextures.emplace(h, m_device->CreateTexture(td)).first;
            else if (!it->second.IsValid())
                it->second = m_device->CreateTexture(td);

            const size_t expectedBytes = ComputeTotalTextureByteSize(a);
            if (expectedBytes == 0u)
            {
                Debug::LogError("AssetPipeline.cpp: texture '%s' uses unsupported upload format=%u for mip upload",
                    a.debugName.c_str(), static_cast<unsigned>(a.format));
                a.gpuStatus.uploaded = false;
                a.gpuStatus.dirty = true;
                return;
            }
            if (a.pixelData.size() != expectedBytes)
            {
                Debug::LogError("AssetPipeline.cpp: texture '%s' byte size mismatch for %u mip(s), %u layer(s): have=%zu expected=%zu",
                    a.debugName.c_str(), a.mipLevels, a.arraySize, a.pixelData.size(), expectedBytes);
                a.gpuStatus.uploaded = false;
                a.gpuStatus.dirty = true;
                return;
            }

            size_t byteOffset = 0u;
            for (uint32_t arrayLayer = 0u; arrayLayer < std::max(1u, a.arraySize); ++arrayLayer)
            {
                for (uint32_t mip = 0u; mip < std::max(1u, a.mipLevels); ++mip)
                {
                    const uint32_t mipWidth = std::max(1u, a.width >> mip);
                    const uint32_t mipHeight = std::max(1u, a.height >> mip);
                    const uint32_t mipDepth = std::max(1u, a.depth >> mip);
                    const auto layout = ComputeTextureUploadLayout(a.format, mipWidth, mipHeight, mipDepth);
                    const size_t mipByteSize = static_cast<size_t>(layout.byteSize);
                    if (mipByteSize == 0u || (byteOffset + mipByteSize) > a.pixelData.size())
                    {
                        Debug::LogError("AssetPipeline.cpp: texture '%s' computed invalid mip upload layout at mip=%u layer=%u",
                            a.debugName.c_str(), mip, arrayLayer);
                        a.gpuStatus.uploaded = false;
                        a.gpuStatus.dirty = true;
                        return;
                    }

                    m_device->UploadTextureData(it->second, a.pixelData.data() + byteOffset, mipByteSize, mip, arrayLayer);
                    byteOffset += mipByteSize;
                }
            }

            a.gpuStatus.uploaded = true;
            a.gpuStatus.dirty = false;
            });

        m_registry.shaders.ForEach([&](ShaderHandle h, ShaderAsset& a) {
            if (a.state != AssetState::Loaded || (!a.gpuStatus.dirty && a.gpuStatus.uploaded)) return;
            auto it = m_gpuShaders.find(h);
            if (it != m_gpuShaders.end() && it->second.IsValid()) m_device->DestroyShader(it->second);

            ShaderHandle gpu = ShaderHandle::Invalid();
            const auto target = renderer::ShaderCompiler::ResolveTargetProfile(*m_device);
            auto compiledIt = std::find_if(a.compiledArtifacts.begin(), a.compiledArtifacts.end(), [&](const CompiledShaderArtifact& artifact) {
                return artifact.target == target && artifact.IsValid();
                });
            if (compiledIt != a.compiledArtifacts.end())
            {
                if (!compiledIt->bytecode.empty())
                {
                    gpu = m_device->CreateShaderFromBytecode(compiledIt->bytecode.data(), compiledIt->bytecode.size(), ToStageMask(a.stage), a.debugName);
                }
                else if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                {
                    Debug::LogError("AssetPipeline.cpp: Vulkan requires SPIR-V bytecode for compiled shader '%s'", a.debugName.c_str());
                }
                else
                {
                    gpu = m_device->CreateShaderFromSource(compiledIt->sourceText, ToStageMask(a.stage), compiledIt->entryPoint, a.debugName);
                }
            }
            else
            {
                if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
                {
                    Debug::LogError("AssetPipeline.cpp: no compiled Vulkan_SPIRV artifact available for shader '%s'", a.debugName.c_str());
                }
                else
                {
                    gpu = a.bytecode.empty()
                        ? m_device->CreateShaderFromSource(a.sourceCode, ToStageMask(a.stage), a.entryPoint, a.debugName)
                        : m_device->CreateShaderFromBytecode(a.bytecode.data(), a.bytecode.size(), ToStageMask(a.stage), a.debugName);
                }
            }
            m_gpuShaders[h] = gpu;
            a.gpuStatus.uploaded = gpu.IsValid();
            a.gpuStatus.dirty = !gpu.IsValid();
            });

        m_registry.materials.ForEach([&](MaterialHandle, MaterialAsset& a) {
            if (a.state != AssetState::Loaded) return;
            a.gpuStatus.uploaded = true;
            a.gpuStatus.dirty = false;
            });
    }

    TextureHandle AssetPipeline::GetGpuTexture(TextureHandle handle) const noexcept
    {
        auto it = m_gpuTextures.find(handle);
        return it != m_gpuTextures.end() ? it->second : TextureHandle{};
    }

    ShaderHandle AssetPipeline::GetGpuShader(ShaderHandle handle) const noexcept
    {
        auto it = m_gpuShaders.find(handle);
        return it != m_gpuShaders.end() ? it->second : ShaderHandle{};
    }

    void AssetPipeline::ResolveTextureRef(MaterialTextureRef& ref,
                                          ColorSpace colorSpace,
                                          TextureSemantic semantic,
                                          NormalEncoding normalEncoding)
    {
        if (ref.path.empty()) return;

        ref.texture = LoadTexture(ref.path);
        if (!ref.texture.IsValid()) return;

        auto* tex = m_registry.textures.Get(ref.texture);
        if (!tex) return;

        tex->metadata.colorSpace     = colorSpace;
        tex->metadata.semantic       = semantic;
        tex->metadata.normalEncoding = normalEncoding;
        tex->gpuStatus.dirty         = true;
        tex->gpuStatus.uploaded      = false;
    }

}
