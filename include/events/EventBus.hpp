#pragma once
// =============================================================================
// KROM Engine - events/EventBus.hpp
// Typsicheres Event-System mit loser Kopplung zwischen Modulen.
// Primär synchroner EventBus ohne globalen State.
//
// Laufzeit-Semantik:
// - Publish dispatcht synchron im aufrufenden Thread.
// - Subscribe während eines laufenden Dispatchs wird erst ab zukünftigen Publishes sichtbar.
// - Unsubscribe während eines laufenden Dispatchs deaktiviert den Handler sofort für spätere
//   Aufrufe; bereits erzeugte Snapshots überspringen deaktivierte Einträge.
// - Verschachtelte Publish-Aufrufe (auch desselben Event-Typs) sind erlaubt. Jeder Publish
//   arbeitet auf einem eigenen Snapshot der zum Start aktiven Subscriber.
// - Keine Exceptions als Kontrollfluss. Keine globalen Singletons.
// =============================================================================
#include "core/Types.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::events {

class ISubscriptionTarget
{
public:
    virtual ~ISubscriptionTarget() = default;
    virtual void RemoveSubscription(uint64_t id) = 0;
};

// Subscription-Handle - bei Zerstörung wird automatisch unsubscribed.
// Sicher auch dann, wenn der EventBus bereits zerstört wurde.
class Subscription
{
public:
    Subscription() = default;
    Subscription(Subscription&& other) noexcept
        : m_target(std::move(other.m_target))
        , m_id(std::exchange(other.m_id, 0u))
    {
    }

    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this == &other)
            return *this;

        Unsubscribe();
        m_target = std::move(other.m_target);
        m_id = std::exchange(other.m_id, 0u);
        return *this;
    }

    ~Subscription() { Unsubscribe(); }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void Unsubscribe()
    {
        if (m_id == 0u)
            return;

        if (std::shared_ptr<ISubscriptionTarget> target = m_target.lock())
            target->RemoveSubscription(m_id);

        m_target.reset();
        m_id = 0u;
    }

private:
    friend class EventBus;

    Subscription(std::weak_ptr<ISubscriptionTarget> target, uint64_t id)
        : m_target(std::move(target))
        , m_id(id)
    {
    }

    std::weak_ptr<ISubscriptionTarget> m_target;
    uint64_t m_id = 0u;
};

// =============================================================================
// EventBus
// =============================================================================
class EventBus
{
public:
    EventBus() = default;
    ~EventBus() = default;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template<typename EventType>
    [[nodiscard]] Subscription Subscribe(std::function<void(const EventType&)> handler)
    {
        std::shared_ptr<TypedChannel<EventType>> channel;
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            const std::type_index key = typeid(EventType);
            auto& channelPtr = m_channels[key];
            if (!channelPtr)
                channelPtr = std::make_shared<TypedChannel<EventType>>();

            channel = std::static_pointer_cast<TypedChannel<EventType>>(channelPtr);
        }

        const uint64_t id = channel->Add(std::move(handler));
        return Subscription{ channel, id };
    }

    template<typename EventType>
    void Publish(const EventType& event) const
    {
        std::shared_ptr<const TypedChannel<EventType>> channel;
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            const std::type_index key = typeid(EventType);
            auto it = m_channels.find(key);
            if (it == m_channels.end())
                return;

            channel = std::static_pointer_cast<const TypedChannel<EventType>>(it->second);
        }

        channel->Dispatch(event);
    }

    template<typename EventType, typename... Args>
    void Emit(Args&&... args) const
    {
        Publish(EventType{ std::forward<Args>(args)... });
    }

    [[nodiscard]] size_t ChannelCount() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        return m_channels.size();
    }

private:
    struct IChannel : ISubscriptionTarget
    {
        ~IChannel() override = default;
    };

    template<typename EventType>
    struct TypedChannel final : IChannel, std::enable_shared_from_this<TypedChannel<EventType>>
    {
        struct Entry
        {
            uint64_t id = 0u;
            std::function<void(const EventType&)> handler;
            std::atomic<bool> active{ true };
        };

        uint64_t Add(std::function<void(const EventType&)> fn)
        {
            auto entry = std::make_shared<Entry>();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                entry->id = m_nextId++;
                entry->handler = std::move(fn);
                m_entries.push_back(entry);
            }
            return entry->id;
        }

        void RemoveSubscription(uint64_t id) override
        {
            Remove(id);
        }

        void Remove(uint64_t id)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const std::shared_ptr<Entry>& entry : m_entries)
            {
                if (entry->id == id)
                {
                    entry->active.store(false, std::memory_order_release);
                    break;
                }
            }

            if (m_dispatchDepth == 0u)
                CompactInactiveLocked();
        }

        void Dispatch(const EventType& event) const
        {
            std::vector<std::shared_ptr<Entry>> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_dispatchDepth;
                snapshot.reserve(m_entries.size());
                for (const std::shared_ptr<Entry>& entry : m_entries)
                {
                    if (entry->active.load(std::memory_order_acquire))
                        snapshot.push_back(entry);
                }
            }

            for (const std::shared_ptr<Entry>& entry : snapshot)
            {
                if (entry->active.load(std::memory_order_acquire) && entry->handler)
                    entry->handler(event);
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_dispatchDepth > 0u)
                    --m_dispatchDepth;

                if (m_dispatchDepth == 0u)
                    CompactInactiveLocked();
            }
        }

    private:
        void CompactInactiveLocked() const
        {
            size_t writeIndex = 0u;
            for (size_t readIndex = 0u; readIndex < m_entries.size(); ++readIndex)
            {
                if (m_entries[readIndex]->active.load(std::memory_order_acquire))
                {
                    if (writeIndex != readIndex)
                        m_entries[writeIndex] = std::move(m_entries[readIndex]);
                    ++writeIndex;
                }
            }
            m_entries.resize(writeIndex);
        }

        mutable std::mutex m_mutex;
        mutable uint32_t m_dispatchDepth = 0u;
        uint64_t m_nextId = 1u;
        mutable std::vector<std::shared_ptr<Entry>> m_entries;
    };

    mutable std::mutex m_channelsMutex;
    mutable std::unordered_map<std::type_index, std::shared_ptr<IChannel>> m_channels;
};

// =============================================================================
// Standard-Engine-Events
// =============================================================================

struct EntityCreatedEvent    { EntityID entity; };
struct EntityDestroyedEvent  { EntityID entity; };
struct ComponentAddedEvent   { EntityID entity; uint32_t typeId; };
struct ComponentRemovedEvent { EntityID entity; uint32_t typeId; };
struct SceneLoadedEvent      { std::string sceneName; };
struct SceneUnloadedEvent    { std::string sceneName; };
struct WindowResizedEvent    { uint32_t width; uint32_t height; };
struct FrameBeginEvent       { float deltaTime; uint64_t frameIndex; };
struct FrameEndEvent         { float deltaTime; uint64_t frameIndex; };

} // namespace engine::events
