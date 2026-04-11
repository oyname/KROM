#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "renderer/ShaderBindingModel.hpp"

namespace engine::renderer {

enum class ShaderTargetApi : uint8_t
{
    Generic = 0,
    Null,
    DirectX11,
    DirectX12,
    Vulkan,
    OpenGL,
};

enum class ShaderBinaryFormat : uint8_t
{
    None = 0,
    SourceText,
    Spirv,
    Dxbc,
    Dxil,
    GlslSource,
};

enum class ShaderBindingClass : uint8_t
{
    ConstantBuffer = 0,
    ShaderResource,
    Sampler,
    UnorderedAccess,
};

struct ShaderBindingDeclaration
{
    std::string        name;
    uint32_t           logicalSlot = 0u;
    uint32_t           apiBinding = 0u;
    uint32_t           space = 0u;
    ShaderBindingClass bindingClass = ShaderBindingClass::ConstantBuffer;
    uint32_t           stageMask = 0u;
};

struct ShaderInterfaceLayout
{
    std::vector<ShaderBindingDeclaration> bindings;
    bool                                  usesEngineBindingModel = false;
    uint64_t                              layoutHash = 0ull;
};

struct PipelineBindingContract
{
    PipelineBindingLayoutDesc   bindingLayout{};
    DescriptorRuntimeLayoutDesc runtimeLayout{};
    PipelineBindingSignatureDesc bindingSignature{};
    uint64_t                    bindingLayoutHash = 0ull;
    uint64_t                    runtimeLayoutHash = 0ull;
    uint64_t                    bindingSignatureHash = 0ull;
    uint64_t                    interfaceLayoutHash = 0ull;
    uint64_t                    bindingSignatureKey = 0ull;
    ComputeRuntimeDesc          computeRuntime{};
    uint64_t                    computeRuntimeHash = 0ull;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return bindingLayoutHash != 0ull || runtimeLayoutHash != 0ull || bindingSignatureHash != 0ull || interfaceLayoutHash != 0ull || bindingSignatureKey != 0ull || computeRuntimeHash != 0ull;
    }
};

using PipelineLayoutContract = PipelineBindingContract;

struct ShaderPipelineContract
{
    ShaderTargetApi         api = ShaderTargetApi::Generic;
    ShaderBinaryFormat      binaryFormat = ShaderBinaryFormat::None;
    uint32_t                stageMask = 0u;
    ShaderInterfaceLayout   interfaceLayout{};
    PipelineBindingContract pipelineBinding{};
    uint64_t                pipelineStateKey = 0ull;
    uint64_t                contractHash = 0ull;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return binaryFormat != ShaderBinaryFormat::None || interfaceLayout.usesEngineBindingModel || pipelineBinding.IsValid();
    }
};

} // namespace engine::renderer
