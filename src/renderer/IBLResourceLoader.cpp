#include "renderer/IBLResourceLoader.hpp"
#include "renderer/RendererTypes.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cstdint>

namespace engine::renderer {

    IBLGpuResources IBLResourceLoader::Upload(IDevice& device, const IBLBakedData& data)
    {
        if (!data.IsValid())
        {
            Debug::LogError("IBLResourceLoader: received invalid IBLBakedData");
            return {};
        }

        IBLGpuResources res{};

        // -----------------------------------------------------------------
        // Environment cube (mip 0 only, used for skybox / reflection source)
        // -----------------------------------------------------------------
        {
            TextureDesc desc{};
            desc.width        = data.environmentSize;
            desc.height       = data.environmentSize;
            desc.arraySize    = 6u;
            desc.mipLevels    = 1u;
            desc.dimension    = TextureDimension::Cubemap;
            desc.format       = Format::RGBA16_FLOAT;
            desc.usage        = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName    = "IBL_Environment";
            res.environment = device.CreateTexture(desc);
            if (!res.environment.IsValid())
            {
                Debug::LogError("IBLResourceLoader: CreateTexture failed for environment cube");
                Destroy(device, res);
                return {};
            }
            const size_t faceBytes = static_cast<size_t>(data.environmentSize)
                                   * data.environmentSize * 4u * sizeof(uint16_t);
            for (uint32_t face = 0u; face < 6u; ++face)
            {
                device.UploadTextureData(res.environment,
                    data.environmentData.data() + face * faceBytes,
                    faceBytes, 0u, face);
            }
        }

        // -----------------------------------------------------------------
        // Irradiance cube (diffuse IBL)
        // -----------------------------------------------------------------
        {
            TextureDesc desc{};
            desc.width        = data.irradianceSize;
            desc.height       = data.irradianceSize;
            desc.arraySize    = 6u;
            desc.mipLevels    = 1u;
            desc.dimension    = TextureDimension::Cubemap;
            desc.format       = Format::RGBA16_FLOAT;
            desc.usage        = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName    = "IBL_Irradiance";
            res.irradiance = device.CreateTexture(desc);
            if (!res.irradiance.IsValid())
            {
                Debug::LogError("IBLResourceLoader: CreateTexture failed for irradiance cube");
                Destroy(device, res);
                return {};
            }
            const size_t faceBytes = static_cast<size_t>(data.irradianceSize)
                                   * data.irradianceSize * 4u * sizeof(uint16_t);
            for (uint32_t face = 0u; face < 6u; ++face)
            {
                device.UploadTextureData(res.irradiance,
                    data.irradianceData.data() + face * faceBytes,
                    faceBytes, 0u, face);
            }
        }

        // -----------------------------------------------------------------
        // Prefilter cube (specular IBL, mip chain)
        // Data layout: [mip0: 6 faces][mip1: 6 faces]...
        // -----------------------------------------------------------------
        {
            TextureDesc desc{};
            desc.width        = data.prefilterBaseSize;
            desc.height       = data.prefilterBaseSize;
            desc.arraySize    = 6u;
            desc.mipLevels    = data.prefilterMipCount;
            desc.dimension    = TextureDimension::Cubemap;
            desc.format       = Format::RGBA16_FLOAT;
            desc.usage        = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
            desc.initialState = ResourceState::ShaderRead;
            desc.debugName    = "IBL_Prefilter";
            res.prefiltered = device.CreateTexture(desc);
            if (!res.prefiltered.IsValid())
            {
                Debug::LogError("IBLResourceLoader: CreateTexture failed for prefilter cube");
                Destroy(device, res);
                return {};
            }

            size_t   mipOffset = 0u;
            uint32_t mipSize   = data.prefilterBaseSize;
            for (uint32_t mip = 0u; mip < data.prefilterMipCount; ++mip)
            {
                const size_t faceBytes = static_cast<size_t>(mipSize) * mipSize * 4u * sizeof(uint16_t);
                for (uint32_t face = 0u; face < 6u; ++face)
                {
                    device.UploadTextureData(res.prefiltered,
                        data.prefilterData.data() + mipOffset + face * faceBytes,
                        faceBytes, mip, face);
                }
                mipOffset += 6u * faceBytes;
                mipSize    = std::max(mipSize / 2u, 1u);
            }
        }

        return res;
    }

    void IBLResourceLoader::Destroy(IDevice& device, IBLGpuResources& res)
    {
        if (res.environment.IsValid()) { device.DestroyTexture(res.environment); res.environment = TextureHandle::Invalid(); }
        if (res.irradiance.IsValid())  { device.DestroyTexture(res.irradiance);  res.irradiance  = TextureHandle::Invalid(); }
        if (res.prefiltered.IsValid()) { device.DestroyTexture(res.prefiltered); res.prefiltered = TextureHandle::Invalid(); }
    }

} // namespace engine::renderer
