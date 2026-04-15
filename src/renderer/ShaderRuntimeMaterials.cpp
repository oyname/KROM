#include "renderer/ShaderRuntime.hpp"
#include "renderer/MaterialSystem.hpp"
#include "core/Debug.hpp"
#include <array>
#include <algorithm>
#include <unordered_set>

namespace engine::renderer {

    namespace {

        uint64_t FoldContractHash(const assets::CompiledShaderArtifact* artifact, uint64_t salt)
        {
            if (!artifact)
                return 0ull;
            const uint64_t key = artifact->contract.pipelineStateKey != 0ull
                ? artifact->contract.pipelineStateKey
                : artifact->contract.contractHash;
            return key != 0ull ? (key ^ salt) : 0ull;
        }

        uint64_t FoldPipelineBindingHash(const assets::CompiledShaderArtifact* artifact, uint64_t salt)
        {
            if (!artifact)
                return 0ull;

            const uint64_t rootKey = artifact->contract.pipelineBinding.bindingSignatureKey != 0ull
                ? artifact->contract.pipelineBinding.bindingSignatureKey
                : artifact->contract.interfaceLayout.layoutHash;
            return rootKey != 0ull ? (rootKey ^ salt) : 0ull;
        }

    } // namespace
    namespace {
        const ShaderStageMask kGraphicsStages = ShaderStageMask::Vertex | ShaderStageMask::Fragment;

        uint32_t ToContractStageBits(ShaderStageMask mask) noexcept
        {
            return static_cast<uint32_t>(mask);
        }

        uint64_t HashContractBindings(const ShaderInterfaceLayout& layout) noexcept
        {
            uint64_t h = 1469598103934665603ull;
            for (const auto& binding : layout.bindings)
            {
                h ^= static_cast<uint64_t>(binding.logicalSlot) + (h << 1u);
                h ^= static_cast<uint64_t>(binding.apiBinding) + (h << 1u);
                h ^= static_cast<uint64_t>(binding.space) + (h << 1u);
                h ^= static_cast<uint64_t>(binding.bindingClass) + (h << 1u);
                h ^= static_cast<uint64_t>(binding.stageMask) + (h << 1u);
                for (char c : binding.name)
                {
                    h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
                    h *= 1099511628211ull;
                }
            }
            return h;
        }

        std::string ToLower(std::string s)
        {
            for (char& ch : s)
                if (ch >= "A"[0] && ch <= "Z"[0]) ch = static_cast<char>(ch - "A"[0] + "a"[0]);
            return s;
        }

        uint32_t ResolveTextureSlotByName(const std::string& name)
        {
            const std::string lower = ToLower(name);
            if (lower.find("albedo") != std::string::npos || lower.find("basecolor") != std::string::npos || lower.find("diffuse") != std::string::npos)
                return TexSlots::Albedo;
            if (lower.find("normal") != std::string::npos)
                return TexSlots::Normal;
            if (lower.find("orm") != std::string::npos || lower.find("rough") != std::string::npos || lower.find("metal") != std::string::npos || lower.find("occlusion") != std::string::npos)
                return TexSlots::ORM;
            if (lower.find("emissive") != std::string::npos)
                return TexSlots::Emissive;
            if (lower.find("shadow") != std::string::npos)
                return TexSlots::ShadowMap;
            if (lower.find("history") != std::string::npos)
                return TexSlots::HistoryBuffer;
            if (lower.find("bloom") != std::string::npos)
                return TexSlots::BloomTexture;
            return TexSlots::PassSRV0;
        }

        uint32_t ResolveSamplerSlotByName(const std::string& name)
        {
            const std::string lower = ToLower(name);
            if (lower.find("shadow") != std::string::npos || lower.find("pcf") != std::string::npos)
                return SamplerSlots::ShadowPCF;
            if (lower.find("point") != std::string::npos)
                return SamplerSlots::PointClamp;
            if (lower.find("clamp") != std::string::npos)
                return SamplerSlots::LinearClamp;
            return SamplerSlots::LinearWrap;
        }

    } // namespace
    std::vector<ResolvedMaterialBinding> ShaderRuntime::ResolveBindings(const MaterialSystem& materials,
        MaterialHandle material) const
    {
        std::vector<ResolvedMaterialBinding> resolved;
        const MaterialDesc* desc = materials.GetDesc(material);
        const MaterialInstance* inst = materials.GetInstance(material);
        if (!desc || !inst) return resolved;

        ResolvedMaterialBinding materialCB{};
        materialCB.kind = ResolvedMaterialBinding::Kind::ConstantBuffer;
        materialCB.name = "PerMaterial";
        materialCB.slot = CBSlots::PerMaterial;
        materialCB.stages = kGraphicsStages;
        resolved.push_back(materialCB);

        const auto pushTexture = [&](const char* name, uint32_t slot, TextureHandle texture, uint32_t /*samplerIdx*/ = 0u)
            {
                if (!texture.IsValid())
                    return;
                ResolvedMaterialBinding tex{};
                tex.kind = ResolvedMaterialBinding::Kind::Texture;
                tex.name = name;
                tex.slot = slot;
                tex.stages = ShaderStageMask::Fragment;
                tex.texture = texture;
                resolved.push_back(tex);
            };

        const auto baseColorTex = materials.GetSemanticTexture(material, MaterialSemantic::BaseColor);
        const auto normalTex = materials.GetSemanticTexture(material, MaterialSemantic::Normal);
        const auto emissiveTex = materials.GetSemanticTexture(material, MaterialSemantic::Emissive);
        const auto ormExplicit = materials.GetSemanticTexture(material, MaterialSemantic::ORM);
        const auto metallicTex = materials.GetSemanticTexture(material, MaterialSemantic::Metallic);
        const auto roughnessTex = materials.GetSemanticTexture(material, MaterialSemantic::Roughness);
        const auto occlusionTex = materials.GetSemanticTexture(material, MaterialSemantic::Occlusion);

        // Resolve the ORM carrier for slot t2.
        // Priority: explicit ORM semantic > Metallic > Roughness > Occlusion (individual single-channel).
        // Explicit ORM is the preferred GLTF-style path; the individual fallback is the legacy path.
        TextureHandle ormTex = TextureHandle::Invalid();
        uint32_t ormSamplerIdx = 0u;
        if (ormExplicit.IsValid())
        {
            ormTex = ormExplicit;
            ormSamplerIdx = inst->semanticTextures[static_cast<size_t>(MaterialSemantic::ORM)].samplerIdx;
        }
        else
        {
            const auto tryTakeOrm = [&](MaterialSemantic semantic, TextureHandle tex)
                {
                    if (!tex.IsValid() || ormTex.IsValid())
                        return;
                    ormTex = tex;
                    ormSamplerIdx = inst->semanticTextures[static_cast<size_t>(semantic)].samplerIdx;
                };
            tryTakeOrm(MaterialSemantic::Metallic, metallicTex);
            tryTakeOrm(MaterialSemantic::Roughness, roughnessTex);
            tryTakeOrm(MaterialSemantic::Occlusion, occlusionTex);
        }

        const TextureHandle ormFallback = ResolveFallbackTexture(MaterialSemantic::ORM);
        pushTexture("BaseColor", TexSlots::Albedo,
            baseColorTex.IsValid() ? baseColorTex : ResolveFallbackTexture(MaterialSemantic::BaseColor),
            inst->semanticTextures[static_cast<size_t>(MaterialSemantic::BaseColor)].samplerIdx);
        pushTexture("Normal", TexSlots::Normal,
            normalTex.IsValid() ? normalTex : ResolveFallbackTexture(MaterialSemantic::Normal),
            inst->semanticTextures[static_cast<size_t>(MaterialSemantic::Normal)].samplerIdx);
        pushTexture("ORM", TexSlots::ORM,
            ormTex.IsValid() ? ormTex : ormFallback,
            ormSamplerIdx);
        pushTexture("Emissive", TexSlots::Emissive,
            emissiveTex.IsValid() ? emissiveTex : ResolveFallbackTexture(MaterialSemantic::Emissive),
            inst->semanticTextures[static_cast<size_t>(MaterialSemantic::Emissive)].samplerIdx);

        const TextureHandle iblIrr = m_environment.active && m_environment.irradiance.IsValid()
            ? m_environment.irradiance
            : m_fallbackTextures.iblIrradiance;
        const TextureHandle iblPre = m_environment.active && m_environment.prefiltered.IsValid()
            ? m_environment.prefiltered
            : m_fallbackTextures.iblPrefiltered;
        const TextureHandle iblLut = m_environment.active && m_environment.brdfLut.IsValid()
            ? m_environment.brdfLut
            : m_fallbackTextures.brdfLut;

        pushTexture("IBLIrradiance", TexSlots::IBLIrradiance, iblIrr);
        pushTexture("IBLPrefiltered", TexSlots::IBLPrefiltered, iblPre);
        pushTexture("BRDFLUT", TexSlots::BRDFLUT, iblLut);

        const auto& params = inst->instanceParams.empty() ? desc->params : inst->instanceParams;
        for (const auto& param : params)
        {
            if (param.type == MaterialParam::Type::Texture)
            {
                const uint32_t slot = ResolveTextureSlotByName(param.name);
                if (slot == TexSlots::Albedo || slot == TexSlots::Normal ||
                    slot == TexSlots::ORM || slot == TexSlots::Emissive ||
                    slot == TexSlots::IBLIrradiance || slot == TexSlots::IBLPrefiltered ||
                    slot == TexSlots::BRDFLUT || slot == TexSlots::ShadowMap)
                    continue;

                ResolvedMaterialBinding binding{};
                binding.kind = ResolvedMaterialBinding::Kind::Texture;
                binding.name = param.name;
                binding.slot = slot;
                binding.stages = ShaderStageMask::Fragment;
                binding.texture = param.texture;
                resolved.push_back(binding);
            }
            else if (param.type == MaterialParam::Type::Sampler)
            {
                ResolvedMaterialBinding binding{};
                binding.kind = ResolvedMaterialBinding::Kind::Sampler;
                binding.name = param.name;
                binding.slot = ResolveSamplerSlotByName(param.name);
                binding.stages = ShaderStageMask::Fragment;
                binding.samplerIndex = param.samplerIdx;
                resolved.push_back(binding);
            }
        }

        // Alle 4 Engine-Default-Sampler immer binden, sofern nicht bereits durch
        // explizite MaterialParam::Type::Sampler-Einträge belegt.
        // Verhindert D3D11 WARNING #352 (DEVICE_DRAW_SAMPLER_NOT_SET) für Materialien
        // die keine Sampler-Params explizit setzen (z.B. PBR ohne Legacy-Params).
        {
            bool bound[SamplerSlots::COUNT] = {};
            for (const auto& b : resolved)
                if (b.kind == ResolvedMaterialBinding::Kind::Sampler && b.slot < SamplerSlots::COUNT)
                    bound[b.slot] = true;

            struct DefaultSampler { uint32_t slot; const char* name; };
            static constexpr DefaultSampler kDefaults[] = {
                { SamplerSlots::LinearWrap,  "sLinearWrap"  },
                { SamplerSlots::LinearClamp, "sLinearClamp" },
                { SamplerSlots::PointClamp,  "sPointClamp"  },
                { SamplerSlots::ShadowPCF,   "sShadowPCF"   },
            };
            for (const auto& s : kDefaults)
            {
                if (bound[s.slot]) continue;
                ResolvedMaterialBinding b{};
                b.kind = ResolvedMaterialBinding::Kind::Sampler;
                b.name = s.name;
                b.slot = s.slot;
                b.stages = ShaderStageMask::Fragment;
                b.samplerIndex = 0u; // wird in BindMaterial per Slot auf echten Handle gemappt
                resolved.push_back(b);
            }
        }

        return resolved;
    }

    PipelineDesc ShaderRuntime::BuildPipelineDesc(const MaterialSystem& materials,
        MaterialHandle material,
        ShaderHandle gpuVS,
        ShaderHandle gpuPS) const
    {
        const MaterialDesc* desc = materials.GetDesc(material);
        return BuildPipelineDescForPass(materials, material, gpuVS, gpuPS,
            desc ? desc->passTag : RenderPassTag::Opaque);
    }

    PipelineDesc ShaderRuntime::BuildPipelineDescForPass(const MaterialSystem& materials,
        MaterialHandle material,
        ShaderHandle gpuVS,
        ShaderHandle gpuPS,
        RenderPassTag pass) const
    {
        PipelineDesc pd;
        const MaterialDesc* desc = materials.GetDesc(material);
        if (!desc) return pd;

        if (gpuVS.IsValid()) pd.shaderStages.push_back({ gpuVS, ShaderStageMask::Vertex });
        if (gpuPS.IsValid()) pd.shaderStages.push_back({ gpuPS, ShaderStageMask::Fragment });
        pd.vertexLayout = desc->vertexLayout;
        pd.rasterizer = desc->rasterizer;
        pd.blendStates[0] = desc->blend;
        pd.depthStencil = desc->depthStencil;
        pd.topology = desc->topology;
        pd.colorFormat = (pass == RenderPassTag::Shadow) ? Format::Unknown : desc->colorFormat;
        pd.depthFormat = (pass == RenderPassTag::Shadow) ? Format::D32_FLOAT : desc->depthFormat;
        pd.pipelineClass = PipelineClass::Graphics;
        if (m_assets)
        {
            uint64_t contractHash = 0ull;
            uint64_t pipelineBindingHash = 0ull;

            const auto* vsAsset = m_assets->shaders.Get((pass == RenderPassTag::Shadow && desc->shadowShader.IsValid()) ? desc->shadowShader : desc->vertexShader);
            const auto* vsArtifact = vsAsset ? FindCompiledArtifact(*vsAsset) : nullptr;
            contractHash ^= FoldContractHash(vsArtifact, 0x9E3779B185EBCA87ull);
            pipelineBindingHash ^= FoldPipelineBindingHash(vsArtifact, 0x517CC1B727220A95ull);

            if (gpuPS.IsValid())
            {
                const auto* psAsset = m_assets->shaders.Get(desc->fragmentShader);
                const auto* psArtifact = psAsset ? FindCompiledArtifact(*psAsset) : nullptr;
                contractHash ^= FoldContractHash(psArtifact, 0xC2B2AE3D27D4EB4Full);
                pipelineBindingHash ^= FoldPipelineBindingHash(psArtifact, 0x165667B19E3779F9ull);
            }

            pd.shaderContractHash = contractHash;
            pd.pipelineLayoutHash = pipelineBindingHash != 0ull ? pipelineBindingHash : contractHash;
        }
        pd.debugName = desc->name + ((pass == RenderPassTag::Shadow) ? "_ShadowPipeline" : "_Pipeline");
        return pd;
    }

    PipelineHandle ShaderRuntime::ResolvePipelineForPass(const MaterialSystem& materials,
        MaterialHandle material,
        const MaterialGpuState& state,
        RenderPassTag pass)
    {
        const MaterialDesc* desc = materials.GetDesc(material);
        if (!desc)
            return PipelineHandle::Invalid();

        if (pass != RenderPassTag::Shadow || desc->passTag == RenderPassTag::Shadow)
            return state.pipeline;

        ShaderHandle gpuVS = desc->shadowShader.IsValid() ? desc->shadowShader : desc->vertexShader;
        if (m_assets && gpuVS.IsValid())
            gpuVS = PrepareShaderAsset(gpuVS);
        if (!gpuVS.IsValid())
            return PipelineHandle::Invalid();

        const PipelineDesc pipelineDesc = BuildPipelineDescForPass(materials, material, gpuVS, ShaderHandle::Invalid(), pass);
        const PipelineKey pipelineKey = PipelineKey::From(pipelineDesc, pass);
        return m_pipelineCache.GetOrCreate(pipelineKey, [&](const PipelineKey&) {
            return m_device->CreatePipeline(pipelineDesc);
            });
    }

    bool ShaderRuntime::ValidateMaterial(const MaterialSystem& materials,
        MaterialHandle material,
        std::vector<ShaderValidationIssue>& outIssues) const
    {
        outIssues.clear();
        const MaterialDesc* desc = materials.GetDesc(material);
        const MaterialInstance* inst = materials.GetInstance(material);
        if (!desc || !inst)
        {
            outIssues.push_back({ ShaderValidationIssue::Severity::Error, "invalid material handle" });
            return false;
        }

        const bool isShadowPass = desc->passTag == RenderPassTag::Shadow;
        const ShaderHandle vertexShader = (isShadowPass && desc->shadowShader.IsValid())
            ? desc->shadowShader
            : desc->vertexShader;

        if (!vertexShader.IsValid())
            outIssues.push_back({ ShaderValidationIssue::Severity::Error, "missing vertex shader" });
        if (!desc->fragmentShader.IsValid() && !isShadowPass)
            outIssues.push_back({ ShaderValidationIssue::Severity::Error, "missing fragment shader" });

        {
            // Validate ORM slot t2 contract: if the explicit ORM semantic is set, it is the sole carrier
            // and individual M/R/O textures are ignored for slot t2 (no conflict possible).
            // If no explicit ORM is set, all individual textures that are present must share the same
            // handle — otherwise the shader would see an undefined ORM composition.
            const TextureHandle ormExplicitHandle = materials.GetSemanticTexture(material, MaterialSemantic::ORM);
            if (!ormExplicitHandle.IsValid())
            {
                const TextureHandle metallic = materials.GetSemanticTexture(material, MaterialSemantic::Metallic);
                const TextureHandle roughness = materials.GetSemanticTexture(material, MaterialSemantic::Roughness);
                const TextureHandle occlusion = materials.GetSemanticTexture(material, MaterialSemantic::Occlusion);

                TextureHandle ormCarrier = TextureHandle::Invalid();
                const auto validateOrmCarrier = [&](TextureHandle candidate, const char* label)
                    {
                        if (!candidate.IsValid())
                            return;
                        if (!ormCarrier.IsValid())
                        {
                            ormCarrier = candidate;
                            return;
                        }
                        if (ormCarrier != candidate)
                        {
                            outIssues.push_back({ ShaderValidationIssue::Severity::Error,
                                                 std::string("packed ORM contract violated: '") + label +
                                                 "' uses a different texture handle. "
                                                 "Either use MaterialSemantic::ORM for a pre-packed texture, "
                                                 "or ensure Metallic/Roughness/Occlusion all reference the same handle." });
                        }
                    };

                validateOrmCarrier(metallic, "Metallic");
                validateOrmCarrier(roughness, "Roughness");
                validateOrmCarrier(occlusion, "Occlusion");
            }
        }

        // Explizite Params direkt auf Slot-Duplikate prüfen.
        // ResolveBindings filtert Semantic-Slots aus expliziten Params heraus,
        // weshalb Duplikate dort nicht sichtbar wären.
        {
            std::unordered_map<uint32_t, std::string> seenParamSlots;
            const auto& rawParams = inst->instanceParams.empty() ? desc->params : inst->instanceParams;
            for (const auto& param : rawParams)
            {
                if (param.type != MaterialParam::Type::Texture)
                    continue;
                const uint32_t slot = ResolveTextureSlotByName(param.name);
                const auto [it, inserted] = seenParamSlots.emplace(slot, param.name);
                if (!inserted)
                    outIssues.push_back({ ShaderValidationIssue::Severity::Error,
                                          "duplicate texture slot " + std::to_string(slot) +
                                          " ('" + it->second + "' and '" + param.name + "')" });
            }
        }

        std::unordered_set<uint32_t> textureSlots;
        std::unordered_set<uint32_t> samplerSlots;
        for (const auto& binding : ResolveBindings(materials, material))
        {
            if (binding.kind == ResolvedMaterialBinding::Kind::Texture)
            {
                if (!binding.texture.IsValid())
                    outIssues.push_back({ ShaderValidationIssue::Severity::Warning, "texture binding '" + binding.name + "' has invalid texture handle" });
                if (!textureSlots.insert(binding.slot).second)
                    outIssues.push_back({ ShaderValidationIssue::Severity::Error, "duplicate texture slot " + std::to_string(binding.slot) });
            }
            if (binding.kind == ResolvedMaterialBinding::Kind::Sampler)
            {
                if (!samplerSlots.insert(binding.slot).second)
                    outIssues.push_back({ ShaderValidationIssue::Severity::Error, "duplicate sampler slot " + std::to_string(binding.slot) });
            }
        }

        const auto& cbData = const_cast<MaterialSystem&>(materials).GetCBData(material);
        if (!cbData.empty() && (cbData.size() % 16u) != 0u)
            outIssues.push_back({ ShaderValidationIssue::Severity::Error, "per-material constant buffer is not 16-byte aligned" });

        const auto validateShaderContract = [&](ShaderHandle shaderHandle, ShaderStageMask expectedStage, const char* role)
            {
                // Null-Backend: kein Shader-Vertrag vorhanden — Contract-Prüfung überspringen.
                if (m_device && ShaderCompiler::ResolveTargetProfile(*m_device) == assets::ShaderTargetProfile::Null)
                    return;
                if (!m_assets || !shaderHandle.IsValid())
                    return;
                const auto* shaderAsset = m_assets->shaders.Get(shaderHandle);
                if (!shaderAsset)
                    return;
                const auto* artifact = FindCompiledArtifact(*shaderAsset);
                if (!artifact)
                {
                    outIssues.push_back({ ShaderValidationIssue::Severity::Warning, std::string(role) + " has no compiled artifact contract" });
                    return;
                }
                if (!artifact->contract.interfaceLayout.usesEngineBindingModel)
                    outIssues.push_back({ ShaderValidationIssue::Severity::Error, std::string(role) + " does not use the engine binding contract" });
                if ((artifact->contract.stageMask & ToContractStageBits(expectedStage)) == 0u)
                    outIssues.push_back({ ShaderValidationIssue::Severity::Error, std::string(role) + " contract stage mismatch" });
            };

        validateShaderContract(vertexShader, ShaderStageMask::Vertex, "vertex shader");
        if (!isShadowPass)
            validateShaderContract(desc->fragmentShader, ShaderStageMask::Fragment, "fragment shader");

        return std::none_of(outIssues.begin(), outIssues.end(), [](const ShaderValidationIssue& issue) {
            return issue.severity == ShaderValidationIssue::Severity::Error;
            });
    }

    bool ShaderRuntime::PrepareMaterial(const MaterialSystem& materials, MaterialHandle material)
    {
        if (!RequireRenderThread("PrepareMaterial"))
            return false;
        if (!m_device || !material.IsValid())
            return false;

        std::vector<ShaderValidationIssue> issues;
        const bool valid = ValidateMaterial(materials, material, issues);
        const MaterialDesc* desc = materials.GetDesc(material);
        if (!desc)
            return false;

        const bool isShadowPass = desc->passTag == RenderPassTag::Shadow;
        const ShaderHandle sourceVS = (isShadowPass && desc->shadowShader.IsValid())
            ? desc->shadowShader
            : desc->vertexShader;

        ShaderHandle gpuVS = ShaderHandle::Invalid();
        ShaderHandle gpuPS = ShaderHandle::Invalid();

        if (m_assets)
        {
            // Varianten-Flags aus dem Material ableiten: diese steuern welche
            // #defines (KROM_BASECOLOR_MAP, KROM_PBR_METAL_ROUGH usw.) der
            // Shader-Compiler für dieses konkrete Material aktiviert.
            // PrepareShaderAsset (kein #define) wird nur als Fallback genutzt falls
            // GetOrCreateVariant scheitert (z. B. D3DCompile nicht verfügbar).
            ShaderVariantFlag variantFlags = materials.BuildShaderVariantFlags(material);
            // IBL nur aktivieren, wenn ein echtes Environment inklusive BRDF-LUT vorliegt.
            if (m_environment.active && m_environment.irradiance.IsValid() &&
                m_environment.prefiltered.IsValid() && m_environment.brdfLut.IsValid())
            {
                variantFlags = variantFlags | ShaderVariantFlag::IBLMap;
            }

            const ShaderPassType passType = isShadowPass ? ShaderPassType::Shadow
                : ShaderPassType::Main;

            gpuVS = GetOrCreateVariant(sourceVS, passType, variantFlags);
            if (!gpuVS.IsValid())
                gpuVS = PrepareShaderAsset(sourceVS);

            if (desc->fragmentShader.IsValid())
            {
                gpuPS = GetOrCreateVariant(desc->fragmentShader, passType, variantFlags);
                if (!gpuPS.IsValid())
                    gpuPS = PrepareShaderAsset(desc->fragmentShader);
            }
        }
        else
        {
            // Kein AssetRegistry → GPU-Handles direkt aus den Desc-Feldern nutzen.
            gpuVS = sourceVS;
            gpuPS = desc->fragmentShader;
        }

        if (!gpuVS.IsValid() || (!gpuPS.IsValid() && !isShadowPass))
            return false;

        MaterialGpuState next{};
        next.material = material;
        next.vertexShader = gpuVS;
        next.fragmentShader = gpuPS;
        next.bindings = ResolveBindings(materials, material);
        next.issues = issues;
        next.valid = valid;

        const auto& cbData = const_cast<MaterialSystem&>(materials).GetCBData(material);
        next.contentHash = HashMaterialState(cbData, next.bindings);

        const PipelineDesc pipelineDesc = BuildPipelineDesc(materials, material, gpuVS, gpuPS);
        PipelineKey pipelineKey = PipelineKey::From(pipelineDesc, desc->passTag);
        next.pipeline = m_pipelineCache.GetOrCreate(pipelineKey, [&](const PipelineKey&) {
            return m_device->CreatePipeline(pipelineDesc);
            });

        if (!cbData.empty())
        {
            BufferDesc cbDesc{};
            cbDesc.byteSize = cbData.size();
            cbDesc.type = BufferType::Constant;
            cbDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
            cbDesc.access = MemoryAccess::GpuOnly;
            cbDesc.debugName = desc->name + "_PerMaterialCB";
            next.perMaterialCB = m_device->CreateBuffer(cbDesc);
            next.perMaterialCBSize = static_cast<uint32_t>(cbData.size());
            m_device->UploadBufferData(next.perMaterialCB, cbData.data(), cbData.size());
        }

        auto it = m_materialStates.find(material);
        if (it != m_materialStates.end())
            DestroyMaterialState(it->second);
        m_materialStates[material] = next;
        return next.valid && next.pipeline.IsValid();
    }

    bool ShaderRuntime::CommitMaterialRequests(const MaterialSystem& materials,
        const std::vector<MaterialHandle>& requests)
    {
        if (!RequireRenderThread("CommitMaterialRequests"))
            return false;

        bool ok = true;
        for (MaterialHandle handle : requests)
            ok = PrepareMaterial(materials, handle) && ok;
        return ok;
    }

    bool ShaderRuntime::PrepareAllMaterials(const MaterialSystem& materials)
    {
        std::vector<MaterialHandle> requests;
        const bool collected = CollectMaterialRequests(materials, requests);
        return collected && CommitMaterialRequests(materials, requests);
    }

    const MaterialGpuState* ShaderRuntime::GetMaterialState(MaterialHandle material) const noexcept
    {
        if (auto it = m_materialStates.find(material); it != m_materialStates.end())
            return &it->second;
        return nullptr;
    }

    bool ShaderRuntime::BindMaterial(ICommandList& cmd,
        const MaterialSystem& materials,
        MaterialHandle material,
        BufferHandle perFrameCB,
        BufferHandle perObjectCB,
        BufferHandle perPassCB,
        RenderPassTag passOverride)
    {
        if (!RequireRenderThread("BindMaterial"))
            return false;
        auto* state = const_cast<MaterialGpuState*>(GetMaterialState(material));
        if (!state || !state->valid)
        {
            if (!PrepareMaterial(materials, material))
                return false;
            state = const_cast<MaterialGpuState*>(GetMaterialState(material));
            if (!state || !state->valid) return false;
        }

        const PipelineHandle pipeline = ResolvePipelineForPass(materials, material, *state, passOverride);
        if (!pipeline.IsValid())
            return false;

        cmd.SetPipeline(pipeline);
        if (perFrameCB.IsValid())
            cmd.SetConstantBuffer(CBSlots::PerFrame, perFrameCB, kGraphicsStages);
        if (perObjectCB.IsValid())
            cmd.SetConstantBuffer(CBSlots::PerObject, perObjectCB, kGraphicsStages);
        if (state->perMaterialCB.IsValid())
            cmd.SetConstantBuffer(CBSlots::PerMaterial, state->perMaterialCB, kGraphicsStages);
        if (perPassCB.IsValid())
            cmd.SetConstantBuffer(CBSlots::PerPass, perPassCB, kGraphicsStages);

        for (const auto& binding : state->bindings)
        {
            switch (binding.kind)
            {
            case ResolvedMaterialBinding::Kind::Texture:
                if (binding.texture.IsValid())
                {
                    cmd.SetShaderResource(binding.slot, binding.texture, binding.stages);
                }
                break;
            case ResolvedMaterialBinding::Kind::Sampler:
            {
                uint32_t sampler = binding.samplerIndex;
                if (sampler == 0u)
                {
                    switch (binding.slot)
                    {
                    case SamplerSlots::LinearClamp: sampler = m_samplers.linearClamp; break;
                    case SamplerSlots::PointClamp: sampler = m_samplers.pointClamp; break;
                    case SamplerSlots::ShadowPCF: sampler = m_samplers.shadowPCF; break;
                    default: sampler = m_samplers.linearWrap; break;
                    }
                }
                cmd.SetSampler(binding.slot, sampler, binding.stages);
                break;
            }
            case ResolvedMaterialBinding::Kind::ConstantBuffer:
                break;
            }
        }
        return true;
    }

    bool ShaderRuntime::BindMaterialWithRange(ICommandList& cmd,
        const MaterialSystem& materials,
        MaterialHandle material,
        BufferHandle   perFrameCB,
        BufferBinding  perObjectBinding,
        BufferBinding  perPassBinding,
        RenderPassTag  passOverride)
    {
        if (!RequireRenderThread("BindMaterialWithRange"))
            return false;
        auto* state = const_cast<MaterialGpuState*>(GetMaterialState(material));
        if (!state || !state->valid)
        {
            if (!PrepareMaterial(materials, material))
                return false;
            state = const_cast<MaterialGpuState*>(GetMaterialState(material));
            if (!state || !state->valid) return false;
        }

        const PipelineHandle pipeline = ResolvePipelineForPass(materials, material, *state, passOverride);
        if (!pipeline.IsValid())
            return false;

        cmd.SetPipeline(pipeline);
        if (perFrameCB.IsValid())
            cmd.SetConstantBuffer(CBSlots::PerFrame, perFrameCB, kGraphicsStages);
        if (perObjectBinding.IsValid())
            cmd.SetConstantBufferRange(CBSlots::PerObject, perObjectBinding, kGraphicsStages);
        if (state->perMaterialCB.IsValid())
            cmd.SetConstantBuffer(CBSlots::PerMaterial, state->perMaterialCB, kGraphicsStages);
        if (perPassBinding.IsValid())
            cmd.SetConstantBufferRange(CBSlots::PerPass, perPassBinding, kGraphicsStages);

        for (const auto& binding : state->bindings)
        {
            switch (binding.kind)
            {
            case ResolvedMaterialBinding::Kind::Texture:
                if (binding.texture.IsValid())
                {
                    cmd.SetShaderResource(binding.slot, binding.texture, binding.stages);
                }
                break;
            case ResolvedMaterialBinding::Kind::Sampler:
            {
                uint32_t sampler = binding.samplerIndex;
                if (sampler == 0u)
                {
                    switch (binding.slot)
                    {
                    case SamplerSlots::LinearClamp: sampler = m_samplers.linearClamp; break;
                    case SamplerSlots::PointClamp:  sampler = m_samplers.pointClamp;  break;
                    case SamplerSlots::ShadowPCF:   sampler = m_samplers.shadowPCF;   break;
                    default: sampler = m_samplers.linearWrap; break;
                    }
                }
                cmd.SetSampler(binding.slot, sampler, binding.stages);
                break;
            }
            case ResolvedMaterialBinding::Kind::ConstantBuffer:
                break;
            }
        }
        return true;
    }

    void ShaderRuntime::DestroyMaterialState(MaterialGpuState& state)
    {
        if (!m_device) return;
        if (state.perMaterialCB.IsValid())
            m_device->DestroyBuffer(state.perMaterialCB);
        state.perMaterialCB = BufferHandle::Invalid();
        state.perMaterialCBSize = 0u;
    }

} // namespace engine::renderer

