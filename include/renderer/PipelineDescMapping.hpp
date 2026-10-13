#pragma once

// PipelineDescMapping — konzeptioneller Stub für DX12 und Metal.
//
// Dieses Header ist NICHT kompilierbare Produktionscode, sondern eine
// Architekturskizze: Wie würde engine::renderer::PipelineDesc auf die
// nativen Descriptor-Typen von DX12 und Metal abgebildet?
//
// Zweck:
//   - Dokumentiert das Mapping bevor DX12/Metal-Backends existieren
//   - Stellt sicher dass PipelineDesc die nötigen Felder enthält
//   - Dient als Referenz für zukünftige Backend-Autoren
//
// Kompilierungshinweis:
//   Dieser Header ist nur informativ. Er ist in keinem Build-Target registriert
//   und enthält plattform-spezifische Kommentare statt echter Includes.

#include "renderer/RendererTypes.hpp"

namespace engine::renderer::mapping {

// =============================================================================
// DirectX 12 — D3D12_GRAPHICS_PIPELINE_STATE_DESC
// =============================================================================
//
// PipelineDesc-Feld                →  DX12-Feld
// ─────────────────────────────────────────────────────────────────────────────
// shaderStages[Vertex].handle      →  VS (D3D12_SHADER_BYTECODE)
// shaderStages[Fragment].handle    →  PS (D3D12_SHADER_BYTECODE)
//
// rasterizer.cullMode (None)       →  RasterizerState.CullMode = D3D12_CULL_MODE_NONE
// rasterizer.cullMode (Back)       →  RasterizerState.CullMode = D3D12_CULL_MODE_BACK
// rasterizer.cullMode (Front)      →  RasterizerState.CullMode = D3D12_CULL_MODE_FRONT
// rasterizer.frontFace (CCW)       →  RasterizerState.FrontCounterClockwise = TRUE
// rasterizer.depthBias             →  RasterizerState.DepthBias (int32_t, skaliert)
// rasterizer.slopeScaledBias       →  RasterizerState.SlopeScaledDepthBias
//
// depthStencil.depthEnable         →  DepthStencilState.DepthEnable
// depthStencil.depthWrite          →  DepthStencilState.DepthWriteMask (ALL oder ZERO)
// depthStencil.depthFunc (Less)    →  DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS
// depthStencil.depthFunc (LessEq)  →  D3D12_COMPARISON_FUNC_LESS_EQUAL
// depthStencil.depthFunc (Always)  →  D3D12_COMPARISON_FUNC_ALWAYS
//
// blendStates[0].blendEnable       →  BlendState.RenderTarget[0].BlendEnable
// blendStates[0].srcBlend          →  BlendState.RenderTarget[0].SrcBlend
// blendStates[0].dstBlend          →  BlendState.RenderTarget[0].DestBlend
// blendStates[0].srcBlendAlpha     →  BlendState.RenderTarget[0].SrcBlendAlpha
// blendStates[0].dstBlendAlpha     →  BlendState.RenderTarget[0].DestBlendAlpha
//
// vertexLayout.attributes          →  InputLayout (D3D12_INPUT_ELEMENT_DESC[])
// vertexLayout.bindings[i].stride  →  IA stride (per-VertexBufferView)
//
// colorFormat                      →  RTVFormats[0] (DXGI_FORMAT)
// depthFormat                      →  DSVFormat (DXGI_FORMAT)
// sampleCount                      →  SampleDesc.Count
//
// topology (TriangleList)          →  PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
//
// Anmerkung: DX12 trennt PrimitiveTopologyType (im PSO) von PrimitiveTopology
// (im DrawCall). Engine-seitig wird nur TriangleList erwartet, daher kein Mapping-Problem.
//
// Anmerkung: Root Signature ist nicht Teil von PipelineDesc — sie wird vom
// ShaderContract/ShaderBindingModel abgeleitet und separat erstellt.

// =============================================================================
// Metal — MTLRenderPipelineDescriptor + MTLDepthStencilDescriptor
// =============================================================================
//
// PipelineDesc-Feld                →  Metal-API
// ─────────────────────────────────────────────────────────────────────────────
// shaderStages[Vertex].handle      →  vertexFunction (id<MTLFunction>)
// shaderStages[Fragment].handle    →  fragmentFunction (id<MTLFunction>)
//
// colorFormat                      →  colorAttachments[0].pixelFormat
// depthFormat                      →  depthAttachmentPixelFormat
//                                     stencilAttachmentPixelFormat (wenn D24S8)
//
// blendStates[0].blendEnable       →  colorAttachments[0].blendingEnabled
// blendStates[0].srcBlend          →  colorAttachments[0].sourceRGBBlendFactor
// blendStates[0].dstBlend          →  colorAttachments[0].destinationRGBBlendFactor
// blendStates[0].srcBlendAlpha     →  colorAttachments[0].sourceAlphaBlendFactor
// blendStates[0].dstBlendAlpha     →  colorAttachments[0].destinationAlphaBlendFactor
//
// vertexLayout                     →  MTLVertexDescriptor (attributes + layouts)
// vertexLayout.bindings[i].stride  →  layouts[i].stride
// vertexLayout.attributes[j].offset→  attributes[j].offset
// vertexLayout.attributes[j].format→  attributes[j].format (MTLVertexFormat)
//
// sampleCount                      →  rasterSampleCount
//
// -- Depth/Stencil (MTLDepthStencilDescriptor, separat vom Pipeline-Descriptor) --
// depthStencil.depthEnable         →  depthCompareFunction != MTLCompareFunctionNever
// depthStencil.depthWrite          →  depthWriteEnabled
// depthStencil.depthFunc (Less)    →  depthCompareFunction = MTLCompareFunctionLess
// depthStencil.depthFunc (LessEq)  →  MTLCompareFunctionLessEqual
// depthStencil.depthFunc (Always)  →  MTLCompareFunctionAlways
//
// -- Rasterizer (MTLRenderCommandEncoder, pro DrawCall oder via RenderPipelineState) --
// rasterizer.cullMode (None)       →  setCullMode: MTLCullModeNone
// rasterizer.cullMode (Back)       →  setCullMode: MTLCullModeBack
// rasterizer.cullMode (Front)      →  setCullMode: MTLCullModeFront
// rasterizer.frontFace (CCW)       →  setFrontFacingWinding: MTLWindingCounterClockwise
// rasterizer.depthBias             →  setDepthBias:slopeScale:clamp:
// rasterizer.slopeScaledBias       →  (zweiter Parameter von setDepthBias:)
//
// Anmerkung: Metal kombiniert Depth/Stencil-State und Rasterizer-State NICHT in einem
// einzigen Descriptor wie DX12. Der Depth-State ist ein separates MTLDepthStencilState-
// Objekt. Cull-Mode und Bias werden pro Encoder-Befehl gesetzt.
//
// Anmerkung: Metal hat kein explizites "depthEnable"-Flag. Wenn depthCompareFunction
// auf MTLCompareFunctionAlways gesetzt ist und depthWriteEnabled = NO, verhält sich das
// wie depthEnable = false (obwohl der DepthTest technisch noch läuft).
//
// Empfehlung: Wenn depthEnable == false → MTLCompareFunctionAlways + depthWriteEnabled = NO.

// =============================================================================
// BlendFactor-Mapping (DX12 und Metal, gemeinsam)
// =============================================================================
//
// engine::BlendFactor     →  DXGI (D3D12_BLEND_*)      →  Metal (MTLBlendFactor*)
// ────────────────────────────────────────────────────────────────────────────
// Zero                    →  D3D12_BLEND_ZERO           →  MTLBlendFactorZero
// One                     →  D3D12_BLEND_ONE            →  MTLBlendFactorOne
// SrcAlpha                →  D3D12_BLEND_SRC_ALPHA      →  MTLBlendFactorSourceAlpha
// InvSrcAlpha             →  D3D12_BLEND_INV_SRC_ALPHA  →  MTLBlendFactorOneMinusSourceAlpha
// SrcColor                →  D3D12_BLEND_SRC_COLOR      →  MTLBlendFactorSourceColor
// DstColor                →  D3D12_BLEND_DEST_COLOR     →  MTLBlendFactorDestinationColor
// DstAlpha                →  D3D12_BLEND_DEST_ALPHA     →  MTLBlendFactorDestinationAlpha
// InvDstAlpha             →  D3D12_BLEND_INV_DEST_ALPHA →  MTLBlendFactorOneMinusDestinationAlpha

} // namespace engine::renderer::mapping
