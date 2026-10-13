// =============================================================================
// KROM Engine - src/renderer/ECSExtractor.cpp
// =============================================================================
#include "renderer/ECSExtractor.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"

namespace engine::renderer {

bool ECSExtractor::IsEntityActive(const ecs::World& world, EntityID entity)
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

} // namespace engine::renderer
