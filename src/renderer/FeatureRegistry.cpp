#include "renderer/FeatureRegistry.hpp"
#include "core/Debug.hpp"
#include <queue>
#include <unordered_map>

namespace engine::renderer {

void FeatureRegistrationContext::RegisterSceneExtractionStep(SceneExtractionStepPtr step) const noexcept
{
    registry.RegisterSceneExtractionStep(owner, std::move(step));
}

void FeatureRegistrationContext::RegisterFrameConstantsContributor(FrameConstantsContributorPtr contributor) const noexcept
{
    registry.RegisterFrameConstantsContributor(owner, std::move(contributor));
}

void FeatureRegistrationContext::RegisterRenderPipeline(RenderPipelinePtr pipeline, bool makeDefault) const noexcept
{
    registry.RegisterRenderPipeline(owner, std::move(pipeline), makeDefault);
}

bool FeatureRegistry::AddFeature(std::unique_ptr<IEngineFeature> feature)
{
    if (!feature)
        return false;
    if (m_initialized)
    {
        Debug::LogError("FeatureRegistry: cannot add feature '%s' after initialization",
                        std::string(feature->GetName()).c_str());
        return false;
    }

    const FeatureID id = feature->GetID();
    const std::string_view name = feature->GetName();
    if (!id.IsValid())
    {
        Debug::LogError("FeatureRegistry: feature '%s' has invalid FeatureID",
                        std::string(name).c_str());
        return false;
    }

    const auto existingIt = m_featuresById.find(id);
    if (existingIt != m_featuresById.end())
    {
        const std::string_view existingName = GetRegisteredFeatureName(id);
        if (existingName == name)
        {
            Debug::LogError("FeatureRegistry: duplicate feature registration for '%s' (id=%u)",
                            std::string(name).c_str(),
                            id.value);
        }
        else
        {
            Debug::LogError("FeatureRegistry: FeatureID collision for '%s' (id=%u already used by '%s')",
                            std::string(name).c_str(),
                            id.value,
                            std::string(existingName).c_str());
        }
        return false;
    }

    m_featureNamesById.emplace(id, name);
    m_featuresById.emplace(id, feature.get());
    m_features.push_back(std::move(feature));
    return true;
}

bool FeatureRegistry::TopologicallySorted(std::vector<IEngineFeature*>& outSorted) const
{
    std::unordered_map<FeatureID, uint32_t> inDegree;
    std::unordered_map<FeatureID, std::vector<FeatureID>> dependents;

    for (const auto& f : m_features)
        inDegree.emplace(f->GetID(), 0u);

    for (const auto& f : m_features)
    {
        const FeatureID featureId = f->GetID();
        for (const FeatureID dep : f->GetDependencies())
        {
            const auto depIt = m_featuresById.find(dep);
            if (depIt == m_featuresById.end())
            {
                Debug::LogError("FeatureRegistry: feature '%s' depends on unknown feature id=%u",
                                std::string(f->GetName()).c_str(), dep.value);
                return false;
            }
            dependents[dep].push_back(featureId);
            ++inDegree[featureId];
        }
    }

    std::queue<FeatureID> ready;
    for (const auto& f : m_features)
    {
        const FeatureID id = f->GetID();
        auto it = inDegree.find(id);
        if (it != inDegree.end() && it->second == 0u)
            ready.push(id);
    }

    outSorted.clear();
    outSorted.reserve(m_features.size());
    while (!ready.empty())
    {
        const FeatureID id = ready.front();
        ready.pop();

        auto byIdIt = m_featuresById.find(id);
        if (byIdIt == m_featuresById.end())
            continue;

        outSorted.push_back(byIdIt->second);
        auto depIt = dependents.find(id);
        if (depIt == dependents.end())
            continue;

        for (const FeatureID dependentId : depIt->second)
        {
            auto indegreeIt = inDegree.find(dependentId);
            if (indegreeIt == inDegree.end())
                continue;
            if (indegreeIt->second > 0u)
                --indegreeIt->second;
            if (indegreeIt->second == 0u)
                ready.push(dependentId);
        }
    }

    if (outSorted.size() != m_features.size())
    {
        Debug::LogError("FeatureRegistry: circular dependency detected among features");
        return false;
    }
    return true;
}

bool FeatureRegistry::InitializeAll(const FeatureInitializationContext& context)
{
    if (m_initialized)
        return true;

    ClearRegistrations();

    std::vector<IEngineFeature*> sorted;
    if (!TopologicallySorted(sorted))
        return false;

    m_initOrder.clear();
    m_initOrder.reserve(sorted.size());

    for (auto* feature : sorted)
    {
        FeatureRegistrationContext registration{*this, *feature};
        feature->Register(registration);
        if (!feature->Initialize(context))
        {
            Debug::LogError("FeatureRegistry: feature initialize failed: %s",
                            std::string(feature->GetName()).c_str());
            ShutdownAll(FeatureShutdownContext{context.eventBus});
            return false;
        }
        m_initOrder.push_back(feature);
    }

    RefreshSceneExtractionStepViews();
    RefreshFrameConstantsContributorViews();
    RefreshRenderPipelineViews();

    m_initialized = true;
    Debug::Log("FeatureRegistry: %zu features initialized, %zu extraction steps, %zu frame contributors, active pipeline=%s",
               m_features.size(),
               m_sceneExtractionSteps.size(),
               m_frameConstantsContributors.size(),
               m_activeRenderPipeline ? std::string(m_activeRenderPipeline->GetName()).c_str() : "<none>");
    return true;
}

void FeatureRegistry::ShutdownAll(const FeatureShutdownContext& context) noexcept
{
    RefreshFrameConstantsContributorViews();
    for (const IFrameConstantsContributor* contributor : m_frameConstantsContributors)
    {
        if (contributor)
            const_cast<IFrameConstantsContributor*>(contributor)->OnDeviceShutdown();
    }

    RefreshRenderPipelineViews();
    for (const IRenderPipeline* pipeline : m_renderPipelines)
    {
        if (pipeline)
            const_cast<IRenderPipeline*>(pipeline)->OnDeviceShutdown();
    }

    for (auto it = m_initOrder.rbegin(); it != m_initOrder.rend(); ++it)
        (*it)->Shutdown(context);
    m_initOrder.clear();
    m_initialized = false;
    ClearRegistrations();
}

void FeatureRegistry::RegisterSceneExtractionStep(const IEngineFeature& owner, SceneExtractionStepPtr step) noexcept
{
    if (!step)
    {
        Debug::LogError("FeatureRegistry: rejecting null scene extraction step registration");
        return;
    }

    if (!IsKnownFeature(owner))
    {
        Debug::LogError("FeatureRegistry: rejecting scene extraction step '%s' from unknown feature owner",
                        std::string(step->GetName()).c_str());
        return;
    }

    RefreshSceneExtractionStepViews();

    for (const RegisteredExtractionStep& entry : m_registeredSceneExtractionSteps)
    {
        if (entry.step.get() == step.get())
        {
            Debug::LogError("FeatureRegistry: duplicate scene extraction step registration '%s'",
                            std::string(step->GetName()).c_str());
            return;
        }
    }

    m_registeredSceneExtractionSteps.push_back({std::move(step), owner.GetRuntimeRegistrationOwnerToken(), &owner});
    m_sceneExtractionSteps.clear();
}

void FeatureRegistry::RegisterFrameConstantsContributor(const IEngineFeature& owner,
                                                        FrameConstantsContributorPtr contributor) noexcept
{
    if (!contributor)
    {
        Debug::LogError("FeatureRegistry: rejecting null frame constants contributor registration");
        return;
    }

    if (!IsKnownFeature(owner))
    {
        Debug::LogError("FeatureRegistry: rejecting frame constants contributor '%s' from unknown feature owner",
                        std::string(contributor->GetName()).c_str());
        return;
    }

    RefreshFrameConstantsContributorViews();

    for (const RegisteredFrameConstantsContributor& entry : m_registeredFrameConstantsContributors)
    {
        if (entry.contributor.get() == contributor.get())
        {
            Debug::LogError("FeatureRegistry: duplicate frame constants contributor registration '%s'",
                            std::string(contributor->GetName()).c_str());
            return;
        }
    }

    m_registeredFrameConstantsContributors.push_back({std::move(contributor), owner.GetRuntimeRegistrationOwnerToken(), &owner});
    m_frameConstantsContributors.clear();
}

void FeatureRegistry::RegisterRenderPipeline(const IEngineFeature& owner,
                                             RenderPipelinePtr pipeline,
                                             bool makeDefault) noexcept
{
    if (!pipeline)
    {
        Debug::LogError("FeatureRegistry: rejecting null render pipeline registration");
        return;
    }

    if (!IsKnownFeature(owner))
    {
        Debug::LogError("FeatureRegistry: rejecting render pipeline '%s' from unknown feature owner",
                        std::string(pipeline->GetName()).c_str());
        return;
    }

    RefreshRenderPipelineViews();

    for (const RegisteredRenderPipeline& entry : m_registeredRenderPipelines)
    {
        if (entry.pipeline.get() == pipeline.get())
        {
            Debug::LogError("FeatureRegistry: duplicate render pipeline registration '%s'",
                            std::string(pipeline->GetName()).c_str());
            return;
        }
    }

    m_registeredRenderPipelines.push_back({std::move(pipeline), owner.GetRuntimeRegistrationOwnerToken(), &owner});
    if (makeDefault || m_activeRenderPipelineIndex == kInvalidRegistrationIndex)
        m_activeRenderPipelineIndex = m_registeredRenderPipelines.size() - 1u;
    m_renderPipelines.clear();
    m_activeRenderPipeline = nullptr;
}

const std::vector<const ISceneExtractionStep*>& FeatureRegistry::GetSceneExtractionSteps() const noexcept
{
    RefreshSceneExtractionStepViews();
    return m_sceneExtractionSteps;
}

const std::vector<const IFrameConstantsContributor*>& FeatureRegistry::GetFrameConstantsContributors() const noexcept
{
    RefreshFrameConstantsContributorViews();
    return m_frameConstantsContributors;
}

const IRenderPipeline* FeatureRegistry::GetActiveRenderPipeline() const noexcept
{
    RefreshRenderPipelineViews();
    return m_activeRenderPipeline;
}

void FeatureRegistry::ClearRegistrations() noexcept
{
    m_registeredSceneExtractionSteps.clear();
    m_sceneExtractionSteps.clear();
    m_registeredFrameConstantsContributors.clear();
    m_frameConstantsContributors.clear();
    m_registeredRenderPipelines.clear();
    m_renderPipelines.clear();
    m_activeRenderPipeline = nullptr;
    m_activeRenderPipelineIndex = kInvalidRegistrationIndex;
}

bool FeatureRegistry::IsKnownFeature(const IEngineFeature& feature) const noexcept
{
    const FeatureID id = feature.GetID();
    const auto it = m_featuresById.find(id);
    return it != m_featuresById.end() && it->second == &feature;
}

std::string_view FeatureRegistry::GetRegisteredFeatureName(FeatureID id) const noexcept
{
    const auto it = m_featureNamesById.find(id);
    if (it == m_featureNamesById.end())
        return {};
    return it->second;
}

void FeatureRegistry::RefreshSceneExtractionStepViews() const noexcept
{
    m_sceneExtractionSteps.clear();

    for (auto it = m_registeredSceneExtractionSteps.begin(); it != m_registeredSceneExtractionSteps.end();)
    {
        const bool ownerAlive = !it->ownerToken.expired();
        const bool stepAlive = static_cast<bool>(it->step);
        if (!ownerAlive || !stepAlive)
        {
            const std::string ownerName = (it->owner != nullptr) ? std::string(it->owner->GetName()) : std::string("<unknown>");
            const std::string stepName = stepAlive ? std::string(it->step->GetName()) : std::string("<expired>");
            Debug::LogError("FeatureRegistry: removing invalid scene extraction step registration '%s' from owner '%s'",
                            stepName.c_str(),
                            ownerName.c_str());
            it = m_registeredSceneExtractionSteps.erase(it);
            continue;
        }

        m_sceneExtractionSteps.push_back(it->step.get());
        ++it;
    }
}

void FeatureRegistry::RefreshFrameConstantsContributorViews() const noexcept
{
    m_frameConstantsContributors.clear();

    for (auto it = m_registeredFrameConstantsContributors.begin(); it != m_registeredFrameConstantsContributors.end();)
    {
        const bool ownerAlive = !it->ownerToken.expired();
        const bool contributorAlive = static_cast<bool>(it->contributor);
        if (!ownerAlive || !contributorAlive)
        {
            const std::string ownerName = (it->owner != nullptr) ? std::string(it->owner->GetName()) : std::string("<unknown>");
            const std::string contributorName = contributorAlive ? std::string(it->contributor->GetName()) : std::string("<expired>");
            Debug::LogError("FeatureRegistry: removing invalid frame constants contributor '%s' from owner '%s'",
                            contributorName.c_str(),
                            ownerName.c_str());
            it = m_registeredFrameConstantsContributors.erase(it);
            continue;
        }

        m_frameConstantsContributors.push_back(it->contributor.get());
        ++it;
    }
}

void FeatureRegistry::RefreshRenderPipelineViews() const noexcept
{
    m_renderPipelines.clear();
    m_activeRenderPipeline = nullptr;

    size_t nextActiveIndex = kInvalidRegistrationIndex;
    size_t currentIndex = 0u;
    for (auto it = m_registeredRenderPipelines.begin(); it != m_registeredRenderPipelines.end();)
    {
        const bool ownerAlive = !it->ownerToken.expired();
        const bool pipelineAlive = static_cast<bool>(it->pipeline);
        if (!ownerAlive || !pipelineAlive)
        {
            const bool wasActive = (currentIndex == m_activeRenderPipelineIndex);
            const std::string ownerName = (it->owner != nullptr) ? std::string(it->owner->GetName()) : std::string("<unknown>");
            const std::string pipelineName = pipelineAlive ? std::string(it->pipeline->GetName()) : std::string("<expired>");
            Debug::LogError("FeatureRegistry: removing invalid render pipeline registration '%s' from owner '%s'%s",
                            pipelineName.c_str(),
                            ownerName.c_str(),
                            wasActive ? " (active)" : "");
            it = m_registeredRenderPipelines.erase(it);
            continue;
        }

        m_renderPipelines.push_back(it->pipeline.get());
        if (currentIndex == m_activeRenderPipelineIndex)
        {
            m_activeRenderPipeline = it->pipeline.get();
            nextActiveIndex = m_renderPipelines.size() - 1u;
        }

        ++it;
        ++currentIndex;
    }

    if (m_registeredRenderPipelines.empty())
    {
        m_activeRenderPipelineIndex = kInvalidRegistrationIndex;
        return;
    }

    if (!m_activeRenderPipeline)
    {
        if (m_activeRenderPipelineIndex != kInvalidRegistrationIndex)
        {
            Debug::LogError("FeatureRegistry: active render pipeline registration became invalid");
            m_activeRenderPipelineIndex = kInvalidRegistrationIndex;
            return;
        }

        m_activeRenderPipeline = m_registeredRenderPipelines.front().pipeline.get();
        m_activeRenderPipelineIndex = 0u;
        return;
    }

    m_activeRenderPipelineIndex = nextActiveIndex;
}

} // namespace engine::renderer
