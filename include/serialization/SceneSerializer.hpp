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

    void Prefix(const std::string& key);
    void Indent();
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
        const uint32_t id = ComponentTypeID<T>::value;
        m_handlers[id] = [fn](JsonWriter& w, const ecs::World& world, EntityID eid) {
            const T* c = world.Get<T>(eid);
            if (c) fn(w, *c);
        };
    }

    template<typename T>
    void UnregisterSerializer()
    {
        m_handlers.erase(ComponentTypeID<T>::value);
    }

    void UnregisterSerializer(uint32_t typeId)
    {
        m_handlers.erase(typeId);
    }

    void RegisterDefaultHandlers();

    // Serialisiert ALLE lebendigen Entities - unabhängig von ihren Komponenten.
    [[nodiscard]] std::string SerializeToJson(const std::string& sceneName = "Scene") const;

private:
    const ecs::World& m_world;
    using HandlerFn = std::function<void(JsonWriter&, const ecs::World&, EntityID)>;
    std::unordered_map<uint32_t, HandlerFn> m_handlers;
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
            registry.Get(ComponentTypeID<T>::value) ? registry.Get(ComponentTypeID<T>::value)->name : "";
        m_handlers[name] = [fn](const JsonValue& v, ecs::World& w, EntityID id,
                                 const std::unordered_map<uint32_t,EntityID>&)
        { fn(v, w, id); };
    }

    void RegisterDefaultHandlers();

    // Parst JSON, erstellt neue Entities, fügt Komponenten ein.
    // Zweiter Pass für Parent-Referenzen (ID-Remapping).
    [[nodiscard]] DeserializeResult DeserializeFromJson(const std::string& json);

private:
    ecs::World& m_world;
    std::unordered_map<std::string, HandlerFn> m_handlers;
};

} // namespace engine::serialization
