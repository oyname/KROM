#pragma once
// =============================================================================
// KROM Engine - jobs/ComponentWriteGuard.hpp
// Laufzeit-Schutz gegen gleichzeitige Schreiber auf denselben Komponententyp.
//
// Problem das gelöst wird:
//   Physik und Animation laufen versehentlich parallel und schreiben beide
//   TransformComponent — der FrameScheduler hat die Abhängigkeit nicht.
//   ComponentWriteGuard erkennt das sofort zur Laufzeit.
//
// Verwendung:
//   void PhysicsJob() {
//       ComponentWriteGuard<TransformComponent> guard;  // RAII, am Job-Anfang
//       for (auto& transform : ...) { transform.position += ...; }
//   }
//
// Zwei Jobs mit demselben Guard-Typ laufen gleichzeitig → LogError mit
// dem Hinweis welche Abhängigkeit im FrameScheduler fehlt.
//
// Release-Builds (NDEBUG): vollständig wegoptimiert, kein Overhead.
// =============================================================================
#include "core/Debug.hpp"
#include <atomic>
#include <typeinfo>

namespace engine::jobs {

template<typename T>
class ComponentWriteGuard
{
public:
#if defined(NDEBUG)
    // Release: leere Structs — der Compiler optimiert alles weg
    ComponentWriteGuard()  = default;
    ~ComponentWriteGuard() = default;

#else
    ComponentWriteGuard()
    {
        const int prev = s_writers.fetch_add(1, std::memory_order_acq_rel);
        if (prev != 0)
            Debug::LogError(
                "ComponentWriteGuard: Datenrennen auf '%s' — "
                "%d gleichzeitige Schreiber erkannt! "
                "Fehlende Abhängigkeit im FrameScheduler: "
                "der schreibende Stage muss vor dem anderen abgeschlossen sein.",
                typeid(T).name(), prev + 1);
    }

    ~ComponentWriteGuard()
    {
        s_writers.fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    // Prozess-globaler Zähler pro Komponententyp T.
    // inline: jede TU sieht dieselbe Instanz (C++17).
    static inline std::atomic<int> s_writers{ 0 };

#endif // NDEBUG

public:
    ComponentWriteGuard(const ComponentWriteGuard&)            = delete;
    ComponentWriteGuard& operator=(const ComponentWriteGuard&) = delete;
    ComponentWriteGuard(ComponentWriteGuard&&)                 = delete;
    ComponentWriteGuard& operator=(ComponentWriteGuard&&)      = delete;
};

} // namespace engine::jobs
