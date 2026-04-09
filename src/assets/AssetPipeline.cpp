#include "assets/AssetPipeline.hpp"
#include "core/Debug.hpp"
#include <sstream>
#include <algorithm>
#include <cstring>

// stb_image - einmalig in dieser TU, nie im Header
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb_image.h"

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

// ---------------------------------------------------------------------------
// Texture helpers
// ---------------------------------------------------------------------------
static std::string ToLower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Heuristik: Dateien mit diesen Namen-Fragmenten sind Daten-Texturen (linear, kein sRGB)
static bool InferSRGB(const std::filesystem::path& path)
{
    const std::string lower = ToLower(path.filename().string());
    return lower.find("normal") == std::string::npos
        && lower.find("rough")  == std::string::npos
        && lower.find("metal")  == std::string::npos
        && lower.find("ao")     == std::string::npos
        && lower.find("mask")   == std::string::npos
        && lower.find("depth")  == std::string::npos;
}

static ShaderStage InferShaderStage(const fs::path& path, ShaderStage fallback)
{
    const auto ext = path.extension().string();
    if (ext == ".vert" || ext == ".vs" || ext == ".hlslvs") return ShaderStage::Vertex;
    if (ext == ".frag" || ext == ".fs" || ext == ".ps" || ext == ".hlslps") return ShaderStage::Fragment;
    if (ext == ".comp" || ext == ".cs") return ShaderStage::Compute;
    return fallback;
}

static ShaderSourceLanguage InferLanguageFromExt(const fs::path& path)
{
    const auto ext = path.extension().string();
    if (ext == ".hlsl" || ext == ".hlslvs" || ext == ".hlslps" || ext == ".vs" || ext == ".ps" || ext == ".cs")
        return ShaderSourceLanguage::HLSL;
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".comp")
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
    : m_registry(registry), m_device(device), m_fs(fs ? fs : &m_ownedFs) {}

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
    ReloadTexture(h, Resolve(path));
    return h;
}

ShaderHandle AssetPipeline::LoadShader(const std::string& path, ShaderStage fallbackStage)
{
    auto asset = std::make_unique<ShaderAsset>();
    const ShaderHandle h = m_registry.GetOrAddShader(path, std::move(asset));
    ReloadShader(h, Resolve(path), fallbackStage);
    return h;
}

MaterialHandle AssetPipeline::LoadMaterial(const std::string& path)
{
    auto asset = std::make_unique<MaterialAsset>();
    const MaterialHandle h = m_registry.GetOrAddMaterial(path, std::move(asset));
    ReloadMaterial(h, Resolve(path));
    return h;
}

bool AssetPipeline::ReloadMesh(MeshHandle handle, const fs::path& path)
{
    auto* mesh = m_registry.meshes.Get(handle);
    if (!mesh) return false;

    std::string source;
    if (!m_fs->ReadText(path.string().c_str(), source))
        { mesh->state = AssetState::Failed; return false; }

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
        else if (parts[0] == "i" && parts.size() >= 4)
        {
            sm.indices.push_back(static_cast<uint32_t>(std::stoul(parts[1])));
            sm.indices.push_back(static_cast<uint32_t>(std::stoul(parts[2])));
            sm.indices.push_back(static_cast<uint32_t>(std::stoul(parts[3])));
        }
    }
    if (sm.positions.empty()) { mesh->state = AssetState::Failed; return false; }
    loaded.submeshes = { std::move(sm) };
    loaded.gpuStatus.dirty = true;
    loaded.gpuStatus.uploaded = false;
    *mesh = std::move(loaded);
    m_registry.NotifyMeshReloaded(handle);
    return true;
}

bool AssetPipeline::ReloadTexture(TextureHandle handle, const fs::path& path)
{
    auto* tex = m_registry.textures.Get(handle);
    if (!tex) return false;

    // Binär über IFilesystem einlesen (testbar, hot-reload-fähig, pak-dateien-kompatibel)
    std::vector<uint8_t> fileData;
    if (!m_fs->ReadFile(path.string().c_str(), fileData))
    {
        Debug::LogError("AssetPipeline: failed to read texture file '%s'",
                        path.string().c_str());
        tex->state = AssetState::Failed;
        return false;
    }

    // UV-Konvention: Y=0 ist oben (DX-style, konsistent über alle Backends).
    // Kein Flip hier - OpenGL-Backend kompensiert im Sampler oder via UV im Shader.
    // stbi_set_flip_vertically_on_load(0); // explizit: kein Flip

    int w = 0, h = 0, srcChannels = 0;
    uint8_t* pixels = stbi_load_from_memory(
        fileData.data(),
        static_cast<int>(fileData.size()),
        &w, &h, &srcChannels,
        4); // 4 = RGBA forcieren - einheitlicher GPU-Pfad, kein Format-Chaos

    if (!pixels || w <= 0 || h <= 0)
    {
        if (pixels) stbi_image_free(pixels);
        Debug::LogError("AssetPipeline: stb_image decode failed for '%s' (src channels: %d)",
                        path.filename().string().c_str(), srcChannels);
        tex->state = AssetState::Failed;
        return false;
    }

    const size_t pixelBytes = static_cast<size_t>(w)
                            * static_cast<size_t>(h) * 4u;

    TextureAsset loaded;
    loaded.path        = tex->path;
    loaded.debugName   = path.filename().string();
    loaded.state       = AssetState::Loaded;
    loaded.width       = static_cast<uint32_t>(w);
    loaded.height      = static_cast<uint32_t>(h);
    loaded.format      = TextureFormat::RGBA8_UNORM;
    loaded.sRGB        = InferSRGB(path); // Dateiname-Heuristik; später durch Import-Metadaten ersetzen
    loaded.pixelData.assign(pixels, pixels + pixelBytes);
    stbi_image_free(pixels);

    const auto stats = m_fs->GetFileStats(path.string().c_str());
    if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;
    loaded.gpuStatus.dirty    = true;
    loaded.gpuStatus.uploaded = false;

    Debug::Log("AssetPipeline: loaded texture '%s' %ux%u sRGB=%d (src channels: %d)",
               loaded.debugName.c_str(), loaded.width, loaded.height,
               loaded.sRGB ? 1 : 0, srcChannels);

    *tex = std::move(loaded);
    return true;
}

bool AssetPipeline::ReloadShader(ShaderHandle handle, const fs::path& path, ShaderStage fallbackStage)
{
    auto* shader = m_registry.shaders.Get(handle);
    if (!shader) return false;

    std::string source;
    if (!m_fs->ReadText(path.string().c_str(), source))
        { shader->state = AssetState::Failed; return false; }

    ShaderAsset loaded;
    loaded.path = shader->path;
    loaded.debugName = path.filename().string();
    loaded.state = AssetState::Loaded;
    loaded.stage = InferShaderStage(path, fallbackStage);
    loaded.sourceCode = std::move(source);
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
        { mat->state = AssetState::Failed; return false; }

    MaterialAsset loaded;
    loaded.path = mat->path;
    loaded.debugName = path.filename().string();
    loaded.state = AssetState::Loaded;
    const auto stats = m_fs->GetFileStats(path.string().c_str());
    if (stats.exists) loaded.lastModifiedTimestamp = stats.lastModifiedTimestamp;

    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto parts = SplitWs(line);
        if (parts.empty()) continue;
        if (parts[0] == "vertex" && parts.size() >= 2) loaded.vertexShader = LoadShader(parts[1], ShaderStage::Vertex);
        else if (parts[0] == "fragment" && parts.size() >= 2) loaded.fragmentShader = LoadShader(parts[1], ShaderStage::Fragment);
        else if (parts[0] == "transparent" && parts.size() >= 2) loaded.transparent = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "doubleSided" && parts.size() >= 2) loaded.doubleSided = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "castShadows" && parts.size() >= 2) loaded.castShadows = (parts[1] == "1" || parts[1] == "true");
        else if (parts[0] == "float" && parts.size() >= 3)
        {
            MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Float; p.value.f[0] = std::stof(parts[2]); loaded.params.push_back(p);
        }
        else if (parts[0] == "vec4" && parts.size() >= 6)
        {
            MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Vec4;
            p.value.f[0]=std::stof(parts[2]); p.value.f[1]=std::stof(parts[3]); p.value.f[2]=std::stof(parts[4]); p.value.f[3]=std::stof(parts[5]);
            loaded.params.push_back(p);
        }
        else if (parts[0] == "texture" && parts.size() >= 3)
        {
            MaterialParam p{}; p.name = parts[1]; p.type = MaterialParam::Type::Texture; p.texture = LoadTexture(parts[2]); loaded.params.push_back(p);
        }
    }
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
            scene.SetLocalPosition(current, Vec3{std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3])});
        }
        else if (parts[0] == "scale" && parts.size() >= 4 && current.IsValid())
        {
            scene.SetLocalScale(current, Vec3{std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3])});
        }
        else if (parts[0] == "mesh" && parts.size() >= 2 && current.IsValid())
        {
            auto mh = LoadMesh(parts[1]);
            if (!scene.GetWorld().Has<MeshComponent>(current)) scene.GetWorld().Add<MeshComponent>(current, mh);
            else scene.GetWorld().Get<MeshComponent>(current)->mesh = mh;
        }
        else if (parts[0] == "material" && parts.size() >= 2 && current.IsValid())
        {
            auto mh = LoadMaterial(parts[1]);
            if (!scene.GetWorld().Has<MaterialComponent>(current)) scene.GetWorld().Add<MaterialComponent>(current, mh);
            else scene.GetWorld().Get<MaterialComponent>(current)->material = mh;
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
    m_registry.meshes.ForEach([&](MeshHandle h, MeshAsset& a){
        const auto path = Resolve(a.path);
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
            ReloadMesh(h, path);
    });
    m_registry.textures.ForEach([&](TextureHandle h, TextureAsset& a){
        const auto path = Resolve(a.path);
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
            ReloadTexture(h, path);
    });
    m_registry.shaders.ForEach([&](ShaderHandle h, ShaderAsset& a){
        const auto path = Resolve(a.path);
        const auto stats = m_fs->GetFileStats(path.string().c_str());
        if (stats.exists && a.lastModifiedTimestamp != 0 && stats.lastModifiedTimestamp != a.lastModifiedTimestamp)
            ReloadShader(h, path, a.stage);
    });
    m_registry.materials.ForEach([&](MaterialHandle h, MaterialAsset& a){
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
    m_registry.shaders.ForEach([&](ShaderHandle h, ShaderAsset& a){
        if (a.state != AssetState::Loaded) return;
        const bool hasCache = std::any_of(a.compiledArtifacts.begin(), a.compiledArtifacts.end(), [&](const CompiledShaderArtifact& artifact) {
            return artifact.target == target && artifact.IsValid();
        });
        if (!hasCache || a.gpuStatus.dirty)
            BuildShaderCache(h, target);
    });
}

void AssetPipeline::UploadPendingGpuAssets()
{
    if (!m_device) return;
    BuildPendingShaderCaches();
    m_registry.textures.ForEach([&](TextureHandle h, TextureAsset& a){
        if (a.state != AssetState::Loaded || (!a.gpuStatus.dirty && a.gpuStatus.uploaded)) return;
        TextureDesc td{};
        td.width = a.width; td.height = a.height; td.depth = a.depth; td.mipLevels = a.mipLevels; td.arraySize = a.arraySize;
        td.format = ToFormat(a.format, a.sRGB); td.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest; td.initialState = ResourceState::ShaderRead; td.debugName = a.debugName;
        auto it = m_gpuTextures.find(h);
        if (it == m_gpuTextures.end() || !it->second.IsValid()) it = m_gpuTextures.emplace(h, m_device->CreateTexture(td)).first;
        m_device->UploadTextureData(it->second, a.pixelData.data(), a.pixelData.size(), 0u, 0u);
        a.gpuStatus.uploaded = true; a.gpuStatus.dirty = false;
    });

    m_registry.shaders.ForEach([&](ShaderHandle h, ShaderAsset& a){
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

    m_registry.materials.ForEach([&](MaterialHandle, MaterialAsset& a){
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

}
