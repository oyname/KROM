#include "ShaderCompilerInternal.hpp"
#include "core/Debug.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#   include <process.h>
#   include <windows.h>
#   include <fileapi.h>
#   include <d3dcompiler.h>
#endif

namespace engine::renderer::internal {
#ifdef _WIN32
using D3DCompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static std::filesystem::path FindFxcExe()
{
    namespace fs = std::filesystem;

    if (const char* explicitPath = std::getenv("FXC_PATH"))
    {
        const fs::path candidate(explicitPath);
        if (fs::exists(candidate))
            return candidate;
    }

    const auto trySdkRoot = [](const fs::path& sdkRoot) -> fs::path
    {
        std::error_code ec;
        const fs::path binRoot = sdkRoot / "bin";
        if (!fs::exists(binRoot, ec))
            return {};

        fs::path bestMatch;
        for (const auto& entry : fs::directory_iterator(binRoot, ec))
        {
            if (ec || !entry.is_directory())
                continue;

            const fs::path versionDir = entry.path();
            for (const wchar_t* arch : { L"x64", L"x86" })
            {
                const fs::path candidate = versionDir / arch / "fxc.exe";
                if (fs::exists(candidate, ec))
                {
                    if (bestMatch.empty() || candidate.wstring() > bestMatch.wstring())
                        bestMatch = candidate;
                }
            }
        }
        return bestMatch;
    };

    if (const char* sdkDir = std::getenv("WindowsSdkDir"))
    {
        const fs::path candidate = trySdkRoot(fs::path(sdkDir));
        if (!candidate.empty())
            return candidate;
    }

    if (const char* programFilesX86 = std::getenv("ProgramFiles(x86)"))
    {
        const fs::path candidate = trySdkRoot(fs::path(programFilesX86) / "Windows Kits" / "10");
        if (!candidate.empty())
            return candidate;
    }

    if (const wchar_t* pathVar = _wgetenv(L"PATH"))
    {
        std::wstring pathList(pathVar);
        size_t start = 0u;
        while (start <= pathList.size())
        {
            const size_t end = pathList.find(L';', start);
            const std::wstring segment = pathList.substr(start,
                end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!segment.empty())
            {
                const fs::path candidate = fs::path(segment) / "fxc.exe";
                if (fs::exists(candidate))
                    return candidate;
            }
            if (end == std::wstring::npos)
                break;
            start = end + 1u;
        }
    }

    return {};
}

// Gibt den 8.3-Kurzpfad ohne Leerzeichen zurück (für _wspawnv-Kompatibilität).
// Falls GetShortPathNameW fehlschlägt (z.B. Datei existiert noch nicht), wird der Originalpfad zurückgegeben.
static std::wstring ToShortPath(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    wchar_t shortBuf[MAX_PATH];
    const DWORD len = ::GetShortPathNameW(wide.c_str(), shortBuf, MAX_PATH);
    return (len > 0u && len < MAX_PATH) ? std::wstring(shortBuf, len) : wide;
}

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
    std::vector<std::wstring> dllPaths;

    wchar_t exePath[MAX_PATH] = {};
    const DWORD exeLen = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLen > 0u && exeLen < MAX_PATH)
    {
        std::filesystem::path exeDir(exePath);
        exeDir = exeDir.parent_path();
        dllPaths.push_back((exeDir / L"d3dcompiler_47.dll").wstring());
        dllPaths.push_back((exeDir / L"D3DCompiler_47.dll").wstring());
    }

    dllPaths.push_back(L"d3dcompiler_47.dll");
    dllPaths.push_back(L"D3DCompiler_47.dll");
    dllPaths.push_back(L"d3dcompiler_46.dll");
    dllPaths.push_back(L"d3dcompiler_43.dll");

    for (const std::wstring& dllPath : dllPaths)
    {
        HMODULE module = ::LoadLibraryW(dllPath.c_str());
        if (!module)
            continue;
        fn = reinterpret_cast<D3DCompileFn>(::GetProcAddress(module, "D3DCompile"));
        if (fn)
            return fn;
    }

    SetError(outError, "failed to load D3DCompile from d3dcompiler_47/46/43.dll");
    return nullptr;
}

std::string GetHlslTargetProfile(assets::ShaderStage stage, assets::ShaderTargetProfile target)
{
    const char* suffix = "vs_5_0";
    switch (stage)
    {
    case assets::ShaderStage::Vertex:   suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "vs_6_0" : "vs_5_0"; break;
    case assets::ShaderStage::Fragment: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "ps_6_0" : "ps_5_0"; break;
    case assets::ShaderStage::Compute:  suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "cs_6_0" : "cs_5_0"; break;
    case assets::ShaderStage::Geometry: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "gs_6_0" : "gs_5_0"; break;
    case assets::ShaderStage::Hull:     suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "hs_6_0" : "hs_5_0"; break;
    case assets::ShaderStage::Domain:   suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "ds_6_0" : "ds_5_0"; break;
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
    using Clock = std::chrono::steady_clock;
    if (asset.sourceLanguage != assets::ShaderSourceLanguage::HLSL)
    {
        SetError(outError, "DirectX shader compilation requires HLSL shader sources");
        return false;
    }

    namespace fs = std::filesystem;

    const std::string sourceText = BuildShaderSource(bundle, defines);
    if (sourceText.empty())
    {
        SetError(outError, "failed to build HLSL source for DirectX shader compilation");
        return false;
    }

    const std::string entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    const std::string targetProfile = GetHlslTargetProfile(asset.stage, target);

    // Prefer the in-process D3DCompile path for DX11/DXBC.
    //
    // The previous code invoked fxc.exe first and wrote temporary .hlsl/.dxbc files
    // below %TEMP%\krom_fxc_dxbc via /Fo. fxc.exe prints messages like
    // "compilation object save succeeded; see ..." for those files, which polluted
    // the debug output and made DX11 behave differently from the Vulkan path.
    //
    // Normal DX11 builds now compile in-process and do not save temporary DXBC
    // objects to disk.

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1;

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const D3DCompileFn d3dCompile = ResolveD3DCompile(outError);
    if (!d3dCompile)
        return false;

    const std::string sourceName = bundle.canonicalSourcePath.empty()
        ? (asset.debugName.empty() ? asset.path : asset.debugName)
        : bundle.canonicalSourcePath.string();
    std::vector<D3D_SHADER_MACRO> macros;
    macros.push_back({ nullptr, nullptr });

    const auto d3dCompileStart = Clock::now();
    const HRESULT hr = d3dCompile(sourceText.data(),
                                  sourceText.size(),
                                  sourceName.c_str(),
                                  macros.data(),
                                  nullptr,
                                  entryPoint.c_str(),
                                  targetProfile.c_str(),
                                  flags,
                                  0u,
                                  &code,
                                  &errors);
    const auto d3dCompileMs = std::chrono::duration<double, std::milli>(Clock::now() - d3dCompileStart).count();
    if (FAILED(hr) || !code)
    {
        std::string error = "D3DCompile failed";
        if (errors && errors->GetBufferPointer())
            error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        Debug::LogError("ShaderCompile DX11: FAIL shader='%s' stage=%u defines=%zu time=%.2f ms error=%s",
            asset.debugName.empty() ? asset.path.c_str() : asset.debugName.c_str(),
            static_cast<unsigned>(asset.stage),
            defines.size(),
            d3dCompileMs,
            error.c_str());
        if (errors) errors->Release();
        if (code) code->Release();
        SetError(outError, error);
        return false;
    }

    outCompiled.bytecode.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
                                static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
    outCompiled.sourceText.clear();
    Debug::Log("ShaderCompile DX11: OK shader='%s' stage=%u defines=%zu source=%zu bytes=%zu time=%.2f ms",
        asset.debugName.empty() ? asset.path.c_str() : asset.debugName.c_str(),
        static_cast<unsigned>(asset.stage),
        defines.size(),
        sourceText.size(),
        outCompiled.bytecode.size(),
        d3dCompileMs);
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
