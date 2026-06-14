#include "core/ModuleSystem.hpp"

#include <algorithm>
#include <queue>

namespace engine::modules {

namespace {

template<class T>
bool ContainsId(std::span<const T> items, T id) noexcept
{
    for (const T& item : items)
    {
        if (item == id)
            return true;
    }
    return false;
}

bool FindCyclePath(const std::vector<std::vector<size_t>>& adjacency,
                   std::span<const size_t> activeNodes,
                   std::vector<size_t>& outCycle)
{
    enum class VisitState : uint8_t
    {
        Unvisited,
        Visiting,
        Visited
    };

    std::vector<VisitState> state(adjacency.size(), VisitState::Unvisited);
    std::vector<size_t> stack;
    std::vector<size_t> parent(adjacency.size(), static_cast<size_t>(-1));
    std::vector<bool> isActive(adjacency.size(), false);
    for (size_t index : activeNodes)
        isActive[index] = true;

    std::function<bool(size_t)> dfs = [&](size_t node) -> bool {
        state[node] = VisitState::Visiting;
        stack.push_back(node);

        for (size_t next : adjacency[node])
        {
            if (!isActive[next])
                continue;

            if (state[next] == VisitState::Unvisited)
            {
                parent[next] = node;
                if (dfs(next))
                    return true;
            }
            else if (state[next] == VisitState::Visiting)
            {
                auto it = std::find(stack.begin(), stack.end(), next);
                if (it != stack.end())
                {
                    outCycle.assign(it, stack.end());
                    outCycle.push_back(next);
                    return true;
                }
            }
        }

        stack.pop_back();
        state[node] = VisitState::Visited;
        return false;
    };

    for (size_t node : activeNodes)
    {
        if (state[node] == VisitState::Unvisited && dfs(node))
            return true;
    }

    return false;
}

} // namespace

bool ModuleCatalog::AddModule(std::unique_ptr<IEngineModule> module)
{
    m_lastError.clear();
    if (!module)
    {
        m_lastError = "cannot add null module";
        return false;
    }

    ResolvedModuleDescriptor descriptor;
    if (!NormalizeDescriptor(*module, descriptor))
        return false;

    if (HasDuplicateModuleId(descriptor.id))
    {
        m_lastError = "duplicate module id: " + std::string(descriptor.id.View());
        return false;
    }

    m_declared = false;
    m_modules.push_back(ModuleRecord{
        std::move(module),
        std::move(descriptor),
        {},
        {}
    });
    return true;
}

bool ModuleCatalog::DeclareAll()
{
    m_lastError.clear();
    m_allDeclaredServices.clear();
    for (ModuleRecord& module : m_modules)
    {
        module.providedServices.clear();
        module.contributions.clear();
    }

    for (size_t index = 0; index < m_modules.size(); ++index)
    {
        ModuleRecord& module = m_modules[index];
        try
        {
            ModuleDeclarationContext context(*this, index, module.descriptor.id);
            module.module->DeclareContributions(context);
        }
        catch (const std::exception& e)
        {
            m_lastError = "module declaration threw for '" + std::string(module.descriptor.name.View()) + "': " + e.what();
            return false;
        }
    }

    m_declared = true;
    return true;
}

const ResolvedModuleDescriptor& ModuleCatalog::GetModuleDescriptor(size_t index) const
{
    return m_modules.at(index).descriptor;
}

IEngineModule& ModuleCatalog::GetModule(size_t index) const
{
    return *m_modules.at(index).module;
}

const std::vector<ServiceDeclaration>& ModuleCatalog::GetProvidedServices(size_t index) const
{
    return m_modules.at(index).providedServices;
}

const std::vector<ContributionDeclaration>& ModuleCatalog::GetContributions(size_t index) const
{
    return m_modules.at(index).contributions;
}

const std::vector<CapabilityDeclaration>& ModuleCatalog::GetCapabilities(size_t index) const
{
    return m_modules.at(index).contributions;
}

bool ModuleCatalog::NormalizeDescriptor(const IEngineModule& module, ResolvedModuleDescriptor& outDescriptor)
{
    const ModuleDescriptor descriptor = module.Describe();
    if (descriptor.name.empty())
    {
        m_lastError = "module name must not be empty";
        return false;
    }
    if (descriptor.id.empty())
    {
        m_lastError = "module id must not be empty";
        return false;
    }

    outDescriptor.name = m_stringPool.Intern(descriptor.name);
    outDescriptor.id = ModuleId{m_stringPool.Intern(descriptor.id)};

    for (RawModuleId id : descriptor.requiredModules)
        outDescriptor.requiredModules.push_back(ModuleId{m_stringPool.Intern(id)});
    for (RawModuleId id : descriptor.optionalModules)
        outDescriptor.optionalModules.push_back(ModuleId{m_stringPool.Intern(id)});
    for (RawServiceId id : descriptor.requiredServices)
        outDescriptor.requiredServices.push_back(ServiceId{m_stringPool.Intern(id)});
    for (RawServiceId id : descriptor.optionalServices)
        outDescriptor.optionalServices.push_back(ServiceId{m_stringPool.Intern(id)});
    return true;
}

bool ModuleCatalog::HasDuplicateModuleId(const ModuleId& id) const noexcept
{
    for (const ModuleRecord& module : m_modules)
    {
        if (module.descriptor.id == id)
            return true;
    }
    return false;
}

bool ModuleCatalog::HasDeclaredServiceId(const ServiceId& id) const noexcept
{
    for (const ServiceDeclaration& declaration : m_allDeclaredServices)
    {
        if (declaration.id == id)
            return true;
    }
    return false;
}

void ModuleCatalog::AppendProvidedService(size_t moduleIndex, ServiceDeclaration declaration)
{
    if (!declaration.id.IsValid())
        throw std::runtime_error("provided service id must not be empty");
    if (HasDeclaredServiceId(declaration.id))
        throw std::runtime_error("duplicate provided service id: " + std::string(declaration.id.View()));

    m_modules.at(moduleIndex).providedServices.push_back(declaration);
    m_allDeclaredServices.push_back(declaration);
}

void ModuleCatalog::AppendContribution(size_t moduleIndex, ContributionDeclaration declaration)
{
    if (!declaration.factory)
        throw std::runtime_error("contribution factory must not be empty");
    m_modules.at(moduleIndex).contributions.push_back(std::move(declaration));
}

void ModuleCatalog::AppendCapability(size_t moduleIndex, CapabilityDeclaration declaration)
{
    AppendContribution(moduleIndex, std::move(declaration));
}

bool ServiceContainer::HasService(ServiceId id) const noexcept
{
    return FindEntry(id) != nullptr;
}

void ServiceContainer::UnregisterModuleServices(ModuleId provider) noexcept
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (it->declaration.tier == ServiceTier::Module && it->declaration.provider == provider)
            it = m_entries.erase(it);
        else
            ++it;
    }

    for (auto it = m_descriptions.begin(); it != m_descriptions.end();)
    {
        if (it->tier == ServiceTier::Module && it->provider == provider)
            it = m_descriptions.erase(it);
        else
            ++it;
    }
}

bool ServiceContainer::Register(ServiceTier tier,
                                ServiceId id,
                                std::type_index type,
                                const ModuleId* provider,
                                void* instance)
{
    m_lastError.clear();
    if (!id.IsValid())
    {
        m_lastError = "service id must not be empty";
        return false;
    }
    if (instance == nullptr)
    {
        m_lastError = "service instance must not be null";
        return false;
    }

    Entry* existing = FindEntry(id);
    if (existing != nullptr)
    {
        m_lastError = "service already registered: " + std::string(id.View());
        return false;
    }

    Entry entry{};
    entry.declaration.id = id;
    entry.declaration.type = type;
    entry.declaration.tier = tier;
    if (provider != nullptr)
        entry.declaration.provider = *provider;
    entry.instance = instance;

    m_entries.push_back(entry);
    m_descriptions.push_back(entry.declaration);
    return true;
}

const ServiceContainer::Entry* ServiceContainer::FindEntry(ServiceId id) const noexcept
{
    for (const Entry& entry : m_entries)
    {
        if (entry.declaration.id == id)
            return &entry;
    }
    return nullptr;
}

ServiceContainer::Entry* ServiceContainer::FindEntry(ServiceId id) noexcept
{
    for (Entry& entry : m_entries)
    {
        if (entry.declaration.id == id)
            return &entry;
    }
    return nullptr;
}

void ModuleInitContext::EnsureDeclaredRequired(ServiceId id) const
{
    if (!ContainsId<ServiceId>(m_descriptor->requiredServices, id))
        throw std::runtime_error("required service access was not declared");
}

void ModuleInitContext::EnsureDeclaredOptional(ServiceId id) const
{
    if (!ContainsId<ServiceId>(m_descriptor->requiredServices, id) &&
        !ContainsId<ServiceId>(m_descriptor->optionalServices, id))
    {
        throw std::runtime_error("optional service access was not declared");
    }
}

void ModuleInitContext::EnsureDeclaredProvided(ServiceId id, std::type_index type) const
{
    for (const ServiceDeclaration& declaration : m_providedServices)
    {
        if (declaration.id == id)
        {
            if (declaration.type != type)
                throw std::runtime_error("provided service type does not match declaration");
            return;
        }
    }
    throw std::runtime_error("provided service registration was not declared");
}

bool ModuleGraph::Build(const ModuleCatalog& catalog,
                        std::span<const ServiceDeclaration> preRegisteredServices)
{
    m_lastError.clear();
    m_initOrder.clear();

    const size_t moduleCount = catalog.ModuleCount();
    std::unordered_map<ModuleId, size_t> moduleIndices;
    moduleIndices.reserve(moduleCount);
    for (size_t index = 0; index < moduleCount; ++index)
        moduleIndices.emplace(catalog.GetModuleDescriptor(index).id, index);

    std::unordered_map<ServiceId, size_t> serviceProviders;
    for (const ServiceDeclaration& service : preRegisteredServices)
        serviceProviders.emplace(service.id, static_cast<size_t>(-1));

    for (size_t index = 0; index < moduleCount; ++index)
    {
        for (const ServiceDeclaration& service : catalog.GetProvidedServices(index))
        {
            auto [it, inserted] = serviceProviders.emplace(service.id, index);
            if (!inserted)
            {
                m_lastError = "service provider collision for '" + std::string(service.id.View()) + "'";
                return false;
            }
        }
    }

    std::vector<std::vector<size_t>> adjacency(moduleCount);
    std::vector<size_t> indegree(moduleCount, 0u);
    auto addEdge = [&](size_t from, size_t to) {
        adjacency[from].push_back(to);
        ++indegree[to];
    };

    for (size_t index = 0; index < moduleCount; ++index)
    {
        const ResolvedModuleDescriptor& descriptor = catalog.GetModuleDescriptor(index);

        for (const ModuleId& required : descriptor.requiredModules)
        {
            const auto it = moduleIndices.find(required);
            if (it == moduleIndices.end())
            {
                m_lastError = "missing required module '" + std::string(required.View()) + "'";
                return false;
            }
            addEdge(it->second, index);
        }

        for (const ModuleId& optional : descriptor.optionalModules)
        {
            const auto it = moduleIndices.find(optional);
            if (it != moduleIndices.end())
                addEdge(it->second, index);
        }

        for (const ServiceId& required : descriptor.requiredServices)
        {
            const auto it = serviceProviders.find(required);
            if (it == serviceProviders.end())
            {
                m_lastError = "missing required service '" + std::string(required.View()) + "'";
                return false;
            }
            if (it->second != static_cast<size_t>(-1))
                addEdge(it->second, index);
        }

        for (const ServiceId& optional : descriptor.optionalServices)
        {
            const auto it = serviceProviders.find(optional);
            if (it != serviceProviders.end() && it->second != static_cast<size_t>(-1))
                addEdge(it->second, index);
        }
    }

    std::queue<size_t> ready;
    for (size_t index = 0; index < moduleCount; ++index)
    {
        if (indegree[index] == 0u)
            ready.push(index);
    }

    while (!ready.empty())
    {
        const size_t current = ready.front();
        ready.pop();
        m_initOrder.push_back(current);

        for (size_t dependent : adjacency[current])
        {
            if (--indegree[dependent] == 0u)
                ready.push(dependent);
        }
    }

    if (m_initOrder.size() != moduleCount)
    {
        std::vector<size_t> activeNodes;
        activeNodes.reserve(moduleCount - m_initOrder.size());
        for (size_t index = 0; index < moduleCount; ++index)
        {
            if (indegree[index] != 0u)
                activeNodes.push_back(index);
        }

        std::vector<size_t> cyclePath;
        if (FindCyclePath(adjacency, activeNodes, cyclePath) && !cyclePath.empty())
        {
            m_lastError = "module graph contains a cycle: ";
            for (size_t i = 0; i < cyclePath.size(); ++i)
            {
                if (i != 0u)
                    m_lastError += " -> ";
                m_lastError += std::string(catalog.GetModuleDescriptor(cyclePath[i]).name.View());
            }
        }
        else
        {
            m_lastError = "module graph contains a cycle";
        }
        m_initOrder.clear();
        return false;
    }

    return true;
}

bool ModuleRuntime::InitializeAll(ModuleCatalog& catalog,
                                  const ModuleGraph& graph,
                                  ServiceContainer& services)
{
    m_lastError.clear();
    m_initializedModules.clear();

    for (size_t moduleIndex : graph.InitializationOrder())
    {
        const ResolvedModuleDescriptor& descriptor = catalog.GetModuleDescriptor(moduleIndex);
        ModuleInitContext context(services, descriptor, catalog.GetProvidedServices(moduleIndex));
        if (!catalog.GetModule(moduleIndex).Initialize(context))
        {
            m_lastError = "module initialize failed: " + std::string(descriptor.name.View());
            ShutdownAll(catalog, services);
            return false;
        }
        m_initializedModules.push_back(moduleIndex);
    }

    return true;
}

void ModuleRuntime::ShutdownAll(ModuleCatalog& catalog, ServiceContainer& services) noexcept
{
    for (auto it = m_initializedModules.rbegin(); it != m_initializedModules.rend(); ++it)
    {
        const size_t moduleIndex = *it;
        const ResolvedModuleDescriptor& descriptor = catalog.GetModuleDescriptor(moduleIndex);
        ModuleShutdownContext context(services, descriptor);
        catalog.GetModule(moduleIndex).Shutdown(context);
        services.UnregisterModuleServices(descriptor.id);
    }
    m_initializedModules.clear();
}

} // namespace engine::modules
