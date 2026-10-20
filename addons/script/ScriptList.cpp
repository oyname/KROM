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
    // Gespeicherte Werte koennen einen abgeleiteten Typ tragen (z. B. aus einem
    // Platzhalter-Script) - erst in den deklarierten Typ umrechnen, sonst lehnt
    // WriteField den Wert ab.
    const std::vector<ScriptFieldMeta>* metas = registry.GetFields(inst.className);
    for (auto& [name, value] : inst.fieldValues)
    {
        if (metas)
        {
            for (const ScriptFieldMeta& meta : *metas)
            {
                if (meta.name == name && meta.type != value.type)
                {
                    value = ConvertScriptFieldValue(value, meta.type);
                    break;
                }
            }
        }
        (void)registry.WriteField(*inst.script, inst.className, name, value);
    }
}

void ScriptList::Update(float deltaTime, EntityID entity, const ScriptServices& services)
{
    ScriptFrameContext ctx{ deltaTime, entity, services };
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

void ScriptList::FixedUpdate(float fixedDeltaTime, EntityID entity, const ScriptServices& services)
{
    ScriptFrameContext ctx{ fixedDeltaTime, entity, services, true };
    for (auto& inst : m_instances)
    {
        if (!inst.script || !inst.started) continue;
        inst.script->OnFixedUpdate(ctx);
    }
}

void ScriptList::NotifyTrigger(bool enter, EntityID entity, EntityID other, const ScriptServices& services)
{
    ScriptFrameContext ctx{ 0.0f, entity, services };
    for (auto& inst : m_instances)
    {
        if (!inst.script || !inst.started) continue;
        if (enter) inst.script->OnTriggerEnter(ctx, other);
        else       inst.script->OnTriggerExit(ctx, other);
    }
}

void ScriptList::NotifyCollision(bool enter, EntityID entity, EntityID other, const ScriptServices& services)
{
    ScriptFrameContext ctx{ 0.0f, entity, services };
    for (auto& inst : m_instances)
    {
        if (!inst.script || !inst.started) continue;
        if (enter) inst.script->OnCollisionEnter(ctx, other);
        else       inst.script->OnCollisionExit(ctx, other);
    }
}

void ScriptList::NotifyEvent(const ScriptEvent& event, EntityID entity, const ScriptServices& services)
{
    ScriptFrameContext ctx{ 0.0f, entity, services };
    for (auto& inst : m_instances)
    {
        if (!inst.script || !inst.started) continue;
        inst.script->OnEvent(ctx, event);
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
