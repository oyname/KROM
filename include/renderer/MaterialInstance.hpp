#pragma once

#include "renderer/MaterialTypes.hpp"
#include "renderer/ParameterBlob.hpp"

namespace engine::renderer {

struct MaterialInstance
{
    MaterialHandle              materialDefinition = MaterialHandle::Invalid();
    ParameterBlob               parameters{};
    std::vector<MaterialParam>  parameterOverrides;
    MaterialFeatureFlags        featureOverrides = MaterialFeatureFlags::None;
    bool                        dirty = true;
    uint64_t                    revision = 1ull;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return materialDefinition.IsValid();
    }

    [[nodiscard]] float* GetFloatPtr(const std::string& name) noexcept;
};

} // namespace engine::renderer
