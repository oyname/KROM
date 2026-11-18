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
| 3 | Specular-AA | offen |
| 4 | Script-API: GTAO und SSR an/aus | erledigt (2026-11-18), GTAO mit Projekt-Häkchen |
| 5 | Post-AA: SMAA 1x, FXAA | offen |
| 6 | Auto-Exposure | offen |
| 7 | Qualitätsstufen | offen |
| 8 | Post-Process-Volume: Bedienablauf abstimmen | offen |
| 9 | Post-Process-Volume: Umsetzung | offen |
| 10 | Motion Vectors (optional, später entscheiden) | offen |
| 11 | TAA und zeitliche Akkumulation für GTAO/SSR (optional) | offen |
| 12 | Weiteres: Color Grading, Shading-Models, Flächenlichter (optional) | offen |

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

## Phase 3: Volumes

### 8. Bedienablauf abstimmen

- Nur Text, mit dem Nutzer abstimmen, bevor Code entsteht. Vorschlag:
  - Komponente „Post Process“ an einer Entity; global oder mit Trigger-Collider als Bereich,
  - Priorität und Überblendweite,
  - Werte direkt in der Komponente, keine Profil-Assets; jedes Feld mit Override-Häkchen,
  - Wiederverwendung über Prefabs.
- Volumes ändern nur **Werte**, keine Techniken (kein MSAA, kein Operator), damit beim
  Betreten keine Pipelines neu gebaut werden.

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

- Color Grading (LUT, Weißabgleich), Shading-Models (Clearcoat, Sheen), Flächenlichter
  (LTC), offene 1/π-Frage der Probes.

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
