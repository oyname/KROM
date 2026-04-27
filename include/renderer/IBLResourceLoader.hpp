#pragma once
// =============================================================================
// KROM Engine - renderer/IBLResourceLoader.hpp
//
// Runtime Loader layer: takes RGBA16F-packed IBLBakedData and creates GPU
// textures via the backend-neutral IDevice interface.
//
// Uses Format::RGBA16_FLOAT for all cube maps (instead of RGBA32_FLOAT),
// halving memory bandwidth and upload time with no visual difference for IBL.
// TextureDimension::Cubemap is used consistently across all three maps.
// =============================================================================
#include "renderer/IBLTypes.hpp"
#include "renderer/IDevice.hpp"

namespace engine::renderer {

struct IBLGpuResources
{
    TextureHandle environment = TextureHandle::Invalid();
    TextureHandle irradiance  = TextureHandle::Invalid();
    TextureHandle prefiltered = TextureHandle::Invalid();

    [[nodiscard]] bool IsValid() const noexcept
    {
        return environment.IsValid() && irradiance.IsValid() && prefiltered.IsValid();
    }
};

class IBLResourceLoader
{
public:
    // Upload RGBA16F-packed baked data to the GPU via IDevice.
    // Returns resources with invalid handles on any allocation failure.
    [[nodiscard]] static IBLGpuResources Upload(IDevice& device, const IBLBakedData& data);

    // Destroy all GPU handles. Sets them to Invalid after destruction.
    static void Destroy(IDevice& device, IBLGpuResources& res);
};

} // namespace engine::renderer
