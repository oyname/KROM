#include "renderer/MaterialFeatureEval.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include <cstring>

namespace engine::renderer {

ShaderVariantFlag MaterialFeatureEval::BuildShaderVariantFlags(const MaterialDesc& desc,
                                                               const MaterialInstance& inst) noexcept
{
    (void)inst;
    ShaderVariantFlag flags = static_cast<ShaderVariantFlag>(desc.permutationFlags);
    if (desc.renderPolicy.alphaTest)
        flags = flags | ShaderVariantFlag::AlphaTest;
    if (desc.renderPolicy.doubleSided)
        flags = flags | ShaderVariantFlag::DoubleSided;
    return flags;
}

PipelineKey MaterialFeatureEval::BuildPipelineKey(const MaterialDesc& d,
                                                  const MaterialInstance& inst) noexcept
{
    const bool isShadowPass = d.renderPass == StandardRenderPasses::Shadow();
    const ShaderHandle pipelineVS = (isShadowPass && d.shadowShader.IsValid()) ? d.shadowShader : d.vertexShader;

    PipelineDesc pd;
    if (pipelineVS.IsValid())
        pd.shaderStages.push_back({ pipelineVS, ShaderStageMask::Vertex });
    if (!isShadowPass && d.fragmentShader.IsValid())
        pd.shaderStages.push_back({ d.fragmentShader, ShaderStageMask::Fragment });
    pd.rasterizer = d.rasterizer;
    pd.depthStencil = d.depthStencil;
    pd.blendStates[0] = d.blend;
    pd.topology = d.topology;
    pd.vertexLayout = d.vertexLayout;
    pd.colorFormat = isShadowPass ? Format::Unknown : d.colorFormat;
    pd.depthFormat = d.depthFormat;
    pd.shaderContractHash = inst.layout.layoutHash;
    pd.pipelineLayoutHash = inst.layout.layoutHash ^ (static_cast<uint64_t>(d.renderPass.value) << 32u);
    return PipelineKey::From(pd, d.renderPass);
}

void MaterialFeatureEval::NormalizeDesc(MaterialDesc& desc) noexcept
{
    if (desc.doubleSided)
        desc.renderPolicy.doubleSided = true;
    if (!desc.castShadows)
        desc.renderPolicy.castShadows = false;
    desc.renderPolicy.alphaCutoff = desc.alphaCutoff;

    switch (desc.renderPolicy.blendMode)
    {
    case MaterialBlendMode::Opaque:
        desc.blend.blendEnable = false;
        break;
    case MaterialBlendMode::AlphaBlend:
        desc.blend.blendEnable = true;
        desc.blend.srcBlend = BlendFactor::SrcAlpha;
        desc.blend.dstBlend = BlendFactor::InvSrcAlpha;
        desc.blend.srcBlendAlpha = BlendFactor::One;
        desc.blend.dstBlendAlpha = BlendFactor::InvSrcAlpha;
        break;
    case MaterialBlendMode::Additive:
        desc.blend.blendEnable = true;
        desc.blend.srcBlend = BlendFactor::One;
        desc.blend.dstBlend = BlendFactor::One;
        desc.blend.srcBlendAlpha = BlendFactor::One;
        desc.blend.dstBlendAlpha = BlendFactor::One;
        break;
    }

    if (desc.renderPolicy.doubleSided)
    {
        desc.rasterizer.cullMode = CullMode::None;
    }
    else
    {
        switch (desc.renderPolicy.cullMode)
        {
        case MaterialCullMode::Back:  desc.rasterizer.cullMode = CullMode::Back; break;
        case MaterialCullMode::Front: desc.rasterizer.cullMode = CullMode::Front; break;
        case MaterialCullMode::None:  desc.rasterizer.cullMode = CullMode::None; break;
        }
    }
}

} // namespace engine::renderer
