#include "core/Debug.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderRuntime.hpp"
#include <array>
#include <cstdio>
#include <memory>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

TextureHandle CreateSolidTexture(IDevice& device,
                                 uint8_t r,
                                 uint8_t g,
                                 uint8_t b,
                                 uint8_t a,
                                 const char* debugName)
{
    TextureDesc td{};
    td.width = 1u;
    td.height = 1u;
    td.format = Format::RGBA8_UNORM;
    td.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
    td.debugName = debugName;

    const TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
    {
        std::fprintf(stderr, "CreateTexture failed for %s\n", debugName ? debugName : "<unnamed>");
        return TextureHandle::Invalid();
    }

    const std::array<uint8_t, 4> pixel = {r, g, b, a};
    device.UploadTextureData(tex, pixel.data(), pixel.size());
    return tex;
}

const char* BoolText(bool v) { return v ? "yes" : "no"; }

bool HasMaterialFlag(MaterialFeatureFlag flags, MaterialFeatureFlag bit)
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0u;
}

void PrintFeatureFlags(MaterialFeatureFlag flags)
{
    struct Entry { MaterialFeatureFlag bit; const char* name; };
    static constexpr Entry kEntries[] = {
        {MaterialFeatureFlag::PBRMetalRough, "PBRMetalRough"},
        {MaterialFeatureFlag::Unlit, "Unlit"},
        {MaterialFeatureFlag::BaseColorValue, "BaseColorValue"},
        {MaterialFeatureFlag::BaseColorTexture, "BaseColorTexture"},
        {MaterialFeatureFlag::NormalTexture, "NormalTexture"},
        {MaterialFeatureFlag::MetallicValue, "MetallicValue"},
        {MaterialFeatureFlag::MetallicTexture, "MetallicTexture"},
        {MaterialFeatureFlag::RoughnessValue, "RoughnessValue"},
        {MaterialFeatureFlag::RoughnessTexture, "RoughnessTexture"},
        {MaterialFeatureFlag::OcclusionValue, "OcclusionValue"},
        {MaterialFeatureFlag::OcclusionTexture, "OcclusionTexture"},
        {MaterialFeatureFlag::EmissiveValue, "EmissiveValue"},
        {MaterialFeatureFlag::EmissiveTexture, "EmissiveTexture"},
        {MaterialFeatureFlag::OpacityValue, "OpacityValue"},
        {MaterialFeatureFlag::OpacityTexture, "OpacityTexture"},
        {MaterialFeatureFlag::AlphaTest, "AlphaTest"},
        {MaterialFeatureFlag::DoubleSided, "DoubleSided"},
        {MaterialFeatureFlag::CastShadows, "CastShadows"},
        {MaterialFeatureFlag::ReceiveShadows, "ReceiveShadows"},
    };

    bool first = true;
    for (const auto& entry : kEntries)
    {
        if (!HasMaterialFlag(flags, entry.bit))
            continue;
        std::printf("%s%s", first ? "" : ", ", entry.name);
        first = false;
    }
    if (first)
        std::printf("None");
}

bool PrintValidationIssues(ShaderRuntime& shaderRuntime,
                           MaterialSystem& materials,
                           MaterialHandle material,
                           const char* label)
{
    std::vector<ShaderValidationIssue> issues;
    const bool valid = shaderRuntime.ValidateMaterial(materials, material, issues);
    if (issues.empty())
        return valid;

    std::fprintf(stderr, "Material '%s' validation issues:\n", label ? label : "<unnamed>");
    for (const auto& issue : issues)
    {
        std::fprintf(stderr, "  - [%s] %s\n",
                     issue.severity == ShaderValidationIssue::Severity::Error ? "error" : "warning",
                     issue.message.c_str());
    }
    return valid;
}

bool RequireMaterialState(ShaderRuntime& shaderRuntime,
                          MaterialSystem& materials,
                          MaterialHandle material,
                          const char* label,
                          const MaterialGpuState*& outState)
{
    outState = shaderRuntime.GetMaterialState(material);
    if (outState)
        return true;

    std::fprintf(stderr, "No MaterialGpuState for '%s'. Trying individual prepare...\n", label ? label : "<unnamed>");
    PrintValidationIssues(shaderRuntime, materials, material, label);
    if (!shaderRuntime.PrepareMaterial(materials, material))
    {
        std::fprintf(stderr, "PrepareMaterial failed for '%s'.\n", label ? label : "<unnamed>");
        outState = shaderRuntime.GetMaterialState(material);
        if (outState && !outState->issues.empty())
        {
            for (const auto& issue : outState->issues)
            {
                std::fprintf(stderr, "  - runtime [%s] %s\n",
                             issue.severity == ShaderValidationIssue::Severity::Error ? "error" : "warning",
                             issue.message.c_str());
            }
        }
        return false;
    }

    outState = shaderRuntime.GetMaterialState(material);
    if (!outState)
    {
        std::fprintf(stderr, "PrepareMaterial returned success but state is still missing for '%s'.\n", label ? label : "<unnamed>");
        return false;
    }

    if (!outState->valid)
    {
        std::fprintf(stderr, "MaterialGpuState exists but is invalid for '%s'.\n", label ? label : "<unnamed>");
        for (const auto& issue : outState->issues)
        {
            std::fprintf(stderr, "  - runtime [%s] %s\n",
                         issue.severity == ShaderValidationIssue::Severity::Error ? "error" : "warning",
                         issue.message.c_str());
        }
        return false;
    }

    return true;
}

TextureHandle FindBoundTexture(const MaterialGpuState& state, const char* bindingName)
{
    for (const auto& binding : state.bindings)
    {
        if (binding.kind == ResolvedMaterialBinding::Kind::Texture && binding.name == bindingName)
            return binding.texture;
    }
    return TextureHandle::Invalid();
}

bool PrintMaterialSummary(const char* label,
                          MaterialSystem& materials,
                          ShaderRuntime& shaderRuntime,
                          MaterialHandle material,
                          TextureHandle expectedBaseColor,
                          TextureHandle expectedNormal,
                          TextureHandle expectedEmissive,
                          TextureHandle expectedOrm)
{
    const MaterialDesc* desc = materials.GetDesc(material);
    const MaterialInstance* inst = materials.GetInstance(material);
    const MaterialGpuState* state = nullptr;

    if (!desc || !inst)
    {
        std::fprintf(stderr, "Invalid material handle for '%s'.\n", label ? label : "<unnamed>");
        return false;
    }

    if (!RequireMaterialState(shaderRuntime, materials, material, label, state))
        return false;

    std::printf("\n=== %s ===\n", label);
    std::printf("model: %s\n", desc->model == MaterialModel::Unlit ? "Unlit" : "PBRMetalRough");
    std::printf("feature flags: ");
    PrintFeatureFlags(materials.GetFeatureFlags(material));
    std::printf("\n");

    const auto dumpSemantic = [&](MaterialSemantic semantic)
    {
        const auto value = materials.GetSemanticValue(material, semantic);
        const auto tex = materials.GetSemanticTexture(material, semantic);
        std::printf("  %-12s value=%s texture=%s\n",
                    MaterialSystem::SemanticName(semantic),
                    BoolText(value.set),
                    BoolText(tex.IsValid()));
    };

    dumpSemantic(MaterialSemantic::BaseColor);
    dumpSemantic(MaterialSemantic::Normal);
    dumpSemantic(MaterialSemantic::Metallic);
    dumpSemantic(MaterialSemantic::Roughness);
    dumpSemantic(MaterialSemantic::Occlusion);
    dumpSemantic(MaterialSemantic::Emissive);
    dumpSemantic(MaterialSemantic::Opacity);
    dumpSemantic(MaterialSemantic::AlphaCutoff);

    const TextureHandle baseColorBound = FindBoundTexture(*state, "BaseColor");
    const TextureHandle normalBound = FindBoundTexture(*state, "Normal");
    const TextureHandle emissiveBound = FindBoundTexture(*state, "Emissive");
    const TextureHandle ormBound = FindBoundTexture(*state, "ORM");

    for (const auto& binding : state->bindings)
    {
        if (binding.kind != ResolvedMaterialBinding::Kind::Texture)
            continue;
        std::printf("  binding %-10s slot=%u handle=%u\n",
                    binding.name.c_str(),
                    binding.slot,
                    binding.texture.value);
    }

    const bool okBase = baseColorBound == expectedBaseColor;
    const bool okNormal = normalBound == expectedNormal;
    const bool okEmissive = emissiveBound == expectedEmissive;
    const bool okOrm = ormBound == expectedOrm;

    std::printf("  checks: BaseColor=%s Normal=%s Emissive=%s ORM=%s\n",
                okBase ? "OK" : "FAIL",
                okNormal ? "OK" : "FAIL",
                okEmissive ? "OK" : "FAIL",
                okOrm ? "OK" : "FAIL");

    if (!(okBase && okNormal && okEmissive && okOrm))
    {
        std::fprintf(stderr, "Binding mismatch for '%s'.\n", label ? label : "<unnamed>");
        return false;
    }

    return true;
}

} // namespace

int main()
{
    engine::Debug::ResetMinLevelForBuild();

    DeviceFactory::Registry deviceFactoryRegistry;
    std::unique_ptr<IDevice> device = deviceFactoryRegistry.Create(DeviceFactory::BackendType::Null);
    if (!device)
    {
        std::fprintf(stderr, "Failed to create null device.\n");
        return 1;
    }

    IDevice::DeviceDesc deviceDesc{};
    deviceDesc.enableDebugLayer = true;
    deviceDesc.appName = "material_semantic_demo";
    if (!device->Initialize(deviceDesc))
    {
        std::fprintf(stderr, "Failed to initialize null device.\n");
        return 1;
    }

    const ShaderHandle vs = device->CreateShaderFromSource("void VSMain() {}", ShaderStageMask::Vertex, "VSMain", "DemoVS");
    const ShaderHandle ps = device->CreateShaderFromSource("void PSMain() {}", ShaderStageMask::Fragment, "PSMain", "DemoPS");
    if (!vs.IsValid() || !ps.IsValid())
    {
        std::fprintf(stderr, "Failed to create demo shaders. VS=%u PS=%u\n", vs.value, ps.value);
        device->Shutdown();
        return 1;
    }

    MaterialSystem materials;

    MaterialDesc baseDesc{};
    baseDesc.vertexShader = vs;
    baseDesc.fragmentShader = ps;
    baseDesc.name = "Base";

    const TextureHandle customBaseColor = CreateSolidTexture(*device, 255u, 0u, 0u, 255u, "CustomBaseColor");
    const TextureHandle customNormal = CreateSolidTexture(*device, 128u, 128u, 255u, 255u, "CustomNormal");
    const TextureHandle customOrm = CreateSolidTexture(*device, 16u, 200u, 64u, 255u, "CustomORM");
    const TextureHandle customEmissive = CreateSolidTexture(*device, 0u, 32u, 255u, 255u, "CustomEmissive");
    if (!customBaseColor.IsValid() || !customNormal.IsValid() || !customOrm.IsValid() || !customEmissive.IsValid())
    {
        std::fprintf(stderr, "Failed to create one or more demo textures.\n");
        device->Shutdown();
        return 1;
    }

    auto makeMaterial = [&](const char* name) {
        MaterialDesc d = baseDesc;
        d.name = name;
        return materials.RegisterMaterial(std::move(d));
    };

    const MaterialHandle onlyAlbedo = makeMaterial("OnlyAlbedo");
    materials.SetSemanticVec4(onlyAlbedo, MaterialSemantic::BaseColor, {1.f, 0.2f, 0.2f, 1.f});

    const MaterialHandle onlyEmissive = makeMaterial("OnlyEmissive");
    materials.SetSemanticVec4(onlyEmissive, MaterialSemantic::Emissive, {0.f, 0.f, 3.f, 1.f});

    const MaterialHandle albedoNormal = makeMaterial("AlbedoNormal");
    materials.SetSemanticTexture(albedoNormal, MaterialSemantic::BaseColor, customBaseColor);
    materials.SetSemanticTexture(albedoNormal, MaterialSemantic::Normal, customNormal);

    const MaterialHandle albedoMetalRough = makeMaterial("AlbedoMetalRough");
    materials.SetSemanticTexture(albedoMetalRough, MaterialSemantic::BaseColor, customBaseColor);
    materials.SetSemanticFloat(albedoMetalRough, MaterialSemantic::Metallic, 1.f);
    materials.SetSemanticFloat(albedoMetalRough, MaterialSemantic::Roughness, 0.35f);

    const MaterialHandle fullPbr = makeMaterial("FullPBR");
    materials.SetSemanticTexture(fullPbr, MaterialSemantic::BaseColor, customBaseColor);
    materials.SetSemanticTexture(fullPbr, MaterialSemantic::Normal, customNormal);
    materials.SetSemanticTexture(fullPbr, MaterialSemantic::Metallic, customOrm);
    materials.SetSemanticTexture(fullPbr, MaterialSemantic::Emissive, customEmissive);
    materials.SetSemanticFloat(fullPbr, MaterialSemantic::Roughness, 0.2f);
    materials.SetSemanticFloat(fullPbr, MaterialSemantic::Occlusion, 1.f);

    MaterialDesc unlitDesc = baseDesc;
    unlitDesc.name = "UnlitOnlyBaseColor";
    unlitDesc.model = MaterialModel::Unlit;
    const MaterialHandle unlit = materials.RegisterMaterial(std::move(unlitDesc));
    materials.SetSemanticTexture(unlit, MaterialSemantic::BaseColor, customBaseColor);

    ShaderRuntime shaderRuntime;
    if (!shaderRuntime.Initialize(*device))
    {
        std::fprintf(stderr, "ShaderRuntime initialization failed.\n");
        device->Shutdown();
        return 1;
    }

    const bool preparedAll = shaderRuntime.PrepareAllMaterials(materials);
    if (!preparedAll)
        std::fprintf(stderr, "PrepareAllMaterials reported failure. Individual materials will be diagnosed below.\n");

    const MaterialGpuState* firstState = nullptr;
    if (!RequireMaterialState(shaderRuntime, materials, onlyAlbedo, "Only Albedo value", firstState))
    {
        shaderRuntime.Shutdown();
        device->WaitIdle();
        device->Shutdown();
        return 1;
    }

    const TextureHandle fallbackWhite = FindBoundTexture(*firstState, "BaseColor");
    const TextureHandle fallbackBlack = FindBoundTexture(*firstState, "Emissive");
    const TextureHandle fallbackNormal = FindBoundTexture(*firstState, "Normal");
    const TextureHandle fallbackGray = FindBoundTexture(*firstState, "ORM");

    bool ok = true;
    ok = PrintMaterialSummary("Only Albedo value", materials, shaderRuntime, onlyAlbedo,
                              fallbackWhite, fallbackNormal, fallbackBlack, fallbackGray) && ok;
    ok = PrintMaterialSummary("Only Emissive value", materials, shaderRuntime, onlyEmissive,
                              fallbackWhite, fallbackNormal, fallbackBlack, fallbackGray) && ok;
    ok = PrintMaterialSummary("Albedo + Normal textures", materials, shaderRuntime, albedoNormal,
                              customBaseColor, customNormal, fallbackBlack, fallbackGray) && ok;
    ok = PrintMaterialSummary("Albedo + Metallic/Roughness", materials, shaderRuntime, albedoMetalRough,
                              customBaseColor, fallbackNormal, fallbackBlack, fallbackGray) && ok;
    ok = PrintMaterialSummary("Full PBR", materials, shaderRuntime, fullPbr,
                              customBaseColor, customNormal, customEmissive, customOrm) && ok;
    ok = PrintMaterialSummary("Unlit", materials, shaderRuntime, unlit,
                              customBaseColor, fallbackNormal, fallbackBlack, fallbackGray) && ok;

    std::printf("\nResult: %s\n", ok ? "semantic materials and engine fallbacks are wired." : "FAILED - see stderr diagnostics.");

    shaderRuntime.Shutdown();
    device->WaitIdle();
    device->Shutdown();
    return ok ? 0 : 1;
}
