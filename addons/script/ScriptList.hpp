#pragma once
#include "addons/script/ScriptRegistry.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::script {

using PrefabInstantiateFn = std::function<EntityID(
    std::string_view prefabAssetPath,
    const math::Vec3& position,
    const math::Quat& rotation,
    const math::Vec3& scale)>;

struct ScriptInstance
{
    std::string                      className;     // unveränderlicher Klassen-ID
    std::string                      instanceName;  // frei wählbarer Anzeigename (default = className)
    std::unique_ptr<ComponentScript> script;
    std::unordered_map<std::string, ScriptFieldValue> fieldValues;
    bool                             started = false;

    ScriptInstance() = default;
    ScriptInstance(std::string cls, std::unique_ptr<ComponentScript> s)
        : className(cls), instanceName(cls), script(std::move(s)) {}
    ScriptInstance(std::string cls, std::string instName, std::unique_ptr<ComponentScript> s)
        : className(std::move(cls)), instanceName(std::move(instName)), script(std::move(s)) {}

    ScriptInstance(const ScriptInstance&)            = delete;
    ScriptInstance& operator=(const ScriptInstance&) = delete;
    ScriptInstance(ScriptInstance&&)                 = default;
    ScriptInstance& operator=(ScriptInstance&&)      = default;
};

// ─── interner Frame-Kontext ───────────────────────────────────────────────────

class ScriptFrameContext final : public IScriptContext
{
public:
    ScriptFrameContext(float dt, EntityID entity, const PrefabInstantiateFn* instantiatePrefab = nullptr) noexcept
        : m_dt(dt), m_entity(entity), m_instantiatePrefab(instantiatePrefab) {}

    float    DeltaTime() const noexcept override { return m_dt;     }
    EntityID GetEntity() const noexcept override { return m_entity; }
    EntityID InstantiatePrefab(std::string_view prefabAssetPath,
                               const math::Vec3& position = {0.f, 0.f, 0.f},
                               const math::Quat& rotation = math::Quat::Identity(),
                               const math::Vec3& scale = {1.f, 1.f, 1.f}) override
    {
        return m_instantiatePrefab ? (*m_instantiatePrefab)(prefabAssetPath, position, rotation, scale)
                                   : NULL_ENTITY;
    }

private:
    float    m_dt;
    EntityID m_entity;
    const PrefabInstantiateFn* m_instantiatePrefab = nullptr;
};

// ─── ECS-Komponente ───────────────────────────────────────────────────────────

class ScriptList
{
public:
    ScriptList()                             = default;
    ScriptList(const ScriptList&)            = delete;
    ScriptList& operator=(const ScriptList&) = delete;
    ScriptList(ScriptList&&)                 = default;
    ScriptList& operator=(ScriptList&&)      = default;

    // Per Registry hinzufügen — gibt false zurück wenn className unbekannt.
    bool Add(const std::string& className, EntityID entity, const ScriptRegistry& registry);

    // Direkt hinzufügen (z.B. in Tests). script darf null sein (Editor-only).
    void Add(std::string className, std::unique_ptr<ComponentScript> script, EntityID entity);

    // Nur Klassen- und Instanzname eintragen, kein Live-Skript (Editor / Undo-Redo).
    // instanceName optional — Default = className.
    void AddNameOnly(std::string className, std::string instanceName = {});

    void ApplyStoredFieldValues(size_t index, const ScriptRegistry& registry);

    // OnStart (einmalig) + OnUpdate pro Frame.
    void Update(float deltaTime, EntityID entity, const PrefabInstantiateFn* instantiatePrefab = nullptr);

    // OnDestroy in umgekehrter Reihenfolge; danach Liste leer.
    // entity muss die owning Entity sein; wird auch vom Destruktor benutzt.
    void DestroyAll(EntityID entity);

    // Entfernt eine einzelne Script-Instanz und ruft OnDestroy, falls sie gestartet wurde.
    bool RemoveAt(size_t index, EntityID entity);

    // Destruktor ruft DestroyAll mit gespeicherter Entity.
    // Setzt voraus, dass SetOwnerEntity() nach der Komponenten-Zuweisung aufgerufen wird.
    ~ScriptList() noexcept;

    void SetOwnerEntity(EntityID e) noexcept { m_ownerEntity = e; }

    [[nodiscard]] size_t Count()   const noexcept { return m_instances.size(); }
    [[nodiscard]] bool   IsEmpty() const noexcept { return m_instances.empty(); }
    [[nodiscard]] const std::vector<ScriptInstance>& Instances() const noexcept { return m_instances; }
    [[nodiscard]]       std::vector<ScriptInstance>& Instances_Mutable()        { return m_instances; }
    [[nodiscard]] EntityID OwnerEntity() const noexcept { return m_ownerEntity; }

private:
    std::vector<ScriptInstance> m_instances;
    EntityID                    m_ownerEntity = NULL_ENTITY;
};

} // namespace engine::script
