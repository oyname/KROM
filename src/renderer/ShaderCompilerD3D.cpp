#include "ShaderCompilerInternal.hpp"
#ifdef _WIN32
#   include <windows.h>
#   include <d3dcompiler.h>
#endif

namespace engine::renderer::internal {
#ifdef _WIN32
using D3DCompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn ResolveD3DCompile(std::string* outError)
{
    static D3DCompileFn fn = nullptr;
    static bool attempted = false;
    if (attempted)
    {
        if (!fn)
            SetError(outError, "failed to load D3DCompile from d3dcompiler DLL");
        return fn;
    }

    attempted = true;
    const wchar_t* dllNames[] = { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" };
    for (const wchar_t* dllName : dllNames)
    {
        HMODULE module = ::LoadLibraryW(dllName);
        if (!module)
            continue;
        fn = reinterpret_cast<D3DCompileFn>(::GetProcAddress(module, "D3DCompile"));
        if (fn)
            return fn;
    }

    SetError(outError, "failed to load D3DCompile from d3dcompiler_47/46/43.dll");
    return nullptr;
}
#endif

#ifdef _WIN32
std::string GetHlslTargetProfile(assets::ShaderStage stage, assets::ShaderTargetProfile target)
{
    const char* suffix = "vs_5_0";
    switch (stage)
    {
    case assets::ShaderStage::Vertex: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "vs_6_0" : "vs_5_0"; break;
    case assets::ShaderStage::Fragment: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "ps_6_0" : "ps_5_0"; break;
    case assets::ShaderStage::Compute: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "cs_6_0" : "cs_5_0"; break;
    case assets::ShaderStage::Geometry: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "gs_6_0" : "gs_5_0"; break;
    case assets::ShaderStage::Hull: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "hs_6_0" : "hs_5_0"; break;
    case assets::ShaderStage::Domain: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "ds_6_0" : "ds_5_0"; break;
    }
    return suffix;
}
#endif

bool CompileToD3DBytecode(const assets::ShaderAsset& asset,
                          assets::ShaderTargetProfile target,
                          const SourceBundle& bundle,
                          const std::vector<std::string>& defines,
                          assets::CompiledShaderArtifact& outCompiled,
                          std::string* outError)
{
#ifdef _WIN32
    if (asset.sourceLanguage != assets::ShaderSourceLanguage::HLSL)
    {
        SetError(outError, "DirectX shader compilation requires HLSL shader sources");
        return false;
    }

    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(defines.size() + 1u);
    for (const auto& def : defines)
    {
        D3D_SHADER_MACRO macro{};
        macro.Name = def.c_str();
        macro.Definition = "1";
        macros.push_back(macro);
    }
    macros.push_back({ nullptr, nullptr });

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const D3DCompileFn d3dCompile = ResolveD3DCompile(outError);
    if (!d3dCompile)
        return false;

    const std::string sourceName = bundle.canonicalSourcePath.empty() ? (asset.debugName.empty() ? asset.path : asset.debugName) : bundle.canonicalSourcePath.string();
    const HRESULT hr = d3dCompile(bundle.preprocessedSource.data(),
                                  bundle.preprocessedSource.size(),
                                  sourceName.c_str(),
                                  macros.data(),
                                  nullptr,
                                  asset.entryPoint.empty() ? "main" : asset.entryPoint.c_str(),
                                  GetHlslTargetProfile(asset.stage, target).c_str(),
                                  flags,
                                  0u,
                                  &code,
                                  &errors);
    if (FAILED(hr) || !code)
    {
        std::string error = "D3DCompile failed";
        if (errors && errors->GetBufferPointer())
            error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        if (errors) errors->Release();
        if (code) code->Release();
        SetError(outError, error);
        return false;
    }

    outCompiled.bytecode.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
                                static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
    outCompiled.sourceText.clear();
    if (errors) errors->Release();
    code->Release();
    return true;
#else
    (void)asset; (void)target; (void)bundle; (void)defines; (void)outCompiled;
    SetError(outError, "DirectX bytecode compilation is only available on Windows builds");
    return false;
#endif
}

} // namespace engine::renderer::internal
