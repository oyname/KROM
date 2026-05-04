// =============================================================================
// KROM Engine - examples/addon_bootstrap/main.cpp
//
// Minimalbeispiel: Zeigt den IEngineAddon-Vertrag ohne Render-Loop.
//
// Reihenfolge:
//   1. ServiceRegistry befüllen (welche Core-Services stehen bereit)
//   2. AddonManager befüllen  (welche Add-ons sollen laufen)
//   3. AddonContext erzeugen
//   4. RegisterAll()          (vorwärts, in Einfüge-Reihenfolge)
//   5. Nutzungsphase
//   6. UnregisterAll()        (rückwärts)
//
// Dieses Beispiel braucht keinen Grafik-Backend und kein Fenster.
// Um Render-Add-ons (MeshRenderer, Lighting, Shadow, Forward) zu aktivieren,
// muss zusätzlich ein renderer::RenderSystem als Service registriert werden –
// see ExampleApp.cpp für das vollständige Setup.
// =============================================================================

#include "addons/camera/CameraComponents.hpp"
#include "addons/runtime/EngineAddonAdapters.hpp"
#include "core/AddonContext.hpp"
#include "core/AddonManager.hpp"
#include "core/IEngineAddon.hpp"
#include "core/Logger.hpp"
#include "core/ServiceRegistry.hpp"
#include "ecs/ComponentMeta.hpp"
#include "ecs/Components.hpp"
#include "events/EventBus.hpp"

#include <cstdio>
#include <memory>

using namespace engine;

// -----------------------------------------------------------------------------
// Beispiel-Add-on: zeigt, wie ein eigenes IEngineAddon aussieht.
// Keine Renderer-Abhängigkeit – nutzt nur Logger und ServiceRegistry.
// -----------------------------------------------------------------------------
class DiagnosticsAddon final : public IEngineAddon
{
public:
    [[nodiscard]] const char*      Name()    const noexcept override { return "Diagnostics"; }
    [[nodiscard]] std::string_view Version() const noexcept override { return "1.0.0"; }

    bool Register(AddonContext& ctx) override
    {
        ctx.Logger.Info("[DiagnosticsAddon] Registrierung gestartet");
        ctx.Logger.Info(ctx.Services.Has<ecs::ComponentMetaRegistry>()
            ? "[DiagnosticsAddon] ComponentMetaRegistry verfuegbar: ja"
            : "[DiagnosticsAddon] ComponentMetaRegistry verfuegbar: nein");
        return true;
    }

    void Unregister(AddonContext& ctx) override
    {
        ctx.Logger.Info("[DiagnosticsAddon] Abgemeldet");
        (void)ctx;
    }
};

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main()
{
    std::fprintf(stdout, "=== KROM Addon Bootstrap ===\n\n");

    // -------------------------------------------------------------------------
    // 1. Core-Services
    // -------------------------------------------------------------------------
    ecs::ComponentMetaRegistry componentRegistry;
    RegisterCoreComponents(componentRegistry);    // Transform, Parent, Children, Name, …

    events::EventBus eventBus;

    ServiceRegistry services;
    services.Register<ecs::ComponentMetaRegistry>(&componentRegistry);
    // Für Render-Add-ons hier zusätzlich eintragen:
    //   services.Register<renderer::RenderSystem>(&renderSystem);

    // -------------------------------------------------------------------------
    // 2. Add-ons eintragen
    //    Reihenfolge = Registrierungs-Reihenfolge (Unregister läuft umgekehrt)
    // -------------------------------------------------------------------------
    AddonManager manager;

    // Eigenes Add-on
    if (!manager.AddAddon(std::make_unique<DiagnosticsAddon>()))
        return 1;

    // Camera-Add-on: benötigt nur ComponentMetaRegistry, kein RenderSystem
    if (!manager.AddAddon(CreateCameraAddon()))
        return 1;

    // Render-Add-ons: auskommentiert, da kein RenderSystem registriert ist.
    // manager.AddAddon(CreateMeshRendererAddon());
    // manager.AddAddon(CreateLightingAddon());
    // manager.AddAddon(CreateShadowAddon());
    // manager.AddAddon(CreateForwardAddon({ 0.1f, 0.1f, 0.1f, 1.f }));

    // -------------------------------------------------------------------------
    // 3. Kontext + RegisterAll
    // -------------------------------------------------------------------------
    AddonContext ctx{ GetDefaultLogger(), eventBus, services };

    std::fprintf(stdout, "Registrierung laeuft...\n");
    if (!manager.RegisterAll(ctx))
    {
        std::fprintf(stderr, "FEHLER: Add-on-Registrierung fehlgeschlagen\n");
        return 2;
    }
    std::fprintf(stdout, "Alle Add-ons erfolgreich registriert.\n\n");

    // -------------------------------------------------------------------------
    // 4. Nutzungsphase (hier: nur Komponentenpruefung als Demo)
    // -------------------------------------------------------------------------
    // CameraAddon hat CameraComponent registriert - Metadaten prüfen
    const ecs::ComponentMeta* cameraMeta = componentRegistry.Get<CameraComponent>();
    if (cameraMeta)
        std::fprintf(stdout, "CameraComponent registriert: '%s' (typeId=%u)\n\n",
            cameraMeta->name, cameraMeta->typeId);

    // -------------------------------------------------------------------------
    // 5. UnregisterAll (rückwärts)
    // -------------------------------------------------------------------------
    std::fprintf(stdout, "Abmeldung laeuft...\n");
    manager.UnregisterAll(ctx);
    std::fprintf(stdout, "Sauberes Shutdown abgeschlossen.\n");

    return 0;
}
