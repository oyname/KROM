#pragma once

#include "PbrMasterMaterial.hpp"
#include "PbrSlot.hpp"
#include "PbrSlotTable.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include <string>
#include <unordered_map>

namespace engine::renderer::pbr {

// ---------------------------------------------------------------------------
// PbrInstanceBuilder
//
// Fluent API for configuring a PBR material instance.
// Obtained via PbrMasterMaterial::CreateInstance(name).
//
// Each slot accepts either a constant value or a texture source.
// Build() derives the permutation flags automatically and returns a
// per-object MaterialHandle with its own parameter copy.
//
// Usage:
//   MaterialHandle steel = master.CreateInstance("Steel")
//       .BaseColor(albedoTex)
//       .Roughness(ormTex, MaterialChannel::G)
//       .Metallic(ormTex, MaterialChannel::B)
//       .Occlusion(ormTex, MaterialChannel::R)
//       .Normal(normalTex)
//       .Build();
// ---------------------------------------------------------------------------
class PbrInstanceBuilder
{
public:
    PbrInstanceBuilder(PbrMasterMaterial& master, std::string name) noexcept
        : m_master(master), m_name(std::move(name))
    {}

    // ── Base Color ────────────────────────────────────────────────────────────

    PbrInstanceBuilder& BaseColor(TextureHandle tex) noexcept
    {
        m_slots["baseColor"] = PbrSlotValue::FromTexture(tex);
        return *this;
    }

    PbrInstanceBuilder& BaseColor(math::Vec4 color) noexcept
    {
        m_slots["baseColor"] = PbrSlotValue::FromConstant(color);
        return *this;
    }

    PbrInstanceBuilder& BaseColor(float r, float g, float b, float a = 1.f) noexcept
    {
        return BaseColor({r, g, b, a});
    }

    // ── Normal ────────────────────────────────────────────────────────────────

    PbrInstanceBuilder& Normal(TextureHandle tex, float strength = 1.f) noexcept
    {
        m_slots["normal"] = PbrSlotValue::FromTexture(tex, MaterialChannel::R, strength);
        return *this;
    }

    PbrInstanceBuilder& Normal(bool enabled) noexcept
    {
        if (!enabled) m_slots.erase("normal");
        return *this;
    }

    // ── Roughness ─────────────────────────────────────────────────────────────

    PbrInstanceBuilder& Roughness(TextureHandle tex,
                                  MaterialChannel channel = MaterialChannel::G,
                                  float scale = 1.f,
                                  float bias  = 0.f) noexcept
    {
        m_slots["roughness"] = PbrSlotValue::FromTexture(tex, channel, scale, bias);
        return *this;
    }

    PbrInstanceBuilder& Roughness(float value) noexcept
    {
        m_slots["roughness"] = PbrSlotValue::FromConstant(value);
        return *this;
    }

    // ── Metallic ──────────────────────────────────────────────────────────────

    PbrInstanceBuilder& Metallic(TextureHandle tex,
                                 MaterialChannel channel = MaterialChannel::B,
                                 float scale = 1.f,
                                 float bias  = 0.f) noexcept
    {
        m_slots["metallic"] = PbrSlotValue::FromTexture(tex, channel, scale, bias);
        return *this;
    }

    PbrInstanceBuilder& Metallic(float value) noexcept
    {
        m_slots["metallic"] = PbrSlotValue::FromConstant(value);
        return *this;
    }

    // ── Occlusion ─────────────────────────────────────────────────────────────

    PbrInstanceBuilder& Occlusion(TextureHandle tex,
                                  MaterialChannel channel = MaterialChannel::R,
                                  float strength = 1.f,
                                  float bias     = 0.f) noexcept
    {
        m_slots["occlusion"] = PbrSlotValue::FromTexture(tex, channel, strength, bias);
        return *this;
    }

    PbrInstanceBuilder& Occlusion(float value) noexcept
    {
        m_slots["occlusion"] = PbrSlotValue::FromConstant(value);
        return *this;
    }

    // ── Emissive ──────────────────────────────────────────────────────────────

    PbrInstanceBuilder& Emissive(TextureHandle tex) noexcept
    {
        m_slots["emissive"] = PbrSlotValue::FromTexture(tex);
        return *this;
    }

    PbrInstanceBuilder& Emissive(math::Vec4 color) noexcept
    {
        m_slots["emissive"] = PbrSlotValue::FromConstant(color);
        return *this;
    }

    PbrInstanceBuilder& Emissive(float r, float g, float b) noexcept
    {
        return Emissive({r, g, b, 0.f});
    }

    // ── Opacity ───────────────────────────────────────────────────────────────

    PbrInstanceBuilder& Opacity(float value) noexcept
    {
        m_slots["opacity"] = PbrSlotValue::FromConstant(value);
        return *this;
    }

    // ── Render policy ─────────────────────────────────────────────────────────

    PbrInstanceBuilder& DoubleSided(bool v = true) noexcept
    {
        m_doubleSided = v;
        return *this;
    }

    PbrInstanceBuilder& AlphaTest(float cutoff = 0.5f) noexcept
    {
        m_alphaTest  = true;
        m_alphaCutoff = cutoff;
        return *this;
    }

    PbrInstanceBuilder& IBL(bool enabled = true) noexcept
    {
        m_ibl = enabled;
        return *this;
    }

    // ── Build ─────────────────────────────────────────────────────────────────

    [[nodiscard]] MaterialHandle Build()
    {
        const uint64_t flags = ComputeFlags();

        MaterialHandle base = m_master.GetOrRegisterPermutation(flags);
        if (!base.IsValid())
            return MaterialHandle{};

        MaterialSystem* ms = m_master.GetMaterialSystem();
        MaterialHandle inst = ms->CreateInstance(base, m_name);
        if (!inst.IsValid())
            return MaterialHandle{};

        for (auto& [id, value] : m_slots)
            m_master.SetSlotValue(inst, id, value);

        if (m_alphaTest)
            ms->SetFloat(inst, "alphaCutoff", m_alphaCutoff);

        return inst;
    }

private:
    [[nodiscard]] uint64_t ComputeFlags() const noexcept
    {
        ShaderVariantFlag flags = ShaderVariantFlag::PBRMetalRough;

        for (const detail::SlotDef& s : detail::kSlots)
        {
            if (s.variantFlag == ShaderVariantFlag::None) continue;
            auto it = m_slots.find(s.id);
            if (it != m_slots.end() && it->second.HasTexture())
                flags = flags | s.variantFlag;
        }

        if (m_ibl)         flags = flags | ShaderVariantFlag::IBLMap;
        if (m_doubleSided) flags = flags | ShaderVariantFlag::DoubleSided;
        if (m_alphaTest)   flags = flags | ShaderVariantFlag::AlphaTest;

        return static_cast<uint64_t>(flags);
    }

    PbrMasterMaterial&                        m_master;
    std::string                               m_name;
    std::unordered_map<std::string, PbrSlotValue> m_slots;
    bool  m_doubleSided  = false;
    bool  m_alphaTest    = false;
    float m_alphaCutoff  = 0.5f;
    bool  m_ibl          = true;
};

} // namespace engine::renderer::pbr
