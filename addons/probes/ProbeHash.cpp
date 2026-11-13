#include "addons/probes/ProbeBake.hpp"

#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/RenderLayers.hpp"

#include <cmath>
#include <cstring>
#include <string_view>

namespace engine::addons::probes {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

// Editor gizmos and previews are not in the capture (see ProbeCapture) and
// must not mark probes as outdated when they move.
constexpr uint32_t kEditorOnlyLayers = renderer::LAYER_EDITOR_GIZMO | renderer::LAYER_EDITOR_GIZMO_DEPTH |
                                       renderer::LAYER_EDITOR_PREVIEW_MASK | renderer::LAYER_UI;

struct Hasher
{
    uint64_t h = kFnvOffset;

    void Bytes(const void* data, size_t size) noexcept
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0u; i < size; ++i)
        {
            h ^= p[i];
            h *= kFnvPrime;
        }
    }
    void U32(uint32_t v) noexcept { Bytes(&v, sizeof(v)); }
    // Rounded to a millimetre / a thousandth, so a save/load round trip of the
    // scene does not change the hash.
    static int64_t Quantize(float v) noexcept
    {
        return static_cast<int64_t>(std::llround(static_cast<double>(v) * 1000.0));
    }
    void F(float v) noexcept
    {
        const int64_t q = Quantize(v);
        Bytes(&q, sizeof(q));
    }
    void V3(const math::Vec3& v) noexcept { F(v.x); F(v.y); F(v.z); }
    void Str(std::string_view s) noexcept { U32(static_cast<uint32_t>(s.size())); Bytes(s.data(), s.size()); }
    void Transform(const WorldTransformComponent& xf) noexcept
    {
        V3(xf.position);
        F(xf.rotation.x); F(xf.rotation.y); F(xf.rotation.z); F(xf.rotation.w);
        V3(xf.scale);
    }
};

} // namespace

uint64_t ComputeProbeInputHash(const ecs::World& world, EntityID probeEntity)
{
    Hasher own;
    if (const auto* probe = world.Get<ReflectionProbeComponent>(probeEntity))
    {
        own.V3(probe->extents);
        own.U32(probe->resolution);
        const auto& offset = probe->boxOffset;
        // Preserve the stored hash for centered probes baked before offsets were supported.
        if (Hasher::Quantize(offset.x) || Hasher::Quantize(offset.y) || Hasher::Quantize(offset.z))
            own.V3(offset);
    }
    if (const auto* xf = world.Get<WorldTransformComponent>(probeEntity))
        own.Transform(*xf);

    // Order-independent sum of per-entity hashes (entity ids and iteration
    // order change between sessions).
    uint64_t scene = 0u;
    world.View<MeshComponent, WorldTransformComponent>(
        [&](EntityID entity, const MeshComponent& mesh, const WorldTransformComponent& xf)
        {
            if (!mesh.visible || (mesh.layerMask & kEditorOnlyLayers) != 0u)
                return;
            Hasher e;
            e.Str("mesh");
            e.Str(mesh.meshAssetPath);
            e.Transform(xf);
            e.U32(mesh.castShadows ? 1u : 0u);
            if (const auto* material = world.Get<MaterialComponent>(entity))
            {
                e.Str(material->materialAssetPath);
                for (const MaterialComponent::SlotOverride& slot : material->slotOverrides)
                {
                    e.U32(slot.submeshIndex);
                    e.Str(slot.materialAssetPath);
                }
            }
            scene += e.h;
        });
    world.View<LightComponent, WorldTransformComponent>(
        [&](EntityID, const LightComponent& light, const WorldTransformComponent& xf)
        {
            if ((light.layerMask & renderer::LAYER_EDITOR_PREVIEW_MASK) != 0u &&
                (light.layerMask & ~renderer::LAYER_EDITOR_PREVIEW_MASK) == 0u)
                return;   // preview-only light rig
            Hasher e;
            e.Str("light");
            e.U32(static_cast<uint32_t>(light.type));
            e.V3(light.color);
            e.F(light.intensity);
            e.F(light.range);
            e.F(light.spotInnerDeg);
            e.F(light.spotOuterDeg);
            e.U32(light.castShadows ? 1u : 0u);
            e.Transform(xf);
            scene += e.h;
        });

    Hasher total;
    total.Bytes(&own.h, sizeof(own.h));
    total.Bytes(&scene, sizeof(scene));
    total.U32(kProbeBakeVersion);
    return total.h;
}

} // namespace engine::addons::probes
