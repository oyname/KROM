# KROM Engine — Custom Shader Guide

Dieses Dokument beschreibt die Regeln für eigene Shader im Custom-Material-Template.

> Stand: am Engine-Code abgeglichen (2026-10-19). Die Engine liest beim Anlegen
> eines Custom-Materials die Feld-Offsets des `PerMaterial`-Blocks aus dem
> **kompilierten Shader** und schreibt jeden Wert an seinen Offset. Reihenfolge
> und Auswahl der Felder bestimmt der Shader, nicht die `.mat`-Datei.

---

## Cbuffer-Slots

| Slot | Name | Beschreibung |
|------|------|-------------|
| `b0` | `PerFrame` | Kamera, Lichter, Zeit — **nicht verwenden** |
| `b1` | `PerObject` | World-Matrix, Entity-ID — **nicht verwenden** |
| `b2` | `PerMaterial` | Standardfelder + **alle** eigenen Parameter |
| `b3` | `PerPass` | Von der Engine pro Pass belegt — **nicht verwenden** |

Ein zweiter eigener Cbuffer wird nicht unterstützt. Felder in anderen Cbuffern
(z. B. an `b3`) befüllt die Engine nicht.

---

## PerMaterial — Layout

Der Block muss `PerMaterial` heißen und an `b2` liegen. Darin gilt:

- **Reihenfolge frei.** Eigene Felder dürfen auch zwischen den Standardfeldern
  stehen, Standardfelder dürfen umsortiert werden.
- **Standardfelder optional.** Was der Shader nicht deklariert, schreibt die
  Engine nicht. Kein Platzhalter für ungenutzte Felder nötig.
- **Zuordnung über den Namen.** Ein Feld bekommt den Wert des gleichnamigen
  Standardfelds bzw. des gleichnamigen Parameters aus der `.mat`. Felder ohne
  passenden Wert (z. B. `_pad0`) bleiben 0.

Standardfelder, die die Engine kennt und befüllt:

| Feld | Typ | Wert |
|------|-----|------|
| `baseColorFactor` | `float4` | Base Color aus dem Material-Editor |
| `emissiveFactor` | `float4` | Emissive |
| `metallicFactor` | `float` | Metallic-Regler |
| `roughnessFactor` | `float` | Roughness-Regler |
| `occlusionStrength` | `float` | Occlusion-Regler |
| `opacityFactor` | `float` | Alpha der Base Color |
| `alphaCutoff` | `float` | Alpha-Cutoff |
| `materialFeatureMask` | `int` | Bit 1 (Wert 2) = Albedo-Textur, Bit 11 (Wert 2048) = Emissive-Textur |
| `materialModel` | `float` | immer 0 |
| `normalScale` bzw. `normalStrength` | `float` | Normal-Strength-Regler |

Beispiel mit eigenem Feld zwischen Standardfeldern und ohne Occlusion:

```hlsl
cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  waveSpeed;           // eigenes Feld, Wert aus der .mat
    float  metallicFactor;
    float  roughnessFactor;
    float  opacityFactor;
    int    materialFeatureMask;
    float4 tint;                // eigenes Feld
};
```

GLSL (OpenGL) deklariert denselben Block als `layout(std140) uniform PerMaterial`
mit `vec4` statt `float4`. HLSL- und GLSL-Variante eines Shaders werden je für
sich ausgewertet und dürfen unterschiedlich angeordnet sein; die Feldnamen
müssen aber übereinstimmen, sonst fehlen unter einem Backend die Werte.

### Wie die Offsets ermittelt werden

| Backend | Quelle |
|---------|--------|
| DX11 | `D3DReflect` auf dem kompilierten DXBC |
| Vulkan | Member-Offsets im kompilierten SPIR-V (`-fvk-use-dx-layout`, also HLSL-Packing) |
| OpenGL | `std140`-Regeln, angewendet auf die Deklaration im GLSL-Quelltext |

Unter OpenGL wertet die Engine die Deklaration selbst aus. Unterstützt sind
`float`, `int`, `uint`, `bool`, `vec2`–`vec4` (auch `ivec`/`uvec`/`bvec`),
`mat2`–`mat4` und Arrays mit Zahlenlänge (`float w[4]`). Präprozessorzeilen im
Block werden ignoriert, also keine Felder per `#ifdef` ein- oder ausblenden.

**Fallback:** Lässt sich der Block nicht auswerten (Shader kompiliert nicht,
nicht unterstützter Typ wie ein `struct` im GLSL-Block, Backend ohne
Reflection), schreibt der Editor bzw. die Runtime eine Warnung ins Log und
packt die Werte wie früher in fester Reihenfolge: die neun Felder
`baseColorFactor` … `materialModel` aus der Tabelle oben (ohne `normalScale`)
ab Offset 0, eigene Parameter danach in `.mat`-Reihenfolge ab Offset 60. Ein
Shader in genau dieser Anordnung funktioniert in beiden Fällen; die Beispiele
in `assets/examples/` sind so aufgebaut.

### Regeln für eigene Parameter

- **Werte in der `.mat`:** eine Zeile pro Parameter, z. B.
  `float waveSpeed 0.5`, `vec4 tint 1 0 0 1`, `int modus 2`,
  `bool aktiv true`, `texture rauschen Textures/noise.png`.
- **Umbenennen:** Der Wert hängt am Namen. Nach dem Umbenennen eines Felds im
  Shader die Zeile in der `.mat` mit umbenennen, sonst bleibt das Feld 0.
- **Typen:** `float`, `int`, `bool`, `float2`–`float4` (`vec2`–`vec4`). Der Typ
  in der `.mat` sollte zum Shader passen. Ist er größer als das Feld im
  Shader, wird auf die Feldgröße gekürzt, Nachbarfelder werden nie
  überschrieben.
- **Namen mit `_`** (z. B. `_pad0`) zeigt der Editor nicht an und die Engine
  befüllt sie nicht. Sie verschieben nichts.

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

Mehr als 16 Textur-Slots (`t0`–`t15`) gibt es nicht. **Bekannte Lücke:** Das
Register eigener Texturen liest die Engine derzeit nicht aus dem Shader (die
vorgesehene Textur-Reflection in Editor und Runtime ist nicht aktiv). Eigene
Texturen werden der Reihe nach ab `t2` gebunden und kollidieren dann mit
ORM/Emissive. Eigene Texturen sind deshalb noch nicht verlässlich nutzbar.

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
- Wer Metallic/Roughness/Occlusion nicht braucht, lässt die Felder einfach
  weg; dann gibt es auch keine Regler.

---

## Beispiel-Shader

Fertige Beispiele im Ordner `assets/examples/`:

| Datei | Beschreibung |
|-------|-------------|
| `animated_color.vs/ps.hlsl` | Animierte Regenbogenfarben + UV-Wellen |
| `grayscale.vs/ps.hlsl` | Bild schwarz-weiß mit Intensität, Helligkeit, Kontrast |

Die zugehörigen `.mat`-Dateien können direkt geladen werden. Beide Beispiele
nutzen statt eigener Parameter die Standardfelder Metallic/Roughness/Occlusion.
