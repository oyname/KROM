#pragma once

// PipelineStateBuilder — prioritätsbasierter State-Merge für PipelineDesc.
//
// Jeder State-Slot hat eine Priorität. Höhere Priorität gewinnt immer.
// Provenienz-Tracking im Debug-Build: Wer hat diesen State gesetzt?
//
// Prioritätshierarchie (aufsteigend):
//   0  Default        — Engine-Standard, gilt wenn niemand sonst schreibt
//   1  DomainDefault  — Domain-spezifischer Vorschlag (z.B. Mesh = DepthTest ON)
//   2  MaterialPolicy — renderPolicy des Materials (Authoring-Ebene)
//   3  PassRequirement— Pass empfiehlt State, aber Material darf überschreiben
//   4  PassLock       — Pass erzwingt State, Material kann nicht überschreiben

#include "renderer/MaterialTypes.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RendererTypes.hpp"
#include <cstdint>

namespace engine::renderer {

enum class StatePriority : uint8_t
{
    Default         = 0,
    DomainDefault   = 1,
    MaterialPolicy  = 2,
    PassRequirement = 3,
    PassLock        = 4,
};

template<typename T>
struct PrioritizedState
{
    T             value{};
    StatePriority priority = StatePriority::Default;
#ifndef NDEBUG
    const char*   source = "default";
#endif

    // Setzt den Wert nur, wenn die neue Priorität >= aktuelle Priorität ist.
    // Bei Gleichstand gewinnt der neue Wert (letzter Schreiber mit gleicher Prio).
    void Set(T newValue, StatePriority newPriority, [[maybe_unused]] const char* sourceName = nullptr) noexcept
    {
        if (static_cast<uint8_t>(newPriority) >= static_cast<uint8_t>(priority))
        {
            value    = newValue;
            priority = newPriority;
#ifndef NDEBUG
            if (sourceName) source = sourceName;
#endif
        }
    }

    [[nodiscard]] const T& Get() const noexcept { return value; }
    [[nodiscard]] bool IsLockedBy(StatePriority p) const noexcept
    {
        return static_cast<uint8_t>(priority) >= static_cast<uint8_t>(p);
    }
};

// Vollständiger Pipeline-State, aufgebaut via PrioritizedState<T>.
// Am Ende wird ToDesc() aufgerufen, um die fertige PipelineDesc zu erhalten.
struct PipelineStateBuilder
{
    // Sichere Strukturdefaults — identisch mit ApplyEngineDefaults().
    // Verhindert dass ein vergessener Set()-Aufruf DepthFunc::Never oder
    // BlendFactor::Zero als stille Defaults in den PipelineDesc schreibt.
    PrioritizedState<bool>        depthEnable { true,             StatePriority::Default };
    PrioritizedState<bool>        depthWrite  { true,             StatePriority::Default };
    PrioritizedState<DepthFunc>   depthFunc   { DepthFunc::Less,  StatePriority::Default };
    PrioritizedState<CullMode>    cullMode    { CullMode::Back,   StatePriority::Default };
    PrioritizedState<bool>        blendEnable { false,            StatePriority::Default };
    PrioritizedState<BlendFactor> srcBlend    { BlendFactor::One, StatePriority::Default };
    PrioritizedState<BlendFactor> dstBlend    { BlendFactor::Zero,StatePriority::Default };

    // Setzt Engine-Defaults (Priorität 0). Immer zuerst aufrufen.
    PipelineStateBuilder& ApplyEngineDefaults() noexcept
    {
        depthEnable.Set(true,          StatePriority::Default, "engine");
        depthWrite.Set(true,           StatePriority::Default, "engine");
        depthFunc.Set(DepthFunc::Less, StatePriority::Default, "engine");
        cullMode.Set(CullMode::Back,   StatePriority::Default, "engine");
        blendEnable.Set(false,         StatePriority::Default, "engine");
        srcBlend.Set(BlendFactor::One, StatePriority::Default, "engine");
        dstBlend.Set(BlendFactor::Zero,StatePriority::Default, "engine");
        return *this;
    }

    // Wendet MaterialRenderPolicy an (Priorität MaterialPolicy).
    PipelineStateBuilder& ApplyMaterialPolicy(const MaterialRenderPolicy& policy) noexcept
    {
        depthEnable.Set(policy.depth.test,  StatePriority::MaterialPolicy, "material.renderPolicy");
        depthWrite.Set(policy.depth.write,  StatePriority::MaterialPolicy, "material.renderPolicy");

        DepthFunc df = DepthFunc::Less;
        switch (policy.depth.compare)
        {
        case MaterialDepthCompare::Less:      df = DepthFunc::Less;      break;
        case MaterialDepthCompare::LessEqual: df = DepthFunc::LessEqual; break;
        case MaterialDepthCompare::Equal:     df = DepthFunc::Equal;     break;
        case MaterialDepthCompare::Always:    df = DepthFunc::Always;    break;
        }
        depthFunc.Set(df, StatePriority::MaterialPolicy, "material.renderPolicy");

        CullMode cm = CullMode::Back;
        if (policy.cull.doubleSided)
            cm = CullMode::None;
        else switch (policy.cull.mode)
        {
        case MaterialCullMode::Back:  cm = CullMode::Back;  break;
        case MaterialCullMode::Front: cm = CullMode::Front; break;
        case MaterialCullMode::None:  cm = CullMode::None;  break;
        }
        cullMode.Set(cm, StatePriority::MaterialPolicy, "material.renderPolicy");

        const bool blend = policy.blend.mode != MaterialBlendMode::Opaque;
        blendEnable.Set(blend, StatePriority::MaterialPolicy, "material.renderPolicy");
        if (blend && policy.blend.mode == MaterialBlendMode::AlphaBlend)
        {
            srcBlend.Set(BlendFactor::SrcAlpha,    StatePriority::MaterialPolicy, "material.renderPolicy");
            dstBlend.Set(BlendFactor::InvSrcAlpha, StatePriority::MaterialPolicy, "material.renderPolicy");
        }
        else if (blend && policy.blend.mode == MaterialBlendMode::Additive)
        {
            srcBlend.Set(BlendFactor::One, StatePriority::MaterialPolicy, "material.renderPolicy");
            dstBlend.Set(BlendFactor::One, StatePriority::MaterialPolicy, "material.renderPolicy");
        }
        return *this;
    }

    // Wendet Pass-Locks an (Priorität PassLock — überschreibt alles).
    PipelineStateBuilder& ApplyPassLocks(const PassLocks& locks) noexcept
    {
        if (locks.depthTestLocked)
            depthEnable.Set(locks.depthTestValue,  StatePriority::PassLock, "passLock");
        if (locks.depthWriteLocked)
            depthWrite.Set(locks.depthWriteValue,  StatePriority::PassLock, "passLock");
        if (locks.cullModeLocked)
            cullMode.Set(locks.cullModeValue,      StatePriority::PassLock, "passLock");
        return *this;
    }

    // Schreibt die ermittelten States in eine bestehende PipelineDesc.
    // Schreibt nur depth/cull/blend — Shader, VertexLayout, Formate werden
    // vom Aufrufer separat gesetzt.
    void ApplyToDesc(PipelineDesc& pd) const noexcept
    {
        pd.depthStencil.depthEnable = depthEnable.Get();
        pd.depthStencil.depthWrite  = depthWrite.Get();
        pd.depthStencil.depthFunc   = depthFunc.Get();
        pd.rasterizer.cullMode      = cullMode.Get();
        pd.blendStates[0].blendEnable = blendEnable.Get();
        pd.blendStates[0].srcBlend  = srcBlend.Get();
        pd.blendStates[0].dstBlend  = dstBlend.Get();
    }
};

} // namespace engine::renderer
