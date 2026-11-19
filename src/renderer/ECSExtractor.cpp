#include "ecs/HierarchyState.hpp"
// =============================================================================
// KROM Engine - src/renderer/ECSExtractor.cpp
// =============================================================================
#include "renderer/ECSExtractor.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"

namespace engine::renderer {

bool ECSExtractor::IsEntityActive(const ecs::World& world, EntityID entity)
{
    return IsEntityActiveInHierarchy(world, entity);
}

} // namespace engine::renderer
