#include "addons/script/ScriptList.hpp"
#include "core/Logger.hpp"
#include <cstdio>

namespace engine::script {

bool ScriptList::Add(const std::string& className,
                     EntityID           entity,
                     const ScriptRegistry& registry)
{
    auto script = registry.Create(className);
    if (!script)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "ScriptList::Add: unbekannte Script-Klasse '%s' (Entity %u)",
            className.c_str(), entity.value);
        engine::GetDefaultLogger().Warning(buf);
        return false;
    }
    Add(className, std::move(script), entity);
    return true;
}

void ScriptList::Add(std::string                      className,
                     std::unique_ptr<ComponentScript> script,
                     EntityID                         entity)
{
    m_ownerEntity = entity;
    if (script) script->SetEntity(entity);
    m_instances.emplace_back(std::move(className), std::move(script));
}

void ScriptList::AddNameOnly(std::string className, std::string instanceName)
{
    const std::string& iname = instanceName.empty() ? className : instanceName;
    m_instances.emplace_back(className, iname, nullptr);
}

void ScriptList::ApplyStoredFieldValues(size_t index, const ScriptRegistry& registry)
{
    if (index >= m_instances.size())
        return;
    ScriptInstance& inst = m_instances[index];
    if (!inst.script)
        return;
    for (const auto& [name, value] : inst.fieldValues)
        (void)registry.WriteField(*inst.script, inst.className, name, value);
}

void ScriptList::Update(float deltaTime, EntityID entity, const PrefabInstantiateFn* instantiatePrefab)
{
    ScriptFrameContext ctx{ deltaTime, entity, instantiatePrefab };
    for (auto& inst : m_instances)
    {
        if (!inst.script) continue;
        if (!inst.started)
        {
            inst.script->OnStart(ctx);
            inst.started = true;
        }
        inst.script->OnUpdate(ctx);
    }
}

void ScriptList::DestroyAll(EntityID entity)
{
    if (m_instances.empty()) return;
    ScriptFrameContext ctx{ 0.0f, entity };
    for (auto it = m_instances.rbegin(); it != m_instances.rend(); ++it)
    {
        if (it->started && it->script)
            it->script->OnDestroy(ctx);
    }
    m_instances.clear();
}

bool ScriptList::RemoveAt(size_t index, EntityID entity)
{
    if (index >= m_instances.size())
        return false;

    ScriptInstance& inst = m_instances[index];
    if (inst.started && inst.script)
    {
        ScriptFrameContext ctx{ 0.0f, entity };
        inst.script->OnDestroy(ctx);
    }
    m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

ScriptList::~ScriptList() noexcept
{
    DestroyAll(m_ownerEntity);
}

} // namespace engine::script
