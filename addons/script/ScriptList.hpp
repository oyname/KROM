#pragma once
#include "addons/script/ScriptRegistry.hpp"
#include <functional>
#include <optional>
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

// ─── Runtime-Dienste fuer Scripts ─────────────────────────────────────────────

// Was die Runtime den Scripts pro Frame bereitstellt. Alle Felder optional.
struct ScriptServices
{
    ecs::World*                world             = nullptr;
    const platform::IInput*    input             = nullptr;
    const PrefabInstantiateFn* instantiatePrefab = nullptr;
    // Ziel fuer IScriptContext::Destroy(); die Runtime zerstoert nach dem Script-Update.
    std::vector<EntityID>*     pendingDestroy    = nullptr;
    const collision::SceneQueries* sceneQueries  = nullptr;
    // Ziel fuer IScriptContext::LoadScene(); der letzte Request im Frame gewinnt.
    std::optional<SceneLoadRequest>* pendingSceneLoad = nullptr;
};

// ─── interner Frame-Kontext ───────────────────────────────────────────────────

class ScriptFrameContext final : public IScriptContext
{
public:
    ScriptFrameContext(float dt, EntityID entity, const ScriptServices& services = {}) noexcept
        : m_dt(dt), m_entity(entity), m_services(services) {}

    float    DeltaTime() const noexcept override { return m_dt;     }
    EntityID GetEntity() const noexcept override { return m_entity; }

    ecs::World*             GetWorld() noexcept override       { return m_services.world; }
    const platform::IInput* GetInput() const noexcept override { return m_services.input; }

    EntityID InstantiatePrefab(std::string_view prefabAssetPath,
                               const math::Vec3& position = {0.f, 0.f, 0.f},
                               const math::Quat& rotation = math::Quat::Identity(),
                               const math::Vec3& scale = {1.f, 1.f, 1.f}) override
    {
        return m_services.instantiatePrefab
            ? (*m_services.instantiatePrefab)(prefabAssetPath, position, rotation, scale)
            : NULL_ENTITY;
    }

    void Destroy(EntityID entity = NULL_ENTITY) override
    {
        const EntityID id = entity.IsValid() ? entity : m_entity;
        if (m_services.pendingDestroy && id.IsValid())
            m_services.pendingDestroy->push_back(id);
    }

    const collision::SceneQueries* GetSceneQueries() const noexcept override { return m_services.sceneQueries; }

    void RequestSceneLoad(const SceneLoadRequest& request) override
    {
        if (m_services.pendingSceneLoad)
            *m_services.pendingSceneLoad = request;
    }

private:
    float          m_dt;
    EntityID       m_entity;
    ScriptServices m_services;
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
    void Update(float deltaTime, EntityID entity, const ScriptServices& services = {});

    // OnTriggerEnter/OnTriggerExit an alle gestarteten Scripts.
    void NotifyTrigger(bool enter, EntityID entity, EntityID other, const ScriptServices& services = {});

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
