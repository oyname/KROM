#pragma once

#include "core/InternedString.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace engine::modules {

using RawModuleId = std::string_view;
using RawServiceId = std::string_view;

struct ModuleId
{
    InternedString value{};

    [[nodiscard]] std::string_view View() const noexcept { return value.View(); }
    [[nodiscard]] bool IsValid() const noexcept { return value.IsValid(); }

    friend bool operator==(const ModuleId& a, const ModuleId& b) noexcept = default;
};

struct ServiceId
{
    InternedString value{};

    [[nodiscard]] std::string_view View() const noexcept { return value.View(); }
    [[nodiscard]] bool IsValid() const noexcept { return value.IsValid(); }

    friend bool operator==(const ServiceId& a, const ServiceId& b) noexcept = default;
};

enum class ServiceTier
{
    Core,
    Module
};

struct ModuleDescriptor
{
    std::string_view name{};
    RawModuleId id{};
    std::span<const RawModuleId> requiredModules{};
    std::span<const RawModuleId> optionalModules{};
    std::span<const RawServiceId> requiredServices{};
    std::span<const RawServiceId> optionalServices{};
};

struct ResolvedModuleDescriptor
{
    InternedString name{};
    ModuleId id{};
    std::vector<ModuleId> requiredModules;
    std::vector<ModuleId> optionalModules;
    std::vector<ServiceId> requiredServices;
    std::vector<ServiceId> optionalServices;
};

struct ServiceDeclaration
{
    ServiceId id{};
    std::type_index type = typeid(void);
    ServiceTier tier = ServiceTier::Module;
    ModuleId provider{};
};

struct ContributionDeclaration
{
    std::type_index type = typeid(void);
    InternedString name{};
    ModuleId provider{};
    std::function<std::shared_ptr<void>()> factory;
};

template<class TContribution>
struct ContributionDescriptor
{
    std::string_view name{};
    std::function<std::unique_ptr<TContribution>()> factory;
};

using CapabilityDeclaration = ContributionDeclaration;

template<class TCapability>
using CapabilityDescriptor = ContributionDescriptor<TCapability>;

class ModuleDeclarationContext;
class ModuleInitContext;
class ModuleShutdownContext;
class ServiceContainer;

class IEngineModule
{
public:
    virtual ~IEngineModule() = default;

    [[nodiscard]] virtual ModuleDescriptor Describe() const noexcept = 0;
    virtual void DeclareContributions(ModuleDeclarationContext& ctx) const = 0;
    virtual bool Initialize(ModuleInitContext& ctx) = 0;
    virtual void Shutdown(ModuleShutdownContext& ctx) noexcept = 0;
};

template<class T>
struct ServiceTraits;

namespace detail {

template<class T, class = void>
struct HasServiceTraits : std::false_type {};

template<class T>
struct HasServiceTraits<T, std::void_t<decltype(ServiceTraits<T>::kId)>> : std::true_type {};

} // namespace detail

class ModuleCatalog
{
public:
    ModuleCatalog() = default;

    [[nodiscard]] bool AddModule(std::unique_ptr<IEngineModule> module);
    [[nodiscard]] bool DeclareAll();

    [[nodiscard]] size_t ModuleCount() const noexcept { return m_modules.size(); }
    [[nodiscard]] const ResolvedModuleDescriptor& GetModuleDescriptor(size_t index) const;
    [[nodiscard]] IEngineModule& GetModule(size_t index) const;
    [[nodiscard]] const std::vector<ServiceDeclaration>& GetProvidedServices(size_t index) const;
    [[nodiscard]] const std::vector<ContributionDeclaration>& GetContributions(size_t index) const;
    [[nodiscard]] const std::vector<CapabilityDeclaration>& GetCapabilities(size_t index) const;
    [[nodiscard]] const std::vector<ServiceDeclaration>& GetAllDeclaredServices() const noexcept { return m_allDeclaredServices; }
    [[nodiscard]] const std::string& GetLastError() const noexcept { return m_lastError; }

    template<class TCapability>
    [[nodiscard]] std::vector<const ContributionDeclaration*> GetContributionDeclarations() const
    {
        std::vector<const ContributionDeclaration*> matches;
        for (const ModuleRecord& module : m_modules)
        {
            for (const ContributionDeclaration& contribution : module.contributions)
            {
                if (contribution.type == std::type_index(typeid(TCapability)))
                    matches.push_back(&contribution);
            }
        }
        return matches;
    }

    template<class TCapability>
    [[nodiscard]] std::vector<const CapabilityDeclaration*> GetCapabilityDeclarations() const
    {
        return GetContributionDeclarations<TCapability>();
    }

    [[nodiscard]] ModuleId InternModuleId(RawModuleId id) { return ModuleId{m_stringPool.Intern(id)}; }
    [[nodiscard]] ServiceId InternServiceId(RawServiceId id) { return ServiceId{m_stringPool.Intern(id)}; }

private:
    friend class ModuleDeclarationContext;

    struct ModuleRecord
    {
        std::unique_ptr<IEngineModule> module;
        ResolvedModuleDescriptor descriptor;
        std::vector<ServiceDeclaration> providedServices;
        std::vector<ContributionDeclaration> contributions;
    };

    [[nodiscard]] bool NormalizeDescriptor(const IEngineModule& module, ResolvedModuleDescriptor& outDescriptor);
    [[nodiscard]] bool HasDuplicateModuleId(const ModuleId& id) const noexcept;
    [[nodiscard]] bool HasDeclaredServiceId(const ServiceId& id) const noexcept;

    void AppendProvidedService(size_t moduleIndex, ServiceDeclaration declaration);
    void AppendContribution(size_t moduleIndex, ContributionDeclaration declaration);
    void AppendCapability(size_t moduleIndex, CapabilityDeclaration declaration);

    StringPool m_stringPool;
    std::vector<ModuleRecord> m_modules;
    std::vector<ServiceDeclaration> m_allDeclaredServices;
    bool m_declared = false;
    std::string m_lastError;
};

class ModuleDeclarationContext
{
public:
    template<class TService>
    void ProvideService(RawServiceId id)
    {
        ServiceDeclaration declaration{};
        declaration.id = m_catalog->InternServiceId(id);
        declaration.type = std::type_index(typeid(TService));
        declaration.tier = ServiceTier::Module;
        declaration.provider = m_moduleId;
        m_catalog->AppendProvidedService(m_moduleIndex, declaration);
    }

    template<class TContribution>
    void Contribute(const ContributionDescriptor<TContribution>& descriptor)
    {
        ContributionDeclaration erased{};
        erased.type = std::type_index(typeid(TContribution));
        erased.name = m_catalog->m_stringPool.Intern(descriptor.name);
        erased.provider = m_moduleId;
        erased.factory = [factory = descriptor.factory]() -> std::shared_ptr<void> {
            if (!factory)
                return {};
            std::shared_ptr<TContribution> typedInstance(factory());
            return std::static_pointer_cast<void>(typedInstance);
        };
        m_catalog->AppendContribution(m_moduleIndex, std::move(erased));
    }

    template<class TCapability>
    void ContributeCapability(const CapabilityDescriptor<TCapability>& descriptor)
    {
        Contribute<TCapability>(descriptor);
    }

private:
    friend class ModuleCatalog;

    ModuleDeclarationContext(ModuleCatalog& catalog, size_t moduleIndex, ModuleId moduleId) noexcept
        : m_catalog(&catalog)
        , m_moduleIndex(moduleIndex)
        , m_moduleId(moduleId)
    {
    }

    ModuleCatalog* m_catalog = nullptr;
    size_t m_moduleIndex = 0u;
    ModuleId m_moduleId{};
};

class ServiceContainer
{
public:
    template<class TService>
    [[nodiscard]] bool RegisterCore(RawServiceId id, TService& service)
    {
        return Register(ServiceTier::Core, ServiceId{m_stringPool.Intern(id)}, std::type_index(typeid(TService)), nullptr, &service);
    }

    template<class TService>
    [[nodiscard]] bool RegisterModule(ServiceId id, ModuleId provider, TService& service)
    {
        return Register(ServiceTier::Module, id, std::type_index(typeid(TService)), &provider, &service);
    }

    [[nodiscard]] const std::vector<ServiceDeclaration>& DescribeServices() const noexcept
    {
        return m_descriptions;
    }

    [[nodiscard]] ServiceId ResolveServiceId(RawServiceId id) const
    {
        return ServiceId{m_stringPool.Intern(id)};
    }

    [[nodiscard]] bool HasService(ServiceId id) const noexcept;
    [[nodiscard]] const std::string& GetLastError() const noexcept { return m_lastError; }

    template<class TService>
    [[nodiscard]] TService& GetRequired(ServiceId id) const
    {
        const Entry* entry = FindEntry(id);
        if (entry == nullptr)
            throw std::runtime_error("required service is not registered");
        VerifyType<TService>(*entry);
        return *static_cast<TService*>(entry->instance);
    }

    template<class TService>
    [[nodiscard]] TService* GetOptional(ServiceId id) const
    {
        const Entry* entry = FindEntry(id);
        if (entry == nullptr)
            return nullptr;
        VerifyType<TService>(*entry);
        return static_cast<TService*>(entry->instance);
    }

    template<class TService, std::enable_if_t<detail::HasServiceTraits<TService>::value, int> = 0>
    [[nodiscard]] TService& GetRequired() const
    {
        return GetRequired<TService>(ServiceId{m_stringPool.Intern(ServiceTraits<TService>::kId)});
    }

    template<class TService, std::enable_if_t<detail::HasServiceTraits<TService>::value, int> = 0>
    [[nodiscard]] TService* GetOptional() const
    {
        return GetOptional<TService>(ServiceId{m_stringPool.Intern(ServiceTraits<TService>::kId)});
    }

    void UnregisterModuleServices(ModuleId provider) noexcept;

private:
    struct Entry
    {
        ServiceDeclaration declaration;
        void* instance = nullptr;
    };

    template<class TService>
    static void VerifyType(const Entry& entry)
    {
        if (entry.declaration.type != std::type_index(typeid(TService)))
            throw std::runtime_error("service type mismatch during lookup");
    }

    [[nodiscard]] bool Register(ServiceTier tier,
                                ServiceId id,
                                std::type_index type,
                                const ModuleId* provider,
                                void* instance);
    [[nodiscard]] const Entry* FindEntry(ServiceId id) const noexcept;
    [[nodiscard]] Entry* FindEntry(ServiceId id) noexcept;

    StringPool m_stringPool;
    std::vector<Entry> m_entries;
    std::vector<ServiceDeclaration> m_descriptions;
    std::string m_lastError;
};

class ModuleInitContext
{
public:
    ModuleInitContext(ServiceContainer& services,
                      const ResolvedModuleDescriptor& descriptor,
                      std::span<const ServiceDeclaration> providedServices) noexcept
        : m_services(&services)
        , m_descriptor(&descriptor)
        , m_providedServices(providedServices)
    {
    }

    template<class TService>
    [[nodiscard]] TService& GetRequired(ServiceId id) const
    {
        EnsureDeclaredRequired(id);
        return m_services->GetRequired<TService>(id);
    }

    template<class TService>
    [[nodiscard]] TService* GetOptional(ServiceId id) const
    {
        EnsureDeclaredOptional(id);
        return m_services->GetOptional<TService>(id);
    }

    template<class TService, std::enable_if_t<detail::HasServiceTraits<TService>::value, int> = 0>
    [[nodiscard]] TService& GetRequired() const
    {
        return GetRequired<TService>(m_services->ResolveServiceId(ServiceTraits<TService>::kId));
    }

    template<class TService, std::enable_if_t<detail::HasServiceTraits<TService>::value, int> = 0>
    [[nodiscard]] TService* GetOptional() const
    {
        return GetOptional<TService>(m_services->ResolveServiceId(ServiceTraits<TService>::kId));
    }

    template<class TService>
    void RegisterProvidedService(ServiceId id, TService& service)
    {
        EnsureDeclaredProvided(id, std::type_index(typeid(TService)));
        if (!m_services->RegisterModule(id, m_descriptor->id, service))
            throw std::runtime_error(m_services->GetLastError());
    }

private:
    void EnsureDeclaredRequired(ServiceId id) const;
    void EnsureDeclaredOptional(ServiceId id) const;
    void EnsureDeclaredProvided(ServiceId id, std::type_index type) const;

    ServiceContainer* m_services = nullptr;
    const ResolvedModuleDescriptor* m_descriptor = nullptr;
    std::span<const ServiceDeclaration> m_providedServices;
};

class ModuleShutdownContext
{
public:
    ModuleShutdownContext(ServiceContainer& services,
                          const ResolvedModuleDescriptor& descriptor) noexcept
        : m_services(&services)
        , m_descriptor(&descriptor)
    {
    }

    template<class TService>
    [[nodiscard]] TService* GetOptional(ServiceId id) const
    {
        return m_services->GetOptional<TService>(id);
    }

private:
    ServiceContainer* m_services = nullptr;
    const ResolvedModuleDescriptor* m_descriptor = nullptr;
};

class ModuleGraph
{
public:
    [[nodiscard]] bool Build(const ModuleCatalog& catalog,
                             std::span<const ServiceDeclaration> preRegisteredServices);

    [[nodiscard]] const std::vector<size_t>& InitializationOrder() const noexcept
    {
        return m_initOrder;
    }

    [[nodiscard]] const std::string& GetLastError() const noexcept { return m_lastError; }

private:
    std::vector<size_t> m_initOrder;
    std::string m_lastError;
};

class ModuleRuntime
{
public:
    [[nodiscard]] bool InitializeAll(ModuleCatalog& catalog,
                                     const ModuleGraph& graph,
                                     ServiceContainer& services);
    void ShutdownAll(ModuleCatalog& catalog, ServiceContainer& services) noexcept;

    [[nodiscard]] const std::string& GetLastError() const noexcept { return m_lastError; }

private:
    std::vector<size_t> m_initializedModules;
    std::string m_lastError;
};

template<class TContribution>
[[nodiscard]] std::shared_ptr<TContribution> InstantiateContribution(const ContributionDeclaration& declaration);

template<class TContribution>
class ContributionRegistry
{
public:
    explicit ContributionRegistry(const ModuleCatalog& catalog)
        : m_declarations(catalog.GetContributionDeclarations<TContribution>())
    {
    }

    [[nodiscard]] size_t Size() const noexcept { return m_declarations.size(); }

    [[nodiscard]] const ContributionDeclaration& Declaration(size_t index) const
    {
        return *m_declarations.at(index);
    }

    [[nodiscard]] std::shared_ptr<TContribution> Instantiate(size_t index) const
    {
        return InstantiateContribution<TContribution>(*m_declarations.at(index));
    }

    [[nodiscard]] std::vector<std::shared_ptr<TContribution>> InstantiateAll() const
    {
        std::vector<std::shared_ptr<TContribution>> instances;
        instances.reserve(m_declarations.size());
        for (const ContributionDeclaration* declaration : m_declarations)
            instances.push_back(InstantiateContribution<TContribution>(*declaration));
        return instances;
    }

private:
    std::vector<const ContributionDeclaration*> m_declarations;
};

template<class TContribution>
[[nodiscard]] std::shared_ptr<TContribution> InstantiateContribution(const ContributionDeclaration& declaration)
{
    if (declaration.type != std::type_index(typeid(TContribution)))
        throw std::runtime_error("contribution type mismatch during instantiation");
    if (!declaration.factory)
        return {};
    return std::static_pointer_cast<TContribution>(declaration.factory());
}

template<class TCapability>
[[nodiscard]] std::shared_ptr<TCapability> InstantiateCapability(const CapabilityDeclaration& declaration)
{
    return InstantiateContribution<TCapability>(declaration);
}

} // namespace engine::modules

namespace std {

template<>
struct hash<engine::modules::ModuleId>
{
    size_t operator()(const engine::modules::ModuleId& value) const noexcept
    {
        return std::hash<engine::InternedString>{}(value.value);
    }
};

template<>
struct hash<engine::modules::ServiceId>
{
    size_t operator()(const engine::modules::ServiceId& value) const noexcept
    {
        return std::hash<engine::InternedString>{}(value.value);
    }
};

} // namespace std
