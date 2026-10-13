# KROM Engine — Custom Shader Guide

Dieses Dokument beschreibt die Regeln für eigene Shader im Custom-Material-Template.

---

## Cbuffer-Slots (fest belegt)

| Slot | Name | Beschreibung |
|------|------|-------------|
| `b0` | `PerFrame` | Kamera, Lichter, Zeit — **nicht verwenden** |
| `b1` | `PerObject` | World-Matrix, Entity-ID — **nicht verwenden** |
| `b2` | `PerMaterial` | Eigene Parameter hier eintragen |
| `b3` | `UserParams` | Optionaler zweiter eigener Cbuffer |

---

## PerMaterial — Pflichtstruktur

Die ersten beiden Felder und die letzten vier Felder sind **Pflicht** und müssen in dieser
Reihenfolge stehen. Dazwischen können eigene Parameter frei eingefügt werden.

```hlsl
cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;     // PFLICHT — immer erstes Feld
    float4 emissiveFactor;      // PFLICHT — immer zweites Feld

    // --- Eigene Parameter hier ---
    float  meinParameter1;
    float  meinParameter2;
    float3 meinefarbe;
    // -----------------------------

    float  opacityFactor;       // PFLICHT
    float  alphaCutoff;         // PFLICHT
    int    materialFeatureMask; // PFLICHT — Bit 0 = Albedo-Textur vorhanden
    float  materialModel;       // PFLICHT
    float  _pad0;               // PFLICHT — Padding
};
```

> **Wichtig:** Reihenfolge und Pflichtfelder müssen exakt stimmen.
> Das Byte-Layout wird direkt auf den GPU-Buffer gemappt.

---

## Textur-Slots

| Slot | Verwendung |
|------|------------|
| `t0` | Albedo / Base Color |
| `t1` | Normal Map |
| `t2` | ORM (Occlusion/Roughness/Metallic) |
| `t3` | Emissive |
| `t4` | Occlusion |
| `t5+` | Eigene Texturen — frei verwendbar |

Ob eine Textur vorhanden ist, kann über `materialFeatureMask` geprüft werden:

```hlsl
float4 texColor = (materialFeatureMask & 1)
    ? tAlbedo.Sample(sLinear, uv)
    : float4(1, 1, 1, 1);
```

---

## Vertex-Layout (fest)

```hlsl
VK_LOC(0) float3 position : POSITION;
VK_LOC(1) float3 normal   : NORMAL;
VK_LOC(4) float2 texCoord : TEXCOORD0;
```

---

## Einstiegspunkt

Der Einstiegspunkt muss immer `main` heißen:

```hlsl
float4 main(PSInput IN) : SV_TARGET { ... }  // korrekt
float4 myShader(...)                { ... }  // falsch
```

---

## PerObject-Binding (Vulkan vs. DX11)

Nicht `#include "per_object_binding.hlsl"` verwenden — direkt inlinen:

```hlsl
#ifdef KROM_VULKAN_PUSH_PER_OBJECT
// Vulkan: Push Constants
struct KromPerObjectPushConstantsData {
    float4 worldRow0; float4 worldRow1; float4 worldRow2;
    float4 worldInvTRow0; float4 worldInvTRow1; float4 worldInvTRow2;
    float4 entityId;
};
[[vk::push_constant]] KromPerObjectPushConstantsData g_kromPerObjectPush;

float4 KromObjectPositionWS(float3 p) {
    float4 lp = float4(p, 1.0f);
    return float4(dot(g_kromPerObjectPush.worldRow0, lp),
                  dot(g_kromPerObjectPush.worldRow1, lp),
                  dot(g_kromPerObjectPush.worldRow2, lp), 1.0f);
}
float3 KromObjectNormalWS(float3 n) {
    float4 ln = float4(n, 0.0f);
    return float3(dot(g_kromPerObjectPush.worldInvTRow0, ln),
                  dot(g_kromPerObjectPush.worldInvTRow1, ln),
                  dot(g_kromPerObjectPush.worldInvTRow2, ln));
}
#else
// DX11 / OpenGL: Constant Buffer
cbuffer PerObject : register(b1) {
    float4x4 worldMatrix;
    float4x4 worldMatrixInvT;
    float4   entityId;
};
float4 KromObjectPositionWS(float3 p) { return mul(worldMatrix, float4(p, 1.0f)); }
float3 KromObjectNormalWS(float3 n)   { return mul((float3x3)worldMatrixInvT, n); }
#endif
```

---

## Namensregeln für Parameter

Folgende Namen sind **reserviert** (Standard-Parameter) und werden vom Editor
gefiltert — nicht als eigene Parameter verwenden:

```
baseColorFactor  emissiveFactor  metallicFactor  roughnessFactor
normalStrength   normalScale     occlusionStrength  opacityFactor
alphaCutoff      uvScale         uvOffset        materialFeatureMask
materialModel    albedo          normal          orm   emissive
```

---

## Shader-Reflection im Editor

Der Material-Editor liest automatisch alle Parameter aus dem Shader:

- Eigene Parameter in `PerMaterial` (b2) oder `UserParams` (b3) erscheinen
  als Slider, ColorPicker oder Textur-Picker im Editor
- Werte werden in der `.mat`-Datei gespeichert und beim nächsten Start wiederhergestellt
- Reservierte Namen (siehe oben) werden **nicht** als Custom-Parameter angezeigt

---

## Beispiel-Shader

Fertige Beispiele im Ordner `assets/examples/`:

| Datei | Beschreibung |
|-------|-------------|
| `animated_color.vs/ps.hlsl` | Animierte Regenbogenfarben + UV-Wellen |
| `grayscale.vs/ps.hlsl` | Bild schwarz-weiß mit Intensität, Helligkeit, Kontrast |

Die zugehörigen `.mat`-Dateien können direkt geladen werden.
