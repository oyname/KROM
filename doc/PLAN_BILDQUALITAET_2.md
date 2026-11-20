# Plan: Bildqualität, Teil 2

Stand: 2026-11-18, auf `main` nach `7e3287f`. Fortsetzung von `doc/PLAN_BILDQUALITAET.md`
(abgeschlossen). Jede Sitzung liest diese Datei zuerst und trägt am Ende ein, was erledigt
ist, was offen bleibt und was vom Plan abweicht.

## Ziel

Den Rückstand zu Unity HDRP / Unreal 5 dort aufholen, wo er am größten ist und am wenigsten
kostet: physikalisch richtiges AO, Kantenglättung über MSAA hinaus, automatische Belichtung,
Qualitätsstufen und Post-Process-Volumes. Zeitliche Verfahren (TAA) kommen zuletzt.

## Rahmen

- Backends: **DX11 und Vulkan**. OpenGL ist eingefroren: neue Features bleiben dort aus,
  OpenGL-Editor und -Runtime müssen aber weiter bauen und starten.
- Pro Schritt ein eigener Zweig von `main` und ein Eintrag in `KOORDINATION.md`; Codex prüft.
- Vor und nach jedem Render-Schritt die GPU-Zeit pro Pass messen (Auftrag 2 der ersten
  Reihe) und hier eintragen.
- Fremde, nicht committete Änderungen im Arbeitsordner (DemoProject-Szenen,
  `assets/weisse_textur.png`, `editor/logo.png`) bleiben unangetastet.
- Bedienabläufe im Editor werden vor der Umsetzung abgestimmt (vorhandene Konzepte nutzen:
  Hierarchie, Inspector, Projekteinstellungen).

## Ausgangslage (geprüft 2026-11-18)

| Thema | Stand | Folge |
|---|---|---|
| AO | `addons/gtao/GtaoPass.cpp:129`: HBAO-artig, 4 Richtungen × 4 Schritte, ein Horizont pro Richtung, kein Cosinus-Gewicht, Radius auf 64 px begrenzt, `sin`-Hash als Rauschen, ein bilateraler Blur | Schritt 2 |
| AO-Zeitpunkt | Seit 2026-11-15 aus dem Tiefen-Vorpass desselben Frames (`doc/POSTMORTEM_GTAO_DEPTH_PREPASS.md`); der Vorpass war aber fest an GTAO gekoppelt | Schritt 1 |
| Anwendung AO | nur auf Umgebungslicht, Specular Occlusion vorhanden (`shaders/pbr_lit.ps.hlsl:674 ff.`) | bleibt |
| Tonemapping | Reinhard, ACES (Hill), AgX (Standard), Khronos PBR Neutral, Belichtung in EV pro Szene (`shaders/passthrough.ps.hlsl`) | bleibt; Auto-Exposure fehlt (Schritt 6) |
| AA | nur MSAA 2×/4× mit luminanzgewichtetem Resolve (`addons/forward/ForwardFeature.cpp:128`); kein FXAA/SMAA/TAA, kein Specular-AA | Schritte 3, 5, 11 |
| Shading | GGX, Smith (optional höhenkorreliert), Schlick, Multi-Scatter, Split-Sum-IBL, Lambert diffus; Optionen in den Projekteinstellungen standardmäßig an | bleibt; Ausbau in Schritt 12 |
| Bedienung | Projekteinstellungen (Operator, MSAA, GTAO, SSR, Shading), Belichtung im Inspector pro Szene, Script-API `IRenderSettingsService` ohne GTAO/SSR; keine Volumes, keine Qualitätsstufen | Schritte 4, 7, 8, 9 |

## Schritte

| Nr. | Schritt | Status |
|-----|---------|--------|
| 1 | Tiefen-Vorpass von GTAO lösen, allgemeine Opaque-Eingaben (= Kontaktschatten Schritt 1) | erledigt (2026-11-18), Review offen |
| 2 | Echtes GTAO | erledigt (2026-11-18), Review offen |
| 3 | Specular-AA | erledigt (2026-11-18), Review offen |
| 4 | Script-API: GTAO und SSR an/aus | erledigt (2026-11-18), GTAO mit Projekt-Häkchen |
| 5 | Post-AA: SMAA 1x, FXAA | erledigt (2026-11-19: FXAA, SMAA 1x), Review offen |
| 6 | Auto-Exposure | erledigt (2026-11-19), Review offen |
| 7 | Qualitätsstufen | erledigt (2026-11-19), Review offen |
| 7b | Color Grading und AgX-Looks (pro Szene) | erledigt (2026-11-19), Review offen |
| 8 | Post-Process-Volume: Bedienablauf abstimmen | erledigt (2026-11-19) |
| 9 | Post-Process-Volume: Umsetzung | erledigt (2026-11-19), Review offen |
| 10 | Motion Vectors (optional, später entscheiden) | offen |
| 11 | TAA und zeitliche Akkumulation für GTAO/SSR (optional) | offen |
| 12 | Weiteres: 3D-LUT, HDR-Display-Ausgabe, Shading-Models, Flächenlichter (optional) | offen |

Reihenfolge: 1 → 2 → 3 → 4, dann 5 und 6 (unabhängig voneinander), dann 7 → 8 → 9.
Phase 4 (10–12) erst, wenn der Rest sitzt. Der Kontaktschatten-Plan läuft nach Schritt 1
unabhängig weiter.

## Phase 1: Grundlage und schnelle Verbesserungen

### 1. Vorpass von GTAO lösen

- Korrektur zum ersten Planstand: GTAO las schon seit 2026-11-15 nicht mehr das AO des
  Vorframes. Übrig blieb Schritt 1 aus `doc/PLAN_KONTAKTSCHATTEN.md`: Vorpass von GTAO
  lösen, allgemeine Eingaben des Opaque-Passes (`FrameRecipe::opaqueInputs`), GTAO auf
  diesen Weg umstellen. Details und Prüfung dort im Protokoll.

### 2. Echtes GTAO

- `addons/gtao/GtaoPass.cpp`, Hauptshader. Nach Jimenez et al. 2016:
  - pro Scheibe zwei Horizonte (h1, h2) in beide Richtungen,
  - Normale aus der Tiefe rekonstruiert und in die Scheibenebene projiziert,
  - analytisches cosinusgewichtetes Integral statt `1 - sin(h)`,
  - Dicken-Heuristik gegen zu viel AO hinter dünnen Objekten,
  - Multi-Bounce-Annäherung (Albedo-abhängig) im PBR-Shader,
  - Interleaved Gradient Noise statt `sin`-Hash; Radiusgrenze von 64 px prüfen.
- Regler (Radius, Intensität, Bias) bleiben; ein zusätzlicher Regler nur, wenn nötig
  (z. B. Dicke).
- Prüfen: Vergleich vorher/nachher in Ecken, an Kanten und an dünnen Objekten; GPU-Zeit.

### 3. Specular-AA

- `shaders/pbr_lit.ps.hlsl`, `shaders/pbr_forward_plus.ps.hlsl` (OpenGL-Zwilling nur, wenn
  gemeinsamer Code es verlangt).
- Roughness abhängig von der Varianz der Normale im Bildraum anheben (Kaplanyan 2016 /
  Tokuyoshi). Als Shading-Option in den Projekteinstellungen, Standard an.
- Prüfen: Normal-Map-Flächen und glänzende Kanten in Bewegung, mit und ohne MSAA.

### 4. Script-API

- `addons/script/IRenderSettingsService.hpp` und `krom.h`: `gtaoEnabled`, `ssrEnabled`
  (Projektwerte als Startwerte, wie die übrigen Felder).
- Tests für Get/Set; im Editor kehren nach Play die Projektwerte zurück.

## Phase 2: Bild fertig machen

### 5. Post-AA

- Eigener Pass nach dem Tonemapping (SMAA braucht ein LDR-Bild): SMAA 1x (Kanten, Gewichte,
  Mischen; Area/Search-Texturen als Assets) und FXAA als günstige Stufe.
- Projekteinstellungen: „Post-AA“ Aus / FXAA / SMAA, unabhängig von MSAA.
- Prüfen: Alpha-Test-Kanten, Text/Editor-Overlays dürfen nicht weichgezeichnet werden
  (Reihenfolge zu Outline und Gizmos klären).

### 6. Auto-Exposure

- Luminanz-Histogramm per Compute aus dem HDR-Bild vor dem Tonemap-Pass, Auswertung
  ohne Ausreißer (z. B. 10–90 %), Wert bleibt auf der GPU und wird zeitlich angepasst.
- Einstellungen: Modus Fest/Automatisch, Min-EV, Max-EV, Geschwindigkeit hell→dunkel und
  dunkel→hell, Korrektur (± EV). Die heutige Szenen-Belichtung wird im Modus Fest
  weiterverwendet; alte Szenen behalten ihr Aussehen.
- Editor: Schalter „Belichtung einfrieren“ und Anzeige des aktuellen EV.
- Ohne Compute im Backend: fester Wert.
- Vor Schritt 8 abschließen, weil die Volumes davon abhängen (Grenzen/Korrektur statt
  festem EV).

### 7. Qualitätsstufen

- Projekteinstellungen: Niedrig / Mittel / Hoch / Eigene, jeweils MSAA, Post-AA, GTAO,
  SSR, Kontaktschatten (sobald vorhanden).
- Script-API: Stufe setzen und lesen.
- Abgestimmt (Nutzer, 2026-11-19): Der Spieler wählt nur die Stufe. Niedrig = FXAA, sonst alles
  aus; Mittel = SMAA, GTAO, Kontaktschatten; Hoch = MSAA 2×, SMAA, alles an. Stufen setzen nur
  Techniken, keine Regler. Die Stufe wird aus den Optionen abgeleitet (eine Option einzeln
  geändert → „Eigene“), kein eigener Projektschlüssel. Das Projekt liefert den Startwert; eine
  per Script gewählte Stufe merkt sich das exportierte Spiel pro Rechner (wie Unreal
  `GameUserSettings.ini`). Script-API: Stufe setzen/lesen, dazu Post-AA und Kontaktschatten.

### 7b. Color Grading und AgX-Looks

- Eingeschoben am 2026-11-19 (Nutzerwunsch), vor den Volumes, damit Schritt 9 die
  Grading-Werte gleich mit überschreiben kann.
- Im Tonemap-Pass (`shaders/passthrough.ps.hlsl`), pro Szene wie die Belichtung
  (Inspector der Szene, eigene Karte), Script-API und krom.h.
- Parametrisch: Weißabgleich (Temperatur, Tönung), Farbfilter, Kontrast, Sättigung,
  Lift/Gamma/Gain; AgX-Looks als Voreinstellungen im AgX-Pfad. Fehlende Schlüssel =
  neutral, alte Szenen sehen gleich aus.
- Nicht in diesem Schritt: 3D-LUT-Import, HDR-Display-Ausgabe (beide Schritt 12).
- Bedienablauf vor dem Bauen mit dem Nutzer abstimmen.

## Phase 3: Volumes

### 8. Bedienablauf abstimmen

Abgestimmt mit dem Nutzer am 2026-11-19 (gilt für Schritt 9):

- **Anlegen:** Hierarchie → Erstellen → „Post-Process-Volume“ (Entity mit Komponente) oder
  an einer vorhandenen Entity „Komponente hinzufügen → Post Process“.
- **Bereich: eigene Box wie die Reflection Probe** (nicht die Trigger-Box): Ausdehnung,
  Versatz, Überblendweite in Metern nach innen (an der Wand 0 %, ab dieser Tiefe 100 %),
  Priorität; Griffe im Viewport wie bei der Probe, innere Überblendgrenze gestrichelt.
- **Kopf der Komponente:** Häkchen „Global“ (Box wird ignoriert), Gewicht 0..1 (auch per
  Script, z. B. Schadens-Effekt).
- **Überschreibbar, je Feld mit Häkchen** (nicht angehakt = ausgegraut, wirkt nicht):
  - Belichtung: feste Belichtung bzw. Korrektur, Min/Max-EV der Automatik,
  - Farbkorrektur: alle Regler der Szenenkarte (`ColorGradingSettings`),
  - Effekte: Intensität von Bloom, GTAO, SSR, Kontaktschatten.
- **Nicht überschreibbar:** Belichtungsmodus, Tonemap-Operator, MSAA, Post-AA, Effekte an/aus
  – sie bauen den Frame-Graph oder Pipelines um (Ruckler beim Betreten).
- **Mischen:** Grundwerte = Szenenkarten „Belichtung“/„Farbkorrektur“ und
  Projekteinstellungen; Volumes nach Priorität aufsteigend, je Gewicht × Überblendfaktor
  linear darüber. Maßgeblich ist die Editor-Kamera bzw. im Play-Modus die Spielkamera.
  Werte ohne Zwischenstufen (AgX-Look) wechseln ab mehr als 50 %.
- Werte stehen direkt in der Komponente, keine Profil-Assets; Wiederverwendung über Prefabs.

### 9. Umsetzung

- Überschreibbar: Belichtungskorrektur und -grenzen, Bloom-Intensität, GTAO-, SSR- und
  Kontaktschatten-Intensität.
- Mischen pro Frame aus der Kameraposition (Priorität, Abstand zur Grenze).
- Inspector, Gizmo für die Grenzen, Szenen-Serialisierung (fehlender Schlüssel → kein
  Override), Script: Gewicht setzen (z. B. für Schadens-Effekte).

## Phase 4: Groß und optional

### 10. Motion Vectors

- Pro Pixel Bewegung aus aktueller und letzter Matrix; Skinning, Partikel, Kamera.

### 11. TAA

- Jitter, Reprojektion, Nachbarschafts-Clamping; danach zeitliche Akkumulation für GTAO
  (dann ggf. halbe Auflösung) und SSR.
- Risiken: Ghosting, Unschärfe, Editor-Gizmos/Outline, Zusammenspiel mit MSAA.

### 12. Weiteres

- 3D-LUT für das Color Grading, HDR-Display-Ausgabe (PQ/scRGB; auf 2026-11-19 vom Nutzer
  zurückgestellt), Shading-Models (Clearcoat, Sheen), Flächenlichter (LTC), offene
  1/π-Frage der Probes.

## Bewusst nicht geplant

- Weitere Tonemap-Operatoren (die vier vorhandenen reichen).
- MSAA oder neue Effekte unter OpenGL.
- TAA vor den Phasen 1–3.

## Protokoll

(Jede Sitzung trägt hier ein: Datum, Schritt, Zweig, erledigt / offen / Abweichung.)

### 2026-11-18 – Schritt 1 (Claude, Zweig `feature/opaque-inputs`)

- Umgesetzt wie in `doc/PLAN_KONTAKTSCHATTEN.md`, Protokoll vom selben Tag. Nicht committet.

### 2026-11-18 – Schritt 4, Teil GTAO (Claude, Zweig `feature/opaque-inputs`)

- Vorgezogen: echter GTAO-Schalter. Projektschlüssel `gtaoEnabled` (fehlt = an), Häkchen
  „Aktiv“ in den Projekteinstellungen, `RenderSettings::gtaoEnabled` für Scripts, krom.h
  `SetGtaoEnabled`/`GetGtaoEnabled`. Aus spart GTAO-Pässe und Vorpass (24 → 20 Pässe in
  Room 2, DX11 und Vulkan ohne Validierungsmeldung). Offen: `ssrEnabled` in der Script-API.

### 2026-11-18 – Schritt 4, Teil SSR (Claude, Zweig `feature/opaque-inputs`)

- `RenderSettings::ssrEnabled` für Scripts, krom.h `SetSsrEnabled`/`GetSsrEnabled`. Gebaut,
  `krom_tests` grün. Nicht geprüft: Umschalten per Script zur Laufzeit (kein Test dafür).

### 2026-11-18 – Schritt 2 (Claude, Zweig `feature/opaque-inputs`)

- Nicht wie geplant auf eigenem Zweig: auf `feature/opaque-inputs` über den Stand von Schritt 1 und 4. Nicht committet.
- `addons/gtao/GtaoPass.cpp`, Hauptshader neu nach Jimenez et al. 2016: 3 Scheiben × 2 Seiten × 4 Schritte
  (quadratisch verteilt, erster Schritt ≥ 1,3 px), Normale aus der Tiefe (pro Achse der Nachbar mit kleinerem
  Tiefensprung, zur Kamera gedreht), in die Scheibenebene projiziert, zwei Horizonte, analytisches
  cosinusgewichtetes Integral, Interleaved Gradient Noise für Scheibenwinkel und Schritt-Jitter. Bias = Winkel,
  um den beide Horizonte von V weggekippt werden. Abfall: Gewicht 1 bis 1 Radius, 0 bei 2 Radien (wie vorher).
- Dicke: Ein Sample unter dem bisherigen Horizont zieht ihn um 25 % (× Abfallgewicht) zurück. Fester Wert,
  kein neuer Regler (im Testbild kein Bedarf). Bei Bedarf als Projektregler „Dicke“ nachrüsten.
- 64-px-Grenze ersetzt durch 0,25 × Bildhöhe (bei 1080p 270 px). Der Weltradius bleibt damit bis nah an die
  Kamera erhalten.
- Multi-Bounce (Jimenez-Fit, albedoabhängig) in `shaders/pbr_lit.ps.hlsl` und `shaders/pbr_forward_plus.ps.hlsl`,
  nur auf das diffuse Umgebungslicht; Specular Occlusion bekommt weiter den skalaren Faktor. GTAO aus → wie vorher.
- Befund: Der alte Shader kannte keine Normale und dunkelte jede schräg gesehene ebene Fläche ab (Boden in Room 2
  ~15 % dunkler, F3 Mittelwert 192,9 → 203,5 von 255). Am Säulenfuß gab es keinen Kontaktschatten, sondern einen
  hellen Saum. Jetzt sind freie Flächen 1, AO nur an Kontakten. Mit Radius 0,10 m (DemoProject) ist das ein
  schmaler Kontaktschatten; 0,3–0,5 m zeigt Ecken und Kisten deutlich. Folge: Szenen wirken heller; ggf.
  Standardradius anheben (offen, Nutzer).
- Bilder: `out/claude-realgtao-compare.png`, `out/claude-realgtao-compare-crop.png` (F3, DX11, Room 2).
  DX11 und Vulkan nachher: mittlere Abweichung 0,09/255. Bewegung = Standbild bis auf die animierte Tür.

GPU-Zeit pro Pass (`--perf`, `KROM_PERF_GPU_PASSES=1` = Profiler mit GPU-Abschnitt offen, Room 2, Debug x64,
GTX 1660, Viewport 1181×709, MSAA wie Projekt, ohne Validierung), ms:

| Backend | GtaoMain vorher | GtaoMain nachher | GTAO-Kette vorher | GTAO-Kette nachher | GPU gesamt vorher → nachher |
|---|---:|---:|---:|---:|---:|
| DX11 | 0,161 | 0,500 | 0,575 | 0,912 | 4,20 → 4,51 |
| Vulkan | 0,179 | 0,500 | 0,610 | 0,929 | 4,09 → 4,38 |

- Kette = Linearize + Main + Blur + Composite. Frame-Zeit ist im Debug-Build CPU-gebunden und schwankt unter DX11
  stark (22–33 ms zwischen Läufen), daher nicht aussagekräftig; Vulkan 16,0 → 15,3 ms (Rauschen).
- Offen: weniger Scheiben (2 statt 3) würde ~0,15 ms sparen, rauscht aber stärker; zeitliche Akkumulation
  (Schritt 11) wäre der eigentliche Weg zu weniger Samples. Feine senkrechte Streifen auf steil gesehenen
  Flächen und ein kaum sichtbarer Bogen an der Radiusgrenze (≤ 1/255 nach Tonemapping) bleiben.
- Sichtprüfung (Nutzer, 2026-11-18): „alles super“.
- Nachtrag (Nutzer): Standardradius 0,10 → 0,50 m (`GtaoSettings`, `ProjectSettings`), Reglergrenze und Laden
  1,0 → 2,0 m, Tooltip nennt statt 64 px ein Viertel der Bildhöhe. Gespeicherte Projekte behalten ihren Wert
  (DemoProject 0,1). Test: fehlender Schlüssel = 0,5, 1,5 m übersteht den Rundweg. Bei 0,5 m GtaoMain DX11
  0,533 ms, Vulkan 0,683 ms; DX11/Vulkan mit Validierung ohne Meldung; `krom_tests` grün.
- Nicht geprüft: andere Szenen, MSAA 4×, dünne Objekte gezielt
  (Room 2 hat nur Hörner/Geländer als Beispiel).

### 2026-11-18 – Schritt 3 (Claude, Arbeitsordner auf `feature/opaque-inputs`)

- Umgesetzt von der Sitzung „specular-aa“, nach dem Rechnerabsturz in der GTAO-Sitzung geprüft und
  abgeschlossen. Nicht committet; liegt uncommittet über PR #32 (eigener Zweig noch offen).
- `ApplySpecularAA` in `shaders/pbr_lit.ps.hlsl` und `shaders/pbr_forward_plus.ps.hlsl` nach Tokuyoshi &
  Kaplanyan 2021: Bildraum-Ableitungen der Shading-Normale → Kernel-α² = 2σ²(|dN/dx|² + |dN/dy|²),
  σ² = 1/2π, begrenzt auf 0,18, auf α² addiert; Roughness nie kleiner als vorher. Wirkt jetzt auch ohne
  Normal-Map (gekrümmte glänzende Geometrie); die alte Variante lief nur mit `KROM_NORMAL_MAP` und immer.
- Shading-Option Bit 3 (`ShadingSettings::specularAntiAliasing`, Standard an): Projektschlüssel
  `specularAntiAliasing` (fehlt = an), Häkchen in Projekteinstellungen > Shading, `RenderSettings` für
  Scripts, krom.h über `SetShading`. OpenGL wertet Bit 3 nicht aus und behält die alte Normal-Map-Variante.
- Testhilfen in `--gtao-motion`: `KROM_CAPTURE_LIT=1` nimmt die Lit-Ansicht statt F3 auf,
  `KROM_CAPTURE_YAW` setzt den Drehschritt (Grad).
- Geprüft: Debug-Build aller Ziele, `krom_tests` grün (Flags, Rundweg, fehlender Schlüssel). Room 2 Lit,
  DX11 MSAA 1×/2× und Vulkan MSAA 2× mit `KROM_VALIDATION=1` ohne Meldung. Unterschiede an/aus nur auf
  dem Normal-Map-Boden und an den glänzenden Masken; ein Firefly neben dem Maskenauge verschwindet
  (`out/claude-specaa-compare-crop.png`). Drehung um 0,02° (unter 1 px), Masken: Pixel mit Sprung > 32/255
  764 → 490 (MSAA 1×), 632 → 539 (MSAA 2×); Boden und Kisten unverändert.
- GPU-Zeit MainOpaquePass (Debug, 1181×709): DX11 1,650 → 1,683 ms, Vulkan 1,588 → 1,612 ms.
- Nicht geprüft: Sichtprüfung im Editor in echter Bewegung, harte Kanten flach schattierter Geometrie
  (Roughness-Saum an Kanten möglich), MSAA 4×, Release.

### 2026-11-19 – Schritt 5, Teil FXAA (Claude, Zweig `feature/fxaa`)

- Nutzerentscheidung: erst FXAA, SMAA danach in einer eigenen Session. Editor-Overlays (Outline, Gizmos,
  Gitter) liegen im HDR-Bild vor dem Tonemapping und werden mitgeglättet (Empfehlung, keine Rückmeldung);
  ImGui-Text liegt separat und bleibt scharf.
- Einstellung: `PostAAMode` Off/Fxaa + `PostAASettings` in `addons/forward/ForwardAntiAliasing.hpp`
  (Namen `off`/`fxaa`), `ForwardFeatureConfig::postAASettings` (geteilt wie MSAA, Änderung baut den Graph
  neu: Strukturrevision = MSAA-Samples | Modus << 8). Unter OpenGL aus (wie MSAA).
- Rezept: `BuildParams::postAAEnabled/postAAFormat`. Bei an schreibt `TonemapPass` in `Tonemapped`
  (Format der Tonemap-Material-Pipeline = Swapchain-Format, aus `MaterialRuntimeDesc::colorFormat`),
  `PostAAPass` (Executor `frame.post_aa`) liest es und schreibt das bisherige Ziel (Backbuffer oder
  `EditorSceneView`, gleiche Clear-Einstellung). Ohne eingebautes Tonemapping (Probe-Bake bringt einen
  eigenen Tonemap-Executor) bleibt Post-AA aus.
- Shader `kFxaaPsHlsl` in `ForwardFeature.cpp`: eigene Umsetzung des FXAA-3-Quality-Verfahrens
  (Kontrasttest 0,125/0,0312, Kantenrichtung, Suche entlang der Kante 12 Schritte, Subpixel 0,75). Eingabe
  ist eine sRGB-Textur, Luma = sqrt(lineare Luma). Pixelposition statt interpolierter UV.
- Einstellungen: Projektschlüssel `postAA` (fehlend = `off`), Auswahl "Post-AA" unter Kantenglättung in
  Projekteinstellungen > Rendering (de/en, Tooltip). Editor und krom.h-Runtime geben den Wert in
  `ApplyProjectRenderSettings` weiter; Runtime zusätzlich `KROM_POST_AA=fxaa`. Script-API: mit Schritt 7.
- Geprüft: Debug x64, `krom_tests` grün (neuer Test "Post-AA recipe": Backbuffer- und Editor-Ziel,
  Format, Reihenfolge Tonemap → PostAA → UI; Projekt-Rundweg). Room 2 mit `KROM_VALIDATION=1` ohne
  Meldung: DX11 MSAA 1×/2× + FXAA, Vulkan MSAA 1× + FXAA; 24 → 25 Pässe. DX11/Vulkan mittlere Abweichung
  0,41/255. OpenGL startet (15 Pässe, ohne FXAA). Bild: `out/claude-fxaa-compare.png` (links aus,
  rechts FXAA, 4×).
- GPU-Zeit (Debug, 1181×709, MSAA 2×): `PostAAPass` DX11 0,139 ms, Vulkan 0,135 ms.
- Offen: Sichtprüfung durch den Nutzer (Textschärfe, Outline), Release. SMAA 1x: siehe nächster Eintrag.

### 2026-11-19 – Schritt 5, Teil SMAA 1x (Claude, Zweig `claude/smaa-b6b7b7`, auf `feature/fxaa`)

- Modus `PostAAMode::Smaa` (Name `smaa`) in `addons/forward/ForwardAntiAliasing.hpp`; Auswahl „SMAA“ im
  Feld „Post-AA“ (Projekteinstellungen > Rendering), Tooltip de/en ergänzt. Projektschlüssel, Editor,
  Runtime und `KROM_POST_AA=smaa` laufen über den vorhandenen FXAA-Weg.
- Rezept: `BuildParams::postAASmaa`. Zwischen `TonemapPass` und `PostAAPass` liegen `SmaaEdgePass`
  (`frame.smaa_edges`, liest `Tonemapped`, schreibt `SmaaEdges` RG8) und `SmaaWeightPass`
  (`frame.smaa_weights`, liest `SmaaEdges`, schreibt `SmaaWeights` RGBA8); beide Ziele werden auf 0
  gelöscht. `PostAAPass` ist bei SMAA das Neighborhood Blending (liest `Tonemapped` + `SmaaWeights`) und
  schreibt wie bei FXAA das bisherige Ziel (Backbuffer oder `EditorSceneView`).
- Shader in `addons/forward/ForwardSmaa.cpp`: Portierung aus dem offiziellen `SMAA.hlsl`
  (Jimenez et al., MIT), Voreinstellung „high“ (Schwelle 0,1, 16 Suchschritte, Diagonale 8, Ecken 25),
  Luma-Kantenerkennung ohne Predication/Reprojection. Texturkoordinaten aus `SV_Position` (DX11 und
  Vulkan gleich). Die Eingabe ist sRGB: die Kantenerkennung rechnet zurück in Gammaraum, das Mischen
  bleibt linear. Samplerslots s1 (linear) und s2 (Punkt), Texturen t8–t10.
- Lookup-Texturen: offizielle `AreaTex.h`/`SearchTex.h` als erzeugtes Array
  `addons/forward/SmaaLookupTextures.inl` (74 KB). Von der Area-Textur nur Zeilen 0–79 (Subsample 0,
  das einzige, was SMAA 1x liest; 160×80 RG8 statt 160×560), Search 64×16 R8 unverändert. Beim ersten
  Einschalten hochgeladen, beim Device-Ende mit allen Post-AA-Pipelines freigegeben. Lizenz:
  `licenses/SMAA.txt`, Eintrag in `licenses/README.txt`. Arrays statt Asset-Dateien: kein Pfad- oder
  Exportproblem, die Daten ändern sich nie.
- Geprüft: Debug x64 (nur `-j 4`, der volle Parallelbuild hat den Rechner zweimal abstürzen lassen),
  `krom_tests` grün (Rezept SMAA: Pässe +3, Ressourcen +3, Formate, Zugriffe, Reihenfolge
  Tonemap → Kanten → Gewichte → PostAA → UI; Namen; Projekt-Rundweg `smaa`; neuer Test: die drei Shader
  kompilieren für DX11 und Vulkan, Texturgrößen). Room 2 mit `KROM_VALIDATION=1`, MSAA 2×, ohne Meldung:
  DX11 aus/FXAA/SMAA, Vulkan aus/SMAA; 26 → 29 Pässe. SMAA DX11/Vulkan mittlere Abweichung 0,53/255
  (aus: 0,49/255). OpenGL startet (15 Pässe, ohne Post-AA).
- Bildvergleich `out/claude-smaa-compare.png` (Worktree; aus / FXAA / SMAA, 4×): SMAA glättet Treppen an
  Türkante, Warnstreifen und Kiste und bleibt schärfer als FXAA. Geänderte Pixel (> 8/255) gegenüber aus:
  FXAA 11 091, SMAA 6 484.
- GPU-Zeit (Debug, 1181×709, MSAA 2×): DX11 Kanten 0,055 + Gewichte 0,115 + Mischen 0,090 = 0,26 ms
  (FXAA 0,152 ms); Vulkan 0,066 + 0,116 + 0,057 = 0,24 ms (FXAA 0,146 ms).
- Nicht gemacht: Stencil-Maske für den Gewichte-Pass (Pixel ohne Kante kosten dort nur einen Abruf),
  Release-Messung, Sichtprüfung im Editor durch den Nutzer.

### 2026-11-19 – Schritt 6, Auto-Exposure (Claude, Zweig `claude/wonderful-taussig-04bff6`)

- Nutzerentscheidung (vorher abgestimmt): Einstellung **pro Szene** in der vorhandenen Inspector-Karte
  „Belichtung“ (Szene), keine neue Oberfläche. Modus Fest/Automatisch; das bisherige EV-Feld ist im Modus
  Automatisch die **Korrektur**. Neue und alte Szenen starten **Fest** (fehlender Schlüssel = Fest, alte
  Szenen sehen gleich aus). Script-API: Modus + Korrektur.
- Einstellungen (`addons/forward/ForwardTonemap.hpp`): `ExposureMode` Fixed/Auto (`fixed`/`auto`),
  `AutoExposureSettings` Min-EV −4, Max-EV +4, Dunkel→hell 3 EV/s, Hell→dunkel 1 EV/s (Grenzen −10…+10 EV,
  0,1…20 EV/s), `freezeAutoExposure` nur im Editor (nicht gespeichert). `AutoExposureStatus` (geteilt
  über `ForwardFeatureConfig::autoExposureStatus`): zurückgelesener aktueller EV für die Anzeige und
  Reset-Anforderung beim Szenenladen (die neue Szene springt auf ihren Wert statt überzublenden).
- Szenenschlüssel im `environment`-Block (Editor-.json und .kscene): `exposureMode`,
  `autoExposureMinEv`, `autoExposureMaxEv`, `autoExposureToBright`, `autoExposureToDark`; Lesen über
  `ReadSceneExposureJson` (`addons/forward/ForwardTonemapJson.hpp`) in Editor und krom.h-Runtime.
- Inspector: Modus, Belichtung/Korrektur, bei Automatisch Min./Max. Belichtung, beide Geschwindigkeiten,
  „Belichtung einfrieren“ und „Aktuell: +x,xx EV“ (de/en, Tooltips). Unter OpenGL bleibt die Auswahl
  bedienbar, Tooltip und Anzeige sagen „nicht verfügbar“; es gilt der feste Wert.
- Script-API: `RenderSettings::autoExposure` (Editor und Runtime); `exposureEv` ist dann die Korrektur.
- GPU (`addons/forward/ForwardAutoExposure.*`): zwei Compute-Shader direkt vor dem Tonemap-Draw.
  Histogramm: 16×16-Gruppen, ein Pixel je Thread aus dem HDR-Bild (`GTAOComposite` bzw. `HDRSceneColor`,
  ohne Bloom), 64 Bins über log2-Luminanz −14…+8, Gruppen-Histogramm in `groupshared`. Anpassung: eine
  Gruppe, liest und löscht das Histogramm, Mittel der log2-Luminanz zwischen 10 % und 90 % der Pixel,
  Ziel = log2(0,18) − Mittel, begrenzt auf Min/Max, Schritt höchstens Geschwindigkeit × dt in Richtung
  Ziel (dt aus der Wandzeit, höchstens 0,25 s). Der Zustand (float4) bleibt im GPU-Puffer; der
  Tonemap-Shader liest ihn als Buffer-SRV t13 (`shaders/passthrough.ps.hlsl`, Slot der nur im
  Vertex-Shader genutzten Bone-Palette) und rechnet 2^(EV + Korrektur). CPU-Spiegel der Rechnung
  (`AutoExposureTargetEv`, `AdaptExposureEv`) für die Tests.
- Rezept: neuer allgemeiner Haken `FrameRecipePassDesc::preRenderPassExecutorName`: ein Executor, der im
  selben Graph-Pass **vor** `BeginRenderPass` läuft (Compute ist in einem Vulkan-Render-Pass verboten;
  ein eigener Pass ohne Schreibzugriff würde vom Graphen entfernt). `TonemapPass` bekommt
  `frame.auto_exposure`, wenn `BuildParams::autoExposureEnabled`; keine zusätzlichen Pässe.
  Moduswechsel baut den Graph neu (Strukturrevision Bit 16). Ohne eingebautes Tonemapping
  (Probe-Bake) bleibt Auto-Exposure aus.
- Backend: Vulkan-Texturen sind jetzt auch für Compute sichtbar (`BuildEnginePipelineBindingLayout`,
  Bereich 4). DX11 `MapBuffer` liest Staging-Puffer (`MemoryAccess::CpuRead`) mit `D3D11_MAP_READ`.
  Rücklesen für die Anzeige über 4 Kopien (älteste wird gelesen, also 4 Frames alt, keine Wartezeit).
- Geprüft: Debug x64 (nur `-j 4`), `krom_tests` grün, neu: „Auto-exposure settings“ (Namen, Grenzen,
  Ziel/Schritt), „Auto-exposure scene keys“ (alte Szene = Fest, Rundweg, Klemmen), „Auto-exposure
  recipe“ (Pre-Executor am Tonemap-Pass mit Backbuffer/Editor-Ziel/Post-AA, keine Zusatzpässe),
  „Recipe pre-render-pass executor“ (läuft vor `BeginRenderPass`), „Auto-exposure shaders compile“
  (beide CS + Tonemap-PS für DX11 und SPIR-V, Textursichtbarkeit Compute). Room 2 (Kopie unter
  `out/ae-auto`, `exposureMode=auto`, Korrektur +0,5) mit `KROM_VALIDATION=1 KROM_CAPTURE_LIT=1
  --gtao-motion` ohne Meldung: DX11 und Vulkan je Fest und Automatisch, 26 Pässe wie vorher. Gemessen
  (inkl. Korrektur): DX11 +1,350 EV, Vulkan +1,349 EV; DX11/Vulkan mittlere Abweichung 0,50/255
  (Fest 0,49/255); Automatisch gegenüber Fest 25,5/255 heller. OpenGL startet (15 Pässe, fester Wert).
- GPU-Zeit (Debug, 1181×709, MSAA 2×, `--perf`): TonemapPass DX11 0,043 → 0,126 ms (Auto-Exposure
  0,083 ms), Vulkan 0,040 → 0,101 ms (0,061 ms); Descriptor-Updates weiter 0/Frame.
- Abweichung vom Plan: kein eigener Pass im GPU-Profiler (die Zeit steckt im TonemapPass, siehe oben).
  Die erzeugte GLSL-Variante (`shaders/generated/`, Experiment) ist nicht neu erzeugt; der neue Teil
  von `passthrough.ps.hlsl` ist mit `KROM_OPENGL` ausgeklammert.
- Sichtprüfung im Editor durch den Nutzer nach dem lokalen Merge in `main` (55d4172): „wirkt sehr gut“
  (2026-11-19).
- Offen: Release-Messung, Review durch Codex; Mittengewichtung bewusst weggelassen.

### 2026-11-19 – Schritt 7, Qualitätsstufen (Claude, Zweig `claude/admiring-cannon-fdd311`)

- Zweig vorher per Fast-Forward auf `main` (909900d, mit Auto-Exposure) gebracht.
- Nutzerentscheidung (vorher abgestimmt, nach Vergleich mit Unreal/Unity): Der Spieler wählt nur die Stufe.
  Niedrig = MSAA aus, FXAA, GTAO/SSR/Kontaktschatten aus; Mittel = MSAA aus, SMAA, GTAO und Kontaktschatten an,
  SSR aus; Hoch = MSAA 2×, SMAA, alles an. Stufen setzen nur diese fünf Techniken, keine Regler (Radius,
  Intensität, Schritte bleiben). Das passt zu Schritt 8: Volumes ändern nur Werte.
- `include/core/RenderQuality.hpp` (neu): `RenderQualityLevel` Low/Medium/High/Custom, `RenderPostAA` (Ids wie
  `PostAAMode`, `static_assert` in `KromWrapper.cpp`), `RenderQualityOptions`, `RenderQualityPreset`,
  `DetectRenderQualityLevel`, Namen `low`/`medium`/`high`/`custom`.
- Kein eigener Projektschlüssel: Die Stufe wird aus den Optionen abgeleitet (`GetRenderQualityLevel`); passt
  keine, ist es „Eigene“. Auswahl einer Stufe setzt die fünf Optionen (`ApplyRenderQualityLevel`), „Eigene“
  ändert nichts. Alte Projekte zeigen, was zu ihren Optionen passt (DemoProject: „Eigene“, Post-AA aus).
- Editor: Projekteinstellungen > Rendering, ganz oben Abschnitt „Qualitätsstufe“ mit Auswahl „Stufe“, Tooltip
  (Inhalte der Stufen) und Hinweis (Startwert; Spielerwahl merkt sich das exportierte Spiel), de/en.
- Pro Rechner (nur krom.h-Runtime): `Krom::SetQualityLevel` bzw. `IRenderSettingsService::SetQualityLevel`
  schreibt `user-settings.json` (`{"qualityLevel": "low"}`) nach `%LOCALAPPDATA%\KROM\Games\<Fenstertitel>\`
  (Fenstertitel ohne in Dateinamen verbotene Zeichen, sonst EXE-Name; sonst XDG/`~/.config/krom/games`),
  `KROM_USER_SETTINGS_DIR` ersetzt den Ordner (Tests). `LoadProject` liest sie; die gespeicherte Stufe ersetzt
  in `ApplyProjectRenderSettings` die fünf Projektoptionen (auch bei Szenen alter Projekte). Aus dem Editor
  gestartete Läufe (`--editor-live-state`) lesen und schreiben sie nicht. Der Editor selbst nutzt immer das
  Projekt; im Editor-Play wirkt `SetQualityLevel` nur bis Play-Ende.
- Script-API (`IRenderSettingsService`): `GetQualityLevel`/`SetQualityLevel` (Standard über Get/Set,
  Runtime überschreibt zum Speichern), neue Felder `postAA` (Aus/FXAA/SMAA, aus Schritt 5 nachgeholt) und
  `contactShadowsEnabled`. krom.h: `SetQualityLevel`/`GetQualityLevel`, `SetPostAA`/`GetPostAA`,
  `SetContactShadowsEnabled`/`GetContactShadowsEnabled`. Diagnosezeile nach dem Registrieren der Features:
  „render quality 'low' (MSAA 1, post-AA 1, …), player's level“.
- Geprüft: Debug x64 (`-j 4`, nur benötigte Ziele), `krom_tests` grün, neuer Test „Render quality levels“
  (55 Prüfungen: Inhalte, Erkennen/Namen, Projektoptionen inkl. unbekanntem Post-AA und MSAA 3 → 2, Regler
  bleiben, Script-Standardfunktionen, `user-settings.json` Rundweg/ungültig/custom, Ordnernamen). Room 2
  (Kopien `out/q-low|medium|high`) mit `KROM_VALIDATION=1 KROM_CAPTURE_LIT=1 --gtao-motion` ohne Meldung:
  DX11 und Vulkan je Niedrig/Mittel/Hoch, OpenGL Hoch (startet, Optionen ohne Wirkung). DX11/Vulkan mittlere
  Abweichung 0,51 / 0,52 / 0,52 von 255; Hoch gegenüber Niedrig 1,43/255, gegenüber Mittel 1,01/255 (Room 2 hat
  wenig Spiegelung und kleinen GTAO-Radius). Runtime (`editor_scene_runtime_*`, Projekt Hoch,
  `user-settings.json` = low): DX11 und Vulkan starten mit „low … player's level“; mit `--editor-live-state`
  „high“; ohne Datei „high“; OpenGL startet („custom“, weil GTAO/SSR/Kontaktschatten dort fehlen).
- GPU-Zeit (`--perf`, `KROM_PERF_GPU_PASSES=1`, Debug, GTX 1660, Fenster 1920×1040, Room 2), ms:

| Stufe | Pässe | DX11 GPU gesamt | Vulkan GPU gesamt | Frame DX11 / Vulkan |
|---|---:|---:|---:|---:|
| Niedrig | 16 | 2,53 | 2,36 | 32,4 / 14,4 |
| Mittel | 25 | 3,94 | 3,96 | 32,4 / 15,5 |
| Hoch | 29 | 5,98 | 5,52 | 31,3 / 15,9 |

  Größte Posten: Hoch SSR (Trace + Resolve + Composite) DX11 1,59, Vulkan 1,44 ms; GTAO-Kette Mittel/Hoch
  ≈ 1,0 ms; Kontaktschatten ≈ 0,4 ms; FXAA 0,16 ms, SMAA ≈ 0,23–0,29 ms; MSAA 2× hebt MainOpaquePass DX11
  1,87 → 2,18 ms (+ Resolve 0,08). Frame-Zeit im Debug-Build CPU-gebunden (DX11 ≈ 32 ms in allen Stufen).
- Nicht geprüft: Schreiben von `user-settings.json` über ein echtes Script im exportierten Spiel (nur
  Lesen per Lauf, Schreiben per Test der Textfunktion), Sichtprüfung der neuen Auswahl im Editor, Release.

### 2026-11-19 – Schritt 7b (Claude, auf `main`, nicht committet)

- Bedienablauf mit dem Nutzer abgestimmt: Karte „Farbkorrektur“ im Inspector der Szene unter
  „Belichtung“; Look (nur bei AgX), Temperatur, Tönung, Farbfilter, Farbton, Kontrast,
  Sättigung, aufklappbar Lift/Gamma/Gain (Farbe + Wert), Zurücksetzen. Vergleich mit
  Unity/Unreal: Kurven, Channel Mixer, Split Toning bewusst weggelassen.
- `addons/forward/ForwardColorGrading.hpp` (Einstellungen, AgX-Looks, Umrechnung in
  Shader-Konstanten), Teil von `TonemapSettings`; Shader `shaders/passthrough.ps.hlsl`:
  Kontrast (log um 0,18) + eine 3×3-Matrix (Sättigung · Farbton · Filter · Weißabgleich LMS)
  vor dem Operator, AgX-Look (ASC CDL) im AgX-Pfad, Lift/Gamma/Gain danach (Gamma 2,2).
  Neutral = keine Flags = Bild unverändert.
- Szene: Objekt `colorGrading` im `environment`-Block (Editor-.json und .kscene), nur
  geschrieben, wenn nicht neutral; fehlt es, bleibt alles neutral. krom.h-Runtime liest es.
- Script-API `RenderSettings::grading` (eigene Struktur `script::ColorGrading`,
  Umwandlung in `addons/script/ScriptColorGrading.hpp`), krom.h `SetColorGrading`/
  `GetColorGrading`. Script-ABI 4 → 5 (Größe von `RenderSettings` geändert).
- Geprüft: `krom_tests` grün (2 neue Tests, 30 Prüfungen), Tonemap-Shader für DX11/Vulkan
  übersetzt; Room 2 Lit-Bild neutral/mit Grading auf DX11 und Vulkan mit
  `KROM_VALIDATION=1` ohne Meldung, DX11 ≈ Vulkan (0,45/255). Bild:
  `out/claude-grading-check/vergleich.png`.
- Nicht geprüft: Bedienung der Inspector-Karte von Hand; OpenGL (eingefroren, ohne Grading,
  nur gebaut).

### 2026-11-19 – Schritt 9, Post-Process-Volumes (Claude, Zweig `feature/post-process-volumes`)

- Umgesetzt wie unter „8. Bedienablauf abstimmen“. Nicht committet.
- Neues Add-on `addons/postprocess` (ohne GPU-Code): `PostProcessVolumeComponent` (Global, Gewicht,
  Priorität, Box wie die Reflection Probe: Ausdehnung, Versatz, Überblendweite nach innen; Häkchen-Bits
  für 17 Felder: feste Belichtung/Korrektur, Min/Max-EV, alle Felder von `ColorGradingSettings`
  (Lift/Gamma/Gain je Farbe + Wert zusammen), Intensität von Bloom, GTAO, SSR, Kontaktschatten).
  `PostProcessBlend`: Faktor 0 an der Wand → 1 ab Überblendweite (0 = harte Kante), Box dreht mit der
  Entity, Skalierung ignoriert; Volumes nach Priorität aufsteigend, bei gleicher Priorität die größere
  Box zuerst (kleinere gewinnt, wie bei den Probes), linear mit Gewicht × Faktor; AgX-Look wechselt ab
  > 50 %. Inaktive Entities zählen nicht.
- Hosts: Editor (`KromEditorApp`, jeden Frame vor dem Rendern) und krom.h-Runtime mischen über die
  Grundwerte (Szenenkarten bzw. Script-/Szenenwerte + Projekteinstellungen) und schreiben das Ergebnis
  in die Add-on-Einstellungen; gespeicherte Werte bleiben unberührt. Runtime: neuer
  `effectiveTonemapSettings` für den Forward-Pass, `tonemapSettings` bleibt Grundwert (Scripts, krom.h).
  Position: Editor-Kamera, im Play-Modus Hauptkamera (sonst erste Kamera); Runtime: Kamera der Ansicht.
- Bloom: `TonemapSettings::bloomIntensity` (neu, < 0 = Feature-Wert, nicht gespeichert), vom
  Tonemap-Pass jeden Frame gelesen.
- Editor: Hierarchie → Erstellen → „Post-Process-Volume“, „Komponente hinzufügen → Post Process“,
  Inspector-Bereich (Kopf, Box, Abschnitte Belichtung/Farbkorrektur/Effekte, je Zeile Häkchen, ausgegraut
  wenn aus, Undo über `BeginPendingComponentEdit`/`EditComponents`), Duplizieren, Komponenten-Zwischenablage.
  Viewport: blaue Box, innere Überblendgrenze gestrichelt, Flächengriffe. **Abweichung:** Die Probe hat im
  Viewport keine Griffe (nur Linien und Felder im Inspector); die Griffe kommen deshalb von der
  Kollisionsbox (`UpdateOBBHandles` arbeitet jetzt auf OBB oder Volume-Box, Strg = symmetrisch).
- Szene (Editor-.json und .kscene) und Prefabs: Komponente `PostProcessVolumeComponent`, `overrides` als
  Namensliste (fehlender Name = kein Override), alle Werte werden gespeichert (Häkchen aus behält sie),
  Farbkorrektur als `colorGrading`-Objekt wie in der Szene. Prefab-Record `postProcessVolume`, auch beim
  Abgleich von Instanzen.
- Script: `IScriptContext::SetPostProcessWeight/GetPostProcessWeight` (inline über `Get<T>`), krom.h
  `SetPostProcessWeight/GetPostProcessWeight`. Kein ABI-Sprung: keine per Wert übergebene Struktur geändert.
- Geprüft: Debug x64 (`-j6`, nur nötige Ziele, `out/claude-ppvolume-build.log`), `krom_tests` grün, neu
  „Post-process volumes“ (Faktor an Rand/innen/außen mit Versatz und Drehung, global, Gewicht, Priorität,
  kleinere Box, Häkchen aus, inaktiv, Look-Schwelle 0,5/0,51, Szenen-Rundweg, fehlende Schlüssel,
  Prefab-Rundweg; 57 Prüfungen) und ein Editor-Test (Erstellen, Override + Undo/Redo, Duplizieren,
  Inspector zeichnet). Room 2 (Kopie `out/claude-ppvolume-DemoProject` mit Volume: Halbausdehnung 3 m,
  Überblendung 2 m, −0,5 EV, Temperatur 60, Sättigung 0,4, Kontrast 1,15) mit `KROM_VALIDATION=1
  KROM_CAPTURE_LIT=1 KROM_CAPTURE_PPVOLUME=1 --gtao-motion` auf DX11 und Vulkan ohne Meldung; außen
  +0,5 EV (Szene), am Rand (Mitte des Bandes) 0,0 EV / Temperatur 30 / Sättigung 0,7, innen −0,5 / 60 /
  0,4; DX11/Vulkan mittlere Abweichung 0,42–0,47/255. Bild: `out/claude-ppvolume-check/vergleich.png`
  (oben DX11, unten Vulkan; außen / Rand / innen).
  OpenGL-Editor und -Ladetest bauen und starten; die Werte werden dort ebenso gemischt (Grading wirkt
  unter OpenGL nicht, eingefroren).
- Nicht geprüft: Bedienung im Editor von Hand (Griffe ziehen, Häkchen), Play-Modus mit Spielkamera,
  exportiertes Spiel, Script-DLL mit `SetPostProcessWeight`. Bekannt: Material-/Animationsvorschau
  benutzen dieselben Tonemap-Einstellungen und zeigen daher auch die Volume-Werte an der Kamera.
