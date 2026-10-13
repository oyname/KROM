#pragma once

#include "renderer/MaterialTypes.hpp"
#include "renderer/RendererTypes.hpp"
#include "renderer/RenderPassRegistry.hpp"

namespace engine::renderer {

struct MaterialRuntimeDesc
{
    RenderPassID renderPass = StandardRenderPasses::Opaque();

    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle shadowShader;
    ShaderHandle shadowFragmentShader; // Nur für Alpha-Test (KROM_ALPHA_TEST-Variante)

    RasterizerState    rasterizer;
    DepthStencilState  depthStencil;
    BlendState         blend;
    PrimitiveTopology  topology = PrimitiveTopology::TriangleList;
    VertexLayout       vertexLayout;

    Format colorFormat = Format::RGBA16_FLOAT;
    Format depthFormat = Format::D24_UNORM_S8_UINT;
};

} // namespace engine::renderer
