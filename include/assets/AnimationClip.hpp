#pragma once
// =============================================================================
// KROM Engine - assets/AnimationClip.hpp
// CPU-seitige Animations-Keyframe-Daten — format-agnostisch.
//
// Design-Prinzipien:
//   • Jede animierte Eigenschaft ist ein eigener Channel (T/R/S getrennt).
//     Das erlaubt unterschiedliche Keyframe-Abstände pro Eigenschaft und
//     ist die direkte Abbildung aller gängigen Formate (glTF, FBX, USD).
//   • Kein "merged keyframe" — kein Informationsverlust bei CUBICSPLINE.
//   • targetIndex + targetName ermöglichen sowohl Node-Animation (ECS-Entity
//     per NameComponent) als auch Skelett-Animation (Bone-Index-Lookup).
//
// CubicSpline-Wertelayout pro Keyframe i (N Keyframes gesamt):
//   vec3Values[3*i+0] = in-tangent     (skaliert mit Zeitdelta durch Sampler)
//   vec3Values[3*i+1] = Wert
//   vec3Values[3*i+2] = out-tangent    (skaliert mit Zeitdelta durch Sampler)
//   quatValues analog, je 3 Einträge pro Keyframe.
// =============================================================================
#include "assets/AssetBase.hpp"
#include "core/Math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::assets {

// -----------------------------------------------------------------------------
// Interpolationsmodus
// -----------------------------------------------------------------------------
enum class AnimationInterpolation : uint8_t
{
    Linear,       // Vec3: LERP  |  Quat: SLERP
    Step,         // Nächster linker Keyframe, keine Interpolation
    CubicSpline,  // Kubische Hermite-Spline mit In/Out-Tangenten (glTF CUBICSPLINE)
};

// -----------------------------------------------------------------------------
// Animierte Eigenschaft — ein Channel trägt genau eine davon
// -----------------------------------------------------------------------------
enum class AnimationTargetProperty : uint8_t
{
    Translation,
    Rotation,
    Scale,
};

// -----------------------------------------------------------------------------
// AnimationChannel
//
// Speicherlayout der Wert-Arrays:
//   Linear / Step:   values.size() == times.size()
//   CubicSpline:     values.size() == times.size() * 3
// Nur eines der beiden Arrays (vec3Values / quatValues) ist befüllt,
// je nach `property`.
// -----------------------------------------------------------------------------
struct AnimationChannel
{
    std::string              targetName;        // NameComponent-Lookup (Node-Animation)
    int32_t                  targetIndex = -1;  // Node-Index oder Bone-Index (vom Importer)

    AnimationTargetProperty  property      = AnimationTargetProperty::Translation;
    AnimationInterpolation   interpolation = AnimationInterpolation::Linear;

    std::vector<float>       times;       // Keyframe-Zeitpunkte (aufsteigend, Sekunden)
    std::vector<math::Vec3>  vec3Values;  // Translation oder Scale
    std::vector<math::Quat>  quatValues;  // Rotation
};

// -----------------------------------------------------------------------------
// AnimationClip
// -----------------------------------------------------------------------------
struct AnimationClip : AssetBase
{
    std::string                   name;
    float                         duration = 0.f;   // Gesamtlänge in Sekunden
    std::vector<AnimationChannel> channels;
    bool                          looping  = false;
};

} // namespace engine::assets
