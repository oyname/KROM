#include "TestFramework.hpp"
#include "core/ModuleSystem.hpp"

using namespace engine;
using namespace engine::modules;

namespace {

struct IClockService
{
    virtual ~IClockService() = default;
    virtual int Read() const = 0;
};

struct CounterService
{
    int value = 0;
};

struct DummyCapability
{
    virtual ~DummyCapability() = default;
    virtual int Score() const = 0;
};

class DummyCapabilityImpl final : public DummyCapability
{
public:
    explicit DummyCapabilityImpl(int score) noexcept
        : m_score(score)
    {
    }

    int Score() const override { return m_score; }

private:
    int m_score = 0;
};

class CoreConsumerModule final : public IEngineModule
{
public:
    ModuleDescriptor Describe() const noexcept override
    {
        static constexpr RawServiceId requiredServices[] = {"engine.clock"};
        return ModuleDescriptor{
            "CoreConsumer",
            "engine.core_consumer",
            {},
            {},
            requiredServices,
            {}
        };
    }

    void DeclareContributions(ModuleDeclarationContext&) const override {}

    bool Initialize(ModuleInitContext& ctx) override
    {
        m_value = ctx.GetRequired<IClockService>(ServiceId{m_clockId}).Read();
        return true;
    }

    void Shutdown(ModuleShutdownContext&) noexcept override {}

    void BindIds(ServiceId clockId) noexcept
    {
        m_clockId = clockId.value;
    }

    int Value() const noexcept { return m_value; }

private:
    InternedString m_clockId{};
    int m_value = -1;
};

class ProviderModule final : public IEngineModule
{
public:
    ModuleDescriptor Describe() const noexcept override
    {
        return ModuleDescriptor{
            "Provider",
            "engine.provider",
            {},
            {},
            {},
            {}
        };
    }

    void DeclareContributions(ModuleDeclarationContext& ctx) const override
    {
        ctx.ProvideService<CounterService>("engine.counter");
        ctx.Contribute<DummyCapability>(ContributionDescriptor<DummyCapability>{
            "dummy.capability",
            []() { return std::make_unique<DummyCapabilityImpl>(7); }
        });
    }

    bool Initialize(ModuleInitContext& ctx) override
    {
        ctx.RegisterProvidedService<CounterService>(ServiceId{m_counterId}, m_service);
        m_service.value = 42;
        return true;
    }

    void Shutdown(ModuleShutdownContext&) noexcept override {}

    void BindIds(ServiceId counterId) noexcept
    {
        m_counterId = counterId.value;
    }

private:
    InternedString m_counterId{};
    CounterService m_service{};
};

class ConsumerModule final : public IEngineModule
{
public:
    ModuleDescriptor Describe() const noexcept override
    {
        static constexpr RawServiceId requiredServices[] = {"engine.counter"};
        static constexpr RawServiceId optionalServices[] = {"engine.debug_overlay"};
        return ModuleDescriptor{
            "Consumer",
            "engine.consumer",
            {},
            {},
            requiredServices,
            optionalServices
        };
    }

    void DeclareContributions(ModuleDeclarationContext&) const override {}

    bool Initialize(ModuleInitContext& ctx) override
    {
        m_seenValue = ctx.GetRequired<CounterService>(ServiceId{m_counterId}).value;
        m_debugSeen = ctx.GetOptional<CounterService>(ServiceId{m_optionalId}) != nullptr;
        return true;
    }

    void Shutdown(ModuleShutdownContext&) noexcept override {}

    void BindIds(ServiceId counterId, ServiceId optionalId) noexcept
    {
        m_counterId = counterId.value;
        m_optionalId = optionalId.value;
    }

    int SeenValue() const noexcept { return m_seenValue; }
    bool DebugSeen() const noexcept { return m_debugSeen; }

private:
    InternedString m_counterId{};
    InternedString m_optionalId{};
    int m_seenValue = -1;
    bool m_debugSeen = false;
};

class MissingRequirementModule final : public IEngineModule
{
public:
    ModuleDescriptor Describe() const noexcept override
    {
        static constexpr RawServiceId requiredServices[] = {"engine.missing"};
        return ModuleDescriptor{
            "MissingRequirement",
            "engine.missing_requirement",
            {},
            {},
            requiredServices,
            {}
        };
    }

    void DeclareContributions(ModuleDeclarationContext&) const override {}
    bool Initialize(ModuleInitContext&) override { return true; }
    void Shutdown(ModuleShutdownContext&) noexcept override {}
};

class CycleModuleA final : public IEngineModule
{
public:
    ModuleDescriptor Describe() const noexcept override
    {
        static constexpr RawModuleId requiredModules[] = {"engine.cycle_b"};
        return ModuleDescriptor{
            "CycleA",
            "engine.cycle_a",
            requiredModules,
            {},
            {},
            {}
        };
    }

    void DeclareContributions(ModuleDeclarationContext&) const override {}
    bool Initialize(ModuleInitContext&) override { return true; }
    void Shutdown(ModuleShutdownContext&) noexcept override {}
};

class CycleModuleB final : public IEngineModule
{
public:
    ModuleDescriptor Describe() const noexcept override
    {
        static constexpr RawModuleId requiredModules[] = {"engine.cycle_a"};
        return ModuleDescriptor{
            "CycleB",
            "engine.cycle_b",
            requiredModules,
            {},
            {},
            {}
        };
    }

    void DeclareContributions(ModuleDeclarationContext&) const override {}
    bool Initialize(ModuleInitContext&) override { return true; }
    void Shutdown(ModuleShutdownContext&) noexcept override {}
};

class ClockService final : public IClockService
{
public:
    explicit ClockService(int value) noexcept
        : m_value(value)
    {
    }

    int Read() const override { return m_value; }

private:
    int m_value = 0;
};

void TestStringPoolInternsStableStorage(test::TestContext& ctx)
{
    StringPool pool;
    const InternedString a = pool.Intern("engine.render");
    const InternedString b = pool.Intern(std::string("engine.render"));
    CHECK(ctx, a.IsValid());
    CHECK_EQ(ctx, a, b);
    CHECK_EQ(ctx, std::string(a.View()), std::string("engine.render"));
}

void TestServiceContainerCoreAndTierRules(test::TestContext& ctx)
{
    ServiceContainer services;
    ClockService clock(17);
    CHECK(ctx, services.RegisterCore<IClockService>("engine.clock", clock));
    const ServiceId clockId = services.ResolveServiceId("engine.clock");
    CHECK_EQ(ctx, services.GetRequired<IClockService>(clockId).Read(), 17);
    CounterService counter{};
    CHECK(ctx, !services.RegisterModule<CounterService>(clockId, ModuleId{}, counter));
}

void TestModuleGraphWithCoreService(test::TestContext& ctx)
{
    ModuleCatalog catalog;
    auto consumer = std::make_unique<CoreConsumerModule>();
    CoreConsumerModule* consumerPtr = consumer.get();
    CHECK(ctx, catalog.AddModule(std::move(consumer)));
    CHECK(ctx, catalog.DeclareAll());

    ServiceContainer services;
    ClockService clock(23);
    CHECK(ctx, services.RegisterCore<IClockService>("engine.clock", clock));

    const ServiceId clockId = catalog.InternServiceId("engine.clock");
    consumerPtr->BindIds(clockId);

    ModuleGraph graph;
    CHECK(ctx, graph.Build(catalog, services.DescribeServices()));

    ModuleRuntime runtime;
    CHECK(ctx, runtime.InitializeAll(catalog, graph, services));
    CHECK_EQ(ctx, consumerPtr->Value(), 23);
    runtime.ShutdownAll(catalog, services);
}

void TestModuleCatalogDeclarationsAndCapabilities(test::TestContext& ctx)
{
    ModuleCatalog catalog;
    auto provider = std::make_unique<ProviderModule>();
    ProviderModule* providerPtr = provider.get();
    CHECK(ctx, catalog.AddModule(std::move(provider)));
    CHECK(ctx, catalog.DeclareAll());

    const ServiceId counterId = catalog.InternServiceId("engine.counter");
    providerPtr->BindIds(counterId);

    CHECK_EQ(ctx, catalog.GetProvidedServices(0).size(), 1u);
    CHECK_EQ(ctx, std::string(catalog.GetProvidedServices(0)[0].id.View()), std::string("engine.counter"));

    ContributionRegistry<DummyCapability> registry(catalog);
    CHECK_EQ(ctx, registry.Size(), 1u);
    CHECK_EQ(ctx, std::string(registry.Declaration(0).name.View()), std::string("dummy.capability"));
    const auto instance = registry.Instantiate(0);
    CHECK(ctx, instance != nullptr);
    CHECK_EQ(ctx, instance->Score(), 7);
}

void TestModuleGraphAndRuntime(test::TestContext& ctx)
{
    ModuleCatalog catalog;

    auto provider = std::make_unique<ProviderModule>();
    ProviderModule* providerPtr = provider.get();
    auto consumer = std::make_unique<ConsumerModule>();
    ConsumerModule* consumerPtr = consumer.get();

    CHECK(ctx, catalog.AddModule(std::move(provider)));
    CHECK(ctx, catalog.AddModule(std::move(consumer)));
    CHECK(ctx, catalog.DeclareAll());

    const ServiceId counterId = catalog.InternServiceId("engine.counter");
    const ServiceId overlayId = catalog.InternServiceId("engine.debug_overlay");
    providerPtr->BindIds(counterId);
    consumerPtr->BindIds(counterId, overlayId);

    ServiceContainer services;
    ModuleGraph graph;
    CHECK(ctx, graph.Build(catalog, services.DescribeServices()));
    CHECK_EQ(ctx, graph.InitializationOrder().size(), 2u);

    ModuleRuntime runtime;
    CHECK(ctx, runtime.InitializeAll(catalog, graph, services));
    CHECK_EQ(ctx, consumerPtr->SeenValue(), 42);
    CHECK(ctx, !consumerPtr->DebugSeen());

    runtime.ShutdownAll(catalog, services);
    CHECK(ctx, !services.HasService(counterId));
}

void TestModuleGraphRejectsMissingRequiredService(test::TestContext& ctx)
{
    ModuleCatalog catalog;
    CHECK(ctx, catalog.AddModule(std::make_unique<MissingRequirementModule>()));
    CHECK(ctx, catalog.DeclareAll());

    ServiceContainer services;
    ModuleGraph graph;
    CHECK(ctx, !graph.Build(catalog, services.DescribeServices()));
}

void TestModuleGraphReportsCyclePath(test::TestContext& ctx)
{
    ModuleCatalog catalog;
    CHECK(ctx, catalog.AddModule(std::make_unique<CycleModuleA>()));
    CHECK(ctx, catalog.AddModule(std::make_unique<CycleModuleB>()));
    CHECK(ctx, catalog.DeclareAll());

    ServiceContainer services;
    ModuleGraph graph;
    CHECK(ctx, !graph.Build(catalog, services.DescribeServices()));
    const std::string error = graph.GetLastError();
    CHECK(ctx, error.find("CycleA") != std::string::npos);
    CHECK(ctx, error.find("CycleB") != std::string::npos);
    CHECK(ctx, error.find("->") != std::string::npos);
}

} // namespace

int RunModuleSystemTests()
{
    test::TestSuite suite("ModuleSystem");
    suite
        .Add("StringPool stable intern", TestStringPoolInternsStableStorage)
        .Add("ServiceContainer core and tier rules", TestServiceContainerCoreAndTierRules)
        .Add("Catalog declarations and capabilities", TestModuleCatalogDeclarationsAndCapabilities)
        .Add("Module graph with core service", TestModuleGraphWithCoreService)
        .Add("Module graph and runtime", TestModuleGraphAndRuntime)
        .Add("Missing required service rejected", TestModuleGraphRejectsMissingRequiredService)
        .Add("Cycle path reported", TestModuleGraphReportsCyclePath);

    return suite.Run();
}
