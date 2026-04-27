#pragma once

#include "assets/AssetRegistry.hpp"
#include "assets/ImageDecoder.hpp"
#include <filesystem>
#include <vector>

namespace engine::assets {

struct TextureImportRequest
{
    std::filesystem::path sourcePath;
    std::vector<uint8_t> fileBytes;
};

class TextureImporter
{
public:
    [[nodiscard]] static bool Import(const TextureImportRequest& request, TextureAsset& outTexture);

private:
    [[nodiscard]] static bool ImportTextTexture(const TextureImportRequest& request, TextureAsset& outTexture);
    [[nodiscard]] static bool ImportDecodedImage(const TextureImportRequest& request, const DecodedImage& image, TextureAsset& outTexture);
    [[nodiscard]] static TextureMetadata InferMetadata(const std::filesystem::path& path, bool isHdr) noexcept;
    [[nodiscard]] static uint16_t Float32ToFloat16(float value) noexcept;
};

}
