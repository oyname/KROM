#pragma once
#include "core/Types.hpp"
#include "core/Math.hpp"

namespace engine::renderer {

// =============================================================================
// IBL Prefilter constants — shared between CPU (EnvironmentSystem) and the
// FrameConstants packing path (FrameConstantStage).
// kIBLPrefilterMipCount must equal the mip count used when building prefiltered
// environment textures in EnvironmentSystem::UploadPrefilterTexture.
// =============================================================================
static constexpr uint32_t kIBLPrefilterMipCount = 6u;

enum class IBLRuntimeMode : uint32_t
{
    HDR = 0u,
    LDRDiffuseOnly = 1u,
};

// =============================================================================
// EnvironmentMode
//
// None          : no environment active; IBL disabled, ambient term only.
// Texture       : HDR source texture loaded from asset (currently expected as
//                 equirectangular lat-long); runtime builds true cube maps from it.
// ProceduralSky : CPU-generated sky radiance; IBL derived through same pipeline
//                 as Texture mode. No external asset required.
// =============================================================================
enum class EnvironmentMode : uint8_t
{
    None          = 0,
    Texture       = 1,
    ProceduralSky = 2,
};

// =============================================================================
// ProceduralSkyDesc
//
// Parameters for the CPU sky generator (EnvironmentMode::ProceduralSky).
// The generator produces a linear HDR source image that feeds the same
// environment-cube / irradiance-cube / prefilter-cube / BRDF-LUT pipeline as a
// loaded HDR texture.
//
// All color values are linear HDR (no sRGB encoding).
// sunDirection is world-space; it is normalised internally before use.
//
// Defaults produce a blue-sky daylight environment with an overhead sun.
// =============================================================================
struct ProceduralSkyDesc
{
    // Sky gradient: horizon blends to zenith in the upper hemisphere.
    // All three values are linear HDR radiance (can exceed 1.0).
    math::Vec3 zenithColor      = { 0.05f, 0.20f, 0.60f }; // deep blue zenith
    math::Vec3 horizonColor     = { 0.50f, 0.70f, 0.90f }; // pale sky-blue horizon
    math::Vec3 groundColor      = { 0.10f, 0.08f, 0.06f }; // dark ground

    // Sun disc
    math::Vec3 sunDirection     = { 0.0f, 0.8f, 0.6f };    // world-space, normalised internally
    math::Vec3 sunColor         = { 1.8f, 1.5f, 1.2f };    // slightly warm white tint
    float      sunIntensity     = 15.0f;                     // HDR multiplier for the disc
    // Angular radius in radians. Default ~1 deg — slightly larger than the real sun
    // (0.5 deg) to remain visible in IBL convolution at 128x64 px source resolution.
    float      sunAngularRadius = 0.0175f;

    // Controls the rolloff from horizon to zenith:
    //   t = 1 - exp(-elevation * horizonSharpness)
    // Higher values push more sky-blue toward the horizon.
    // Range [0.5, 6]. Default 2.5 gives a natural sky appearance.
    float      horizonSharpness = 2.5f;
};

// =============================================================================
// EnvironmentDesc
//
// Passed to EnvironmentSystem::CreateEnvironment().
// Only the fields relevant to the chosen mode need to be set:
//   Texture       : sourceTexture must be valid; skyDesc is ignored.
//   ProceduralSky : skyDesc is used; sourceTexture is ignored.
//   None          : CreateEnvironment returns an invalid handle immediately.
// =============================================================================
struct EnvironmentDesc
{
    EnvironmentMode   mode          = EnvironmentMode::Texture;
    float             intensity     = 1.0f;   // applied to source radiance before IBL build
    bool              enableIBL     = true;

    // Texture mode
    TextureHandle     sourceTexture = TextureHandle::Invalid();

    // ProceduralSky mode
    ProceduralSkyDesc skyDesc       = {};
};

struct EnvironmentHandle
{
    uint32_t id = 0u;
    [[nodiscard]] bool IsValid() const noexcept { return id != 0u; }
    static EnvironmentHandle Invalid() noexcept { return {}; }
    bool operator==(const EnvironmentHandle&) const noexcept = default;
};

// =============================================================================
// EnvironmentRuntimeState
//
// Snapshot consumed by ShaderRuntime to bind IBL resources each frame.
// Backend-neutral: environment, irradiance, prefiltered and brdfLut are plain
// TextureHandles. The active PBR path samples irradiance/prefiltered as cube maps
// and keeps the environment cube available for future skybox/reflection features.
// =============================================================================
struct EnvironmentRuntimeState
{
    TextureHandle   environment = TextureHandle::Invalid();
    TextureHandle   irradiance  = TextureHandle::Invalid();
    TextureHandle   prefiltered = TextureHandle::Invalid();
    TextureHandle   brdfLut     = TextureHandle::Invalid();
    float           intensity   = 1.0f;
    bool            active      = false;
    EnvironmentMode mode        = EnvironmentMode::None;
    IBLRuntimeMode  iblMode     = IBLRuntimeMode::HDR;
};

} // namespace engine::renderer
