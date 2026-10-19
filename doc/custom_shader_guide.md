# KROM Engine — Custom Shader Guide

Dieses Dokument beschreibt die Regeln für eigene Shader im Custom-Material-Template.

> Stand: am Engine-Code abgeglichen (2026-10-19). Die Engine baut den
> `PerMaterial`-Buffer **selbst** aus einer festen Feldliste und hängt eigene
> Parameter in der Reihenfolge der `.mat`-Datei an. Sie liest die Offsets
> (noch) nicht aus dem Shader. Deshalb muss die Deklaration im Shader exakt zu
> diesem Layout passen.

---

## Cbuffer-Slots

| Slot | Name | Beschreibung |
|------|------|-------------|
| `b0` | `PerFrame` | Kamera, Lichter, Zeit — **nicht verwenden** |
| `b1` | `PerObject` | World-Matrix, Entity-ID — **nicht verwenden** |
| `b2` | `PerMaterial` | Standardfelder + **alle** eigenen Parameter |
| `b3` | `PerPass` | Von der Engine pro Pass belegt — **nicht verwenden** |

Ein zweiter eigener Cbuffer wird nicht unterstützt. Felder in anderen Cbuffern
(z. B. an `b3`) befüllt die Engine nicht. Der Editor zeigt sie zwar an, die
Werte landen aber hinten im `PerMaterial`-Buffer.

---

## PerMaterial — Layout

Die ersten neun Felder sind **Pflicht** und müssen genau in dieser Reihenfolge
stehen, auch wenn der Shader sie nicht benutzt. Die Engine schreibt sie immer.
Eigene Parameter folgen **danach**.

```hlsl
cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;     //  0  PFLICHT
    float4 emissiveFactor;      // 16  PFLICHT
    float  metallicFactor;      // 32  PFLICHT (auch wenn ungenutzt)
    float  roughnessFactor;     // 36  PFLICHT
    float  occlusionStrength;   // 40  PFLICHT
    float  opacityFactor;       // 44  PFLICHT
    float  alphaCutoff;         // 48  PFLICHT
    int    materialFeatureMask; // 52  PFLICHT — Bit 1 (Wert 2) = Albedo-Textur vorhanden
    float  materialModel;       // 56  PFLICHT

    // --- Eigene Parameter ab Offset 60, Reihenfolge wie in der .mat ---
    float  meinParameter1;      // 60
    float4 meineFarbe;          // 64
    float  meinParameter2;      // 80

    // Optional und immer ganz am Ende: Normalen-Stärke
    // float normalScale;       // bzw. normalStrength
};
```

GLSL (OpenGL) deklariert denselben Block als `layout(std140) uniform PerMaterial`
mit `vec4` statt `float4`.

### Regeln für eigene Parameter

- **Reihenfolge = Reihenfolge in der `.mat`.** Der Editor erkennt neue Felder im
  Shader und hängt sie **hinten** an die Parameterliste der `.mat` an. Bestehende
  Einträge sortiert er nie um und entfernt sie nicht.
  - Neue Felder deshalb nur **am Ende** des `PerMaterial`-Blocks einfügen
    (vor einem optionalen `normalScale`).
  - Wer Felder umbenennt, löscht oder umsortiert, muss die `.mat` von Hand
    angleichen: veraltete Zeilen löschen, Reihenfolge an den Shader anpassen.
    Sonst verschieben sich alle folgenden Werte.
- **Typen:** `float`, `int`, `bool` und `float4`/`vec4` sind in allen Backends
  sicher. Die Engine packt nach HLSL-Regeln, OpenGL nutzt `std140`. Beide
  unterscheiden sich bei `float2`/`float3`: Diese nur an einer 16-Byte-Grenze
  beginnen lassen (z. B. direkt nach einem `float4`) oder gleich als `float4`
  deklarieren.
- **Keine Felder mit `_` am Anfang zwischen eigenen Parametern.** Der Editor
  ignoriert solche Namen, die Offsets aller folgenden Felder verschieben sich.
  Ein `_pad0` am Ende ist nur erlaubt, wenn danach keine eigenen Parameter
  kommen (wie in den Beispielen).
- **Werte in der `.mat`:** eine Zeile pro Parameter, z. B.
  `float meinParameter1 0.5`, `vec4 meineFarbe 1 0 0 1`, `int modus 2`,
  `bool aktiv true`, `texture rauschen Textures/noise.png`.

---

## Textur-Slots

| Slot | Verwendung |
|------|------------|
| `t0` | Albedo / Base Color (`materialFeatureMask & 2`) |
| `t1` | Normal Map |
| `t2` | ORM (Occlusion/Roughness/Metallic) |
| `t3` | Emissive (`materialFeatureMask & 2048`) |
| `t4`–`t12` | Von der Engine pro Pass belegt (Shadow-Map, IBL, Pass-Texturen, History, Bloom) — **nicht verwenden** |
| `t13`–`t15` | Eigene Texturen — frei |

Mehr als 16 Textur-Slots (`t0`–`t15`) gibt es nicht. Unter DX11 und Vulkan
übernimmt der Editor das Register eigener Texturen aus der Shader-Reflection.
Unter OpenGL ist das für eigene Texturen nicht verifiziert.

Ob eine Albedo-Textur vorhanden ist, kann über `materialFeatureMask` geprüft werden:

```hlsl
float4 texColor = (materialFeatureMask & 2)
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

Die Vulkan-Location entspricht dem Wert von `VertexSemantic`: Position 0,
Normal 1, Tangent 2, Bitangent 3, TexCoord0 4, TexCoord1 5, Color0 6.

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

Folgende Namen sind **reserviert** und werden vom Editor nicht als eigene
Parameter geführt:

```
baseColorFactor  emissiveFactor  metallicFactor  roughnessFactor
normalStrength   normalScale     occlusionStrength  opacityFactor
alphaCutoff      uvScale         uvOffset        materialFeatureMask
materialModel    albedo  baseColor  baseColorMap  normal  normalMap
orm  ormMap  emissive  emissiveMap  tAlbedo  tNormal  tORM  tEmissive
uAlbedo  uNormal  uORM  uEmissive  sLinear  sLinearWrap
```

Außerdem alle Namen, die mit `_` beginnen.

---

## Shader-Reflection im Editor

Der Material-Editor liest die Parameter aus dem Shader:

- Eigene Parameter in `PerMaterial` erscheinen als Slider, ColorPicker oder
  Textur-Picker im Editor.
- Werte werden in der `.mat`-Datei gespeichert und beim nächsten Start wiederhergestellt.
- Reservierte Namen (siehe oben) werden **nicht** als Custom-Parameter angezeigt.
- Die Standard-Regler **Metallic**, **Roughness**, **Occlusion** und **Normal
  Strength** erscheinen nur, wenn `PerMaterial` `metallicFactor`,
  `roughnessFactor`, `occlusionStrength` bzw. `normalScale`/`normalStrength`
  unter genau diesem Namen deklariert. Ihre Werte kommen dann im Shader an
  (Editor und Runtime). `animated_color` nutzt z. B. Metallic als
  Geschwindigkeit und Roughness als Wellenstärke.
  Die Erkennung liest den Shader-Quelltext und funktioniert daher für alle Backends.
- Wer die drei Pflichtfelder an Offset 32–40 nicht benutzt und keine Regler
  will, deklariert sie unter Namen mit `_` (z. B. `float _unused0;`). Die
  Position bleibt belegt, der Editor zeigt keine Regler.

---

## Beispiel-Shader

Fertige Beispiele im Ordner `assets/examples/`:

| Datei | Beschreibung |
|-------|-------------|
| `animated_color.vs/ps.hlsl` | Animierte Regenbogenfarben + UV-Wellen |
| `grayscale.vs/ps.hlsl` | Bild schwarz-weiß mit Intensität, Helligkeit, Kontrast |

Die zugehörigen `.mat`-Dateien können direkt geladen werden. Beide Beispiele
nutzen statt eigener Parameter die Standardfelder Metallic/Roughness/Occlusion.
