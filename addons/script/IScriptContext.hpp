#pragma once
#include "core/Math.hpp"
#include "core/Types.hpp"
#include <string_view>

namespace engine::script {

class IScriptContext
{
public:
    virtual ~IScriptContext() = default;

    [[nodiscard]] virtual float    DeltaTime() const noexcept = 0;
    [[nodiscard]] virtual EntityID GetEntity() const noexcept = 0;
    [[nodiscard]] virtual EntityID InstantiatePrefab(
        std::string_view prefabAssetPath,
        const math::Vec3& position = {0.f, 0.f, 0.f},
        const math::Quat& rotation = math::Quat::Identity(),
        const math::Vec3& scale = {1.f, 1.f, 1.f}) = 0;
};

} // namespace engine::script
