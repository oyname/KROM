# Post-Mortem: Shadow Banding auf Kugeln

## Symptom
Beim Aktivieren des Schattens erschienen konzentrische Ringe (Shadow Acne) auf allen Kugeln in der `PbrShadowScene`.  
Beim Hineinzoomen wurden zusätzlich Shadow-Map-Texel als Gittermuster sichtbar.

---

## Ursachen

### 1. Hardware Slope-Scale Depth Bias deaktiviert (Hauptursache)
In `ShadowFeature.cpp` gab `GetShadowHardwareDepthBias()` seit einem früheren Refactoring `{0, 0}` zurück.  
Der Kommentar verwies auf Backend-Inkonsistenzen zwischen DX11/Vulkan/OpenGL als Begründung.

**Problem:**
- Der `slopeFactor` (`SlopeScaledDepthBias` / `glPolygonOffset factor` / `depthBiasSlopeFactor`) hat auf allen Backends identische Semantik:
  → Multiplikator auf den maximalen Depth-Slope pro Fragment.
- Nur `constantFactor` ist tatsächlich inkonsistent (DX11 + `D32_FLOAT`: INT ohne Wirkung).

Die pauschale Deaktivierung hat damit auch den funktionierenden Teil abgeschaltet.

**Folge:**
- Keine Korrektur für Polygon-Kanten in der Shadow Map
- Benachbarte Faces haben unterschiedliche Tiefen
- Fehler wächst mit Einfallswinkel

Der im Shader gesetzte:
```cpp
shadowBias = 0.00015f;
```
war zu klein, um diesen Effekt zu kompensieren, besonders bei mittleren `NoL`-Werten (0.4–0.7) auf der Kugeloberfläche.

---

### 2. Zu geringe Tessellation (Verstärker)
Die Kugel in `PbrShadowScene` hatte:
```cpp
kRings = 12;
kSegs  = 16;
```

→ 192 Dreiecke  
→ ~15° pro Face  
→ Sag-Fehler ≈ 0.0043 Welteinheiten

Zum Vergleich:
`LightingValidationScene` verwendete bereits 36 × 48.

---

### 3. Gittermuster (sekundär)
Das reguläre 3×3 PCF-Kernel erzeugt kohärente Abtastpositionen auf dem Shadow-Map-Texelgitter.

→ Wird sichtbar, wenn Shadow-Map-Texel groß genug projizieren  
→ Ergebnis: sichtbares Grid/Pattern

---

## Was nicht funktioniert hat

### RPDB (Receiver Plane Depth Bias) per ddx/ddy
- Instabil an Sphere-Silhouetten und Grazing-Winkeln
- Problem:
  ddx(depth) / ddx(uv.x) divergiert an Dreiecksgrenzen  
- Ergebnis: großflächige falsche Shadow-Patches

---

### NoL-basierter Shader-Bias
- Nur grobe Approximation
- Trifft nicht die tatsächlich benötigte Bias-Kurve über alle Winkel

---

### Tessellation allein
- Reduziert den Fehler
- Beseitigt ihn nicht

→ Ohne Slope Bias bleibt Shadow Acne bestehen

---

## Lösung

| Datei                    | Änderung |
|-------------------------|----------|
| `ShadowFeature.cpp`     | `slopeFactor = 3.0f`, `constantFactor = 0.0f` |
| `PbrShadowScene.cpp`    | `kRings: 12 → 36`, `kSegs: 16 → 48` |
| `pbr_lit.ps.hlsl`       | Poisson-Disk-Kernel (8 Taps) statt 3×3 |
| `pbr_lit.opengl.fs.glsl`| Gleiche Änderung |

---

## Lernpunkte

- Backend-Unterschiede präzise analysieren, nicht pauschal abstrahieren
- Funktionierende Teile (hier: `slopeFactor`) nicht mit deaktivieren
- Shadow Bias ist ein kombiniertes Problem aus:
  - Geometrie (Tessellation)
  - Rasterisierung (Slope Bias)
  - Sampling (PCF Kernel)

→ Fixes müssen alle Ebenen berücksichtigen
