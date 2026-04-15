#include "renderer/ShaderRuntime.hpp"
#include "core/Debug.hpp"
#include <thread>

namespace engine::renderer {

bool ShaderRuntime::IsRenderThread() const noexcept
{
    return m_renderThreadId == std::this_thread::get_id();
}

bool ShaderRuntime::RequireRenderThread(const char* opName) const noexcept
{
    if (IsRenderThread())
        return true;

    Debug::LogError("ShaderRuntime.cpp: %s must run on render thread", opName ? opName : "operation");
    return false;
}

    bool ShaderRuntime::Initialize(IDevice& device)
    {
        m_device = &device;
        m_renderThreadId = std::this_thread::get_id();
        m_shaderAssets.clear();
        m_materialStates.clear();
        m_pipelineCache.Clear();
        m_variantCache.SetUploadFunction(
            [this](const assets::ShaderAsset&, const assets::CompiledShaderArtifact& artifact) -> ShaderHandle
            {
                if (!m_device)
                    return ShaderHandle::Invalid();

                if (!artifact.bytecode.empty())
                {
                    return m_device->CreateShaderFromBytecode(
                        artifact.bytecode.data(),
                        artifact.bytecode.size(),
                        ToStageMask(artifact.stage),
                        artifact.debugName);
                }

                if (!artifact.sourceText.empty())
                {
                    // artifact.sourceText wurde von ShaderCompiler::BuildShaderSource
                    // assembliert und enthält die Defines bereits korrekt positioniert
                    // (nach der #version-Direktive). Würde hier nochmals prepended,
                    // landet #version auf Zeile 2+ → OpenGL-Kompilierfehler C0204.
                    // Deshalb: sourceText unverändert übergeben.
                    return m_device->CreateShaderFromSource(
                        artifact.sourceText,
                        ToStageMask(artifact.stage),
                        artifact.entryPoint,
                        artifact.debugName);
                }

                return ShaderHandle::Invalid();
            });
        CreateDefaultSamplers();
        CreateFallbackTextures();
        return true;
    }

    void ShaderRuntime::Shutdown()
    {
        if (!IsRenderThread())
        {
            Debug::LogWarning("ShaderRuntime.cpp: Shutdown not on render thread after WaitIdle - continuing cleanup");
        }
        if (m_device)
        {
            for (auto& [_, state] : m_materialStates)
                DestroyMaterialState(state);
            for (auto& [_, status] : m_shaderAssets)
            {
                if (status.gpuHandle.IsValid())
                    m_device->DestroyShader(status.gpuHandle);
            }
            m_pipelineCache.ForEach([&](const PipelineKey&, PipelineHandle handle) {
                if (handle.IsValid())
                    m_device->DestroyPipeline(handle);
                });
            if (m_fallbackTextures.white.IsValid()) m_device->DestroyTexture(m_fallbackTextures.white);
            if (m_fallbackTextures.black.IsValid()) m_device->DestroyTexture(m_fallbackTextures.black);
            if (m_fallbackTextures.gray.IsValid()) m_device->DestroyTexture(m_fallbackTextures.gray);
            if (m_fallbackTextures.ormNeutral.IsValid()) m_device->DestroyTexture(m_fallbackTextures.ormNeutral);
            if (m_fallbackTextures.neutralNormal.IsValid()) m_device->DestroyTexture(m_fallbackTextures.neutralNormal);
            if (m_fallbackTextures.iblIrradiance.IsValid()) m_device->DestroyTexture(m_fallbackTextures.iblIrradiance);
            if (m_fallbackTextures.iblPrefiltered.IsValid()) m_device->DestroyTexture(m_fallbackTextures.iblPrefiltered);
            if (m_fallbackTextures.brdfLut.IsValid()) m_device->DestroyTexture(m_fallbackTextures.brdfLut);
        }
        m_environment = {};
        m_materialStates.clear();
        m_shaderAssets.clear();
        m_variantCache.Clear();
        m_pipelineCache.Clear();
        m_device = nullptr;
        m_renderThreadId = std::thread::id{};
    }

} // namespace engine::renderer
