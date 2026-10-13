#include "addons/script/ScriptSerialization.hpp"
#include "addons/script/ScriptList.hpp"

namespace engine::script {
namespace {

EntityID FindEntityByGuid(ecs::World& world, const std::string& guid)
{
    if (guid.empty())
        return NULL_ENTITY;

    EntityID result = NULL_ENTITY;
    world.ForEachAlive([&](EntityID entity)
    {
        if (result.IsValid())
            return;
        const auto* component = world.Get<GuidComponent>(entity);
        if (component && component->guid == guid)
            result = entity;
    });
    return result;
}

void WriteFieldValue(serialization::JsonWriter& w, const std::string& name, const ScriptFieldValue& value)
{
    switch (value.type)
    {
        case ScriptFieldType::Float: w.WriteFloat(name, value.floatValue); break;
        case ScriptFieldType::Int:   w.WriteInt(name, value.intValue); break;
        case ScriptFieldType::Bool:  w.WriteBool(name, value.boolValue); break;
        case ScriptFieldType::Vec3:  w.WriteVec3(name, value.vec3Value); break;
        case ScriptFieldType::Entity:
        case ScriptFieldType::String:
        case ScriptFieldType::Prefab:
            w.WriteString(name, value.stringValue);
            break;
    }
}

bool ReadFieldValue(const serialization::JsonValue& node,
                    ScriptFieldType type,
                    ScriptFieldValue& outValue)
{
    outValue.type = type;
    switch (type)
    {
        case ScriptFieldType::Float:
            if (!node.IsNumber()) return false;
            outValue.floatValue = node.AsFloat();
            return true;
        case ScriptFieldType::Int:
            if (!node.IsNumber()) return false;
            outValue.intValue = node.AsInt();
            return true;
        case ScriptFieldType::Bool:
            if (!node.IsBool()) return false;
            outValue.boolValue = node.AsBool();
            return true;
        case ScriptFieldType::Vec3:
            if (!node.IsArray() || node.arrayVal.size() < 3u) return false;
            outValue.vec3Value = node.AsVec3();
            return true;
        case ScriptFieldType::String:
        case ScriptFieldType::Prefab:
        case ScriptFieldType::Entity:
            if (!node.IsString()) return false;
            outValue.stringValue = node.AsString();
            return true;
    }
    return false;
}

} // namespace

void RegisterScriptSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<ScriptList>([](serialization::JsonWriter& w, const ScriptList& sl)
    {
        w.BeginObject();
        w.WriteString("type", "ScriptList");
        w.BeginArray("scripts");
        for (const auto& inst : sl.Instances())
        {
            w.BeginObject();
            w.WriteString("class", inst.className);
            if (inst.instanceName != inst.className)
                w.WriteString("name", inst.instanceName);
            if (!inst.fieldValues.empty())
            {
                w.BeginObject("fields");
                for (const auto& [name, value] : inst.fieldValues)
                    WriteFieldValue(w, name, value);
                w.EndObject();
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    });
}

void UnregisterScriptSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<ScriptList>();
}

void RegisterScriptDeserializationHandlers(serialization::SceneDeserializer& deserializer,
                                            ScriptRegistry*                   registry)
{
    deserializer.RegisterHandler("ScriptList",
        [registry](const serialization::JsonValue& v,
                   ecs::World&   w,
                   EntityID      id,
                   const std::unordered_map<uint32_t, EntityID>&)
        {
            const auto* scripts = v.Get("scripts");
            if (!scripts || !scripts->IsArray()) return;

            ScriptList sl;
            sl.SetOwnerEntity(id);

            for (const auto& entry : scripts->arrayVal)
            {
                const auto* cls = entry.Get("class");
                if (!cls) continue;
                const std::string& className = cls->AsString();
                const std::string  instName  = [&]() -> std::string {
                    const auto* n = entry.Get("name");
                    return n ? n->AsString() : className;
                }();
                if (registry)
                {
                    if (sl.Add(className, id, *registry))
                    {
                        sl.Instances_Mutable().back().instanceName = instName;
                        ScriptInstance& inst = sl.Instances_Mutable().back();
                        if (const auto* fieldsNode = entry.Get("fields"))
                        {
                            if (const auto* metas = registry->GetFields(className))
                            {
                                for (const ScriptFieldMeta& meta : *metas)
                                {
                                    const auto* valueNode = fieldsNode->Get(meta.name);
                                    if (!valueNode) continue;
                                    ScriptFieldValue value{};
                                    if (ReadFieldValue(*valueNode, meta.type, value))
                                    {
                                        if (meta.type == ScriptFieldType::Entity)
                                            value.entityValue = FindEntityByGuid(w, value.stringValue);
                                        inst.fieldValues[meta.name] = value;
                                    }
                                }
                            }
                        }
                        sl.ApplyStoredFieldValues(sl.Count() - 1u, *registry);
                    }
                }
                else
                {
                    sl.AddNameOnly(className, instName);
                }
            }

            if (!sl.IsEmpty())
                w.Add<ScriptList>(id, std::move(sl));
        });
}

void UnregisterScriptDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("ScriptList");
}

void ResolveScriptEntityReferences(ecs::World& world, const ScriptRegistry& registry)
{
    world.ForEachAlive([&](EntityID entity)
    {
        auto* scripts = world.Get<ScriptList>(entity);
        if (!scripts)
            return;

        for (ScriptInstance& inst : scripts->Instances_Mutable())
        {
            if (!inst.script)
                continue;
            const auto* fields = registry.GetFields(inst.className);
            if (!fields)
                continue;

            for (const ScriptFieldMeta& field : *fields)
            {
                if (field.type != ScriptFieldType::Entity)
                    continue;
                auto valueIt = inst.fieldValues.find(field.name);
                if (valueIt == inst.fieldValues.end())
                    continue;
                ScriptFieldValue& value = valueIt->second;
                value.type = ScriptFieldType::Entity;
                value.entityValue = FindEntityByGuid(world, value.stringValue);
                (void)registry.WriteField(*inst.script, inst.className, field.name, value);
            }
        }
    });
}

} // namespace engine::script
