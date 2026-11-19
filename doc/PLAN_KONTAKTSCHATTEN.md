# Plan: Kontaktschatten (Screen-Space Contact Shadows)

Stand: 2026-11-17, auf `main` nach `5adc3c0`. Jede Sitzung liest diese Datei zuerst und
trägt am Ende ein, was erledigt ist, was offen bleibt und was vom Plan abweicht.

## Ziel

Kurze, scharfe Schatten des **direkten Lichts** dort, wo die Shadow Map zu grob ist:
unter Füßen und Objekten auf dem Boden, an Kanten, bei kleinen Details. Abgrenzung:

- **GTAO** dunkelt nur das Umgebungslicht ab, nicht das direkte Licht. Es bleibt
  unverändert und ergänzt die Kontaktschatten.
- **Shadow Maps (CSM/Atlas)** bleiben die Hauptschatten. Kontaktschatten multiplizieren
  sich nur auf deren Sichtbarkeit (`shadowVisibility`).
- **Kein GI.** Räume werden dadurch nicht dunkler (siehe Gespräch vom 2026-11-17).

## Rahmen

- Backends: **DX11 und Vulkan**. OpenGL ist eingefroren: Dort bleibt das Feature aus,
  OpenGL muss aber weiter bauen und starten.
- Licht: zuerst nur **ein Hauptlicht**, das erste gerichtete Licht mit Schatten und
  eingeschaltetem Kontaktschatten. Punkt- und Spotlichter sind Ausbaustufe (Schritt 7).
- Verfahren: ein eigener Pass nach dem Tiefen-Vorpass schreibt eine Sichtbarkeits-Textur.
  Die PBR-Shader lesen sie im Opaque-Pass, so wie heute `GTAOOutput`.
- Eigenes Addon `addons/contact_shadow`, aufgebaut wie `addons/gtao` und `addons/ssr`.
- Arbeitszweig: `feature/contact-shadows` von `main`. Die fremden, nicht committeten
  Änderungen im Arbeitsordner (DemoProject-Szenen, `assets/weisse_textur.png`,
  `editor/logo.png`) bleiben unangetastet.

Vorher lesen: `doc/POSTMORTEM_GTAO_DEPTH_PREPASS.md`, `doc/POSTMORTEM_VULKAN_GTAO.md`,
`doc/POSTMORTEM_VULKAN_BINDING.md`, `doc/POSTMORTEM_SHADOW_BANDING.md`.

## Befunde im aktuellen Code

| Thema | Stand | Folge für den Plan |
|---|---|---|
| Tiefen-Vorpass | `ForwardFeature.cpp:1607`: `depthPrepassEnabled` nur, wenn ein Contributor `GTAOOutput` anlegt. Editor/Runtime setzen `view.enableDepthPrepass` aus `gtaoResources->settings.enabled` (`KromEditorApp.cpp` ~3243/3326, `KromWrapper.cpp` ~4787). | Vorpass ist an GTAO gekoppelt. Muss sich lösen, sonst gibt es ohne GTAO keine Kontaktschatten (Schritt 1). |
| Lesen im Opaque-Pass | `StandardFramePipeline.hpp:556` hängt fest `GTAOOutput` als `ReadTexture` an; `ForwardFeature.cpp:1118` bindet ihn auf `TexSlots::PassSRV0` (= `t8`). | Für eine zweite Textur aus `AfterDepthPrepass` braucht das Forward-Addon einen allgemeinen Weg (Schritt 1). |
| Textur-Slots in PBR-Shadern | belegt: t0–t13, t16–t19 (Materialgraph t9/t13/t16/t17, Probes t18/t19, SSR-Pässe t10–t12 nur intern). Frei für den Opaque-Pass: **t14, t15**. | Kontaktschatten auf **t14**. In Schritt 1 gegen `TexSlots` und Vulkan-Bindings prüfen. |
| Frame-Parameterblöcke | belegt: 0 GTAO, 1 Probes, 2 Shading, 6 Shadow-Filter, 7 SSR (`kFrameParameterSlotCount` = 8). | Kontaktschatten auf **Slot 3** (1 Slot = 16 Byte). |
| Debug-Bits | `pbr_lit.ps.hlsl`/`pbr_forward_plus.ps.hlsl` bis `1u << 21` belegt. | `DBG_VIEW_CONTACT_SHADOW = 1u << 22`, `DBG_DISABLE_CONTACT_SHADOW = 1u << 23`. |
| Lichtschleife | `pbr_lit.ps.hlsl` ~700–840 und `pbr_forward_plus.ps.hlsl` gleich aufgebaut; `atten *= shadowVisibility`. | Kontaktschatten an dieser Stelle einrechnen, nur für das Hauptlicht. |
| Licht-Komponente | `addons/lighting/LightingComponents.hpp`: `LightComponent` mit `ShadowSettings shadowSettings`. Inspector: `EditorUIInspector.cpp` ~1790–1850. | Neues Feld `shadowSettings.contactShadows` (bool), Checkbox im Inspector. |
| Projekt-Einstellungen | `ProjectRenderSettings` + Leser/Schreiber in `include/core/ProjectSettings.hpp`; Weitergabe wie GTAO/SSR in `KromEditorApp.cpp` (`ApplyProjectRenderSettings`, Frame-Schleife) und `KromWrapper.cpp` (`ApplyProjectRenderSettings`). | Gleiches Muster wie die GTAO-Regler vom 2026-11-17. |

## Verfahren (Shader)

Pass `ContactShadowTrace`, Vollbild, volle Auflösung, Ausgabe `ContactShadowOutput` (R8_UNORM):

1. Tiefe des Pixels aus der Vorpass-Tiefe lesen (bei MSAA: Sample 0, wie GTAOs
   `psLinearizeDepthMS`), View-Position rekonstruieren. Himmel → 1.
2. Strahl im View-Space Richtung Licht (`-lightDirVS`), Länge `length` (Welteinheiten),
   `steps` Schritte (Standard 12). Startpunkt um einen Bruchteil eines Schritts mit
   Interleaved Gradient Noise versetzt (gegen Treppenstufen), dazu ein kleiner Versatz
   entlang der Normalen bzw. Richtung Licht gegen Selbstverschattung.
3. Jeder Schritt wird auf den Bildschirm projiziert, die Szenentiefe dort gelesen. Liegt
   die Szene vor dem Strahl und ist der Abstand kleiner als `thickness` → Treffer.
4. Ausblenden: am Bildrand, zum Ende des Strahls hin, mit der Entfernung zur Kamera
   (Kontaktschatten nur im Nahbereich, Standard bis 50 m).
5. Ausgabe `1 - intensity * occlusion`.

Danach ein tiefenbewusster Blur (2× separabel oder 4 Taps), wie GTAOs `psBlur`, gegen das
Rauschen aus Schritt 2. Ohne zeitliche Glättung (TAA gibt es nicht).

Im PBR-Shader, nur für das Hauptlicht:
`shadowVisibility *= contactShadow.Load(pixel).r`, wenn `contactEnabled` und das Debug-Bit
es nicht abschaltet.

## Parameter

Frame-Parameterblock `krom-contact-shadow`, Slot 3:

| Feld | Bedeutung |
|---|---|
| `lightDirVS.xyz` | Richtung zum Hauptlicht im View-Space |
| `w` (uint-Bits) | Index des Hauptlichts in `lights[]` + 1, 0 = aus |

Wenn `length`/`thickness`/`intensity`/`steps` nicht mehr in Slot 3 passen: zweiten Slot
(4) dazunehmen. Länge des Blocks dann 2. Steht bei Schritt 2 fest.

Projekt-Einstellungen (`rendering`-Block), Editor: Projekteinstellungen → Rendering,
eigener Abschnitt „Kontaktschatten“ mit eigener ImGui-ID (vgl. ID-Konflikt der GTAO-Regler):

| Schlüssel | Standard | Bereich |
|---|---|---|
| `contactShadowsEnabled` | true | – |
| `contactShadowLength` | 0.30 m | 0.02–2 m (log) |
| `contactShadowThickness` | 0.05 m | 0.005–0.5 m (log) |
| `contactShadowIntensity` | 1.0 | 0–1 |
| `contactShadowSteps` | 12 | 4–32 |

Pro Licht: `ShadowSettings::contactShadows` (Standard: true für gerichtete Lichter), in der
Szene gespeichert, Checkbox „Kontaktschatten“ im Inspector unter „Cast Shadows“ mit
Undo über `BeginPendingComponentEdit`/`CommitPendingComponentEdit`.

## Schritte

| Nr. | Schritt | Status |
|-----|---------|--------|
| 1 | Forward-Vorarbeit: Vorpass von GTAO lösen, allgemeine Lese-Texturen aus `AfterDepthPrepass` | erledigt (2026-11-18), Review offen |
| 2 | Addon-Gerüst, Parameterblock, Pass mit Trace + Blur | erledigt (2026-11-18), Review offen |
| 3 | PBR-Shader: Kontaktschatten im Hauptlicht (Forward und Forward+) | erledigt (2026-11-18), Review offen |
| 4 | Einstellungen: Projekt, Licht-Komponente, Inspector, Runtime | erledigt (2026-11-18), Review offen |
| 5 | Debug-Ansicht und GPU-Zeit | erledigt (2026-11-18) |
| 6 | Tests, Vulkan-Validierung, Sichtprüfung, Feinschliff gegen Artefakte | teilweise: Sichtprüfung Nutzer, MSAA 4×, Feinschliff offen |
| 7 | Ausbau (optional): Punkt- und Spotlichter | offen |

### 1. Forward-Vorarbeit

- `ForwardFeature.cpp`: `depthPrepassEnabled` gilt, sobald irgendein Contributor in
  `AfterDepthPrepass` eine Ressource anmeldet, die der Opaque-Pass lesen soll. Nicht mehr
  nur bei `GTAOOutput`.
- Neuer Mechanismus „Opaque-Eingaben“: Ein Contributor meldet `{Ressourcenname, Textur-Slot}`
  an. `AppendOpaquePass` hängt dafür `ReadTexture` an, die Ausführung bindet die Textur auf
  den Slot. Ist die Ressource in diesem Frame nicht vorhanden, wird ein 1×1-Weiß-Fallback
  gebunden, damit kein alter oder ungültiger Inhalt liegen bleibt (Lehre aus
  `POSTMORTEM_VULKAN_BINDING.md`).
- GTAO auf denselben Weg umstellen (`GTAOOutput` → t8). Danach kennt `StandardFramePipeline`
  GTAO nicht mehr beim Namen. Wenn das zu viel auf einmal ist, bleibt GTAO vorerst
  hartkodiert und nur die Kontaktschatten nutzen den neuen Weg; das wird hier vermerkt.
- Editor/Runtime: `view.enableDepthPrepass` = GTAO aktiv **oder** Kontaktschatten aktiv.
  Am besten fragt die View jedes Addon über eine kleine Funktion `WantsDepthPrepass()` ab,
  statt beide Bedingungen in `KromEditorApp.cpp` und `KromWrapper.cpp` zu wiederholen.
- Tests (`tests/test_renderer.cpp`): Rezept ohne GTAO, aber mit einer Opaque-Eingabe →
  Vorpass vorhanden, Opaque liest die Ressource. Rezept ohne beides → kein Vorpass
  (unverändertes Verhalten).
- Prüfen: Bild mit GTAO wie vorher (DX11, Vulkan), OpenGL startet.

### 2. Addon `addons/contact_shadow`

- Dateien: `CMakeLists.txt`, `ContactShadowFeature.{hpp,cpp}`, `ContactShadowPass.{hpp,cpp}`.
  Shader als String im Pass wie bei GTAO/SSR, `KROM_CONTACT_SHADOW_MSAA`-Variante für die
  Tiefe aus einem MS-Ziel.
- `ContactShadowSettings` (enabled, length, thickness, intensity, steps),
  `ContactShadowGpuResources`, `CreateContactShadowFeature(std::shared_ptr<...>*)`.
- Frame-Constants-Contributor: sucht das Hauptlicht aus dem Snapshot
  (`FrameConstantsContributionContext::snapshot`), schreibt Richtung und Index. **Zu
  prüfen:** ob der Index im Snapshot dem Index in `lights[]` des Shaders entspricht. Wenn
  nicht, liefert die Licht-Extraktion den Index mit (Shadow-Addon macht das über
  `shadowLightMeta.x`).
- Pässe in `PassPhase::AfterDepthPrepass`: Trace → `ContactShadowRaw`, Blur →
  `ContactShadowOutput`, als Opaque-Eingabe auf t14 angemeldet (Schritt 1).
- Auf OpenGL: Warnung, Pass bleibt No-op (wie GTAO).
- Registrierung in `KromEditorApp.cpp` (~1790, neben GTAO) und `KromWrapper.cpp` (~2705),
  Konfig-Schalter `enableContactShadows` wie `enableGtao`.

### 3. PBR-Shader

- `shaders/pbr_lit.ps.hlsl`, `shaders/pbr_forward_plus.ps.hlsl`:
  `Texture2D contactShadow : register(t14);`, Makros für Slot 3, Debug-Bits 22/23.
- In der Lichtschleife nach der Shadow-Map-Abfrage: für `i == contactLightIndex`
  `shadowVisibility *= contactShadowFactor`. Auch wirksam, wenn das Licht keine
  Shadow-Map-Ansicht im Bild hat (Kontaktschatten sind unabhängig davon gültig).
- `KROM_SHADOWS_NONE`-Variante: Kontaktschatten trotzdem anwenden? Entscheidung: nein,
  ohne Schatten auch keine Kontaktschatten (einfacher, erwartbarer).
- Materialgraph- und Custom-Shader: prüfen, ob sie dieselbe Lichtschleife einbinden. Wenn
  ja, gilt die Änderung automatisch, sonst hier als offen vermerken.
- Generierte Shader unter `shaders/generated/` neu erzeugen, falls die Quelle das verlangt
  (`doc/KONZEPT_SHADER_EINE_QUELLE.md`).

### 4. Einstellungen

- `ProjectRenderSettings`: die fünf Werte, Lesen mit Begrenzung, Schreiben.
- `ShadowSettings::contactShadows` + Szenen-Serialisierung (fehlender Schlüssel → Standard).
- Editor: Abschnitt in den Projekteinstellungen (de/en-Texte mit Tooltips), Checkbox im
  Inspector, `EditorState`-Werte und Weitergabe in der Frame-Schleife.
- Runtime: `KromWrapper.cpp` `ApplyProjectRenderSettings`.
- Script-API (`IRenderSettingsService`, `krom.h`): vorerst nicht, als offen vermerken.

### 5. Debug und Messung

- Viewport-Debug-Ansicht „Kontaktschatten“ (Bit 22) neben der GTAO-Ansicht, Schalter zum
  Abschalten (Bit 23) im selben Menü.
- Pass-Namen im GPU-Profiler (`ContactShadowTrace`, `ContactShadowBlur`). Ziel: unter
  0,5 ms bei 1080p auf der Entwickler-GPU, sonst `steps`-Standard senken oder halbe
  Auflösung erwägen.

### 6. Prüfung und Feinschliff

- Build Debug x64: Editoren dx11/vulkan/opengl, `editor_scene_runtime_dx11`, `krom_tests`.
- `krom_editor_loading_tests_dx11 --settings-ui` / `--play-ui` und die Vulkan-Varianten.
- Vulkan mit Validierung: keine neuen Meldungen (Bindings t14, Slot 3).
- MSAA 1×/4× unter DX11 und Vulkan.
- Sichtprüfung durch den Nutzer im DemoProject: Objekte auf dem Boden, dünne Objekte
  (Stuhlbeine, Geländer), schräge Flächen, Kameraschwenk. Typische Fehler und Gegenmittel:
  - Streifen/Flecken auf flachen Flächen → Startversatz, `thickness` kleiner.
  - Schwebende Schatten hinter dünnen Objekten → `thickness` kleiner.
  - Fehlende Schatten am Bildrand → Ausblendung am Rand ist gewollt; nur prüfen, dass es
    weich ist.
  - Rauschen → Blur-Radius, IGN statt reinem Zufall.
- Eintrag in `KOORDINATION.md` für die Prüfung durch Codex.

### 7. Ausbau (optional)

Punkt- und Spotlichter: pro Licht ein Strahl zur Lichtposition. Teurer (ein Trace pro
Licht). Entweder auf die N nächsten Lichter begrenzt oder direkt in der Lichtschleife der
PBR-Shader statt als eigener Pass. Erst angehen, wenn Schritt 6 abgenommen ist.

## Risiken

- **Schritt 1 berührt den Forward-Pfad für alle.** Ein Fehler dort trifft auch GTAO und
  SSR. Darum Schritt 1 einzeln bauen, testen und committen, bevor das Addon kommt.
- **Licht-Index** zwischen Snapshot und Shader (Schritt 2) ist nicht geprüft.
- **Screen-Space-Grenzen:** Was nicht im Bild ist, wirft keinen Kontaktschatten. Das ist
  bei jeder Engine so und wird nur ausgeblendet, nicht behoben.

## Protokoll

(Jede Sitzung trägt hier ein: Datum, Schritt, Ergebnis, Abweichungen, offene Punkte.)

### 2026-11-18 – Schritt 1 (Claude, Zweig `feature/opaque-inputs`)

- Erledigt: `FrameRecipe::opaqueInputs` (`{resourceName, textureSlot}`, nur aus der Phase
  `AfterDepthPrepass`). Der Vorpass läuft genau dann, wenn es eine solche Eingabe gibt und
  die View ihn erlaubt; Opaque bekommt dafür `ReadTexture`, die Ausführung bindet die Textur
  auf den Slot (fehlt sie: Weiß-Fallback). `PassSRV0` wird vorher immer auf Weiß gesetzt,
  weil der Sky-Pass dort seine Cubemap lässt.
- GTAO nutzt den neuen Weg (`GTAOOutput` → `PassSRV0`); `StandardFramePipeline` kennt GTAO
  nicht mehr beim Namen.
- Abweichung: statt `WantsDepthPrepass()` bedeutet `RenderView::enableDepthPrepass` jetzt
  „View erlaubt den Vorpass“. Editor-Szene, Game-View und Runtime setzen es immer; ob er
  läuft, entscheiden die Opaque-Eingaben. Neu: `PassFrameView::enableDepthPrepass` (für die
  Kontaktschatten in Schritt 2); `enableAmbientOcclusion` ist wieder nur der View-Schalter,
  GTAO prüft beide.
- Geprüft: Debug x64 komplett gebaut, `krom_tests` grün (neuer Test „opaque input enables
  depth prepass“). `--gtao-motion` auf DX11 und Vulkan (`KROM_VALIDATION=1`) ohne Fehler,
  AO bei Bewegung = Standbild außer am animierten Türpaneel (wie am 2026-11-15). OpenGL
  startet, ohne Vorpass.
- Offen: kein Pixelvergleich gegen `main` (nur gegen das Standbild desselben Builds).
  „GTAO aus zur Laufzeit“ inzwischen geprüft (echter Schalter `gtaoEnabled`, Vorpass fällt
  weg, siehe `KOORDINATION.md`).

### 2026-11-18 – Schritte 2–6 (Claude, Zweig `feature/contact-shadows`)

- Addon `addons/contact_shadow` (`engine_contact_shadow`, hängt an `engine_forward` und `engine_lighting`):
  `ContactShadowSettings` (enabled, length 0,30 m, thickness 0,05 m, intensity 1, steps 12),
  `ContactShadowGpuResources`, `CreateContactShadowFeature`. Pässe in `AfterDepthPrepass`:
  `ContactShadowTrace` (liest `OpaqueDepth`, bei MSAA Sample 0, schreibt `ContactShadowRaw` RG16F =
  Sichtbarkeit + lineare Tiefe) → `ContactShadowBlur` (3×3 tiefenbewusst) → `ContactShadowOutput` R8,
  als Opaque-Eingabe angemeldet. Nur bei Views mit erlaubtem Vorpass; ohne Hauptlicht endet der Trace sofort.
- **Abweichung Slot:** nicht t14, sondern **t20** (`TexSlots::ContactShadow`, `TexSlots::COUNT` 20 → 21).
  t14/t15 sind in Forward+ die Schatten-Buffer (`BufSRVSlots`; unter DX11 teilen sich Texturen und Buffer
  den t-Raum), t0–t19 sind im Fragment-Shader voll. Vulkan-Binding 36 liegt im reservierten Bereich (16–39).
  Ein alter Test nahm Binding 20 als "außerhalb" an → jetzt 21.
- **Licht-Index (Risiko aus dem Plan):** Der Frame-Constants-Beitrag liest `LightingFrameData::lights`, also
  genau die Reihenfolge von `lights[]` bzw. dem Forward+-Lichtpuffer. Hauptlicht = erstes Richtungslicht mit
  `castShadows` und `contactShadows` (`SelectContactShadowLight`, getestet). Parameterblock Slots 3+4:
  Richtung zum Licht im **Weltraum** (statt View-Space; der Trace rechnet in Weltkoordinaten), Index+1,
  Länge, Dicke, Stärke, Schritte.
- Trace: Normale aus der Tiefe (ddx/ddy); vom Licht abgewandte Flächen werden übersprungen (der erste Stand
  machte sie komplett schwarz), Start um 2 mm + 0,05 % der Tiefe entlang der Normale versetzt, IGN-Jitter,
  Tiefentoleranz wächst mit der Entfernung, Ausblenden am Strahlende, am Bildrand und von 40 bis 50 m.
- PBR-Shader (`pbr_lit`, `pbr_forward_plus`): `contactShadowTex` auf t20, einmal pro Pixel gelesen,
  `shadowVisibility *= contactShadowFactor` nur beim Hauptlicht, nicht bei `DBG_DISABLE_SHADOWS`; in
  `pbr_lit` innerhalb von `!KROM_SHADOWS_NONE` (Forward+ hat diese Variante nicht). Debug-Bits 22/23.
  Der Transparent-Pass bindet auf allen Opaque-Eingabe-Slots Weiß (vorher nur `PassSRV0`).
- Einstellungen: `ProjectRenderSettings` `contactShadowsEnabled/Length/Thickness/Intensity/Steps` (fehlend =
  Standard), Abschnitt "Kontaktschatten" in Projekteinstellungen > Rendering (eigene ImGui-ID, de/en),
  `ShadowSettings::contactShadows` (Standard an, Szenen-Schlüssel `contactShadows`), Checkbox im Inspector
  unter "Schatten werfen" (nur Richtungslicht, mit Undo). Editor und krom.h-Runtime geben die Projektwerte in
  `ApplyProjectRenderSettings` weiter; `AppConfig::enableContactShadows` / krom.h `enableContactShadows`.
  Script-API: offen (wie geplant).
- Debug: Umschalt+F3 = Kontaktschatten-Ansicht, Umschalt+F4 = Kontaktschatten aus (F3/F4 = GTAO; ein Menü
  gibt es nicht). Testhilfe `KROM_CAPTURE_CONTACT=1` für `--gtao-motion`.
- Geprüft: Debug x64 alle Ziele, `krom_tests` grün (neuer Test "Contact shadow pass recipe": Lichtauswahl,
  Block, Slot, Pässe/Opaque-Lesezugriff mit/ohne Vorpass/aus; Rundweg Projekt + Licht). Room 2: DX11 MSAA
  1×/2× und Vulkan MSAA 2× mit `KROM_VALIDATION=1` ohne Meldung; DX11/Vulkan-Ansicht mittlere Abweichung
  0,12/255; Lit an/aus 26 vs. 24 Pässe. OpenGL startet (12 Pässe, ohne Kontaktschatten). Bild:
  `out/claude-contact-compare.png` (Kontaktlinien unter Säule und Kisten; wo die Shadow Map schon
  greift, kaum Unterschied).
- GPU-Zeit (Debug, 1181×709, `KROM_PERF_GPU_PASSES=1`): Trace DX11 0,27–0,35 ms, Vulkan 0,21–0,29 ms;
  Blur 0,06–0,23 ms. Das Ziel "< 0,5 ms bei 1080p" wird hochgerechnet (~2,5× Pixel) **nicht** erreicht →
  Feinschliff: weniger Schritte als Standard oder halbe Auflösung.
- Offen: Sichtprüfung durch den Nutzer (dünne Objekte, schräge Flächen, Schwenk), MSAA 4×, gepunktetes
  Treppenmuster an steil gesehenen Kanten (Jitter + kleiner Blur), dünne dunkle Linie an Silhouetten
  (ddx-Normale über Tiefensprung), Script-API, Punkt-/Spotlichter (Schritt 7).

### 2026-11-19 – Feinschliff nach Sichtprüfung (Claude)

- Nutzer sah harte schwarze Sicheln an der Wand neben einem Würfel. Treffer sind jetzt weich: Stärke fällt
  mit dem Abstand zum Verdecker (Smoothstep über die Strahllänge), angenommene Dicke wächst entlang des
  Strahls (`thickness + t·0,5`), Ausblenden am Bildrand pro Treffer und bei streifendem Licht
  (`dot(N,L)` 0,02–0,145). Nutzer hat das Ergebnis im Editor bestätigt.
