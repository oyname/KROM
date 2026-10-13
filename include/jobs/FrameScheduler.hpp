#pragma once
// =============================================================================
// KROM Engine - jobs/FrameScheduler.hpp
// Geordnete Frame-Pipeline mit Schreibkonflikt-Erkennung.
//
// Problem das gelöst wird:
//   Physik und Animation laufen parallel, beide schreiben TransformComponent
//   → Datenrennen, undefiniertes Verhalten, sporadische Bugs.
//
// Lösung:
//   Jede Stage deklariert was sie liest/schreibt.
//   Build() erkennt parallele Stages mit überlappenden Schreib-Tags und loggt
//   den Konflikt mit einem konkreten Hinweis welche Abhängigkeit fehlt.
//
// Typische Spielpipeline:
//   auto input  = scheduler.RegisterStage("Input",     {},       inputFn,  {},            {"Input"});
//   auto phys   = scheduler.RegisterStage("Physics",   {input},  physFn,   {"Transform", "RigidBody"});
//   auto anim   = scheduler.RegisterStage("Animation", {phys},   animFn,   {"Transform"}, {"Skeleton"});
//   auto render = scheduler.RegisterStage("Rendering", {anim},   renderFn, {},            {"Transform"});
//   scheduler.Build();   // → Fehler wenn zwei parallele Stages dasselbe schreiben
//   scheduler.Execute(jobSystem);
// =============================================================================
#include "jobs/JobSystem.hpp"
#include "jobs/TaskGraph.hpp"
#include "core/Debug.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::jobs {

using StageHandle = uint32_t;
static constexpr StageHandle INVALID_STAGE = UINT32_MAX;

class FrameScheduler
{
public:
    // Registriert eine Stage.
    //   deps      — Stages die vor dieser fertig sein müssen.
    //   fn        — Arbeit der Stage. Kann selbst parallel via JobSystem arbeiten,
    //               darf aber nicht blockierend auf denselben Pool warten.
    //   writeTags — Ressourcen/Komponenten die exklusiv geschrieben werden.
    //   readTags  — Ressourcen/Komponenten die nur gelesen werden.
    //
    // Empfohlene Tags: Komponentenname als Konstante, z.B. kTagTransform = "Transform".
    StageHandle RegisterStage(
        std::string                  name,
        std::vector<StageHandle>     deps,
        std::function<TaskResult()>  fn,
        std::vector<std::string>     writeTags = {},
        std::vector<std::string>     readTags  = {})
    {
        const auto h = static_cast<StageHandle>(m_stages.size());

        std::vector<TaskHandle> graphDeps;
        graphDeps.reserve(deps.size());
        for (StageHandle d : deps)
            graphDeps.push_back(static_cast<TaskHandle>(d));

        m_graph.Add(name, std::move(graphDeps), fn);

        m_stages.push_back({ std::move(name), std::move(writeTags), std::move(readTags) });
        m_built         = false;
        m_conflictCount = 0u;
        return h;
    }

    // Baut den TaskGraph und prüft auf parallele Zugriffskonflikte.
    // Gibt false zurück bei Zyklen. Konflikte erzeugen LogError-Meldungen
    // und werden über ConflictCount() abfragbar — der Build schlägt
    // deswegen nicht fehl, damit das Spiel auch mit Konflikten starten kann.
    [[nodiscard]] bool Build()
    {
        m_conflictCount = 0u;
        if (!m_graph.Build())
            return false;

        ValidateAccessConflicts();
        m_built = true;

        if (m_conflictCount > 0u)
            Debug::LogError(
                "FrameScheduler: %u Konflikte gefunden — "
                "Physik/Animation/Rendering könnten inkorrekte Ergebnisse liefern!",
                m_conflictCount);
        return true;
    }

    // Führt alle Stages in der deklarierten Reihenfolge aus.
    [[nodiscard]] TaskResult Execute(JobSystem& js)
    {
        if (!m_built)
        {
            Debug::LogError("FrameScheduler: Execute - Build() wurde nicht aufgerufen");
            return TaskResult::Fail("FrameScheduler not built");
        }
        return m_graph.Execute(js);
    }

    void Clear()
    {
        m_graph.Clear();
        m_stages.clear();
        m_built         = false;
        m_conflictCount = 0u;
    }

    [[nodiscard]] size_t   StageCount()     const noexcept { return m_stages.size(); }
    [[nodiscard]] bool     IsBuilt()        const noexcept { return m_built; }

    // Anzahl der bei Build() erkannten Write/Read-Konflikte.
    // 0 = alles in Ordnung; >0 = Abhängigkeit fehlt.
    [[nodiscard]] uint32_t ConflictCount()  const noexcept { return m_conflictCount; }

private:
    struct StageInfo
    {
        std::string              name;
        std::vector<std::string> writeTags;
        std::vector<std::string> readTags;
    };

    void ValidateAccessConflicts()
    {
        const size_t n = m_stages.size();

        // Stages nach TaskGraph-Level gruppieren (gleiche Ebene = läuft parallel)
        std::unordered_map<uint32_t, std::vector<size_t>> byLevel;
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t lvl = m_graph.GetTask(static_cast<TaskHandle>(i)).level;
            byLevel[lvl].push_back(i);
        }

        for (auto& [level, indices] : byLevel)
        {
            if (indices.size() < 2u) continue;

            for (size_t a = 0u; a < indices.size(); ++a)
            for (size_t b = a + 1u; b < indices.size(); ++b)
            {
                const StageInfo& sa = m_stages[indices[a]];
                const StageInfo& sb = m_stages[indices[b]];

                // Write ↔ Write: beide schreiben dieselbe Ressource
                for (const auto& ta : sa.writeTags)
                for (const auto& tb : sb.writeTags)
                {
                    if (ta != tb) continue;
                    ++m_conflictCount;
                    Debug::LogError(
                        "FrameScheduler [Level %u] Write-Write-Konflikt auf '%s': "
                        "'%s' und '%s' schreiben beide, laufen aber parallel! "
                        "→ Abhängigkeit '%s' nach '%s' (oder umgekehrt) eintragen.",
                        level, ta.c_str(),
                        sa.name.c_str(), sb.name.c_str(),
                        sa.name.c_str(), sb.name.c_str());
                }

                // Write ↔ Read: eine Stage schreibt, die andere liest gleichzeitig
                for (const auto& wTag : sa.writeTags)
                for (const auto& rTag : sb.readTags)
                {
                    if (wTag != rTag) continue;
                    ++m_conflictCount;
                    Debug::LogError(
                        "FrameScheduler [Level %u] Write-Read-Konflikt auf '%s': "
                        "'%s' schreibt, '%s' liest gleichzeitig! "
                        "→ Abhängigkeit '%s' nach '%s' eintragen.",
                        level, wTag.c_str(),
                        sa.name.c_str(), sb.name.c_str(),
                        sa.name.c_str(), sb.name.c_str());
                }

                for (const auto& wTag : sb.writeTags)
                for (const auto& rTag : sa.readTags)
                {
                    if (wTag != rTag) continue;
                    ++m_conflictCount;
                    Debug::LogError(
                        "FrameScheduler [Level %u] Write-Read-Konflikt auf '%s': "
                        "'%s' schreibt, '%s' liest gleichzeitig! "
                        "→ Abhängigkeit '%s' nach '%s' eintragen.",
                        level, wTag.c_str(),
                        sb.name.c_str(), sa.name.c_str(),
                        sb.name.c_str(), sa.name.c_str());
                }
            }
        }
    }

    std::vector<StageInfo> m_stages;
    TaskGraph              m_graph;
    bool                   m_built         = false;
    uint32_t               m_conflictCount = 0u;
};

// Empfohlene Standard-Tags für Spielsysteme.
// Eigene Tags können als lokale constexpr-Strings definiert werden.
namespace FrameTags {
    inline constexpr const char* Transform  = "Transform";
    inline constexpr const char* RigidBody  = "RigidBody";
    inline constexpr const char* Skeleton   = "Skeleton";
    inline constexpr const char* Animation  = "Animation";
    inline constexpr const char* Camera     = "Camera";
    inline constexpr const char* Light      = "Light";
    inline constexpr const char* Particle   = "Particle";
    inline constexpr const char* AI         = "AI";
    inline constexpr const char* Audio      = "Audio";
} // namespace FrameTags

} // namespace engine::jobs
