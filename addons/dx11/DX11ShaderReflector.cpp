#include "DX11ShaderReflector.hpp"
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include "core/Debug.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <d3d11.h>
#   include <d3dcompiler.h>
#endif

namespace engine::renderer::dx11 {

namespace {

struct ReflectBytes
{
    const void* data = nullptr;
    size_t      size = 0u;
};

ShaderStageMask ToStageMask(assets::ShaderStage stage) noexcept
{
    switch (stage)
    {
    case assets::ShaderStage::Vertex: return ShaderStageMask::Vertex;
    case assets::ShaderStage::Fragment: return ShaderStageMask::Fragment;
    case assets::ShaderStage::Compute: return ShaderStageMask::Compute;
    case assets::ShaderStage::Geometry: return ShaderStageMask::Geometry;
    case assets::ShaderStage::Hull: return ShaderStageMask::Hull;
    case assets::ShaderStage::Domain: return ShaderStageMask::Domain;
    default: return ShaderStageMask::None;
    }
}

const assets::CompiledShaderArtifact* FindDX11Artifact(const assets::ShaderAsset& shader) noexcept
{
    auto it = std::find_if(shader.compiledArtifacts.begin(), shader.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == assets::ShaderTargetProfile::DirectX11_SM5 &&
               artifact.stage == shader.stage &&
               !artifact.bytecode.empty();
    });
    return it != shader.compiledArtifacts.end() ? &(*it) : nullptr;
}

ReflectBytes ResolveBytes(const assets::ShaderAsset& shader) noexcept
{
    if (const auto* artifact = FindDX11Artifact(shader))
        return { artifact->bytecode.data(), artifact->bytecode.size() };
    if (!shader.bytecode.empty())
        return { shader.bytecode.data(), shader.bytecode.size() };
    return {};
}

bool AddOrMergeSlot(ShaderParameterLayout& layout, const ParameterSlot& candidate) noexcept
{
    if (!candidate.IsValid())
        return false;

    for (uint32_t i = 0u; i < layout.slotCount; ++i)
    {
        ParameterSlot& existing = layout.slots[i];
        const bool sameBinding = existing.type == candidate.type &&
                                 existing.binding == candidate.binding &&
                                 existing.set == candidate.set;
        const bool sameName = existing.Name() == candidate.Name() && candidate.Name() != std::string_view{};
        if (!sameBinding && !sameName)
            continue;

        existing.stageFlags = existing.stageFlags | candidate.stageFlags;
        existing.byteOffset = existing.byteOffset == 0u ? candidate.byteOffset : existing.byteOffset;
        existing.byteSize = (std::max)(existing.byteSize, candidate.byteSize);
        existing.elementCount = (std::max)(existing.elementCount, candidate.elementCount);
        if (existing.name[0] == '\0' && candidate.name[0] != '\0')
            existing.SetName(candidate.Name());
        layout.RecalculateHash();
        return true;
    }

    return layout.AddSlot(candidate);
}

bool ReflectSingle(const assets::ShaderAsset& shader,
                   ShaderParameterLayout& outLayout,
                   std::string* outError)
{
#ifndef _WIN32
    (void)shader;
    outLayout.Clear();
    if (outError)
        *outError = "DX11 reflection is only available on Windows";
    return false;
#else
    const ReflectBytes bytes = ResolveBytes(shader);
    if (!bytes.data || bytes.size == 0u)
    {
        outLayout.Clear();
        if (outError)
            *outError = "shader has no DX11 bytecode for reflection";
        return false;
    }

    ID3D11ShaderReflection* reflection = nullptr;
    const HRESULT hr = D3DReflect(bytes.data, bytes.size, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&reflection));
    if (FAILED(hr) || !reflection)
    {
        outLayout.Clear();
        if (outError)
            *outError = "D3DReflect failed";
        return false;
    }

    D3D11_SHADER_DESC shaderDesc{};
    if (FAILED(reflection->GetDesc(&shaderDesc)))
    {
        reflection->Release();
        outLayout.Clear();
        if (outError)
            *outError = "ID3D11ShaderReflection::GetDesc failed";
        return false;
    }

    ShaderParameterLayout layout{};
    const ShaderStageMask stageMask = ToStageMask(shader.stage);

    for (UINT i = 0u; i < shaderDesc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bindDesc{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &bindDesc)))
            continue;

        ParameterSlot slot{};
        slot.SetName(bindDesc.Name ? bindDesc.Name : "");
        slot.binding = bindDesc.BindPoint;
        slot.set = 0u;
        slot.stageFlags = stageMask;
        slot.elementCount = std::max<UINT>(bindDesc.BindCount, 1u);

        switch (bindDesc.Type)
        {
        case D3D_SIT_CBUFFER:
        {
            slot.type = ParameterType::ConstantBuffer;
            if (ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByName(bindDesc.Name))
            {
                D3D11_SHADER_BUFFER_DESC cbDesc{};
                if (SUCCEEDED(cb->GetDesc(&cbDesc)))
                    slot.byteSize = cbDesc.Size;
            }
            break;
        }
        case D3D_SIT_TEXTURE:
            slot.type = (bindDesc.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE)
                ? ParameterType::TextureCube
                : ParameterType::Texture2D;
            break;
        case D3D_SIT_SAMPLER:
            slot.type = ParameterType::Sampler;
            break;
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
        case D3D_SIT_TBUFFER:
            slot.type = ParameterType::StructuredBuffer;
            break;
        default:
            continue;
        }

        if (!AddOrMergeSlot(layout, slot))
        {
            reflection->Release();
            outLayout.Clear();
            if (outError)
                *outError = "shader parameter layout is full while reflecting DX11 shader";
            return false;
        }
    }

    reflection->Release();
    outLayout = layout;
    if (!outLayout.IsValid())
    {
        if (outError)
            *outError = "DX11 shader reflection found no material-relevant bindings";
        return false;
    }
    return true;
#endif
}

} // namespace

bool DX11ShaderReflector::Reflect(const assets::ShaderAsset& shader,
                                  ShaderParameterLayout& outLayout,
                                  std::string* outError) const
{
    return ReflectSingle(shader, outLayout, outError);
}

bool DX11ShaderReflector::ReflectProgram(const assets::ShaderAsset& vertexShader,
                                         const assets::ShaderAsset& fragmentShader,
                                         ShaderParameterLayout& outLayout,
                                         std::string* outError) const
{
    outLayout.Clear();

    ShaderParameterLayout vertexLayout{};
    if (!ReflectSingle(vertexShader, vertexLayout, outError))
        return false;

    for (uint32_t i = 0u; i < vertexLayout.slotCount; ++i)
    {
        if (!AddOrMergeSlot(outLayout, vertexLayout.slots[i]))
        {
            if (outError)
                *outError = "failed to merge reflected DX11 vertex layout";
            outLayout.Clear();
            return false;
        }
    }

    ShaderParameterLayout fragmentLayout{};
    if (!ReflectSingle(fragmentShader, fragmentLayout, outError))
        return false;

    for (uint32_t i = 0u; i < fragmentLayout.slotCount; ++i)
    {
        if (!AddOrMergeSlot(outLayout, fragmentLayout.slots[i]))
        {
            if (outError)
                *outError = "failed to merge reflected DX11 fragment layout";
            outLayout.Clear();
            return false;
        }
    }

    outLayout.RecalculateHash();
    return outLayout.IsValid();
}

} // namespace engine::renderer::dx11
