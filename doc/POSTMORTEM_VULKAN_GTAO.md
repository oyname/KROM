# Post Mortem: Vulkan GTAO

Datum: 2026-05-15

## Kurzfassung

GTAO war im Vulkan-Backend nicht korrekt nutzbar, weil Depth-Stencil-Textures beim Sampling mit einem ImageView gebunden wurden, der weiterhin Depth und Stencil als Aspect-Maske enthielt. Der GTAO-Linearize-Pass liest die Scene-Depth als `Texture2D<float>`. Dafuer muss Vulkan einen depth-only Sample-View verwenden. Der Attachment-View darf Depth+Stencil behalten, aber der Sample-View nicht.

Der Fix erzeugt fuer Depth-Texturen nun einen separaten `sampleView` mit `VK_IMAGE_ASPECT_DEPTH_BIT`. Danach liefert der GTAO-Buffer plausibles AO um die Kugel. Der sichtbare Buffer zeigt erwartungsgemaess hellere freie Flaechen und dunklere Kontakt-/Randbereiche.

## Symptome

- GTAO funktionierte im Vulkan-Pfad nicht sichtbar oder nicht korrekt.
- Der GTAO-Debugbuffer zeigte vorher kein vertrauenswuerdiges Ergebnis.
- Nach dem Fix ist um die Kugel ein AO-Verlauf sichtbar.
- Zusaetzlich gab es massiven Log-Spam:
  `ShadowTexture(vulkan transition): ...`

## Ursache

In `VulkanDevice::CreateImage` wurde fuer Depth-Texturen zwar ein `sampleView` angelegt, aber mit derselben `subresourceRange.aspectMask` wie der normale Texture-View. Bei `D24S8` ist diese Maske `VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT`.

Das ist fuer Depth-Stencil-Attachments korrekt, aber fuer Shader-Sampling als `Texture2D<float>` falsch. Der Shader will nur die Depth-Komponente lesen. Vulkan erwartet dafuer einen ImageView, der nur die Depth-Aspect beschreibt.

## Fix

Datei: `addons/vulkan/VulkanDevice.cpp`

Der Sample-View fuer Depth-Texturen wird jetzt mit depth-only Aspect erstellt:

```cpp
outEntry.sampleView = outEntry.view;
if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u)
{
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vkCreateImageView(m_device, &vci, nullptr, &outEntry.sampleView) != VK_SUCCESS)
    {
        ...
    }
}
```

Damit bleibt der normale View fuer Attachment-Nutzung unveraendert, waehrend Descriptor-Sampling ueber `sampleView` den korrekten Vulkan-View bekommt.

## Nebenfund: Log-Spam

Datei: `addons/vulkan/VulkanCommandList.cpp`

Der wiederholte `ShadowTexture(vulkan transition)`-Log war kein Fehler, sondern eine Diagnoseausgabe im Descriptor-Flush. Sie wurde bei jeder Depth-Texture-Transition auf normalem Info-Level ausgegeben. Das wurde auf `Debug::LogVerbose` umgestellt und allgemeiner benannt:

```cpp
Debug::LogVerbose("VulkanCommandList: depth texture transition ...");
```

## Verifikation

- `krom_pbr_shadow_vulkan` wurde nach dem Depth-View-Fix gebaut und kurz gestartet.
- Der Lauf zeigte keine relevanten Vulkan-/GTAO-Fehler, nur bekannte Shutdown/IBL-Warnungen.
- `krom_tests` lief vollstaendig durch: `ALL TESTS PASSED`.
- Visuelle Kontrolle des GTAO-Buffers: AO um die Kugel ist sichtbar und grundsaetzlich plausibel.

Hinweis: Nach der spaeteren reinen Log-Level-Aenderung hing Ninja beim Relink reproduzierbar in diesem Workspace. `VulkanCommandList.cpp.obj` wurde aktualisiert, die finale Beispiel-EXE wurde in diesem Lauf aber nicht neu gelinkt. Die verwaiste `.ninja_lock` wurde entfernt.

## Aktueller Zustand

GTAO funktioniert im Vulkan-Pfad grundsaetzlich. Der Buffer ist noch roh und sichtbar verrauscht/gepunktet, besonders am Kontaktbereich unter der Kugel. Das passt zum aktuellen Shader-Setup mit wenigen Samples und per-pixel Noise.

Der aktuelle GTAO-Shader ist eher ein einfacher HBAO/GTAO-aehnlicher Prototyp:

- 4 Richtungen
- 4 Schritte
- kein Blur/Denoise
- keine temporale Stabilisierung
- feste Radius-/Bias-Werte

## Noch Zu Tun

1. GTAO-Qualitaet verbessern

- Denoise-/Blur-Pass fuer den AO-Buffer hinzufuegen.
- Sample-Anzahl konfigurierbar machen, z.B. `4x4`, `6x6`, `8x4`.
- Radius, Bias und Intensitaet als Parameter auslagern.
- Debug-View fuer raw AO, blurred AO und final composite beibehalten.

2. Vulkan-Resource-State-Tracking pruefen

- Depth-Read-Abhaengigkeit des GTAO-Linearize-Passes sauber im RenderGraph modellieren.
- Aktuell holt GTAO die Depth-Texture manuell aus dem HDR-RenderTarget.
- Ziel: weniger Sonderlogik im Executor und klarere Transitions ueber den Graph.

3. Log-Hygiene abschliessen

- Nach erfolgreichem Relink pruefen, dass `ShadowTexture(vulkan transition)` nicht mehr im normalen Output erscheint.
- Vulkan-Diagnoseausgaben nach `Verbose`/`Warning`/`Error` sortieren.

4. Build-Hang untersuchen

- Ninja hing nach mehreren Timeout-Laeufen, obwohl einzelne Artefakte teilweise aktualisiert wurden.
- Ursachen pruefen: verwaiste `.ninja_lock`, blockierende Linker-Prozesse, Antivirus/File-Locks oder Toolchain-Umgebung.
- Optional ein kleines Build-Script fuer den VSDevCmd/Ninja-Pfad anlegen, damit diese Befehle deterministischer laufen.

5. Tests ergaenzen

- RenderGraph-Test fuer GTAO-Pass-Reihenfolge und Ressourcenabhaengigkeiten.
- Vulkan-spezifischer Test oder Debug-Assert fuer Depth-Sampling-Views:
  Depth-Stencil-Format darf beim Sample-View nur `VK_IMAGE_ASPECT_DEPTH_BIT` verwenden.
- Optional Screenshot-/bufferbasierter Smoke-Test fuer den GTAO-Debugbuffer.

6. Langfristig

- GTAO in eine echte Render-Feature-Konfiguration aufnehmen.
- Presets fuer Performance/Quality einfuehren.
- Falls TAA/History verfuegbar wird: temporale AO-Stabilisierung einplanen.
- Composite-Staerke material-/lightingfreundlicher machen, statt HDR pauschal mit AO zu multiplizieren.
