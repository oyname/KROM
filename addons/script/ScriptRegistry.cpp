#include "addons/script/ScriptRegistry.hpp"
#include <algorithm>

namespace engine::script {

void ScriptRegistry::Register(std::string className, ScriptFactory factory)
{
    m_factories[std::move(className)] = std::move(factory);
}

std::unique_ptr<ComponentScript> ScriptRegistry::Create(const std::string& className) const
{
    auto it = m_factories.find(className);
    if (it == m_factories.end())
        return nullptr;
    return it->second();
}

bool ScriptRegistry::Has(const std::string& className) const noexcept
{
    return m_factories.count(className) > 0;
}

const std::vector<ScriptFieldMeta>* ScriptRegistry::GetFields(const std::string& className) const noexcept
{
    auto it = m_fields.find(className);
    return it == m_fields.end() ? nullptr : &it->second;
}

void ScriptRegistry::RegisterFieldInternal(const std::string& className,
                                           std::string fieldName,
                                           ScriptFieldType type,
                                           size_t offset)
{
    auto& fields = m_fields[className];
    auto it = std::find_if(fields.begin(), fields.end(),
        [&](const ScriptFieldMeta& f) { return f.name == fieldName; });
    ScriptFieldMeta meta{std::move(fieldName), type, offset};
    if (it != fields.end())
        *it = std::move(meta);
    else
        fields.push_back(std::move(meta));
}

const ScriptFieldMeta* ScriptRegistry::FindField(const std::string& className,
                                                 const std::string& fieldName) const noexcept
{
    const auto* fields = GetFields(className);
    if (!fields)
        return nullptr;
    auto it = std::find_if(fields->begin(), fields->end(),
        [&](const ScriptFieldMeta& f) { return f.name == fieldName; });
    return it == fields->end() ? nullptr : &*it;
}

bool ScriptRegistry::ReadField(const ComponentScript& script,
                               const std::string& className,
                               const std::string& fieldName,
                               ScriptFieldValue& outValue) const
{
    const ScriptFieldMeta* field = FindField(className, fieldName);
    if (!field)
        return false;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&script);
    const uint8_t* ptr = base + field->offset;
    outValue.type = field->type;
    switch (field->type)
    {
        case ScriptFieldType::Float: outValue.floatValue = *reinterpret_cast<const float*>(ptr); return true;
        case ScriptFieldType::Int:   outValue.intValue   = *reinterpret_cast<const int32_t*>(ptr); return true;
        case ScriptFieldType::Bool:  outValue.boolValue  = *reinterpret_cast<const bool*>(ptr); return true;
        case ScriptFieldType::Vec3:  outValue.vec3Value  = *reinterpret_cast<const math::Vec3*>(ptr); return true;
        case ScriptFieldType::Entity: outValue.entityValue = *reinterpret_cast<const EntityID*>(ptr); return true;
        case ScriptFieldType::String:
        case ScriptFieldType::Prefab:
            outValue.stringValue = *reinterpret_cast<const std::string*>(ptr);
            return true;
    }
    return false;
}

bool ScriptRegistry::WriteField(ComponentScript& script,
                                const std::string& className,
                                const std::string& fieldName,
                                const ScriptFieldValue& value) const
{
    const ScriptFieldMeta* field = FindField(className, fieldName);
    if (!field || field->type != value.type)
        return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(&script);
    uint8_t* ptr = base + field->offset;
    switch (field->type)
    {
        case ScriptFieldType::Float: *reinterpret_cast<float*>(ptr) = value.floatValue; return true;
        case ScriptFieldType::Int:   *reinterpret_cast<int32_t*>(ptr) = value.intValue; return true;
        case ScriptFieldType::Bool:  *reinterpret_cast<bool*>(ptr) = value.boolValue; return true;
        case ScriptFieldType::Vec3:  *reinterpret_cast<math::Vec3*>(ptr) = value.vec3Value; return true;
        case ScriptFieldType::Entity: *reinterpret_cast<EntityID*>(ptr) = value.entityValue; return true;
        case ScriptFieldType::String:
        case ScriptFieldType::Prefab:
            *reinterpret_cast<std::string*>(ptr) = value.stringValue;
            return true;
    }
    return false;
}

std::vector<std::string> ScriptRegistry::GetClassNames() const
{
    std::vector<std::string> names;
    names.reserve(m_factories.size());
    for (const auto& kv : m_factories)
        names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace engine::script
