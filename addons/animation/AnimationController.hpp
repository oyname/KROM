#pragma once
// =============================================================================
// KROM Engine - addons/animation/AnimationController.hpp
// Animation controller (*.animctrl): a state machine that decides which clip
// plays when. Three layers:
//
//   AnimationControllerAsset     the file as edited (JSON). States, parameters
//                                and transitions have stable ids; renaming
//                                breaks nothing. Clips are asset paths.
//   CompiledAnimationController  built once per load: indices instead of ids,
//                                transitions sorted by priority, clip handles.
//                                Immutable and shared by every figure.
//   AnimationControllerInstance  per figure: parameter values, current state,
//                                running cross-fade. Sized once when bound;
//                                StepAnimationController allocates nothing.
//
// File format ("kromanimctrl": 1):
//   entry        state id entered on start (missing: first state)
//   parameters   { id, name, type: float|int|bool|trigger, default }
//   states       { id, name, kind: "clip", clip, speed, pos: [x, y] }
//   transitions  { id, from: state id | "any", to, priority, conditions,
//                  blend (s), exitTime (normalized, absent = none),
//                  interruptible }
//   conditions   { param, op: greater|less|equals|notEquals|isTrue|isFalse,
//                  value }  (a trigger needs no op: it holds while set)
//   editor       { entryPos, anyPos }
//
// Rules: of all valid transitions the highest priority wins, ties go to the
// order in the file. Any State never enters the current state. A transition
// with exit time fires once the normalized time reaches it (also across loops
// and large steps). While a cross-fade runs, new transitions are checked only
// if it is interruptible; an interruption freezes the blended pose. Triggers
// stay set until a firing transition consumes them.
// =============================================================================
#include "core/Types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::addons::animation {

inline constexpr const char* kAnimationControllerExtension = ".animctrl";
inline constexpr const char* kAnyStateId = "any";

// ---------------------------------------------------------------------------
// Asset (as stored)
// ---------------------------------------------------------------------------

enum class AnimationParameterType : uint8_t { Float, Int, Bool, Trigger };

enum class AnimationConditionOp : uint8_t
{
    Greater,
    Less,
    Equals,     // int
    NotEquals,  // int
    IsTrue,     // bool
    IsFalse,    // bool
    Triggered,  // trigger is set
};

struct AnimationParameterDesc
{
    std::string            id;
    std::string            name;
    AnimationParameterType type = AnimationParameterType::Float;
    float                  defaultValue = 0.f;   // bool/trigger: 0 or 1

    bool operator==(const AnimationParameterDesc&) const = default;
};

struct AnimationConditionDesc
{
    std::string          parameterId;
    AnimationConditionOp op    = AnimationConditionOp::Greater;
    float                value = 0.f;

    bool operator==(const AnimationConditionDesc&) const = default;
};

enum class AnimationStateKind : uint8_t { Clip };

struct AnimationStateDesc
{
    std::string        id;
    std::string        name;
    AnimationStateKind kind = AnimationStateKind::Clip;
    std::string        clipAssetPath;   // relative to Assets
    float              speed = 1.f;
    float              posX = 0.f;      // editor graph
    float              posY = 0.f;

    bool operator==(const AnimationStateDesc&) const = default;
};

struct AnimationTransitionDesc
{
    std::string                         id;
    std::string                         from = kAnyStateId;   // state id or "any"
    std::string                         to;
    int32_t                             priority = 0;
    std::vector<AnimationConditionDesc> conditions;
    float                               blendDuration = 0.25f;
    bool                                hasExitTime = false;
    float                               exitTime = 1.f;
    bool                                interruptible = true;

    bool operator==(const AnimationTransitionDesc&) const = default;
};

struct AnimationControllerAsset
{
    std::string                          entryState;
    std::vector<AnimationParameterDesc>  parameters;
    std::vector<AnimationStateDesc>      states;
    std::vector<AnimationTransitionDesc> transitions;
    float entryPosX = 0.f, entryPosY = 0.f;
    float anyPosX = 0.f, anyPosY = 120.f;

    bool operator==(const AnimationControllerAsset&) const = default;

    [[nodiscard]] const AnimationStateDesc*     FindState(const std::string& id) const noexcept;
    [[nodiscard]] const AnimationParameterDesc* FindParameter(const std::string& id) const noexcept;
    // A new id not used by any state, parameter or transition.
    [[nodiscard]] std::string NewId(char prefix) const;
};

[[nodiscard]] std::string SerializeAnimationController(const AnimationControllerAsset& asset);
bool ParseAnimationController(const std::string& json, AnimationControllerAsset& out, std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Compiled (indices, shared)
// ---------------------------------------------------------------------------

// What the compiler needs to know about a clip.
struct AnimationClipInfo
{
    AnimationHandle handle;
    float           duration = 0.f;
    bool            looping = false;
    bool            found = false;
};
using AnimationClipResolver = std::function<AnimationClipInfo(const std::string& clipAssetPath)>;

struct CompiledAnimationController
{
    struct Parameter
    {
        std::string            name;
        AnimationParameterType type = AnimationParameterType::Float;
        float                  defaultValue = 0.f;
    };
    struct Condition
    {
        int32_t              parameter = -1;   // -1: deleted parameter, never holds
        AnimationConditionOp op = AnimationConditionOp::Greater;
        float                value = 0.f;
    };
    struct Transition
    {
        int32_t  from = -1;                    // -1: Any State
        int32_t  to = -1;
        int32_t  priority = 0;
        uint32_t order = 0;                    // position in the file (tie breaker)
        uint32_t firstCondition = 0;
        uint32_t conditionCount = 0;
        float    blendDuration = 0.f;
        bool     hasExitTime = false;
        float    exitTime = 1.f;
        bool     interruptible = true;
        bool     valid = true;                 // false: refers to a deleted parameter
    };
    struct State
    {
        std::string     name;
        std::string     clipAssetPath;
        AnimationHandle clip;
        float           duration = 0.f;
        bool            looping = false;
        bool            clipFound = false;
        float           speed = 1.f;
        uint32_t        firstTransition = 0;   // into stateTransitions, sorted
        uint32_t        transitionCount = 0;
    };

    std::vector<Parameter>  parameters;
    std::vector<State>      states;
    std::vector<Condition>  conditions;
    std::vector<Transition> transitions;       // file order
    std::vector<uint32_t>   stateTransitions;  // per state, sorted by priority
    std::vector<uint32_t>   anyTransitions;    // Any State, sorted by priority
    int32_t                 entry = -1;
    std::vector<std::string> problems;         // shown in the editor and logged

    [[nodiscard]] int32_t FindParameter(const std::string& name) const noexcept;
    [[nodiscard]] int32_t FindState(const std::string& name) const noexcept;
};

[[nodiscard]] CompiledAnimationController CompileAnimationController(const AnimationControllerAsset& asset,
                                                                     const AnimationClipResolver& resolveClip);

// ---------------------------------------------------------------------------
// Per-figure runtime
// ---------------------------------------------------------------------------

struct AnimationControllerInstance
{
    std::vector<float> parameters;   // one value per compiled parameter
    int32_t current = -1;
    float   currentTime = 0.f;       // seconds since entering (speed applied, unwrapped)
    int32_t previous = -1;           // state fading out, -1 when none
    float   previousTime = 0.f;
    bool    previousFrozen = false;  // fading out from a frozen pose (interrupted fade)
    float   blendElapsed = 0.f;
    float   blendDuration = 0.f;
    bool    blendInterruptible = true;
    int32_t lastTransition = -1;     // fired in the last step, -1 when none
    int32_t blendTransition = -1;    // transition of the running cross-fade, -1 for CrossFadeToState

    [[nodiscard]] bool Blending() const noexcept { return previous >= 0 || previousFrozen; }
    // Weight of the current state in [0, 1]; the rest belongs to the previous pose.
    [[nodiscard]] float CurrentWeight() const noexcept;
};

// Parameters to their defaults, the entry state at time 0.
void ResetAnimationController(const CompiledAnimationController& controller, AnimationControllerInstance& instance);

// Normalized time of a state (1 = one pass through its clip).
[[nodiscard]] float NormalizedStateTime(const CompiledAnimationController& controller, int32_t state, float time) noexcept;
// Clip time to sample: wrapped for looping clips, held at the end otherwise.
[[nodiscard]] float StateSampleTime(const CompiledAnimationController& controller, int32_t state, float time) noexcept;

// Advances times and the cross-fade, then fires at most one transition.
// Returns true when a transition fired (instance.lastTransition).
bool StepAnimationController(const CompiledAnimationController& controller, AnimationControllerInstance& instance, float dt);

// Starts a cross-fade into a state directly (scripts, editor preview).
void CrossFadeToState(const CompiledAnimationController& controller, AnimationControllerInstance& instance,
                      int32_t state, float blendDuration);

// ---------------------------------------------------------------------------
// Library: loaded and compiled controllers by asset path
// ---------------------------------------------------------------------------

class AnimationControllerLibrary
{
public:
    void SetAssetRoot(const std::filesystem::path& assetRoot);
    [[nodiscard]] const std::filesystem::path& AssetRoot() const noexcept { return m_assetRoot; }
    void SetClipResolver(AnimationClipResolver resolver) { m_resolver = std::move(resolver); }

    // Compiled controller for "<path>.animctrl" (relative to Assets); nullptr if the
    // file is missing or invalid (reported once). The file is read again when it
    // changed on disk (checked at most twice a second); figures notice through the
    // changed pointer. A path seen before costs a lookup and no allocation.
    [[nodiscard]] std::shared_ptr<const CompiledAnimationController> Get(const std::string& assetPath);
    // Replaces the in-memory version (editor edits before saving).
    void Set(const std::string& assetPath, const AnimationControllerAsset& asset);
    void Clear();

private:
    struct Entry
    {
        std::shared_ptr<const CompiledAnimationController> compiled;
        std::filesystem::file_time_type stamp{};
        std::chrono::steady_clock::time_point checked{};
        bool loaded = false;
        bool failed = false;
        bool fromMemory = false;
    };
    std::filesystem::path                  m_assetRoot;
    AnimationClipResolver                  m_resolver;
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace engine::addons::animation
