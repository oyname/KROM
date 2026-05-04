#pragma once

#include "core/ServiceRegistry.hpp"

namespace engine {

class ILogger;

namespace events {
class EventBus;
}

struct AddonContext
{
    ILogger& Logger;
    events::EventBus& EventBus;
    ServiceRegistry& Services;
    void* Config = nullptr;
    void* Allocator = nullptr;
};

} // namespace engine
