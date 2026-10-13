#include "TestFramework.hpp"
#include "events/EventBus.hpp"
#include <algorithm>
#include <vector>

using namespace engine::events;

namespace {

struct TestEvent
{
    int value = 0;
};

void TestPublishOrderStable(test::TestContext& ctx)
{
    EventBus bus;
    std::vector<int> calls;

    auto a = bus.Subscribe<TestEvent>([&](const TestEvent&) { calls.push_back(1); });
    auto b = bus.Subscribe<TestEvent>([&](const TestEvent&) { calls.push_back(2); });
    auto c = bus.Subscribe<TestEvent>([&](const TestEvent&) { calls.push_back(3); });

    bus.Publish(TestEvent{});

    CHECK_EQ(ctx, calls.size(), size_t{3});
    CHECK_EQ(ctx, calls[0], 1);
    CHECK_EQ(ctx, calls[1], 2);
    CHECK_EQ(ctx, calls[2], 3);

    (void)a; (void)b; (void)c;
}

void TestUnsubscribeDuringDispatch(test::TestContext& ctx)
{
    EventBus bus;
    std::vector<int> calls;
    Subscription victim;

    auto first = bus.Subscribe<TestEvent>([&](const TestEvent&) {
        calls.push_back(1);
        victim.Unsubscribe();
    });

    victim = bus.Subscribe<TestEvent>([&](const TestEvent&) {
        calls.push_back(2);
    });

    auto third = bus.Subscribe<TestEvent>([&](const TestEvent&) {
        calls.push_back(3);
    });

    bus.Publish(TestEvent{});
    bus.Publish(TestEvent{});

    // Erwartet: [1, 3, 1, 3] – victim (B) feuert nie, da sofort deregistriert.
    CHECK_EQ(ctx, calls.size(), size_t{4});
    CHECK_EQ(ctx, calls[0], 1);
    CHECK_EQ(ctx, calls[1], 3);
    CHECK_EQ(ctx, calls[2], 1);
    CHECK_EQ(ctx, calls[3], 3);
    CHECK(ctx, std::find(calls.begin(), calls.end(), 2) == calls.end());

    (void)first; (void)third;
}

void TestSubscribeDuringDispatchDeferredToNextPublish(test::TestContext& ctx)
{
    EventBus bus;
    std::vector<int> calls;
    bool added = false;
    std::vector<Subscription> lateSubs;

    auto first = bus.Subscribe<TestEvent>([&](const TestEvent&) {
        calls.push_back(1);
        if (!added)
        {
            added = true;
            lateSubs.push_back(bus.Subscribe<TestEvent>([&](const TestEvent&) {
                calls.push_back(3);
            }));
        }
    });

    auto second = bus.Subscribe<TestEvent>([&](const TestEvent&) {
        calls.push_back(2);
    });

    bus.Publish(TestEvent{});
    bus.Publish(TestEvent{});

    CHECK_EQ(ctx, calls.size(), size_t{5});
    CHECK_EQ(ctx, calls[0], 1);
    CHECK_EQ(ctx, calls[1], 2);
    CHECK_EQ(ctx, calls[2], 1);
    CHECK_EQ(ctx, calls[3], 2);
    CHECK_EQ(ctx, calls[4], 3);

    (void)first; (void)second;
}

void TestNestedPublishSameType(test::TestContext& ctx)
{
    EventBus bus;
    std::vector<int> values;
    int depth = 0;

    auto first = bus.Subscribe<TestEvent>([&](const TestEvent& ev) {
        values.push_back(ev.value * 10 + 1);
        if (depth == 0)
        {
            ++depth;
            bus.Publish(TestEvent{ ev.value + 1 });
            --depth;
        }
    });

    auto second = bus.Subscribe<TestEvent>([&](const TestEvent& ev) {
        values.push_back(ev.value * 10 + 2);
    });

    bus.Publish(TestEvent{ 1 });

    CHECK_EQ(ctx, values.size(), size_t{4});
    CHECK_EQ(ctx, values[0], 11);
    CHECK_EQ(ctx, values[1], 21);
    CHECK_EQ(ctx, values[2], 22);
    CHECK_EQ(ctx, values[3], 12);

    (void)first; (void)second;
}

} // namespace

int RunEventTests()
{
    test::TestSuite suite("Events");
    suite
        .Add("publish order stable", TestPublishOrderStable)
        .Add("unsubscribe during dispatch", TestUnsubscribeDuringDispatch)
        .Add("subscribe during dispatch deferred", TestSubscribeDuringDispatchDeferredToNextPublish)
        .Add("nested publish same type", TestNestedPublishSameType);
    return suite.Run();
}
