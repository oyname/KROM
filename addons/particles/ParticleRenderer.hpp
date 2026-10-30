#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleRenderer.hpp
// GPU-Darstellung des Partikel-Addons. Keine ECS-Abhaengigkeit.
//
// Submit baut aus den Billboards des Frames kameraausgerichtete Quads (CPU,
// 6 Vertices je Partikel) und Draw-Batches je Textur. Prepare legt fehlende
// Texturen ausserhalb des Render-Passes an (Laden ueber den Loader der App).
// Der Partikel-Pass ("frame.particles") zeichnet nach Opaque/Transparent in
// HDRSceneColor: gegen deren implizite Tiefe getestet, ohne Tiefe zu
// schreiben, zuerst alpha-geblendet (von hinten nach vorn), dann additiv.
// Ohne eigene Textur nutzt jeder Partikel eine erzeugte weiche Scheibe.
// =============================================================================

#include "addons/particles/ParticleSystem.hpp"
#include "core/Math.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/RenderPipelineTypes.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::assets {
enum class ShaderTargetProfile : uint8_t;
class AssetPipeline;
class AssetRegistry;
}

namespace engine::particles {

inline constexpr std::string_view kParticleFrameExecutor = "frame.particles";

struct ParticleVertex
{
    math::Vec3 position;
    float u = 0.f, v = 0.f;
    math::Vec4 color;
};

// Bild fuer eine Partikel-Textur: RGBA8, Zeile fuer Zeile von oben.
struct ParticleImage
{
    uint32_t width  = 0u;
    uint32_t height = 0u;
    std::vector<uint8_t> rgba;
    bool srgb = true;   // Farbbild (sRGB); false = lineare Daten
};

// Laedt das Bild zu einem Asset-Pfad; false = nicht ladbar (Partikel nutzen
// dann die weiche Scheibe, der Fehler wird einmal geloggt).
using ParticleTextureLoader = std::function<bool(const std::string& path, ParticleImage& out)>;

// Ein Draw: zusammenhaengende Vertices mit gleicher Mischung und Textur.
struct ParticleDrawBatch
{
    bool        additive = false;
    std::string texture;           // leer = weiche Scheibe
    uint32_t    firstVertex = 0u;
    uint32_t    vertexCount = 0u;
};

class ParticleRenderer
{
public:
    ParticleRenderer() = default;
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    void SetTextureLoader(ParticleTextureLoader loader);

    // viewProjection inkl. Clip-Space-Anpassung des Devices; view fuer die
    // Kamera-Achsen der Billboards.
    void SetCamera(const math::Mat4& viewProjection, const math::Mat4& view) noexcept;
    void Submit(std::span<const ParticleBillboard> alpha,
                std::span<const ParticleBillboard> additive);
    void Clear() noexcept;

    // Nach Submit, vor dem Rendern (ausserhalb eines Render-Passes): laedt und
    // laedt hoch, was an Texturen fuer die anstehenden Batches fehlt.
    void Prepare(renderer::IDevice& device);

    [[nodiscard]] uint32_t PendingParticleCount() const noexcept;
    [[nodiscard]] bool HasPending() const noexcept { return PendingParticleCount() > 0u; }
    [[nodiscard]] std::vector<ParticleDrawBatch> PendingBatches() const;

    void Flush(const renderer::RGExecContext& execCtx);
    void OnDeviceShutdown() noexcept;
    [[nodiscard]] renderer::FramePipelinePassCallback MakeFrameCallback();

private:
    struct GpuResources
    {
        ShaderHandle vertexShader = ShaderHandle::Invalid();
        ShaderHandle fragmentShader = ShaderHandle::Invalid();
        PipelineHandle pipelineAlpha = PipelineHandle::Invalid();
        PipelineHandle pipelineAdditive = PipelineHandle::Invalid();
        BufferHandle vertexBuffer = BufferHandle::Invalid();
        BufferHandle constantsBuffer = BufferHandle::Invalid();
        TextureHandle defaultTexture = TextureHandle::Invalid();   // weiche Scheibe
        uint32_t vertexCapacity = 0u;
        bool initialized = false;
        bool shadersFailed = false;
    };

    void AppendQuads(std::span<const ParticleBillboard> billboards, bool additive);
    bool EnsureGpuResources(renderer::IDevice& device, uint32_t requiredVertices);
    bool EnsureDefaultTexture(renderer::IDevice& device);
    void DestroyGpuResources(renderer::IDevice& device) noexcept;
    [[nodiscard]] TextureHandle ResolveTexture(const std::string& path) const;

    math::Mat4 m_viewProjection = math::Mat4::Identity();
    math::Vec3 m_cameraRight{1.f, 0.f, 0.f};
    math::Vec3 m_cameraUp{0.f, 1.f, 0.f};
    std::vector<ParticleVertex> m_vertices;
    std::vector<ParticleDrawBatch> m_batches;
    ParticleTextureLoader m_loader;
    // Pfad -> GPU-Textur; Invalid = Laden fehlgeschlagen (nicht erneut versuchen).
    std::unordered_map<std::string, TextureHandle> m_textures;
    GpuResources m_gpu{};
    renderer::IDevice* m_device = nullptr;
    mutable std::mutex m_mutex;
};

// Kompiliert beide HLSL-Stufen fuer ein Ziel ohne Device (fuer Tests).
[[nodiscard]] bool CompileParticleShaders(assets::ShaderTargetProfile target, std::string* error);

// Laedt ein Bild (Pfad relativ zum Asset-Root) ueber die Asset-Pipeline als
// RGBA8; false bei Ladefehler oder anderem Format (z. B. HDR).
[[nodiscard]] bool LoadParticleImage(assets::AssetPipeline& pipeline, const assets::AssetRegistry& registry,
                                     const std::string& path, ParticleImage& out);

// Erzeugt die weiche Scheibe (Standard-Textur) als RGBA8 size x size.
[[nodiscard]] ParticleImage MakeSoftDiscImage(uint32_t size);

// Registriert den Partikel-Pass; ohne Callback baut die Pipeline ihn nicht.
inline void AddParticleCallback(renderer::FramePipelineCallbacks& callbacks, ParticleRenderer& particles)
{
    callbacks.Register(kParticleFrameExecutor, particles.MakeFrameCallback());
}

} // namespace engine::particles
