#include "renderer/MaterialSystem.hpp"
#include "renderer/MaterialFeatureEval.hpp"
#include "renderer/MaterialShaderRequest.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/MaterialCBLayout.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <type_traits>

namespace engine::renderer {

namespace {

uint32_t HashVertexLayout(const VertexLayout& layout) noexcept
{
    static constexpr uint32_t kPrime = 0x01000193u;
    static constexpr uint32_t kOffset = 0x811C9DC5u;
    uint32_t hash = kOffset;
    for (const auto& attr : layout.attributes)
    {
        hash ^= static_cast<uint32_t>(attr.semantic); hash *= kPrime;
        hash ^= static_cast<uint32_t>(attr.format); hash *= kPrime;
        hash ^= attr.binding; hash *= kPrime;
        hash ^= attr.offset; hash *= kPrime;
    }
    for (const auto& binding : layout.bindings)
    {
        hash ^= binding.binding; hash *= kPrime;
        hash ^= binding.stride; hash *= kPrime;
        hash ^= static_cast<uint32_t>(binding.inputRate); hash *= kPrime;
    }
    return hash;
}

uint32_t Align16(uint32_t v) noexcept
{
    return (v + 15u) & ~15u;
}

uint32_t ParameterByteSize(MaterialParameterType type) noexcept
{
    switch (type)
    {
    case MaterialParameterType::Float: return 4u;
    case MaterialParameterType::Vec2: return 8u;
    case MaterialParameterType::Vec3: return 12u;
    case MaterialParameterType::Vec4: return 16u;
    case MaterialParameterType::Int: return 4u;
    case MaterialParameterType::Bool: return 4u;
    default: return 0u;
    }
}

bool IsResourceType(MaterialParameterType type) noexcept
{
    switch (type)
    {
    case MaterialParameterType::Texture2D:
    case MaterialParameterType::TextureCube:
    case MaterialParameterType::Sampler:
    case MaterialParameterType::StructuredBuffer:
        return true;
    default:
        return false;
    }
}

struct NameToSlot { std::string_view name; uint32_t slot; };

static constexpr NameToSlot kTextureSlotTable[] = {
    { "albedo",                TexSlots::Albedo         },
    { "albedoMap",             TexSlots::Albedo         },
    { "baseColorMap",          TexSlots::Albedo         },
    { "baseColor",             TexSlots::Albedo         },
    { "normal",                TexSlots::Normal         },
    { "normalMap",             TexSlots::Normal         },
    { "orm",                   TexSlots::ORM            },
    { "ormMap",                TexSlots::ORM            },
    { "metallicRoughnessMap",  TexSlots::ORM            },
    { "emissive",              TexSlots::Emissive       },
    { "emissiveMap",           TexSlots::Emissive       },
    { "shadowMap",             TexSlots::ShadowMap      },
    { "tIBLIrradiance",        TexSlots::IBLIrradiance  },
    { "tIBLPrefiltered",       TexSlots::IBLPrefiltered },
    { "tBRDFLut",              TexSlots::BRDFLUT        },
    { "brdfLut",               TexSlots::BRDFLUT        },
};

static constexpr NameToSlot kSamplerSlotTable[] = {
    { "sLinear",              SamplerSlots::LinearWrap  },
    { "sLinearWrap",          SamplerSlots::LinearWrap  },
    { "linearWrapSampler",    SamplerSlots::LinearWrap  },
    { "sClamp",               SamplerSlots::LinearClamp },
    { "sLinearClamp",         SamplerSlots::LinearClamp },
    { "linearClampSampler",   SamplerSlots::LinearClamp },
    { "linearclamp",          SamplerSlots::LinearClamp },
    { "pointClampSampler",    SamplerSlots::PointClamp  },
    { "shadowSampler",        SamplerSlots::ShadowPCF   },
    { "sShadow",              SamplerSlots::ShadowPCF   },
};

template<size_t N>
uint32_t LookupSlot(const NameToSlot (&table)[N], std::string_view name, uint32_t fallback) noexcept
{
    for (const auto& entry : table)
        if (entry.name == name) return entry.slot;
    return fallback;
}

uint32_t InferTextureSlot(std::string_view name, uint32_t fallback) noexcept
{
    return LookupSlot(kTextureSlotTable, name, fallback);
}

uint32_t InferSamplerSlot(std::string_view name, uint32_t fallback) noexcept
{
    return LookupSlot(kSamplerSlotTable, name, fallback);
}

MaterialParameterType ToLayoutType(MaterialParam::Type type) noexcept
{
    switch (type)
    {
    case MaterialParam::Type::Float: return MaterialParameterType::Float;
    case MaterialParam::Type::Vec2: return MaterialParameterType::Vec2;
    case MaterialParam::Type::Vec3: return MaterialParameterType::Vec3;
    case MaterialParam::Type::Vec4: return MaterialParameterType::Vec4;
    case MaterialParam::Type::Int: return MaterialParameterType::Int;
    case MaterialParam::Type::Bool: return MaterialParameterType::Bool;
    case MaterialParam::Type::Texture: return MaterialParameterType::Texture2D;
    case MaterialParam::Type::Sampler: return MaterialParameterType::Sampler;
    case MaterialParam::Type::Buffer: return MaterialParameterType::StructuredBuffer;
    }
    return MaterialParameterType::Unknown;
}

void HashMix(uint64_t& hash, uint64_t value) noexcept
{
    hash ^= value;
    hash *= 1099511628211ull;
}

void HashString(uint64_t& hash, std::string_view value) noexcept
{
    for (const char c : value)
        HashMix(hash, static_cast<uint8_t>(c));
}

MaterialResourceBindingKind ToBindingKind(MaterialValueType type) noexcept
{
    switch (type)
    {
    case MaterialValueType::Texture: return MaterialResourceBindingKind::Texture;
    case MaterialValueType::Sampler: return MaterialResourceBindingKind::Sampler;
    case MaterialValueType::Buffer: return MaterialResourceBindingKind::Buffer;
    default: return MaterialResourceBindingKind::Constant;
    }
}

MaterialRenderPassRequirement ToRenderPassRequirement(MaterialShaderPassRequirement pass) noexcept
{
    switch (pass)
    {
    case MaterialShaderPassRequirement::Shadow: return MaterialRenderPassRequirement::Shadow;
    case MaterialShaderPassRequirement::Depth: return MaterialRenderPassRequirement::Depth;
    case MaterialShaderPassRequirement::PostProcess: return MaterialRenderPassRequirement::PostProcess;
    case MaterialShaderPassRequirement::Custom: return MaterialRenderPassRequirement::Custom;
    case MaterialShaderPassRequirement::Forward:
    default:
        return MaterialRenderPassRequirement::Forward;
    }
}

MaterialLightingMode ResolveLightingMode(const CompiledMaterialDesc& compiled) noexcept
{
    if (compiled.surface == MaterialSurfaceType::Unlit || HasFlag(compiled.features, MaterialFeatureFlags::Unlit))
        return MaterialLightingMode::Unlit;
    if (HasFlag(compiled.features, MaterialFeatureFlags::PbrMetalRough))
        return MaterialLightingMode::Pbr;
    return MaterialLightingMode::Lit;
}

uint64_t ShaderAssetHash(std::string_view asset) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    HashString(hash, asset);
    return hash;
}

MaterialVertexLayoutRequirement BuildVertexLayoutRequirement(const CompiledMaterialDesc& compiled) noexcept
{
    MaterialVertexLayoutRequirement requirement{};
    requirement.layoutHash = static_cast<uint32_t>(compiled.resourceLayoutHash);
    requirement.requiresNormals = !HasFlag(compiled.features, MaterialFeatureFlags::Unlit);
    requirement.requiresTangents = HasFlag(compiled.features, MaterialFeatureFlags::NormalMap) ||
                                   HasFlag(compiled.features, MaterialFeatureFlags::NormalMapBC5);
    requirement.requiresTexCoords = HasFlag(compiled.features, MaterialFeatureFlags::BaseColorMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::NormalMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::NormalMapBC5) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::OrmMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::ChannelMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::MetallicMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::RoughnessMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::OcclusionMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::EmissiveMap) ||
                                    HasFlag(compiled.features, MaterialFeatureFlags::OpacityMap);
    requirement.requiresVertexColor = HasFlag(compiled.features, MaterialFeatureFlags::VertexColor);
    requirement.supportsInstancing = HasFlag(compiled.features, MaterialFeatureFlags::Instanced);
    requirement.supportsSkinning = HasFlag(compiled.features, MaterialFeatureFlags::Skinned);

    requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::Position);
    if (requirement.requiresNormals)
        requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::Normal);
    if (requirement.requiresTangents)
        requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::Tangent);
    if (requirement.requiresTexCoords)
        requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::TexCoord0);
    if (requirement.requiresVertexColor)
        requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::Color0);
    if (requirement.supportsSkinning)
    {
        requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::BoneWeight);
        requirement.semanticMask |= 1ull << static_cast<uint8_t>(VertexSemantic::BoneIndex);
    }
    return requirement;
}

ShaderVariantFlag ToShaderVariantFlags(MaterialFeatureFlags features) noexcept
{
    return static_cast<ShaderVariantFlag>(features);
}

void NormalizeDefinitionIdentity(MaterialDefinition& definition) noexcept
{
    if (definition.identity.displayName.empty())
        definition.identity.displayName = definition.desc.name;
    if (definition.identity.stableId.empty())
        definition.identity.stableId = definition.desc.name;
}

} // namespace

uint64_t HashMaterialDefinition(const MaterialDefinition& definition) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    const MaterialDesc& desc = definition.desc;

    HashString(hash, definition.identity.stableId);
    HashString(hash, definition.identity.displayName);
    HashString(hash, definition.identity.sourceAsset);
    HashMix(hash, definition.identity.version);
    HashString(hash, desc.name);
    HashMix(hash, static_cast<uint64_t>(desc.surface));
    HashMix(hash, static_cast<uint64_t>(desc.features));
    HashMix(hash, desc.sortLayer);

    HashMix(hash, static_cast<uint64_t>(desc.renderPolicy.blend.mode));
    HashMix(hash, static_cast<uint64_t>(desc.renderPolicy.cull.mode));
    HashMix(hash, desc.renderPolicy.cull.doubleSided ? 1ull : 0ull);
    HashMix(hash, desc.renderPolicy.depth.test ? 1ull : 0ull);
    HashMix(hash, desc.renderPolicy.depth.write ? 1ull : 0ull);
    HashMix(hash, static_cast<uint64_t>(desc.renderPolicy.depth.compare));
    HashMix(hash, desc.renderPolicy.alphaTest ? 1ull : 0ull);
    HashMix(hash, static_cast<uint64_t>(desc.renderPolicy.alphaCutoff * 100000.0f));
    HashMix(hash, desc.renderPolicy.castShadows ? 1ull : 0ull);
    HashMix(hash, desc.renderPolicy.receiveShadows ? 1ull : 0ull);

    for (const auto& param : desc.parameters)
    {
        HashString(hash, param.name);
        HashMix(hash, static_cast<uint64_t>(param.type));
    }

    for (const auto& slot : desc.textureSlots)
    {
        HashString(hash, slot.name);
        HashString(hash, slot.defaultTextureAsset);
        HashMix(hash, static_cast<uint64_t>(slot.semantic));
        HashMix(hash, slot.uvSet);
        HashMix(hash, slot.required ? 1ull : 0ull);
    }

    HashString(hash, desc.materialGraph);
    HashString(hash, definition.graph.graphId);
    HashString(hash, definition.graph.serializedGraph);
    for (const auto& output : definition.graph.outputs)
    {
        HashString(hash, output.nodeId);
        HashString(hash, output.outputName);
    }

    return hash;
}

uint64_t HashCompiledMaterialDesc(const CompiledMaterialDesc& compiled) noexcept
{
    uint64_t hash = 14695981039346656037ull;

    HashString(hash, compiled.stableId);
    HashString(hash, compiled.name);
    HashMix(hash, compiled.definitionVersion);
    HashMix(hash, static_cast<uint64_t>(compiled.surface));
    HashMix(hash, static_cast<uint64_t>(compiled.features));
    HashMix(hash, static_cast<uint64_t>(compiled.renderState.renderPolicy.blend.mode));
    HashMix(hash, static_cast<uint64_t>(compiled.renderState.renderPolicy.cull.mode));
    HashMix(hash, compiled.renderState.renderPolicy.cull.doubleSided ? 1ull : 0ull);
    HashMix(hash, compiled.renderState.renderPolicy.depth.test ? 1ull : 0ull);
    HashMix(hash, compiled.renderState.renderPolicy.depth.write ? 1ull : 0ull);
    HashMix(hash, static_cast<uint64_t>(compiled.renderState.renderPolicy.depth.compare));
    HashMix(hash, compiled.renderState.renderPolicy.alphaTest ? 1ull : 0ull);
    HashMix(hash, compiled.renderState.renderPolicy.castShadows ? 1ull : 0ull);
    HashMix(hash, compiled.renderState.renderPolicy.receiveShadows ? 1ull : 0ull);
    HashMix(hash, static_cast<uint64_t>(compiled.renderState.primitive));
    HashMix(hash, compiled.renderState.sortLayer);
    HashMix(hash, compiled.parameterLayout.layoutHash);

    for (const auto& requirement : compiled.shaderRequirements)
    {
        HashMix(hash, static_cast<uint64_t>(requirement.stage));
        HashMix(hash, static_cast<uint64_t>(requirement.pass));
        HashString(hash, requirement.entryPoint);
        HashString(hash, requirement.shaderAsset);
        HashMix(hash, static_cast<uint64_t>(requirement.featureMask));
    }

    for (const auto& binding : compiled.resourceBindings)
    {
        HashString(hash, binding.name);
        HashMix(hash, static_cast<uint64_t>(binding.kind));
        HashMix(hash, static_cast<uint64_t>(binding.valueType));
        HashMix(hash, static_cast<uint64_t>(binding.textureSemantic));
        HashMix(hash, binding.logicalSlot);
        HashMix(hash, binding.byteOffset);
        HashMix(hash, binding.byteSize);
        HashMix(hash, binding.required ? 1ull : 0ull);
    }

    HashMix(hash, compiled.definitionHash);
    HashMix(hash, compiled.variantHash);
    HashMix(hash, compiled.resourceLayoutHash);
    return hash;
}

CompiledMaterialDesc CompileMaterialDefinition(const MaterialDefinition& definition) noexcept
{
    const MaterialDesc& desc = definition.desc;

    CompiledMaterialDesc compiled{};
    compiled.stableId = definition.identity.stableId.empty() ? desc.name : definition.identity.stableId;
    compiled.name = desc.name;
    compiled.definitionVersion = definition.identity.version;
    compiled.surface = desc.surface;
    compiled.features = desc.features;
    compiled.renderState.renderPolicy = desc.renderPolicy;
    compiled.renderState.sortLayer = desc.sortLayer;
    compiled.parameterLayout = desc.parameterLayout;
    compiled.definitionHash = definition.contentHash != 0ull ? definition.contentHash : HashMaterialDefinition(definition);
    ShaderVariantFlag variantFlags = ToShaderVariantFlags(desc.features);
    if (desc.renderPolicy.alphaTest)
        variantFlags = variantFlags | ShaderVariantFlag::AlphaTest;
    if (desc.renderPolicy.cull.doubleSided)
        variantFlags = variantFlags | ShaderVariantFlag::DoubleSided;
    compiled.variantHash = static_cast<uint64_t>(variantFlags);
    compiled.resourceLayoutHash = desc.parameterLayout.layoutHash;

    for (const auto& param : desc.parameters)
    {
        MaterialResourceBindingDesc binding{};
        binding.name = param.name;
        binding.kind = ToBindingKind(param.type);
        binding.valueType = param.type;
        compiled.resourceBindings.push_back(binding);
    }

    for (uint32_t slotIndex = 0u; slotIndex < desc.parameterLayout.slotCount; ++slotIndex)
    {
        const MaterialParameterSlot& slot = desc.parameterLayout.slots[slotIndex];
        if (!slot.IsValid())
            continue;

        for (auto& binding : compiled.resourceBindings)
        {
            if (binding.name != slot.Name())
                continue;
            binding.logicalSlot = slot.binding;
            binding.byteOffset = slot.byteOffset;
            binding.byteSize = slot.byteSize;
            break;
        }
    }

    for (const auto& textureSlot : desc.textureSlots)
    {
        auto it = std::find_if(compiled.resourceBindings.begin(), compiled.resourceBindings.end(), [&](const MaterialResourceBindingDesc& binding) {
            return binding.name == textureSlot.name;
        });

        MaterialResourceBindingDesc* binding = nullptr;
        if (it == compiled.resourceBindings.end())
        {
            MaterialResourceBindingDesc created{};
            created.name = textureSlot.name;
            created.kind = MaterialResourceBindingKind::Texture;
            created.valueType = MaterialValueType::Texture;
            compiled.resourceBindings.push_back(created);
            binding = &compiled.resourceBindings.back();
        }
        else
        {
            binding = &(*it);
        }

        binding->textureSemantic = textureSlot.semantic;
        binding->logicalSlot = InferTextureSlot(textureSlot.name, binding->logicalSlot);
        binding->required = textureSlot.required;
    }

    if (desc.renderPolicy.castShadows || HasFlag(desc.features, MaterialFeatureFlags::ShadowCaster))
    {
        MaterialShaderRequirement shadow{};
        shadow.stage = MaterialShaderStageRequirement::Vertex;
        shadow.pass = MaterialShaderPassRequirement::Shadow;
        shadow.entryPoint = "VSMain";
        shadow.featureMask = desc.features | MaterialFeatureFlags::ShadowCaster;
        compiled.shaderRequirements.push_back(std::move(shadow));
    }

    MaterialShaderRequirement vertex{};
    vertex.stage = MaterialShaderStageRequirement::Vertex;
    vertex.pass = MaterialShaderPassRequirement::Forward;
    vertex.entryPoint = "VSMain";
    vertex.featureMask = desc.features;
    compiled.shaderRequirements.push_back(std::move(vertex));

    MaterialShaderRequirement fragment{};
    fragment.stage = MaterialShaderStageRequirement::Fragment;
    fragment.pass = MaterialShaderPassRequirement::Forward;
    fragment.entryPoint = "PSMain";
    fragment.featureMask = desc.features;
    compiled.shaderRequirements.push_back(std::move(fragment));

    compiled.compiledHash = HashCompiledMaterialDesc(compiled);
    return compiled;
}

MaterialShaderVariantKey MaterialShaderVariantKey::Normalized() const noexcept
{
    MaterialShaderVariantKey key = *this;
    if (key.renderPass == MaterialRenderPassRequirement::Shadow || key.renderPass == MaterialRenderPassRequirement::Depth)
    {
        const auto passMask = ShaderVariantFlag::Skinned |
                              ShaderVariantFlag::AlphaTest |
                              ShaderVariantFlag::ShadowPass |
                              ShaderVariantFlag::Instanced;
        key.flags = key.flags & passMask;
        key.lighting = MaterialLightingMode::Unlit;
    }
    return key;
}

uint64_t MaterialShaderVariantKey::Hash() const noexcept
{
    const MaterialShaderVariantKey normalized = Normalized();
    uint64_t hash = 14695981039346656037ull;
    HashMix(hash, normalized.shaderAssetHash);
    HashMix(hash, static_cast<uint64_t>(normalized.stage));
    HashMix(hash, static_cast<uint64_t>(normalized.renderPass));
    HashMix(hash, static_cast<uint64_t>(normalized.lighting));
    HashMix(hash, static_cast<uint64_t>(normalized.flags));
    HashMix(hash, normalized.vertexLayoutHash);
    HashMix(hash, normalized.resourceLayoutHash);
    return hash;
}

uint64_t HashMaterialShaderRequest(const MaterialShaderRequest& request) noexcept
{
    uint64_t hash = request.variantKey.Hash();
    HashString(hash, request.materialStableId);
    HashString(hash, request.materialName);
    HashString(hash, request.shaderAsset);
    HashString(hash, request.entryPoint);
    HashMix(hash, request.vertexLayout.semanticMask);
    HashMix(hash, request.vertexLayout.requiresNormals ? 1ull : 0ull);
    HashMix(hash, request.vertexLayout.requiresTangents ? 1ull : 0ull);
    HashMix(hash, request.vertexLayout.requiresTexCoords ? 1ull : 0ull);
    HashMix(hash, request.vertexLayout.requiresVertexColor ? 1ull : 0ull);
    HashMix(hash, request.vertexLayout.supportsInstancing ? 1ull : 0ull);
    HashMix(hash, request.vertexLayout.supportsSkinning ? 1ull : 0ull);
    return hash;
}

std::vector<MaterialShaderRequest> BuildMaterialShaderRequests(const CompiledMaterialDesc& compiled,
                                                               MaterialFeatureFlags instanceOverrides) noexcept
{
    std::vector<MaterialShaderRequest> requests;
    requests.reserve(compiled.shaderRequirements.size());

    const MaterialLightingMode lighting = ResolveLightingMode(compiled);
    const MaterialFeatureFlags combinedFeatures = compiled.features | instanceOverrides;
    ShaderVariantFlag flags = ToShaderVariantFlags(combinedFeatures);
    if (compiled.renderState.renderPolicy.alphaTest)
        flags = flags | ShaderVariantFlag::AlphaTest;
    if (compiled.renderState.renderPolicy.cull.doubleSided)
        flags = flags | ShaderVariantFlag::DoubleSided;

    const MaterialVertexLayoutRequirement vertexLayout = BuildVertexLayoutRequirement(compiled);

    for (const auto& requirement : compiled.shaderRequirements)
    {
        MaterialShaderRequest request{};
        request.materialStableId = compiled.stableId;
        request.materialName = compiled.name;
        request.shaderAsset = requirement.shaderAsset;
        request.entryPoint = requirement.entryPoint;
        request.stage = requirement.stage;
        request.renderPass = ToRenderPassRequirement(requirement.pass);
        request.lighting = lighting;
        request.vertexLayout = vertexLayout;
        request.flags = flags | ToShaderVariantFlags(requirement.featureMask);
        if (request.renderPass == MaterialRenderPassRequirement::Shadow)
            request.flags = request.flags | ShaderVariantFlag::ShadowPass;

        request.variantKey.shaderAssetHash = ShaderAssetHash(request.shaderAsset);
        request.variantKey.stage = request.stage;
        request.variantKey.renderPass = request.renderPass;
        request.variantKey.lighting = request.lighting;
        request.variantKey.flags = request.flags;
        request.variantKey.vertexLayoutHash = request.vertexLayout.layoutHash;
        request.variantKey.resourceLayoutHash = compiled.resourceLayoutHash;
        request.variantKey = request.variantKey.Normalized();
        request.flags = request.variantKey.flags;
        request.requestHash = HashMaterialShaderRequest(request);
        requests.push_back(std::move(request));
    }

    return requests;
}

void ValidateShaderBindings(const MaterialParameterLayout& reflected,
                             std::string_view shaderName) noexcept
{
    for (uint32_t i = 0u; i < reflected.slotCount; ++i)
    {
        const MaterialParameterSlot& slot = reflected.slots[i];
        if (!slot.IsValid()) continue;

        if (slot.type == MaterialParameterType::Texture2D || slot.type == MaterialParameterType::TextureCube)
        {
            for (const auto& entry : kTextureSlotTable)
            {
                if (entry.name != slot.Name()) continue;
                if (slot.binding != entry.slot)
                    Debug::LogError("Shader '%.*s': material expects '%s' at t%u, shader has it at t%u",
                                    static_cast<int>(shaderName.size()), shaderName.data(),
                                    slot.name, entry.slot, slot.binding);
                break;
            }
        }
        else if (slot.type == MaterialParameterType::Sampler)
        {
            for (const auto& entry : kSamplerSlotTable)
            {
                if (entry.name != slot.Name()) continue;
                if (slot.binding != entry.slot)
                    Debug::LogError("Shader '%.*s': material expects '%s' at s%u, shader has it at s%u",
                                    static_cast<int>(shaderName.size()), shaderName.data(),
                                    slot.name, entry.slot, slot.binding);
                break;
            }
        }
    }
}

ShaderVariantKey ShaderVariantKey::Normalized() const noexcept
{
    ShaderVariantKey k = *this;
    if (k.pass == ShaderPassType::Shadow || k.pass == ShaderPassType::Depth)
    {
        const auto shadowMask = ShaderVariantFlag::Skinned |
                                ShaderVariantFlag::AlphaTest |
                                ShaderVariantFlag::ShadowPass;
        k.flags = k.flags & shadowMask;
    }
    return k;
}

uint64_t ShaderVariantKey::Hash() const noexcept
{
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ull;
    };

    mix(static_cast<uint64_t>(baseShader.value));
    mix(static_cast<uint64_t>(static_cast<uint8_t>(pass)));
    mix(static_cast<uint64_t>(flags));
    return h;
}

bool PipelineKey::operator==(const PipelineKey& o) const noexcept
{
    return std::memcmp(this, &o, sizeof(PipelineKey)) == 0;
}

uint64_t PipelineKey::Hash() const noexcept
{
    static constexpr uint64_t kPrime = 1099511628211ull;
    static constexpr uint64_t kOffset = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(this);
    uint64_t hash = kOffset;
    for (size_t i = 0; i < sizeof(PipelineKey); ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kPrime;
    }
    return hash;
}

PipelineKey PipelineKey::From(const PipelineDesc& desc, RenderPassID pass) noexcept
{
    PipelineKey key{};
    static_assert(std::is_trivially_copyable_v<PipelineKey>, "PipelineKey must be trivially copyable");

    for (const auto& stage : desc.shaderStages)
    {
        if (stage.stage == ShaderStageMask::Vertex)
            key.vertexShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Fragment)
            key.fragmentShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Compute)
            key.computeShader = stage.handle.value;
    }

    key.fillMode = static_cast<uint8_t>(desc.rasterizer.fillMode);
    key.cullMode = static_cast<uint8_t>(desc.rasterizer.cullMode);
    key.frontFace = static_cast<uint8_t>(desc.rasterizer.frontFace);
    key.depthEnable = desc.depthStencil.depthEnable ? 1u : 0u;
    key.depthWrite = desc.depthStencil.depthWrite ? 1u : 0u;
    key.depthFunc = static_cast<uint8_t>(desc.depthStencil.depthFunc);
    key.stencilEnable = desc.depthStencil.stencilEnable ? 1u : 0u;

    const auto& blend = desc.blendStates[0];
    key.blendEnable = blend.blendEnable ? 1u : 0u;
    key.srcBlend = static_cast<uint8_t>(blend.srcBlend);
    key.dstBlend = static_cast<uint8_t>(blend.dstBlend);
    key.blendOp = static_cast<uint8_t>(blend.blendOp);
    key.srcBlendAlpha = static_cast<uint8_t>(blend.srcBlendAlpha);
    key.dstBlendAlpha = static_cast<uint8_t>(blend.dstBlendAlpha);
    key.blendOpAlpha = static_cast<uint8_t>(blend.blendOpAlpha);
    key.writeMask = blend.writeMask;

    key.colorFormat = static_cast<uint8_t>(desc.colorFormat);
    key.depthFormat = static_cast<uint8_t>(desc.depthFormat);
    key.sampleCount = static_cast<uint8_t>(desc.sampleCount);
    key.topology = static_cast<uint8_t>(desc.topology);
    key.vertexLayoutHash = HashVertexLayout(desc.vertexLayout);
    key.shaderContractHash = static_cast<uint32_t>(desc.shaderContractHash);
    key.pipelineLayoutHash = static_cast<uint32_t>(desc.pipelineLayoutHash);
    key.renderPassId = pass.value;
    return key;
}

SortKey SortKey::ForFrontToBack(RenderPassID pass,
                                uint8_t layer,
                                uint32_t pipelineHash,
                                uint32_t materialKey,
                                float linearDepth) noexcept
{
    const uint32_t depthKey = static_cast<uint32_t>(std::clamp(linearDepth, 0.0f, 1.0f) * 255.0f);
    SortKey key{};
    key.value = (static_cast<uint64_t>(pass.value & 0xFFu) << 56u) |
                (static_cast<uint64_t>(layer) << 48u) |
                (static_cast<uint64_t>(pipelineHash & 0x00FFFFFFu) << 24u) |
                (static_cast<uint64_t>(materialKey & 0xFFFFu) << 8u) |
                static_cast<uint64_t>(depthKey);
    return key;
}

SortKey SortKey::ForBackToFront(RenderPassID pass, uint8_t layer, float linearDepth) noexcept
{
    const uint32_t depthKey = 65535u - static_cast<uint32_t>(std::clamp(linearDepth, 0.0f, 1.0f) * 65535.0f);
    SortKey key{};
    key.value = (static_cast<uint64_t>(pass.value) << 48u) |
                (static_cast<uint64_t>(layer) << 40u) |
                static_cast<uint64_t>(depthKey);
    return key;
}

SortKey SortKey::ForSubmissionOrder(RenderPassID pass, uint8_t layer, uint32_t drawOrder) noexcept
{
    SortKey key{};
    key.value = (static_cast<uint64_t>(pass.value) << 48u) |
                (static_cast<uint64_t>(layer) << 40u) |
                static_cast<uint64_t>(drawOrder);
    return key;
}

float* MaterialInstance::GetFloatPtr(const std::string& name) noexcept
{
    for (auto& param : parameterOverrides)
    {
        if (param.name != name) continue;
        switch (param.type)
        {
        case MaterialParam::Type::Float:
        case MaterialParam::Type::Vec2:
        case MaterialParam::Type::Vec3:
        case MaterialParam::Type::Vec4:
            return &param.value.f[0];
        default:
            return nullptr;
        }
    }
    return nullptr;
}

uint32_t MaterialSystem::AllocSlot()
{
    if (!m_freeSlots.empty())
    {
        const uint32_t index = m_freeSlots.back();
        m_freeSlots.pop_back();
        return index;
    }

    const uint32_t index = static_cast<uint32_t>(m_descs.size());
    m_descs.emplace_back();
    m_instances.emplace_back();
    m_instanceDerived.emplace_back();
    m_generations.push_back(1u);
    return index;
}

bool MaterialSystem::ValidHandle(MaterialHandle h) const noexcept
{
    if (!h.IsValid())
        return false;
    const uint32_t index = h.Index();
    return index < m_generations.size() && m_generations[index] == h.Generation();
}

void MaterialSystem::NormalizeDesc(MaterialDesc& desc) const noexcept
{
    MaterialFeatureEval::NormalizeDesc(desc);
}

MaterialParameterLayout MaterialSystem::BuildLayoutFromDesc(const MaterialDesc& desc) const noexcept
{
    if (desc.parameterLayout.IsValid())
        return desc.parameterLayout;

    MaterialParameterLayout layout{};
    uint32_t cbOffset = 0u;
    uint32_t nextTextureSlot = 0u;
    uint32_t nextSamplerSlot = 0u;
    uint32_t nextBufferSlot = 0u;

    for (const auto& param : desc.parameters)
    {
        MaterialParameterSlot slot{};
        slot.SetName(param.name);
        slot.type = ToLayoutType(param.type);
        slot.set = 0u;
        slot.stageFlags = (param.type == MaterialParam::Type::Texture || param.type == MaterialParam::Type::Sampler)
                        ? MaterialShaderStageMask::Fragment
                        : MaterialShaderStageMask::Vertex | MaterialShaderStageMask::Fragment;
        slot.elementCount = 1u;

        if (IsResourceType(slot.type))
        {
            switch (slot.type)
            {
            case MaterialParameterType::Texture2D:
            case MaterialParameterType::TextureCube:
            {
                auto overrideIt = desc.textureBindingOverrides.find(param.name);
                slot.binding = (overrideIt != desc.textureBindingOverrides.end())
                    ? overrideIt->second
                    : InferTextureSlot(param.name, nextTextureSlot);
                ++nextTextureSlot;
                break;
            }
            case MaterialParameterType::Sampler:
                slot.binding = InferSamplerSlot(param.name, nextSamplerSlot++);
                break;
            case MaterialParameterType::StructuredBuffer:
                slot.binding = nextBufferSlot++;
                break;
            default:
                break;
            }
        }
        else
        {
            const uint32_t byteSize = ParameterByteSize(slot.type);
            const uint32_t rowOffset = cbOffset & 15u;
            if (byteSize == 16u || (rowOffset + byteSize) > 16u)
                cbOffset = Align16(cbOffset);

            slot.binding = CBSlots::PerMaterial;
            slot.byteOffset = cbOffset;
            slot.byteSize = byteSize;
            cbOffset += byteSize;
        }

        if (!layout.AddSlot(slot))
            break;
    }

    return layout;
}

void MaterialSystem::InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept
{
    inst.parameters.Reset(desc.parameterLayout);
    inst.parameterOverrides = desc.parameters;
    inst.featureOverrides = MaterialFeatureFlags::None;
    inst.dirty = true;
}

void MaterialSystem::SyncBlobFromParams(MaterialInstance& inst, InstanceDerivedData& derived) const
{
    for (uint32_t slotIndex = 0u; slotIndex < derived.layout.slotCount; ++slotIndex)
    {
        const MaterialParameterSlot& slot = derived.layout.slots[slotIndex];
        auto it = std::find_if(inst.parameterOverrides.begin(), inst.parameterOverrides.end(), [&](const MaterialParam& param) {
            return param.name == slot.Name();
        });
        if (it == inst.parameterOverrides.end())
            continue;

        switch (slot.type)
        {
        case MaterialParameterType::Float:
        case MaterialParameterType::Vec2:
        case MaterialParameterType::Vec3:
        case MaterialParameterType::Vec4:
            std::memcpy(derived.cbData.data() + slot.byteOffset, it->value.f, std::min<uint32_t>(slot.byteSize, 16u));
            break;
        case MaterialParameterType::Int:
            std::memcpy(derived.cbData.data() + slot.byteOffset, &it->value.i, 4u);
            break;
        case MaterialParameterType::Bool:
        {
            const uint32_t value = it->value.b ? 1u : 0u;
            std::memcpy(derived.cbData.data() + slot.byteOffset, &value, 4u);
            break;
        }
        case MaterialParameterType::Texture2D:
        case MaterialParameterType::TextureCube:
(void)inst.parameters.SetTexture(slotIndex, it->texture);
            break;
        case MaterialParameterType::Sampler:
(void)inst.parameters.SetSampler(slotIndex, it->samplerIdx);
            break;
        case MaterialParameterType::StructuredBuffer:
(void)inst.parameters.SetBuffer(slotIndex, it->buffer);
            break;
        default:
            break;
        }
    }
}

MaterialHandle MaterialSystem::RegisterMaterial(MaterialDesc desc)
{
    MaterialDefinition definition{};
    definition.desc = std::move(desc);
    return RegisterMaterialDefinition(std::move(definition));
}

MaterialHandle MaterialSystem::RegisterMaterialDefinition(MaterialDefinition definition)
{
    NormalizeDesc(definition.desc);

    const uint32_t index = AllocSlot();
    const MaterialHandle handle = MaterialHandle::Make(index, m_generations[index]);
    const std::string name = definition.desc.name.empty() ? ("Material_" + std::to_string(index)) : definition.desc.name;

    definition.desc.name = name;
    NormalizeDefinitionIdentity(definition);
    if (!definition.desc.parameterLayout.IsValid())
        definition.desc.parameterLayout = BuildLayoutFromDesc(definition.desc);
    definition.contentHash = HashMaterialDefinition(definition);
    const CompiledMaterialDesc compiled = MaterialFeatureEval::BuildCompiledDesc(definition);

    m_descs[index].definition = definition;
    m_descs[index].compiled = compiled;
    m_descs[index].isInstance = false;
    m_descs[index].baseHandle = MaterialHandle::Invalid();
    m_nameLookup[name] = handle;

    MaterialInstance& inst = m_instances[index];
    inst.materialDefinition = handle;
    m_instanceDerived[index].layout = definition.desc.parameterLayout;
    m_instanceDerived[index].layoutDirty = true;
    m_instanceDerived[index].cbDirty = true;
    InitializeInstanceFromDesc(inst, definition.desc);

    Debug::Log("MaterialSystem.cpp: RegisterMaterial '%s' idx=%u",
               name.c_str(),
               index);
    return handle;
}

MaterialHandle MaterialSystem::CreateInstance(MaterialHandle base, std::string instanceName)
{
    if (!ValidHandle(base))
    {
        Debug::LogError("MaterialSystem.cpp: CreateInstance - invalid base handle");
        return MaterialHandle::Invalid();
    }

    const MaterialDefinition baseDefinition = m_descs[base.Index()].definition;
    MaterialDefinition definition = baseDefinition;
    MaterialDesc& desc = definition.desc;
    const MaterialDesc& baseDesc = baseDefinition.desc;
    const uint32_t index = AllocSlot();
    const MaterialHandle handle = MaterialHandle::Make(index, m_generations[index]);

    desc.name = instanceName.empty() ? (baseDesc.name + "_Instance_" + std::to_string(index)) : std::move(instanceName);
    NormalizeDefinitionIdentity(definition);
    definition.contentHash = HashMaterialDefinition(definition);

    m_descs[index].definition = definition;
    m_descs[index].compiled = MaterialFeatureEval::BuildCompiledDesc(definition);
    m_descs[index].isInstance = true;
    m_descs[index].baseHandle = base;
    m_nameLookup[desc.name] = handle;

    MaterialInstance& inst = m_instances[index];
    inst.materialDefinition = base;
    m_instanceDerived[index].layout = desc.parameterLayout;
    m_instanceDerived[index].layoutDirty = true;
    m_instanceDerived[index].cbDirty = true;
    InitializeInstanceFromDesc(inst, desc);
    return handle;
}

const MaterialDefinition* MaterialSystem::GetDefinition(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_descs[h.Index()].definition : nullptr;
}

const CompiledMaterialDesc* MaterialSystem::GetCompiledDesc(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_descs[h.Index()].compiled : nullptr;
}

const MaterialDesc* MaterialSystem::GetDesc(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_descs[h.Index()].definition.desc : nullptr;
}

const MaterialParameterLayout* MaterialSystem::GetParameterLayout(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_instanceDerived[h.Index()].layout : nullptr;
}

MaterialHandle MaterialSystem::FindMaterial(const std::string& name) const noexcept
{
    auto it = m_nameLookup.find(name);
    return it != m_nameLookup.end() ? it->second : MaterialHandle::Invalid();
}

MaterialInstance* MaterialSystem::GetInstance(MaterialHandle h) noexcept
{
    return ValidHandle(h) ? &m_instances[h.Index()] : nullptr;
}

const MaterialInstance* MaterialSystem::GetInstance(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_instances[h.Index()] : nullptr;
}

void MaterialSystem::SetFloat(MaterialHandle h, const std::string& name, float v)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Float, [&](MaterialParam& param) {
        param.value.f[0] = v;
    });
}

void MaterialSystem::SetInt(MaterialHandle h, const std::string& name, int32_t v)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Int, [&](MaterialParam& param) {
        param.value.i = v;
    });
}

void MaterialSystem::SetBool(MaterialHandle h, const std::string& name, bool v)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Bool, [&](MaterialParam& param) {
        param.value.b = v;
    });
}

void MaterialSystem::SetVec2(MaterialHandle h, const std::string& name, const math::Vec2& v)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Vec2, [&](MaterialParam& param) {
        param.value.f[0] = v.x;
        param.value.f[1] = v.y;
    });
}

void MaterialSystem::SetVec3(MaterialHandle h, const std::string& name, const math::Vec3& v)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Vec3, [&](MaterialParam& param) {
        param.value.f[0] = v.x;
        param.value.f[1] = v.y;
        param.value.f[2] = v.z;
    });
}

void MaterialSystem::SetVec4(MaterialHandle h, const std::string& name, const math::Vec4& v)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Vec4, [&](MaterialParam& param) {
        param.value.f[0] = v.x;
        param.value.f[1] = v.y;
        param.value.f[2] = v.z;
        param.value.f[3] = v.w;
    });
}

void MaterialSystem::SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex)
{
    (void)MutateParameter(h, name, MaterialParam::Type::Texture, [&](MaterialParam& param) {
        param.texture = tex;
    });
}

void MaterialSystem::MarkDirty(MaterialHandle h)
{
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return;
    m_instanceDerived[h.Index()].cbDirty = true;
    inst->dirty = true;
    ++inst->revision;
}

uint64_t MaterialSystem::GetRevision(MaterialHandle h) const noexcept
{
    const MaterialInstance* inst = GetInstance(h);
    return inst ? inst->revision : 0ull;
}

const std::vector<uint8_t>& MaterialSystem::GetCBData(MaterialHandle h)
{
    static const std::vector<uint8_t> kEmpty{};
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return kEmpty;
    InstanceDerivedData& derived = m_instanceDerived[h.Index()];

    if (derived.cbDirty || derived.layoutDirty || inst->dirty)
    {
        MaterialCBLayout::BuildCBData(derived.layout, inst->parameters, derived.cbLayout, derived.cbData);
        if (derived.cbData.empty() && derived.cbLayout.totalSize > 0u)
            derived.cbData.assign(derived.cbLayout.totalSize, 0u);
        SyncBlobFromParams(*inst, derived);
        derived.cbDirty = false;
        derived.layoutDirty = false;
        inst->dirty = false;
    }

    return derived.cbData;
}

const CbLayout& MaterialSystem::GetCBLayout(MaterialHandle h)
{
    static const CbLayout kEmpty{};
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return kEmpty;
    GetCBData(h);
    return m_instanceDerived[h.Index()].cbLayout;
}

void MaterialSystem::DestroyMaterial(MaterialHandle h)
{
    if (!ValidHandle(h))
        return;

    const uint32_t index = h.Index();
    const std::string name = m_descs[index].definition.desc.name;
    m_nameLookup.erase(name);

    m_descs[index]          = DescSlot{};
    m_instances[index]      = MaterialInstance{};
    m_instanceDerived[index] = InstanceDerivedData{};
    ++m_generations[index]; // alle bestehenden Handles auf diesen Slot werden invalid
    m_freeSlots.push_back(index);

    Debug::Log("MaterialSystem: DestroyMaterial '%s' idx=%u", name.c_str(), index);
}

template<typename Fn>
bool MaterialSystem::MutateParameter(MaterialHandle h,
                                     const std::string& name,
                                     MaterialParam::Type expectedType,
                                     Fn&& fn)
{
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return false;

    for (auto& param : inst->parameterOverrides)
    {
        if (param.name != name)
            continue;

        if (param.type != expectedType)
        {
            Debug::LogWarning("MaterialSystem.cpp: parameter '%s' has incompatible type", name.c_str());
            return false;
        }

        fn(param);
        MarkDirty(h);
        return true;
    }

    Debug::LogWarning("MaterialSystem.cpp: parameter '%s' not found", name.c_str());
    return false;
}

} // namespace engine::renderer
