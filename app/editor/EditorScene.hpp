#pragma once

#include "app/AppScene.hpp"
#include "renderer/Environment.hpp"
#include "core/Types.hpp"
#include "core/Math.hpp"

namespace engine::app {

class EditorScene final : public IAppScene
{
public:
    [[nodiscard]] bool Build(AppSceneContext& context) override;
    [[nodiscard]] bool Update(AppSceneContext& context, float deltaSeconds) override;

private:
    EntityID                        m_cameraEntity      = NULL_ENTITY;
    renderer::EnvironmentHandle     m_environmentHandle = renderer::EnvironmentHandle::Invalid();
};

} // namespace engine::app
