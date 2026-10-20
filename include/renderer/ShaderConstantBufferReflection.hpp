#pragma once

#include "assets/AssetRegistry.hpp"
#include "renderer/MaterialTypes.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine::renderer {

// Liest Feld-Offsets eines Cbuffers aus kompilierten Shadern, unabhaengig vom
// aktiven Backend-Addon und ohne GPU:
//   DXBC   (DX11)   ueber D3DReflect aus d3dcompiler_47.dll
//   SPIR-V (Vulkan) ueber die OpMemberDecorate-Offsets
//   GLSL   (OpenGL) ueber die std140-Regeln des Blocks; std140 legt die Offsets
//                   eindeutig fest, der Treiber meldet dieselben Werte.
// Gesucht wird der Cbuffer per Name (DXBC, GLSL) bzw. per Name oder logischem
// Slot (SPIR-V, DXC benennt den Typ "type.<Name>").

[[nodiscard]] bool ReflectDxbcConstantBuffer(const void* bytecode, size_t byteSize,
                                             std::string_view cbufferName,
                                             ReflectedConstantBuffer& out,
                                             std::string* outError = nullptr);

[[nodiscard]] bool ReflectSpirvConstantBuffer(const void* bytecode, size_t byteSize,
                                              std::string_view cbufferName,
                                              uint32_t logicalSlot,
                                              ReflectedConstantBuffer& out,
                                              std::string* outError = nullptr);

[[nodiscard]] bool ReflectGlslStd140Block(std::string_view source,
                                          std::string_view blockName,
                                          ReflectedConstantBuffer& out,
                                          std::string* outError = nullptr);

// Waehlt den passenden Weg nach artifact.target.
[[nodiscard]] bool ReflectConstantBuffer(const assets::CompiledShaderArtifact& artifact,
                                         std::string_view cbufferName,
                                         uint32_t logicalSlot,
                                         ReflectedConstantBuffer& out,
                                         std::string* outError = nullptr);

// Reflektiert den Cbuffer aus Vertex- und Fragment-Shader und vereinigt die
// Felder. Fehlt ein Artefakt fuer target, wird temporaer kompiliert; die
// ShaderAssets bleiben unveraendert. Liefert false (out.valid = false), wenn
// keine Stufe den Cbuffer reflektieren konnte.
[[nodiscard]] bool ReflectProgramConstantBuffer(const assets::ShaderAsset& vertexShader,
                                                const assets::ShaderAsset& fragmentShader,
                                                assets::ShaderTargetProfile target,
                                                std::string_view cbufferName,
                                                uint32_t logicalSlot,
                                                ReflectedConstantBuffer& out,
                                                std::string* outError = nullptr);

// ---------------------------------------------------------------------------
// Textur-Register
// ---------------------------------------------------------------------------
// Name der Textur im Shader -> logischer Textur-Slot (TexSlots, entspricht tN).
// Fuellt MaterialDesc::textureBindingOverrides, damit eigene Texturen an dem
// Register landen, das der Shader deklariert:
//   DXBC   ueber die Resource-Bindings aus D3DReflect (BindPoint)
//   SPIR-V ueber die Binding-Decoration minus ShaderResourceBase (DXC-Shift)
//   GLSL   ueber layout(binding = N) an der Sampler-Deklaration; Sampler ohne
//          binding fehlen im Ergebnis (die Engine ordnet sie per Name zu).
// Nur Slots < TexSlots::COUNT werden gemeldet.
using ReflectedTextureBindings = std::unordered_map<std::string, uint32_t>;

[[nodiscard]] bool ReflectDxbcTextureBindings(const void* bytecode, size_t byteSize,
                                              ReflectedTextureBindings& out,
                                              std::string* outError = nullptr);

[[nodiscard]] bool ReflectSpirvTextureBindings(const void* bytecode, size_t byteSize,
                                               ReflectedTextureBindings& out,
                                               std::string* outError = nullptr);

[[nodiscard]] bool ReflectGlslTextureBindings(std::string_view source,
                                              ReflectedTextureBindings& out,
                                              std::string* outError = nullptr);

// Waehlt den passenden Weg nach artifact.target.
[[nodiscard]] bool ReflectTextureBindings(const assets::CompiledShaderArtifact& artifact,
                                          ReflectedTextureBindings& out,
                                          std::string* outError = nullptr);

// Vertex- und Fragment-Shader zusammen; fehlt ein Artefakt fuer target, wird
// temporaer kompiliert. Liefert false, wenn keine Stufe reflektiert werden
// konnte (out bleibt dann leer).
[[nodiscard]] bool ReflectProgramTextureBindings(const assets::ShaderAsset& vertexShader,
                                                 const assets::ShaderAsset& fragmentShader,
                                                 assets::ShaderTargetProfile target,
                                                 ReflectedTextureBindings& out,
                                                 std::string* outError = nullptr);

} // namespace engine::renderer
