#pragma once
#include "addons/script/ComponentScript.hpp"
#include "core/Math.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::script {

using ScriptFactory = std::function<std::unique_ptr<ComponentScript>()>;

enum class ScriptFieldType
{
    Float,
    Int,
    Bool,
    Vec3,
    String,
    Prefab,
    Entity
};

struct ScriptFieldValue
{
    ScriptFieldType type = ScriptFieldType::Float;
    float           floatValue = 0.f;
    int32_t         intValue = 0;
    bool            boolValue = false;
    math::Vec3      vec3Value{};
    std::string     stringValue;
    EntityID        entityValue = NULL_ENTITY;
};

struct ScriptFieldMeta
{
    std::string name;
    ScriptFieldType type = ScriptFieldType::Float;
    size_t offset = 0u;
};

class ScriptRegistry
{
public:
    void Register(std::string className, ScriptFactory factory);

    [[nodiscard]] std::unique_ptr<ComponentScript> Create(const std::string& className) const;
    [[nodiscard]] bool   Has(const std::string& className) const noexcept;
    [[nodiscard]] size_t GetFactoryCount() const noexcept { return m_factories.size(); }
    [[nodiscard]] const std::vector<ScriptFieldMeta>* GetFields(const std::string& className) const noexcept;
    [[nodiscard]] bool ReadField(const ComponentScript& script,
                                 const std::string& className,
                                 const std::string& fieldName,
                                 ScriptFieldValue& outValue) const;
    [[nodiscard]] bool WriteField(ComponentScript& script,
                                  const std::string& className,
                                  const std::string& fieldName,
                                  const ScriptFieldValue& value) const;

    // Sortierte Liste aller registrierten Klassennamen (fuer Inspector-Picker).
    [[nodiscard]] std::vector<std::string> GetClassNames() const;

    template<typename T>
    void Register(std::string className)
    {
        Register(std::move(className), [] { return std::make_unique<T>(); });
    }

    template<typename T>
    void RegisterField(const std::string& className,
                       std::string fieldName,
                       ScriptFieldType type,
                       float T::*member)
    {
        RegisterFieldInternal(className, std::move(fieldName), type, MemberOffset(member));
    }

    template<typename T>
    void RegisterField(const std::string& className,
                       std::string fieldName,
                       ScriptFieldType type,
                       int32_t T::*member)
    {
        RegisterFieldInternal(className, std::move(fieldName), type, MemberOffset(member));
    }

    template<typename T>
    void RegisterField(const std::string& className,
                       std::string fieldName,
                       ScriptFieldType type,
                       bool T::*member)
    {
        RegisterFieldInternal(className, std::move(fieldName), type, MemberOffset(member));
    }

    template<typename T>
    void RegisterField(const std::string& className,
                       std::string fieldName,
                       ScriptFieldType type,
                       math::Vec3 T::*member)
    {
        RegisterFieldInternal(className, std::move(fieldName), type, MemberOffset(member));
    }

    template<typename T>
    void RegisterField(const std::string& className,
                       std::string fieldName,
                       ScriptFieldType type,
                       std::string T::*member)
    {
        RegisterFieldInternal(className, std::move(fieldName), type, MemberOffset(member));
    }

    template<typename T>
    void RegisterField(const std::string& className,
                       std::string fieldName,
                       ScriptFieldType type,
                       EntityID T::*member)
    {
        RegisterFieldInternal(className, std::move(fieldName), type, MemberOffset(member));
    }

private:
    void RegisterFieldInternal(const std::string& className,
                               std::string fieldName,
                               ScriptFieldType type,
                               size_t offset);
    [[nodiscard]] const ScriptFieldMeta* FindField(const std::string& className,
                                                   const std::string& fieldName) const noexcept;

    template<typename T, typename M>
    static size_t MemberOffset(M T::*member)
    {
        const T* base = reinterpret_cast<const T*>(0x1000);
        const auto* field = &(base->*member);
        return static_cast<size_t>(
            reinterpret_cast<const uint8_t*>(field) - reinterpret_cast<const uint8_t*>(base));
    }

    std::unordered_map<std::string, ScriptFactory> m_factories;
    std::unordered_map<std::string, std::vector<ScriptFieldMeta>> m_fields;
};

} // namespace engine::script
