#pragma once
// =============================================================================
// KROM Engine - serialization/SceneSerializer.hpp
//
// BUGFIX: SerializeToJson() iterierte über View<NameComponent> →
//         Entities ohne NameComponent wurden komplett ignoriert.
//         Fix: World::ForEachAlive() - jede Entity wird erfasst.
//
// NEU: SceneDeserializer - liest JSON zurück in eine World.
//      Minimaler eigener JsonParser (kein extern).
//      EntityID-Remapping für Parent/Child-Referenzen.
// =============================================================================
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "ecs/ComponentMeta.hpp"
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace engine::serialization {

// =============================================================================
// JsonWriter
// =============================================================================
class JsonWriter
{
public:
    JsonWriter() = default;
    explicit JsonWriter(bool roundTripFloats) : m_roundTripFloats(roundTripFloats) {}
    void BeginObject(const std::string& key = "");
    void EndObject();
    void BeginArray(const std::string& key);
    void EndArray();

    void WriteString(const std::string& key, const std::string& val);
    void WriteFloat (const std::string& key, float val);
    void WriteUint  (const std::string& key, uint32_t val);
    void WriteInt   (const std::string& key, int32_t val);
    void WriteBool  (const std::string& key, bool val);
    void WriteVec3  (const std::string& key, const math::Vec3& v);
    void WriteQuat  (const std::string& key, const math::Quat& q);

    [[nodiscard]] std::string GetString() const;

private:
    struct Frame { bool isArray; bool hadItem; };
    std::string        m_buf;
    std::vector<Frame> m_stack;
    bool               m_roundTripFloats = false;

    void Prefix(const std::string& key);
    void Indent();
    std::string FormatFloat(float value) const;
    static std::string Escape(const std::string& s);
};

// =============================================================================
// JsonValue - minimaler DOM für den Deserializer
// =============================================================================
enum class JsonType : uint8_t { Null, Bool, Number, String, Array, Object };

struct JsonValue
{
    JsonType    type    = JsonType::Null;
    bool        boolVal = false;
    double      numVal  = 0.0;
    std::string strVal;
    std::vector<JsonValue>                         arrayVal;
    std::vector<std::pair<std::string,JsonValue>>  objectVal;
    // Byte range of the original JSON string token, including its quotes.
    size_t stringOffset = 0;
    size_t stringLength = 0;

    [[nodiscard]] bool     IsNull()   const noexcept { return type == JsonType::Null;   }
    [[nodiscard]] bool     IsBool()   const noexcept { return type == JsonType::Bool;   }
    [[nodiscard]] bool     IsNumber() const noexcept { return type == JsonType::Number; }
    [[nodiscard]] bool     IsString() const noexcept { return type == JsonType::String; }
    [[nodiscard]] bool     IsArray()  const noexcept { return type == JsonType::Array;  }
    [[nodiscard]] bool     IsObject() const noexcept { return type == JsonType::Object; }

    [[nodiscard]] float    AsFloat()  const noexcept { return static_cast<float>(numVal); }
    [[nodiscard]] uint32_t AsUint()   const noexcept { return static_cast<uint32_t>(numVal); }
    [[nodiscard]] int32_t  AsInt()    const noexcept { return static_cast<int32_t>(numVal); }
    [[nodiscard]] bool     AsBool()   const noexcept { return boolVal; }
    [[nodiscard]] const std::string& AsString() const noexcept { return strVal; }

    [[nodiscard]] const JsonValue* Get(const std::string& key) const noexcept;
    [[nodiscard]] const JsonValue& At(size_t index)            const noexcept;
    [[nodiscard]] math::Vec3       AsVec3()                    const noexcept;
    [[nodiscard]] math::Quat       AsQuat()                    const noexcept;

    static const JsonValue& Null() noexcept;
};

// =============================================================================
// JsonParser
// =============================================================================
class JsonParser
{
public:
    static JsonValue Parse(const std::string& json, std::string& outError);

private:
    explicit JsonParser(const char* src) : m_src(src), m_pos(0u) {}

    JsonValue ParseValue();
    JsonValue ParseObject();
    JsonValue ParseArray();
    JsonValue ParseString();
    JsonValue ParseNumber();
    JsonValue ParseLiteral();

    void SkipWs();
    char Peek()  const noexcept;
    char Consume()     noexcept;

    const char* m_src;
    size_t      m_pos;
    std::string m_error;
};

// =============================================================================
// SceneSerializer - BUGFIX: ForEachAlive statt View<NameComponent>
// =============================================================================
class SceneSerializer
{
public:
    explicit SceneSerializer(const ecs::World& world) : m_world(world) {}

    template<typename T>
    void RegisterSerializer(std::function<void(JsonWriter&, const T&)> fn)
    {
        const uint32_t id = ComponentTypeID<T>::Get();
        m_handlers[id] = [fn](JsonWriter& w, const void* component) {
            fn(w, *static_cast<const T*>(component));
        };
    }

    template<typename T>
    void UnregisterSerializer()
    {
        m_handlers.erase(ComponentTypeID<T>::Get());
    }

    void UnregisterSerializer(uint32_t typeId)
    {
        m_handlers.erase(typeId);
    }

    void RegisterDefaultHandlers();

    // Optionaler Entity-Filter: gibt false zurück → Entity wird übersprungen.
    // nullptr (Default) = alle Entities serialisieren.
    void SetEntityFilter(std::function<bool(EntityID)> filter)
    {
        m_entityFilter = std::move(filter);
    }
    void ClearEntityFilter() { m_entityFilter = nullptr; }

    // Optionaler Root-Metadaten-Writer: schreibt zusätzliche Root-Felder vor dem
    // entities-Array. Wird fuer Editor-only Scene-Metadaten genutzt, ohne diese
    // als ECS-Entity/Main-Camera zu serialisieren.
    void SetRootMetadataWriter(std::function<void(JsonWriter&)> writer)
    {
        m_rootMetadataWriter = std::move(writer);
    }
    void ClearRootMetadataWriter() { m_rootMetadataWriter = nullptr; }

    // Serialisiert lebendige Entities - gefiltert wenn SetEntityFilter gesetzt.
    [[nodiscard]] std::string SerializeToJson(const std::string& sceneName = "Scene") const;
    [[nodiscard]] bool HasComponentHandler(uint32_t typeId) const { return m_handlers.contains(typeId); }
    [[nodiscard]] std::string SerializeComponent(EntityID entity, uint32_t typeId) const;
    [[nodiscard]] std::string SerializeComponentValue(uint32_t typeId, const void* component) const;

private:
    const ecs::World& m_world;
    using HandlerFn = std::function<void(JsonWriter&, const void*)>;
    std::unordered_map<uint32_t, HandlerFn> m_handlers;
    std::function<bool(EntityID)>           m_entityFilter; // nullptr = kein Filter
    std::function<void(JsonWriter&)>        m_rootMetadataWriter;
};

// =============================================================================
// SceneDeserializer
// =============================================================================
struct DeserializeResult
{
    bool        success          = false;
    std::string error;
    uint32_t    entitiesRead     = 0u;
    uint32_t    componentsRead   = 0u;
    uint32_t    entitiesSkipped  = 0u;
    std::unordered_map<uint32_t, EntityID> entityRemap;
};

class SceneDeserializer
{
public:
    explicit SceneDeserializer(ecs::World& world) : m_world(world) {}

    // Handler bekommt den JsonValue des Komponenten-Objekts + neue EntityID.
    // remap: entityId-Alt → EntityID-Neu (für Parent-Referenzen etc.)
    using HandlerFn = std::function<void(
        const JsonValue&,
        ecs::World&,
        EntityID,
        const std::unordered_map<uint32_t, EntityID>& remap)>;

    void RegisterHandler(const std::string& typeName, HandlerFn fn)
    {
        m_handlers[typeName] = std::move(fn);
    }

    void UnregisterHandler(const std::string& typeName)
    {
        m_handlers.erase(typeName);
    }

    template<typename T>
    void RegisterHandler(std::function<void(const JsonValue&, ecs::World&, EntityID)> fn)
    {
        const ecs::ComponentMetaRegistry& registry = m_world.GetComponentMetaRegistry();
        const std::string name =
            registry.Get(ComponentTypeID<T>::Get()) ? registry.Get(ComponentTypeID<T>::Get())->name : "";
        m_handlers[name] = [fn](const JsonValue& v, ecs::World& w, EntityID id,
                                 const std::unordered_map<uint32_t,EntityID>&)
        { fn(v, w, id); };
    }

    void RegisterDefaultHandlers();

    // Parst JSON, erstellt neue Entities, fügt Komponenten ein.
    // Zweiter Pass für Parent-Referenzen (ID-Remapping).
    [[nodiscard]] DeserializeResult DeserializeFromJson(const std::string& json);
    // Bereits geparstes Dokument (z.B. Datei-I/O/Parsing im Hintergrund).
    // Die World wird ausschliesslich im aufrufenden Thread veraendert.
    [[nodiscard]] DeserializeResult DeserializeFromValue(const JsonValue& root);
    [[nodiscard]] bool HasComponentHandler(const std::string& name) const { return m_handlers.contains(name); }
    [[nodiscard]] bool DeserializeComponent(EntityID entity, const JsonValue& component,
        const std::unordered_map<uint32_t, EntityID>& remap = {});

private:
    ecs::World& m_world;
    std::unordered_map<std::string, HandlerFn> m_handlers;
};

// ColliderComponent fields in the currently open JSON object (shared by scenes
// and prefabs). Read replaces the shape list when "shapes" is present.
void WriteColliderFields(JsonWriter& w, const ColliderComponent& c);
void ReadColliderFields(const JsonValue& v, ColliderComponent& c);

// Older files stored each shape as its own component. These read one such
// shape and append it to the collider (showInEditor is OR-ed in).
void AppendLegacyBoxCollider(const OBBComponent& obb, ColliderComponent& c);
void AppendLegacySphereCollider(const JsonValue& v, ColliderComponent& c);
void AppendLegacyCapsuleCollider(const JsonValue& v, ColliderComponent& c);

} // namespace engine::serialization
